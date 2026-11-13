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

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/array-data.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/func-emitter.h"

#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct UnitEmitter;

///////////////////////////////////////////////////////////////////////////////

/*
 * Information about an extension subclass of ObjectData.
 */
struct BuiltinObjExtents {
  // Total sizeof(c_Foo).
  size_t totalSizeBytes;

  // The offset of the ObjectData subobject in c_Foo.
  ptrdiff_t odOffsetBytes;
};

struct PreClassEmitter {
  using MethodVec = std::vector<FuncEmitter*>;

  struct Prop {
    Prop()
      : m_name(nullptr)
      , m_attrs(AttrNone)
      , m_userType(nullptr)
      , m_docComment(nullptr)
      , m_repoAuthType{}
      , m_userAttributes{}
    {}

    Prop(const PreClassEmitter* pce,
         const StringData* n,
         Attr attrs,
         const StringData* userType,
         TypeIntersectionConstraint&& typeConstraints,
         const StringData* docComment,
         const TypedValue* val,
         RepoAuthType repoAuthType,
         UserAttributeMap userAttributes);
    ~Prop();

    const StringData* name() const { return m_name; }
    Attr attrs() const { return m_attrs; }
    const StringData* userType() const { return m_userType; }
    const TypeIntersectionConstraint& typeConstraints() const {
      return m_typeConstraints;
    }
    const StringData* docComment() const { return m_docComment; }
    const TypedValue& val() const { return m_val; }
    RepoAuthType repoAuthType() const { return m_repoAuthType; }
    UserAttributeMap userAttributes() const { return m_userAttributes; }

    template<class SerDe> void serde(SerDe& sd) {
      sd(m_name)
        (m_attrs)
        (m_userType)
        (m_docComment)
        (m_val)
        (m_repoAuthType)
        (m_typeConstraints)
        (m_userAttributes)
        ;
    }

  private:
    friend struct PreClassEmitter;

    PackedStringPtr m_name;
    Attr m_attrs;
    PackedStringPtr m_userType;
    PackedStringPtr m_docComment;
    TypedValue m_val;
    RepoAuthType m_repoAuthType;
    TypeIntersectionConstraint m_typeConstraints{};
    UserAttributeMap m_userAttributes;
  };

  struct Const {
    using CoeffectsVec = std::vector<PackedStringPtr>;
    using Invariance = PreClass::Const::Invariance;

    Const()
      : m_name(nullptr)
      , m_cls(nullptr)
      , m_val(make_tv<KindOfUninit>())
      , m_coeffects({})
      , m_resolvedTypeStructure()
      , m_kind(ConstModifierFlags::Kind::Value)
      , m_invariance(Invariance::None)
      , m_isAbstract(false)
      , m_fromTrait(false)
    {}
    Const(const StringData* n,
          const StringData* cls,
          const TypedValue* val,
          CoeffectsVec coeffects,
          Array resolvedTypeStructure,
          ConstModifierFlags::Kind kind,
          Invariance invariance,
          bool isAbstract,
          bool fromTrait)
      : m_name(n)
      , m_cls(cls)
      , m_coeffects(std::move(coeffects))
      , m_resolvedTypeStructure(std::move(resolvedTypeStructure))
      , m_kind(kind)
      , m_invariance(invariance)
      , m_isAbstract(isAbstract)
      , m_fromTrait(fromTrait) {
      if (!val) {
        m_val.reset();
      } else {
        m_val = *val;
      }
    }

    const StringData* name() const { return m_name; }
    const StringData* cls() const { return m_cls; }
    const TypedValue& val() const { return m_val.value(); }
    const Optional<TypedValue>& valOption() const { return m_val; }
    bool isAbstract() const { return m_isAbstract; }
    const CoeffectsVec& coeffects() const { return m_coeffects; }
    const Array& resolvedTypeStructure() const {
      return m_resolvedTypeStructure;
    }
    ConstModifierFlags::Kind kind() const { return m_kind; }
    Invariance invariance() const { return m_invariance; }
    bool isFromTrait() const { return m_fromTrait; }

    template<class SerDe> void serde(SerDe& sd) {
      sd(m_name)
        (m_cls)
        (m_val)
        (m_coeffects)
        (m_resolvedTypeStructure)
        (m_kind)
        (m_invariance)
        (m_isAbstract)
        (m_fromTrait);
      assertx(IMPLIES(!m_resolvedTypeStructure.isNull(),
                      m_resolvedTypeStructure.isDict() &&
                      !m_resolvedTypeStructure.empty() &&
                      m_resolvedTypeStructure->isStatic() &&
                      m_kind == ConstModifierFlags::Kind::Type &&
                      m_val.has_value()));
      assertx(IMPLIES(m_invariance != Invariance::None,
                      !m_resolvedTypeStructure.isNull()));
    }

   private:
   PackedStringPtr m_name;
    // Class which originally defined this const, if HHBBC has
    // propagated the definition (nullptr otherwise).
    PackedStringPtr m_cls;
    Optional<TypedValue> m_val;
    CoeffectsVec m_coeffects;
    Array m_resolvedTypeStructure;
    ConstModifierFlags::Kind m_kind;
    Invariance m_invariance;
    bool m_isAbstract;
    bool m_fromTrait;
  };

