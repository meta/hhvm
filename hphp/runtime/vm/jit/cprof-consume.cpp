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

#include "hphp/runtime/vm/jit/cprof-consume.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>

#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/vm-worker.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-controller.h"
#include "hphp/runtime/vm/jit/cprof-counter.h"
#include "hphp/runtime/vm/jit/cprof-key.h"
#include "hphp/runtime/vm/jit/cprof-reader.h"
#include "hphp/runtime/vm/jit/cprof-replay.h"
#include "hphp/runtime/vm/jit/cprof-target-profile.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/prof-data-target-profile.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/vm-protect.h"
#include "hphp/runtime/vm/jit/write-lease.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/type-profile.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/assertions.h"
#include "hphp/util/boot-stats.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/logger.h"
#include "hphp/util/random.h"
#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {

namespace {

enum class StartupEligibility {
  Active,
  NotServerMode,
  NotRepoAuthoritative,
  ZeroMaxFunctions,
  EmptyDirectory,
  JitDisabled,
  PgoDisabled,
  RetranslateAllEnabled,
  JumpstartEnabled,
  DebuggerAttached,
};

StartupEligibility contProfStartupEligibility() {
  if (!Cfg::Server::Mode) return StartupEligibility::NotServerMode;
  if (!Cfg::Repo::Authoritative) {
    return StartupEligibility::NotRepoAuthoritative;
  }
  if (!Cfg::Jit::ContProfStartupMaxFunctions) {
    return StartupEligibility::ZeroMaxFunctions;
  }
  if (Cfg::Jit::ContProfCheckpointDirectory.empty()) {
    return StartupEligibility::EmptyDirectory;
  }
  if (!Cfg::Jit::Enabled) return StartupEligibility::JitDisabled;
  if (!Cfg::Jit::PGO) return StartupEligibility::PgoDisabled;
  if (mcgen::retranslateAllEnabled()) {
    return StartupEligibility::RetranslateAllEnabled;
  }
  if (RuntimeOption::EvalJitSerdesMode != JitSerdesMode::Off) {
    return StartupEligibility::JumpstartEnabled;
  }
  if (Cfg::Jit::DisabledByVSDebug && isDebuggerAttachedProcess()) {
    return StartupEligibility::DebuggerAttached;
  }
  return StartupEligibility::Active;
}

// Snapshots of the most recent startup that reached each stage.
auto const s_startupPreparedFuncs =
  ContProfCounter("jit.cprof.startup.prepared_funcs");
auto const s_startupPublishedFuncs =
  ContProfCounter("jit.cprof.startup.published_funcs");
// Prepared functions withheld from import for ordinary profiling. Selection
// does not guarantee execution, profiling-budget headroom, or a fresh capture.
auto const s_refreshSelected =
  ContProfCounter("jit.cprof.startup.refresh_selected");
// Configuration eligibility, not whether replay is currently running. The
// replay guard or reader can still prevent an eligible startup from importing.
auto const s_startupActive =
  ContProfCounter("jit.cprof.startup.active");
// The event totals below are cumulative. Inactive reasons are first-match and
// counted only at startup, never by calls to the public eligibility predicate.
auto const s_startupAttempts =
  ContProfCounter("jit.cprof.startup.attempts");
auto const s_startupInactive =
  ContProfCounter("jit.cprof.startup.inactive");
ContProfCounter const s_startupInactiveReasons[] = {
  ContProfCounter{nullptr}, // StartupEligibility::Active
  ContProfCounter("jit.cprof.startup.inactive.not_server_mode"),
  ContProfCounter("jit.cprof.startup.inactive.not_repo_authoritative"),
  ContProfCounter("jit.cprof.startup.inactive.zero_max_functions"),
  ContProfCounter("jit.cprof.startup.inactive.empty_directory"),
  ContProfCounter("jit.cprof.startup.inactive.jit_disabled"),
  ContProfCounter("jit.cprof.startup.inactive.pgo_disabled"),
  ContProfCounter("jit.cprof.startup.inactive.retranslate_all_enabled"),
  ContProfCounter("jit.cprof.startup.inactive.jumpstart_enabled"),
  ContProfCounter("jit.cprof.startup.inactive.debugger_attached"),
};
auto const s_startupErrors =
  ContProfCounter("jit.cprof.startup.errors");
auto const s_startupProfDataUnavailable =
  ContProfCounter("jit.cprof.startup.prof_data_unavailable");
// Deduplicated unit paths per load call, including target-profile dependencies.
auto const s_unitsRequested =
  ContProfCounter("jit.cprof.startup.units_requested");
auto const s_unitsReady =
  ContProfCounter("jit.cprof.startup.units_ready");
auto const s_unitLoadFailures =
  ContProfCounter("jit.cprof.startup.unit_load_failures");
auto const s_unitMergeFailures =
  ContProfCounter("jit.cprof.startup.unit_merge_failures");
// Records are examined in hotness order. Each rejected record has one reason;
// records_incompatible also has a detailed first-failure counter in the reader.
auto const s_recordsExamined =
  ContProfCounter("jit.cprof.startup.records_examined");
auto const s_closureContextRecords =
  ContProfCounter("jit.cprof.startup.rejected.closure_context");
auto const s_missingUnitRecords =
  ContProfCounter("jit.cprof.startup.rejected.missing_unit");
auto const s_unresolvedRecords =
  ContProfCounter("jit.cprof.startup.rejected.unresolved_or_stale");
auto const s_resolveExceptions =
  ContProfCounter("jit.cprof.startup.rejected.resolve_exception");
auto const s_incompatibleRecords =
  ContProfCounter("jit.cprof.startup.records_incompatible");
// Records not examined after the preparation cap, not known-compatible drops.
auto const s_limitUnexamined =
  ContProfCounter("jit.cprof.startup.limit_unexamined");
// Prepared translations can belong to functions later skipped during import.
auto const s_translationsPrepared =
  ContProfCounter("jit.cprof.startup.translations_prepared");
auto const s_translationsImported =
  ContProfCounter("jit.cprof.startup.translations_imported");
auto const s_installedFuncs =
  ContProfCounter("jit.cprof.startup.installed_funcs");
// Every normally returned install attempt is installed or has one skip reason.
auto const s_installAttempts =
  ContProfCounter("jit.cprof.startup.install_attempts");
auto const s_installSkippedLease =
  ContProfCounter("jit.cprof.startup.install_skipped.lease");
auto const s_installSkippedUnavailable =
  ContProfCounter("jit.cprof.startup.install_skipped.unavailable");
auto const s_installSkippedTouched =
  ContProfCounter("jit.cprof.startup.install_skipped.already_touched");
auto const s_installSkippedCounter =
  ContProfCounter("jit.cprof.startup.install_skipped.counter_default");
auto const s_installSkippedSrcRec =
  ContProfCounter("jit.cprof.startup.install_skipped.src_rec");
// Optimize attempts include pre-optimization skips. No-code counts completed
// optimizations with no translation at any imported start, not thrown failures.
auto const s_optimizeAttempts =
  ContProfCounter("jit.cprof.startup.optimize_attempts");
auto const s_optimizeSkippedLease =
  ContProfCounter("jit.cprof.startup.optimize_skipped.lease");
auto const s_optimizeSkippedUnavailable =
  ContProfCounter("jit.cprof.startup.optimize_skipped.unavailable");
auto const s_optimizeSkippedProfileState =
  ContProfCounter("jit.cprof.startup.optimize_skipped.profile_state");
auto const s_optimizeNoCode =
  ContProfCounter("jit.cprof.startup.optimize_no_code");
// Prepared profile entries erased across translations, not distinct call sites.
auto const s_methConflicts =
  ContProfCounter("jit.cprof.target.meth.method_conflicts");

// Restore counters count individual extension payloads, not batches or keys.
auto const s_restoreAttempts =
  ContProfCounter("jit.cprof.extensions.restore_attempts");
auto const s_restoredExtensions =
  ContProfCounter("jit.cprof.extensions.restored_extensions");
auto const s_restoreSkippedExisting =
  ContProfCounter("jit.cprof.extensions.restore_skipped_existing");
// Missing or disabled extensions.
auto const s_restoreSkippedUnavailable =
  ContProfCounter("jit.cprof.extensions.restore_skipped_unavailable");
auto const s_restoreFailures =
  ContProfCounter("jit.cprof.extensions.restore_failures");

void restoreExtensions(
  const ExtensionRegistry::SerializedExtensions& extensions
) noexcept {
  for (auto const& payload : extensions) {
    s_restoreAttempts->increment();
    try {
      auto const extension = ExtensionRegistry::get(payload.name.c_str());
      if (extension && extension->getWarmupData()) {
        s_restoreSkippedExisting->increment();
        continue;
      }
      if (!ExtensionRegistry::deserialize(payload)) {
        s_restoreSkippedUnavailable->increment();
        continue;
      }
      s_restoredExtensions->increment();
    } catch (const std::exception& exception) {
      s_restoreFailures->increment();
      Logger::Warning("cont-prof extension restore failed: %s", exception.what());
    } catch (...) {
      s_restoreFailures->increment();
      Logger::Warning("cont-prof extension restore failed");
    }
  }
}

struct LoadedUnits {
  size_t pathsRequested{0};
  std::map<std::string, Unit*> units;
};

LoadedUnits loadStartupUnits(
  const std::vector<ContProfProfileRecord>& records,
  const std::vector<std::string>& unitPaths
) {
  LoadedUnits result{};

  std::set<std::string> paths{unitPaths.begin(), unitPaths.end()};
  auto const addPaths = [&] (const ContProfFuncKey& key) {
    paths.emplace(key.resolutionUnitPath);
    if (key.bytecodeUnitPath) paths.emplace(*key.bytecodeUnitPath);
  };
  for (auto const& record : records) {
    auto const& key = record.header.funcKey;
    // Skip class-scoped closures to avoid populating the cache.
    if (key.closureContextName) continue;
    addPaths(key);
    for (auto const& translation : record.translations) {
      for (auto const& profile : translation.targetProfiles) {
        for (auto const& path : contProfTargetProfileUnitPaths(profile)) {
          paths.emplace(path);
        }
      }
    }
  }
  result.pathsRequested = paths.size();
  s_unitsRequested->addValue(paths.size());

  for (auto const& path : paths) {
    try {
      auto const lookupPath = String{Cfg::Server::SourceRoot + path};
      auto const unit = lookupUnit(
        lookupPath.get(), "", nullptr, nullptr, false
      );
      if (unit) {
        result.units.emplace(path, unit);
      } else {
        s_unitLoadFailures->increment();
      }
    } catch (...) {
      s_unitLoadFailures->increment();
      continue;
    }
  }

  // Merge all units before resolving functions so class lookups are available.
  for (auto it = result.units.begin(); it != result.units.end();) {
    try {
      it->second->merge();
      ++it;
    } catch (...) {
      s_unitMergeFailures->increment();
      it = result.units.erase(it);
    }
  }

  s_unitsReady->addValue(result.units.size());
  return result;
}

void discardConflictingMethProfiles(ContProfStartupCandidate& candidate) {
  using Site = std::pair<Offset, const StringData*>;
  std::map<Site, const Func*> methods;
  for (auto const& translation : candidate.translations) {
    for (auto const& profile : translation.targetProfiles) {
      if (!profile.meth) continue;
      auto const func = profile.meth->uniqueMeth();
      auto const [it, inserted] = methods.emplace(
        Site{profile.bytecodeOffset, profile.name}, func
      );
      if (!inserted && it->second != func) it->second = nullptr;
    }
  }

  // Different methods could reduce to BaseMeth/InterfaceMeth, whose broader
  // dispatch assumptions aren't established by the imported function keys.
  for (auto& translation : candidate.translations) {
    auto const removed = std::erase_if(
      translation.targetProfiles, [&] (auto const& profile) {
        return profile.meth &&
          !methods.at(Site{profile.bytecodeOffset, profile.name});
      }
    );
    s_methConflicts->addValue(removed);
  }
}

ContProfStartupCandidate prepareCandidate(
  const ContProfProfileRecord& record,
  Func& func,
  const std::map<std::string, Unit*>& units
) {
  ContProfStartupCandidate result{};
  result.func = &func;
  result.translations.reserve(record.translations.size());

  // Rebuild each retained translation as a one-block profiling region.
  for (auto const& translation : record.translations) {
    auto const start = contProfTranslationSrcKey(translation, func);
    assertx(start);

    auto region = std::make_shared<RegionDesc>();
    auto const block = region->addBlock(
      *start,
      static_cast<int>(translation.regionLength),
      SBInvOffset{0}
    );
    for (auto const& guard : translation.localTypeGuards) {
      block->addPreCondition(RegionDesc::GuardedLocation{
        Location::Local{guard.localId},
        Type{guard.type, PtrLocation::Bottom},
        guard.category,
      });
    }

    PostConditions postConditions{};

    for (auto const& post : translation.localPostConditions) {
      auto const location = RegionDesc::TypedLocation{
        Location::Local{post.localId},
        Type{post.type, PtrLocation::Bottom},
      };

      if (post.changed) {
        postConditions.changed.push_back(location);
      } else {
        postConditions.refined.push_back(location);
      }
    }

    std::vector<ContProfPreparedTargetProfile> targetProfiles;
    targetProfiles.reserve(translation.targetProfiles.size());

    for (auto const& profile : translation.targetProfiles) {
      auto prepared = prepareContProfTargetProfile(profile, units);
      if (!prepared) continue;

      targetProfiles.push_back(std::move(*prepared));
    }

    result.translations.push_back({
      *start,
      std::move(region),
      static_cast<int64_t>(translation.executionCount),
      translation.incoming,
      std::move(postConditions),
      std::move(targetProfiles),
    });
  }

  discardConflictingMethProfiles(result);
  s_translationsPrepared->addValue(result.translations.size());
  return result;
}

void installCandidateProfileData(
  ProfData& profData,
  const ContProfStartupCandidate& candidate,
  int64_t counterDefault
) {
  auto const numTranslations = candidate.translations.size();

  std::vector<TransID> transIds;
  transIds.reserve(numTranslations);
  for (size_t i = 0; i < numTranslations; ++i) {
    transIds.push_back(profData.allocTransID());
  }

  auto const targetProfiles = profData.targetProfile();
  assertx(targetProfiles);

  for (size_t i = 0; i < numTranslations; ++i) {
    auto const& translation = candidate.translations[i];

    for (auto const& profile : translation.targetProfiles) {
      (void)installContProfTargetProfile(
        profile,
        transIds[i],
        *targetProfiles
      );
    }

    // Predecessor indices are record-local and were bounds-checked against
    // record.translations.size() during validation.
    RegionDesc::BlockIdSet incoming;
    incoming.reserve(translation.incoming.size());
    for (auto const predecessor : translation.incoming) {
      incoming.insert(transIds.at(predecessor));
    }

    *profData.transCounterAddr(transIds[i]) =
      counterDefault - translation.executionCount;

    translation.region->incoming(std::move(incoming));
    profData.addTransProfile(
      transIds[i],
      translation.region,
      translation.postConditions,
      0
    );
  }
}

bool installCandidate(
  ProfData& profData,
  const ContProfStartupCandidate& candidate
) {
  s_installAttempts->increment();
  auto const func = candidate.func;

  VMProtect protect;
  LeaseHolder lease{func, TransKind::Optimize, true};
  if (!lease) {
    s_installSkippedLease->increment();
    return false;
  }

  auto const funcId = func->getFuncId();

  // Only import into a Func the JIT hasn't touched; the profData identity
  // checks catch a discard racing the caller's check.
  if (!Func::isFuncIdValid(funcId) || Func::fromFuncId(funcId) != func ||
      jit::profData() != &profData || globalProfData() != &profData) {
    s_installSkippedUnavailable->increment();
    return false;
  }
  if (profData.profiling(funcId) || profData.optimized(funcId) ||
      !profData.funcProfTransIDs(func).empty()) {
    s_installSkippedTouched->increment();
    return false;
  }

  auto const counterDefault = profData.counterDefault();
  if (counterDefault < 0) {
    s_installSkippedCounter->increment();
    return false;
  }

  // Create every SrcRec before mutating ProfData. Replay supports only
  // empty-stack starts, which capture and compatibility checks enforce.
  for (auto const& translation : candidate.translations) {
    if (!tc::createSrcRec(translation.start, SBInvOffset{0})) {
      s_installSkippedSrcRec->increment();
      return false;
    }
  }

  SCOPE_FAIL {
    always_assert_flog(
      false, "cont-prof startup failed during ProfData installation"
    );
  };
  installCandidateProfileData(profData, candidate, counterDefault);
  profData.setProfiling(func);
  s_installedFuncs->increment();
  s_translationsImported->addValue(candidate.translations.size());
  return true;
}

bool optimizeCandidate(
  ProfData& profData,
  const ContProfStartupCandidate& candidate
) {
  s_optimizeAttempts->increment();
  auto const func = candidate.func;

  VMProtect protect;
  LeaseHolder lease{func, TransKind::Optimize, true};
  if (!lease) {
    s_optimizeSkippedLease->increment();
    return false;
  }

  auto const funcId = func->getFuncId();
  if (!Func::isFuncIdValid(funcId) || Func::fromFuncId(funcId) != func ||
      jit::profData() != &profData || globalProfData() != &profData) {
    s_optimizeSkippedUnavailable->increment();
    return false;
  }
  if (!profData.profiling(funcId) || profData.optimized(funcId) ||
      profData.funcProfTransIDs(func).empty()) {
    s_optimizeSkippedProfileState->increment();
    return false;
  }

  SCOPE_FAIL {
    always_assert_flog(
      false, "cont-prof startup failed during optimization"
    );
  };
  profData.setOptimized(funcId);
  mcgen::optimizeFunc(func);

  auto const published = std::any_of(
    candidate.translations.begin(),
    candidate.translations.end(),
    [](const ContProfStartupCandidate::Translation& translation) {
      auto const srcRec = tc::findSrcRec(translation.start);
      return srcRec && srcRec->numTrans() != 0;
    }
  );
  if (!published) s_optimizeNoCode->increment();
  return published;
}

}

