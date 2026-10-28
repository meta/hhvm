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

#include <iterator>
#include <vector>

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/test/test-context.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/check.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/opt.h"

namespace HPHP::jit {
namespace {

// In the diagrams, newVec(n)/newDict(n) mean a copy of a static array with
// n references (including n - 1 IncRefs). Unknown counted values may alias
// whenever their types overlap. "exit" omits the final Unreachable.
struct PhiEnv {
  explicit PhiEnv(bool reverse = false) {
    auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
    auto const cond = unit.gen(Conjure, bcctx, TBool);
    auto const branch = unit.gen(JmpZero, bcctx,
                                reverse ? left : right, cond->dst());
    branch->setNext(reverse ? right : left);
    unit.entry()->push_back({fp, cond, branch});
  }

  SSATmp* phi(SSATmp* leftValue, SSATmp* rightValue) {
    left->push_back(unit.gen(Jmp, bcctx, join, leftValue));
    right->push_back(unit.gen(Jmp, bcctx, join, rightValue));
    auto const label = unit.defLabel(1, join, bcctx);
    retypeDests(label, &unit);
    return label->dst(0);
  }

  SSATmp* array(Block* block, uint32_t refs = 1) {
    auto const original = unit.gen(Conjure, bcctx, TStaticVec);
    auto const owned = unit.gen(CopyArray, bcctx, original->dst());
    auto const pos = !block->empty() && block->back().isBlockEnd()
      ? block->backIter() : block->end();
    block->insert(pos, original);
    block->insert(pos, owned);
    for (auto i = uint32_t{1}; i < refs; ++i) {
      block->insert(pos, unit.gen(IncRef, bcctx, owned->dst()));
    }
    return owned->dst();
  }

  void finish(Block* block) {
    block->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  }

  IRUnit unit{test_context};
  BCContext bcctx{BCMarker::Dummy(), 0};
  Block* left = unit.defBlock();
  Block* right = unit.defBlock();
  Block* join = unit.defBlock();
};

}

/*
 * a: CountedVec, unknown refcount
 *            |
 *         DecRef a
 *
 * A decrement must not turn an infinite upper bound into a finite bound.
 */
TEST(RefcountOpts, PreserveInfiniteUpperBoundAfterDecref) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const value = unit.gen(Conjure, bcctx, TCountedVec);
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  unit.entry()->push_back({fp, value, dec,
                          unit.gen(Unreachable, bcctx, ASSERT_REASON)});

  optimizeRefcounts(unit);

  // Keep DecRef: no lower bound proves it non-releasing. Analysis must also
  // leave the unknown upper bound infinite after subtracting one.
  EXPECT_EQ(DecRef, dec->op());
}

/*
 *       owner = LdLoc 0; StLoc 0, null
 *       value: Obj|Int, borrowed from owner
 *                    IncRef value
 *                    DecRef owner
 *               CheckType<Obj> value
 *                  /             \
 *           IncRef obj           (nothing)
 *                  \             /
 *                   DecRefNZ value
 *                   StLoc 1, value
 *
 * owner may hold value's only reference. Its release must not precede the
 * increment retaining value, even though their types also permit aliasing.
 * The final NZ is valid: objects take another reference and integers are
 * uncounted.
 */
TEST(RefcountOpts, RetainPayloadAcrossOwnerRelease) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const value = unit.gen(Conjure, bcctx, TObj | TInt);
  auto const clearOwner =
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull));
  auto const retain = unit.gen(IncRef, bcctx, value->dst());
  auto const release = unit.gen(DecRef, bcctx, DecRefData{}, owner->dst());
  auto const counted = unit.defBlock();
  auto const join = unit.defBlock();
  auto const check = unit.gen(CheckType, bcctx, TObj, join, value->dst());
  check->setNext(counted);
  unit.entry()->push_back({
    fp, owner, value, clearOwner, retain, release, check
  });
  auto const inc = unit.gen(IncRef, bcctx, check->dst());
  counted->push_back({inc, unit.gen(Jmp, bcctx, join)});
  auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, value->dst());
  join->push_back({
    dec, unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), value->dst()),
    unit.gen(Unreachable, bcctx, ASSERT_REASON)
  });

  optimizeRefcounts(unit);

  // Retain value before releasing its owner. Only the later increment can
  // be cancelled against the final decrement.
  EXPECT_EQ(IncRef, retain->op());
  EXPECT_EQ(DecRef, release->op());
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_EQ(Nop, counted->front().op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       owner = LdLoc 0; StLoc 0, null
 *       value: Obj|Int, borrowed from owner
 *                    IncRef value
 *                    DecRef owner
 *               CheckType<Obj> value
 *                  /             \
 *           obj: Obj             int: Int
 *           IncRef obj           (nothing)
 *                  \             /
 *                   p = phi(obj, int)
 *                       DecRef p
 *                       StLoc 1, p
 *
 * The phi joins the two refinements of value, as refineTmps would do. Unlike
 * the NZ case above, this decrement relies on the merged reference bound.
 */
TEST(RefcountOpts, RetainPayloadAcrossOwnerReleaseWithPhi) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const value = unit.gen(Conjure, bcctx, TObj | TInt);
  auto const clearOwner =
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull));
  auto const retain = unit.gen(IncRef, bcctx, value->dst());
  auto const release = unit.gen(DecRef, bcctx, DecRefData{}, owner->dst());
  auto const counted = unit.defBlock();
  auto const uncounted = unit.defBlock();
  auto const join = unit.defBlock();
  auto const check = unit.gen(CheckType, bcctx, TObj, uncounted, value->dst());
  check->setNext(counted);
  unit.entry()->push_back({
    fp, owner, value, clearOwner, retain, release, check
  });
  auto const inc = unit.gen(IncRef, bcctx, check->dst());
  counted->push_back({inc, unit.gen(Jmp, bcctx, join, check->dst())});
  auto const integer = unit.gen(AssertType, bcctx, TInt, value->dst());
  uncounted->push_back({integer, unit.gen(Jmp, bcctx, join, integer->dst())});
  auto const label = unit.defLabel(1, join, bcctx);
  retypeDests(label, &unit);
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, label->dst(0));
  join->push_back({
    dec, unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), label->dst(0)),
    unit.gen(Unreachable, bcctx, ASSERT_REASON)
  });

  optimizeRefcounts(unit);

  // Keep the retain before releasing the owner. The phi's bound permits
  // cancellation of only the later increment and decrement.
  EXPECT_EQ(IncRef, retain->op());
  EXPECT_EQ(DecRef, release->op());
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_EQ(Nop, counted->front().op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       a: Obj, unknown refcount
 *       b = LdLoc 0; StLoc 0, null
 *                 IncRef a
 *                    |
 *                   Jmp
 *                    |
 *                DecRefNZ b
 *                DecRefNZ a
 *
 * b's decrement is balanced. The final NZ guarantee permits cancellation
 * even if a == b; an eager requirement at b's decrement would block it.
 */
TEST(RefcountOpts, CancelAcrossBalancedDecref) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const a = unit.gen(Conjure, bcctx, TObj);
  auto const b = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const inc = unit.gen(IncRef, bcctx, a->dst());
  auto const body = unit.defBlock();
  unit.entry()->push_back({
    fp, a, b,
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    inc, unit.gen(Jmp, bcctx, body)
  });
  auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, a->dst());
  body->push_back({
    unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()), dec,
    unit.gen(Unreachable, bcctx, ASSERT_REASON)
  });

  optimizeRefcounts(unit);

  // b's balanced decrement must not prevent cancellation of a's pair.
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *                  a = LdLoc 0
 *                  b = LdLoc 0
 *              value = LdLoc 1: Obj|Int
 *           StLoc 0, null; StLoc 1, null
 *           owner = LdLoc 2; StLoc 2, null
 *             p: unknown property address
 *                    DecRefNZ b
 *                    StMem p, a
 *                    IncRef value
 *                    DecRef owner
 *               CheckType<Obj> value
 *                  /             \
 *              object           integer
 *           IncRef value        (nothing)
 *                  \             /
 *                   DecRefNZ value
 *                   StLoc 3, value
 *
 * a, b, and value may alias, with two initial references in locals 0 and 1.
 * b's decrement consumes local 0's reference. The store can move local 1's
 * surviving reference into owner's property. The retain must precede owner's
 * release even though a's unsupported credit was consumed by DecRefNZ b.
 */
TEST(RefcountOpts, RetainPayloadAfterUnknownStore) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const a = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const b = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const value =
    unit.gen(LdLoc, bcctx, TObj | TInt, LocalId{1}, fp->dst());
  auto const clearA =
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull));
  auto const clearValue =
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull));
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{2}, fp->dst());
  auto const clearOwner =
    unit.gen(StLoc, bcctx, LocalId{2}, fp->dst(), unit.cns(TInitNull));
  auto const ptr = unit.gen(Conjure, bcctx, TLvalToProp);
  auto const drop = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
  auto const store = unit.gen(StMem, bcctx, ptr->dst(), a->dst());
  auto const retain = unit.gen(IncRef, bcctx, value->dst());
  auto const release = unit.gen(DecRef, bcctx, DecRefData{}, owner->dst());
  auto const counted = unit.defBlock();
  auto const join = unit.defBlock();
  auto const check = unit.gen(CheckType, bcctx, TObj, join, value->dst());
  check->setNext(counted);
  unit.entry()->push_back({
    fp, a, b, value, clearA, clearValue, owner, clearOwner, ptr,
    drop, store, retain, release, check
  });
  auto const inc = unit.gen(IncRef, bcctx, check->dst());
  counted->push_back({inc, unit.gen(Jmp, bcctx, join)});
  auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, value->dst());
  join->push_back({
    dec, unit.gen(StLoc, bcctx, LocalId{3}, fp->dst(), value->dst()),
    unit.gen(Unreachable, bcctx, ASSERT_REASON)
  });

  optimizeRefcounts(unit);

  EXPECT_EQ(IncRef, retain->op());
  EXPECT_EQ(DecRef, release->op());
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_EQ(Nop, counted->front().op());
  EXPECT_TRUE(checkCfg(unit));
}


/*
 *      a = LdLoc 0; [IncRef a]
 *      b = LdLoc 1; owner = LdLoc 2
 *      p: unknown property address
 *             StLoc 1, null; StLoc 2, null
 *                  StMem p, a
 *                  DecRefNZ b
 *            value = LdLoc 0: Obj|Int
 *                  StLoc 0, null
 *                  IncRef value
 *                  DecRef owner
 *             CheckType<Obj> value
 *                /             \
 *           IncRef obj          (nothing)
 *                \             /
 *                 DecRefNZ value
 *                 StLoc 3, value
 *
 * Locals 0 and 1 may initially hold the same object. The store moves local 0's
 * reference into owner's property, leaving a borrowed pointer in local 0.
 * After b's decrement, the property owns the only remaining reference unless
 * the optional IncRef a supplies another. Reloading local 0 must not count the
 * property's reference as independent ownership.
 */
TEST(RefcountOpts, RetainPayloadAfterPendingLoad) {
  for (auto const extra : {false, true}) {
    SCOPED_TRACE(extra);
    IRUnit unit{test_context};
    BCContext const bcctx{BCMarker::Dummy(), 0};
    auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
    auto const a = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
    auto const b = unit.gen(LdLoc, bcctx, TObj, LocalId{1}, fp->dst());
    auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{2}, fp->dst());
    auto const ptr = unit.gen(Conjure, bcctx, TLvalToProp);
    auto const value = unit.gen(LdLoc, bcctx, TObj | TInt, LocalId{0}, fp->dst());
    auto const retain = unit.gen(IncRef, bcctx, value->dst());
    auto const release = unit.gen(DecRef, bcctx, DecRefData{}, owner->dst());
    auto const counted = unit.defBlock();
    auto const join = unit.defBlock();
    auto const check = unit.gen(CheckType, bcctx, TObj, join, value->dst());
    check->setNext(counted);
    unit.entry()->push_back({fp, a});
    if (extra) unit.entry()->push_back(unit.gen(IncRef, bcctx, a->dst()));
    unit.entry()->push_back({
      b, owner, ptr,
      unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)),
      unit.gen(StLoc, bcctx, LocalId{2}, fp->dst(), unit.cns(TInitNull)),
      unit.gen(StMem, bcctx, ptr->dst(), a->dst()),
      unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()),
      value,
      unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
      retain, release, check
    });
    auto const inc = unit.gen(IncRef, bcctx, check->dst());
    counted->push_back({inc, unit.gen(Jmp, bcctx, join)});
    auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, value->dst());
    join->push_back({dec,
      unit.gen(StLoc, bcctx, LocalId{3}, fp->dst(), value->dst()),
      unit.gen(Unreachable, bcctx, ASSERT_REASON)});
    optimizeRefcounts(unit);

    // With extra credit, the retain can cancel against the final decrement.
    // Otherwise owner releases the payload before the replacement increment.
    EXPECT_EQ(extra ? Nop : IncRef, retain->op());
    EXPECT_EQ(DecRef, release->op());
    EXPECT_EQ(Nop, inc->op());
    EXPECT_EQ(Nop, dec->op());
    EXPECT_EQ(extra ? IncRef : Nop, counted->front().op());
    EXPECT_TRUE(checkCfg(unit));
  }
}

/*
 *      value = LdLoc 0
 *      first: CountedVec; second = LdLoc 1
 *                 IncRef value; IncRef value
 *              StLoc 0, null; StLoc 1, null
 *                   BespokeAppend first, second ---> catch: exit
 *                            |
 *                   CheckArrayCOW value
 *                      /          \
 *                    copy        unique: exit
 *                  DecRef value
 *
 * value owns three references, and second owns an independent fourth if they
 * alias. Consuming first and second leaves at least two references to value.
 * The first consume can leave a pending reduction on the second operand before
 * its balanced/unbalanced classification within the same instruction.
 */
TEST(RefcountOpts, PreserveCreditAcrossMultipleConsumes) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
  auto const first = unit.gen(Conjure, bcctx, TCountedVec);
  auto const second = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{1}, fp->dst());
  auto const copy = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const check = unit.gen(CheckArrayCOW, bcctx, copy, value->dst());
  check->setNext(unique);
  auto const catchBlock = unit.defBlock();
  unit.defLabel(0, catchBlock, bcctx);
  catchBlock->push_back({unit.gen(BeginCatch, bcctx, TObj | TNullptr),
                        unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  auto const body = unit.defBlock();
  auto const append = unit.gen(BespokeAppend, bcctx, catchBlock,
                               first->dst(), second->dst());
  append->setNext(body);
  unit.entry()->push_back({fp, value, first, second,
    unit.gen(IncRef, bcctx, value->dst()),
    unit.gen(IncRef, bcctx, value->dst()),
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)),
    append});
  body->push_back(check);
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  copy->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  optimizeRefcounts(unit);

  // TODO: Fold the COW check: at least two references remain.
  EXPECT_EQ(CheckArrayCOW, check->op());
  // TODO: Preserve the independent credit supplied by second. Applying
  // first's pending reduction when classifying second loses the proof that
  // the final decrement is non-releasing.
  EXPECT_EQ(DecRef, dec->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       a = LdLoc 0; [IncRef a]
 *       b = LdLoc 0; value = LdLoc 1: Obj|Int
 *             StLoc 0, null; StLoc 1, null
 *                       DecRefNZ b
 *                  owner = NewPair(a, null)
 *                       IncRef value
 *                       DecRef owner
 *                  CheckType<Obj> value
 *                     /             \
 *                IncRef obj          (nothing)
 *                     \             /
 *                      DecRefNZ value
 *                      StLoc 3, value
 *
 * a, b, and value may alias. b consumes local 0's reference, and NewPair moves
 * the remaining reference into the pair. The optional increment supplies an
 * independent reference; otherwise value must be retained before the pair is
 * released. NewPair moves references without a PureStore memory effect.
 */
TEST(RefcountOpts, RetainPayloadAfterPendingMove) {
  for (auto const extra : {false, true}) {
    SCOPED_TRACE(extra);
    IRUnit unit{test_context};
    BCContext const bcctx{BCMarker::Dummy(), 0};
    auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
    auto const a = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
    auto const b = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
    auto const value =
      unit.gen(LdLoc, bcctx, TObj | TInt, LocalId{1}, fp->dst());
    auto const clearA =
      unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull));
    auto const clearValue =
      unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull));
    auto const drop = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
    auto const owner = unit.gen(NewPair, bcctx, a->dst(), unit.cns(TInitNull));
    auto const retain = unit.gen(IncRef, bcctx, value->dst());
    auto const release = unit.gen(DecRef, bcctx, DecRefData{}, owner->dst());
    auto const counted = unit.defBlock();
    auto const join = unit.defBlock();
    auto const check = unit.gen(CheckType, bcctx, TObj, join, value->dst());
    check->setNext(counted);
    unit.entry()->push_back({fp, a});
    if (extra) unit.entry()->push_back(unit.gen(IncRef, bcctx, a->dst()));
    unit.entry()->push_back({
      b, value, clearA, clearValue, drop, owner, retain, release, check
    });
    auto const inc = unit.gen(IncRef, bcctx, check->dst());
    counted->push_back({inc, unit.gen(Jmp, bcctx, join)});
    auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, value->dst());
    join->push_back({
      dec, unit.gen(StLoc, bcctx, LocalId{3}, fp->dst(), value->dst()),
      unit.gen(Unreachable, bcctx, ASSERT_REASON)
    });

    optimizeRefcounts(unit);

    // With extra credit, the retain can cancel against the final decrement.
    // Otherwise owner releases the payload before the replacement increment.
    // FIXME[CRASH]: Keep the early retain when extra is false.
    EXPECT_EQ(Nop, retain->op());
    EXPECT_EQ(DecRef, release->op());
    EXPECT_EQ(Nop, inc->op());
    EXPECT_EQ(Nop, dec->op());
    EXPECT_EQ(IncRef, counted->front().op());
    EXPECT_TRUE(checkCfg(unit));
  }
}

