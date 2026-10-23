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

#include "hphp/runtime/vm/jit/simplify.h"

// #include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/double-to-int64.h"
#include "hphp/runtime/base/package.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-vec-defs.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/runtime.h"

#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/simple-propagation.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"
#include "hphp/runtime/vm/jit/type.h"

#include "hphp/runtime/ext/hh/ext_hh.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/overflow.h"
#include "hphp/util/trace.h"

#include "hphp/util/text-util.h"

#include <limits>

namespace HPHP::jit {

TRACE_SET_MOD(simplify)

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_isEmpty("isEmpty");
const StaticString s_count("count");
const StaticString s_1("1");
const StaticString s_isFinished("isFinished");
const StaticString s_isSucceeded("isSucceeded");
const StaticString s_isFailed("isFailed");
const StaticString s_Awaitable("HH\\Awaitable");

//////////////////////////////////////////////////////////////////////

struct State {
  explicit State(IRUnit& unit) : unit(unit) {}
  IRUnit& unit;

  // The current instruction being simplified is always at insts.top(). This
  // has to be a stack instead of just a pointer because simplify is reentrant.
  jit::stack<const IRInstruction*> insts;
  jit::vector<IRInstruction*> newInsts;
};

//////////////////////////////////////////////////////////////////////

SSATmp* simplifyWork(State&, const IRInstruction*);

template<class... Args>
SSATmp* cns(State& env, Args&&... cns) {
  return env.unit.cns(std::forward<Args>(cns)...);
}

template<class... Args>
SSATmp* gen(State& env, Opcode op, Args&&... args);

template<class... Args>
SSATmp* gen(State& env, Opcode op, BCContext bcctx, Args&&... args) {
  return makeInstruction(
    [&] (IRInstruction* inst) -> SSATmp* {
      auto const prevNewCount = env.newInsts.size();
      auto const newDest = simplifyWork(env, inst);

      // If any simplification happened to this instruction, drop it. We have to
      // check that nothing was added to newInsts because that's the only way
      // we can tell simplification happened to a no-dest instruction.
      if (newDest || env.newInsts.size() != prevNewCount) {
        return newDest;
      } else {
        assertx(inst->isTransient());
        if (!env.newInsts.empty() && env.newInsts.back()->is(Unreachable)) {
          return cns(env, TBottom);
        }

        assertx(checkOperandTypes(inst));
        inst = env.unit.clone(inst);
        env.newInsts.push_back(inst);

        // If the new instruction's result is unreachable (and it is not block
        // ending), indicate this in the IR stream.
        if (inst->isNextEdgeUnreachable() && !inst->isBlockEnd()) {
          gen(env, Unreachable, ASSERT_REASON);
        }

        return inst->dst(0);
      }
    },
    op,
    bcctx,
    std::forward<Args>(args)...
  );
}

template<class... Args>
SSATmp* gen(State& env, Opcode op, Args&&... args) {
  assertx(!env.insts.empty());
  return gen(env, op, env.insts.top()->bcctx(), std::forward<Args>(args)...);
}

//////////////////////////////////////////////////////////////////////

DEBUG_ONLY bool validate(const State& env,
                         SSATmp* newDst,
                         const IRInstruction* origInst) {
  // simplify() rules are not allowed to add new uses to SSATmps that aren't
  // known to be available.  All the sources to the original instruction and
  // sources produced from new instructions must be available, and
  // non-reference counted values reachable through the source chain are also
  // always available. Anything else requires more complicated analysis than
  // belongs in the simplifier right now.
  auto known_available = [&] (SSATmp* src) -> bool {
    if (!src->type().maybe(TCounted)) return true;
    src = canonical(src);

    for (auto oldSrc : origInst->srcs()) {
      oldSrc = canonical(oldSrc);
      if (oldSrc == src) return true;

      // Some instructions consume a counted SSATmp and produce a new SSATmp
      // which supports the consumed location. If the result of one such
      // instruction is available then the value whose count it supports must
      // also be available. We care only about these few instructions below.
      if ((oldSrc->inst()->is(CreateSSWH) &&
             canonical(oldSrc->inst()->src(0)) == src) ||
          (oldSrc->inst()->is(CreateFSWH) &&
             canonical(oldSrc->inst()->src(0)) == src) ||
          (oldSrc->inst()->is(NewRFunc) &&
             canonical(oldSrc->inst()->src(1)) == src) ||
          (oldSrc->inst()->is(NewRClsMeth) &&
             canonical(oldSrc->inst()->src(2)) == src) ||
          (oldSrc->inst()->is(ConstructClosure) &&
             canonical(oldSrc->inst()->src(0)) == src)) {
        return true;
      }
    }
    for (auto& newInst : env.newInsts) {
      if (canonical(newInst->dst()) == src) {
        return true;
      }
    }
    return false;
  };

  // Return early for the no-simplification case.
  if (env.newInsts.empty() && !newDst) {
    return true;
  }

  const IRInstruction* last = nullptr;

  if (!env.newInsts.empty()) {
    for (size_t i = 0, n = env.newInsts.size(); i < n; ++i) {
      auto const newInst = env.newInsts[i];

      for (auto& src : newInst->srcs()) {
        always_assert_flog(
          known_available(src),
          "A simplification rule produced an instruction that used a value "
          "that wasn't known to be available:\n"
          "  original inst: {}\n"
          "  new inst:      {}\n"
          "  src:           {}\n",
          origInst->toString(),
          newInst->toString(),
          src->toString()
        );
      }

      if (i == n - 1) {
        last = newInst;
        continue;
      }

      always_assert_flog(
        !newInst->isBlockEnd(),
        "Block-ending instruction produced in the middle of a simplified "
        "instruction stream:\n"
        "  original inst: {}\n"
        "  new inst:      {}\n",
        origInst->toString(),
        newInst->toString()
      );
    }
  }

  if (newDst) {
    const bool available = known_available(newDst);

    always_assert_flog(
      available,
      "simplify() produced a new destination that wasn't known to be "
      "available:\n"
      "  original inst: {}\n"
      "  new dst:       {}\n",
      origInst->toString(),
      newDst->toString()
    );
  }

  if (!last) return true;

  auto assert_last = [&] (bool cond, const char* msg) {
    always_assert_flog(
      cond,
      "{}:\n"
      "  original inst: {}\n"
      "  last new inst: {}\n",
      msg,
      origInst->toString(),
      last->toString()
    );
  };

  assert_last(
    !origInst->naryDst(),
    "Nontrivial simplification returned for instruction with NaryDest"
  );

  assert_last(
    origInst->hasDst() == (newDst != nullptr),
    "HasDest mismatch between input and output"
  );

  if (last->hasEdges()) {
    assert_last(
      origInst->hasEdges(),
      "Instruction with edges produced for simplification of edge-free "
      "instruction"
    );

    assert_last(
      IMPLIES(last->next(), last->next() == origInst->next() ||
                            last->next() == ultimateDst(origInst->next()) ||
                            last->next() == origInst->taken() ||
                            last->next() == ultimateDst(origInst->taken())),
      "Last instruction of simplified stream has next edge not reachable from "
      "the input instruction"
    );

    assert_last(
      IMPLIES(last->taken(), last->taken() == origInst->next() ||
                             last->taken() == ultimateDst(origInst->next()) ||
                             last->taken() == origInst->taken() ||
                             last->taken() == ultimateDst(origInst->taken())),
      "Last instruction of simplified stream has taken edge not reachable "
      "from the input instruction"
    );
  }

  return true;
}

//////////////////////////////////////////////////////////////////////

/*
 * Individual simplification routines return nullptr if they don't want to
 * change anything, or they can call gen any number of times to produce a
 * different IR sequence, returning the thing gen'd that should be used as the
 * value of the simplified instruction sequence.
 */

SSATmp* mergeBranchDests(State& env, const IRInstruction* inst) {
  // Replace a conditional branch with a Jmp if both branches go to the same
  // block. Only work if the instruction does not have side effect.
  // JmpZero/JmpNZero is handled separately.
  assertx(inst->is(CheckRDSInitialized,
                   CheckVecBounds,
                   CheckDictKeys,
                   CheckPtrIterTombstone,
                   CheckMissingKeyInArrLike,
                   CheckDictOffset,
                   CheckKeysetOffset));
  assertx(inst->taken() != nullptr);
  if (inst->next() != nullptr &&
      ultimateDst(inst->next()) == ultimateDst(inst->taken())) {
    return gen(env, Jmp, ultimateDst(inst->next()));
  }
  if (inst->taken() && inst->taken()->isUnreachable()) {
    return gen(env, Nop);
  }
  if (inst->next() && inst->next()->isUnreachable()) {
    assertx(inst->taken());
    return gen(env, Jmp, inst->taken());
  }
  return nullptr;
}

SSATmp* simplifyCallViolatesModuleBoundary(State& env,
                                           const IRInstruction* inst) {
  if (inst->src(0)->hasConstVal(TFunc)) {
      auto const callee = inst->src(0)->funcVal();
      auto const caller = inst->extra<FuncData>()->func;
      return cns(env, will_symbol_raise_module_boundary_violation(callee, caller));
  }
  if (inst->src(0)->hasConstVal(TCls)) {
    auto const callee = inst->src(0)->clsVal();
    auto const caller = inst->extra<FuncData>()->func;
    return cns(env, will_symbol_raise_module_boundary_violation(callee, caller));
  }
  return nullptr;
}

SSATmp* simplifyEqFunc(State& env, const IRInstruction* inst) {
  auto const src0 = canonical(inst->src(0));
  auto const src1 = canonical(inst->src(1));
  if (src0 == src1) return cns(env, true);
  if (src0->hasConstVal() && src1->hasConstVal()) {
    return cns(env, src0->funcVal() == src1->funcVal());
  }
  if (!src0->hasConstVal() && !src1->hasConstVal()) return nullptr;

  auto const func = src0->hasConstVal() ? src0->funcVal() : src1->funcVal();
  auto const cls = func->implCls();

  // Consider only methods on final classes, where we could compare class
  // pointers instead.
  if (!cls || !(cls->attrs() & AttrNoOverride)) return nullptr;

  auto const funcInst = (src0->hasConstVal() ? src1 : src0)->inst();
  switch (funcInst->op()) {
    case LdClsMethod:
      if (cls->getMethodSafe(funcInst->src(1)->intVal()) == func) {
        return gen(env, EqCls, funcInst->src(0), cns(env, cls));
      }
      break;

    case LdIfaceMethod: {
      auto const extra = funcInst->extra<LdIfaceMethod>();
      if (cls->getIfaceMethodSafe(extra->vtableIdx, extra->methodIdx) == func) {
        return gen(env, EqCls, funcInst->src(0), cns(env, cls));
      }
      break;
    }

    case LdObjInvoke:
      if (cls->getRegularInvoke() == func) {
        return gen(env, EqCls, funcInst->src(0), cns(env, cls));
      }
      break;

    default:
      break;
  }

  return nullptr;
}

SSATmp* simplifyLdClosureCtx(State& env, const IRInstruction* inst) {
  auto const closure = canonical(inst->src(0));
  if (!closure->inst()->is(ConstructClosure)) return nullptr;
  return gen(env, AssertType, inst->typeParam(), closure->inst()->src(0));
}

SSATmp* simplifyLdClosureCls(State& env, const IRInstruction* inst) {
  return simplifyLdClosureCtx(env, inst);
}

SSATmp* simplifyLdClosureThis(State& env, const IRInstruction* inst) {
  return simplifyLdClosureCtx(env, inst);
}

SSATmp* simplifyLdFuncCls(State& env, const IRInstruction* inst) {
  auto const funcTmp = inst->src(0);
  return funcTmp->hasConstVal(TFunc)
    ? funcTmp->funcVal()->cls()
      ? cns(env, funcTmp->funcVal()->cls())
      : cns(env, nullptr)
    : nullptr;
}

SSATmp* simplifyLdFuncInOutBits(State& env, const IRInstruction* inst) {
  auto const funcTmp = inst->src(0);
  return funcTmp->hasConstVal(TFunc)
    ? cns(env, funcTmp->funcVal()->inOutBits())
    : nullptr;
}

SSATmp* simplifyLdFuncNumParams(State& env, const IRInstruction* inst) {
  auto const funcTmp = inst->src(0);
  return funcTmp->hasConstVal(TFunc)
    ? cns(env, (funcTmp->funcVal()->numParams()))
    : nullptr;
}

SSATmp* simplifyFuncHasAttr(State& env, const IRInstruction* inst) {
  auto const funcTmp = inst->src(0);
  return funcTmp->hasConstVal(TFunc)
    ? cns(env, !!(funcTmp->funcVal()->attrs() & inst->extra<AttrData>()->attr))
    : nullptr;
}

SSATmp* simplifyClassHasAttr(State& env, const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  return clsTmp->hasConstVal(TCls)
    ? cns(env, (clsTmp->clsVal()->attrs() & inst->extra<AttrData>()->attr))
    : nullptr;
}

SSATmp* simplifyLdFuncRequiredCoeffects(State& env, const IRInstruction* inst) {
  auto const funcTmp = inst->src(0);
  return funcTmp->hasConstVal(TFunc)
    ? cns(env, (funcTmp->funcVal()->requiredCoeffects().value()))
    : nullptr;
}

SSATmp* simplifyLookupClsCtxCns(State& env, const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const clsSpec = clsTmp->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const nameTmp = inst->src(1);
  if (!nameTmp->hasConstVal(TStr)) return nullptr;

  auto const cls = clsSpec.cls();
  auto const ctxName = nameTmp->strVal();

  auto const objectExistsTmp = inst->src(2);
  bool objectExists = (objectExistsTmp->hasConstVal(TBool)
                      ? objectExistsTmp->boolVal()
                      : false);

  if (clsSpec.exact()) {
    auto const result = cls->clsCtxCnsGet(ctxName, false);
    if (!result) return nullptr; // we will raise warning/error
    return cns(env, result->value());
  }

  // If it is an interface/trait/enum etc, don't optimize
  if (!isNormalClass(cls)) return nullptr;

  // if an object of this cls is known to exist, we can safely use
  // slot based optimizations
  auto const slot =
    cls->clsCnsSlot(ctxName, ConstModifierFlags::Kind::Context, objectExists);
  if (slot == kInvalidSlot) return nullptr; // we will raise warning/error
  return gen(env, LdClsCtxCns, ClsCnsSlotData { ctxName, slot}, clsTmp);
}

SSATmp* simplifyLdClsCtxCns(State& env, const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const clsSpec = clsTmp->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.cls();
  auto const extra = inst->extra<LdClsCtxCns>();

  assertx(extra->slot < cls->numConstants());
  auto const& ctxCns = cls->constants()[extra->slot];
  assertx(ctxCns.kind() == ConstModifierFlags::Kind::Context);

  if (clsSpec.exact()) {
    assertx(!ctxCns.isAbstractAndUninit());
    auto const coeffect =
      ctxCns.val.constModifiers().getCoeffects(ctxCns.val.constModifierFlags()).toRequired();
    return cns(env, coeffect.value());
  }
  return nullptr;
}

SSATmp* simplifyLdResolvedTypeCns(State& env, const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const clsSpec = clsTmp->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.cls();
  auto const extra = inst->extra<LdResolvedTypeCns>();

  assertx(cls->hasTypeConstant(extra->cnsName, true));
  assertx(extra->slot < cls->numConstants());
  auto const& typeCns = cls->constants()[extra->slot];
  auto const& resolved = typeCns.preConst->resolvedTypeStructure();

  if (clsSpec.exact()) {
    if (typeCns.isAbstractAndUninit()) {
      gen(env, Jmp, inst->taken());
      return cns(env, TBottom);
    }
    if (resolved.isNull()) return nullptr;
    return cns(env, resolved.get());
  }
  if (resolved.isNull()) return nullptr;

  switch (typeCns.preConst->invariance()) {
    case PreClass::Const::Invariance::None:
      return nullptr;
    case PreClass::Const::Invariance::Present:
    case PreClass::Const::Invariance::ClassnamePresent:
      return gen(env, LdResolvedTypeCnsNoCheck, *extra, clsTmp);
    case PreClass::Const::Invariance::Same:
      return cns(env, resolved.get());
  }
  always_assert(false);
}

SSATmp* simplifyLdResolvedTypeCnsNoCheck(State& env,
                                         const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const clsSpec = clsTmp->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.cls();
  auto const extra = inst->extra<LdResolvedTypeCnsNoCheck>();

  assertx(cls->hasTypeConstant(extra->cnsName, true));
  assertx(extra->slot < cls->numConstants());
  auto const& typeCns = cls->constants()[extra->slot];

  auto const& resolved = typeCns.preConst->resolvedTypeStructure();
  if (resolved.isNull()) return nullptr;

  if (clsSpec.exact()) return cns(env, resolved.get());
  if (typeCns.preConst->invariance() == PreClass::Const::Invariance::Same) {
    return cns(env, resolved.get());
  }
  return nullptr;
}

SSATmp* simplifyLdResolvedTypeCnsClsName(State& env,
                                         const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const clsSpec = clsTmp->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.cls();
  auto const extra = inst->extra<LdResolvedTypeCnsClsName>();

  assertx(cls->hasTypeConstant(extra->cnsName, true));
  assertx(extra->slot < cls->numConstants());
  auto const& typeCns = cls->constants()[extra->slot];
  auto const& resolved = typeCns.preConst->resolvedTypeStructure();

  auto const classname = [&] {
    auto const name = resolved->get(s_classname);
    if (tvIsString(name)) return cns(env, name);
    return cns(env, nullptr);
  };

  if (clsSpec.exact()) {
    if (typeCns.isAbstractAndUninit()) return cns(env, nullptr);
    if (resolved.isNull()) return nullptr;
    return classname();
  }
  if (resolved.isNull()) return nullptr;

  if (typeCns.preConst->invariance() == PreClass::Const::Invariance::Same) {
    return classname();
  }
  return nullptr;
}

SSATmp* simplifyLdCls(State& env, const IRInstruction* inst) {
  auto const str = inst->src(0);
  auto const cls = inst->src(1);
  if (str->inst()->is(LdClsName)) {
    return str->inst()->src(0);
  }
  if (str->hasConstVal() && (cls->hasConstVal(TCls) || cls->isA(TNullptr))) {
    auto const sval = str->strVal();
    auto const cval = cls->hasConstVal(TCls) ? cls->clsVal() : nullptr;
    auto const result = Class::lookupKnown(sval, cval);
    if (result) return cns(env, result);
  }
  return nullptr;
}

SSATmp* simplifyLdClsMethod(State& env, const IRInstruction* inst) {
  auto const clsTmp = inst->src(0);
  auto const idxTmp = inst->src(1);

  if (!idxTmp->hasConstVal()) return nullptr;
  auto const idx = idxTmp->intVal();

  if (clsTmp->hasConstVal()) {
    auto const cls = clsTmp->clsVal();
    if (idx < cls->numMethods()) {
      return cns(env, cls->getMethod(idx));
    }
  }

  if (auto const cls = clsTmp->type().clsSpec().cls()) {
    if (idx < cls->numMethods()) {
      auto func = cls->getMethod(idx);
      if (func->isImmutableFrom(cls)) {
        return cns(env, func);
      }
    }
  }

  return nullptr;
}

SSATmp* simplifyLdObjClass(State& env, const IRInstruction* inst) {
  auto const ty = inst->src(0)->type();

  if (!(ty < TObj)) return nullptr;

  if (auto const exact = ty.clsSpec().exactCls()) return cns(env, exact);

  if (auto const srcInst = inst->src(0)->inst(); srcInst->is(AllocObj)) {
    return srcInst->src(0);
  }

  return nullptr;
}

SSATmp* simplifyLdFrameCls(State& env, const IRInstruction* inst) {
  auto const ty = inst->typeParam();

  if (!(ty < TCls)) return nullptr;

  if (auto const cls = ty.clsSpec().exactCls()) {
    return cns(env, cls);
  }

  return nullptr;
}

SSATmp* simplifyLdObjInvoke(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (!src->hasConstVal(TCls)) return nullptr;

  auto const meth = src->clsVal()->getRegularInvoke();
  return meth == nullptr ? cns(env, nullptr) : cns(env, meth);
}

SSATmp* simplifyMov(State& /*env*/, const IRInstruction* inst) {
  return inst->src(0);
}

SSATmp* simplifyAbsDbl(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) return cns(env, fabs(src->dblVal()));

