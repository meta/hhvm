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

#include "hphp/runtime/vm/jit/unique-stubs.h"

#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/rds-header.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/event-hook.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/interp-helpers.h"
#include "hphp/runtime/vm/prologue-flags.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/align.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/code-gen-tls.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/phys-reg-saver.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/service-request-handlers.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/stack-overflow.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc-record.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#ifdef __aarch64__
#include "hphp/runtime/vm/jit/unique-stubs-arm.h"
#else
#include "hphp/runtime/vm/jit/unique-stubs-x64.h"
#endif
#include "hphp/runtime/vm/jit/unwind-itanium.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vtune-jit.h"

#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/generator/ext_generator.h"

#include "hphp/util/arch.h"
#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/data-block.h"
#include "hphp/util/roar.h"
#include "hphp/util/trace.h"

#include <fmt/format.h>

#include <algorithm>
#include <cxxabi.h>

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(ustubs)

///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////

// Synthetic return addresses enter after a nop included in the symbol range.
struct StubEmission {
  explicit StubEmission(TCA address) : start(address), entry(address) {}
  StubEmission(TCA start, TCA entry) : start(start), entry(entry) {}

  TCA start;
  TCA entry;
};

/*
 * Profilers and debuggers resolve a saved return address at RIP - 1
 * to identify the function containing the call instruction. When a frame's
 * return address is set directly to a unique stub's entry point, RIP - 1 would
 * instead fall outside that stub.
 *
 * Emit a nop to cover that address. The stub's entry point must be immediately
 * after the nop, while its reported symbol range must start at the address
 * returned here, including the nop. An unrelocated fallthru emits a real nop on
 * both x64 and ARM; an ordinary vasm nop can be removed by optimization.
 */
TCA emitSavedReturnAddressPadding(CodeBlock& cb, DataBlock& data) {
  auto const start = vwrap(cb, data, [] (Vout& v) {
    v << fallthru{};
  }, nullptr, false /* relocate */);
  assertx(cb.frontier() > start);
  return start;
}

void alignJmpTarget(CodeBlock& cb) {
  if (Cfg::Jit::AlignUniqueStubs) {
    align(cb, nullptr, Alignment::JmpTarget, AlignContext::Dead);
  }
}

void alignCacheLine(CodeBlock& cb) {
  if (Cfg::Jit::AlignUniqueStubs) {
    align(cb, nullptr, Alignment::CacheLine, AlignContext::Dead);
  }
}

/*
 * Load and store the VM registers from/to RDS.
 */
void loadVmfp(Vout& v) { v << load{rvmtl()[rds::kVmfpOff], rvmfp()}; }
void loadVmsp(Vout& v) { v << load{rvmtl()[rds::kVmspOff], rvmsp()}; }
void loadVMRegs(Vout& v) { loadVmfp(v); loadVmsp(v); }
void storeVmfp(Vout& v) { v << store{rvmfp(), rvmtl()[rds::kVmfpOff]}; }
void storeVmsp(Vout& v) { v << store{rvmsp(), rvmtl()[rds::kVmspOff]}; }
void storeVMRegs(Vout& v) { storeVmfp(v); storeVmsp(v); }

/*
 * Pop frame from the VM stack to the func entry registers and unlink it.
 */
void popFrameToFuncEntryRegs(Vout& v) {
  v << load{Vreg(rvmfp()) + AROFF(m_thisUnsafe), r_func_entry_ctx()};
  v << loadl{Vreg(rvmfp()) + AROFF(m_callOffAndFlags), r_func_entry_ar_flags()};
  v << loadl{Vreg(rvmfp()) + AROFF(m_funcId), r_func_entry_callee_id()};
  v << copy{rvmfp(), rvmsp()};
  v << restoreripm{Vreg(rvmfp()) + AROFF(m_savedRip)};
  v << load{Vreg(rvmsp()) + AROFF(m_sfp), rvmfp()};
}

/*
 * Push frame from the func entry registers to the VM stack and link it.
 */
void pushFrameFromFuncEntryRegs(Vout& v) {
  v << store{rvmfp(), Vreg(rvmsp()) + AROFF(m_sfp)};
  v << copy{rvmsp(), rvmfp()};
  v << phplogue{rvmfp()};
  v << storel{r_func_entry_callee_id(), Vreg(rvmfp()) + AROFF(m_funcId)};
  v << storel{r_func_entry_ar_flags(),
              Vreg(rvmfp()) + AROFF(m_callOffAndFlags)};
  v << store{r_func_entry_ctx(), Vreg(rvmfp()) + AROFF(m_thisUnsafe)};
}

/*
 * Load and store the PHP return registers from/to the top of the VM stack.
 *
 * Note that we don't do loadb{}/storeb{} for the type register, because we
 * sometimes need to preserve the m_aux field across returns.
 */
void loadReturnRegs(Vout& v) {
  v << load{rvmsp()[TVOFF(m_data)], rret_data()};
  v << load{rvmsp()[TVOFF(m_type)], rret_type()};
}
void storeReturnRegs(Vout& v) {
  v << store{rret_data(), rvmsp()[TVOFF(m_data)]};
  v << store{rret_type(), rvmsp()[TVOFF(m_type)]};
}

/*
 * Convenience wrapper around a simple vcall to `helper', with a single `arg'
 * and a return value in `d'.
 */
template<class F>
Vinstr simplecall(Vout& v, F helper, Vreg arg, Vreg d) {
  return vcall{
    CallSpec::direct(helper, nullptr),
    v.makeVcallArgs({{arg}}),
    v.makeTuple({d}),
    Fixup::none(),
    DestType::SSA
  };
}

/*
 * Emit a catch trace that unwinds a stub context back to the PHP context that
 * called it.
 */
template<class GenFn>
void emitStubCatch(Vout& v, const UniqueStubs& us, GenFn gen) {
  always_assert(us.endCatchStublogueHelper);
  v << landingpad{};
  gen(v);
  v << jmpi{us.endCatchStublogueHelper, vm_regs_no_sp()};
}

///////////////////////////////////////////////////////////////////////////////

TCA emitFreeLocalsHelpers(CodeBlock& cb, DataBlock& data, UniqueStubs& us) {
  alignCacheLine(cb);
  return ARCH_SWITCH_CALL(emitFreeLocalsHelpers, cb, data, us);
}

StubEmission emitCallToExit(CodeBlock& cb, DataBlock& data, UniqueStubs& us,
                            const char* /* name*/) {
  alignJmpTarget(cb);
  auto const start = emitSavedReturnAddressPadding(cb, data);
  auto const entry = ARCH_SWITCH_CALL(emitCallToExit, cb, data, us);
  return {start, entry};
}

///////////////////////////////////////////////////////////////////////////////

template<class Result, class Handler>
Result withVMRegsForCall(PrologueFlags prologueFlags, const Func* func,
                         uint32_t numArgs, bool hasUnpack, TCA savedRip,
                         Handler handler) {
  // The stub already synced the vmfp() and vmsp() registers, but the vmsp() is
  // pointing to the space reserved for the ActRec. Adjust it to point to the
  // top of the stack containing call inputs and set vmpc()/vmJitReturnAddr().
  auto& unsafeRegs = vmRegsUnsafe();
  auto const calleeFP = unsafeRegs.stack.top();
  auto const callerFP = unsafeRegs.fp;

  unsafeRegs.stack.nalloc(
    numArgs + (hasUnpack ? 1 : 0) + (prologueFlags.hasGenerics() ? 1 : 0));
  unsafeRegs.pc = callerFP->func()->at(prologueFlags.callOffset());
  unsafeRegs.jitReturnAddr = savedRip;
  regState() = VMRegState::CLEAN;

  try {
    // If we did not throw, we are going to reenter TC, so set the registers
    // as dirty.
    SCOPE_SUCCESS { regState() = VMRegState::DIRTY; };
    return handler(unsafeRegs.stack, calleeFP);
  } catch (...) {
    // Manually unwind the stack to the point expected by the Call opcode as
    // defined by its fixup. Note that the callee frame must be either not
    // initialized yet, or already unwound by the handler. In the first case
    // the stack space needs to be discarded. In the second case, the stack
    // does not contain that space anymore.
    assertx(callerFP == vmfp());
    auto& stack = vmStack();
    while (stack.top() < calleeFP) stack.popTV();
    if (stack.top() == calleeFP) stack.discardAR();
    auto const numInOutParams = func->numInOutParamsForArgs(numArgs);
    assertx(stack.top() <= calleeFP + numInOutParams + kNumActRecCells);
    stack.ndiscard(calleeFP + numInOutParams + kNumActRecCells - stack.top());
    assertx(stack.top() == calleeFP + numInOutParams + kNumActRecCells);
    throw;
  }
}