bool contProfStartupActive() {
  return contProfStartupEligibility() == StartupEligibility::Active;
}

ContProfStartupCandidates prepareContProfStartupCandidates(
  const std::vector<ContProfProfileRecord>& records,
  size_t maxFunctions,
  const std::vector<std::string>& unitPaths,
  const ExtensionRegistry::SerializedExtensions& extensions
) {
  restoreExtensions(extensions);
  if (records.empty() || !maxFunctions) {
    return prepareContProfStartupCandidates(records, {}, maxFunctions);
  }
  auto const loaded = [&] {
    BootStats::Block timer{"cont_prof_resolve", Cfg::Server::Mode, true};
    return loadStartupUnits(records, unitPaths);
  }();
  BootStats::Block timer{"cont_prof_prepare", Cfg::Server::Mode, true};
  auto result = prepareContProfStartupCandidates(
    records, loaded.units, maxFunctions
  );
  result.unitsRequested = loaded.pathsRequested;
  result.unitsReady = loaded.units.size();
  return result;
}

ContProfStartupCandidates prepareContProfStartupCandidates(
  const std::vector<ContProfProfileRecord>& records,
  const std::map<std::string, Unit*>& units,
  size_t maxFunctions
) {
  ContProfStartupCandidates result{};
  if (!maxFunctions) {
    s_limitUnexamined->addValue(records.size());
    return result;
  }
  result.candidates.reserve(std::min(records.size(), maxFunctions));

  std::vector<const ContProfProfileRecord*> ordered;
  ordered.reserve(records.size());
  for (auto const& record : records) ordered.push_back(&record);
  std::sort(ordered.begin(), ordered.end(), [](auto const lhs, auto const rhs) {
    auto const lhsExecutions = lhs->functionExecutions();
    auto const rhsExecutions = rhs->functionExecutions();
    if (lhsExecutions != rhsExecutions) {
      return lhsExecutions > rhsExecutions;
    }
    return lhs->header.funcKey < rhs->header.funcKey;
  });

  size_t examined{};
  for (auto const record : ordered) {
    ++examined;
    s_recordsExamined->increment();
    auto const& key = record->header.funcKey;
    if (key.closureContextName) {
      s_closureContextRecords->increment();
      continue;
    }

    auto const resolutionUnit = units.find(key.resolutionUnitPath);
    if (resolutionUnit == units.end()) {
      s_missingUnitRecords->increment();
      continue;
    }
    if (key.bytecodeUnitPath &&
        units.find(*key.bytecodeUnitPath) == units.end()) {
      s_missingUnitRecords->increment();
      continue;
    }

    Func* func{nullptr};
    try {
      func = resolveContProfFunc(key, *resolutionUnit->second);
      if (!func) {
        s_unresolvedRecords->increment();
        continue;
      }

      ++result.recordsResolved;
      if (!isContProfProfileRecordCompatible(*record, *func)) {
        s_incompatibleRecords->increment();
        continue;
      }
    } catch (...) {
      s_resolveExceptions->increment();
      continue;
    }

    result.candidates.push_back(prepareCandidate(*record, *func, units));
    if (result.candidates.size() == maxFunctions) {
      // These records were not examined, so they are not known-compatible drops.
      s_limitUnexamined->addValue(ordered.size() - examined);
      break;
    }
  }

  return result;
}

