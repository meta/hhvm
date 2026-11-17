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

#include "hphp/runtime/vm/jit/irgen-func-prologue.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/generics-info.h"
#include "hphp/runtime/vm/named-params.h"
#include "hphp/runtime/vm/srckey.h"

#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/irgen-control.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-minstr.h"
#include "hphp/runtime/vm/jit/irgen-state.h"
#include "hphp/runtime/vm/jit/irgen-types.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/translation-stats.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/type.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/text-util.h"

namespace HPHP::jit::irgen {

///////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * How to perform our stack overflow check.
 */
enum class StackCheck {
  None,       // not needed
  EarlyNamed, // must occur before optional named parameters are pushed
  Early,      // must occur before setting up locals
  Combine     // can be delayed and combined with surprise flags check
};

StackCheck stack_check_kind(const Func* func) {
  if (func->isPhpLeafFn() &&
      func->maxStackCells() < Cfg::Eval::StackCheckLeafPadding) {
    return StackCheck::None;
  }

  /*
   * Determine how many stack slots we're going to write that the caller hasn't
   * already checked we have space for.
   *
   * We don't need to worry about any of the passed parameter locals, because
   * the caller must have checked for that in its maxStackCells().  However,
   * we'd like to delay our stack overflow check until after we've entered our
   * frame, so we can combine it with the surprise flag check (which must run
   * after we've created the callee).
   *
   * The only things we are going to do is write uninits to the non-passed
   * params and to the non-parameter locals, and possibly shuffle some of the
   * locals into the variadic capture param.  The uninits are harmless to the
   * stack overflow code as long as we know we aren't going to segfault while
   * we write them.
   *
   * There's always sSurprisePageSize extra space at the bottom (lowest
   * addresses) of the eval stack, so we just only do this optimization if
   * we're sure we're going to write few enough uninits that we would be
   * staying within that region if the locals are actually too deep.
   */
  auto const safeFromSEGV = Stack::sSurprisePageSize / sizeof(TypedValue);
  auto safeBeforeNamedParams =
    func->numLocals() < safeFromSEGV + func->numRequiredNamedParams();
  auto canCombine =
    func->numLocals() < safeFromSEGV + func->numRequiredNamedParams() +
                        func->numRequiredPositionalParams();

  return canCombine
    ? StackCheck::Combine
    : (safeBeforeNamedParams ? StackCheck::Early : StackCheck::EarlyNamed);
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

void emitCalleeNamedArgChecks(IRGS& env, const Func* callee,
                              uint32_t posArgc,
                              SSATmp* prologueFlags, SSATmp* namedArgNames) {
  auto throwIfNamedArgsPassed = [&] {
    ifThen(
      env,
      [&] (Block* taken) {
        auto constexpr flag = 1 << PrologueFlags::Flags::HasNamedArguments;
        auto const hasNamedArgs = gen(env, AndInt, prologueFlags, cns(env, flag));
        gen(env, JmpNZero, taken, hasNamedArgs);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, ThrowUnexpectedNamedArguments, FuncData{callee});
      });
  };
  // As noted in the .h comment, this is a special case to facilitate fast prologues
  // for functions without named parameters. The prologue flags tell us whether
  // any named args were passed.
  if (namedArgNames == nullptr) {
    throwIfNamedArgsPassed();
    return;
  }
  if (!namedArgNames->isA(TNullptr) && !namedArgNames->hasConstVal(TVec)) {
    assertx(callee->hasNamedParams() || callee->hasReifiedGenerics());
    if (!callee->hasNamedParams()) {
      throwIfNamedArgsPassed();
      return;
    }
    // We punt the work to a native helper for now to keep implementation simple
    // in the case where named params exist.
    auto stackTopOffset = offsetFromIRSP(env, env.irb->curMarker().bcSPOff()).offset;
    auto const data = CheckFunNamedArgsMismatchData { callee, posArgc, stackTopOffset };
    // TODO(named_params) add at least a pointer equality check for the case where
    // the arrays match exactly.
    gen(env, CheckFunNamedArgsMismatch, data, namedArgNames);
    // The helper above will ensure that the named arg count will match
    // the named param count (or throw), so the rest of codegen may assume
    // the named params are placed properly.
    return;
  }
  // The rest of the function may assume it's called from a prepareAndCallKnown context,
  // where we know the arg names.
  auto const namedArgNamesVal =
    namedArgNames->isA(TNullptr) ? nullptr : namedArgNames->arrLikeVal();
  checkNamedArgMismatch(
    callee, namedArgNamesVal,
    [&](const Func* callee) {
      gen(env, ThrowUnexpectedNamedArguments, FuncData{callee});
    },
    [&](const Func* callee, const StringData* argName) {
      gen(env, ThrowNamedArgumentNameMismatch, FuncData{callee},
          cns(env, argName));
    },
    [&](const Func* callee, const StringData* paramName) {
      gen(env, ThrowMissingNamedParam, FuncData{callee},
          cns(env, paramName));
    }
  );
}

void emitCalleeGenericsChecks(IRGS& env, const Func* callee,
                              SSATmp* prologueFlags, SSATmp* namedArgNames,
                              bool pushed, bool namedArgsAccountedInStack) {
  assertx(
    IMPLIES(
      namedArgsAccountedInStack,
      namedArgNames->isA(TNullptr) || namedArgNames->hasConstVal(TVec)
    )
  );
  if (!callee->hasReifiedGenerics()) {
    // FIXME: leaks memory if generics were given but not expected nor pushed.
    if (pushed) {
      // pushed is only true from a prepareAndCallKnown context, so this pop is
      // safe; we have accurate stack offsets.
      popDecRef(env);
      updateStackOffsetAndExceptionBoundary(env);
    }
    return;
  }

  // Fail if generics were not passed.
  ifThenElse(
    env,
    [&] (Block* taken) {
      if (pushed) return;
      auto constexpr flag = 1 << PrologueFlags::Flags::HasGenerics;
      auto const hasGenerics = gen(env, AndInt, prologueFlags, cns(env, flag));
      gen(env, JmpZero, taken, hasGenerics);
    },
    [&] {
      // Generics were passed. Make them visible on the stack. We don't necessarily
      // know that we have a TVec yet, that's only true in the knownNamedArgs case.
      auto const generics = pushed ? topC(env) : apparate(env, TInitCell);
      updateStackOffsetAndExceptionBoundary(env);

      // Generics may be known if we are inlining.
      if (generics->hasConstVal(TVec)) {
        auto const genericsArr = generics->arrLikeVal();
        auto const& genericsDef =
          callee->getGenericsInfo().m_typeParamInfo;
        if (genericsArr->size() == genericsDef.size()) {
          bool match = true;
          IterateKV(genericsArr, [&](TypedValue k, TypedValue v) {
            assertx(tvIsInt(k) && tvIsArrayLike(v));
            auto const idx = k.m_data.num;
            auto const ts = v.m_data.parr;
            if (isWildCard(ts) && genericsDef[idx].m_isReified) {
              match = false;
              return true;
            }
            return false;
          });
          if (match) return;
        }
      }
      // Fail on generics count/wildcard mismatch.
      ifThen(
        env,
        [&] (Block* taken) {
          auto const match = gen(
            env, IsFunReifiedGenericsMatched, FuncData{callee}, prologueFlags);
          gen(env, JmpZero, taken, match);
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          if (namedArgsAccountedInStack) {
            auto const knownGenerics = gen(env, AssertType, TVec, generics);
            gen(env, CheckFunReifiedGenericMismatch, cns(env, callee), knownGenerics);
            return;
          }
          // This situation happens when we're in a prologue. We may have passed the
          // named arg names via a register. We don't in general statically know the
          // location of the reified generics that were passed and need to punt, as
          // our stack state doesn't know the named arg count. The named arg checks
          // that come after will fix the state. We apparate to make the branches
          // of the if-then-else consistently wrong.
          auto stackTopOffset =
            offsetFromIRSP(env, env.irb->curMarker().bcSPOff()).offset;
          auto const data = GenericsWithNamedArgsData { callee, stackTopOffset };
          // This opcode assumes that the reified generics are at
          // rvmsp + namedArgNames->size().
          gen(env, CheckFunReifiedGenericsWithNamedArgs, data, namedArgNames);
        }
      );
    },
    [&] {
      hint(env, Block::Hint::Unlikely);

      // FIXME: ifThenElse() doesn't save/restore marker and stack boundary.
      updateStackOffsetAndExceptionBoundary(env);

      if (pushed) {
        gen(env, Unreachable, ASSERT_REASON);
        return;
      }

      // Generics not given. We will either fail or raise a warning.
      if (!areAllGenericsSoft(callee->getGenericsInfo())) {
        gen(env, ThrowCallReifiedFunctionWithoutGenerics, cns(env, callee));
        return;
      }

      auto const errMsg = makeStaticString(fmt::format(
        "Generic at index 0 to Function {} must be reified, erased given",
        callee->fullName()->data()));
      gen(env, RaiseWarning, cns(env, errMsg));

      if (namedArgsAccountedInStack) {
        push(env, cns(env, staticEmptyVec()));
        updateStackOffsetAndExceptionBoundary(env);
        return;
      }
      // Push an empty array, as the remainder of the prologue assumes generics
      // are on the stack.
      auto stackTopOffset =
        offsetFromIRSP(env, BCSPRelOffset{ 0 }).offset;
      auto const data =
        GenericsWithNamedArgsData { callee, stackTopOffset };
      gen(env, PushEmptyReifiedGenericsWithNamedArgs, data, namedArgNames);
      // The stack top is still off by namedArgNames' len here, will be
      // accurate after the named arg inputs are placed, so we can't
      // apparate a TVec.
      apparate(env, TInitCell);
      updateStackOffsetAndExceptionBoundary(env);
    }
  );

  // FIXME: ifThenElse() doesn't save/restore marker and stack boundary.
  updateStackOffsetAndExceptionBoundary(env);
}

/*
 * Check for too few or too many arguments and trim extra args.
 */
void emitCalleeArgumentArityChecks(IRGS& env, const Func* callee,
                                   uint32_t& posArgc) {
  // The arity checks are emitted after the named arg ones, which will
  // populate the stack with default values for named params that weren't passed in.
  if (posArgc < callee->numRequiredPositionalParams()) {
    gen(env, ThrowMissingArg, FuncArgData { callee, posArgc });
  }

  auto numPositionalsInclVariadic = callee->numParams() - callee->numNamedParams();
  if (posArgc > numPositionalsInclVariadic) {
    assertx(!callee->hasVariadicCaptureParam());
    assertx(posArgc == callee->numPositionalParams() + 1);
    --posArgc;

    // Pop unpack args, skipping generics (we already know their type).
    auto const generics = callee->hasReifiedGenerics()
      ? popC(env, DataTypeGeneric) : nullptr;
    auto const unpackArgs = pop(env, DataTypeGeneric);
    if (generics != nullptr) push(env, generics);

    // We have updated the stack.
    updateStackOffsetAndExceptionBoundary(env);

    // Pass unpack args to the raiseTooManyArgumentsPrologue() helper, which
    // will use them to report the correct number and also take care of decref.
    auto const unpackArgsArr = gen(env, AssertType, TVec, unpackArgs);
    gen(env, RaiseTooManyArg, FuncData { callee }, unpackArgsArr);
  }
}

void emitCalleeArgumentTypeChecks(IRGS& env, const Func* callee,
                                  uint32_t posArgc, SSATmp* prologueCtx) {
  auto const argc = posArgc + callee->numNamedParams();
  auto const numArgs = std::min(argc, callee->numNonVariadicParams());
  auto const firstArgIdx = argc - 1 + (callee->hasReifiedGenerics() ? 1 : 0);
  for (auto i = 0; i < numArgs; ++i) {
    auto const isOptionalNamedParam =
      i < callee->numNamedParams() && callee->params()[i].hasDefaultValue();
    auto const offset = BCSPRelOffset { safe_cast<int32_t>(firstArgIdx - i) };
    auto const irsproData = IRSPRelOffsetData { offsetFromIRSP(env, offset) };
    if (isOptionalNamedParam) {
        ifElse(
          env,
          [&] (Block* taken) {
            auto const arg = top(env, offset);
            gen(env, CheckType, TInitCell, taken, arg);
          },
          [&] {
            gen(env, AssertStk, TInitCell, irsproData, sp(env));
            verifyParamType(env, callee, i, offset, prologueCtx);
          });
   } else {
      gen(env, AssertStk, TInitCell, irsproData, sp(env));
      verifyParamType(env, callee, i, offset, prologueCtx);
    }
  }
}

void emitCalleeDynamicCallChecks(IRGS& env, const Func* callee,
                                 SSATmp* prologueFlags) {
  if (!Cfg::Eval::NoticeOnBuiltinDynamicCalls || !callee->isBuiltin()) {
    return;
  }

  ifThen(
    env,
    [&] (Block* taken) {
      auto constexpr flag = 1 << PrologueFlags::Flags::IsDynamicCall;
      auto const isDynCall = gen(env, AndInt, prologueFlags, cns(env, flag));
      gen(env, JmpNZero, taken, isDynCall);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);

      std::string errMsg;
      auto const fmtString = callee->isDynamicallyCallable()
        ? Strings::FUNCTION_CALLED_DYNAMICALLY_WITH_ATTRIBUTE
        : Strings::FUNCTION_CALLED_DYNAMICALLY_WITHOUT_ATTRIBUTE;
      string_printf(errMsg, fmtString, callee->fullName()->data());
      gen(env, RaiseNotice, SampleRateData {}, cns(env, makeStaticString(errMsg)));
    }
  );
}

