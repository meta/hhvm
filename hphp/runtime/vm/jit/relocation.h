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
#pragma once

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/util/data-block.h"

#include <map>
#include <set>
#include <vector>

namespace HPHP::jit {

//////////////////////////////////////////////////////////////////////

/*
 * Support for relocation involves implementing the following:
 *
 *   adjustForRelocation - Adjusts a range of instructions which may
 *                         contain addresses which have been moved.
 *   adjustCodeForRelocation - Similar to adjustForRelocation, but is
 *                             reserved for ranges which may still
 *                             hold live instructions.
 *   adjustMetaDataForRelocation - Adjusts the affected metadata
 *                                 following a relocation.
 *   relocate - Relocates a range of instructions to a new
 *              destination. Critically, relocation is a chance
 *              to grow/shrink the region which is advantageous
 *              when moving a far branch to simple near relative
 *              branch for instance.
 *
 * adjustForRelocation and adjustCodeForRelocation both search for
 * addresses in instruction ranges that need to be updated following
 * a relocation. This involves finding instructions which might
 * be loading an address and then determining if that target is
 * actually an address for an instruction which has been moved.
 */

//////////////////////////////////////////////////////////////////////

struct AsmInfo;
struct CGMeta;

struct RelocationInfo {
  RelocationInfo() {}

  void recordRange(TCA start, TCA end,
                   TCA destStart, TCA destEnd);
  void recordAddress(TCA src, TCA dest, int range);
  TcaRange fixupRange(const TcaRange& rng);
  void fixupRanges(AsmInfo* asmInfo, AreaIndex area);
  TCA adjustedAddressAfter(TCA addr) const;
  TCA adjustedAddressBefore(TCA addr) const;
  CTCA adjustedAddressAfter(CTCA addr) const {
    return adjustedAddressAfter(const_cast<TCA>(addr));
  }
  CTCA adjustedAddressBefore(CTCA addr) const {
    return adjustedAddressBefore(const_cast<TCA>(addr));
  }
  void rewind(TCA start, TCA end);
  void markAddressImmediates(const std::set<TCA>& ai) {
    addressImmediates.insert(ai.begin(), ai.end());
    // We should add the relocated address immediates as well.  During
    // adjustForRelocation, relocated ranges may need to have their address
    // immediates updated to point to other relocated ranges.  This requires
    // adjustForRelocation to know which immediates are address immediates in
    // the newly relocated range.
    decltype(addressImmediates) updatedAI;
    for (auto addrImm : ai) {
      if (m_addressImmediatesInvalidToMap.contains(addrImm)) continue;
      if (TCA adjusted = adjustedAddressAfter(addrImm)) {
        updatedAI.insert(adjusted);
      } else if (TCA odd = adjustedAddressAfter((TCA)~uintptr_t(addrImm))) {
        // just for LdSmashable
        updatedAI.insert((TCA)~uintptr_t(odd));
      }
    }
    addressImmediates.insert(updatedAI.begin(), updatedAI.end());
  }
  bool isAddressImmediate(TCA ip) {
    return addressImmediates.count(ip);
  }
  /*
   * Keep the marker for the original instruction, but do not transfer it to
   * the instruction's mapped address in markAddressImmediates().
   */
  void markAddressImmediateInvalidToMap(TCA ip) {
    m_addressImmediatesInvalidToMap.insert(ip);
  }
  bool isAddressImmediateInvalidToMap(TCA ip) const {
    return m_addressImmediatesInvalidToMap.contains(ip);
  }
  void markSmashableRelocation(TCA ip) {
    m_smashableRelocations.insert(ip);
  }
  bool isSmashableRelocation(TCA ip) {
    return m_smashableRelocations.count(ip);
  }
  using RangeVec = std::vector<std::pair<TCA,TCA>>;
  const RangeVec& srcRanges() { return m_srcRanges; }
  const RangeVec& dstRanges() { return m_dstRanges; }
 private:
  RangeVec m_srcRanges;
  RangeVec m_dstRanges;
  /*
   * maps from src address, to range of destination address
   * This is because we could insert nops before the instruction
   * corresponding to src. Most things want the address of the
   * instruction corresponding to the src instruction; but eg
   * the fixup map would want the address of the nop.
   */
  jit::fast_map<TCA,std::pair<TCA,TCA>> m_adjustedAddresses;
  std::set<TCA> addressImmediates;
  std::set<TCA> m_addressImmediatesInvalidToMap;
  std::set<TCA> m_smashableRelocations;
};

void adjustForRelocation(RelocationInfo&);
void adjustForRelocation(RelocationInfo& rel, TCA srcStart, TCA srcEnd);
void adjustCodeForRelocation(RelocationInfo& rel, CGMeta& meta);
void adjustMetaDataForRelocation(RelocationInfo& rel,
                                 AsmInfo* asmInfo,
                                 CGMeta& meta);
/*
 * ARM relocation expects Vasm-emitted literal loads to use the far ADRP/LDR
 * form before relocation. Relocation may shorten that form to a near LDR
 * literal when the final literal address is in range, but it does not expand
 * near LDR literals that become out of range.
 */
size_t relocate(RelocationInfo& rel,
                CodeBlock& destBlock,
                TCA start, TCA end,
                DataBlock& srcBlock,
                CGMeta& meta,
                AreaIndex codeArea);

}
