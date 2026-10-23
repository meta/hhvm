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

#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/asm-x64.h"

namespace HPHP::jit::irlower {

///////////////////////////////////////////////////////////////////////////////

inline Vout& vmain(IRLS& env) { assertx(env.vmain); return *env.vmain; }
inline Vout& vcold(IRLS& env) { assertx(env.vcold); return *env.vcold; }

inline Vlabel label(IRLS& env, Block* b) { return env.labels[b]; }

inline Vloc tmpLoc(IRLS& env, const SSATmp* tmp) {
  return env.locs[tmp];
}

inline Vloc srcLoc(IRLS& env, const IRInstruction* inst, unsigned i) {
  return tmpLoc(env, inst->src(i));
}

inline Vloc dstLoc(IRLS& env, const IRInstruction* inst, unsigned i) {
  return tmpLoc(env, inst->dst(i));
}

inline ArgGroup argGroup(IRLS& env, const IRInstruction* inst) {
  return ArgGroup(inst, env.locs);
}

inline CallDest callDest(Vreg reg0) {
  return { DestType::SSA, reg0 };
}

inline CallDest callDest(Vreg reg0, Vreg reg1) {
  return { DestType::TV, reg0, reg1 };
}

inline CallDest callDest(IRLS& env, const IRInstruction* inst) {
  if (inst->numDsts() == 0) return kVoidDest;
  assertx(inst->numDsts() == 1);
  if (inst->dst()->isA(TBottom)) return kVoidDest;

  auto const loc = dstLoc(env, inst, 0);
  DEBUG_ONLY auto const maybe_lval = inst->dst()->type().maybe(TLval);
  assertx(loc.numAllocated() == 1 || (maybe_lval && loc.numAllocated() == 2));
  assertx(IMPLIES(maybe_lval, inst->dst()->isA(TLval|TNullptr)));

  auto const dst = inst->dst();
  auto const kind = dst->isA(TBool) ? DestType::Byte :
                    dst->isA(TDbl) ? DestType::Dbl :
                    DestType::SSA;

  return { kind, dst->type(), loc.reg(0), loc.reg(1) };
}

inline CallDest callDestTV(IRLS& env, const IRInstruction* inst) {
  assertx(inst->numDsts() == 1);

  auto const loc = dstLoc(env, inst, 0);
  assertx(loc.numAllocated() == 1 || loc.numAllocated() == 2);

  if (loc.isFullSIMD()) {
    assertx(loc.numAllocated() == 1);
    return { DestType::SIMD, TCell, loc.reg(0) };
  }

  // loc.reg(1) may be InvalidReg, if the type is statically known. This is
  // expected and handled by users of CallDest.
  return { DestType::TV, TCell, loc.reg(0), loc.reg(1) };
}

///////////////////////////////////////////////////////////////////////////////

inline void fwdJcc(Vout& v, IRLS& env, ConditionCode cc,
                   Vreg sf, Vlabel target) {
  auto const next = v.makeBlock();
  v << jcc{cc, sf, {next, target}};
  v = next;
}

inline void fwdJcc(Vout& v, IRLS& env, ConditionCode cc,
                   Vreg sf, Block* target) {
  fwdJcc(v, env, cc, sf, label(env, target));
}

///////////////////////////////////////////////////////////////////////////////

}
