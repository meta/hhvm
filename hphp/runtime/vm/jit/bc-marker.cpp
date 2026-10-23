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

#include "hphp/runtime/vm/jit/bc-marker.h"

#include <fmt/format.h>
#include <folly/Conv.h>

#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"

namespace HPHP::jit {

//////////////////////////////////////////////////////////////////////

std::string BCMarker::show() const {
  if (!valid()) return "invalid BCMarker";

  return fmt::format(
    "--- bc {}, fp {}, fixupFP {}, spOff {}, {}",
    showShort(m_sk),
    m_fp ? folly::to<std::string>(m_fp->id()) : "_",
    m_fixupFP ? folly::to<std::string>(m_fixupFP->id()) : "_",
    m_bcSPOff.offset,
    m_profTransIDs.empty()
      ? ""
      : fmt::format(" [profTrans={}]", folly::join(',', m_profTransIDs))
  );
}

bool BCMarker::valid() const {
  // Note, we can't check stack bounds here because of inlining, and
  // instructions like idx, which can create php-level calls.
  assertx(m_profTransIDs.find(kInvalidTransID) == m_profTransIDs.end());
  return m_sk.valid();
}

SBInvOffset BCMarker::fixupSBOff() const {
  assertx(valid());
  if (fp() == fixupFP()) return SBInvOffset{0};
  auto const begin_inlining = fp()->inst();
  assertx(begin_inlining->is(DefCalleeFP));
  auto const fpSBOffset = begin_inlining->extra<DefCalleeFP>()->sbOffset;

  auto const fixupFPStackBaseOffset = [&] {
    if (fixupFP()->inst()->is(DefCalleeFP)) {
      return fixupFP()->inst()->extra<DefCalleeFP>()->sbOffset;
    }
    assertx(fixupFP()->inst()->is(DefFP, EnterFrame));
    auto const defSP = fp()->inst()->src(0)->inst();
    auto const irSPOff = defSP->extra<DefStackData>()->irSPOff;
    return SBInvOffset{0}.to<IRSPRelOffset>(irSPOff);
  }();
  return SBInvOffset{fixupFPStackBaseOffset - fpSBOffset};
}

SrcKey BCMarker::fixupSk() const {
  assertx(valid());
  if (fp() == fixupFP()) return sk();

  auto curFP = fp();
  do {
    assertx(curFP->inst()->is(DefCalleeFP));
    // Walking the FP chain created from the marker is suspect, but we aren't
    // using it to materialize the fp SSATmp, or offsets based on the begin
    // inlinings.  We are only materializing srckey info.
    auto const nextFP = curFP->inst()->marker().fp();
    if (nextFP == fixupFP()) return curFP->inst()->marker().sk();
    curFP = nextFP;
  } while (true);
}

//////////////////////////////////////////////////////////////////////

}
