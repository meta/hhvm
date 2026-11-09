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

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/phys-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

namespace HPHP::jit {

struct CGMeta;

///////////////////////////////////////////////////////////////////////////////

/*
 * This header provides the API for interacting with smashable instructions.
 *
 * Smashable instructions are logical instructions that can be safely mutated
 * even in the presence of concurrent execution.  These may map to more than
 * one machine instruction, depending on the target architecture.
 *
 * Supported instructions include:
 *
 *    [insts w/ smashable immediates]
 *      - movq
 *      - cmpq:   A cmpq of a 32-bit immediate and a memory operand with 8-bit
 *                displacement (and no index or scale).  Used for func guards.
 *
 *    [insts w/ smashable targets]
 *      - call
 *      - jmp
 *      - jcc
 *      - jcc_jmp:  A jcc followed contiguously by a jmp, both of which have
 *                  independently smashable targets.  Used for bindjcc1st.
 *      - interceptjcc:  A jcc instruction that can be smashed into a
 *                       jmp instruction to the same target.
 *      - interceptjmp:  A jmp instruction that can be smashed into a
 *                       jcc instruction to the same target.
 *
 * Smashable instructions must have a statically known maximum length (though
 * they may require nop-gap realignment when they are emitted, and some
 * architectures may emit a shorter variant).
 */

/*
 * Maximum size of the smashable machine code sequence.
 */
size_t smashableMovqLen();
size_t smashableCmpqLen();
size_t smashableCallLen();
size_t smashableJmpLen();
size_t smashableJccLen();
size_t smashableInterceptLen();

/*
 * Emit a smashable instruction and return the instruction's address.
 *
 * For jcc_and_jmp, return a pair of (jcc_addr, jmp_addr).
 */
TCA emitSmashableMovq(CodeBlock& cb, CGMeta& fixups, uint64_t imm,
                      PhysReg d, bool emitFarLiteral = false);
TCA emitSmashableCall(CodeBlock& cb, CGMeta& fixups, TCA target,
                      bool emitFarLiteral);
TCA emitSmashableJmp(CodeBlock& cb, CGMeta& fixups, TCA target,
                     bool emitFarLiteral);
TCA emitSmashableJcc(CodeBlock& cb, CGMeta& fixups, TCA target,
                     ConditionCode cc);

/*
 * Logically smash the smashable operand of an instruction.
 *
 * These operations are allowed to change the instructions themselves, provided
 * they still behave appropriately (e.g., a fallthrough jmp can be smashed to a
 * nop sequence).
 *
 * The `smashJcc' routine leaves the condition unchanged if `cc' is CC_None.
 */
void smashMovq(TCA inst, uint64_t imm);
void smashCmpq(TCA inst, uint32_t imm);
void smashCall(TCA inst, TCA target);
void smashJmp(TCA inst, TCA target);
void smashJcc(TCA inst, TCA target);
void smashInterceptJcc(TCA inst);
void smashInterceptJmp(TCA inst);

/*
 * Extract instruction operands from assembly.
 *
 * If `inst' does not represent the correct smashable instruction, the
 * _target() routines will return nullptr, and the remaining routines will
 * produce unspecified behavior.
 */
uint64_t smashableMovqImm(TCA inst);
uint32_t smashableCmpqImm(TCA inst);
TCA smashableCallTarget(TCA inst);
TCA smashableJmpTarget(TCA inst);
/*
 * If a smashable call reaches its target by loading a pointer rather than by
 * branching straight there, the address of that pointer; nullptr otherwise.
 * Lets an external patcher retarget the call without decoding our encoding.
 */
TCA smashableCallLiteral(TCA inst);
TCA smashableJccTarget(TCA inst);
ConditionCode smashableJccCond(TCA inst);

/*
 * Obtain the address of a smashable call from its return IP.
 */
TCA smashableCallFromRet(TCA ret);

/*
 * Normalize a smashable call address to the emitted sequence start used by
 * profiling metadata. May return nullptr if the call has already collapsed.
 */
TCA smashableCallStart(TCA inst);

/*
 * Optimize a smashable instruction in place after it has been smashed.  Returns
 * whether or not the instruction was optimized.
 */
bool optimizeSmashedCall(TCA inst);
bool optimizeSmashedJmp(TCA inst);
bool optimizeSmashedJcc(TCA inst);

///////////////////////////////////////////////////////////////////////////////

}