  return nullptr;
}

template<class Oper>
SSATmp* constImpl(State& env, SSATmp* src1, SSATmp* src2, Oper op) {
  // don't canonicalize to the right, OP might not be commutative
  if (!src1->hasConstVal() || !src2->hasConstVal()) return nullptr;

  auto both = [&](Type ty) { return src1->type() <= ty && src2->type() <= ty; };

  if (both(TBool)) return cns(env, op(src1->boolVal(), src2->boolVal()));
  if (both(TInt))  return cns(env, op(src1->intVal(), src2->intVal()));
  if (both(TDbl))  return cns(env, op(src1->dblVal(), src2->dblVal()));
  return nullptr;
}

template<class Oper>
SSATmp* commutativeImpl(State& env,
                        SSATmp* src1,
                        SSATmp* src2,
                        Opcode opcode,
                        Oper op) {
  if (auto simp = constImpl(env, src1, src2, op)) return simp;

  // Canonicalize constants to the right.
  if (src1->hasConstVal() && !src2->hasConstVal()) {
    return gen(env, opcode, src2, src1);
  }

  // Only handle integer operations for now.
  if (!src1->isA(TInt) || !src2->isA(TInt)) return nullptr;

  auto const inst1 = src1->inst();
  auto const inst2 = src2->inst();
  if (inst1->op() == opcode && inst1->src(1)->hasConstVal()) {
    // (X + C1) + C2 --> X + C3
    if (src2->hasConstVal()) {
      auto const right = op(inst1->src(1)->intVal(), src2->intVal());
      return gen(env, opcode, inst1->src(0), cns(env, right));
    }
    // (X + C1) + (Y + C2) --> X + Y + C3
    if (inst2->op() == opcode && inst2->src(1)->hasConstVal()) {
      auto const right = op(inst1->src(1)->intVal(), inst2->src(1)->intVal());
      auto const left = gen(env, opcode, inst1->src(0), inst2->src(0));
      return gen(env, opcode, left, cns(env, right));
    }
  }
  return nullptr;
}

/*
 * Assumes that outop is commutative, don't use with subtract!
 *
 * Assumes that the values we're going to add new uses to are not reference
 * counted.  (I.e. this is a distributive FooInt opcode.)
 */
template <class OutOper, class InOper>
SSATmp* distributiveImpl(State& env, SSATmp* src1, SSATmp* src2, Opcode outcode,
                         Opcode incode, OutOper outop, InOper /*inop*/) {
  if (auto simp = commutativeImpl(env, src1, src2, outcode, outop)) {
    return simp;
  }

  auto const inst1 = src1->inst();
  auto const inst2 = src2->inst();
  auto const op1 = inst1->op();
  auto const op2 = inst2->op();
  // all combinations of X * Y + X * Z --> X * (Y + Z)
  if (op1 == incode && op2 == incode) {
    if (inst1->src(0) == inst2->src(0)) {
      auto const fold = gen(env, outcode, inst1->src(1), inst2->src(1));
      return gen(env, incode, inst1->src(0), fold);
    }
    if (inst1->src(0) == inst2->src(1)) {
      auto const fold = gen(env, outcode, inst1->src(1), inst2->src(0));
      return gen(env, incode, inst1->src(0), fold);
    }
    if (inst1->src(1) == inst2->src(0)) {
      auto const fold = gen(env, outcode, inst1->src(0), inst2->src(1));
      return gen(env, incode, inst1->src(1), fold);
    }
    if (inst1->src(1) == inst2->src(1)) {
      auto const fold = gen(env, outcode, inst1->src(0), inst2->src(0));
      return gen(env, incode, inst1->src(1), fold);
    }
  }
  return nullptr;
}

SSATmp* simplifyAddInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  auto add = std::plus<uint64_t>();
  auto mul = std::multiplies<uint64_t>();
  if (auto simp = distributiveImpl(env, src1, src2, AddInt,
                                   MulInt, add, mul)) {
    return simp;
  }
  if (src2->hasConstVal()) {
    int64_t src2Val = src2->intVal();
    // X + 0 --> X
    if (src2Val == 0) return src1;

    // X + -C --> X - C
    // Weird, but can show up as a result of other simplifications.
    auto const min = std::numeric_limits<int64_t>::min();
    if (src2Val < 0 && src2Val > min) {
      return gen(env, SubInt, src1, cns(env, -src2Val));
    }
  }
  // X + (0 - Y) --> X - Y
  auto const inst2 = src2->inst();
  if (inst2->op() == SubInt) {
    auto const src = inst2->src(0);
    if (src->hasConstVal() && src->intVal() == 0) {
      return gen(env, SubInt, src1, inst2->src(1));
    }
  }
  auto const inst1 = src1->inst();

  // (X - C1) + ...
  if (inst1->op() == SubInt && inst1->src(1)->hasConstVal()) {
    auto const x = inst1->src(0);
    auto const c1 = inst1->src(1);

    // (X - C1) + C2 --> X + (C2 - C1)
    if (src2->hasConstVal()) {
      auto const rhs = gen(env, SubInt, cns(env, src2->intVal()), c1);
      return gen(env, AddInt, x, rhs);
    }

    // (X - C1) + (Y +/- C2)
    if ((inst2->op() == AddInt || inst2->op() == SubInt) &&
        inst2->src(1)->hasConstVal()) {
      auto const y = inst2->src(0);
      auto const c2 = inst2->src(1);
      auto const rhs = inst2->op() == SubInt ?
        // (X - C1) + (Y - C2) --> X + Y + (-C1 - C2)
        gen(env, SubInt, gen(env, SubInt, cns(env, 0), c1), c2) :
        // (X - C1) + (Y + C2) --> X + Y + (C2 - C1)
        gen(env, SubInt, c2, c1);

      auto const lhs = gen(env, AddInt, x, y);
      return gen(env, AddInt, lhs, rhs);
    }
    // (X - C1) + (Y + C2) --> X + Y + (C2 - C1)
    if (inst2->op() == AddInt && inst2->src(1)->hasConstVal()) {
      auto const y = inst2->src(0);
      auto const c2 = inst2->src(1);

      auto const lhs = gen(env, AddInt, x, y);
      auto const rhs = gen(env, SubInt, c2, c1);
      return gen(env, AddInt, lhs, rhs);
    }
  }

  return nullptr;
}

SSATmp* simplifySubInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  auto const sub = std::minus<uint64_t>();
  if (auto simp = constImpl(env, src1, src2, sub)) return simp;

  // X - X --> 0
  if (src1 == src2) return cns(env, 0);

  if (src2->hasConstVal()) {
    auto const src2Val = src2->intVal();
    // X - 0 --> X
    if (src2Val == 0) return src1;

    // X - -C --> X + C
    // Need to check for C == INT_MIN, otherwise we'd infinite loop as
    // X + -C would send us back here.
    auto const min = std::numeric_limits<int64_t>::min();
    if (src2Val > min && src2Val < 0) {
      return gen(env, AddInt, src1, cns(env, -src2Val));
    }
  }
  // X - (0 - Y) --> X + Y
  auto const inst2 = src2->inst();
  if (inst2->op() == SubInt) {
    auto const src = inst2->src(0);
    if (src->hasConstVal(0)) return gen(env, AddInt, src1, inst2->src(1));
  }
  return nullptr;
}

SSATmp* simplifyMulInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  auto const mul = std::multiplies<uint64_t>();
  if (auto simp = commutativeImpl(env, src1, src2, MulInt, mul)) return simp;

  if (!src2->hasConstVal()) return nullptr;

  auto const rhs = src2->intVal();

  // X * (-1) --> -X
  if (rhs == -1) return gen(env, SubInt, cns(env, 0), src1);
  // X * 0 --> 0
  if (rhs == 0) return cns(env, 0);
  // X * 1 --> X
  if (rhs == 1) return src1;
  // X * 2 --> X + X
  if (rhs == 2) return gen(env, AddInt, src1, src1);

  /*
   * MulInt wraps, so these rewrites are modular arithmetic and belong in
   * unsigned. Computing rhs +- 1 signed would overflow at the int boundaries,
   * and because that is undefined the compiler may assume it cannot happen and
   * drop the positivity test inside isPowTwo(), leaving log2() to assert on a
   * value isPowTwo() claimed was fine.
   *
   * Unsigned also makes the boundaries themselves rewritable: 2^63 is a power
   * of two as an unsigned value, so X * INT64_MIN, X * (INT64_MIN + 1) and
   * X * INT64_MAX now fold like any other multiply.
   */
  auto const urhs = static_cast<uint64_t>(rhs);

  auto isPowTwo = [](uint64_t a) {
    return a > 0 && folly::isPowTwo<uint64_t>(a);
  };
  auto log2 = [](uint64_t a) {
    assertx(a > 0);
    return folly::findLastSet<uint64_t>(a) - 1;
  };

  // X * 2^C --> X << C
  if (isPowTwo(urhs)) return gen(env, Shl, src1, cns(env, log2(urhs)));

  // X * (2^C + 1) --> ((X << C) + X)
  if (isPowTwo(urhs - 1)) {
    auto const lhs = gen(env, Shl, src1, cns(env, log2(urhs - 1)));
    return gen(env, AddInt, lhs, src1);
  }
  // X * (2^C - 1) --> ((X << C) - X)
  if (isPowTwo(urhs + 1)) {
    auto const lhs = gen(env, Shl, src1, cns(env, log2(urhs + 1)));
    return gen(env, SubInt, lhs, src1);
  }

  return nullptr;
}

SSATmp* simplifyAddDbl(State& env, const IRInstruction* inst) {
  return constImpl(env, inst->src(0), inst->src(1), std::plus<double>());
}

SSATmp* simplifySubDbl(State& env, const IRInstruction* inst) {
  return constImpl(env, inst->src(0), inst->src(1), std::minus<double>());
}

SSATmp* simplifyMulDbl(State& env, const IRInstruction* inst) {
  return constImpl(env, inst->src(0), inst->src(1), std::multiplies<double>());
}

SSATmp* simplifyMulIntO(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  if (src1->hasConstVal() && src2->hasConstVal()) {
    auto const a = src1->intVal();
    auto const b = src2->intVal();
    if (mul_overflow(a, b)) {
      gen(env, Jmp, inst->taken());
      return cns(env, TBottom);
    }
    return cns(env, a * b);
  }
  return nullptr;
}

/*
Integer Modulo/Remainder Operator: a % b = a - a / b * b
*/
SSATmp* simplifyMod(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  if (src1 == src2) {
    // X % X -> 0
    return cns(env, 0);
  }
  if (!src2->hasConstVal()) return nullptr;

  auto const src2Val = src2->intVal();

  if (src2Val == 0) {
    // Undefined behavior, so we might as well constant propagate whatever we
    // want. If we're being asked to simplify this, it better be dynamically
    // unreachable code.
    // TODO we can do `return gen(env, Unreachable, ASSERT_REASON);` here
    return cns(env, 0);
  }

  // X % 1 --> 0, X % -1 --> 0
  if (src2Val == -1 || src2Val == 1) return cns(env, 0);

  if (src1->hasConstVal()) return cns(env, src1->intVal() % src2Val);

  // Optimization: x % 2^n == x & (2^n - 1)
  if (folly::popcount(llabs(src2Val)) == 1) {
    // long shft =
    //  static_cast<unsigned long>(x >> 63) >> (64 - __builtin_ctzll(y));
    // ret ((x + shft) & (y - 1)) - shft;
    auto const divisor = llabs(src2Val);
    auto const trailingZeros = cns(env, 64 - __builtin_ctzll(divisor));
    auto const shft = gen(env, Lshr,
                          gen(env, Shr, src1, cns(env, 63)), trailingZeros);
    return gen(env, SubInt,
               gen(env, AndInt,
                   gen(env, AddInt, src1, shft), cns(env, divisor - 1)), shft);
  }
  return nullptr;
}

SSATmp* simplifyDivDbl(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  /*
   * There is deliberately no X / X -> 1.0 here.  That identity holds for
   * integers, where simplifyDivInt uses it, but not for doubles: both
   * infinities and NaN divided by themselves are NaN, and nothing in the type
   * system can rule those out for a TDbl.  When X is a constant the fold at
   * the end of this function already produces the right answer, so the
   * rewrite bought nothing that is not still covered.
   */

  if (!src2->hasConstVal()) return nullptr;

  auto const src2Val = src2->dblVal();

  // The branch emitted during irgen will deal with this
  assertx(src2Val != 0.0);
  // X / 1.0 -> X
  if (src2Val == 1.0) {
    return src1;
  }

  // statically compute X / Y
  return src1->hasConstVal() ? cns(env, src1->dblVal() / src2Val) : nullptr;
}

SSATmp* simplifyDivInt(State& env, const IRInstruction* inst) {
  auto isPowTwo = [](uint64_t a) {
    return a > 0 && folly::isPowTwo<uint64_t>(a);
  };
  auto log2 = [](uint64_t a) {
    assertx(a > 0);
    return folly::findLastSet<uint64_t>(a) - 1;
  };
  auto const dividend = inst->src(0);
  auto const divisor  = inst->src(1);
  // X / X -> 1
  if (dividend == divisor) {
    return cns(env, 1);
  }

  if (!divisor->hasConstVal()) return nullptr;

  auto const divisorVal = divisor->intVal();

  // The branch emitted during irgen will deal with this
  assertx(divisorVal != 0);

  // X / 1 -> X
  if (divisorVal == 1) {
    return dividend;
  }

  // X / -1 -> -X
  if (divisorVal == -1) {
    return gen(env, SubInt, cns(env, 0), dividend);
  }

  if (!dividend->hasConstVal()) {
    // X / 2^n -> X >> n
    if (divisorVal > 0 && isPowTwo(divisorVal)) {
      return gen(env, Shr, dividend, cns(env, log2(divisorVal)));
    }
    if (divisorVal < 0) {
      /*
       * X / -2^n -> -(X >> n)
       *
       * The magnitude is taken in unsigned because negating LLONG_MIN
       * overflows, and since that is undefined the compiler may assume the
       * result is positive and drop isPowTwo()'s guard, leaving log2() to
       * assert on it.  Unsigned also lets LLONG_MIN itself fold: DivInt is
       * only emitted where the division is exact, so the dividend is either
       * 0 or LLONG_MIN and -(X >> 63) gives 0 or 1 as it should.
       */
      auto const magnitude = -static_cast<uint64_t>(divisorVal);
      if (isPowTwo(magnitude)) {
        return gen(env, SubInt, cns(env, 0),
                   gen(env, Shr, dividend, cns(env, log2(magnitude))));
      }
    }
    return nullptr;
  }
  auto const dividendVal = dividend->intVal();

  /*
   * irgen only emits DivInt on the branch where the division came out exact.
   * It does not, however, keep LLONG_MIN away from here: emitDiv() only
   * diverts to DivDbl when the dividend is LLONG_MIN *and* the divisor is -1,
   * so every other LLONG_MIN division falls through to DivInt.  That is fine,
   * because divisorVal == -1 was handled above and the division below can no
   * longer overflow.
   */
  assertx(dividendVal % divisorVal == 0);
  return cns(env, dividendVal / divisorVal);
}

SSATmp* simplifyAndInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto bit_and = [](int64_t a, int64_t b) { return a & b; };
  auto bit_or = [](int64_t a, int64_t b) { return a | b; };
  auto const simp = distributiveImpl(env, src1, src2, AndInt, OrInt,
                                     bit_and, bit_or);
  if (simp != nullptr) {
    return simp;
  }
  // X & X --> X
  if (src1 == src2) {
    return src1;
  }
  if (src2->hasConstVal()) {
    // X & 0 --> 0
    if (src2->intVal() == 0) {
      return cns(env, 0);
    }
    // X & (~0) --> X
    if (src2->intVal() == ~0L) {
      return src1;
    }
  }
  return nullptr;
}

SSATmp* simplifyOrInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  auto bit_and = [](int64_t a, int64_t b) { return a & b; };
  auto bit_or = [](int64_t a, int64_t b) { return a | b; };
  auto const simp = distributiveImpl(env, src1, src2, OrInt, AndInt,
                                     bit_or, bit_and);
  if (simp != nullptr) {
    return simp;
  }
  // X | X --> X
  if (src1 == src2) {
    return src1;
  }
  if (src2->hasConstVal()) {
    // X | 0 --> X
    if (src2->intVal() == 0) {
      return src1;
    }
    // X | (~0) --> ~0
    if (src2->intVal() == ~uint64_t(0)) {
      return cns(env, ~uint64_t(0));
    }
  }
  return nullptr;
}

SSATmp* simplifyXorInt(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto bitxor = [](int64_t a, int64_t b) { return a ^ b; };
  if (auto simp = commutativeImpl(env, src1, src2, XorInt, bitxor)) {
    return simp;
  }
  // X ^ X --> 0
  if (src1 == src2) return cns(env, 0);
  // X ^ 0 --> X
  if (src2->hasConstVal(0)) return src1;
  return nullptr;
}