void emitCalleeCoeffectChecks(IRGS& env, const Func* callee,
                              SSATmp* prologueFlags, SSATmp* providedCoeffects,
                              bool skipCoeffectsCheck,
                              uint32_t posArgc, SSATmp* prologueCtx) {
  assertx(callee);
  assertx(prologueFlags);

  if (!CoeffectsConfig::enabled()) {
    if (callee->hasCoeffectsLocal()) {
      push(env, cns(env, RuntimeCoeffects::none().value()));
      updateStackOffsetAndExceptionBoundary(env);
    }
    return;
  }

  // If ambient coeffects are directly provided use them, otherwise extract
  // them from prologueFlags
  if (!providedCoeffects) {
    providedCoeffects =
      gen(env, Lshr, prologueFlags, cns(env, PrologueFlags::CoeffectsStart));
  }

  if (skipCoeffectsCheck) {
    if (callee->hasCoeffectsLocal()) {
      push(env, providedCoeffects);
      updateStackOffsetAndExceptionBoundary(env);
    }
    return;
  }

  auto const requiredCoeffects = [&] {
    auto required = cns(env, callee->requiredCoeffects().value());
    if (!callee->hasCoeffectRules()) return required;
    for (auto const& rule : callee->getCoeffectRules()) {
      if (auto const coeffect = rule.emitJit(env, callee, posArgc, prologueCtx,
                                             providedCoeffects)) {
        required = gen(env, OrInt, required, coeffect);
      }
    }
    if (callee->hasCoeffectsLocal()) {
      push(env, required);
      updateStackOffsetAndExceptionBoundary(env);
    }
    return required;
  }();

  ifThen(
    env,
    [&] (Block* taken) {
      // (~providedCoeffects) & requiredCoeffects == 0

      // The unused higher order bits of providedCoeffects will be 0
      // We want to flip the used lower order bits
      auto const mask = (1 << CoeffectsConfig::numUsedBits()) - 1;
      auto const providedCoeffectsFlipped =
        gen(env, XorInt, providedCoeffects, cns(env, mask));

      auto const cond =
        gen(env, AndInt, providedCoeffectsFlipped, requiredCoeffects);
      gen(env, JmpNZero, taken, cond);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, RaiseCoeffectsCallViolation, FuncData{callee},
          providedCoeffects, requiredCoeffects);
    }
  );
}

