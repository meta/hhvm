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

#include "hphp/runtime/vm/type-constraint.h"

#include <variant>

#include <fmt/format.h>
#include <folly/MapUtil.h>
#include <folly/Random.h>

#include "hphp/util/configs/eval.h"
#include "hphp/util/match.h"
#include "hphp/util/random.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/base/annot-type.h"

#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/unit-util.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

TRACE_SET_MOD(runtime)

using ClassConstraint = TypeConstraint::ClassConstraint;
using UnionConstraint = TypeConstraint::UnionConstraint;
using UnionClassList = TypeConstraint::UnionClassList;
using UnionTypeMask = TypeConstraint::UnionTypeMask;

//////////////////////////////////////////////////////////////////////

const StaticString s___invoke("__invoke");

TypeConstraint::TypeConstraint()
  : m_flags(TypeConstraintFlags::NoFlags)
  , m_u {
      .single={
        .type=Type::Nothing,
        .class_=ClassConstraint{nullptr}
      }
    } {
  initSingle();
}

TypeConstraint::TypeConstraint(const StringData* typeName, TypeConstraintFlags flags)
  : m_flags(flags)
  , m_u {
      .single={
        .type=Type::Nothing,
        .class_=ClassConstraint{typeName}
      }
    } {
  assert(!isUnion());
  initSingle();
}

TypeConstraint::TypeConstraint(Type type, TypeConstraintFlags flags, ClassConstraint class_)
  : m_flags(flags)
  , m_u {
      .single={
        .type=type,
        .class_=class_
      }
    } {
  assert(!isUnion());
  m_u.single.class_.init(m_u.single.type);
}

TypeConstraint::TypeConstraint(Type type,
                               TypeConstraintFlags flags,
                               const PackedStringPtr typeName)
  : TypeConstraint(
    type,
    flags,
    makeClass(type, typeName)
  ) {}

TypeConstraint::TypeConstraint(Type type, TypeConstraintFlags flags)
  : TypeConstraint(type, flags, nullptr) {
}

TypeConstraint::TypeConstraint(TypeConstraintFlags flags,
                               UnionTypeMask mask,
                               PackedStringPtr typeName,
                               PackedPtr<const UnionClassList> classes)
  : m_flags(flags)
  , m_u {
      .union_={mask, typeName, classes}
    } {
  assert(isUnion());
  initUnion();
}

TypeConstraint::UnionBuilder::UnionBuilder(PackedStringPtr typeName, size_t capacity) : m_typeName(typeName) {
  m_classes.m_list.reserve(capacity);
}

Optional<TypeConstraint> TypeConstraint::UnionBuilder::recordConstraint(const TypeConstraint& tc) {
  if (!tc.isCheckable()) {
    // Canonicalization: If we contain a non-checkable member then we're not
    // checkable so just return mixed.
    return TypeConstraint{
      AnnotType::Mixed,
      TypeConstraintFlags::Resolved,
      ClassConstraint { m_typeName }
    };
  }

  // Copy over common flags.
  m_flags |= tc.flags() & (TypeConstraintFlags::Nullable
                           | TypeConstraintFlags::TypeVar
                           | TypeConstraintFlags::Soft
                           | TypeConstraintFlags::TypeConstant
                           | TypeConstraintFlags::DisplayNullable
                           | TypeConstraintFlags::UpperBound);

  switch (tc.type()) {
    case AnnotType::Mixed: {
      // Canonicalization: If we have a mixed then we're just mixed.
      return TypeConstraint{
        AnnotType::Mixed,
        TypeConstraintFlags::Resolved,
        ClassConstraint { m_typeName }
      };
    }
    case AnnotType::Nonnull: {
      m_containsNonnull = true;
      break;
    }
    case AnnotType::Null: {
      // Canonicalization: If we expect a null then just apply nullable. Note
      // that this means we might end up with a simple nullable constraint
      // (like `?int`) represented differently than `null | int`.
      m_flags |= TypeConstraintFlags::Nullable;
      break;
    }
    case AnnotType::Nothing:
    case AnnotType::NoReturn: {
      // Canonicalization: Bottom adds nothing to our union. This can cause
      // problems with canonical representations like `int | bottom` not
      // returning the same as `int`.
      break;
    }

    case AnnotType::Bool: {
      m_preciseTypeMask |= kUnionTypeBool;
      break;
    }
    case AnnotType::Int: {
      m_preciseTypeMask |= kUnionTypeInt;
      break;
    }
    case AnnotType::Float: {
      m_preciseTypeMask |= kUnionTypeFloat;
      break;
    }
    case AnnotType::String: {
      m_preciseTypeMask |= kUnionTypeString;
      break;
    }
    case AnnotType::Object: {
      m_preciseTypeMask |= kUnionTypeObject;
      break;
    }
    case AnnotType::Resource: {
      m_preciseTypeMask |= kUnionTypeResource;
      break;
    }
    case AnnotType::Dict: {
      m_preciseTypeMask |= kUnionTypeDict;
      break;
    }
    case AnnotType::Vec: {
      m_preciseTypeMask |= kUnionTypeVec;
      break;
    }
    case AnnotType::Keyset: {
      m_preciseTypeMask |= kUnionTypeKeyset;
      break;
    }
    case AnnotType::Callable: {
      m_preciseTypeMask |= kUnionTypeCallable;
      break;
    }
    case AnnotType::This: {
      // Not currently possible to specify with case types - case types are
      // defined at top-level where 'this' isn't legal. But we might as well
      // support it minimally.
      m_preciseTypeMask |= kUnionTypeThis;
      break;
    }
    case AnnotType::VecOrDict: {
      m_preciseTypeMask |= kUnionTypeDict | kUnionTypeVec;
      break;
    }

    case AnnotType::Number: {
      m_preciseTypeMask |= kUnionTypeInt | kUnionTypeFloat;
      break;
    }
    case AnnotType::ArrayKey: {
      m_preciseTypeMask |= kUnionTypeInt | kUnionTypeString;
      break;
    }

    case AnnotType::ArrayLike: {
      m_preciseTypeMask |= kUnionTypeDict | kUnionTypeVec | kUnionTypeKeyset;
      break;
    }
    case AnnotType::Classname: {
      m_preciseTypeMask |= kUnionTypeClassname;
      break;
    }
    case AnnotType::Class: {
      // TODO(T199611023) when we enforce the inner class, share the SubObject bit
      m_preciseTypeMask |= kUnionTypeClass;
      break;
    }
    case AnnotType::ClassOrClassname: {
      // This type won't be supported by case types, nor will class<T> or classname<T>
      // until configs ClassPassesClassname and StringPassesClass are both false
      m_preciseTypeMask |= kUnionTypeClassname;
      m_preciseTypeMask |= kUnionTypeClass;
      break;
    }
    case AnnotType::SubObject: {
      assertx(tc.typeName());
      m_classes.m_list.emplace_back(tc.m_u.single.class_);
      m_preciseTypeMask |= kUnionTypeSubObject;
      switch (m_resolved) {
      case ResolvedType::Unspecified:
        m_resolved = ResolvedType::Resolved;
        break;
      case ResolvedType::Unresolved:
        m_resolved = ResolvedType::Mixed;
        break;
      case ResolvedType::Resolved:
      case ResolvedType::Mixed:
        break;
      }
      break;
    }
    case AnnotType::Unresolved: {
      assertx(tc.typeName());
      m_classes.m_list.emplace_back(tc.m_u.single.class_);
      m_preciseTypeMask |= kUnionTypeSubObject;
      switch (m_resolved) {
      case ResolvedType::Unspecified:
        m_resolved = ResolvedType::Unresolved;
        break;
      case ResolvedType::Resolved:
        m_resolved = ResolvedType::Mixed;
        break;
      case ResolvedType::Unresolved:
      case ResolvedType::Mixed:
        break;
      }
      break;
    }
  }

  return std::nullopt;
}

TypeConstraint TypeConstraint::UnionBuilder::finish() && {
  if (m_containsNonnull) {
    // Canonicalization: If we have a nonnull then we're just nonnull -
    // unless we also have a nullable in which case we're mixed.
    if (contains(m_flags, TypeConstraintFlags::Nullable)) {
      return TypeConstraint{
        AnnotType::Mixed,
        TypeConstraintFlags::Resolved,
        ClassConstraint { m_typeName }
      };
    } else {
      return TypeConstraint{
        AnnotType::Nonnull,
        TypeConstraintFlags::Resolved,
        ClassConstraint { m_typeName }
      };
    }
  }

  if (!m_preciseTypeMask) {
    // Canonicalization: An empty union mask can either be nullable, which case
    // the type is precisely null, or non-nullable in which case the type is
    // nothing.
    assertx(m_classes.m_list.empty());
    if (contains(m_flags, TypeConstraintFlags::Nullable)) {
      return TypeConstraint{
        AnnotType::Null,
        TypeConstraintFlags::Resolved,
        ClassConstraint { m_typeName }
      };
    } else {
      return TypeConstraint{
        AnnotType::Nothing,
        TypeConstraintFlags::Resolved,
        ClassConstraint { m_typeName }
      };
    }
  }

  if (m_classes.m_list.size() > Cfg::Eval::MaxCaseTypeVariants) {
    auto msg = fmt::format(
      "Case type '{}' exceeds allowed variants of {} ({} requested)",
      m_typeName,
      Cfg::Eval::MaxCaseTypeVariants,
      m_classes.m_list.size());
    raise_error(msg);
  }

  PackedPtr<const UnionClassList> classes = nullptr;
  if (!m_classes.m_list.empty()) {
    classes = UnionConstraint::allocObjects(std::move(m_classes));
  }

  // If we haven't seen any objects or unresolved then we're just simple
  // primitives - which are resolved.
  switch (m_resolved) {
  case ResolvedType::Unspecified:
  case ResolvedType::Resolved:
    m_flags |= TypeConstraintFlags::Resolved;
    break;
  case ResolvedType::Unresolved:
  case ResolvedType::Mixed:
    break;
  }

  return TypeConstraint{ m_flags, m_preciseTypeMask, m_typeName, classes };
}