/*
 *       a = LdLoc 0; value = LdLoc 1
 *       other: CountedVec; p: unknown property address
 *                          owner = LdLoc 2
 *                    IncRef value; IncRef value
 *                       CheckArrayCOW value
 *                          /          \
 *                        copy        unique: exit
 *             StLoc 0, null; StLoc 1, null; StLoc 2, null
 *                       DecRefNZ other
 *                         StMem p, a
 *                        DecRef owner
 *                        DecRef value
 *
 * a has a pending reduction when stored into owner's property. value has three
 * supported references, plus a's independent reference if they alias. The
 * unbalanced decrement and the property release remove at most two in total.
 * Canceling one of value's increments with its final decrement is safe.
 */
TEST(RefcountOpts, CancelPairAfterPendingUnknownStore) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const a = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
  auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{1}, fp->dst());
  auto const other = unit.gen(Conjure, bcctx, TCountedVec);
  auto const ptr = unit.gen(Conjure, bcctx, TLvalToProp);
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{2}, fp->dst());
  auto const copy = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const check = unit.gen(CheckArrayCOW, bcctx, copy, value->dst());
  check->setNext(unique);
  unit.entry()->push_back({fp, a, value, other, ptr, owner,
    unit.gen(IncRef, bcctx, value->dst()),
    unit.gen(IncRef, bcctx, value->dst()), check});
  copy->push_back({
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{2}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(DecRefNZ, bcctx, DecRefData{}, other->dst()),
    unit.gen(StMem, bcctx, ptr->dst(), a->dst()),
    unit.gen(DecRef, bcctx, DecRefData{}, owner->dst())});
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  copy->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  optimizeRefcounts(unit);

  // TODO: Fold the initial COW check using value's three references.
  EXPECT_EQ(CheckArrayCOW, check->op());
  // TODO: Preserve enough independent credit to cancel the pair. The
  // effective-bound check makes the store discard all supported credit,
  // which is more pessimistic than accounting for the two possible decrements.
  EXPECT_EQ(DecRef, dec->op());
  EXPECT_TRUE(checkCfg(unit));
}


/*
 *       owner = LdLoc 0; value = LdLoc 1
 *       a = LdMem &owner.prop0; b = LdMem &owner.prop1
 *                IncRef value; IncRef value
 *                 StLoc 0, null; StLoc 1, null
 *                       CheckArrayCOW a
 *                         /          \
 *                   copy: exit      unique
 *                                DecRef owner
 *                             CheckArrayCOW value
 *                                /          \
 *                              copy        unique: exit
 *                            DecRef value
 *
 * The first COW check transfers a's reference to its result on the unique
 * path. Removing a's old memory support then creates pending reductions
 * before b's support is removed by the same heap effect. b's independent
 * credit must not be spent a second time against value's references.
 */
TEST(RefcountOpts, PreserveCreditAcrossHeapSupportRemovals) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{1}, fp->dst());
  auto const ptrA = unit.gen(LdPropAddr, bcctx, IndexData{0}, owner->dst());
  auto const ptrB = unit.gen(LdPropAddr, bcctx, IndexData{1}, owner->dst());
  auto const a = unit.gen(LdMem, bcctx, TCountedVec, ptrA->dst());
  auto const b = unit.gen(LdMem, bcctx, TCountedVec, ptrB->dst());
  auto const body = unit.defBlock();
  auto const release = unit.defBlock();
  auto const drop = unit.gen(CheckArrayCOW, bcctx, release, a->dst());
  drop->setNext(body);
  unit.entry()->push_back({fp, owner, value, ptrA, ptrB, a, b,
    unit.gen(IncRef, bcctx, value->dst()),
    unit.gen(IncRef, bcctx, value->dst()),
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)), drop});
  release->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  auto const copy = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const check = unit.gen(CheckArrayCOW, bcctx, copy, value->dst());
  check->setNext(unique);
  body->push_back({unit.gen(DecRef, bcctx, DecRefData{}, owner->dst()), check});
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  copy->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  optimizeRefcounts(unit);

  // TODO: Fold the COW check: value still has independent references.
  EXPECT_EQ(CheckArrayCOW, check->op());
  // TODO: Preserve b's independent credit while removing the heap support.
  // The effective-bound check treats b's removal as another unbalanced
  // decrement, losing the proof that value's final decrement cannot release.
  EXPECT_EQ(DecRef, dec->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       owner = LdLoc 0; value = LdLoc 1
 *             a = LdMem &owner.prop0
 *                    IncRef value
 *                 CheckArrayCOW value
 *                    /          \
 *                  copy        unique: exit
 *         StLoc 0, null; StLoc 1, null
 *                 DecReleaseCheck a
 *                    /          \
 *             non-releasing    releasing: exit
 *               DecRef owner
 *               DecRef value
 *
 * If a == value, the property, local, and increment supply three references.
 * DecReleaseCheck's taken edge consumes one; releasing owner consumes another.
 * The final decrement must release the remaining reference. The initial COW
 * observation prevents canceling the increment against that decrement.
 */
TEST(RefcountOpts, ReleaseAfterDecReleaseCheckTaken) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const owner = unit.gen(LdLoc, bcctx, TObj, LocalId{0}, fp->dst());
  auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{1}, fp->dst());
  auto const ptrA = unit.gen(LdPropAddr, bcctx, IndexData{0}, owner->dst());
  auto const a = unit.gen(LdMem, bcctx, TCountedVec, ptrA->dst());
  auto const body = unit.defBlock();
  auto const release = unit.defBlock();
  auto const drop = unit.gen(DecReleaseCheck, bcctx, DecRefData{}, body, a->dst());
  drop->setNext(release);
  auto const shared = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const check = unit.gen(CheckArrayCOW, bcctx, shared, value->dst());
  check->setNext(unique);
  unit.entry()->push_back({fp, owner, value, ptrA, a,
    unit.gen(IncRef, bcctx, value->dst()), check});
  shared->push_back({
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)), drop});
  release->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  body->push_back(unit.gen(DecRef, bcctx, DecRefData{}, owner->dst()));
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  body->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  optimizeRefcounts(unit);

  // FIXME[LEAK]: Keep DecRef. DecReleaseCheck decremented a on its taken
  // edge, so releasing owner can leave value with exactly one reference.
  EXPECT_EQ(DecRefNZ, dec->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       a = LdLoc 0; b = LdLoc 0
 *       value = LdLoc 1: CountedDict|Int
 *           StLoc 0, null; StLoc 1, null
 *                     DecRefNZ b
 *       owner = StPtrIterVal a, &a[0], null
 *                    IncRef value
 *                    DecRef owner
 *           CheckType<CountedDict> value
 *                  /             \
 *            IncRef dict         (nothing)
 *                  \             /
 *                   DecRefNZ value
 *                   StLoc 3, value
 *
 * Both locals may own the same one-element dict containing null. b's decrement
 * leaves it unique. StPtrIterVal transfers that reference to its result without
 * changing the refcount, so value needs a retain before releasing owner.
 */
TEST(RefcountOpts, RetainPayloadAcrossUniqueDictTransfer) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const a = unit.gen(LdLoc, bcctx, TCountedDict, LocalId{0}, fp->dst());
  auto const b = unit.gen(LdLoc, bcctx, TCountedDict, LocalId{0}, fp->dst());
  auto const value =
    unit.gen(LdLoc, bcctx, TCountedDict | TInt, LocalId{1}, fp->dst());
  auto const ptr = unit.gen(GetDictPtrIter, bcctx, a->dst(), unit.cns(int64_t{0}));
  auto const transferred =
    unit.gen(StPtrIterVal, bcctx, a->dst(), ptr->dst(), unit.cns(TInitNull));
  auto const retain = unit.gen(IncRef, bcctx, value->dst());
  auto const release = unit.gen(DecRef, bcctx, DecRefData{}, transferred->dst());
  auto const counted = unit.defBlock();
  auto const join = unit.defBlock();
  auto const check = unit.gen(CheckType, bcctx, TCountedDict, join, value->dst());
  check->setNext(counted);
  unit.entry()->push_back({fp, a, b, value, ptr,
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(StLoc, bcctx, LocalId{1}, fp->dst(), unit.cns(TInitNull)),
    unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()),
    transferred, retain, release, check});
  auto const inc = unit.gen(IncRef, bcctx, check->dst());
  counted->push_back({inc, unit.gen(Jmp, bcctx, join)});
  auto const dec = unit.gen(DecRefNZ, bcctx, DecRefData{}, value->dst());
  join->push_back({dec, unit.gen(StLoc, bcctx, LocalId{3}, fp->dst(), value->dst()),
                   unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  optimizeRefcounts(unit);

  // The result owns the last reference before the retain.
  EXPECT_EQ(IncRef, retain->op());
  EXPECT_EQ(DecRef, release->op());
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_EQ(Nop, counted->front().op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *                 value = LdLoc 0
 *                   IncRef value
 *               CheckArrayCOW value
 *                   /          \
 *                 shared      unique: exit
 *              StLoc 0, null
 *             ReturnHook fp, value
 *                /            \
 *        exception            normal: exit
 *        DecRef value
 *
 * The local and increment supply two references. If the hook throws, it
 * decrements the return value before entering the catch block. The cleanup
 * must release the remaining reference. The initial COW check preserves the
 * increment; source 0 of ReturnHook is an uncounted frame pointer.
 */
TEST(RefcountOpts, ReleaseAfterReturnHookTaken) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
  auto const shared = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const cow = unit.gen(CheckArrayCOW, bcctx, shared, value->dst());
  cow->setNext(unique);
  unit.entry()->push_back({fp, value, unit.gen(IncRef, bcctx, value->dst()), cow});
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  auto const caught = unit.defBlock();
  unit.defLabel(0, caught, bcctx);
  auto const after = unit.defBlock();
  auto const hook = unit.gen(ReturnHook, bcctx, caught, fp->dst(), value->dst());
  hook->setNext(after);
  shared->push_back({
    unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(), unit.cns(TInitNull)), hook});
  auto const tail = unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
  caught->push_back({unit.gen(BeginCatch, bcctx, TObj | TNullptr), tail,
                    unit.gen(Unreachable, bcctx, ASSERT_REASON)});
  after->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  optimizeRefcounts(unit);

  // FIXME[LEAK]: Keep DecRef. ReturnHook consumed its return value on
  // the exception edge, leaving only the reference held for this cleanup.
  EXPECT_EQ(DecRefNZ, tail->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *                 value = LdLoc 0: CountedVec
 *                        IncRef value
 *                     CheckArrayCOW value
 *                        /          \
 *                      copy        unique: exit
 *                  StLoc 0, null
 *           [DecRefNZ other] repeated pending times
 *            DecRef value / DecReleaseCheck value
 *
 * The observation preserves the initial increment. Pending decrements through
 * an unknown alias can leave the stored bound of two with an effective bound
 * of two, one, zero, or zero after clamping a negative difference. The latter
 * three cases cannot prove that the final decrement is non-releasing.
 */
TEST(RefcountOpts, WeakenWithPendingReductions) {
  for (auto const pending : {0, 1, 2, 3}) {
    for (auto const releaseCheck : {false, true}) {
      SCOPED_TRACE(pending);
      SCOPED_TRACE(releaseCheck);
      IRUnit unit{test_context};
      BCContext const bcctx{BCMarker::Dummy(), 0};
      auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
      auto const value = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
      auto const other = unit.gen(Conjure, bcctx, TCountedVec);
      auto const copy = unit.defBlock();
      auto const unique = unit.defBlock();
      auto const check = unit.gen(CheckArrayCOW, bcctx, copy, value->dst());
      check->setNext(unique);
      unit.entry()->push_back({fp, value, other,
        unit.gen(IncRef, bcctx, value->dst()), check});
      copy->push_back(unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(),
                               unit.cns(TInitNull)));
      for (int i = 0; i < pending; ++i) {
        copy->push_back(unit.gen(DecRefNZ, bcctx, DecRefData{}, other->dst()));
      }
      auto const dec = releaseCheck
        ? unit.gen(DecReleaseCheck, bcctx, DecRefData{}, unique, value->dst())
        : unit.gen(DecRef, bcctx, DecRefData{}, value->dst());
      if (releaseCheck) {
        auto const release = unit.defBlock();
        dec->setNext(release);
        release->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
        copy->push_back(dec);
      } else {
        copy->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});
      }
      unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
      optimizeRefcounts(unit);

      auto const expected = pending
        ? (releaseCheck ? DecReleaseCheck : DecRef)
        : DecRefNZ;
      EXPECT_EQ(expected, dec->op());
      EXPECT_TRUE(checkCfg(unit));
    }
  }
}

/*
 *                     a = newVec(3)
 *                       Jmp(false)
 *                            |
 *                +----> loop(done)
 *                |         /    \
 *                |     false    true
 *                |       /        \
 *                | DecRefNZ a    CheckArrayCOW a
 *                | DecRefNZ a      /       \
 *                | IncRef a      copy     unique
 *                +-Jmp(true)
 *
 * The body executes once, but its two NZ decrements followed by an increment
 * establish a lower bound of two even without using that control-flow fact.
 */
TEST(RefcountOpts, PreserveFiniteLoopLowerBound) {
  PhiEnv env;
  auto const value = env.array(env.unit.entry(), 3);
  auto const done = env.phi(env.unit.cns(false), env.unit.cns(false));
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const branch = env.unit.gen(JmpNZero, env.bcctx, exit, done);
  branch->setNext(body);
  env.join->push_back(branch);
  body->push_back({
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, value),
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, value),
    env.unit.gen(IncRef, env.bcctx, value),
    env.unit.gen(Jmp, env.bcctx, env.join, env.unit.cns(true))
  });
  retypeDests(done->inst(), &env.unit);
  auto const copy = env.unit.defBlock();
  auto const unique = env.unit.defBlock();
  auto const check = env.unit.gen(CheckArrayCOW, env.bcctx, copy, value);
  check->setNext(unique);
  exit->push_back(check);
  env.finish(copy);
  env.finish(unique);

  optimizeRefcounts(env.unit);

  // Expected: jump directly to copy. Entry supplies three references and
  // every backedge supplies at least two, so the array cannot be unique.
  // TODO: Fold the COW check using the loop's guaranteed lower bound.
  EXPECT_EQ(CheckArrayCOW, check->op());
  EXPECT_EQ(copy, check->taken());
}

/*
 *                     a = newVec(3)
 *                       Jmp(false)
 *                            |
 *                +----> loop(done): CheckArrayCOW a
 *                |                  /          \
 *                |                copy        unique: exit
 *                |               /    \
 *                |          !done      done
 *                |            /          \
 *                |      DecRefNZ a      DecRef a
 *                |      DecRefNZ a        exit
 *                |      IncRef a
 *                +------Jmp(true)
 *
 * The COW observation prevents moving an increment past the loop header.
 */
TEST(RefcountOpts, WeakenDecrefAfterFiniteLoop) {
  PhiEnv env;
  auto const value = env.array(env.unit.entry(), 3);
  auto const done = env.phi(env.unit.cns(false), env.unit.cns(false));
  auto const decide = env.unit.defBlock();
  auto const unique = env.unit.defBlock();
  auto const check = env.unit.gen(CheckArrayCOW, env.bcctx, decide, value);
  check->setNext(unique);
  env.join->push_back(check);
  env.finish(unique);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const branch = env.unit.gen(JmpNZero, env.bcctx, exit, done);
  branch->setNext(body);
  decide->push_back(branch);
  body->push_back({
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, value),
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, value),
    env.unit.gen(IncRef, env.bcctx, value),
    env.unit.gen(Jmp, env.bcctx, env.join, env.unit.cns(true))
  });
  retypeDests(done->inst(), &env.unit);
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, value);
  exit->push_back(dec);
  env.finish(exit);

  optimizeRefcounts(env.unit);

  // Expected: weaken the exit decrement to DecRefNZ. Entry supplies three
  // references and every backedge supplies at least two: the last DecRefNZ
  // leaves one, then IncRef adds one. The COW observation keeps those refs
  // from being canceled against the exit decrement.
  EXPECT_EQ(DecRefNZ, dec->op());
}

/*
 *                 a = newVec(2)
 *                    IncRef a
 *                 /           \
 *         DecRefNZ b       DecRefNZ c
 *                 \           /
 *                  newVec(1)
 *                   DecRef a
 *
 * b and c may alias a. Each path removes at most one of a's three refs.
 */
TEST(RefcountOpts, MergePendingDecrefsByAffectedSet) {
  PhiEnv env;
  auto const value = env.array(env.unit.entry(), 2);
  auto const inc = env.unit.gen(IncRef, env.bcctx, value);
  env.unit.entry()->insert(env.unit.entry()->backIter(), inc);
  for (auto const block : {env.left, env.right}) {
    auto const alias = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    block->push_back({alias,
      env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, alias->dst()),
      env.unit.gen(Jmp, env.bcctx, env.join)});
  }
  env.array(env.join);
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, value);
  env.join->push_back(dec);
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Expected: cancel the explicit IncRef a and DecRef a. Only one predecessor
  // executes, so even without the pair, a retains at least one reference.
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
}

/*
 *                    a = newVec(1)
 *                    /           \
 *       DecRefNZ b1,b2,b3       DecRefNZ c1,c2,c3
 *                    \           /
 *                     newVec(1)
 *                      DecRef a
 *
 * The type-based may-alias sets include a, but its upper bound is one and
 * each NZ operand has at least two references before its decrement.
 */
