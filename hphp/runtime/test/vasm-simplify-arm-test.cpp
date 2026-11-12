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

#ifdef __aarch64__

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/abi-arm.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/test/vasm-test-helpers.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/immed.h"

#include <folly/portability/GTest.h>

namespace HPHP::jit {

namespace {

Vptr128 ptr128(Vreg64 base, int32_t disp) {
  return Vptr128{base[disp]};
}

void expectStorePairUps(const Vinstr& inst,
                        Vreg128 s0,
                        Vreg128 s1,
                        Vptr128 ptr,
                        const IRInstruction* origin) {
  ASSERT_EQ(inst.op, Vinstr::storepairups);
  EXPECT_EQ(inst.storepairups_.s0, s0);
  EXPECT_EQ(inst.storepairups_.s1, s1);
  EXPECT_EQ(inst.storepairups_.d, ptr);
  EXPECT_EQ(inst.origin, origin);
}

void expectLoadPairUps(const Vinstr& inst,
                       Vptr128 ptr,
                       Vreg128 d0,
                       Vreg128 d1,
                       const IRInstruction* origin) {
  ASSERT_EQ(inst.op, Vinstr::loadpairups);
  EXPECT_EQ(inst.loadpairups_.s, ptr);
  EXPECT_EQ(inst.loadpairups_.d0, d0);
  EXPECT_EQ(inst.loadpairups_.d1, d1);
  EXPECT_EQ(inst.origin, origin);
}

void expectStoreUps(const Vinstr& inst, Vreg128 s, Vptr128 ptr) {
  ASSERT_EQ(inst.op, Vinstr::storeups);
  EXPECT_EQ(inst.storeups_.s, s);
  EXPECT_EQ(inst.storeups_.m, ptr);
}

void expectLoadUps(const Vinstr& inst, Vptr128 ptr, Vreg128 d) {
  ASSERT_EQ(inst.op, Vinstr::loadups);
  EXPECT_EQ(inst.loadups_.s, ptr);
  EXPECT_EQ(inst.loadups_.d, d);
}

enum class PairUpsOp {
  Load,
  Store,
};

enum class PairUpsOrigin {
  None,
  Same,
  Split,
};

struct PairUpsScenario {
  const char* name;
  PairUpsOp op;
  PairUpsOrigin origin;
  bool reverse;
  bool expectPair;
  bool sameReg;
  int32_t secondDisp;
};

void emitPairUps(Vout& v, PairUpsOp op, Vreg128 reg, Vptr128 ptr) {
  switch (op) {
    case PairUpsOp::Load:
      v << loadups{ptr, reg};
      return;
    case PairUpsOp::Store:
      v << storeups{reg, ptr};
      return;
  }
  not_reached();
}

void expectPairUps(const Vinstr& inst,
                   PairUpsOp op,
                   Vptr128 ptr,
                   Vreg128 simd0,
                   Vreg128 simd1,
                   const IRInstruction* origin) {
  switch (op) {
    case PairUpsOp::Load:
      expectLoadPairUps(inst, ptr, simd0, simd1, origin);
      return;
    case PairUpsOp::Store:
      expectStorePairUps(inst, simd0, simd1, ptr, origin);
      return;
  }
  not_reached();
}

void expectUps(const Vinstr& inst, PairUpsOp op, Vptr128 ptr, Vreg128 reg) {
  switch (op) {
    case PairUpsOp::Load:
      expectLoadUps(inst, ptr, reg);
      return;
    case PairUpsOp::Store:
      expectStoreUps(inst, reg, ptr);
      return;
  }
  not_reached();
}

void checkPairUpsSimplification(const PairUpsScenario& scenario) {
  SCOPED_TRACE(scenario.name);

  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  IRInstruction origin0{DefConst, {test_marker(), 0}};
  IRInstruction origin1{DefConst, {test_marker(), 0}};

  auto const base = Vreg64{Reg64{0}};
  auto const simd0 = Vreg128{RegXMM{0}};
  auto const simd1 = scenario.sameReg ? simd0 : Vreg128{RegXMM{1}};
  auto const ptr0 = ptr128(base, -32);
  auto const ptr1 = ptr128(base, scenario.secondDisp);

  auto const firstReg = scenario.reverse ? simd1 : simd0;
  auto const secondReg = scenario.reverse ? simd0 : simd1;
  auto const firstPtr = scenario.reverse ? ptr1 : ptr0;
  auto const secondPtr = scenario.reverse ? ptr0 : ptr1;

  if (scenario.origin != PairUpsOrigin::None) v.setOrigin(&origin0);
  emitPairUps(v, scenario.op, firstReg, firstPtr);
  if (scenario.origin == PairUpsOrigin::Split) v.setOrigin(&origin1);
  emitPairUps(v, scenario.op, secondReg, secondPtr);

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  if (scenario.expectPair) {
    auto const expectedOrigin =
      scenario.origin == PairUpsOrigin::Same ? &origin0 : nullptr;
    ASSERT_EQ(code.size(), 1);
    expectPairUps(code[0], scenario.op, ptr0, simd0, Vreg128{RegXMM{1}},
                  expectedOrigin);
    return;
  }

  ASSERT_EQ(code.size(), 2);
  expectUps(code[0], scenario.op, firstPtr, firstReg);
  expectUps(code[1], scenario.op, secondPtr, secondReg);
}

void testArmLeaLowering() {
  auto const test = [] (Vptr ptr) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const dst = arm::rret(0);
    v << lea{ptr, Vreg64{dst}};
    v << ret{RegSet{dst}};

    optimize(unit, arm::abi(), false);
    return stripWhitespace(show(unit));
  };

  auto const base = Vreg64{arm::rarg(2)};
  auto const index = Vreg64{arm::rarg(3)};

  auto const scaled = test(Vptr{base, index, 8, 0});
  EXPECT_EQ(scaled.find("shlqi"), std::string::npos);
  EXPECT_NE(scaled.find("lea ["), std::string::npos);
  EXPECT_NE(scaled.find("* 8"), std::string::npos);