void emitCalleeRecordFuncCoverage(IRGS& env, const Func* callee) {
  if (Cfg::Repo::Authoritative || !Cfg::Eval::EnableFuncCoverage) return;
  if (callee->isNoInjection() || callee->isMethCaller()) return;

  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, CheckFuncNeedsCoverage, FuncData{callee}, taken);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, RecordFuncCall, FuncData{callee});
    }
  );
}

///////////////////////////////////////////////////////////////////////////////

namespace {

bool shouldComputeNamedArgNamesInPrologue(const Func* callee) {
  return callee->hasNamedParams() || callee->hasReifiedGenerics();
}

void emitPrologueEntry(IRGS& env, const Func* callee, uint32_t posArgc,
                       TransID transID) {
  gen(env, EnterPrologue);

  // Update marker with the stublogue bit.
  updateMarker(env);

  if (Cfg::HHIR::GenerateAsserts) {
    // Make sure we are at the right function.
    auto const calleeSSA = gen(env, DefFuncPrologueCallee);
    auto const calleeOK = gen(env, EqFunc, calleeSSA, cns(env, callee));
    gen(env, JmpZero, makeUnreachable(env, ASSERT_REASON), calleeOK);

    // Make sure we are at the right prologue.
    auto const numArgs = gen(env, DefFuncPrologueNumArgs);
    auto const numArgsOK = gen(env, EqInt, numArgs, cns(env, posArgc));
    gen(env, JmpZero, makeUnreachable(env, ASSERT_REASON), numArgsOK);
  }

  // Emit debug code.
  if (Trace::moduleEnabled(Trace::ringbuffer)) {
    auto msg = RBMsgData { Trace::RBTypeFuncPrologue, callee->fullName() };
    gen(env, RBTraceMsg, msg);
  }

  // Increment profiling counter.
  if (isProfiling(env.context.kind)) {
    gen(env, IncProfCounter, TransIDData{transID});
    profData()->setProfiling(callee);
  }

  if (Cfg::Jit::CollectTranslationStats) {
    auto transStats = globalTransStats();
    assertx(transStats != nullptr);
    auto sk = SrcKey{callee, posArgc, SrcKey::PrologueTag {}};
    TransID transStatsID = transStats->initTransStats(env.context.kind, sk);
    assertx(transStatsID != kInvalidTransID);
    gen(env, IncStatCounter, TransIDData{transStatsID});
  }
}

void emitStackAdjustmentsForNamedParams(IRGS& env, const Func* callee) {
  // This is subtle - when we're emitting callee checks from a prologue, the bcsp
  // depth needs to be fixed up to now include the prologue named args. We place this
  // here rather than within emitCalleeNamedArgChecks to avoid modifying the stack
  // for known calls.
  env.irb->fs().incBCSPDepth(callee->numNamedParams());
  updateStackOffsetAndExceptionBoundary(env);
  if (callee->hasReifiedGenerics()) {
    // The reified generics have moved to the new top of the stack if we reach
    // this code, so assert a type at the right location.
    auto const offset = offsetFromIRSP(env, BCSPRelOffset{0});
    gen(env, AssertStk, TVec, IRSPRelOffsetData{offset}, sp(env));
  }
}

void emitCalleeChecks(IRGS& env, const Func* callee, uint32_t& posArgc,
                      SSATmp* prologueFlags, SSATmp* prologueCtx,
                      SSATmp* namedArgNames) {
  // Generics are special and need to be checked first, as they may or may not
  // be on the stack. This check makes sure they materialize on the stack
  // if we expect them.
  emitCalleeGenericsChecks(env, callee, prologueFlags, namedArgNames, false, false);
  // Prologues are identified with positional argc. The callee named arg checks
  // will materialize missing optional named params on the stack.
  // Emit early stack overflow check if necessary.
  if (stack_check_kind(callee) == StackCheck::EarlyNamed) {
    gen(env, CheckStackOverflow, sp(env));
  }
  emitCalleeNamedArgChecks(env, callee, posArgc, prologueFlags, namedArgNames);
  emitStackAdjustmentsForNamedParams(env, callee);

  emitCalleeArgumentArityChecks(env, callee, posArgc);
  emitCalleeArgumentTypeChecks(env, callee, posArgc, prologueCtx);
  emitCalleeDynamicCallChecks(env, callee, prologueFlags);
  auto const argc = posArgc + callee->numNamedParams();
  emitCalleeCoeffectChecks(env, callee, prologueFlags, nullptr, false,
                           argc, prologueCtx);
  emitCalleeRecordFuncCoverage(env, callee);

  // Emit early stack overflow check if necessary.
  if (stack_check_kind(callee) == StackCheck::Early) {
    gen(env, CheckStackOverflow, sp(env));
  }
}

} // namespace