TEST(RefcountOpts, SaturatePendingDecrefs) {
  PhiEnv env;
  auto const value = env.array(env.unit.entry());
  for (auto const block : {env.left, env.right}) {
    for (auto i = 0; i < 3; ++i) {
      auto const alias = env.unit.gen(Conjure, env.bcctx, TCountedVec);
      block->push_back({alias,
        env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, alias->dst())});
    }
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  }
  env.array(env.join);
  auto const release = env.unit.gen(DecRef, env.bcctx, DecRefData{}, value);
  env.join->push_back(release);
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Keep the releasing DecRef: a retains its single reference throughout.
  // TODO: Exclude a from the NZ operands' aliases using their disjoint bounds,
  // preserving a's lower bound without recording pending reductions for it.
  // This test's final opcode is unchanged by that improvement; the next test
  // checks a cancellation which needs the resulting non-alias fact.
  // TODO: Saturate pending reductions once the effective lower bound is zero.
  EXPECT_EQ(DecRef, release->op());
}

/*
 *                   a = newVec(n), b: CountedVec
 *                           DecRefNZ b
 *                               |
 *                              Jmp
 *                               |
 *                            IncRef a
 *                               |
 *                              Jmp
 *                               |
 *                           DecRefNZ b
 *                            DecRef a
 *
 * For n = 1, a's [1,1] range is disjoint from b's [2,infinity] range at
 * the first NZ. Both values remain live, so they still cannot alias after
 * IncRef a makes their ranges overlap. For n = 2, b may alias a throughout.
 */
TEST(RefcountOpts, ExcludeAliasesWithDisjointRefcountBounds) {
  for (auto const refs : {1, 2}) {
    SCOPED_TRACE(refs);
    IRUnit unit{test_context};
    BCContext const bcctx{BCMarker::Dummy(), 0};
    auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
    auto const original = unit.gen(Conjure, bcctx, TStaticVec);
    auto const a = unit.gen(CopyArray, bcctx, original->dst());
    auto const b = unit.gen(Conjure, bcctx, TCountedVec);
    unit.entry()->push_back({fp, original, a, b});
    auto const extra = refs == 2 ? unit.gen(IncRef, bcctx, a->dst()) : nullptr;
    if (extra) unit.entry()->push_back(extra);
    auto const first = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
    auto const body = unit.defBlock();
    unit.entry()->push_back({first, unit.gen(Jmp, bcctx, body)});
    auto const inc = unit.gen(IncRef, bcctx, a->dst());
    auto const exit = unit.defBlock();
    body->push_back({inc, unit.gen(Jmp, bcctx, exit)});
    auto const second = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
    auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, a->dst());
    exit->push_back({second, dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});

    optimizeRefcounts(unit);

    // For n = 1, cancel inc/dec while keeping both NZ decrements: neither
    // touches a. The first NZ proves a != b before a's increment.
    // For n = 2 and a == b, counts are 2 -> 1 -> 2 -> 1 -> 0. Removing the
    // pair while leaving the second NZ would omit the final release. Keep
    // the pair, or cancel it and change the second NZ to a releasing DecRef.
    // TODO: Infer a != b from the disjoint bounds for n = 1 and retain it
    // across IncRef a, allowing cancellation without weakening either NZ.
    if (extra) EXPECT_EQ(IncRef, extra->op());
    EXPECT_EQ(DecRefNZ, first->op());
    EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRefNZ, second->op());
    EXPECT_EQ(DecRef, dec->op());
    EXPECT_TRUE(checkCfg(unit));
  }
}

/*
 *       v = newVec(1)                   w = newVec(1)
 *          IncRef v                        IncRef w
 *             \ (v,v,false)           (w,w,false) /
 *   +----------> join(p,q,done) <---------------+
 *   |                  /       \
 *   |             !done         done
 *   |               /             \
 *   |     body: DecRefNZ q     exit: newVec(1)
 *   +---------- Jmp(p,p,true)        DecRef q
 *
 * p and q receive the same array on every incoming edge, including the backedge.
 */
TEST(RefcountOpts, CarryPendingDecrefsThroughPhi) {
  PhiEnv env;
  std::vector<IRInstruction*> incs;
  for (auto const block : {env.left, env.right}) {
    auto const value = env.array(block);
    auto const inc = env.unit.gen(IncRef, env.bcctx, value);
    incs.push_back(inc);
    block->push_back({inc,
      env.unit.gen(Jmp, env.bcctx, env.join,
                   value, value, env.unit.cns(false))});
  }
  auto const label = env.unit.defLabel(3, env.join, env.bcctx);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const branch = env.unit.gen(JmpNZero, env.bcctx, exit, label->dst(2));
  branch->setNext(body);
  env.join->push_back(branch);
  auto const bodyDec = env.unit.gen(DecRefNZ, env.bcctx,
                                    DecRefData{}, label->dst(1));
  body->push_back({bodyDec,
    env.unit.gen(Jmp, env.bcctx, env.join,
                 label->dst(0), label->dst(0), env.unit.cns(true))
  });
  retypeDests(label, &env.unit);
  env.array(exit);
  auto const release = env.unit.gen(DecRef, env.bcctx,
                                    DecRefData{}, label->dst(1));
  exit->push_back(release);
  env.finish(exit);

  optimizeRefcounts(env.unit);

  std::vector<IRInstruction*> bodyDecrefs;
  for (auto& inst : *body) {
    if (inst.is(DecRef, DecRefNZ)) bodyDecrefs.push_back(&inst);
  }

  /*
   * Expected after PRE (both entry IncRefs and both original decrements gone):
   *
   *     v = newVec(1)                 w = newVec(1)
   *          \ (v,v,false)         (w,w,false) /
   *     +-----> join(p,q,done) <---------------+
   *     |             /       \
   *     |        !done         done
   *     |          /             \
   *     | body: DecRef p      exit: newVec(1)
   *     +------ Jmp(p,p,true)
   *
   * The backedge has no IncRef to cancel, so it needs a compensating DecRef.
   * On the first iteration, removing the entry IncRef accounts for the removed
   * body decrement. Later iterations and the exit are balanced by the backedge
   * compensation. This does not require proving the body executes exactly once.
   * The compensation can release and must not become DecRefNZ merely because
   * Jmp passes p to phis whose values are no longer consumed.
   */
  // TODO: Remove both entry IncRefs and both original decrements, inserting
  // a releasing DecRef on the backedge as shown above.
  for (auto const inc : incs) EXPECT_EQ(IncRef, inc->op());
  EXPECT_EQ(DecRefNZ, bodyDec->op());
  EXPECT_EQ(DecRef, release->op());
  ASSERT_EQ(1, bodyDecrefs.size());
  EXPECT_EQ(bodyDec, bodyDecrefs[0]);
}

/*
 *                   a = newVec(2)
 *                   /           \
 *                  a         b = newVec(2)
 *                   \           /
 *                      p = phi
 *                         |
 *                DecRef a; DecRef p
 *
 * Also test the reverse order. On the left path, a and p share two refs;
 * after canceling the extra increment, the last decrement must still release.
 */
TEST(RefcountOpts, PreserveLivePhiSource) {
  for (auto const reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    PhiEnv env;
    auto const source = env.array(env.unit.entry());
    auto const sourceInc = env.unit.gen(IncRef, env.bcctx, source);
    env.unit.entry()->insert(env.unit.entry()->backIter(), sourceInc);
    auto const other = env.array(env.right);
    auto const otherInc = env.unit.gen(IncRef, env.bcctx, other);
    env.right->push_back(otherInc);
    auto const result = env.phi(source, other);
    auto const first = reverse ? result : source;
    auto const last = reverse ? source : result;
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, first);
    env.join->push_back(dec);
    auto const release = env.unit.gen(DecRef, env.bcctx, DecRefData{}, last);
    env.join->push_back(release);
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Forward order cancels sourceInc with the first decrement. Reverse order
    // cancels both original increments with the first decrement.
    // Keep the last decrement releasing in both orders: on the left path,
    // source and result are the same array and the two decrements consume both
    // references. Changing alias sets at the phi must not hide that consumption.
    // TODO: Cancel both pairs with a releasing compensation on the left edge.
    EXPECT_EQ(Nop, sourceInc->op());
    EXPECT_EQ(reverse ? Nop : IncRef, otherInc->op());
    EXPECT_EQ(Nop, dec->op());
    EXPECT_EQ(DecRef, release->op());
    auto releases = 0;
    for (auto const& inst : *env.left) {
      if (!inst.is(DecRef, DecRefNZ)) continue;
      EXPECT_EQ(DecRef, inst.op());
      EXPECT_EQ(source, inst.src(0));
      ++releases;
    }
    EXPECT_EQ(0, releases);
    // A retained final DecRef source in reverse order needs a compensating
    // IncRef source on the right edge, where the phi refers to other.
    auto compensations = 0;
    for (auto const& inst : *env.right) {
      if (!inst.is(IncRef) || &inst == otherInc) continue;
      EXPECT_EQ(source, inst.src(0));
      ++compensations;
    }
    EXPECT_EQ(reverse ? 1 : 0, compensations);
  }
}

/*
 *     a = newVec(2)             b = newVec(2)
 *                 \             /
 *                    p = phi
 *                       |
 *                  c = newVec(1)
 *                    DecRef c
 *                    DecRef p
 *
 * Also test a == b from a common dominator (same alias set). Releasing c must
 * not discard the supported references transferred to p.
 */
TEST(RefcountOpts, TransferSupportedReferencesThroughPhi) {
  for (auto const same : {false, true}) {
    SCOPED_TRACE(same);
    PhiEnv env;
    auto const left = env.array(same ? env.unit.entry() : env.left, 2);
    auto const right = same ? left : env.array(env.right, 2);
    auto const result = env.phi(left, right);
    auto const other = env.array(env.join);
    env.join->push_back(env.unit.gen(DecRef, env.bcctx, DecRefData{}, other));
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
    env.join->push_back(dec);
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Cancel p's decrement with the incoming increments for both same and
    // distinct alias sets. Releasing the unrelated c cannot consume either
    // of p's two supported references.
    EXPECT_EQ(Nop, dec->op());
  }
}

/*
 *     a = newVec(2)             b = newVec(2)
 *              \ (a,a)       (b,b or c) /
 *                     (p,q) = phi
 *                          |
 *                       DecRef p
 *                       DecRef q
 *
 * With identical columns, p and q share two references. In the control,
 * c is a separate allocation with one reference on the second incoming edge.
 */
TEST(RefcountOpts, DoNotDuplicatePhiSupport) {
  for (auto const distinct : {false, true}) {
    SCOPED_TRACE(distinct);
    PhiEnv env;
    auto const left = env.array(env.left, 2);
    auto const right = env.array(env.right, 2);
    env.left->push_back(env.unit.gen(Jmp, env.bcctx, env.join, left, left));
    auto const other = distinct ? env.array(env.right) : right;
    env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join, right, other));
    auto const label = env.unit.defLabel(2, env.join, env.bcctx);
    retypeDests(label, &env.unit);
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(0));
    env.join->push_back(dec);
    auto const release = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(1));
    env.join->push_back(release);
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Cancel the first decrement against the incoming increments.
    // Keep the last decrement releasing. If p == q, consuming p leaves one
    // reference for q. On the distinct b/c edge, q owns c's only reference.
    // The identical-column optimization must not hide the first-edge aliasing
    // when the columns differ on the other edge.
    EXPECT_EQ(Nop, dec->op());
    EXPECT_EQ(DecRef, release->op());
  }
}

/*
 *      a = newVec(3), b = newVec(1)   c = newVec(3), d = newVec(1)
 *                         \ (a,b)   (c,d) /
 *                          join(p,q) <-----+
 *                            /    \        |
 *                     DecRef p     Jmp(q,p)+
 *                     DecRef q
 *
 * Either destination can hold the single-reference array after a backedge.
 */
TEST(RefcountOpts, SwapFiniteLoopPhiBounds) {
  PhiEnv env;
  for (auto const block : {env.left, env.right}) {
    auto const left = env.array(block, 3);
    auto const right = env.array(block);
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join, left, right));
  }
  auto const label = env.unit.defLabel(2, env.join, env.bcctx);
  retypeDests(label, &env.unit);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const branch = env.unit.gen(JmpZero, env.bcctx, exit, cond->dst());
  branch->setNext(body);
  env.join->push_back({cond, branch});
  body->push_back(env.unit.gen(Jmp, env.bcctx, env.join,
                               label->dst(1), label->dst(0)));
  auto const left = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(0));
  auto const right = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(1));
  exit->push_back({left, right});
  env.finish(exit);

  optimizeRefcounts(env.unit);

  // Expected: finish analysis and keep both exit decrements releasing.
  // After an unknown number of swaps, either phi may contain the allocation
  // with just one reference; neither old destination bound can survive a swap.
  EXPECT_EQ(DecRef, left->op());
  EXPECT_EQ(DecRef, right->op());
}

/*
 *     a = LdLoc 0: Vec             b = LdLoc 0: Vec
 *     s = AssertType<StaticVec> a          |
 *                \ (a,s)           (b,b) /
 *                       join(p,q)
 *                        IncRef p
 *                        DecRef p
 *                        CountVec q
 *
 * Swap the two left inputs as well. Canonicalizing the refinement makes the
 * phi columns identical, but only the unrefined input has an alias-set ID.
 */
TEST(RefcountOpts, PreserveSupportForRefinedPhiInputs) {
  for (auto const reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const left = env.unit.gen(LdLoc, env.bcctx, TVec, LocalId{0}, fp);
    auto const refined = env.unit.gen(AssertType, env.bcctx, TStaticVec,
                                      left->dst());
    auto const right = env.unit.gen(LdLoc, env.bcctx, TVec, LocalId{0}, fp);
    env.left->push_back({left, refined,
      env.unit.gen(Jmp, env.bcctx, env.join,
                   reverse ? refined->dst() : left->dst(),
                   reverse ? left->dst() : refined->dst())});
    env.right->push_back({right,
      env.unit.gen(Jmp, env.bcctx, env.join, right->dst(), right->dst())});
    auto const label = env.unit.defLabel(2, env.join, env.bcctx);
    retypeDests(label, &env.unit);
    auto const inc = env.unit.gen(IncRef, env.bcctx, label->dst(0));
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(0));
    env.join->push_back({inc, dec,
      env.unit.gen(CountVec, env.bcctx, label->dst(1))});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // The local keeps the borrowed input live, so cancel the temporary pair.
    // Coalescing the phi columns must keep memory support and its reverse map
    // consistent even when one input has an uncounted type refinement.
    EXPECT_EQ(Nop, inc->op());
    EXPECT_EQ(Nop, dec->op());
  }
}

/*
 *           a = LdLoc 0             b = LdLoc 0
 *             IncRef a                IncRef b
 *                     \              /
 *                        p = phi
 *                           |
 *                        r = LdLoc 0
 *                        StLoc 0, null
 *                        DecRef r
 *                        DecRef p
 *
 * The local's reference supports only one alias set. Reloading and consuming
 * it must not leave a second copy of that support on the phi destination.
 */
TEST(RefcountOpts, TransferPhiMemorySupport) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const left = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const right = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  env.left->push_back({left, env.unit.gen(IncRef, env.bcctx, left->dst())});
  env.right->push_back({right, env.unit.gen(IncRef, env.bcctx, right->dst())});
  auto const result = env.phi(left->dst(), right->dst());
  auto const reload = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  env.join->push_back({reload,
    env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(TInitNull)),
    env.unit.gen(DecRef, env.bcctx, DecRefData{}, reload->dst())});
  auto const release = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
  env.join->push_back(release);
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Keep the last decrement releasing. Reloading the local exposes the same
  // reference whose support was moved to p. After clearing the local and
  // consuming the reload, only the extra reference remains for p.
  EXPECT_EQ(DecRef, release->op());
}

/*
 *       a = LdLoc 0                 b = LdLoc 1; c = LdLoc 0
 *          (a, a)                          (b, c)
 *                  \                    /
 *                    (p, q) = phi
 *                       IncRef q
 *                          |
 *                         Jmp
 *                          |
 *                DecRefNZ (LdLoc 2)
 *                       DecRef q
 *
 * Only q has local 0 on every incoming edge. Vary column order and traversal
 * order: p must not take the left edge's memory credit away from q.
 */
TEST(RefcountOpts, SharePhiMemorySupportAcrossColumns) {
  for (auto const reverse : {false, true}) {
    for (auto const first : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "reverse=" << reverse
                   << " first=" << first);
      PhiEnv env{reverse};
      auto const fp = env.unit.entry()->front().dst();
      auto const other = env.unit.gen(LdLoc, env.bcctx, TCountedVec,
                                       LocalId{2}, fp);
      env.unit.entry()->insert(env.unit.entry()->backIter(), other);
      auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{1}, fp);
      auto const c = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      env.left->push_back({a,
        env.unit.gen(Jmp, env.bcctx, env.join, a->dst(), a->dst())});
      env.right->push_back({b, c,
        env.unit.gen(Jmp, env.bcctx, env.join,
                     first ? c->dst() : b->dst(),
                     first ? b->dst() : c->dst())});
      auto const phi = env.unit.defLabel(2, env.join, env.bcctx);
      retypeDests(phi, &env.unit);
      auto const q = phi->dst(first ? 0 : 1);
      auto const inc = env.unit.gen(IncRef, env.bcctx, q);
      auto const body = env.unit.defBlock();
      env.join->push_back({inc, env.unit.gen(Jmp, env.bcctx, body)});
      auto const otherDec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{},
                                         other->dst());
      auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, q);
      body->push_back({otherDec, dec});
      env.finish(body);

      optimizeRefcounts(env.unit);

      // Cancel q's pair in either column. Local 0 supplies independent credit
      // from local 2, even when their values alias. Its support must survive
      // the phi merge instead of becoming unsupported credit spent by otherDec.
      // TODO: Preserve local 0 support when q is not the first phi column.
      EXPECT_EQ(first ? Nop : IncRef, inc->op());
      EXPECT_EQ(first ? Nop : DecRef, dec->op());
      EXPECT_EQ(DecRefNZ, otherDec->op());
      EXPECT_TRUE(checkCfg(env.unit));
    }
  }
}