SSATmp* xorTrueImpl(State& env, SSATmp* src) {
  auto const inst = src->inst();
  auto const op = inst->op();

  // !(X cmp Y) --> X opposite_cmp Y
  if (auto const negated = negateCmpOp(op)) {
    if (opcodeMayRaise(op)) return nullptr;
    auto const s0 = inst->src(0);
    auto const s1 = inst->src(1);
    // We can't add new uses to reference counted types without a more
    // advanced availability analysis.
    if (!s0->type().maybe(TCounted) && !s1->type().maybe(TCounted)) {
      return gen(env, *negated, s0, s1);
    }
    return nullptr;
  }

  switch (op) {
    // !!X --> X
    case XorBool:
      if (inst->src(1)->hasConstVal(true)) {
        // This is safe to add a new use to because inst->src(0) is a bool.
        assertx(inst->src(0)->isA(TBool));
        return inst->src(0);
      }
      return nullptr;
    case InstanceOfBitmask:
    case NInstanceOfBitmask:
      // This is safe because instanceofs don't take reference counted
      // arguments.
      assertx(!inst->src(0)->type().maybe(TCounted) &&
              !inst->src(1)->type().maybe(TCounted));
      return gen(
        env,
        (op == InstanceOfBitmask) ?
        NInstanceOfBitmask :
        InstanceOfBitmask,
        inst->src(0),
        inst->src(1)
      );
    default:
      break;
  }

  return nullptr;
}

SSATmp* simplifyXorBool(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  // Both constants.
  if (src1->hasConstVal() && src2->hasConstVal()) {
    return cns(env, bool(src1->boolVal() ^ src2->boolVal()));
  }

  // Canonicalize constants to the right.
  if (src1->hasConstVal() && !src2->hasConstVal()) {
    return gen(env, XorBool, src2, src1);
  }

  // X^0 => X
  if (src2->hasConstVal(false)) return src1;

  // X^1 => simplify "not" logic
  if (src2->hasConstVal(true)) return xorTrueImpl(env, src1);

  // X^X => false
  if (src1 == src2) return cns(env, false);

  return nullptr;
}

template<Opcode Opcode, class Oper>
SSATmp* shiftImpl(State& env, const IRInstruction* inst, Oper op) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  if (src1->hasConstVal()) {
    if (src1->intVal() == 0) {
      return cns(env, 0);
    }

    if (src2->hasConstVal()) {
      return cns(env, op(src1->intVal(), src2->intVal() & 63));
    }
  }

  if (src2->hasConstVal() && src2->intVal() == 0) {
    return src1;
  }

  auto const inst1 = src1->inst();
  // (X << A) << B --> X << (A + B) if (A + B) < 64
  if (inst1->is(Opcode) && inst1->src(1)->hasConstVal() && src2->hasConstVal()) {
    if (0 <= inst1->src(1)->intVal() && inst1->src(1)->intVal() < 64 &&
        0 <= src2->intVal() && src2->intVal() < 64) {
      auto operand = inst1->src(1)->intVal() + src2->intVal();
      // Restrict shifting by at most 63 to be platform independent
      if (operand < 64) {
        return gen(env, Opcode, inst1->src(0), cns(env, operand));
      }
    }
  }

  return nullptr;
}

SSATmp* simplifyShl(State& env, const IRInstruction* inst) {
  return shiftImpl<Shl>(env, inst,
                   [] (uint64_t a, int64_t b) -> int64_t {
                     return a << b;
                   });
}

SSATmp* simplifyLshr(State& env, const IRInstruction* inst) {
  return shiftImpl<Lshr>(env, inst,
                   [] (uint64_t a, int64_t b) -> int64_t {
                     return a >> b;
                   });
}

SSATmp* simplifyShr(State& env, const IRInstruction* inst) {
  return shiftImpl<Shr>(env, inst,
                   [] (int64_t a, int64_t b) {
                     // avoid implementation defined behavior
                     // gcc optimizes this to a signed right shift.
                     return a >= 0 ? a >> b : -(-(a+1) >> b) - 1;
                   });
}

// This function isn't meant to perform the actual comparison at
// compile-time. Instead, it performs the matching comparison against a
// primitive type (usually bool).
template<class T, class U>
static bool cmpOp(Opcode opc, T a, U b) {
  switch (opc) {
  case GtBool:
  case GtInt:
  case GtDbl:
  case GtStr:
  case GtObj:
  case GtArrLike:
  case GtRes: return a > b;
  case GteBool:
  case GteInt:
  case GteDbl:
  case GteStr:
  case GteObj:
  case GteArrLike:
  case GteRes: return a >= b;
  case LtBool:
  case LtInt:
  case LtDbl:
  case LtStr:
  case LtObj:
  case LtArrLike:
  case LtRes: return a < b;
  case LteBool:
  case LteInt:
  case LteDbl:
  case LteStr:
  case LteObj:
  case LteArrLike:
  case LteRes: return a <= b;
  case SameStr:
  case SameObj:
  case SameArrLike:
  case EqBool:
  case EqInt:
  case EqDbl:
  case EqStr:
  case EqObj:
  case EqArrLike:
  case EqRes: return a == b;
  case NSameStr:
  case NSameObj:
  case NSameArrLike:
  case NeqBool:
  case NeqInt:
  case NeqDbl:
  case NeqStr:
  case NeqObj:
  case NeqArrLike:
  case NeqRes: return a != b;
  default:
    always_assert(false);
  }
}

SSATmp* cmpBoolImpl(State& env,
                    Opcode opc,
                    const IRInstruction* const inst,
                    SSATmp* left,
                    SSATmp* right) {
  assertx(left->type() <= TBool);
  assertx(right->type() <= TBool);

  auto newInst = [&](Opcode op, SSATmp* src1, SSATmp* src2) {
    return gen(env, op, inst ? inst->taken() : (Block*)nullptr, src1, src2);
  };

  // Identity optimization
  if (left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  if (left->hasConstVal()) {
    // If both operands are constants, constant-fold them. Otherwise, move the
    // constant over to the right.
    if (right->hasConstVal()) {
      return cns(env, cmpOp(opc, left->boolVal(), right->boolVal()));
    } else {
      auto const newOpc = [](Opcode opcode) {
        switch (opcode) {
          case GtBool:  return LtBool;
          case GteBool: return LteBool;
          case LtBool:  return GtBool;
          case LteBool: return GteBool;
          case EqBool:  return EqBool;
          case NeqBool: return NeqBool;
          default: always_assert(false);
        }
      }(opc);
      return newInst(newOpc, right, left);
    }
  }

  if (right->hasConstVal()) {
    bool b = right->boolVal();

    // The result of the comparison might be independent of the truth
    // value of the LHS. If so, then simplify.
    if (cmpOp(opc, false, b) == cmpOp(opc, true, b)) {
      return cns(env, cmpOp(opc, false, b));
    }

    // There are only two distinct booleans - false and true (0 and 1).
    // From above, we know that (0 OP b) != (1 OP b).
    // Hence exactly one of (0 OP b) and (1 OP b) is true.
    // Hence there is exactly one boolean value of "left" that results in the
    // overall expression being true.
    // Hence we may check for equality with that boolean.
    if (opc != EqBool) {
      return newInst(EqBool, left, cns(env, !cmpOp(opc, false, b)));
    }

    // If we reach here, this is an equality comparison against a
    // constant. Testing for equality with true simplifies to just the left
    // operand, while equality with false is the negation of the left operand
    // (equivalent to XORing with true).
    return b ? left : newInst(XorBool, left, cns(env, true));
  }

  return nullptr;
}

SSATmp* cmpIntImpl(State& env,
                   Opcode opc,
                   const IRInstruction* const inst,
                   SSATmp* left,
                   SSATmp* right) {
  assertx(left->type() <= TInt);
  assertx(right->type() <= TInt);

  auto newInst = [&](Opcode op, SSATmp* src1, SSATmp* src2) {
    return gen(env, op, inst ? inst->taken() : (Block*)nullptr, src1, src2);
  };

  // Identity optimization
  if (left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  // Arithmetic optimization
  if (opc == EqInt || opc == NeqInt) {
    if (left->inst()->is(AddInt)) {
      auto const add = left->inst();
      if (add->src(0) == right) return gen(env, opc, cns(env, 0), add->src(1));
      if (add->src(1) == right) return gen(env, opc, cns(env, 0), add->src(0));
    }
    if (right->inst()->is(AddInt)) {
      auto const add = right->inst();
      if (add->src(0) == left) return gen(env, opc, cns(env, 0), add->src(1));
      if (add->src(1) == left) return gen(env, opc, cns(env, 0), add->src(0));
    }
  }

  if (left->hasConstVal()) {
    // If both operands are constants, constant-fold them. Otherwise, move the
    // constant over to the right.
    if (right->hasConstVal()) {
      return cns(env, cmpOp(opc, left->intVal(), right->intVal()));
    } else {
      auto const newOpc = [](Opcode opcode) {
        switch (opcode) {
          case GtInt:  return LtInt;
          case GteInt: return LteInt;
          case LtInt:  return GtInt;
          case LteInt: return GteInt;
          case EqInt:  return EqInt;
          case NeqInt: return NeqInt;
          default: always_assert(false);
        }
      }(opc);
      return newInst(newOpc, right, left);
    }
  }

  return nullptr;
}

SSATmp* cmpDblImpl(State& env,
                   Opcode opc,
                   const IRInstruction* const inst,
                   SSATmp* left,
                   SSATmp* right) {
  assertx(left->type() <= TDbl);
  assertx(right->type() <= TDbl);

  // Identity optimization is not safe because Nan's compare unequal.
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, cmpOp(opc, left->dblVal(), right->dblVal()));
  }

  return nullptr;
}

SSATmp* cmpStrImpl(State& env,
                   Opcode opc,
                   const IRInstruction* const inst,
                   SSATmp* left,
                   SSATmp* right) {
  assertx(left->type() <= TStr);
  assertx(right->type() <= TStr);

  auto newInst = [&](Opcode op, SSATmp* src1, SSATmp* src2) {
    return gen(env, op, inst ? inst->taken() : (Block*)nullptr, src1, src2);
  };

  // Identity optimization
  if (left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  if (left->hasConstVal()) {
    // If both operands are constants, constant-fold them. Otherwise, move the
    // constant over to the right.
    if (right->hasConstVal()) {
      if (opc == SameStr || opc == NSameStr) {
        return cns(
          env,
          cmpOp(opc, left->strVal()->same(right->strVal()), true)
        );
      } else if (opc == EqStr || opc == NeqStr) {
        return cns(
          env,
          cmpOp(opc, left->strVal()->equal(right->strVal()), true)
        );
      } else{
        return cns(
          env,
          cmpOp(opc, left->strVal()->compare(right->strVal()), 0)
        );
      }
    } else {
      auto const newOpc = [](Opcode opcode) {
        switch (opcode) {
          case GtStr:    return LtStr;
          case GteStr:   return LteStr;
          case LtStr:    return GtStr;
          case LteStr:   return GteStr;
          case EqStr:    return EqStr;
          case NeqStr:   return NeqStr;
          case SameStr:  return SameStr;
          case NSameStr: return NSameStr;
          default: always_assert(false);
        }
      }(opc);
      return newInst(newOpc, right, left);
    }
  }

  // Comparisons against the empty string can be optimized to checks on the
  // string length.
  if (right->hasConstVal() && right->strVal()->empty()) {
    const auto op =
     opc == EqStr || opc == SameStr || opc == LteStr ? EqInt : NeqInt;
    switch (opc) {
      case EqStr:
      case NeqStr: return gen(env, op, gen(env, LdStrLen, left), cns(env, 0));
      case SameStr:
      case LteStr:
      case NSameStr:
      case GtStr: return newInst(op, gen(env, LdStrLen, left), cns(env, 0));
      case LtStr: return cns(env, false);
      case GteStr: return cns(env, true);
      default: always_assert(false);
    }
  }

  return nullptr;
}

SSATmp* cmpObjImpl(State& env, Opcode opc, const IRInstruction* const /*inst*/,
                   SSATmp* left, SSATmp* right) {
  assertx(left->type() <= TObj);
  assertx(right->type() <= TObj);

  // Identity optimization. Object comparisons can produce arbitrary
  // side-effects, so we can only eliminate the comparison if its checking for
  // sameness.
  if ((opc == SameObj || opc == NSameObj) && left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  return nullptr;
}

SSATmp*
cmpKeysetImpl(State& env, Opcode opc, SSATmp* left, SSATmp* right) {
  assertx(left->type() <= TKeyset);
  assertx(right->type() <= TKeyset);

  // Unlike other array types, keyset comparisons can never throw or re-enter
  // (because they can only store integers and strings). Therefore we can fully
  // simplify equality comparisons if both arrays are constants.
  if (left->hasConstVal() && right->hasConstVal()) {
    auto const leftVal = left->keysetVal();
    auto const rightVal = right->keysetVal();
    switch (opc) {
      case EqArrLike:
        return cns(env, VanillaKeyset::Equal(leftVal, rightVal));
      case SameArrLike:
        return cns(env, VanillaKeyset::Same(leftVal, rightVal));
      case NeqArrLike:
        return cns(env, VanillaKeyset::NotEqual(leftVal, rightVal));
      case NSameArrLike:
        return cns(env, VanillaKeyset::NotSame(leftVal, rightVal));
      default:
        break;
    }
  }

  // Even if not a constant, we can apply an identity simplification as long as
  // we're doing an equality comparison.
  if ((opc == SameArrLike || opc == NSameArrLike ||
       opc == EqArrLike || opc == NeqArrLike)
      && left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  return nullptr;
}

SSATmp* cmpArrLikeImpl(State& env, Opcode opc,
                       const IRInstruction* const /*inst*/,
                       SSATmp* left, SSATmp* right) {
  assertx(left->type() <= TArrLike);
  assertx(right->type() <= TArrLike);

  if (left->type() <= TKeyset && right->type() <= TKeyset) {
    return cmpKeysetImpl(env, opc, left, right);
  }

  // Identity optimization. Array comparisons can produce arbitrary
  // side-effects, so we can only eliminate the comparison if its checking for
  // sameness.
  if ((opc == SameArrLike || opc == NSameArrLike) && left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  return nullptr;
}

SSATmp* cmpResImpl(State& env, Opcode opc, const IRInstruction* const /*inst*/,
                   SSATmp* left, SSATmp* right) {
  assertx(left->type() <= TRes);
  assertx(right->type() <= TRes);

  // Identity optimization.
  if (left == right) {
    return cns(env, cmpOp(opc, true, true));
  }

  return nullptr;
}

#define X(name, type)                                                   \
  SSATmp* simplify##name(State& env, const IRInstruction* i) {          \
    return cmp##type##Impl(env, i->op(), i, i->src(0), i->src(1));      \
  }

X(GtBool, Bool)
X(GteBool, Bool)
X(LtBool, Bool)
X(LteBool, Bool)
X(EqBool, Bool)
X(NeqBool, Bool)

X(GtInt, Int)
X(GteInt, Int)
X(LtInt, Int)
X(LteInt, Int)
X(EqInt, Int)
X(NeqInt, Int)

X(GtDbl, Dbl)
X(GteDbl, Dbl)
X(LtDbl, Dbl)
X(LteDbl, Dbl)
X(EqDbl, Dbl)
X(NeqDbl, Dbl)

X(GtStr, Str)
X(GteStr, Str)
X(LtStr, Str)
X(LteStr, Str)
X(EqStr, Str)
X(NeqStr, Str)
X(SameStr, Str)
X(NSameStr, Str)

X(GtObj, Obj)
X(GteObj, Obj)
X(LtObj, Obj)
X(LteObj, Obj)
X(EqObj, Obj)
X(NeqObj, Obj)
X(SameObj, Obj)
X(NSameObj, Obj)

X(GtArrLike, ArrLike)
X(GteArrLike, ArrLike)
X(LtArrLike, ArrLike)
X(LteArrLike, ArrLike)
X(EqArrLike, ArrLike)
X(NeqArrLike, ArrLike)
X(SameArrLike, ArrLike)
X(NSameArrLike, ArrLike)

X(GtRes, Res)
X(GteRes, Res)
X(LtRes, Res)
X(LteRes, Res)
X(EqRes, Res)
X(NeqRes, Res)

#undef X

SSATmp* simplifyEqCls(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  if (left == right) return cns(env, true);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, left->clsVal() == right->clsVal());
  }
  return nullptr;
}

SSATmp* simplifyEqLazyCls(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  if (left == right) return cns(env, true);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, left->lclsVal().name() == right->lclsVal().name());
  }
  return nullptr;
}

SSATmp* simplifyEqStrPtr(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  if (left == right) return cns(env, true);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, left->strVal() == right->strVal());
  }
  return nullptr;
}

SSATmp* simplifyEqArrayDataPtr(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  if (left == right) return cns(env, true);
  if (!left->type().maybe(right->type())) return cns(env, false);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, left->arrLikeVal() == right->arrLikeVal());
  }
  return nullptr;
}

SSATmp* simplifyCmpBool(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  assertx(left->type() <= TBool);
  assertx(right->type() <= TBool);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, HPHP::compare(left->boolVal(), right->boolVal()));
  }
  return nullptr;
}

SSATmp* simplifyCmpInt(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  assertx(left->type() <= TInt);
  assertx(right->type() <= TInt);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, HPHP::compare(left->intVal(), right->intVal()));
  }
  return nullptr;
}

SSATmp* simplifyCmpDbl(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  assertx(left->type() <= TDbl);
  assertx(right->type() <= TDbl);
  if (left->hasConstVal() && right->hasConstVal()) {
    return cns(env, HPHP::compare(left->dblVal(), right->dblVal()));
  }
  return nullptr;
}

SSATmp* simplifyCmpStr(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);

  assertx(left->type() <= TStr);
  assertx(right->type() <= TStr);

  auto newInst = [&](Opcode op, SSATmp* src1, SSATmp* src2) {
    return gen(env, op, inst ? inst->taken() : (Block*)nullptr, src1, src2);
  };

  if (left->hasConstVal()) {
    if (right->hasConstVal()) {
      return cns(env, HPHP::compare(left->strVal(), right->strVal()));
    } else if (left->strVal()->empty()) {
      // Comparisons against the empty string can be optimized to a comparison
      // on the string length.
      return newInst(CmpInt, cns(env, 0), gen(env, LdStrLen, right));
    }
  } else if (right->hasConstVal() && right->strVal()->empty()) {
    return newInst(CmpInt, gen(env, LdStrLen, left), cns(env, 0));
  }

  return nullptr;
}

SSATmp* simplifyCmpRes(State& env, const IRInstruction* inst) {
  auto const left = inst->src(0);
  auto const right = inst->src(1);
  assertx(left->type() <= TRes);
  assertx(right->type() <= TRes);
  return (left == right) ? cns(env, 0) : nullptr;
}

