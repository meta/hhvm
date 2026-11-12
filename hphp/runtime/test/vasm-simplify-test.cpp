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

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/test/test-context.h"
#include "hphp/runtime/test/vasm-test-helpers.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/configs/jit.h"
#include "hphp/util/immed.h"

#include <folly/ScopeGuard.h>

#include <folly/portability/GTest.h>

#include <optional>

namespace HPHP::jit {

namespace {

IRInstruction* makeLdLocOrigin(IRUnit& unit,
                               BCContext bcctx,
                               SSATmp* fp,
                               uint32_t locId) {
  return unit.gen(LdLoc, bcctx, TCell, LocalId{locId}, fp);
}

IRInstruction* makeStLocOrigin(IRUnit& unit,
                               BCContext bcctx,
                               SSATmp* fp,
                               uint32_t locId) {
  auto const value = unit.gen(Conjure, bcctx, TCell)->dst();
  return unit.gen(StLoc, bcctx, LocalId{locId}, fp, value);
}

IRInstruction* makeLdStkOrigin(IRUnit& unit,
                               BCContext bcctx,
                               IRSPRelOffset offset) {
  auto const sp = unit.gen(
    DefRegSP,
    bcctx,
    DefStackData{SBInvOffset{0}, SBInvOffset{0}}
  )->dst();
  return unit.gen(LdStk, bcctx, TCell, IRSPRelOffsetData{offset}, sp);
}

IRInstruction* makeEnterInlineFrameOrigin(IRUnit& unit, BCContext bcctx) {
  auto const sp = unit.gen(
    DefRegSP,
    bcctx,
    DefStackData{SBInvOffset{0}, SBInvOffset{0}}
  )->dst();
  auto const calleeFP = unit.gen(
    DefCalleeFP,
    bcctx,
    DefCalleeFPData{
      IRSPRelOffset{0},
      nullptr,
      1,
      SrcKey{},
      IRSPRelOffset{0},
      SBInvOffset{0},
      0
    },
    sp
  )->dst();
  return unit.gen(EnterInlineFrame, bcctx, calleeFP);
}

// Used only as a stable direct-call address for Vasm call/vinvoke tests.
void sinkDefsTestHelper() {}

template<class F>
void testPostRAWithTraceAndPrologueAbi(F f) {
  f(abi(CodeKind::Trace));
  f(abi(CodeKind::Prologue));
}

void testSetccXor() {
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    auto xdst = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_Z, sf, dst};
    v << xorbi{1, dst, xdst, v.makeReg()};

    simplify(unit);

    // Test that setcc/xor pair is collapsed.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %131(42l) => %128\n"
      "setcc NE, %128 => %130\n",
      stripWhitespace(show(unit))
    );
  }

  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    auto xdst = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_Z, sf, dst};
    v << xorbi{1, dst, xdst, v.makeReg()};
    v << movl{dst, v.makeReg()};

    simplify(unit);

    // Test that setcc/xor pair is not collapsed when setcc result
    // has more than one use.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %131(42l) => %128\n"
      "setcc E, %128 => %129\n"
      "xorbi 1, %129 => %130, %132\n"
      "movl %129 => %133\n",
      stripWhitespace(show(unit))
    );
  }

  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    auto xdst = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_NZ, sf, dst};
    v << xorbi{1, dst, xdst, v.makeReg()};

    simplify(unit);

    // Check that setcc/xor pair is collapsed with different condition.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %131(42l) => %128\n"
      "setcc E, %128 => %130\n",
      stripWhitespace(show(unit))
    );
  }

  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_NZ, sf, dst};

    simplify(unit);

    // Make sure that setcc with no xor doesn't cause a buffer overrun.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %130(42l) => %128\n"
      "setcc NE, %128 => %129\n",
      stripWhitespace(show(unit))
    );
  }

  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    auto xdst = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_NZ, sf, dst};
    v << xorbi{2, dst, xdst, v.makeReg()};

    simplify(unit);

    // Make sure that setcc/xor with an non-1 xor constant is skipped.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %131(42l) => %128\n"
      "setcc NE, %128 => %129\n"
      "xorbi 2, %129 => %130, %132\n",
      stripWhitespace(show(unit))
    );
  }

  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto sf = v.makeReg();
    auto dst = v.makeReg();
    auto xdst = v.makeReg();
    auto xsf = v.makeReg();
    v << movl{unit.makeConst(42u), sf};
    v << setcc{CC_NZ, sf, dst};
    v << xorbi{1, dst, xdst, xsf};
    v << movl{xsf, v.makeReg()};

    simplify(unit);

    // Make sure that setcc/xor with xor status flags being used is skipped.
    EXPECT_EQ(
      "B0 main (1)\n"
      "movl %132(42l) => %128\n"
      "setcc NE, %128 => %129\n"
      "xorbi 1, %129 => %130, %131\n"
      "movl %131 => %133\n",
      stripWhitespace(show(unit))
    );
  }

}

void testPostRACopyFold() {
  // Use register numbers valid on both x64 (0-15) and ARM (0-30),
  // avoiding rsp (4 on x64, 31 on ARM).
  auto const r0 = Vreg{Reg64{0}};
  auto const r1 = Vreg{Reg64{1}};
  auto const r8 = Vreg{Reg64{8}};
  auto const r9 = Vreg{Reg64{9}};
  auto const r10 = Vreg{Reg64{10}};

  // Post-RA fold: shrli{2, r8, r8}; copy{r8, r0} -> shrli{2, r8, r0}
  testPostRAWithTraceAndPrologueAbi([&] (const Abi& postRAAbi) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const sf = v.makeReg();
    v << shrli{2, Vreg32(r8), Vreg32(r8), sf, 0};
    v << copy{r8, r0};

    postRASimplify(unit, postRAAbi);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(1, code.size());
    EXPECT_EQ(Vinstr::shrli, code[0].op);
    EXPECT_EQ(Vreg32(r0), code[0].shrli_.d);
  });

  // Post-RA fold: load{[r10-0x10], r8}; copy{r8, r0} -> load{[r10-0x10], r0}
  testPostRAWithTraceAndPrologueAbi([&] (const Abi& postRAAbi) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    v << load{r10[-0x10], r8};
    v << copy{r8, r0};

    postRASimplify(unit, postRAAbi);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(1, code.size());
    EXPECT_EQ(Vinstr::load, code[0].op);
    EXPECT_EQ(r0, code[0].load_.d);
  });

  // Negative: copy source used after the copy — should NOT fold.
  testPostRAWithTraceAndPrologueAbi([&] (const Abi& postRAAbi) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const sf = v.makeReg();
    v << shrli{2, Vreg32(r8), Vreg32(r8), sf, 0};
    v << copy{r8, r0};
    v << copy{r8, r1};  // r8 still used

    postRASimplify(unit, postRAAbi);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(3, code.size());
    EXPECT_EQ(Vinstr::shrli, code[0].op);
    EXPECT_EQ(Vreg32(r8), code[0].shrli_.d);
    EXPECT_EQ(Vinstr::copy, code[1].op);
    EXPECT_EQ(Vinstr::copy, code[2].op);
  });

  // Negative: intervening instruction reads cp.d — should NOT fold.
  testPostRAWithTraceAndPrologueAbi([&] (const Abi& postRAAbi) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const sf = v.makeReg();
    v << shrli{2, Vreg32(r8), Vreg32(r8), sf, 0};
    v << copy{r0, r10};  // reads r0 (the copy dest)
    v << copy{r8, r0};

    postRASimplify(unit, postRAAbi);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(3, code.size());
    EXPECT_EQ(Vinstr::shrli, code[0].op);
    EXPECT_EQ(Vreg32(r8), code[0].shrli_.d);
    EXPECT_EQ(Vinstr::copy, code[1].op);
    EXPECT_EQ(Vinstr::copy, code[2].op);
  });

  // Negative: intervening call may clobber cp.d implicitly — should NOT fold.
  testPostRAWithTraceAndPrologueAbi([&] (const Abi& postRAAbi) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const sf = v.makeReg();
    v << shrli{2, Vreg32(r8), Vreg32(r8), sf, 0};
    v << callr{Vreg64(r9), RegSet{}};
    v << copy{r8, r0};

    postRASimplify(unit, postRAAbi);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(3, code.size());
    EXPECT_EQ(Vinstr::shrli, code[0].op);
    EXPECT_EQ(Vreg32(r8), code[0].shrli_.d);
    EXPECT_EQ(Vinstr::callr, code[1].op);
    EXPECT_EQ(Vinstr::copy, code[2].op);
  });
}

