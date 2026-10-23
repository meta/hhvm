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

#include "hphp/runtime/vm/jit/type.h"

#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/prof-data-sb.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"

#include "hphp/util/abi-cxx.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/text-util.h"
#include "hphp/util/trace.h"

#include <boost/algorithm/string/trim.hpp>
#include <fmt/format.h>

#include <folly/Conv.h>
#include <folly/MapUtil.h>

#include <vector>

namespace HPHP::jit {

TRACE_SET_MOD(hhir)

///////////////////////////////////////////////////////////////////////////////


#define IRT(name, ...)       constexpr Type::bits_t Type::k##name;
#define IRTP(name, ...)
#define IRTL(name, ...)
#define IRTM(name, ...)
#define IRTX(name, ...)
  IR_TYPES
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX

constexpr Type::bits_t Type::kPtr;
constexpr Type::bits_t Type::kLval;
constexpr Type::bits_t Type::kMem;

constexpr Type::bits_t Type::kArrSpecBits;
constexpr Type::bits_t Type::kClsSpecBits;

///////////////////////////////////////////////////////////////////////////////
// Vanilla array-spec manipulation.

Type Type::narrowToVanilla() const {
  return narrowToLayout(ArrayLayout::Vanilla());
}

Type Type::narrowToLayout(ArrayLayout layout) const {
  if (!supports(SpecKind::Array)) return *this;
  if (supports(SpecKind::Class)) return *this;
  auto const spec = arrSpec().narrowToLayout(layout);
  if (spec == ArraySpec::Bottom()) return TBottom;
  return Type(*this, spec);
}

///////////////////////////////////////////////////////////////////////////////

const ArrayData* Type::arrLikeVal() const {
  assertx(hasConstVal(TArrLike));
  assertx(subtypeOfAny(TVec, TDict, TKeyset));
  return reinterpret_cast<const ArrayData*>(m_extra);
}

std::string Type::constValString() const {
  if (*this <= TBottom)   return "Bottom";
  if (*this <= TUninit)   return "Uninit";
  if (*this <= TInitNull) return "InitNull";
  if (*this <= TNullptr)  return "Nullptr";

  assertx(hasConstVal());

  if (*this <= TBool) {
    return m_boolVal ? "true" : "false";
  }
  if (*this <= TInt) {
    return fmt::format("{}", m_intVal);
  }
  if (*this <= TDbl) {
    // don't format doubles as integers.
    auto s = fmt::format("{}", m_dblVal);
    if (!strchr(s.c_str(), '.') && !strchr(s.c_str(), 'e')) {
      return fmt::format("{:.1f}", m_dblVal);
    }
    return s;
  }
  if (*this <= TStaticStr) {
    auto str = m_strVal;
    return fmt::format("\"{}\"", escapeStringForCPP(str->data(),
                                                      str->size()));
  }
  if (*this <= TArrLike) {
    auto const type = getDataTypeString(m_arrVal->toDataType());
    auto const layout = arrSpec().layout().describe();
    return m_arrVal->empty()
      ? fmt::format("{}[]={}", type, layout)
      : fmt::format("{}({})={}", type, fmt::ptr(m_arrVal), layout);
  }
  if (*this <= TFunc) {
    return fmt::format("Func({})", m_funcVal->fullName()->data());
  }
  if (*this <= TCls) {
    return fmt::format("Cls({})", m_clsVal->name()->data());
  }
  if (*this <= TLazyCls) {
    return fmt::format("LazyCls({})", m_lclsVal.name()->data());
  }
  if (*this <= TClsMeth) {
    return fmt::format("ClsMeth({},{})",
      m_clsmethVal->getCls() ?
      m_clsmethVal->getCls()->name()->data() : "nullptr",
      m_clsmethVal->getFunc() ?
      m_clsmethVal->getFunc()->fullName()->data() : "nullptr"
    );
  }
  if (*this <= TEnumClassLabel) {
    return fmt::format("EnumClassLabel({})", m_strVal->data());
  }
  if (*this <= TTCA) {
    auto name = getNativeFunctionName(m_tcaVal);
    const char* hphp = "HPHP::";

    if (!name.compare(0, strlen(hphp), hphp)) {
      name = name.substr(strlen(hphp));
    }
    auto pos = name.find_first_of('(');
    if (pos != std::string::npos) {
      name = name.substr(0, pos);
    }
    return fmt::format("TCA: {}({})", fmt::ptr(m_tcaVal), boost::trim_copy(name));
  }
  if (*this <= TVoidPtr) {
    return fmt::format("VoidPtr({})", m_voidPtrVal);
  }
  if (*this <= TRDSHandle) {
    return fmt::format("rds::Handle({:#x})", m_rdsHandleVal);
  }
  if (*this <= TPtr) {
    return fmt::format("TV: {}", fmt::ptr(m_ptrVal));
  }
  if (*this <= TLval) {
    return fmt::format("Lval: {}", fmt::ptr(m_ptrVal));
  }
  if (*this <= TMem) {
    return fmt::format("Mem: {}", fmt::ptr(m_ptrVal));
  }

  always_assert_flog(
    false,
    "Bad type in constValString(): {}:{}:{}:{:#16x}",
    m_bits.hexStr(),
    static_cast<ptr_location_t>(m_ptr),
    m_hasConstVal,
    m_extra
  );
}

static std::string show(PtrLocation ptr) {
  always_assert(ptr <= PtrLocation::All);

  switch (ptr) {
    case PtrLocation::Bottom:
    case PtrLocation::All:    not_reached();
#define PTRT(name, ...) case PtrLocation::name: return #name;
    PTR_LOCATION_TYPES(PTRT)
#undef PTRT
  }

  std::vector<const char*> parts;
#define PTRT(name, ...) \
  if (PtrLocation::name <= ptr) parts.emplace_back(#name);
  PTR_LOCATION_PRIMITIVE(PTRT)
#undef PTRT
  return fmt::format("{{{}}}", folly::join('|', parts));
}

static const jit::fast_map<Type, const char*> s_typeNames{
#define IRT(x, ...) {T##x, #x},
#define IRTP IRT
#define IRTL IRT
#define IRTM IRT
#define IRTX IRT
  IR_TYPES
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX
};

std::string Type::toString() const {
  // First, see if this is a predefined type.
  auto it = s_typeNames.find(*this);
  if (it != s_typeNames.end()) return it->second;

  if (maybe(TNullptr)) {
    return folly::to<std::string>(
      "Nullptr|",
      (*this - TNullptr).toString()
    );
  }

  if (m_hasConstVal) {
    if (*this <= TCls) {
      return fmt::format("Cls={}", m_clsVal->name()->data());
    }
    if (*this <= TLazyCls) {
      return fmt::format("LazyCls={}", m_lclsVal.name()->data());
    }
    if (*this <= TEnumClassLabel) {
      return fmt::format("EnumClassLabel={}", m_strVal->data());
    }
    return fmt::format("{}<{}>",
                          dropConstVal().toString(), constValString());
  }

  auto t = *this;

  if (t.maybe(TMem)) {
    assertx(!t.m_hasConstVal);
    auto const kind = t.ptrLocation();
    assertx(kind != PtrLocation::Bottom);
    auto ret = [&] () -> std::string {
      if (t.maybe(TPtr) && t.maybe(TLval)) {
        if (kind == PtrLocation::All) return "Mem";
        return "MemTo" + show(kind);
      }
      if (t.maybe(TPtr)) {
        if (kind == PtrLocation::All) return "Ptr";
        return "PtrTo" + show(kind);
      }
      assertx(t.maybe(TLval));
      if (kind == PtrLocation::All) return "Lval";
      return "LvalTo" + show(kind);
    }();
    t -= TMem;
    if (t != TBottom) ret += "|" + t.toString();
    return ret;
  }

  assertx(t.ptrLocation() == PtrLocation::Bottom);

  std::vector<std::string> parts;
  if (isSpecialized()) {
    if (auto const cls = t.clsSpec()) {
      auto const base = Type{m_bits & kClsSpecBits, t.ptrLocation()};
      parts.push_back(folly::to<std::string>(base.toString(), cls.toString()));
      t -= base;
    } else if (auto const arr = t.arrSpec()) {
      auto const base = Type{m_bits & kArrSpecBits, t.ptrLocation()};
      parts.push_back(folly::to<std::string>(base.toString(), arr.toString()));
      t -= base;
    } else {
      always_assert(false);
    }
  }

  // Sort all types by decreasing number of bits in their representation. This
  // ensures that larger unions come first.
  static auto const sortedTypes = []{
    std::vector<std::pair<Type, const char*>> types{
#define IRT(x, ...) {T##x, #x},
#define IRTP IRT
#define IRTL IRT
#define IRTM IRT
#define IRTX IRT
      IR_TYPES
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX
    };
    std::sort(
      types.begin(), types.end(),
      [](const std::pair<Type, const char*>& a,
         const std::pair<Type, const char*>& b) {
        auto const pop1 = a.first.m_bits.count();
        auto const pop2 = b.first.m_bits.count();
        if (pop1 != pop2) return pop1 > pop2;
        return std::strcmp(a.second, b.second) < 0;
      }
    );
    // Remove Bottom
    while (types.back().first.m_bits == kBottom) types.pop_back();
    return types;
  }();

  // Decompose the type into a union of pre-defined types. Since we've sorted
  // the type list in decreasing size, this means the decomposition will be
  // minimal.
  for (auto const& t2 : sortedTypes) {
    if (t <= TBottom) break;
    if (t2.first <= t) {
      parts.push_back(t2.second);
      t -= t2.first;
    }
  }

  assertx(!parts.empty());
  if (parts.size() == 1) return parts.front();
  return fmt::format("{{{}}}", folly::join('|', parts));
}

std::string Type::debugString(Type t) {
  return t.toString();
}

namespace {
enum TypeKey : uint8_t {
  None,
  Const,
  ClsSub,
  ClsExact,
  ArrSpec,
};

TypeKey getTypeKey(const Type& t) {
  return t.hasConstVal() ? TypeKey::Const :
    t.clsSpec() ? (t.clsSpec().exact() ? TypeKey::ClsExact : TypeKey::ClsSub) :
    t.arrSpec() ? TypeKey::ArrSpec : TypeKey::None;
}
}

void Type::serialize(ProfDataSerializer& ser) const {
  SCOPE_EXIT {
    ITRACE_MOD(Trace::hhbc, 2, "Type: {}\n", toString());
  };
  ITRACE_MOD(Trace::hhbc, 2, "Type>\n");
  Trace::Indent _;

  write_raw(ser, m_bits);
  if (m_bits & kMem) write_raw(ser, m_ptr);

  Type t = *this;
  if (t.maybe(TNullptr)) t = t - TNullptr;

  auto const key = getTypeKey(t);
  write_raw(ser, key);

  if (key == TypeKey::Const) {
    if (t <= TCls)       return write_class(ser, t.m_clsVal);
    if (t <= TLazyCls)   return write_lclass(ser, t.m_lclsVal);
    if (t <= TFunc)      return write_func(ser, t.m_funcVal);
    if (t <= TStaticStr || t <= TEnumClassLabel) {
      return write_string(ser, t.m_strVal);
    }
    if (t < TArrLike) {
      return write_array(ser, t.m_arrVal);
    }
    if (t <= TClsMeth) {
      return write_clsmeth(ser, t.m_clsmethVal);
    }
    assertx(t.subtypeOfAny(TBool, TInt, TDbl));
    return write_raw(ser, t.m_extra);
  }

  if (key == TypeKey::ClsSub || key == TypeKey::ClsExact) {
    return write_class(ser, t.clsSpec().cls());
  }
  if (key == TypeKey::ArrSpec) {
    auto const arrSpec = t.arrSpec();
    write_layout(ser, arrSpec.layout());
    return write_array_rat(ser, arrSpec.type());
  }
}

Type Type::deserialize(ProfDataDeserializer& ser) {
  ITRACE_MOD(Trace::hhbc, 2, "Type>\n");
  auto const ret = [&] {
    Trace::Indent _;
    Type t{};

    read_raw(ser, t.m_bits);
    if (t.m_bits & kMem) read_raw(ser, t.m_ptr);

    auto const key = read_raw<TypeKey>(ser);
    if (key == TypeKey::Const) {
      t.m_hasConstVal = true;
      if (t <= TCls) {
        t.m_clsVal = read_class(ser);
        return t;
      }
      if (t <= TLazyCls) {
        t.m_lclsVal = read_lclass(ser);
        return t;
      }
      if (t <= TFunc) {
        t.m_funcVal = read_func(ser);
        return t;
      }
      if (t <= TStaticStr || t <= TEnumClassLabel) {
        t.m_strVal = read_string(ser);
        return t;
      }
      if (t < TArrLike) {
        t.m_arrVal = read_array(ser);
        return t;
      }
      if (t <= TClsMeth) {
        t.m_clsmethVal = read_clsmeth(ser);
        return t;
      }
      read_raw(ser, t.m_extra);
      return t;
    }

    t.m_hasConstVal = false;
    if (key == TypeKey::None) return t;
    if (key == TypeKey::ClsSub || key == TypeKey::ClsExact) {
      auto const cls = read_class(ser);
      if (key == TypeKey::ClsExact) {
        t.m_clsSpec = ClassSpec{cls, ClassSpec::ExactTag{}};
      } else {
        t.m_clsSpec = ClassSpec{cls, ClassSpec::SubTag{}};
      }
    } else {
      assertx(key == TypeKey::ArrSpec);
      auto const layout = read_layout(ser);
      auto const type = read_array_rat(ser);
      t.m_arrSpec = ArraySpec{layout, type};
    }
    return t;
  }();
  ITRACE_MOD(Trace::hhbc, 2, "Type: {}\n", ret.toString());
  return ret;
}

size_t Type::stableHash() const {
  // Base hash
  auto const hash = hash_int64_pair(
    m_bits.hash(),
    static_cast<uint64_t>(m_ptr) ^ m_hasConstVal
  );

  // Specialization data
  Type t = *this;
  auto const key = getTypeKey(t);
  auto const extra = [&] () -> size_t {
    if (key == TypeKey::Const) {
      if (t <= TCls) return t.m_clsVal->stableHash();
      if (t <= TLazyCls) return t.m_lclsVal.name()->hashStatic();
      if (t <= TFunc) return t.m_funcVal->stableHash();
      if (t <= TStaticStr || t <= TEnumClassLabel) {
        return t.m_strVal->hashStatic();
      }
      if (t < TArrLike) {
        return internal_serialize(
          VarNR(const_cast<ArrayData*>(t.m_arrVal))
        ).get()->hash();
      }
      if (t <= TClsMeth) {
        auto const cls = t.m_clsmethVal->getCls();
        auto const func = t.m_clsmethVal->getFunc();
        return (cls ? cls->stableHash() : 0 ) ^ (func ? func->stableHash() : 0);
      }
      if (t.subtypeOfAny(TBool, TInt, TDbl)) return t.m_extra;
    }
    if (key == TypeKey::ClsSub || key == TypeKey::ClsExact) {
      auto const cls = t.clsSpec().cls();
      return cls ? cls->stableHash() : 0;
    }
    if (key == TypeKey::ArrSpec) {
      auto const spec = t.arrSpec();
      return folly::hash::hash_combine(
        spec.layout().toUint16(),
        spec.type() ? spec.type()->stableHash() : 0
      );
    }
    return 0;
  }();

  return folly::hash::hash_combine(
    hash,
    key,
    extra
  );
}

Type::bits_t Type::rawBits() const {
  return m_bits;
}

///////////////////////////////////////////////////////////////////////////////

bool Type::checkValid() const {
  // NOTE: Be careful: the TFoo objects aren't all constructed yet in this
  // function, and we can't call operator<=, etc. because they call checkValid.
  auto constexpr kNonNullConstVals = kArrLike | kCls | kLazyCls |
                                     kFunc | kStr | kEnumClassLabel;
  if (m_hasConstVal && ((m_bits & kNonNullConstVals) == m_bits)) {
    assert_flog(m_extra, "Null constant type: {}", m_bits.hexStr());
  }
  if (m_extra) {
    assert_flog(((m_bits & kClsSpecBits) == kBottom ||
                 (m_bits & kArrSpecBits) == kBottom) &&
                "Conflicting specialization: {}", "{}", m_bits.hexStr());
  }

  // We should have one canonical representation of Bottom.
  if (m_bits == kBottom) {
    assert_flog(*this == TBottom,
                "Bottom m_bits but nonzero others in {}:{}:{}:{:#16x}",
                m_bits.hexStr(), m_ptrVal, m_hasConstVal, m_extra);
  }

  // We should have a non-empty set of ptr locations iff any of the
  // kMem bits are set.
  assertx(bool(m_bits & kMem) == (m_ptr != PtrLocation::Bottom));

  return true;
}

Type::bits_t Type::bitsFromDataType(DataType outer) {
  switch (outer) {
    case KindOfUninit           : return kUninit;
    case KindOfNull             : return kInitNull;
    case KindOfBoolean          : return kBool;
    case KindOfInt64            : return kInt;
    case KindOfDouble           : return kDbl;
    case KindOfPersistentString : return kPersistentStr;
    case KindOfString           : return kStr;

    case KindOfPersistentVec    : return kPersistentVec;
    case KindOfPersistentDict   : return kPersistentDict;
    case KindOfPersistentKeyset : return kPersistentKeyset;

    case KindOfVec              : return kVec;
    case KindOfDict             : return kDict;
    case KindOfKeyset           : return kKeyset;

    case KindOfResource         : return kRes;
    case KindOfObject           : return kObj;
    case KindOfRFunc            : return kRFunc;
    case KindOfFunc             : return kFunc;
    case KindOfClass            : return kCls;
    case KindOfLazyClass        : return kLazyCls;
    case KindOfClsMeth          : return kClsMeth;
    case KindOfRClsMeth         : return kRClsMeth;

    case KindOfEnumClassLabel   : return kEnumClassLabel;
  }
  not_reached();
}

DataType Type::toDataType() const {
  assertx(!maybe(TMem) || m_bits == kBottom);
  assertx(isKnownDataType());

  // Order is important here: types must progress from more specific
  // to less specific to return the most specific DataType.
  if (*this <= TUninit)      return KindOfUninit;
  if (*this <= TInitNull)    return KindOfNull;
  if (*this <= TBool)        return KindOfBoolean;
  if (*this <= TInt)         return KindOfInt64;
  if (*this <= TDbl)         return KindOfDouble;
  if (*this <= TPersistentStr) return KindOfPersistentString;
  if (*this <= TStr)         return KindOfString;
  if (*this <= TPersistentVec) return KindOfPersistentVec;
  if (*this <= TVec)         return KindOfVec;
  if (*this <= TPersistentDict) return KindOfPersistentDict;
  if (*this <= TDict)        return KindOfDict;
  if (*this <= TPersistentKeyset) return KindOfPersistentKeyset;
  if (*this <= TKeyset)      return KindOfKeyset;
  if (*this <= TObj)         return KindOfObject;
  if (*this <= TRes)         return KindOfResource;
  if (*this <= TFunc)        return KindOfFunc;
  if (*this <= TCls)         return KindOfClass;
  if (*this <= TLazyCls)     return KindOfLazyClass;
  if (*this <= TClsMeth)     return KindOfClsMeth;
  if (*this <= TRFunc)       return KindOfRFunc;
  if (*this <= TRClsMeth)    return KindOfRClsMeth;
  if (*this <= TEnumClassLabel) return KindOfEnumClassLabel;
  always_assert_flog(false, "Bad Type {} in Type::toDataType()", *this);
}

Optional<TypedValue> Type::tv() const {
  if (!(admitsSingleVal() && *this <= TCell)) return std::nullopt;
  TypedValue result;
  result.m_type = toDataType();
  if (hasConstVal()) result.m_data.num = rawVal();
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Combinators.

Type Type::specialize(TypeSpec spec) const {
  assertx(!spec.arrSpec() || supports(SpecKind::Array));
  assertx(!spec.clsSpec() || supports(SpecKind::Class));

  // If we don't have exactly one kind of specialization, or if our bits
  // support more than one kinds, don't specialize.
  if ((bool)spec.arrSpec() + (bool)spec.clsSpec() != 1) {
    return *this;
  }
  if (supports(SpecKind::Array) + supports(SpecKind::Class) > 1) return *this;

  if (spec.arrSpec() != ArraySpec::Bottom()) {
    return Type{*this, spec.arrSpec()};
  }

  assertx(spec.clsSpec() != ClassSpec::Bottom());
  return Type{*this, spec.clsSpec()};
}

Type Type::modified() const {
  auto t = unspecialize();
  if (t.maybe(TVec))    t |= TVec;
  if (t.maybe(TDict))   t |= TDict;
  if (t.maybe(TKeyset)) t |= TKeyset;
  if (t.maybe(TStr))    t |= TStr;
  auto const spec = ArraySpec(ArrayLayout::Vanilla());
  return arrSpec().vanilla() ? Type(t, spec) : t;
}

Type Type::refine(Type refinement) const {
  auto original = *this;

  // While ideally we would intersect the source and parameter types in all
  // cases but when we have a RAT we fallback to a selection heuristic that may
  // widen the type. If we have two RAT types we need to assume that the type
  // being asserted is the one we want to keep. Generally when we assert a RAT
  // it is because HHBBC statically determined that that information would be
  // valuable when doing its analysis.
  if (Cfg::Jit::PreferRefinedRATArraySpecializations &&
      original.arrSpec().type() && refinement.arrSpec().type()) {
    original = original
      .unspecialize()
      .narrowToLayout(original.arrSpec().layout());
  }
  return refinement & original;
}


/*
 * Return true if the array satisfies requirement on the ArraySpec.
 * If the kind and RepoAuthType are both set, the array must match both.
 */
static bool arrayFitsSpec(const ArrayData* arr, ArraySpec spec) {
  if (spec == ArraySpec::Top()) return true;
  if (spec == ArraySpec::Bottom()) return false;

  if (!(ArrayLayout::FromArray(arr) <= spec.layout())) {
    return false;
  }

  if (!spec.type()) return true;

  using A = RepoAuthType::Array;
  auto const& type = *spec.type();
  if (arr->empty()) return type.emptiness() != A::Empty::No;

  // Right now, the only non-trivial RepoAuthType::Array we support is
  // for arrays with "vector" keys (0, 1, ... n - 1). They don't have
  // to be PackedArray.
  if (!arr->isVectorData()) return false;
  switch (type.tag()) {
    case A::Tag::Tuple:
      if (arr->size() != type.size()) return false;
      [[fallthrough]];
    case A::Tag::Packed: {
      int64_t k = 0;
      auto const tuple = type.tag() == A::Tag::Tuple;
      for ( ; k < arr->size(); ++k) {
        auto const elemType = tuple ? type.tupleElem(k) : type.packedElems();
        if (!tvMatchesRepoAuthType(arr->get(k), elemType)) {
          return false;
        }
      }
      return true;
    }
  }

  always_assert(false);
}

bool Type::operator<=(Type rhs) const {
  auto const lhs = *this;

  // Check for any members in lhs.m_bits that aren't in rhs.m_bits.
  if ((lhs.m_bits & rhs.m_bits) != lhs.m_bits) {
    return false;
  }

  // Check for Bottom; all the remaining cases assume `lhs' is not Bottom.
  if (lhs.m_bits == kBottom) return true;

  // If `rhs' is a constant, we must be the same constant.
  if (rhs.m_hasConstVal) {
    assertx(!rhs.isUnion());
    return lhs.m_hasConstVal && lhs.m_extra == rhs.m_extra;
  }

  // Make sure lhs's ptr and mem kinds are subtypes of rhs's.
  if (!(lhs.ptrLocation() <= rhs.ptrLocation())) return false;

  // Compare specializations only if `rhs' is specialized.
  if (!rhs.isSpecialized()) {
    return true;
  }
  if (lhs.hasConstVal(TArrLike)) {
    return arrayFitsSpec(lhs.m_arrVal, rhs.arrSpec());
  }

  // If `lhs' doesn't support a specialization, it doesn't matter what
  // the `rhs' specialization is.
  if (!supports(SpecKind::Array) && !supports(SpecKind::Class)) {
    return true;
  }

  return lhs.spec() <= rhs.spec();
}

Type Type::operator|(Type rhs) const {
  auto lhs = *this;

  if (lhs <= rhs) return rhs;
  if (rhs <= lhs) return lhs;

  // Representing types like {Int<12>|Arr} could get messy and isn't useful in
  // practice, so unless we hit one of the trivial cases above, drop the
  // constant value(s).
  lhs = lhs.dropConstVal();
  rhs = rhs.dropConstVal();

  auto const bits = lhs.m_bits | rhs.m_bits;
  auto const ptr = lhs.ptrLocation() | rhs.ptrLocation();
  auto const spec = lhs.spec() | rhs.spec();

  return Type{bits, ptr}.specialize(spec);
}

Type Type::operator&(Type rhs) const {
  auto lhs = *this;

  if (lhs <= rhs) return lhs;
  if (rhs <= lhs) return rhs;

  // When intersecting a constant type with another type, the result
  // is the constant type if the other type is a supertype of the
  // constant, and Bottom otherwise.
  auto const handle_constant = [](const Type& constant, const Type& other) {
    if (constant <= other) return constant;
    return TBottom;
  };

  if (lhs.m_hasConstVal) return handle_constant(lhs, rhs);
  if (rhs.m_hasConstVal) return handle_constant(rhs, lhs);

  auto bits = lhs.m_bits & rhs.m_bits;
  auto ptr = lhs.ptrLocation() & rhs.ptrLocation();
  auto arrSpec = lhs.arrSpec() & rhs.arrSpec();
  auto clsSpec = lhs.clsSpec() & rhs.clsSpec();

  if (ptr == PtrLocation::Bottom) bits &= ~kMem;
  if (!(bits & kMem)) ptr = PtrLocation::Bottom;

  if (arrSpec == ArraySpec::Bottom()) bits &= ~kArrSpecBits;
  if (clsSpec == ClassSpec::Bottom()) bits &= ~kClsSpecBits;
  if (!supports(bits, SpecKind::Array)) arrSpec = ArraySpec::Bottom();
  if (!supports(bits, SpecKind::Class)) clsSpec = ClassSpec::Bottom();

  return Type{bits, ptr}.specialize({arrSpec, clsSpec});
}

Type Type::operator-(Type rhs) const {
  auto lhs = *this;
  if (rhs == TBottom) return lhs;
  if (lhs <= rhs) return TBottom;
  if (lhs.hasConstVal()) return lhs;    // not covered by rhs.

  // If `rhs' has a constant value, but `lhs' doesn't, conservatively return
  // `lhs', rather than trying to represent things like "everything except
  // Int<24>". Boolean is a special case.
  if (rhs.m_hasConstVal) {
    if (rhs <= TBool && lhs <= TBool) {
      auto const res = !rhs.boolVal();
      if (lhs.hasConstVal() && lhs.boolVal() != res) return TBottom;
      return cns(res);
    }
    return lhs;
  }

  auto bits = lhs.m_bits & ~rhs.m_bits;
  auto ptr = lhs.ptrLocation() - rhs.ptrLocation();
  auto arrSpec = lhs.arrSpec() - rhs.arrSpec();
  auto clsSpec = lhs.clsSpec() - rhs.clsSpec();

  // Above we subtracted out kPtr or kLval like any other bit in
  // m_bits. However these bits are special, since their removal is
  // determined by the corresponding subtraction of the ptr location
  // bits. We check that here, and fixup the bits as necessary.
  if (ptr != PtrLocation::Bottom) {
    // If there are still ptr location bits remaining after the
    // subtraction, the kMem bits are actually unaffected. This means
    // the subtraction is just removing particular locations, and not
    // the entire "ptr-ish" of the type.
    bits |= lhs.m_bits & kMem;
    // If there's no intersection between the kMem bits on both sides,
    // not even the ptr locations are affected.
    if (!(lhs.m_bits & rhs.m_bits & kMem)) ptr = lhs.ptrLocation();
  } else if (bits & kMem) {
    // On the other hand, if there are no ptr location bits remaining,
    // the subtraction is removing the entire "ptr-ish" from the
    // type. This means the subtraction on m_bits above is correct and
    // no fixup is needed.
    //
    // If the resultant bits still has any kMem bit set, however, it
    // means entire kPtr or kLval survives, so restore the ptr
    // location bits for that half.
    ptr = lhs.ptrLocation();
  }

  auto const supportsArrSpec = supports(bits, SpecKind::Array);
  auto const supportsClsSpec = supports(bits, SpecKind::Class);
  if (arrSpec != ArraySpec::Bottom()) bits |= lhs.m_bits & kArrSpecBits;
  if (clsSpec != ClassSpec::Bottom()) bits |= lhs.m_bits & kClsSpecBits;
  if (supportsArrSpec) arrSpec = lhs.arrSpec();
  if (supportsClsSpec) clsSpec = lhs.clsSpec();

  return Type{bits, ptr}.specialize({arrSpec, clsSpec});
}

///////////////////////////////////////////////////////////////////////////////
// Conversions.

Type typeFromTV(tv_rval tv, const Class* ctx) {
  assertx(tvIsPlausible(*tv));

  if (type(tv) == KindOfObject) {
    auto const cls = val(tv).pobj->getVMClass();
    assertx(cls);

    // We only allow specialization on classes that can't be overridden for
    // now. If this changes, then this will need to specialize on sub object
    // types instead.
    if (!(cls->attrs() & AttrNoOverrideRegular) ||
        (!(cls->attrs() & AttrPersistent) && (!ctx || !ctx->classof(cls)))) {
      return TObj;
    }
    return Type::ExactObj(cls);
  }

  auto const result = Type(dt_modulo_persistence(type(tv)));

  if (isArrayLikeType(type(tv))) {
    return allowBespokeArrayLikes() ? result : result.narrowToVanilla();
  }

  return result;
}

namespace {

bool ratArrIsCounted(const RepoAuthType::Array* arr, const Class* ctx) {
  using E = RepoAuthType::Array::Empty;
  using T = RepoAuthType::Array::Tag;

  if (arr->emptiness() == E::Maybe) return false;

  switch (arr->tag()) {
    case T::Tuple: {
      auto const size = arr->size();
      for (size_t i = 0; i < size; ++i) {
        if (!(typeFromRAT(arr->tupleElem(i), ctx) <= TCounted)) {
          return false;
        }
      }
      return true;
    }
    case T::Packed:
      return typeFromRAT(arr->packedElems(), ctx) <= TCounted;
  }

  return false;
}

}

//////////////////////////////////////////////////////////////////////

Type typeFromRAT(RepoAuthType ty, const Class* ctx) {
  using T = RepoAuthType::Tag;

  #define O(tag, type)                               \
    case T::Opt##tag: return type | TInitNull;       \
    case T::tag:      return type;                   \

  #define U(tag, type)                          \
    case T::Uninit##tag: return type | TUninit; \
    O(tag, type)                                \

  #define NAME_SPEC(type, spec)                                         \
    [&] {                                                               \
      assertx(ty.name() != nullptr);                                    \
      auto const cls = Class::lookupKnown(ty.name(), ctx);              \
      return cls ? Type::spec(cls) : type;                              \
    }()                                                                 \

  #define N(tag, type, sub, exact)                                    \
    O(tag, type)                                                      \
    case T::OptExact##tag: return NAME_SPEC(type, exact) | TInitNull; \
    case T::Exact##tag:    return NAME_SPEC(type, exact);             \
    case T::OptSub##tag:   return NAME_SPEC(type, sub) | TInitNull;   \
    case T::Sub##tag:      return NAME_SPEC(type, sub);               \

  #define UN(tag, type, sub, exact)                                    \
    N(tag, type, sub, exact)                                           \
    case T::Uninit##tag:      return type | TUninit;                   \
    case T::UninitExact##tag: return NAME_SPEC(type, exact) | TUninit; \
    case T::UninitSub##tag:   return NAME_SPEC(type, sub) | TUninit;   \

  #define ARR_SPEC(spec, counted)                                              \
    [&] {                                                                      \
      auto const arr = ty.array();                                             \
      assertx(arr != nullptr);                                                 \
      return ratArrIsCounted(arr, ctx) ? Type::counted(arr) : Type::spec(arr); \
    }()                                                                        \

  #define A(tag, type, spec, counted)                                   \
    O(tag, type)                                                        \
    case T::Opt##tag##Spec: return ARR_SPEC(spec, counted) | TInitNull; \
    case T::tag##Spec:      return ARR_SPEC(spec, counted);             \

  switch (ty.tag()) {
    U(Bool,            TBool)
    U(Int,             TInt)
    U(Str,             TStr)
    U(SStr,            TStaticStr)
    O(Dbl,             TDbl)
    O(Res,             TRes)
    O(Func,            TFunc)
    O(LazyCls,         TLazyCls)
    O(ClsMeth,         TClsMeth)
    O(EnumClassLabel,  TEnumClassLabel)
    O(ArrKey,          TInt | TStr)
    O(UncArrKey,       TInt | TPersistentStr)
    O(ArrKeyCompat,    TInt | TStr | TCls | TLazyCls)
    O(UncArrKeyCompat, TInt | TPersistentStr | TCls | TLazyCls)
    O(StrLike,         TCls | TLazyCls | TStr)
    O(UncStrLike,      TCls | TLazyCls | TPersistentStr)
    O(Num,             TInt | TDbl)
    O(VecCompat,       TVec | TClsMeth)
    O(ArrLikeCompat,   TArrLike | TClsMeth)
    O(ArrLike,         TArrLike)
    O(SArrLike,        TStaticArrLike)
    N(Cls,  TCls, SubCls, ExactCls)
    UN(Obj, TObj, SubObj, ExactObj)
    A(Vec,     TVec,          Vec,          CountedVec)
    A(SVec,    TStaticVec,    StaticVec,    CountedVec)
    A(Dict,    TDict,         Dict,         CountedDict)
    A(SDict,   TStaticDict,   StaticDict,   CountedDict)
    A(Keyset,  TKeyset,       Keyset,       CountedKeyset)
    A(SKeyset, TStaticKeyset, StaticKeyset, CountedKeyset)
    case T::InitPrim:       return TInitNull | TBool | TInt | TDbl;
    case T::Uninit:         return TUninit;
    case T::InitNull:       return TInitNull;
    case T::Null:           return TNull;
    case T::InitUnc:        return TUncountedInit;
    case T::Unc:            return TUncounted;
    case T::NonNull:        return TNonNull;
    case T::InitCell:       return TInitCell;
    case T::Cell:           return TCell;
  }
  not_reached();

  #undef A
  #undef ARR_SPEC
  #undef UN
  #undef N
  #undef NAME_SPEC
  #undef U
  #undef O
}

namespace {

template<class TGetThisType>
Type typeFromTCImpl(const HPHP::TypeIntersectionConstraint& tcs,
                    TGetThisType getThisType,
                    const Class* ctx,
                    bool useObjectForUnresolved = false,
                    bool systemLibOnly = false) {
  using A = AnnotType;
  auto const atToType = [&](AnnotType at) {
    assertx(at != A::SubObject && at != A::Unresolved);
    switch (at) {
      case A::Null:       return TNull;
      case A::Bool:       return TBool;
      case A::Int:        return TInt;
      case A::Float:      return TDbl;
      case A::String:     return TStr;
      case A::Mixed:      return TCell;
      case A::Object:     return TObj;
      case A::Resource:   return TRes;
      case A::Dict:       return TDict;
      case A::Vec:        return TVec;
      case A::Keyset:     return TKeyset;
      case A::Nonnull:    return TInitCell - TInitNull;
      case A::Number:     return TInt | TDbl;
      case A::ArrayKey:   return TInt | TStr;
      case A::VecOrDict:  return TVec | TDict;
      case A::ArrayLike:  return TArrLike;
      case A::Classname:
        if (!Cfg::Eval::ClassPassesClassname) {
          return TStr;
        }
        return TStr | TCls | TLazyCls;
      case A::Class:
        if (Cfg::Eval::ClassTypeLevel > 0) {
          return TCls | TLazyCls;
        }
        return TStr | TCls | TLazyCls;
      case A::ClassOrClassname:
        return TStr | TCls | TLazyCls;
      case A::This:       return getThisType();
      case A::Nothing:
      case A::NoReturn:   return TBottom;
      case A::Callable:
        return TStr | TVec | TDict | TObj | TFuncLike | TObj | TClsMethLike;
      case A::SubObject:
      case A::Unresolved:
        break;
    }
    always_assert(false);
  };

  auto const isSystemLib = [](const Class* cls) {
    return cls->preClass()->unit()->isSystemLib();
  };

  auto baseForTC = [&](const TypeConstraint& tc) {
    if (!tc.isSubObject() && !tc.isUnresolved()) return atToType(tc.type());

    if (tc.isSubObject()) {
      // Don't try to be clever with magic interfaces.
      if (interface_supports_non_objects(tc.clsName())) return TInitCell;

      auto const cls = Class::lookupKnown(tc.clsName(), ctx);
      if (!cls || (systemLibOnly && !isSystemLib(cls))) return TObj;
      assertx(!isEnum(cls));
      return Type::SubObj(cls);
    }

    assertx(tc.isUnresolved());
    if (interface_supports_non_objects(tc.typeName())) return TInitCell;

    // Native return and inout constraints use an object fallback to match HNI
    // ABI rules. User type aliases can resolve to non-object types.
    auto const unresolvedFallback = useObjectForUnresolved ? TObj : TCell;

    if (auto const cls = Class::lookupKnown(tc.typeName(), ctx)) {
      if (systemLibOnly && !isSystemLib(cls)) {
        return unresolvedFallback;
      }
      if (isEnum(cls)) {
        assertx(tc.isUnresolved());
        return atToType(cls->enumBaseTy().type());
      }
      return Type::SubObj(cls);
    }

    bool persistent = false;
    if (auto const alias = TypeAlias::lookup(tc.typeName(), &persistent)) {
      if (systemLibOnly && !alias->unit()->isSystemLib()) {
        return unresolvedFallback;
      }
      if (persistent && !alias->invalid) {
        auto ty = TBottom;
        for (auto const& sub : eachTypeConstraintInUnion(alias->value)) {
          auto type = sub.type();
          auto klass = type == AnnotType::SubObject
            ? sub.clsNamedType()->getCachedClass() : nullptr;
          if (klass) {
            if (interface_supports_non_objects(klass->name())) {
              ty |= TInitCell;
            } else {
              ty |= Type::SubObj(klass);
            }
          } else {
            ty |= atToType(type);
          }
        }
        if (alias->value.isNullable()) ty |= TInitNull;
        return ty;
      }
    }

    return unresolvedFallback;
  };

  auto type = TCell;
  for (auto const& tc : tcs.range()) {
    if (!tc.isCheckable() || tc.isSoft()) continue;
    auto ty = TBottom;
    auto nullable = false;
    auto stringCompatible = false;
    if (tc.isUnion()) {
      for (auto& innerTc : eachTypeConstraintInUnion(tc)) {
        ty |= baseForTC(innerTc);
        if (innerTc.isNullable()) nullable = true;
        if (innerTc.maybeStringCompatible()) stringCompatible = true;
      }
    } else {
      ty |= baseForTC(tc);
      if (tc.isNullable()) nullable = true;
      stringCompatible = tc.maybeStringCompatible();
    }
    // For an inherited string-compatible constraint, if a class or lazy class
    // will not be converted to a string at runtime, widen the type to include
    // TCls | TLazyCls. This avoids constructing an implausible type
    // (e.g. type: TStr, val: Class<C>).
    if (tc.isInherited() && stringCompatible &&
        !Cfg::Eval::ConvertClassToStringForInheritedConstraints) {
      ty |= TCls | TLazyCls;
    }
    // Intersect types from each constraint
    if (nullable) ty |= TInitNull;
    type &= ty;
  }

  assertx(!type.arrSpec().vanilla());
  if (!allowBespokeArrayLikes()) type = type.narrowToVanilla();
  return type;
}

} // namespace

Type typeFromPropTC(const HPHP::TypeIntersectionConstraint& tcs,
                    const Class* propCls,
                    const Class* ctx,
                    bool isSProp,
                    bool systemLibOnly) {
  assertx(tcs.validForProp());

  auto const getThisType = [&] {
    if (!propCls) return TObj;
    return isSProp && (propCls->attrs() & AttrNoMock)
      ? Type::ExactObj(propCls)
      : Type::SubObj(propCls);
  };

  return typeFromTCImpl(tcs, getThisType, ctx, false, systemLibOnly);
}


Type typeFromFuncParam(const Func* func, uint32_t paramId) {
  assertx(paramId < func->numNonVariadicParams());

  if (func->params()[paramId].isOutOnly()) return TInitCell;

  auto const getThisType = [&] {
    return func->cls() ? Type::SubObj(func->cls()) : TBottom;
  };

  auto const& tcs = func->params()[paramId].typeConstraints;
  return typeFromTCImpl(tcs, getThisType, func->cls());
}

Type typeFromFuncReturn(const Func* func, bool pessimizeForBuiltin) {
  auto const getThisType = [&] {
    return func->cls() ? Type::SubObj(func->cls()) : TBottom;
  };

  auto const& tcs = func->returnTypeConstraints();
  auto const rt =
    typeFromTCImpl(tcs, getThisType, func->cls(), func->isCPPBuiltin()) &
    TInitCell;

  if (func->hasUntrustedReturnType()) {
    assertx(func->isCPPBuiltin());
    return rt | TInitNull;
  }

  if (pessimizeForBuiltin) {
    assertx(func->isCPPBuiltin());
    if (rt.maybe(TInitNull) && rt > TInitNull) return TInitCell;
    if (rt == TBottom) return TInitNull;
  }

  return rt;
}

Type typeFromFuncOut(const Func* func, uint32_t inOutIdx) {
  uint32_t paramId = 0;
  for (; paramId < func->numNonVariadicParams(); paramId++) {
    if (!func->isInOut(paramId)) continue;
    if (!inOutIdx) break;
    inOutIdx--;
  }
  assertx(!inOutIdx);

  auto const getThisType = [&] {
    return func->cls() ? Type::SubObj(func->cls()) : TBottom;
  };

  auto const& tcs = func->params()[paramId].typeConstraints;
  return
    typeFromTCImpl(tcs, getThisType, func->cls(), func->isCPPBuiltin()) &
    TInitCell;
}

//////////////////////////////////////////////////////////////////////

Type typeFromSBProfType(const SBProfType& ty) {
  Type t{ty.m_bits, PtrLocation::Bottom};
  if (ty.m_spec == SBProfTypeSpec::None) {
    return t;
  }
  auto const cls = Class::load(ty.m_clsName);
  if (!cls) {
    throw Exception("Class %s not found", ty.m_clsName->data());
  }
  if (ty.m_spec == SBProfTypeSpec::Sub) {
    t.m_clsSpec = ClassSpec{cls, ClassSpec::SubTag{}};
  } else {
    t.m_clsSpec = ClassSpec{cls, ClassSpec::ExactTag{}};
  }
  return t;
}

//////////////////////////////////////////////////////////////////////

Type negativeCheckType(Type srcType, Type typeParam) {
  // Checks relating to StaticStr and StaticArr are not, in general, precise.
  // They may reject some Statics in some situations, where we only guard using
  // the type tag and not by loading the count field.
  if (!typeParam.maybe(TCountedStr)) typeParam -= TStr;
  if (!typeParam.maybe(TCountedVec)) typeParam -= TVec;
  if (!typeParam.maybe(TCountedDict)) typeParam -= TDict;
  if (!typeParam.maybe(TCountedKeyset)) typeParam -= TKeyset;

  if (srcType <= typeParam)      return TBottom;
  if (!srcType.maybe(typeParam)) return srcType;
  return srcType - typeParam;
}

//////////////////////////////////////////////////////////////////////

Type relaxType(Type t, DataTypeCategory cat) {
  assertx(t <= TCell);

  switch (cat) {
    case DataTypeGeneric:
      return TCell;

    case DataTypeCountnessInit:
      if (t <= TUninit) return TUninit;
      if (t <= TUncountedInit) return TUncountedInit;
      return t.unspecialize();

    case DataTypeSpecific:
      return t.unspecialize();

    case DataTypeSpecialized:
      assertx(t.isSpecialized());
      return t;
  }

  not_reached();
}

Type relaxToConstraint(Type t, const GuardConstraint& gc) {
  assertx(t <= TCell);
  if (!gc.isSpecialized()) return relaxType(t, gc.category);

  assertx(t.isSpecialized());
  assertx(gc.wantClass() + gc.isArrayLayoutSensitive() == 1);

  // NOTE: This second check here causes us to guard to specific classes where
  // we could guard to superclasses and unify multiple regions. Rethink it.
  if (gc.wantClass()) return t;
  assertx(gc.isArrayLayoutSensitive());
  assertx(allowBespokeArrayLikes());
  return t.arrSpec().vanilla() ? t.unspecialize().narrowToVanilla() : t;
}

Type relaxToGuardable(Type ty) {
  assertx(ty <= TCell);
  ty = ty.unspecialize();

  // We don't support guarding on counted-ness or static-ness, so widen
  // subtypes of any maybe-countable types to the full type.
  if (ty <= TVec) return TVec;
  if (ty <= TDict) return TDict;
  if (ty <= TKeyset) return TKeyset;
  if (ty <= TArrLike) return TArrLike;

  // We can guard on StaticStr but not CountedStr.
  if (ty <= TCountedStr)     return TStr;

  if (ty.isKnownDataType())  return ty;
  if (ty <= TUncountedInit)  return TUncountedInit;
  if (ty <= TUncounted)      return TUncounted;
  if (ty <= TCell)           return TCell;
  not_reached();
}

}