/*
 *               a = LdLoc 0; b = LdLoc 1
 *                        Jmp(a, a)
 *                            |
 *                   +--> (p, q) = phi
 *                   |       /     \
 *                   |   continue  exit
 *                   +--Jmp(q, p)   |
 *                               IncRef q
 *                                  |
 *                                 Jmp
 *                                  |
 *                              DecRefNZ b
 *                               DecRef q
 *
 * Both phis can use the same local credit across an arbitrary number of swaps.
 * No owner is selected until a reference-count operation uses that credit.
 */
TEST(RefcountOpts, ShareSupportAcrossSwappingLoopPhis) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const a = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
  auto const b = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{1}, fp->dst());
  auto const loop = unit.defBlock();
  auto const body = unit.defBlock();
  auto const exit = unit.defBlock();
  auto const tail = unit.defBlock();
  unit.entry()->push_back({fp, a, b,
    unit.gen(Jmp, bcctx, loop, a->dst(), a->dst())});
  auto const phi = unit.defLabel(2, loop, bcctx);
  retypeDests(phi, &unit);
  auto const cond = unit.gen(Conjure, bcctx, TBool);
  auto const branch = unit.gen(JmpZero, bcctx, exit, cond->dst());
  branch->setNext(body);
  loop->push_back({cond, branch});
  body->push_back(unit.gen(Jmp, bcctx, loop, phi->dst(1), phi->dst(0)));
  retypeDests(phi, &unit);
  auto const inc = unit.gen(IncRef, bcctx, phi->dst(1));
  exit->push_back({inc, unit.gen(Jmp, bcctx, tail)});
  auto const otherDec = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
  auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, phi->dst(1));
  tail->push_back({otherDec, dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});

  optimizeRefcounts(unit);

  // The fixed point retains local 0 for both phis, so cancel q's pair. Swapping
  // names must neither duplicate the reference nor alternate ownership forever.
  // TODO: Retain shared support across the swap instead of losing both names.
  EXPECT_EQ(IncRef, inc->op());
  EXPECT_EQ(DecRef, dec->op());
  EXPECT_EQ(DecRefNZ, otherDec->op());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *       a = LdLoc 0                    b = LdLoc 0
 *         IncRef a                      IncRef b
 *      CheckArrayCOW a               CheckArrayCOW b
 *       /         \                   /         \
 *     copy    unique: exit           copy    unique: exit
 *       |                             |
 *   DecRefNZ a                    DecRefNZ b
 *          (a, a)                       (b, b)
 *                  \                   /
 *                      (p, q) = phi
 *                  [DecRefNZ other]
 *                       DecRefNZ p
 *                           |
 *                          Jmp
 *                           |
 *                        IncRef q
 *
 * The COW observations preserve the incoming increments. After the first
 * decrements, p and q share the local's remaining credit. Consuming it through
 * p reduces q's bound to zero, but p's NZ decrement proves a reference remains.
 */
TEST(RefcountOpts, UseClaimantCreditForDecIncPRE) {
  for (auto const pending : {false, true}) {
    SCOPED_TRACE(pending);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const other = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    env.unit.entry()->insert(env.unit.entry()->backIter(), other);
    std::vector<IRInstruction*> increments;
    std::vector<IRInstruction*> decrements;
    for (auto const block : {env.left, env.right}) {
      auto const load = env.unit.gen(LdLoc, env.bcctx, TCountedVec,
                                      LocalId{0}, fp);
      auto const inc = env.unit.gen(IncRef, env.bcctx, load->dst());
      auto const copy = env.unit.defBlock();
      auto const unique = env.unit.defBlock();
      auto const after = env.unit.defBlock();
      auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, copy, load->dst());
      cow->setNext(unique);
      block->push_back({load, inc, cow});
      copy->push_back(env.unit.gen(Jmp, env.bcctx, after));
      env.finish(unique);
      auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, load->dst());
      after->push_back({dec, env.unit.gen(Jmp, env.bcctx, env.join,
                                          load->dst(), load->dst())});
      increments.push_back(inc);
      decrements.push_back(dec);
    }
    auto const phi = env.unit.defLabel(2, env.join, env.bcctx);
    retypeDests(phi, &env.unit);
    if (pending) {
      env.join->push_back(env.unit.gen(DecRefNZ, env.bcctx, DecRefData{},
                                       other->dst()));
    }
    auto const consume = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{},
                                       phi->dst(0));
    auto const exit = env.unit.defBlock();
    env.join->push_back({consume, env.unit.gen(Jmp, env.bcctx, exit)});
    auto const restore = env.unit.gen(IncRef, env.bcctx, phi->dst(1));
    exit->push_back(restore);
    env.finish(exit);

    optimizeRefcounts(env.unit);

    // Without pending reductions, cancel the incoming decrements against
    // IncRef q. Keeping those references across DecRefNZ p is safe, and its
    // post-decrement bound satisfies q's requirement when p and q alias.
    // With an earlier pending reduction, keep its unconditional requirement:
    // the current claimant's credit cannot discharge that older obligation.
    // TODO: Recover cancellation when pending is false. Deferring p's
    // balanced decrement now also blocks that previously valid optimization.
    for (auto const inc : increments) EXPECT_EQ(IncRef, inc->op());
    for (auto const dec : decrements) {
      EXPECT_EQ(DecRefNZ, dec->op());
    }
    EXPECT_EQ(DecRefNZ, consume->op());
    EXPECT_EQ(IncRef, restore->op());
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *            a = LdLoc 0; b = LdLoc 0
 *                     IncRef a
 *                   StLoc 0, null
 *                        |
 *                       Jmp
 *                        |
 *                    DecRef[NZ] b
 *                     CountVec a
 *                      DecRef a
 *
 * Clearing the local transfers its ownership to b; a's increment owns the
 * other reference. Both names must not independently spend the local's credit.
 * Also clear the local before IncRef a, while both sets still share support.
 */
TEST(RefcountOpts, ConsumeSharedMemorySupportOnce) {
  for (auto const clearFirst : {false, true}) {
    for (auto const nz : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "clearFirst=" << clearFirst
                   << " nz=" << nz);
      IRUnit unit{test_context};
      BCContext const bcctx{BCMarker::Dummy(), 0};
      auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
      auto const a = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
      auto const b = unit.gen(LdLoc, bcctx, TCountedVec, LocalId{0}, fp->dst());
      auto const inc = unit.gen(IncRef, bcctx, a->dst());
      auto const body = unit.defBlock();
      auto const clear = unit.gen(StLoc, bcctx, LocalId{0}, fp->dst(),
                                    unit.cns(TInitNull));
      unit.entry()->push_back({fp, a, b, clearFirst ? clear : inc,
        clearFirst ? inc : clear, unit.gen(Jmp, bcctx, body)});
      auto const first = unit.gen(nz ? DecRefNZ : DecRef, bcctx, DecRefData{},
                                   b->dst());
      auto const last = unit.gen(DecRef, bcctx, DecRefData{}, a->dst());
      body->push_back({first, unit.gen(CountVec, bcctx, a->dst()), last,
        unit.gen(Unreachable, bcctx, ASSERT_REASON)});

      optimizeRefcounts(unit);

      // Keep the final decrement releasing. If the first decrement remains,
      // IncRef a must also remain, or CountVec a can access a freed array.
      // TODO: Recognize the identical loads and cancel IncRef a against the
      // first decrement of b, preserving the final releasing DecRef a.
      EXPECT_EQ(IncRef, inc->op());
      EXPECT_EQ(nz ? DecRefNZ : DecRef, first->op());
      EXPECT_EQ(DecRef, last->op());
    }
  }
}

/*
 *        s: CountedStr             u: Uncounted
 *          IncRef s                     |
 *                  \                   /
 *                       p = phi
 *                          |
 *                       DecRef p
 *
 * Cancel the pair for each uncounted type and both branch traversal orders.
 */
TEST(RefcountOpts, CancelAcrossUncountedPhiInputs) {
  for (auto const type : {TInitNull, TInt, TPrimitive, TStaticStr, TStaticVec,
                         TSharedVec, TUncounted}) {
    SCOPED_TRACE(type.toString());
    for (auto const reverse : {false, true}) {
      SCOPED_TRACE(reverse);
      PhiEnv env{reverse};
      auto const value = env.unit.gen(Conjure, env.bcctx, TCountedStr);
      auto const inc = env.unit.gen(IncRef, env.bcctx, value->dst());
      env.left->push_back({value, inc});
      auto const other = env.unit.gen(Conjure, env.bcctx, type);
      env.right->push_back(other);
      auto const result = env.phi(value->dst(), other->dst());
      auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
      env.join->push_back(dec);
      env.finish(env.join);

      optimizeRefcounts(env.unit);

      // Expected: remove IncRef s and DecRef p for every type and branch order.
      // The counted path has a balanced pair; the uncounted path needs neither
      // operation and must not restrict the counted path's lower bound.
      EXPECT_EQ(Nop, inc->op());
      EXPECT_EQ(Nop, dec->op());
    }
  }
}

/*
 *          a = newVec(1)          s: StaticVec
 *                      \          /
 *                         p = phi
 *                            |
 *                         DecRef p
 *
 * The counted path still needs a release even though the static path is inert.
 */
TEST(RefcountOpts, PreserveReleasingDecrefAfterUncountedPhi) {
  PhiEnv env;
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  auto const owned = env.unit.gen(CopyArray, env.bcctx, original->dst());
  env.left->push_back({original, owned});
  auto const other = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  env.right->push_back(other);
  auto const result = env.phi(owned->dst(), other->dst());
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
  env.join->push_back(dec);
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Keep DecRef p: the counted path transfers its only reference to p.
  // The static input needs no release, but cannot justify weakening the
  // operation on the counted path.
  EXPECT_EQ(DecRef, dec->op());
}

/*
 *          a = newVec(1)          s: StaticVec
 *                      \          /
 *                         p = phi
 *                            |
 *                      CheckArrayCOW p
 *                        /        \
 *                      copy      unique
 *
 * A unique counted array and a static array need different COW outcomes.
 */
TEST(RefcountOpts, PreserveCOWCheckAfterStaticPhiInput) {
  PhiEnv env;
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  auto const owned = env.unit.gen(CopyArray, env.bcctx, original->dst());
  env.left->push_back({original, owned});
  auto const other = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  env.right->push_back(other);
  auto const result = env.phi(owned->dst(), other->dst());
  auto const copy = env.unit.defBlock();
  auto const unique = env.unit.defBlock();
  auto const check = env.unit.gen(CheckArrayCOW, env.bcctx, copy, result);
  check->setNext(unique);
  env.join->push_back(check);
  env.finish(copy);
  env.finish(unique);

  optimizeRefcounts(env.unit);

  // Keep CheckArrayCOW. The counted path is unique, but the static path must
  // copy before a write. An infinite uncounted lower bound does not establish
  // a finite upper bound of one.
  EXPECT_EQ(CheckArrayCOW, check->op());
}

/*
 *                   null             null
 *                      \             /
 *                       join(p) <----------------------+
 *                        /   \                         |
 *                 DecRef p    choose                   |
 *                             /    \                   |
 *                      Jmp(p)       DecRef p twice     |
 *                         |         a = newVec(2)      |
 *                         |         Jmp(a)             |
 *                         +-----------+----------------+
 *
 * Both the carry and replacement paths return to join. Null contributes no
 * finite lower-bound limit; a replacement contributes two references.
 */
TEST(RefcountOpts, CarryUncountedPhiOverBackedge) {
  PhiEnv env;
  auto const nil = env.unit.cns(TInitNull);
  auto const result = env.phi(nil, nil);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
  auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const branch = env.unit.gen(JmpZero, env.bcctx, exit, cond->dst());
  branch->setNext(body);
  env.join->push_back({cond, branch});
  exit->push_back(dec);
  env.finish(exit);
  auto const carry = env.unit.defBlock();
  auto const replace = env.unit.defBlock();
  auto const replaceCond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const choose = env.unit.gen(JmpZero, env.bcctx, replace, replaceCond->dst());
  choose->setNext(carry);
  body->push_back({replaceCond, choose});
  carry->push_back(env.unit.gen(Jmp, env.bcctx, env.join, result));
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  auto const owned = env.unit.gen(CopyArray, env.bcctx, original->dst());
  replace->push_back({
    env.unit.gen(DecRef, env.bcctx, DecRefData{}, result),
    env.unit.gen(DecRef, env.bcctx, DecRefData{}, result),
    original, owned, env.unit.gen(IncRef, env.bcctx, owned->dst()),
    env.unit.gen(Jmp, env.bcctx, env.join, owned->dst())
  });
  retypeDests(result->inst(), &env.unit);

  optimizeRefcounts(env.unit);

  // Every counted input has two references, including after arbitrarily many
  // carry edges; null must not reduce that lower bound. The exit decrement
  // can therefore use DecRefNZ.
  // TODO: Cancel the replacement IncRef against the exit decrement or the
  // first decrement on the next replacement. Keep the second replacement
  // decrement releasing; carry iterations consume no references.
  EXPECT_EQ(DecRefNZ, dec->op());
}

/*
 *         a = newVec(1)                    null
 *            \ (a,null)               (null,null) /
 *                        join(p,q) <------+
 *                         /     \        |
 *                   DecRef p     Jmp(q,p)+
 *                   DecRef q
 *
 * Swapping a counted value and null must not turn a finite bound into infinity.
 */
TEST(RefcountOpts, SwapCountedAndUncountedLoopPhis) {
  PhiEnv env;
  auto const nil = env.unit.cns(TInitNull);
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  auto const owned = env.unit.gen(CopyArray, env.bcctx, original->dst());
  env.left->push_back({original, owned,
    env.unit.gen(Jmp, env.bcctx, env.join, owned->dst(), nil)});
  env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join, nil, nil));
  auto const label = env.unit.defLabel(2, env.join, env.bcctx);
  auto const left = label->dst(0);
  auto const right = label->dst(1);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const branch = env.unit.gen(JmpZero, env.bcctx, exit, cond->dst());
  branch->setNext(body);
  env.join->push_back({cond, branch});
  body->push_back(env.unit.gen(Jmp, env.bcctx, env.join, right, left));
  retypeDests(label, &env.unit);
  retypeDests(label, &env.unit);
  auto const releaseLeft = env.unit.gen(DecRef, env.bcctx, DecRefData{}, left);
  auto const releaseRight = env.unit.gen(DecRef, env.bcctx, DecRefData{}, right);
  exit->push_back({releaseLeft, releaseRight});
  env.finish(exit);

  optimizeRefcounts(env.unit);

  // Keep both exit DecRefs. Either phi can hold the counted allocation's
  // only reference after a swap, even if its previous value was uncounted.
  // Infinity belongs to the incoming value, not permanently to a destination.
  EXPECT_EQ(DecRef, releaseLeft->op());
  EXPECT_EQ(DecRef, releaseRight->op());
}

/*
 *                 null             null
 *                    \             /
 *                      join(p) <----------+
 *                         |               |
 *                      IncRef p           |
 *                      DecRef p           |
 *                      DecRef p           |
 *                       /    \            |
 *                     exit    a = newVec(1)|
 *                             Jmp(a) -----+
 *
 * The pair cancels, but the third instruction must release each counted input.
 */
TEST(RefcountOpts, InitiallyUncountedLoopPhi) {
  PhiEnv env;
  auto const result = env.phi(env.unit.cns(TInitNull), env.unit.cns(TInitNull));
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const inc = env.unit.gen(IncRef, env.bcctx, result);
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
  auto const release = env.unit.gen(DecRef, env.bcctx, DecRefData{}, result);
  auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const branch = env.unit.gen(JmpZero, env.bcctx, exit, cond->dst());
  branch->setNext(body);
  env.join->push_back({inc, dec, release, cond, branch});
  env.finish(exit);
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticVec);
  auto const owned = env.unit.gen(CopyArray, env.bcctx, original->dst());
  body->push_back({original, owned,
                  env.unit.gen(Jmp, env.bcctx, env.join, owned->dst())});
  retypeDests(result->inst(), &env.unit);

  optimizeRefcounts(env.unit);

  // Remove the adjacent IncRef/DecRef pair, and keep the third instruction
  // as a releasing DecRef. Initial nulls do not change the fact that each
  // counted backedge input contributes only one reference.
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
  EXPECT_EQ(DecRef, release->op());
}

/*
 *      c = newDict(1); IncRef c
 *      a: CountedVec|CountedDict; IncRef a
 *      b: CountedVec or CountedVec|CountedDict
 *                    |
 *             CheckArrayCOW a
 *                /       \
 *                \       /
 *                DecRefNZ b
 *                DecRefNZ a
 *                DecRef[NZ] c
 *
 * With b:Vec, b may alias a and a may alias c, but b cannot alias c.
 * The wider b type is a control where both decrements may reach c.
 */