TypeConstraint TypeConstraint::makeMixed() {
  return TypeConstraint{
    AnnotType::Mixed,
    TypeConstraintFlags::Nullable,
    LAZY_STATIC_STRING(annotTypeName(AnnotType::Mixed))
  };
}

template<class SerDe>
void TypeConstraint::serde(SerDe& sd) {
  sd(m_flags);
  if (isUnion()) {
    serdeUnion(sd);
  } else {
    serdeSingle(sd);
  }
}

template void TypeConstraint::serde(BlobEncoder&);
template void TypeConstraint::serde(BlobDecoder&);

template<class SerDe>
void TypeConstraint::serdeUnion(SerDe& sd) {
  sd(m_u.union_.m_mask);
  sd(m_u.union_.m_typeName);

  // m_classes
  if constexpr (SerDe::deserializing) {
    size_t sz;
    sd(sz);
    m_u.union_.m_classes = nullptr;
    if (sz) {
      UnionClassList classes;
      classes.m_list.reserve(sz);
      bool resolved = contains(m_flags, TypeConstraintFlags::Resolved);
      for (size_t i = 0; i < sz; ++i) {
        ClassConstraint oc;
        oc.serdeHelper(sd, resolved);
        oc.init(resolved ? AnnotType::SubObject : AnnotType::Unresolved);
        classes.m_list.emplace_back(std::move(oc));
      }
      m_u.union_.m_classes = UnionConstraint::allocObjects(std::move(classes));
    }
  } else {
    auto classes = m_u.union_.m_classes;
    size_t sz = classes ? classes->m_list.size() : 0;
    sd(sz);
    if (classes) {
      bool resolved = contains(m_flags, TypeConstraintFlags::Resolved);
      for (const ClassConstraint& oc : classes->m_list) {
        const_cast<ClassConstraint&>(oc).serdeHelper(sd, resolved);
      }
    }
  }
}

template void TypeConstraint::serdeSingle(BlobEncoder&);
template void TypeConstraint::serdeSingle(BlobDecoder&);

template void TypeConstraint::serdeUnion(BlobEncoder&);
template void TypeConstraint::serdeUnion(BlobDecoder&);

template<class SerDe>
void TypeConstraint::serdeSingle(SerDe& sd)  {
  sd(m_u.single.type);
  bool isSubObject = m_u.single.type == AnnotType::SubObject;
  m_u.single.class_.serdeHelper(sd, isSubObject);
  if constexpr (SerDe::deserializing) {
    m_u.single.class_.init(m_u.single.type);
  }
}

ClassConstraint::ClassConstraint(PackedStringPtr typeName)
  : ClassConstraint(nullptr, typeName, nullptr) {
  assertx(!typeName || typeName->isStatic());
}

ClassConstraint::ClassConstraint(PackedStringPtr clsName,
  PackedStringPtr typeName,
                                 PackedPtr<const NamedType> namedType)
  : m_clsName(clsName)
  , m_typeName(typeName)
  , m_namedType(namedType)
{
  assertx(!clsName || clsName->isStatic());
  assertx(!typeName || typeName->isStatic());
}

ClassConstraint::ClassConstraint(Class& cls)
  : ClassConstraint(cls.name(),
                    cls.name(),
                    nullptr) {}

template <typename SerDe>
void ClassConstraint::serdeHelper(SerDe& sd, bool isSubObject) {
  sd(m_typeName);
  if (isSubObject) {
    sd(m_clsName);
  }
}

template void ClassConstraint::serdeHelper(BlobEncoder&, bool isSubObject);
template void ClassConstraint::serdeHelper(BlobDecoder&, bool isSubObject);

void ClassConstraint::init(AnnotType const type) {
  bool isSubObject = type == Type::SubObject;
  bool isUnresolved = type == Type::Unresolved;
  if (isSubObject) {
    assertx(m_clsName);
    m_namedType = NamedType::getOrCreate(m_clsName);
  } else if (isUnresolved) {
    assertx(m_typeName);
    m_namedType = NamedType::getOrCreate(m_typeName);
  }
  FTRACE(5, "TypeConstraint: this {} NamedType: {}\n",
        this, m_namedType.get());
}

void TypeConstraint::initUnion() {
  TRACE(5, "TypeConstraint: this %p union nullable %d\n",
        this, isNullable());
}

void TypeConstraint::initSingle() {
  auto& single = m_u.single;
  if (single.class_.m_typeName == nullptr
      || single.class_.m_typeName->empty()
      || isTypeVar()
      || isTypeConstant()) {
    single.type = Type::Mixed;
    return;
  }
  FTRACE(5, "TypeConstraint: this {} type {}, nullable {}\n",
        this, typeName(), isNullable());
  auto const mptr = nameToAnnotType(typeName());
  if (mptr) {
    single.type = *mptr;
    m_flags |= TypeConstraintFlags::Resolved;
    assertx(single.type != Type::SubObject);
    assertx(getAnnotDataType(single.type) != KindOfPersistentString);
    return;
  }
  bool resolved = contains(m_flags, TypeConstraintFlags::Resolved);
  single.type = resolved ? Type::SubObject : Type::Unresolved;
  if (resolved) {
    TRACE(5, "TypeConstraint: this %p pre-resolved type %s, treating as %s\n",
          this, typeName()->data(), tname(getAnnotDataType(single.type)).c_str());
  } else {
    TRACE(5, "TypeConstraint: this %p no such type %s, marking as unresolved\n",
          this, typeName()->data());
  }
  single.class_.init(single.type);
}

ClassConstraint TypeConstraint::makeClass(Type type,
                                          const PackedStringPtr typeName) {
  switch (type) {
    case Type::SubObject:
      return ClassConstraint { typeName, typeName, nullptr };
    default:
      return ClassConstraint { typeName };
  }
}

namespace {
template<std::ranges::input_range R>
size_t stableHashVec(R&& v) {
  size_t h = 0;
  for (auto& c : v) {
    h = folly::hash::hash_combine(h, c.stableHash());
  }
  return h;
}

// Returns the set of all possible default values for a given annot-type.
AnnotTypeDefault annotTypeDefaultValues(AnnotType at) {
  switch (at) {
    case AnnotType::This:
    case AnnotType::Callable:
    case AnnotType::Resource:
    case AnnotType::Object:
    case AnnotType::SubObject:
    case AnnotType::Nothing:
    case AnnotType::NoReturn:
    case AnnotType::Classname:
    case AnnotType::Class:
    case AnnotType::ClassOrClassname:
    case AnnotType::Null:       return AnnotTypeDefault::Null;
    case AnnotType::Mixed:      return AnnotTypeDefault::Any;
    case AnnotType::Nonnull:    return AnnotTypeDefault::AnyNonNull;
    case AnnotType::Number:     return AnnotTypeDefault::ZeroNumber;
    case AnnotType::ArrayKey:   return AnnotTypeDefault::ZeroIntOrEmptyString;
    case AnnotType::Int:        return AnnotTypeDefault::ZeroInt;
    case AnnotType::Bool:       return AnnotTypeDefault::False;
    case AnnotType::Float:      return AnnotTypeDefault::ZeroDouble;
    case AnnotType::ArrayLike:  return AnnotTypeDefault::EmptyArray;
    case AnnotType::VecOrDict:  return AnnotTypeDefault::EmptyVecOrDict;
    case AnnotType::Vec:        return AnnotTypeDefault::EmptyVec;
    case AnnotType::String:     return AnnotTypeDefault::EmptyString;
    case AnnotType::Dict:       return AnnotTypeDefault::EmptyDict;
    case AnnotType::Keyset:     return AnnotTypeDefault::EmptyKeyset;
    case AnnotType::Unresolved: return AnnotTypeDefault::None;
  }
  always_assert(false);
}
} // anonymous namespace

size_t UnionClassList::stableHash() const {
  return stableHashVec(m_list);
}

namespace {
struct UnionClassListHasher {
  using is_transparent = void;
  size_t operator()(PackedPtr<const UnionClassList> s) const {
    return s->stableHash();
  }
  size_t operator()(const UnionClassList &s) const {
    return s.stableHash();
  }
};
struct UnionClassListEq {
  using is_transparent = void;
  bool operator()(const UnionClassList& s1, PackedPtr<const UnionClassList> s2) const {
    return s1 == *s2;
  }
  bool operator()(PackedPtr<const UnionClassList> s1, PackedPtr<const UnionClassList> s2) const {
    return *s1 == *s2;
  }
};
}

PackedPtr<const UnionClassList>
UnionConstraint::allocObjects(UnionClassList objects) {
  struct Table {
    hphp_fast_set<PackedPtr<const UnionClassList>, UnionClassListHasher, UnionClassListEq> table;
  };
  static folly::Synchronized<Table> g_table;

  // Sort by name to ensure a consistent ordering (important when they
  // might be serialized).
  std::sort(
    objects.m_list.begin(),
    objects.m_list.end(),
    [] (const ClassConstraint& a, const ClassConstraint& b) {
      return string_data_lt{}(a.m_typeName, b.m_typeName);
    }
  );

  {
    auto rlock = g_table.rlock();
    auto it = rlock->table.find(objects);
    if (it != rlock->table.end()) {
      return *it;
    }
  }

  // release the lock and relock as writable.

  auto wlock = g_table.wlock();

  {
    // ensure that the type wasn't created during the unlocked interval.
    auto it = wlock->table.find(objects);
    if (it != wlock->table.end()) {
      return *it;
    }
  }

  auto tc = new(low_malloc(sizeof(UnionClassList))) UnionClassList(std::move(objects));
  wlock->table.insert(tc);
  return tc;
}

bool ClassConstraint::operator==(const ClassConstraint& o) const {
  // The named entity is defined based on the typeName() and is redundant to
  // include in the equality operation.
  return m_clsName == o.m_clsName && m_typeName == o.m_typeName;
}

bool UnionConstraint::operator==(const UnionConstraint& o) const {
  return
    m_mask == o.m_mask &&
    m_typeName == o.m_typeName &&
    (m_classes != nullptr) == (o.m_classes != nullptr) &&
    (m_classes == nullptr || *m_classes == *o.m_classes);
}

