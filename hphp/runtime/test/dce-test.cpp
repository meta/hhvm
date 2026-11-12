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

#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/dce.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ir-unit.h"

#include "hphp/runtime/test/test-context.h"

namespace HPHP::jit {

TEST(DCE, NonDCEablePassthroughKeepsProducerLive) {
  auto const bcctx = BCContext{test_marker(), 0};
  IRUnit unit{test_context};

  auto const entry = unit.entry();
  auto const taken = unit.defBlock();
  auto const end = unit.defBlock();

  auto const fp = unit.gen(DefFP, bcctx, DefFPData{std::nullopt});
  auto const str = unit.gen(Conjure, bcctx, TStr);
  auto const conv = unit.gen(StrictlyIntegerConv, bcctx, str->dst());
  auto const check = unit.gen(CheckType, bcctx, TInt, taken, conv->dst());
  check->setNext(end);
  entry->push_back({fp, str, conv, check});

  auto const endBlockData =
    EndBlockData{IRSPRelOffset{0}, nullptr, ASSERT_REASON.reason};
  taken->push_back(unit.gen(EndBlock, bcctx, endBlockData));
  end->push_back(unit.gen(EndBlock, bcctx, endBlockData));

  fullDCE(unit);

  ASSERT_EQ(4, entry->instrs().size());
  auto it = entry->begin();
  EXPECT_EQ(DefFP, it->op());

  auto& strInst = *(++it);
  EXPECT_EQ(Conjure, strInst.op());

  auto& convInst = *(++it);
  EXPECT_EQ(StrictlyIntegerConv, convInst.op());
  EXPECT_EQ(str->dst(), convInst.src(0));

  auto& checkInst = *(++it);
  EXPECT_EQ(CheckType, checkInst.op());
  EXPECT_EQ(conv->dst(), checkInst.src(0));
  EXPECT_EQ(taken, checkInst.taken());
  EXPECT_EQ(end, checkInst.next());
}

}
