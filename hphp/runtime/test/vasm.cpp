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

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/test/test-context.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/irlower-internal.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/relocation.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-text.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"

#ifdef __aarch64__
#include "hphp/runtime/vm/jit/vasm-util-arm.h"
#endif

#include "hphp/util/arch.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/data-block.h"
#include "hphp/vixl/hphp-compat.h"

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#include <optional>
#include <cstring>

#include <gtest/gtest.h>

namespace HPHP { namespace jit {

namespace {
constexpr size_t kDefaultBlockSize = 4096;
constexpr size_t kDataSize = 100;

template<typename Lcodegen, typename Ltest>
void test_vasm_areas(size_t mainSize,
                     size_t coldSize,
                     Lcodegen lcodegen,
                     Ltest ltest,
                     bool forceFarLiteral,
                     TCA logicalBase) {
  auto const mappingSize = mainSize + coldSize + kDataSize;
  auto code = static_cast<uint8_t*>(mmap(nullptr, mappingSize,
                                         PROT_READ | PROT_WRITE | PROT_EXEC,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  SCOPE_EXIT { munmap(code, mappingSize); };

  auto const coldBuffer = code + mainSize;
  auto const dataBuffer = coldBuffer + coldSize;

  CodeBlock main;
  CodeBlock cold;
  DataBlock data;
  if (logicalBase) {
    main.init(logicalBase, code, mainSize, "test");
    if (coldSize) {
      cold.init(
        logicalBase + mainSize,
        coldBuffer,
        coldSize,
        "cold"
      );
    }
    data.init(
      logicalBase + mainSize + coldSize,
      dataBuffer,
      kDataSize,
      "data"
    );
  } else {
    main.init(code, mainSize, "test");
    if (coldSize) cold.init(coldBuffer, coldSize, "cold");
    data.init(dataBuffer, kDataSize, "data");
  }

  Vunit unit;
  if (forceFarLiteral) unit.enableFarLiteral();
  Vasm vasm{unit};

  auto& v = vasm.main();
  unit.entry = v;

  lcodegen(unit, v);

  CGMeta meta;
  std::optional<Vtext> text;
  if (coldSize) {
    text.emplace(main, cold, data);
  } else {
    text.emplace(main, data);
  }
  emit(unit, *text, meta, nullptr);

  ltest(code, main, meta);
}

template<typename Lcodegen, typename Ltest>
void test_vasm(size_t blockSize,
               Lcodegen lcodegen,
               Ltest ltest,
               bool forceFarLiteral = false,
               TCA logicalBase = nullptr) {
  test_vasm_areas(
    blockSize - kDataSize,
    0,
    [&] (Vunit&, Vout& v) { lcodegen(v); },
    ltest,
    forceFarLiteral,
    logicalBase
  );
}

template<typename Lcodegen, typename Ltest>
void test_cross_area_emission(Lcodegen lcodegen,
                              Ltest ltest,
                              size_t mainSize = kDefaultBlockSize) {
  test_vasm_areas(
    mainSize,
    kDefaultBlockSize,
    lcodegen,
    [&] (uint8_t*, CodeBlock& main, CGMeta& meta) {
      ltest(main, meta);
    },
    false,
    reinterpret_cast<TCA>(0x10000000)
  );
}

template<typename T, typename Lcodegen, typename Ltest>
void test_function(Lcodegen lcodegen, Ltest ltest) {
  test_vasm(kDefaultBlockSize, lcodegen, [&] (uint8_t* code,
                                              CodeBlock&,
                                              const CGMeta&) {
    ltest((T)code);
  });
}

template<typename Lcodegen, typename Ltest>
void test_emission(size_t blockSize,
                   Lcodegen lcodegen,
                   Ltest ltest,
                   bool forceFarLiteral = false,
                   TCA logicalBase = nullptr) {
  test_vasm(blockSize, lcodegen, [&] (uint8_t*,
                                      CodeBlock& main,
                                      const CGMeta& meta) {
    ltest(main, meta);
  }, forceFarLiteral, logicalBase);
}
}

TEST(Vasm, Move128ARM) {
  // A long double will use simd 128 bit registers for arguments and return
  // types on ARM.  __m128 will do the same on x86.
  // This code checks that we actually copy a full 128 bit value when copying
  // simd registers.
#if defined(__aarch64__)
  using xmmType = long double;
#else
  using xmmType = __m128;
#endif
  using testfunc = xmmType (*)(xmmType, xmmType);
  using namespace reg;

  auto const l = [](Vout& v) {
    v << copy{Vreg{xmm1}, Vreg{xmm0}};
    v << ret{RegSet{xmm0}};
  };

  test_function<testfunc>(l, [](testfunc f) {
    union { xmmType x; struct { uint64_t t; uint64_t v; }; } a0, a1, r;

    a0.t = 0x88a6b5e2c10d3f9f; a0.v = 0x98713e0ad31c6b55;
    a1.t = 0x256b36bd8ef79236; a1.v = 0x63f4b85d224d9dfb;
    r.t = 0; r.v = 0;

    r.x = f(a0.x, a1.x);

    EXPECT_EQ(r.t, 0x256b36bd8ef79236);
    EXPECT_EQ(r.v, 0x63f4b85d224d9dfb);
  });
}

TEST(Vasm, LoadedTypeCheckIgnoresNonTypeBytes) {
  using Check = bool (*)(const TypedValue*);
  for (auto const op : {LdLoc, LdStk}) {
    for (auto const type : {TVec, TDict, TKeyset, TStr, TBool}) {
      SCOPED_TRACE(opcodeName(op));
      SCOPED_TRACE(type.toString());
      test_function<Check>([&] (Vout& v) {
        IRUnit unit{test_context};
        auto const bcctx = BCContext{test_marker(), 0};
        auto const base = unit.gen(Conjure, bcctx,
                                  op == LdLoc ? TFramePtr : TStkPtr);
        auto const load = op == LdLoc
          ? unit.gen(LdLoc, bcctx, TCell, LocalId{0}, base->dst())
          : unit.gen(LdStk, bcctx, TCell,
                     IRSPRelOffsetData{IRSPRelOffset{0}}, base->dst());
        auto const taken = unit.defBlock();
        auto const check = unit.gen(CheckType, bcctx, type, taken, load->dst());

        irlower::IRLS env{unit};
        env.vmain = &v;
        env.labels[taken] = v.makeBlock();
        env.locs[base->dst()] = Vloc{rarg(0)};
        env.locs[load->dst()] = Vloc{v.makeReg(), v.makeReg()};
        env.locs[check->dst()] = Vloc{v.makeReg()};
        if (op == LdLoc) {
          v << lea{rarg(0)[sizeof(TypedValue)], rarg(0)};
          irlower::cgLdLoc(env, load);
        } else {
          irlower::cgLdStk(env, load);
        }
        irlower::cgCheckType(env, check);
        v << ldimmb{1, rret()};
        v << ret{RegSet{rret()}};
        v = env.labels[taken];
        v << ldimmb{0, rret()};
        v << ret{RegSet{rret()}};

        auto& entry = v.unit().blocks[v.unit().entry].code;
        ASSERT_EQ(entry.back().op, Vinstr::jcc);
        auto const branch = entry.back().jcc_;
        ASSERT_EQ(branch.targets[1], env.labels[taken]);
        entry.back() = setcc{ccNegate(branch.cc), branch.sf, rret()};
        entry.emplace_back(ret{RegSet{rret()}});
        optimize(v.unit(), abi(CodeKind::Helper), true);
      }, [&] (Check check) {
        TypedValue value{};
        value.m_type = type.toDataType();
        if (type == TVec) value.m_data.parr = staticEmptyVec();
        if (type == TDict) value.m_data.parr = staticEmptyDictArray();
        if (type == TKeyset) value.m_data.parr = staticEmptyKeysetArray();
        if (type == TStr) value.m_data.pstr = staticEmptyString();
        if (type == TBool) value.m_data.num = 1;

        for (auto const extra : {uint64_t{0}, uint64_t{0x100},
                                 uint64_t{0x10000}, uint64_t{0x80000000},
                                 uint64_t{0xffffffff00000000}}) {
          SCOPED_TRACE(extra);
          auto const word = extra | static_cast<uint8_t>(value.m_type);
          std::memcpy(&value.m_type, &word, sizeof(word));
          ASSERT_TRUE(tvIsPlausible(value));
          EXPECT_TRUE(check(&value));
        }
        value.m_type = KindOfInt64;
        value.m_data.num = 42;
        EXPECT_FALSE(check(&value));
      });
    }
  }
}

TEST(Vasm, PrintVptr) {
  auto v0 = Vreg{Vreg::V0};
  auto v1 = Vreg{Vreg::V0 + 1};
  Vptr p{Vreg{v0}, Vreg{}, 1, 0};
  EXPECT_EQ("[%128]", show(p));
  p.index = v1;
  EXPECT_EQ("[%128 + %129]", show(p));
  p.scale = 4;
  EXPECT_EQ("[%128 + %129 * 4]", show(p));
  p.disp = 0xbeef;
  EXPECT_EQ("[%128 + 0xbeef + %129 * 4]", show(p));
  p.disp = -16;
  p.index = Vreg{};
  EXPECT_EQ("[%128 - 0x10]", show(p));
  p.seg = Segment::FS;
  EXPECT_EQ("[%fs + %128 - 0x10]", show(p));
  p.base = Vreg{};
  EXPECT_EQ("[%fs - 0x10]", show(p));
}

#ifdef __aarch64__

TEST(Vasm, LoadTVPreservesAux) {
  using Load = uint64_t (*)(const TypedValue*);
  for (auto const type : {TCell, TBool}) {
    SCOPED_TRACE(type.toString());
    test_function<Load>([&] (Vout& v) {
      loadTV(v, type, Vloc{rarg(1), rarg(2)},
             rarg(0)[TVOFF(m_type)], rarg(0)[TVOFF(m_data)], true);
      v << copy{rarg(2), rret()};
      v << ret{RegSet{rret()}};
      optimize(v.unit(), abi(CodeKind::Helper), true);
    }, [] (Load load) {
      TypedValue value{};
      value.m_data.num = 1;
      for (auto const aux : {0u, 1u, 0x80000000u, 0xffffffffu}) {
        for (auto const extra : {0u, 0xffffff00u}) {
          SCOPED_TRACE(aux);
          SCOPED_TRACE(extra);
          auto const word = (uint64_t{aux} << 32) | extra |
                            static_cast<uint8_t>(KindOfBoolean);
          std::memcpy(&value.m_type, &word, sizeof(word));
          EXPECT_EQ(load(&value), word);
        }
      }
    });
  }
}

TEST(Vasm, ArmNearLiteralLoadStaysLiteralLdr) {
  test_emission(4096, [] (Vout& v) {
    v << leap{reg::rip[(intptr_t)0x12340000], rarg(0)};
    v << ret{RegSet{}};
  }, [] (CodeBlock& main, const CGMeta&) {
    auto const start = vixl::Instruction::Cast(main.base());
    auto const load = jit::arm::LoadLiteral::at(start);

    ASSERT_TRUE(load);
    EXPECT_FALSE(load.isFar());

    EXPECT_EQ(start->Mask(vixl::LoadLiteralMask), vixl::LDR_w_lit);
  });
}

TEST(Vasm, ArmFarLiteralLoadPatchesToAdrpLdr) {
  constexpr size_t kBlockSize = 2 * 1024 * 1024;
  constexpr size_t kFillerNops = (1024 * 1024) / vixl::kInstructionSize + 1024;

  test_emission(kBlockSize, [] (Vout& v) {
    v << leap{reg::rip[(intptr_t)0x12340000], rarg(0)};
    for (size_t i = 0; i < kFillerNops; ++i) {
      v << nop{};
    }
    v << ret{RegSet{}};
  }, [] (CodeBlock& main, const CGMeta&) {
    auto const start = vixl::Instruction::Cast(main.base());
    auto const load = jit::arm::LoadLiteral::at(start);

    ASSERT_TRUE(load && load.isFar());
    auto const ldr = load.ldr();
    EXPECT_EQ(ldr->Mask(vixl::LoadStoreUnsignedOffsetMask), vixl::LDR_w_unsigned);
    EXPECT_EQ(start->Rd(), ldr->Rn());
    EXPECT_EQ(start->Rd(), ldr->Rt());
  },
  // Direct code emission no longer retries on far literals. Force the
  // ADRP form explicitly so this test still exercises the far-literal rewrite.
  true);
}

TEST(Vasm, ArmMcprepSeedsTaggedMovqLiteral) {
  test_emission(4096, [] (Vout& v) {
    v << mcprep{rarg(0)};
    v << ret{RegSet{}};
  }, [] (CodeBlock& main, const CGMeta&) {
    auto const start = vixl::Instruction::Cast(main.base());
    auto const load = jit::arm::LoadLiteral::at(start);
    ASSERT_TRUE(load);
    auto const literal =
      *reinterpret_cast<uint64_t*>(load.literalAddress());
    EXPECT_EQ(literal, (reinterpret_cast<uint64_t>(main.base()) << 1) | 1);
  });
}

TEST(Vasm, ArmTestBranchEmissionMarksRecognizableTst) {
  test_emission(4096, [] (Vout& v) {
    auto taken = v.makeBlock();
    auto next = v.makeBlock();
    v << tbzq{Immed{63}, Vreg64{Reg64{0}}, {taken, next}};
    taken << ret{};
    next << ret{};
  }, [] (CodeBlock& main, const CGMeta& meta) {
    auto const start = vixl::Instruction::Cast(
      main.toDestAddress(main.base())
    );
    EXPECT_TRUE(meta.testBranches.contains(main.base()));

    arm::BitTestDetails details;
    ASSERT_TRUE(arm::getBitTestDetails(start, details));
    EXPECT_EQ(details.reg.code(), vixl::x0.code());
    EXPECT_EQ(details.reg.size(), vixl::x0.size());
    EXPECT_EQ(details.bit, 63);

    EXPECT_TRUE(start->GetNextInstruction()->IsCondBranchImm());
  }, false, reinterpret_cast<TCA>(0x10000000));
}

TEST(Vasm, ArmTestBranchIdenticalTargetsOmitsTest) {
  test_emission(4096, [] (Vout& v) {
    auto target = v.makeBlock();
    v << tbzq{Immed{42}, Vreg64{Reg64{0}}, {target, target}};
    target << ret{};
  }, [] (CodeBlock& main, const CGMeta& meta) {
    EXPECT_TRUE(meta.testBranches.empty());

    arm::BitTestDetails details;
    EXPECT_FALSE(arm::getBitTestDetails(
      vixl::Instruction::Cast(main.toDestAddress(main.base())), details
    ));
  });
}

TEST(Vasm, ArmCrossAreaTestBranchRelocatesToVeneer) {
  test_cross_area_emission([] (Vunit& unit, Vout& v) {
    auto next = v.makeBlock();
    auto const takenLabel = unit.makeBlock(AreaIndex::Cold, 1);
    Vout taken{unit, takenLabel};
    v << tbzq{Immed{63}, Vreg64{Reg64{0}}, {next, takenLabel}};
    next << ret{};
    taken << ret{};
  }, [] (CodeBlock& main, CGMeta& meta) {
    auto const start = main.base();
    auto const end = main.frontier();

    auto const emitted = vixl::Instruction::Cast(main.toDestAddress(start));
    arm::BitTestDetails test;
    ASSERT_TRUE(arm::getBitTestDetails(emitted, test));
    auto const branch = emitted->GetNextInstruction();
    ASSERT_TRUE(branch->IsCondBranchImm());
    auto const branchFrom = vixl::Instruction::Cast(
      start + vixl::kInstructionSize
    );
    auto const veneer = reinterpret_cast<TCA>(
      branch->GetImmPCOffsetTarget(branchFrom)
    );
    ASSERT_TRUE(main.contains(veneer));
    EXPECT_TRUE(meta.veneerAddrs.contains(veneer));

    RelocationInfo rel;
    auto area = AreaIndex::Main;
    relocate(rel, main, start, end, main, meta, area);

    auto const relocatedAddr = rel.adjustedAddressAfter(start);
    auto const relocatedVeneer = rel.adjustedAddressAfter(veneer);
    ASSERT_NE(relocatedAddr, nullptr);
    ASSERT_NE(relocatedVeneer, nullptr);
    auto const relocated = vixl::Instruction::Cast(
      main.toDestAddress(relocatedAddr)
    );
    ASSERT_TRUE(relocated->IsTestBranch());
    auto const details = arm::getTestAndBranchDetails(relocated);
    EXPECT_FALSE(details.isTbnz);
    EXPECT_EQ(
      relocated->ImmPCOffsetTarget(),
      vixl::Instruction::CastConst(main.toDestAddress(relocatedVeneer))
    );
  });
}

TEST(Vasm, ArmFarCrossAreaConditionalVeneersPreserveSourceBranches) {
  constexpr size_t kMainSize = 2 * 1024 * 1024;
  constexpr size_t kFillerNops =
    1024 * 1024 / vixl::kInstructionSize + 1024;

  test_cross_area_emission([] (Vunit& unit, Vout& v) {
    auto compareBranch = v.makeBlock();
    auto filler = compareBranch.makeBlock();
    auto const takenLabel = unit.makeBlock(AreaIndex::Cold, 1);
    Vout taken{unit, takenLabel};

    v << jcc{
      CC_E,
      VregSF{RegSF{0}},
      {compareBranch, takenLabel},
      StringTag{}
    };
    compareBranch << cbzq{
      Vreg64{Reg64{0}},
      {filler, takenLabel}
    };
    for (size_t i = 0; i < kFillerNops; ++i) filler << nop{};
    filler << ret{};
    taken << ret{};
  }, [] (CodeBlock& main, CGMeta& meta) {
    ASSERT_EQ(meta.veneerAddrs.size(), 2);

    auto source = vixl::Instruction::Cast(
      main.toDestAddress(main.base())
    );
    auto veneerIt = meta.veneerAddrs.begin();
    auto const checkAppendix = [&] (bool compareBranch) {
      ASSERT_EQ(source->Mask(vixl::UnconditionalBranchMask), vixl::B);

      auto veneer = vixl::Instruction::Cast(
        main.toDestAddress(*veneerIt++)
      );
      auto appendix = veneer;
      for (size_t i = 0; i < 3; ++i) {
        appendix = appendix->GetNextInstruction();
      }
      EXPECT_EQ(source->ImmPCOffsetTarget(), appendix);

      if (compareBranch) {
        ASSERT_TRUE(appendix->IsCompareBranch());
        auto const details = arm::getCompareAndBranchDetails(appendix);
        EXPECT_FALSE(details.isCbnz);
        EXPECT_EQ(details.reg.code(), vixl::x0.code());
        EXPECT_EQ(details.reg.size(), vixl::x0.size());
      } else {
        ASSERT_TRUE(appendix->IsCondBranchImm());
        EXPECT_EQ(appendix->ConditionBranch(), vixl::eq);
      }
      EXPECT_EQ(appendix->ImmPCOffsetTarget(), veneer);

      auto const next = appendix->GetNextInstruction();
      ASSERT_EQ(next->Mask(vixl::UnconditionalBranchMask), vixl::B);
      EXPECT_EQ(next->ImmPCOffsetTarget(), source->GetNextInstruction());
      source = source->GetNextInstruction();
    };

    checkAppendix(false);
    checkAppendix(true);
  }, kMainSize);
}

#endif

} }
