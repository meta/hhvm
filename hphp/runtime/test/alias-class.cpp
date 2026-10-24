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

#include <boost/range/join.hpp>
#include <vector>

#include "hphp/runtime/test/test-context.h"

#include "hphp/runtime/vm/jit/alias-class.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"

namespace HPHP::jit {

namespace {

//////////////////////////////////////////////////////////////////////

std::vector<AliasClass> generic_classes() {
  return {
    ALocalAny,
    APropAny,
    AHeapAny,
    AStackAny,
    AElemIAny,
    AElemSAny,
    AElemAny,
    AIterAny,
    AFContextAny,
    AFFuncAny,
    AFMetaAny,
    AActRecAny
  };
}

std::vector<AliasClass> specialized_classes(IRUnit& unit) {
  auto const bcctx = BCContext { BCMarker::Dummy(), 0 };

  // Specialized test cases need some SSATmp*'s and similar things, so let's
  // make some instructions.
  auto const mainFP = unit.gen(DefFP, bcctx, DefFPData { std::nullopt })->dst();

  return {
    // Frame locals.
    ALocal { mainFP, 1 },
    ALocal { mainFP, 2 },
    ALocal { mainFP, 6 },

    // Some stack locations.
    AStack::at(IRSPRelOffset { -1 }),
    AStack::range(IRSPRelOffset { -4 }, IRSPRelOffset { -1 }),
    AStack::below(IRSPRelOffset { -11 }),
    AStack::range(IRSPRelOffset { -15 }, IRSPRelOffset { -11 }),
    AStack::range(IRSPRelOffset { -13 }, IRSPRelOffset { -10 }),
    AStack::range(IRSPRelOffset { -61 }, IRSPRelOffset { -51 }),

    // ActRec locations
    AFContext { mainFP },
    AFFunc    { mainFP },
    AFMeta    { mainFP },
    AActRec   { mainFP },
  };
}

}

//////////////////////////////////////////////////////////////////////

TEST(AliasClass, AliasIdSet) {
  constexpr auto Max = AliasIdSet::Max;
  constexpr auto BitsetMax = AliasIdSet::BitsetMax;

  EXPECT_TRUE(BitsetMax < 64);

  AliasIdSet big = BitsetMax + 100;
  EXPECT_EQ(big.size(), 1);
  EXPECT_TRUE(big.isBigInteger());
  EXPECT_FALSE(big.empty());

  big.unset(BitsetMax);
  EXPECT_EQ(big.size(), 1);
  EXPECT_TRUE(big.isBigInteger());
  EXPECT_FALSE(big.empty());

  big.set(BitsetMax + 100);
  EXPECT_EQ(big.size(), 1);
  EXPECT_TRUE(big.isBigInteger());
  EXPECT_FALSE(big.empty());
  EXPECT_TRUE(big.hasSingleValue());

  big.unset(BitsetMax + 100);
  EXPECT_EQ(big.size(), 0);
  EXPECT_TRUE(big.isBitset());
  EXPECT_TRUE(big.empty());
  EXPECT_FALSE(big.hasSingleValue());

  AliasIdSet ids { 0u, 3u, IdRange { 6, 9 }, IdRange { 15, 12 }, BitsetMax };

  EXPECT_EQ(ids.size(), 6);
  EXPECT_TRUE(ids.isBitset());
  EXPECT_FALSE(ids.empty());
  EXPECT_FALSE(ids.isBigInteger());
  EXPECT_FALSE(ids.hasUpperRange());

  EXPECT_TRUE(ids.test(0));
  EXPECT_TRUE(ids.test(3));
  EXPECT_FALSE(ids.test(5));
  EXPECT_TRUE(ids.test(6));
  EXPECT_TRUE(ids.test(8));
  EXPECT_FALSE(ids.test(9));
  EXPECT_FALSE(ids.test(12));
  EXPECT_FALSE(ids.test(14));
  EXPECT_FALSE(ids.test(15));
  EXPECT_TRUE(ids.test(BitsetMax));
  EXPECT_FALSE(ids.test(BitsetMax + 1));
  EXPECT_FALSE(ids.test(63));

  EXPECT_TRUE(ids == (ids | AliasIdSet{}));
  EXPECT_TRUE(ids == (ids | 6));
  EXPECT_TRUE(ids == (ids | BitsetMax));
  EXPECT_TRUE((1000 | ids).test(1000));

  AliasIdSet unbounded = IdRange { 4 };

  EXPECT_EQ(unbounded.size(), Max);
  EXPECT_TRUE(unbounded.isBitset());
  EXPECT_FALSE(unbounded.empty());
  EXPECT_FALSE(unbounded.isBigInteger());
  EXPECT_TRUE(unbounded.hasUpperRange());

  EXPECT_TRUE(unbounded.test(4));
  EXPECT_TRUE(unbounded.test(12));
  EXPECT_TRUE(unbounded.test(61));
  EXPECT_TRUE(unbounded.test(62));
  EXPECT_TRUE(unbounded.test(BitsetMax));
  EXPECT_TRUE(unbounded.test(BitsetMax + 1));
  EXPECT_TRUE(unbounded.test(64));
  EXPECT_TRUE(unbounded.test(100));

  EXPECT_TRUE(ids.maybe(IdRange { 5, 7 }));
  EXPECT_TRUE(ids.maybe(unbounded));

  EXPECT_TRUE((ids | unbounded).hasUpperRange());
}

//////////////////////////////////////////////////////////////////////

TEST(AliasClass, Basic) {
  IRUnit unit{test_context};
  auto const specialized = specialized_classes(unit);
  auto const generic = generic_classes();
  auto const joined = boost::join(generic, specialized);

  for (auto cls : joined) {
    // Everything is a subclass of AUnknown and intersects AUnknown.
    EXPECT_TRUE(cls.maybe(AUnknown));
    EXPECT_TRUE(cls <= AUnknown);

    // Everything contains AEmpty, but AEmpty can't be anything.
    EXPECT_TRUE(AEmpty <= cls);
    EXPECT_FALSE(AEmpty.maybe(cls));
  }

  // maybe() is a symmetric relation
  for (auto c1 : joined) {
    EXPECT_EQ(c1.maybe(AUnknown), AUnknown.maybe(c1));
    EXPECT_EQ(c1.maybe(AEmpty), AEmpty.maybe(c1));
    for (auto c2 : joined) {
      EXPECT_EQ(c1.maybe(c2), c2.maybe(c1));
    }
  }

  // If one class is a subclass of another, and they aren't equal, the other
  // isn't a subclass of it.
  for (auto c1 : joined) {
    for (auto c2 : joined) {
      if (c1 != c2) {
        if (c1 <= c2) {
          EXPECT_FALSE(c2 <= c1);
        }
      }
    }
  }

  // == implies <=, and <= implies maybe
  for (auto c1 : joined) {
    for (auto c2 : joined) {
      EXPECT_TRUE(!(c1 == c2) || c1 <= c2);
      EXPECT_TRUE(!(c1 <= c2) || c1.maybe(c2));
    }
  }

  // Arguments to operator| are subclasses of the result, and operator| is
  // commutative.
  for (auto c1 : joined) {
    for (auto c2 : joined) {
      auto const res = c1 | c2;
      EXPECT_EQ(res, c2 | c1);
      EXPECT_TRUE(c1 <= res);
      EXPECT_TRUE(c2 <= res);
    }
  }
}

TEST(AliasClass, StackBasics) {
  IRUnit unit{test_context};

  // Some basic canonicalization and maybe.
  {
    AliasClass const stk = AStack::at(IRSPRelOffset { 0 });

    EXPECT_TRUE(stk <= AStackAny);
    EXPECT_TRUE(stk != AStackAny);

    EXPECT_EQ(stk, stk);
    EXPECT_TRUE(stk.maybe(stk));
    EXPECT_TRUE(stk <= stk);
  }

  // Stack ranges, with subtype and maybe.
  {
    AliasClass const stk1 = AStack::at(IRSPRelOffset { -10 });
    AliasClass const stk2 = AStack::range(IRSPRelOffset { -11 },
                                          IRSPRelOffset { -9 });
    EXPECT_TRUE(stk1 <= stk2);
    EXPECT_TRUE(stk1.maybe(stk2));

    AliasClass const stk3 = AStack::range(IRSPRelOffset { -14 },
                                          IRSPRelOffset { -9 });
    EXPECT_TRUE(stk1 <= stk3);
    EXPECT_TRUE(stk1.maybe(stk3));
    AliasClass const stk4 = AStack::at(IRSPRelOffset { -15 });
    AliasClass const stk5 = AStack::at(IRSPRelOffset { -14 });
    // stk4's slot is immediately below stk3's range, but stk5 is the last slot
    // of its range.
    EXPECT_FALSE(stk3.maybe(stk4));
    EXPECT_FALSE(stk3 <= stk4);
    EXPECT_TRUE(stk5.maybe(stk3));
    EXPECT_TRUE(stk5 <= stk3);
    EXPECT_FALSE(stk5.maybe(stk4));
    EXPECT_FALSE(stk5 <= stk4);
  }
}

TEST(AliasClass, SpecializedUnions) {
  IRUnit unit{test_context};

  AliasClass const stk = AStack::range(IRSPRelOffset { -12 },
                                       IRSPRelOffset { -9 });
  AliasClass const unrelated_stk = AStack::at(IRSPRelOffset { -14 });
  AliasClass const related_stk = AStack::range(IRSPRelOffset { -12 },
                                               IRSPRelOffset { -10 });

  auto const stk_and_frame = stk | ALocalAny;
  EXPECT_TRUE(!stk_and_frame.is_stack());
  EXPECT_TRUE(ALocalAny <= stk_and_frame);
  EXPECT_TRUE(stk <= stk_and_frame);
  EXPECT_TRUE(AStackAny.maybe(stk_and_frame));
  EXPECT_TRUE(ALocalAny.maybe(stk_and_frame));
  EXPECT_FALSE(unrelated_stk <= stk_and_frame);
  EXPECT_FALSE(stk_and_frame.maybe(unrelated_stk));

  auto const stk_and_prop = stk | APropAny;
  EXPECT_TRUE(stk_and_prop.maybe(stk_and_frame));
  EXPECT_TRUE(stk_and_frame.maybe(stk_and_prop));
  EXPECT_FALSE(stk_and_prop <= stk_and_frame);
  EXPECT_FALSE(stk_and_frame <= stk_and_prop);
  EXPECT_TRUE(APropAny.maybe(stk_and_prop));
  EXPECT_TRUE(AStackAny.maybe(stk_and_prop));

  auto const unrelated_stk_and_prop = unrelated_stk | APropAny;
  EXPECT_FALSE(stk_and_frame.maybe(unrelated_stk_and_prop));
  EXPECT_FALSE(unrelated_stk_and_prop.maybe(stk_and_frame));
  EXPECT_TRUE(unrelated_stk_and_prop.maybe(stk_and_prop)); // because of prop
  EXPECT_FALSE(unrelated_stk_and_prop <= stk_and_prop);
  EXPECT_FALSE(stk_and_prop <= unrelated_stk_and_prop);
  EXPECT_FALSE(unrelated_stk_and_prop <= stk_and_frame);
  EXPECT_FALSE(stk_and_frame <= unrelated_stk_and_prop);

  EXPECT_FALSE(stk_and_prop <= AHeapAny);
  EXPECT_TRUE(stk_and_prop.maybe(AHeapAny));
  EXPECT_FALSE(stk_and_frame <= AHeapAny);
  EXPECT_FALSE(stk_and_frame.maybe(AHeapAny));

  auto const rel_stk_and_frame = related_stk | ALocalAny;
  EXPECT_TRUE(stk_and_frame.maybe(rel_stk_and_frame));
  EXPECT_TRUE(rel_stk_and_frame.maybe(stk_and_frame));
  EXPECT_TRUE(related_stk <= stk);
  EXPECT_TRUE(rel_stk_and_frame <= stk_and_frame);
  EXPECT_FALSE(stk_and_frame <= rel_stk_and_frame);
  EXPECT_TRUE(rel_stk_and_frame.maybe(stk_and_prop));
  EXPECT_TRUE(stk_and_prop.maybe(rel_stk_and_frame));
  EXPECT_FALSE(rel_stk_and_frame <= stk_and_prop);

  auto const some_mis = AMIStateTempBase;
  {
    auto const some_heap = AElemIAny;
    auto const u1 = some_heap | some_mis;
    auto const u2 = ALocalAny | u1;
    EXPECT_TRUE((AHeapAny | some_heap) == AHeapAny);
    EXPECT_TRUE(AHeapAny <= (AHeapAny | u1));
    EXPECT_TRUE(AHeapAny <= (AHeapAny | u2));
  }

  auto const mis_stk = some_mis | stk;
  auto const mis_stk_any = AStackAny | mis_stk;

  EXPECT_EQ(some_mis, AliasClass{*mis_stk_any.mis()});
  EXPECT_NE(mis_stk_any, AStackAny | AMIStateAny);

  auto const other_mis = AMIStateBase;

  EXPECT_LE(some_mis,  some_mis | other_mis);
  EXPECT_LE(other_mis, some_mis | other_mis);

  EXPECT_NE(some_mis, some_mis | other_mis);
  EXPECT_NE(other_mis, some_mis | other_mis);
}

TEST(AliasClass, StackUnions) {
  IRUnit unit{test_context};

  {
    AliasClass const stk1  = AStack::at(IRSPRelOffset { -3 });
    AliasClass const stk2  = AStack::at(IRSPRelOffset { -4 });
    AliasClass const stk3  = AStack::at(IRSPRelOffset { -5 });
    AliasClass const stk12 = AStack::range(IRSPRelOffset { -4 },
                                           IRSPRelOffset { -2 });
    AliasClass const stk23 = AStack::range(IRSPRelOffset { -5 },
                                           IRSPRelOffset { -3 });
    AliasClass const stk13 = AStack::range(IRSPRelOffset { -5 },
                                           IRSPRelOffset { -2 });
    EXPECT_EQ(stk1 | stk2, stk12);
    EXPECT_EQ(stk2 | stk3, stk23);
    EXPECT_EQ(stk1 | stk3, stk13);
  }

  // Same as above but with some other bits.
  {
    AliasClass const stk1  = AHeapAny | AStack::at(IRSPRelOffset { -3 });
    AliasClass const stk2  = AHeapAny | AStack::at(IRSPRelOffset { -4 });
    AliasClass const stk3  = AHeapAny | AStack::at(IRSPRelOffset { -5 });
    AliasClass const stk12 = AHeapAny | AStack::range(IRSPRelOffset { -4 },
                                                      IRSPRelOffset { -2 });
    AliasClass const stk23 = AHeapAny | AStack::range(IRSPRelOffset { -5 },
                                                      IRSPRelOffset { -3 });
    AliasClass const stk13 = AHeapAny | AStack::range(IRSPRelOffset { -5 },
                                                      IRSPRelOffset { -2 });
    EXPECT_EQ(stk1 | stk2, stk12);
    EXPECT_EQ(stk2 | stk3, stk23);
    EXPECT_EQ(stk1 | stk3, stk13);
  }

  {
    AliasClass const stk1 = AStack::at(IRSPRelOffset { 0 });
    AliasClass const stk2 = AStack::at(IRSPRelOffset { -2 });
    AliasClass const true_union = AStack::range(IRSPRelOffset { -2 },
                                                IRSPRelOffset { 1 });
    EXPECT_NE(stk1 | stk2, AStackAny);
    EXPECT_EQ(stk1 | stk2, true_union);
  }

  {
    AliasClass const deep_stk1 = AStack::below(IRSPRelOffset { -9 });
    AliasClass const deep_stk2 = AStack::below(IRSPRelOffset { -13 });
    EXPECT_EQ(deep_stk1 | deep_stk2, deep_stk1);
  }
}

TEST(AliasClass, IterUnion) {
  IRUnit unit{test_context};
  auto const bcctx = BCContext { BCMarker::Dummy(), 0 };
  auto const FP = unit.gen(DefFP, bcctx, DefFPData { std::nullopt })->dst();

  {
    AliasClass const iterP0 = aiter_pos(FP, 0);
    AliasClass const iterP1 = aiter_pos(FP, 1);
    auto const u1 = iterP0 | iterP1;
    EXPECT_TRUE(u1 <= AIterAny);
    EXPECT_TRUE(iterP0 <= u1);
    EXPECT_TRUE(iterP1 <= u1);
  }

  {
    AliasClass const iterP0 = aiter_pos(FP, 0);
    AliasClass const iterE0 = aiter_end(FP, 0);
    AliasClass const iterP1 = aiter_pos(FP, 1);

    // All the alias classes should be distinct to start.
    auto const classes = std::vector{iterP0, iterE0, iterP1};
    for (auto const cls1 : classes) {
      for (auto const cls2 : classes) {
        if (cls1 == cls2) continue;
        EXPECT_FALSE(cls1 <= cls2);
      }
    }

    auto const u1 = iterP0 | iterE0;
    EXPECT_TRUE(iterP0 <= u1);
    EXPECT_TRUE(iterE0 <= u1);
    EXPECT_FALSE(iterP1 <= u1);
    EXPECT_TRUE(u1 <= AIterAny);

    EXPECT_TRUE(u1.iter());
    EXPECT_TRUE(u1.is_iter());

    auto const u2 = iterE0 | iterP1;
    EXPECT_FALSE(iterP0 <= u2);
    EXPECT_TRUE(iterE0 <= u2);
    EXPECT_TRUE(iterP1 <= u2);
    EXPECT_TRUE(u2 <= AIterAny);

    EXPECT_TRUE(u1.iter());
    EXPECT_TRUE(u1.is_iter());
  }

  {
    AliasClass const local = ALocal { FP, 0 };
    AliasClass const iter  = aiter_pos(FP, 0);
    auto const u1 = local | iter;
    EXPECT_TRUE(local <= u1);
    EXPECT_TRUE(iter <= u1);
    EXPECT_FALSE(!!u1.is_iter());
    EXPECT_FALSE(!!u1.is_local());
    EXPECT_TRUE(!!u1.local());  // locals are preferred in unions to iters
    EXPECT_FALSE(!!u1.iter());
  }

  {
    AliasClass const iterP0 = aiter_pos(FP, 0);
    AliasClass const iterE0 = aiter_end(FP, 0);
    AliasClass const iterP1 = aiter_pos(FP, 1);
    AliasClass const iterE1 = aiter_end(FP, 1);

    EXPECT_FALSE(iterP0.maybe(iterP1));
    EXPECT_FALSE(iterE0.maybe(iterE1));

    auto const u1 = iterP0 | iterE0;
    auto const u2 = iterP1 | iterE1;
    EXPECT_FALSE(u1 == u2);
    EXPECT_FALSE(u1.maybe(u2));
    EXPECT_FALSE(u1 <= u2);
    EXPECT_FALSE(u2 <= u1);

    EXPECT_TRUE(iterE1 <= u2);
    EXPECT_TRUE(iterP1 <= u2);
    EXPECT_FALSE(iterP0 <= u2);
    EXPECT_FALSE(iterE0 <= u2);

    auto const u3 = u1 | iterP1;
    EXPECT_TRUE(u3.iter());
    EXPECT_TRUE(iterP1 <= u3);
    EXPECT_TRUE(iterP0 <= u3);
    EXPECT_TRUE(iterE0 <= u3);
    EXPECT_TRUE(u1 <= u3);
    EXPECT_TRUE(u2.maybe(u3));

    // u2 <= u3 isn't 'really' true, but operator| is conservative and makes u3
    // too big for that right now.
    EXPECT_TRUE(!u1.precise_union(iterP1));
  }
}

TEST(AliasClass, FrameUnion) {
  IRUnit unit{test_context};
  auto const bcctx = BCContext { BCMarker::Dummy(), 0 };
  auto const dsData = DefStackData { SBInvOffset { 0 }, SBInvOffset { 0 } };
  auto const biData = DefCalleeFPData {
    IRSPRelOffset { 0 }, nullptr, 1, SrcKey {}, IRSPRelOffset { 0 },
    SBInvOffset { 0 }, 0
  };
  auto const SP = unit.gen(DefRegSP, bcctx, dsData)->dst();
  auto const FP1 = unit.gen(DefFP, bcctx, DefFPData { std::nullopt })->dst();
  auto const FP2 = unit.gen(DefCalleeFP, bcctx, biData, SP)->dst();

  AliasClass const local1 = ALocal { FP1, 0 };
  AliasClass const localR = ALocal { FP1, AliasIdSet::IdRange(0, 2) };
  AliasClass const fctx1   = AFContext { FP1 };
  AliasClass const fctx2   = AFContext { FP2 };

  EXPECT_TRUE(local1.maybe(localR));
  EXPECT_TRUE(local1 <= localR);
  EXPECT_FALSE(local1.maybe(fctx1));
  EXPECT_FALSE(localR.maybe(fctx1));
  EXPECT_TRUE(fctx1.maybe(AFContextAny));
  EXPECT_TRUE(fctx1.maybe(AActRecAny));
  EXPECT_TRUE(fctx1.maybe(AActRec{FP1}));

  auto const u1 = local1 | fctx1;
  auto const u2 = localR | fctx1;
  auto const u3 = local1 | fctx2;
  auto const u4 = u3 | fctx1;
  auto const u5 = u4 | AMIStateAny;
  EXPECT_TRUE(u1 <= u2);
  EXPECT_TRUE(u1.maybe(u2));
  EXPECT_FALSE(u1 <= localR);
  EXPECT_FALSE(u1.maybe(fctx2));
  EXPECT_FALSE(u2.maybe(fctx2));
  EXPECT_TRUE(u3.local().has_value());
  EXPECT_FALSE(u3.fcontext().has_value());
  EXPECT_EQ(u3, u4);
  EXPECT_TRUE(u5.local().has_value());
  EXPECT_EQ(u5.local()->frameIdx, local1.local()->frameIdx);
  EXPECT_EQ(u5.local()->ids, local1.local()->ids);
}

TEST(AliasClass, RematerializationWriteUnion) {
  AliasClass const callerLocal = ALocal { 0u, 5 };
  auto writes = AliasClass { ALocal { 0u, AliasIdSet { 1u, 2u, 3u, 4u, 5u } } };
  writes |= ALocal { 2u, 0 };
  writes |= AFContext { 2u };
  writes |= AVMRegAny;

  auto const inlineExit = AliasClass { ALocal { 2u, AliasIdSet { 0u, 1u } } }
    | AActRec { 2u } | AMIStateAny;
  auto const expected = ALocalAny | AActRec { 2u } | AVMRegAny | AMIStateAny;

  EXPECT_TRUE(writes.maybe(callerLocal));
  EXPECT_TRUE((writes | inlineExit).maybe(callerLocal));
  EXPECT_TRUE(callerLocal <= (writes | inlineExit));
  EXPECT_EQ(expected, writes | inlineExit);
  EXPECT_EQ(expected, inlineExit | writes);
  auto const precise = writes.precise_union(inlineExit);
  ASSERT_TRUE(precise.has_value());
  EXPECT_EQ(expected, *precise);
}

TEST(AliasClass, FrameUnionGenericIterators) {
  auto const a = AIterAny | AFContext { 2u };
  auto const b = AliasClass { AIter { 2u, 0 } } | AActRec { 2u };
  auto const expected = AIterAny | AActRec { 2u };
  EXPECT_TRUE(AIterAny <= (a | b));
  EXPECT_EQ(expected, a | b);
  EXPECT_EQ(expected, b | a);
  auto const precise = a.precise_union(b);
  ASSERT_TRUE(precise.has_value());
  EXPECT_EQ(expected, *precise);
}

TEST(AliasClass, FrameUnionGenericActRec) {
  struct Case { AliasClass a; AliasClass b; };
  std::vector<Case> const cases {
    {AliasClass { ALocal { 2u, 0 } } | AFContextAny,
     AliasClass { ALocal { 2u, 1 } } | AFContext { 2u }},
    {AliasClass { AIter { 2u, 0 } } | AFContextAny,
     AliasClass { AIter { 2u, 1 } } | AFContext { 2u }},
    {AliasClass { ALocal { 2u, 0 } } | AFContextAny,
     AliasClass { AIter { 2u, 0 } } | AFContext { 2u }},
    {AliasClass { AFFunc { 2u } } | AFContextAny,
     AliasClass { AFContext { 2u } } | AFFuncAny},
  };

  for (auto const& [a, b] : cases) {
    SCOPED_TRACE(show(a) + " | " + show(b));
    auto const combined = a | b;
    EXPECT_EQ(combined, b | a);
    EXPECT_TRUE(AFContextAny <= combined);
    EXPECT_TRUE(combined.maybe(AFContext { 0u }));
    EXPECT_TRUE(a <= combined);
  }
}

TEST(AliasClass, FrameSubsetUnrelatedSpecialization) {
  AliasClass const a = AFMeta { 0u };
  auto const b = AFMetaAny | AFFunc { 1u };
  EXPECT_TRUE(a <= b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b <= a);
  EXPECT_TRUE(a.maybe(b));
  EXPECT_TRUE(b.maybe(a));

  auto const genericLocal = ALocalAny | AFContext { 1u };
  auto const specificLocal = AliasClass { ALocal { 1u, 0 } } | AFContext { 1u };
  EXPECT_FALSE(genericLocal <= specificLocal);
  EXPECT_TRUE(specificLocal <= genericLocal);
  EXPECT_TRUE(AliasClass(ALocal { 0u, 0 }) <= genericLocal);
  EXPECT_TRUE(genericLocal.maybe(ALocal { 0u, 0 }));
  EXPECT_FALSE(specificLocal.maybe(ALocal { 0u, 0 }));
}

TEST(AliasClass, Pointees) {
  IRUnit unit{test_context};
  auto const bcctx = BCContext { BCMarker::Dummy(), 0 };
  auto ptr = unit.gen(LdMBase, bcctx, TLval, AliasClassData{ ALocalAny | APropAny })->dst();
  auto const acls = pointee(ptr);
  EXPECT_EQ(ALocalAny | APropAny, acls);
}

//////////////////////////////////////////////////////////////////////

}