TEST(RefcountOpts, NontransitiveMayAliasDecrefs) {
  for (auto const overlap : {false, true}) {
    for (auto const nz : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "overlap=" << overlap << " nz=" << nz);
      IRUnit unit{test_context};
      BCContext const bcctx{BCMarker::Dummy(), 0};
      auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
      auto const original = unit.gen(Conjure, bcctx, TStaticDict);
      auto const c = unit.gen(CopyArray, bcctx, original->dst());
      auto const incC = unit.gen(IncRef, bcctx, c->dst());
      auto const a = unit.gen(Conjure, bcctx, TCountedVec | TCountedDict);
      auto const b = unit.gen(Conjure, bcctx,
                              overlap ? TCountedVec | TCountedDict : TCountedVec);
      auto const incA = unit.gen(IncRef, bcctx, a->dst());
      auto const copy = unit.defBlock();
      auto const unique = unit.defBlock();
      auto const body = unit.defBlock();
      auto const cow = unit.gen(CheckArrayCOW, bcctx, copy, a->dst());
      cow->setNext(unique);
      unit.entry()->push_back({fp, original, c, incC, a, b, incA, cow});
      copy->push_back(unit.gen(Jmp, bcctx, body));
      unique->push_back(unit.gen(Jmp, bcctx, body));
      auto const decB = unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst());
      auto const decA = unit.gen(DecRefNZ, bcctx, DecRefData{}, a->dst());
      auto const decC = unit.gen(nz ? DecRefNZ : DecRef, bcctx, DecRefData{}, c->dst());
      body->push_back({decB, decA, decC,
                      unit.gen(Unreachable, bcctx, ASSERT_REASON)});
      optimizeRefcounts(unit);

      // Expected: fold the shared COW decision, then cancel a's pair.
      // The folded branch stays shared after removing its proof.
      // Cancel c's pair without changing NZs when b cannot alias c or the
      // final decrement is NZ. Otherwise cancellation can make b release.
      // TODO: Also cancel c's overlapping ordinary-final pair by changing
      // the single remaining DecRefNZ b to DecRef.
      // TODO: Preserve c's supported references when b cannot alias c.
      // TODO: Revisit the observed pairs after folding the COW checks.
      EXPECT_EQ(IncRef, incA->op());
      EXPECT_EQ(DecRefNZ, decA->op());
      EXPECT_EQ(nz ? Nop : IncRef, incC->op());
      EXPECT_EQ(nz ? Nop : DecRef, decC->op());
      EXPECT_EQ(CheckArrayCOW, cow->op());
    }
  }
}

/*
 *                 c = newDict(1); IncRef c
 *           a: CountedVec|CountedDict|CountedKeyset
 *                        IncRef a
 *                     CheckArrayCOW a
 *                unique       shared
 *                 exit           cond
 *                      /        \
 *              DecRefNZ b      DecRefNZ d
 *               b:Vec       d:Keyset or Dict
 *                      \        /
 *                       DecRefNZ a
 *                        DecRef c
 *
 * Vec and Keyset decrements cannot reach c through a. The Dict alternative
 * must preserve that possibility when merging the pending reductions.
 */
TEST(RefcountOpts, MergeConditionalPendingDecrefTypes) {
  for (auto const overlap : {false, true}) {
    for (auto const reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "overlap=" << overlap
                                       << " reverse=" << reverse);
      IRUnit unit{test_context};
      BCContext const bcctx{BCMarker::Dummy(), 0};
      auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
      auto const original = unit.gen(Conjure, bcctx, TStaticDict);
      auto const c = unit.gen(CopyArray, bcctx, original->dst());
      auto const incC = unit.gen(IncRef, bcctx, c->dst());
      auto const a = unit.gen(Conjure, bcctx,
                              TCountedVec | TCountedDict | TCountedKeyset);
      auto const b = unit.gen(Conjure, bcctx, TCountedVec);
      auto const d = unit.gen(Conjure, bcctx,
                              overlap ? TCountedDict : TCountedKeyset);
      auto const cond = unit.gen(Conjure, bcctx, TBool);
      auto const incA = unit.gen(IncRef, bcctx, a->dst());
      auto const copy = unit.defBlock();
      auto const unique = unit.defBlock();
      auto const choose = unit.defBlock();
      auto const left = unit.defBlock();
      auto const right = unit.defBlock();
      auto const join = unit.defBlock();
      auto const cow = unit.gen(CheckArrayCOW, bcctx, copy, a->dst());
      cow->setNext(unique);
      unit.entry()->push_back({fp, original, c, incC, a, b, d, cond, incA, cow});
      copy->push_back(unit.gen(Jmp, bcctx, choose));
      unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
      auto const branch = unit.gen(JmpZero, bcctx,
                                   reverse ? left : right, cond->dst());
      branch->setNext(reverse ? right : left);
      choose->push_back(branch);
      left->push_back({unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()),
                       unit.gen(Jmp, bcctx, join)});
      right->push_back({unit.gen(DecRefNZ, bcctx, DecRefData{}, d->dst()),
                        unit.gen(Jmp, bcctx, join)});
      auto const decA = unit.gen(DecRefNZ, bcctx, DecRefData{}, a->dst());
      auto const decC = unit.gen(DecRef, bcctx, DecRefData{}, c->dst());
      join->push_back({decA, decC, unit.gen(Unreachable, bcctx, ASSERT_REASON)});

      optimizeRefcounts(unit);

      // Expected: fold the shared COW decision, then cancel a's pair.
      // Cancel c's pair without changing NZs when neither branch can alias c.
      // On the Dict edge with d == a == c, DecRef c can release.
      // TODO: Also cancel c's overlapping pair by changing the Dict-edge
      // DecRefNZ d to DecRef. The Vec-edge decrement stays NZ.
      // TODO: Preserve c's credit when both incoming types are disjoint.
      // TODO: Revisit the observed pairs after folding the COW checks.
      EXPECT_EQ(IncRef, incA->op());
      EXPECT_EQ(DecRefNZ, decA->op());
      EXPECT_EQ(IncRef, incC->op());
      EXPECT_EQ(DecRef, decC->op());
      EXPECT_TRUE(checkCfg(unit));
    }
  }
}

/*
 *      x = newDict(1), a: CountedVec|CountedDict, b: CountedVec
 *                       /             \
 *                    IncRef a        (nothing)
 *                       \             /
 *                          DecRef[NZ] b
 *                          CheckArrayCOW x
 *                           /       \
 *                   StLoc 0,true   StLoc 0,false
 *                           \       /
 *                           DecRefNZ a
 *
 * b cannot alias x, but a can. Removing a's pair would change the COW result
 * when a == x, even though the merged lower bound for a is zero.
 */
TEST(RefcountOpts, PreserveCOWObservationAfterZeroBoundDecref) {
  for (auto const nz : {false, true}) {
    SCOPED_TRACE(nz);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const original = env.unit.gen(Conjure, env.bcctx, TStaticDict);
    auto const x = env.unit.gen(CopyArray, env.bcctx, original->dst());
    auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec | TCountedDict);
    auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    for (auto inst : {original, x, a, b}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
    env.left->push_back({inc, env.unit.gen(Jmp, env.bcctx, env.join)});
    env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
    auto const copy = env.unit.defBlock();
    auto const unique = env.unit.defBlock();
    auto const exit = env.unit.defBlock();
    auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, copy, x->dst());
    cow->setNext(unique);
    env.join->push_back({
      env.unit.gen(nz ? DecRefNZ : DecRef, env.bcctx, DecRefData{}, b->dst()), cow
    });
    copy->push_back({
      env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(true)),
      env.unit.gen(Jmp, env.bcctx, exit)
    });
    unique->push_back({
      env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(false)),
      env.unit.gen(Jmp, env.bcctx, exit)
    });
    auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
    exit->push_back(dec);
    env.finish(exit);
    optimizeRefcounts(env.unit);

    // Keep a's increment, its decrement, and CheckArrayCOW x. If a == x,
    // the increment determines which value is stored. A zero lower bound after
    // merging does not make that increment unobservable.
    EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRefNZ, dec->op());
    EXPECT_EQ(CheckArrayCOW, cow->op());
  }
}

/*
 *              a,b,c: CountedVec
 *                      |
 *                   IncRef a
 *                      |
 *                     Jmp
 *                      |
 *                 DecRef[NZ] b
 *                [DecRef[NZ] c]
 *                 DecRef[NZ] a
 *
 * Vary one/two middle decrements and both middle/final opcodes. The final NZ
 * guarantee permits cancellation across NZ middles even when all values alias.
 */
TEST(RefcountOpts, CancelAcrossConsecutiveDecrefs) {
  for (auto const middleNZ : {false, true}) {
    for (auto const finalNZ : {false, true}) {
      for (auto const count : {1, 2}) {
        SCOPED_TRACE(::testing::Message() << "middleNZ=" << middleNZ
                     << " finalNZ=" << finalNZ << " count=" << count);
        IRUnit unit{test_context};
        BCContext const bcctx{BCMarker::Dummy(), 0};
        auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
        auto const a = unit.gen(Conjure, bcctx, TCountedVec);
        auto const b = unit.gen(Conjure, bcctx, TCountedVec);
        auto const c = unit.gen(Conjure, bcctx, TCountedVec);
        auto const inc = unit.gen(IncRef, bcctx, a->dst());
        auto const body = unit.defBlock();
        unit.entry()->push_back({fp, a, b, c, inc,
                                unit.gen(Jmp, bcctx, body)});
        for (auto const value : {b, c}) {
          if (value == c && count == 1) break;
          body->push_back(unit.gen(middleNZ ? DecRefNZ : DecRef, bcctx,
                                   DecRefData{}, value->dst()));
        }
        auto const dec = unit.gen(finalNZ ? DecRefNZ : DecRef, bcctx,
                                  DecRefData{}, a->dst());
        body->push_back({dec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});

        optimizeRefcounts(unit);

        // Expected: cancel a's pair when the final decrement is NZ. Its guarantee
        // leaves enough references for the middle NZ decrements even if a == b == c.
        // For ordinary middle decrements, this also requires knowing their effects
        // only decrement references. For NZ middles followed by an ordinary final
        // decrement, cancellation must allow any newly possible middle release.
        // TODO: With one NZ middle and an ordinary final decrement, cancel
        // a's pair and change the middle to DecRef. For multiple NZ middles,
        // weigh the added release checks against the pair being removed;
        // retaining the pair can be cheaper.
        // TODO: Cancel across releasing decrefs with only decrementing effects.
        // TODO: Also cancel when every decrement is ordinary; moving a release
        // earlier within a decrement-only sequence is safe.
        EXPECT_EQ(middleNZ && finalNZ ? Nop : IncRef, inc->op());
        EXPECT_EQ(middleNZ && finalNZ ? Nop : (finalNZ ? DecRefNZ : DecRef),
                  dec->op());
      }
    }
  }
}

/*
 *                 a,b: CountedVec
 *                   /           \
 *                IncRef a      (nothing)
 *                   \           /
 *                     DecRefNZ b
 *                     DecRefNZ a
 *
 * The merged bound for a is zero. PRE can remove a's pair and compensate on
 * the incoming edge without an increment; an eager requirement would block it.
 */
TEST(RefcountOpts, PartialCancellationAfterUnbalancedDecref) {
  PhiEnv env;
  auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
  auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
  for (auto inst : {a, b}) {
    env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
  }
  auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
  env.left->push_back({inc, env.unit.gen(Jmp, env.bcctx, env.join)});
  env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
  env.join->push_back({
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b->dst()), dec
  });
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Remove the original IncRef a and DecRefNZ a. PRE compensates on the
  // incoming edge that had no increment. The final NZ guarantee permits
  // cancellation on the other edge even if b aliases a; a zero merged bound
  // must not force an eager requirement at DecRefNZ b.
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
}

/*
 *                   a,b: CountedVec
 *                   /             \
 *               IncRef a          IncRef a
 *               DecRefNZ b        DecRefNZ b
 *                   \             /
 *                        IncRef d
 *                        DecRefNZ a
 *
 * With mergedZero, the right branch is empty. Vary d between CountedStr
 * (disjoint) and CountedVec (may alias a); only the disjoint case can cancel.
 */
TEST(RefcountOpts, PendingDecrefsBeforeIncRef) {
  for (auto const alias : {false, true}) {
    for (auto const mergedZero : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "alias=" << alias
                   << " mergedZero=" << mergedZero);
      PhiEnv env;
      auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
      auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
      auto const d = env.unit.gen(Conjure, env.bcctx,
                                  alias ? TCountedVec : TCountedStr);
      for (auto inst : {a, b, d}) {
        env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
      }
      auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
      env.left->push_back({inc,
        env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b->dst()),
        env.unit.gen(Jmp, env.bcctx, env.join)});
      if (!mergedZero) {
        env.right->push_back({
          env.unit.gen(IncRef, env.bcctx, a->dst()),
          env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b->dst())
        });
      }
      env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
      auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
      env.join->push_back({env.unit.gen(IncRef, env.bcctx, d->dst()), dec});
      env.finish(env.join);

      optimizeRefcounts(env.unit);

      // Expected: cancel a's pair only when d cannot alias a, in both merge cases.
      // An aliasing IncRef d may provide the reference that makes the final
      // DecRefNZ safe after DecRefNZ b. A disjoint increment cannot do that and
      // should not force observation of a's pending decrements.
      EXPECT_EQ(alias ? IncRef : Nop, inc->op());
      EXPECT_EQ(alias ? DecRefNZ : Nop, dec->op());
    }
  }
}


/*
 *                   a,b,d: CountedVec
 *                     IncRef d (x2)
 *                    /             \
 *                IncRef a        IncRef a
 *                DecRefNZ b      DecRefNZ b
 *                    \             /
 *                         IncRef d
 *                      DecRef{NZ} a
 *
 * d has a lower bound of two after applying its pending reduction from b.
 * a and b may refer to one array while d refers to a different array.
 */
TEST(RefcountOpts, PendingReductionsAcrossSharedInc) {
  for (auto const nz : {false, true}) {
    SCOPED_TRACE(nz);
    PhiEnv env;
    auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const d = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    for (auto inst : {a, b, d}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    for (auto i = 0; i < 2; ++i) {
      env.unit.entry()->insert(env.unit.entry()->backIter(),
        env.unit.gen(IncRef, env.bcctx, d->dst()));
    }
    std::vector<IRInstruction*> incs;
    std::vector<IRInstruction*> middles;
    for (auto block : {env.left, env.right}) {
      auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
      auto const middle = env.unit.gen(DecRefNZ, env.bcctx,
                                       DecRefData{}, b->dst());
      incs.push_back(inc);
      middles.push_back(middle);
      block->push_back({inc, middle,
        env.unit.gen(Jmp, env.bcctx, env.join)});
    }
    auto const dec = env.unit.gen(nz ? DecRefNZ : DecRef,
                                env.bcctx, DecRefData{}, a->dst());
    env.join->push_back({env.unit.gen(IncRef, env.bcctx, d->dst()), dec});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // With a == b != d and one initial reference to a, its final ordinary
    // decrement must release. Removing its pair would instead require b's NZ
    // decrement to release. The lower bound for d cannot prove a is shared.
    // With a final NZ decrement, cancel a's pair: d's bound keeps a alive
    // if they alias, and otherwise IncRef d cannot supply a's NZ guarantee.
    // The ordinary endpoint must still apply a's pending reduction before
    // using its bound; d's bound alone cannot justify removing a's release.
    // TODO: For an ordinary endpoint, cancellation could instead change b's
    // single NZ decrement on each path to DecRef to preserve the release.
    for (auto const middle : middles) EXPECT_EQ(DecRefNZ, middle->op());
    for (auto const inc : incs) EXPECT_EQ(nz ? Nop : IncRef, inc->op());
    EXPECT_EQ(nz ? Nop : DecRef, dec->op());
  }
}

/*
 *                   a: CountedVec, d
 *                       [IncRef d]
 *                   /             \
 *               IncRef a       [IncRef a]
 *                   \             /
 *                        IncRef d
 *                        DecRefNZ a
 *
 * Vary d between CountedVec (may alias a) and CountedStr (disjoint), and
 * omit the right increment for partial PRE. An optional entry IncRef d gives
 * d a lower bound of two before its final IncRef. There are no pending decrefs.
 */
TEST(RefcountOpts, AliasIncBeforePartialPREWithoutPendingDecrefs) {
  for (auto const alias : {false, true}) {
    for (auto const partial : {false, true}) {
      for (auto const entryInc : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "alias=" << alias
                     << " partial=" << partial << " entryInc=" << entryInc);
        PhiEnv env;
        auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
        auto const d = env.unit.gen(Conjure, env.bcctx,
                                    alias ? TCountedVec : TCountedStr);
        for (auto inst : {a, d}) {
          env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
        }
        if (entryInc) {
          env.unit.entry()->insert(env.unit.entry()->backIter(),
            env.unit.gen(IncRef, env.bcctx, d->dst()));
        }
        auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
        auto const otherInc = partial ? nullptr :
          env.unit.gen(IncRef, env.bcctx, a->dst());
        env.left->push_back({inc, env.unit.gen(Jmp, env.bcctx, env.join)});
        if (otherInc) env.right->push_back(otherInc);
        env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
        auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
        env.join->push_back({env.unit.gen(IncRef, env.bcctx, d->dst()), dec});
        env.finish(env.join);

        optimizeRefcounts(env.unit);

        // Keep the pair only for aliasing partial PRE without the entry IncRef d:
        // compensation could release a == d before the final IncRef d. The entry
        // increment gives d two references, so compensation cannot release it.
        // With increments on both branches, cancellation needs no compensation.
        // For disjoint values, IncRef d cannot supply a's final NZ guarantee.
        // TODO: In safe partial cases, insert DecRefNZ a on the empty edge: the
        // original NZ guarantee or d's entry reference rules out releasing.
        auto const blocked = alias && partial && !entryInc;
        EXPECT_EQ(blocked ? IncRef : Nop, inc->op());
        EXPECT_EQ(blocked ? DecRefNZ : Nop, dec->op());
        if (otherInc) {
          EXPECT_EQ(Nop, otherInc->op());
        } else if (blocked) {
          ASSERT_EQ(1, env.right->instrs().size());
          EXPECT_EQ(Jmp, env.right->front().op());
        } else {
          ASSERT_EQ(2, env.right->instrs().size());
          EXPECT_EQ(DecRef, env.right->front().op());
          EXPECT_EQ(a->dst(), env.right->front().src(0));
        }
      }
    }
  }
}