void testSinkDefsMovesIntoMergeAfterPhidef() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const pred = unit.makeBlock(AreaIndex::Main, 1);
    auto const merge = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vp(unit, pred);
    Vout vm(unit, merge);

    auto const cand = Vreg64{v.makeReg()};
    auto const phiIn = Vreg64{vp.makeReg()};
    auto const phi = Vreg64{vm.makeReg()};
    auto const out = Vreg64{vm.makeReg()};

    v << ldimmq{Immed64{42}, cand};
    v << jmp{pred};

    vp << ldimmq{Immed64{1}, phiIn};
    vp << phijmp{merge, vp.makeTuple({phiIn})};

    vm << phidef{vm.makeTuple({phi})};
    vm << copy{cand, out};
    vm << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 1);
    ASSERT_EQ(entryCode[0].op, Vinstr::jmp);
    EXPECT_EQ(entryCode[0].jmp_.target, pred);

    auto const& mergeCode = unit.blocks[merge].code;
    ASSERT_EQ(mergeCode.size(), 4);
    ASSERT_EQ(mergeCode[0].op, Vinstr::phidef);
    ASSERT_EQ(mergeCode[1].op, Vinstr::ldimmq);
    EXPECT_EQ(static_cast<uint64_t>(mergeCode[1].ldimmq_.s.q()), 42);
    EXPECT_EQ(mergeCode[1].ldimmq_.d, cand);
    ASSERT_EQ(mergeCode[2].op, Vinstr::copy);
    EXPECT_EQ(mergeCode[2].copy_.s, Vreg{cand});
    EXPECT_EQ(mergeCode[2].copy_.d, Vreg{out});
    ASSERT_EQ(mergeCode[3].op, Vinstr::ret);
  }
}

void testSinkDefsKeepsJoinPointDefsInPlace() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const left = unit.makeBlock(AreaIndex::Main, 1);
    auto const right = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vl(unit, left);
    Vout vr(unit, right);

    auto const cand = Vreg64{v.makeReg()};
    auto const sf = v.makeReg();
    auto const leftUse = Vreg64{vl.makeReg()};
    auto const rightUse = Vreg64{vr.makeReg()};

    v << ldimmq{Immed64{42}, cand};
    v << cmpqi{Immed{0}, Vreg64{Reg64{0}}, sf, Vflags{}};
    v << jcc{CC_E, sf, {left, right}, StringTag{}};

    vl << copy{cand, leftUse};
    vl << ret{};

    vr << copy{cand, rightUse};
    vr << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 3);
    ASSERT_EQ(entryCode[0].op, Vinstr::ldimmq);
    EXPECT_EQ(static_cast<uint64_t>(entryCode[0].ldimmq_.s.q()), 42);
    EXPECT_EQ(entryCode[0].ldimmq_.d, cand);
    ASSERT_EQ(entryCode[2].op, Vinstr::jcc);

    auto const& leftCode = unit.blocks[left].code;
    ASSERT_EQ(leftCode.size(), 2);
    ASSERT_EQ(leftCode[0].op, Vinstr::copy);
    EXPECT_EQ(leftCode[0].copy_.s, Vreg{cand});

    auto const& rightCode = unit.blocks[right].code;
    ASSERT_EQ(rightCode.size(), 2);
    ASSERT_EQ(rightCode[0].op, Vinstr::copy);
    EXPECT_EQ(rightCode[0].copy_.s, Vreg{cand});
  }
}

void testSinkDefsKeepsDefsOutOfHotterBlocks() {
  auto const test = [] (AreaIndex srcArea,
                        uint64_t srcWeight,
                        AreaIndex targetArea,
                        uint64_t targetWeight) {
    for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
      SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

      Vunit unit;
      unit.entry = unit.makeBlock(AreaIndex::Main, 1);
      auto const src = unit.makeBlock(srcArea, srcWeight);
      auto const target = unit.makeBlock(targetArea, targetWeight);

      Vout v(unit, unit.entry);
      Vout vs(unit, src);
      Vout vt(unit, target);

      auto const cand = Vreg64{vs.makeReg()};
      auto const out = Vreg64{vt.makeReg()};

      v << jmp{src};

      vs << ldimmq{Immed64{42}, cand};
      vs << jmp{target};

      vt << copy{cand, out};
      vt << ret{};

      sinkDefs(unit, abi(kind));

      auto const& srcCode = unit.blocks[src].code;
      ASSERT_EQ(srcCode.size(), 2);
      ASSERT_EQ(srcCode[0].op, Vinstr::ldimmq);
      EXPECT_EQ(static_cast<uint64_t>(srcCode[0].ldimmq_.s.q()), 42);
      EXPECT_EQ(srcCode[0].ldimmq_.d, cand);
      ASSERT_EQ(srcCode[1].op, Vinstr::jmp);

      auto const& targetCode = unit.blocks[target].code;
      ASSERT_EQ(targetCode.size(), 2);
      ASSERT_EQ(targetCode[0].op, Vinstr::copy);
      EXPECT_EQ(targetCode[0].copy_.s, Vreg{cand});
      ASSERT_EQ(targetCode[1].op, Vinstr::ret);
    }
  };

  test(AreaIndex::Main, 1, AreaIndex::Main, 10);
  test(AreaIndex::Cold, 10, AreaIndex::Main, 10);
}

void testSinkDefsFallsBackToLegalDominator() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const src = unit.makeBlock(AreaIndex::Main, 10);
    auto const mid = unit.makeBlock(AreaIndex::Main, 10);
    auto const target = unit.makeBlock(AreaIndex::Main, 20);

    Vout v(unit, unit.entry);
    Vout vs(unit, src);
    Vout vm(unit, mid);
    Vout vt(unit, target);

    auto const cand = Vreg64{vs.makeReg()};
    auto const out = Vreg64{vt.makeReg()};

    v << jmp{src};

    vs << ldimmq{Immed64{42}, cand};
    vs << jmp{mid};

    vm << jmp{target};

    vt << copy{cand, out};
    vt << ret{};

    sinkDefs(unit, abi(kind));

    auto const& srcCode = unit.blocks[src].code;
    ASSERT_EQ(srcCode.size(), 1);
    ASSERT_EQ(srcCode[0].op, Vinstr::jmp);
    EXPECT_EQ(srcCode[0].jmp_.target, mid);

    auto const& midCode = unit.blocks[mid].code;
    ASSERT_EQ(midCode.size(), 2);
    ASSERT_EQ(midCode[0].op, Vinstr::ldimmq);
    EXPECT_EQ(static_cast<uint64_t>(midCode[0].ldimmq_.s.q()), 42);
    EXPECT_EQ(midCode[0].ldimmq_.d, cand);
    ASSERT_EQ(midCode[1].op, Vinstr::jmp);
    EXPECT_EQ(midCode[1].jmp_.target, target);

    auto const& targetCode = unit.blocks[target].code;
    ASSERT_EQ(targetCode.size(), 2);
    ASSERT_EQ(targetCode[0].op, Vinstr::copy);
    EXPECT_EQ(targetCode[0].copy_.s, Vreg{cand});
    EXPECT_EQ(targetCode[0].copy_.d, Vreg{out});
    ASSERT_EQ(targetCode[1].op, Vinstr::ret);
  }
}

void testSinkDefsMovesDefsWithDeadSF() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const slow = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vs(unit, slow);

    auto const src = Vreg64{v.makeReg()};
    auto const shifted = Vreg64{v.makeReg()};
    auto const out = Vreg64{vs.makeReg()};
    auto const sf = v.makeReg();

    v << ldimmq{Immed64{42}, src};
    v << shrqi{Immed{5}, src, shifted, sf, Vflags{}};
    v << jmp{slow};

    vs << copy{shifted, out};
    vs << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 1);
    ASSERT_EQ(entryCode[0].op, Vinstr::jmp);
    EXPECT_EQ(entryCode[0].jmp_.target, slow);

    auto const& slowCode = unit.blocks[slow].code;
    ASSERT_EQ(slowCode.size(), 4);
    ASSERT_EQ(slowCode[0].op, Vinstr::ldimmq);
    EXPECT_EQ(slowCode[0].ldimmq_.d, src);
    ASSERT_EQ(slowCode[1].op, Vinstr::shrqi);
    EXPECT_EQ(slowCode[1].shrqi_.s1, src);
    EXPECT_EQ(slowCode[1].shrqi_.d, shifted);
    ASSERT_EQ(slowCode[2].op, Vinstr::copy);
    EXPECT_EQ(slowCode[2].copy_.s, Vreg{shifted});
    EXPECT_EQ(slowCode[2].copy_.d, Vreg{out});
    ASSERT_EQ(slowCode[3].op, Vinstr::ret);
  }
}

