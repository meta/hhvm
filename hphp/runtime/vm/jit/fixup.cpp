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

#include "hphp/runtime/vm/jit/fixup.h"

#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/util/arch.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/portability.h"

TRACE_SET_MOD(fixup)

namespace HPHP {

bool isVMFrame(const ActRec* ar, bool may_be_non_runtime) {
  assertx(ar);
  // Determine whether the frame pointer is outside the native stack, cleverly
  // using a single unsigned comparison to do both halves of the bounds check.
  auto const ret = uintptr_t(ar) - s_stackLimit >= s_stackSize;
  assertx(
    !ret ||
    may_be_non_runtime ||
    (ar->func()->validate(), true)
  );
  return ret;
}

//////////////////////////////////////////////////////////////////////

namespace jit {

ActRec* findVMFrameForDebug(uintptr_t start) {
  DECLARE_FRAME_POINTER(framePtr);
  auto rbp = start != 0 ? (ActRec*)start : framePtr;

  while (!isVMFrame(rbp, true)) {
    auto const nextRbp = rbp->m_sfp;
    if (nextRbp == nullptr || nextRbp == rbp) return nullptr;
    rbp = nextRbp;
  }

  return rbp;
}

std::string Fixup::show() const {
  if (!isValid()) return "invalid";
  if (isIndirect()) {
    return fmt::format("indirect ripOff={} extraSpOff={}",
                          ripOffset(), spOffset().offset);
  } else if (isAsioStub()) {
    return fmt::format("asio stub spOff={}", spOffset().offset);
  } else {
    return fmt::format("direct pcOff={} spOff={}",
                          pcOffset(), spOffset().offset);
  }
}

//////////////////////////////////////////////////////////////////////

namespace FixupMap { namespace {

constexpr unsigned kInitCapac = 128;

struct VMRegs {
  PC pc;
  TypedValue* sp;
  const ActRec* fp;
  TCA retAddr;
};

struct FixupHash {
  size_t operator()(uint32_t k) const {
    return hash_int64(k);
  }
};

TreadHashMap<uint32_t,Fixup,FixupHash> s_fixups{kInitCapac};
static ServiceData::ExportedCounter* s_fixupmap_counter =
  ServiceData::createCounter("admin.fixup_map_size");

void regsFromActRec(TCA tca, const ActRec* ar, const Fixup& fixup,
                    SBInvOffset extraSpOffset, VMRegs* outRegs) {
  assertx(!fixup.isIndirect());
  const Func* f = ar->func();
  assertx(f);
  TRACE(3, "regsFromActRec: tca %p -> %s\n", tca, fixup.show().c_str());
  if (fixup.isAsioStub()) {
    auto const prevAR = g_context->getOuterVMFrame(ar);
    auto const prevFunc = prevAR->func();
    outRegs->pc = prevFunc->at(ar->callOffset());
    outRegs->fp = prevAR;
  } else {
    outRegs->pc = f->entry() + fixup.pcOffset();
    outRegs->fp = ar;
  }
  outRegs->retAddr = tca;

  auto const stackBase = Stack::anyFrameStackBase(ar);
  outRegs->sp = stackBase - fixup.spOffset().offset - extraSpOffset.offset;
}

//////////////////////////////////////////////////////////////////////

bool getFrameRegs(VMFrame frame, VMRegs* outVMRegs) {
  auto tca = frame.m_rip;
  auto fixup = s_fixups.find(tc::addrToOffset(tca));
  if (!fixup) return false;

  auto extraSpOffset = SBInvOffset{0};
  if (fixup->isIndirect()) {
    auto const ar = frame.m_prevCfa - kNativeFrameSize;
    TRACE(3, "getFrameRegs: fp %p -> %s\n", (void*) ar, fixup->show().c_str());
    auto ripAddr = ar + fixup->ripOffset();

    // On aarch64, clang release builds may allocate locals above the frame record,
    // which causes the CFA-relative %rip offset of indirect fixups to be incorrect,
    // as they assume that the CFA = fp + kNativeFrameSize.
    // Since the fixup is tied to the VM frame we already hold,
    // scan further up the stack looking for the first ActRec-shaped slot with a matching
    // `m_sfp` whose `m_savedRip` resolves to a valid, non-indirect fixup.
    // If the frame does happen to be immediately preceded by the prior frame,
    // this just matches at the very first candidate.
    if constexpr (arch::any<arch::ARM>()) {
      auto const stackEnd = s_stackLimit + s_stackSize;
      while (ripAddr < stackEnd) {
        auto const sfpSlot = ripAddr - AROFF(m_savedRip) + AROFF(m_sfp);
        if (*reinterpret_cast<ActRec**>(sfpSlot) == frame.m_actRec) {
          auto const candidate = *reinterpret_cast<TCA*>(ripAddr);
          auto const maybeFixup = s_fixups.find(tc::addrToOffset(candidate));
          if (maybeFixup && !maybeFixup->isIndirect()) {
            break;
          }
        }
        ripAddr += sizeof(uintptr_t);
      }
      always_assert(ripAddr < stackEnd &&
                    "Could not locate stub frame for indirect fixup");
    }

    tca = *reinterpret_cast<TCA*>(ripAddr);
    extraSpOffset = fixup->spOffset();
    fixup = s_fixups.find(tc::addrToOffset(tca));
    assertx(fixup && "Missing fixup for indirect fixup");
    assertx(!fixup->isIndirect() && "Invalid doubly indirect fixup");
  }

  // Non-obvious off-by-one fun: if the *return address* points into the TC,
  // then the frame we were running on in the TC is actually the previous
  // frame.
  auto const nextAR = frame.m_actRec;
  regsFromActRec(tca, nextAR, *fixup, extraSpOffset, outVMRegs);
  return true;
}

//////////////////////////////////////////////////////////////////////
}

void recordFixup(CTCA tca, const Fixup& fixup) {
  TRACE(3, "recordFixup: tca %p -> %s\n", tca, fixup.show().c_str());

  assertx(fixup.isValid());
  auto const offset = tc::addrToOffset(tca);
  if (auto pos = s_fixups.find(offset)) {
    *pos = fixup;
  } else {
    s_fixups.insert(offset, fixup);
    s_fixupmap_counter->increment();
  }
}

const Fixup* findFixup(CTCA tca) {
  auto const fixup = s_fixups.find(tc::addrToOffset(tca));
  return fixup ? &*fixup : nullptr;
}

size_t size() { return s_fixups.size(); }

bool processFixupForVMFrame(VMFrame frame) {
  assertx(isVMFrame(frame.m_actRec, false));

  VMRegs regs;
  if (!getFrameRegs(frame, &regs)) return false;

  TRACE(2, "fixup(end): func %s fp %p sp %p pc %p retAddr %p\n",
      regs.fp->func()->name()->data(),
      regs.fp, regs.sp, regs.pc, regs.retAddr);
  auto& vmRegs = vmRegsUnsafe();
  vmRegs.fp = const_cast<ActRec*>(regs.fp);
  vmRegs.pc = reinterpret_cast<PC>(regs.pc);
  vmRegs.stack.top() = regs.sp;
  vmRegs.jitReturnAddr = regs.retAddr;
  return true;
}

bool fixupWork(ActRec* nextRbp, bool soft) {
  assertx(Cfg::Jit::Enabled);

  TRACE(1, "fixup(begin):\n");

  while (true) {
    auto const rbp = nextRbp;
    nextRbp = rbp->m_sfp;

    if (UNLIKELY(soft) && (!nextRbp || nextRbp == rbp)) return false;
    assertx(nextRbp && nextRbp != rbp && "Missing fixup for native call");

    TRACE(2, "considering frame %p, %p\n", rbp, (void*)rbp->m_savedRip);

    if (isVMFrame(nextRbp, soft)) {
      TRACE(2, "fixup checking vm frame %s\n",
            nextRbp->func()->name()->data());
#ifdef HHVM_RECORD_JIT_CFA
      auto const cfa = vmJitCfa();
#else
      auto const cfa = uintptr_t(rbp) + kNativeFrameSize;
#endif
      auto const frame = VMFrame{nextRbp, TCA(rbp->m_savedRip), cfa};
      auto const res = processFixupForVMFrame(frame);
      if (res || LIKELY(soft)) return res;
      always_assert(false && "Fixup expected for leafmost VM frame");
    }
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
}

namespace detail {
void syncVMRegsWork(bool soft) {
  // Start looking for fixup entries at the current (C++) frame.  This
  // will walk the frames upward until we find a TC frame.
  DECLARE_FRAME_POINTER(framePtr);
  auto fp = regState() >= VMRegState::GUARDED_THRESHOLD ?
    (ActRec*)regState() : framePtr;

  auto const synced = FixupMap::fixupWork(fp, soft);

  if (synced) regState() = VMRegState::CLEAN;
  Stats::inc(Stats::TC_Sync);
}
}

static std::atomic<int32_t> s_nextFakeAddress{-1};

int32_t getNextFakeReturnAddress() {
  return s_nextFakeAddress.fetch_sub(1, std::memory_order_acq_rel);
}

}}