  auto const scaledDisp = test(Vptr{base, index, 8, 16});
  EXPECT_EQ(scaledDisp.find("shlqi"), std::string::npos);
  EXPECT_NE(scaledDisp.find("lea ["), std::string::npos);
  EXPECT_NE(scaledDisp.find("* 8"), std::string::npos);
  EXPECT_NE(scaledDisp.find("0x10"), std::string::npos);
}

void testArmLeaChainSimplification() {
  auto const base = Vreg64{arm::rarg(2)};
  auto const index = Vreg64{arm::rarg(3)};
  auto const check = [] (const char* name, Vptr first, int32_t secondDisp,
                         Vptr expected) {
    SCOPED_TRACE(name);

    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const tmp = Vreg64{v.makeReg()};
    auto const dst = Vreg64{v.makeReg()};
    v << lea{first, tmp};
    v << lea{tmp[secondDisp], dst};

    simplify(unit);

    auto const& code = unit.blocks[unit.entry].code;
    ASSERT_EQ(code.size(), 1);
    ASSERT_EQ(code[0].op, Vinstr::lea);
    EXPECT_EQ(code[0].lea_.s, expected);
    EXPECT_EQ(code[0].lea_.d, dst);
  };

  // Test ARM chained leas that fold into addresses valid for lea emission even
  // though they would be invalid memory operands:
  //   lea base[index * 8], tmp; lea tmp[16], dst => lea base[index * 8 + 16], dst
  //   lea base[256], tmp; lea tmp[272], dst => lea base[528], dst
  check("index and displacement", Vptr{base, index, 8, 0}, 16,
        Vptr{base, index, 8, 16});
  check("large base displacement", Vptr{base, 256}, 272, Vptr{base, 528});
}

void testArmStoreOffsetNoMaterialize() {
  // A store to a constant offset that fits the 64-bit scaled-imm12 addressing
  // form (528 = 0x210, a multiple of 8 below 32760) must fold the offset into
  // the store's immediate displacement, not materialize it into a register --
  // even when the stored value register is still virtual at lowering time. The
  // load of the same field already keeps its immediate offset; the store must
  // match it.
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const base = Vreg64{arm::rarg(2)};
  auto const val = v.makeReg();          // virtual GP value
  v << load{base[0], val};               // define `val` (no constant offset)
  v << store{val, base[528]};            // offset must fold into addressing mode
  v << ret{RegSet{}};

  optimize(unit, arm::abi(), false);
  auto const out = stripWhitespace(show(unit));

  // The constant offset must not be materialized into a register via ldimmq.
  EXPECT_EQ(out.find("ldimmq"), std::string::npos) << out;
  // The store keeps the constant displacement in its addressing mode.
  EXPECT_NE(out.find("+ 0x210]"), std::string::npos) << out;
}

template<class Load, class Dst>
void expectArmExtendingLoad(const Load& load, Vptr8 ptr, Dst dst) {
  EXPECT_EQ(load.s, ptr);
  EXPECT_EQ(load.d, dst);
}

template<class Emit>
void checkArmSimplifiedBlock(const char* name, Emit emit) {
  SCOPED_TRACE(name);

  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto check = emit(unit, v);
  simplify(unit);
  check(unit.blocks[unit.entry].code);
}

template<class Dst, class EmitExt, class ExpectLoad>
void checkArmLoadExtMapping(const char* name,
                            int32_t disp,
                            EmitExt emitExt,
                            ExpectLoad expectLoad) {
  checkArmSimplifiedBlock(name, [=] (Vunit&, Vout& v) {
    auto const base = Vreg64{Reg64{0}};
    auto const ptr = Vptr8{base[disp]};
    auto const loaded = Vreg8{v.makeReg()};
    auto const dst = Dst{v.makeReg()};

    v << loadb{ptr, loaded};
    emitExt(v, loaded, dst);

    return [=] (auto const& code) {
      ASSERT_EQ(code.size(), 1);
      expectLoad(code[0], ptr, dst);
    };
  });
}

void testArmLoadExtFoldMappings() {
  checkArmLoadExtMapping<Vreg32>(
    "loadzbl",
    8,
    [] (Vout& v, Vreg8 loaded, Vreg32 dst) { v << movzbl{loaded, dst}; },
    [] (const Vinstr& inst, Vptr8 ptr, Vreg32 dst) {
      ASSERT_EQ(inst.op, Vinstr::loadzbl);
      expectArmExtendingLoad(inst.loadzbl_, ptr, dst);
    });

  checkArmLoadExtMapping<Vreg64>(
    "loadzbq",
    16,
    [] (Vout& v, Vreg8 loaded, Vreg64 dst) { v << movzbq{loaded, dst}; },
    [] (const Vinstr& inst, Vptr8 ptr, Vreg64 dst) {
      ASSERT_EQ(inst.op, Vinstr::loadzbq);
      expectArmExtendingLoad(inst.loadzbq_, ptr, dst);
    });

  checkArmLoadExtMapping<Vreg32>(
    "loadsbl",
    -8,
    [] (Vout& v, Vreg8 loaded, Vreg32 dst) { v << movsbl{loaded, dst}; },
    [] (const Vinstr& inst, Vptr8 ptr, Vreg32 dst) {
      ASSERT_EQ(inst.op, Vinstr::loadsbl);
      expectArmExtendingLoad(inst.loadsbl_, ptr, dst);
    });

  checkArmLoadExtMapping<Vreg64>(
    "loadsbq",
    -16,
    [] (Vout& v, Vreg8 loaded, Vreg64 dst) { v << movsbq{loaded, dst}; },
    [] (const Vinstr& inst, Vptr8 ptr, Vreg64 dst) {
      ASSERT_EQ(inst.op, Vinstr::loadsbq);
      expectArmExtendingLoad(inst.loadsbq_, ptr, dst);
    });
}

void testArmLoadExtRejectsMultipleUses() {
  checkArmSimplifiedBlock(
    "loaded byte has another use", [] (Vunit&, Vout& v) {
      auto const base = Vreg64{Reg64{0}};
      auto const loadPtr = Vptr8{base[0]};
      auto const storePtr = Vptr8{base[8]};
      auto const loaded = Vreg8{v.makeReg()};
      auto const dst = Vreg64{v.makeReg()};

      v << loadb{loadPtr, loaded};
      v << movzbq{loaded, dst};
      v << storeb{loaded, storePtr};

      return [=] (auto const& code) {
        ASSERT_EQ(code.size(), 3);
        EXPECT_EQ(code[0].op, Vinstr::loadb);
        EXPECT_EQ(code[1].op, Vinstr::movzbq);
        EXPECT_EQ(code[2].op, Vinstr::storeb);
      };
    }
  );
}

void testArmShiftedBitTestFromShrqi() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{5}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{1}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{1}, xored, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s0.l(), 5);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);
  EXPECT_EQ(code[0].shrqi_.sf, shiftSf);

  ASSERT_EQ(code[1].op, Vinstr::xorqi64);
  EXPECT_EQ(static_cast<uint64_t>(code[1].xorqi64_.s0.q()), 0x20);
  EXPECT_EQ(code[1].xorqi64_.s1, src);
  EXPECT_EQ(code[1].xorqi64_.d, xored);
  EXPECT_EQ(code[1].xorqi64_.sf, xorSf);

  ASSERT_EQ(code[2].op, Vinstr::testqi64);
  EXPECT_EQ(static_cast<uint64_t>(code[2].testqi64_.s0.q()), 0x20);
  EXPECT_EQ(code[2].testqi64_.s1, xored);
  EXPECT_EQ(code[2].testqi64_.sf, testSf);
}

struct ShrqiTestlFold {
  const Vunit& unit;
  Vreg64 src;
  Vreg testSf;
  Vlabel taken;
  Vlabel next;
};

