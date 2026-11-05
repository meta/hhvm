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

#include <gtest/gtest.h>

#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/normalized-instruction.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"

namespace HPHP::jit {
namespace {

const Op arithmeticOps[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod};

struct ArithmeticResult {
  Type type;
  bool unreachable;
};

ArithmeticResult translateArithmetic(Op op, Type lhs, Type rhs) {
  UnitEmitter ue{
    SHA1{301}, SHA1{302}, RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults()
  };
  ue.m_filepath = makeStaticString("irgen-arithmetic.php");
  auto const fe = ue.newFuncEmitter(makeStaticString("irgen_arithmetic"));
  fe->init(1, 1, AttrNone, nullptr, false);
  fe->retTypeConstraints = TypeIntersectionConstraint{TypeConstraint{}};
  fe->recordSourceLocation(HPHP::Location::Range{1, 1, 1, 1}, 0);
  fe->emitOp(Op::Int);
  fe->emitInt64(6);
  fe->emitOp(Op::Int);
  fe->emitInt64(2);
  auto const offset = fe->bcPos();
  fe->emitOp(op);
  fe->emitOp(Op::RetC);
  fe->emitByte(static_cast<uint8_t>(VerifyRetKind::None));
  fe->maxStackCells = 2;
  fe->finish();
  ue.finish();
  std::unique_ptr<Unit> source{ue.create()};
  always_assert(!source->funcs().empty());
  auto const func = source->funcs().front();
  auto const sk = SrcKey{func, offset, ResumeMode::None};
  TransContext context{TransIDSet{}, 0, TransKind::Optimize, sk};
  IRUnit unit{context};
  irgen::IRGS env{unit, nullptr, 10, nullptr};
  auto const fp = irgen::gen(env, DefFP, DefFPData{std::nullopt});
  irgen::gen(env, DefFrameRelSP,
            DefStackData{SBInvOffset{0}, SBInvOffset{0}}, fp);
  irgen::push(env, irgen::gen(env, Conjure, lhs));
  irgen::push(env, irgen::gen(env, Conjure, rhs));
  irgen::prepareForNextHHBC(env, sk);
  NormalizedInstruction ni{sk, source.get()};
  translateInstr(env, ni);

  auto const result = env.irb->stack(
    irgen::offsetFromIRSP(env, BCSPRelOffset{0}), DataTypeGeneric
  ).type;
  irgen::assertTypeStack(env, BCSPRelOffset{0}, TInitCell);
  return {result, env.irb->inUnreachableState()};
}

TEST(IRGenArithmetic, BroadNumericOperandsCanReturnFromInterpreter) {
  for (auto const op : arithmeticOps) {
    for (auto const input : {TInt | TDbl, TInitCell}) {
      for (auto const broadLhs : {false, true}) {
        SCOPED_TRACE(opcodeToName(op));
        SCOPED_TRACE(input.toString());
        SCOPED_TRACE(broadLhs);
        auto const result = translateArithmetic(
          op, broadLhs ? input : TInt, broadLhs ? TInt : input
        );
        EXPECT_NE(result.type, TBottom);
        EXPECT_TRUE(result.type.maybe(TInt | TDbl));
        EXPECT_FALSE(result.unreachable);
      }
    }
  }
}

TEST(IRGenArithmetic, DefinitelyNonNumericOperandsCannotReturn) {
  for (auto const op : arithmeticOps) {
    for (auto const invalidLhs : {false, true}) {
      SCOPED_TRACE(opcodeToName(op));
      SCOPED_TRACE(invalidLhs);
      auto const result = translateArithmetic(
        op, invalidLhs ? TStr : TInt, invalidLhs ? TInt : TStr
      );
      EXPECT_EQ(result.type, TBottom);
      EXPECT_TRUE(result.unreachable);
    }
  }
}

}
}
