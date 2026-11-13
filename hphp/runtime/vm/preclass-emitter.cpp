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
#include "hphp/runtime/vm/preclass-emitter.h"

#include <limits>

#include <folly/Memory.h>

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/native-data.h"

#include "hphp/util/blob-encoder.h"

namespace HPHP {

const StaticString s_SoftInternal("__SoftInternal");

//=============================================================================
// PreClassEmitter::Prop.

PreClassEmitter::Prop::Prop(
  const PreClassEmitter* pce,
  const StringData* n,
  Attr attrs,
  const StringData* userType,
  TypeIntersectionConstraint&& typeConstraints,
  const StringData* docComment,
  const TypedValue* val,
  RepoAuthType repoAuthType,
  UserAttributeMap userAttributes
)
  : m_name(n)
  , m_attrs(attrs)
  , m_userType(userType)
  , m_docComment(docComment)
  , m_repoAuthType(repoAuthType)
  , m_typeConstraints(std::move(typeConstraints))
  , m_userAttributes(userAttributes)
{
  memcpy(&m_val, val, sizeof(TypedValue));
  if (m_attrs & AttrInternal &&
      m_userAttributes.contains(s_SoftInternal.get())) {
    m_attrs |= AttrInternalSoft;
  }
}

PreClassEmitter::Prop::~Prop() {
}

//=============================================================================
// PreClassEmitter.

PreClassEmitter::PreClassEmitter(const StringData* n)
  : m_name(n), m_typeParamNames() {}

void PreClassEmitter::init(int line1, int line2, Attr attrs,
                           const StringData* parent,
                           const StringData* docComment,
                           bool isSystemLib) {
  m_line1 = line1;
  m_line2 = line2;
  m_attrs = attrs;
  m_parent = parent;
  m_docComment = docComment;
  if (isSystemLib) {
    m_attrs = m_attrs | AttrBuiltin;
  }
}

PreClassEmitter::~PreClassEmitter() {
  for (MethodVec::const_iterator it = m_methods.begin();
       it != m_methods.end(); ++it) {
    delete *it;
  }
}

void PreClassEmitter::addInterface(const StringData* n) {
  m_interfaces.push_back(n);
}

void PreClassEmitter::addEnumInclude(const StringData* n) {
  m_enumIncludes.push_back(n);
}

bool PreClassEmitter::addMethod(FuncEmitter* method) {
  MethodMap::const_iterator it = m_methodMap.find(method->name);
  if (it != m_methodMap.end()) {
    return false;
  }
  m_methods.push_back(method);
  m_methodMap[method->name] = method;
  return true;
}

bool PreClassEmitter::addProperty(
  const StringData* n,
  Attr attrs,
  const StringData* userType,
  TypeIntersectionConstraint&& typeConstraints,
  const StringData* docComment,
  const TypedValue* val,
  RepoAuthType repoAuthType,
  UserAttributeMap userAttributes
) {
  assertx(typeConstraints.validForProp());
  PropMap::Builder::const_iterator it = m_propMap.find(n);
  if (it != m_propMap.end()) {
    return false;
  }
  PreClassEmitter::Prop prop{
    this,
    n,
    attrs,
    userType,
    std::move(typeConstraints),
    docComment,
    val,
    repoAuthType,
    userAttributes
  };
  m_propMap.add(prop.name(), prop);
  return true;
}

bool PreClassEmitter::addAbstractConstant(const StringData* n,
                                          ConstModifierFlags::Kind kind,
                                          bool fromTrait) {
  assertx(kind == ConstModifierFlags::Kind::Value ||
          kind == ConstModifierFlags::Kind::Type);

  auto it = m_constMap.find(n);
  if (it != m_constMap.end()) {
    return false;
  }
  PreClassEmitter::Const cns(n, nullptr, nullptr,
                             {}, Array{}, kind,
                             Const::Invariance::None,
                             true, fromTrait);
  m_constMap.add(cns.name(), cns);
  return true;
}

bool PreClassEmitter::addContextConstant(
    const StringData* n,
    PreClassEmitter::Const::CoeffectsVec coeffects,
    bool isAbstract,
    bool fromTrait) {
  auto it = m_constMap.find(n);
  if (it != m_constMap.end()) {
    return false;
  }
  PreClassEmitter::Const cns(n, nullptr, nullptr,
                             std::move(coeffects),
                             Array{},
                             ConstModifierFlags::Kind::Context,
                             Const::Invariance::None,
                             isAbstract, fromTrait);
  m_constMap.add(cns.name(), cns);
  return true;
}

bool PreClassEmitter::addConstant(const StringData* n,
                                  const StringData* cls,
                                  const TypedValue* val,
                                  Array resolvedTypeStructure,
                                  ConstModifierFlags::Kind kind,
                                  Const::Invariance invariance,
                                  bool fromTrait,
                                  bool isAbstract) {
  assertx(kind == ConstModifierFlags::Kind::Value ||
          kind == ConstModifierFlags::Kind::Type);
  assertx(IMPLIES(kind == ConstModifierFlags::Kind::Value, !cls));
  assertx(IMPLIES(kind == ConstModifierFlags::Kind::Value,
                  resolvedTypeStructure.isNull()));
  assertx(IMPLIES(!resolvedTypeStructure.isNull(),
                  resolvedTypeStructure.isDict() &&
                  !resolvedTypeStructure.empty() &&
                  resolvedTypeStructure->isStatic()));
  assertx(IMPLIES(invariance != Const::Invariance::None,
                  !resolvedTypeStructure.isNull()));
  assertx(val);

  ConstMap::Builder::const_iterator it = m_constMap.find(n);
  if (it != m_constMap.end()) {
    return false;
  }
  PreClassEmitter::Const cns(n, cls, val, {}, std::move(resolvedTypeStructure),
                             kind, invariance, isAbstract, fromTrait);
  m_constMap.add(cns.name(), cns);
  return true;
}

void PreClassEmitter::setTypeParamNames(
  std::vector<PackedStringPtr>&& typeParamNames
) {
  this->m_typeParamNames = FixedVector(typeParamNames);
}

folly::Range<const PackedStringPtr*> PreClassEmitter::getTypeParamNames() const {
  return folly::Range<const PackedStringPtr*>(
    m_typeParamNames.begin(),
    m_typeParamNames.end()
  );
}

void PreClassEmitter::addUsedTrait(const StringData* traitName) {
  m_usedTraits.push_back(traitName);
}

const StaticString
  s_NativeData("__NativeData"),
  s_DynamicallyConstructible("__DynamicallyConstructible"),
  s_DynamicallyReferenced("__DynamicallyReferenced"),
  s_invoke("__invoke"),
  s_coeffectsProp("86coeffects");

FuncEmitter* PreClassEmitter::closureBody() const {
  auto const invoke = lookupMethod(s_invoke.get());
  return invoke && invoke->isClosureBody ? invoke : nullptr;
}

PreClass* PreClassEmitter::create(Unit& unit) const {
  Attr attrs = m_attrs;
  assertx(IMPLIES(unit.isSystemLib(), attrs & AttrBuiltin));
  if (Cfg::Eval::ForceAllPersistent) {
    attrs |= AttrPersistent;
  } else if (attrs & AttrPersistent &&
      !Cfg::Repo::Authoritative && !unit.isSystemLib()) {
    attrs = Attr(attrs & ~AttrPersistent);
  }

  auto const dynConstructSampleRate = [&] () -> int64_t {
    if (!(attrs & AttrDynamicallyConstructible)) return -1;

    auto const it = m_userAttributes.find(s_DynamicallyConstructible.get());
    if (it == m_userAttributes.end()) return -1;

    assertx(isArrayLikeType(type(it->second)));
    auto const rate = val(it->second).parr->get(int64_t(0));
    if (!isIntType(type(rate)) || val(rate).num < 0) return -1;

    attrs = Attr(attrs & ~AttrDynamicallyConstructible);
    return val(rate).num;
  }();

  if (auto const invoke = closureBody()) {
    attrs |= AttrIsClosureClass;
    if (!invoke->coeffectRules.empty()) {
      assertx(invoke->coeffectRules.size() == 1);
      if (invoke->coeffectRules[0].isClosureParentScope()) {
        attrs |= AttrHasClosureCoeffectsProp;
      } else {
        assertx(invoke->coeffectRules[0].isCaller());
      }
    }
  }

  if (attrs & AttrInternal &&
      m_userAttributes.contains(s_SoftInternal.get())) {
    attrs |= AttrInternalSoft;
  }

  assertx(attrs & AttrPersistent || !unit.isSystemLib());

  auto pc = std::make_unique<PreClass>(
    &unit, m_line1, m_line2, m_name,
    attrs, m_parent, m_docComment);
  pc->m_interfaces = m_interfaces;
  pc->m_includedEnums = m_enumIncludes;
  pc->m_usedTraits = m_usedTraits;
  pc->m_requirements = m_requirements;
  pc->m_enumBaseTy = m_enumBaseTy;
  pc->m_numDeclMethods = -1;
  pc->m_ifaceVtableSlot = m_ifaceVtableSlot;
  pc->m_dynConstructSampleRate = dynConstructSampleRate;
  pc->m_typeParamNames = FixedVector<PackedStringPtr>(m_typeParamNames);

  // Set user attributes.
  [&] {
    pc->m_userAttributes = m_userAttributes;
    pc->m_nativeDataInfo = nullptr;
    if (!m_userAttributes.size()) return;

    // Check for <<__NativeData>>.
    auto it = m_userAttributes.find(s_NativeData.get());
    if (it == m_userAttributes.end()) return;

    pc->m_nativeDataInfo = Native::getNativeDataInfo(m_name.get());
  }();

  PreClass::MethodMap::Builder methodBuild;
  for (MethodVec::const_iterator it = m_methods.begin();
       it != m_methods.end(); ++it) {
    Func* f = (*it)->create(unit, pc.get());
    if (f->attrs() & AttrTrait) {
      if (pc->m_numDeclMethods == -1) {
        pc->m_numDeclMethods = it - m_methods.begin();
      }
    } else if (!f->isGenerated()) {
      assertx(pc->m_numDeclMethods == -1);
    }
    methodBuild.add(f->name(), f);
  }
  pc->m_methods.create(methodBuild);

  PreClass::PropMap::Builder propBuild;
  if (pc->attrs() & AttrHasClosureCoeffectsProp) {
    TypedValue tvInit;
    tvWriteUninit(tvInit);

    propBuild.add(
      s_coeffectsProp.get(),
      PreClass::Prop(pc.get(),
      s_coeffectsProp.get(),
      AttrPrivate|AttrSystemInitialValue,
      staticEmptyString(),
      TypeIntersectionConstraint(TypeConstraint()),
      staticEmptyString(),
      tvInit,
      RepoAuthType{},
      UserAttributeMap{}
    ));
  }
  for (unsigned i = 0; i < m_propMap.size(); ++i) {
    const Prop& prop = m_propMap[i];
    assertx(!prop.typeConstraints().isTop());
    propBuild.add(
      prop.name(),
      PreClass::Prop(
        pc.get(),
        prop.name(),
        prop.attrs(),
        prop.userType(),
        TypeIntersectionConstraint(prop.typeConstraints()),
        prop.docComment(),
        prop.val(),
        prop.repoAuthType(),
        prop.userAttributes())
    );
  }

  pc->m_properties.create(propBuild);

  PreClass::ConstMap::Builder constBuild;
  for (unsigned i = 0; i < m_constMap.size(); ++i) {
    const Const& const_ = m_constMap[i];
    TypedValueAux tvaux;
    tvaux.constModifierFlags() = {};
    tvaux.constModifiers() = {};
    tvaux.constModifierFlags().isAbstract = const_.isAbstract();
    if (const_.kind() == ConstModifierFlags::Kind::Context) {
      auto const coeffects =
        getCoeffectsInfoFromList(const_.coeffects(), false).first;
      tvaux.constModifiers().setCoeffects(coeffects);
      if (!const_.coeffects().empty()) {
        tvCopy(make_tv<KindOfInt64>(0), tvaux); // dummy value for m_data
      } else {
        tvWriteConstValMissing(tvaux);
      }
    } else {
      if (const_.valOption()) {
        tvCopy(const_.val(), tvaux);
      } else {
        tvWriteConstValMissing(tvaux);
      }
    }

    tvaux.constModifierFlags().kind = const_.kind();

    assertx(
      IMPLIES(const_.kind() != ConstModifierFlags::Kind::Type, !const_.cls())
    );
    assertx(
      IMPLIES(const_.kind() != ConstModifierFlags::Kind::Type,
              const_.resolvedTypeStructure().isNull())
    );
    assertx(
      IMPLIES(!const_.resolvedTypeStructure().isNull(),
              const_.resolvedTypeStructure().isDict() &&
              !const_.resolvedTypeStructure().empty())
    );
    assertx(
      IMPLIES(const_.invariance() != Const::Invariance::None,
              !const_.resolvedTypeStructure().isNull())
    );

    constBuild.add(
      const_.name(),
      PreClass::Const(
        const_.name(),
        const_.cls() ? const_.cls() : m_name.get(),
        tvaux,
        const_.resolvedTypeStructure(),
        const_.invariance(),
        const_.isFromTrait()
      )
    );
  }
  if (auto nativeConsts = Native::getClassConstants(m_name)) {
    for (auto cnsMap : *nativeConsts) {
      TypedValueAux tvaux;
      tvCopy(cnsMap.second, tvaux);
      tvaux.constModifierFlags() = {};
      tvaux.constModifiers() = {};
      constBuild.add(cnsMap.first, PreClass::Const(cnsMap.first,
                                                   m_name,
                                                   tvaux,
                                                   Array{},
                                                   Const::Invariance::None,
                                                   false));
    }
  }

  pc->m_constants.create(constBuild);
  return pc.release();
}

template<class SerDe> void PreClassEmitter::serdeMetaData(SerDe& sd) {
  // NOTE: name and a few other fields currently
  // serialized outside of this.
  sd(m_line1)
    (m_line2)
    (m_attrs)
    (m_parent)
    (m_docComment)
    (m_ifaceVtableSlot)

    (m_interfaces)
    (m_enumIncludes)
    (m_usedTraits)
    (m_requirements)
    (m_userAttributes)
    (m_propMap, [](Prop p) { return p.name(); })
    (m_constMap, [](Const c) { return c.name(); })
    (m_enumBaseTy)
    (m_typeParamNames)
    ;
}

template void PreClassEmitter::serdeMetaData<>(BlobDecoder&);
template void PreClassEmitter::serdeMetaData<>(BlobEncoder&);

} // HPHP
