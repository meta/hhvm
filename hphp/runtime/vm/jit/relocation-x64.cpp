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

#include "hphp/runtime/vm/jit/align-x64.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"

#include "hphp/util/configs/jit.h"

namespace HPHP::jit::x64 {

namespace {

TRACE_SET_MOD(hhir)

//////////////////////////////////////////////////////////////////////

constexpr int kJmpLen = 5;

using WideJmpSet = hphp_hash_set<void*>;
struct JmpOutOfRange : std::exception {};

// For align = 2^n for some n
#define ALIGN(x, align) ((((align) - 1) | (x)) + 1)
#define ALIGN_OFFSET(x, align) ((x) & ((align) - 1))

size_t relocateImpl(RelocationInfo& rel,
                    CodeBlock& destBlock,
                    TCA start, TCA end,
                    DataBlock& srcBlock,
                    CGMeta& fixups,
                    WideJmpSet& wideJmps,
                    AreaIndex codeArea) {
  TCA src = start;
  size_t range = end - src;
  bool hasInternalRefs = false;
  bool internalRefsNeedUpdating = false;
  TCA destStart = destBlock.frontier();
  size_t asm_count{0};
  TCA keepNopLow = nullptr;
  TCA keepNopHigh = nullptr;
  // src only moves forward, so walk the alignments alongside it.
  auto alignIt = fixups.alignments.lower_bound(start);
  auto const alignEnd = fixups.alignments.end();
  try {
    while (src != end) {
      assertx(src < end);
      DecodedInstruction di(srcBlock.toDestAddress(src), src);
      asm_count++;
      int destRange = 0;

      auto willAlignTo64 = [&](TCA src, TCA dest) {
        bool align = false;
        auto af = fixups.alignments.equal_range(src);
        while (af.first != af.second) {
          auto const alignPair = af.first->second;
          auto const alignInfo = alignment_info(alignPair.first);
          if (alignPair.second == AlignContext::Live &&
              !is_aligned(dest, alignPair.first)) {
            align = align ||
                    0 == ALIGN_OFFSET(ALIGN((uint64_t)dest, alignInfo.align),
                                      x64::cache_line_size());
          }
          ++af.first;
        }
        return align;
      };
      // Make macro-fusion pairs not be split by the end of a cache line.
      // According to Intel 64 and IA-32 Architectures Optimization Refernce
      // Manual (page 2-18):
      // "Macro fusion does not happen if the first instruction ends on byte 63
      // of a cache line, and the second instruction is a conditional branch
      // that starts at byte 0 of the next cache line."
      auto nextSrc = src + di.size();
      auto const nextDest = destBlock.frontier() + di.size();
      if (Cfg::Jit::AlignMacroFusionPairs &&
          codeArea == AreaIndex::Main) {
        while (nextSrc != end) {
          DecodedInstruction next(srcBlock.toDestAddress(nextSrc), nextSrc);
          if (!next.isNop()) {
            if (di.isFuseable(next) &&
                (0 == ALIGN_OFFSET((uint64_t)nextDest,
                                   x64::cache_line_size()) ||
                  willAlignTo64(nextSrc, nextDest))) {
              // Offset to 1 past end of cache line.
              size_t offset = ALIGN_OFFSET((~(uint64_t)nextDest) + 2,
                                           x64::cache_line_size());
              X64Assembler a(destBlock);
              a.emitNop(offset);
              destRange += offset;
              internalRefsNeedUpdating = true;
            }
            break;
          }
          nextSrc += next.size();
        }
      }

      while (alignIt != alignEnd && alignIt->first < src) ++alignIt;
      for (; alignIt != alignEnd && alignIt->first == src; ++alignIt) {
        auto const alignPair = alignIt->second;
        auto const alignInfo = alignment_info(alignPair.first);

        auto const low = src + alignInfo.offset;
        auto const hi = src + alignInfo.nbytes;
        assertx(low < hi);

        if (!keepNopLow || keepNopLow > low) keepNopLow = low;
        if (!keepNopHigh || keepNopHigh < hi) keepNopHigh = hi;

        TCA tmp = destBlock.frontier();
        align(destBlock, nullptr, alignPair.first, alignPair.second);

        if (destBlock.frontier() != tmp) {
          destRange += destBlock.frontier() - tmp;
          internalRefsNeedUpdating = true;
        }
      }

      bool preserveAlignment = keepNopLow && keepNopHigh &&
        keepNopLow <= src && keepNopHigh > src;
      TCA dest = destBlock.frontier();
      destBlock.bytes(di.size(), srcBlock.toDestAddress(src));
      DecodedInstruction d2(destBlock.toDestAddress(dest), dest);
      if (di.hasPicOffset()) {
        /*
         * Rip-relative offsets that point outside the range
         * being moved need to be adjusted so they continue
         * to point at the right thing
         */
        if (size_t(di.picAddress() - start) >= range) {
          bool DEBUG_ONLY success = d2.setPicAddress(di.picAddress());
          assertx(success);
        } else {
          if (!preserveAlignment && d2.isBranch()) {
            if (wideJmps.contains(src)) {
              if (d2.size() < kJmpLen) {
                d2.widenBranch();
                internalRefsNeedUpdating = true;
                // widening a branch makes the dest instruction bigger
                destBlock.setFrontier(dest + d2.size());
              }
            } else if (d2.shrinkBranch()) {
              internalRefsNeedUpdating = true;
            }
          }
          hasInternalRefs = true;
        }
      }
      if (di.hasImmediate()) {
        if (fixups.addressImmediates.contains(src)) {
          if (size_t(di.immediate() - (uint64_t)start) < range) {
            hasInternalRefs = internalRefsNeedUpdating = true;
          }
        } else {
          if (fixups.addressImmediates.contains((TCA)~uintptr_t(src))) {
            // Handle weird, encoded offset, used by LdSmashable
            always_assert(di.immediate() == ((uintptr_t(src) << 1) | 1));
            bool DEBUG_ONLY success =
              d2.setImmediate(((uintptr_t)dest << 1) | 1);
            assertx(success);
          }
          /*
           * An immediate that points into the range being moved, but which
           * isn't tagged as an addressImmediate, is most likely a bug
           * and its instruction's address needs to be put into
           * fixups.addressImmediates. But it could just happen by bad
           * luck, so just log it.
           */
          if (size_t(di.immediate() - (uint64_t)start) < range) {
            FTRACE(3,
                   "relocate: instruction at {} has immediate 0x{:x}"
                   "which looks like an address that needs relocating\n",
                   src, di.immediate());
          }
        }
      }

      if (src == start) {
        // for the start of the range, we only want to overwrite the "after"
        // address (since the "before" address could belong to the previous
        // tracelet, which could be being relocated to a completely different
        // address. recordRange will do that for us, so just make sure we
        // have the right address setup.
        destStart = dest;
      } else {
        rel.recordAddress(src, dest - destRange, destRange);
      }
      if (preserveAlignment && di.size() == kJmpLen &&
          di.isNop() && src + kJmpLen == end) {
        smashJmp(dest, src + kJmpLen);
        dest += kJmpLen;
      } else if (di.isNop() && !preserveAlignment) {
        internalRefsNeedUpdating = true;
      } else {
        dest += d2.size();
      }
      assertx(dest <= destBlock.frontier());
      destBlock.setFrontier(dest);
      src += di.size();
      if (keepNopHigh && src >= keepNopHigh) {
        keepNopLow = keepNopHigh = nullptr;
      }
    } // while (src != end)

    rel.recordRange(start, end, destStart, destBlock.frontier());

    if (hasInternalRefs && internalRefsNeedUpdating) {
      src = start;
      bool ok = true;
      while (src != end) {
        DecodedInstruction di(srcBlock.toDestAddress(src), src);
        TCA newPicAddress = nullptr;
        int64_t newImmediate = 0;
        if (di.hasPicOffset() &&
            size_t(di.picAddress() - start) < range) {
          newPicAddress = rel.adjustedAddressAfter(di.picAddress());
          always_assert(newPicAddress);
        }
        if (di.hasImmediate() &&
            size_t((TCA)di.immediate() - start) < range &&
            fixups.addressImmediates.count(src)) {
          newImmediate =
            (int64_t)rel.adjustedAddressAfter((TCA)di.immediate());
          always_assert(newImmediate);
        }
        if (newImmediate || newPicAddress) {
          TCA dest = rel.adjustedAddressAfter(src);
          DecodedInstruction d2(destBlock.toDestAddress(dest), dest);
          if (newPicAddress) {
            if (!d2.setPicAddress(newPicAddress)) {
              always_assert(d2.isBranch() && d2.size() == 2);
              wideJmps.insert(src);
              ok = false;
            }
          }
          if (newImmediate) {
            if (!d2.setImmediate(newImmediate)) {
              always_assert(false);
            }
          }
        }
        src += di.size();
      }
      if (!ok) {
        throw JmpOutOfRange();
      }
    }
    rel.markAddressImmediates(fixups.addressImmediates);
  } catch (...) {
    rel.rewind(start, end);
    destBlock.setFrontier(destStart);
    throw;
  }
  return asm_count;
}

//////////////////////////////////////////////////////////////////////

}

/*
 * This should be called after calling relocate on all relevant ranges. It
 * will adjust all references into the original src ranges to point into the
 * corresponding relocated ranges.
 */
void adjustForRelocation(RelocationInfo& rel) {
  for (const auto& range : rel.srcRanges()) {
    adjustForRelocation(rel, range.first, range.second);
  }
}

/*
 * This will update a single range that was not relocated, but that
 * might refer to relocated code (such as the cold code corresponding
 * to a tracelet). Unless its guaranteed to be all position independent,
 * its "fixups" should have been passed into a relocate call earlier.
 */
void adjustForRelocation(RelocationInfo& rel, TCA srcStart, TCA srcEnd) {
  auto start = rel.adjustedAddressAfter(srcStart);
  auto end = rel.adjustedAddressBefore(srcEnd);
  if (!start) {
    start = srcStart;
    end = srcEnd;
  } else {
    always_assert(end);
  }
  while (start != end) {
    assertx(start < end);
    DecodedInstruction di(start);

    if (di.hasPicOffset()) {
      /*
       * A pointer into something that has been relocated needs to be
       * updated.
       */
      if (TCA adjusted = rel.adjustedAddressAfter(di.picAddress())) {
        di.setPicAddress(adjusted);
      }
    }

    if (di.hasImmediate()) {
      /*
       * Similarly for addressImmediates - and see comment above
       * for non-address immediates.
       */
      if (TCA adjusted = rel.adjustedAddressAfter((TCA)di.immediate())) {
        if (rel.isAddressImmediate(start)) {
          di.setImmediate((int64_t)adjusted);
        } else {
          FTRACE(3,
                 "relocate: instruction at {} has immediate 0x{:x}"
                 "which looks like an address that needs relocating\n",
                 start, di.immediate());
        }
      }
    }

    start += di.size();

    if (start == end && di.isNop() &&
        di.size() == kJmpLen &&
        rel.adjustedAddressAfter(srcEnd)) {

      smashJmp(start - di.size(), rel.adjustedAddressAfter(end));
    }
  }
}

/*
 * Adjust potentially live references that point into the relocated
 * area.
 * Must not be called until its safe to run the relocated code.
 */
void adjustCodeForRelocation(RelocationInfo& rel, CGMeta& fixups) {
  for (auto codePtr : fixups.codePointers) {
    if (TCA adjusted = rel.adjustedAddressAfter(*codePtr)) {
      *codePtr = adjusted;
    }
  }
}

/*
 * Relocate code in the range start, end into dest, and record
 * information about what was done to rel.
 * On exit, internal references (references into the source range)
 * will have been adjusted (ie they are still references into the
 * relocated code). External code references continue to point to
 * the same address as before relocation.
 */
size_t relocate(RelocationInfo& rel,
                CodeBlock& destBlock,
                TCA start, TCA end,
                DataBlock& srcBlock,
                CGMeta& fixups,
                AreaIndex codeArea) {
  WideJmpSet wideJmps;
  while (true) {
    try {
      return relocateImpl(rel, destBlock, start, end, srcBlock,
                          fixups, wideJmps, codeArea);
    } catch (JmpOutOfRange&) {
    }
  }
}

//////////////////////////////////////////////////////////////////////

}