/*
 *              a: CountedVec, d
 *                 /       \
 *          DecRefNZ a    [DecRefNZ a]
 *                 \       /
 *                  IncRef d
 *                  IncRef a
 *
 * Vary d between CountedVec (may alias a) and CountedStr (disjoint), and
 * omit the right decrement for partial PRE. Loading a from local 0 optionally
 * supplies a lower bound of one even on the empty branch.
 */
TEST(RefcountOpts, DecIncPREAcrossAliasInc) {
  for (auto const alias : {false, true}) {
    for (auto const partial : {false, true}) {
      for (auto const knownBound : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "alias=" << alias
                     << " partial=" << partial << " knownBound=" << knownBound);
        PhiEnv env;
        auto const fp = env.unit.entry()->front().dst();
        auto const a = knownBound
          ? env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp)
          : env.unit.gen(Conjure, env.bcctx, TCountedVec);
        auto const d = env.unit.gen(Conjure, env.bcctx,
                                    alias ? TCountedVec : TCountedStr);
        for (auto inst : {a, d}) {
          env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
        }
        auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
        auto const otherDec = partial ? nullptr :
          env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
        env.left->push_back({dec, env.unit.gen(Jmp, env.bcctx, env.join)});
        if (otherDec) env.right->push_back(otherDec);
        env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
        auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
        auto const aliasInc = env.unit.gen(IncRef, env.bcctx, d->dst());
        env.join->push_back({aliasInc, inc});
        env.finish(env.join);

        optimizeRefcounts(env.unit);

        // Cancel the decrements with the final increment. With partial
        // availability, insert IncRef a on the empty edge. This raises the
        // count across IncRef d, so aliasing cannot cause a premature release.
        // Preserve this even when a's merged lower bound is zero: an IncRef
        // lifetime requirement must not block dec/inc PRE in that case.
        EXPECT_EQ(Nop, dec->op());
        EXPECT_EQ(Nop, inc->op());
        EXPECT_EQ(IncRef, aliasInc->op());
        if (otherDec) {
          EXPECT_EQ(Nop, otherDec->op());
        } else {
          ASSERT_EQ(2, env.right->instrs().size());
          EXPECT_EQ(IncRef, env.right->front().op());
          EXPECT_EQ(a->dst(), env.right->front().src(0));
        }
        EXPECT_TRUE(checkCfg(env.unit));
      }
    }
  }
}

/*
 *                    a = newVec(1)
 *                      /        \
 *                      \        /
 *                      loop <---------+
 *                        |            |
 *                    DecRefNZ b       |
 *                      /      \-------+
 *                 newVec(1)
 *                  DecRef a
 *
 * Each iteration has another possibly-aliasing decrement. The pending state
 * must converge without incorrectly proving that the exit cannot release.
 */
TEST(RefcountOpts, PendingDecrefsAcrossBackedge) {
  PhiEnv env;
  auto const value = env.array(env.unit.entry());
  for (auto block : {env.left, env.right}) {
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  }
  auto const alias = env.unit.gen(Conjure, env.bcctx, TCountedVec);
  auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
  auto const exit = env.unit.defBlock();
  auto const branch = env.unit.gen(JmpNZero, env.bcctx, env.join, cond->dst());
  branch->setNext(exit);
  env.join->push_back({alias,
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, alias->dst()), cond, branch});
  env.array(exit);
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, value);
  exit->push_back(dec);
  env.finish(exit);

  optimizeRefcounts(env.unit);

  // Keep the exit DecRef releasing and converge even after arbitrary loop
  // iterations. Each iteration may consume a reference to a, so deferred
  // reductions must not leave a false non-releasing lower bound.
  // TODO: Bound the pending-decref state across backedges.
  EXPECT_EQ(DecRef, dec->op());
}

/*
 *           b = newVec(2), c: owning CountedVec
 *                        a = LdLoc 0
 *                         DecRefNZ c
 *                        r = LdLoc 0
 *                          IncRef r
 *                         /        \
 *                         \        /
 *                         DecRefNZ b
 *                          DecRef r
 *
 * c may alias the local, but releases a separate reference. The local remains
 * balanced, so its reload is valid despite the pending reduction of a.
 */
TEST(RefcountOpts, ReloadAfterPendingAliasDecref) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const b = env.array(env.unit.entry(), 2);
  auto const c = env.unit.gen(Conjure, env.bcctx, TCountedVec);
  auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const reload = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const inc = env.unit.gen(IncRef, env.bcctx, reload->dst());
  for (auto inst : {c, a,
       env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, c->dst()), reload, inc}) {
    env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
  }
  for (auto block : {env.left, env.right}) {
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  }
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, reload->dst());
  env.join->push_back({
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b), dec
  });
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Expected: cancel r's pair. The balanced local supports r independently
  // of b's reference, so releasing one of b's references cannot invalidate r.
  // TODO: Do not apply a deferred reduction from before the reload to its
  // newly established memory support.
  EXPECT_EQ(IncRef, inc->op());
  EXPECT_EQ(DecRef, dec->op());
}

/*
 *                 b = LdLoc 0
 *                 [Jmp to another block]
 *                 a = LdLoc 0
 *                   IncRef b
 *                 DecRef[NZ] a
 *                 r = LdLoc 0
 *                 StLoc 0, null
 *                   IncRef r
 *                  /        \
 *                  \        /
 *                   DecRef b
 *                   CountVec r
 *                   DecRef r
 *
 * The first decrement releases b's temporary reference, not the local's.
 * The null store transfers the local's remaining ownership to b. Thus the
 * reload is valid, but b and r must not both get credit for that one reference.
 */
TEST(RefcountOpts, ReloadAfterReleasingTemporaryAlias) {
  for (auto const nz : {false, true}) {
    for (auto const split : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "nz=" << nz << " split=" << split);
      PhiEnv env;
      auto const fp = env.unit.entry()->front().dst();
      auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      auto const reload = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      auto const inc = env.unit.gen(IncRef, env.bcctx, reload->dst());
      env.unit.entry()->insert(env.unit.entry()->backIter(), b);
      auto prefix = env.unit.entry();
      if (split) {
        prefix = env.unit.defBlock();
        prefix->splice(prefix->end(), env.unit.entry(),
                       env.unit.entry()->backIter(), env.unit.entry()->end());
        env.unit.entry()->push_back(env.unit.gen(Jmp, env.bcctx, prefix));
      }
      for (auto inst : {a, env.unit.gen(IncRef, env.bcctx, b->dst()),
           env.unit.gen(nz ? DecRefNZ : DecRef, env.bcctx, DecRefData{}, a->dst()),
           reload,
           env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(TInitNull)),
           inc}) {
        prefix->insert(prefix->backIter(), inst);
      }
      for (auto block : {env.left, env.right}) {
        block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
      }
      auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, reload->dst());
      env.join->push_back({
        env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst()),
        env.unit.gen(CountVec, env.bcctx, reload->dst()), dec
      });
      env.finish(env.join);

      optimizeRefcounts(env.unit);

      // Keep r's increment and releasing decrement. When the local initially
      // owns the only reference, removing r's pair would release the array at
      // DecRef b before CountVec r. The final decrement must not become NZ.
      EXPECT_EQ(IncRef, inc->op());
      EXPECT_EQ(DecRef, dec->op());
    }
  }
}

/*
 *                    s = LdLoc 0
 *                    a = LdLoc 0
 *                      IncRef s
 *                     DecRefNZ a
 *                [InterpOne PopL local]
 *                    b = newVec(2)
 *                    r = LdLoc 0
 *                      IncRef r
 *                   CheckArrayCOW b
 *                     /        \
 *                     \        /
 *                     DecRefNZ b
 *                      DecRef r
 *
 * Vary local between 0, 1, and no store. PopL consumes a CountedVec already
 * owned by the VM stack. Neither s nor a is used after PopL; local 0 remains
 * balanced whether it was replaced or left alone.
 */
TEST(RefcountOpts, ReloadAfterGeneralEffectsStore) {
  auto const emitter = assemble_string(
    R"(.function N refcount_reload_store(N $a, N $b) {
  Null
  RetC None
})",
    "refcount-reload-store.php", SHA1{uint64_t{1}}, nullptr,
    RepoOptions::defaults().packageInfo(), UnitEmitterAttributes::defaults(),
    false
  );
  ASSERT_NE(nullptr, emitter);
  ASSERT_FALSE(emitter->m_fatalUnit);
  auto const bytecode = emitter->create();
  auto const func = bytecode->funcs()[0];
  for (auto const local : {-1, 0, 1}) {
    SCOPED_TRACE(::testing::Message() << "local=" << local);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    fp->inst()->marker() = BCMarker{
      SrcKey{func, 0, ResumeMode::None}, SBInvOffset{0}, false, TransIDSet{},
      nullptr, nullptr, nullptr
    };
    auto const s = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
    auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
    for (auto inst : {s, a, env.unit.gen(IncRef, env.bcctx, s->dst()),
         env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst())}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    auto prefix = env.unit.entry();
    prefix->back().convertToNop();
    if (local >= 0) {
      auto const sp = env.unit.gen(DefRegSP, env.bcctx,
                                   DefStackData{SBInvOffset{1}, SBInvOffset{1}});
      env.unit.entry()->insert(env.unit.entry()->backIter(), sp);
      BCContext const interpCtx{
        BCMarker{fp->inst()->marker().sk(), SBInvOffset{1}, false, TransIDSet{},
                 fp, fp, sp->dst()}, 0
      };
      InterpOneData::LocalType changed{uint32_t(local), TCountedVec};
      InterpOneData data{IRSPRelOffset{0}};
      data.bcOff = 0;
      data.cellsPopped = 1;
      data.cellsPushed = 0;
      data.opcode = Op::PopL;
      data.nChangedLocals = 1;
      data.changedLocals = &changed;
      auto const catchBlock = env.unit.defBlock();
      env.unit.defLabel(0, catchBlock, interpCtx);
      catchBlock->push_back(env.unit.gen(BeginCatch, interpCtx, TObj | TNullptr));
      env.finish(catchBlock);
      auto const store = env.unit.gen(InterpOne, interpCtx, data, catchBlock,
                                      sp->dst(), fp);
      prefix = env.unit.defBlock();
      store->setNext(prefix);
      env.unit.entry()->push_back(store);
    }
    auto const b = env.array(prefix, 2);
    auto const reload = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
    auto const inc = env.unit.gen(IncRef, env.bcctx, reload->dst());
    auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, env.left, b);
    cow->setNext(env.right);
    prefix->push_back({reload, inc, cow});
    for (auto block : {env.left, env.right}) {
      block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
    }
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, reload->dst());
    env.join->push_back({
      env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b), dec
    });
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Expected: fold b's shared COW decision and cancel both b's and r's pairs
    // in every case. The local retains its own reference. A may-store to local 0
    // clears the old support, accounting for possible decrefs of aliases, and
    // lets the reload acquire support.
    // TODO: Transfer support when the old set has no supported references,
    // including when a may-store touches only a different local.
    EXPECT_EQ(local == 0 ? Nop : IncRef, inc->op());
    EXPECT_EQ(local == 0 ? Nop : DecRef, dec->op());
  }
}

/*
 *                        a = LdLoc 0
 *                        d = LdLoc k
 *                      /           \
 *                   IncRef a      (empty)
 *                      \           /
 *                         IncRef d
 *                         DecRefNZ a
 *
 * For k = 1, the two locals independently own a reference each. For k = 0,
 * both loads describe the same reference; their lower bounds cannot be added.
 */
TEST(RefcountOpts, PartialPREAcrossAliasIncBounds) {
  for (auto const local : {0, 1}) {
    SCOPED_TRACE(local);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
    auto const d = env.unit.gen(LdLoc, env.bcctx, TCountedVec,
                                LocalId{uint32_t(local)}, fp);
    for (auto inst : {a, d}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
    env.left->push_back({inc, env.unit.gen(Jmp, env.bcctx, env.join)});
    env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
    auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
    auto const joinInc = env.unit.gen(IncRef, env.bcctx, d->dst());
    env.join->push_back({joinInc, dec});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Cancel a's pair and compensate on the empty edge. If a == d, the two
    // pre-existing references ensure compensation cannot release. If a != d,
    // IncRef d cannot provide the reference required by the final NZ guarantee.
    // Keep the pair for repeated loads of local 0: they provide only one
    // reference, so compensation could release before IncRef d.
    // TODO: For k = 0, cancel the join increment and final decrement instead.
    EXPECT_EQ(IncRef, joinInc->op());
    auto const blocked = local == 0;
    EXPECT_EQ(blocked ? IncRef : Nop, inc->op());
    EXPECT_EQ(blocked ? DecRefNZ : Nop, dec->op());
    if (blocked) {
      ASSERT_EQ(1, env.right->instrs().size());
      EXPECT_EQ(Jmp, env.right->front().op());
    } else {
      ASSERT_EQ(2, env.right->instrs().size());
      EXPECT_EQ(DecRef, env.right->front().op());
      EXPECT_EQ(a->dst(), env.right->front().src(0));
    }
  }
}

/*
 *              [unrelated CountedStr]
 *                 a = newVec(n)
 *                 CheckArrayCOW a
 *                  /           \
 *               copy         unique(a')
 *                exit        CountVec a'
 *                               exit
 *
 * Vary n between one and two references, with an unknown Vec as a control.
 * The optional unrelated value changes which alias-set ID is assigned to a.
 */
TEST(RefcountOpts, FoldArrayCOWBounds) {
  for (auto const refs : {0, 1, 2}) {
    for (auto const firstSet : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "refs=" << refs
                   << " firstSet=" << firstSet);
      IRUnit unit{test_context};
      BCContext const bcctx{BCMarker::Dummy(), 0};
      auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
      unit.entry()->push_back(fp);
      if (!firstSet) {
        unit.entry()->push_back(unit.gen(Conjure, bcctx, TCountedStr));
      }
      auto const value = [&] {
        if (!refs) {
          auto const unknown = unit.gen(Conjure, bcctx, TVec);
          unit.entry()->push_back(unknown);
          return unknown->dst();
        }
        auto const original = unit.gen(Conjure, bcctx, TStaticVec);
        auto const owned = unit.gen(CopyArray, bcctx, original->dst());
        unit.entry()->push_back({original, owned});
        if (refs == 2) unit.entry()->push_back(unit.gen(IncRef, bcctx, owned->dst()));
        return owned->dst();
      }();
      auto const copy = unit.defBlock();
      auto const unique = unit.defBlock();
      auto const cow = unit.gen(CheckArrayCOW, bcctx, copy, value);
      cow->setNext(unique);
      unit.entry()->push_back(cow);
      copy->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
      unique->push_back({unit.gen(CountVec, bcctx, cow->dst()),
                        unit.gen(Unreachable, bcctx, ASSERT_REASON)});

      optimizeRefcounts(unit);

      // Expected: one reference selects unique and preserves the refined SSA
      // destination; two select copy, making that destination's use unreachable.
      // An unknown Vec may be unique, shared, or static, so retain its check.
      // TODO: Select copy when the lower bound proves at least two references.
      auto const folded = refs == 1;
      EXPECT_EQ(folded ? AssertType : CheckArrayCOW, cow->op());
      if (folded) {
        EXPECT_EQ(Jmp, unit.entry()->back().op());
        EXPECT_EQ(unique, unit.entry()->back().taken());
      }
      EXPECT_TRUE(checkCfg(unit));
    }
  }
}

/*
 *                   a = newVec(2)
 *                   b: CountedVec
 *                     DecRefNZ b
 *                   CheckArrayCOW a
 *                    /           \
 *                  copy         unique
 *                  exit          exit
 *
 * b may alias a and consume one of its two references. The pending reduction
 * matters even before it has been subtracted from the raw lower bound.
 */
TEST(RefcountOpts, PreserveCOWAfterPendingAliasDecref) {
  IRUnit unit{test_context};
  BCContext const bcctx{BCMarker::Dummy(), 0};
  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const original = unit.gen(Conjure, bcctx, TStaticVec);
  auto const a = unit.gen(CopyArray, bcctx, original->dst());
  auto const b = unit.gen(Conjure, bcctx, TCountedVec);
  auto const copy = unit.defBlock();
  auto const unique = unit.defBlock();
  auto const cow = unit.gen(CheckArrayCOW, bcctx, copy, a->dst());
  cow->setNext(unique);
  unit.entry()->push_back({fp, original, a,
    unit.gen(IncRef, bcctx, a->dst()), b,
    unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()), cow});
  copy->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
  unique->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));

  optimizeRefcounts(unit);

  // Keep both outcomes: a has one reference when b == a, and two otherwise.
  // Folding with the raw lower bound would incorrectly select copy always.
  EXPECT_EQ(CheckArrayCOW, cow->op());
  EXPECT_EQ(copy, cow->taken());
  EXPECT_EQ(unique, cow->next());
  EXPECT_TRUE(checkCfg(unit));
}

/*
 *                  s = LdLoc 0
 *                  a = LdLoc 0
 *                    IncRef s
 *                   DecRefNZ a
 *                   b = newVec(1)
 *                    IncRef b
 *                  r = LdLoc 0
 *                  c = LdLoc 1
 *                  StLoc 1, null
 *                   DecRef c
 *                   /       \
 *                   \       /
 *                    DecRef b
 *
 * The first decrement releases s's temporary reference; local 0 stays
 * balanced. b is allocated afterward, so the reload cannot be reusing any
 * of b's reference credit. c releases local 1's separate reference.
 */
