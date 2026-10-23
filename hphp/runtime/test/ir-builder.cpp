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

#include "hphp/runtime/test/test-context.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/jit/ir-builder.h"
#include "hphp/system/systemlib.h"

namespace HPHP::jit {
namespace {

struct GuardEnv {
  GuardEnv() {
    irb.setCurMarker(BCMarker::Dummy());
    irb.enableConstrainGuards();
    auto const fp = gen(DefFP, DefFPData{std::nullopt});
    gen(DefFrameRelSP, DefStackData{SBInvOffset{7}, SBInvOffset{5}}, fp);
  }

  template<class... Args>
  SSATmp* gen(Opcode op, Args&&... args) {
    auto const inst = unit.gen(
      op, irb.nextBCContext(), std::forward<Args>(args)...
    );
    return irb.optimizeInst(inst, irgen::IRBuilder::CloneFlag::No, nullptr);
  }

  SSATmp* load(Location l) {
    switch (l.tag()) {
      case LTag::Local:
        return gen(LdLoc, TCell, LocalId{l.localId()}, irb.fs().fp());
      case LTag::Stack:
        return gen(
          LdStk, TCell,
          IRSPRelOffsetData{l.stackIdx().to<IRSPRelOffset>(irb.fs().irSPOff())},
          irb.fs().sp()
        );
      case LTag::MBase: {
        auto const ptr = gen(LdMBase, TLval, AliasClassData{AUnknownTV});
        return gen(LdMem, irb.fs().mbase().type, ptr);
      }
    }
    not_reached();
  }

  GuardConstraint constraint(const IRInstruction* guard) const {
    return irb.guards()->guards.at(guard);
  }