uint32_t fcallRepackHelper(PrologueFlags prologueFlags, const Func* func,
                           uint32_t numPositionalsInclUnpack, TCA savedRip,
                           uintptr_t namedArgNamesPtr) {
  assert_native_stack_aligned();
  const ArrayData* namedArgNames =
    prologueFlags.hasNamedArgs()
      ? reinterpret_cast<const ArrayData*>(namedArgNamesPtr)
      : nullptr;
  auto numArgs = numPositionalsInclUnpack - 1;
  if (namedArgNames) numArgs += namedArgNames->size();
  return withVMRegsForCall<uint32_t>(
      prologueFlags, func, numArgs, true, savedRip,
      [&] (Stack& stack, TypedValue* calleeFP) {
    // Check for stack overflow as we may unpack arbitrary number of arguments.
    if (checkCalleeStackOverflow(calleeFP, func)) {
      throw_stack_overflow();
    }
    // Repack arguments while saving generics.
    GenericsSaver gs{prologueFlags.hasGenerics()};
    return prepareUnpackArgs(func, numPositionalsInclUnpack - 1, true);
  });
}

uint32_t fcallPackExtraHelper(PrologueFlags prologueFlags, const Func* func,
                              uint32_t posArgc, TCA savedRip,
                              uintptr_t namedArgNamesPtr) {
  assert_native_stack_aligned();
  // namedArgNames arg register contains garbage unless hasNamedArgs is set.
  const ArrayData* namedArgNames =
    prologueFlags.hasNamedArgs()
      ? reinterpret_cast<const ArrayData*>(namedArgNamesPtr)
      : nullptr;
  auto numArgs = posArgc;
  if (namedArgNames) numArgs += namedArgNames->size();
  return withVMRegsForCall<uint32_t>(
    prologueFlags, func, numArgs, false, savedRip,
    [&] (Stack& stack, TypedValue* calleeFP) {
      GenericsSaver gs{prologueFlags.hasGenerics()};
      auto const numPositionalParams = func->numPositionalParams();
      // If extra named args are passed, there's nothing to pack.
      if (posArgc <= numPositionalParams) return posArgc;
      auto const numToPack = posArgc - numPositionalParams;
      auto const ad = VanillaVec::MakeVec(numToPack, vmStack().topC());
      vmStack().ndiscard(numToPack);
      vmStack().pushVecNoRc(ad);
      return static_cast<uint32_t>(numPositionalParams) + 1;
  });
}

void fcallHelper(PrologueFlags prologueFlags, Func* func,
                 uint32_t posArgc, void* ctx, TCA savedRip,
                 const uintptr_t namedArgNamesPtr) {
  assert_native_stack_aligned();
  assertx(posArgc <= func->numPositionalParams() + 1);
  const ArrayData* namedArgNames =
    prologueFlags.hasNamedArgs()
      ? reinterpret_cast<const ArrayData*>(namedArgNamesPtr)
      : nullptr;
  auto namedArgNamesSize = namedArgNames ? namedArgNames->size() : 0;
  auto numArgsInclUnpack = posArgc + namedArgNamesSize;
  auto const hasUnpack = numArgsInclUnpack == func->numNonVariadicParams() + 1;
  auto const numArgs = numArgsInclUnpack - (hasUnpack ? 1 : 0);
  withVMRegsForCall<void>(
      prologueFlags, func, numArgs, hasUnpack, savedRip,
      [&](Stack&, TypedValue* calleeFP) {
    // Check for stack overflow in the same place func prologues make their
    // StackCheck::Early check (see irgen-func-prologue.cpp).
    if (checkCalleeStackOverflow(calleeFP, func)) {
      throw_stack_overflow();
    }

    doFCall(prologueFlags, func, namedArgNames, numArgsInclUnpack - namedArgNamesSize, ctx, savedRip);
  });
}

TCA getFuncPrologueHelper(Func* func, int nPassed) {
  return mcgen::getFuncPrologue(func, nPassed).addr();
}

///////////////////////////////////////////////////////////////////////////////

TCA emitFuncPrologueRedispatch(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);

  return vwrap(cb, data, [] (Vout& v) {
    auto const callee = v.makeReg();
    auto const posArgc = v.makeReg();
    v << copy{r_func_prologue_callee(), callee};
    v << copy{r_func_prologue_num_args(), posArgc};

    auto const pTabOff = safe_cast<int32_t>(Func::prologueTableOff());
    auto const ptrSize = safe_cast<int32_t>(sizeof(LowTCA));

    auto const hasNamedParams = v.makeReg();

    auto const paramCounts = v.makeReg();
    auto const paramCountsMinusOne = v.makeReg();
    auto const numNonVariadicParams = v.makeReg();

    v << loadl{callee[Func::paramCountsOff()], paramCounts};
    v << decl{paramCounts, paramCountsMinusOne, v.makeReg()};
    v << shrli{1, paramCountsMinusOne, numNonVariadicParams, v.makeReg()};
    v << testbim{(int32_t)Func::hasNamedParamsMask(), callee[Func::allFlagsOff()], hasNamedParams};
    ifThen(v, CC_Z, hasNamedParams, [&] (Vout& v) {
      auto const sf = v.makeReg();
      // This stub will only get called if named args are absent, so this check is
      // OK.
      v << cmpl{numNonVariadicParams, posArgc, sf};

      auto const pTabOff = safe_cast<int32_t>(Func::prologueTableOff());
      auto const ptrSize = safe_cast<int32_t>(sizeof(LowTCA));

      ifThen(v, CC_LE, sf, [&] (Vout& v) {
          // Fast path (posArgc <= numPositionalParams). Call the posArgc prologue.
          auto const dest = v.makeReg();
          v << loadzlq{callee[posArgc * ptrSize + pTabOff], dest};
          v << jmpr{dest, func_prologue_regs(true, false)};
      });
    });
    // Slow path: either we passed more positional arguments than declared, or
    // the callee has named params. The C++ helper will read the named args,
    // understand how many positionals were passed, pack any extra positional
    // args and dispatch to the appropriate prologue.

    // Need to ensure native stack is aligned for the C++ call.
    v << stublogue{false};

    auto const flags = v.makeReg();
    auto const savedRip = v.makeReg();
    v << copy{r_func_prologue_flags(), flags};
    v << loadstubret{savedRip};
    // Call C++ helper which will pack extra arguments into
    // a vec in-place. The total number of args afterwards
    // will not necessarily be numNonVariadicParams() + 1
    // due to the presence of optional named params.
    auto const newPosArgc = v.makeReg();
    storeVMRegs(v);
    {
      // We don't have named args if we're calling this redispatch stub.
      PhysRegSaver prs{v, RegSet{r_func_prologue_ctx()}};
      auto const namedArgs = v.cns(nullptr); 
      v << vcall{
        CallSpec::direct(fcallPackExtraHelper),
        v.makeVcallArgs({{flags, callee, posArgc, savedRip, namedArgs}}),
        v.makeTuple({newPosArgc}),
        Fixup::none(),
        DestType::SSA
      };
    }
    // Restore all inputs.
    v << copy{flags, r_func_prologue_flags()};
    v << copy{callee, r_func_prologue_callee()};
    v << copy{newPosArgc, r_func_prologue_num_args()};

    // Call the newPosArgc prologue.
    auto const dest = v.makeReg();
    v << loadzlq{Vreg(r_func_prologue_callee())[newPosArgc * ptrSize + pTabOff], dest};
    v << tailcallstubr{dest, func_prologue_regs(true, false)};
  }, name);
}

