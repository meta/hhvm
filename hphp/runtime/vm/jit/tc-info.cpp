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

#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-internal.h"

#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/trans-db.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/data-block.h"
#include "hphp/util/build-info.h"

#include <fmt/format.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>

#include <algorithm>
#include <string>
#include <vector>

namespace HPHP::jit::tc {

namespace {

bool dumpTCCode(folly::StringPiece filename) {
#define OPEN_FILE(F, SUFFIX)                                            \
  auto const F ## name = folly::to<std::string>(filename, SUFFIX);      \
  FILE* F = fopen(F ## name .c_str(),"wb");                             \
  if (F == nullptr) return false;                                       \
  SCOPE_EXIT{ fclose(F); };

  OPEN_FILE(aFile,          "_a");
  // Preserve the three-file dump shape for consumers even though all code is
  // stored in the sparse unified file.
  OPEN_FILE(acoldFile,      "_acold");
  OPEN_FILE(afrozenFile,    "_afrozen");

#undef OPEN_FILE

  // Dynamic TC sections are noncontiguous, so preserve each code block's
  // address as an offset in the sparse unified dump.
  auto result = true;
  auto const& all = code().all();
  auto writeBlock = [&](const CodeBlock& cb) {
    if (!result) return;
    assertx(all.contains(cb.base(), cb.frontier()));
    auto const offset = cb.base() - all.base();
    if (fseeko(aFile, offset, SEEK_SET) != 0) {
      result = false;
      return;
    }
    auto const count = cb.used();
    result = fwrite(cb.base(), 1, count, aFile) == count;
  };

  code().forEachBlock(writeBlock);
  return result;
}

bool dumpTCData() {
  auto const dataPath = Cfg::Eval::DumpTCPath + "/tc_data.txt.gz";
  gzFile tcDataFile = gzopen(dataPath.c_str(), "w");
  if (!tcDataFile) return false;
  SCOPE_EXIT { gzclose(tcDataFile); };

  auto const& main = code().all();

  if (!gzprintf(tcDataFile,
                "repo_schema      = %s\n"
                "a.base           = %p\n"
                "a.frontier       = %p\n\n",
                repoSchemaId().begin(),
                main.base(),   main.frontier())) {
    return false;
  }

  if (!gzprintf(tcDataFile, "total_translations = %zu\n\n",
                transdb::getNumTranslations())) {
    return false;
  }

  if (!gzprintf(tcDataFile, "block_map = %s\n\n", code().blockMap().data())) {
    return false;
  }

  // Print all translations, including their execution counters. If global
  // counters are disabled (default), fall back to using ProfData, covering
  // only profiling translations.
  if (transdb::enabled()) {
    // Admin requests do not automatically init ProfData, so do it explicitly.
    // No need for matching exit call; data is immortal with trans DB enabled.
    requestInitProfData();
  }
  const TransRec invalid;
  assertx(!invalid.isValid());
  for (TransID t = 0; t < transdb::getNumTranslations(); t++) {
    auto transRec = transdb::getTransRec(t);
    if (!transRec) transRec = &invalid;
    auto const ret = gzputs(tcDataFile, transRec->print().c_str());
    if (ret == -1) {
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
}

bool dumpEnabled() {
  return Cfg::Eval::DumpTC ||
         Cfg::Eval::DumpIR ||
         Cfg::Eval::DumpRegion ||
         Cfg::Eval::DumpInlDecision ||
         Cfg::Eval::DumpCallTargets ||
         Cfg::Eval::DumpLayoutCFG ||
         Cfg::Eval::DumpVBC;
}

bool dump(bool ignoreLease /* = false */) {
  if (!mcgen::initialized()) return false;

  {
    std::unique_lock<SimpleMutex> codeLock;
    std::unique_lock<SimpleMutex> metaLock;
    if (!ignoreLease) {
      codeLock = lockCode();
      metaLock = lockMetadata();
    }
    if (!dumpTCData()) return false;
    if (!dumpTCCode(Cfg::Eval::DumpTCPath + "/tc_dump")) return false;
  }

  if (!Cfg::Repo::Authoritative) {
    dump_compiled_units(Cfg::Eval::DumpTCPath + "/hhvm.hhbc");
  }

  return true;
}

std::vector<UsageInfo> getUsageInfo() {
  std::vector<UsageInfo> tcUsageInfo;

  code().forEachSection([&] (const char* name, auto const& s) {
    tcUsageInfo.emplace_back(UsageInfo{
      std::string("code.") + name,
      s.used(),
      s.capacity(),
      true
    });
  });
  tcUsageInfo.emplace_back(UsageInfo{
    "data",
    code().dataUsed(),
    code().dataCapacity(),
    true
  });
  tcUsageInfo.emplace_back(UsageInfo{
    "RDS",
    rds::usedBytes(),
    rds::capacityBytes(),
    false
  });
  tcUsageInfo.emplace_back(UsageInfo{
    "RDSLocal",
    rds::usedLocalBytes(),
    rds::capacityLocalBytes(),
    false
  });
  tcUsageInfo.emplace_back(UsageInfo{
    "persistentRDS",
    rds::usedPersistentBytes(),
    rds::capacityPersistentBytes(),
    false
  });
  return tcUsageInfo;
}

std::string getTCSpace() {
  std::string usage;
  size_t total_size = 0;
  size_t total_capacity = 0;

  auto const add_row = [&] (const UsageInfo& ui) {
    auto const percent = ui.capacity ?  100 * ui.used / ui.capacity : 0;

    usage += fmt::format(
      "mcg: {:9} bytes ({}%) in {}\n",
      ui.used, percent, ui.name
    );

    if (ui.global) {
      total_size += ui.used;
      total_capacity += ui.capacity;
    }
  };

  auto const uis = getUsageInfo();
  std::for_each(uis.begin(), uis.end(), add_row);
  add_row(UsageInfo { "total", total_size, total_capacity, false });

  return usage;
}

std::string getTCAddrs() {
  std::string addrs;

  code().forEachSection([&] (const char* name, auto const&) {
    addrs += fmt::format("{}: {}\n", name, fmt::ptr(code().base()));
  });
  return addrs;
}

std::vector<TCMemInfo> getTCMemoryUsage() {
  std::vector<TCMemInfo> ret;
  code().forEachSection(
    [&](const char* name, auto const& s) {
      ret.emplace_back(TCMemInfo{
        name,
        s.used(),
        s.numAllocs(),
        s.numFrees(),
        s.bytesFree(),
        s.blocksFree()
      });
    }
  );
  return ret;
}

std::string getRDSUsageJSON() {
  folly::dynamic summary = folly::dynamic::object
    ("normal", folly::dynamic::object
      ("used", rds::usedBytes())
      ("capacity", rds::capacityBytes()))
    ("local", folly::dynamic::object
      ("used", rds::usedLocalBytes())
      ("capacity", rds::capacityLocalBytes()))
    ("persistent", folly::dynamic::object
      ("used", rds::usedPersistentBytes())
      ("capacity", rds::capacityPersistentBytes()));

  auto const categories = rds::usageByCategory();
  folly::dynamic catArray = folly::dynamic::array;
  for (auto const& cat : categories) {
    auto const modeName = [&] {
      switch (cat.mode) {
        case rds::Mode::Normal:     return "Normal";
        case rds::Mode::Local:      return "Local";
        case rds::Mode::Persistent: return "Persistent";
        default:                    return "Unknown";
      }
    }();

    size_t regionUsed = 0;
    switch (cat.mode) {
      case rds::Mode::Normal:     regionUsed = rds::usedBytes(); break;
      case rds::Mode::Local:      regionUsed = rds::usedLocalBytes(); break;
      case rds::Mode::Persistent: regionUsed = rds::usedPersistentBytes(); break;
      default: break;
    }
    double pct = regionUsed > 0
      ? 100.0 * cat.bytes / regionUsed
      : 0.0;

    catArray.push_back(folly::dynamic::object
      ("category", cat.category)
      ("mode", modeName)
      ("bytes", cat.bytes)
      ("count", cat.count)
      ("pct_of_region", pct));
  }

  folly::dynamic result = folly::dynamic::object
    ("summary", std::move(summary))
    ("categories", std::move(catArray));
  return folly::toJson(result);
}

}