SSATmp* simplifyCGetPropQ(State& env, const IRInstruction* inst) {
  if (inst->src(0)->isA(TNull)) return cns(env, TInitNull);
  return nullptr;
}

SSATmp* instanceOfImpl(State& env, SSATmp* ssatmp1, ClassSpec spec2,
                       bool canCheckVtable) {
  assertx(ssatmp1->type() <= TCls);
  if (!spec2) return nullptr;
  auto const cls2 = spec2.cls();

  ClassSpec spec1 = ssatmp1->type().clsSpec();
  if (spec2.exact() && spec1 && spec1.cls()->classof(cls2)) {
    return cns(env, true);
  }

  // If spec2 is exact and we have an instance bit for it, use
  // InstanceOfBitmask.
  const bool useInstanceBits = InstanceBits::initted() ||
                               env.unit.context().kind == TransKind::Optimize;
  if (spec2.exact() && useInstanceBits) {
    InstanceBits::init();
    if (InstanceBits::lookup(cls2->name()) != 0) {
      return gen(env, InstanceOfBitmask, ssatmp1, cns(env, cls2->name()));
    }
  }

  // If spec2 is an interface and we've assigned it a vtable slot, use that.
  if (canCheckVtable && spec2.exact() && isInterface(cls2)
      && Cfg::Repo::Authoritative) {
    auto const slot = cls2->preClass()->ifaceVtableSlot();
    if (slot != kInvalidSlot) {
      auto const data = InstanceOfIfaceVtableData{spec2.cls(), true};
      return gen(env, InstanceOfIfaceVtable, data, ssatmp1);
    }
  }

  if (!spec1) return nullptr;
  auto const cls1 = spec1.cls();

  if (cls1->classof(cls2)) {
    assertx(!spec2.exact()); // the exact case is handled above
    return nullptr;
  }

  if (isInterface(cls1)) return nullptr;

  if (spec1.exact()) return cns(env, false);

  // At this point cls1 is not a cls2, and its not exact, so:
  //
  //  - If cls2 is an interface, a descendent of cls1 could implement
  //    that interface
  //  - If cls2 is a descendent of cls1, then (clearly) a descendent
  //    of cls1 could be a cls2
  //  - Otherwise no descendent of cls1 can be a cls2 or any class
  //    that isa cls2
  if (!isInterface(cls2) && !cls2->classof(cls1)) {
    return cns(env, false);
  }

  return nullptr;
}

SSATmp* simplifyInstanceOf(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto const extra = inst->extra<InstanceOfData>();
  auto const canCheckVtable = extra->canCheckVtable;

  if (src2->isA(TNullptr)) {
    return cns(env, false);
  }

  auto const spec2 = src2->type().clsSpec();

  if (auto const cls = spec2.exactCls()) {
    if (isNormalClass(cls) && (cls->attrs() & AttrPersistent)) {
      return gen(env, ExtendsClass, ExtendsClassData{ cls }, src1);
    }
    if (isInterface(cls) && (cls->attrs() & AttrPersistent)) {
      return gen(env, InstanceOfIface, *extra, src1, cns(env, cls->name()));
    }
  }

  return instanceOfImpl(env, src1, src2->type().clsSpec(), canCheckVtable);
}

SSATmp* simplifyExtendsClass(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const cls2 = inst->extra<ExtendsClassData>()->cls;
  assertx(cls2 && isNormalClass(cls2));
  auto const spec2 = ClassSpec{cls2, ClassSpec::ExactTag{}};
  return instanceOfImpl(env, src1, spec2, false);
}

SSATmp* simplifyInstanceOfBitmask(State& env, const IRInstruction* inst) {
  auto const cls = inst->src(0);
  auto const name = inst->src(1);

  if (!name->hasConstVal(TStr)) return nullptr;

  auto const bit = InstanceBits::lookup(name->strVal());
  always_assert(bit && "cgInstanceOfBitmask had no bitmask");

  if (cls->type().clsSpec() &&
      cls->type().clsSpec().cls()->checkInstanceBit(bit)) {
    return cns(env, true);
  }

  if (!cls->hasConstVal(TCls)) return nullptr;
  return cns(env, false);
}

SSATmp* simplifyNInstanceOfBitmask(State& env, const IRInstruction* inst) {
  auto const cls = inst->src(0);
  auto const name = inst->src(1);

  if (!name->hasConstVal(TStr)) return nullptr;

  auto const bit = InstanceBits::lookup(name->strVal());
  always_assert(bit && "cgNInstanceOfBitmask had no bitmask");

  if (cls->type().clsSpec() &&
      cls->type().clsSpec().cls()->checkInstanceBit(bit)) {
    return cns(env, false);
  }

  if (!cls->hasConstVal(TCls)) return nullptr;
  return cns(env, true);
}

SSATmp* simplifyDeserializeLazyProp(State& env, const IRInstruction* inst) {
  auto const cls = inst->src(0)->type().clsSpec().cls();
  return cls->mayUseLazyAPCDeserialization() ? nullptr : gen(env, Nop);
}

SSATmp* simplifyInstanceOfIface(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto const canCheckVtable = inst->extra<InstanceOfData>()->canCheckVtable;

  // We only emit InstanceOfIface if we checked that we could trust the class.
  // Just grab it from the named-entity map.
  auto const ne = NamedType::getOrCreate(src2->strVal());
  auto const cls2 = ne->clsList();
  assertx(cls2 && isInterface(cls2));
  auto const spec2 = ClassSpec{cls2, ClassSpec::ExactTag{}};

  return instanceOfImpl(env, src1, spec2, canCheckVtable);
}

SSATmp* simplifyInstanceOfIfaceVtable(State& env, const IRInstruction* inst) {
  if (!inst->extra<InstanceOfIfaceVtable>()->canOptimize) return nullptr;
  auto const cls = inst->src(0);
  auto const iface = inst->extra<InstanceOfIfaceVtable>()->cls;
  if (cls->type().clsSpec() &&
      cls->type().clsSpec().cls()->classof(iface)) {
    return cns(env, true);
  }

  const bool useInstanceBits = InstanceBits::initted() ||
                               env.unit.context().kind == TransKind::Optimize;
  if (useInstanceBits) {
    InstanceBits::init();
    auto const ifaceName = iface->name();
    if (InstanceBits::lookup(ifaceName) != 0) {
      return gen(env, InstanceOfBitmask, cls, cns(env, ifaceName));
    }
  }
  return nullptr;
}

SSATmp* simplifyMethodExists(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);

  auto const clsSpec = src1->type().clsSpec();

  if (clsSpec.cls() != nullptr && src2->hasConstVal(TStr)) {
    // If we don't have an exact type, then we can't say for sure the class
    // doesn't have the method.
    auto const result = clsSpec.cls()->lookupMethod(src2->strVal()) != nullptr;
    return (clsSpec.exact() || result) ? cns(env, result) : nullptr;
  }
  return nullptr;
}

static auto concat_litstrs(State& env, SSATmp* s1, SSATmp* s2) {
  auto const str1 = const_cast<StringData*>(s1->strVal());
  auto const str2 = const_cast<StringData*>(s2->strVal());
  auto const sval = OptString::attach(concat_ss(str1, str2));
  return cns(env, makeStaticString(sval.get()));
}

SSATmp* simplifyConcatStrStr(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  if (src1->hasConstVal(TStaticStr) &&
      src2->hasConstVal(TStaticStr)) {
    return concat_litstrs(env, src1, src2);
  }

  // ConcatStrStr consumes a reference to src1 and produces a reference, so
  // anything we replace it with must do the same thing.
  if (src1->hasConstVal(staticEmptyString())) {
    // Produce a reference on src2.
    gen(env, IncRef, src2);
    return src2;
  }
  if (src2->hasConstVal(staticEmptyString())) {
    // Forward the reference on src1 from input to output.
    return src1;
  }

  return nullptr;
}

SSATmp* simplifyConcatStr3(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto const src3 = inst->src(2);

  if (src2->hasConstVal(TStaticStr)) {
    if (src1->hasConstVal(TStaticStr)) {
      return gen(env, ConcatStrStr, inst->taken(),
                 concat_litstrs(env, src1, src2), src3);
    }
    if (src3->hasConstVal(TStaticStr)) {
      return gen(env, ConcatStrStr, inst->taken(),
                 src1, concat_litstrs(env, src2, src3));
    }
    if (src2->hasConstVal(staticEmptyString())) {
      return gen(env, ConcatStrStr, inst->taken(), src1, src3);
    }
  }

  // ConcatStr3 consumes a reference to src1 and produces a reference, so
  // anything we replace it with must do the same thing.
  if (src1->hasConstVal(staticEmptyString())) {
    // Compensate for ConcatStrStr consuming src2.
    gen(env, IncRef, src2);
    return gen(env, ConcatStrStr, inst->taken(), src2, src3);
  }
  if (src3->hasConstVal(staticEmptyString())) {
    // ConcatStrStr also consumes a reference to src1
    return gen(env, ConcatStrStr, inst->taken(), src1, src2);
  }

  return nullptr;
}

SSATmp* simplifyConcatStr4(State& env, const IRInstruction* inst) {
  auto const src1 = inst->src(0);
  auto const src2 = inst->src(1);
  auto const src3 = inst->src(2);
  auto const src4 = inst->src(3);

  if (src2->hasConstVal(TStaticStr)) {
    if (src1->hasConstVal(TStaticStr)) {
      return gen(env, ConcatStr3, inst->taken(),
                 concat_litstrs(env, src1, src2), src3, src4);
    }
    if (src3->hasConstVal(TStaticStr)) {
      return gen(env, ConcatStr3, inst->taken(),
                 src1, concat_litstrs(env, src2, src3), src4);
    }
    if (src2->hasConstVal(staticEmptyString())) {
      return gen(env, ConcatStr3, inst->taken(), src1, src3, src4);
    }
  }

  if (src3->hasConstVal(TStaticStr)) {
    if (src4->hasConstVal(TStaticStr)) {
      return gen(env, ConcatStr3, inst->taken(),
                 src1, src2, concat_litstrs(env, src3, src4));
    }
    if (src3->hasConstVal(staticEmptyString())) {
      return gen(env, ConcatStr3, inst->taken(), src1, src2, src4);
    }
  }

  // ConcatStr4 consumes a reference to src1 and produces a reference, so
  // anything we replace it with must do the same thing.
  if (src1->hasConstVal(staticEmptyString())) {
    // Compensate for ConcatStrStr consuming src2.
    gen(env, IncRef, src2);
    return gen(env, ConcatStr3, inst->taken(), src2, src3, src4);
  }
  if (src4->hasConstVal(staticEmptyString())) {
    // ConcatStr3 also consumes a reference to src1
    return gen(env, ConcatStr3, inst->taken(), src1, src2, src3);
  }

  return nullptr;
}

SSATmp* simplifyConcatIntStr(State& env, const IRInstruction* inst) {
  auto const lhs = inst->src(0);
  auto const rhs = inst->src(1);

  if (lhs->hasConstVal()) {
    auto const lhsStr =
      cns(env, makeStaticString(folly::to<std::string>(lhs->intVal())));
    return gen(env, ConcatStrStr, inst->taken(), lhsStr, rhs);
  }
  if (rhs->hasConstVal(staticEmptyString())) {
    return gen(env, ConvIntToStr, lhs);
  }

  return nullptr;
}

SSATmp* simplifyConcatStrInt(State& env, const IRInstruction* inst) {
  auto const lhs = inst->src(0);
  auto const rhs = inst->src(1);

  if (rhs->hasConstVal()) {
    auto const rhsStr =
      cns(env, makeStaticString(folly::to<std::string>(rhs->intVal())));
    return gen(env, ConcatStrStr, inst->taken(), lhs, rhsStr);
  }
  if (lhs->hasConstVal(staticEmptyString())) {
    return gen(env, ConvIntToStr, rhs);
  }

  return nullptr;
}

namespace {

template <typename C>
SSATmp* arrayLikeConvImpl(State& env, const IRInstruction* inst, C convert) {
  auto const src = inst->src(0);
  if (!src->hasConstVal()) return nullptr;
  auto const before = src->arrLikeVal();
  auto converted = convert(const_cast<ArrayData*>(before));
  if (!converted) return nullptr;
  ArrayData::GetScalarArray(&converted);
  return cns(env, converted);
}

}

SSATmp* simplifyConvArrLikeToVec(State& env, const IRInstruction* inst) {
  return arrayLikeConvImpl(
    env, inst,
    [&](ArrayData* a) { return a->toVec(true); }
  );
}

SSATmp* simplifyConvArrLikeToDict(State& env, const IRInstruction* inst) {
  return arrayLikeConvImpl(
    env, inst,
    [&](ArrayData* a) { return a->toDict(true); }
  );
}

SSATmp* simplifyConvArrLikeToKeyset(State& env, const IRInstruction* inst) {
  return arrayLikeConvImpl(
    env, inst,
    [&](ArrayData* a) {
      // We need to check if the array contains values suitable for keyset
      // before attempting the conversion. Otherwise, toKeyset() might re-enter
      // which we can't do from the simplifier.
      bool keylike = true;
      IterateV(
        a,
        [&](TypedValue v) {
          if (!isIntType(v.m_type) && !isStringType(v.m_type)) {
            keylike = false;
            return true;
          }
          return false;
        }
      );
      return keylike ? a->toKeyset(true) : nullptr;
    }
  );
}

SSATmp* simplifyConvDblToBool(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    bool const bval = src->dblVal();
    return cns(env, bval);
  }
  return nullptr;
}

SSATmp* simplifyConvIntToBool(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    bool const bval = src->intVal();
    return cns(env, bval);
  }
  return nullptr;
}

SSATmp* simplifyConvStrToBool(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    // only the strings "", and "0" convert to false, all other strings
    // are converted to true
    auto const str = src->strVal();
    return cns(env, !str->empty() && !str->isZero());
  }
  return nullptr;
}

SSATmp* simplifyConvBoolToDbl(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    double const dval = src->boolVal();
    return cns(env, dval);
  }
  return nullptr;
}

SSATmp* simplifyConvIntToDbl(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    double const dval = src->intVal();
    return cns(env, dval);
  }
  if (src->inst()->is(ConvBoolToInt)) {
    // This is safe, because the bool src is not reference counted.
    return gen(env, ConvBoolToDbl, src->inst()->src(0));
  }
  return nullptr;
}

SSATmp* simplifyConvStrToDbl(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal() ? cns(env, src->strVal()->toDouble()) : nullptr;
}

SSATmp* simplifyConvBoolToInt(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) return cns(env, static_cast<int>(src->boolVal()));
  return nullptr;
}

SSATmp* simplifyConvDblToInt(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) return cns(env, double_to_int64(src->dblVal()));
  return nullptr;
}

SSATmp* simplifyConvStrToInt(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal() ? cns(env, src->strVal()->toInt64()) : nullptr;
}

SSATmp* simplifyConvDblToStr(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    auto const dblStr = OptString::attach(buildStringData(src->dblVal()));
    return cns(env, makeStaticString(dblStr));
  }
  return nullptr;
}

SSATmp* simplifyConvIntToStr(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    return cns(env,
      makeStaticString(folly::to<std::string>(src->intVal()))
    );
  }
  return nullptr;
}

namespace {
const StaticString
    s_msgArrToStr("Array to string conversion"),
    s_msgVecToStr("Vec to string conversion"),
    s_msgDictToStr("Dict to string conversion"),
    s_msgKeysetToStr("Keyset to string conversion"),
    s_msgClsMethToStr("Cannot convert class method to string"),
    s_msgClsMethToInt("Cannot convert class method to int"),
    s_msgClsMethToIntImpl("Implicit class method to int conversion"),
    s_msgClsMethToDbl("Cannot convert class method to float"),
    s_msgClsMethToDblImpl("Implicit class method to double conversion");
}

SSATmp* simplifyConvTVToBool(State& env, const IRInstruction* inst) {
  auto const src     = inst->src(0);
  auto const srcType = src->type();

  if (srcType <= TBool) return src;
  if (srcType <= TNull) return cns(env, false);
  if (srcType <= TVec) {
    auto const length = gen(env, CountVec, src);
    return gen(env, NeqInt, length, cns(env, 0));
  }
  if (srcType <= TDict) {
    auto const length = gen(env, CountDict, src);
    return gen(env, NeqInt, length, cns(env, 0));
  }
  if (srcType <= TKeyset) {
    auto const length = gen(env, CountKeyset, src);
    return gen(env, NeqInt, length, cns(env, 0));
  }
  if (srcType <= TDbl)  return gen(env, ConvDblToBool, src);
  if (srcType <= TInt)  return gen(env, ConvIntToBool, src);
  if (srcType <= TStr)  return gen(env, ConvStrToBool, src);
  if (srcType <= TObj) {
    if (auto const cls = srcType.clsSpec().cls()) {
      // We need to exclude interfaces like ConstSet.  For now, just
      // skip anything that's an interface or extension.
      if (!(cls->attrs() & AttrInterface)) {
        if (!cls->instanceCtor()) {
          return cns(env, true);
        }
      }
    }
    return gen(env, ConvObjToBool, inst->taken(), src);
  }
  if (srcType <= TRes)  return cns(env, true);
  if (srcType <= TFunc) return cns(env, true);
  if (srcType <= TClsMeth) return cns(env, true);

  return nullptr;
}

// return true if throws
bool handleConvThrowMode(
  State& env,
  Block* trace,
  const ConvThrowData* data,
  const char* const from,
  const char* const to) {
  if (data->mode == ConvThrowMode::None) return false;
  const auto str = cns(env, makeStaticString(folly::sformat(
    "Attempted {} to {} conversion for {}", from, to,
    s_ConvThrowReasonConcat.get())));
  gen(env, ThrowInvalidOperation, trace, str);
  return true;
}

