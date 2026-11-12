/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/cprof-capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/trans-cfg.h"
#include "hphp/util/assertions.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::cprof {

TRACE_SET_MOD(cprof)

namespace {

/*
 * Snapshot basic local types and guard categories. Entry starts may omit
 * non-local preconditions; bytecode starts reject them.
 */
std::optional<std::vector<ContProfLocalTypeGuard>>
snapshotLocalTypeGuards(const RegionDesc::Block& block, const Func& func) {
  auto const isEntry = block.start().anyFuncEntry();
  auto const localLimit = isEntry
    ? func.numFuncEntryInputs()
    : static_cast<uint32_t>(func.numLocals());

  std::map<uint32_t, ContProfLocalTypeGuard> guards;

  for (auto const& precondition : block.typePreConditions()) {
    auto const& location = precondition.location;
    if (location.tag() != LTag::Local) {
      if (!isEntry) return std::nullopt;
      continue;
    }

    auto const localId = location.localId();
    if (localId >= localLimit) return std::nullopt;

    auto const type = precondition.type;
    if (type == TBottom || !(type <= TCell)) return std::nullopt;
    auto const guardType = relaxToGuardable(type);
    if (guardType == TCell) continue;

    // Class/layout specializations are not retained in the basic type bits.
    auto const category = std::min(precondition.category, DataTypeSpecific);
    auto const bits = guardType.rawBits();
    if (!typeFitsConstraint(guardType, category)) {
      return std::nullopt;
    }

    auto [it, inserted] = guards.try_emplace(
      localId, ContProfLocalTypeGuard{localId, bits, category}
    );
    if (!inserted) {
      if (it->second.type != bits) return std::nullopt;
      it->second.category = std::max(it->second.category, category);
    }
  }

  std::vector<ContProfLocalTypeGuard> result;
  result.reserve(guards.size());
  for (auto const& [_, guard] : guards) result.push_back(guard);
  return result;
}

std::optional<std::vector<ContProfLocalPostCondition>>
snapshotLocalPostConditions(
  const PostConditions& postConditions,
  const Func& func
) {
  struct State {
    bool changed;
    Type::bits_t type;
  };

  std::map<uint32_t, State> states;

  auto const merge = [&](const TypedLocations& locations, bool changed) {
    for (auto const& condition : locations) {
      auto const& location = condition.location;
      if (location.tag() != LTag::Local) continue;

      auto const localId = location.localId();
      if (localId >= static_cast<uint32_t>(func.numLocals())) return false;

      auto const type = condition.type;
      if (type == TBottom || !(type <= TCell)) return false;

      auto const bits = type.rawBits();
      auto [it, inserted] = states.emplace(localId, State{changed, bits});

      if (!inserted) {
        it->second.changed = it->second.changed || changed;
        it->second.type |= bits;
      }
    }

    return true;
  };

  if (!merge(postConditions.refined, false) ||
      !merge(postConditions.changed, true)) {
    return std::nullopt;
  }

  std::vector<ContProfLocalPostCondition> result;
  result.reserve(states.size());

  for (auto const& [localId, state] : states) {
    if (!state.changed && state.type == Type::kCell) continue;
    result.push_back({localId, state.changed, state.type});
  }

  return result;
}

struct TranslationCandidate {
  TransID transId{kInvalidTransID};
  RegionDescPtr region;
  ContProfProfileTranslation translation;
};

auto startKey(const TranslationCandidate& candidate) {
  return candidate.translation.startKey();
}

// Group candidates by start, with the winner first, for one-pass dedup below.
bool candidateLess(
  const TranslationCandidate& lhs,
  const TranslationCandidate& rhs
) {
  auto const leftKey = startKey(lhs);
  auto const rightKey = startKey(rhs);
  if (leftKey != rightKey) return leftKey < rightKey;

  auto const& left = lhs.translation;
  auto const& right = rhs.translation;

  if (left.executionCount != right.executionCount) {
    return left.executionCount > right.executionCount;
  }
  return std::tie(left.regionLength, lhs.transId) <
    std::tie(right.regionLength, rhs.transId);
}

}