bool TypeConstraint::operator==(const TypeConstraint& o) const {
  if (m_flags != o.m_flags) return false;
  if (isUnion()) {
    return m_u.union_ == o.m_u.union_;
  }
  auto& a = m_u.single;
  auto& b = o.m_u.single;
  return a.type == b.type && a.class_ == b.class_;
}

size_t ClassConstraint::stableHash() const {
  size_t clsHash = m_clsName ? m_clsName->hashStatic() : 0;
  size_t typeName = m_typeName ? m_typeName->hashStatic() : 0;
  return folly::hash::hash_combine(clsHash, typeName);
}

size_t UnionConstraint::stableHash() const {
  size_t typeNameHash = m_typeName->hashStatic();
  size_t classesHash = 0;
  if (m_classes) {
    for (auto it : m_classes->m_list) {
      classesHash = folly::hash::hash_combine(classesHash, it.stableHash());
    }
  }
  return folly::hash::hash_combine(typeNameHash, classesHash, m_mask);
}

size_t TypeConstraint::stableHash() const {
  if (isUnion()) {
    return folly::hash::hash_combine(
      std::hash<TypeConstraintFlags>()(m_flags),
      m_u.union_.stableHash()
    );
  } else {
    return folly::hash::hash_combine(
      std::hash<TypeConstraintFlags>()(m_flags),
      std::hash<AnnotType>()(m_u.single.type),
      m_u.single.class_.stableHash()
    );
  }
}

std::string TypeConstraint::displayName(const Class* context /*= nullptr*/,
                                        bool extra /* = false */) const {
  std::string name;

  if (isSoft()) {
    name += '@';
  }
  if (isDisplayNullable()) {
    name += '?';
  }

  const StringData* tn = typeName();
  const char* str = tn ? tn->data() : "";
  auto len = tn ? tn->size() : 0;
  if (len > 3 && tolower(str[0]) == 'h' && tolower(str[1]) == 'h' &&
      str[2] == '\\') {
    bool strip = false;
    const char* stripped = str + 3;
    switch (len - 3) {
      case 3:
        strip = (!tstrcmp(stripped, "int") ||
                 !tstrcmp(stripped, "num"));
        break;
      case 4:
        strip = (!tstrcmp(stripped, "bool") ||
                 !tstrcmp(stripped, "this") ||
                 !tstrcmp(stripped, "null"));
        break;
      case 5: strip = !tstrcmp(stripped, "float"); break;
      case 6: strip = !tstrcmp(stripped, "string"); break;
      case 7:
        strip = (!tstrcmp(stripped, "nonnull") ||
                 !tstrcmp(stripped, "nothing"));
        break;
      case 8:
        strip = (!tstrcmp(stripped, "resource") ||
                 !tstrcmp(stripped, "noreturn") ||
                 !tstrcmp(stripped, "arraykey"));
        break;
      default:
        break;
    }
    if (strip) {
      str = stripped;
    }
  }
  name += str;

  if (extra) {
    if (isUnion()) {
      name.push_back(' ');
      name.push_back('(');
      bool first = true;
      for (auto const& tc : eachTypeConstraintInUnion(*this)) {
        if (!first) {
          name.append(" | ");
        }
        name.append(tc.displayName(context, extra));
        first = false;
      }
      name.push_back(')');
    } else if (contains(m_flags, TypeConstraintFlags::Resolved)) {
      str = nullptr;
      switch (m_u.single.type) {
        case AnnotType::Nothing:  str = "nothing"; break;
        case AnnotType::NoReturn: str = "noreturn"; break;
        case AnnotType::Null:     str = "null"; break;
        case AnnotType::Bool:     str = "bool"; break;
        case AnnotType::Int:      str = "int";  break;
        case AnnotType::Float:    str = "float"; break;
        case AnnotType::String:   str = "string"; break;
        case AnnotType::Object:   str = "object"; break;
        case AnnotType::Resource: str = "resource"; break;
        case AnnotType::Dict:     str = "dict"; break;
        case AnnotType::Vec:      str = "vec"; break;
        case AnnotType::Keyset:   str = "keyset"; break;
        case AnnotType::Number:   str = "num"; break;
        case AnnotType::ArrayKey: str = "arraykey"; break;
        case AnnotType::VecOrDict: str = "vec_or_dict"; break;
        case AnnotType::ArrayLike: str = "AnyArray"; break;
        case AnnotType::Nonnull:  str = "nonnull"; break;
        case AnnotType::Classname: str = "classname"; break;
        case AnnotType::Class:    str = "class"; break;
        case AnnotType::ClassOrClassname: str = "class_or_classname"; break;
        case AnnotType::SubObject: str = clsName()->data(); break;
        case AnnotType::This:
        case AnnotType::Mixed:
        case AnnotType::Callable:
          break;
        case AnnotType::Unresolved:
          not_reached();
      }
      if (str) fmt::format_to(std::back_inserter(name), " ({})", str);
    }
  }
  return name;
}

namespace {

bool contains(UnionTypeMask value, UnionTypeMask bit) {
  return (value & bit) != 0;
}

template<typename T>
void bitName(std::string& out, T& value, T bit, const char* name, const char* sep = "|") {
  if (contains(value, bit)) {
    if (!out.empty()) out.append(sep);
    out.append(name);
    value = (T)(value & ~bit);
  }
}

std::string showUnionTypeMask(UnionTypeMask mask) {
  std::string res;
  bitName(res, mask, TypeConstraint::kUnionTypeBool, "bool");
  bitName(res, mask, TypeConstraint::kUnionTypeCallable, "callable");
  bitName(res, mask, TypeConstraint::kUnionTypeDict, "dict");
  bitName(res, mask, TypeConstraint::kUnionTypeFloat, "float");
  bitName(res, mask, TypeConstraint::kUnionTypeInt, "int");
  bitName(res, mask, TypeConstraint::kUnionTypeKeyset, "keyset");
  bitName(res, mask, TypeConstraint::kUnionTypeObject, "object");
  bitName(res, mask, TypeConstraint::kUnionTypeResource, "resource");
  bitName(res, mask, TypeConstraint::kUnionTypeString, "string");
  bitName(res, mask, TypeConstraint::kUnionTypeThis, "this");
  bitName(res, mask, TypeConstraint::kUnionTypeVec, "vec");
  bitName(res, mask, TypeConstraint::kUnionTypeSubObject, "subObject");
  bitName(res, mask, TypeConstraint::kUnionTypeClassname, "classname");
  bitName(res, mask, TypeConstraint::kUnionTypeClass, "class");
  assertx(mask == 0);
  return res;
}

std::string show(TypeConstraintFlags flags) {
  std::string res;
  bitName(res, flags, TypeConstraintFlags::Nullable, "Nullable");
  bitName(res, flags, TypeConstraintFlags::TypeVar, "TypeVar");
  bitName(res, flags, TypeConstraintFlags::Soft, "Soft");
  bitName(res, flags, TypeConstraintFlags::TypeConstant, "TypeConstant");
  bitName(res, flags, TypeConstraintFlags::Resolved, "Resolved");
  bitName(res, flags, TypeConstraintFlags::DisplayNullable, "DisplayNullable");
  bitName(res, flags, TypeConstraintFlags::UpperBound, "UpperBound");
  bitName(res, flags, TypeConstraintFlags::Union, "Union");
  bitName(res, flags, TypeConstraintFlags::Inherited, "Inherited");
  assertx(flags == TypeConstraintFlags::NoFlags
      || flags == TypeConstraintFlags::SingleTypeConstraint);
  return res;
}

std::string show(const StringData* s) {
  return s ? s->toCppString() : "<null>";
}

std::string show(AnnotType t) {
  switch(t) {
    case AnnotType::Null: return "Null";
    case AnnotType::Bool: return "Bool";
    case AnnotType::Int: return "Int";
    case AnnotType::Float: return "Float";
    case AnnotType::String: return "String";
    case AnnotType::SubObject: return "SubObject";
    case AnnotType::Object: return "Object";
    case AnnotType::Resource: return "Resource";
    case AnnotType::Dict: return "Dict";
    case AnnotType::Vec: return "Vec";
    case AnnotType::Keyset: return "Keyset";
    case AnnotType::Mixed: return "Mixed";
    case AnnotType::Nonnull: return "Nonnull";
    case AnnotType::Callable: return "Callable";
    case AnnotType::Number: return "Number";
    case AnnotType::ArrayKey: return "ArrayKey";
    case AnnotType::This: return "This";
    case AnnotType::VecOrDict: return "VecOrDict";
    case AnnotType::ArrayLike: return "ArrayLike";
    case AnnotType::NoReturn: return "NoReturn";
    case AnnotType::Nothing: return "Nothing";
    case AnnotType::Classname: return "Classname";
    case AnnotType::Class: return "Class";
    case AnnotType::ClassOrClassname: return "ClassOrClassname";
    case AnnotType::Unresolved: return "Unresolved";
  }
  not_reached();
}

} // anonymous namespace

std::string TypeConstraint::debugName() const {
  std::string name;

  auto flags = show(m_flags);
  auto tn = show(typeName());

  if (isUnion()) {
    std::string classes;
    if (auto pcls = m_u.union_.m_classes) {
      for (auto& cls : pcls->m_list) {
        if (!classes.empty()) classes.append(", ");
        classes.append(fmt::format("{{cls:{}, tn:{}}}",
                                      show(cls.m_clsName),
                                      show(cls.m_typeName)));
      }
    }

    return fmt::format(
      "TypeConstraint{{flags:{}, typeName:{}, mask:{}, classes:[{}]}}",
      flags,
      tn,
      showUnionTypeMask(m_u.union_.m_mask),
      classes);
  } else {
    return fmt::format(
      "TypeConstraint{{flags:{}, type:{}, clsName:{}, typeName:{}}}",
      flags,
      show(m_u.single.type),
      show(clsName()),
      tn);
  }
}