SSATmp* simplifyConvTVToStr(State& env, const IRInstruction* inst) {
  auto const src           = inst->src(0);
  auto const srcType       = src->type();
  auto const catchTrace    = inst->taken();
  auto const data   = inst->extra<ConvThrowData>();

  if (srcType <= TBool) {
    const auto tmp = gen(
      env,
      Select,
      src,
      cns(env, s_1.get()),
      cns(env, staticEmptyString())
    );
    auto throws = handleConvThrowMode(
      env, catchTrace, data, "bool", "string");
    return throws ? cns(env, TBottom) : tmp ;
  }
  if (srcType <= TNull) {
    auto throws = handleConvThrowMode(
      env, catchTrace, data, "null", "string");
    return throws ? cns(env, TBottom) : cns(env, staticEmptyString());
  }
  if (srcType <= TVec) {
    gen(env, ThrowInvalidOperation, catchTrace, cns(env, s_msgVecToStr.get()));
    return cns(env, TBottom);
  }
  if (srcType <= TDict) {
    gen(env, ThrowInvalidOperation, catchTrace, cns(env, s_msgDictToStr.get()));
    return cns(env, TBottom);
  }
  if (srcType <= TKeyset) {
    auto const message = cns(env, s_msgKeysetToStr.get());
    gen(env, ThrowInvalidOperation, catchTrace, message);
    return cns(env, TBottom);
  }

  if (srcType <= TDbl) {
    const auto tmp = gen(env, ConvDblToStr, src);
    auto throws = handleConvThrowMode(
      env, catchTrace, data, "double", "string");
    return throws ? cns(env, TBottom) : tmp;
  }
  if (srcType <= TInt)    return gen(env, ConvIntToStr, src);
  if (srcType <= TStr) {
    gen(env, IncRef, src);
    return src;
  }
  if (srcType <= TObj)    return gen(env, ConvObjToStr, catchTrace, src);
  if (srcType <= TClsMeth) {
    auto const message = cns(env, s_msgClsMethToStr.get());
    gen(env, ThrowInvalidOperation, catchTrace, message);
    return cns(env, TBottom);
  }

  auto classConversionNotice = [&] () {
    if (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0) {
      std::string msg;
      string_printf(msg, Strings::CLASS_TO_STRING_IMPLICIT, "string conversion");
      gen(env,
        RaiseNotice,
        SampleRateData { Cfg::Eval::RaiseClassConversionNoticeSampleRate },
        catchTrace,
        cns(env, makeStaticString(msg)));
    };
  };

  if (srcType <= TCls) {
    auto const tmp = gen(env, LdClsName, src);
    classConversionNotice();
    return tmp;
  }
  if (srcType <= TLazyCls) {
    auto const tmp = gen(env, LdLazyClsName, src);
    classConversionNotice();
    return tmp;
  }

  return nullptr;
}

SSATmp* simplifyConvTVToInt(State& env, const IRInstruction* inst) {
  auto const src      = inst->src(0);
  auto const srcType  = src->type();
  auto const catchTrace = inst->taken();

  if (srcType <= TInt)  return src;
  if (srcType <= TNull) return cns(env, 0);

  auto genFromArray = [&](Opcode op) {
    return gen(env, Select, gen(env, op, src), cns(env, 1), cns(env, 0));
  };

  if (srcType <= TVec)    return genFromArray(CountVec);
  if (srcType <= TDict)   return genFromArray(CountDict);
  if (srcType <= TKeyset) return genFromArray(CountKeyset);
  if (srcType <= TBool) return gen(env, ConvBoolToInt, src);
  if (srcType <= TDbl)  return gen(env, ConvDblToInt, src);
  if (srcType <= TStr)  return gen(env, ConvStrToInt, src);
  if (srcType <= TObj)  return gen(env, ConvObjToInt, catchTrace, src);
  if (srcType <= TRes)  return gen(env, ConvResToInt, src);
  if (srcType <= TClsMeth) {
    gen(env, ThrowInvalidOperation, catchTrace,
        cns(env, s_msgClsMethToInt.get()));
    return cns(env, TBottom);
  }
  return nullptr;
}

SSATmp* simplifyConvTVToDbl(State& env, const IRInstruction* inst) {
  auto const src      = inst->src(0);
  auto const srcType  = src->type();
  auto const catchTrace = inst->taken();

  if (srcType <= TDbl)  return src;
  if (srcType <= TNull) return cns(env, 0.0);
  if (srcType <= TVec) {
    auto const length = gen(env, CountVec, src);
    return gen(env, ConvBoolToDbl, gen(env, ConvIntToBool, length));
  }
  if (srcType <= TDict) {
    auto const length = gen(env, CountDict, src);
    return gen(env, ConvBoolToDbl, gen(env, ConvIntToBool, length));
  }
  if (srcType <= TKeyset) {
    auto const length = gen(env, CountKeyset, src);
    return gen(env, ConvBoolToDbl, gen(env, ConvIntToBool, length));
  }
  if (srcType <= TBool) return gen(env, ConvBoolToDbl, src);
  if (srcType <= TInt)  return gen(env, ConvIntToDbl, src);
  if (srcType <= TStr)  return gen(env, ConvStrToDbl, src);
  if (srcType <= TObj)  return gen(env, ConvObjToDbl, inst->taken(), src);
  if (srcType <= TRes)  return gen(env, ConvResToDbl, src);
  if (srcType <= TClsMeth) {
    gen(env, ThrowInvalidOperation, catchTrace,
        cns(env, s_msgClsMethToDbl.get()));
    return cns(env, TBottom);
  }

  return nullptr;
}

SSATmp* simplifyConvObjToBool(State& env, const IRInstruction* inst) {
  auto const ty = inst->src(0)->type();

  if (ty < TObj &&
      ty.clsSpec().cls() &&
      ty.clsSpec().cls()->isCollectionClass()) {
    if (Cfg::Eval::NoticeOnCollectionToBool) return nullptr;
    return gen(env, ColIsNEmpty, inst->src(0));
  }
  return nullptr;
}


SSATmp* simplifyDblAsBits(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    union {
      int64_t i;
      double d;
    };
    d = src->dblVal();
    return cns(env, i);
  }
  return nullptr;
}

SSATmp* simplifyIntAsPtrToElem(State&, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->inst()->is(PtrToElemAsInt)) return src->inst()->src(0);
  return nullptr;
}

SSATmp* simplifyPtrToElemAsInt(State&, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->inst()->is(IntAsPtrToElem)) return src->inst()->src(0);
  return nullptr;
}

SSATmp* roundImpl(State& env, const IRInstruction* inst, double (*op)(double)) {
  auto const src  = inst->src(0);

  if (src->hasConstVal()) {
    return cns(env, op(src->dblVal()));
  }

  auto const srcInst = src->inst();
  if (srcInst->op() == ConvIntToDbl || srcInst->op() == ConvBoolToDbl) {
    return src;
  }

  return nullptr;
}

SSATmp* simplifyFloor(State& env, const IRInstruction* inst) {
  return roundImpl(env, inst, floor);
}

SSATmp* simplifyCeil(State& env, const IRInstruction* inst) {
  return roundImpl(env, inst, ceil);
}

SSATmp* simplifyCheckRDSInitialized(State& env, const IRInstruction* inst) {
  auto const handle = inst->extra<CheckRDSInitialized>()->handle;
  if (!rds::isNormalHandle(handle)) return gen(env, Jmp, inst->next());
  return mergeBranchDests(env, inst);
}

SSATmp* simplifyMarkRDSInitialized(State& env, const IRInstruction* inst) {
  auto const handle = inst->extra<MarkRDSInitialized>()->handle;
  if (!rds::isNormalHandle(handle)) return gen(env, Nop);
  return nullptr;
}

SSATmp* simplifyMarkRDSAccess(State& env, const IRInstruction* inst) {
  auto const handle = inst->extra<MarkRDSAccess>()->handle;
  auto const profile = rds::profileForHandle(handle);
  if (profile == rds::kUninitHandle) return gen(env, Nop);
  return nullptr;
}

SSATmp* simplifyInitObjProps(State& env, const IRInstruction* inst) {
  auto const cls = inst->extra<InitObjProps>()->cls;
  if (cls->numDeclProperties() == 0 && !cls->hasMemoSlots()) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* simplifyInitObjMemoSlots(State& env, const IRInstruction* inst) {
  auto const cls = inst->extra<InitObjMemoSlots>()->cls;
  if (!cls->hasMemoSlots()) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* simplifyCheckType(State& env, const IRInstruction* inst) {
  auto const typeParam = inst->typeParam();
  auto const srcType = inst->src(0)->type();
  assertx(inst->taken() != nullptr);

  if (!srcType.maybe(typeParam) ||
      (inst->next() != nullptr && inst->next()->isUnreachable())) {
    /*
     * Convert the check into a Jmp.  The dest of the CheckType (which would've
     * been Bottom) is now never going to be defined, so we return a Bottom.
     */
    gen(env, Jmp, inst->taken());
    return cns(env, TBottom);
  }

  if (inst->next() != nullptr &&
      ultimateDst(inst->next()) == ultimateDst(inst->taken())) {
    gen(env, Jmp, ultimateDst(inst->taken()));
    return cns(env, TBottom);
  }

  auto const newType = srcType & typeParam;
  if (srcType <= newType) {
    // The type of the src is the same or more refined than type, so the guard
    // is unnecessary.
    return inst->src(0);
  }

  if (inst->taken() && inst->taken()->isUnreachable()) {
    return gen(env, AssertType, newType, inst->src(0));
  }

  return nullptr;
}

SSATmp* simplifyAssertType(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);

  auto const newType = src->type().refine(inst->typeParam());
  if (newType == TBottom) {
    return cns(env, TBottom);
  }

  return src->isA(newType) ? src : nullptr;
}

SSATmp* simplifyCheckNonNull(State& env, const IRInstruction* inst) {
  auto const type = inst->src(0)->type();
  assertx(inst->taken() != nullptr);

  if (type <= TNullptr ||
      (inst->next() != nullptr && inst->next()->isUnreachable())) {
    gen(env, Jmp, inst->taken());
    return cns(env, TBottom);
  }

  if (inst->next() != nullptr &&
      ultimateDst(inst->next()) == ultimateDst(inst->taken())) {
    gen(env, Jmp, ultimateDst(inst->taken()));
    return cns(env, TBottom);
  }

  if (inst->taken() && inst->taken()->isUnreachable()) {
    return gen(env, AssertType, type - TNullptr, inst->src(0));
  }

  if (!type.maybe(TNullptr)) return inst->src(0);

  return nullptr;
}

SSATmp* simplifyDefLabel(State& env, const IRInstruction* inst) {
  if (inst->numDsts() == 0) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* decRefImpl(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (noop_decref || !src->type().maybe(TCounted)) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* simplifyDecRef(State& env, const IRInstruction* inst) {
  return decRefImpl(env, inst);
}

SSATmp* simplifyDecRefNZ(State& env, const IRInstruction* inst) {
  return decRefImpl(env, inst);
}

SSATmp* simplifyDecReleaseCheck(State& env, const IRInstruction* inst) {
  if (inst->src(0)->type() <= TUncounted) {
    return gen(env, Jmp, inst->taken());
  }
  return nullptr;
}

SSATmp* simplifyIncRef(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (!src->type().maybe(TCounted)) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* condJmpImpl(State& env, const IRInstruction* inst) {
  assertx(inst->is(JmpZero, JmpNZero));
  assertx(inst->taken() != nullptr);

  // Both ways go to the same block.
  if (inst->next() != nullptr &&
      ultimateDst(inst->next()) == ultimateDst(inst->taken())) {
    return gen(env, Jmp, ultimateDst(inst->taken()));
  }

  if (inst->taken() && inst->taken()->isUnreachable()) {
    return gen(env, Nop);
  }

  if (inst->next() && inst->next()->isUnreachable()) {
    return gen(env, Jmp, inst->taken());
  }

  auto const src = inst->src(0);
  auto const srcInst = src->inst();

  // Constant propagate.
  if (src->hasConstVal()) {
    bool val = src->isA(TBool) ? src->boolVal()
                               : static_cast<bool>(src->intVal());
    if (val == inst->is(JmpNZero)) {
      // always taken
      assertx(inst->taken());
      return gen(env, Jmp, inst->taken());
    }
    // Never taken. Since simplify() is also run when building the IR,
    // inst->next() could be nullptr at this moment.
    return gen(env, Nop);
  }

  auto absorb = [&](){
    return gen(env, inst->op(), inst->taken(), srcInst->src(0));
  };
  auto absorbOpp = [&](){
    return gen(env,
               inst->op() == JmpZero ? JmpNZero : JmpZero,
               inst->taken(),
               srcInst->src(0)
              );
  };

  // Absorb negations.
  if (srcInst->is(XorBool) && srcInst->src(1)->hasConstVal(true)) {
    return absorbOpp();
  }

  // Absorb ConvIntToBool.
  if (srcInst->is(ConvIntToBool)) {
    return absorb();
  }
  if (srcInst->is(ConvBoolToInt)) {
    return absorb();
  }

  // Absorb boolean comparisons.
  if (srcInst->is(EqBool) && srcInst->src(1)->hasConstVal()) {
    return srcInst->src(1)->boolVal() ? absorb() : absorbOpp();
  }
  if (srcInst->is(NeqBool) && srcInst->src(1)->hasConstVal()) {
    return srcInst->src(1)->boolVal() ? absorbOpp() : absorb();
  }

  // Absorb integer comparisons against constant zero.
  if (srcInst->is(EqInt) && srcInst->src(1)->hasConstVal(0)) {
    return absorbOpp();
  }
  if (srcInst->is(NeqInt) && srcInst->src(1)->hasConstVal(0)) {
    return absorb();
  }

  return nullptr;
}

SSATmp* simplifyJmpZero(State& env, const IRInstruction* i) {
  return condJmpImpl(env, i);
}

SSATmp* simplifyJmpNZero(State& env, const IRInstruction* i) {
  return condJmpImpl(env, i);
}

SSATmp* simplifyJmp(State& env, const IRInstruction* inst) {
  assertx(inst->taken());
  if (inst->taken()->isUnreachable()) {
    return gen(env, Unreachable, *inst->taken()->back().extra<Unreachable>());
  }

  return nullptr;
}

SSATmp* simplifySelect(State& env, const IRInstruction* inst) {
  auto const cond = inst->src(0);
  auto const tval = inst->src(1);
  auto const fval = inst->src(2);

  // Simplifications based on condition:

  if (cond->hasConstVal()) {
    auto const cval = cond->isA(TBool)
      ? cond->boolVal()
      : static_cast<bool>(cond->intVal());
    return cval ? tval : fval;
  }

  // The condition isn't statically known, but could be computed from a
  // different operation we can absorb.
  auto const condInst = cond->inst();
  auto const absorb = [&]{
    return gen(env, Select, condInst->src(0), tval, fval);
  };
  auto const absorbOpp = [&]{
    return gen(env, Select, condInst->src(0), fval, tval);
  };

  // Conversions between int and bool can't change which value is selected.
  if (condInst->is(ConvIntToBool)) return absorb();
  if (condInst->is(ConvBoolToInt)) return absorb();

  // Condition is negated, so check the pre-negated condition with flipped
  // values.
  if (condInst->is(XorBool) && condInst->src(1)->hasConstVal(true)) {
    return absorbOpp();
  }

  // Condition comes from comparisons against true/false or 0, which is
  // equivalent to selecting on original value (possibly with values flipped).
  if (condInst->is(EqBool) && condInst->src(1)->hasConstVal()) {
    return condInst->src(1)->boolVal() ? absorb() : absorbOpp();
  }
  if (condInst->is(NeqBool) && condInst->src(1)->hasConstVal()) {
    return condInst->src(1)->boolVal() ? absorbOpp() : absorb();
  }
  if (condInst->is(EqInt) && condInst->src(1)->hasConstVal(0)) {
    return absorbOpp();
  }
  if (condInst->is(NeqInt) && condInst->src(1)->hasConstVal(0)) {
    return absorb();
  }

  // Condition comes from Select C, X, 0 or Select C, 0, X (where X != 0), which
  // is equivalent to selecting on C directly.
  if (condInst->is(Select)) {
    auto const condInstT = condInst->src(1);
    auto const condInstF = condInst->src(2);
    if (condInstT->hasConstVal(TInt) &&
        condInstT->intVal() != 0 &&
        condInstF->hasConstVal(0)) {
      return absorb();
    }
    if (condInstT->hasConstVal(0) &&
        condInstF->hasConstVal(TInt) &&
        condInstF->intVal() != 0) {
      return absorbOpp();
    }
  }

  // Simplifications based on known value choices:

  // If the two values are the same tmp, or if they're both constants with the
  // same value, no need to do a select, as we'll always get the same value.
  if (tval == fval) return tval;
  if ((tval->type() == fval->type()) &&
      (tval->type().hasConstVal() ||
       tval->type().subtypeOfAny(TUninit, TInitNull, TNullptr))) {
    return tval;
  }

  // If either value isn't satisfiable, assume it comes from unreachable code.
  if (tval->isA(TBottom)) return fval;
  if (fval->isA(TBottom)) return tval;

  // If the values are true and false (and vice-versa), then its equal to the
  // value of the condition itself (or inverted).
  if (tval->hasConstVal(true) && fval->hasConstVal(false)) {
    if (cond->isA(TBool)) return cond;
    if (cond->isA(TInt)) return gen(env, NeqInt, cond, cns(env, 0));
  }
  if (tval->hasConstVal(false) && fval->hasConstVal(true)) {
    if (cond->isA(TBool)) return gen(env, XorBool, cond, cns(env, true));
    if (cond->isA(TInt)) return gen(env, EqInt, cond, cns(env, 0));
  }

  // Select C, 0, 1 or Select C, 1, 0 is the same as a bool -> int conversion.
  if (tval->hasConstVal(1) && fval->hasConstVal(0) && cond->isA(TBool)) {
    return gen(env, ConvBoolToInt, cond);
  }
  if (tval->hasConstVal(0) && fval->hasConstVal(1) && cond->isA(TBool)) {
    return gen(env, ConvBoolToInt, gen(env, XorBool, cond, cns(env, true)));
  }

  return nullptr;
}

SSATmp* simplifyAssertNonNull(State& /*env*/, const IRInstruction* inst) {
  if (!inst->src(0)->type().maybe(TNullptr)) {
    return inst->src(0);
  }
  return nullptr;
}

SSATmp* simplifyCheckVecBounds(State& env, const IRInstruction* inst) {
  auto const array = inst->src(0);
  auto const idx   = inst->src(1);

  auto const idxVal = idx->hasConstVal()
    ? Optional<int64_t>(idx->intVal())
    : std::nullopt;
  switch (vecBoundsStaticCheck(array->type(), idxVal)) {
  case VecBounds::In:       return gen(env, Nop);
  case VecBounds::Out:      return gen(env, Jmp, inst->taken());
  case VecBounds::Unknown:  break;
  }

  return mergeBranchDests(env, inst);
}

SSATmp* simplifyReserveVecNewElem(State& env, const IRInstruction* inst) {
  auto const base = inst->src(0);

  if (base->type() <= TPersistentVec) {
    return cns(env, TBottom);
  }
  return nullptr;
}

namespace {

SSATmp* arrGetKImpl(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (!inst->src(2)->hasConstVal(TInt)) return nullptr;
  auto const pos = inst->src(2)->intVal();
  assertx(validPos(ssize_t(pos)));
  if (!arr->hasConstVal()) return nullptr;

  auto const mixed = VanillaDict::as(arr->arrLikeVal());
  auto const tv = mixed->getArrayElmPtr(pos);

  // The array doesn't contain a valid element at that offset. Since this
  // instruction should be guarded by a check, this (should be) unreachable.
  if (!tv) {
    return cns(env, TBottom);
  }

  assertx(tvIsPlausible(*tv));
  return cns(env, *tv);
}

template <typename I, typename S, typename F>
SSATmp* hackArrQueryImpl(State& /*env*/, const IRInstruction* inst, I getInt,
                         S getStr, F finish) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);

  if (!arr->hasConstVal()) return nullptr;
  if (!key->hasConstVal(TInt) && !key->hasConstVal(TStr)) return nullptr;

  auto const value = key->hasConstVal(TInt)
    ? getInt(arr, key->intVal())
    : getStr(arr, key->strVal());
  return finish(value);
}

template <typename I, typename S>
SSATmp* hackArrGetImpl(State& env, const IRInstruction* inst,
                       I getInt, S getStr) {
  return hackArrQueryImpl(
    env, inst,
    getInt, getStr,
    [&] (TypedValue tv) {
      if (tv.is_init()) return cns(env, tv);
      gen(env, ThrowOutOfBounds, inst->taken(), inst->src(0), inst->src(1));
      return cns(env, TBottom);
    }
  );
}

template <typename I, typename S>
SSATmp* hackArrGetQuietImpl(State& env, const IRInstruction* inst,
                            I getInt, S getStr) {
  return hackArrQueryImpl(
    env, inst,
    getInt, getStr,
    [&] (TypedValue tv) {
      return tv.is_init() ? cns(env, tv) : cns(env, TInitNull);
    }
  );
}

template <typename I, typename S>
SSATmp* hackArrIssetImpl(State& env, const IRInstruction* inst,
                         I getInt, S getStr) {
  return hackArrQueryImpl(
    env, inst,
    getInt, getStr,
    [&] (TypedValue tv) { return cns(env, !tvIsNull(tv)); }
  );
}

template <typename I, typename S>
SSATmp* hackArrIdxImpl(State& env, const IRInstruction* inst,
                       I getInt, S getStr) {
  return hackArrQueryImpl(
    env, inst,
    getInt, getStr,
    [&] (TypedValue tv) { return tv.is_init() ? cns(env, tv) : inst->src(2); }
  );
}

template <typename I, typename S>
SSATmp* hackArrAKExistsImpl(State& env, const IRInstruction* inst,
                            I getInt, S getStr) {
  return hackArrQueryImpl(
    env, inst,
    getInt, getStr,
    [&] (TypedValue tv) { return cns(env, tv.is_init()); }
  );
}

}

#define X(Name, Action)                                               \
SSATmp* simplify##Name(State& env, const IRInstruction* inst) {       \
  return hackArr##Action##Impl(                                       \
    env, inst,                                                        \
    [](SSATmp* a, int64_t k) {                                        \
      return VanillaDict::NvGetInt(a->arrLikeVal(), k);                \
    },                                                                \
    [](SSATmp* a, const StringData* k) {                              \
      return VanillaDict::NvGetStr(a->arrLikeVal(), k);                \
    }                                                                 \
  );                                                                  \
}

