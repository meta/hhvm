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

#include "hphp/runtime/vm/jit/align-arm.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/phys-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

#include "hphp/vixl/hphp-compat.h"

namespace HPHP::jit {

struct CGMeta;

namespace arm {

///////////////////////////////////////////////////////////////////////////////

/*
 * Mirrors the API of smashable-instr.h.
 */

/*
 * Maximum number of bytes in the instruction sequence. Smashable movq emits a
 * direct LDR normally and ADRP+LDR when far literals are enabled.
 */
constexpr size_t smashableMovqLen() { return 8; } // ADRP + LDR for far literals
constexpr size_t smashableCmpqLen() { return 0; }
constexpr size_t smashableCallLen() { return 4; } // not including veneer
constexpr size_t smashableJmpLen()  { return 4; } // not including veneer
constexpr size_t smashableJccLen()  { return 4; } // not including veneer
constexpr size_t smashableInterceptLen() { return 4; } // not including veneer

void emitPooledLiteralLoad(vixl::MacroAssembler& a,
                           CodeBlock& cb,
                           CGMeta& meta,
                           uint64_t val,
                           vixl::Register dst,
                           uint8_t width,
                           bool smashable,
                           bool emitFarLiteral);

/*
 * Inline lengths exclude the optional ADRP prefix: the LDR + BLR/BR pair is
 * always 8 bytes, while the full sequence with a far literal is 12 bytes.
 * This lets smashableCallFromRet() subtract a fixed length and recover the
 * LDR slot in either form. Literal decoding finds the preceding ADRP when
 * needed.
 */
constexpr size_t smashableCallLenInline() { return 8; }
constexpr size_t smashableCallLenVeneer() { return 4; }
size_t smashableCallLenActual(); // runtime-dispatched

constexpr size_t smashableJmpLenInline() { return 8; }
constexpr size_t smashableJmpLenVeneer() { return 4; }
size_t smashableJmpLenActual();

TCA emitSmashableMovq(CodeBlock& cb, CGMeta& meta, uint64_t imm,
                      PhysReg d, bool emitFarLiteral = false);
TCA emitSmashableCall(CodeBlock& cb, CGMeta& meta, TCA target,
                      bool emitFarLiteral);
TCA emitSmashableCallInline(CodeBlock& cb, CGMeta& meta, TCA target,
                            bool emitFarLiteral);
TCA emitSmashableCallVeneer(CodeBlock& cb, CGMeta& meta, TCA target);
TCA emitSmashableJmp(CodeBlock& cb, CGMeta& meta, TCA target,
                     bool emitFarLiteral);
TCA emitSmashableJmpInline(CodeBlock& cb, CGMeta& meta, TCA target,
                           bool emitFarLiteral);
TCA emitSmashableJmpVeneer(CodeBlock& cb, CGMeta& meta, TCA target);
TCA emitSmashableJcc(CodeBlock& cb, CGMeta& meta, TCA target,
                     ConditionCode cc);
void smashMovq(TCA inst, uint64_t imm);
void smashCmpq(TCA inst, uint32_t imm);
void smashCall(TCA inst, TCA target);
void smashCallInline(TCA inst, TCA target);
void smashCallVeneer(TCA inst, TCA target);
void smashJmp(TCA inst, TCA target);
void smashJmpInline(TCA inst, TCA target);
void smashJmpVeneer(TCA inst, TCA target);
void smashJcc(TCA inst, TCA target);
void smashInterceptJcc(TCA inst);
void smashInterceptJmp(TCA inst);

bool possiblySmashableMovq(TCA inst);
bool possiblySmashableCall(TCA inst);
bool possiblySmashableCallInline(TCA inst);
bool possiblySmashableCallVeneer(TCA inst);
bool possiblySmashableJmp(TCA inst);
bool possiblySmashableJmpInline(TCA inst);
bool possiblySmashableJmpVeneer(TCA inst);
bool possiblySmashableJcc(TCA inst);

uint64_t smashableMovqImm(TCA inst);
uint32_t smashableCmpqImm(TCA inst);
TCA smashableCallTarget(TCA inst);
TCA smashableCallTargetInline(TCA inst);
TCA smashableCallTargetVeneer(TCA inst);
TCA smashableCallLiteral(TCA inst);
TCA smashableCallStart(TCA inst);
TCA smashableJmpTarget(TCA inst);
TCA smashableJmpTargetInline(TCA inst);
TCA smashableJmpTargetVeneer(TCA inst);
TCA smashableJccTarget(TCA inst);
ConditionCode smashableJccCond(TCA inst);

bool optimizeSmashedCall(TCA inst);
bool optimizeSmashedCallInline(TCA inst);
bool optimizeSmashedJmp(TCA inst);
bool optimizeSmashedJmpInline(TCA inst);
bool optimizeSmashedJcc(TCA inst);

constexpr size_t kSmashMovqImmOff = 0;
constexpr size_t kSmashCmpqImmOff = 0;
constexpr size_t kSmashCallTargetOff = 0;
constexpr size_t kSmashJmpTargetOff = 0;
constexpr size_t kSmashJccTargetOff = 0;

///////////////////////////////////////////////////////////////////////////////

template <class T>
inline uint32_t makeTarget32(T target) {
  assertx(!(reinterpret_cast<intptr_t>(target) >> 32));
  return static_cast<uint32_t>(reinterpret_cast<intptr_t>(target));
}

inline void patchTarget32(TCA inst, TCA target) {
  *reinterpret_cast<uint32_t*>(inst) = makeTarget32(target);
  auto const begin = inst;
  auto const end = begin + 4;
  DataBlock::syncDirect(begin, end);
}

inline void patchTarget64(TCA inst, TCA target) {
  assertx(is_aligned(inst - kSmashMovqImmOff, Alignment::SmashMovq));
  *reinterpret_cast<uint64_t*>(inst) = reinterpret_cast<uint64_t>(target);
  auto const begin = inst;
  auto const end = begin + 8;
  DataBlock::syncDirect(begin, end);
}

/*
 * Read a smashable literal, to be paired with casTarget32() below.
 */
inline uint32_t readTarget32(TCA inst) {
  assertx((reinterpret_cast<uintptr_t>(inst) & 3) == 0);
  return __atomic_load_n(reinterpret_cast<uint32_t*>(inst), __ATOMIC_RELAXED);
}

/*
 * Claim a smashable site: move its literal to `target`, but only if it still
 * holds `expected`, which the caller read before it began.  A false return
 * means another thread moved the literal in the meantime and is smashing this
 * site, so this one must leave it alone -- in particular it must not collapse
 * the branch or retire the load, or the two threads would disagree about
 * which target the site has.
 *
 * Sound as a claim because a literal only ever moves from one target to
 * another and never back to a value it held before, so a successful exchange
 * cannot be an ABA.  Finding it already set to `target` is a loss too: this
 * thread read the slot before it began, so a value it did not write means
 * another thread got there first and owns the site.
 */
inline bool casTarget32(TCA inst, uint32_t expected, TCA target) {
  assertx((reinterpret_cast<uintptr_t>(inst) & 3) == 0);
  auto const want = makeTarget32(target);
  if (expected == want) return false;
  auto* const slot = reinterpret_cast<uint32_t*>(inst);
  if (!__atomic_compare_exchange_n(slot, &expected, want, false,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    return false;
  }
  DataBlock::syncDirect(inst, inst + 4);
  return true;
}

inline void smashInst(TCA addr, uint32_t newInst) {
  assertx(((uint64_t)addr & 3) == 0);
  *reinterpret_cast<uint32_t*>(addr) = newInst;
  auto const begin = addr;
  auto const end = begin + 4;
  DataBlock::syncDirect(begin, end);
}

}}