AnnotTypeDefault TypeConstraint::getPossibleDefaultValues() const {
  // Nullable type-constraints should always default to null, as Hack
  // guarantees this.
  if (isNullable()) return AnnotTypeDefault::Null;
  AnnotTypeDefault dv = AnnotTypeDefault::None;
  for (const auto& tc : eachTypeConstraintInUnion(*this)) {
    dv = (dv | annotTypeDefaultValues(tc.type()));
  }
  return dv;
}

/*
 * Choose a default value that satisfies all the constraints in the given
 * intersection. Returns nullopt if no such default value exists.
 */
HPHP::Optional<TypedValue> TypeIntersectionConstraint::defaultValue() const {
  AnnotTypeDefault dv = AnnotTypeDefault::Any;
  for (auto const& tc : range()) {
    dv = (dv & tc.getPossibleDefaultValues());
  }

  // It is possible that multiple default values satisfy the given
  // intersection. Choose one in the following order of precedence.
  if (has_flag(dv, AnnotTypeDefault::Null)) return make_tv<KindOfNull>();
  if (has_flag(dv, AnnotTypeDefault::ZeroInt)) return make_tv<KindOfInt64>(0);
  if (has_flag(dv, AnnotTypeDefault::False)) return make_tv<KindOfBoolean>(false);
  if (has_flag(dv, AnnotTypeDefault::ZeroDouble)) return make_tv<KindOfDouble>(0.0);
  if (has_flag(dv, AnnotTypeDefault::EmptyString)) {
    return make_tv<KindOfPersistentString>(staticEmptyString());
  }
  if (has_flag(dv, AnnotTypeDefault::EmptyVec)) {
    return make_tv<KindOfPersistentVec>(staticEmptyVec());
  }
  if (has_flag(dv, AnnotTypeDefault::EmptyDict)) {
    return make_tv<KindOfPersistentDict>(staticEmptyDictArray());
  }
  if (has_flag(dv, AnnotTypeDefault::EmptyKeyset)) {
    return make_tv<KindOfPersistentKeyset>(staticEmptyKeysetArray());
  }
  assertx(dv == AnnotTypeDefault::None);
  return std::nullopt;
}

MemoKeyConstraint TypeIntersectionConstraint::getMemoKeyConstraint() const {
  auto result = MemoKeyConstraint::Any;
  for (auto const& tc : range()) {
    result = result & tc.getMemoKeyConstraint();
  }
  return result;
}

namespace {

/*
 * Look up a TypeAlias for the supplied NamedType (which must be the
 * NamedType for `name'), invoking autoload if necessary for types but not
 * for classes.
 *
 * We don't need to autoload classes because it is impossible to have an
 * instance of a class if it's not defined.  However, we need to autoload
 * typedefs because they can affect whether the parameter type verification
 * would succeed.
 */
const TypeAlias* getTypeAliasWithAutoload(const NamedType* ne,
                                          const StringData* name) {
  auto def = ne->getCachedTypeAlias();
  if (!def) {
    VMRegAnchor _;
    OptString nameStr(const_cast<StringData*>(name));
    if (!AutoloadHandler::s_instance->autoloadTypeAlias(nameStr)) {
      return nullptr;
    }
    def = ne->getCachedTypeAlias();
  }
  return def;
}

struct FoundTypeAlias { const TypeAlias* value; };
struct FoundClass { const Class* value; };
struct NotFound {};
using NamedTypeValue = std::variant<FoundTypeAlias, FoundClass, NotFound>;

/*
 * Look up a TypeAlias or a Class for the supplied NamedType
 * (which must be the NamedType for `name'), invoking autoload if
 * necessary.
 *
 * This is useful when looking up a type annotation that could be either a
 * type alias or an enum class; enum classes are strange in that it
 * *is* possible to have an instance of them even if they are not defined.
 */
NamedTypeValue
getNamedTypeWithAutoload(const NamedType* ne,
                         const StringData* name) {

  if (auto def = ne->getCachedTypeAlias()) {
    return FoundTypeAlias{def};
  }
  if (auto klass = ne->getCachedClass()) return FoundClass{klass};

  // We don't have the class or the typedef, so autoload.
  OptString nameStr(const_cast<StringData*>(name));
  if (AutoloadHandler::s_instance->autoloadTypeOrTypeAlias(nameStr)) {
    // Autoload succeeded, try to grab a typedef or a class.
    if (auto def = ne->getCachedTypeAlias()) return FoundTypeAlias{def};
    if (auto klass = ne->getCachedClass()) return FoundClass{klass};
  }

  return NotFound{};
}

} // namespace

TypeConstraint TypeConstraint::resolvedWithAutoload() const {
  // Nothing to do if we are not unresolved.
  if (!isUnresolved()) return *this;

  auto const p = getNamedTypeWithAutoload(typeNamedType(), typeName());
  auto result = match<Optional<TypeConstraint>>(
    p,
    // Type alias.
    [this](FoundTypeAlias td) -> Optional<TypeConstraint> {
      if (!td.value->value.isUnion()) {
        auto const& tc = td.value->value;
        auto type = tc.type();
        auto klass = type == AnnotType::SubObject
          ? tc.clsNamedType()->getCachedClass() : nullptr;
        auto copy = *this;
        auto const typeName = klass ? klass->name() : nullptr;
        copy.resolveType(type, td.value->value.isNullable(), typeName);
        return copy;
      }
      std::vector<TypeConstraint> parts;
      auto const flags = m_flags & (TypeConstraintFlags::Nullable
                                    | TypeConstraintFlags::TypeVar
                                    | TypeConstraintFlags::Soft
                                    | TypeConstraintFlags::TypeConstant
                                    | TypeConstraintFlags::DisplayNullable
                                    | TypeConstraintFlags::UpperBound);
      for (auto const& tc : eachTypeConstraintInUnion(td.value->value)) {
        parts.push_back(tc);
        parts.back().addFlags(flags);
      }
      return makeUnion(typeName(), parts);
    },
    // Enum.
    [this](FoundClass cls) -> Optional<TypeConstraint> {
      if (isEnum(cls.value)) {
        auto const type = cls.value->enumBaseTy().type();
        auto copy = *this;
        copy.resolveType(type, false, nullptr);
        return copy;
      }
      return std::nullopt;
    },
    [&](NotFound) -> Optional<TypeConstraint> {
      return std::nullopt;
    });

  if (result) {
    return *result;
  }

  // Existing or non-existing class.
  auto copy = *this;
  copy.resolveType(AnnotType::SubObject, false, typeName());
  return copy;
}

MaybeDataType TypeConstraint::underlyingDataTypeResolved() const {
  assertx(!isCallable());
  assertx(IMPLIES(
    !hasConstraint() || isTypeVar() || isTypeConstant(),
    isMixed()));

  auto const tc = resolvedWithAutoload();
  return tc.underlyingDataType();
}

bool TypeConstraint::isMixedResolved() const {
  if (!isCheckable()) return true;
  // isCheckable() implies !isMixed(), so if its not an unresolved object here,
  // we know it cannot be mixed.
  if (!isUnresolved()) return false;
  auto const resolved = resolvedWithAutoload();
  auto each = eachTypeConstraintInUnion(resolved);
  return std::ranges::any_of(each,
                     [](const TypeConstraint& tc) { return !tc.isCheckable(); });
}

bool TypeConstraint::maybeMixed() const {
  if (!isCheckable()) return true;
  // isCheckable() implies !isMixed(), so if its not an unresolved object here,
  // we know it cannot be mixed.
  if (!isUnresolved()) return false;
  if (auto const def = typeNamedType()->getCachedTypeAlias()) {
    auto const each = eachTypeConstraintInUnion(def->value);
    return std::ranges::any_of(
      each,
      [] (auto const& tcu) { return tcu.type() == AnnotType::Mixed; });
  }
  // If its a known class, its definitely not mixed. Otherwise it might be.
  return !typeNamedType()->getCachedClass();
}

bool
TypeConstraint::maybeInequivalentForProp(const TypeConstraint& other) const {
  assertx(validForProp());
  assertx(other.validForProp());

  if (isSoft() != other.isSoft()) return true;

  if (!isCheckable()) return other.isCheckable();
  if (!other.isCheckable()) return true;

  if (isNullable() != other.isNullable()) return true;

  // If one side is unresolved then the best we can do is check the typeName.
  if (isUnresolved() || other.isUnresolved()) {
    return !typeName()->tsame(other.typeName());
  }

  if (isUnion() || other.isUnion()) {
    // unions in property position must match nominally.
   return !typeName()->tsame(other.typeName());
  }

  if (isSubObject() && other.isSubObject()) {
    return !clsName()->tsame(other.clsName());
  }
  if (isSubObject() || other.isSubObject()) return true;

  return type() != other.type();
}

bool
TypeIntersectionConstraint::maybeInequivalentForProp(
  const TypeIntersectionConstraint& other
) const {
  auto check = [](
    const TypeIntersectionConstraint& one,
    const TypeIntersectionConstraint& other
  ) {
    return std::any_of(
      one.range().begin(),
      one.range().end(),
      [&](auto const& tc) {
        return std::all_of(
          other.range().begin(),
          other.range().end(),
          [&](auto const& otc) {
            return tc.maybeInequivalentForProp(otc);
          }
        );
      }
    );
  };
  return check(*this, other) || check(other, *this);
}