// Emits `shrqi; testl; [setcc;] jcc CC_E`, runs the simplifier, and hands the
// simplified entry block to `check`.
template<class Check>
void runArmShrqiTestlFold(const char* name,
                          int32_t shift,
                          uint32_t mask,
                          bool maskFirst,
                          bool addSecondFlagUse,
                          Check check) {
  SCOPED_TRACE(name);

  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);

  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const testSf = v.makeReg();
  auto const maskReg = Vreg32{unit.makeConst(mask)};
  auto const shiftedReg = Vreg32{Vreg{shifted}};

  v << shrqi{Immed{shift}, src, shifted, shiftSf, Vflags{}};
  v << testl{
    maskFirst ? maskReg : shiftedReg,
    maskFirst ? shiftedReg : maskReg,
    testSf,
    Vflags{}
  };
  if (addSecondFlagUse) {
    v << setcc{CC_NE, testSf, Vreg8{v.makeReg()}};
  }
  v << jcc{CC_E, testSf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  check(ShrqiTestlFold{unit, src, testSf, taken, next},
        unit.blocks[unit.entry].code);
}

// Covers the shrqi+testl fold stopping at a test, which happens when the
// folded mask is not a single bit or its flags have a non-branch consumer.
template<Vinstr::Opcode testOp>
void checkArmShrqiTestlFold(const char* name,
                            int32_t shift,
                            uint32_t mask,
                            bool maskFirst,
                            bool addSecondFlagUse = false) {
  runArmShrqiTestlFold(
    name, shift, mask, maskFirst, addSecondFlagUse,
    [&] (const ShrqiTestlFold& f, auto const& code) {
      ASSERT_EQ(code.size(), addSecondFlagUse ? 3 : 2);
      ASSERT_EQ(code[0].op, testOp) << stripWhitespace(show(f.unit));
      auto const& test = code[0].template get<testOp>();
      auto const constIt = f.unit.regToConst.find(test.s0);
      ASSERT_NE(constIt, f.unit.regToConst.end());
      EXPECT_EQ(constIt->second.val, uint64_t{mask} << shift);
      EXPECT_EQ(Vreg{test.s1}, Vreg{f.src});
      EXPECT_EQ(test.sf, f.testSf);

      if (addSecondFlagUse) {
        ASSERT_EQ(code[1].op, Vinstr::setcc);
        EXPECT_EQ(code[1].setcc_.cc, CC_NE);
        EXPECT_EQ(code[1].setcc_.sf, f.testSf);
      }
      ASSERT_EQ(code.back().op, Vinstr::jcc);
      EXPECT_EQ(code.back().jcc_.cc, CC_E);
      EXPECT_EQ(code.back().jcc_.sf, f.testSf);
    });
}

// Covers the shrqi+testl fold composing with the test+jcc fold: when the folded
// mask leaves a single bit, the resulting test and its branch collapse further
// into a tbz against that bit, leaving no test behind.
void checkArmShrqiTestlFoldsToTbz(const char* name,
                                  int32_t shift,
                                  uint32_t mask,
                                  int32_t expectedBit) {
  runArmShrqiTestlFold(
    name, shift, mask, true, false,
    [&] (const ShrqiTestlFold& f, auto const& code) {
      ASSERT_EQ(code.size(), 1) << stripWhitespace(show(f.unit));
      ASSERT_EQ(code[0].op, Vinstr::tbzq);
      EXPECT_EQ(code[0].tbzq_.bit.l(), expectedBit);
      EXPECT_EQ(Vreg{code[0].tbzq_.s}, Vreg{f.src});
      EXPECT_EQ(code[0].tbzq_.targets[0], f.taken);
      EXPECT_EQ(code[0].tbzq_.targets[1], f.next);
    });
}

// The shifted-bit-test rewrite (shr+xor+test -> xor+test on the unshifted
// source) and the test+jcc fold compose: the rewritten testqi64 must still
// collapse into a tbz against the pre-shift bit.
void testArmShiftedBitTestFoldsBeforeJcc() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);

  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{5}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{1}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{1}, xored, testSf, Vflags{}};
  v << jcc{CC_E, testSf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::shrqi);

  ASSERT_EQ(code[1].op, Vinstr::xorqi64);
  EXPECT_EQ(static_cast<uint64_t>(code[1].xorqi64_.s0.q()), 0x20);
  EXPECT_EQ(code[1].xorqi64_.s1, src);

  ASSERT_EQ(code[2].op, Vinstr::tbzq);
  EXPECT_EQ(code[2].tbzq_.bit.l(), 5);
  EXPECT_EQ(code[2].tbzq_.s, xored);
  EXPECT_EQ(code[2].tbzq_.targets[0], taken);
  EXPECT_EQ(code[2].tbzq_.targets[1], next);
}

void testArmShiftedBitTestFromShrqiWithoutXor() {
  checkArmShrqiTestlFold<Vinstr::testb>("single-bit mask first", 5, 4, true);
}

void testArmShrqiTestlFoldsFlippedOperands() {
  checkArmShrqiTestlFold<Vinstr::testb>("shifted value first", 5, 4, false);
}

void testArmShrqiTestlFoldsMultipleZUses() {
  checkArmShrqiTestlFold<Vinstr::testb>(
    "two Z-only flag consumers", 5, 4, true, true
  );
}

void testArmShrqiTestlFoldsEncodableMultiBitMasks() {
  checkArmShrqiTestlFold<Vinstr::testb>(
    "two contiguous bits", 5, 0b11, true
  );
  checkArmShrqiTestlFold<Vinstr::testq>(
    "two contiguous high bits", 32, uint32_t{0b11} << 30, true
  );
  // Mask bit 31 would test bit 71 of the source, which cannot be set, so the
  // shift drops it. That leaves the single bit 40, which folds on into a tbz.
  checkArmShrqiTestlFoldsToTbz(
    "shift above 32 drops mask bits beyond the source width",
    40,
    (uint32_t{1} << 31) | 1,
    40
  );
}

void checkArmShrqiTestlNotFolded(const char* name,
                                 int32_t shift,
                                 uint64_t mask,
                                 ConditionCode cc) {
  SCOPED_TRACE(name);

  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);

  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{shift}, src, shifted, shiftSf, Vflags{}};
  v << testl{Vreg32{unit.makeConst(mask)},
             Vreg32{Vreg{shifted}},
             testSf,
             Vflags{}};
  v << jcc{cc, testSf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[1].op, Vinstr::testl);
  EXPECT_EQ(code[2].op, Vinstr::jcc);
  EXPECT_EQ(code[2].jcc_.cc, cc);
}

void testArmShrqiTestlRejectsUnsafeFolds() {
  checkArmShrqiTestlNotFolded(
    "sign flag is not preserved when widening", 1, uint64_t{1} << 31, CC_S
  );
  checkArmShrqiTestlNotFolded(
    "bits above testl width are ignored", 1, uint64_t{1} << 40, CC_E
  );
}

void testArmShrqiTestlRejectsNonEncodableMultiBitMasks() {
  checkArmShrqiTestlNotFolded(
    "non-contiguous bits", 1, 0b1011, CC_E
  );
  checkArmShrqiTestlNotFolded(
    "repeated 32-bit pattern is not a 64-bit logical immediate",
    1,
    0x00ff00ff,
    CC_E
  );
}