/*
 * Shuffles the passed arguments to match the func entry's signature, pushing
 * uninits for any optional named parameters that weren't passed and moves any
 * arguments that were after where the optional named param is expected to be.
 *
 * The idea of the algorithm is to have two pointers, one for the new top of
 * the stack (always <= in address of the old top of the stack), and move
 * typed values over via StStk/LdStk to their new positions and push uninits
 * at the appropriate positions. Starting from the top means that we don't
 * need any intermediate data structures. We can eagerly stop when we've
 * emitted the last uninit.
 */
bool emitInitFuncNamedParams(IRGS& env, const Func* callee,
                             uint32_t posArgc, const ArrayData* namedArgNames) {
  auto const numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  int32_t numToPush =
    static_cast<int32_t>(callee->numNamedParams()) - numNamedArgs;
  assertx(numToPush >= 0);
  if (numToPush == 0) return false;
  env.irb->fs().incBCSPDepth(numToPush);
  int32_t extraInputs = (callee->hasReifiedGenerics() ? 1 : 0);
  int32_t paramIdx = posArgc + callee->numNamedParams() + extraInputs - 1;
  const int32_t firstIdx = paramIdx;
  int32_t argIdx = posArgc + numNamedArgs + extraInputs - 1;
  auto const namedParamNames = callee->sortedNamedParamNames();
  // If numToPush ever hits 0, the rest of the arguments will be unchanged so we
  // can bail early.
  for (; paramIdx >= 0 && numToPush > 0; --paramIdx) {
    auto const paramOff = offsetFromIRSP(env, BCSPRelOffset { firstIdx - paramIdx });
    if (paramIdx >= callee->numNamedParams() ||
        (argIdx >= 0 &&
         namedParamNames[paramIdx] == namedArgNames->at(argIdx).val().pstr)) {

      auto const argOff = offsetFromIRSP(env, BCSPRelOffset { firstIdx - argIdx });
      auto arg = gen(env, LdStk, TCell, IRSPRelOffsetData { argOff }, sp(env));
      gen(env, StStk, IRSPRelOffsetData { paramOff } , sp(env), arg);
      --argIdx;
    } else {
      gen(env, StStk, IRSPRelOffsetData { paramOff }, sp(env), cns(env, TUninit));
      --numToPush;
    }
  }
  updateStackOffsetAndExceptionBoundary(env);
  return true;
}