X(DictGet, Get)
X(DictGetQuiet, GetQuiet)
X(DictIsset, Isset)
X(DictIdx, Idx)
X(AKExistsDict, AKExists)

#undef X

#define X(Name, Action)                                               \
SSATmp* simplify##Name(State& env, const IRInstruction* inst) {       \
  return hackArr##Action##Impl(                                       \
    env, inst,                                                        \
    [](SSATmp* a, int64_t k) {                                        \
      return VanillaKeyset::NvGetInt(a->keysetVal(), k);              \
    },                                                                \
    [](SSATmp* a, const StringData* k) {                              \
      return VanillaKeyset::NvGetStr(a->keysetVal(), k);              \
    }                                                                 \
  );                                                                  \
}

X(KeysetGet, Get)
X(KeysetGetQuiet, GetQuiet)
X(KeysetIsset, Isset)
X(KeysetIdx, Idx)
X(AKExistsKeyset, AKExists)

#undef X

SSATmp* simplifyDictGetK(State& env, const IRInstruction* inst) {
  return arrGetKImpl(env, inst);
}

SSATmp* simplifyKeysetGetK(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (!inst->src(2)->hasConstVal(TInt)) return nullptr;
  auto const pos = inst->src(2)->intVal();

  assertx(validPos(ssize_t(pos)));
  if (!arr->hasConstVal()) return nullptr;

  auto const set = VanillaKeyset::asSet(arr->keysetVal());
  auto const tv = set->tvOfPos(pos);

  // The array doesn't contain a valid element at that offset. Since this
  // instruction should be guarded by a check, this (should be) unreachable.
  if (!tv) {
    return cns(env, TBottom);
  }

  assertx(tvIsPlausible(*tv));
  assertx(isStringType(tv->m_type) || isIntType(tv->m_type));
  return cns(env, *tv);
}

SSATmp* simplifyGetDictPtrIter(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const idx = inst->src(1);
  if (!arr->hasConstVal(TArrLike)) return nullptr;
  if (!idx->hasConstVal(TInt)) return nullptr;
  auto const ad  = VanillaDict::as(arr->arrLikeVal());
  auto const elm = ad->data() + idx->intVal();
  return cns(env, Type::cns(elm, outputType(inst)));
}

SSATmp* simplifyGetKeysetPtrIter(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const idx = inst->src(1);
  if (!arr->hasConstVal(TArrLike)) return nullptr;
  if (!idx->hasConstVal(TInt)) return nullptr;
  auto const ad  = VanillaKeyset::asSet(arr->arrLikeVal());
  auto const elm = ad->data() + idx->intVal();
  return cns(env, Type::cns(elm, outputType(inst)));
}

SSATmp* simplifyGetVecPtrIter(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const idx = inst->src(1);
  if (!arr->hasConstVal(TArrLike)) return nullptr;
  if (!idx->hasConstVal(TInt)) return nullptr;
  auto const entries = VanillaVec::entries(arr->arrLikeVal());
  auto const elm = entries + idx->intVal();
  return cns(env, Type::cns(elm, outputType(inst)));
}

SSATmp* simplifyCheckPtrIterTombstone(State& env, const IRInstruction* inst) {
  if (inst->src(0)->isA(TStaticArrLike)) return gen(env, Nop);
  return mergeBranchDests(env, inst);
}

SSATmp* simplifyCheckDictKeys(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (!src->hasConstVal()) return mergeBranchDests(env, inst);
  auto const arr = src->arrLikeVal();

  auto match = true;
  IterateKV(arr, [&](TypedValue key, TypedValue val){
    match &= Type::cns(key) <= inst->typeParam();
    return !match;
  });
  return match ? gen(env, Nop) : gen(env, Jmp, inst->taken());
}

SSATmp* simplifyCheckMissingKeyInArrLike(State& env,
                                         const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  if (!arr->hasConstVal(TArrLike) || !key->hasConstVal(TStaticStr)) {
    return mergeBranchDests(env, inst);
  }
  auto const ad = arr->arrLikeVal();
  auto const sd = key->strVal();
  if (ad->hasStrKeyTable() && !ad->missingKeySideTable().mayContain(sd)) {
    return gen(env, Nop);
  }
  return gen(env, Jmp, inst->taken());
}

SSATmp* simplifyCheckOffsetImpl(State& env, const IRInstruction* inst) {
  assertx(inst->is(CheckDictOffset, CheckKeysetOffset));
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const& extra = inst->extra<IndexData>();
  assertx(validPos(ssize_t(extra->index)));

  auto const keyType =
    arrLikePosType(arr->type(), Type::cns(extra->index), true, inst->ctx());
  assertx(keyType <= (TInt | TStr));
  assertx(key->isA(TInt | TStr));
  if (keyType <= TBottom) return gen(env, Jmp, inst->taken());
  if (keyType <= TInt) {
    if (!key->type().maybe(TInt)) return gen(env, Jmp, inst->taken());
  }
  if (keyType <= TStr) {
    if (!key->type().maybe(TStr)) return gen(env, Jmp, inst->taken());
  }

  return mergeBranchDests(env, inst);
}

SSATmp* simplifyCheckDictOffset(State& env, const IRInstruction* inst) {
  return simplifyCheckOffsetImpl(env, inst);
}

SSATmp* simplifyCheckKeysetOffset(State& env, const IRInstruction* inst) {
  return simplifyCheckOffsetImpl(env, inst);
}

SSATmp* simplifyCheckArrayCOW(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (arr->isA(TPersistentArrLike)) {
    gen(env, Jmp, inst->taken());
    return cns(env, TBottom);
  }
  return nullptr;
}

SSATmp* simplifyCount(State& env, const IRInstruction* inst) {
  auto const val = inst->src(0);
  auto const ty = val->type();

  if (ty <= TNull) return cns(env, 0);

  auto const oneTy = TBool | TInt | TDbl | TStr | TRes;
  if (ty <= oneTy) return cns(env, 1);

  if (ty <= TVec) return gen(env, CountVec, val);
  if (ty <= TDict) return gen(env, CountDict, val);
  if (ty <= TKeyset) return gen(env, CountKeyset, val);

  if (ty < TObj) {
    auto const cls = ty.clsSpec().cls();
    if (cls != nullptr && cls->isCollectionClass()) {
      return gen(env, CountCollection, val);
    }
  }
  return nullptr;
}

SSATmp* simplifyCountHelper(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal(TArrLike)) return cns(env, src->arrLikeVal()->size());

  auto const layout = src->type().arrSpec().layout();
  if (auto const num = layout.numElements()) return cns(env, *num);

  auto const at = src->type().arrSpec().type();
  if (!at) return nullptr;
  using A = RepoAuthType::Array;
  switch (at->tag()) {
  case A::Tag::Tuple:
    if (at->emptiness() == A::Empty::No) return cns(env, at->size());
    break;
  case A::Tag::Packed:
    break;
  }
  return nullptr;
}

#define X(Name)                                                 \
SSATmp* simplify##Name(State& env, const IRInstruction* inst) { \
  return simplifyCountHelper(env, inst);                        \
}

X(CountVec)
X(CountDict)
X(CountKeyset)

#undef X

SSATmp* simplifyDictIterEnd(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);

  if (arr->hasConstVal()) {
    auto const dict = VanillaDict::as(arr->type().arrLikeVal());
    return cns(env, dict->iterLimit());
  }

  // Static dicts do not have tombstones.
  if (arr->isA(TStaticDict)) return gen(env, CountDict, arr);

  return nullptr;
}

SSATmp* simplifyKeysetIterEnd(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);

  if (arr->hasConstVal()) {
    auto const keyset = VanillaKeyset::asSet(arr->type().arrLikeVal());
    return cns(env, keyset->iterLimit());
  }

  // Static keysets do not have tombstones.
  if (arr->isA(TStaticKeyset)) return gen(env, CountKeyset, arr);

  return nullptr;
}

// Simplify generic bespoke getters, either based on the DataType (often we
// can make simplifications for all varrays and vecs) or specific layout.

SSATmp* simplifyBespokeGet(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  assertx(key->type().subtypeOfAny(TInt, TStr));

  if (arr->hasConstVal()) {
    auto const ad = arr->arrLikeVal();
    if (ad->empty()) return cns(env, TUninit);
    if (key->hasConstVal()) {
      auto const tv = key->isA(TInt) ? ad->get(key->intVal())
                                     : ad->get(key->strVal());
      return cns(env, tv);
    }
  }

  if (arr->isA(TVec)) {
    if (key->isA(TStr)) return cns(env, TUninit);
    if (arr->type().arrSpec().monotype() &&
        inst->extra<BespokeGet>()->state == BespokeGetData::KeyState::Present) {
      return gen(env, LdMonotypeVecElem, arr, key);
    }
  }

  return nullptr;
}

SSATmp* simplifyBespokeGetThrow(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  assertx(key->type().subtypeOfAny(TInt, TStr));

  if (arr->hasConstVal()) {
    auto const ad = arr->arrLikeVal();
    if (ad->empty()) {
      gen(env, ThrowOutOfBounds, inst->taken(), arr, key);
      return cns(env, TBottom);
    }
    if (key->hasConstVal()) {
      auto const tv = key->isA(TInt) ? ad->get(key->intVal())
                                     : ad->get(key->strVal());
      if (tv.is_init()) {
        return cns(env, tv);
      } else {
        gen(env, ThrowOutOfBounds, inst->taken(), arr, key);
        return cns(env, TBottom);
      }
    }
  }

  auto const arrSpec = arr->type().arrSpec();
  if ((arrSpec.is_struct() || arrSpec.is_type_structure()) && key->isA(TInt)) {
    gen(env, ThrowOutOfBounds, inst->taken(), arr, key);
    return cns(env, TBottom);
  }

  return nullptr;
}

SSATmp* simplifyStructDictSlot(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  assertx(arr->type().arrSpec().is_struct());

  auto const& layout = arr->type().arrSpec().layout();
  if (key->hasConstVal(TStr) && layout.bespokeLayout()->isConcrete()) {
    auto const& slayout = bespoke::StructLayout::As(layout.bespokeLayout());
    auto const slot = slayout->keySlot(key->strVal());
    if (slot == kInvalidSlot) {
      gen(env, Jmp, inst->taken());
      return cns(env, TBottom);
    }
    return cns(env, slot);
  }

  return nullptr;
}

SSATmp* simplifyBespokeUnset(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);

  // If the key is definitely missing, we can skip the remove.
  auto const missing = [&]{
    if (arr->isA(TVec) && key->isA(TStr)) return true;
    auto const type = arrLikeElemType(arr->type(), key->type(), inst->ctx());
    return type.first == TBottom;
  }();
  if (missing) return arr;

  if (arr->type().arrSpec().is_struct() && key->hasConstVal(TStr)) {
    return gen(env, StructDictUnset, arr, key);
  }

  return nullptr;
}

SSATmp* simplifyBespokeIterFirstPos(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);

  if (arr->hasConstVal()) {
    auto const pos = arr->type().arrLikeVal()->iter_begin();
    return cns(env, pos);
  }

  if (arr->isA(TVec)) {
    return cns(env, 0);
  }

  return nullptr;
}

SSATmp* simplifyBespokeIterLastPos(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);

  if (arr->hasConstVal()) {
    auto const pos = arr->type().arrLikeVal()->iter_last();
    return cns(env, pos);
  }

  if (arr->isA(TVec)) {
    auto const size = gen(env, CountVec, arr);
    return gen(env, SubInt, size, cns(env, 1));
  }

  return nullptr;
}

SSATmp* simplifyBespokeIterEnd(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const spec = arr->type().arrSpec();

  if (arr->hasConstVal()) {
    auto const pos = arr->type().arrLikeVal()->iter_end();
    return cns(env, pos);
  }

  if (arr->isA(TVec)) {
    return gen(env, CountVec, arr);
  }

  if (arr->isA(TDict) && spec.monotype()) {
    auto const size = gen(env, CountDict, arr);
    auto const tombstones = gen(env, LdMonotypeDictTombstones, arr);
    return gen(env, AddInt, size, tombstones);
  }

  if (arr->isA(TDict) && spec.is_struct()) {
    return gen(env, CountDict, arr);
  }

  // Static arrays do not have tombstones.
  if (arr->isA(TStaticArrLike)) return gen(env, Count, arr);

  return nullptr;
}

SSATmp* simplifyBespokeIterGetKey(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const pos = inst->src(1);

  if (arr->hasConstVal() && pos->hasConstVal()) {
    auto const idx = pos->intVal();
    auto const ad = arr->type().arrLikeVal();
    if (idx < 0 || idx >= ad->size()) return cns(env, TBottom);

    auto const key = arr->type().arrLikeVal()->nvGetKey(pos->intVal());
    return cns(env, key);
  }

  if (arr->isA(TVec)) return pos;

  if (arr->isA(TDict) && arr->type().arrSpec().monotype()) {
    return gen(env, LdMonotypeDictKey, arr, pos);
  }

  if (arr->isA(TDict) && arr->type().arrSpec().is_struct()) {
    auto const slot = gen(env, StructDictSlotInPos, arr, pos);
    return gen(env, LdStructDictKey, arr, slot);
  }

  return nullptr;
}

SSATmp* simplifyBespokeIterGetVal(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const pos = inst->src(1);

  if (arr->hasConstVal() && pos->hasConstVal()) {
    auto const idx = pos->intVal();
    auto const ad = arr->type().arrLikeVal();
    if (idx < 0 || idx >= ad->size()) return cns(env, TBottom);

    auto const val = arr->type().arrLikeVal()->nvGetVal(pos->intVal());
    return cns(env, val);
  }

  if (arr->isA(TVec)) {
    auto const data = BespokeGetData { BespokeGetData::KeyState::Present };
    return gen(env, BespokeGet, data, arr, pos);
  }

  if (arr->isA(TDict) && arr->type().arrSpec().monotype()) {
    return gen(env, LdMonotypeDictVal, arr, pos);
  }

  if (arr->isA(TDict) && arr->type().arrSpec().is_struct()) {
    auto const slot = gen(env, StructDictSlotInPos, arr, pos);
    return gen(env, LdStructDictVal, arr, slot);
  }

  return nullptr;
}

SSATmp* simplifyIterGetKeyArr(State& env, const IRInstruction* inst) {
  auto const base = inst->src(0);
  auto const pos = inst->src(1);
  auto const flags = inst->extra<IterData>()->args.flags;
  auto const baseConst = has_flag(flags, IterArgs::Flags::BaseConst);

  if (!allowBespokeArrayLikes() || base->type().arrSpec().vanilla()) {
    if (base->isA(TVec)) return pos;
    if (base->isA(TDict)) {
      auto const elm = baseConst
        ? gen(env, IntAsPtrToElem, pos)
        : gen(env, GetDictPtrIter, base, pos);
      return gen(env, LdPtrIterKey, TInt | TStr, base, elm);
    }
    if (base->isA(TKeyset)) {
      auto const elm = baseConst
        ? gen(env, IntAsPtrToElem, pos)
        : gen(env, GetKeysetPtrIter, base, pos);
      return gen(env, LdPtrIterVal, TInt | TStr, base, elm);
    }
  }

  if (base->type().arrSpec().bespoke()) {
    return gen(env, BespokeIterGetKey, base, pos);
  }

  return nullptr;
}