void testArmShrqiTestlRejectsOutOfRangeShift() {
  checkArmShrqiTestlNotFolded(
    "shift equals source width", 64, 1, CC_E
  );
}

void testArmShiftedBitTestRejectsOutOfRangeMask() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{63}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{3}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{3}, xored, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s0.l(), 63);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);
  EXPECT_EQ(code[0].shrqi_.sf, shiftSf);

  ASSERT_EQ(code[1].op, Vinstr::xorqi);
  EXPECT_EQ(static_cast<uint64_t>(code[1].xorqi_.s0.q()), 3);
  EXPECT_EQ(code[1].xorqi_.s1, shifted);
  EXPECT_EQ(code[1].xorqi_.d, xored);
  EXPECT_EQ(code[1].xorqi_.sf, xorSf);

  ASSERT_EQ(code[2].op, Vinstr::testqi);
  EXPECT_EQ(static_cast<uint64_t>(code[2].testqi_.s0.q()), 3);
  EXPECT_EQ(code[2].testqi_.s1, xored);
  EXPECT_EQ(code[2].testqi_.sf, testSf);
}

void testArmShiftedBitTestZeroExtendsByteMask() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const xoredByte = Vreg8{Vreg{xored}};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{1}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{1}, shifted, xored, xorSf, Vflags{}};
  v << testbi{Immed{-128}, xoredByte, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s0.l(), 1);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);
  EXPECT_EQ(code[0].shrqi_.sf, shiftSf);

  ASSERT_EQ(code[1].op, Vinstr::xorqi64);
  EXPECT_EQ(static_cast<uint64_t>(code[1].xorqi64_.s0.q()), 0x2);
  EXPECT_EQ(code[1].xorqi64_.s1, src);
  EXPECT_EQ(code[1].xorqi64_.d, xored);
  EXPECT_EQ(code[1].xorqi64_.sf, xorSf);

  ASSERT_EQ(code[2].op, Vinstr::testqi64);
  EXPECT_EQ(static_cast<uint64_t>(code[2].testqi64_.s0.q()), 0x100);
  EXPECT_EQ(code[2].testqi64_.s1, xored);
  EXPECT_EQ(code[2].testqi64_.sf, testSf);
}

void testArmShiftedBitTestNonAdjacent() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const side = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{48}, src, shifted, shiftSf, Vflags{}};
  v << copy{src, side};
  v << xorqi{Immed{0x1fff}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{0x7f}, xored, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 4);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s0.l(), 48);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);

  ASSERT_EQ(code[1].op, Vinstr::copy);
  EXPECT_EQ(code[1].copy_.s, Vreg{src});
  EXPECT_EQ(code[1].copy_.d, Vreg{side});

  ASSERT_EQ(code[2].op, Vinstr::xorqi64);
  EXPECT_EQ(
    static_cast<uint64_t>(code[2].xorqi64_.s0.q()),
    0x1fff000000000000ull
  );
  EXPECT_EQ(code[2].xorqi64_.s1, src);
  EXPECT_EQ(code[2].xorqi64_.d, xored);
  EXPECT_EQ(code[2].xorqi64_.sf, xorSf);

  ASSERT_EQ(code[3].op, Vinstr::testqi64);
  EXPECT_EQ(
    static_cast<uint64_t>(code[3].testqi64_.s0.q()),
    0x007f000000000000ull
  );
  EXPECT_EQ(code[3].testqi64_.s1, xored);
  EXPECT_EQ(code[3].testqi64_.sf, testSf);
}

void testArmShiftedBitTestRejectsPhysSourceClobber() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{5}, src, shifted, shiftSf, Vflags{}};
  v << copy{shifted, src};
  v << xorqi{Immed{1}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{1}, xored, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 4);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);

  ASSERT_EQ(code[1].op, Vinstr::copy);
  EXPECT_EQ(code[1].copy_.s, Vreg{shifted});
  EXPECT_EQ(code[1].copy_.d, Vreg{src});

  ASSERT_EQ(code[2].op, Vinstr::xorqi);
  EXPECT_EQ(static_cast<uint64_t>(code[2].xorqi_.s0.q()), 1);
  EXPECT_EQ(code[2].xorqi_.s1, shifted);
  EXPECT_EQ(code[2].xorqi_.d, xored);

  ASSERT_EQ(code[3].op, Vinstr::testqi);
  EXPECT_EQ(static_cast<uint64_t>(code[3].testqi_.s0.q()), 1);
  EXPECT_EQ(code[3].testqi_.s1, xored);
  EXPECT_EQ(code[3].testqi_.sf, testSf);
}

void testArmShiftedBitTestRejectsSignExtendedMask() {
  // testqi emits its int32_t immediate sign-extended to 64 bits. A negative
  // immediate is therefore not a single-bit test and cannot be shifted.
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{1}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{int32_t(0x80000000)}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{int32_t(0x80000000)}, xored, testSf, Vflags{}};

  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);

  ASSERT_EQ(code[1].op, Vinstr::xorqi);
  EXPECT_EQ(
    static_cast<uint64_t>(code[1].xorqi_.s0.q()),
    0xffffffff80000000ull
  );
  EXPECT_EQ(code[1].xorqi_.s1, shifted);
  EXPECT_EQ(code[1].xorqi_.d, xored);
  EXPECT_EQ(code[1].xorqi_.sf, xorSf);

  ASSERT_EQ(code[2].op, Vinstr::testqi);
  EXPECT_EQ(
    static_cast<uint64_t>(code[2].testqi_.s0.q()),
    0xffffffff80000000ull
  );
  EXPECT_EQ(code[2].testqi_.s1, xored);
  EXPECT_EQ(code[2].testqi_.sf, testSf);
}

void testArmShiftedBitTestRejectsNonZFlagUse() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);

  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const src = Vreg64{Reg64{0}};
  auto const shifted = Vreg64{v.makeReg()};
  auto const xored = Vreg64{v.makeReg()};
  auto const shiftSf = v.makeReg();
  auto const xorSf = v.makeReg();
  auto const testSf = v.makeReg();

  v << shrqi{Immed{5}, src, shifted, shiftSf, Vflags{}};
  v << xorqi{Immed{1}, shifted, xored, xorSf, Vflags{}};
  v << testqi{Immed{1}, xored, testSf, Vflags{}};
  v << jcc{CC_S, testSf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 4);
  ASSERT_EQ(code[0].op, Vinstr::shrqi);
  EXPECT_EQ(code[0].shrqi_.s0.l(), 5);
  EXPECT_EQ(code[0].shrqi_.s1, src);
  EXPECT_EQ(code[0].shrqi_.d, shifted);
  EXPECT_EQ(code[0].shrqi_.sf, shiftSf);

  ASSERT_EQ(code[1].op, Vinstr::xorqi);
  EXPECT_EQ(static_cast<uint64_t>(code[1].xorqi_.s0.q()), 1);
  EXPECT_EQ(code[1].xorqi_.s1, shifted);
  EXPECT_EQ(code[1].xorqi_.d, xored);

  ASSERT_EQ(code[2].op, Vinstr::testqi);
  EXPECT_EQ(static_cast<uint64_t>(code[2].testqi_.s0.q()), 1);
  EXPECT_EQ(code[2].testqi_.s1, xored);
  EXPECT_EQ(code[2].testqi_.sf, testSf);

  ASSERT_EQ(code[3].op, Vinstr::jcc);
  EXPECT_EQ(code[3].jcc_.cc, CC_S);
  EXPECT_EQ(code[3].jcc_.sf, testSf);
}