void emitInitFuncInputsInline(IRGS& env, const Func* callee, uint32_t posArgc,
                              SSATmp* fp) {
  assertx(posArgc <= callee->numPositionalParamsVariadic());
  std::vector<SSATmp*> args;
  auto const argc = posArgc + callee->numNamedParams();

  auto const popInput = [&](Optional<uint32_t> idx = std::nullopt) {
    // We may have pushed uninits for any optional
    // named params.
    // TODO(named_params) we have access to the
    // named arg names here and can do better
    auto typeToAssert = [&] {
      if (idx && *idx < callee->numNamedParams()) {
        const Func::ParamInfo& pi = callee->params()[*idx];
        if (pi.hasDefaultValue()) return TCell;
      }
      return TInitCell;
    }();
    gen(
      env,
      AssertStk,
      typeToAssert,
      IRSPRelOffsetData { offsetFromIRSP(env, BCSPRelOffset{0}) },
      sp(env)
    );
    auto popped = assertType(pop(env, DataTypeGeneric), typeToAssert);
    args.emplace_back(popped);
  };

  // Generics and coeffects are already initialized
  if (callee->hasCoeffectsLocal())  popInput();
  if (callee->hasReifiedGenerics()) popInput();

  // Empty array for `...$args`
  if (callee->hasVariadicCaptureParam() && argc < callee->numParams()) {
    args.emplace_back(cns(env, staticEmptyVec()));
  }

  // Uninit for un-passed positionals
  if (argc < callee->numNonVariadicParams()) {
    for (int c = callee->numNonVariadicParams() - argc; c; c--) {
      args.emplace_back(cns(env, TUninit));
    }
  }

  for (int i = 0; i < argc; i++) popInput(argc - i - 1);
  assertx(args.size() == callee->numFuncEntryInputs());

  // Make the new FramePtr live (marking the caller stack below the frame as
  // killed).
  gen(env, EnterInlineFrame, EnterInlineFrameData::NoCopy(), fp);
  updateMarker(env);

  int loc = 0;
  for (; loc < callee->numFuncEntryInputs(); ++loc) {
    stLocRaw(env, loc, fp, args[callee->numFuncEntryInputs() - loc - 1]);
  }
}

