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

#include "hphp/runtime/vm/jit/unique-stubs-x64.h"

#include "hphp/runtime/base/header-kind.h"
#include "hphp/runtime/base/stats.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi-x64.h"
#include "hphp/runtime/vm/jit/align-x64.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/phys-reg-saver.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"

#include "hphp/util/address-range.h"
#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/data-block.h"
#include "hphp/util/trace.h"

namespace HPHP::jit {

TRACE_SET_MOD(ustubs)

///////////////////////////////////////////////////////////////////////////////

namespace x64 {

///////////////////////////////////////////////////////////////////////////////

/*
 * Helper for the freeLocalsHelpers which does the actual work of decrementing
 * a value's refcount or releasing it.
 *
 * This helper is reached via call from the various freeLocalHelpers.
 * It expects `dataVal' to be a counted Value, and `typeVal' to be the
 * associated DataType.
 *
 * The `live' registers must be preserved across any native calls (and
 * generally left untouched).
 */
static TCA emitDecRefHelper(CodeBlock& cb, DataBlock& data,
                            PhysReg dataVal, PhysReg typeVal, RegSet live) {
  CGMeta meta;
  auto addr = vwrap(cb, data, meta, [&] (Vout& v) {
    auto emit_destroy = [&](Vout& v) {
      // Note that the stack is aligned since we called to this helper from an
      // stack-unaligned stub.
      PhysRegSaver prs{v, live};

      // The refcount is exactly 1; release the value.
      // Avoid 'this' pointer overwriting by reserving it as an argument.
      assertx(dataVal == rarg(0));
      v << callm{lookupDestructor(v, typeVal, true), arg_regs(1)};

      // Between where %rsp is now and the saved RIP of the call into the
      // freeLocalsHelpers stub, we have all the live regs we pushed, plus the
      // saved RIP of the call from the stub to this helper.
      v << syncpoint{Fixup::indirect(prs.qwordsPushed(), SBInvOffset{0})};
    };

    auto const sf = emitCmpRefCount(v, OneReference, dataVal);

    ifThen(v, CC_NE, sf, [&] (Vout& v) {
      emitDecRefCount(v, dataVal);
      v << ret{live};
    });

    emit_destroy(v);
    v << ret{live};
  }, nullptr, CodeKind::CrossTrace, true);

  meta.process(nullptr);
  return addr;
}

TCA emitFreeLocalsHelpers(CodeBlock& cb, DataBlock& data, UniqueStubs& us) {
  // The address of the first local's type is passed in the second
  // argument register. The address of the first local's value is
  // passed in the third argument register. We use the first to store
  // the loaded value (so its already in the right place when calling
  // the release function).  We use the fourth as a scratch register
  // for loading the type and the fifth as a scratch register for
  // storing the end pointer.
  auto const dataVal = rarg(0);
  auto const typePtr = rarg(1);
  auto const dataPtr = rarg(2);
  auto const typeVal = rarg(3);
  auto const end = rarg(4);
  auto const start = cb.frontier();
  // We want the release function to come last; we enter the slide at
  // several different points, but always execute through to the end
  // of the slide - so its better to put the end of the slide close to
  // the release helper. Since we don't know exactly where the release
  // helper will be, use a fake address that we can recognize and
  // fixup after the fact.
  auto const releaseFake = start - 1;

  auto const decref_local = [&] (Vout& v, Vptr d, Vptr t) {
    auto const sf = v.makeReg();

    // We can't do a byte load here---we have to sign-extend since we use
    // `type' as a 64-bit array index to the destructor table.
    v << loadsbq{t, typeVal};
    auto const cc = emitIsTVTypeRefCounted(v, sf, typeVal);

    ifThen(v, cc, sf, [&] (Vout& v) {
      v << load{d, dataVal};
      auto const counted = v.makeReg();
      v << shrqi{int(kSharedMaxShift), dataVal, v.makeReg(), counted};
      ifThen(v, CC_NZ, counted, [&] (Vout& v) {
        v << call{releaseFake, dataVal | typeVal, nullptr /* watch */,
                  true /* stackUnaligned */};
      });
    });
  };

  us.freeManyLocalsHelper = vwrap(cb, data, [&] (Vout& v) {
    // We always unroll the final `kNumFreeLocalsHelpers' decrefs, so only loop
    // until we hit that point.
    v << lea{ptrToLocalType(rvmfp(), kNumFreeLocalsHelpers - 1), end};
    doWhile(v, CC_NZ, {}, [&](const VregList&, const VregList&) {
      decref_local(v, *dataPtr, *typePtr);
      auto const sf = v.makeReg();
      prevLocal(v, typePtr, dataPtr, typePtr, dataPtr);
      v << cmpq{typePtr, end, sf};
      return sf;
    });
  }, nullptr, true);

  for (auto i = kNumFreeLocalsHelpers - 1; i >= 0; --i) {
    us.freeLocalsHelpers[i] = vwrap(cb, data, [&] (Vout& v) {
      decref_local(v, ptrToLocalData(rvmfp(), i), ptrToLocalType(rvmfp(), i));
      if (i == 0) {
        // The helpers all fall through to each other. Only the last
        // one needs a ret.
        v << ret{};
      } else {
        v << fallthru{};
      }
    }, nullptr, true);
  }

  auto const release = emitDecRefHelper(
    cb, data, dataVal, typeVal,
    dataPtr | typePtr | end
  );
  // Now we know where release is, we can patch the calls to
  // releaseFake and point them to the correct address.
  for (auto addr = start; addr < release; ) {
    x64::DecodedInstruction di(addr, addr);
    if (di.hasPicOffset() && di.picAddress() == releaseFake) {
      always_assert(di.isCall());
      di.setPicAddress(release);
    }
    addr += di.size();
  }

  return start;
}

///////////////////////////////////////////////////////////////////////////////

EXTERNALLY_VISIBLE
void assert_tc_saved_rip(void* sp) {
  auto const saved_rip = *reinterpret_cast<uint8_t**>(sp);
  auto const exittc = tc::ustubs().enterTCExit;

  DecodedInstruction di(saved_rip);
  auto const jmp_target = [&] { return saved_rip + di.size() + di.offset(); };

  // We should either be returning to enterTCExit, or to a jmp to enterTCExit.
  always_assert(saved_rip == exittc || (di.isJmp() && jmp_target() == exittc));
}

TCA emitCallToExit(CodeBlock& cb, DataBlock& /*data*/, const UniqueStubs& us) {
  X64Assembler a(cb);

  auto const start = a.frontier();
  if (Cfg::HHIR::GenerateAsserts) {
    always_assert(rarg(0) != rret(0) &&
                  rarg(0) != rret(1));
    a.movq(rsp(), rarg(0));

    // We need to spill the return registers around the assert call.
    a.push(rret(0));
    a.push(rret(1));
    auto target = TCA(assert_tc_saved_rip);
    if (a.jmpDeltaFits(target)) {
      a.call(target);
    } else {
      a.emitImmReg(target, reg::rax);
      a.call(reg::rax);
    }
    a.pop(rret(1));
    a.pop(rret(0));
  }

  // Emulate a ret to enterTCExit without actually doing one to avoid
  // unbalancing the return stack buffer. The call from enterTCHelper() that
  // got us into the TC was popped off the RSB by the ret that got us to this
  // stub.
  a.addq(8, rsp());
  if (a.jmpDeltaFits(us.enterTCExit)) {
    a.jmp(us.enterTCExit);
  } else {
    // can't do a near jmp and a rip-relative load/jmp would require threading
    // through extra state to allocate a literal. use an indirect jump through
    // a register
    a.emitImmReg(us.enterTCExit, reg::rax);
    a.jmp(reg::rax);
  }

  return start;
}

///////////////////////////////////////////////////////////////////////////////

}}