void testSinkDefsKeepsDeadSFDefsOutOfLiveSFRanges() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const target = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vt(unit, target);

    auto const src = Vreg64{rarg(0)};
    auto const shifted = Vreg64{v.makeReg()};
    auto const deadSf = v.makeReg();
    auto const liveSf = v.makeReg();
    auto const condition = Vreg8{vt.makeReg()};
    auto const out = Vreg64{vt.makeReg()};

    v << shrqi{Immed{5}, src, shifted, deadSf, Vflags{}};
    v << testqi{Immed{1}, src, liveSf, Vflags{}};
    v << jmp{target};

    vt << setcc{CC_NZ, liveSf, condition};
    vt << copy{shifted, out};
    vt << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 3);
    ASSERT_EQ(entryCode[0].op, Vinstr::shrqi);
    EXPECT_EQ(entryCode[0].shrqi_.d, shifted);
    ASSERT_EQ(entryCode[1].op, Vinstr::testqi);
    EXPECT_EQ(entryCode[1].testqi_.sf, liveSf);
    ASSERT_EQ(entryCode[2].op, Vinstr::jmp);
    EXPECT_EQ(entryCode[2].jmp_.target, target);

    auto const& targetCode = unit.blocks[target].code;
    ASSERT_EQ(targetCode.size(), 3);
    ASSERT_EQ(targetCode[0].op, Vinstr::setcc);
    EXPECT_EQ(targetCode[0].setcc_.sf, liveSf);
    ASSERT_EQ(targetCode[1].op, Vinstr::copy);
    EXPECT_EQ(targetCode[1].copy_.s, Vreg{shifted});
    EXPECT_EQ(targetCode[1].copy_.d, Vreg{out});
    ASSERT_EQ(targetCode[2].op, Vinstr::ret);
  }
}

struct PureLoadSinkLinearContext {
  Vunit& unit;
  Vlabel mid;
  Vlabel exit;
  Vreg64 base;
  Vreg64 cand;
};

template<class MakeLoadOrigin, class EmitMid, class Verify>
void testSinkDefsPureLoadLinear(MakeLoadOrigin makeLoadOrigin,
                                EmitMid emitMid,
                                Verify verify) {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLoadOrigin(irUnit, bcctx, fp);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const mid = unit.makeBlock(AreaIndex::Main, 1);
    auto const exit = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vm(unit, mid);
    Vout ve(unit, exit);

    auto const base = Vreg64{v.makeReg()};
    auto const keepBase = Vreg64{v.makeReg()};
    auto const cand = Vreg64{v.makeReg()};
    auto const out = Vreg64{ve.makeReg()};

    v << ldimmq{Immed64{0x100}, base};
    v.setOrigin(loadOrigin);
    v << load{base[0], cand};
    v.setOrigin(nullptr);
    v << copy{base, keepBase};
    v << jmp{mid};

    emitMid(irUnit, bcctx, fp, vm, base, exit);

    ve << copy{cand, out};
    ve << ret{};

    sinkDefs(unit, abi(kind));

    verify(PureLoadSinkLinearContext{unit, mid, exit, base, cand});
  }
}

template<class EmitMid, class Verify>
void testSinkDefsLocalPureLoadLinear(uint32_t loadLoc,
                                     EmitMid emitMid,
                                     Verify verify) {
  testSinkDefsPureLoadLinear(
    [=] (IRUnit& irUnit, BCContext bcctx, SSATmp* fp) {
      return makeLdLocOrigin(irUnit, bcctx, fp, loadLoc);
    },
    emitMid,
    verify
  );
}

std::optional<size_t> findLoadDef(const Vblock& block, Vreg def) {
  for (auto i = size_t{0}; i < block.code.size(); ++i) {
    auto const& inst = block.code[i];
    if (inst.op == Vinstr::load && inst.load_.d == def) return i;
  }
  return std::nullopt;
}

std::optional<size_t> findUse(const Vunit& unit,
                              const Vblock& block,
                              Vreg use,
                              size_t begin = 0) {
  for (auto i = begin; i < block.code.size(); ++i) {
    auto found = false;
    visitUses(unit, block.code[i], [&] (Vreg r) {
      if (r == use) found = true;
    });
    if (found) return i;
  }
  return std::nullopt;
}

size_t countLoadDefs(const Vunit& unit, Vreg def) {
  auto count = size_t{0};
  for (auto const& block : unit.blocks) {
    for (auto const& inst : block.code) {
      if (inst.op == Vinstr::load && inst.load_.d == def) ++count;
    }
  }
  return count;
}

std::optional<size_t> expectOnlyLoadDefInBlock(const Vunit& unit,
                                               Vlabel block,
                                               Vreg def) {
  EXPECT_EQ(size_t{1}, countLoadDefs(unit, def))
    << stripWhitespace(show(unit));

  auto const pos = findLoadDef(unit.blocks[block], def);
  EXPECT_TRUE(pos.has_value()) << stripWhitespace(show(unit));
  return pos;
}

void expectPureLoadSunkToExit(const PureLoadSinkLinearContext& ctx) {
  auto const cand = Vreg{ctx.cand};
  auto const& entry = ctx.unit.blocks[ctx.unit.entry];
  auto const& exit = ctx.unit.blocks[ctx.exit];

  EXPECT_FALSE(findLoadDef(entry, cand).has_value())
    << stripWhitespace(show(ctx.unit));

  auto const exitLoad = expectOnlyLoadDefInBlock(ctx.unit, ctx.exit, cand);
  ASSERT_TRUE(exitLoad.has_value()) << stripWhitespace(show(ctx.unit));
  ASSERT_TRUE(findUse(ctx.unit, exit, cand, *exitLoad + 1).has_value())
    << stripWhitespace(show(ctx.unit));
}

void expectPureLoadKeptInEntry(const PureLoadSinkLinearContext& ctx) {
  auto const cand = Vreg{ctx.cand};
  auto const& exit = ctx.unit.blocks[ctx.exit];

  ASSERT_TRUE(expectOnlyLoadDefInBlock(ctx.unit, ctx.unit.entry, cand)
                .has_value()) << stripWhitespace(show(ctx.unit));
  EXPECT_FALSE(findLoadDef(exit, cand).has_value())
    << stripWhitespace(show(ctx.unit));
  ASSERT_TRUE(findUse(ctx.unit, exit, cand).has_value())
    << stripWhitespace(show(ctx.unit));
}

void expectPureLoadSunkToMid(const PureLoadSinkLinearContext& ctx) {
  auto const cand = Vreg{ctx.cand};
  auto const& entry = ctx.unit.blocks[ctx.unit.entry];
  auto const& exit = ctx.unit.blocks[ctx.exit];

  EXPECT_FALSE(findLoadDef(entry, cand).has_value())
    << stripWhitespace(show(ctx.unit));
  ASSERT_TRUE(expectOnlyLoadDefInBlock(ctx.unit, ctx.mid, cand).has_value())
    << stripWhitespace(show(ctx.unit));
  EXPECT_FALSE(findLoadDef(exit, cand).has_value())
    << stripWhitespace(show(ctx.unit));
  ASSERT_TRUE(findUse(ctx.unit, exit, cand).has_value())
    << stripWhitespace(show(ctx.unit));
}

void testSinkDefsPureLoadSinksWithPhysicalBase() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const exit = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout ve(unit, exit);

    auto const cand = Vreg64{v.makeReg()};
    auto const out = Vreg64{ve.makeReg()};

    v.setOrigin(loadOrigin);
    v << load{rvmfp()[0], cand};
    v.setOrigin(nullptr);
    v << jmp{exit};

    ve << copy{cand, out};
    ve << ret{};

    sinkDefs(unit, abi(kind));

    EXPECT_FALSE(findLoadDef(unit.blocks[unit.entry], cand).has_value())
      << stripWhitespace(show(unit));
    auto const exitLoad = expectOnlyLoadDefInBlock(unit, exit, cand);
    ASSERT_TRUE(exitLoad.has_value()) << stripWhitespace(show(unit));
    ASSERT_TRUE(findUse(unit, unit.blocks[exit], cand, *exitLoad + 1).has_value())
      << stripWhitespace(show(unit));
  }
}