void testArmLoadPairNoClobberBase() {
  // Test that two adjacent loads are NOT combined when the first load's
  // destination overwrites the second load's base register.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const dst2 = Vreg{Reg64{1}};
    v << ldimmq{uintptr_t(0x1000), base};
    // First load writes to 'base', clobbering the second load's base.
    v << load{base[0x10], base};
    v << load{base[0x18], dst2};
    v << ret{RegSet{PhysReg{Reg64{0}}} | RegSet{PhysReg{Reg64{1}}}};

    simplify(unit);

    auto const result = stripWhitespace(show(unit));
    // Should NOT be combined — first load overwrites the base of the second.
    EXPECT_EQ(result.find("loadpair"), std::string::npos)
      << "Should not combine dependent loads (base), got:\n" << result;
  }

  // Test that two adjacent loads are NOT combined when the first load's
  // destination overwrites the second load's index register.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const idx = Vreg{Reg64{1}};
    auto const dst2 = Vreg{Reg64{2}};
    v << ldimmq{uintptr_t(0x1000), base};
    v << ldimmq{uintptr_t(0x8), idx};
    // First load writes to 'idx', clobbering the second load's index.
    v << load{Vptr{base, idx, 1, 0x10}, idx};
    v << load{Vptr{base, idx, 1, 0x18}, dst2};
    v << ret{RegSet{PhysReg{Reg64{1}}} | RegSet{PhysReg{Reg64{2}}}};

    simplify(unit);

    auto const result = stripWhitespace(show(unit));
    // Should NOT be combined — first load overwrites the index of the second.
    EXPECT_EQ(result.find("loadpair"), std::string::npos)
      << "Should not combine dependent loads (index), got:\n" << result;
  }

  // Test that two adjacent loads with the same destination are NOT combined.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const dst = Vreg{Reg64{1}};
    v << ldimmq{uintptr_t(0x1000), base};
    v << load{base[0x10], dst};
    v << load{base[0x18], dst};
    v << ret{RegSet{PhysReg{Reg64{1}}}};

    simplify(unit);

    auto const result = stripWhitespace(show(unit));
    EXPECT_EQ(result.find("loadpair"), std::string::npos)
      << "Should not fuse loads with same destination, got:\n" << result;
  }
}

void testArmStorePairUpsSimplification() {
  checkPairUpsSimplification({
    "same origin",
    PairUpsOp::Store,
    PairUpsOrigin::Same,
    false,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "reverse order",
    PairUpsOp::Store,
    PairUpsOrigin::None,
    true,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "split origin",
    PairUpsOp::Store,
    PairUpsOrigin::Split,
    false,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "non-adjacent",
    PairUpsOp::Store,
    PairUpsOrigin::None,
    false,
    false,
    false,
    -64,
  });
}

void testArmLoadPairUpsSimplification() {
  checkPairUpsSimplification({
    "same origin",
    PairUpsOp::Load,
    PairUpsOrigin::Same,
    false,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "reverse order",
    PairUpsOp::Load,
    PairUpsOrigin::None,
    true,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "split origin",
    PairUpsOp::Load,
    PairUpsOrigin::Split,
    false,
    true,
    false,
    -16,
  });
  checkPairUpsSimplification({
    "same destination",
    PairUpsOp::Load,
    PairUpsOrigin::None,
    false,
    false,
    true,
    -16,
  });
}

void testArmWritebackBaseOverlap() {
  // Post-index load: load{base[0], base}; addqi{8, base, base}
  // Must NOT fold — the addqi operates on the loaded value, not the address.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << load{base[0], base};
    v << addqi{8, base, base, sf};
    v << ret{RegSet{base}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_EQ(result.find("loadpi"), std::string::npos)
      << "Should not fold post-index load when dest == base, got:\n" << result;
    EXPECT_EQ(result.find("loadpri"), std::string::npos)
      << "Should not fold pre-index load when dest == base, got:\n" << result;
  }

  // Pre-index load: addqi{8, base, base}; load{base[0], base}
  // Must NOT fold.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << addqi{8, base, base, sf};
    v << load{base[0], base};
    v << ret{RegSet{base}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_EQ(result.find("loadpri"), std::string::npos)
      << "Should not fold pre-index load when dest == base, got:\n" << result;
    EXPECT_EQ(result.find("loadpi"), std::string::npos)
      << "Should not fold post-index load when dest == base, got:\n" << result;
  }

  // Post-index store: store{base, base[0]}; addqi{8, base, base}
  // Must NOT fold — value register aliases the base.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << store{base, base[0]};
    v << addqi{8, base, base, sf};
    v << ret{RegSet{base}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_EQ(result.find("storepi"), std::string::npos)
      << "Should not fold post-index store when value == base, got:\n" << result;
    EXPECT_EQ(result.find("storepri"), std::string::npos)
      << "Should not fold pre-index store when value == base, got:\n" << result;
  }

  // Positive case: load where dest != base should still fold.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const dst = Vreg{Reg64{1}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << load{base[0], dst};
    v << addqi{8, base, base, sf};
    v << ret{RegSet{base} | RegSet{dst}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_NE(result.find("loadpi"), std::string::npos)
      << "Should fold post-index load when dest != base, got:\n" << result;
  }

  // Positive case: store where value != base should still fold.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const val = Vreg{Reg64{1}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << ldimmq{uintptr_t(42), val};
    v << store{val, base[0]};
    v << addqi{8, base, base, sf};
    v << ret{RegSet{base}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_NE(result.find("storepi"), std::string::npos)
      << "Should fold post-index store when value != base, got:\n" << result;
  }

  // Loadpair: load{base[0], base}; load{base[8], dst2}
  // The loadpair fusion should already be blocked by dependent-base logic,
  // but verify the writeback path is also guarded.
  {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);

    auto const base = Vreg{Reg64{0}};
    auto const dst2 = Vreg{Reg64{1}};
    auto const sf = v.makeReg();
    v << ldimmq{uintptr_t(0x1000), base};
    v << loadpair{Vptr128{base, 0}, base, dst2};
    v << addqi{16, base, base, sf};
    v << ret{RegSet{base} | RegSet{dst2}};

    postRASimplify(unit, abi(CodeKind::Trace));

    auto const result = stripWhitespace(show(unit));
    EXPECT_EQ(result.find("loadpairpi"), std::string::npos)
      << "Should not fold post-index loadpair when d0 == base, got:\n" << result;
  }
}

