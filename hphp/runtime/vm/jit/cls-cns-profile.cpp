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

#include "hphp/runtime/vm/jit/cls-cns-profile.h"

#include "hphp/runtime/base/type-variant.h"

#include "hphp/runtime/vm/class.h"

#include "hphp/util/assertions.h"

#include <fmt/format.h>

#include <string>

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

namespace {

Slot updateSlot(Slot curSlot, Slot newSlot) {
  if (newSlot == kInvalidSlot || curSlot == kInvalidSlot) return kInvalidSlot;
  if (!curSlot) return newSlot + 1;
  if (curSlot != newSlot + 1) return kInvalidSlot;
  return curSlot;
}

}

TypedValue ClsCnsProfile::reportClsCns(const Class* cls,
                                       const StringData* cnsName) {
  Slot slot;
  auto const tv = cls->cnsNameToTV(cnsName, slot);
  if (slot == kInvalidSlot) return make_tv<KindOfUninit>();
  m_curSlot = updateSlot(m_curSlot, slot);
  if (!tv) return make_tv<KindOfUninit>();
  return tv->m_type != KindOfUninit ? *tv : cls->clsCnsGet(cnsName);
}

void ClsCnsProfile::reduce(ClsCnsProfile& a, const ClsCnsProfile& b) {
  if (a.m_curSlot == kInvalidSlot || !b.m_curSlot) return;
  if (b.m_curSlot == kInvalidSlot) {
    a.m_curSlot = kInvalidSlot;
    return;
  }
  if (!a.m_curSlot) {
    a.m_curSlot = b.m_curSlot;
    return;
  }
  if (a.m_curSlot != b.m_curSlot) {
    a.m_curSlot = kInvalidSlot;
    return;
  }
}

std::string ClsCnsProfile::toString() const {
  if (!m_curSlot) return "empty";
  if (m_curSlot == kInvalidSlot) return "InvalidSlot";
  return fmt::format("Slot {}", getSlot());
}

folly::dynamic ClsCnsProfile::toDynamic() const {
  if (!m_curSlot) return folly::dynamic();
  return folly::dynamic::object("slot", m_curSlot == kInvalidSlot ?
                                        folly::dynamic() :
                                        getSlot())
                               ("profileType", "ClsCnsProfile");
}

///////////////////////////////////////////////////////////////////////////////

}