  IRUnit unit{test_context};
  irgen::IRBuilder irb{unit, SystemLib::getExceptionClass()->getCtor()};
};

const Location locations[] = {
  Location::Local{0},
  Location::Stack{SBInvOffset{3}},
  Location::MBase{}
};

}

TEST(IRBuilder, GenericGuardDoesNotTrackLocation) {
  GuardEnv env;
  auto const numInsts = env.unit.numInsts();

  for (auto const l : locations) {
    env.irb.guardType(l, TCell);
    EXPECT_FALSE(env.irb.fs().tracked(l));
  }

  EXPECT_EQ(numInsts, env.unit.numInsts());
  EXPECT_TRUE(env.irb.guards()->guards.empty());
}

TEST(IRBuilder, GuardAssumptionsAreConstrainedThroughLoads) {
  for (auto const l : locations) {
    SCOPED_TRACE(show(l));
    GuardEnv env;
    env.irb.guardType(l, TStr);
    ASSERT_EQ(1, env.irb.guards()->guards.size());
    auto const guard = env.irb.guards()->guards.begin()->first;
    EXPECT_EQ(TStr, env.irb.fs().typeOf(l));
    EXPECT_EQ(nullptr, guard->taken());

    auto const value = env.load(l);
    EXPECT_TRUE(env.irb.constrainValue(
      value, GuardConstraint{DataTypeSpecific}.setWeak()
    ));
    EXPECT_EQ(TCell, relaxToConstraint(TStr, env.constraint(guard)));
    EXPECT_EQ(0, env.irb.numGuards());

    EXPECT_TRUE(env.irb.constrainValue(value, DataTypeSpecific));
    EXPECT_EQ(TStr, relaxToConstraint(TStr, env.constraint(guard)));
    EXPECT_EQ(1, env.irb.numGuards());
    EXPECT_FALSE(env.irb.constrainValue(
      value, GuardConstraint{DataTypeSpecific}.setWeak()
    ));
  }
}

TEST(IRBuilder, OrdinaryAssertionsCanRemoveGuardDependencies) {
  for (auto const loadFirst : {false, true}) {
    SCOPED_TRACE(loadFirst);
    GuardEnv env;
    auto const l = Location{Location::Local{0}};
    env.irb.guardType(l, TStr);
    auto const guard = env.irb.guards()->guards.begin()->first;

    // Loading first makes the ordinary AssertLoc become an AssertType.
    if (loadFirst) env.load(l);
    env.gen(AssertLoc, TStr, LocalId{0}, env.irb.fs().fp());
    auto const value = env.load(l);

    EXPECT_FALSE(env.irb.constrainValue(value, DataTypeSpecific));
    EXPECT_EQ(TCell, relaxToConstraint(TStr, env.constraint(guard)));
    EXPECT_EQ(1, env.irb.guards()->guards.size());
  }
}

TEST(IRBuilder, OrdinaryAssertionsCanRetainGuardDependencies) {
  GuardEnv env;
  auto const l = Location{Location::Local{0}};
  env.irb.guardType(l, TInt);
  auto const guard = env.irb.guards()->guards.begin()->first;
  env.gen(AssertLoc, TInt | TBool, LocalId{0}, env.irb.fs().fp());

  EXPECT_TRUE(env.irb.constrainValue(env.load(l), DataTypeSpecific));
  EXPECT_EQ(TInt, relaxToConstraint(TInt, env.constraint(guard)));
}

TEST(IRBuilder, OrdinaryChecksAreNotRegionGuards) {
  GuardEnv env;
  auto const l = Location{Location::Local{0}};
  auto const l1 = Location{Location::Local{1}};
  env.irb.guardType(l, TStr);
  auto const guard = env.irb.guards()->guards.begin()->first;

  env.gen(CheckType, TInt, env.unit.defBlock(), env.load(l1));
  EXPECT_FALSE(env.irb.constrainLocation(Location::Local{1}, DataTypeSpecific));

  auto const value = env.gen(Conjure, TCell);
  auto const checked = env.gen(CheckType, TInt, env.unit.defBlock(), value);
  EXPECT_FALSE(env.irb.constrainValue(checked, DataTypeSpecific));

  ASSERT_EQ(1, env.irb.guards()->guards.size());
  EXPECT_EQ(guard, env.irb.guards()->guards.begin()->first);
  EXPECT_EQ(TCell, relaxToConstraint(TStr, env.constraint(guard)));
  EXPECT_EQ(0, env.irb.numGuards());
}

TEST(IRBuilder, OrdinaryChecksCanRetainGuardDependencies) {
  GuardEnv env;
  auto const l = Location{Location::Local{0}};
  env.irb.guardType(l, TStr);
  auto const guard = env.irb.guards()->guards.begin()->first;

  env.gen(
    CheckType, TStaticStr | TInt,
    env.unit.defBlock(), env.load(l)
  );
  auto const value = env.load(l);
  EXPECT_EQ(TStaticStr, value->type());

  EXPECT_TRUE(env.irb.constrainValue(
    value, GuardConstraint{DataTypeSpecific}.setWeak()
  ));
  EXPECT_EQ(DataTypeGeneric, env.constraint(guard).category);
  EXPECT_TRUE(env.irb.constrainValue(value, DataTypeSpecific));
  EXPECT_EQ(TStr, relaxToConstraint(TStr, env.constraint(guard)));
  EXPECT_EQ(1, env.irb.guards()->guards.size());
}

TEST(IRBuilder, GuardConstraintsAreMinimized) {
  GuardEnv env;
  auto const l = Location{Location::Local{0}};
  env.irb.guardType(l, TUninit);
  auto const guard = env.irb.guards()->guards.begin()->first;

  EXPECT_TRUE(env.irb.constrainLocation(l, DataTypeSpecific));
  EXPECT_EQ(DataTypeCountnessInit, env.constraint(guard).category);
  EXPECT_FALSE(env.irb.constrainLocation(l, DataTypeSpecific));
}

TEST(IRBuilder, SpecializedGuardAssumptionsRetainSpecialization) {
  GuardEnv env;
  auto const l = Location{Location::Local{0}};
  auto const cls = SystemLib::getExceptionClass();
  auto const type = Type::SubObj(cls);
  env.irb.guardType(l, type);
  auto const guard = env.irb.guards()->guards.begin()->first;

  EXPECT_TRUE(env.irb.constrainValue(env.load(l), GuardConstraint{cls}));
  EXPECT_EQ(type, relaxToConstraint(type, env.constraint(guard)));
}

}
