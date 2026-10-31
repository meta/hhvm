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

#include "hphp/runtime/vm/jit/meth-profile.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"

#include "hphp/util/assertions.h"

#include <fmt/format.h>

#include <string>

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

void MethProfile::reportMeth(const Class* cls, const Func* meth,
                             const Func* callerFunc) {
  auto const checkModule = [&](const Func* f) {
    if (!Module::warningsEnabled(f)) return false;
    return (callerFunc &&
            will_symbol_raise_module_boundary_violation(f, callerFunc));
   };

  auto val = methValue();
  if (!val && !checkModule(meth)) {
    assertx(cls);
    m_curClass = cls;
    setMeth(meth, Tag::UniqueClass);
    return;
  }

  auto curMeth = rawMeth();
  switch (toTag(val)) {
    case Tag::Invalid:
      return;

    case Tag::UniqueClass:
      if (rawClass() == cls) {
        assertx(curMeth == meth);
        return;
      }
      if (checkModule(meth)) return;
      setMeth(curMeth, Tag::UniqueMeth);
      [[fallthrough]];
    case Tag::UniqueMeth:
      if (curMeth == meth) return;
      if (checkModule(meth)) return;
      curMeth = curMeth->baseCls()->getMethod(curMeth->methodSlot());
      setMeth(curMeth, Tag::BaseMeth);
      [[fallthrough]];
    case Tag::BaseMeth: {
      assertx(curMeth->baseCls() == curMeth->cls());
      if (curMeth->baseCls() == meth->baseCls()) return;
      for (auto iface : curMeth->baseCls()->allInterfaces().range()) {
        if (auto imeth = iface->lookupMethod(curMeth->name())) {
          if (meth->cls()->classof(iface)) {
            if (checkModule(imeth)) return;
            setMeth(imeth, Tag::InterfaceMeth);
            return;
          }
        }
      }
      break;
    }
    case Tag::InterfaceMeth:
      assertx(curMeth->cls()->attrs() & AttrInterface);
      if (meth->cls()->classof(curMeth->cls())) return;
      break;
  }

  setMeth(nullptr, Tag::Invalid);
}

void MethProfile::reduce(MethProfile& a, const MethProfile& b) {
  if (a.curTag() == Tag::Invalid) return;

  uintptr_t bMethVal;

  // this part is racy, and could slice if we're not careful
  while (true) {
    auto cls = b.m_curClass.get();
    bMethVal = b.methValue();
    if (!bMethVal) return;
    if (toTag(bMethVal) != Tag::UniqueClass) break;
    if (UNLIKELY(cls != b.m_curClass.get())) {
      continue;
    }
    assertx(cls);
    a.reportMeth(cls, fromValue(bMethVal), nullptr);
    return;
  }

  // Now we know the entire representation is in bMethVal,
  // so no more danger of a race
  if (toTag(bMethVal) == Tag::Invalid) {
    a.setMeth(nullptr, Tag::Invalid);
    return;
  }

  assertx(toTag(bMethVal) == Tag::UniqueMeth ||
          toTag(bMethVal) == Tag::BaseMeth ||
          toTag(bMethVal) == Tag::InterfaceMeth);

  auto const meth = fromValue(bMethVal);
  if (!a.methValue()) {
    a.setMeth(meth, toTag(bMethVal));
    return;
  }

  a.reportMeth(nullptr, meth, nullptr);
  if (a.curTag() == Tag::UniqueMeth && toTag(bMethVal) == Tag::BaseMeth) {
    a.setMeth(meth, Tag::BaseMeth);
  }
}

std::string MethProfile::toString() const {
  if (auto cls = uniqueClass()) {
    return fmt::format("uniqueClass {}", cls->name()->data());
  } else if (auto meth = uniqueMeth()) {
    return fmt::format("uniqueMeth {}", meth->fullName()->data());
  } else if (auto meth = baseMeth()) {
    return fmt::format("baseMeth {}", meth->fullName()->data());
  } else if (auto meth = interfaceMeth()) {
    return fmt::format("interfaceMeth {}", meth->fullName()->data());
  }
  return std::string("none");
}

folly::dynamic MethProfile::toDynamic() const {
  using folly::dynamic;

  dynamic baseObj = dynamic::object("profileType", "MethProfile");
  if (auto cls = uniqueClass()) {
    return dynamic::merge(baseObj,
                          dynamic::object("type", "uniqueClass")
                                         ("name", cls->name()->slice()));
  } else if (auto meth = uniqueMeth()) {
    return dynamic::merge(baseObj,
                          dynamic::object("type", "uniqueMeth")
                                         ("name", meth->fullName()->slice()));
  } else if (auto meth = baseMeth()) {
    return dynamic::merge(baseObj,
                          dynamic::object("type", "baseMeth")
                                         ("name", meth->fullName()->slice()));
  } else if (auto meth = interfaceMeth()) {
    return dynamic::merge(baseObj,
                          dynamic::object("type", "interfaceMeth")
                                         ("name", meth->fullName()->slice()));
  }
  return dynamic();
}

void MethProfile::serialize(ProfDataSerializer& ser) const {
  write_raw(ser, curTag());
  write_func(ser, rawMeth());
  write_class(ser, rawClass());
}

void MethProfile::deserialize(ProfDataDeserializer& ser) {
  auto const tag = read_raw<Tag>(ser);
  auto const func = read_func(ser);
  setMeth(func, tag);
  m_curClass = read_class(ser);
}

///////////////////////////////////////////////////////////////////////////////

}
