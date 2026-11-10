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

#include "hphp/runtime/vm/jit/cprof-reader.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-counter.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/switch-profile.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {

namespace {

// Cumulative work across checkpoint reads, including repeated startup attempts.
auto const s_filesDiscovered =
  ContProfCounter("jit.cprof.reader.files_discovered");
auto const s_filesAttempted =
  ContProfCounter("jit.cprof.reader.files_attempted");
auto const s_filesRead =
  ContProfCounter("jit.cprof.reader.files_read");
auto const s_filesRejected =
  ContProfCounter("jit.cprof.reader.files_rejected");
auto const s_recordsDecoded =
  ContProfCounter("jit.cprof.reader.records_decoded");
auto const s_recordsUnique =
  ContProfCounter("jit.cprof.reader.records_unique");
auto const s_directoryOpenFailures =
  ContProfCounter("jit.cprof.reader.directory_open_failures");
auto const s_fileMetadataFailures =
  ContProfCounter("jit.cprof.reader.file_metadata_failures");

// Exactly the first rejection per compatibility check; these count records,
// not individual translations, guards, postconditions, or CFG edges.
auto const s_invalidRecord =
  ContProfCounter("jit.cprof.startup.rejected.invalid_record");
auto const s_keyMismatch =
  ContProfCounter("jit.cprof.startup.rejected.key_mismatch");
auto const s_invalidStart =
  ContProfCounter("jit.cprof.startup.rejected.invalid_start");
auto const s_unknownStackOffset =
  ContProfCounter("jit.cprof.startup.rejected.unknown_stack_offset");
auto const s_nonzeroStackOffset =
  ContProfCounter("jit.cprof.startup.rejected.nonzero_stack_offset");
auto const s_invalidRegion =
  ContProfCounter("jit.cprof.startup.rejected.invalid_region");
auto const s_invalidGuards =
  ContProfCounter("jit.cprof.startup.rejected.invalid_guards");
auto const s_invalidPostconditions =
  ContProfCounter("jit.cprof.startup.rejected.invalid_postconditions");
auto const s_invalidEdges =
  ContProfCounter("jit.cprof.startup.rejected.invalid_edges");

bool hasValidTargetProfilePayload(
  const Func& func,
  const ContProfTargetProfile& profile
) {
  if (profile.kind != ContProfTargetProfileKind::Switch) {
    return true;
  }

  auto const pc = func.at(profile.bytecodeOffset);
  if (peek_op(pc) != Op::Switch) return false;

  auto const numCases = getImmVector(pc).size();
  // Replay uses the bytecode's case count, so the saved allocation must match.
  return profile.payload.size() ==
    sizeof(SwitchProfile) + SwitchProfile::extraSize(numCases);
}

std::optional<SrcKey> validateTranslationRegion(
  const Func& func,
  SrcKey start,
  const ContProfProfileTranslation& translation
) {
  auto profile = translation.targetProfiles.begin();
  auto last = start;

  for (uint32_t i = 0; i < translation.regionLength; ++i) {
    if (i != 0) last.advance(&func);
    // Function entries are synthetic instructions, not bytecode sites.
    if (last.anyFuncEntry()) continue;

    while (profile != translation.targetProfiles.end() &&
           profile->bytecodeOffset == last.offset()) {
      if (!hasValidTargetProfilePayload(func, *profile)) {
        return std::nullopt;
      }
      ++profile;
    }
  }

  if (profile != translation.targetProfiles.end()) {
    return std::nullopt;
  }

  return last;
}

}

std::optional<SrcKey> contProfTranslationSrcKey(
  const ContProfProfileTranslation& translation,
  const Func& func
) {
  switch (translation.startKind) {
    case ContProfStartKind::FuncEntry: {
      if (translation.numEntryArgs() > func.numPositionalParams()) {
        return std::nullopt;
      }

      return SrcKey{
        &func,
        translation.numEntryArgs(),
        false,
        SrcKey::FuncEntryTag{},
      };
    }

    case ContProfStartKind::NamedParamsFuncEntry: {
      if (!func.hasOptionalNamedParameters()) {
        return std::nullopt;
      }

      return SrcKey{
        &func,
        func.numPositionalParams(),
        true,
        SrcKey::FuncEntryTag{},
      };
    }

    case ContProfStartKind::Bytecode: {
      auto const offset = static_cast<Offset>(translation.offset());

      if (!func.contains(offset) || func.isEntry(offset)) {
        return std::nullopt;
      }

      return SrcKey{&func, offset, ResumeMode::None};
    }
  }

  return std::nullopt;
}