void testSinkDefsPureLoadStopsBeforeExplicitPhysicalClobber() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const mid = unit.makeBlock(AreaIndex::Main, 1);
    auto const exit = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vm(unit, mid);
    Vout ve(unit, exit);

    auto const cand = Vreg64{v.makeReg()};
    auto const out = Vreg64{ve.makeReg()};

    v.setOrigin(loadOrigin);
    v << load{rvmfp()[0], cand};
    v.setOrigin(nullptr);
    v << jmp{mid};

    vm << copy{rvmsp(), rvmfp()};
    vm << jmp{exit};

    ve << copy{cand, out};
    ve << ret{};

    sinkDefs(unit, abi(kind));

    EXPECT_FALSE(findLoadDef(unit.blocks[unit.entry], cand).has_value())
      << stripWhitespace(show(unit));
    auto const midLoad = expectOnlyLoadDefInBlock(unit, mid, cand);
    ASSERT_TRUE(midLoad.has_value()) << stripWhitespace(show(unit));
    ASSERT_LT(*midLoad + 1, unit.blocks[mid].code.size());
    EXPECT_EQ(unit.blocks[mid].code[*midLoad + 1].op, Vinstr::copy);
    EXPECT_FALSE(findLoadDef(unit.blocks[exit], cand).has_value())
      << stripWhitespace(show(unit));
  }
}

void testSinkDefsPureLoadStopsBeforeImplicitPhysicalClobber() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const mid = unit.makeBlock(AreaIndex::Main, 1);
    auto const exit = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vm(unit, mid);
    Vout ve(unit, exit);

    auto const cand = Vreg64{v.makeReg()};
    auto const out = Vreg64{ve.makeReg()};

    v.setOrigin(loadOrigin);
    v << load{rarg(0)[0], cand};
    v.setOrigin(nullptr);
    v << jmp{mid};

    vm << call{TCA(sinkDefsTestHelper), RegSet{}};
    vm << jmp{exit};

    ve << copy{cand, out};
    ve << ret{};

    sinkDefs(unit, abi(kind));

    EXPECT_FALSE(findLoadDef(unit.blocks[unit.entry], cand).has_value())
      << stripWhitespace(show(unit));
    auto const midLoad = expectOnlyLoadDefInBlock(unit, mid, cand);
    ASSERT_TRUE(midLoad.has_value()) << stripWhitespace(show(unit));
    ASSERT_LT(*midLoad + 1, unit.blocks[mid].code.size());
    EXPECT_EQ(unit.blocks[mid].code[*midLoad + 1].op, Vinstr::call);
    EXPECT_FALSE(findLoadDef(unit.blocks[exit], cand).has_value())
      << stripWhitespace(show(unit));
  }
}

void testSinkDefsPureLoadSinksAcrossUnrelatedStoreOrigin() {
  testSinkDefsLocalPureLoadLinear(
    0,
    [] (IRUnit& irUnit,
        BCContext bcctx,
        SSATmp* fp,
        Vout& vm,
        Vreg64 base,
        Vlabel exit) {
      auto const storeOrigin = makeStLocOrigin(irUnit, bcctx, fp, 1);
      auto const tmp = Vreg64{vm.makeReg()};
      vm.setOrigin(storeOrigin);
      vm << copy{base, tmp};
      vm.setOrigin(nullptr);
      vm << jmp{exit};
    },
    [] (const PureLoadSinkLinearContext& ctx) {
      expectPureLoadSunkToExit(ctx);

      auto const& midCode = ctx.unit.blocks[ctx.mid].code;
      ASSERT_EQ(midCode.size(), 2);
      ASSERT_EQ(midCode[0].op, Vinstr::copy);
      ASSERT_EQ(midCode[1].op, Vinstr::jmp);
    }
  );
}

void testSinkDefsPureLoadSinksAfterUserMoves() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const exit = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout ve(unit, exit);

    auto const base = Vreg64{v.makeReg()};
    auto const keepBase = Vreg64{v.makeReg()};
    auto const cand = Vreg64{v.makeReg()};
    auto const copied = Vreg64{v.makeReg()};
    auto const out = Vreg64{ve.makeReg()};

    v << ldimmq{Immed64{0x100}, base};
    v.setOrigin(loadOrigin);
    v << load{base[0], cand};
    v.setOrigin(nullptr);
    v << copy{base, keepBase};
    v << copy{cand, copied};
    v << jmp{exit};

    ve << copy{copied, out};
    ve << ret{};

    sinkDefs(unit, abi(kind));

    auto const candReg = Vreg{cand};
    auto const& entry = unit.blocks[unit.entry];
    auto const& exitBlock = unit.blocks[exit];

    EXPECT_FALSE(findLoadDef(entry, candReg).has_value())
      << stripWhitespace(show(unit));
    ASSERT_EQ(size_t{1}, countLoadDefs(unit, candReg))
      << stripWhitespace(show(unit));

    auto const exitLoad = findLoadDef(exitBlock, candReg);
    ASSERT_TRUE(exitLoad.has_value()) << stripWhitespace(show(unit));

    auto const movedUser = findUse(unit, exitBlock, candReg, *exitLoad + 1);
    ASSERT_TRUE(movedUser.has_value()) << stripWhitespace(show(unit));
    ASSERT_EQ(exitBlock.code[*movedUser].op, Vinstr::copy);
    EXPECT_EQ(exitBlock.code[*movedUser].copy_.s, candReg);
    EXPECT_EQ(exitBlock.code[*movedUser].copy_.d, Vreg{copied});
  }
}

void testSinkDefsPureLoadStopsBeforeClobberingStore() {
  testSinkDefsLocalPureLoadLinear(
    0,
    [] (IRUnit& irUnit,
        BCContext bcctx,
        SSATmp* fp,
        Vout& vm,
        Vreg64 base,
        Vlabel exit) {
      auto const storeOrigin = makeStLocOrigin(irUnit, bcctx, fp, 0);
      auto const storeVal = Vreg64{vm.makeReg()};
      vm << ldimmq{Immed64{7}, storeVal};
      vm.setOrigin(storeOrigin);
      vm << store{storeVal, base[0]};
      vm.setOrigin(nullptr);
      vm << jmp{exit};
    },
    [] (const PureLoadSinkLinearContext& ctx) {
      expectPureLoadSunkToMid(ctx);

      auto const& midCode = ctx.unit.blocks[ctx.mid].code;
      ASSERT_EQ(midCode.size(), 4);
      ASSERT_EQ(midCode[0].op, Vinstr::load);
      EXPECT_EQ(midCode[0].load_.d, Vreg{ctx.cand});
      ASSERT_EQ(midCode[1].op, Vinstr::ldimmq);
      ASSERT_EQ(midCode[2].op, Vinstr::store);
      ASSERT_EQ(midCode[3].op, Vinstr::jmp);
    }
  );
}

void testSinkDefsPureLoadStopsBeforeEnterInlineFrame() {
  testSinkDefsPureLoadLinear(
    [] (IRUnit& irUnit, BCContext bcctx, SSATmp*) {
      return makeLdStkOrigin(irUnit, bcctx, IRSPRelOffset{0});
    },
    [] (IRUnit& irUnit,
        BCContext bcctx,
        SSATmp*,
        Vout& vm,
        Vreg64,
        Vlabel exit) {
      auto const enterOrigin = makeEnterInlineFrameOrigin(irUnit, bcctx);
      vm.setOrigin(enterOrigin);
      vm << inlinestart{};
      vm.setOrigin(nullptr);
      vm << jmp{exit};
    },
    [] (const PureLoadSinkLinearContext& ctx) {
      expectPureLoadSunkToMid(ctx);

      auto const& midCode = ctx.unit.blocks[ctx.mid].code;
      ASSERT_EQ(midCode.size(), 3);
      ASSERT_EQ(midCode[0].op, Vinstr::load);
      EXPECT_EQ(midCode[0].load_.d, Vreg{ctx.cand});
      ASSERT_EQ(midCode[1].op, Vinstr::inlinestart);
      ASSERT_EQ(midCode[2].op, Vinstr::jmp);
    }
  );
}

