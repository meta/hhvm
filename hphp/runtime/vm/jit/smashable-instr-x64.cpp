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

#include "hphp/runtime/vm/jit/smashable-instr-x64.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

#include <folly/MicroSpinLock.h>

namespace HPHP::jit::x64 {

///////////////////////////////////////////////////////////////////////////////

/*
 * On x64, concurrent modification and execution of instructions is safe if all
 * of the following hold:
 *
 *  1/  The modification is done with a single processor store.
 *  2/  Only one instruction in the original stream is modified.
 *  3/  The modified instruction does not cross a cacheline boundary.
 *
 * Cache alignment is required for mutable instructions to make sure mutations
 * don't "tear" on remote CPUs.
 */

#define EMIT_BODY(cb, inst, Inst, ...)  \
  ([&] {                                \
    align(cb, &fixups,                  \
          Alignment::Smash##Inst,       \
          AlignContext::Live);          \
    auto const theStart = cb.frontier();\
    X64Assembler a(cb);                 \
    a.inst(__VA_ARGS__);                \
    return theStart;                    \
  }())

// Far-literal encodings are ARM-only; x64 movq embeds its immediate directly.
TCA emitSmashableMovq(CodeBlock& cb, CGMeta& fixups, uint64_t imm,
                      PhysReg d, bool /*emitFarLiteral*/) {
  auto const start =
    EMIT_BODY(cb, movq, Movq, 0xdeadbeeffeedface, d);

  auto frontier = cb.toDestAddress(cb.frontier());
  auto immp = reinterpret_cast<uint64_t*>(
    frontier - smashableMovqLen() + kSmashMovqImmOff
  );
  *immp = imm;

  return start;
}

TCA emitSmashableCall(CodeBlock& cb, CGMeta& fixups, TCA target,
                      bool /*emitFarLiteral*/) {
  return EMIT_BODY(cb, call, Call, target);
}

TCA emitSmashableJmp(CodeBlock& cb, CGMeta& fixups, TCA target,
                     bool /*emitFarLiteral*/) {
  return EMIT_BODY(cb, jmp, Jmp, target);
}

TCA emitSmashableJcc(CodeBlock& cb, CGMeta& fixups, TCA target,
                     ConditionCode cc) {
  assertx(cc != CC_None);
  return EMIT_BODY(cb, jcc, Jcc, cc, target);
}

#undef EMIT_BODY

///////////////////////////////////////////////////////////////////////////////

void smashMovq(TCA inst, uint64_t imm) {
  always_assert(is_aligned(inst, Alignment::SmashMovq));
  *reinterpret_cast<uint64_t*>(inst + kSmashMovqImmOff) = imm;
}

void smashCmpq(TCA inst, uint32_t imm) {
  always_assert(is_aligned(inst, Alignment::SmashCmpq));
  *reinterpret_cast<uint32_t*>(inst + kSmashCmpqImmOff) = imm;
}

void smashCall(TCA inst, TCA target) {
  always_assert(is_aligned(inst, Alignment::SmashCall));
  X64Assembler::patchCall(inst, inst, target);
}

void smashJmp(TCA inst, TCA target) {
  always_assert(is_aligned(inst, Alignment::SmashJmp));

  // Smashing jmps can be tricky because we sometimes override the entire five
  // byte instruction with a nop rather than updating the four byte immediate.
  // In order to both leave the instruction in a valid state and avoid writing
  // to someone else's bytes we perform a four and a one byte write. This should
  // generally be safe (see below), but can be racy with multiple threads
  // writing to the same jmp. Grab a spin lock to serialize smashJmp calls.
  static folly::MicroSpinLock s_lock;
  folly::MSLGuard g{s_lock};

  if (target > inst && target - inst <= smashableJmpLen()) {
    // Using emitNop here would write eight bytes to smash the 5 byte Jmp,
    // unfortunately this is racy without the codeLock, so instead emit the
    // nop using a single machine instruction writing four bytes (the final
    // two bytes of the nop aren't required to be anything specific).
    *reinterpret_cast<uint32_t*>(inst) = 0x00441f0f;

    // While zeroing the final bytes is not a requirement intel does recommend
    // that this canonical form be used. So as this is not required for
    // correctness, it's okay for it to not be atomic with the above operation.
    *reinterpret_cast<uint8_t*>(inst + 4) = 0x00;
  } else {
    // We may be unsmashing a nop. We have to be careful here, to ensure that
    // we always leave the instruction in a consistent state.
    auto const imm = safe_cast<int32_t>(target - (inst + 5));
    if (inst[0] != 0xe9) {
      assertx(inst[0] == 0x0f && inst[1] == 0x1f && inst[2] == 0x44);

      auto const last = 0xff & (imm >> 24);
      auto const first = 0xe9 | ((size_t(imm) & 0xffffffULL) << 8);

      // As noted above, storing to the final two bytes of our five byte nop
      // will still allow it to remain a nop, so we can write to the final byte
      // and then atomically set the remaining four bytes, smashing in our new
      // jmp instruction.
      *reinterpret_cast<uint8_t*>(inst + 4) = last;
      *reinterpret_cast<uint32_t*>(inst) = first;
    } else {
      *reinterpret_cast<uint32_t*>(inst + 1) = imm;
    }
  }
}

void smashJcc(TCA inst, TCA target) {
  always_assert(is_aligned(inst, Alignment::SmashJcc));
  X64Assembler::patchJcc(inst, inst, target);
}

void smashInterceptJcc(TCA inst) {
  X64Assembler::patchInterceptJcc(inst);
}

void smashInterceptJmp(TCA inst) {
  X64Assembler::patchInterceptJmp(inst);
}

///////////////////////////////////////////////////////////////////////////////

uint64_t smashableMovqImm(TCA inst) {
  return *reinterpret_cast<uint64_t*>(inst + kSmashMovqImmOff);
}

uint32_t smashableCmpqImm(TCA inst) {
  return *reinterpret_cast<uint32_t*>(inst + kSmashCmpqImmOff);
}

TCA smashableCallTarget(TCA inst) {
  if (inst[0] != 0xE8) return nullptr;
  return inst + 5 + ((int32_t*)(inst + 5))[-1];
}

TCA smashableCallLiteral(TCA /*inst*/) {
  // A smashable call on x64 holds its target in the displacement, so there is
  // no separate pointer to hand out.
  return nullptr;
}

TCA smashableJmpTarget(TCA inst) {
  if (inst[0] != 0xe9) {
    if (inst[0] == 0x0f &&
        inst[1] == 0x1f &&
        inst[2] == 0x44) {
      // 5 byte nop
      return inst + 5;
    }
    return nullptr;
  }
  return inst + 5 + ((int32_t*)(inst + 5))[-1];
}

TCA smashableJccTarget(TCA inst) {
  if (inst[0] != 0x0F || (inst[1] & 0xF0) != 0x80) return nullptr;
  return inst + 6 + ((int32_t*)(inst + 6))[-1];
}

ConditionCode smashableJccCond(TCA inst) {
  return DecodedInstruction(inst).jccCondCode();
}

///////////////////////////////////////////////////////////////////////////////

bool optimizeSmashedCall(TCA inst) {
  return false;
}

bool optimizeSmashedJmp(TCA inst) {
  return false;
}

bool optimizeSmashedJcc(TCA inst) {
  return false;
}

///////////////////////////////////////////////////////////////////////////////

}