void testArmWritebackPairUps() {
  auto const postRAShow = [](auto emit) {
    Vunit unit;
    unit.entry = unit.makeBlock(AreaIndex::Main, 1);
    Vout v(unit, unit.entry);
    emit(v);
    postRASimplify(unit, abi(CodeKind::Trace));
    return stripWhitespace(show(unit));
  };

  auto const base = Vreg{Reg64{0}};
  auto const simd0 = Vreg128{RegXMM{0}};
  auto const simd1 = Vreg128{RegXMM{1}};
  auto const liveLoadRegs =
    PhysReg{Reg64{0}} | PhysReg{RegXMM{0}} | PhysReg{RegXMM{1}};

  auto const emitBaseInit = [&](Vout& v) {
    v << ldimmq{uintptr_t(0x1000), base};
  };
  auto const emitBaseUpdate = [&](Vout& v, int32_t offset) {
    auto const sf = v.makeReg();
    v << addqi{offset, base, base, sf};
  };
  auto const expectFolded = [&](const char* opcode, auto emit) {
    auto const result = postRAShow(emit);
    EXPECT_NE(result.find(opcode), std::string::npos)
      << "Should fold to " << opcode << ", got:\n" << result;
  };
  auto const expectNotFolded = [&](const char* opcode, auto emit) {
    auto const result = postRAShow(emit);
    EXPECT_EQ(result.find(opcode), std::string::npos)
      << "Should not fold to " << opcode << ", got:\n" << result;
  };

  expectFolded("loadpairupspi", [&](Vout& v) {
    emitBaseInit(v);
    v << loadpairups{Vptr128{base, 0}, simd0, simd1};
    emitBaseUpdate(v, 32);
    v << ret{liveLoadRegs};
  });

  expectFolded("loadpairupspri", [&](Vout& v) {
    emitBaseInit(v);
    emitBaseUpdate(v, 32);
    v << loadpairups{Vptr128{base, 0}, simd0, simd1};
    v << ret{liveLoadRegs};
  });

  expectFolded("storepairupspi", [&](Vout& v) {
    emitBaseInit(v);
    v << storepairups{simd0, simd1, Vptr128{base, 0}};
    emitBaseUpdate(v, 32);
    v << ret{RegSet{PhysReg{Reg64{0}}}};
  });

  expectFolded("storepairupspri", [&](Vout& v) {
    emitBaseInit(v);
    emitBaseUpdate(v, 32);
    v << storepairups{simd0, simd1, Vptr128{base, 0}};
    v << ret{RegSet{PhysReg{Reg64{0}}}};
  });

  expectNotFolded("loadpairupspi", [&](Vout& v) {
    emitBaseInit(v);
    v << loadpairups{Vptr128{base, 0}, simd0, simd1};
    emitBaseUpdate(v, 8);
    v << ret{liveLoadRegs};
  });

  expectNotFolded("loadpairpi", [&](Vout& v) {
    emitBaseInit(v);
    v << loadpair{Vptr128{base, 0}, Vreg{simd0}, Vreg{simd1}};
    emitBaseUpdate(v, 16);
    v << ret{liveLoadRegs};
  });

  expectNotFolded("storepairpi", [&](Vout& v) {
    emitBaseInit(v);
    v << storepair{Vreg{simd0}, Vreg{simd1}, Vptr128{base, 0}};
    emitBaseUpdate(v, 16);
    v << ret{RegSet{PhysReg{Reg64{0}}}};
  });
}

// Emits `<test>; jcc{cc} -> {taken, next}` into the entry block, annotates SF
// uses (so foldTestJcc observes a single flags consumer, use_counts == 1), and
// runs the ARM simplify pass. `emitTest` emits the test op against `sf`.
template <typename EmitTest>
void runTestJccFold(Vunit& unit, ConditionCode cc, EmitTest emitTest,
                    Vlabel& taken, Vlabel& next) {
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  taken = unit.makeBlock(AreaIndex::Main, 1);
  next = unit.makeBlock(AreaIndex::Main, 1);

  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const sf = v.makeReg();
  emitTest(v, sf);
  v << jcc{cc, sf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);
}

template<class Branch, class EmitTest>
void expectTestBranchFold(const char* name,
                          Vinstr::Opcode opcode,
                          Branch Vinstr::* branchMember,
                          ConditionCode cc,
                          int32_t bit,
                          Vreg src,
                          EmitTest emitTest) {
  SCOPED_TRACE(name);

  Vunit unit;
  Vlabel taken, next;
  runTestJccFold(
    unit,
    cc,
    [&] (Vout& v, Vreg sf) { emitTest(unit, v, sf); },
    taken,
    next
  );

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, opcode);
  auto const& branch = code[0].*branchMember;
  EXPECT_EQ(branch.bit.l(), bit);
  EXPECT_EQ(Vreg{branch.s}, src);
  EXPECT_EQ(branch.targets[0], taken);
  EXPECT_EQ(branch.targets[1], next);
}