TCA emitFuncPrologueRedispatchUnpack(CodeBlock& main, CodeBlock& cold,
                                     DataBlock& data, UniqueStubs& us,
                                     const char* name) {
  alignCacheLine(main);
  CGMeta meta;

  auto const start = vwrap2(main, cold, data, meta, [&] (Vout& v, Vout& vc) {
    v << stublogue{false};

    // Save all inputs.
    auto const flags = v.makeReg();
    auto const callee = v.makeReg();
    auto const numPositionals = v.makeReg();
    auto const savedRip = v.makeReg();
    v << copy{r_func_prologue_flags(), flags};
    v << copy{r_func_prologue_callee(), callee};
    v << copy{r_func_prologue_num_args(), numPositionals};
    v << loadstubret{savedRip};

    // Call C++ helper to repack arguments.
    auto const newPosArgc = v.makeReg();
    storeVMRegs(v);
    {
      PhysRegSaver prs{v, RegSet{r_func_prologue_ctx() | r_func_prologue_named_args()}};
      auto const namedArgs = v.makeReg();
      v << copy{r_func_prologue_named_args(), namedArgs};
      auto const done = v.makeBlock();
      auto const ctch = vc.makeBlock();
      v << vinvoke{
        CallSpec::direct(fcallRepackHelper),
        v.makeVcallArgs({{flags, callee, numPositionals, savedRip, namedArgs}}),
        v.makeTuple({newPosArgc}),
        {done, ctch},
        Fixup::indirect(prs.qwordsPushed(), SBInvOffset{0}),
        DestType::SSA
      };

      vc = ctch;
      emitStubCatch(vc, us, [&] (Vout& v) {
        v << lea{rsp()[prs.qwordsPushed() * sizeof(uintptr_t)], rsp()};
        loadVmfp(v);
      });

      v = done;
    }

    // Restore all inputs.
    v << copy{flags, r_func_prologue_flags()};
    v << copy{callee, r_func_prologue_callee()};
    v << copy{newPosArgc, r_func_prologue_num_args()};

    // Call the newPosArgc prologue.
    auto const pTabOff = safe_cast<int32_t>(Func::prologueTableOff());
    auto const ptrSize = safe_cast<int32_t>(sizeof(LowTCA));
    auto const dest = v.makeReg();
    v << loadzlq{callee[newPosArgc * ptrSize + pTabOff], dest};
    v << tailcallstubr{dest, func_prologue_regs(true, true)};
  }, name);

  meta.process(nullptr);
  return start;
}


TCA emitFuncPrologueRedispatchNamedArgs(CodeBlock& main, CodeBlock& cold,
                                        DataBlock& data, UniqueStubs& us,
                                        const char* name) {
  alignCacheLine(main);
  CGMeta meta;

  auto const start = vwrap2(main, cold, data, meta, [&] (Vout& v, Vout& vc) {
    v << stublogue{false};

    // Save all inputs.
    auto const flags = v.makeReg();
    auto const callee = v.makeReg();
    auto const posArgc = v.makeReg();
    auto const savedRip = v.makeReg();

    v << copy{r_func_prologue_flags(), flags};
    v << copy{r_func_prologue_callee(), callee};
    v << copy{r_func_prologue_num_args(), posArgc};

    v << loadstubret{savedRip};
    // Call C++ helper to repack arguments.
    auto const newPosArgc = v.makeReg();
    storeVMRegs(v);
    {
      // This is written unidiomatically, using a PhysRegSaver rather than
      // moving namedArgs back to r_func_prologue_named_args to avoid
      // spilling issues. A more straightforward attempt tripped the "spilling
      // before RSP is set" asserts.
      PhysRegSaver prs{v, RegSet{r_func_prologue_ctx() | r_func_prologue_named_args()}};
      auto const namedArgs = v.makeReg();
      v << copy{r_func_prologue_named_args(), namedArgs};
      auto const done = v.makeBlock();
      auto const ctch = vc.makeBlock();
      v << vinvoke{
        CallSpec::direct(fcallPackExtraHelper),
        v.makeVcallArgs({{flags, callee, posArgc, savedRip, namedArgs}}),
        v.makeTuple({newPosArgc}),
        {done, ctch},
        Fixup::indirect(prs.qwordsPushed(), SBInvOffset{0}),
        DestType::SSA
      };

      vc = ctch;
      emitStubCatch(vc, us, [&] (Vout& v) {
        v << lea{rsp()[prs.qwordsPushed() * sizeof(uintptr_t)], rsp()};
        loadVmfp(v);
      });

      v = done;
    }

    // Restore all inputs.
    v << copy{flags, r_func_prologue_flags()};
    v << copy{callee, r_func_prologue_callee()};
    v << copy{newPosArgc, r_func_prologue_num_args()};

    // Call the newPosArgc prologue.
    auto const pTabOff = safe_cast<int32_t>(Func::prologueTableOff());
    auto const ptrSize = safe_cast<int32_t>(sizeof(LowTCA));
    auto const dest = v.makeReg();
    v << loadzlq{callee[newPosArgc * ptrSize + pTabOff], dest};
    v << tailcallstubr{dest, func_prologue_regs(true, true)};
  }, name);

  meta.process(nullptr);
  return start;
}

TCA emitFCallHelperThunkImpl(CodeBlock& main, CodeBlock& cold,
                             DataBlock& data, UniqueStubs& us,
                             const char* name, bool translate) {
  alignJmpTarget(main);
  CGMeta meta;

  auto const start = vwrap2(main, cold, data, meta, [&] (Vout& v, Vout& vc) {
    v << stublogue{false};

    // Save all inputs.
    auto const callee = v.makeReg();
    auto const numPositionals = v.makeReg();
    v << copy{r_func_prologue_callee(), callee};
    v << copy{r_func_prologue_num_args(), numPositionals};

    if (translate) {
      // Try to JIT the prologue first.
      auto const target = v.makeReg();
      {
        PhysRegSaver prs{v, r_func_prologue_flags() |
                             r_func_prologue_ctx() |
                             r_func_prologue_named_args()};
        v << vcall{
          CallSpec::direct(getFuncPrologueHelper),
          v.makeVcallArgs({{callee, numPositionals}}),
          v.makeTuple({target}),
          Fixup::none(),
          DestType::SSA
        };
      }

      auto const targetSF = v.makeReg();
      v << testq{target, target, targetSF};
      ifThen(v, CC_NZ, targetSF, [&] (Vout& v) {
          // Restore all inputs and call the resolved prologue.
          v << copy{callee, r_func_prologue_callee()};
          v << copy{numPositionals, r_func_prologue_num_args()};
          v << tailcallstubr{target, func_prologue_regs(true, true)};
        });
    }

    auto const flags = v.makeReg();
    auto const ctx = v.makeReg();
    auto const savedRip = v.makeReg();
    auto const namedArgs = v.makeReg();

    v << copy{r_func_prologue_flags(), flags};
    v << copy{r_func_prologue_ctx(), ctx};
    v << loadstubret{savedRip};
    v << copy{r_func_prologue_named_args(), namedArgs};
    // Call C++ helper to perform the equivalent of the func prologue logic.
    auto const done = v.makeBlock();
    auto const ctch = vc.makeBlock();
    storeVMRegs(v);
    v << vinvoke{
      CallSpec::direct(fcallHelper),
      v.makeVcallArgs({{flags, callee, numPositionals, ctx, savedRip, namedArgs}}),
      v.makeTuple({}),
      {done, ctch},
      Fixup::none()
    };

    vc = ctch;
    emitStubCatch(vc, us, [] (Vout& v) { loadVmfp(v); });

    v = done;

    // Use resumeHelper stub to resume the execution. It assumes phplogue{}
    // context, so convert the context first. Note that the VM registers are
    // not synced yet, but that's fine as resumeHelper operates on TLS data.
    v << stubtophp{};
    v << jmpi{
      translate
        ? us.resumeHelperFuncEntryFromInterp
        : us.resumeHelperNoTranslateFuncEntryFromInterp,
      RegSet(rvmtl())
    };
  }, name);

  meta.process(nullptr);
  return start;
}

TCA emitFCallHelperThunk(CodeBlock& main, CodeBlock& cold,
                         DataBlock& data, UniqueStubs& us,
                         const char* name) {
  return emitFCallHelperThunkImpl(main, cold, data, us, name, true);
}
TCA emitFCallHelperNoTranslateThunk(CodeBlock& main, CodeBlock& cold,
                         DataBlock& data, UniqueStubs& us,
                         const char* name) {
  return emitFCallHelperThunkImpl(main, cold, data, us, name, false);
}