std::optional<ContProfCheckpointReadResult>
readContProfCheckpointDirectory(const std::string& directory) {
  ContProfCheckpointReadResult result{};
  if (directory.empty()) return result;

  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    s_directoryOpenFailures->increment();
    return std::nullopt;
  }
  auto const path = std::filesystem::path{directory} / kContProfCheckpointFilename;
  auto const status = std::filesystem::symlink_status(path, error);
  if (status.type() == std::filesystem::file_type::not_found) return result;
  if (error) {
    s_fileMetadataFailures->increment();
    return std::nullopt;
  }
  if (!std::filesystem::is_regular_file(status)) return result;

  s_filesDiscovered->increment();
  s_filesAttempted->increment();
  auto checkpoint = readContProfCheckpointFile(path.native());
  if (!checkpoint) {
    s_filesRejected->increment();
    return result;
  }

  result.filesRead = 1;
  result.recordsDecoded = checkpoint->records.size();
  result.records = std::move(checkpoint->records);
  result.unitPaths = std::move(checkpoint->unitPaths);
  if (checkpoint->extensionData) {
    result.extensions = std::move(*checkpoint->extensionData);
  }
  s_filesRead->increment();
  s_recordsDecoded->addValue(result.recordsDecoded);
  s_recordsUnique->addValue(result.records.size());
  return result;
}

bool isContProfProfileRecordCompatible(
  const ContProfProfileRecord& record,
  const Func& func
) {
  if (!isValidContProfProfileRecord(record)) {
    s_invalidRecord->increment();
    return false;
  }

  auto const currentKey = makeContProfFuncKey(func);
  if (!currentKey || *currentKey != record.header.funcKey) {
    s_keyMismatch->increment();
    return false;
  }

  std::vector<SrcKey> starts;
  std::vector<SrcKey> lasts;
  starts.reserve(record.translations.size());
  lasts.reserve(record.translations.size());
  for (auto const& translation : record.translations) {
    auto const start = contProfTranslationSrcKey(translation, func);
    if (!start) {
      s_invalidStart->increment();
      return false;
    }

    if (!start->anyFuncEntry()) {
      // Replay supports only empty-stack starts.
      auto const stackOffset = mcgen::offsetAtLocation(*start);
      if (!stackOffset) {
        s_unknownStackOffset->increment();
        return false;
      }
      if (*stackOffset != SBInvOffset{0}) {
        s_nonzeroStackOffset->increment();
        return false;
      }
    }

    auto const region = validateTranslationRegion(func, *start, translation);
    if (!region) {
      s_invalidRegion->increment();
      return false;
    }

    starts.push_back(*start);
    lasts.push_back(*region);

    auto const& guards = translation.localTypeGuards;

    if (!guards.empty() && start->funcEntry()) {
      auto const numEntryArgs = start->numEntryArgs();
      if (numEntryArgs < func.numRequiredPositionalParams() ||
          start->trivialDVFuncEntry()) {
        s_invalidGuards->increment();
        return false;
      }
    }

    auto const localLimit = start->anyFuncEntry()
      ? func.numFuncEntryInputs()
      : static_cast<uint32_t>(func.numLocals());

    for (auto const& guard : guards) {
      if (guard.localId >= localLimit) {
        s_invalidGuards->increment();
        return false;
      }
    }

    for (auto const& post : translation.localPostConditions) {
      if (post.localId >= func.numLocals()) {
        s_invalidPostconditions->increment();
        return false;
      }
    }
  }

  for (size_t i = 0; i < record.translations.size(); ++i) {
    for (auto const predecessor : record.translations[i].incoming) {
      auto const last = lasts[predecessor];

      if (last.funcEntry() &&
          last.numEntryArgs() < func.numRequiredPositionalParams()) {
        s_invalidEdges->increment();
        return false;
      }

      if (!last.succSrcKeys().contains(starts[i])) {
        s_invalidEdges->increment();
        return false;
      }
    }
  }

  return true;
}

}
