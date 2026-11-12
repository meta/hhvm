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

#include "hphp/runtime/vm/preclass.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/unit.h"

#include <ostream>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

PreClass::PreClass(Unit* unit, int line1, int line2, const StringData* n,
                   Attr attrs, const StringData* parent,
                   const StringData* docComment)
  : m_unit(unit)
  , m_namedType(NamedType::getOrCreate(n))
  , m_line1(line1)
  , m_line2(line2)
  , m_attrs(attrs)
  , m_name(n)
  , m_parent(parent)
  , m_docComment(docComment)
  , m_typeParamNames()
{
}

PreClass::~PreClass() {
  std::for_each(methods(), methods() + numMethods(), [](Func* f) { f->finishDestroy(); });
}

void PreClass::atomicRelease() {
  delete this;
}

const StringData* PreClass::manglePropName(const StringData* className,
                                           const StringData* propName,
                                           Attr attrs) {
  switch (attrs & VisibilityAttrs) {
    case AttrPublic: {
      return propName;
    }
    case AttrProtected: {
      std::string mangledName;
      mangledName.push_back('\0');
      mangledName.push_back('*');
      mangledName.push_back('\0');
      mangledName += propName->data();
      return makeStaticString(mangledName);
    }
    case AttrPrivate: {
      std::string mangledName;
      mangledName.push_back('\0');
      mangledName += className->data();
      mangledName.push_back('\0');
      mangledName += propName->data();
      return makeStaticString(mangledName);
    }
    default:
      //Failing here will cause the VM to crash before the Verifier runs, so we
      //defer the failure to runtime so the Verifier can report this problem to
      //the user.
      return staticEmptyString();
  }
}

void PreClass::prettyPrint(std::ostream &out) const {
  if (m_attrs & AttrSealed) { out << "<<__Sealed()>> "; }
  out << "Class ";
  if (m_attrs & AttrAbstract) { out << "abstract "; }
  if (m_attrs & AttrFinal) { out << "final "; }
  if (m_attrs & AttrInterface) { out << "interface "; }
  out << m_name->data();
  if (m_attrs & AttrNoOverrideRegular) {
    if (m_attrs & AttrNoOverride) {
      out << " (no-override)";
    } else {
      out << " (no-override-regular)";
    }
  }
  if (m_attrs & AttrPersistent) out << " (persistent)";
  if (m_attrs & AttrIsConst) {
    // AttrIsConst classes will always also have AttrForbidDynamicProps set,
    // so don't bother printing it
    out << " (const)";
  } else if (m_attrs & AttrForbidDynamicProps) {
    out << " (no-dynamic-props)";
  }
  if (m_attrs & AttrDynamicallyConstructible) out << " (dyn_constructible)";
  if (m_attrs & AttrDynamicallyReferenced) out << " (dyn_referenced)";
  if (m_attrs & AttrNoMock) out << " (no-mock)";
  out << std::endl;

  for (Func* const* it = methods(); it != methods() + numMethods(); ++it) {
    out << " ";
    (*it)->prettyPrint(out);
  }
  for (const Prop* it = properties();
      it != properties() + numProperties();
      ++it) {
    out << " ";
    it->prettyPrint(out, this);
  }
  for (const Const* it = constants();
      it != constants() + numConstants();
      ++it) {
    out << " ";
    it->prettyPrint(out, this);
  }
}

const StaticString s___Sealed("__Sealed");
void PreClass::enforceInMaybeSealedParentWhitelist(
  const PreClass* parentPreClass) const {
  // if our parent isn't sealed, then we're fine. If we're a mock, YOLO
  if (!(parentPreClass->attrs() & AttrSealed) ||
      m_userAttributes.contains(s___MockClass.get())) {
    return;
  }
  const UserAttributeMap& parent_attrs = parentPreClass->userAttributes();
  assertx(parent_attrs.contains(s___Sealed.get()));
  const auto& parent_sealed_attr = parent_attrs.find(s___Sealed.get())->second;
  bool in_sealed_whitelist = false;
  IterateV(parent_sealed_attr.m_data.parr,
           [&in_sealed_whitelist, this](TypedValue v) -> bool {
             if (v.m_data.pstr->same(name())) {
               in_sealed_whitelist = true;
               return true;
             }
             return false;
           });
  if (!in_sealed_whitelist) {
    raise_error("Class %s may not inherit from sealed %s (%s) without "
                "being in the whitelist",
                name()->data(),
                parentPreClass->attrs() & AttrInterface ? "interface" : "class",
                parentPreClass->name()->data());
  }
}

const StaticString s___EnableMethodTraitDiamond("__EnableMethodTraitDiamond");
bool PreClass::enableMethodTraitDiamond() {
  return m_userAttributes.contains(s___EnableMethodTraitDiamond.get());
}

///////////////////////////////////////////////////////////////////////////////
// PreClass::Prop.