void testSinkDefsPureLoadStopsAtSelfLoopClobber() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);
    auto const storeOrigin = makeStLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const loop = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vl(unit, loop);

    auto const base = Vreg64{v.makeReg()};
    auto const cand = Vreg64{v.makeReg()};
    auto const storeVal = Vreg64{vl.makeReg()};
    auto const out = Vreg64{vl.makeReg()};

    v << ldimmq{Immed64{0x100}, base};
    v.setOrigin(loadOrigin);
    v << load{base[0], cand};
    v.setOrigin(nullptr);
    v << jmp{loop};

    vl << ldimmq{Immed64{7}, storeVal};
    vl.setOrigin(storeOrigin);
    vl << store{storeVal, base[0]};
    vl.setOrigin(nullptr);
    vl << copy{cand, out};
    vl << jmp{loop};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 3);
    ASSERT_EQ(entryCode[0].op, Vinstr::ldimmq);
    ASSERT_EQ(entryCode[1].op, Vinstr::load);
    EXPECT_EQ(entryCode[1].load_.d, Vreg{cand});
    ASSERT_EQ(entryCode[2].op, Vinstr::jmp);

    auto const& loopCode = unit.blocks[loop].code;
    ASSERT_EQ(loopCode.size(), 4);
    ASSERT_EQ(loopCode[0].op, Vinstr::ldimmq);
    ASSERT_EQ(loopCode[1].op, Vinstr::store);
    ASSERT_EQ(loopCode[2].op, Vinstr::copy);
    EXPECT_EQ(loopCode[2].copy_.s, Vreg{cand});
    ASSERT_EQ(loopCode[3].op, Vinstr::jmp);
    EXPECT_EQ(loopCode[3].jmp_.target, loop);
  }
}

void testSinkDefsPureLoadStopsAtClobberingBranchPath() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);
    auto const storeOrigin = makeStLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const left = unit.makeBlock(AreaIndex::Main, 1);
    auto const right = unit.makeBlock(AreaIndex::Main, 1);
    auto const merge = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vl(unit, left);
    Vout vr(unit, right);
    Vout vm(unit, merge);

    auto const base = Vreg64{v.makeReg()};
    auto const cand = Vreg64{v.makeReg()};
    auto const sf = v.makeReg();
    auto const storeVal = Vreg64{vl.makeReg()};
    auto const out = Vreg64{vm.makeReg()};

    v << ldimmq{Immed64{0x100}, base};
    v.setOrigin(loadOrigin);
    v << load{base[0], cand};
    v.setOrigin(nullptr);
    v << cmpqi{Immed{0}, Vreg64{Reg64{0}}, sf, Vflags{}};
    v << jcc{CC_E, sf, {left, right}, StringTag{}};

    vl << ldimmq{Immed64{7}, storeVal};
    vl.setOrigin(storeOrigin);
    vl << store{storeVal, base[0]};
    vl.setOrigin(nullptr);
    vl << jmp{merge};

    vr << jmp{merge};

    vm << copy{cand, out};
    vm << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 4);
    ASSERT_EQ(entryCode[0].op, Vinstr::ldimmq);
    ASSERT_EQ(entryCode[1].op, Vinstr::load);
    EXPECT_EQ(entryCode[1].load_.d, Vreg{cand});
    ASSERT_EQ(entryCode[2].op, Vinstr::cmpqi);
    ASSERT_EQ(entryCode[3].op, Vinstr::jcc);

    auto const& mergeCode = unit.blocks[merge].code;
    ASSERT_EQ(mergeCode.size(), 2);
    ASSERT_EQ(mergeCode[0].op, Vinstr::copy);
    EXPECT_EQ(mergeCode[0].copy_.s, Vreg{cand});
    ASSERT_EQ(mergeCode[1].op, Vinstr::ret);
  }
}

void testSinkDefsPureLoadStopsBeforeClobberingBlockEnd() {
  for (auto const kind : {CodeKind::Trace, CodeKind::Prologue}) {
    SCOPED_TRACE(kind == CodeKind::Trace ? "trace_abi" : "prologue_abi");

    IRUnit irUnit{test_context};
    auto const bcctx = BCContext{test_marker(), 0};
    auto const fp = irUnit.gen(DefFP, bcctx, DefFPData{std::nullopt})->dst();
    auto const loadOrigin = makeLdLocOrigin(irUnit, bcctx, fp, 0);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    auto const mid = unit.makeBlock(AreaIndex::Main, 1);
    auto const done = unit.makeBlock(AreaIndex::Main, 1);
    auto const catchBlock = unit.makeBlock(AreaIndex::Main, 1);

    Vout v(unit, unit.entry);
    Vout vm(unit, mid);
    Vout vd(unit, done);
    Vout vc(unit, catchBlock);

    auto const base = Vreg64{v.makeReg()};
    auto const cand = Vreg64{v.makeReg()};
    auto const out = Vreg64{vd.makeReg()};

    v << ldimmq{Immed64{0x100}, base};
    v.setOrigin(loadOrigin);
    v << load{base[0], cand};
    v.setOrigin(nullptr);
    v << jmp{mid};

    vm << vinvoke{
      CallSpec::direct(sinkDefsTestHelper),
      vm.makeVcallArgs({{}}),
      vm.makeTuple({}),
      {done, catchBlock},
      Fixup::none(),
      DestType::None
    };

    vd << copy{cand, out};
    vd << ret{};

    vc << ret{};

    sinkDefs(unit, abi(kind));

    auto const& entryCode = unit.blocks[unit.entry].code;
    ASSERT_EQ(entryCode.size(), 1);
    ASSERT_EQ(entryCode[0].op, Vinstr::jmp);

    auto const& midCode = unit.blocks[mid].code;
    ASSERT_EQ(midCode.size(), 3);
    ASSERT_EQ(midCode[0].op, Vinstr::ldimmq);
    EXPECT_EQ(midCode[0].ldimmq_.d, base);
    ASSERT_EQ(midCode[1].op, Vinstr::load);
    EXPECT_EQ(midCode[1].load_.d, Vreg{cand});
    ASSERT_EQ(midCode[2].op, Vinstr::vinvoke);

    auto const& doneCode = unit.blocks[done].code;
    ASSERT_EQ(doneCode.size(), 2);
    ASSERT_EQ(doneCode[0].op, Vinstr::copy);
    EXPECT_EQ(doneCode[0].copy_.s, Vreg{cand});
    ASSERT_EQ(doneCode[1].op, Vinstr::ret);
  }
}

}

TEST(Vasm, Simplifier) {
  testSetccXor();
  testPostRACopyFold();
  testSinkDefsMovesIntoMergeAfterPhidef();
  testSinkDefsKeepsJoinPointDefsInPlace();
  testSinkDefsKeepsDefsOutOfHotterBlocks();
  testSinkDefsFallsBackToLegalDominator();
  testSinkDefsMovesDefsWithDeadSF();
  testSinkDefsKeepsDeadSFDefsOutOfLiveSFRanges();
  testSinkDefsPureLoadSinksWithPhysicalBase();
  testSinkDefsPureLoadStopsBeforeExplicitPhysicalClobber();
  testSinkDefsPureLoadStopsBeforeImplicitPhysicalClobber();
  testSinkDefsPureLoadSinksAcrossUnrelatedStoreOrigin();
  testSinkDefsPureLoadSinksAfterUserMoves();
  testSinkDefsPureLoadStopsBeforeClobberingStore();
  testSinkDefsPureLoadStopsBeforeEnterInlineFrame();
  testSinkDefsPureLoadStopsAtSelfLoopClobber();
  testSinkDefsPureLoadStopsAtClobberingBranchPath();
  testSinkDefsPureLoadStopsBeforeClobberingBlockEnd();
}


void testLoadElimIncRef() {
  // IncRef pattern (single hop): loadl in predecessor feeds a cmpli+jcc,
  // successor block redundantly loads from the same address.
  //
  // B0: loadl [base+off] => r1; cmpli 0, r1 => sf; jcc GE => B1, B2
  // B1: loadl [base+off] => r2; incl r2 => r3, sf2; storel r3 [base+off]; ret
  // B2: ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b2 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);
    Vout v2(unit, b2);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const sf = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << cmpli{0, Vreg32{r1}, sf};
    v0 << jcc{CC_GE, sf, {b1, b2}, StringTag{}};

    auto const r2 = v1.makeReg();
    auto const r3 = v1.makeReg();
    auto const sf2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << incl{Vreg32{r2}, Vreg32{r3}, sf2};
    v1 << storel{Vreg32{r3}, Vreg64{base}[0x10]};
    v1 << ret{};

    v2 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 4u);
    EXPECT_EQ(code1[0].op, Vinstr::movl)
      << "redundant loadl should be replaced with a zero-extending movl, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code1[0].movl_.s}, Vreg{r1});
    EXPECT_EQ(Vreg{code1[0].movl_.d}, Vreg{r2});
  }
}