bool TypeConstraint::equivalentForProp(const TypeConstraint& other) const {
  assertx(validForProp());
  assertx(other.validForProp());

  if (isSoft() != other.isSoft()) return false;

  if (isSubObject() && other.isSubObject()) {
    return clsName()->tsame(other.clsName());
  }

  if ((isSubObject() || isUnresolved() || isUnion()) &&
      (other.isSubObject() || other.isUnresolved() || isUnion()) &&
      isNullable() == other.isNullable() &&
      typeName()->tsame(other.typeName())) {
    // We can avoid having to resolve the type-hint if they have the same name.
    // TODO: take advantage of clsName() if one of them is object
    return true;
  }

  auto resolved0 = resolvedWithAutoload();
  auto resolved1 = other.resolvedWithAutoload();

  if (resolved0.isUnion() || resolved1.isUnion()) {
    // unions in property position must match nominally.
    return resolved0.typeName()->tsame(other.typeName());
  }

  auto const simplify = [&] (const TypeConstraint& tc) -> std::tuple<AnnotType, const StringData*, bool> {
    if (!tc.isCheckable()) return { AnnotType::Mixed, nullptr, false };

    switch (tc.metaType()) {
      case MetaType::This:
      case MetaType::Number:
      case MetaType::ArrayKey:
      case MetaType::Nonnull:
      case MetaType::VecOrDict:
      case MetaType::ArrayLike:
      case MetaType::Classname:
      case MetaType::Class:
      case MetaType::ClassOrClassname:
      case MetaType::Precise:
      case MetaType::SubObject:
        return { tc.type(), tc.clsName(), tc.isNullable() };
      case MetaType::Nothing:
      case MetaType::NoReturn:
      case MetaType::Callable:
      case MetaType::Mixed:
      case MetaType::Unresolved:
        always_assert(false);
    }

    not_reached();
  };

  return simplify(resolved0) == simplify(resolved1);
}

template <TypeConstraint::CheckMode Mode>
bool TypeConstraint::checkNamedTypeNonObj(tv_rval val,
                                          const Class* context) const {
  assertx(val.type() != KindOfObject);
  assertx(isUnresolved());

  constexpr auto Assert  = Mode == CheckMode::Assert;
  constexpr auto ForProp = Mode == CheckMode::ExactProp;

  auto const p = [&]() -> NamedTypeValue {
    if (!Assert) {
      return getNamedTypeWithAutoload(typeNamedType(), typeName());
    }
    if (auto const def = typeNamedType()->getCachedTypeAlias()) {
      return FoundTypeAlias{def};
    }
    if (auto cls = typeNamedType()->getCachedClass()) return FoundClass{cls};
    return NotFound{};
  }();
  return match<bool>(
    p,
    [&](FoundTypeAlias td) {
      // Common case is that we actually find the alias:
      if (td.value->value.isNullable() && val.type() == KindOfNull) return true;
      Optional<AnnotAction> fallback;
      for (auto const& tc : eachTypeConstraintInUnion(td.value->value)) {
        auto type = tc.type();
        auto const name = tc.isSubObject() ? tc.clsName() : tc.typeName();
        auto result = annotCompat(val.type(), type, name);
        switch (result) {
          case AnnotAction::Pass: return true;
          case AnnotAction::Fail: continue;
          case AnnotAction::CallableCheck:
            if (!ForProp && (Assert || is_callable(tvAsCVarRef(*val)))) return true;
            continue;
          case AnnotAction::WarnClassToString:
          case AnnotAction::ConvertClassToString:
          case AnnotAction::WarnLazyClassToString:
          case AnnotAction::ConvertLazyClassToString:
            // Defer the action to see if there's a more appropriate action later.
            fallback = fallback ? std::min(*fallback, result) : result;
            continue;
          case AnnotAction::WarnClassname:
            assertx(isClassType(val.type()) || isLazyClassType(val.type()));
            assertx(Cfg::Eval::ClassPassesClassname);
            assertx(Cfg::Eval::ClassnameNoticesSampleRate > 0);
            if (Assert) return true;
            fallback = fallback ? std::min(*fallback, result) : result;
            continue;
          case AnnotAction::WarnClass:
            assertx(isStringType(val.type()));
            assertx(Cfg::Eval::ClassTypeLevel == 0);
            assertx(Cfg::Eval::ClassNoticesSampleRate > 0);
            if (Assert) return true;
            fallback = fallback ? std::min(*fallback, result) : result;
            continue;
          case AnnotAction::ObjectCheck:
          case AnnotAction::Fallback:
          case AnnotAction::FallbackCoerce:
            not_reached();
        }
      }

      switch (fallback.value_or(AnnotAction::Fail)) {
        case AnnotAction::WarnClassToString:
        case AnnotAction::ConvertClassToString:
        case AnnotAction::WarnLazyClassToString:
        case AnnotAction::ConvertLazyClassToString:
          // verify*Fail will deal with the conversion/warning
          return false;
        case AnnotAction::WarnClassname:
          if (folly::Random::oneIn(Cfg::Eval::ClassnameNoticesSampleRate, threadLocalRng64())) {
            raise_notice(
              isInherited()
                ? Strings::CLASS_TO_CLASSNAME_INHERITED
                : Strings::CLASS_TO_CLASSNAME
            );
          }
          return true;
        case AnnotAction::WarnClass:
          if (folly::Random::oneIn(Cfg::Eval::ClassNoticesSampleRate, threadLocalRng64())) {
            raise_notice(
              isInherited()
                ? Strings::STRING_TO_CLASS_INHERITED
                : Strings::STRING_TO_CLASS
            );
          }
          return true;
        default:
          return false;
      }
    },
    [&](FoundClass c) {
      // Otherwise, this isn't a proper type alias, but it *might* be a
      // first-class enum. Check if the type is an enum and check the
      // constraint if it is. We only need to do this when the underlying
      // type is not an object, since only int and string can be enums.
      if (isEnum(c.value)) {
        return c.value->enumBaseTy().checkImpl<Mode>(val, context);
      }
      return false;
    },
    [&](NotFound) {
      return Assert;
    }
  );
}

template <bool Assert>
bool TypeConstraint::checkTypeAliasImpl(const ClassConstraint& oc, const Class* cls) {
  // Look up the type alias (autoloading if necessary)
  // and fail if we can't find it
  auto const td = [&]{
    if (!Assert) {
      return getTypeAliasWithAutoload(oc.m_namedType, oc.m_typeName);
    }
    return oc.m_namedType->getCachedTypeAlias();
  }();
  if (!td) return Assert;

  // We found the type alias, check if an object of type 'type' is
  // compatible
  for (auto const& tc : eachTypeConstraintInUnion(td->value)) {
    auto type = tc.type();
    auto klass = type == AnnotType::SubObject
      ? tc.clsNamedType()->getCachedClass() : nullptr;
    switch (getAnnotMetaType(type)) {
      case AnnotMetaType::Precise:
        if (tc.isAnyObject()) return true;
        continue;
      case AnnotMetaType::Mixed:
      case AnnotMetaType::Nonnull:
        return true;
      case AnnotMetaType::Callable:
        if (cls->lookupMethod(s___invoke.get()) != nullptr) return true;
        continue;
      case AnnotMetaType::Nothing:
      case AnnotMetaType::NoReturn:
      case AnnotMetaType::Number:
      case AnnotMetaType::ArrayKey:
      case AnnotMetaType::This:
      case AnnotMetaType::VecOrDict:
      case AnnotMetaType::ArrayLike:
      case AnnotMetaType::Classname:  // TODO: T83332251
      case AnnotMetaType::Class:
      case AnnotMetaType::ClassOrClassname:
        continue;
      case AnnotMetaType::SubObject:
        if (klass && cls->classof(klass)) return true;
        continue;
      case AnnotMetaType::Unresolved:
        not_reached();
        break;
    }
  }
  return false;
}

template bool TypeConstraint::checkTypeAliasImpl<false>(
  const ClassConstraint& oc, const Class* type);
template bool TypeConstraint::checkTypeAliasImpl<true>(
  const ClassConstraint& oc, const Class* type);

