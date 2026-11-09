/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2017-present Facebook, Inc. (http://www.facebook.com)  |
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

#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/align-arm.h"
#include "hphp/runtime/vm/jit/relocation.h"
#include "hphp/runtime/vm/jit/smashable-instr-arm.h"
#include "hphp/runtime/vm/jit/vasm-util-arm.h"
#include "hphp/util/arch.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/data-block.h"
#include "hphp/vixl/hphp-compat.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <set>

namespace HPHP::jit {

namespace arm {
using namespace vixl;

namespace {

void patchAdrpLiteralLoadTarget(Instruction* adrp,
                                Instruction* ldr,
                                const Instruction* from,
                                TCA target) {
  assertx(ldr == adrp->GetNextInstruction());
  auto const load = LoadLiteral::at(adrp);
  always_assert(load && load.isFar());
  auto const ok = load.setTarget(Instruction::Cast(target), from);
  always_assert(ok);
}

void patchAdrpLiteralLoadTarget(Instruction* adrp,
                                Instruction* ldr,
                                TCA target) {
  patchAdrpLiteralLoadTarget(adrp, ldr, adrp, target);
}

struct Veneer {
  TCA branch;
  TCA veneer;
  TCA literal;
};

struct DecodedMovWideSequence {
  uint64_t target;
  int32_t reg;
  const Instruction* next;
};

DecodedMovWideSequence decodeMovWideSequence(const Instruction* mov) {
  assertx(mov->IsMovz());
  auto const reg = mov->Rd();
  auto target = static_cast<uint64_t>(mov->ImmMoveWide())
                << (16 * mov->ShiftMoveWide());
  mov = mov->GetNextInstruction();
  while (mov->IsMovk()) {
    assertx(mov->Rd() == reg);
    target |= static_cast<uint64_t>(mov->ImmMoveWide())
              << (16 * mov->ShiftMoveWide());
    mov = mov->GetNextInstruction();
  }
  return {target, reg, mov};
}

void expectAddressImmediatesAfterRelocation(
  RelocationInfo& rel,
  CGMeta& meta,
  std::initializer_list<TCA> expected) {
  adjustMetaDataForRelocation(rel, nullptr, meta);
  EXPECT_EQ(meta.addressImmediates, std::set<TCA>(expected));
}

template<class EmitBranch>
Veneer emitDirectVeneer(MacroAssembler& a,
                        CodeBlock& main,
                        TCA target,
                        EmitBranch emitBranch) {
  vixl::Label veneerLabel, literalLabel;
  auto const branch = main.frontier();
  emitBranch(veneerLabel);
  a.bind(&veneerLabel);
  auto const veneer = main.frontier();
  a.Ldr(w17, &literalLabel);
  a.Br(x17);
  a.bind(&literalLabel);
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  return Veneer{branch, veneer, literal};
}

template<class EmitBranch>
Veneer emitFarVeneer(MacroAssembler& a,
                     CodeBlock& main,
                     TCA target,
                     EmitBranch emitBranch) {
  vixl::Label veneerLabel;
  auto const branch = main.frontier();
  emitBranch(veneerLabel);
  a.bind(&veneerLabel);
  auto const veneer = main.frontier();
  a.adrp(x17, int64_t{0});
  auto const ldr = main.frontier();
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  patchAdrpLiteralLoadTarget(
    Instruction::Cast(veneer),
    Instruction::Cast(ldr),
    literal
  );
  return Veneer{branch, veneer, literal};
}

}
uint8_t* code_;
size_t blockSize_ = 4096;

void initBlocks(size_t size,
                CodeBlock& main,
                DataBlock& data,
                uint8_t* logicalStart = nullptr) {
  blockSize_ = size;
  code_ = static_cast<uint8_t*>(mmap(nullptr, blockSize_,
                                         PROT_READ | PROT_WRITE | PROT_EXEC,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  auto dataSize = 100;
  auto codeSize = blockSize_ - dataSize;
  // None of these tests should use much data.
  auto data_buffer = code_ + codeSize;

  if (logicalStart) {
    main.init(logicalStart, code_, codeSize, "test");
    data.init(logicalStart + codeSize, data_buffer, dataSize, "data");
  } else {
    main.init(code_, codeSize, "test");
    data.init(data_buffer, dataSize, "data");
  }

}

void freeBlocks() {
  munmap(code_, blockSize_);
}

/*
 * Tests relocating PC relative conditional branches
 * to a sequence of a conditional branch and an absolute
 * branch to register. See arm::relocatePCRelativeHelper()
 *
 *     b.cc <target>
 * to
 *     b.!cc <past>
 *     movz/movk $tmp, <target>
 *     br $tmp
 */
TEST(Relocation, RelocateBccImm2MovzMovkBccReg) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit b.cc <imm> of -0x40000 (19-bit int min)
  auto start = main.frontier();

  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.b(-0x40000, eq);
  auto bccOrig = Instruction::Cast(start);
  auto const cond = static_cast<Condition>(bccOrig->ConditionBranch());

  auto end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect !b.cc, movz/movk X and br X where with imm in X
  auto bcc = instr;
  EXPECT_TRUE(bcc->IsCondBranchImm());
  EXPECT_EQ(bcc->ConditionBranch(), InvertCondition(cond));

  auto const mov = decodeMovWideSequence(bcc->GetNextInstruction());
  EXPECT_EQ(Instruction::Cast(mov.target), bccOrig->ImmPCOffsetTarget());

  auto const br = mov.next;
  EXPECT_TRUE(br->IsUncondBranchReg());
  EXPECT_EQ(br->Rn(), mov.reg);
  EXPECT_EQ(bcc->ImmPCOffsetTarget(), br->GetNextInstruction());
}

/*
 * Tests relocating PC relative compare and branch
 * to a sequence of a compare and branch and an absolute
 * branch to register. See arm::relocatePCRelativeHelper()
 *
 *     cbz $rt, <target>
 * to
 *     cbnz $rt, <past>
 *     movz/movk $tmp, <target>
 *     br $tmp
 */
TEST(Relocation, RelocateCbz2MovzMovkCbnzReg) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit cbz <imm> of -0x40000 (19-bit int min)
  auto start = main.frontier();

  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.cbz(x0, -0x40000);
  auto cbzOrig = Instruction::Cast(start);

  auto end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect cbnz, movz/movk X and br X where with imm in X
  auto cbnz = instr;
  EXPECT_TRUE(cbnz->Mask(LoadStoreUnsignedOffsetMask) == CBNZ_x);
  const auto rt = cbnz->Rt();
  EXPECT_EQ(cbzOrig->Rt(), rt);

  auto const mov = decodeMovWideSequence(cbnz->GetNextInstruction());
  EXPECT_EQ(Instruction::Cast(mov.target), cbzOrig->ImmPCOffsetTarget());

  auto const br = mov.next;
  EXPECT_TRUE(br->IsUncondBranchReg());
  EXPECT_EQ(br->Rn(), mov.reg);
  EXPECT_EQ(cbnz->ImmPCOffsetTarget(), br->GetNextInstruction());
}

uint32_t compareBranchEncoding(bool is64, bool isCbnz) {
  return is64 ? (isCbnz ? CBNZ_x : CBZ_x) : (isCbnz ? CBNZ_w : CBZ_w);
}

uint32_t invertedCompareBranchEncoding(bool is64, bool isCbnz) {
  return compareBranchEncoding(is64, !isCbnz);
}

void expectFarCondBranchMappings(RelocationInfo& rel,
                                 TCA srcHead,
                                 TCA srcFarJump,
                                 TCA srcAfter,
                                 TCA destHead,
                                 TCA destFarJump,
                                 TCA destAfter) {
  EXPECT_EQ(rel.adjustedAddressAfter(srcHead), destHead);
  EXPECT_EQ(rel.adjustedAddressAfter(srcFarJump), destFarJump);
  EXPECT_EQ(rel.adjustedAddressAfter(srcAfter), destAfter);
}

void expectRemovedFarJumpAddressImmediate(
  RelocationInfo& rel,
  TCA srcFarJump,
  TCA destTarget,
  bool targetHasIndependentMarker = false) {
  EXPECT_TRUE(rel.isAddressImmediate(srcFarJump));
  EXPECT_EQ(
    rel.isAddressImmediate(destTarget),
    targetHasIndependentMarker
  );
}

template<class EmitBranch>
TCA emitFarConditionalBranchSequence(MacroAssembler& a,
                                     CodeBlock& main,
                                     CGMeta& meta,
                                     EmitBranch emitBranch) {
  vixl::Label fallthrough;
  meta.addressImmediates.insert(main.frontier());
  emitBranch(fallthrough);

  auto const farJump = main.frontier();
  meta.addressImmediates.insert(farJump);
  a.adrp(x17, int64_t{0});
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);

  a.bind(&fallthrough);
  return farJump;
}

TCA emitFarBccSequence(MacroAssembler& a,
                       CodeBlock& main,
                       CGMeta& meta,
                       Condition cond) {
  return emitFarConditionalBranchSequence(
    a,
    main,
    meta,
    [&] (vixl::Label& fallthrough) { a.b(&fallthrough, cond); }
  );
}

TCA emitFarCompareBranchSequence(MacroAssembler& a,
                                 CodeBlock& main,
                                 CGMeta& meta,
                                 const Register& rt,
                                 bool isCbnz) {
  return emitFarConditionalBranchSequence(
    a,
    main,
    meta,
    [&] (vixl::Label& fallthrough) {
      if (isCbnz) {
        a.cbz(rt, &fallthrough);
      } else {
        a.cbnz(rt, &fallthrough);
      }
    }
  );
}

void patchFarJumpLiteral(CodeBlock& main, TCA farJump, TCA literal) {
  auto const logicalAdrp = Instruction::Cast(farJump);
  auto const adrp = Instruction::Cast(main.toDestAddress(farJump));
  patchAdrpLiteralLoadTarget(
    adrp, adrp->GetNextInstruction(), logicalAdrp, literal
  );
}

void emitNonNopPaddingPastCompareBranchRange(MacroAssembler& a) {
  constexpr auto kPaddingInstrs =
    (1024 * 1024) / kInstructionSize + 1024;
  for (auto i = 0; i < kPaddingInstrs; ++i) {
    a.brk(0);
  }
}

void emitUnrelatedFallthroughBranch(MacroAssembler& a) {
  // Keep a B adjacent to one-instruction rewrites so it cannot be mistaken for
  // the unconditional branch produced by a two-instruction rewrite.
  a.b(1);
}

/*
 * Relocate a compare-branch whose target is at the imm19 minimum, forcing the
 * far-branch veneer, and verify the veneer's head is an inverted compare-branch
 * of the SAME operand width (W for cbzl/cbnzl, X for cbzq/cbnzq) and register,
 * followed by the movz/movk <target>; br <tmp> jump. Guards the width-sensitive
 * veneer emission (a W branch must not be widened to an X branch, and vice
 * versa).
 */
void checkFarCompareBranchVeneer(const Register& rt, bool is64, bool isCbnz) {
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto start = main.frontier();
  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  if (isCbnz) {
    a.cbnz(rt, -0x40000);  // imm19 min -> forced far on relocate
  } else {
    a.cbz(rt, -0x40000);   // imm19 min -> forced far on relocate
  }
  auto cbOrig = Instruction::Cast(start);

  auto end = main.frontier();

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // Head: inverted compare-branch of the same width and register as the
  // original compare-branch.
  auto cb = instr;
  ASSERT_TRUE(cb->IsCompareBranch());
  EXPECT_EQ(
    cb->Mask(CompareBranchMask),
    invertedCompareBranchEncoding(is64, isCbnz)
  );
  EXPECT_EQ(cb->GetSixtyFourBits(), is64 ? 1 : 0);
  EXPECT_EQ(cb->Rt(), cbOrig->Rt());

  auto const mov = decodeMovWideSequence(cb->GetNextInstruction());
  EXPECT_EQ(Instruction::Cast(mov.target), cbOrig->ImmPCOffsetTarget());

  auto const br = mov.next;
  EXPECT_TRUE(br->IsUncondBranchReg());
  EXPECT_EQ(br->Rn(), mov.reg);
  EXPECT_EQ(cb->ImmPCOffsetTarget(), br->GetNextInstruction());
}

TEST(Relocation, RelocateFarCbzlToCbnzlVeneer) {
  checkFarCompareBranchVeneer(w0, false, false);  // 32-bit cbzl
}

TEST(Relocation, RelocateFarCbzqToCbnzqVeneer) {
  checkFarCompareBranchVeneer(x0, true, false);   // 64-bit cbzq
}

TEST(Relocation, RelocateFarCbnzlToCbzlVeneer) {
  checkFarCompareBranchVeneer(w0, false, true);   // 32-bit cbnzl
}

TEST(Relocation, RelocateFarCbnzqToCbzqVeneer) {
  checkFarCompareBranchVeneer(x0, true, true);    // 64-bit cbnzq
}

template<class EmitBranch, class CheckBranch>
void checkOptimizeFarConditionalBranchToDirect(EmitBranch emitBranch,
                                               CheckBranch checkBranch,
                                               bool addressImmediateTarget =
                                                 false) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  auto const farJump = emitBranch(a, main, meta);
  auto const fallthrough = main.frontier();
  emitUnrelatedFallthroughBranch(a);
  auto const target = main.frontier();
  if (addressImmediateTarget) meta.addressImmediates.insert(target);
  a.b(1);

  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedTarget, nullptr);
  expectRemovedFarJumpAddressImmediate(
    rel, farJump, relocatedTarget, addressImmediateTarget
  );
  if (addressImmediateTarget) {
    expectAddressImmediatesAfterRelocation(
      rel, meta, {end, relocatedTarget}
    );
  } else {
    expectAddressImmediatesAfterRelocation(rel, meta, {end});
  }

  ASSERT_TRUE(checkBranch(relocated));
  ASSERT_TRUE(relocated->GetNextInstruction()->IsUncondBranchImm());
  EXPECT_EQ(
    relocated->ImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(relocatedTarget))
  );
  expectFarCondBranchMappings(
    rel,
    start,
    farJump,
    fallthrough,
    end,
    relocatedTarget,
    end + kInstructionSize
  );
}

