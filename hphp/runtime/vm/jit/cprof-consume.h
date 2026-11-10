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

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/cprof-target-profile.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/srckey.h"

namespace HPHP {
struct Func;
struct Unit;
}

namespace HPHP::jit {
struct ProfData;
}

namespace HPHP::jit::cprof {

struct ContProfProfileRecord;

struct ContProfStartupCandidate {
  struct Translation {
    SrcKey start;
    std::shared_ptr<RegionDesc> region{};
    int64_t executionCount{0};
    std::vector<uint32_t> incoming;
    PostConditions postConditions{};
    std::vector<ContProfPreparedTargetProfile> targetProfiles;
  };

  Func* func{nullptr};
  std::vector<Translation> translations;
};

struct ContProfStartupCandidates {
  // Only includes records resolved before reaching the preparation cap.
  size_t recordsResolved{0};
  size_t unitsRequested{0};
  size_t unitsReady{0};
  std::vector<ContProfStartupCandidate> candidates;
};

/*
 * Restore extension data, then load and merge saved and referenced units
 * before preparing functions.
 * Preloading is best-effort across repo changes.
 * Must run in a VM session, before installing profiles or entering the optimizer.
 */
ContProfStartupCandidates prepareContProfStartupCandidates(
  const std::vector<ContProfProfileRecord>& records,
  size_t maxFunctions,
  const std::vector<std::string>& unitPaths = {},
  const ExtensionRegistry::SerializedExtensions& extensions = {}
);

/*
 * Resolve records against already merged units and prepare compatible functions
 * in descending hotness order. Only successful preparations count toward the
 * cap; the result does not own the units or functions.
 */
ContProfStartupCandidates prepareContProfStartupCandidates(
  const std::vector<ContProfProfileRecord>& records,
  const std::map<std::string, Unit*>& units,
  size_t maxFunctions
);

/*
 * Install profiles without optimizing any functions. Returns successful imports
 * in candidate order, borrowing pointers from candidates. The caller must keep
 * candidates alive, hold a VM/Treadmill session for the current ProfData, and
 * allow write-lease acquisition. When supplied, records must be the key-ordered
 * originals used to prepare candidates. With active capture, retain those
 * originals and leave the sampled refresh cohort uninstalled.
 */
std::vector<const ContProfStartupCandidate*> installContProfStartupCandidates(
  ProfData& profData,
  const std::vector<ContProfStartupCandidate>& candidates,
  const std::vector<ContProfProfileRecord>& records = {}
);

/* Whether continuous-profile startup replay is enabled. */
bool contProfStartupActive();

/*
 * Load compatible checkpoints and publish optimized code during server
 * startup.
 */
void consumeContProfAtStartup() noexcept;

}
