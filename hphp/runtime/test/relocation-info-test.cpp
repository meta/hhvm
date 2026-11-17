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

#include "hphp/runtime/vm/jit/relocation.h"

#include <folly/portability/GTest.h>

#include <utility>

namespace HPHP::jit {

namespace {

// RelocationInfo only stores addresses, so the tests never touch these bytes.
uint8_t s_src[256];
uint8_t s_dst[256];

TCA src(size_t off) { return s_src + off; }
TCA dst(size_t off) { return s_dst + off; }

using RangeVec = RelocationInfo::RangeVec;

/*
 * Record [start, end) moved to dstOff the way relocate() does: one
 * recordAddress() per instruction after the first, then recordRange().
 */
void recordMove(RelocationInfo& rel, size_t start, size_t end,
                size_t dstOff, std::initializer_list<size_t> insts) {
  for (auto const i : insts) {
    rel.recordAddress(src(i), dst(dstOff + i - start), 0);
  }
  rel.recordRange(src(start), src(end), dst(dstOff), dst(dstOff + end - start));
}

}

TEST(RelocationInfo, RecordAddressMapsBeforeAndAfter) {
  RelocationInfo rel;
  rel.recordAddress(src(8), dst(16), 4);
  EXPECT_EQ(rel.adjustedAddressBefore(src(8)), dst(16));
  EXPECT_EQ(rel.adjustedAddressAfter(src(8)), dst(20));
  EXPECT_EQ(rel.adjustedAddressBefore(src(9)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(9)), nullptr);
}

TEST(RelocationInfo, RecordAddressKeepsFirstMapping) {
  RelocationInfo rel;
  rel.recordAddress(src(8), dst(16), 0);
  rel.recordAddress(src(8), dst(32), 0);
  EXPECT_EQ(rel.adjustedAddressAfter(src(8)), dst(16));
}

TEST(RelocationInfo, RecordRangeMapsEndpoints) {
  RelocationInfo rel;
  rel.recordRange(src(0), src(10), dst(100), dst(112));
  EXPECT_EQ(rel.adjustedAddressAfter(src(0)), dst(100));
  EXPECT_EQ(rel.adjustedAddressBefore(src(0)), nullptr);
  EXPECT_EQ(rel.adjustedAddressBefore(src(10)), dst(112));
  EXPECT_EQ(rel.adjustedAddressAfter(src(10)), nullptr);
  EXPECT_EQ(rel.srcRanges(), (RangeVec{{src(0), src(10)}}));
  EXPECT_EQ(rel.dstRanges(), (RangeVec{{dst(100), dst(112)}}));
}

TEST(RelocationInfo, RecordRangeSharedBoundary) {
  RelocationInfo rel;
  rel.recordRange(src(0), src(10), dst(100), dst(110));
  rel.recordRange(src(10), src(20), dst(200), dst(210));
  EXPECT_EQ(rel.adjustedAddressBefore(src(10)), dst(110));
  EXPECT_EQ(rel.adjustedAddressAfter(src(10)), dst(200));
}

TEST(RelocationInfo, RewindDropsLastRange) {
  RelocationInfo rel;
  recordMove(rel, 10, 20, 100, {12, 15, 18});
  rel.rewind(src(10), src(20));
  for (auto const i : {10, 12, 15, 18, 20}) {
    EXPECT_EQ(rel.adjustedAddressAfter(src(i)), nullptr) << i;
    EXPECT_EQ(rel.adjustedAddressBefore(src(i)), nullptr) << i;
  }
  EXPECT_TRUE(rel.srcRanges().empty());
  EXPECT_TRUE(rel.dstRanges().empty());
}

TEST(RelocationInfo, RewindKeepsAddressesOutsideRange) {
  RelocationInfo rel;
  recordMove(rel, 0, 8, 50, {2, 5});
  recordMove(rel, 30, 40, 150, {33, 37});
  recordMove(rel, 10, 20, 100, {12, 15, 18});
  rel.rewind(src(10), src(20));

  for (auto const i : {12, 15, 18}) {
    EXPECT_EQ(rel.adjustedAddressAfter(src(i)), nullptr) << i;
  }
  EXPECT_EQ(rel.adjustedAddressAfter(src(0)), dst(50));
  EXPECT_EQ(rel.adjustedAddressAfter(src(2)), dst(52));
  EXPECT_EQ(rel.adjustedAddressAfter(src(5)), dst(55));
  EXPECT_EQ(rel.adjustedAddressBefore(src(8)), dst(58));
  EXPECT_EQ(rel.adjustedAddressAfter(src(30)), dst(150));
  EXPECT_EQ(rel.adjustedAddressAfter(src(33)), dst(153));
  EXPECT_EQ(rel.adjustedAddressAfter(src(37)), dst(157));
  EXPECT_EQ(rel.adjustedAddressBefore(src(40)), dst(160));
  EXPECT_EQ(rel.srcRanges(),
            (RangeVec{{src(0), src(8)}, {src(30), src(40)}}));
}

/*
 * The start of the rewound range is also the end of the previous range, so
 * its "before" mapping belongs to the previous range and must survive.
 */
TEST(RelocationInfo, RewindKeepsEndOfPreviousRange) {
  RelocationInfo rel;
  recordMove(rel, 0, 10, 50, {4});
  recordMove(rel, 10, 20, 100, {12, 15});
  rel.markSmashableRelocation(dst(100));
  rel.markSmashableRelocation(dst(102));
  rel.markSmashableRelocation(dst(54));
  rel.rewind(src(10), src(20));

  EXPECT_EQ(rel.adjustedAddressBefore(src(10)), dst(60));
  EXPECT_EQ(rel.adjustedAddressAfter(src(10)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(12)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(15)), nullptr);
  EXPECT_EQ(rel.adjustedAddressBefore(src(20)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(4)), dst(54));
  EXPECT_FALSE(rel.isSmashableRelocation(dst(100)));
  EXPECT_FALSE(rel.isSmashableRelocation(dst(102)));
  EXPECT_TRUE(rel.isSmashableRelocation(dst(54)));
  EXPECT_EQ(rel.srcRanges(), (RangeVec{{src(0), src(10)}}));
}

/*
 * The end of the rewound range is also the start of a range relocated
 * earlier, so its "after" mapping belongs to that range and must survive.
 */
TEST(RelocationInfo, RewindKeepsStartOfNextRange) {
  RelocationInfo rel;
  recordMove(rel, 20, 30, 150, {25});
  recordMove(rel, 10, 20, 100, {12, 15});
  rel.rewind(src(10), src(20));

  EXPECT_EQ(rel.adjustedAddressAfter(src(20)), dst(150));
  EXPECT_EQ(rel.adjustedAddressBefore(src(20)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(10)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(12)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(15)), nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(src(25)), dst(155));
  EXPECT_EQ(rel.srcRanges(), (RangeVec{{src(20), src(30)}}));
}

TEST(RelocationInfo, RewindErasesSmashableRelocationsInRange) {
  RelocationInfo rel;
  recordMove(rel, 0, 8, 50, {4});
  recordMove(rel, 10, 20, 100, {12, 15});
  rel.markSmashableRelocation(dst(54));
  rel.markSmashableRelocation(dst(102));
  rel.markSmashableRelocation(dst(105));
  rel.rewind(src(10), src(20));

  EXPECT_FALSE(rel.isSmashableRelocation(dst(102)));
  EXPECT_FALSE(rel.isSmashableRelocation(dst(105)));
  EXPECT_TRUE(rel.isSmashableRelocation(dst(54)));
}

TEST(RelocationInfo, RewindThenRecordAgain) {
  RelocationInfo rel;
  recordMove(rel, 10, 20, 100, {12, 15});
  rel.rewind(src(10), src(20));
  recordMove(rel, 10, 20, 200, {12, 15});

  EXPECT_EQ(rel.adjustedAddressAfter(src(10)), dst(200));
  EXPECT_EQ(rel.adjustedAddressAfter(src(12)), dst(202));
  EXPECT_EQ(rel.adjustedAddressAfter(src(15)), dst(205));
  EXPECT_EQ(rel.adjustedAddressBefore(src(20)), dst(210));
  EXPECT_EQ(rel.srcRanges(), (RangeVec{{src(10), src(20)}}));
  EXPECT_EQ(rel.dstRanges(), (RangeVec{{dst(200), dst(210)}}));
}

}