/*
 * Covers a far compare-branch whose target becomes near. The conditional head
 * shrinks to the original direct compare-branch. The old far-jump entry maps
 * to the relocated semantic target rather than the conditional instruction.
 */
void checkOptimizeFarCompareBranchToDirect(const Register& rt,
                                           bool is64,
                                           bool isCbnz) {
  checkOptimizeFarConditionalBranchToDirect(
    [&] (MacroAssembler& a, CodeBlock& main, CGMeta& meta) {
      return emitFarCompareBranchSequence(a, main, meta, rt, isCbnz);
    },
    [&] (const Instruction* relocated) {
      if (!relocated->IsCompareBranch()) return false;
      EXPECT_EQ(relocated->Mask(CompareBranchMask),
                compareBranchEncoding(is64, isCbnz));
      EXPECT_EQ(relocated->GetSixtyFourBits(), is64 ? 1 : 0);
      EXPECT_EQ(relocated->Rt(), rt.code());
      return true;
    }
  );
}

/*
 * Covers the crash shape: after a far B.cond becomes a direct B.cond, the old
 * far-jump entry maps to the shortened branch's semantic target. A veneer
 * entering there therefore does not depend on NZCV.
 */
void checkOptimizeFarBccToDirect(Condition farCond,
                                 bool addressImmediateTarget = false) {
  checkOptimizeFarConditionalBranchToDirect(
    [&] (MacroAssembler& a, CodeBlock& main, CGMeta& meta) {
      return emitFarBccSequence(a, main, meta, farCond);
    },
    [&] (const Instruction* relocated) {
      if (!relocated->IsCondBranchImm()) return false;
      EXPECT_EQ(
        static_cast<Condition>(relocated->ConditionBranch()),
        InvertCondition(farCond)
      );
      return true;
    },
    addressImmediateTarget
  );
}

/*
 * Relocate an emitted far compare-branch sequence whose forward target remains
 * outside the compare-branch range but inside the unconditional branch range.
 * The optimizer can still remove the far jump, but must patch the second
 * instruction after the target's relocated address becomes known.
 *
 *     cb!op $rt, fallthrough
 *     adrp/ldr/br target
 *   fallthrough:
 *     ...
 *   target:
 *
 * to
 *
 *     cb!op $rt, fallthrough
 *     b target
 *   fallthrough:
 */
void checkOptimizeFarCompareBranchToCompareBranchAndB(
  const Register& rt,
  bool is64,
  bool isCbnz) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  auto const farJump = emitFarCompareBranchSequence(a, main, meta, rt, isCbnz);
  auto const fallthrough = main.frontier();
  emitNonNopPaddingPastCompareBranchRange(a);
  auto const target = main.frontier();
  a.brk(1);

  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  ASSERT_TRUE(relocated->IsCompareBranch());
  EXPECT_EQ(
    relocated->Mask(CompareBranchMask),
    invertedCompareBranchEncoding(is64, isCbnz)
  );
  EXPECT_EQ(relocated->GetSixtyFourBits(), is64 ? 1 : 0);
  EXPECT_EQ(relocated->Rt(), rt.code());

  auto const branch = relocated->GetNextInstruction();
  EXPECT_EQ(relocated->ImmPCOffsetTarget(), branch->GetNextInstruction());

  ASSERT_TRUE(branch->IsUncondBranchImm());
  EXPECT_EQ(branch->Mask(UnconditionalBranchMask), B);
  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedTarget, nullptr);
  expectRemovedFarJumpAddressImmediate(rel, farJump, relocatedTarget);
  expectAddressImmediatesAfterRelocation(rel, meta, {end});
  EXPECT_EQ(
    branch->GetImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(relocatedTarget))
  );
  expectFarCondBranchMappings(
    rel,
    start,
    farJump,
    fallthrough,
    end,
    relocatedTarget,
    end + 2 * kInstructionSize
  );
}

/*
 * Covers the far B.cond path for a forward target that remains outside the
 * conditional-branch range but fits an unconditional branch. This mirrors the
 * compare-branch helper above, but exercises optimizeFarCondBranch()'s
 * IsCondBranchImm arm and verifies the deferred rewrite patches the generated
 * B target, not the conditional skip branch.
 */