SSATmp* simplifyIterGetValArr(State& env, const IRInstruction* inst) {
  auto const base = inst->src(0);
  auto const pos = inst->src(1);
  auto const flags = inst->extra<IterData>()->args.flags;
  auto const baseConst = has_flag(flags, IterArgs::Flags::BaseConst);
  auto const withKeys = has_flag(flags, IterArgs::Flags::WithKeys);

  if (!allowBespokeArrayLikes() || base->type().arrSpec().vanilla()) {
    if (base->isA(TVec)) {
      if (baseConst && !withKeys) {
        auto const elm = gen(env, IntAsPtrToElem, pos);
        return gen(env, LdPtrIterVal, TInitCell, base, elm);
      }
      return gen(env, LdVecElem, base, pos);
    }
    if (base->isA(TDict)) {
      auto const elm = baseConst
        ? gen(env, IntAsPtrToElem, pos)
        : gen(env, GetDictPtrIter, base, pos);
      return gen(env, LdPtrIterVal, TInitCell, base, elm);
    }
    if (base->isA(TKeyset)) {
      auto const elm = baseConst
        ? gen(env, IntAsPtrToElem, pos)
        : gen(env, GetKeysetPtrIter, base, pos);
      return gen(env, LdPtrIterVal, TInt | TStr, base, elm);
    }
  }

  if (base->type().arrSpec().bespoke()) {
    return gen(env, BespokeIterGetVal, base, pos);
  }

  return nullptr;
}

// Simplify layout-specific bespoke helpers.

SSATmp* simplifyLdMonotypeDictTombstones(
    State& env, const IRInstruction* inst) {
  auto const type = inst->src(0)->type();

  if (type.hasConstVal()) {
    auto const arr = type.arrLikeVal();
    return cns(env, arr->iter_end() - arr->size());
  }

  return nullptr;
}

SSATmp* simplifyLdMonotypeDictKey(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const pos = inst->src(1);
  if (arr->hasConstVal() && pos->hasConstVal()) {
    auto const tv = arr->arrLikeVal()->nvGetKey(pos->intVal());
    return tv.is_init() ? cns(env, tv) : nullptr;
  }
  return nullptr;
}

SSATmp* simplifyLdMonotypeDictVal(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const pos = inst->src(1);
  if (arr->hasConstVal() && pos->hasConstVal()) {
    auto const tv = arr->arrLikeVal()->nvGetVal(pos->intVal());
    return tv.is_init() ? cns(env, tv) : nullptr;
  }
  return nullptr;
}

SSATmp* simplifyLdMonotypeVecElem(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  if (arr->hasConstVal() && key->hasConstVal()) {
    auto const tv = arr->arrLikeVal()->get(key->intVal());
    return tv.is_init() ? cns(env, tv) : nullptr;
  }
  return nullptr;
}

SSATmp* simplifyLdStructDictKey(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (arr->hasConstVal() && arr->arrLikeVal()->size() == 1) {
    return cns(env, arr->arrLikeVal()->nvGetKey(0));
  }
  return nullptr;
}

SSATmp* simplifyLdStructDictVal(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (arr->hasConstVal() && arr->arrLikeVal()->size() == 1) {
    return cns(env, arr->arrLikeVal()->nvGetVal(0));
  }
  return nullptr;
}

SSATmp* simplifyLdTypeStructureVal(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  if (arr->hasConstVal() && key->hasConstVal(TStr)) {
    auto const tv = arr->arrLikeVal()->get(key->strVal());
    if (!tv.is_init()) {
      gen(env, Jmp, inst->taken());
      return cns(env, TBottom);
    }
    return cns(env, tv);
  }

  if (key->hasConstVal(TStr)) {
    auto const dt = bespoke::TypeStructure::getFieldPair(key->strVal()).first;

    if (dt == KindOfUninit) {
      gen(env, Jmp, inst->taken());
      return cns(env, TBottom);
    }

    auto const val =
      gen(env, LdTypeStructureValCns, KeyedData{ key->strVal() }, arr);

    if (dt == KindOfBoolean) {
      // match current TS array behaviour - treat false as not present
      gen(env, JmpZero, inst->taken(), val);
      return cns(env, true);
    } else if (dt == KindOfInt64) {
      return val;
    } else if (isArrayLikeType(dt) || isStringType(dt)) {
      return gen(env, CheckNonNull, inst->taken(), val);
    }

    not_reached();
  }

  return nullptr;
}

SSATmp* simplifyLdClsName(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal(TCls) ? cns(env, src->clsVal()->name()) : nullptr;
}

SSATmp* simplifyLdLazyClsName(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal(TLazyCls) ?
    cns(env, src->lclsVal().name()) : nullptr;
}

SSATmp* simplifyLdEnumClassLabelName(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal(TEnumClassLabel) ? cns(env, src->eclVal()) : nullptr;
}

SSATmp* simplifyLookupCls(State& env, const IRInstruction* inst) {
  auto const str = inst->src(0);
  if (str->inst()->is(LdClsName)) {
    return str->inst()->src(0);
  }
  if (str->hasConstVal()) {
    auto const sval = str->strVal();
    auto const result = Class::lookupKnown(sval, nullptr);
    if (result) return cns(env, result);
  }
  return nullptr;
}

SSATmp* simplifyLookupSPropSlot(State& env, const IRInstruction* inst) {
  auto const cls = inst->src(0);
  auto const name = inst->src(1);
  if (!cls->hasConstVal(TCls) || !name->hasConstVal(TStr)) return nullptr;
  return cns(env, cls->clsVal()->lookupSProp(name->strVal()));
}

SSATmp* simplifyLdStrLen(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  return src->hasConstVal(TStr) ? cns(env, src->strVal()->size()) : nullptr;
}

SSATmp* simplifyLdVecElem(State& env, const IRInstruction* inst) {
  auto const src0 = inst->src(0);
  auto const src1 = inst->src(1);
  if (src0->hasConstVal() && src1->hasConstVal(TInt)) {
    auto const arr = src0->arrLikeVal();
    auto const idx = src1->intVal();
    assertx(arr->isVanillaVec());
    auto const tv = VanillaVec::NvGetInt(arr, idx);
    return tv.is_init() ? cns(env, tv) : nullptr;
  }
  return nullptr;
}

SSATmp* simplifyPseudoRandomInt(State& env, const IRInstruction* inst) {
  auto const src0 = inst->src(0);
  auto const src1 = inst->src(1);
  if (!src0->hasConstVal(TInt) || !src1->hasConstVal(TInt)) return nullptr;
  if (src0->intVal() != src1->intVal()) return nullptr;
  return cns(env, src0->intVal());
}

template <class F>
SSATmp* simplifyByClass(State& /*env*/, const SSATmp* src, F f) {
  if (!src->isA(TObj)) return nullptr;
  if (auto const spec = src->type().clsSpec()) {
    return f(spec.cls(), spec.exact());
  }
  return nullptr;
}

SSATmp* simplifyCallBuiltin(State& env, const IRInstruction* inst) {
  if (inst->numSrcs() != 3) return nullptr;

  auto const thiz = inst->src(2);
  return simplifyByClass(
    env, thiz,
    [&](const Class* cls, bool) -> SSATmp* {
      auto const callee = inst->extra<CallBuiltin>()->callee;
      if (cls->isCollectionClass()) {
        auto const methName = callee->name();
        if (methName == s_isEmpty.get()) {
          FTRACE(3, "simplifying collection: {}\n", callee->name()->data());
          return gen(env, ColIsEmpty, thiz);
        }
        if (methName == s_count.get()) {
          FTRACE(3, "simplifying collection: {}\n", callee->name()->data());
          return gen(env, CountCollection, thiz);
        }
        return nullptr;
      }

      if (cls->classof(c_Awaitable::classof())) {
        auto const genState = [&] (Opcode op, int64_t whstate) -> SSATmp* {
          // these methods all spring from the base class
          assertx(callee->cls()->name()->tsame(s_Awaitable.get()));
          auto const state = gen(env, LdWHState, thiz);
          return gen(env, op, state, cns(env, whstate));
        };
        auto const methName = callee->name();
        if (methName == s_isFinished.get()) {
          return genState(LteInt, int64_t{c_Awaitable::STATE_FAILED});
        }
        if (methName == s_isSucceeded.get()) {
          return genState(EqInt, int64_t{c_Awaitable::STATE_SUCCEEDED});
        }
        if (methName == s_isFailed.get()) {
          return genState(EqInt, int64_t{c_Awaitable::STATE_FAILED});
        }
      }

      return nullptr;
    });
}

SSATmp* simplifyIsWaitHandle(State& env, const IRInstruction* inst) {
  return simplifyByClass(
    env, inst->src(0),
    [&](const Class* cls, bool) -> SSATmp* {
      if (cls->classof(c_Awaitable::classof())) return cns(env, true);
      if (!isInterface(cls) &&
          !c_Awaitable::classof()->classof(cls)) {
        return cns(env, false);
      }
      return nullptr;
    });
}

SSATmp* simplifyLdWHState(State& env, const IRInstruction* inst) {
  auto const wh = canonical(inst->src(0));
  if (wh->inst()->is(CreateSSWH)) {
    return cns(env, int64_t{c_Awaitable::STATE_SUCCEEDED});
  }
  if (wh->inst()->is(CreateFSWH)) {
    return cns(env, int64_t{c_Awaitable::STATE_FAILED});
  }
  return nullptr;
}

SSATmp* simplifyLdWHResult(State& env, const IRInstruction* inst) {
  auto const wh = canonical(inst->src(0));
  if (wh->inst()->is(CreateSSWH) || wh->inst()->is(CreateFSWH)) {
    return wh->inst()->src(0);
  }
  return nullptr;
}

SSATmp* simplifyLdWHNotDone(State& env, const IRInstruction* inst) {
  return simplifyByClass(
    env, inst->src(0),
    [&](const Class* cls, bool) -> SSATmp* {
      if (cls->classof(c_StaticWaitHandle::classof())) return cns(env, 0);
      return nullptr;
    });
}

SSATmp* simplifyIsCol(State& env, const IRInstruction* inst) {
  return simplifyByClass(
    env, inst->src(0),
    [&](const Class* cls, bool) -> SSATmp* {
      if (cls->isCollectionClass()) return cns(env, true);
      if (!isInterface(cls)) return cns(env, false);
      return nullptr;
    });
}

SSATmp* simplifyIsLegacyArrLike(State& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  if (arr->hasConstVal()) {
    return cns(env, arr->arrLikeVal()->isLegacyArray());
  }
  return nullptr;
}

SSATmp* simplifyHasToString(State& env, const IRInstruction* inst) {
  return simplifyByClass(
    env, inst->src(0),
    [&](const Class* cls, bool exact) -> SSATmp* {
      if (cls->getToString() != nullptr) return cns(env, true);
      if (exact) return cns(env, false);
      return nullptr;
    });
}

SSATmp* simplifyFuncHasReifiedGenerics(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal()) {
    return cns(env, src->funcVal()->hasReifiedGenerics());
  }
  return nullptr;
}

SSATmp* simplifyClassHasReifiedGenerics(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  auto const clsSpec = src->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.exactCls();
  if (cls) {
    return cns(env, cls->hasReifiedGenerics());
  }
  return nullptr;
}

SSATmp* simplifyHasReifiedParent(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  auto const clsSpec = src->type().clsSpec();
  if (!clsSpec) return nullptr;
  auto const cls = clsSpec.exactCls();
  if (cls) {
    return cns(env, cls->hasReifiedParent());
  }
  return nullptr;
}

SSATmp* simplifyChrInt(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal(TInt)) {
    auto const str = makeStaticString(char(src->intVal() & 255));
    return cns(env, str);
  }
  return nullptr;
}

SSATmp* simplifyOrdStr(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (src->hasConstVal(TStr)) {
    // a static string is passed in, resolve with a constant.
    unsigned char first = src->strVal()->data()[0];
    return cns(env, int64_t{first});
  }
  if (src->inst()->is(ChrInt)) {
    return gen(env, AndInt, src->inst()->src(0), cns(env, 255));
  }
  return nullptr;
}

SSATmp* simplifyLdSwitchDest(State& env, const IRInstruction* inst) {
  auto const index = inst->src(0);
  if (!index->hasConstVal(TInt)) return nullptr;

  auto const indexVal = index->intVal();
  auto const extra = inst->extra<LdSwitchDest>();

  if (indexVal < 0 || indexVal >= extra->cases) {
    // Instruction is unreachable.
    return gen(env, Unreachable, ASSERT_REASON);
  }

  assertx(!extra->targets[indexVal].anyFuncEntry());
  auto const bindData = LdBindAddrData {
    extra->targets[indexVal],
    extra->spOffBCFromStackBase,
  };
  return gen(env, LdBindAddr, bindData);
}

SSATmp* simplifyLdSSwitchDest(State& env, const IRInstruction* inst) {
  auto const index = inst->src(0);
  if (!index->hasConstVal(TStr)) return nullptr;

  auto const indexVal = index->strVal();
  auto const extra = inst->extra<LdSSwitchDest>();

  auto const target = [&]{
    for (int64_t i = 0; i < extra->numCases; ++i) {
      if (!indexVal->same(extra->cases[i].str)) continue;
      return extra->cases[i].dest;
    }
    return extra->defaultSk;
  }();

  assertx(!target.anyFuncEntry());
  auto const bindData = LdBindAddrData { target, extra->bcSPOff };
  return gen(env, LdBindAddr, bindData);
}

SSATmp* simplifyJmpExit(State& env, const IRInstruction* inst) {
  auto const bindAddr = inst->src(0)->inst();
  if (!bindAddr->is(LdBindAddr)) return nullptr;

  auto const bindExtra = bindAddr->extra<LdBindAddr>();
  auto const jmpExtra = inst->extra<JmpExit>();

  assertx(!bindExtra->sk.anyFuncEntry());
  auto const rbjData = ReqBindJmpData {
    bindExtra->sk,
    bindExtra->bcSPOff,
    jmpExtra->offset,
    false
  };
  return gen(env, ReqBindJmp, rbjData, inst->src(1), inst->src(2));
}

SSATmp* simplifyCheckRange(State& env, const IRInstruction* inst) {
  auto const val = inst->src(0);
  auto const limit = inst->src(1);

  // CheckRange returns (0 <= val < limit).
  if (val && val->hasConstVal(TInt)) {
    if (val->intVal() < 0) return cns(env, false);

    if (limit && limit->hasConstVal(TInt)) {
      return cns(env, val->intVal() < limit->intVal());
    }
  }

  if (limit && limit->hasConstVal(TInt) && limit->intVal() <= 0) {
    return cns(env, false);
  }

  return nullptr;
}

SSATmp* simplifyGetMemoKeyScalar(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);

  // Note: this all uses the fully generic memo key scheme. If we used a more
  // specific scheme, then the GetMemoKey op wouldn't have been emitted.

  if (src->hasConstVal()) {
    try {
      ThrowAllErrorsSetter taes;
      auto const key =
        HHVM_FN(serialize_memoize_param)(*src->variantVal().asTypedValue());
      SCOPE_EXIT { tvDecRefGen(key); };
      assertx(tvIsPlausible(key));
      if (tvIsString(&key)) {
        return cns(env, makeStaticString(key.m_data.pstr));
      } else {
        assertx(key.m_type == KindOfInt64);
        return cns(env, key.m_data.num);
      }
    } catch (...) {
    }
  }

  if (src->isA(TInt)) return src;
  if (src->isA(TBool)) {
    return gen(
      env,
      Select,
      src,
      cns(env, s_trueMemoKey.get()),
      cns(env, s_falseMemoKey.get())
    );
  }
  if (src->isA(TNull)) return cns(env, s_nullMemoKey.get());

  return nullptr;
}

SSATmp* simplifyGetMemoKey(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);

  if (auto ret = simplifyGetMemoKeyScalar(env, inst)) return ret;
  if (src->isA(TUncounted|TStr) &&
      !Cfg::Eval::ClassMemoNotices) {
    return gen(env, GetMemoKeyScalar, src);
  }
  return nullptr;
}

SSATmp* simplifyStrictlyIntegerConv(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  if (!src->hasConstVal()) return nullptr;
  int64_t n;
  if (src->strVal()->isStrictlyInteger(n)) return cns(env, n);
  gen(env, IncRef, src);
  return src;
}

SSATmp* simplifyRaiseErrorOnInvalidIsAsExpressionType(
  State& env,
  const IRInstruction* inst
) {
  auto const ts = inst->src(0);
  if (!ts->hasConstVal(TDict)) {
    return nullptr;
  }
  auto const tsVal = ts->arrLikeVal();
  if (errorOnIsAsExpressionInvalidTypes(ArrNR(tsVal), true)) {
    return nullptr;
  }
  return ts;
}

SSATmp* simplifyCheckClsMethFunc(State& env, const IRInstruction* inst) {
  if (!inst->src(0)->hasConstVal(TFunc)) return nullptr;
  auto const func = inst->src(0)->funcVal();
  if (func->isStaticInPrologue() && !func->isAbstract()) {
    return gen(env, Nop);
  }
  return nullptr;
}

SSATmp* simplifyNewClsMeth(State& env, const IRInstruction* inst) {
  auto const cls = inst->src(0);
  auto const func = inst->src(1);
  if (!cls->hasConstVal() || !func->hasConstVal()) return nullptr;
  auto const clsmeth = ClsMethDataRef::create(
      const_cast<Class*>(cls->clsVal()),
      const_cast<Func*>(func->funcVal()));
  return cns(env, clsmeth);
}

SSATmp* simplifyLdFuncFromRFunc(State& env, const IRInstruction* inst) {
  auto const rfunc = canonical(inst->src(0));
  if (rfunc->inst()->is(NewRFunc)) return rfunc->inst()->src(0);
  return nullptr;
}

SSATmp* simplifyLdGenericsFromRFunc(State& env, const IRInstruction* inst) {
  auto const rfunc = canonical(inst->src(0));
  if (rfunc->inst()->is(NewRFunc)) return rfunc->inst()->src(1);
  return nullptr;
}

SSATmp* simplifyLdClsFromClsMeth(State& env, const IRInstruction* inst) {
  auto const clsmeth = canonical(inst->src(0));
  if (clsmeth->hasConstVal()) return cns(env, clsmeth->clsmethVal()->getCls());
  if (clsmeth->inst()->is(NewClsMeth)) return clsmeth->inst()->src(0);
  return nullptr;
}

SSATmp* simplifyLdFuncFromClsMeth(State& env, const IRInstruction* inst) {
  auto const clsmeth = canonical(inst->src(0));
  if (clsmeth->hasConstVal()) return cns(env, clsmeth->clsmethVal()->getFunc());
  if (clsmeth->inst()->is(NewClsMeth)) return clsmeth->inst()->src(1);
  return nullptr;
}

SSATmp* simplifyLdClsFromRClsMeth(State& env, const IRInstruction* inst) {
  auto const rclsmeth = canonical(inst->src(0));
  if (rclsmeth->inst()->is(NewRClsMeth)) return rclsmeth->inst()->src(0);
  return nullptr;
}

SSATmp* simplifyLdFuncFromRClsMeth(State& env, const IRInstruction* inst) {
  auto const rclsmeth = canonical(inst->src(0));
  if (rclsmeth->inst()->is(NewRClsMeth)) return rclsmeth->inst()->src(1);
  return nullptr;
}