TCA emitFunctionSurprised(CodeBlock& main, CodeBlock& cold,
                          DataBlock& data, UniqueStubs& us,
                          const char* name) {
  alignCacheLine(main);

  CGMeta meta;

  auto const start = vwrap2(main, cold, data, meta, [&] (Vout& v, Vout& vc) {
    auto const ar = v.makeReg();

    v << copy{rvmfp(), ar};

    // Fully set up the call frame for the stub.  We can't skip this like we do
    // in other stubs because we need the return IP for this frame in the %rbp
    // chain, in order to find the proper fixup for the VMRegAnchor in the
    // intercept handler.
    v << stublogue{true};
    v << copy{rsp(), rvmfp()};

    auto const done = v.makeBlock();
    auto const ctch = vc.makeBlock();
    auto const interceptRip = v.makeReg();
    uint64_t (*hook)(const ActRec*, int) = &EventHook::onFunctionCallJit;

    v << vinvoke{
      CallSpec::direct(hook),
      v.makeVcallArgs({{ar, v.cns(EventHook::NormalFunc)}}),
      v.makeTuple({interceptRip}),
      {done, ctch},
      Fixup::none(),
      DestType::SSA
    };

    vc = ctch;
    emitStubCatch(vc, us, [] (Vout& v) {
      // Undo our stub frame, so that rvmfp() points to the parent VM frame.
      v << load{rsp()[AROFF(m_sfp)], rvmfp()};
    });

    v = done;

    auto const sf = v.makeReg();
    v << testq{interceptRip, interceptRip, sf};

    auto const rIntercept = rarg(3); // NB: must match cgCheckSurprise*
    assertx(!php_return_regs().contains(rIntercept));

    unlikelyIfThen(v, vc, CC_NZ, sf, [&] (Vout& v) {
      // The event hook has already cleaned up the stack and popped the
      // callee's frame, so we're ready to continue from the original call
      // site.  We just need to sync the rvmfp() and rvmsp() regs, and load
      // the return regs.  Spurious loads if resume helper is used.
      loadVMRegs(v);
      loadReturnRegs(v);

      // Return to CheckSurprise*, which will jump to the intercept rip.
      v << copy{interceptRip, rIntercept};
      v << stubret{RegSet(php_return_regs() | rIntercept), false};
    });

    // Restore rvmfp() and return to the CheckSurprise* logic.
    v << copy{v.cns(0), rIntercept};
    v << stubret{RegSet(php_return_regs() | rIntercept), true};
  }, name);

  meta.process(nullptr);
  return start;
}

TCA emitFunctionSurprisedOrStackOverflow(CodeBlock& main,
                                         CodeBlock& cold,
                                         DataBlock& data,
                                         const UniqueStubs& us,
                                         const char* name) {
  alignJmpTarget(main);

  CGMeta meta;

  auto const start = vwrap2(main, cold, data, meta, [&] (Vout& v, Vout& vc) {
    v << stublogue{};

    auto const done = v.makeBlock();
    auto const ctch = vc.makeBlock();

    v << vinvoke{CallSpec::direct(handlePossibleStackOverflow),
                 v.makeVcallArgs({{rvmfp()}}), v.makeTuple({}),
                 {done, ctch}, Fixup::none()};
    vc = ctch;
    emitStubCatch(vc, us, [] (Vout& v) { loadVmfp(v); });

    v = done;
    v << tailcallstub{us.functionSurprised};
  }, name);

  meta.process(nullptr);
  return start;
}

///////////////////////////////////////////////////////////////////////////////

TCA emitInlineSideExit(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);

  auto const start = vwrap(cb, data, [] (Vout& v) {
    v << store{rvmfp(), Vreg(rvmsp()) + AROFF(m_sfp)};
    v << copy{rvmsp(), rvmfp()};
    v << phplogue{rvmfp()};

    v << storel{rarg(1), Vreg(rvmfp()) + AROFF(m_funcId)};
    v << storel{rarg(2), Vreg(rvmfp()) + AROFF(m_callOffAndFlags)};

    v << jmpr{rarg(0), vm_regs_no_sp()};
  }, name);
  return start;
}

///////////////////////////////////////////////////////////////////////////////

template<bool async>
void loadGenFrame(Vout& v, Vreg d) {
  auto const arOff = BaseGenerator::arOff() -
    (async ? AsyncGenerator::objectOff() : Generator::objectOff());

  auto const gen = v.makeReg();

  // We have to get the Generator object from the current frame's $this, then
  // load the embedded frame.
  v << load{rvmfp()[AROFF(m_thisUnsafe)], gen};
  v << lea{gen[arOff], d};
}

StubEmission emitInterpRet(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);
  auto const start = emitSavedReturnAddressPadding(cb, data);

  auto const entry = vwrap(cb, data, [] (Vout& v) {
    // Load ActRec::m_callOffAndFlags.
    auto const callOffAndFlags = v.makeReg();
    v << loadl{rvmsp()[-kArRetOff + AROFF(m_callOffAndFlags)], callOffAndFlags};

    // Store return value (overwrites ActRec) and sync VM regs.
    storeReturnRegs(v);
    storeVMRegs(v);

    auto const ret = v.makeReg();
    v << vcall{
      CallSpec::direct(svcreq::handlePostInterpRet),
      v.makeVcallArgs({{callOffAndFlags}}),
      v.makeTuple({ret}),
      Fixup::none(),
      DestType::SSA
    };

    loadVMRegs(v);
    loadReturnRegs(v);  // spurious load if we're not returning
    v << jmpr{ret, php_return_regs()};
  }, name);
  return {start, entry};
}

template<bool async>
StubEmission emitInterpGenRet(CodeBlock& cb, DataBlock& data, const char* name) {
  alignJmpTarget(cb);
  auto const start = emitSavedReturnAddressPadding(cb, data);

  auto const entry = vwrap(cb, data, [] (Vout& v) {
    // Load ActRec::m_callOffAndFlags.
    auto const calleeFP = v.makeReg();
    auto const callOffAndFlags = v.makeReg();
    loadGenFrame<async>(v, calleeFP);
    v << loadl{calleeFP[AROFF(m_callOffAndFlags)], callOffAndFlags};

    // Store return value and sync VM regs.
    storeReturnRegs(v);
    storeVMRegs(v);

    auto const ret = v.makeReg();
    v << vcall{
      CallSpec::direct(svcreq::handlePostInterpRet),
      v.makeVcallArgs({{callOffAndFlags}}),
      v.makeTuple({ret}),
      Fixup::none(),
      DestType::SSA
    };

    loadVMRegs(v);
    loadReturnRegs(v);  // spurious load if we're not returning
    v << jmpr{ret, php_return_regs()};
  }, name);
  return {start, entry};
}

///////////////////////////////////////////////////////////////////////////////

template<class Handler>
TCA emitHandleServiceRequest(CodeBlock& cb, DataBlock& data, Handler handler, const char* name) {
  alignCacheLine(cb);

  return vwrap(cb, data, [&] (Vout& v) {
    auto const bcOff = v.makeReg();
    auto const spOff = v.makeReg();
    v << copy{rarg(0), bcOff};
    v << copy{rarg(1), spOff};

    storeVmfp(v);

    auto const ret = v.makeReg();
    v << vcall{
      CallSpec::direct(handler),
      v.makeVcallArgs({{bcOff, spOff}}),
      v.makeTuple({ret}),
      Fixup::none(),
      DestType::SSA
    };

    // We are going to jump either to a translation, or to a resume helper
    // operating in a TC context. The latter requires rvmsp(), the former may
    // require it as well in case of resumables. Luckily, the `handler' synced
    // vmsp(), so load it from there.
    loadVmsp(v);

    v << jmpr{ret, vm_regs_with_sp()};
  }, name);
}

template<class Handler>
TCA emitHandleServiceRequestMainFE(CodeBlock& cb, DataBlock& data,
                                   Handler handler, const char* name) {
    alignCacheLine(cb);
    return vwrap(cb, data, [&] (Vout& v) {
      pushFrameFromFuncEntryRegs(v);
      storeVmfp(v);

      auto const ret = v.makeReg();
      v << vcall{
        CallSpec::direct(handler),
        v.makeVcallArgs({{}}),
        v.makeTuple({ret}),
        Fixup::none(),
        DestType::SSA
      };

      // We are going to jump either to a translation, or to a resume helper
      // operating in a TC context. Both expect the callee frame in func entry
      // regs, so move it there from the VM stack.
      popFrameToFuncEntryRegs(v);
      v << jmpr{ret, func_entry_regs(true /* withCtx */)};
    }, name);
}

template<class Handler>
TCA emitHandleServiceRequestFE(CodeBlock& cb, DataBlock& data,
                               Handler handler, bool pushFrame,
                               const char* name) {
  alignCacheLine(cb);

  return vwrap(cb, data, [&] (Vout& v) {
    auto const numArgs = v.makeReg();
    v << copy{rarg(pushFrame ? 3 : 0), numArgs};

    if (pushFrame) pushFrameFromFuncEntryRegs(v);
    storeVmfp(v);

    auto const ret = v.makeReg();
    v << vcall{
      CallSpec::direct(handler),
      v.makeVcallArgs({{numArgs}}),
      v.makeTuple({ret}),
      Fixup::none(),
      DestType::SSA
    };

    // We are going to jump either to a translation, or to a resume helper
    // operating in a TC context. Both expect the callee frame in func entry
    // regs, so move it there from the VM stack.
    popFrameToFuncEntryRegs(v);
    v << jmpr{ret, func_entry_regs(true /* withCtx */)};
  }, name);
}