void checkOptimizeFarBccToBccAndB(Condition cond) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  auto const farJump = emitFarBccSequence(a, main, meta, cond);
  auto const fallthrough = main.frontier();
  emitNonNopPaddingPastCompareBranchRange(a);
  auto const target = main.frontier();
  a.brk(1);

  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  ASSERT_TRUE(relocated->IsCondBranchImm());
  EXPECT_EQ(static_cast<Condition>(relocated->ConditionBranch()), cond);

  auto const branch = relocated->GetNextInstruction();
  EXPECT_EQ(relocated->ImmPCOffsetTarget(), branch->GetNextInstruction());

  ASSERT_TRUE(branch->IsUncondBranchImm());
  EXPECT_EQ(branch->Mask(UnconditionalBranchMask), B);
  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedTarget, nullptr);
  expectRemovedFarJumpAddressImmediate(rel, farJump, relocatedTarget);
  expectAddressImmediatesAfterRelocation(rel, meta, {end});
  EXPECT_EQ(
    branch->GetImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(relocatedTarget))
  );
  expectFarCondBranchMappings(
    rel,
    start,
    farJump,
    fallthrough,
    end,
    relocatedTarget,
    end + 2 * kInstructionSize
  );
}

template<class EmitBranch, class CheckBranch>
void checkPreserveFarConditionalBranch(EmitBranch emitBranch,
                                       CheckBranch checkBranch) {
  auto const old = Cfg::Jit::ArmDisableAdrpLdrShortening;
  Cfg::Jit::ArmDisableAdrpLdrShortening = true;
  SCOPE_EXIT { Cfg::Jit::ArmDisableAdrpLdrShortening = old; };

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4096, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  auto const start = main.frontier();
  meta.smashableLocations.insert(start);

  MacroAssembler a { main };
  auto const farJump = emitBranch(a, main, meta);
  meta.veneerAddrs.insert(farJump);
  auto const fallthrough = main.frontier();
  emitUnrelatedFallthroughBranch(a);
  auto const target = main.frontier();
  a.brk(1);
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();
  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedFarJump = end + kInstructionSize;
  auto const relocatedFallthrough =
    relocatedFarJump + (fallthrough - farJump);
  expectAddressImmediatesAfterRelocation(
    rel, meta, {end, relocatedFarJump}
  );
  EXPECT_TRUE(checkBranch(relocated));
  EXPECT_TRUE(
    LoadLiteral::at(
      Instruction::Cast(main.toDestAddress(relocatedFarJump))
    ).isFar()
  );
  expectFarCondBranchMappings(
    rel,
    start,
    farJump,
    fallthrough,
    end,
    relocatedFarJump,
    relocatedFallthrough
  );
}

TEST(Relocation, OptimizeFarCbzlToDirectCbzl) {
  checkOptimizeFarCompareBranchToDirect(w0, false, false);
}

TEST(Relocation, OptimizeFarCbzqToDirectCbzq) {
  checkOptimizeFarCompareBranchToDirect(x0, true, false);
}

TEST(Relocation, OptimizeFarCbnzlToDirectCbnzl) {
  checkOptimizeFarCompareBranchToDirect(w0, false, true);
}

TEST(Relocation, OptimizeFarCbnzqToDirectCbnzq) {
  checkOptimizeFarCompareBranchToDirect(x0, true, true);
}

TEST(Relocation, OptimizeFarBccDirectMapsFarJumpEntryToSemanticTarget) {
  checkOptimizeFarBccToDirect(ne);
}

TEST(Relocation, PreserveIndependentTargetAddressImmediate) {
  checkOptimizeFarBccToDirect(ne, true);
}

/*
 * A far Bcc whose semantic target is itself a literal load. Relocation removes
 * the far jump and maps its source address to that target. The internal-ref
 * pass walks every source address in the range, so it reaches the removed
 * ADRP; it must not then adjust the unrelated load at the target using the far
 * jump's address-immediate marker. The target's own metadata says its literal
 * is not an address, so the literal must survive relocation untouched.
 */
TEST(Relocation, RemovedFarJumpDoesNotPatchLiteralLoadAtTarget) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  auto const farJump = emitFarBccSequence(a, main, meta, ne);
  emitUnrelatedFallthroughBranch(a);

  // The branch's semantic target is a 32-bit literal load whose constant only
  // looks like a relocatable address. It is deliberately absent from
  // meta.addressImmediates. Relocation requires the far ADRP/LDR form.
  auto const target = main.frontier();
  a.adrp(x0, int64_t{0});
  a.ldr(w0, MemOperand(x0, 0));
  auto const decoy = main.frontier();
  main.dword(makeTarget32(start));
  patchFarJumpLiteral(main, target, decoy);

  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  patchFarJumpLiteral(main, farJump, literal);

  auto const decoyValueBefore =
    *reinterpret_cast<uint32_t*>(main.toDestAddress(decoy));

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  relocate(rel, main, start, end, main, meta, ai);

  // Read the relocated literal back through the relocated load, the same way
  // adjustLiteralLoadImmediate() would reach it.
  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedTarget, nullptr);
  auto const relocatedLoad = LoadLiteral::at(
    Instruction::Cast(main.toDestAddress(relocatedTarget))
  );
  ASSERT_TRUE(relocatedLoad);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(relocatedLoad.literalAddress()),
    decoyValueBefore
  );
}

TEST(Relocation, OptimizeFarCbzlToCbzlBWithForwardTarget) {
  checkOptimizeFarCompareBranchToCompareBranchAndB(w0, false, false);
}

TEST(Relocation, OptimizeFarCbzqToCbzqBWithForwardTarget) {
  checkOptimizeFarCompareBranchToCompareBranchAndB(x0, true, false);
}

TEST(Relocation, OptimizeFarCbnzlToCbnzlBWithForwardTarget) {
  checkOptimizeFarCompareBranchToCompareBranchAndB(w0, false, true);
}

TEST(Relocation, OptimizeFarCbnzqToCbnzqBWithForwardTarget) {
  checkOptimizeFarCompareBranchToCompareBranchAndB(x0, true, true);
}

TEST(Relocation, OptimizeFarBccToBccBWithForwardTarget) {
  checkOptimizeFarBccToBccAndB(lt);
}

TEST(Relocation, PreserveFarBccMapsFarJumpToRelocatedFarJump) {
  checkPreserveFarConditionalBranch(
    [&] (MacroAssembler& a, CodeBlock& main, CGMeta& meta) {
      return emitFarBccSequence(a, main, meta, ne);
    },
    [] (const Instruction* relocated) {
      return relocated->IsCondBranchImm();
    }
  );
}

TEST(Relocation, PreserveFarCbzMapsFarJumpToRelocatedFarJump) {
  checkPreserveFarConditionalBranch(
    [&] (MacroAssembler& a, CodeBlock& main, CGMeta& meta) {
      return emitFarCompareBranchSequence(a, main, meta, x0, false);
    },
    [] (const Instruction* relocated) {
      return relocated->IsCompareBranch();
    }
  );
}

/*
 * Which instruction inside a far-JCC window carries metadata that must stop the
 * window from being folded.  The far-jump slot matters because an inline
 * smashable jmp and a veneer have the same (ADRP+)LDR+BR shape as the window's
 * tail: bytes alone cannot tell a real far JCC from a conditional branch that
 * merely skips over one, so only the metadata can.
 */
enum class MarkedSite {
  None,
  Branch,
  FarJumpSmashable,
  FarJumpVeneer,
};

void markSite(CGMeta& meta, MarkedSite marked, TCA farJump) {
  switch (marked) {
    case MarkedSite::FarJumpSmashable:
      meta.smashableLocations.insert(farJump);
      break;
    case MarkedSite::FarJumpVeneer:
      meta.veneerAddrs.insert(farJump);
      break;
    case MarkedSite::None:
    case MarkedSite::Branch:
      break;
  }}

void checkRelocateSingleBitTestFarJcc(vixl::Register reg,
                                      uint8_t bit,
                                      bool isTbnz,
                                      MarkedSite marked = MarkedSite::None,
                                      bool testBranchAndB = false) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  auto const start = main.frontier();
  meta.testBranches.insert(start);

  MacroAssembler a { main };
  a.Tst(reg, uint64_t{1} << bit);
  auto const branch = main.frontier();
  if (marked == MarkedSite::Branch) meta.smashableLocations.insert(branch);
  auto const farJump = emitFarBccSequence(
    a,
    main,
    meta,
    isTbnz ? eq : ne
  );
  markSite(meta, marked, farJump);
  auto const fallthrough = main.frontier();
  emitUnrelatedFallthroughBranch(a);
  if (testBranchAndB) emitNonNopPaddingPastCompareBranchRange(a);
  auto const target = main.frontier();
  a.brk(1);

  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();
  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  if (marked != MarkedSite::None) {
    auto const relocatedFarJump = end + 2 * kInstructionSize;
    auto const relocatedFallthrough =
      relocatedFarJump + (fallthrough - farJump);
    expectAddressImmediatesAfterRelocation(
      rel,
      meta,
      {end + kInstructionSize, relocatedFarJump}
    );

    ASSERT_TRUE(relocated->IsLogicalImmediate());
    EXPECT_EQ(relocated->ImmLogical(), uint64_t{1} << bit);

    auto const relocatedBranch = relocated->GetNextInstruction();
    ASSERT_TRUE(relocatedBranch->IsCondBranchImm());
    EXPECT_EQ(
      static_cast<Condition>(relocatedBranch->ConditionBranch()),
      isTbnz ? eq : ne
    );
    EXPECT_TRUE(
      LoadLiteral::at(
        Instruction::Cast(main.toDestAddress(relocatedFarJump))
      ).isFar()
    );
    EXPECT_EQ(rel.adjustedAddressAfter(start), end);
    EXPECT_EQ(
      rel.adjustedAddressAfter(branch),
      end + kInstructionSize
    );
    EXPECT_EQ(rel.adjustedAddressAfter(farJump), relocatedFarJump);
    EXPECT_EQ(
      rel.adjustedAddressAfter(fallthrough),
      relocatedFallthrough
    );
    return;
  }

  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedTarget, nullptr);

  EXPECT_EQ(rel.adjustedAddressAfter(start), end);
  EXPECT_EQ(rel.adjustedAddressAfter(branch), nullptr);

  ASSERT_TRUE(relocated->IsTestBranch());
  ASSERT_TRUE(relocated->GetNextInstruction()->IsUncondBranchImm());
  auto const details = getTestAndBranchDetails(relocated);
  EXPECT_EQ(details.bit, bit);
  EXPECT_EQ(details.reg.code(), reg.code());
  EXPECT_EQ(details.reg.size(), bit < 32 ? kWRegSize : kXRegSize);
  expectRemovedFarJumpAddressImmediate(rel, farJump, relocatedTarget);
  expectAddressImmediatesAfterRelocation(rel, meta, {branch});

  if (testBranchAndB) {
    auto const relocatedFallthrough = end + 2 * kInstructionSize;
    EXPECT_EQ(details.isTbnz, !isTbnz);
    EXPECT_EQ(
      relocated->ImmPCOffsetTarget(),
      Instruction::CastConst(main.toDestAddress(relocatedFallthrough))
    );
    auto const directBranch = relocated->GetNextInstruction();
    EXPECT_EQ(
      directBranch->ImmPCOffsetTarget(),
      Instruction::CastConst(main.toDestAddress(relocatedTarget))
    );
    EXPECT_EQ(rel.adjustedAddressAfter(farJump), relocatedTarget);
    EXPECT_EQ(
      rel.adjustedAddressAfter(fallthrough),
      relocatedFallthrough
    );
  } else {
    EXPECT_EQ(details.isTbnz, isTbnz);
    EXPECT_EQ(
      relocated->ImmPCOffsetTarget(),
      Instruction::CastConst(main.toDestAddress(relocatedTarget))
    );
    EXPECT_EQ(rel.adjustedAddressAfter(farJump), relocatedTarget);
    EXPECT_EQ(
      rel.adjustedAddressAfter(fallthrough),
      end + kInstructionSize
    );
  }
}

