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

#ifdef __x86_64__

#include "hphp/runtime/vm/jit/align-x64.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/relocation-x64.h"
#include "hphp/runtime/vm/jit/relocation.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

#include <folly/portability/GTest.h>

#include <sys/mman.h>

#include <cstring>
#include <vector>

namespace HPHP::jit::x64 {

namespace {

constexpr size_t kBlockSize = 4096;
constexpr size_t kMovqLen = 3;

/*
 * Page-aligned src and dest blocks, so offsets within them have known
 * cache-line alignment.
 */
struct Blocks {
  Blocks() {
    mem = static_cast<uint8_t*>(mmap(nullptr, 2 * kBlockSize,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    always_assert(mem != MAP_FAILED);
    src.init(mem, kBlockSize, "src");
    dest.init(mem + kBlockSize, kBlockSize, "dest");
  }
  ~Blocks() { munmap(mem, 2 * kBlockSize); }

  uint8_t* mem;
  CodeBlock src;
  CodeBlock dest;
};

/*
 * Emit non-nop, non-branch instructions up to `until', recording where each
 * one starts.
 */
void fill(CodeBlock& cb, TCA until, std::vector<TCA>& insts) {
  X64Assembler a{cb};
  while (cb.frontier() < until) {
    insts.push_back(cb.frontier());
    if (size_t(until - cb.frontier()) % kMovqLen == 0) {
      a.movq(reg::rax, reg::rdx);
    } else {
      a.push(reg::rax);
    }
  }
  always_assert(cb.frontier() == until);
}

size_t relocateAll(RelocationInfo& rel, Blocks& b, TCA start, TCA end,
                   CGMeta& meta) {
  return x64::relocate(rel, b.dest, start, end, b.src, meta, AreaIndex::Main);
}

TCA destOf(RelocationInfo& rel, TCA start, TCA destStart, TCA inst) {
  return inst == start ? destStart : rel.adjustedAddressAfter(inst);
}

void expectSameBytes(TCA a, TCA b, size_t len) {
  EXPECT_EQ(memcmp(a, b, len), 0);
}

}

TEST(RelocationX64, CopiesStraightLineCode) {
  Blocks b;
  std::vector<TCA> insts;
  auto const start = b.src.frontier();
  fill(b.src, start + 30, insts);
  auto const end = b.src.frontier();

  CGMeta meta;
  RelocationInfo rel;
  auto const destStart = b.dest.frontier();
  EXPECT_EQ(relocateAll(rel, b, start, end, meta), insts.size());

  for (auto const i : insts) {
    if (i == start) continue;
    EXPECT_EQ(rel.adjustedAddressAfter(i), destStart + (i - start));
    EXPECT_EQ(rel.adjustedAddressBefore(i), destStart + (i - start));
  }
  EXPECT_EQ(rel.adjustedAddressAfter(start), destStart);
  EXPECT_EQ(rel.adjustedAddressBefore(end), destStart + (end - start));
  EXPECT_EQ(b.dest.frontier(), destStart + (end - start));
  expectSameBytes(destStart, start, end - start);
}

/*
 * The src nops that aligned each marked instruction are dropped, and the
 * instruction is re-aligned in dest instead.
 */
TEST(RelocationX64, ReAlignsAtRecordedAlignments) {
  Blocks b;
  CGMeta meta;
  std::vector<TCA> insts;
  auto const start = b.src.frontier();

  fill(b.src, start + 10, insts);
  x64::align(b.src, &meta, Alignment::CacheLine, AlignContext::Live);
  auto const lineAligned = b.src.frontier();
  // More than a cache line, so the next alignment's nops aren't kept.
  fill(b.src, lineAligned + 70, insts);
  x64::align(b.src, &meta, Alignment::JmpTarget, AlignContext::Live);
  auto const jmpAligned = b.src.frontier();
  fill(b.src, jmpAligned + 20, insts);
  auto const end = b.src.frontier();
  ASSERT_EQ(meta.alignments.size(), 2u);

  b.dest.setFrontier(b.dest.frontier() + 5);
  auto const destStart = b.dest.frontier();
  RelocationInfo rel;
  relocateAll(rel, b, start, end, meta);

  TCA prevEnd = destStart;
  for (auto const i : insts) {
    auto const d = destOf(rel, start, destStart, i);
    if (i == lineAligned) {
      EXPECT_EQ(rel.adjustedAddressBefore(i), prevEnd);
      EXPECT_TRUE(x64::is_aligned(d, Alignment::CacheLine));
      EXPECT_GT(d, prevEnd);
    } else if (i == jmpAligned) {
      EXPECT_EQ(rel.adjustedAddressBefore(i), prevEnd);
      EXPECT_TRUE(x64::is_aligned(d, Alignment::JmpTarget));
      EXPECT_GT(d, prevEnd);
    } else {
      EXPECT_EQ(d, prevEnd) << (i - start);
    }
    auto const len = DecodedInstruction(i, i).size();
    expectSameBytes(d, i, len);
    prevEnd = d + len;
  }
  EXPECT_EQ(rel.adjustedAddressBefore(end), prevEnd);
  EXPECT_EQ(b.dest.frontier(), prevEnd);
}

TEST(RelocationX64, AppliesEveryAlignmentAtOneAddress) {
  Blocks b;
  std::vector<TCA> insts;
  auto const start = b.src.frontier();
  fill(b.src, start + 30, insts);
  auto const marked = b.src.frontier();
  fill(b.src, marked + 9, insts);
  auto const end = b.src.frontier();

  // JmpTarget alone would put `marked' at dest+32; CacheLine moves it on to
  // dest+64.
  CGMeta meta;
  meta.alignments.emplace(
    marked, std::make_pair(Alignment::JmpTarget, AlignContext::Live));
  meta.alignments.emplace(
    marked, std::make_pair(Alignment::CacheLine, AlignContext::Live));

  auto const destBase = b.dest.frontier();
  b.dest.setFrontier(destBase + 1);
  RelocationInfo rel;
  relocateAll(rel, b, start, end, meta);

  EXPECT_EQ(rel.adjustedAddressBefore(marked), destBase + 31);
  EXPECT_EQ(rel.adjustedAddressAfter(marked), destBase + 64);
  EXPECT_EQ(b.dest.frontier(), destBase + 64 + 9);
}

/*
 * Alignments before the range, at its end, or inside an instruction don't
 * apply to the instructions being relocated.
 */
TEST(RelocationX64, IgnoresAlignmentsOffInstructionStarts) {
  Blocks b;
  std::vector<TCA> before, insts;
  fill(b.src, b.src.frontier() + 12, before);
  auto const start = b.src.frontier();
  fill(b.src, start + 30, insts);
  auto const end = b.src.frontier();
  std::vector<TCA> after;
  fill(b.src, end + 6, after);

  CGMeta meta;
  auto const cacheLine =
    std::make_pair(Alignment::CacheLine, AlignContext::Live);
  meta.alignments.emplace(before[1], cacheLine);
  meta.alignments.emplace(insts[2] + 1, cacheLine);
  meta.alignments.emplace(insts[5] + 2, cacheLine);
  meta.alignments.emplace(end, cacheLine);

  b.dest.setFrontier(b.dest.frontier() + 1);
  auto const destStart = b.dest.frontier();
  RelocationInfo rel;
  relocateAll(rel, b, start, end, meta);

  for (auto const i : insts) {
    EXPECT_EQ(destOf(rel, start, destStart, i), destStart + (i - start));
  }
  EXPECT_EQ(b.dest.frontier(), destStart + (end - start));
}

TEST(RelocationX64, AlignmentAtRangeStart) {
  Blocks b;
  std::vector<TCA> insts;
  auto const start = b.src.frontier();
  fill(b.src, start + 12, insts);
  auto const end = b.src.frontier();

  CGMeta meta;
  meta.alignments.emplace(
    start, std::make_pair(Alignment::CacheLine, AlignContext::Live));

  auto const destBase = b.dest.frontier();
  b.dest.setFrontier(destBase + 7);
  RelocationInfo rel;
  relocateAll(rel, b, start, end, meta);

  EXPECT_EQ(rel.adjustedAddressAfter(start), destBase + 64);
  EXPECT_EQ(rel.adjustedAddressAfter(insts[1]), destBase + 64 + kMovqLen);
  EXPECT_EQ(b.dest.frontier(), destBase + 64 + 12);
}

/*
 * The first attempt shrinks the jmp to 2 bytes, then the padding in front of
 * the aligned instruction pushes its target out of rel8 range. relocate()
 * rewinds and retries with a 5 byte jmp, and every mapping must come from the
 * second attempt.
 *
 *   src+0   jmp src+125        (5 bytes)
 *   src+5   filler
 *   src+64  filler             (CacheLine aligned)
 *   src+125 movq               (jmp target)
 *   src+128 end
 */
TEST(RelocationX64, RetryAfterJmpOutOfRangeDiscardsFirstAttempt) {
  Blocks b;
  CGMeta meta;
  std::vector<TCA> insts;
  auto const start = b.src.frontier();
  auto const target = start + 125;
  X64Assembler a{b.src};
  a.jmp(target);
  ASSERT_EQ(b.src.frontier(), start + 5);
  fill(b.src, start + 64, insts);
  x64::align(b.src, &meta, Alignment::CacheLine, AlignContext::Live);
  auto const aligned = b.src.frontier();
  ASSERT_EQ(aligned, start + 64);
  fill(b.src, target + kMovqLen, insts);
  auto const end = b.src.frontier();
  ASSERT_EQ(end, start + 128);

  // With a 2 byte jmp `aligned' would start at dest+65 and need 63 bytes of
  // padding, leaving the target 183 bytes past the jmp.
  auto const destBase = b.dest.frontier();
  auto const destStart = destBase + 4;
  b.dest.setFrontier(destStart);
  RelocationInfo rel;
  relocateAll(rel, b, start, end, meta);

  DecodedInstruction jmp(destStart, destStart);
  ASSERT_TRUE(jmp.isBranch());
  EXPECT_EQ(jmp.size(), 5u);
  EXPECT_EQ(jmp.picAddress(), destBase + 128 + (target - aligned));

  for (auto const i : insts) {
    if (i < aligned) {
      EXPECT_EQ(rel.adjustedAddressAfter(i), destStart + (i - start))
        << (i - start);
    } else {
      EXPECT_EQ(rel.adjustedAddressAfter(i), destBase + 128 + (i - aligned))
        << (i - start);
    }
  }
  EXPECT_EQ(rel.adjustedAddressBefore(aligned), destBase + 68);
  EXPECT_EQ(rel.adjustedAddressAfter(target), jmp.picAddress());
  EXPECT_EQ(rel.srcRanges(), (RelocationInfo::RangeVec{{start, end}}));
  EXPECT_EQ(rel.dstRanges(),
            (RelocationInfo::RangeVec{{destStart, destBase + 192}}));
  EXPECT_EQ(b.dest.frontier(), destBase + 192);
}

}

#endif