TCA emitHandleTranslate(CodeBlock& cb, DataBlock& data, const char* name) {
  return emitHandleServiceRequest(cb, data, svcreq::handleTranslate, name);
}
TCA emitHandleTranslateFE(CodeBlock& cb, DataBlock& data,
                          bool optionalNamedParams, const char* name) {
  return emitHandleServiceRequestFE(
    cb, data,
    optionalNamedParams
      ? svcreq::handleTranslateNamedParamsFuncEntry
      : svcreq::handleTranslateFuncEntry,
    true, name);
}
TCA emitHandleRetranslate(CodeBlock& cb, DataBlock& data, const char* name) {
  return emitHandleServiceRequest(cb, data, svcreq::handleRetranslate, name);
}
TCA emitHandleRetranslateFE(CodeBlock& cb, DataBlock& data,
                            bool optionalNamedParams, const char* name) {
  return emitHandleServiceRequestFE(
    cb, data,
    optionalNamedParams
      ? svcreq::handleRetranslateNamedParamsFuncEntry
      : svcreq::handleRetranslateFuncEntry,
    true, name);
}

TCA emitHandleTranslateMainFE(CodeBlock& cb, DataBlock& data, const char* name) {
  return emitHandleServiceRequestMainFE(
    cb, data, svcreq::handleTranslateMainFuncEntry, name);
}

TCA emitHandleInterpMainFENoTranslate(CodeBlock& cb, DataBlock& data,
                                      const char* name) {
  return emitHandleServiceRequestMainFE(
    cb, data, svcreq::handleInterpMainFuncEntryNoTranslate, name);
}

TCA emitHandleRetranslateOpt(CodeBlock& cb, DataBlock& data,
                             bool optionalNamedParams, const char* name) {
  return emitHandleServiceRequestFE(
    cb, data,
    optionalNamedParams
      ? svcreq::handleRetranslateOptNamedFE
      : svcreq::handleRetranslateOpt,
    false, name);
}

///////////////////////////////////////////////////////////////////////////////

TCA emitBindCallStub(CodeBlock& cb, DataBlock& data, const char* name) {
  return vwrap(cb, data, [] (Vout& v) {
    v << stublogue{false};

    // Save all inputs.
    auto const callee = v.makeReg();
    auto const numArgs = v.makeReg();
    v << copy{r_func_prologue_callee(), callee};
    v << copy{r_func_prologue_num_args(), numArgs};

    // Reconstruct the address of the call from the saved RIP.
    auto const savedRip = v.makeReg();
    auto const toSmash = v.makeReg();
    auto const callLen = safe_cast<int>(smashableCallLen());
    v << loadstubret{savedRip};
    v << subqi{callLen, savedRip, toSmash, v.makeReg()};

    // Call C++ helper to bind the call.
    auto const target = v.makeReg();
    {
      PhysRegSaver prs{v, r_func_prologue_flags()
                          | r_func_prologue_ctx()
                          | r_func_prologue_named_args()};
      v << vcall{
        CallSpec::direct(svcreq::handleBindCall),
        v.makeVcallArgs({{toSmash, callee, numArgs}}),
        v.makeTuple({target}),
        Fixup::none(),
        DestType::SSA
      };
    }

    // Restore all inputs and call the resolved prologue.
    v << copy{callee, r_func_prologue_callee()};
    v << copy{numArgs, r_func_prologue_num_args()};
    v << tailcallstubr{target, func_prologue_regs(true, true)};
  }, name);
}

///////////////////////////////////////////////////////////////////////////////

struct ResumeHelperEntryPoints {
  TCA resumeHelperFromInterp;
  TCA reenterTC;
};

TCA emitResumeHelpers(CodeBlock& cb, DataBlock& data, UniqueStubs& us,
                      ResumeHelperEntryPoints& rh) {
  alignCacheLine(cb);

  auto const handleResume = vwrap(cb, data, [] (Vout& v) {
    loadVmfp(v);

    auto const handler = reinterpret_cast<TCA>(svcreq::handleResume);
    // When using ROAR, emit a smashable call so that ROAR can patch it later.
    if (use_roar) {
      v << calls{handler, arg_regs(1)};
    } else {
      v << call{handler, arg_regs(1)};
    }
    v << fallthru{rret(0) | rret(1)};
  });

  rh.reenterTC = vwrap(cb, data, [] (Vout& v) {
    // Save the return of handleResume(), then sync regs.
    auto const handler = v.makeReg();
    auto const arg = v.makeReg();
    v << copy{rret(0), handler};
    v << copy{rret(1), arg};

    loadVMRegs(v);

    v << copy{arg, rret(1)};
    v << jmpr{handler, vm_regs_with_sp() | rret(1)};
  });

  us.interpToTCFuncEntry = vwrap(cb, data, [&] (Vout& v) {
    auto const handler = v.makeReg();
    v << copy{rret(1), handler};
    popFrameToFuncEntryRegs(v);
    v << jmpr{handler, func_entry_regs(true /* withCtx */)};
  });

  us.interpToTCRet = vwrap(cb, data, [] (Vout& v) {
    auto const handler = v.makeReg();
    v << copy{rret(1), handler};
    loadReturnRegs(v);
    v << jmpr{handler, php_return_regs()};
  });

  auto const emitOne = [&] (svcreq::ResumeFlags flags, TCA& interp, TCA& tc) {
    tc = vwrap(cb, data, [&] (Vout& v) {
      if (flags.m_funcEntry) {
        pushFrameFromFuncEntryRegs(v);
        storeVmfp(v);
        // vmsp() will be synced by handleResume()
      } else {
        storeVMRegs(v);
      }
      v << fallthru{vm_regs_with_sp()};
    });
    interp = vwrap(cb, data, [&] (Vout& v) {
      v << ldimmb{flags.m_asByte, rarg(0)};
      v << jmpi{handleResume, vm_regs_with_sp() | rarg(0)};
    });
  };

#define EMIT(F, N) emitOne(F, us.N##FromInterp, us.N##FromTC)
  EMIT(svcreq::ResumeFlags(), resumeHelper);
  EMIT(svcreq::ResumeFlags().funcEntry(), resumeHelperFuncEntry);
  EMIT(svcreq::ResumeFlags().noTranslate(), resumeHelperNoTranslate);
  EMIT(svcreq::ResumeFlags().noTranslate().funcEntry(),
       resumeHelperNoTranslateFuncEntry);
  EMIT(svcreq::ResumeFlags().interpFirst(), interpHelper);
  EMIT(svcreq::ResumeFlags().interpFirst().funcEntry(), interpHelperFuncEntry);
  EMIT(svcreq::ResumeFlags().noTranslate().interpFirst(),
       interpHelperNoTranslate);
  EMIT(svcreq::ResumeFlags().noTranslate().interpFirst().funcEntry(),
       interpHelperNoTranslateFuncEntry);
#undef EMIT

  rh.resumeHelperFromInterp = us.resumeHelperFromInterp;
  return handleResume;
}

TCA emitInterpOneCFHelper(CodeBlock& cb, DataBlock& data, Op op,
                          const ResumeHelperEntryPoints& rh,
                          const char* name) {
  alignJmpTarget(cb);

  return vwrap(cb, data, [&] (Vout& v) {
    v << copyargs{
      v.makeTuple({rvmfp(), rvmsp()}),
      v.makeTuple({rarg(0), rarg(1)})
    };
    // rarg(2) is set at the stub callsite.

    auto const handler = reinterpret_cast<TCA>(
      interpOneEntryPoints[static_cast<size_t>(op)]
    );
    // When using ROAR, emit a smashable call so ROAR can patch it later.
    if (use_roar) {
      v << calls{handler, arg_regs(3)};
    } else {
      v << call{handler, arg_regs(3)};
    }
    auto const sf = v.makeReg();
    v << testq{rret(0), rret(0), sf};
    ifThenElse(
      v, CC_NZ, sf,
      [&] (Vout& v) { v << jmpi{rh.reenterTC, rret(0) | rret(1)}; },
      [&] (Vout& v) { v << jmpi{rh.resumeHelperFromInterp}; }
    );
  }, name);
}