TEST(Relocation, OptimizeSingleBitTestFarJccPreservesFarJumpEntry) {
  checkRelocateSingleBitTestFarJcc(x0, 63, false);
}

TEST(Relocation, OptimizeSingleBitTestFarJccToTbnzq) {
  checkRelocateSingleBitTestFarJcc(x0, 7, true);
}

TEST(Relocation, OptimizeSingleBitTestFarJccToTbzlPreservesBit31) {
  checkRelocateSingleBitTestFarJcc(w0, 31, false);
}

TEST(Relocation, OptimizeSingleBitTestFarJccToTbnzl) {
  checkRelocateSingleBitTestFarJcc(w0, 7, true);
}

TEST(Relocation, OptimizeSingleBitTestFarJccToTbnzB) {
  checkRelocateSingleBitTestFarJcc(x0, 63, false, MarkedSite::None, true);
}

TEST(Relocation, OptimizeSingleBitTestFarJccToTbzB) {
  checkRelocateSingleBitTestFarJcc(w0, 31, true, MarkedSite::None, true);
}

TEST(Relocation, PreserveSmashableSingleBitTestFarJcc) {
  auto const old = Cfg::Jit::ArmDisableAdrpLdrShortening;
  Cfg::Jit::ArmDisableAdrpLdrShortening = true;
  SCOPE_EXIT { Cfg::Jit::ArmDisableAdrpLdrShortening = old; };
  checkRelocateSingleBitTestFarJcc(x0, 63, false, MarkedSite::Branch);
}

// The window's (ADRP; LDR; BR) tail can be an independent inline smashable jmp
// that the conditional branch merely skips over.  Folding the window would
// destroy that site and drop its start from the relocation map, leaving its
// bind pointing into the stale source buffer.
TEST(Relocation, PreserveSmashableJmpInSingleBitTestFarJccWindow) {
  checkRelocateSingleBitTestFarJcc(
    x0, 63, false, MarkedSite::FarJumpSmashable
  );
}

// A veneer has the same tail shape and is just as unrecoverable once folded.
TEST(Relocation, PreserveVeneerInSingleBitTestFarJccWindow) {
  checkRelocateSingleBitTestFarJcc(x0, 63, false, MarkedSite::FarJumpVeneer);
}

template<class Check>
void checkRelocateSingleBitTestNearJcc(size_t padding, Check check) {
  CodeBlock main;
  DataBlock data;
  initBlocks(128 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  auto const start = main.frontier();
  meta.testBranches.insert(start);

  MacroAssembler a { main };
  vixl::Label targetLabel;
  a.Tst(x0, uint64_t{1} << 63);
  auto const branch = main.frontier();
  meta.addressImmediates.insert(branch);
  a.b(&targetLabel, eq);
  for (size_t i = 0; i < padding; ++i) a.brk(0);
  a.bind(&targetLabel);
  auto const target = main.frontier();
  a.brk(1);
  auto const end = main.frontier();

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  auto const adjustedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(adjustedTarget, nullptr);
  if (relocated->IsTestBranch()) {
    EXPECT_EQ(rel.adjustedAddressAfter(branch), nullptr);
  } else {
    EXPECT_EQ(
      rel.adjustedAddressAfter(branch),
      end + kInstructionSize
    );
  }
  check(
    relocated,
    Instruction::CastConst(main.toDestAddress(adjustedTarget))
  );
  auto const expectedBranch = relocated->IsTestBranch()
    ? branch
    : end + kInstructionSize;
  expectAddressImmediatesAfterRelocation(rel, meta, {expectedBranch});
  EXPECT_TRUE(meta.testBranches.empty());
}

void checkRelocateSingleBitTestNearJccToTbzq(size_t padding) {
  checkRelocateSingleBitTestNearJcc(
    padding,
    [] (const Instruction* relocated, const Instruction* target) {
      ASSERT_TRUE(relocated->IsTestBranch());
      auto const details = getTestAndBranchDetails(relocated);
      EXPECT_FALSE(details.isTbnz);
      EXPECT_EQ(details.bit, 63);
      EXPECT_EQ(details.reg.code(), x0.code());
      EXPECT_EQ(relocated->ImmPCOffsetTarget(), target);
    }
  );
}

// The padding follows the TST and B.cond instructions.
constexpr size_t kNearJccTargetPrefix = 2;
constexpr size_t kMaxTestBranchForwardOffset = (1 << 13) - 1;

TEST(Relocation, OptimizeSingleBitTestNearJccToTbzqClearsMetadata) {
  checkRelocateSingleBitTestNearJccToTbzq(1);
}

TEST(Relocation, OptimizeSingleBitTestNearJccAtRangeLimit) {
  checkRelocateSingleBitTestNearJccToTbzq(
    kMaxTestBranchForwardOffset - kNearJccTargetPrefix
  );
}

TEST(Relocation, PreserveSingleBitTestNearJccOutsideTestBranchRange) {
  checkRelocateSingleBitTestNearJcc(
    kMaxTestBranchForwardOffset + 1 - kNearJccTargetPrefix,
    [] (const Instruction* relocated, const Instruction* target) {
      BitTestDetails test;
      ASSERT_TRUE(getBitTestDetails(relocated, test));
      EXPECT_EQ(test.bit, 63);
      EXPECT_EQ(test.reg.code(), x0.code());

      auto const branch = relocated->GetNextInstruction();
      ASSERT_TRUE(branch->IsCondBranchImm());
      EXPECT_EQ(static_cast<Condition>(branch->ConditionBranch()), eq);
      EXPECT_EQ(branch->ImmPCOffsetTarget(), target);
    }
  );
}

TEST(Relocation, RejectMarkedTestBranchInput) {
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  auto const start = main.frontier();
  meta.testBranches.insert(start);

  MacroAssembler a { main };
  a.tbz(x0, 63, 1);
  auto const end = main.frontier();

  EXPECT_DEATH(
    {
      RelocationInfo rel;
      AreaIndex ai = AreaIndex::Main;
      relocate(rel, main, start, end, main, meta, ai);
    },
    "marked test branch.*must be TST"
  );
}

TEST(Relocation, RelocateMarkedTestFarJccRetriesGrowingInternalTarget) {
  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  MacroAssembler a { main };
  auto const start = main.frontier();
  auto const target = start;
  a.brk(0);

  constexpr auto kExpandingBranches = 4096;
  for (auto i = 0; i < kExpandingBranches; ++i) {
    a.b(-0x40000, eq);
  }

  auto const test = main.frontier();
  meta.testBranches.insert(test);
  a.Tst(x0, uint64_t{1} << 63);
  auto const branch = main.frontier();
  auto const farJump = emitFarBccSequence(a, main, meta, ne);
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();
  patchFarJumpLiteral(main, farJump, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedAddr = rel.adjustedAddressAfter(test);
  auto const relocatedTarget = rel.adjustedAddressAfter(target);
  ASSERT_NE(relocatedAddr, nullptr);
  ASSERT_NE(relocatedTarget, nullptr);
  auto const relocated = Instruction::Cast(main.toDestAddress(relocatedAddr));
  ASSERT_TRUE(relocated->IsLogicalImmediate());
  EXPECT_EQ(relocated->ImmLogical(), uint64_t{1} << 63);

  auto const relocatedBranch = relocated->GetNextInstruction();
  ASSERT_TRUE(relocatedBranch->IsCondBranchImm());
  EXPECT_EQ(static_cast<Condition>(relocatedBranch->ConditionBranch()), eq);
  EXPECT_EQ(
    relocatedBranch->ImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(relocatedTarget))
  );
  auto const relocatedBranchAddr = rel.adjustedAddressAfter(branch);
  ASSERT_NE(relocatedBranchAddr, nullptr);
  EXPECT_EQ(rel.adjustedAddressAfter(farJump), relocatedTarget);
  expectRemovedFarJumpAddressImmediate(rel, farJump, relocatedTarget);
  expectAddressImmediatesAfterRelocation(
    rel, meta, {relocatedBranchAddr}
  );
}

/*
 * Tests relocating PC relative test bit and branch
 * to a sequence of a test bit and branch and an absolute
 * branch to register. See arm::relocatePCRelativeHelper()
 *
 *     tbz $rt, <bit>, <target>
 * to
 *     tbnz $rt, <bit>, <past>
 *     movz/movk $tmp, <target>
 *     br $tmp
 */
void checkRelocateFarTestBranch(uint8_t bit) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit tbz <imm> of -0x2000 (14-bit int min)
  auto const start = main.frontier();

  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.tbz(x0, bit, -0x2000);
  auto const tbzOrig = Instruction::Cast(start);

  auto const end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect tbnz, movz/movk X and br X where with imm in X
  auto const tbnz = instr;
  ASSERT_TRUE(tbnz->IsTestBranch());
  auto const details = getTestAndBranchDetails(tbnz);
  EXPECT_TRUE(details.isTbnz);
  EXPECT_EQ(details.bit, bit);
  EXPECT_EQ(details.reg.code(), tbzOrig->Rt());

  auto const mov = decodeMovWideSequence(tbnz->GetNextInstruction());
  EXPECT_EQ(Instruction::Cast(mov.target), tbzOrig->ImmPCOffsetTarget());

  auto const br = mov.next;
  EXPECT_TRUE(br->IsUncondBranchReg());
  EXPECT_EQ(br->Rn(), mov.reg);
  EXPECT_EQ(tbnz->ImmPCOffsetTarget(), br->GetNextInstruction());
}