void testLoadElimDecRef() {
  // DecRef pattern (two hops): loadl in B0 feeds cmpli+jcc, then B1 re-uses
  // sf for another jcc, and B2 (two blocks away) redundantly loads from the
  // same address.
  //
  // B0: loadl [base+off] => r1; cmpli 1, r1 => sf; jcc NE => B1, B3
  // B1: jcc NL, sf => B2, B4
  // B2: loadl [base+off] => r2; decl r2 => r3, sf2; storel r3 [base+off]; ret
  // B3: ret   (destroy path)
  // B4: ret   (static path)
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b2 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b3 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b4 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);
    Vout v2(unit, b2);
    Vout v3(unit, b3);
    Vout v4(unit, b4);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const sf = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << cmpli{1, Vreg32{r1}, sf};
    v0 << jcc{CC_NE, sf, {b1, b3}, StringTag{}};

    v1 << jcc{CC_NL, sf, {b2, b4}, StringTag{}};

    auto const r2 = v2.makeReg();
    auto const r3 = v2.makeReg();
    auto const sf2 = v2.makeReg();
    v2 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v2 << decl{Vreg32{r2}, Vreg32{r3}, sf2};
    v2 << storel{Vreg32{r3}, Vreg64{base}[0x10]};
    v2 << ret{};

    v3 << ret{};
    v4 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code2 = unit.blocks[b2].code;
    ASSERT_GE(code2.size(), 4u);
    EXPECT_EQ(code2[0].op, Vinstr::movl)
      << "redundant loadl should be replaced with a zero-extending movl, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code2[0].movl_.s}, Vreg{r1});
    EXPECT_EQ(Vreg{code2[0].movl_.d}, Vreg{r2});
  }
}

void testLoadElimStoreSupersedesPriorLoad() {
  // A store to the same address supersedes the value of a prior load: a later
  // load must be forwarded from the STORED value (rOther), never from the now
  // stale pre-store value (r1).
  //
  // B0: loadl [base+off] => r1; storel rOther [base+off]; jmp B1
  // B1: loadl [base+off] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const rOther = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << ldimmq{Immed64{99}, Vreg64{rOther}};
    v0 << storel{Vreg32{rOther}, Vreg64{base}[0x10]};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::movl)
      << "load after store should be forwarded (zero-extending) from the stored "
         "value, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code1[0].movl_.s}, Vreg{rOther})
      << "must forward the STORED value, not the stale prior load";
    EXPECT_NE(Vreg{code1[0].movl_.s}, Vreg{r1})
      << "must NOT forward the stale pre-store load value";
    EXPECT_EQ(Vreg{code1[0].movl_.d}, Vreg{r2});
  }
}

void testLoadElimStoreToLoadForwardL() {
  // Store-to-load forwarding (4-byte): a storel makes its value available to a
  // later loadl at the same address.
  //
  // B0: storel rVal [base+0x10]; loadl [base+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    auto const base = v0.makeReg();
    auto const rVal = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << ldimmq{Immed64{99}, Vreg64{rVal}};
    v0 << storel{Vreg32{rVal}, Vreg64{base}[0x10]};
    auto const r2 = v0.makeReg();
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v0 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    // 0:ldimmq 1:ldimmq 2:storel 3:(loadl->movl) 4:ret
    auto const& code = unit.blocks[b0].code;
    ASSERT_GE(code.size(), 5u);
    EXPECT_EQ(code[3].op, Vinstr::movl)
      << "loadl should be forwarded (zero-extending) from the prior storel, "
         "got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code[3].movl_.s}, Vreg{rVal});
    EXPECT_EQ(Vreg{code[3].movl_.d}, Vreg{r2});
  }
}

void testLoadElimStoreToLoadForwardQ() {
  // Store-to-load forwarding (8-byte): a store makes its value available to a
  // later load at the same address.
  //
  // B0: store rVal [base+0x10]; load [base+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    auto const base = v0.makeReg();
    auto const rVal = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << ldimmq{Immed64{99}, Vreg64{rVal}};
    v0 << store{Vreg64{rVal}, Vreg64{base}[0x10]};
    auto const r2 = v0.makeReg();
    v0 << load{Vreg64{base}[0x10], Vreg64{r2}};
    v0 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code = unit.blocks[b0].code;
    ASSERT_GE(code.size(), 5u);
    EXPECT_EQ(code[3].op, Vinstr::copy)
      << "load should be forwarded from the prior store, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(code[3].copy_.s, Vreg{rVal});
    EXPECT_EQ(code[3].copy_.d, Vreg{r2});
  }
}

void testLoadElimDisjointStoreSurvives() {
  // Precise alias-based kill: a store to a provably-disjoint address (same
  // base, non-overlapping displacement) must NOT kill an available load. The
  // later load from the original address is still forwarded.
  //
  // B0: loadl [base+0x10] => r1; storel rOther [base+0x20]; jmp B1
  // B1: loadl [base+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const rOther = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << ldimmq{Immed64{99}, Vreg64{rOther}};
    v0 << storel{Vreg32{rOther}, Vreg64{base}[0x20]};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::movl)
      << "load should survive a disjoint store and be forwarded "
         "(zero-extending), got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code1[0].movl_.s}, Vreg{r1});
    EXPECT_EQ(Vreg{code1[0].movl_.d}, Vreg{r2});
  }
}

void testLoadElimOverlappingStoreKills() {
  // A store whose range overlaps an available load (same base, disp 0x12 with
  // width 4 overlaps [0x10, 0x14)) must kill it. The later load is NOT
  // forwarded.
  //
  // B0: loadl [base+0x10] => r1; storel rOther [base+0x12]; jmp B1
  // B1: loadl [base+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const rOther = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << ldimmq{Immed64{99}, Vreg64{rOther}};
    v0 << storel{Vreg32{rOther}, Vreg64{base}[0x12]};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::loadl)
      << "load overlapping a store must NOT be forwarded, got: "
      << stripWhitespace(show(unit));
  }
}

void testLoadElimKilledByBaseRedef() {
  // If the base register is redefined between the load and a successor block's
  // load, the second load must NOT be eliminated. Use a physical register so
  // we can legally redefine it (virtual regs are SSA).
  //
  // B0: loadl [base+off] => r1; base = new_val; jmp B1
  // B1: loadl [base+off] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = Vreg{Reg64{0}};
    auto const r1 = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << ldimmq{Immed64{0x2000}, Vreg64{base}};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::loadl)
      << "load after base redef should NOT be eliminated, got: "
      << stripWhitespace(show(unit));
  }
}

void testLoadElimWiderReadUsesZeroExtend() {
  // Regression: a forwarded 4-byte value may be read at full 64-bit width by a
  // later consumer. checkWidths permits reading a loadl's dest as a Vreg64
  // because loadl defines the whole register, zero-extending bits [32,64). The
  // forward must preserve that: a 32-bit-semantics `copy` would leak the source
  // reg's upper half into the wider read. The rewrite must be a zero-extending
  // `movl`, never a `copy`.
  //
  // B0: loadl [base+0x10] => r1; jmp B1
  // B1: loadl [base+0x10] => r2; testqi imm, (Vreg64)r2 => sf; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    auto const sf = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    // Wider read of the 4-byte value: depends on the upper half being zero.
    v1 << testqi{Immed{0x12345678}, Vreg64{r2}, sf};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 3u);
    EXPECT_EQ(code1[0].op, Vinstr::movl)
      << "a loadl forwarded to a wider read must use a zero-extending movl, "
         "not a copy, got: "
      << stripWhitespace(show(unit));
    EXPECT_NE(code1[0].op, Vinstr::copy)
      << "a copy would leak the source reg's upper bits to the 64-bit read";
    EXPECT_EQ(Vreg{code1[0].movl_.s}, Vreg{r1});
    EXPECT_EQ(Vreg{code1[0].movl_.d}, Vreg{r2});
  }
}

TEST(Vasm, LoadElimWiderReadUsesZeroExtend) {
  testLoadElimWiderReadUsesZeroExtend();
}

TEST(Vasm, LoadElimIncRef) {
  testLoadElimIncRef();
}

TEST(Vasm, LoadElimDecRef) {
  testLoadElimDecRef();
}