SSATmp* simplifyLdGenericsFromRClsMeth(State& env, const IRInstruction* inst) {
  auto const rclsmeth = canonical(inst->src(0));
  if (rclsmeth->inst()->is(NewRClsMeth)) return rclsmeth->inst()->src(2);
  return nullptr;
}

SSATmp* simplifyStructDictTypeBoundCheck(State& env,
                                         const IRInstruction* inst) {
  auto const val  = inst->src(0);
  auto const arr  = inst->src(1);
  auto const slot = inst->src(2);

  if ((inst->dst() != nullptr && inst->dst()->type() == TBottom) ||
      (inst->next() != nullptr && inst->next()->isUnreachable())) {
    // The check always branches, convert it to a Jmp to taken.
    gen(env, Jmp, inst->taken());
    return cns(env, TBottom);
  }

  auto const& layout = arr->type().arrSpec().layout();
  assertx(layout.is_struct());
  if (!layout.bespokeLayout()->isConcrete()) return nullptr;
  auto const slayout = bespoke::StructLayout::As(layout.bespokeLayout());

  if (slot->hasConstVal()) {
    if (slot->intVal() >= slayout->numFields()) return cns(env, TBottom);
    return gen(
      env,
      CheckType,
      slayout->getTypeBound(slot->intVal()),
      inst->taken(),
      val
    );
  } else {
    auto const type = slayout->getUnionTypeBound();
    if (!type.isKnownDataType()) return nullptr;
    return gen(
      env,
      CheckType,
      type,
      inst->taken(),
      val
    );
  }

  return nullptr;
}

SSATmp* simplifyLdPublicFunc(State& env, const IRInstruction* inst) {
  auto fp = inst->src(0);
  while (fp->inst()->is(DefCalleeFP) && funcFromFP(fp)->isNoInjection()) {
    fp = fp->inst()->marker().fp();
  }

  if (!funcFromFP(fp)->isNoInjection()) return cns(env, funcFromFP(fp));
  if (inst->src(0) != fp) return gen(env, LdPublicFunc, fp);
  return nullptr;
}

//////////////////////////////////////////////////////////////////////

SSATmp* simplifyWork(State& env, const IRInstruction* inst) {
  env.insts.push(inst);

  SCOPE_EXIT {
    assertx(env.insts.top() == inst);
    env.insts.pop();
  };

  auto res = [&] () -> SSATmp* {
    switch (inst->op()) {
#define X(x) case x: return simplify##x(env, inst);
      X(Shl)
      X(Shr)
      X(Lshr)
      X(AbsDbl)
      X(AssertNonNull)
      X(PseudoRandomInt)
      X(CallBuiltin)
      X(Ceil)
      X(CheckRDSInitialized)
      X(MarkRDSInitialized)
      X(MarkRDSAccess)
      X(CheckType)
      X(AssertType)
      X(CheckNonNull)
      X(CheckVecBounds)
      X(ReserveVecNewElem)
      X(ConcatStrStr)
      X(ConcatStr3)
      X(ConcatStr4)
      X(ConcatIntStr)
      X(ConcatStrInt)
      X(ConvBoolToDbl)
      X(ConvBoolToInt)
      X(ConvTVToBool)
      X(ConvTVToDbl)
      X(ConvTVToInt)
      X(ConvTVToStr)
      X(ConvDblToBool)
      X(ConvDblToInt)
      X(ConvDblToStr)
      X(ConvIntToBool)
      X(ConvIntToDbl)
      X(ConvIntToStr)
      X(ConvObjToBool)
      X(ConvStrToBool)
      X(ConvStrToDbl)
      X(ConvStrToInt)
      X(ConvArrLikeToVec)
      X(ConvArrLikeToDict)
      X(ConvArrLikeToKeyset)
      X(DblAsBits)
      X(IntAsPtrToElem)
      X(PtrToElemAsInt)
      X(Count)
      X(CountVec)
      X(CountDict)
      X(CountKeyset)
      X(DictIterEnd)
      X(KeysetIterEnd)
      X(DecRef)
      X(DecRefNZ)
      X(DecReleaseCheck)
      X(DefLabel)
      X(DivDbl)
      X(DivInt)
      X(EqFunc)
      X(ExtendsClass)
      X(InstanceOfBitmask)
      X(NInstanceOfBitmask)
      X(DeserializeLazyProp)
      X(Floor)
      X(IncRef)
      X(InitObjProps)
      X(InitObjMemoSlots)
      X(InstanceOf)
      X(InstanceOfIface)
      X(InstanceOfIfaceVtable)
      X(IsLegacyArrLike)
      X(IsWaitHandle)
      X(IsCol)
      X(HasToString)
      X(FuncHasReifiedGenerics)
      X(ClassHasReifiedGenerics)
      X(HasReifiedParent)
      X(LdCls)
      X(LdClsName)
      X(LdLazyClsName)
      X(LdEnumClassLabelName)
      X(LdWHResult)
      X(LdWHState)
      X(LdWHNotDone)
      X(LookupCls)
      X(LookupSPropSlot)
      X(LdClsMethod)
      X(LdStrLen)
      X(BespokeGet)
      X(BespokeUnset)
      X(BespokeGetThrow)
      X(StructDictSlot)
      X(BespokeIterFirstPos)
      X(BespokeIterLastPos)
      X(BespokeIterEnd)
      X(BespokeIterGetKey)
      X(BespokeIterGetVal)
      X(IterGetKeyArr)
      X(IterGetValArr)
      X(LdMonotypeDictTombstones)
      X(LdMonotypeDictKey)
      X(LdMonotypeDictVal)
      X(LdMonotypeVecElem)
      X(LdVecElem)
      X(LdStructDictKey)
      X(LdStructDictVal)
      X(LdTypeStructureVal)
      X(MethodExists)
      X(LdClosureCls)
      X(LdClosureThis)
      X(LdFuncCls)
      X(LdFuncInOutBits)
      X(LdFuncNumParams)
      X(FuncHasAttr)
      X(ClassHasAttr)
      X(LdFuncRequiredCoeffects)
      X(CallViolatesModuleBoundary)
      X(LookupClsCtxCns)
      X(LdClsCtxCns)
      X(LdObjClass)
      X(LdObjInvoke)
      X(Mov)
      X(Jmp)
      X(JmpZero)
      X(JmpNZero)
      X(Select)
      X(OrInt)
      X(AddInt)
      X(SubInt)
      X(MulInt)
      X(AddDbl)
      X(SubDbl)
      X(MulDbl)
      X(Mod)
      X(AndInt)
      X(XorInt)
      X(XorBool)
      X(MulIntO)
      X(GtBool)
      X(GteBool)
      X(LtBool)
      X(LteBool)
      X(EqBool)
      X(NeqBool)
      X(CmpBool)
      X(GtInt)
      X(GteInt)
      X(LtInt)
      X(LteInt)
      X(EqInt)
      X(NeqInt)
      X(CmpInt)
      X(GtDbl)
      X(GteDbl)
      X(LtDbl)
      X(LteDbl)
      X(EqDbl)
      X(NeqDbl)
      X(CmpDbl)
      X(GtStr)
      X(GteStr)
      X(LtStr)
      X(LteStr)
      X(EqStr)
      X(NeqStr)
      X(SameStr)
      X(NSameStr)
      X(CmpStr)
      X(GtObj)
      X(GteObj)
      X(LtObj)
      X(LteObj)
      X(EqObj)
      X(NeqObj)
      X(SameObj)
      X(NSameObj)
      X(GtArrLike)
      X(GteArrLike)
      X(LtArrLike)
      X(LteArrLike)
      X(EqArrLike)
      X(NeqArrLike)
      X(SameArrLike)
      X(NSameArrLike)
      X(GtRes)
      X(GteRes)
      X(LtRes)
      X(LteRes)
      X(EqRes)
      X(NeqRes)
      X(CmpRes)
      X(EqCls)
      X(EqLazyCls)
      X(EqStrPtr)
      X(EqArrayDataPtr)
      X(DictGet)
      X(DictGetQuiet)
      X(DictGetK)
      X(KeysetGet)
      X(KeysetGetQuiet)
      X(KeysetGetK)
      X(GetDictPtrIter)
      X(GetKeysetPtrIter)
      X(GetVecPtrIter)
      X(CheckPtrIterTombstone)
      X(CheckDictKeys)
      X(CheckDictOffset)
      X(CheckKeysetOffset)
      X(CheckArrayCOW)
      X(CheckMissingKeyInArrLike)
      X(DictIsset)
      X(KeysetIsset)
      X(DictIdx)
      X(AKExistsDict)
      X(KeysetIdx)
      X(AKExistsKeyset)
      X(OrdStr)
      X(ChrInt)
      X(LdSwitchDest)
      X(LdSSwitchDest)
      X(JmpExit)
      X(CheckRange)
      X(GetMemoKey)
      X(GetMemoKeyScalar)
      X(StrictlyIntegerConv)
      X(RaiseErrorOnInvalidIsAsExpressionType)
      X(LdFrameCls)
      X(NewClsMeth)
      X(CheckClsMethFunc)
      X(LdFuncFromRFunc)
      X(LdGenericsFromRFunc)
      X(LdClsFromClsMeth)
      X(LdFuncFromClsMeth)
      X(LdClsFromRClsMeth)
      X(LdFuncFromRClsMeth)
      X(LdGenericsFromRClsMeth)
      X(LdResolvedTypeCns)
      X(LdResolvedTypeCnsNoCheck)
      X(LdResolvedTypeCnsClsName)
      X(CGetPropQ)
      X(StructDictTypeBoundCheck)
      X(LdPublicFunc)
#undef X
      default: break;
    }
    return nullptr;
  }();

  // If the new instruction list is empty, we are either returning a known
  // value (res != nullptr), or leaving the instruction unchanged (res ==
  // nullptr). We should mark the instruction as unreachable if the known value
  // is Bottom. Cases where the new final instruction is a block-ending
  // instruction are handled by consumers.

  if (inst->hasDst() && !inst->naryDst() && env.newInsts.empty()) {
    if (res && res->type() == TBottom) {
      // The instruction has been simplified away, leaving only a bottom
      // constant. Mark it as unreachable.
      gen(env, Unreachable, ASSERT_REASON);
    } else if (!res && outputType(inst) == TBottom && !inst->isBlockEnd()) {
      // The instruction is passing through unchanged, but is known to have a
      // bottom result. Replace it with the proper unreachable annotations.
      res = cns(env, TBottom);
      gen(env, Unreachable, ASSERT_REASON);
    }
  }

  return res;
}

Block* unreachableBlock(IRUnit& unit, BCContext ctx) {
  auto const unreachableBlock = unit.defBlock(1, Block::Hint::Unused);
  makeInstruction(
    [&] (IRInstruction* inst) -> void {
      unreachableBlock->push_back(unit.clone(inst));
    },
    Unreachable,
    ctx,
    ASSERT_REASON
  );
  return unreachableBlock;
}

//////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

SimplifyResult simplify(IRUnit& unit, const IRInstruction* origInst) {
  auto env = State { unit };

  auto const newDst = simplifyWork(env, origInst);

  assertx(validate(env, newDst, origInst));

  return SimplifyResult { std::move(env.newInsts), newDst };
}

void simplifyInPlace(IRUnit& unit, IRInstruction* origInst) {
  assertx(!origInst->isTransient());

  copyProp(origInst);
  constProp(unit, origInst);
  retypeDests(origInst, &unit);
  auto res = simplify(unit, origInst);

  // No simplification occurred; nothing to do.
  if (res.instrs.empty() && !res.dst) {
    // If we have narrowed the result of a block end instruction to a bottom
    // type, its next block must be unreachable.
    if (origInst->isNextEdgeUnreachable() && origInst->isBlockEnd() &&
        !origInst->next()->isUnreachable()) {
      origInst->setNext(unreachableBlock(unit, origInst->bcctx()));
    }
    return;
  }

  FTRACE(1, "simplifying: {}\n", origInst->toString());

  if (origInst->isBlockEnd()) {
    auto const next = origInst->block()->next();

    if (res.instrs.empty() || !res.instrs.back()->isBlockEnd()) {
      // Our block-end instruction was eliminated (most likely a Jmp* converted
      // to a Nop).  Replace it with a Jmp to the next block.
      res.instrs.push_back(unit.gen(Jmp, origInst->bcctx(), next));
    }

    auto const last = res.instrs.back();
    assertx(last->isBlockEnd());

    if (!last->isTerminal() && !last->next()) {
      // We converted the block-end instruction to a different one.  Set its
      // next block appropriately.
      last->setNext(next);
    }
  }

  size_t out_size = 0;
  bool need_mov = res.dst;
  IRInstruction* last = nullptr;

  for (auto const inst : res.instrs) {
    if (inst->is(Nop)) continue;

    ++out_size;
    last = inst;

    if (res.dst && res.dst == inst->dst()) {
      // One of the new instructions produced the new dst.  Since we're going
      // to drop `origInst', just use origInst->dst() instead.
      inst->setDst(origInst->dst());
      inst->dst()->setInstruction(inst);
      need_mov = false;
    }
  }

  auto const block = origInst->block();
  auto pos = ++block->iteratorTo(origInst);

  if (last != nullptr && last->isTerminal()) {
    // Delete remaining instructions in the block if a terminal is created.
    // This can happen, e.g., 'Unreachable' may be created when the block is
    // unreachable.
    while (pos != block->end()) pos = block->erase(pos);
  }

  // If the last instruction is block-ending and produces a Bottom result, its
  // next block should be unreachable.
  if (last != nullptr && last->isNextEdgeUnreachable() && last->isBlockEnd() &&
      !last->next()->isUnreachable()) {
    last->setNext(unreachableBlock(unit, last->bcctx()));
  }

  if (need_mov) {
    /*
     * In `killed_edge_defining' we have the case that an instruction defining
     * a temp on an edge (like CheckType) determined it can never define that
     * tmp.  In this situation we just Nop out the instruction and leave the
     * old tmp dangling.  The reason this is ok is that one of the following
     * two things are happening:
     *
     *    o The old next() block is becoming unreachable.  It's ok not to make
     *      a new definition of this tmp, because the code running simplify is
     *      going to have to track unreachable blocks and avoid looking at
     *      them.  It will also have to remove unreachable blocks when it's
     *      finished to maintain IR invariants (e.g. through DCE::Minimal),
     *      which will mean the uses of the no-longer-defined tmp will go away.
     *
     *    o The old next() block is still reachable (e.g. if we're removing a
     *      CheckType because it had next == taken).  But in this case, the
     *      next() edge must have been a critical edge, and therefore nothing
     *      could have any use of the old destination of the CheckType, or the
     *      program would already not have been in SSA, because it was only
     *      defined in blocks dominated by the next edge.
     */
    auto const killed_edge_defining = res.dst->type() <= TBottom &&
      origInst->isBlockEnd();
    if (killed_edge_defining) {
      origInst->convertToNop();
    } else {
      unit.replace(origInst, Mov, res.dst);
      // Force the existing dst type to match that of `res.dst'.
      origInst->dst()->setType(res.dst->type());
    }
  } else {
    if (out_size == 1) {
      assertx(origInst->dst() == last->dst());
      FTRACE(1, "    {}\n", last->toString());

      // We only have a single instruction, so just become it.
      origInst->become(unit, last);

      // Make sure to reset our dst's inst pointer, if we have one.  (It may
      // have been set to `last'.
      if (origInst->dst()) {
        origInst->dst()->setInstruction(origInst);
      }

      // And we also need to kill `last', to update preds.
      last->convertToNop();
      return;
    }

    origInst->convertToNop();
  }

  FTRACE(1, "    {}\n", origInst->toString());

  for (auto const inst : res.instrs) {
    if (inst->is(Nop)) continue;
    block->insert(pos, inst);
    FTRACE(1, "    {}\n", inst->toString());
  }
}

////////////////////////////////////////////////////////////////////////////////

void simplifyPass(IRUnit& unit) {
  Timer timer{Timer::optimize_simplify, unit.logEntry().get_pointer()};
  PassTracer tracer{&unit, Trace::simplify, "simplify"};

  do {
    auto reachable = boost::dynamic_bitset<>(unit.numBlocks());

    auto const unreachableTypes = [&] (IRInstruction* inst) {
      for (auto const src : inst->srcs()) {
        if (src->type() == TBottom) return true;
      }
      for (auto const dst : inst->dsts()) {
        if (dst->type() == TBottom) {
          if (inst->isBlockEnd()) {
            assertx(inst->nextEdge());
            if (!inst->next()->isUnreachable()) {
              inst->setNext(unreachableBlock(unit, inst->bcctx()));
            }
            return false;
          }
          return true;
        }
      }
      return false;
    };

    auto const markUnreachable = [&] (Block* block) {
      // Any code that's postdominated by Unreachable is also unreachable, so
      // erase everything else in this block.
      if (block->back().hasDst()) block->back().setDst(nullptr);
      unit.replace(&block->back(), Unreachable, ASSERT_REASON);
      for (auto it = block->skipHeader(), end = block->backIter(); it != end;) {
        auto toErase = it;
        ++it;
        block->erase(toErase);
      }
      reachable.reset(block->id());
    };

    {
      jit::stack<Block*> worklist;
      worklist.push(unit.entry());
      while (!worklist.empty()) {
        auto const block = worklist.top();
        worklist.pop();

        if (reachable.test(block->id())) continue;
        if (block->isUnreachable()) continue;
        reachable.set(block->id());

        for (auto& inst : *block) {
          if (unreachableTypes(&inst)) {
            markUnreachable(block);
            break;
          }
          simplifyInPlace(unit, &inst);
        }

        if (!reachable.test(block->id())) continue;
        if (auto const b = block->next()) {
          if (!b->isUnreachable()) worklist.push(b);
        }
        if (auto const b = block->taken()) {
          if (!b->isUnreachable()) worklist.push(b);
        }
      }
    }

    // We may have introduced new unreachable blocks.
    if (unit.numBlocks() > reachable.size()) reachable.resize(unit.numBlocks());

    auto const poBlocks = poSortCfg(unit);
    // Collapse unreachable blocks
    std::deque<Block*> workQ(poBlocks.cbegin(), poBlocks.cend());
    while (!workQ.empty()) {
      auto const block = workQ.front();
      workQ.pop_front();
      if (!reachable.test(block->id())) continue;

      auto& inst = block->back();
      if (inst.hasEdges() &&
          (!inst.next() || !reachable.test(inst.next()->id())) &&
          (!inst.taken() || !reachable.test(inst.taken()->id()))) {
        auto const& preds = block->preds();
        for (auto const& pred : preds) workQ.push_back(pred.from());
        markUnreachable(block);
      }
    }
  } while (reflowTypes(unit));
}

////////////////////////////////////////////////////////////////////////////////

}