TEST(Relocation, RelocateTbz2MovzMovkTbnzReg) {
  checkRelocateFarTestBranch(3);
}

// Bit 63 lives in the b5 field, which is separate from bits 4-0 (b40). A
// decode that reads only b40 would expand the branch as a test of bit 31.
TEST(Relocation, RelocateTbz2MovzMovkTbnzRegPreservesBit63) {
  checkRelocateFarTestBranch(63);
}

/*
 * See arm::relocateImmediateHelper().
 */
TEST(Relocation, RelocateMovzMovkLdr2LdrLiteral) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit movz $x17 addr; movk $x17 addr; ldr $0, $x17
  auto start = main.frontier();

  MacroAssembler a { main };
  // We will kill this nop during relocation.
  a.nop();
  // The relocator won't know this is a litteral.  This seems like a problem
  // with this test.
  a.dc64(0xdeadbeef);
  meta.addressImmediates.insert(main.frontier());
  a.Mov(x17, reinterpret_cast<uint64_t>(start + 4));

  a.Ldr(x0, MemOperand(x17, 0));

  auto end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end + 8);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect a Ldr Literal to $x0 from addr
  EXPECT_TRUE(instr->IsLoadLiteral());
  EXPECT_EQ(instr->Rd(), 0);
  EXPECT_EQ(reinterpret_cast<TCA>(instr->GetLiteralAddress<uint8_t*>()), end);
}

/*
 * Tests the adjustment of a movz/movk target after that
 * movz/movk is relocated. See arm::relocateImmediateHelper().
 */
TEST(Relocation, RelocateAdjustedMovzMovk) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(5 << 20, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit Mov of start into x17 after padding out 2MB+.
  auto start = main.frontier();

  main.setFrontier(start + (2 << 20) + 16);
  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.Mov(x17, reinterpret_cast<uint64_t>(start + 8));

  auto end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect movz/movk of new target
  auto const mov = decodeMovWideSequence(instr + (2 << 20) + 16);
  EXPECT_EQ(mov.reg, 17);
  EXPECT_EQ(Instruction::Cast(mov.target), Instruction::Cast(end + 8));
}

TEST(Relocation, RelocateAdrpLdrLiteral) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4 * 1024 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();

  MacroAssembler a { main };
  a.adrp(x0, int64_t{0});
  a.ldr(x0, MemOperand(x0, 0));
  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.brk(0);
  }

  auto const literal = main.frontier();
  main.qword(0xfeedfacecafebeef);
  auto const end = main.frontier();

  auto const adrp = Instruction::Cast(start);
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocatedAdrp = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedLoad = LoadLiteral::at(relocatedAdrp);
  EXPECT_TRUE(relocatedLoad && relocatedLoad.isFar());
  EXPECT_EQ(
    reinterpret_cast<TCA>(relocatedLoad.literalAddress()),
    rel.adjustedAddressAfter(literal)
  );
}

TEST(Relocation, RelocateAdrpKeeps4GBRange) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4096, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };
  a.adrp(x0, int64_t{0});
  auto const end = main.frontier();

  auto const logicalAdrp = Instruction::Cast(start);
  auto const adrp = Instruction::Cast(main.toDestAddress(start));
  auto const target = start + (2 << 20);
  adrp->SetImmPCOffsetTarget(Instruction::Cast(target), logicalAdrp);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  EXPECT_TRUE(relocated->IsPCRelAddressing());
  EXPECT_EQ(relocated->Mask(PCRelAddressingMask), ADRP);
  EXPECT_EQ(
    relocated->GetImmPCOffsetTarget(Instruction::Cast(end)),
    Instruction::CastConst(target)
  );
}

TEST(Relocation, ShrinkAdrpLdrLiteralBackToLdr) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4 * 1024 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };
  a.adrp(x0, int64_t{0});
  a.ldr(x0, MemOperand(x0, 0));
  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.qword(0xfeedfacecafebeef);
  auto const end = main.frontier();

  auto const adrp = Instruction::Cast(start);
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedLdr = relocated;
  EXPECT_TRUE(relocatedLdr->IsLoadLiteral());
  EXPECT_EQ(relocatedLdr->Mask(LoadLiteralMask), LDR_x_lit);
  EXPECT_EQ(
    reinterpret_cast<TCA>(relocatedLdr->GetLiteralAddress<uint8_t*>()),
    rel.adjustedAddressAfter(literal)
  );
}

/*
 * Triggers the JmpOutOfRange retry path in relocate() for multiple internal
 * ADRP/LDR pairs in a single relocation, verifying env.far accumulates across
 * them.
 *
 * Layout:
 *   ADRP, LDR              (pair 1, target literal1)
 *   ADRP, LDR              (pair 2, target literal2)
 *   brk * N                (~1MB+ of non-NOP padding; NOPs would be stripped
 *                           by relocate-arm.cpp's NOP-skipping path and
 *                           defeat the test by bringing literals close)
 *   literal1
 *   literal2
 *
 * First pass: optimizeAdrpLiteralLoad shrinks both pairs to single near LDRs
 * with placeholder offsets, queuing two rewriteAdjust patches. After all
 * source is copied, both literals sit >1MB from their respective LDRs in the
 * destination. The rewriteAdjust loop's writePCRelative call fails imm19 for
 * each, inserts both ADRPs into env.far, and throws JmpOutOfRange.
 *
 * Retry: env.far contains both ADRPs; optimizeAdrpLiteralLoad bails on each,
 * so both fall through to the 2-instruction ADRP/LDR copy path and the
 * internal-refs adjustment patches the page / page-offset for both.
 */
TEST(Relocation, RetryMultipleAdrpLdrLiteralsBeyondImm19) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4 * 1024 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  auto const adrp1 = main.frontier();
  a.adrp(x0, int64_t{0});
  auto const ldr1 = main.frontier();
  a.ldr(x0, MemOperand(x0, 0));

  auto const adrp2 = main.frontier();
  a.adrp(x1, int64_t{0});
  auto const ldr2 = main.frontier();
  a.ldr(x1, MemOperand(x1, 0));

  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.brk(0);
  }

  auto const literal1 = main.frontier();
  main.qword(0xfeedfacecafebeef);
  auto const literal2 = main.frontier();
  main.qword(0x0123456789abcdef);
  auto const end = main.frontier();

  patchAdrpLiteralLoadTarget(
    Instruction::Cast(adrp1), Instruction::Cast(ldr1), literal1
  );
  patchAdrpLiteralLoadTarget(
    Instruction::Cast(adrp2), Instruction::Cast(ldr2), literal2
  );

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocatedStart = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedLoad1 = LoadLiteral::at(relocatedStart);
  EXPECT_TRUE(relocatedLoad1 && relocatedLoad1.isFar());
  EXPECT_EQ(
    reinterpret_cast<TCA>(relocatedLoad1.literalAddress()),
    rel.adjustedAddressAfter(literal1)
  );

  auto const relocatedAdrp2 =
    relocatedStart->GetNextInstruction()->GetNextInstruction();
  auto const relocatedLoad2 = LoadLiteral::at(relocatedAdrp2);
  EXPECT_TRUE(relocatedLoad2 && relocatedLoad2.isFar());
  EXPECT_EQ(
    reinterpret_cast<TCA>(relocatedLoad2.literalAddress()),
    rel.adjustedAddressAfter(literal2)
  );
}