template <TypeConstraint::CheckMode Mode>
bool TypeConstraint::checkImpl(tv_rval val,
                               const Class* context) const {
  assertx(isCheckable());
  assertx(tvIsPlausible(*val));

  auto const isAssert          = Mode == CheckMode::Assert;
  auto const isPasses          = Mode == CheckMode::AlwaysPasses;
  auto const isProp DEBUG_ONLY = Mode == CheckMode::ExactProp;

  // We shouldn't provide a context for the conservative checks.
  assertx(!isAssert || !context);
  assertx(!isPasses || !context);
  assertx(!isProp   || validForProp());

  if (isNullable() && val.type() == KindOfNull) return true;

  if (val.type() == KindOfObject) {
    auto const tryCls = [&](const StringData* clsName, const NamedType* ne) {
      // Perfect match seems common enough to be worth skipping the hash
      // table lookup.
      if (clsName->tsame(val.val().pobj->getVMClass()->name())) return true;

      assertx(ne);
      auto const cls = ne->getCachedClass();

      // If we're being conservative we can only use the class if its persistent
      // (otherwise what we infer may not be valid in all requests).
      return
        cls &&
        (!isPasses || classHasPersistentRDS(cls)) &&
        val.val().pobj->instanceof(cls);
    };

    if (isAnyObject()) return true;

    if (isSubObject()) {
      return tryCls(clsName(), clsNamedType());
    }

    if (isUnresolved()) {
      auto vmClass = val.val().pobj->getVMClass();
      for (auto const& tc : eachClassTypeConstraintInUnion(*this)) {
        if (tryCls(tc.typeName(), tc.typeNamedType())) return true;
        if (isPasses) continue;
        if (checkTypeAliasImpl<isAssert>(tc.m_u.single.class_, vmClass)) return true;
      }
      return false;
    }

    if (isUnion()) {
      return std::ranges::any_of(eachTypeConstraintInUnion(*this), [&](const TypeConstraint& sub) {
        return sub.checkImpl<Mode>(val, context);
      });
    }

    // The constraint is resolved but it's not an object.

    switch (metaType()) {
      case MetaType::This:
        if (isAssert) return true;
        if (isPasses) return false;
        return val.val().pobj->getVMClass() == context;
      case MetaType::Callable:
        assertx(!isProp);
        if (isAssert) return true;
        if (isPasses) return false;
        return is_callable(tvAsCVarRef(*val));
      case MetaType::Nothing:
      case MetaType::NoReturn:
        assertx(!isProp);
        [[fallthrough]];
      case MetaType::Precise:
      case MetaType::Number:
      case MetaType::ArrayKey:
      case MetaType::VecOrDict:
      case MetaType::ArrayLike:
      case MetaType::Classname:
      case MetaType::Class:
      case MetaType::ClassOrClassname:
        return false;
      case MetaType::Nonnull:
        return true;
      case MetaType::Mixed:
        // We assert'd at the top of this function that the
        // metatype cannot be Mixed.
        not_reached();
      case MetaType::SubObject:
      case MetaType::Unresolved:
        // SubObject and Unresolved were handled above.
        not_reached();
    }
    not_reached();
  }

  Optional<AnnotAction> fallback;
  for (auto const& tc : eachTypeConstraintInUnion(*this)) {
    auto const name = tc.isSubObject() ? tc.clsName() : tc.typeName();
    auto const result = annotCompat(val.type(), tc.m_u.single.type, name);
    switch (result) {
      case AnnotAction::Pass: return true;
      case AnnotAction::Fail: break;
      case AnnotAction::CallableCheck:
        assertx(!isProp);
        if (isAssert) return true;
        if (!isPasses && is_callable(tvAsCVarRef(*val))) return true;
        break;
      case AnnotAction::Fallback:
      case AnnotAction::FallbackCoerce:
        assertx(tc.isUnresolved());
        if (!isPasses && tc.checkNamedTypeNonObj<Mode>(val, context)) {
          return true;
        }
        break;
      case AnnotAction::WarnClassToString:
      case AnnotAction::ConvertClassToString:
      case AnnotAction::WarnLazyClassToString:
      case AnnotAction::ConvertLazyClassToString:
        // Defer the action to see if there's a more appropriate action later.
        fallback = fallback ? std::min(*fallback, result) : result;
        continue;
      case AnnotAction::WarnClassname:
        if (!isPasses) {
          assertx(isClassType(val.type()) || isLazyClassType(val.type()));
          assertx(Cfg::Eval::ClassPassesClassname);
          assertx(Cfg::Eval::ClassnameNoticesSampleRate > 0);
          if (isAssert) return true;
          fallback = fallback ? std::min(*fallback, result) : result;
        }
        break;
      case AnnotAction::WarnClass:
        if (!isPasses) {
          assertx(isStringType(val.type()));
          assertx(Cfg::Eval::ClassTypeLevel == 0);
          assertx(Cfg::Eval::ClassNoticesSampleRate > 0);
          if (isAssert) return true;
          fallback = fallback ? std::min(*fallback, result) : result;
        }
        break;
      case AnnotAction::ObjectCheck:
        not_reached();
    }
  }

  switch (fallback.value_or(AnnotAction::Fail)) {
    case AnnotAction::WarnClassToString:
    case AnnotAction::ConvertClassToString:
    case AnnotAction::WarnLazyClassToString:
    case AnnotAction::ConvertLazyClassToString:
      // verify*Fail will deal with the conversion/warning
      return false;
    case AnnotAction::WarnClassname:
      if (folly::Random::oneIn(Cfg::Eval::ClassnameNoticesSampleRate, threadLocalRng64())) {
        raise_notice(
          isInherited()
            ? Strings::CLASS_TO_CLASSNAME_INHERITED
            : Strings::CLASS_TO_CLASSNAME
        );
      }
      return true;
    case AnnotAction::WarnClass:
      if (folly::Random::oneIn(Cfg::Eval::ClassNoticesSampleRate, threadLocalRng64())) {
        raise_notice(
          isInherited()
            ? Strings::STRING_TO_CLASS_INHERITED
            : Strings::STRING_TO_CLASS
        );
      }
      return true;
    default:
      return false;
  }
}

template bool TypeConstraint::checkImpl<TypeConstraint::CheckMode::Exact>(
  tv_rval,
  const Class*
) const;
template bool TypeConstraint::checkImpl<TypeConstraint::CheckMode::ExactProp>(
  tv_rval,
  const Class*
) const;
template bool TypeConstraint::checkImpl<TypeConstraint::CheckMode::AlwaysPasses>(
  tv_rval,
  const Class*
) const;
template bool TypeConstraint::checkImpl<TypeConstraint::CheckMode::Assert>(
  tv_rval,
  const Class*
) const;

bool TypeConstraint::alwaysPasses(const StringData* checkedClsName) const {
  if (!isCheckable()) return true;

  auto const tryCls = [&](const StringData* clsName, const NamedType* ne) {
    // Same name is always a match.
    if (clsName->tsame(checkedClsName)) return true;

    assertx(ne);
    auto const c1 = Class::lookup(checkedClsName);
    auto const c2 = ne->getCachedClass();
    // If both names map to persistent classes we can just check for a subtype
    // relationship.
    return
      c1 && c2 &&
      classHasPersistentRDS(c1) &&
      classHasPersistentRDS(c2) &&
      c1->classof(c2);
  };

  switch (metaType()) {
    case MetaType::This:
    case MetaType::Callable:
    case MetaType::Nothing:
    case MetaType::NoReturn:
    case MetaType::Number:
    case MetaType::ArrayKey:
    case MetaType::VecOrDict:
    case MetaType::ArrayLike:
    case MetaType::Classname:
    case MetaType::Class:
    case MetaType::ClassOrClassname:
      return false;
    case MetaType::Nonnull:
      return true;
    case MetaType::Precise:
      return isAnyObject();
    case MetaType::SubObject:
      return tryCls(clsName(), clsNamedType());
    case MetaType::Unresolved:
      return tryCls(typeName(), typeNamedType());
    case MetaType::Mixed:
      // We check at the top of this function that the metatype cannot be
      // Mixed
      break;
  }
  not_reached();
}

bool TypeConstraint::alwaysPasses(DataType dt) const {
  // Use the StringData* overflow for objects.
  assertx(dt != KindOfObject);
  if (!isCheckable()) return true;

  if (isNullable() && dt == KindOfNull) return true;

  auto const name = isSubObject() ? clsName() : typeName();
  auto const result = isUnion()
    ? AnnotAction::Fallback
    : annotCompat(dt, m_u.single.type, name);
  switch (result) {
    case AnnotAction::Pass:
      return true;
    case AnnotAction::Fail:
    case AnnotAction::Fallback:
    case AnnotAction::FallbackCoerce:
    case AnnotAction::CallableCheck:
    case AnnotAction::ObjectCheck:
    case AnnotAction::WarnClassToString:
    case AnnotAction::ConvertClassToString:
    case AnnotAction::WarnLazyClassToString:
    case AnnotAction::ConvertLazyClassToString:
    case AnnotAction::WarnClassname:
    case AnnotAction::WarnClass:
      return false;
  }
  not_reached();
}

bool TypeConstraint::validForProp() const {
  if (isUnion()) {
    auto const r = eachTypeConstraintInUnion(*this);
    return std::ranges::all_of(
      r,
      [] (const TypeConstraint& tc) { return tc.validForProp(); }
    );
  } else {
    return propSupportsAnnot(m_u.single.type);
  }
}

void TypeConstraint::validForPropResolved(const Class* declCls,
                                          const StringData* propName) const {
  assertx(validForProp());
  if (!isUnresolved()) return;
  auto const r = eachTypeConstraintInUnion(resolvedWithAutoload());
  auto const b = std::ranges::all_of(
    r,
    [] (const TypeConstraint& tc) { return tc.validForProp(); }
  );
  if (!b) validForPropFail(declCls, propName);
}

void TypeConstraint::verifyParam(tv_lval val,
                                 const Class* ctx,
                                 const Func* func,
                                 int paramNum) const {
  if (UNLIKELY(!check(val, ctx))) {
    verifyParamFail(val, ctx, func, paramNum);
  }
}

void TypeConstraint::verifyReturn(TypedValue* tv,
                                  const Class* ctx,
                                  const Func* func) const {
  if (UNLIKELY(!check(tv, ctx))) {
    verifyReturnFail(tv, ctx, func);
  }
}

void TypeConstraint::verifyOutParam(TypedValue* tv,
                                    const Class* ctx,
                                    const Func* func,
                                    int paramNum) const {
  if (UNLIKELY(!check(tv, ctx))) {
    verifyOutParamFail(tv, ctx, func, paramNum);
  }
}

void TypeConstraint::verifyProperty(tv_lval val,
                                    const Class* thisCls,
                                    const Class* declCls,
                                    const StringData* propName) const {
  assertx(Cfg::Eval::CheckPropTypeHints > 0);
  assertx(validForProp());
  if (UNLIKELY(!checkImpl<CheckMode::ExactProp>(val, thisCls))) {
    verifyPropFail(thisCls, declCls, val, propName, false);
  }
}

void TypeConstraint::verifyStaticProperty(tv_lval val,
                                          const Class* thisCls,
                                          const Class* declCls,
                                          const StringData* propName) const {
  assertx(Cfg::Eval::CheckPropTypeHints > 0);
  assertx(validForProp());
  if (UNLIKELY(!checkImpl<CheckMode::ExactProp>(val, thisCls))) {
    verifyPropFail(thisCls, declCls, val, propName, true);
  }
}

void TypeConstraint::verifyReturnNonNull(TypedValue* tv,
                                         const Class* ctx,
                                         const Func* func) const {
  if (UNLIKELY(tvIsNull(tv))) {
    verifyReturnFail(tv, ctx, func);
  } else if (debug) {
    auto vm = &*g_context;
    always_assert_flog(
      check(tv, ctx),
      "HHBBC incorrectly converted RetC All to RetC NonNull in {}:{}",
      vm->getContainingFileName()->data(),
      vm->getLine()
    );
  }
}