void emitInterpOneCFHelpers(CodeBlock& cb, DataBlock& data, UniqueStubs& us,
                            CodeCache::View view,
                            const ResumeHelperEntryPoints& rh,
                            const CodeCache& code, Debug::DebugInfo& dbg) {
  alignJmpTarget(cb);

  auto const emit = [&] (Op op, const char* name) {
    tc::TransLocMaker maker{view};
    maker.markStart();
    auto const stub = emitInterpOneCFHelper(cb, data, op, rh, name);
    us.interpOneCFHelpers[op] = stub;
    us.add(name, code, stub, view, maker.markEnd().loc(), dbg);
  };

#define O(name, imm, in, out, flags)          \
  if (((flags) & CF) || ((flags) & TF)) {     \
    emit(Op::name, "interpOneCFHelper"#name); \
  }
  OPCODES
#undef O

  // Exit is a very special snowflake.  Because it can appear in PHP
  // expressions, the emitter pretends that it pushed a value on the eval stack
  // (and iopExit actually does push Null right before throwing).  Marking it
  // as TF would mess up any bytecodes that want to consume its output value,
  // so we can't do that.  But we also don't want to extend regions past it, so
  // the JIT treats it as terminal and uses InterpOneCF to execute it.
  emit(Op::Exit, "interpOneCFHelperExit");
}

///////////////////////////////////////////////////////////////////////////////

TCA emitDecRefGeneric(CodeBlock& cb, DataBlock& data, const char* /*name*/) {
  CGMeta meta;
  alignCacheLine(cb);

  auto const start = vwrap(cb, data, meta, [] (Vout& v) {
    v << vregrestrict{};
    auto const fullFrame = ARCH_MATCH(
      [](arch::X64) { return false; },
      [](arch::ARM) { return true; }
    );
    v << stublogue{fullFrame};
    if (fullFrame) {
      v << copy{rsp(), rvmfp()};
    }

    auto const rdata = rarg(0);
    auto const rtype = rarg(1);

    auto const destroy = [&] (Vout& v) {
      // decRefGeneric is called via callfaststub, whose ABI claims that all
      // registers are preserved.  This is true in the fast path, but in the
      // slow path we need to manually save caller-saved registers.
      auto const callerSaved = abi().gpUnreserved - abi().calleeSaved;
      PhysRegSaver prs{v, callerSaved};

      // Since we've manually saved the caller saved registers, we can
      // use those for Vregs. We use the helper ABI for this stub
      // which only allows caller-saved or reserved registers.
      assertx(callerSaved.contains(rdata));
      assertx(callerSaved.contains(rtype));
      DEBUG_ONLY auto helper_gpUnres = abi(CodeKind::Helper).gpUnreserved;
      DEBUG_ONLY auto std_res = abi().gpReserved;
      assertx(((callerSaved | std_res) & helper_gpUnres) == helper_gpUnres);

      auto const dtor = lookupDestructor(v, rtype);
      v << callm{dtor, arg_regs(1)};

      if (!fullFrame) {
        // The stub frame's saved RIP is at %rsp[8] before we saved the
        // caller-saved registers.
        v << syncpoint{Fixup::indirect(prs.qwordsPushed(), SBInvOffset{0})};
      }
    };

    emitDecRefWork(v, v, rdata, destroy, false, TRAP_REASON);

    v << stubret{{}, fullFrame};
  }, nullptr, CodeKind::Helper);

  meta.process(nullptr);
  return start;
}

///////////////////////////////////////////////////////////////////////////////

namespace {

void alignNativeStack(Vout& v, bool exit) {
  ARCH_MATCH(
    ([&](arch::X64) {
      v << lea{rsp()[exit ? 8 : -8], rsp()};
    }),
    [](arch::ARM) {}
  );
}

}

TCA emitEnterTCExit(CodeBlock& cb, DataBlock& data, UniqueStubs& /*us*/, const char* name) {
  alignCacheLine(cb);

  return vwrap(cb, data, [&] (Vout& v) {
    // Eagerly save VM regs.
    storeVMRegs(v);

    // Store the return value on the top of the eval stack.  Whenever we get to
    // enterTCExit, we're semantically executing some PHP construct that sends
    // a return value out of a function (either a RetC, or a Yield, or an Await
    // that's suspending, etc), and moreover, we must be executing the return
    // that leaves this level of VM reentry (i.e. the only way we get here is
    // by coming from the callToExit stub or by a phpret{} or leavetc{} that
    // undoes the resumetc{} in enterTCHelper).
    //
    // Either way, we have a live PHP return value in the return registers,
    // which we need to put on the top of the evaluation stack.
    storeReturnRegs(v);

    // Restore the registers that need to be saved across enterTC
    auto cross_jit = cross_jit_save();
    if (!(cross_jit.size() & 1)) alignNativeStack(v, true);
    cross_jit.forEachR(
      [&] (PhysReg r) {
        v << pop{r};
      }
    );

    // Perform a native return.
    v << stubret{cross_jit, true};
  }, name);
}

TCA emitEnterTCHelper(CodeBlock& cb, DataBlock& data, UniqueStubs& us, const char* name) {
  alignCacheLine(cb);

  auto const handler = rarg(0);
  auto const arg     = rarg(1);
  auto const tl      = rarg(2);
  auto const firstAR = rarg(3);

  return vwrap2(cb, cb, data, [&] (Vout& v, Vout& vc) {
    // Architecture-specific setup for entering the TC.
    v << inittc{};

    // Native func prologue.
    v << stublogue{true};

    // Set up linkage with the top VM frame in this nesting.
    v << store{rsp(), firstAR[AROFF(m_sfp)]};

    // Save the registers that need to be saved across enterTC
    auto cross_jit = cross_jit_save();
    cross_jit.forEach(
      [&] (PhysReg r) {
        v << push{r};
      }
    );

    // Set up the VM registers.
    v << copy{tl, rvmtl()};
    loadVMRegs(v);

    // Unalign the native stack.
    if (!(cross_jit.size() & 1)) alignNativeStack(v, false);

    // The handler expects its argument in the rret(1) register (see reenterTC).
    v << copy{arg, rret(1)};

    v << resumetc{handler, us.enterTCExit, vm_regs_with_sp() | rret(1)};
  }, name);
}

///////////////////////////////////////////////////////////////////////////////

namespace {

void emitHandleTCUnwindResumeReturn(Vout& v, UniqueStubs& us) {
  v << copy{rret(0), rvmfp()};

  auto const sf = v.makeReg();
  v << testq{rret(1), rret(1), sf};
  ifThenElse(
    v, CC_Z, sf,
    [&] (Vout& v) { v << jmpi{us.resumeCPPUnwind}; },
    [&] (Vout& v) {
      // rret(0) = g_unwind_rds->exn.left()
      auto const exc = v.makeReg();
      auto const sf2 = v.makeReg();
      v << load{rvmtl()[unwinderExnOff()], exc};
      v << testqi{1, exc, sf2};
      v << cmovq{CC_NZ, sf2, exc, v.cns(0), rret(0)};

      v << jmpr{rret(1), vm_regs_with_sp() | rret(0)};
    }
  );
}

}

TCA emitEndCatchHelper(CodeBlock& cb, DataBlock& data, UniqueStubs& us, const char* /*name*/) {
  alignCacheLine(cb);

  CGMeta meta;

  us.resumeCPPUnwind = vwrap(cb, data, meta, [&] (Vout& v) {
    static_assert(sizeof(regState()) == 8,
                  "The following store must match the size of regState().");
    auto const regstate = rvmtl()[rds::kVmRegStateOff];
    v << storeqi{static_cast<int32_t>(VMRegState::CLEAN), regstate};
    // We were about to return to callToExit, so drop the return
    // address, then do what enterTCExit would have done.
    alignNativeStack(v, true);
    auto cross_jit = cross_jit_save();
    if (!(cross_jit.size() & 1)) alignNativeStack(v, true);
    cross_jit.forEachR(
      [&] (PhysReg r) {
        v << pop{r};
      }
    );
    // Under ROAR, emit a smashable call so that ROAR can patch it later.
    if (use_roar) {
      v << calls{
        TCA(__cxxabiv1::__cxa_rethrow),
          arg_regs(0) | cross_jit,
          &us.endCatchHelperPast
      };
    } else {
      v << call{
        TCA(__cxxabiv1::__cxa_rethrow),
          arg_regs(0) | cross_jit,
          &us.endCatchHelperPast
      };
    }
    v << trap{TRAP_REASON, Fixup::none()};
  });
  meta.process(nullptr);

  alignJmpTarget(cb);

  us.endCatchSyncVMSPHelper = vwrap(cb, data, [&] (Vout& v) {
    storeVmsp(v);
    v << fallthru{vm_regs_no_sp()};
  });

  auto const teardownEnter = vwrap(cb, data, [&] (Vout& v) {
    v << copy{v.cns(1LL), rarg(1)};
    v << fallthru{vm_regs_no_sp() | rarg(1)};
  });

  auto const body = vwrap(cb, data, [&] (Vout& v) {
    // Normal end catch situation: call back to tc_unwind_resume, which returns
    // the catch trace (or null) in the first return register, and the new vmfp
    // in the second.
    v << copy{rvmfp(), rarg(0)};

    // Under ROAR, emit a smashable call so that ROAR can patch it later.
    if (use_roar) {
      v << calls{TCA(tc_unwind_resume), arg_regs(2)};
    } else {
      v << call{TCA(tc_unwind_resume), arg_regs(2)};
    }
    emitHandleTCUnwindResumeReturn(v, us);
  });

  us.endCatchTeardownThisSyncVMSP = vwrap(cb, data, [&] (Vout& v) {
    storeVmsp(v);
    v << fallthru{vm_regs_no_sp()};
  });

  us.endCatchTeardownThisHelper = vwrap(cb, data, meta, [&] (Vout& v) {
    auto const thiz = v.makeReg();
    v << load{rvmfp()[AROFF(m_thisUnsafe)], thiz};
    emitDecRefWorkObj(v, thiz, TRAP_REASON);
    if (debug) {
      emitImmStoreq(v, ActRec::kTrashedThisSlot, rvmfp()[AROFF(m_thisUnsafe)]);
    }
    v << copy{v.cns(0LL), rarg(1)};
    v << jmpi{body, vm_regs_no_sp() | rarg(1)};
  });

  us.endCatchSkipTeardownSyncVMSP = vwrap(cb, data, [&] (Vout& v) {
    storeVmsp(v);
    v << fallthru{vm_regs_no_sp()};
  });

  us.endCatchSkipTeardownHelper = vwrap(cb, data, meta, [&] (Vout& v) {
    if (debug) {
      emitImmStoreq(v, ActRec::kTrashedThisSlot, rvmfp()[AROFF(m_thisUnsafe)]);
    }
    v << copy{v.cns(0LL), rarg(1)};
    v << jmpi{body, vm_regs_no_sp() | rarg(1)};
  });

  return teardownEnter;
}

TCA emitEndCatchStublogueHelpers(CodeBlock& cb, DataBlock& data,
                                 UniqueStubs& us) {
  alignJmpTarget(cb);

  us.endCatchStubloguePrologueHelper = vwrap(cb, data, [&] (Vout& v) {
    for (auto i = 0; i < kNumActRecCells; ++i) {
      auto const offset = cellsToBytes(i) + TVOFF(m_type);
      v << storebi{static_cast<int8_t>(KindOfUninit), rvmsp()[offset]};
    }
    v << fallthru{vm_regs_no_sp()};
  });

  us.endCatchStublogueHelper = vwrap(cb, data, [&] (Vout& v) {
    // End catch situation in stublogue context: pop the native frame and
    // pass the curent rvmfp() and saved RIP from the native frame to
    // tc_unwind_resume_stublogue(),which returns the catch trace (or null)
    // in the first return register, and the new vmfp in the second.
    v << copy{rvmfp(), rarg(0)};
    v << stubunwind{rarg(1)};

    // Under ROAR, emit a smashable call so that ROAR can patch it later.
    if (use_roar) {
      v << calls{TCA(tc_unwind_resume_stublogue), arg_regs(2)};
    } else {
      v << call{TCA(tc_unwind_resume_stublogue), arg_regs(2)};
    }

    emitHandleTCUnwindResumeReturn(v, us);
  });

  // Unused.
  return nullptr;
}

TCA emitUnwinderAsyncRet(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);
  alignJmpTarget(cb);
  return vwrap(cb, data, [&] (Vout& v) {
    markRDSAccess(v, g_unwind_rds.handle());
    v << load{rvmtl()[unwinderFSWHOff()], rret_data()};
    v << movzbq{v.cns(KindOfObject), rret_type()};
    v << restoreripm{rvmtl()[unwinderSavedRipOff()]};
    v << load{rvmtl()[rds::kVmspOff], rvmsp()};
    v << ret{php_return_regs()};
  }, name);
}

