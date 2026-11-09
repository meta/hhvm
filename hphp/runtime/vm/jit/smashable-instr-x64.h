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

#include "hphp/runtime/vm/jit/align-x64.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/phys-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

namespace HPHP::jit {

struct CGMeta;

namespace x64 {

///////////////////////////////////////////////////////////////////////////////

/*
 * Mirrors the API of smashable-instr.h.
 */

constexpr size_t smashableMovqLen() { return 10; }
constexpr size_t smashableCmpqLen() { return 8; }
constexpr size_t smashableCallLen() { return 5; }
constexpr size_t smashableJmpLen()  { return 5; }
constexpr size_t smashableJccLen()  { return 6; }
constexpr size_t smashableInterceptLen () { return 2; }

constexpr size_t smashableCallLenActual() { return smashableCallLen(); }
constexpr size_t smashableJmpLenActual()  { return smashableJmpLen(); }

TCA emitSmashableMovq(CodeBlock& cb, CGMeta& fixups, uint64_t imm,
                      PhysReg d, bool emitFarLiteral = false);
TCA emitSmashableCall(CodeBlock& cb, CGMeta& fixups, TCA target,
                      bool emitFarLiteral);
TCA emitSmashableJmp(CodeBlock& cb, CGMeta& fixups, TCA target,
                     bool emitFarLiteral);
TCA emitSmashableJcc(CodeBlock& cb, CGMeta& fixups, TCA target,
                     ConditionCode cc);
void smashMovq(TCA inst, uint64_t imm);
void smashCmpq(TCA inst, uint32_t imm);
void smashCall(TCA inst, TCA target);
void smashJmp(TCA inst, TCA target);
void smashJcc(TCA inst, TCA target);
void smashInterceptJcc(TCA inst);
void smashInterceptJmp(TCA inst);

uint64_t smashableMovqImm(TCA inst);
uint32_t smashableCmpqImm(TCA inst);
TCA smashableCallTarget(TCA inst);
TCA smashableCallLiteral(TCA inst);
inline TCA smashableCallStart(TCA inst) { return inst; }
TCA smashableJmpTarget(TCA inst);
TCA smashableJccTarget(TCA inst);
ConditionCode smashableJccCond(TCA inst);

bool optimizeSmashedCall(TCA inst);
bool optimizeSmashedJmp(TCA inst);
bool optimizeSmashedJcc(TCA inst);

/*
 * Smashable immediate and target offsets.
 *
 * These are also used by align-x64.
 */
constexpr size_t kSmashMovqImmOff = 2;
constexpr size_t kSmashCmpqImmOff = 4;

///////////////////////////////////////////////////////////////////////////////

}}