TEST(Relocation, DisableAdrpLdrShorteningKeepsAdrpLdr) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  auto const old = Cfg::Jit::ArmDisableAdrpLdrShortening;
  Cfg::Jit::ArmDisableAdrpLdrShortening = true;
  SCOPE_EXIT { Cfg::Jit::ArmDisableAdrpLdrShortening = old; };

  CodeBlock main;
  DataBlock data;
  initBlocks(4 * 1024 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };
  a.adrp(x0, int64_t{0});
  a.ldr(x0, MemOperand(x0, 0));
  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.qword(0xfeedfacecafebeef);
  auto const end = main.frontier();

  auto const adrp = Instruction::Cast(start);
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocatedAdrp = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocatedLoad = LoadLiteral::at(relocatedAdrp);
  EXPECT_TRUE(relocatedLoad && relocatedLoad.isFar());
  EXPECT_EQ(
    reinterpret_cast<TCA>(relocatedLoad.literalAddress()),
    rel.adjustedAddressAfter(literal)
  );
}

TEST(Relocation, OptimizeAdrpLdrBrToB) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x10000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  meta.addressImmediates.insert(start);
  MacroAssembler a { main };
  a.adrp(x17, int64_t{0});
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);
  auto const target = main.frontier();
  a.brk(0);
  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  auto const logicalAdrp = Instruction::Cast(start);
  auto const adrp = Instruction::Cast(main.toDestAddress(start));
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, logicalAdrp, literal);

  auto const origLoad = LoadLiteral::at(adrp);
  ASSERT_TRUE(origLoad && origLoad.isFar());
  ASSERT_EQ(
    reinterpret_cast<TCA>(origLoad.literalAddress(logicalAdrp)),
    literal
  );
  auto const actualLiteral = origLoad.literalAddress();
  ASSERT_EQ(actualLiteral, main.toDestAddress(literal));
  ASSERT_EQ(
    *reinterpret_cast<uint32_t*>(actualLiteral),
    makeTarget32(target)
  );

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  EXPECT_TRUE(relocated->IsUncondBranchImm());
  EXPECT_EQ(relocated->Mask(UnconditionalBranchMask), B);
  EXPECT_EQ(
    relocated->GetImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(rel.adjustedAddressAfter(target)))
  );
}

TEST(Relocation, ShrinkExternalAdrpLdrBrToLdrBr) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x11000000);
  initBlocks(4 * 1024 * 1024, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const target = reinterpret_cast<TCA>(0x12345000);
  auto const start = main.frontier();
  meta.addressImmediates.insert(start);
  MacroAssembler a { main };
  a.adrp(x17, int64_t{0});
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);
  for (auto i = 0; i < (1024 * 1024) / kInstructionSize + 1024; ++i) {
    a.nop();
  }
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();

  auto const logicalAdrp = Instruction::Cast(start);
  auto const adrp = Instruction::Cast(main.toDestAddress(start));
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, logicalAdrp, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(main.toDestAddress(end));
  relocate(rel, main, start, end, main, meta, ai);

  EXPECT_TRUE(relocated->IsLoadLiteral());
  EXPECT_EQ(relocated->Mask(LoadLiteralMask), LDR_w_lit);
  auto const relocatedBr = relocated->GetNextInstruction();
  EXPECT_EQ(relocatedBr->Mask(UnconditionalBranchToRegisterMask), BR);
  EXPECT_EQ(relocatedBr->Rn(), relocated->Rd());

  auto const relocatedLiteral = relocated->GetLiteralAddress<uint8_t*>();
  EXPECT_EQ(
    relocatedLiteral,
    main.toDestAddress(rel.adjustedAddressAfter(literal))
  );
  EXPECT_EQ(*reinterpret_cast<uint32_t*>(relocatedLiteral), makeTarget32(target));
}

TEST(Relocation, AdrpLdrBrVeneerCanBeSmashed) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  MacroAssembler a { main };

  auto const oldTarget = reinterpret_cast<TCA>(0x123400);
  auto const newCallTarget = reinterpret_cast<TCA>(0x123800);
  auto const newJmpTarget = reinterpret_cast<TCA>(0x123c00);
  auto const newJccTarget = reinterpret_cast<TCA>(0x124000);

  auto const directCall = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const directJmp = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const directJcc = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  auto const farCall = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const farJmp = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const farJcc = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  EXPECT_EQ(smashableCallTarget(directCall.branch), oldTarget);
  EXPECT_TRUE(possiblySmashableJmp(directJmp.branch));
  EXPECT_TRUE(possiblySmashableJcc(directJcc.branch));

  EXPECT_EQ(smashableCallTarget(farCall.branch), oldTarget);
  EXPECT_TRUE(possiblySmashableJmp(farJmp.branch));
  EXPECT_TRUE(possiblySmashableJcc(farJcc.branch));

  smashCall(farCall.branch, newCallTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farCall.literal),
    makeTarget32(newCallTarget)
  );

  EXPECT_EQ(smashableJmpTarget(farJmp.branch), oldTarget);
  smashJmp(farJmp.branch, newJmpTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farJmp.literal),
    makeTarget32(newJmpTarget)
  );

  EXPECT_EQ(smashableJccTarget(farJcc.branch), oldTarget);
  smashJcc(farJcc.branch, newJccTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farJcc.literal),
    makeTarget32(newJccTarget)
  );
}

TEST(Relocation, SmashableVeneerTargetsCanBeReadAndSmashed) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  auto const oldRepoAuth = Cfg::Repo::Authoritative;
  Cfg::Repo::Authoritative = false;
  SCOPE_EXIT { Cfg::Repo::Authoritative = oldRepoAuth; };

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  MacroAssembler a { main };

  auto const oldTarget = reinterpret_cast<TCA>(0x123400);
  auto const newCallTarget = reinterpret_cast<TCA>(0x123800);
  auto const newJmpTarget = reinterpret_cast<TCA>(0x123c00);
  auto const newJccTarget = reinterpret_cast<TCA>(0x124000);

  auto const directCall = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const directJmp = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const directJcc = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  auto const farCall = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const farJmp = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const farJcc = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  EXPECT_EQ(smashableCallTarget(directCall.branch), oldTarget);
  EXPECT_EQ(smashableJmpTarget(directJmp.branch), oldTarget);
  EXPECT_EQ(smashableJccTarget(directJcc.branch), oldTarget);
  EXPECT_EQ(smashableJccCond(directJcc.branch), CC_L);
  EXPECT_FALSE(optimizeSmashedCall(directCall.branch));
  EXPECT_FALSE(optimizeSmashedJmp(directJmp.branch));
  EXPECT_FALSE(optimizeSmashedJcc(directJcc.branch));

  EXPECT_EQ(smashableCallTarget(farCall.branch), oldTarget);
  EXPECT_EQ(smashableJmpTarget(farJmp.branch), oldTarget);
  EXPECT_EQ(smashableJccTarget(farJcc.branch), oldTarget);
  EXPECT_EQ(smashableJccCond(farJcc.branch), CC_L);

  smashCall(directCall.branch, newCallTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(directCall.literal),
    makeTarget32(newCallTarget)
  );
  EXPECT_EQ(smashableCallTarget(directCall.branch), newCallTarget);
  smashJmp(directJmp.branch, newJmpTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(directJmp.literal),
    makeTarget32(newJmpTarget)
  );
  EXPECT_EQ(smashableJmpTarget(directJmp.branch), newJmpTarget);
  smashJcc(directJcc.branch, newJccTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(directJcc.literal),
    makeTarget32(newJccTarget)
  );
  EXPECT_EQ(smashableJccTarget(directJcc.branch), newJccTarget);

  smashCall(farCall.branch, newCallTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farCall.literal),
    makeTarget32(newCallTarget)
  );
  EXPECT_EQ(smashableCallTarget(farCall.branch), newCallTarget);
  smashJmp(farJmp.branch, newJmpTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farJmp.literal),
    makeTarget32(newJmpTarget)
  );
  EXPECT_EQ(smashableJmpTarget(farJmp.branch), newJmpTarget);
  smashJcc(farJcc.branch, newJccTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(farJcc.literal),
    makeTarget32(newJccTarget)
  );
  EXPECT_EQ(smashableJccTarget(farJcc.branch), newJccTarget);
  EXPECT_FALSE(optimizeSmashedJcc(farJcc.branch));
}

TEST(Relocation, SmashableMovqCanBeReadAndSmashed) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;
  MacroAssembler a { main };

  auto const nearImm = 0x0123456789abcdefULL;
  auto const farImm = 0xfedcba9876543210ULL;
  auto const patchedNearImm = 0x1111222233334444ULL;
  auto const patchedFarImm = 0x5555666677778888ULL;

  align(main, &meta, Alignment::SmashMovq, AlignContext::Live);
  auto const nearMovq = main.frontier();
  vixl::Label nearLiteralLabel;
  a.Ldr(x0, &nearLiteralLabel);
  a.bind(&nearLiteralLabel);
  auto const nearLiteral = main.frontier();
  main.qword(nearImm);

  align(main, &meta, Alignment::SmashMovq, AlignContext::Live);
  auto const farMovq = main.frontier();
  a.adrp(x1, int64_t{0});
  auto const farLdr = main.frontier();
  a.ldr(x1, MemOperand(x1, 0));
  align(main, &meta, Alignment::QuadWordSmashable, AlignContext::Live);
  auto const farLiteral = main.frontier();
  main.qword(farImm);
  patchAdrpLiteralLoadTarget(
    Instruction::Cast(farMovq),
    Instruction::Cast(farLdr),
    farLiteral
  );

  ASSERT_TRUE(possiblySmashableMovq(nearMovq));
  ASSERT_TRUE(possiblySmashableMovq(farMovq));
  EXPECT_EQ(smashableMovqImm(nearMovq), nearImm);
  EXPECT_EQ(smashableMovqImm(farMovq), farImm);

  smashMovq(nearMovq, patchedNearImm);
  smashMovq(farMovq, patchedFarImm);
  EXPECT_TRUE(possiblySmashableMovq(nearMovq));
  EXPECT_TRUE(possiblySmashableMovq(farMovq));
  EXPECT_EQ(smashableMovqImm(nearMovq), patchedNearImm);
  EXPECT_EQ(smashableMovqImm(farMovq), patchedFarImm);
  EXPECT_EQ(*reinterpret_cast<uint64_t*>(nearLiteral), patchedNearImm);
  EXPECT_EQ(*reinterpret_cast<uint64_t*>(farLiteral), patchedFarImm);
}