void emitInitFuncInputs(IRGS& env, const Func* callee, uint32_t posArgc,
                        SSATmp* fp) {
  auto argc = callee->numNamedParams() + posArgc;
  assertx(argc <= callee->numParams());
  auto const numInitPositionals =
    std::min(callee->numPositionalParams(), posArgc);

  if (argc == callee->numParams()) {
    gen(
      env,
      EnterInlineFrame,
      EnterInlineFrameData::Copy(numInitPositionals),
      fp
    );
    updateStackOffsetAndExceptionBoundary(env);
    return;
  }

  // Generics and coeffects are already initialized
  auto const coeffects = callee->hasCoeffectsLocal()
    ? popC(env, DataTypeGeneric) : nullptr;
  auto const generics = callee->hasReifiedGenerics()
    ? popC(env, DataTypeGeneric) : nullptr;

  // Push Uninit for un-passed arguments.
  auto const numParams = callee->numNonVariadicParams();
  if (argc < numParams) {
    auto const kMaxArgsInitUnroll = 10;
    auto const count = numParams - argc;
    if (count <= kMaxArgsInitUnroll) {
      while (argc < numParams) {
        push(env, cns(env, TUninit));
        ++argc;
      }
    } else {
      pushMany(env, cns(env, TUninit), count);
      argc = numParams;
    }
  }

  if (argc < callee->numParams()) {
    // Push an empty array for `...$args'.
    assertx(callee->hasVariadicCaptureParam());
    push(env, cns(env, staticEmptyVec()));
    ++argc;
  }

  assertx(argc == callee->numParams());

  // Place generics and coeffects in the correct position.
  if (generics != nullptr) push(env, generics);
  if (coeffects != nullptr) push(env, coeffects);

  gen(
    env,
    EnterInlineFrame,
    EnterInlineFrameData::Copy(numInitPositionals),
    fp
  );
  updateStackOffsetAndExceptionBoundary(env);
}