std::vector<const ContProfStartupCandidate*> installContProfStartupCandidates(
  ProfData& profData,
  const std::vector<ContProfStartupCandidate>& candidates,
  const std::vector<ContProfProfileRecord>& records
) {
  std::vector<const ContProfStartupCandidate*> installed;
  installed.reserve(candidates.size());

  if (candidates.empty()) return installed;
  if (jit::profData() != &profData || globalProfData() != &profData) {
    s_startupProfDataUnavailable->increment();
    return installed;
  }

  if (!profData.targetProfile()) {
    // Replay covers only a subset of sites, so a miss falls back to live
    // RDS rather than meaning "no data".
    profData.setTargetProfile(
      std::make_unique<ProfDataTargetProfile>(/*liveFallback=*/true)
    );
  }

  auto const retain = !records.empty() && contProfActive();
  for (auto const& candidate : candidates) {
    if (jit::profData() != &profData || globalProfData() != &profData) {
      s_startupProfDataUnavailable->increment();
      break;
    }
    if (retain) {
      auto const key = makeContProfFuncKey(*candidate.func);
      assertx(key);
      auto const record = std::lower_bound(
        records.begin(), records.end(), *key,
        [](auto const& record, auto const& key) {
          return record.header.funcKey < key;
        }
      );
      assertx(record != records.end() && record->header.funcKey == *key);
      retainContProfProfileRecord(*record);
      if (Cfg::Jit::ContProfReuseProbability < 1 &&
          !folly::Random::randBool(
            Cfg::Jit::ContProfReuseProbability, threadLocalRng64()
          )) {
        s_refreshSelected->increment();
        continue;
      }
    }
    if (installCandidate(profData, candidate)) installed.push_back(&candidate);
  }
  return installed;
}