PreClass::Prop::Prop(PreClass* preClass,
                     const StringData* name,
                     Attr attrs,
                     const StringData* userType,
                     TypeIntersectionConstraint&& constraints,
                     const StringData* docComment,
                     const TypedValue& val,
                     RepoAuthType repoAuthType,
                     UserAttributeMap userAttributes)
  : m_name(name)
  , m_attrs(attrs)
  , m_userType{userType}
  , m_docComment(docComment)
  , m_val(val)
  , m_repoAuthType{repoAuthType}
  , m_typeConstraints{std::move(constraints)}
  , m_userAttributes(userAttributes) {}

void PreClass::Prop::prettyPrint(std::ostream& out,
                                 const PreClass* preClass) const {
  out << "Property ";
  if (m_attrs & AttrStatic) { out << "static "; }
  if (m_attrs & AttrPublic) { out << "public "; }
  if (m_attrs & AttrProtected) { out << "protected "; }
  if (m_attrs & AttrPrivate) { out << "private "; }
  if (m_attrs & AttrInternal) { out << "internal "; }
  if (m_attrs & AttrPersistent) { out << "(persistent) "; }
  if (m_attrs & AttrIsConst) { out << "(const) "; }
  if (m_attrs & AttrTrait) { out << "(trait) "; }
  if (m_attrs & AttrNoBadRedeclare) { out << "(no-bad-redeclare) "; }
  if (m_attrs & AttrNoOverride) { out << "(no-override) "; }
  if (m_attrs & AttrSystemInitialValue) { out << "(system-initial-val) "; }
  if (m_attrs & AttrNoImplicitNullable) { out << "(no-implicit-nullable) "; }
  if (m_attrs & AttrInitialSatisfiesTC) { out << "(initial-satisfies-tc) "; }
  if (m_attrs & AttrLSB) { out << "(lsb) "; }
  if (m_attrs & AttrLateInit) { out << "(late-init) "; }
  out << preClass->name()->data() << "::" << m_name->data() << " = ";
  if (m_val.m_type == KindOfUninit) {
    out << "<non-scalar>";
  } else {
    std::string ss;
    staticStreamer(&m_val, ss);
    out << ss;
  }
  out << " (RAT = " << show(m_repoAuthType) << ")";
  if (m_userType && !m_userType->empty()) {
    out << " (user-type = " << m_userType->data() << ")";
  }
  for (auto const& tc : m_typeConstraints.range()) {
    if (tc.hasConstraint()) {
      out << " (tc = " << tc.displayName(nullptr, true) << ")";
    }
  }
  out << std::endl;
}

///////////////////////////////////////////////////////////////////////////////
// PreClass::Const.

PreClass::Const::Const(const StringData* name,
                       const StringData* cls,
                       const TypedValueAux& val,
                       Array resolvedTypeStructure,
                       Invariance invariance,
                       bool fromTrait)
  : m_name(name)
  , m_cls(cls)
  , m_val(val)
  , m_resolvedTypeStructure(std::move(resolvedTypeStructure))
  , m_invariance(invariance)
  , m_fromTrait(fromTrait)
{}

void PreClass::Const::prettyPrint(std::ostream& out,
                                  const PreClass* preClass) const {
  switch (kind()) {
    case ConstModifierFlags::Kind::Value:
      break;
    case ConstModifierFlags::Kind::Type:
      out << "Type ";
      break;
    case ConstModifierFlags::Kind::Context:
      out << "Context ";
      break;
  }
  auto const name = [&] {
    out << preClass->name()->data() << "::" << m_name->data();
    if (cls()) out << " (" << cls()->data() << ")";
  };
  if (isAbstractAndUninit()) {
    out << "Constant (abstract) ";
    name();
    out << std::endl;
    return;
  }
  if (kind() == ConstModifierFlags::Kind::Context) {
    out << "Constant ";
    name();
    out << " " << coeffects().toString()
        << std::endl;
    return;
  }
  out << "Constant ";
  name();
  if (m_val.m_type == KindOfUninit) {
    out << " = <non-scalar>";
  } else {
    std::string ss;
    staticStreamer(&m_val, ss);
    out << " = " << ss;
  }
  if (!m_resolvedTypeStructure.isNull()) {
    std::string ss;
    auto const tv =
      make_array_like_tv(m_resolvedTypeStructure.get());
    staticStreamer(&tv, ss);
    out << " (resolved = " << ss << ")";
    switch (invariance()) {
      case Invariance::None:
        break;
      case Invariance::Present:
        out << " <present>";
        break;
      case Invariance::ClassnamePresent:
        out << " <classname>";
        break;
      case Invariance::Same:
        out << " <same>";
        break;
    }
  }
  out << std::endl;
}

StaticCoeffects PreClass::Const::coeffects() const {
  assertx(kind() == ConstModifierFlags::Kind::Context);
  return m_val.constModifiers().getCoeffects(m_val.constModifierFlags());
}

///////////////////////////////////////////////////////////////////////////////
}
