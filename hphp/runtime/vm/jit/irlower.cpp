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

#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/irlower-internal.h"

#include "hphp/runtime/base/perf-warning.h"
#include "hphp/runtime/base/tracing.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/punt.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"

#include "hphp/util/arch.h"
#include "hphp/util/assertions.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

#include <folly/Range.h>

namespace HPHP::jit::irlower {

///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhir)

namespace {

///////////////////////////////////////////////////////////////////////////////

void cgInst(IRLS& env, const IRInstruction* inst){
  SCOPE_ASSERT_DETAIL("cgInst") { return inst->toString(); };

  switch (inst->op()) {
#define O(name, dsts, srcs, flags)                                           \
    case name:                                                               \
      FTRACE(7, "cg" #name "\n");                                            \
      try {                                                                  \
        cg##name(env, inst);                                                 \
      } catch (const Exception& e) {                                         \
        always_assert_flog(0, "Exception escaped from cg" #name ": {}",      \
                           e.getMessage());                                  \
      } catch (const std::exception& e) {                                    \
        always_assert_flog(0, "std::exception escaped from cg" #name ": {}", \
                           e.what());                                        \
      } catch (...) {                                                        \
        always_assert_flog(0, "unknown exception escaped from cg" #name);    \
      }                                                                      \
      break;
    IR_OPCODES
#undef O
    default:
      always_assert(false);
  }

  auto& v = vmain(env);
  if (inst->isBlockEnd() && !v.closed()) {
    if (auto const next = inst->next()) {
      v << jmp{label(env, next)};
    } else {
      v << trap{TRAP_REASON, Fixup::none()}; // or end?
    }
  }
}

/*
 * Lower `block' from HHIR to vasm.
 */
void genBlock(IRLS& env, Vout& v, Vout& vc, Block& block) {
  FTRACE(6, "genBlock: {}\n", block.id());

  env.vmain = &v;
  env.vcold = &vc;

  for (auto& inst : block) {
    v.unit().cur_voff = 0;
    v.setOrigin(&inst);
    vc.setOrigin(&inst);
    cgInst(env, &inst);
  }
}

}

///////////////////////////////////////////////////////////////////////////////

void optimize(Vunit& unit, CodeKind kind, bool regAlloc) {
  auto const abi = jit::abi(kind);
  optimize(unit, abi, regAlloc);
}

std::unique_ptr<Vunit> lowerUnit(const IRUnit& unit,
                                 CodeKind kind,
                                 bool regAlloc /* = true */) noexcept {
  Timer timer(Timer::hhir_lower, unit.logEntry().get_pointer());

  tracing::Block _{
    "vasm-gen",
    [&] {
      return traceProps(unit)
        .add("code_kind", codeKindAsString(kind))
        .add("reg_alloc", regAlloc);
    }
  };

  rqtrace::EventGuard trace{"VLOWER"};
  SCOPE_ASSERT_DETAIL("hhir unit") { return show(unit); };

  auto vunit = std::make_unique<Vunit>();
  vunit->context = unit.context();
  vunit->log_entry = unit.logEntry().get_pointer();
  vunit->profiling = true;
  Vasm vasm{*vunit};
  SCOPE_ASSERT_DETAIL("vasm unit") { return show(*vunit); };

  IRLS env{unit};
  auto const blocks = rpoSortCfg(unit);

  // Create the initial set of vasm blocks, numbered the same as the
  // corresponding HHIR blocks.
  // We initially create the blocks with 0 weight, and then set them below.
  for (uint32_t i = 0, n = unit.numBlocks(); i < n; ++i) {
    env.labels[i] = vunit->makeBlock(AreaIndex::Main, 0);
  }
  vunit->entry = env.labels[unit.entry()];

  // Create Vregs for all relevant SSATmps.
  assignRegs(unit, *vunit, env, blocks);

  for (auto block : blocks) {
    auto& v = block->hint() == Block::Hint::Unlikely ? vasm.cold() :
              block->hint() == Block::Hint::Unused ? vasm.frozen() :
              vasm.main();
    FTRACE(6, "genBlock {} on {}\n", block->id(),
           area_names[(unsigned)v.area()]);

    auto b = env.labels[block];
    vunit->blocks[b].area_idx = v.area();
    vunit->blocks[b].weight = block->profCount() * areaWeightFactor(v.area());
    v.use(b);

    auto& vcold =
      block->hint() == Block::Hint::Unused ? vasm.frozen() : vasm.cold();

    if (block == unit.entry() && kind == CodeKind::Trace) {
      v.setOrigin(&block->instrs().front());
    }
    genBlock(env, v, vcold, *block);

    assertx(v.closed());
    assertx(vasm.main().empty() || vasm.main().closed());
    assertx(vasm.cold().empty() || vasm.cold().closed());
    assertx(vasm.frozen().empty() || vasm.frozen().closed());
  }

  // This pass requires on some invariants about rvmfp() from HHIR, so we do it
  // here rather than in optimize() as those optimizations may be called for non
  // HHIR Vunits.
  fixupVmfpUses(*vunit);

  printUnit(kInitialVasmLevel, "after initial vasm generation", *vunit);
  assertx(check(*vunit));
  timer.stop();
  trace.finish();

  try {
    optimize(*vunit, kind, regAlloc);
  } catch (const FailedTraceGen& e) {
    // vasm-xls can fail if it tries to allocate too many spill slots.
    tracing::addPoint("vasm-optimize punt");
    logLowPriPerfWarning(
      "vasm-optimize punt",
      1000,
      [&](StructuredLogEntry& cols) {
        cols.setStr("punt_type", e.what());
        cols.setStr("vasm_unit", show(*vunit));
      }
    );
    FTRACE(1, "vasm-optimize failed with {}\n", e.what());
    return nullptr;
  }

  return vunit;
}

Vcost computeIRUnitCost(const IRUnit& unit) {
  auto vunit = lowerUnit(unit, CodeKind::Trace, false /* regAlloc */);
  if (Cfg::HHIR::InliningUseLayoutBlocks) {
    layoutBlocks(*vunit);
  }
  return computeVunitCost(*vunit);
}


///////////////////////////////////////////////////////////////////////////////

}