TEST(Relocation, SmashableTargetsOptimizeInRepoAuthoritativeMode) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  auto const oldRepoAuth = Cfg::Repo::Authoritative;
  Cfg::Repo::Authoritative = true;
  SCOPE_EXIT { Cfg::Repo::Authoritative = oldRepoAuth; };

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  MacroAssembler a { main };

  auto const oldTarget = reinterpret_cast<TCA>(0x123400);

  auto const call = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const jmp = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const jcc = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  auto const callTarget = main.frontier();
  a.brk(0);
  auto const jmpTarget = main.frontier();
  a.brk(0);
  auto const jccTarget = main.frontier();
  a.brk(0);

  if ((reinterpret_cast<uintptr_t>(callTarget) >> 32) ||
      (reinterpret_cast<uintptr_t>(jmpTarget) >> 32) ||
      (reinterpret_cast<uintptr_t>(jccTarget) >> 32)) {
    SUCCEED();
    return;
  }

  ASSERT_EQ(smashableCallTarget(call.branch), oldTarget);
  ASSERT_TRUE(possiblySmashableJmp(jmp.branch));
  ASSERT_TRUE(possiblySmashableJcc(jcc.branch));

  smashCall(call.branch, callTarget);
  smashJmp(jmp.branch, jmpTarget);
  smashJcc(jcc.branch, jccTarget);

  EXPECT_EQ(*reinterpret_cast<uint32_t*>(call.literal), makeTarget32(callTarget));
  EXPECT_EQ(*reinterpret_cast<uint32_t*>(jmp.literal), makeTarget32(jmpTarget));
  EXPECT_EQ(*reinterpret_cast<uint32_t*>(jcc.literal), makeTarget32(jccTarget));

  auto const callInst = Instruction::Cast(call.branch);
  EXPECT_EQ(callInst->Mask(UnconditionalBranchMask), BL);
  EXPECT_EQ(callInst->GetImmPCOffsetTarget(), Instruction::CastConst(callTarget));
  EXPECT_EQ(smashableCallTarget(call.branch), nullptr);
  EXPECT_FALSE(optimizeSmashedCall(call.branch));

  auto const jmpInst = Instruction::Cast(jmp.branch);
  EXPECT_EQ(jmpInst->Mask(UnconditionalBranchMask), B);
  EXPECT_EQ(jmpInst->GetImmPCOffsetTarget(), Instruction::CastConst(jmpTarget));
  EXPECT_FALSE(possiblySmashableJmp(jmp.branch));
  EXPECT_FALSE(optimizeSmashedJmp(jmp.branch));

  auto const jccInst = Instruction::Cast(jcc.branch);
  EXPECT_TRUE(jccInst->IsCondBranchImm());
  EXPECT_EQ(static_cast<Condition>(jccInst->ConditionBranch()), lt);
  EXPECT_EQ(jccInst->GetImmPCOffsetTarget(), Instruction::CastConst(jccTarget));
  EXPECT_FALSE(possiblySmashableJcc(jcc.branch));
  EXPECT_FALSE(optimizeSmashedJcc(jcc.branch));
}

TEST(Relocation, SmashableTargetsStayIndirectWhenTargetIsVeneer) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  auto const oldRepoAuth = Cfg::Repo::Authoritative;
  Cfg::Repo::Authoritative = true;
  SCOPE_EXIT { Cfg::Repo::Authoritative = oldRepoAuth; };

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  MacroAssembler a { main };

  auto const oldTarget = reinterpret_cast<TCA>(0x123400);
  auto const target = emitDirectVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const call = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.bl(&label); }
  );
  auto const jmp = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label); }
  );
  auto const jcc = emitFarVeneer(
    a,
    main,
    oldTarget,
    [&] (vixl::Label& label) { a.b(&label, lt); }
  );

  if (reinterpret_cast<uintptr_t>(target.veneer) >> 32) {
    SUCCEED();
    return;
  }

  smashCall(call.branch, target.veneer);
  smashJmp(jmp.branch, target.veneer);
  smashJcc(jcc.branch, target.veneer);

  EXPECT_EQ(smashableCallTarget(call.branch), target.veneer);
  EXPECT_EQ(smashableJmpTarget(jmp.branch), target.veneer);
  EXPECT_EQ(smashableJccTarget(jcc.branch), target.veneer);
}

TEST(Relocation, OptimizeSmashedJccRewritesFarVeneer) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  auto const oldRepoAuth = Cfg::Repo::Authoritative;
  Cfg::Repo::Authoritative = true;
  SCOPE_EXIT { Cfg::Repo::Authoritative = oldRepoAuth; };

  CodeBlock main;
  DataBlock data;
  initBlocks(4 * 1024 * 1024, main, data);
  SCOPE_EXIT { freeBlocks(); };

  MacroAssembler a { main };

  vixl::Label veneerLabel;
  auto const branch = main.frontier();
  auto const oldTarget = branch + (2 << 20);
  auto const newTarget = branch + (3 << 20);
  if (reinterpret_cast<uintptr_t>(newTarget) >> 32) {
    SUCCEED();
    return;
  }

  a.b(&veneerLabel, lt);
  a.bind(&veneerLabel);
  auto const veneer = main.frontier();
  a.adrp(x17, int64_t{0});
  auto const ldr = main.frontier();
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);
  auto const literal = main.frontier();
  main.dword(makeTarget32(oldTarget));
  patchAdrpLiteralLoadTarget(
    Instruction::Cast(veneer),
    Instruction::Cast(ldr),
    literal
  );

  ASSERT_TRUE(possiblySmashableJcc(branch));
  EXPECT_EQ(smashableJccTarget(branch), oldTarget);

  smashJcc(branch, newTarget);
  EXPECT_EQ(
    *reinterpret_cast<uint32_t*>(literal),
    makeTarget32(newTarget)
  );
  ASSERT_TRUE(possiblySmashableJcc(branch));

  EXPECT_TRUE(optimizeSmashedJcc(branch));
  EXPECT_FALSE(possiblySmashableJcc(branch));

  auto const optimized = Instruction::Cast(veneer);
  EXPECT_EQ(optimized->Mask(UnconditionalBranchMask), B);
  EXPECT_EQ(optimized->ImmPCOffsetTarget(), Instruction::Cast(newTarget));
  EXPECT_TRUE(optimized->GetNextInstruction()->IsNop());
  EXPECT_TRUE(
    optimized->GetNextInstruction()->GetNextInstruction()->IsNop()
  );
}

void checkBccAdrpLdrBr(MarkedSite marked) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x15000000);
  initBlocks(4096, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };
  auto const target = main.frontier();
  a.brk(0);

  vixl::Label skip;
  auto const jcc = main.frontier();
  meta.addressImmediates.insert(jcc);
  a.b(&skip, lt);
  auto const loadStart = main.frontier();
  meta.addressImmediates.insert(loadStart);
  a.adrp(x17, int64_t{0});
  a.ldr(w17, MemOperand(x17, 0));
  a.Br(x17);
  a.bind(&skip);
  a.brk(0);
  auto const literal = main.frontier();
  main.dword(makeTarget32(target));
  auto const end = main.frontier();
  markSite(meta, marked, loadStart);

  auto const logicalAdrp = Instruction::Cast(loadStart);
  auto const adrp = Instruction::Cast(main.toDestAddress(loadStart));
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, logicalAdrp, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  relocate(rel, main, start, end, main, meta, ai);

  auto const relocated =
    Instruction::Cast(main.toDestAddress(rel.adjustedAddressAfter(jcc)));
  ASSERT_TRUE(relocated->IsCondBranchImm());

  if (marked != MarkedSite::None) {
    // Not folded: the branch keeps its original condition and still skips the
    // tail rather than inheriting the far jump's target.
    EXPECT_EQ(static_cast<Condition>(relocated->ConditionBranch()), lt);
    return;
  }

  EXPECT_EQ(static_cast<Condition>(relocated->ConditionBranch()), ge);
  EXPECT_EQ(
    relocated->GetImmPCOffsetTarget(),
    Instruction::CastConst(main.toDestAddress(rel.adjustedAddressAfter(target)))
  );
}

TEST(Relocation, OptimizeBccAdrpLdrBrToBcc) {
  checkBccAdrpLdrBr(MarkedSite::None);
}

// As for the TST window: the (ADRP; LDR; BR) tail can be an independent inline
// smashable jmp or a veneer that the conditional branch merely skips over.
TEST(Relocation, PreserveSmashableJmpInFarBccWindow) {
  checkBccAdrpLdrBr(MarkedSite::FarJumpSmashable);
}

TEST(Relocation, PreserveVeneerInFarBccWindow) {
  checkBccAdrpLdrBr(MarkedSite::FarJumpVeneer);
}

