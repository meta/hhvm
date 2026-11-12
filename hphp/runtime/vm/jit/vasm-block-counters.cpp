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

#include <fmt/format.h>

#include "hphp/runtime/vm/jit/vasm-block-counters.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/runtime/vm/jit/trans-prof-counters.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"

#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"

#include <type_traits>

TRACE_SET_MOD(vasm_block_count)

namespace HPHP { namespace jit {

namespace VasmBlockCounters {

///////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * Profile counters for vasm blocks for each optimized JIT region and prologue.
 * Along with the counters, we keep the opcode of the first Vasm instruction
 * in each block as metadata (prior to inserting the profile counters), which
 * is used to validate the profile.
 */
using VOpRaw = std::underlying_type<Vinstr::Opcode>::type;
TransProfCounters<int64_t, VOpRaw> s_blockCounters;

bool ignoreOp(Vinstr::Opcode op) {
  return op == Vinstr::phidef || op == Vinstr::landingpad;
}

/*
 * Insert profile counters for the blocks in the given unit.
 */
template <typename T>
void insert(Vunit& unit, const T& key) {
  assertx(isJitSerializing());

  // This isn't necessary for correctness, but it ensures we add
  // counters for all possible edges. It also helps us stay in sync
  // with the register allocator (which needs these invariants
  // anyways).
  assertx(checkNoCriticalEdges(unit));
  assertx(checkNoSideExits(unit));

auto const blocks = sortBlocks(unit);
  for (auto const b : blocks) {
    auto& block = unit.blocks[b];

    size_t idx = 0;
    while (ignoreOp(block.code[idx].op)) ++idx;

    auto const counter = s_blockCounters.addCounter(key, block.code[idx].op);

    vmodify(
      unit,
      b,
      idx,
      [&] (Vout& v) {
        // We use decqmlocknosf here so we don't have to worry about
        // potentially clobbering a live sf. If we later determine
        // it's dead (which is the usual case), we'll lower it to a
        // regular decqmlock.
        auto const addr = v.cns(counter);
        v << decqmlocknosf{addr[0]};
        return 0;
      }
    );
  }

  if (Trace::moduleEnabled(Trace::vasm_block_count, 1) ||
      Trace::moduleEnabled(Trace::vasm, kVasmBlockCountLevel)) {
    printUnit(0, "after vasm-block-counts insert counters", unit);
  }
}

/*
 * Checks whether the profile data in `counters' and `opcodes' matches the given
 * `unit'. If they don't, a string with the error found is returned. Otherwise
 * (on success), an empty string is returned.
 */
std::string checkProfile(const Vunit& unit,
                         const jit::vector<Vlabel>& sortedBlocks,
                         const jit::vector<int64_t>& counters,
                         const jit::vector<VOpRaw>& opcodes) {

  auto const kind = unit.context
    ? unit.context->kind == TransKind::OptPrologue ? "prologue" : "region"
    : "unique stub";
  if (counters.size() == 0) return fmt::format("no profile for this {}", kind);

  std::string errorMsg;
  size_t opcodeMismatches=0;

  auto report = [&] {
    if (!errorMsg.empty()) return;
    if (!unit.context) {
      errorMsg = std::string(unit.name) + "\n";
      FTRACE(1, "VasmBlockCounters::checkProfile: UniqueStub={}", errorMsg);
    } else if (unit.context->kind == TransKind::OptPrologue) {
      errorMsg = show(unit.context->pid) + "\n";
      FTRACE(1, "VasmBlockCounters::checkProfile: PrologueID={}", errorMsg);
    } else {
      errorMsg = show(unit.context->region->entry()->start()) + "\n";
      FTRACE(1, "VasmBlockCounters::checkProfile: SrcKey={}", errorMsg);
    }
  };

  for (size_t index = 0; index < sortedBlocks.size(); index++) {
    auto b = sortedBlocks[index];
    auto& block = unit.blocks[b];
    if (index >= counters.size()) {
      report();
      auto const msg = fmt::format(
        "missing block counter for B{} (index = {}, counters.size() = {})\n",
        size_t{b}, index, counters.size());
      FTRACE(1, "VasmBlockCounters::checkProfile: {}", msg);
      errorMsg += msg;
      return errorMsg;
    }

    auto const op_opti = [&] {
      size_t idx = 0;
      while (ignoreOp(block.code[idx].op)) ++idx;
      return block.code[idx].op;
    }();

    auto const op_prof = opcodes[index];
    if (op_prof != op_opti) {
      report();
      auto const msg = fmt::format(
        "mismatch opcode for B{} (index = {}): "
        "profile was {} optimized is {}\n",
        size_t{b}, index, vinst_names[op_prof], vinst_names[op_opti]);
      FTRACE(1, "VasmBlockCounters::checkProfile: {}", msg);
      errorMsg += msg;
      opcodeMismatches++;
    }
  }

  // Consider the profile to match even if we have some opcode mismatches.
  if (opcodeMismatches <=
      Cfg::Jit::PGOVasmBlockCountersMaxOpMismatches) {
    return "";
  }
  return errorMsg;
}

/*
 * Set the weights of the blocks in the given unit based on profile data, if
 * available.
 */
template <typename T>
void setWeights(Vunit& unit, const T& key) {
  assertx(isJitDeserializing());

  jit::vector<VOpRaw> opcodes;
  auto counters = s_blockCounters.getCounters(key, opcodes);

  assertx(checkNoCriticalEdges(unit));
  assertx(checkNoSideExits(unit));

  auto sortedBlocks = sortBlocks(unit);

  FTRACE(2, "VasmBlockCounters::setWeights: original unit\n{}", show(unit));

  std::string errorMsg = checkProfile(unit, sortedBlocks, counters, opcodes);

  bool enoughProfile = false;

  if (errorMsg == "") {
    // Check that enough profile was collected.
    if (counters[0] >= Cfg::Jit::PGOVasmBlockCountersMinEntryValue) {
      enoughProfile = true;
      // Update the block weights.
      for (size_t index = 0; index < sortedBlocks.size(); index++) {
        auto const b = sortedBlocks[index];
        auto& block = unit.blocks[b];
        assertx(index < counters.size());
        block.weight = counters[index];
        // Drop the separation between the main, cold and frozen areas, to avoid
        // scaling of the block weights based on the area since we have accurate
        // counters.  The code-layout pass may re-split the code into
        // hot/cold/frozen later.  If JitPGOLayoutResplitFrozen is false, leave
        // frozen blocks alone as we won't move them to another area.
        if (block.area_idx != AreaIndex::Frozen ||
            Cfg::Jit::PGOLayoutResplitFrozen) {
          block.area_idx = AreaIndex::Main;
        }
      }
    }
  }

  if (Cfg::Eval::DumpVBC) {
    unit.annotations.emplace_back("VasmBlockCounters",
                                  errorMsg != "" ? errorMsg :
                                  enoughProfile ? "matches" :
                                  "matches, but not enough profile data");
  }

  if (Trace::moduleEnabled(Trace::vasm_block_count, 1) ||
      Trace::moduleEnabled(Trace::vasm, kVasmBlockCountLevel)) {
    printUnit(0, "after vasm-block-counts set weights", unit);
  }
}

}

///////////////////////////////////////////////////////////////////////////////

Optional<uint64_t> getRegionWeight(const RegionDesc& region) {
  if (!Cfg::Jit::PGOVasmBlockCounters || !isJitDeserializing()) {
    return std::nullopt;
  }
  auto const key = RegionEntryKey(region);
  auto const weight = s_blockCounters.getFirstCounter(key);
  if (!weight) return std::nullopt;
  return safe_cast<uint64_t>(*weight);
}

template <typename T>
void update(Vunit& unit, const T& key){
  if (isJitSerializing()) {
    // Insert block profile counters.
    insert(unit, key);
  } else if (isJitDeserializing()) {
    // Look up the information from the profile, and use it if found.
    setWeights(unit, key);
  }
}

void profileGuidedUpdate(Vunit& unit) {
  if (!Cfg::Jit::PGOVasmBlockCounters) return;

  if (unit.name){
    // unique stub
    std::string name(unit.name);
    update(unit, name);
    return;
  }

  if (!unit.context) return;
  auto const optimizePrologue = unit.context->kind == TransKind::OptPrologue &&
    Cfg::Jit::PGOVasmBlockCountersOptPrologue;

  if (unit.context->kind == TransKind::Optimize) {
    auto const regionPtr = unit.context->region;
    if (!regionPtr) return;
    const RegionEntryKey regionKey(*regionPtr);
    update(unit, regionKey);
  } else if (optimizePrologue) {
    auto const pid = unit.context->pid;
    if (pid.func() == nullptr) return;
    update(unit, pid);
  }
}

///////////////////////////////////////////////////////////////////////////////

void serialize(ProfDataSerializer& ser) {
  s_blockCounters.serialize(ser);
}

void deserialize(ProfDataDeserializer& des) {
  s_blockCounters.deserialize(des);
}

void free() {
  s_blockCounters.freeCounters();
}

///////////////////////////////////////////////////////////////////////////////

} } }