TEST(Vasm, LoadElimStoreSupersedesPriorLoad) {
  testLoadElimStoreSupersedesPriorLoad();
}

TEST(Vasm, LoadElimStoreToLoadForwardL) {
  testLoadElimStoreToLoadForwardL();
}

TEST(Vasm, LoadElimStoreToLoadForwardQ) {
  testLoadElimStoreToLoadForwardQ();
}

TEST(Vasm, LoadElimDisjointStoreSurvives) {
  testLoadElimDisjointStoreSurvives();
}

TEST(Vasm, LoadElimOverlappingStoreKills) {
  testLoadElimOverlappingStoreKills();
}

TEST(Vasm, LoadElimKilledByBaseRedef) {
  testLoadElimKilledByBaseRedef();
}

void testLoadElimKilledByRegReuse() {
  // When a load's destination register is reused by a subsequent load from a
  // DIFFERENT address, the old entry must be invalidated. A later load from
  // the first address must NOT be replaced with a copy from the now-stale reg.
  //
  // B0: loadl [base+0x10] => r1; loadl [base+0x20] => r1; jmp B1
  // B1: loadl [base+0x10] => r2; ret
  //
  // r1 now holds the value from [base+0x20], so B1's load from [base+0x10]
  // must NOT be replaced with movl r1 => r2.
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = Vreg{Reg64{5}};

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << loadl{Vreg64{base}[0x10], Vreg32{r1}};
    v0 << loadl{Vreg64{base}[0x20], Vreg32{r1}};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::loadl)
      << "load after reg reuse from different addr should NOT be eliminated, "
         "got: " << stripWhitespace(show(unit));
  }
}

TEST(Vasm, LoadElimKilledByRegReuse) {
  testLoadElimKilledByRegReuse();
}

void testLoadElimKilledByWidthMismatch() {
  // A 64-bit `load` and a 32-bit `loadl` from the SAME address must not be
  // forwarded to each other: the access width is part of the load's meaning,
  // and the 8-byte value is not a valid substitute for a 4-byte (zero-extended)
  // load, nor vice versa.
  //
  // B0: load  [base+0x10] => r1 (8 bytes); jmp B1
  // B1: loadl [base+0x10] => r2 (4 bytes); ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << load{Vreg64{base}[0x10], Vreg64{r1}};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << loadl{Vreg64{base}[0x10], Vreg32{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::loadl)
      << "loadl must NOT be forwarded from a wider load at the same address, "
         "got: " << stripWhitespace(show(unit));
  }
}

void testLoadElimKilledByDstAliasesBase() {
  // When a load's destination register IS its own base register, the load
  // overwrites the address it was computed from. The recorded availability
  // entry would be self-referentially stale, so a later load from the same
  // textual address must NOT be forwarded. Use a physical register since a
  // virtual (SSA) dest can never equal an existing base.
  //
  // B0: load [base+0x10] => base (dst == base); jmp B1
  // B1: load [base+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);

    auto const base = Vreg{Reg64{0}};

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << load{Vreg64{base}[0x10], Vreg64{base}};
    v0 << jmp{b1};

    auto const r2 = v1.makeReg();
    v1 << load{Vreg64{base}[0x10], Vreg64{r2}};
    v1 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::load)
      << "load whose dest aliased its base must NOT be forwarded, "
         "got: " << stripWhitespace(show(unit));
  }
}

void testLoadElimKilledByImplicitRegDef() {
  // popm{[rsp+0x40]} writes memory that is provably disjoint from the tracked
  // address [rsp+0x10], so the alias-based kill correctly spares the entry --
  // but popm also implicitly increments rsp, which re-points the entry's own
  // base. That def appears only in getEffects(), never in visitDefs(), so
  // without consulting getEffects() the second load is forwarded from a value
  // read at a different address.
  //
  // B0: load [rsp+0x10] => r1; popm [rsp+0x40]; load [rsp+0x10] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);

    auto const r1 = v0.makeReg();
    auto const r2 = v0.makeReg();

    v0 << load{rsp()[0x10], Vreg64{r1}};
    v0 << popm{rsp()[0x40]};
    v0 << load{rsp()[0x10], Vreg64{r2}};
    v0 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code0 = unit.blocks[b0].code;
    ASSERT_GE(code0.size(), 4u);
    EXPECT_EQ(code0[2].op, Vinstr::load)
      << "popm implicitly redefines rsp, so an rsp-relative entry must not "
         "survive it, got: " << stripWhitespace(show(unit));
  }
}

void testLoadElimKilledByImplicitMemWrite() {
  // push{} writes [rsp-8] with no Vptr operand at all, so writesMemory() -- and
  // therefore the precise alias kill -- never sees it. Here the tracked entry
  // is keyed on a *virtual* alias of that slot, so killing rsp-keyed entries is
  // not enough either: only treating an unrecognized effectful instruction as
  // an unknown memory writer keeps this sound.
  //
  // B0: lea [rsp-0x8] => vp; load [vp] => r1; push r0; load [vp] => r2; ret
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);

    auto const vp = v0.makeReg();
    auto const r0 = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const r2 = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{r0}};
    v0 << lea{rsp()[-0x8], Vreg64{vp}};
    v0 << load{Vreg64{vp}[0], Vreg64{r1}};
    v0 << push{Vreg64{r0}};
    v0 << load{Vreg64{vp}[0], Vreg64{r2}};
    v0 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code0 = unit.blocks[b0].code;
    ASSERT_GE(code0.size(), 6u);
    EXPECT_EQ(code0[4].op, Vinstr::load)
      << "push writes [rsp-8] without a Vptr operand, so it must invalidate "
         "availability, got: " << stripWhitespace(show(unit));
  }
}

void testLoadElimPrefersDominatingValue() {
  // After forwarding, the value lives in both the original register and the
  // load's dest. Keeping the *original* matters at merges: mergeAvailable()
  // only keeps entries that map to the identical Vreg in every predecessor, so
  // re-keying the entry to the dest inside one arm of a diamond makes the two
  // arms disagree and the entry dies at the join.
  //
  // B0: load [base+0x10] => r1; cmpqi 0, r1 => sf; jcc NE => B1, B2
  // B1: load [base+0x10] => r2; jmp B3      (forwarded from r1)
  // B2: jmp B3
  // B3: load [base+0x10] => r3; ret         (must still be forwarded from r1)
  {
    Vunit unit;
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b1 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b2 = unit.makeBlock(AreaIndex::Main, 1);
    auto const b3 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;

    Vout v0(unit, b0);
    Vout v1(unit, b1);
    Vout v2(unit, b2);
    Vout v3(unit, b3);

    auto const base = v0.makeReg();
    auto const r1 = v0.makeReg();
    auto const sf = v0.makeReg();

    v0 << ldimmq{Immed64{0x1000}, Vreg64{base}};
    v0 << load{Vreg64{base}[0x10], Vreg64{r1}};
    v0 << cmpqi{0, Vreg64{r1}, sf};
    v0 << jcc{CC_NE, sf, {b1, b2}, StringTag{}};

    auto const r2 = v1.makeReg();
    v1 << load{Vreg64{base}[0x10], Vreg64{r2}};
    v1 << jmp{b3};

    v2 << jmp{b3};

    auto const r3 = v3.makeReg();
    v3 << load{Vreg64{base}[0x10], Vreg64{r3}};
    v3 << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));

    auto const& code1 = unit.blocks[b1].code;
    ASSERT_GE(code1.size(), 2u);
    EXPECT_EQ(code1[0].op, Vinstr::copy)
      << "redundant load in the diamond arm should be forwarded, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code1[0].copy_.s}, Vreg{r1});

    auto const& code3 = unit.blocks[b3].code;
    ASSERT_GE(code3.size(), 2u);
    EXPECT_EQ(code3[0].op, Vinstr::copy)
      << "the merge block must still see the value available: the arm that "
         "forwarded should keep tracking the dominating register, got: "
      << stripWhitespace(show(unit));
    EXPECT_EQ(Vreg{code3[0].copy_.s}, Vreg{r1});
    EXPECT_EQ(Vreg{code3[0].copy_.d}, Vreg{r3});
  }
}

TEST(Vasm, LoadElimKilledByWidthMismatch) {
  testLoadElimKilledByWidthMismatch();
}

TEST(Vasm, LoadElimKilledByDstAliasesBase) {
  testLoadElimKilledByDstAliasesBase();
}