namespace {

std::tuple<SSATmp*, SSATmp*> emitPrologueExit(IRGS& env, const Func* callee,
                                              SSATmp* prologueFlags) {

  auto const arFlags =
    gen(env, ConvFuncPrologueFlagsToARFlags, FuncData{callee}, prologueFlags);
  auto const calleeId = cns(env, callee->getFuncId().toInt());
  gen(env, ExitPrologue);
  return std::make_tuple(arFlags, calleeId);
}

void emitJmpFuncBody(IRGS& env, const Func* callee, uint32_t posArgc,
                     SSATmp* callerFP, SSATmp* arFlags, SSATmp* calleeId,
                     SSATmp* ctx) {

  // Emit the bindjmp for the function body.
  auto const numPosArgs = std::min(posArgc, callee->numPositionalParams());
  auto mayHaveUninitNamed = callee->hasOptionalNamedParameters();
  auto sk =
    SrcKey { callee, numPosArgs, mayHaveUninitNamed, SrcKey::FuncEntryTag {} };
  gen(
    env,
    ReqBindJmp,
    ReqBindJmpData {
      sk,
      SBInvOffset { 0 },
      spOffBCFromIRSP(env),
      false /* popFrame */
    },
    sp(env),
    callerFP,
    arFlags,
    calleeId,
    ctx
  );
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

namespace {

void definePrologueFrameAndStack(IRGS& env, const Func* callee, uint32_t posArgc) {
  // Define caller's frame. It is unknown if/where it lives on the stack.
  gen(env, DefFP, DefFPData { std::nullopt });
  updateMarker(env);

  // The stack base of prologues points to the stack without the potentially
  // uninitialized space reserved for ActRec and inouts. The rvmsp() register
  // points to the future ActRec. The stack contains additional `posArgc' inputs
  // below the ActRec, as well as an (as of yet) unknown number of named args.
  // The named parameter checks will ensure that exactly
  // `callee->numNamedParams()` named arguments will be present in the stack.
  auto const argc = posArgc + callee->numNamedParams();
  auto const cells = callee->numInOutParamsForArgs(argc) + kNumActRecCells;
  auto const irSPOff = SBInvOffset { safe_cast<int32_t>(cells) };
  auto const bcSPOff = SBInvOffset { safe_cast<int32_t>(cells + posArgc) };
  gen(env, DefRegSP, DefStackData { irSPOff, bcSPOff });

  // Now that the stack is initialized, update the BC marker and perform
  // initial sync of the exception stack boundary.
  updateStackOffsetAndExceptionBoundary(env);
}

} // namespace

void emitFuncPrologue(IRGS& env, const Func* callee, uint32_t positionals,
                      TransID transID) {
  assertx(positionals <= callee->numPositionalParams() + 1);

  definePrologueFrameAndStack(env, callee, positionals);

  // Define register inputs before doing anything else that may clobber them.
  auto const prologueFlags = gen(env, DefFuncPrologueFlags);
  auto const prologueCtx = (callee->isClosureBody() || callee->cls())
    ? gen(env, DefFuncPrologueCtx, callCtxType(callee))
    : cns(env, nullptr);
  // The source register of DefFuncPrologueNamedArgs contains garbage unless
  // named args were passed. To generate less code, we only compute the named
  // args if the callee has named params, and the named args check is aware of
  // how to handle namedArgNames being nullptr.
  SSATmp* namedArgNames = nullptr;
  if (shouldComputeNamedArgNamesInPrologue(callee)) {
    namedArgNames = cond(
      env,
      [&] (Block* taken) {
        auto constexpr flag = 1 << PrologueFlags::Flags::HasNamedArguments;
        auto const hasNamedArgs = gen(env, AndInt, prologueFlags, cns(env, flag));
        gen(env, JmpZero, taken, hasNamedArgs);
      },
      [&] {
        return gen(env, DefFuncPrologueNamedArgs);
      },
      [&] {
        return cns(env, nullptr);
      });
  }
  emitPrologueEntry(env, callee, positionals, transID);
  emitCalleeChecks(env, callee, positionals, prologueFlags, prologueCtx, namedArgNames);
  emitInitFuncInputs(env, callee, positionals, genCalleeFP(env, callee, 0));
  auto [arFlags, calleeId] = emitPrologueExit(env, callee, prologueFlags);
  // Func entries are identified by the number of positional arguments passed.
  emitJmpFuncBody(env, callee, positionals, fp(env), arFlags, calleeId, prologueCtx);
}

namespace {

void emitInitScalarDefaultParamLocal(IRGS& env, const Func* callee,
                                     uint32_t param) {
  auto const dv = callee->params()[param].defaultValue;
  gen(env, StLoc, LocalId{param}, fp(env), cns(env, dv));
}

void emitInitLocalRange(IRGS& env, const Func* callee,
                        uint32_t from, uint32_t to) {
  assertx(from <= to);
  /*
   * Maximum number of local initializations to unroll.
   *
   * The actual crossover point in terms of code size is 6 (just like for the
   * params init unroll limit); 9 was determined by experiment to be the
   * optimal point in certain benchmarks.
   *
   * We don't limit the count in optimized translations, as we expect these
   * stores to be elided.
   */
  auto constexpr kMaxLocalsInitUnroll = 9;

  assertx(from <= to);

  // Set all remaining uninitialized locals to Uninit.
  if (env.context.kind == TransKind::Optimize ||
      to - from <= kMaxLocalsInitUnroll) {
    for (auto i = from; i < to; ++i) {
      gen(env, StLoc, LocalId{i}, fp(env), cns(env, TUninit));
    }
  } else {
    auto const range = LocalIdRange{from, to};
    gen(env, StLocRange, range, fp(env), cns(env, TUninit));
  }
}

void emitInitDefaultParamLocals(IRGS& env, const Func* callee, uint32_t posArgc) {
  assertx(posArgc <= callee->numPositionalParams());

  // We are done. If there were variadics, they were already initialized.
  if (posArgc == callee->numPositionalParams()) return;

  // Set locals of parameters with default values to Uninit. In optimized
  // translations, these locals will be overwritten later, so these stores
  // will be optimized away.
  emitInitLocalRange(env, callee, posArgc + callee->numNamedParams(),
                     callee->numNonVariadicParams());
}

/*
 * Unpack closure use variables into locals.
 */
void emitInitClosureLocals(IRGS& env, const Func* callee) {
  if (!callee->isClosureBody()) return;

  auto const closureTy = Type::ExactObj(callee->implCls());
  auto const closure = gen(env, LdFrameThis, closureTy, fp(env));

  auto const ctx = [&] {
    if (!callee->cls()) return cns(env, nullptr);
    if (callee->isStatic()) {
      return gen(env, LdClosureCls, Type::SubCls(callee->cls()), closure);
    }
    auto const closureThis =
      gen(env, LdClosureThis, Type::SubObj(callee->cls()), closure);
    gen(env, IncRef, closureThis);
    return closureThis;
  }();
  if (!(ctx->type() <= TNullptr)) gen(env, StFrameCtx, fp(env), ctx);

  // Push the closure's use variables (stored in closure object properties).
  auto const firstClosureUseLocal = callee->firstClosureUseLocalId();
  auto const numUses = callee->numClosureUseLocals();
  auto const cls = callee->implCls();

  auto const getProp = [&](auto index) {
    assertx(index < numUses);
    auto const slot = index + (cls->hasClosureCoeffectsProp() ? 1 : 0);
    auto const type =
      typeFromRAT(cls->declPropRepoAuthType(slot), callee->cls()) & TCell;
    return ldClosureArg(env, closure, cls, slot, type);
  };

  // Move props and skip incref when closure refcount is 1 and going to be
  // released.
  ifThenElse(
    env,
    [&](Block* taken){
      gen(env, DecReleaseCheck, DecRefData(), taken, closure);
    },
    [&] { // Next: closure RC goes to 0
      for (auto i = 0; i < numUses; ++i) {
        auto const prop = getProp(i);
        gen(env, StLoc, LocalId{firstClosureUseLocal + i}, fp(env), prop);
      }
      gen(env, ReleaseShallow, closure);
    },
    [&] { // Taken: closure RC != 0
      for (auto i = 0; i < numUses; ++i) {
        auto const prop = getProp(i);
        gen(env, IncRef, prop);
        gen(env, StLoc, LocalId{firstClosureUseLocal + i}, fp(env), prop);
      }
    }
  );
}

/*
 * Set non-input locals to Uninit.
 */
void emitInitRegularLocals(IRGS& env, const Func* callee) {
  emitInitLocalRange(
    env, callee, callee->firstRegularLocalId(), callee->numLocals());
}

void emitSurpriseCheck(IRGS& env, const Func* callee) {
  if (isInlining(env)) return;

  // Check surprise flags in the same place as the interpreter: after func entry
  // and DV initializers, right before entering the main body.
  auto const checkStackOverflow =
    stack_check_kind(callee) == StackCheck::Combine;
  auto const data = CheckHandleSurpriseEnterData { callee, checkStackOverflow };

  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, CheckSurpriseFlagsEnter, data, taken, fp(env));
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, HandleSurpriseEnter, data, fp(env));
    }
  );
}

}