TEST(RefcountOpts, PreserveIndependentSupportAcrossReload) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const s = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  for (auto inst : {s, a, env.unit.gen(IncRef, env.bcctx, s->dst()),
       env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst())}) {
    env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
  }
  auto const b = env.array(env.unit.entry());
  auto const inc = env.unit.gen(IncRef, env.bcctx, b);
  auto const reload = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const c = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{1}, fp);
  for (auto inst : {inc, reload, c,
       env.unit.gen(StLoc, env.bcctx, LocalId{1}, fp, env.unit.cns(TInitNull)),
       env.unit.gen(DecRef, env.bcctx, DecRefData{}, c->dst())}) {
    env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
  }
  for (auto block : {env.left, env.right}) {
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  }
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, b);
  env.join->push_back(dec);
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Cancel b's pair. Its allocation reference survives the reload and c's
  // decrement; treating that credit as shared with r would lose cancellation.
  EXPECT_EQ(Nop, inc->op());
  EXPECT_EQ(Nop, dec->op());
}

/*
 *                    a = newVec(1)
 *                    c = LdLoc 0
 *                    StLoc 0, null
 *                      DecRef c
 *                    CheckArrayCOW a
 *                     /           \
 *                  copy          unique(a')
 *                   exit         CountVec a'
 *                                   exit
 *
 * c releases local 0's reference. Its release may decrement other values,
 * but cannot increase a's count or turn a unique array into a shared one.
 */
TEST(RefcountOpts, PreserveUpperBoundAcrossReleasingDecref) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const a = env.array(env.unit.entry());
  auto const c = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, c->dst());
  auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, env.left, a);
  cow->setNext(env.right);
  env.unit.entry()->back().convertToNop();
  env.unit.entry()->push_back({c,
    env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(TInitNull)),
    dec, cow});
  env.finish(env.left);
  env.right->push_back(env.unit.gen(CountVec, env.bcctx, cow->dst()));
  env.finish(env.right);

  optimizeRefcounts(env.unit);

  // Keep c's release, but select the unique branch for a and preserve a'.
  // TODO: Preserve finite upper bounds across decrement-only release effects.
  EXPECT_EQ(DecRef, dec->op());
  EXPECT_EQ(CheckArrayCOW, cow->op());
  EXPECT_TRUE(checkCfg(env.unit));
}

/*
 *    a: CountedVec or CountedStr (Conjure or LdMem &c[0]), b: releaseType
 *                                  |
 *                               IncRef a
 *                                  |
 *                                 Jmp
 *                                  |
 *                               DecRef b
 *                       CountVec a / LdStrLen a
 *                               DecRef a
 *
 * Vary b between uncounted values, strings, and containers. Vary a between
 * untracked reference credit and support from a heap location. The use of a
 * requires its reference to survive b's release, including when a is b itself
 * or a counted child of b. The jump keeps delay_decrefs from moving b's release.
 */
TEST(RefcountOpts, PreserveReferencesAcrossLeafDecref) {
  for (auto const releaseType : {TInitNull, TStaticVec, TCountedStr,
                                 TCountedStr | TStaticVec,
                                 TCountedVec, TCountedDict, TCountedKeyset}) {
    for (auto const string : {false, true}) {
      for (auto const heap : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "releaseType=" << releaseType.toString()
                     << " string=" << string << " heap=" << heap);
        IRUnit unit{test_context};
        BCContext const bcctx{BCMarker::Dummy(), 0};
        auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
        auto const aType = string ? TCountedStr : TCountedVec;
        unit.entry()->push_back(fp);
        auto const a = [&] {
          if (!heap) return unit.gen(Conjure, bcctx, aType);
          auto const c = unit.gen(Conjure, bcctx, TCountedVec);
          auto const address = unit.gen(LdVecElemAddr, bcctx,
                                        c->dst(), unit.cns(0), c->dst());
          unit.entry()->push_back({c, address});
          return unit.gen(LdMem, bcctx, aType, address->dst());
        }();
        auto const b = unit.gen(Conjure, bcctx, releaseType);
        auto const inc = unit.gen(IncRef, bcctx, a->dst());
        auto const body = unit.defBlock();
        unit.entry()->push_back({a, b, inc, unit.gen(Jmp, bcctx, body)});
        auto const release = unit.gen(DecRef, bcctx, DecRefData{}, b->dst());
        auto const dec = unit.gen(DecRef, bcctx, DecRefData{}, a->dst());
        body->push_back({release,
          unit.gen(string ? LdStrLen : CountVec, bcctx, a->dst()), dec,
          unit.gen(Unreachable, bcctx, ASSERT_REASON)});

        optimizeRefcounts(unit);

        // Cancel a's pair across uncounted decrements and, for vectors, string
        // releases. Neither can consume a reference to a. Keep the pair when b
        // can be a itself or contain it, so a stays live for the intervening use.
        // TODO: Preserve vector credit across keyset releases, which can only
        // decrement strings.
        auto const uncounted = releaseType == TInitNull || releaseType == TStaticVec;
        EXPECT_EQ(uncounted ? Nop : DecRef, release->op());
        // TODO: Skip nested release effects for strings.
        EXPECT_EQ(uncounted ? Nop : IncRef, inc->op());
        EXPECT_EQ(uncounted ? Nop : DecRef, dec->op());
      }
    }
  }
}

/*
 *                    a,b: CountedVec, d
 *                    /              \
 *                IncRef a         IncRef a
 *                 DecRef b         DecRef b
 *                    \              /
 *                         IncRef d
 *                         DecRefNZ a
 *
 * d is either disjoint (CountedStr) or may alias a (CountedVec). Its increment
 * may supply the final NZ guarantee only in the second case.
 */
TEST(RefcountOpts, ObserveAliasIncAfterReleasingDecrefs) {
  for (auto const alias : {false, true}) {
    SCOPED_TRACE(alias);
    PhiEnv env;
    auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const d = env.unit.gen(Conjure, env.bcctx,
                                alias ? TCountedVec : TCountedStr);
    for (auto inst : {a, b, d}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    std::vector<IRInstruction*> incs;
    for (auto block : {env.left, env.right}) {
      auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
      incs.push_back(inc);
      block->push_back({inc,
        env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst()),
        env.unit.gen(Jmp, env.bcctx, env.join)});
    }
    auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
    env.join->push_back({env.unit.gen(IncRef, env.bcctx, d->dst()), dec});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Keep a's pair when d may alias it: for a == b == d with one initial
    // reference, removing the pair releases the array before IncRef d.
    // A disjoint increment cannot supply a's final NZ guarantee, so a's
    // pair should cancel across it and the decrement-only releases.
    // TODO: Defer release requirements until the relevant increment or NZ.
    for (auto const inc : incs) EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRefNZ, dec->op());
  }
}

/*
 *          b: CountedVec containing a CountedDict at index 0
 *                         a = LdMem &b[0]
 *                            IncRef a
 *                               |
 *                              Jmp
 *                               |
 *                            DecRef b
 *                         /      |      \
 *                 IncRef d   IncRef d   CheckArrayCOW a
 *                  d: Str     d: Dict     /       \
 *                         \      |        \       /
 *                            DecRefNZ a
 *
 * The vec and dict cannot alias. Releasing b can still consume the heap
 * reference supporting a, so its deferred requirement must reach a's uses.
 */
TEST(RefcountOpts, ObserveNestedReleaseEffects) {
  for (auto const observation : {0, 1, 2}) {
    SCOPED_TRACE(observation);
    PhiEnv env;
    auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const address = env.unit.gen(LdVecElemAddr, env.bcctx,
                                      b->dst(), env.unit.cns(0), b->dst());
    auto const a = env.unit.gen(LdMem, env.bcctx, TCountedDict, address->dst());
    auto const d = env.unit.gen(Conjure, env.bcctx,
                                observation == 0 ? TCountedStr : TCountedDict);
    auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
    env.unit.entry()->back().convertToNop();
    env.unit.entry()->push_back({b, address, a, d, inc,
      env.unit.gen(Jmp, env.bcctx, env.join)});
    env.join->push_back(env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst()));
    auto const dec = env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst());
    if (observation == 2) {
      auto const exit = env.unit.defBlock();
      auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, env.left,
                                     a->dst());
      cow->setNext(env.right);
      env.join->push_back(cow);
      for (auto block : {env.left, env.right}) {
        block->push_back(env.unit.gen(Jmp, env.bcctx, exit));
      }
      exit->push_back(dec);
      env.finish(exit);
    } else {
      env.join->push_back({env.unit.gen(IncRef, env.bcctx, d->dst()), dec});
      env.finish(env.join);
    }

    optimizeRefcounts(env.unit);

    // Cancel only across the disjoint increment. An aliasing increment can
    // supply the final NZ guarantee after b releases its child; removing a's
    // pair could free that child before the increment. COW observes its count.
    // TODO: Delay the child's release requirement across disjoint increments.
    EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRefNZ, dec->op());
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *                     c = newDict(1); IncRef c
 *                a, d: CountedVec|CountedDict
 *                     IncRef a; IncRef d
 *                       CheckArrayCOW a
 *                        /          \
 *                   unique: exit   shared
 *                              CheckArrayCOW d
 *                               /          \
 *                          unique: exit   shared
 *                                         DecRefNZ b
 *                                         DecRefNZ a
 *                                         DecRefNZ d
 *                                          DecRef c
 *
 * b is either Vec or Vec|Dict. A Vec-only decrement must not gain the
 * ability to consume dict credit as it propagates through a and then d.
 */
TEST(RefcountOpts, ChainConditionalPendingDecrefTypes) {
  for (auto const overlap : {false, true}) {
    SCOPED_TRACE(overlap);
    IRUnit unit{test_context};
    BCContext const bcctx{BCMarker::Dummy(), 0};
    auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
    auto const original = unit.gen(Conjure, bcctx, TStaticDict);
    auto const c = unit.gen(CopyArray, bcctx, original->dst());
    auto const incC = unit.gen(IncRef, bcctx, c->dst());
    auto const a = unit.gen(Conjure, bcctx, TCountedVec | TCountedDict);
    auto const d = unit.gen(Conjure, bcctx, TCountedVec | TCountedDict);
    auto const b = unit.gen(Conjure, bcctx,
                            overlap ? TCountedVec | TCountedDict : TCountedVec);
    auto const incA = unit.gen(IncRef, bcctx, a->dst());
    auto const incD = unit.gen(IncRef, bcctx, d->dst());
    auto const second = unit.defBlock();
    auto const body = unit.defBlock();
    auto const uniqueA = unit.defBlock();
    auto const uniqueD = unit.defBlock();
    auto const cowA = unit.gen(CheckArrayCOW, bcctx, second, a->dst());
    cowA->setNext(uniqueA);
    auto const cowD = unit.gen(CheckArrayCOW, bcctx, body, d->dst());
    cowD->setNext(uniqueD);
    unit.entry()->push_back({fp, original, c, incC, a, d, b, incA, incD, cowA});
    second->push_back(cowD);
    uniqueA->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
    uniqueD->push_back(unit.gen(Unreachable, bcctx, ASSERT_REASON));
    auto const decA = unit.gen(DecRefNZ, bcctx, DecRefData{}, a->dst());
    auto const decD = unit.gen(DecRefNZ, bcctx, DecRefData{}, d->dst());
    auto const decC = unit.gen(DecRef, bcctx, DecRefData{}, c->dst());
    body->push_back({unit.gen(DecRefNZ, bcctx, DecRefData{}, b->dst()),
                    decA, decD, decC,
                    unit.gen(Unreachable, bcctx, ASSERT_REASON)});

    optimizeRefcounts(unit);

    // Expected: fold both shared COW decisions, then cancel a's and d's pairs.
    // TODO: Also cancel c's overlapping pair by changing the single
    // remaining DecRefNZ b to DecRef.
    // Cancel c's pair without changing NZs for b:Vec. If a or d aliases c,
    // its own retained increment pays for its decrement; b cannot consume
    // that dict credit.
    // With b:Vec|Dict, b == a == d == c can make the final decrement release.
    // TODO: Preserve c's credit through both bridges when b cannot alias c.
    // TODO: Revisit the observed pairs after folding the COW checks.
    EXPECT_EQ(IncRef, incA->op());
    EXPECT_EQ(IncRef, incD->op());
    EXPECT_EQ(DecRefNZ, decA->op());
    EXPECT_EQ(DecRefNZ, decD->op());
    EXPECT_EQ(IncRef, incC->op());
    EXPECT_EQ(DecRef, decC->op());
    EXPECT_TRUE(checkCfg(unit));
  }
}

/*
 *                    a = newVec(1); IncRef a
 *                        CheckArrayCOW a
 *                       /              \
 *                 shared              unique(a')
 *              StLoc 0,true       [release local 1]
 *                                  CountVec a'
 *                                  StLoc 0,false
 *                       \              /
 *                           DecRef a
 *                           CountVec a
 *
 * The unique edge is unreachable, including its optional releasing work.
 */
TEST(RefcountOpts, CancelPairAfterSharedCOWFold) {
  for (auto const release : {false, true}) {
    SCOPED_TRACE(release);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const a = env.array(env.unit.entry());
    auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedDict, LocalId{1}, fp);
    auto const inc = env.unit.gen(IncRef, env.bcctx, a);
    auto const cow = env.unit.gen(CheckArrayCOW, env.bcctx, env.left, a);
    cow->setNext(env.right);
    env.unit.entry()->back().convertToNop();
    env.unit.entry()->push_back({b, inc, cow});
    auto const shared = env.unit.gen(StLoc, env.bcctx,
                                     LocalId{0}, fp, env.unit.cns(true));
    env.left->push_back({shared, env.unit.gen(Jmp, env.bcctx, env.join)});
    if (release) {
      env.right->push_back({
        env.unit.gen(StLoc, env.bcctx, LocalId{1}, fp, env.unit.cns(TInitNull)),
        env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst())
      });
    }
    env.right->push_back({
      env.unit.gen(CountVec, env.bcctx, cow->dst()),
      env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(false)),
      env.unit.gen(Jmp, env.bcctx, env.join)
    });
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, a);
    env.join->push_back({dec, env.unit.gen(CountVec, env.bcctx, a)});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // Keep the shared decision even after cancelling the pair that proved it.
    // The allocation reference keeps a alive for CountVec after cancellation.
    // TODO: Revisit refcount pairs after folding the shared COW check.
    EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRefNZ, dec->op());
    EXPECT_EQ(env.left, cow->taken());
    EXPECT_EQ(StLoc, shared->op());
    EXPECT_TRUE(shared->src(1)->boolVal());
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *                    b = newDict(1); IncRef b
 *                         CheckArrayCOW b
 *                        /              \
 *                    shared          unique: exit
 *                   a = LdLoc 0
 *                   StLoc 0,null
 *                   CheckArrayCOW a
 *                  /               \
 *              shared             unique(a')
 *           StLoc 2,true        StLoc 2,false
 *              CountVec a      CountVec a'
 *                  \               /
 *                       DecRef b
 *                       DecRef a
 *
 * a has an unknown count; both of its COW outcomes remain possible after
 * folding b's check and revisiting the graph.
 */
TEST(RefcountOpts, PreserveUnknownCOWAfterSharedFold) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const original = env.unit.gen(Conjure, env.bcctx, TStaticDict);
  auto const b = env.unit.gen(CopyArray, env.bcctx, original->dst());
  auto const inc = env.unit.gen(IncRef, env.bcctx, b->dst());
  auto const cowB = env.unit.gen(CheckArrayCOW, env.bcctx, env.left, b->dst());
  cowB->setNext(env.right);
  env.unit.entry()->back().convertToNop();
  env.unit.entry()->push_back({original, b, inc, cowB});
  env.finish(env.right);
  auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const shared = env.unit.defBlock();
  auto const unique = env.unit.defBlock();
  auto const cowA = env.unit.gen(CheckArrayCOW, env.bcctx, shared, a->dst());
  cowA->setNext(unique);
  env.left->push_back({a,
    env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, env.unit.cns(TInitNull)), cowA});
  shared->push_back({
    env.unit.gen(StLoc, env.bcctx, LocalId{2}, fp, env.unit.cns(true)),
    env.unit.gen(CountVec, env.bcctx, a->dst()),
    env.unit.gen(Jmp, env.bcctx, env.join)
  });
  unique->push_back({
    env.unit.gen(StLoc, env.bcctx, LocalId{2}, fp, env.unit.cns(false)),
    env.unit.gen(CountVec, env.bcctx, cowA->dst()),
    env.unit.gen(Jmp, env.bcctx, env.join)
  });
  auto const decB = env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst());
  auto const decA = env.unit.gen(DecRef, env.bcctx, DecRefData{}, a->dst());
  env.join->push_back({decB, decA});
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Cancel b's pair after fixing its branch decision. Retain both outcomes
  // for a, its unique-edge SSA use, and its potentially releasing decrement.
  // TODO: Cancel b's pair after folding its COW check.
  EXPECT_EQ(IncRef, inc->op());
  EXPECT_EQ(DecRefNZ, decB->op());
  EXPECT_EQ(CheckArrayCOW, cowA->op());
  EXPECT_EQ(shared, cowA->taken());
  EXPECT_EQ(unique, cowA->next());
  EXPECT_EQ(DecRef, decA->op());
  EXPECT_TRUE(checkCfg(env.unit));
}

/*
 *                    s = LdLoc 0
 *                    a = LdLoc 0
 *                      IncRef s
 *                     DecRefNZ a
 *                    b = LdLoc 1
 *                    StLoc 1,null
 *                    r = LdLoc 0
 *                      IncRef r
 *                     /        \
 *                     \        /
 *                     DecRefNZ b
 *                      DecRef r
 *
 * a releases s's temporary reference. b releases the reference formerly
 * owned by local 1. Local 0 remains balanced, even when b aliases r.
 */
