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

#include "hphp/runtime/vm/jit/smashable-instr.h"

#ifdef __aarch64__
#include "hphp/runtime/vm/jit/smashable-instr-arm.h"
#else
#include "hphp/runtime/vm/jit/smashable-instr-x64.h"
#endif
#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/util/arch.h"
#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

size_t smashableMovqLen() {
  return ARCH_SWITCH_CALL(smashableMovqLen);
}
size_t smashableCmpqLen() {
  return ARCH_SWITCH_CALL(smashableCmpqLen);
}
size_t smashableCallLen() {
  return ARCH_SWITCH_CALL(smashableCallLenActual);
}
size_t smashableJmpLen() {
  return ARCH_SWITCH_CALL(smashableJmpLenActual);
}
size_t smashableJccLen() {
  return ARCH_SWITCH_CALL(smashableJccLen);
}

TCA emitSmashableMovq(CodeBlock& cb, CGMeta& fixups, uint64_t imm,
                      PhysReg d, bool emitFarLiteral) {
  return ARCH_SWITCH_CALL(emitSmashableMovq,
                          cb,
                          fixups,
                          imm,
                          d,
                          emitFarLiteral);
}
TCA emitSmashableCall(CodeBlock& cb, CGMeta& fixups, TCA target,
                      bool emitFarLiteral) {
  always_assert(target);
  return ARCH_SWITCH_CALL(emitSmashableCall, cb, fixups, target, emitFarLiteral);
}
TCA emitSmashableJmp(CodeBlock& cb, CGMeta& fixups, TCA target,
                     bool emitFarLiteral) {
  always_assert(target);
  return ARCH_SWITCH_CALL(emitSmashableJmp, cb, fixups, target, emitFarLiteral);
}
TCA emitSmashableJcc(CodeBlock& cb, CGMeta& fixups, TCA target,
                     ConditionCode cc) {
  always_assert(target);
  return ARCH_SWITCH_CALL(emitSmashableJcc, cb, fixups, target, cc);
}

void smashMovq(TCA inst, uint64_t imm) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashMovq, inst, imm);
}
void smashCmpq(TCA inst, uint32_t imm) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashCmpq, inst, imm);
}
void smashCall(TCA inst, TCA target) {
  always_assert(inst);
  always_assert(target);
  return ARCH_SWITCH_CALL(smashCall, inst, target);
}
void smashJmp(TCA inst, TCA target) {
  always_assert(inst);
  always_assert(target);
  return ARCH_SWITCH_CALL(smashJmp, inst, target);
}
void smashJcc(TCA inst, TCA target) {
  always_assert(inst);
  always_assert(target);
  return ARCH_SWITCH_CALL(smashJcc, inst, target);
}
void smashInterceptJcc(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashInterceptJcc, inst);
}
void smashInterceptJmp(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashInterceptJmp, inst);
}
uint64_t smashableMovqImm(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableMovqImm, inst);
}
uint32_t smashableCmpqImm(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableCmpqImm, inst);
}
TCA smashableCallTarget(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableCallTarget, inst);
}
TCA smashableCallLiteral(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableCallLiteral, inst);
}
TCA smashableJmpTarget(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableJmpTarget, inst);
}
TCA smashableJccTarget(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableJccTarget, inst);
}
ConditionCode smashableJccCond(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableJccCond, inst);
}

/*
 * Recover a smashable call from its return address by subtracting a fixed
 * length.  ARM's inline call is not a fixed *sequence* -- a far literal puts an
 * ADRP in front of the LDR -- but the ADRP is a prefix, so `ret - len` always
 * lands on the LDR, and every ARM parser accepts a site named by its LDR and
 * walks back to the ADRP itself (see smashableInlineLoad()).  The result is
 * therefore the site start for a near call and the LDR for a far one, which is
 * what all the smashable*Target/smash* entry points expect.
 */
TCA smashableCallFromRet(TCA ret) {
  always_assert(ret);
  return ret - smashableCallLen();
}

TCA smashableCallStart(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(smashableCallStart, inst);
}

bool optimizeSmashedCall(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(optimizeSmashedCall, inst);
}

bool optimizeSmashedJmp(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(optimizeSmashedJmp, inst);
}

bool optimizeSmashedJcc(TCA inst) {
  always_assert(inst);
  return ARCH_SWITCH_CALL(optimizeSmashedJcc, inst);
}

///////////////////////////////////////////////////////////////////////////////

}