std::string describe_actual_type(tv_rval val) {
  // Validate TV to catch possible memory corruptions.
  // We suspect, some rare unexplained typehint notices are caused by memory
  // corruption. This would detect a problem sooner and give us more info to
  // debug.
  always_assert(tvIsPlausible(val.tv()));

  switch (val.type()) {
    case KindOfUninit:        return "undefined variable";
    case KindOfNull:          return "null";
    case KindOfBoolean:       return "bool";
    case KindOfInt64:         return "int";
    case KindOfDouble:        return "float";
    case KindOfPersistentString:
    case KindOfString:        return "string";
    case KindOfPersistentVec:
    case KindOfVec:           {
      return val.val().parr->isLegacyArray() ? "varray" : annotTypeName(AnnotType::Vec);
    }
    case KindOfPersistentDict:
    case KindOfDict:          {
      return val.val().parr->isLegacyArray() ? "darray" : annotTypeName(AnnotType::Dict);
    }
    case KindOfPersistentKeyset:
    case KindOfKeyset:        return annotTypeName(AnnotType::Keyset);
    case KindOfResource: {
      auto pres = val.val().pres;
      // pres should never be null - but sometimes it is anyway so let's guard
      // against that.
      return pres ? pres->data()->o_getClassName().c_str() : "resource(null)";
    }
    case KindOfEnumClassLabel: return "enum class label";
    case KindOfRFunc:         return "reified function";
    case KindOfFunc:          return "func";
    case KindOfClass:         return "class";
    case KindOfLazyClass:        return "lazy class";
    case KindOfClsMeth:       return "clsmeth";
    case KindOfRClsMeth:      return "reified clsmeth";
    case KindOfObject: {
      auto const obj = val.val().pobj;
      auto const cls = obj->getVMClass();
      auto const name = cls->name()->toCppString();
      if (!cls->hasReifiedGenerics()) return name;
      auto const generics = getClsReifiedGenericsProp(cls, obj);
      return fmt::format("{}{}", name, mangleReifiedGenericsName(generics));
    }
  }
  not_reached();
}

bool TypeConstraint::checkStringCompatible() const {
  if (isString() || isArrayKey() || (isUnion() && unionHasString()) ||
      (isSubObject() && interface_supports_string(clsName())) ||
      (isUnresolved() && interface_supports_string(typeName()))) {
    return true;
  }
  if (!isUnresolved()) return false;
  auto p = getNamedTypeWithAutoload(typeNamedType(), typeName());
  return match<bool>(
    p,
    [&](FoundTypeAlias td) {
      for (auto const& tc : eachTypeConstraintInUnion(td.value->value)) {
        auto type = tc.type();
        auto klass = type == AnnotType::SubObject
          ? tc.clsNamedType()->getCachedClass() : nullptr;
        if (type == AnnotType::String ||
            type == AnnotType::ArrayKey ||
            (type == AnnotType::SubObject &&
             interface_supports_string(klass->name()))) {
          return true;
        }
      }
      return false;
    },
    [&](FoundClass c) {
      return isEnum(c.value) && c.value->enumBaseTy().checkStringCompatible();
    },
    [&](NotFound) {
      return false;
    });
}

template <typename F>
bool TypeConstraint::tryCommonCoercions(tv_lval val, const Class* ctx,
                                        const Class* propDecl, F tcInfo) const {
  if (ctx && isThis() && val.type() == KindOfObject) {
    auto const cls = val.val().pobj->getVMClass();
    if (cls->preClass()->userAttributes().count(s___MockClass.get()) &&
        cls->parent() == ctx) {
      return true;
    }
  }

  if ((isClassType(val.type()) || isLazyClassType(val.type())) &&
      checkStringCompatible()) {
    auto const skipConversion = isInherited() &&
      (isSoft() || !Cfg::Eval::ConvertClassToStringForInheritedConstraints);
    if (folly::Random::oneIn(Cfg::Eval::ClassStringHintNoticesSampleRate, threadLocalRng64())) {
      raise_notice(
        skipConversion
          ? Strings::CLASS_TO_STRING_IMPLICIT_INHERITED
          : Strings::CLASS_TO_STRING_IMPLICIT,
        tcInfo().c_str()
      );
    }
    if (skipConversion) return true;
    val.val().pstr = isClassType(val.type()) ?
      const_cast<StringData*>(val.val().pclass->name()) :
      const_cast<StringData*>(val.val().plazyclass.name());
    val.type() = KindOfPersistentString;
    return true;
  }

  return false;
}

bool TypeConstraint::maybeStringCompatible() const {
  return
    isString() || isArrayKey() || isUnresolved() ||
    (isSubObject() && interface_supports_string(clsName()));
}

MaybeDataType TypeConstraint::asSystemlibType(bool isReturn) const {
  // Same gate as tcCheckNative(), select the 8-byte indirect ABI instead of
  // the 16-byte Variant one, previously triggered by misleading non-nullable
  // string return and manually null-checked in cgCallBuiltin/callFuncImpl
  if (isReturn && isNullable()) {
    if (isString()) return KindOfString;
  }
  // Nullable and soft types are generally unknown: don't give an exact type.
  if (isNullable() || isSoft()) return std::nullopt;
  // TODO(T124220067) `noreturn` and `nothing` are their own non-"precise" types
  // under the hood but for systemlib both of these become KindOfNull. The JIT
  // and HPHP::Native::callFunc both expect this.
  if (isNoReturn() || isNothing()) return KindOfNull;
  // This is a kludge that replicates some unfortunate implicit behavior in
  // systemlib: all unresolved types are assumed to be `KindOfObject`.
  if (isUnresolved()) return KindOfObject;
  return underlyingDataType();
}

void TypeConstraint::verifyOutParamFail(TypedValue* c,
                                        const Class* ctx,
                                        const Func* func,
                                        int paramNum) const {
  auto const tcInfo = [&] {
    return fmt::format("argument {} returned from {}() as an inout parameter",
      paramNum+1, func->fullName()->slice());
  };
  if (tryCommonCoercions(c, ctx, nullptr, tcInfo)) return;

  std::string msg = fmt::format(
      "Argument {} returned from {}() as an inout parameter must be {} "
      "{}, {} given",
      paramNum + 1,
      func->fullName()->slice(),
      isUpperBound() ? "upper-bounded by" : "of type",
      displayName(func->cls()),
      describe_actual_type(c)
  );

  if (!isSoft()) {
    raise_return_typehint_error(msg);
  } else {
    raise_warning_unsampled(msg);
  }
}

void TypeConstraint::verifyPropFail(const Class* thisCls,
                                    const Class* declCls,
                                    tv_lval val,
                                    const StringData* propName,
                                    bool isStatic) const {
  assertx(Cfg::Eval::CheckPropTypeHints > 0);
  assertx(validForProp());

  auto const tcInfo = [&]{ return fmt::format("property {}", propName->slice());};
  if (tryCommonCoercions(val, thisCls, declCls, tcInfo)) return;

  raise_property_typehint_error(
    fmt::format(
      "{} '{}::{}' {} type {}, {} assigned",
      isStatic ? "Static property" : "Property",
      declCls->name()->slice(),
      propName->slice(),
      isUpperBound() ? "upper-bounded by" : "declared as",
      displayName(nullptr),
      describe_actual_type(val)
    ),
    isSoft(),
    isUpperBound()
  );
}

void TypeConstraint::validForPropFail(const Class* declCls,
                                      const StringData* propName) const {
  // (Mostly) match the error that HackC produces during parsing.
  raise_error(
    "Invalid property type-hint for '%s::$%s' (via '%s')",
    declCls->name()->data(),
    propName->data(),
    typeName()->data()
  );
}

void TypeConstraint::verifyParamFail(tv_lval c,
                                     const Class* ctx,
                                     const Func* func,
                                     int id) const {
  assertx(id != ReturnId);
  auto const tcInfo = [&] {
    return fmt::format("argument {} passed to {}()", id+1, func->fullName()->slice());
  };
  if (tryCommonCoercions(c, ctx, nullptr, tcInfo)) return;

  std::string name = displayName(func->cls());
  auto const givenType = describe_actual_type(c);

  // Handle parameter type constraint failures
  if (isSoft()) {
    // Soft type hints raise warnings instead of recoverable errors
    raise_warning_unsampled(
      fmt::format(
        "Argument {} to {}() must be {} {}, {} given",
        id + 1, func->fullName()->slice(),
        isUpperBound() ? "upper-bounded by" : "of type",
        name, givenType
      )
    );
  } else if (isDisplayNullable()) {
    raise_typehint_error(
      fmt::format(
        "Argument {} to {}() must be {} {}, {} given",
        id + 1, func->fullName()->slice(),
        isUpperBound() ? "upper-bounded by" : "of type",
        name, givenType
      )
    );
  } else {
    auto cls = isUnion() ? nullptr : Class::lookup(typeName());
    if (cls && isInterface(cls)) {
      auto const msg =
        fmt::format(
          "Argument {} passed to {}() must {} interface {}, {} given",
          id + 1, func->fullName()->slice(),
          isUpperBound() ? "be upper-bounded by" : "implement",
          name, givenType
        );
      raise_typehint_error(msg);
    } else {
      auto const msg =
        fmt::format(
          "Argument {} passed to {}() must be {} {}, {} given",
          id + 1, func->fullName()->slice(),
          isUpperBound() ? "upper-bounded by" : "an instance of",
          name, givenType
        );
      raise_typehint_error(msg);
    }
  }

  assertx(isSoft() || isThis() || check(c, ctx));
}

void TypeConstraint::verifyReturnFail(tv_lval c, const Class* ctx,
                                      const Func* func) const {
  auto const tcInfo = [&] {
    return fmt::format("return of {}()", func->fullName()->slice());
  };
  if (tryCommonCoercions(c, ctx, nullptr, tcInfo)) return;

  std::string name = displayName(func->cls());
  auto const givenType = describe_actual_type(c);

  std::string msg;
  if (func->isClosureBody()) {
    msg =
      fmt::format(
        "Value returned from {}closure must be of type {}, {} given",
        func->isAsync() ? "async " : "",
        name,
        givenType
      );
  } else {
    msg =
      fmt::format(
        "Value returned from {}{} {}() must be {}{} {}, {} given",
        func->isAsync() ? "async " : "",
        func->preClass() ? "method" : "function",
        func->fullName()->slice(),
        isUpperBound() ? "upper-bounded by" : "of",
        isInherited() ? " inherited type" : (isUpperBound() ? "" : " type"),
        name,
        givenType
      );
  }
  if (!isSoft()) {
    raise_return_typehint_error(msg);
  } else {
    raise_warning_unsampled(msg);
  }
}

//////////////////////////////////////////////////////////////////////