void emitEnter(IRGS& env, Offset relOffset) {
  emitSurpriseCheck(env, curFunc(env));

  jmpImpl(env, bcOff(env) + relOffset);
}

void emitFuncEntry(IRGS& env) {
  assertx(curSrcKey(env).funcEntry());
  auto const callee = curFunc(env);

  if (curSrcKey(env).trivialDVFuncEntry()) {
    assertx(!isInlining(env));
    auto const posParam = curSrcKey(env).numEntryArgs();
    auto const paramIdx = posParam + callee->numNamedParams();
    emitInitScalarDefaultParamLocal(env, callee, paramIdx);
    emitJmpFuncBody(env, callee, posParam + 1, env.funcEntryPrevFP,
                    env.funcEntryArFlags, env.funcEntryCalleeId,
                    env.funcEntryCtx);
    return;
  }

  emitInitDefaultParamLocals(env, callee, curSrcKey(env).numEntryArgs());
  emitInitClosureLocals(env, callee);
  emitInitRegularLocals(env, callee);

  // DV initializers delay the function call event hook until the Enter opcode.
  if (curSrcKey(env).numEntryArgs() < callee->numPositionalParams()) return;

  emitSurpriseCheck(env, callee);
}

void emitNamedParamsFuncEntry(IRGS& env) {
  assertx(curSrcKey(env).namedParamsFuncEntry());
  auto const callee = curFunc(env);

  // Default param locals don't need to be emitted since being here means all
  // positionals were passed in.
  emitInitClosureLocals(env, callee);
  emitInitRegularLocals(env, callee);

  // DV initializers delay the function call event hook until the Enter opcode, so
  // don't emit a surprise check.
  return;
}

///////////////////////////////////////////////////////////////////////////////

}