TEST(Vasm, LoadElimKilledByImplicitRegDef) {
  testLoadElimKilledByImplicitRegDef();
}

TEST(Vasm, LoadElimKilledByImplicitMemWrite) {
  testLoadElimKilledByImplicitMemWrite();
}

TEST(Vasm, LoadElimPrefersDominatingValue) {
  testLoadElimPrefersDominatingValue();
}

// Count loads (8- or 4-byte) that read from [base + disp], independent of how
// any verify check happens to be built.
size_t countLoadsFromDisp(const Vunit& unit, Vreg base, int32_t disp) {
  size_t n = 0;
  for (auto const& block : unit.blocks) {
    for (auto const& inst : block.code) {
      if (inst.op == Vinstr::load &&
          Vreg{inst.load_.s.base} == base && inst.load_.s.disp == disp) {
        ++n;
      }
      if (inst.op == Vinstr::loadl &&
          Vreg{inst.loadl_.s.base} == base && inst.loadl_.s.disp == disp) {
        ++n;
      }
    }
  }
  return n;
}

// Is there a copy (8-byte) or movl (4-byte) that forwards `src` into `dst`?
bool unitHasForward(const Vunit& unit, Vreg dst, Vreg src) {
  for (auto const& block : unit.blocks) {
    for (auto const& inst : block.code) {
      if (inst.op == Vinstr::copy &&
          Vreg{inst.copy_.d} == dst && Vreg{inst.copy_.s} == src) {
        return true;
      }
      if (inst.op == Vinstr::movl &&
          Vreg{inst.movl_.d} == dst && Vreg{inst.movl_.s} == src) {
        return true;
      }
    }
  }
  return false;
}

// Run a load-elim scenario through both shipping (verify off) and verify (on)
// and assert the *contract* of verify mode without inspecting how the check is
// implemented (no assumptions about branches, traps, cmov, stores, block
// structure, or instruction order):
//
//   - Semantics: whatever shipping forwards, verify also forwards — dst still
//     ends up holding the forwarded value.
//   - Detection coverage: for every load the pass forwards, verify keeps one
//     extra real read of that load's address, so a wrong forward is observable
//     at runtime. Where the pass forwards nothing, verify adds nothing.
//
// The re-read is the implementation-independent heart of the feature: to check a
// forward you must read the real memory. Asserting "one extra read per forwarded
// case, and none otherwise" is what proves the pass installs a check for every
// case it should — and only those — regardless of how the check is built.
//
// `build` emits the scenario and reports the candidate load's address
// (base + disp), its dst, and the value that should be forwarded into dst.
template <class Build>
void checkVerifyContract(const char* name, bool forwardable, Build build) {
  SCOPED_TRACE(name);

  struct Result { size_t reads; bool forwarded; bool valid; };
  auto run = [&](bool verifyOn) -> Result {
    auto const saved = Cfg::Jit::ArmLoadElimVerify;
    Cfg::Jit::ArmLoadElimVerify = verifyOn;
    SCOPE_EXIT { Cfg::Jit::ArmLoadElimVerify = saved; };

    Vunit unit;
    unit.context = test_context;  // verify only runs on real translations
    auto const b0 = unit.makeBlock(AreaIndex::Main, 1);
    unit.entry = b0;
    Vout v(unit, b0);

    Vreg base;
    int32_t disp = 0;
    Vreg dst;
    Vreg expected;
    build(v, base, disp, dst, expected);
    v << ret{};

    eliminateRedundantLoads(unit, abi(CodeKind::Trace));
    return Result{countLoadsFromDisp(unit, base, disp),
                  unitHasForward(unit, dst, expected),
                  check(unit)};
  };

  auto const off = run(false);
  auto const on = run(true);

  EXPECT_TRUE(off.valid) << name << ": shipping unit must be well-formed";
  EXPECT_TRUE(on.valid) << name << ": verified unit must be well-formed";

  if (forwardable) {
    EXPECT_TRUE(off.forwarded)
      << name << ": shipping should forward this redundant load";
    EXPECT_TRUE(on.forwarded)
      << name << ": verify must preserve the forward (dst keeps the value)";
    EXPECT_EQ(on.reads, off.reads + 1)
      << name << ": verify must keep exactly one extra read of the forwarded "
                 "address so a wrong forward is detectable";
  } else {
    EXPECT_FALSE(off.forwarded)
      << name << ": a non-redundant load must not be forwarded";
    EXPECT_FALSE(on.forwarded)
      << name << ": verify must not forward what shipping does not";
    EXPECT_EQ(on.reads, off.reads)
      << name << ": verify must add no check where nothing is forwarded";
  }
}

TEST(Vasm, LoadElimVerifyContract) {
  // Every scenario the pass forwards must, under verify, keep dst forwarded and
  // gain exactly one extra read of the forwarded address (the detection). The
  // non-forwardable scenario confirms verify stays silent when nothing is
  // forwarded. None of this inspects how the check is built.

  // 8-byte store-to-load forward.
  checkVerifyContract(
    "store-to-load forward (q)", /*forwardable=*/true,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = v.makeReg();
      auto const rVal = v.makeReg();
      auto const r2 = v.makeReg();
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << ldimmq{Immed64{99}, Vreg64{rVal}};
      v << store{Vreg64{rVal}, Vreg64{base}[0x10]};
      v << load{Vreg64{base}[0x10], Vreg64{r2}};
      disp = 0x10;
      dst = r2;
      expected = rVal;
    });

  // 8-byte redundant load (load CSE).
  checkVerifyContract(
    "load CSE (q)", /*forwardable=*/true,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = v.makeReg();
      auto const r1 = v.makeReg();
      auto const r2 = v.makeReg();
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << load{Vreg64{base}[0x10], Vreg64{r1}};
      v << load{Vreg64{base}[0x10], Vreg64{r2}};
      disp = 0x10;
      dst = r2;
      expected = r1;
    });

  // 4-byte redundant load.
  checkVerifyContract(
    "load CSE (l)", /*forwardable=*/true,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = v.makeReg();
      auto const r1 = v.makeReg();
      auto const r2 = v.makeReg();
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << loadl{Vreg64{base}[0x10], Vreg32{r1}};
      v << loadl{Vreg64{base}[0x10], Vreg32{r2}};
      disp = 0x10;
      dst = r2;
      expected = r1;
    });

  // Self-forward: reload into the same physical register (prev == dst).
  checkVerifyContract(
    "self-forward (q, physical dst)", /*forwardable=*/true,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = v.makeReg();
      auto const x3 = Vreg{Reg64{3}};
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << load{Vreg64{base}[0x10], Vreg64{x3}};
      v << load{Vreg64{base}[0x10], Vreg64{x3}};
      disp = 0x10;
      dst = x3;
      expected = x3;
    });

  // dst aliases its own base register (a pointer chase `load [x1+0x10] => x1`
  // made redundant by a prior load). Only reachable for physical dsts. The check
  // must reload before overwriting the base, or it would read the wrong address;
  // this confirms a check is installed for the case at all.
  checkVerifyContract(
    "dst aliases base (q, physical)", /*forwardable=*/true,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = Vreg{Reg64{1}};  // physical, so a load's dst can equal the base
      auto const r2 = v.makeReg();
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << load{Vreg64{base}[0x10], Vreg64{r2}};    // available: base+0x10 => r2
      v << load{Vreg64{base}[0x10], Vreg64{base}};  // dst == base
      disp = 0x10;
      dst = base;
      expected = r2;
    });

  // Not forwardable: an overlapping store kills the first load's entry and does
  // not itself provide a same-address value, so the second load is a genuine
  // read. Verify must add no check here.
  checkVerifyContract(
    "killed by overlapping store (not forwardable)", /*forwardable=*/false,
    [](Vout& v, Vreg& base, int32_t& disp, Vreg& dst, Vreg& expected) {
      base = v.makeReg();
      auto const r1 = v.makeReg();
      auto const rX = v.makeReg();
      auto const r2 = v.makeReg();
      v << ldimmq{Immed64{0x1000}, Vreg64{base}};
      v << load{Vreg64{base}[0x10], Vreg64{r1}};
      v << ldimmq{Immed64{7}, Vreg64{rX}};
      v << store{Vreg64{rX}, Vreg64{base}[0x14]};  // overlaps [0x10,0x18)
      v << load{Vreg64{base}[0x10], Vreg64{r2}};
      disp = 0x10;
      dst = r2;
      expected = r1;
    });
}

}