void consumeContProfAtStartup() noexcept {
  if (!Cfg::Jit::ContProfStartupMaxFunctions &&
      !s_startupActive.initialized()) return;

  auto const eligibility = contProfStartupEligibility();
  auto const active = eligibility == StartupEligibility::Active;
  s_startupActive->setValue(active);
  if (!active) {
    s_startupInactive->increment();
    s_startupInactiveReasons[static_cast<size_t>(eligibility)]->increment();
    Logger::Info("cont-prof startup: inactive");
    return;
  }
  s_startupAttempts->increment();

  try {
    BootStats::Block timer{"cont_prof_startup", Cfg::Server::Mode, true};
    VMWorker([&] {
      ProfileNonVMThread nonVM;
      HphpSession session{Treadmill::SessionKind::TranslateWorker};

      ContProfReplayGuard replay{Cfg::Jit::ContProfCheckpointDirectory};
      if (!replay.active()) return;
      SCOPE_SUCCESS { replay.complete(); };

      auto const input = [&] {
        BootStats::Block timer{"cont_prof_read", Cfg::Server::Mode, true};
        return readContProfCheckpointDirectory(
          Cfg::Jit::ContProfCheckpointDirectory
        );
      }();
      if (!input) return;

      auto const prepared = prepareContProfStartupCandidates(
        input->records, Cfg::Jit::ContProfStartupMaxFunctions,
        input->unitPaths, input->extensions
      );
      s_startupPreparedFuncs->setValue(prepared.candidates.size());

      auto const profData = jit::profData();
      if (!profData || globalProfData() != profData) {
        s_startupProfDataUnavailable->increment();
        return;
      }

      setMayAcquireLease(true);
      SCOPE_EXIT { setMayAcquireLease(false); };

      // Make every imported callee's profiles available before compiling callers.
      auto const installed = [&] {
        BootStats::Block timer{"cont_prof_install", Cfg::Server::Mode, true};
        return installContProfStartupCandidates(
          *profData, prepared.candidates, input->records
        );
      }();

      size_t published{};

      {
        BootStats::Block timer{
          "cont_prof_optimize_serial", Cfg::Server::Mode, true
        };
        // Optimize candidates in descending function-hotness order.
        for (auto const candidate : installed) {
          if (jit::profData() != profData || globalProfData() != profData) {
            s_startupProfDataUnavailable->increment();
            break;
          }

          if (optimizeCandidate(*profData, *candidate)) ++published;
        }
      }
      s_startupPublishedFuncs->setValue(published);

      Logger::Info(
        "Cont-prof startup published optimized code for %zu of "
        "%zu imported functions (%zu prepared, %zu candidates "
        "from %zu decoded records in %zu files, %zu resolved, "
        "%zu of %zu units ready)",
        published,
        installed.size(),
        prepared.candidates.size(),
        input->records.size(),
        input->recordsDecoded,
        input->filesRead,
        prepared.recordsResolved,
        prepared.unitsReady,
        prepared.unitsRequested
      );
    }).run();
  } catch (...) {
    s_startupErrors->increment();
    Logger::Warning("cont-prof startup consumption failed");
  }
}

}