void testArmFoldTestqEqToCbzq() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg64{Reg64{0}};
  runTestJccFold(unit, CC_E,
                 [&] (Vout& v, Vreg sf) { v << testq{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbzq);
  EXPECT_EQ(Vreg{code[0].cbzq_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbzq_.targets[0], taken);
  EXPECT_EQ(code[0].cbzq_.targets[1], next);
}

void testArmFoldTestqNeToCbnzq() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg64{Reg64{0}};
  runTestJccFold(unit, CC_NE,
                 [&] (Vout& v, Vreg sf) { v << testq{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbnzq);
  EXPECT_EQ(Vreg{code[0].cbnzq_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbnzq_.targets[0], taken);
  EXPECT_EQ(code[0].cbnzq_.targets[1], next);
}

void testArmFoldTestlEqToCbzl() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg32{Vreg{Reg64{0}}};
  runTestJccFold(unit, CC_E,
                 [&] (Vout& v, Vreg sf) { v << testl{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbzl);
  EXPECT_EQ(Vreg{code[0].cbzl_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbzl_.targets[0], taken);
  EXPECT_EQ(code[0].cbzl_.targets[1], next);
}

// The testb -> cbzl fold is the boolean-branch case: emit(testb) tests the full
// W register, so folding a byte self-compare to a 32-bit cbzl is exact.
void testArmFoldTestbEqToCbzl() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg8{Vreg{Reg64{0}}};
  runTestJccFold(unit, CC_E,
                 [&] (Vout& v, Vreg sf) { v << testb{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbzl);
  EXPECT_EQ(Vreg{code[0].cbzl_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbzl_.targets[0], taken);
  EXPECT_EQ(code[0].cbzl_.targets[1], next);
}

void testArmFoldTestbNeToCbnzl() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg8{Vreg{Reg64{0}}};
  runTestJccFold(unit, CC_NE,
                 [&] (Vout& v, Vreg sf) { v << testb{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbnzl);
  EXPECT_EQ(Vreg{code[0].cbnzl_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbnzl_.targets[0], taken);
  EXPECT_EQ(code[0].cbnzl_.targets[1], next);
}

void testArmFoldTestwEqToCbzl() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg16{Vreg{Reg64{0}}};
  runTestJccFold(unit, CC_E,
                 [&] (Vout& v, Vreg sf) { v << testw{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 1) << stripWhitespace(show(unit));
  ASSERT_EQ(code[0].op, Vinstr::cbzl);
  EXPECT_EQ(Vreg{code[0].cbzl_.s}, Vreg{r});
  EXPECT_EQ(code[0].cbzl_.targets[0], taken);
  EXPECT_EQ(code[0].cbzl_.targets[1], next);
}

void testArmFoldSingleBitTestliNeToTbnzl() {
  auto const r = Vreg32{Vreg{Reg64{0}}};
  expectTestBranchFold(
    "testli NE",
    Vinstr::tbnzl,
    &Vinstr::tbnzl_,
    CC_NE,
    7,
    r,
    [&] (Vunit&, Vout& v, Vreg sf) {
      v << testli{Immed{0x80}, r, sf, Vflags{}};
    }
  );
}

void testArmFoldSingleHighBitTestqi64EqToTbzq() {
  auto const r = Vreg64{Reg64{0}};
  expectTestBranchFold(
    "testqi64 E",
    Vinstr::tbzq,
    &Vinstr::tbzq_,
    CC_E,
    63,
    r,
    [&] (Vunit&, Vout& v, Vreg sf) {
      v << testqi64{
        Immed64{static_cast<int64_t>(uint64_t{1} << 63)},
        r,
        sf,
        Vflags{}
      };
    }
  );
}

// testq's mask can arrive in a constant register rather than an immediate;
// get_const_int must recover it so the single-bit fold still applies.
void testArmFoldSingleHighBitTestqEqToTbzq() {
  auto const r = Vreg64{Reg64{0}};
  expectTestBranchFold(
    "testq constant E",
    Vinstr::tbzq,
    &Vinstr::tbzq_,
    CC_E,
    63,
    r,
    [&] (Vunit& unit, Vout& v, Vreg sf) {
      auto const mask = Vreg64{unit.makeConst(uint64_t{1} << 63)};
      v << testq{mask, r, sf, Vflags{}};
    }
  );
}

void testArmFoldSelfTestSignCCToTestBranches() {
  auto const r32 = Vreg32{Vreg{Reg64{0}}};
  auto const r64 = Vreg64{Reg64{0}};

  // Vasm rejects sign-flag consumers of testb/testw because their x64 and
  // arm64 sign bits differ. The portable long and quad forms fold here.
  expectTestBranchFold(
    "testl S", Vinstr::tbnzl, &Vinstr::tbnzl_, CC_S, 31, r32,
    [=] (Vunit&, Vout& v, Vreg sf) { v << testl{r32, r32, sf, Vflags{}}; }
  );
  expectTestBranchFold(
    "testl NS", Vinstr::tbzl, &Vinstr::tbzl_, CC_NS, 31, r32,
    [=] (Vunit&, Vout& v, Vreg sf) { v << testl{r32, r32, sf, Vflags{}}; }
  );
  expectTestBranchFold(
    "testq S", Vinstr::tbnzq, &Vinstr::tbnzq_, CC_S, 63, r64,
    [=] (Vunit&, Vout& v, Vreg sf) { v << testq{r64, r64, sf, Vflags{}}; }
  );
  expectTestBranchFold(
    "testq NS", Vinstr::tbzq, &Vinstr::tbzq_, CC_NS, 63, r64,
    [=] (Vunit&, Vout& v, Vreg sf) { v << testq{r64, r64, sf, Vflags{}}; }
  );
}

// testqi emits its int32_t immediate sign-extended to 64 bits, so a mask with
// bit 31 set tests bits 31-63, not a single bit. It must not fold to a tbz.
void testArmFoldTestqiRejectsSignExtendedMask() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg64{Reg64{0}};
  runTestJccFold(
    unit,
    CC_E,
    [&] (Vout& v, Vreg sf) {
      v << testqi{
        Immed{static_cast<int32_t>(uint32_t{1} << 31)},
        r,
        sf,
        Vflags{}
      };
    },
    taken,
    next
  );

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 2) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::testqi);
  EXPECT_EQ(code[1].op, Vinstr::jcc);
}

void testArmFoldTestBitJccRejectsMultiBitMask() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg32{Vreg{Reg64{0}}};
  runTestJccFold(
    unit,
    CC_E,
    [&] (Vout& v, Vreg sf) {
      v << testli{Immed{0x3}, r, sf, Vflags{}};
    },
    taken,
    next
  );

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 2) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::testli);
  EXPECT_EQ(code[1].op, Vinstr::jcc);
}

// A non-self-compare of two non-constant registers cannot be expressed by
// cbz/cbnz or tbz/tbnz, so it must be left as test + jcc.
void testArmFoldTestJccRejectsNonSelfCompare() {
  Vunit unit;
  Vlabel taken, next;
  auto const r0 = Vreg64{Reg64{0}};
  auto const r1 = Vreg64{Reg64{1}};
  runTestJccFold(unit, CC_E,
                 [&] (Vout& v, Vreg sf) { v << testq{r0, r1, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 2) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::testq);
  EXPECT_EQ(code[1].op, Vinstr::jcc);
}

// Compare/test branches cannot express signed-less, so it must not be folded.
void testArmFoldTestJccRejectsNonEqualCC() {
  Vunit unit;
  Vlabel taken, next;
  auto const r = Vreg64{Reg64{0}};
  runTestJccFold(unit, CC_L,
                 [&] (Vout& v, Vreg sf) { v << testq{r, r, sf, Vflags{}}; },
                 taken, next);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 2) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::testq);
  EXPECT_EQ(code[1].op, Vinstr::jcc);
}

// The test and the terminator jcc need not be adjacent: a pure instruction
// that does not redefine the tested register may sit between them. The fold
// still applies, branching on the register the test examined.
void testArmFoldTestJccNonAdjacent() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const r = Vreg64{Reg64{0}};
  auto const side = Vreg64{v.makeReg()};
  auto const sf = v.makeReg();

  v << testq{r, r, sf, Vflags{}};
  // Pure, sits between the test and the jcc, leaves r intact.
  v << copy{r, side};
  v << jcc{CC_E, sf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  for (auto const& in : code) {
    EXPECT_NE(in.op, Vinstr::testq) << stripWhitespace(show(unit));
  }
  ASSERT_FALSE(code.empty());
  ASSERT_EQ(code.back().op, Vinstr::cbzq) << stripWhitespace(show(unit));
  EXPECT_EQ(Vreg{code.back().cbzq_.s}, Vreg{r});
  EXPECT_EQ(code.back().cbzq_.targets[0], taken);
  EXPECT_EQ(code.back().cbzq_.targets[1], next);
}

// If the tested register is redefined between the test and the jcc, folding to
// cbz/cbnz would branch on the wrong (post-clobber) value, so the fold must be
// rejected. A physical register makes the redefinition an observable clobber.
void testArmFoldTestJccNonAdjacentRejectsClobber() {
  Vunit unit;
  unit.entry = unit.makeBlock(AreaIndex::Main, 1);
  auto const taken = unit.makeBlock(AreaIndex::Main, 1);
  auto const next = unit.makeBlock(AreaIndex::Main, 1);
  Vout v(unit, unit.entry);
  Vout vt(unit, taken);
  Vout vn(unit, next);

  auto const r = Vreg64{Reg64{0}};
  auto const sf = v.makeReg();

  v << testq{r, r, sf, Vflags{}};
  v << ldimmq{uintptr_t(7), r};   // clobbers r before the branch
  v << jcc{CC_E, sf, {taken, next}, StringTag{}};
  vt << ret{};
  vn << ret{};

  annotateSFUses(unit);
  simplify(unit);

  auto const& code = unit.blocks[unit.entry].code;
  ASSERT_EQ(code.size(), 3) << stripWhitespace(show(unit));
  EXPECT_EQ(code[0].op, Vinstr::testq);
  EXPECT_EQ(code[1].op, Vinstr::ldimmq);
  EXPECT_EQ(code[2].op, Vinstr::jcc);
}

}

TEST(Vasm, ArmLeaLowering) {
  testArmLeaLowering();
}

TEST(Vasm, ArmLeaChainSimplification) {
  testArmLeaChainSimplification();
}

TEST(Vasm, ArmStoreOffsetNoMaterialize) {
  testArmStoreOffsetNoMaterialize();
}

TEST(Vasm, ArmLoadExtFoldMappings) {
  testArmLoadExtFoldMappings();
}

TEST(Vasm, ArmLoadExtRejectsMultipleUses) {
  testArmLoadExtRejectsMultipleUses();
}

TEST(Vasm, ArmShiftedBitTestFromShrqi) {
  testArmShiftedBitTestFromShrqi();
}

TEST(Vasm, ArmShiftedBitTestFoldsBeforeJcc) {
  testArmShiftedBitTestFoldsBeforeJcc();
}

TEST(Vasm, ArmShiftedBitTestFromShrqiWithoutXor) {
  testArmShiftedBitTestFromShrqiWithoutXor();
}

TEST(Vasm, ArmShrqiTestlFoldsFlippedOperands) {
  testArmShrqiTestlFoldsFlippedOperands();
}

TEST(Vasm, ArmShrqiTestlFoldsMultipleZUses) {
  testArmShrqiTestlFoldsMultipleZUses();
}

TEST(Vasm, ArmShrqiTestlFoldsEncodableMultiBitMasks) {
  testArmShrqiTestlFoldsEncodableMultiBitMasks();
}

TEST(Vasm, ArmShrqiTestlRejectsUnsafeFolds) {
  testArmShrqiTestlRejectsUnsafeFolds();
}

TEST(Vasm, ArmShrqiTestlRejectsNonEncodableMultiBitMasks) {
  testArmShrqiTestlRejectsNonEncodableMultiBitMasks();
}

TEST(Vasm, ArmShrqiTestlRejectsOutOfRangeShift) {
  testArmShrqiTestlRejectsOutOfRangeShift();
}

TEST(Vasm, ArmShiftedBitTestRejectsOutOfRangeMask) {
  testArmShiftedBitTestRejectsOutOfRangeMask();
}

TEST(Vasm, ArmShiftedBitTestZeroExtendsByteMask) {
  testArmShiftedBitTestZeroExtendsByteMask();
}

TEST(Vasm, ArmShiftedBitTestNonAdjacent) {
  testArmShiftedBitTestNonAdjacent();
}

TEST(Vasm, ArmShiftedBitTestRejectsPhysSourceClobber) {
  testArmShiftedBitTestRejectsPhysSourceClobber();
}

TEST(Vasm, ArmShiftedBitTestRejectsSignExtendedMask) {
  testArmShiftedBitTestRejectsSignExtendedMask();
}

TEST(Vasm, ArmShiftedBitTestRejectsNonZFlagUse) {
  testArmShiftedBitTestRejectsNonZFlagUse();
}

TEST(Vasm, ArmLoadPairNoClobberBase) {
  testArmLoadPairNoClobberBase();
}

TEST(Vasm, ArmStorePairUpsSimplification) {
  testArmStorePairUpsSimplification();
}

TEST(Vasm, ArmLoadPairUpsSimplification) {
  testArmLoadPairUpsSimplification();
}

TEST(Vasm, ArmWritebackBaseOverlap) {
  testArmWritebackBaseOverlap();
}

TEST(Vasm, ArmWritebackPairUps) {
  testArmWritebackPairUps();
}

TEST(Vasm, ArmFoldTestqEqToCbzq) {
  testArmFoldTestqEqToCbzq();
}

TEST(Vasm, ArmFoldTestqNeToCbnzq) {
  testArmFoldTestqNeToCbnzq();
}

TEST(Vasm, ArmFoldTestlEqToCbzl) {
  testArmFoldTestlEqToCbzl();
}

TEST(Vasm, ArmFoldTestbEqToCbzl) {
  testArmFoldTestbEqToCbzl();
}

TEST(Vasm, ArmFoldTestbNeToCbnzl) {
  testArmFoldTestbNeToCbnzl();
}

TEST(Vasm, ArmFoldTestwEqToCbzl) {
  testArmFoldTestwEqToCbzl();
}

TEST(Vasm, ArmFoldSingleBitTestliNeToTbnzl) {
  testArmFoldSingleBitTestliNeToTbnzl();
}

TEST(Vasm, ArmFoldSingleHighBitTestqi64EqToTbzq) {
  testArmFoldSingleHighBitTestqi64EqToTbzq();
}

TEST(Vasm, ArmFoldSingleHighBitTestqEqToTbzq) {
  testArmFoldSingleHighBitTestqEqToTbzq();
}

TEST(Vasm, ArmFoldSelfTestSignCCToTestBranches) {
  testArmFoldSelfTestSignCCToTestBranches();
}

TEST(Vasm, ArmFoldTestqiRejectsSignExtendedMask) {
  testArmFoldTestqiRejectsSignExtendedMask();
}

TEST(Vasm, ArmFoldTestBitJccRejectsMultiBitMask) {
  testArmFoldTestBitJccRejectsMultiBitMask();
}

TEST(Vasm, ArmFoldTestJccRejectsNonSelfCompare) {
  testArmFoldTestJccRejectsNonSelfCompare();
}

TEST(Vasm, ArmFoldTestJccRejectsNonEqualCC) {
  testArmFoldTestJccRejectsNonEqualCC();
}

TEST(Vasm, ArmFoldTestJccNonAdjacent) {
  testArmFoldTestJccNonAdjacent();
}

TEST(Vasm, ArmFoldTestJccNonAdjacentRejectsClobber) {
  testArmFoldTestJccNonAdjacentRejectsClobber();
}

}

#endif