TEST(RefcountOpts, ReloadBeforeBalancedAliasDecref) {
  PhiEnv env;
  auto const fp = env.unit.entry()->front().dst();
  auto const s = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{1}, fp);
  auto const r = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
  auto const inc = env.unit.gen(IncRef, env.bcctx, r->dst());
  for (auto inst : {s, a, env.unit.gen(IncRef, env.bcctx, s->dst()),
       env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, a->dst()), b,
       env.unit.gen(StLoc, env.bcctx, LocalId{1}, fp, env.unit.cns(TInitNull)),
       r, inc}) {
    env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
  }
  for (auto block : {env.left, env.right}) {
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
  }
  auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, r->dst());
  env.join->push_back({
    env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, b->dst()), dec
  });
  env.finish(env.join);

  optimizeRefcounts(env.unit);

  // Cancel r's pair: local 0 owns a reference independently of local 1.
  // Also cancel IncRef s against DecRefNZ a: both load the same local value.
  // TODO: Recover the local's exclusive credit for r after the reload.
  EXPECT_EQ(IncRef, inc->op());
  EXPECT_EQ(DecRef, dec->op());
}

/*
 *          a = newVec(1)              b = newVec(1)
 *                                    [c = newVec(1)]
 *            \ (a,a)              (b,b or c) /
 *                         phi(p,q)
 *                          IncRef p
 *                            Jmp
 *                          DecRef q
 *
 * The phi columns are identical unless the second incoming edge uses c.
 */
TEST(RefcountOpts, CancelPairsAcrossIdenticalPhis) {
  for (auto const distinct : {false, true}) {
    for (auto const reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "distinct=" << distinct
                                       << " reverse=" << reverse);
      PhiEnv env{reverse};
      auto const left = env.array(env.left);
      auto const right = env.array(env.right);
      auto const other = distinct ? env.array(env.right) : right;
      env.left->push_back(env.unit.gen(Jmp, env.bcctx, env.join, left, left));
      env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join, right, other));
      auto const label = env.unit.defLabel(2, env.join, env.bcctx);
      retypeDests(label, &env.unit);
      auto const inc = env.unit.gen(IncRef, env.bcctx, label->dst(0));
      auto const tail = env.unit.defBlock();
      env.join->push_back({inc, env.unit.gen(Jmp, env.bcctx, tail)});
      auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, label->dst(1));
      tail->push_back(dec);
      env.finish(tail);

      optimizeRefcounts(env.unit);

      // Cancel the pair only when p and q select the same value on every edge.
      // Otherwise, on the b/c edge the increment and decrement affect distinct
      // allocations, and q's decrement must still release c.
      // TODO: Recognize identical phi columns and cancel their pair.
      EXPECT_EQ(IncRef, inc->op());
      EXPECT_EQ(DecRef, dec->op());
      EXPECT_TRUE(checkCfg(env.unit));
    }
  }
}

/*
 *        a: owning CountedVec          b: owning CountedVec
 *               DecRef a                    DecRef b
 *                    \ (a)               (b) /
 *                              phi(p)
 *                           [CountVec p]
 *                            [DecRef p]
 *
 * The live case owns an extra reference on entry and dereferences p. The
 * dead case owns one reference and merely forwards its address. Also vary
 * whether both incoming values are the same SSA temporary from entry.
 */
TEST(RefcountOpts, WeakenOnlyLivePhiInputs) {
  for (auto const live : {false, true}) {
    for (auto const same : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "live=" << live << " same=" << same);
      PhiEnv env;
      auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
      auto const b = same ? a : env.unit.gen(Conjure, env.bcctx, TCountedVec);
      if (same) {
        env.unit.entry()->insert(env.unit.entry()->backIter(), a);
      } else {
        env.left->push_back(a);
        env.right->push_back(b);
      }
      auto const leftDec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, a->dst());
      auto const rightDec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst());
      env.left->push_back(leftDec);
      env.right->push_back(rightDec);
      auto const p = env.phi(a->dst(), b->dst());
      if (live) {
        env.join->push_back({
          env.unit.gen(CountVec, env.bcctx, p),
          env.unit.gen(DecRef, env.bcctx, DecRefData{}, p)
        });
      }
      env.finish(env.join);

      optimizeRefcounts(env.unit);

      // Weaken the incoming decrements only when the phi is dereferenced.
      // Forwarding a dead pointer cannot justify skipping its release.
      // FIXME[LEAK]: Both incoming decrements skip the release when p is unused.
      EXPECT_EQ(DecRefNZ, leftDec->op());
      EXPECT_EQ(DecRefNZ, rightDec->op());
      EXPECT_TRUE(checkCfg(env.unit));
    }
  }
}

/*
 *       [initial: owning CountedVec]
 *                      |
 *       +---------> header
 *       |    p = phi(initial,next) OR a new owning Conjure
 *       |           CountVec p
 *       |            /      \
 *       |         body      exit
 *       |       DecRef p   DecRef p
 *       |    [next: owning CountedVec]
 *       +------- Jmp [next]
 *
 * Each iteration defines a new value. A later iteration's use of the same
 * SSA name cannot keep the current iteration's allocation alive.
 */
TEST(RefcountOpts, ReleaseBeforeLoopRedefinition) {
  for (auto const phi : {false, true}) {
    SCOPED_TRACE(phi);
    IRUnit unit{test_context};
    BCContext const bcctx{BCMarker::Dummy(), 0};
    unit.entry()->push_back(unit.gen(DefFP, bcctx, DefFPData{std::nullopt}));
    auto const header = unit.defBlock();
    auto const body = unit.defBlock();
    auto const exit = unit.defBlock();
    SSATmp* p;
    if (phi) {
      auto const initial = unit.gen(Conjure, bcctx, TCountedVec);
      unit.entry()->push_back({initial,
        unit.gen(Jmp, bcctx, header, initial->dst())});
      p = unit.defLabel(1, header, bcctx)->dst(0);
    } else {
      unit.entry()->push_back(unit.gen(Jmp, bcctx, header));
      auto const value = unit.gen(Conjure, bcctx, TCountedVec);
      header->push_back(value);
      p = value->dst();
    }
    auto const cond = unit.gen(Conjure, bcctx, TBool);
    auto const branch = unit.gen(JmpNZero, bcctx, exit, cond->dst());
    branch->setNext(body);
    header->push_back({unit.gen(CountVec, bcctx, p), cond, branch});
    auto const bodyDec = unit.gen(DecRef, bcctx, DecRefData{}, p);
    body->push_back(bodyDec);
    if (phi) {
      auto const next = unit.gen(Conjure, bcctx, TCountedVec);
      body->push_back({next, unit.gen(Jmp, bcctx, header, next->dst())});
      retypeDests(p->inst(), &unit);
    } else {
      body->push_back(unit.gen(Jmp, bcctx, header));
    }
    auto const exitDec = unit.gen(DecRef, bcctx, DecRefData{}, p);
    exit->push_back({exitDec, unit.gen(Unreachable, bcctx, ASSERT_REASON)});

    optimizeRefcounts(unit);

    // Both decrements must be able to release their current value. This
    // applies to assignments through a phi as well as ordinary definitions.
    // FIXME[LEAK]: The next iteration's CountVec turns the current value's
    // final release into NZ, leaking it on every backedge.
    EXPECT_EQ(DecRefNZ, bodyDec->op());
    EXPECT_EQ(DecRef, exitDec->op());
    EXPECT_TRUE(checkCfg(unit));
  }
}

/*
 *       a,b: CountedVec; a contains a CountedDict child at index 0
 *                     address = &a[0]
 *                     child = LdMem address
 *                         IncRef a
 *                            Jmp
 *                         DecRef b
 *                   one of these observations:
 *       CountVec a / CountDict child / IncRef child / LdMem address
 *                    / CheckArrayCOW a
 *                         DecRef a
 *
 * When a == b, b owns the original reference. The increment keeps a and its
 * child alive after DecRef b; the final decrement may release both.
 */
TEST(RefcountOpts, KeepPairAcrossReleaseObservation) {
  for (auto const observation : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(observation);
    PhiEnv env;
    auto const a = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const b = env.unit.gen(Conjure, env.bcctx, TCountedVec);
    auto const address = env.unit.gen(LdVecElemAddr, env.bcctx,
                                      a->dst(), env.unit.cns(0), a->dst());
    auto const child = env.unit.gen(LdMem, env.bcctx, TCountedDict,
                                    address->dst());
    auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
    env.unit.entry()->back().convertToNop();
    env.unit.entry()->push_back({a, b, address, child, inc,
      env.unit.gen(Jmp, env.bcctx, env.join)});
    env.join->push_back(env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst()));
    auto tail = env.join;
    IRInstruction* cow = nullptr;
    if (observation == 0) {
      tail->push_back(env.unit.gen(CountVec, env.bcctx, a->dst()));
    } else if (observation == 1) {
      tail->push_back(env.unit.gen(CountDict, env.bcctx, child->dst()));
    } else if (observation == 2) {
      tail->push_back(env.unit.gen(IncRef, env.bcctx, child->dst()));
    } else if (observation == 3) {
      tail->push_back(env.unit.gen(LdMem, env.bcctx, TCountedDict,
                                   address->dst()));
    } else {
      cow = env.unit.gen(CheckArrayCOW, env.bcctx, env.left, a->dst());
      cow->setNext(env.right);
      tail->push_back(cow);
      tail = env.unit.defBlock();
      for (auto block : {env.left, env.right}) {
        block->push_back(env.unit.gen(Jmp, env.bcctx, tail));
      }
    }
    auto const dec = env.unit.gen(DecRef, env.bcctx, DecRefData{}, a->dst());
    tail->push_back(dec);
    env.finish(tail);

    optimizeRefcounts(env.unit);

    // Keep the pair: removing it could free a and its child at DecRef b,
    // before the observation. Disjoint counted types do not exclude a
    // parent/child lifetime dependency, and a raw address is not counted.
    EXPECT_EQ(IncRef, inc->op());
    EXPECT_EQ(DecRef, dec->op());
    if (cow) EXPECT_EQ(CheckArrayCOW, cow->op());
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *                       a = LdLoc 0
 *                   [StLoc 0, newVec(1)]
 *                    [LdLoc 1: CountedStr]
 *                     b = LdLoc local
 *                     StLoc local,null
 *                         IncRef a
 *                        /        \
 *                        \        /
 *                         DecRef b
 *                         CountVec a
 *                         DecRef a
 *
 * Vary local between 0 and 1, or replace local 0 between the loads. The null
 * store transfers b's local reference into the instruction stream. With two
 * unchanged loads of local 0, both names describe the same single reference,
 * even with an unrelated pure load between them.
 */
TEST(RefcountOpts, ConsecutiveLoadReferenceCredit) {
  for (auto const variant : {0, 1, 2, 3}) {
    SCOPED_TRACE(variant);
    PhiEnv env;
    auto const fp = env.unit.entry()->front().dst();
    auto const local = uint32_t{variant == 1 ? 1u : 0u};
    auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
    env.unit.entry()->insert(env.unit.entry()->backIter(), a);
    if (variant == 2) {
      auto const replacement = env.array(env.unit.entry());
      env.unit.entry()->insert(env.unit.entry()->backIter(),
        env.unit.gen(StLoc, env.bcctx, LocalId{0}, fp, replacement));
    }
    if (variant == 3) {
      env.unit.entry()->insert(env.unit.entry()->backIter(),
        env.unit.gen(LdLoc, env.bcctx, TCountedStr, LocalId{1}, fp));
    }
    auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{local}, fp);
    auto const inc = env.unit.gen(IncRef, env.bcctx, a->dst());
    for (auto inst : {b,
         env.unit.gen(StLoc, env.bcctx, LocalId{local}, fp, env.unit.cns(TInitNull)),
         inc}) {
      env.unit.entry()->insert(env.unit.entry()->backIter(), inst);
    }
    for (auto block : {env.left, env.right}) {
      block->push_back(env.unit.gen(Jmp, env.bcctx, env.join));
    }
    auto const decB = env.unit.gen(DecRef, env.bcctx, DecRefData{}, b->dst());
    auto const decA = env.unit.gen(DecRef, env.bcctx, DecRefData{}, a->dst());
    env.join->push_back({decB, env.unit.gen(CountVec, env.bcctx, a->dst()), decA});
    env.finish(env.join);

    optimizeRefcounts(env.unit);

    // For identical loads, cancel IncRef a against DecRef b, but retain the
    // final releasing DecRef a: two loads cannot create a second credit.
    // With different or overwritten locations, b can be a separate allocation
    // whose decrement must release it, independently of a's temporary pair.
    // TODO: Recognize the identical loads and cancel their pair.
    EXPECT_EQ(DecRef, decB->op());
    if (variant == 0 || variant == 3) {
      EXPECT_EQ(IncRef, inc->op());
      EXPECT_EQ(DecRef, decA->op());
    }
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *        a = newVec(1)             b = newVec(1)
 *                  \ (a,a)       (b,b) /
 *                         phi(p,q)
 *                        /        \
 *                    IncRef p    (empty)
 *                        \        /
 *                         IncRef q
 *                         DecRefNZ p
 *                         CountVec q
 *
 * Also use q = LdLoc 0 in the dominator and p = phi(q, LdLoc 0). Both
 * forms have p == q at runtime, with one reference before either increment.
 */
TEST(RefcountOpts, PreservePhiAliasBeforeIncrement) {
  for (auto const duplicate : {false, true}) {
    SCOPED_TRACE(duplicate);
    PhiEnv env;
    SSATmp* p;
    SSATmp* q;
    if (duplicate) {
      auto const a = env.array(env.left);
      auto const b = env.array(env.right);
      env.left->push_back(env.unit.gen(Jmp, env.bcctx, env.join, a, a));
      env.right->push_back(env.unit.gen(Jmp, env.bcctx, env.join, b, b));
      auto const label = env.unit.defLabel(2, env.join, env.bcctx);
      retypeDests(label, &env.unit);
      p = label->dst(0);
      q = label->dst(1);
    } else {
      auto const fp = env.unit.entry()->front().dst();
      auto const a = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      auto const b = env.unit.gen(LdLoc, env.bcctx, TCountedVec, LocalId{0}, fp);
      env.unit.entry()->insert(env.unit.entry()->backIter(), a);
      env.right->push_back(b);
      p = env.phi(a->dst(), b->dst());
      q = a->dst();
    }
    auto const left = env.unit.defBlock();
    auto const right = env.unit.defBlock();
    auto const tail = env.unit.defBlock();
    auto const cond = env.unit.gen(Conjure, env.bcctx, TBool);
    auto const branch = env.unit.gen(JmpZero, env.bcctx, right, cond->dst());
    branch->setNext(left);
    env.join->push_back({cond, branch});
    auto const inc = env.unit.gen(IncRef, env.bcctx, p);
    left->push_back({inc, env.unit.gen(Jmp, env.bcctx, tail)});
    right->push_back(env.unit.gen(Jmp, env.bcctx, tail));
    tail->push_back({env.unit.gen(IncRef, env.bcctx, q),
      env.unit.gen(DecRefNZ, env.bcctx, DecRefData{}, p),
      env.unit.gen(CountVec, env.bcctx, q)});
    env.finish(tail);

    optimizeRefcounts(env.unit);

    // Keep the branch increment and leave the empty edge unchanged. Its one
    // reference must survive until IncRef q, which supplies the NZ guarantee.
    // Unlike dropping the last release in the phi tests above, a compensating
    // ordinary DecRef here frees p == q before the increment and CountVec.
    EXPECT_EQ(IncRef, inc->op());
    ASSERT_EQ(1, right->instrs().size());
    EXPECT_EQ(Jmp, right->front().op());
    EXPECT_TRUE(checkCfg(env.unit));
  }
}

/*
 *     a = newVec(3)             b = newVec(3)
 *              \ (a,false)   (b,false) /
 *          +-----> join(p,done)
 *          |          /      \
 *          |      !done      done
 *          |        |          |
 *          | q: CountedVec  CheckArrayCOW p
 *          |        |          /      \
 *          +- Jmp(q,true)     copy    unique
 *
 * q has an unknown refcount and may be unique. Replacing p on the backedge
 * must discard the three-reference lower bound for the previous phi value.
 */
TEST(RefcountOpts, PreserveCOWAcrossReplacedLoopPhi) {
  PhiEnv env;
  for (auto const block : {env.left, env.right}) {
    auto const value = env.array(block, 3);
    block->push_back(env.unit.gen(Jmp, env.bcctx, env.join,
                                  value, env.unit.cns(false)));
  }
  auto const label = env.unit.defLabel(2, env.join, env.bcctx);
  auto const body = env.unit.defBlock();
  auto const exit = env.unit.defBlock();
  auto const replacement = env.unit.gen(Conjure, env.bcctx, TCountedVec);
  body->push_back({replacement,
    env.unit.gen(Jmp, env.bcctx, env.join,
                 replacement->dst(), env.unit.cns(true))});
  retypeDests(label, &env.unit);
  auto const branch = env.unit.gen(JmpNZero, env.bcctx, exit, label->dst(1));
  branch->setNext(body);
  env.join->push_back(branch);
  auto const copy = env.unit.defBlock();
  auto const unique = env.unit.defBlock();
  auto const check = env.unit.gen(CheckArrayCOW, env.bcctx, copy, label->dst(0));
  check->setNext(unique);
  exit->push_back(check);
  env.finish(copy);
  env.finish(unique);

  optimizeRefcounts(env.unit);

  // Keep both outcomes: the backedge can replace p with a unique array.
  // Its unknown upper bound also avoids the state-check failure exercised by
  // SwapFiniteLoopPhiBounds, exposing a stale lower bound through COW folding.
  EXPECT_EQ(CheckArrayCOW, check->op());
  EXPECT_EQ(copy, check->taken());
  EXPECT_EQ(unique, check->next());
  EXPECT_TRUE(checkCfg(env.unit));
}

}