TCA emitUnwinderAsyncNullRet(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);
  alignJmpTarget(cb);
  return vwrap(cb, data, [&] (Vout& v) {
    markRDSAccess(v, g_unwind_rds.handle());
    v << movzbq{v.cns(KindOfUninit), rret_type()};
    v << restoreripm{rvmtl()[unwinderSavedRipOff()]};
    v << load{rvmtl()[rds::kVmspOff], rvmsp()};
    v << ret{php_return_regs()};
  }, name);
}

namespace {

[[noreturn]] static void throw_exception_while_unwinding() {
  assert_native_stack_aligned();
  if (auto const pendingException = RI().m_pendingException) {
    RI().m_pendingException = nullptr;
    pendingException->throwException();
  }
  assertx(g_unwind_rds->exn.left());
  throw req::root<Object>(Object::attach(g_unwind_rds->exn.left()));
}

} // namespace

TCA emitThrowExceptionWhileUnwinding(CodeBlock& cb, DataBlock& data, const char* name) {
  alignCacheLine(cb);
  alignJmpTarget(cb);

  return vwrap(cb, data, [&] (Vout& v) {
    // The saved rip is the caller is callToExit. We want to skip over
    // callToExit and enterTCExit, so move the stack pointer and set the rbp
    // to be the next frame in the rbp chain.
    alignNativeStack(v, true);
    auto cross_jit = cross_jit_save();
    if (!(cross_jit.size() & 1)) alignNativeStack(v, true);
    cross_jit.forEachR(
      [&] (PhysReg r) {
        v << pop{r};
      }
    );
    v << load{rsp()[0], rvmfp()};
    v << tailcallstub{TCA(throw_exception_while_unwinding), cross_jit};
  }, name);
}

///////////////////////////////////////////////////////////////////////////////

}