TEST(Relocation, AdjustAdrpLdrLiteralAndShrinkAdrpToLdr) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const orig = main.frontier();
  MacroAssembler a { main };
  a.adrp(x0, int64_t{0});
  a.ldr(x0, MemOperand(x0, 0));

  auto const start = main.frontier();
  a.adrp(x1, int64_t{0});
  a.ldr(x1, MemOperand(x1, 0));
  a.Br(x1);
  align(main, &meta, Alignment::QuadWordSmashable, AlignContext::Live);
  auto const literal = main.frontier();
  a.dc64(0);
  auto const end = main.frontier();

  auto const adrp = Instruction::Cast(orig);
  auto const ldr = adrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(adrp, ldr, literal);
  auto const branchAdrp = Instruction::Cast(start);
  auto const branchLdr = branchAdrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(branchAdrp, branchLdr, literal);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);
  adjustForRelocation(rel, orig, start);

  auto const relocatedLdr = relocated;
  EXPECT_TRUE(relocatedLdr->IsLoadLiteral());
  EXPECT_EQ(relocatedLdr->Mask(LoadLiteralMask), LDR_x_lit);

  auto const finalLoad = LoadLiteral::at(adrp);
  EXPECT_TRUE(finalLoad && finalLoad.isFar());
  EXPECT_EQ(
    reinterpret_cast<TCA>(finalLoad.literalAddress()),
    rel.adjustedAddressAfter(literal)
  );
}

TEST(Relocation, ShrinkAdrpLdrSmashableMovqToLdr) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  initBlocks(4096, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };
  auto const imm = 0xfeedfacecafebeefULL;

  align(main, &meta, Alignment::SmashMovq, AlignContext::Live);
  auto const movq = main.frontier();
  a.adrp(x0, int64_t{0});
  meta.smashableLocations.insert(movq);
  a.ldr(x0, MemOperand(x0, 0));

  align(main, &meta, Alignment::QuadWordSmashable, AlignContext::Live);
  auto const literal = main.frontier();
  a.dc64(imm);
  auto const end = main.frontier();

  auto const origAdrp = Instruction::Cast(start);
  auto const origLdr = origAdrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(origAdrp, origLdr, literal);
  auto const origLoad = LoadLiteral::at(origAdrp);
  ASSERT_TRUE(origLoad && origLoad.isFar());
  ASSERT_EQ(
    reinterpret_cast<TCA>(origLoad.literalAddress()),
    literal
  );
  ASSERT_TRUE(possiblySmashableMovq(movq));
  ASSERT_EQ(smashableMovqImm(movq), imm);

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  auto const relocated = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);
  adjustMetaDataForRelocation(rel, nullptr, meta);

  auto const relocatedLdr = relocated;
  EXPECT_TRUE(relocatedLdr->IsLoadLiteral());
  EXPECT_EQ(relocatedLdr->Mask(LoadLiteralMask), LDR_x_lit);
  EXPECT_EQ(
    rel.adjustedAddressAfter(movq),
    reinterpret_cast<TCA>(relocated)
  );
  EXPECT_TRUE(possiblySmashableMovq(reinterpret_cast<TCA>(relocated)));
  EXPECT_EQ(smashableMovqImm(reinterpret_cast<TCA>(relocated)), imm);
  auto const patchedImm = 0x0123456789abcdefULL;
  smashMovq(reinterpret_cast<TCA>(relocated), patchedImm);
  EXPECT_EQ(smashableMovqImm(reinterpret_cast<TCA>(relocated)), patchedImm);
  EXPECT_TRUE(meta.smashableLocations.contains(reinterpret_cast<TCA>(relocated)));
  EXPECT_FALSE(meta.smashableLocations.contains(movq));
}

TEST(Relocation, RelocateTaggedSmashableMovqWithDistinctLogicalAddress) {
  if (!arch::any<arch::ARM>()) {
    SUCCEED();
    return;
  }

  CodeBlock main;
  DataBlock data;
  auto const logicalStart = reinterpret_cast<uint8_t*>(0x16000000);
  initBlocks(4096, main, data, logicalStart);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  auto const start = main.frontier();
  MacroAssembler a { main };

  align(main, &meta, Alignment::SmashMovq, AlignContext::Live);
  auto const movq = main.frontier();
  a.adrp(x0, int64_t{0});
  meta.smashableLocations.insert(movq);
  meta.addressImmediates.insert(
    reinterpret_cast<TCA>(~reinterpret_cast<uintptr_t>(movq))
  );
  a.ldr(x0, MemOperand(x0, 0));

  align(main, &meta, Alignment::QuadWordSmashable, AlignContext::Live);
  auto const literal = main.frontier();
  auto const tagged = (reinterpret_cast<uint64_t>(movq) << 1) | 1;
  a.dc64(tagged);
  auto const end = main.frontier();

  auto const logicalAdrp = Instruction::Cast(movq);
  auto const origAdrp = Instruction::Cast(main.toDestAddress(start));
  auto const origLdr = origAdrp->GetNextInstruction();
  patchAdrpLiteralLoadTarget(origAdrp, origLdr, logicalAdrp, literal);
  auto const origLoad = LoadLiteral::at(origAdrp);
  ASSERT_TRUE(origLoad && origLoad.isFar());
  ASSERT_EQ(
    reinterpret_cast<TCA>(origLoad.literalAddress(logicalAdrp)),
    literal
  );
  ASSERT_TRUE(possiblySmashableMovq(main.toDestAddress(movq)));

  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  relocate(rel, main, start, end, main, meta, ai);
  adjustMetaDataForRelocation(rel, nullptr, meta);

  auto const relocatedAddr = rel.adjustedAddressAfter(movq);
  ASSERT_NE(relocatedAddr, nullptr);
  auto const relocated = Instruction::Cast(main.toDestAddress(relocatedAddr));

  auto const relocatedLdr = relocated;
  EXPECT_TRUE(relocatedLdr->IsLoadLiteral());
  EXPECT_EQ(relocatedLdr->Mask(LoadLiteralMask), LDR_x_lit);
  EXPECT_EQ(relocatedAddr, rel.adjustedAddressAfter(movq));
  EXPECT_EQ(
    *reinterpret_cast<uint64_t*>(relocatedLdr->GetLiteralAddress<uint8_t*>()),
    (reinterpret_cast<uint64_t>(relocatedAddr) << 1) | 1
  );
  auto const relocatedActual = reinterpret_cast<TCA>(relocated);
  EXPECT_TRUE(possiblySmashableMovq(relocatedActual));
  EXPECT_EQ(
    smashableMovqImm(relocatedActual),
    (reinterpret_cast<uint64_t>(relocatedAddr) << 1) | 1
  );
  EXPECT_TRUE(meta.smashableLocations.contains(relocatedAddr));
  EXPECT_FALSE(meta.smashableLocations.contains(movq));
  EXPECT_TRUE(meta.addressImmediates.contains(
    reinterpret_cast<TCA>(~reinterpret_cast<uintptr_t>(relocatedAddr))
  ));
  EXPECT_FALSE(meta.addressImmediates.contains(
    reinterpret_cast<TCA>(~reinterpret_cast<uintptr_t>(movq))
  ));
}

/*
 * Tests the adjustment of a movz/movk target after that
 * movz/movk is relocated. This is a second adjustment of
 * the internal reference triggered because the translation
 * changed in size during relocation. See second portion of
 * relocateImpl().
 */
TEST(Relocation, RelocateInternalAdjustedMovzMovk) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(9 << 20, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit Mov of start + 2MB into x17 and then pad out 4MB+.
  auto start = main.frontier();

  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.Mov(x17, reinterpret_cast<uint64_t>(start + (2 << 20) + 16));
  main.setFrontier(start + (4 << 20) + 32);

  auto end = main.frontier();

  // 3. Call relocate()
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  const Instruction* instr = Instruction::Cast(end);
  relocate(rel, main, start, end, main, meta, ai);

  // 4. Expect movz/movk of new target
  auto const mov = decodeMovWideSequence(instr);
  EXPECT_EQ(mov.reg, 17);
  EXPECT_EQ(
    Instruction::Cast(mov.target),
    Instruction::Cast(end + (2 << 20) + 16)
  );
}

/*
 * Tests the adjustment of a movz/movk target when its target
 * is relocated. See arm::adjustInstruction().
 */
TEST(Relocation, AdjustMovzMovk) {
  // 1. Init
  CodeBlock main;
  DataBlock data;
  initBlocks(3 << 20, main, data);
  SCOPE_EXIT { freeBlocks(); };

  CGMeta meta;

  // 2. Emit Mov of start + 2MB+ into x17. This is not relocated, but
  //    its target at 2MB+ will be.
  auto orig = main.frontier();

  MacroAssembler a { main };
  meta.addressImmediates.insert(main.frontier());
  a.Mov(x17, reinterpret_cast<uint64_t>(orig + (2 << 20) + 16));
  main.setFrontier(orig + (2 << 20) + 16);

  auto start = main.frontier();
  a.nop();
  auto end = main.frontier();

  // 3. Call relocate() and then adjust the mov/movk
  RelocationInfo rel;
  AreaIndex ai = AreaIndex::Main;
  relocate(rel, main, start, end, main, meta, ai);
  adjustForRelocation(rel, orig, start);

  // 4. Expect movz/movk of new target
  auto const mov = decodeMovWideSequence(Instruction::Cast(orig));
  EXPECT_EQ(mov.reg, 17);
  EXPECT_EQ(Instruction::Cast(mov.target), Instruction::Cast(end));
}

}}

#endif