void TypeConstraint::resolveType(AnnotType t,
                                 bool nullable,
                                 PackedStringPtr clsName) {
  if (isUnion()) {
    not_implemented(); // TODO(T151885113)
  }
  assertx(m_u.single.type == AnnotType::Unresolved);
  assertx(t != AnnotType::Unresolved);
  assertx((t == AnnotType::SubObject) == (clsName != nullptr));
  m_flags |= TypeConstraintFlags::Resolved;
  if (nullable) m_flags |= TypeConstraintFlags::Nullable;
  m_u.single.type = t;
  m_u.single.class_.m_clsName = clsName;
}

void TypeConstraint::unresolve() {
  if (isUnion()) {
    not_implemented(); // TODO(T151885113)
  }
  m_flags &= ~TypeConstraintFlags::Resolved;
  m_u.single.type = AnnotType::Unresolved;
  m_u.single.class_.m_clsName.reset();
}

MemoKeyConstraint TypeConstraint::getMemoKeyConstraint() const {
  using MK = MemoKeyConstraint;

  // Soft constraints aren't useful because they're not enforced.
  if (!hasConstraint() || isTypeVar() ||
      isTypeConstant() || isSoft() || isUnion()) {
    return MK::Any;
  }

  // Only a subset of possible type-constraints are useful to use. Namely,
  // single types which might be nullable, and int/string combination.
  switch (metaType()) {
    case AnnotMetaType::Precise: {
      auto const dt = underlyingDataType();
      if (!dt) return MK::Any;
      switch (*dt) {
        case KindOfBoolean:
          return isNullable() ? MK::BoolOrNull : MK::Bool;
        case KindOfInt64:
          return isNullable() ? MK::IntOrNull : MK::Int;
        case KindOfPersistentString:
        case KindOfString:
        case KindOfLazyClass:
          return isNullable() ? MK::StrOrNull : MK::Str;
        case KindOfObject:
          return isNullable() ? MK::ObjectOrNull : MK::Object;
        case KindOfDouble:
          return isNullable() ? MK::DblOrNull : MK::Dbl;
        case KindOfPersistentVec:
        case KindOfVec:
        case KindOfPersistentDict:
        case KindOfDict:
        case KindOfPersistentKeyset:
        case KindOfKeyset:
        case KindOfClsMeth:
        case KindOfResource:
        case KindOfEnumClassLabel: return MK::Any;
        case KindOfNull:         return MK::Null;
        case KindOfUninit:
        case KindOfRFunc:
        case KindOfFunc:
        case KindOfRClsMeth:
        case KindOfClass:
          always_assert_flog(false, "Unexpected DataType");
      }
      not_reached();
    }
    case AnnotMetaType::ArrayKey:
      return isNullable() ? MK::IntOrStrOrNull : MK::IntOrStr;
    case AnnotMetaType::Classname:
      return isNullable() ? MK::StrOrNull : MK::Str;
    case AnnotMetaType::Class:
      return isNullable() ? MK::StrOrNull : MK::Str;
    case AnnotMetaType::ClassOrClassname:
      return isNullable() ? MK::StrOrNull : MK::Str;
    case AnnotMetaType::SubObject:
      return isNullable() ? MK::ObjectOrNull : MK::Object;
    case AnnotMetaType::Nonnull:
      return isNullable() ? MK::Any : MK::NonNull;
    case AnnotMetaType::Mixed:
    case AnnotMetaType::Nothing:
    case AnnotMetaType::NoReturn:
    case AnnotMetaType::This:
    case AnnotMetaType::Callable:
    case AnnotMetaType::Number:
    case AnnotMetaType::VecOrDict:
    case AnnotMetaType::ArrayLike:
    case AnnotMetaType::Unresolved:
      return MK::Any;
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

bool tcCouldBeReified(const Func* func, uint32_t paramId) {
  auto const& tcs = paramId == TypeConstraint::ReturnId
    ? func->returnTypeConstraints()
    : func->params()[paramId].typeConstraints;
  auto const isReifiedGenericInClosure = [&] {
    if (!func->isClosureBody()) return false;
    auto const lNames = func->localNames();
    auto const nParams = func->numParams();
    auto const nDeclaredProps = func->baseCls()->numDeclProperties();
    assertx(nParams + nDeclaredProps <= func->numNamedLocals());
    // Skip over params
    for (int i = nParams; i < nParams + nDeclaredProps; ++i) {
      assertx(lNames[i]);  // Closures can't have names removed.
      if (isMangledReifiedGenericInClosure(lNames[i])) return true;
    }
    return false;
  };
  return tcs.isTypeVar() &&
         (func->hasReifiedGenerics() ||
          (func->cls() && func->cls()->hasReifiedGenerics()) ||
          isReifiedGenericInClosure());
}

//////////////////////////////////////////////////////////////////////
void applyFlagsToUB(TypeConstraint& ub, const TypeConstraint& tc) {
  auto const tcFlags = tc.flags() & ~TypeConstraintFlags::TypeVar;
  ub.addFlags(tcFlags);
}

//////////////////////////////////////////////////////////////////////

void TcUnionPieceIterator::buildUnionTypeConstraint() {
  using CC = ClassConstraint;

  auto flags = m_flags & (TypeConstraintFlags::Nullable
                          | TypeConstraintFlags::TypeVar
                          | TypeConstraintFlags::Soft
                          | TypeConstraintFlags::TypeConstant
                          | TypeConstraintFlags::DisplayNullable
                          | TypeConstraintFlags::UpperBound
                          | TypeConstraintFlags::Resolved);

  // isolate the low bit
  switch (m_mask & -m_mask) {
    case TypeConstraint::kUnionTypeBool: {
      m_outTc = TypeConstraint{ AnnotType::Bool, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Bool))} };
      break;
    }
    case TypeConstraint::kUnionTypeCallable: {
      m_outTc = TypeConstraint{ AnnotType::Callable, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Callable))} };
      break;
    }
    case TypeConstraint::kUnionTypeDict: {
      m_outTc = TypeConstraint{ AnnotType::Dict, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Dict))} };
      break;
    }
    case TypeConstraint::kUnionTypeFloat: {
      m_outTc = TypeConstraint{ AnnotType::Float, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Float))} };
      break;
    }
    case TypeConstraint::kUnionTypeInt: {
      m_outTc = TypeConstraint{ AnnotType::Int, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Int))} };
      break;
    }
    case TypeConstraint::kUnionTypeKeyset: {
      m_outTc = TypeConstraint{ AnnotType::Keyset, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Keyset))} };
      break;
    }
    case TypeConstraint::kUnionTypeObject: {
      m_outTc = TypeConstraint{ AnnotType::Object, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Object))} };
      break;
    }
    case TypeConstraint::kUnionTypeResource: {
      m_outTc = TypeConstraint{ AnnotType::Resource, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Resource))} };
      break;
    }
    case TypeConstraint::kUnionTypeString: {
      m_outTc = TypeConstraint{ AnnotType::String, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::String))} };
      break;
    }
    case TypeConstraint::kUnionTypeThis: {
      m_outTc = TypeConstraint{ AnnotType::This, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::This))} };
      break;
    }
    case TypeConstraint::kUnionTypeVec: {
      m_outTc = TypeConstraint{ AnnotType::Vec, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Vec))} };
      break;
    }
    case TypeConstraint::kUnionTypeClassname: {
      m_outTc = TypeConstraint{ AnnotType::Classname, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Classname))} };
      break;
    }
    case TypeConstraint::kUnionTypeClass: {
      m_outTc = TypeConstraint{ AnnotType::Class, flags, CC{LAZY_STATIC_STRING(annotTypeName(AnnotType::Class))} };
      break;
    }

    case TypeConstraint::kUnionTypeSubObject: {
      bool resolved = contains(flags, TypeConstraintFlags::Resolved);
      AnnotType type = resolved ? AnnotType::SubObject : AnnotType::Unresolved;
      m_outTc = TypeConstraint{ type, flags, m_classes->m_list[m_nextClass] };
      break;
    }

    default:
      not_reached();
  }
}

TcUnionPieceIterator& TcUnionPieceIterator::operator++() {
  if (m_mask == TypeConstraint::kUnionTypeSubObject) {
    ++m_nextClass;
    if (m_nextClass == m_classes->m_list.size()) {
      // We're done so signal the end.
      m_mask = 0;
      m_nextClass = 0;
    }
  } else {
    // turn off the lowest bit
    m_mask &= ~-m_mask;
  }

  if (m_mask) {
    buildUnionTypeConstraint();
  }

  return *this;
}

TcUnionPieceIterator TcUnionPieceIterator::operator++(int) {
  auto tmp = *this;
  this->operator++();
  return tmp;
}

bool TcUnionPieceIterator::at_end() const {
  return m_mask == 0;
}

TcUnionPieceIterator TcUnionPieceView::begin() const {
  TcUnionPieceIterator it;
  it.m_flags = m_tc.m_flags;
  it.m_classes = nullptr;
  it.m_nextClass = 0;

  if (m_tc.isUnion()) {
    it.m_mask = m_tc.m_u.union_.m_mask;
    switch (m_kind) {
      case Kind::All: break;
      case Kind::ClassesOnly: {
        it.m_mask &= TypeConstraint::kUnionTypeSubObject;
      }
    }

    it.m_classes = m_tc.m_u.union_.m_classes;
    if (it.m_mask) {
      it.buildUnionTypeConstraint();
    }
  } else {
    it.m_outTc = m_tc;

    // just need a single bit to indicate non-end.
    it.m_mask = 1;

    switch (m_kind) {
      case Kind::All:
        break;
      case Kind::ClassesOnly: {
        switch (m_tc.m_u.single.type) {
          case AnnotType::SubObject:
          case AnnotType::Unresolved:
            break;
          default:
            // nothing to do
            it.m_mask = 0;
            break;
        }
        break;
      }
    }
  }
  return it;
}

TcUnionPieceIterator TcUnionPieceView::end() const {
  TcUnionPieceIterator it;
  it.m_classes = nullptr;
  it.m_flags = TypeConstraintFlags::NoFlags;
  it.m_mask = 0;
  it.m_nextClass = 0;
  return it;
}

bool TcUnionPieceIterator::operator==(const TcUnionPieceIterator& o) const {
  return (m_mask == o.m_mask) && (m_nextClass == o.m_nextClass);
}

}