  using PropMap = IndexedStringMap<Prop, Slot>;
  using ConstMap = IndexedStringMap<Const, Slot>;

  PreClassEmitter(const StringData* name);
  ~PreClassEmitter();

  void init(int line1, int line2, Attr attrs,
            const StringData* parent, const StringData* docComment,
            bool isSystemLib);

  const StringData* name() const { return m_name; }
  Attr attrs() const { return m_attrs; }
  void setAttrs(Attr attrs) { m_attrs = attrs; }
  void setEnumBaseTy(TypeConstraint ty) { m_enumBaseTy = ty; }
  const TypeConstraint& enumBaseTy() const { return m_enumBaseTy; }
  void setIfaceVtableSlot(Slot s) { m_ifaceVtableSlot = s; }
  const MethodVec& methods() const { return m_methods; }
  bool hasMethod(const StringData* name) const {
    return m_methodMap.find(name) != m_methodMap.end();
  }
  FuncEmitter* lookupMethod(const StringData* name) const {
    return folly::get_default(m_methodMap, name);
  }
  // The `__invoke` of a closure class; null for any other class.
  FuncEmitter* closureBody() const;
  size_t numProperties() const { return m_propMap.size(); }
  bool hasProp(const StringData* name) const {
    return m_propMap.find(name) != m_propMap.end();
  }
  const PropMap::Builder& propMap() const { return m_propMap; }
  const ConstMap::Builder& constMap() const { return m_constMap; }
  const StringData* docComment() const { return m_docComment; }
  const StringData* parentName() const { return m_parent; }
  static bool IsAnonymousClassName(std::string_view name) {
    return name.find('$') != std::string::npos;
  }

  void setDocComment(const StringData* sd) { m_docComment = sd; }

  void addInterface(const StringData* n);
  const std::vector<PackedStringPtr>& interfaces() const {
    return m_interfaces;
  }
  void addEnumInclude(const StringData* n);
  const std::vector<PackedStringPtr>& enumIncludes() const {
    return m_enumIncludes;
  }
  bool addMethod(FuncEmitter* method);
  bool addProperty(
    const StringData* n,
    Attr attrs,
    const StringData* userType,
    TypeIntersectionConstraint&& typeConstraints,
    const StringData* docComment,
    const TypedValue* val,
    RepoAuthType,
    UserAttributeMap
  );
  bool addConstant(
    const StringData* n,
    const StringData* cls,
    const TypedValue* val,
    Array resolvedTypeStructure,
    ConstModifierFlags::Kind kind,
    Const::Invariance invariance,
    bool fromTrait,
    bool isAbstract
  );
  void setTypeParamNames(std::vector<PackedStringPtr>&& typeParamNames);
  folly::Range<const PackedStringPtr*> getTypeParamNames() const;
  bool addContextConstant(
    const StringData* n,
    Const::CoeffectsVec coeffects,
    bool isAbstract,
    bool fromTrait = false
  );
  bool addAbstractConstant(
    const StringData* n,
    ConstModifierFlags::Kind kind =
    ConstModifierFlags::Kind::Value,
    bool fromTrait = false);
  void addUsedTrait(const StringData* traitName);
  void addClassRequirement(const PreClass::ClassRequirement req) {
    m_requirements.push_back(req);
  }
  const std::vector<PackedStringPtr>& usedTraits() const {
    return m_usedTraits;
  }
  const std::vector<PreClass::ClassRequirement>& requirements() const {
    return m_requirements;
  }

  void setUserAttributes(UserAttributeMap map) {
    m_userAttributes = std::move(map);
  }
  UserAttributeMap userAttributes() const { return m_userAttributes; }

  PreClass* create(Unit& unit) const;

  template<class SerDe> void serdeMetaData(SerDe&);

  std::pair<int,int> getLocation() const {
    return std::make_pair(m_line1, m_line2);
  }

  bool areMemoizeCacheKeysAllocated() const {
    return m_memoizeInstanceSerial > 0;
  }
  int getNextMemoizeCacheKey() {
    return m_memoizeInstanceSerial++;
  }

 private:
  using MethodMap = hphp_hash_map<
    PackedStringPtr, FuncEmitter*, string_data_hash, string_data_same
  >;

  int m_line1;
  int m_line2;
  PackedStringPtr m_name;
  Attr m_attrs;
  PackedStringPtr m_parent;
  PackedStringPtr m_docComment;
  TypeConstraint m_enumBaseTy;
  Slot m_ifaceVtableSlot{kInvalidSlot};
  int m_memoizeInstanceSerial{0};

  std::vector<PackedStringPtr> m_interfaces;
  std::vector<PackedStringPtr> m_enumIncludes;
  std::vector<PackedStringPtr> m_usedTraits;
  std::vector<PreClass::ClassRequirement> m_requirements;
  UserAttributeMap m_userAttributes;
  MethodVec m_methods;
  MethodMap m_methodMap;
  PropMap::Builder m_propMap;
  ConstMap::Builder m_constMap;
  FixedVector<PackedStringPtr> m_typeParamNames;
};

///////////////////////////////////////////////////////////////////////////////
}