void UniqueStubs::emitAll(CodeCache& code, Debug::DebugInfo& dbg) {
  auto view = code.view();
  auto& main = view.main();
  auto& cold = view.cold();
  auto& frozen = view.frozen();
  auto optView = code.view(TransKind::Optimize);
  auto& hotBlock = optView.main();
  auto& data = view.data();

  auto const hot = [&]() -> CodeBlock& {
    return hotBlock.available() > 512 ? hotBlock : main;
  };
  auto const hotView = [&]() -> CodeCache::View& {
    return hotBlock.available() > 512 ? optView : view;
  };

#define EMIT(name, v_in, stub)                                     \
  [&] {                                                            \
    auto const& v = (v_in);                                        \
    tc::TransLocMaker maker{v};                                    \
    maker.markStart();                                             \
    auto const emitted = StubEmission{(stub)()};                   \
    add(name, code, emitted.start, v, maker.markEnd().loc(), dbg);  \
    return emitted.entry;                                        \
  }()

#define ADD(name, v, stub, ...) name = EMIT(#name, v, [&] { return stub(__VA_ARGS__, #name); })
  ADD(enterTCExit, hotView(), emitEnterTCExit, hot(), data, *this);
  enterTCHelper = decltype(enterTCHelper)(
    EMIT("enterTCHelper", view,
      [&] { return emitEnterTCHelper(main, data, *this, "enterTCHelper"); }));

  // These guys are required by a number of other stubs.
  ADD(endCatchHelper, hotView(), emitEndCatchHelper, hot(), data, *this);
  EMIT(
    "endCatchStublogueHelpers",
    hotView(),
    [&] { return emitEndCatchStublogueHelpers(hot(), data, *this); }
  );
  ADD(unwinderAsyncRet, hotView(), emitUnwinderAsyncRet, hot(), data);
  ADD(unwinderAsyncNullRet, hotView(), emitUnwinderAsyncNullRet, hot(), data);
  ADD(throwExceptionWhileUnwinding,
      hotView(),
      emitThrowExceptionWhileUnwinding, hot(), data);

  ADD(funcPrologueRedispatch,
      hotView(),
      emitFuncPrologueRedispatch, hot(), data);
  ADD(funcPrologueRedispatchUnpack,
      hotView(),
      emitFuncPrologueRedispatchUnpack, hot(), cold, data, *this);
  ADD(funcPrologueRedispatchNamedArgs,
      hotView(),
      emitFuncPrologueRedispatchNamedArgs, hot(), cold, data, *this);
  ADD(functionSurprised,
      hotView(),
      emitFunctionSurprised, hot(), cold, data, *this);
  ADD(functionSurprisedOrStackOverflow,
      hotView(),
      emitFunctionSurprisedOrStackOverflow, hot(), cold, data, *this);

  ADD(inlineSideExit, hotView(), emitInlineSideExit, hot(), data);

  ADD(retHelper, hotView(), emitInterpRet, hot(), data);
  ADD(genRetHelper, view, emitInterpGenRet<false>, cold, data);
  ADD(asyncGenRetHelper, hotView(), emitInterpGenRet<true>, hot(), data);

  ADD(handleTranslate, view, emitHandleTranslate, cold, data);
  ADD(handleTranslateFuncEntry, view, emitHandleTranslateFE, cold, data, false);
  ADD(handleTranslateNamedParamsFuncEntry, view, emitHandleTranslateFE, cold, data, true);
  ADD(handleTranslateMainFuncEntry, view, emitHandleTranslateMainFE, cold, data);
  ADD(handleInterpMainFuncEntryNoTranslate, view,
      emitHandleInterpMainFENoTranslate, cold, data);
  ADD(handleRetranslate, view, emitHandleRetranslate, cold, data);
  ADD(handleRetranslateFuncEntry, view, emitHandleRetranslateFE, cold, data, false);
  ADD(handleRetranslateNamedParamsFuncEntry, view, emitHandleRetranslateFE, cold,
      data, true);
  ADD(handleRetranslateOpt, view, emitHandleRetranslateOpt, cold, data, false);
  ADD(handleRetranslateOptNamedFE, view, emitHandleRetranslateOpt, cold, data, true);

  ADD(immutableBindCallStub, view, emitBindCallStub, cold, data);

  ADD(decRefGeneric, hotView(), emitDecRefGeneric, hot(), data);

  ADD(callToExit, hotView(), emitCallToExit, hot(), data, *this);

  EMIT(
    "freeLocalsHelpers",
    hotView(),
    [&] { return emitFreeLocalsHelpers(hot(), data, *this); }
  );

  ResumeHelperEntryPoints rh;
  EMIT(
    "resumeHelpers",
    hotView(),
    [&] { return emitResumeHelpers(hot(), data, *this, rh); }
  );
  emitInterpOneCFHelpers(cold, data, *this, view, rh, code, dbg);

  ADD(fcallHelperThunk,
      view,
      emitFCallHelperThunk, cold, frozen, data, *this);
  ADD(fcallHelperNoTranslateThunk,
      view,
      emitFCallHelperNoTranslateThunk, cold, frozen, data, *this);
#undef ADD

  emitAllResumable(code, dbg);

  tc::updateCodeSizeCounters();
}

///////////////////////////////////////////////////////////////////////////////

void UniqueStubs::add(const char* name,
                      const CodeCache& code,
                      TCA mainStart,
                      CodeCache::View view,
                      TransLoc loc,
                      Debug::DebugInfo& dbg) {
  if (!code.isValidCodeAddress(mainStart)) return;

  auto const& startBlock = code.blockFor(mainStart);

  auto const process = [&] (const CodeBlock& cb, Address start, Address end) {
    if (start == end) return;

    // We may have inserted padding at the beginning, so adjust past it (using
    // the start address).
    if (&cb == &startBlock) start = mainStart;

    if (Trace::moduleEnabledRelease(Trace::ustubs, 1)) {
      std::string disasm;
      if (Trace::moduleEnabledRelease(Trace::ustubs, 2)) {
        std::ostringstream os;
        disasmRange(os, TransKind::Optimize, start, end, 0);
        disasm = os.str() + "\n";
      }
      Trace::ftraceRelease("unique stub: {} @ {} -- {:4} bytes: {}\n{}",
                           cb.name(),
                           static_cast<void*>(start),
                           static_cast<size_t>(end - start),
                           name, disasm);
    }

    auto const isCold = code.inCold(start);
    if (!Cfg::Jit::NoGdb) {
      dbg.recordStub(Debug::TCRange(start, end, isCold),
                     fmt::format("HHVM::{}", name));
    }
    if (Cfg::Jit::UseVtuneAPI) {
      reportHelperToVtune(fmt::format("HHVM::{}", name).c_str(),
                          start,
                          end);
    }
    if (Cfg::Eval::PerfPidMap) {
      dbg.recordPerfMap(Debug::TCRange(start, end, isCold),
                        SrcKey{},
                        fmt::format("HHVM::{}", name));
    }

    registerNativeFunctionName(start, fmt::format("HHVM::{}", name));

    auto const newStub = StubRange{name, start, end};
    auto lower = std::lower_bound(m_ranges.begin(), m_ranges.end(), newStub);

    // We assume ranges are non-overlapping.
    assertx(lower == m_ranges.end() || newStub.end <= lower->start);
    assertx(lower == m_ranges.begin() || (lower - 1)->end <= newStub.start);
    m_ranges.insert(lower, newStub);
  };
  process(view.main(), loc.mainStart(), loc.mainEnd());
  process(view.cold(), loc.coldCodeStart(), loc.coldEnd());
  if (&view.cold() != &view.frozen()) {
    process(view.frozen(), loc.frozenCodeStart(), loc.frozenEnd());
  }
}

std::string UniqueStubs::describe(TCA address) const {
  auto raw = [address] { return fmt::format("{}", fmt::ptr(address)); };
  if (m_ranges.empty()) return raw();

  auto const dummy = StubRange{"", address, nullptr};
  auto lower = std::upper_bound(m_ranges.begin(), m_ranges.end(), dummy);
  if (lower == m_ranges.begin()) return raw();

  --lower;
  if (lower->contains(address)) {
    return fmt::format("{}+{:#x}", lower->name, address - lower->start);
  }
  return raw();
}

///////////////////////////////////////////////////////////////////////////////

RegSet interp_one_cf_regs() {
  return vm_regs_with_sp() | rarg(2);
}

namespace {

void emitUninitDefaultArgs(Vout& v, SrcKey sk, bool withCtx) {
  auto const numEntryArgs = sk.numEntryArgs();
  auto const numPosParams = sk.func()->numPositionalParams();
  assertx(numEntryArgs <= numPosParams);
  if (numEntryArgs == numPosParams) return;

  auto saveRegs = r_func_entry_ar_flags() | r_func_entry_callee_id();
  if (withCtx) saveRegs |= r_func_entry_ctx();
  PhysRegSaver prs{v, saveRegs};

  v << saverips{};

  // The callee's ActRec is at rvmsp().
  v << vcall{
    CallSpec::direct(svcreq::uninitDefaultArgs),
    v.makeVcallArgs({{rvmsp(), v.cns(numEntryArgs),
                      v.cns(sk.func()->numNamedParams()),
                      v.cns(sk.func()->numNonVariadicParams())}}),
    v.makeTuple({}),
    Fixup::none()
  };

  v << restorerips{};
}

void emitInterpReqImpl(Vout& v, SrcKey sk, SBInvOffset spOff, TCA helper) {
  RegSet regSet;
  if (sk.anyFuncEntry()) {
    auto const withCtx =
      sk.func()->isClosureBody() || sk.func()->cls() ||
      Cfg::HHIR::GenerateAsserts;
    emitUninitDefaultArgs(v, sk, withCtx);
    sk.advance();
    regSet = func_entry_regs(withCtx);
  } else if (sk.resumeMode() == ResumeMode::None) {
    auto const frameRelOff = spOff.offset + sk.func()->numSlotsInFrame();
    v << lea{rvmfp()[-cellsToBytes(frameRelOff)], rvmsp()};
    regSet = cross_trace_regs() | rvmsp();
  } else {
    regSet = cross_trace_regs_resumed();
  }
  v << store{v.cns(sk.pc()), rvmtl()[rds::kVmpcOff]};
  v << jmpi{helper, regSet};
}

} // namespace

void emitInterpReq(Vout& v, SrcKey sk, SBInvOffset spOff) {
  auto const helper = sk.anyFuncEntry()
    ? tc::ustubs().interpHelperFuncEntryFromTC
    : tc::ustubs().interpHelperFromTC;
  emitInterpReqImpl(v, sk, spOff, helper);
}

void emitInterpReqNoTranslate(Vout& v, SrcKey sk, SBInvOffset spOff) {
  auto const helper = sk.anyFuncEntry()
    ? tc::ustubs().interpHelperNoTranslateFuncEntryFromTC
    : tc::ustubs().interpHelperNoTranslateFromTC;
  emitInterpReqImpl(v, sk, spOff, helper);
}

///////////////////////////////////////////////////////////////////////////////

}