std::optional<ContProfProfileRecord>
snapshotContProfProfileRecord(
  const ProfData& profData,
  const Func& func,
  std::vector<TransID>* sourceTransIds,
  ContProfCaptureStats* stats
) {
  if (sourceTransIds) sourceTransIds->clear();
  if (stats) *stats = {};

  auto const reject = [&](ContProfCaptureRejection reason) {
    if (stats) ++stats->rejections[static_cast<size_t>(reason)];
  };

  auto funcKey = makeContProfFuncKey(func);
  if (!funcKey) {
    reject(ContProfCaptureRejection::InvalidFuncKey);
    return std::nullopt;
  }

  auto const transIds = profData.funcProfTransIDs(&func);
  if (transIds.empty()) {
    reject(ContProfCaptureRejection::NoProfileTranslations);
    return std::nullopt;
  }

  std::vector<TranslationCandidate> candidates;

  for (auto const transId : transIds) {
    if (stats) ++stats->translationsExamined;
    auto const record = profData.transRec(transId);
    auto const start = record->srcKey();
    if (start.prologue() || start.resumeMode() != ResumeMode::None) {
      reject(ContProfCaptureRejection::PrologueOrResumedTranslation);
      continue;
    }

    auto const region = record->region();
    if (!region || region->blocks().size() != 1) {
      reject(ContProfCaptureRejection::UnexpectedRegion);
      if (stats) {
        if (!region || region->blocks().empty()) {
          ++stats->missingRegions;
        } else {
          ++stats->multipleBlockRegions;
        }
      }
      TRACE(2, "cont-prof: %s trans %d has an unexpected region\n",
            func.fullName()->data(), transId);
      continue;
    }

    auto const block = region->entry();
    if (block->start() != start ||
        block->initialSpOffset() != SBInvOffset{0} ||
        block->length() <= 0) {
      reject(ContProfCaptureRejection::UnexpectedBlock);
      if (stats) {
        if (block->start() != start) {
          ++stats->mismatchedBlockStarts;
        } else if (block->initialSpOffset() != SBInvOffset{0}) {
          ++stats->nonzeroStackOffsets;
        } else {
          ++stats->emptyBlocks;
        }
      }
      TRACE(2, "cont-prof: %s trans %d has an unexpected block\n",
            func.fullName()->data(), transId);
      continue;
    }

    TranslationCandidate candidate{};
    candidate.transId = transId;
    candidate.region = region;
    auto& translation = candidate.translation;
    translation.regionLength = static_cast<uint32_t>(block->length());

    if (start.funcEntry()) {
      if (start.numEntryArgs() > func.numPositionalParams()) {
        reject(ContProfCaptureRejection::InvalidEntry);
        continue;
      }

      translation.startKind = ContProfStartKind::FuncEntry;
      translation.offsetOrNumEntryArgs = start.numEntryArgs();
    } else if (start.namedParamsFuncEntry()) {
      if (!func.hasOptionalNamedParameters() ||
          start.numEntryArgs() != func.numPositionalParams()) {
        reject(ContProfCaptureRejection::InvalidEntry);
        continue;
      }

      translation.startKind = ContProfStartKind::NamedParamsFuncEntry;
    } else {
      auto const offset = start.offset();

      if (!func.contains(offset) || func.isEntry(offset)) {
        reject(ContProfCaptureRejection::InvalidEntry);
        continue;
      }

      translation.startKind = ContProfStartKind::Bytecode;
      translation.offsetOrNumEntryArgs = static_cast<uint32_t>(offset);
    }

    auto localTypeGuards = snapshotLocalTypeGuards(*block, func);
    if (!localTypeGuards) {
      reject(ContProfCaptureRejection::InvalidLocalTypeGuards);
      continue;
    }

    if (!localTypeGuards->empty() && start.funcEntry()) {
      auto const numEntryArgs = start.numEntryArgs();
      if (numEntryArgs < func.numRequiredPositionalParams() ||
          start.trivialDVFuncEntry()) {
        reject(ContProfCaptureRejection::InvalidEntryTypeGuards);
        continue;
      }
    }

    translation.localTypeGuards = std::move(*localTypeGuards);

    auto localPostConditions =
      snapshotLocalPostConditions(block->postConds(), func);
    if (!localPostConditions) {
      reject(ContProfCaptureRejection::InvalidLocalPostconditions);
      continue;
    }

    translation.localPostConditions = std::move(*localPostConditions);

    auto const count = profData.transCounter(transId);
    assertx(count >= 0);
    if (count == 0) {
      reject(ContProfCaptureRejection::ZeroExecutionCount);
      continue;
    }
    translation.executionCount = static_cast<uint64_t>(count);

    candidates.push_back(std::move(candidate));
    if (stats) ++stats->translationsEligible;
  }

  if (candidates.empty()) {
    reject(ContProfCaptureRejection::NoEligibleCandidates);
    return std::nullopt;
  }

  std::sort(candidates.begin(), candidates.end(), candidateLess);

  ContProfProfileRecord result{};
  result.header.funcKey = std::move(*funcKey);

  std::vector<const TranslationCandidate*> selectedCandidates;
  selectedCandidates.reserve(candidates.size());

  // Retain one translation per start. Keep the preferred candidate.
  for (size_t i = 0; i < candidates.size();) {
    auto const& selected = candidates[i];
    auto const& translation = selected.translation;

    selectedCandidates.push_back(&selected);
    result.translations.push_back(translation);

    auto next = i + 1;
    while (next < candidates.size() &&
           startKey(selected) == startKey(candidates[next])) {
      ++next;
    }

    i = next;
  }

  if (stats) {
    stats->translationsRetained = selectedCandidates.size();
    stats->translationsStartDeduplicated =
      candidates.size() - selectedCandidates.size();
  }

  hphp_fast_map<TransID, uint32_t> localIndices;
  localIndices.reserve(selectedCandidates.size());
  for (size_t i = 0; i < selectedCandidates.size(); ++i) {
    localIndices.emplace(
      selectedCandidates[i]->transId,
      static_cast<uint32_t>(i)
    );
  }

  for (size_t i = 0; i < selectedCandidates.size(); ++i) {
    auto const candidate = selectedCandidates[i];
    if (candidate->translation.startKind != ContProfStartKind::Bytecode) {
      continue;
    }

    auto const predecessors = findPredTrans(*candidate->region, &profData);
    auto& incoming = result.translations[i].incoming;

    for (auto const predecessor : predecessors) {
      auto const local = localIndices.find(predecessor);
      if (local != localIndices.end()) incoming.push_back(local->second);
    }

    std::sort(incoming.begin(), incoming.end());
  }
  auto const capturedAtMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()
    ).count();

  result.header.capturedAtMs = static_cast<uint64_t>(capturedAtMs);

  if (!isValidContProfProfileRecord(result)) {
    reject(ContProfCaptureRejection::InvalidFinalRecord);
    return std::nullopt;
  }

  if (sourceTransIds) {
    sourceTransIds->reserve(selectedCandidates.size());
    for (auto const candidate : selectedCandidates) {
      sourceTransIds->push_back(candidate->transId);
    }
  }

  return result;
}

}
