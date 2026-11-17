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

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/types.h"

#include <cstdint>

namespace HPHP::jit {

struct SSATmp;

namespace irgen {

struct IRGS;

///////////////////////////////////////////////////////////////////////////////

/*
 * To facilitate tight code generation for prologues, the definition of namedArgNames
 * is subtle. A nullptr (as opposed to a cns(env, nullptr)) value indicates that we don't
 * know the namedArgNames value, and instead must rely on the prologue flags to check for
 * the existence of any named args. That situation occurs in the prologues of functions
 * without named args. This behavior allows us to immediately release the named arg names
 * register in the common case.
 */
void emitCalleeNamedArgChecks(IRGS& env, const Func* callee, uint32_t posArgc,
                              SSATmp* prologueFlags, SSATmp* namedArgNames);
/*
 * Check for presence, count and wildcard match of generics.
 *
 * If `pushed' is true, generics are on the stack. Otherwise, generics may or
 * may not be above the stack, depending on the prologueFlags.
 *
 * If `namedArgsAccountedInStack` is false, the generics checks will assume that the stack
 * top is namedArgNames->size() lower than it should be.
 *
 * After this check, generics will be on the stack iff the function has reified
 * generics.
 */
void emitCalleeGenericsChecks(IRGS& env, const Func* callee,
                              SSATmp* prologueFlags, SSATmp* namedArgNames,
                              bool pushed, bool namedArgsAccountedInStack);

void emitCalleeArgumentArityChecks(IRGS& env, const Func* callee,
                                   uint32_t& argc);

void emitCalleeArgumentTypeChecks(IRGS& env, const Func* callee,
                                  uint32_t argc, SSATmp* prologueCtx);

void emitCalleeDynamicCallChecks(IRGS& env, const Func* callee,
                                SSATmp* prologueFlags);

void emitCalleeCoeffectChecks(IRGS& env, const Func* callee,
                              SSATmp* prologueFlags, SSATmp* providedCoeffects,
                              bool skipCoeffectsCheck,
                              uint32_t argc, SSATmp* prologueCtx);

void emitCalleeRecordFuncCoverage(IRGS& env, const Func* callee);

void emitInitFuncInputs(IRGS& env, const Func* callee, uint32_t posArgc,
                        SSATmp* fp);

void emitInitFuncInputsInline(IRGS& env, const Func* callee, uint32_t argc,
                              SSATmp* fp);

/* Pushes uninits for missing optional named params and re-arranges positionals
 * and named args such that they're in the position the func entry expects.
 * Expects and reified generics and coeffects to be on the stack already.
 * Returns true if any uninits were emitted.
 */
bool emitInitFuncNamedParams(IRGS& env, const Func* callee,
                             uint32_t posArgc, const ArrayData* namedArgNames);


void emitFuncPrologue(IRGS& env, const Func* callee, uint32_t argc,
                      TransID transID);

void emitFuncEntry(IRGS& env);
void emitNamedParamsFuncEntry(IRGS& env);

///////////////////////////////////////////////////////////////////////////////

}}

