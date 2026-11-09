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
#include "hphp/runtime/base/variable-unserializer.h"

#include <algorithm>
#include <new>
#include <utility>

#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/Range.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/dummy-resource.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/struct-log-util.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/zstd-decompress-source.h"

#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/ext/std/ext_std_classobj.h"

#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/repo-global-data.h"

#include "hphp/runtime/vm/jit/perf-counters.h"

#include "hphp/util/configs/eval.h"

#include "hphp/zend/zend-strtod.h"

namespace HPHP {

namespace {

enum class ArrayKind { PHP, Dict, Vec, Keyset };

[[noreturn]] NEVER_INLINE
void throwUnexpectedSep(char expect, char actual) {
  throw Exception("Expected '%c' but got '%c'", expect, actual);
}

[[noreturn]] NEVER_INLINE
void throwOutOfRange(int64_t id) {
  throw Exception("Id %" PRId64 " out of range", id);
}

[[noreturn]] NEVER_INLINE
void throwUnexpectedStr(const char* expect, folly::StringPiece& actual) {
  throw Exception("Expected '%s' but got '%.*s'", expect,
                  (int)actual.size(), actual.data());
}

[[noreturn]] NEVER_INLINE
void throwUnknownType(char type) {
  throw Exception("Unknown type '%c'", type);
}

[[noreturn]] NEVER_INLINE
void throwInvalidPair() {
  throw Exception("Pair objects must have exactly 2 elements");
}

[[noreturn]] NEVER_INLINE
void throwInvalidOFormat(const OptString& clsName) {
  throw Exception("%s does not support the 'O' serialization format",
                  clsName.data());
}

[[noreturn]] NEVER_INLINE
void throwMangledPrivateProperty() {
  throw Exception("Mangled private object property");
}

[[noreturn]] NEVER_INLINE
void throwUnterminatedProperty() {
  throw Exception("Object property not terminated properly");
}

[[noreturn]] NEVER_INLINE
void throwNotCollection(const OptString& clsName) {
  throw Exception("%s is not a collection class", clsName.data());
}

[[noreturn]] NEVER_INLINE
void throwUnexpectedType(const OptString& key, const ObjectData* obj,
                         TypedValue type) {
  auto msg = folly::format(
    "Property {} for class {} was deserialized with type ({}) that "
    "didn't match what we inferred in static analysis",
    key,
    obj->getVMClass()->name(),
    tname(type.m_type)
  ).str();
  throw Exception(msg);
}

[[noreturn]] NEVER_INLINE
void throwUnexpectedType(const StringData* key, const ObjectData* obj,
                         TypedValue type) {
  OptString str(key->data(), key->size(), CopyString);
  throwUnexpectedType(str, obj, type);
}

[[noreturn]] NEVER_INLINE
void throwArraySizeOutOfBounds() {
  throw Exception("Array size out of bounds");
}

[[noreturn]] NEVER_INLINE
void throwInvalidKey() {
  throw Exception("Invalid key");
}

bool isAggregateSerializationType(char type) {
  switch (type) {
    case 'a': // PHP array
    case 'D': // Dict
    case 'X': // MarkedDArray
    case 'Y': // DArray
    case 'x': // MarkedVArray
    case 'y': // VArray
    case 'v': // Vec
    case 'k': // Keyset
    case 'T': // BespokeTypeStructure
    case 'L': // Resource
    case 'O': // Object
    case 'V': // Collection (Vector/Set)
    case 'K': // Collection (Map)
      return true;
    default:
      return false;
  }
}

[[noreturn]] NEVER_INLINE
void throwUnterminatedElement() {
  throw Exception("Array element not terminated properly");
}

[[noreturn]] NEVER_INLINE
void throwLargeStringSize(int64_t size) {
  throw Exception("Size of serialized string (%" PRId64 ") exceeds max", size);
}

[[noreturn]] NEVER_INLINE
void throwNegativeStringSize(int64_t size) {
  throw Exception("Size of serialized string (%" PRId64 ") "
                  "must not be negative", size);
}

[[noreturn]] NEVER_INLINE
void throwBadFormat(const ObjectData* obj, char type) {
  throw Exception("%s does not support the '%c' serialization format",
                  header_names[(int)obj->headerKind()], type);
}

[[noreturn]] NEVER_INLINE
void throwInvalidHashKey(const ObjectData* obj) {
  throw Exception("%s values must be integers or strings",
                  header_names[(int)obj->headerKind()]);
}

[[noreturn]] NEVER_INLINE
void throwColRKey() {
  throw Exception("Referring to collection keys using the 'r' encoding "
                    "is not supported");
}

[[noreturn]] NEVER_INLINE
void throwColRefValue() {
  throw Exception("Collection values cannot be taken by reference");
}

[[noreturn]] NEVER_INLINE
void throwColRefKey() {
  throw Exception("Collection keys cannot be taken by reference");
}

[[noreturn]] NEVER_INLINE
void throwVecRefValue() {
  throw Exception("Vecs cannot contain references");
}

[[noreturn]] NEVER_INLINE
void throwDictRefValue() {
  throw Exception("Dicts cannot contain references");
}

[[noreturn]] NEVER_INLINE
void throwKeysetValue() {
  throw Exception("Keysets can only contain integers and strings");
}

[[noreturn]] NEVER_INLINE
void throwInvalidClassName() {
  throw Exception("Provided class name is invalid");
}

void warnOrThrowUnknownClass(const OptString& clsName) {
  if (Cfg::Eval::ForbidUnserializeIncompleteClass) {
    auto const msg = fmt::format(
      "Attempted to unserialize class named '{}' but it doesn't exist",
      clsName.toCppString()
    );
    if (Cfg::Eval::ForbidUnserializeIncompleteClass > 1) {
      SystemLib::throwExceptionObject(msg);
    } else {
      raise_warning(msg);
    }
  }
}
}

const StaticString
  s_serialized("serialized"),
  s_unserialize("unserialize"),
  s_PHP_Incomplete_Class("__PHP_Incomplete_Class"),
  s_PHP_Incomplete_Class_Name("__PHP_Incomplete_Class_Name"),
  s___wakeup("__wakeup");

///////////////////////////////////////////////////////////////////////////////

const StaticString s_force_darrays{"force_darrays"};
const StaticString s_mark_legacy_arrays{"mark_legacy_arrays"};

template <class Source>
VariableUnserializerImpl<Source>::VariableUnserializerImpl(
  const char* str,
  size_t len,
  Type type,
  bool allowUnknownSerializableClass,
  const Array& options)
    : m_type(type)
    , m_readOnly(false)
    , m_source(str, len)
    , m_unknownSerializable(allowUnknownSerializableClass)
    , m_options(options)
    , m_forceDArrays{m_options[s_force_darrays].toBoolean()}
    , m_markLegacyArrays{m_options[s_mark_legacy_arrays].toBoolean()}
{}

template <class Source>
VariableUnserializerImpl<Source>::Type VariableUnserializerImpl<Source>::type() const {
  return m_type;
}

template <class Source>
bool VariableUnserializerImpl<Source>::allowUnknownSerializableClass() const {
  return m_unknownSerializable;
}

template <class Source>
char VariableUnserializerImpl<Source>::peek() {
  return m_source.peek();
}

template <class Source>
char VariableUnserializerImpl<Source>::peekBack() {
  return m_source.peekBack();
}

template <class Source>
bool VariableUnserializerImpl<Source>::endOfBuffer() {
  return m_source.endOfBuffer();
}

template <class Source>
char VariableUnserializerImpl<Source>::readChar() {
  return m_source.readChar();
}

template <class Source>
void VariableUnserializerImpl<Source>::add(tv_lval v, UnserializeMode mode) {
  switch (mode) {
    case UnserializeMode::Value:  m_refs.emplace_back(v); break;
    // We don't support refs to collection keys; use nullptr as a sentinel.
    case UnserializeMode::ColKey: m_refs.emplace_back(nullptr); break;
    case UnserializeMode::Key:    break;
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::reserveForAdd(size_t count) {
  // If the array is large, the space for the backrefs could be
  // significant, so we need to check for OOM beforehand. To do this,
  // we need to do some guess work to estimate what memory the vector
  // will consume once we've done the reserve (we assume the vector
  // doubles in capacity as necessary).
  auto const newSize = m_refs.size() + count;
  auto const capacity = m_refs.capacity();
  if (newSize <= capacity) return;
  auto const total =
    folly::nextPowTwo(newSize) * sizeof(typename decltype(m_refs)::value_type);
  if (UNLIKELY(total > kMaxSmallSize && tl_heap->preAllocOOM(total))) {
    check_non_safepoint_surprise();
  }
  m_refs.reserve(newSize);
  check_non_safepoint_surprise();
}

template <class Source>
TypedValue VariableUnserializerImpl<Source>::getByVal(int64_t id) {
  if (id <= 0 || static_cast<uint64_t>(id) > m_refs.size()) {
    throwOutOfRange(id);
  }
  auto const result = m_refs[id - 1];
  if (!result) throwColRKey();
  return result.tv();
}

template <class Source>
void VariableUnserializerImpl<Source>::checkElemTermination() {
  auto const ch = peekBack();
  if (ch != ';' && ch != '}') throwUnterminatedElement();
}

template <class Source>
Variant VariableUnserializerImpl<Source>::unserialize() {
  Variant v;
  unserializeVariant(v.asTypedValue());
  // SerDes sampling reads back the whole serialized span, which only a
  // contiguous source exposes; the streaming source has no such span, so
  // sampling is skipped there (restoring it for streaming is a follow-up).
  if constexpr (Source::contiguous) {
    if (UNLIKELY(StructuredLog::coinflip(Cfg::Eval::SerDesSampleRate))) {
      OptString ser(m_source.begin(), m_source.end() - m_source.begin(),
                    CopyString);
      auto const fmt = fmt::format("VU{}", (int)m_type);
      StructuredLog::logSerDes(fmt.c_str(), "des", ser, v);
    }
  }

  auto const providedCoeffects =
    m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
  for (auto& obj : m_sleepingObjects) {
    obj->invokeWakeup(providedCoeffects);
  }

  return v;
}

template <class Source>
int64_t VariableUnserializerImpl<Source>::readInt() {
  return m_source.readInt();
}

template <class Source>
double VariableUnserializerImpl<Source>::readDouble() {
  return m_source.readDouble();
}

template <class Source>
folly::StringPiece VariableUnserializerImpl<Source>::readStr(unsigned n) {
  return m_source.readStr(n);
}

template <class Source>
void VariableUnserializerImpl<Source>::expectChar(char expected) {
  m_source.expectChar(expected);
}

namespace {
bool isWhitelistClass(const OptString& requestedClassName,
                      const Array& list,
                      bool includeSubclasses) {
  if (!list.empty()) {
    for (ArrayIter iter(list); iter; ++iter) {
      auto allowedClassName = iter.second().toString();
      auto const matches = includeSubclasses
        ? HHVM_FN(is_a)(requestedClassName, allowedClassName, true)
        : allowedClassName.get()->tsame(requestedClassName.get());
      if (matches) return true;
    }
  }
  return false;
}
}

const StaticString s_throw("throw");
const StaticString s_allowed_classes("allowed_classes");
const StaticString s_include_subclasses("include_subclasses");

template <class Source>
bool VariableUnserializerImpl<Source>::whitelistCheck(const OptString& clsName) const {
  if (m_type != Type::Serialize || m_options.isNull()) {
    return true;
  }

  // PHP7-style class whitelisting
  // Allowed classes are allowed,
  // all others result in __Incomplete_PHP_Class
  if (m_options.exists(s_allowed_classes)) {
    auto allowed_classes = m_options[s_allowed_classes];
    auto const ok = [&] {
      if (allowed_classes.isArray()) {
        auto const subs = m_options[s_include_subclasses].toBoolean();
        return isWhitelistClass(clsName,
                                allowed_classes.toArray(),
                                subs);
      } else if (allowed_classes.isBoolean()) {
        return allowed_classes.toBoolean();
      } else {
        throw InvalidAllowedClassesException();
      }
    }();

    if (!ok && m_options[s_throw].toBoolean()) {
      throw_object(m_options[s_throw].toString(),
                   make_vec_array(clsName));
    }
    return ok;
  }

  if (!Cfg::Server::UnserializationWhitelistCheck) {
    // No need for BC HHVM-style whitelist check,
    // since the check isn't enabled.
    // Go with PHP5 default behavior of allowing all
    return true;
  }

  // Check for old-style whitelist
  if (isWhitelistClass(clsName, m_options, false)) {
    return true;
  }

  // Non-whitelisted class with a check enabled,
  // are we willing to hard-error over it?
  const char* err_msg =
    "The object being unserialized with class name '%s' "
    "is not in the given whitelist"; // followed by ' in <filename> on line %d'.

  if (Cfg::Server::UnserializationWhitelistCheckWarningOnly) {
    // Nope, just whine to the user and let it through
    raise_warning(err_msg, clsName.c_str());
    return true;
  } else {
    // Yes, shut it down.
    raise_error(err_msg, clsName.c_str());
    return false;
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::addSleepingObject(const Object& o) {
  m_sleepingObjects.emplace_back(o);
}

template <class Source>
bool VariableUnserializerImpl<Source>::matchString(folly::StringPiece str) {
  // The speculative match/rewind cannot be expressed against the streaming
  // window (no backward seek). Returning false without consuming is the correct
  // "no match, parse normally" fallback, so streaming just skips the fast path.
  if constexpr (!Source::contiguous) {
    return false;
  } else {
    auto const bufStart = m_source.head();
    auto const end = m_source.end();
    const char* p = bufStart;
    assertx(p <= end);
    int total = 0;
    if (*p == 'S' && type() == Type::APCSerialize) {
      total = 2 + 8 + 1;
      if (p + total > end) return false;
      p++;
      if (*p++ != ':') return false;
      auto const sd = *reinterpret_cast<StringData*const*>(p);
      assertx(sd->isStatic());
      if (str.compare(sd->slice()) != 0) return false;
      p += size_t(8);
    } else {
      const auto ss = str.size();
      if (ss >= 100) return false;
      int digits = ss >= 10 ? 2 : 1;
      total = 2 + digits + 2 + ss + 2;
      if (p + total > end) return false;
      if (*p++ != 's') return false;
      if (*p++ != ':') return false;
      if (digits == 2) {
        if (*p++ != '0' + ss/10) return false;
        if (*p++ != '0' + ss%10) return false;
      } else {
        if (*p++ != '0' + ss) return false;
      }
      if (*p++ != ':') return false;
      if (*p++ != '\"') return false;
      if (memcmp(p, str.data(), ss)) return false;
      p += ss;
      if (*p++ != '\"') return false;
    }
    if (*p++ != ';') return false;
    assertx(bufStart + total == p);
    m_source.setHead(p);
    return true;
  }
}

///////////////////////////////////////////////////////////////////////////////

// remainingProps should include the current property being unserialized.
template <class Source>
void VariableUnserializerImpl<Source>::unserializePropertyValue(tv_lval v,
                                                    int remainingProps) {
  assertx(remainingProps > 0);
  unserializeVariant(v);
  if (--remainingProps > 0) {
    auto lastChar = peekBack();
    if (lastChar != ';' && lastChar != '}') {
      throwUnterminatedProperty();
    }
  }
}

// nProp should include the current property being unserialized.
template <class Source>
NEVER_INLINE
void VariableUnserializerImpl<Source>::unserializeProp(ObjectData* obj,
                                           const OptString& key,
                                           Class* ctx,
                                           const OptString& realKey,
                                           int nProp) {

  auto const cls = obj->getVMClass();

  // TODO(T126821336): unserializing variable may require module name
  auto const propCtx = MemberLookupContext(ctx, (StringData *)nullptr);
  auto const lookup = cls->getDeclPropSlot(propCtx, key.get());
  auto const slot = lookup.slot;
  tv_lval t;

  if (slot == kInvalidSlot || !lookup.accessible) {
    // Unserialize as a dynamic property. If this is the first, we need to
    // pre-allocate space in the array to ensure the elements don't move during
    // unserialization.
    obj->reserveDynProps(nProp);
    t = obj->makeDynProp(realKey.get());
  } else {
    // We'll check if this doesn't violate the type-hint once we're done
    // unserializing all the props.
    t = obj->getPropLval(propCtx, key.get());
  }

  unserializePropertyValue(t, nProp);
  if (!Cfg::Repo::Authoritative) return;

  /*
   * We assume for performance reasons in repo authoriative mode that
   * we can see all the sets to private properties in a class.
   *
   * It's a hole in this if we don't check unserialization doesn't
   * violate what we've seen, which we handle by throwing if the repo
   * was built with this option.
   */
  if (UNLIKELY(slot == kInvalidSlot)) return;
  auto const repoTy = cls->declPropRepoAuthType(slot);
  if (LIKELY(tvMatchesRepoAuthType(*t, repoTy))) return;
  if (t.type() == KindOfUninit &&
      (cls->declProperties()[slot].attrs & AttrLateInit)) {
    return;
  }
  throwUnexpectedType(key, obj, *t);
}


template <class Source>
NEVER_INLINE
void VariableUnserializerImpl<Source>::unserializeRemainingProps(
  Object& obj,
  int remainingProps,
  Variant& serializedNativeData,
  bool& hasSerializedNativeData) {
  obj->unlockObject();
  SCOPE_EXIT { obj->lockObject(); };
  while (remainingProps > 0) {
    /*
      use the number of properties remaining as an estimate for
      the total number of dynamic properties when we see the
      first dynamic prop.  see getVariantPtr
    */
    Variant v;
    unserializeVariant(v.asTypedValue(), UnserializeMode::Key);
    OptString key = v.toString();
    int ksize = key.size();
    const char *kdata = key.data();
    int subLen = 0;
    if (key == s_serializedNativeDataKey) {
      unserializePropertyValue(serializedNativeData.asTypedValue(),
                               remainingProps--);
      hasSerializedNativeData = true;
    } else if (kdata[0] == '\0') {
      if (UNLIKELY(!ksize)) {
        raise_error("Cannot access empty property");
      }
      // private or protected
      subLen = strlen(std::launder(kdata) + 1) + 2;
      if (UNLIKELY(subLen >= ksize)) {
        if (subLen == ksize) {
          raise_error("Cannot access empty property");
        } else {
          throwMangledPrivateProperty();
        }
      }
      OptString k(kdata + subLen, ksize - subLen, CopyString);
      // We use (Class*)-8 as the sentinel value during deserialization to represent
      // that we are allowed to look up protected properties. -8 allows 3 extra
      // bits to be used for other purposes.
      Class* ctx = (Class*)-8;
      if (kdata[1] != '*') {
        ctx = Class::lookup(
          OptString(kdata + 1, subLen - 2, CopyString).get());
      }
      unserializeProp(obj.get(), k, ctx, key,
                      remainingProps--);
    } else {
      unserializeProp(obj.get(), key, nullptr, key,
                      remainingProps--);
    }
  }
}

namespace {

static const StaticString
  s_Vector("Vector"), s_HH_Vector("HH\\Vector"),
  s_Map("Map"), s_HH_Map("HH\\Map"),
  s_Set("Set"), s_HH_Set("HH\\Set"),
  s_Pair("Pair"), s_HH_Pair("HH\\Pair"),
  s_StableMap("StableMap");

/*
 * For namespaced collections, returns an "alternate" name, which is a
 * collection name with or without the namespace qualifier, depending on
 * what's passed.
 * If no alternate name is found, returns nullptr.
 */
const StringData* getAlternateCollectionName(const StringData* clsName) {
  using ClsNameMap = hphp_hash_map<const StringData*, const StringData*,
                        string_data_hash, string_data_tsame>;

  auto getAltMap = [] {
    using SStringPair = std::pair<StaticString, StaticString>;

    static ClsNameMap m;

    static std::vector<SStringPair> mappings {
      std::make_pair(s_Vector, s_HH_Vector),
      std::make_pair(s_Map, s_HH_Map),
      std::make_pair(s_Set, s_HH_Set),
      std::make_pair(s_Pair, s_HH_Pair)
    };

    for (const auto& p : mappings) {
      m[p.first.get()] = p.second.get();
      m[p.second.get()] = p.first.get();
    }

    // As part of StableMap merging into Map, StableMap is an alias for HH\\Map,
    // but Map is the sole alias for HH\\Map
    m[s_StableMap.get()] = s_HH_Map.get();
    return &m;
  };

  static const ClsNameMap* altMap = getAltMap();

  auto it = altMap->find(clsName);
  return it != altMap->end() ? it->second : nullptr;
}

Class* tryAlternateCollectionClass(const StringData* clsName) {
  auto altName = getAlternateCollectionName(clsName);
  return altName ? Class::get(altName, /* autoload */ false) : nullptr;
}

/*
 * Try to read 'str' while advancing 'cur' without reaching 'end'.
 */
ALWAYS_INLINE
static bool match(const char*& cur,
                  const char* expected,
                  const char* const end) {
  if (cur + strlen(expected) >= end) return false;
  while (*expected) {
    if (*cur++ != *expected++) return false;
  }
  return true;
}

ALWAYS_INLINE
static int64_t read64(const char*& cur) {
  auto p = hh_strtoll_base10(cur);
  cur = p.second;
  return p.first;
}

/*
 * Read an int64 from 'cur' into 'out'. Returns false on unexpected
 * (but possibly still legal) format or if 'end' is reached.
 */
ALWAYS_INLINE
bool readInt64(const char*& cur, const char* const end, int64_t& out) {
  if (!match(cur, "i:", end)) return false;
  out = read64(cur);
  return match(cur, ";", end);
}

/*
 * Read, allocate, and return a string from 'cur'. Returns null on unexpected
 * (but possibly still legal) format or if 'end' is reached, without allocating.
 */
ALWAYS_INLINE
static StringData* readStringData(const char*& cur, const char* const end,
                                  int maxLen) {
  if (!match(cur, "s:", end)) return nullptr;
  auto len = read64(cur);
  if (len < 0 || len >= maxLen) return nullptr;
  if (!match(cur, ":\"", end)) return nullptr;
  auto const slice = folly::StringPiece(cur, len);
  if ((cur += len) >= end) return nullptr;
  if (!match(cur, "\";", end)) return nullptr;
  // TODO(11398853): Consider streaming/non-temporal stores here.
  auto sd = StringData::Make(slice, CopyString);
  return sd;
}
}

template <class Source>
NEVER_INLINE
void VariableUnserializerImpl<Source>::unserializeVariant(
    tv_lval self,
    UnserializeMode mode /* = UnserializeMode::Value */) {

  // If we're overwriting an array element or property value, save the old
  // value in case it's later referenced via an r: or R: ref.
  if (isRefcountedType(self.type()) && mode == UnserializeMode::Value) {
    m_overwrittenList.append(*self);
  }

  char type = readChar();
  char sep = readChar();

  // T267373869
  if (UNLIKELY(mode != UnserializeMode::Value) &&
      isAggregateSerializationType(type)) {
    throwInvalidKey();
  }

  if (type != 'R') {
    add(self, mode);
  }

  if (type == 'N') {
    if (sep != ';') throwUnexpectedSep(';', sep);
    tvSetNull(self); // NULL *IS* the value, without we get undefined warnings
    return;
  }
  if (sep != ':') throwUnexpectedSep(':', sep);

  switch (type) {
  case 'r':
  case 'R':
    {
      int64_t id = readInt();
      tvSet(getByVal(id), self);
    }
    break;
  case 'b':
    {
      int64_t v = readInt();
      tvSetBool((bool)v, self);
      break;
    }
  case 'i':
    {
      int64_t v = readInt();
      tvSetInt(v, self);
      break;
    }
  case 'd':
    {
      char ch = peek();
      bool negative = false;
      if (ch == '-') {
        negative = true;
        readChar();
        ch = peek();
      }
      double v;
      if (ch == 'I') {
        auto str = readStr(3);
        if (str.size() != 3 || str[1] != 'N' || str[2] != 'F') {
          throwUnexpectedStr("INF", str);
        }
        v = std::numeric_limits<double>::infinity();
      } else if (ch == 'N') {
        auto str = readStr(3);
        if (str.size() != 3 || str[1] != 'A' || str[2] != 'N') {
          throwUnexpectedStr("NAN", str);
        }
        v = std::numeric_limits<double>::quiet_NaN();
      } else {
        v = readDouble();
      }
      tvSetDouble(negative ? -v : v, self);
    }
    break;
  case 'l':
    {
      OptString c = unserializeString();
      if (mode == UnserializeMode::Value) {
        tvMove(
          make_tv<KindOfLazyClass>(
            LazyClassData::create(makeStaticString(c.get()))
          ),
          self
        );
      } else {
        if (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0) {
          raise_class_to_string_conversion_notice("variable unserialization");
        }
        tvMove(
          make_tv<KindOfPersistentString>(makeStaticString(c.get())), self
        );
      }
    }
    break;
  case 's':
    {
      OptString v = unserializeString();
      tvMove(make_tv<KindOfString>(v.detach()), self);
      if (!endOfBuffer()) {
        // Semicolon *should* always be required,
        // but PHP's implementation allows omitting it
        // and still functioning.
        // Worse, it throws it away without any check.
        // So we'll do the same.  Sigh.
        readChar();
      }
    }
    return;
  case 'S':
    if (this->type() == VariableUnserializerImpl<Source>::Type::APCSerialize) {
      auto str = readStr(8);
      assertx(str.size() == 8);
      auto const sd = *reinterpret_cast<StringData*const*>(&str[0]);
      assertx(sd->isStatic());
      tvMove(make_tv<KindOfPersistentString>(sd), self);
    } else {
      throwUnknownType(type);
    }
    break;
  case 'a': // PHP array
  case 'D': // Dict
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      // It seems silly to check this here, but GCC actually generates much
      // better code this way.
      auto a = (type == 'a') ?
        unserializeArray() :
        unserializeDict();
      if (UNLIKELY(m_markLegacyArrays && type == 'a')) {
        a.setLegacyArray(true);
      }
      tvMove(make_array_like_tv(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'X': // MarkedDArray
  case 'Y': // DArray
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      auto a = unserializeDArray();
      if (UNLIKELY(m_markLegacyArrays || type == 'X')) {
        a.setLegacyArray(true);
      }
      tvMove(make_array_like_tv(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'x': // MarkedVArray
  case 'y': // VArray
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      auto a = unserializeVArray();
      if (UNLIKELY(m_markLegacyArrays || type == 'x')) {
        a.setLegacyArray(true);
      }
      tvMove(make_array_like_tv(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'v': // Vec
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      auto a = unserializeVec();
      tvMove(make_tv<KindOfVec>(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'k': // Keyset
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      auto a = unserializeKeyset();
      tvMove(make_tv<KindOfKeyset>(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'T': // BespokeTypeStructure
    {
      // Check stack depth to avoid overflow.
      check_recursion_throw();
      auto a = unserializeBespokeTypeStructure();
      tvMove(make_array_like_tv(a.detach()), self);
    }
    return; // array has '}' terminating
  case 'L':
    {
      int64_t id = readInt();
      expectChar(':');
      OptString rsrcName = unserializeString();
      expectChar('{');
      expectChar('}');
      auto rsrc = req::make<DummyResource>();
      rsrc->o_setResourceId(id);
      rsrc->m_class_name = std::move(rsrcName);
      tvMove(make_tv<KindOfResource>(rsrc.detach()->hdr()), self);
    }
    return; // resource has '}' terminating
  case 'O':
  case 'V':
  case 'K':
    {
      OptString clsName = unserializeString();

      expectChar(':');
      const int64_t size = readInt();
      expectChar(':');
      expectChar('{');

      const bool allowObjectFormatForCollections = true;

      Class* cls = nullptr;

      // If we are potentially dealing with a collection, we need to try to
      // load the collection class under an alternate name so that we can
      // deserialize data that was serialized before the migration of
      // collections to the HH namespace.

      if (type == 'O') {
        if (whitelistCheck(clsName)) {
          if (allowObjectFormatForCollections) {
            // In order to support the legacy {O|V}:{Set|Vector|Map}
            // serialization, we defer autoloading until we know that there's
            // no alternate (builtin) collection class.
            cls = Class::get(clsName.get(), /* autoload */ false);
            if (!cls) {
              cls = tryAlternateCollectionClass(clsName.get());
            }
          }

          // No valid class was found, lets try the autoloader.
          if (!cls) {
            if (!is_valid_class_name(clsName.slice())) {
              throwInvalidClassName();
            }
            cls = Class::load(clsName.get()); // with autoloading
          }
        }
      } else {
        // Collections are CPP builtins; don't attempt to autoload
        cls = Class::get(clsName.get(), /* autoload */ false);
        if (!cls) {
          cls = tryAlternateCollectionClass(clsName.get());
        }
        if (!cls || !cls->isCollectionClass()) {
          throwNotCollection(clsName);
        }
      }

      Object obj;
      auto remainingProps = size;
      if (cls) {
        // Only unserialize CPP extension types which can actually support
        // it. Otherwise, we risk creating a CPP object without having it
        // initialized completely.
        if (cls->instanceCtor() && !cls->isCppSerializable() &&
            !cls->isCollectionClass() && m_type != Type::DebuggerSerialize) {
          assertx(obj.isNull());
          throw_null_pointer_exception();
        } else {
          if (UNLIKELY(collections::isType(cls, CollectionType::Pair))) {
            if (UNLIKELY(size != 2)) {
              throwInvalidPair();
            }
            // pairs can't be constructed without elements
            obj = Object{req::make<c_Pair>(make_tv<KindOfNull>(),
                                           make_tv<KindOfNull>(),
                                           c_Pair::NoIncRef{})};
          } else if (UNLIKELY(cls->hasReifiedGenerics())) {
            // First prop on the serialized list is the reified generics prop.
            // matchString() consumes+verifies the name in one shot, but it needs
            // a backward seek and so only works on a contiguous source; on a
            // streaming source it always returns false. Rather than treat that
            // (expected) false as a format error, read the name via the normal
            // key primitive and verify it explicitly.
            if constexpr (Source::contiguous) {
              if (!matchString(s_86reified_prop.slice())) {
                throwInvalidOFormat(clsName);
              }
            } else {
              Variant reifiedKey;
              unserializeVariant(reifiedKey.asTypedValue(),
                                 UnserializeMode::Key);
              if (!reifiedKey.isString() ||
                  !reifiedKey.asCStrRef().get()->same(s_86reified_prop.get())) {
                throwInvalidOFormat(clsName);
              }
            }
            TypedValue tv = make_tv<KindOfNull>();
            auto const t = tv_lval{&tv};
            unserializePropertyValue(t, remainingProps--);
            if (!TypeStructure::coerceToTypeStructureList_SERDE_ONLY(t)) {
              throwInvalidOFormat(clsName);
            }
            assertx(tvIsVec(t));
            obj = Object{cls, t.val().parr};
          } else {
            obj = Object{cls};
          }
        }
      } else {
        warnOrThrowUnknownClass(clsName);
        obj = Object{SystemLib::get__PHP_Incomplete_ClassClass()};
        obj->setProp(nullctx, s_PHP_Incomplete_Class_Name.get(),
                     clsName.asTypedValue());
      }
      assertx(!obj.isNull());
      tvSet(make_tv<KindOfObject>(obj.get()), self);

      if (remainingProps > 0) {
        // Check stack depth to avoid overflow.
        check_recursion_throw();

        if (type == 'O') {
          // Collections are not allowed
          if (obj->isCollection()) {
            throwInvalidOFormat(clsName);
          }

          Variant serializedNativeData = init_null();
          bool hasSerializedNativeData = false;
          bool checkRepoAuthType = Cfg::Repo::Authoritative;
          Class* objCls = obj->getVMClass();
          // Try fast case.
          if (remainingProps >= objCls->numDeclProperties() -
                                (objCls->hasReifiedGenerics() ? 1 : 0)) {
            auto mismatch = false;
            auto const objProps = obj->props();

            auto const declProps = objCls->declProperties();
            for (auto const& p : declProps) {
              auto slot = p.serializationIdx;
              auto index = objCls->propSlotToIndex(slot);
              auto const& prop = declProps[slot];
              if (prop.name == s_86reified_prop.get()) continue;
              if (!matchString(prop.mangledName()->slice())) {
                mismatch = true;
                break;
              }

              // don't need to worry about overwritten list, because
              // this is definitely the first time we're setting this
              // property.
              auto const t = objProps->at(index);
              unserializePropertyValue(t, remainingProps--);

              if (UNLIKELY(checkRepoAuthType &&
                           !tvMatchesRepoAuthType(*t, prop.repoAuthType))) {
                throwUnexpectedType(prop.name, obj.get(), *t);
              }
            }
            // If everything matched, all remaining properties are dynamic.
            if (!mismatch && remainingProps > 0) {
              // the dynPropTable can be mutated while we're deserializing
              // the contents of this object's prop array. Don't hold a
              // reference to this object's entry in the table while looping.
              obj->reserveDynProps(remainingProps);
              while (remainingProps > 0) {
                Variant v;
                unserializeVariant(v.asTypedValue(), UnserializeMode::Key);
                OptString key = v.toString();
                if (key == s_serializedNativeDataKey) {
                  unserializePropertyValue(serializedNativeData.asTypedValue(),
                                           remainingProps--);
                  hasSerializedNativeData = true;
                } else {
                  auto kdata = key.data();
                  if (kdata[0] == '\0') {
                    auto ksize = key.size();
                    if (UNLIKELY(ksize == 0)) {
                      raise_error("Cannot access empty property");
                    }
                    // private or protected
                    auto subLen = strlen(std::launder(kdata) + 1) + 2;
                    if (UNLIKELY(subLen >= ksize)) {
                      if (subLen == ksize) {
                        raise_error("Cannot access empty property");
                      } else {
                        throwMangledPrivateProperty();
                      }
                    }
                  }
                  auto const lval = obj->makeDynProp(key.get());
                  unserializePropertyValue(lval, remainingProps--);
                }
              }
            }
          }
          if (remainingProps > 0) {
            INC_TPC(unser_prop_slow);
            unserializeRemainingProps(obj, remainingProps,
                                      serializedNativeData,
                                      hasSerializedNativeData);
            remainingProps = 0;
          } else {
            INC_TPC(unser_prop_fast);
          }

          // Verify that all the unserialized properties satisfy their
          // type-hints. Its safe to do it like this (after we've set the values
          // in the properties) because this object hasn't escaped to the
          // outside world yet.
          obj->verifyPropTypeHints();

          // nativeDataWakeup is called last to ensure that all properties are
          // already unserialized. We also ensure that nativeDataWakeup is
          // invoked regardless of whether or not serialized native data exists
          // within the serialized content.
          if (obj->hasNativeData() &&
              obj->getVMClass()->getNativeDataInfo()->isSerializable()) {
            Native::nativeDataWakeup(obj.get(), serializedNativeData);
          } else if (hasSerializedNativeData) {
            raise_warning("%s does not expect any serialized native data.",
                          clsName.data());
          }
        } else {
          assertx(type == 'V' || type == 'K');
          if (!obj->isCollection()) {
            throwNotCollection(clsName);
          }
          unserializeCollection(obj.get(), size, type);
        }
      }
      expectChar('}');

      if (cls &&
          cls->lookupMethod(s___wakeup.get()) &&
          (this->type() != VariableUnserializerImpl<Source>::Type::DebuggerSerialize ||
           (cls->instanceCtor() && cls->isCppSerializable()))) {
        // Don't call wakeup when unserializing for the debugger, except for
        // natively implemented classes.
        addSleepingObject(obj);
      }

      check_non_safepoint_surprise();
    }
    return; // object has '}' terminating
  case 'C':
    {
      if (this->type() == VariableUnserializerImpl<Source>::Type::DebuggerSerialize) {
        raise_error("Debugger shouldn't call custom unserialize method");
      }
      OptString clsName = unserializeString();

      expectChar(':');
      OptString serialized = unserializeString('{', '}');

      auto obj = [&]() -> Object {
        if (whitelistCheck(clsName)) {
          // Try loading without the autoloader first
          auto cls = Class::get(clsName.get(), /* autoload */ false);
          if (!cls) {
            if (!is_valid_class_name(clsName.slice())) {
              throwInvalidClassName();
            }
            cls = Class::load(clsName.get());
          }
          if (cls) {
            return Object::attach(g_context->createObject(cls, init_null_variant,
                                                          nullptr /* namedArgNames */,
                                                          false /* init */));
          }
        }
        if (!allowUnknownSerializableClass()) {
          raise_error("unknown class %s", clsName.data());
        }
        warnOrThrowUnknownClass(clsName);
        Object ret = create_object_only(s_PHP_Incomplete_Class);
        ret->setProp(nullctx, s_PHP_Incomplete_Class_Name.get(),
                     clsName.asTypedValue());
        ret->setProp(nullctx, s_serialized.get(), serialized.asTypedValue());
        return ret;
      }();

      if (!obj->instanceof(SystemLib::getSerializableClass())) {
        raise_warning("Class %s has no unserializer",
                      obj->getClassName().data());
      } else {
        obj->o_invoke_few_args(s_unserialize, RuntimeCoeffects::fixme(), 1, serialized);
      }

      tvMove(make_tv<KindOfObject>(obj.detach()), self);
    }
    return; // object has '}' terminating
  case 'c': // Class
    if (m_type == VariableUnserializerImpl<Source>::Type::DebuggerSerialize) {
      tvMove(make_tv<KindOfClass>(Class::load(unserializeString().get())), self);
      break;
    } else {
      throwUnknownType(type);
    }
  case 'f': // Func
    if (m_type == VariableUnserializerImpl<Source>::Type::DebuggerSerialize) {
      tvMove(make_tv<KindOfFunc>(Func::load(unserializeString().get())), self);
      break;
    } else {
      throwUnknownType(type);
    }
  case 'm': // ClsMeth
    if (m_type == VariableUnserializerImpl<Source>::Type::DebuggerSerialize || m_type == VariableUnserializerImpl<Source>::Type::Internal) {
      const auto cls{Class::load(unserializeString().get())};
      expectChar(':');
      const auto func{cls->lookupMethod(unserializeString().get())};
      tvMove(make_tv<KindOfClsMeth>(ClsMethDataRef::create(cls, func)), self);
      break;
    } else {
      throwUnknownType(type);
    }
  case 'e':
    {
      auto const s = makeStaticString(unserializeString());
      tvMove(make_tv<KindOfEnumClassLabel>(s), self);
      break;
    }
  default:
    throwUnknownType(type);
  }
  expectChar(';');
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeArray() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');

  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  unserializeProvenanceTag();

  if (size == 0) {
    expectChar('}');
    return Array::CreateDict();
  }
  // For large arrays, do a naive pre-check for OOM.
  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(size);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  // Pre-allocate an ArrayData of the given size, to avoid escalation in the
  // middle, which breaks references.
  auto arr = m_forceDArrays || type() == Type::Serialize
    ? DictInit(size).toArray()
    : DictInit(size).toArray();
  reserveForAdd(size);

  for (int64_t i = 0; i < size; i++) {
    Variant key;
    unserializeVariant(key.asTypedValue(), UnserializeMode::Key);
    if (!key.isString() && !key.isInteger()) throwInvalidKey();
    unserializeVariant(VanillaDict::LvalInPlace(arr.get(), key));
    if (i < size - 1) checkElemTermination();
  }

  check_non_safepoint_surprise();
  expectChar('}');
  return arr;
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializeProvenanceTag() {
  if (type() != VariableUnserializerImpl<Source>::Type::Internal &&
      type() != VariableUnserializerImpl<Source>::Type::Serialize) {
    return;
  }

  auto const read_line = [&]() -> int {
    expectChar(':');
    expectChar('i');
    expectChar(':');
    auto const line = static_cast<int>(readInt());
    expectChar(';');
    return line;
  };

  auto const read_name = [&]() -> const StringData* {
    if (peek() == 't') {
      assertx(m_unitFilename);
      expectChar('t');
      return m_unitFilename;
    } else {
      expectChar('s');
      expectChar(':');
      return makeStaticString(unserializeString().get());
    }
  };

  if (peek() != 'p') return;
  expectChar('p');

  auto const peeked = peek();
  if (peeked == ':') {
    read_line();
    read_name();
    expectChar(';');
  } else if (peeked == 'f') {
    readChar();
    read_line();
    read_name();
    expectChar(';');
  } else if (peeked == 'c' || peeked == 'e' || peeked == 'r' || peeked == 'z') {
    readChar();
    expectChar(':');
    read_name();
    expectChar(';');
  }
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeDict() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');

  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  unserializeProvenanceTag();

  if (size == 0) {
    expectChar('}');
    return Array::attach(staticEmptyDictArray());
  }

  // For large arrays, do a naive pre-check for OOM.
  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(size);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  Array arr = DictInit(size).toArray();
  for (int64_t i = 0; i < size; i++) {
    Variant key;
    unserializeVariant(key.asTypedValue(), UnserializeMode::Key);
    if (!key.isString() && !key.isInteger()) throwInvalidKey();
    unserializeVariant(VanillaDict::LvalInPlace(arr.get(), key));
    if (i < size - 1) checkElemTermination();
  }

  check_non_safepoint_surprise();
  expectChar('}');
  return arr;
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeVec() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');

  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  unserializeProvenanceTag();

  if (size == 0) {
    expectChar('}');
    return Array::attach(staticEmptyVec());
  }
  auto const sizeClass = VanillaVec::capacityToSizeIndex(size);
  auto const allocsz = MemoryManager::sizeIndex2Size(sizeClass);

  // For large arrays, do a naive pre-check for OOM.
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  Array arr = VecInit(size).toArray();
  reserveForAdd(size);

  for (int64_t i = 0; i < size; i++) {
    unserializeVariant(VanillaVec::LvalNewInPlace(arr.get()));
    if (i < size - 1) checkElemTermination();
  }
  check_non_safepoint_surprise();
  expectChar('}');
  return arr;
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeVArray() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');

  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  unserializeProvenanceTag();

  if (size == 0) {
    expectChar('}');
    if (m_type != Type::Serialize) return Array::CreateVec();
    return m_forceDArrays ? Array::CreateDict() : Array::CreateVec();
  }

  auto const oomCheck = [&](size_t allocsz) {
    // For large arrays, do a naive pre-check for OOM.
    if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
      check_non_safepoint_surprise();
    }
  };

  auto arr = Array{};
  if (m_forceDArrays && m_type == Type::Serialize) {
    // Deserialize to vector-ish darray. Use direct calls to VanillaDict.
    oomCheck(VanillaDict::computeAllocBytesFromMaxElms(size));

    arr = DictInit(size).toArray();
    reserveForAdd(size);

    for (int64_t i = 0; i < size; i++) {
      unserializeVariant(VanillaDict::LvalInPlace(arr.get(), i));
      if (i < size - 1) checkElemTermination();
    }
  } else {
    // Deserialize to varray. Use direct calls to VanillaDict.
    auto const index = VanillaVec::capacityToSizeIndex(size);
    oomCheck(MemoryManager::sizeIndex2Size(index));

    arr = VecInit(size).toArray();
    reserveForAdd(size);

    for (int64_t i = 0; i < size; i++) {
      unserializeVariant(VanillaVec::LvalNewInPlace(arr.get()));
      if (i < size - 1) checkElemTermination();
    }
  }

  check_non_safepoint_surprise();
  expectChar('}');
  return arr;
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeDArray() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');

  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  unserializeProvenanceTag();

  if (size == 0) {
    expectChar('}');
    return Array::CreateDict();
  }

  // For large arrays, do a naive pre-check for OOM.
  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(size);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  auto arr = DictInit(size).toArray();
  reserveForAdd(size);

  for (int64_t i = 0; i < size; i++) {
    Variant key;
    unserializeVariant(key.asTypedValue(), UnserializeMode::Key);
    if (!key.isString() && !key.isInteger()) throwInvalidKey();
    unserializeVariant(VanillaDict::LvalInPlace(arr.get(), key));
    if (i < size - 1) checkElemTermination();
  }

  check_non_safepoint_surprise();
  expectChar('}');
  return arr;
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeKeyset() {
  int64_t size = readInt();
  expectChar(':');
  expectChar('{');
  if (size == 0) {
    expectChar('}');
    return Array::CreateKeyset();
  }
  if (UNLIKELY(size < 0 || size > std::numeric_limits<int>::max())) {
    throwArraySizeOutOfBounds();
  }

  // For large arrays, do a naive pre-check for OOM.
  auto const allocsz = VanillaKeyset::computeAllocBytesFromMaxElms(size);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  KeysetInit init(size);
  for (int64_t i = 0; i < size; i++) {
    Variant key;
    // Use key mode to stop the unserializer from keeping a pointer to this
    // variant (since its stack-allocated).
    unserializeVariant(key.asTypedValue(), UnserializeMode::Key);

    auto const type = key.getType();
    if (UNLIKELY(!isStringType(type) && !isIntType(type))) {
      throwKeysetValue();
    }

    init.add(key);

    if (i < (size - 1)) {
      auto lastChar = peekBack();
      if ((lastChar != ';' && lastChar != '}')) {
        throwUnterminatedElement();
      }
    }
  }
  check_non_safepoint_surprise();
  expectChar('}');
  return init.toArray();
}

template <class Source>
Array VariableUnserializerImpl<Source>::unserializeBespokeTypeStructure() {
  assertx(Cfg::Eval::EmitBespokeTypeStructures);

  auto arr = unserializeDict();
  auto const ts = bespoke::TypeStructure::MakeFromVanilla(arr.get());
  if (ts) arr = Array::attach(ts);
  return arr;
}

template <class Source>
folly::StringPiece
VariableUnserializerImpl<Source>::unserializeStringPiece(char delimiter0, char delimiter1) {
  int64_t size = readInt();
  if (size >= Cfg::ErrorHandling::MaxSerializedStringSize) {
    throwLargeStringSize(size);
  }
  if (size < 0) {
    throwNegativeStringSize(size);
  }
  expectChar(':');
  expectChar(delimiter0);
  auto const piece = readStr(size);
  expectChar(delimiter1);
  return piece;
}

template <class Source>
OptString VariableUnserializerImpl<Source>::unserializeString(char delimiter0,
                                                  char delimiter1) {
  auto const piece = unserializeStringPiece(delimiter0, delimiter1);
  return OptString::attach(readOnly() ?
                           makeStaticString(piece) :
                           StringData::Make(piece, CopyString));
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializeCollection(ObjectData* obj, int64_t sz,
                                                 char type) {
  switch (obj->collectionType()) {
    case CollectionType::Pair:
      unserializePair(obj, sz, type);
      break;
    case CollectionType::Vector:
    case CollectionType::ImmVector:
      unserializeVector(obj, sz, type);
      break;
    case CollectionType::Map:
    case CollectionType::ImmMap:
      unserializeMap(obj, sz, type);
      break;
    case CollectionType::Set:
    case CollectionType::ImmSet:
      unserializeSet(obj, sz, type);
      break;
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializeVector(ObjectData* obj, int64_t sz,
                                             char type) {
  if (type != 'V') throwBadFormat(obj, type);

  auto const sizeClass = VanillaVec::capacityToSizeIndex(sz);
  auto const allocsz = MemoryManager::sizeIndex2Size(sizeClass);
  // For large vectors, do a naive pre-check for OOM.
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  auto bvec = static_cast<BaseVector*>(obj);
  bvec->reserve(sz);
  reserveForAdd(sz);
  for (int64_t i = 0; i < sz; ++i) {
    auto tv = bvec->appendForUnserialize(i);
    HPHP::type(tv) = KindOfNull;
    unserializeVariant(tv);
  }
}

/*
 * Attempts to unserialize into an initially empty HH\Map of string->int/string.
 * Returns false and leaves both 'map' and 'uns' untouched on failure, including
 * unexpected types and possibly legal, but uncommon, encodings.
 */
template <class Source>
NEVER_INLINE
bool VariableUnserializerImpl<Source>::tryUnserializeStrIntMap(BaseMap* map, int64_t sz) {
  // This fast path reads raw contiguous spans via head()/begin()/end()/set(),
  // which are not available over the streaming window; fall back to the normal
  // map path.
  if constexpr (!Source::contiguous) {
    return false;
  } else {
    auto b = m_source.head();
    /*
     * For efficiency, we don't add the keys/values to m_refs, so don't support
     * back-references appearing after this point. For simplicity, we thus
     * require this map to be the root object being unserialized.
     */
    if (folly::StringPiece(m_source.begin(), b) !=
        folly::to<std::string>("K:6:\"HH\\Map\":", sz, ":{")) {
      return false;
    }
    auto const end = m_source.end();
    auto const maxKeyLen = Cfg::ErrorHandling::MaxSerializedStringSize;
    /*
     * First, parse the entire input and allocate the keys (accessing lots of
     * data, but mostly sequentially).
     */
    auto checkPoint = map->batchInsertBegin(sz);
    int64_t i = 0;
    for (; i < sz; ++i) {
      auto sd = readStringData(b, end, maxKeyLen);
      if (!sd) break;
      OptString key = OptString::attach(sd);
      auto tv = map->batchInsert(key.get());
      tv->m_type = KindOfNull;
      if (*b == 'i') {
        if (!readInt64(b, end, tv->m_data.num)) break;
        tv->m_type = KindOfInt64;
      } else if (*b == 's') {
        auto sd = readStringData(b, end, maxKeyLen);
        if (!sd) break;
        tv->m_data.pstr = sd;
        tv->m_type = KindOfString;
      } else {
        break;
      }
    }
    /*
     * On success, finalize the hash table insertion (very random access).
     */
    if (i == sz && map->tryBatchInsertEnd(checkPoint)) {
      m_source.set(b, end);
      return true;
    }
    map->batchInsertAbort(checkPoint);
    return false;
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializeMap(ObjectData* obj, int64_t sz,
                                          char type) {
  if (type != 'K') throwBadFormat(obj, type);

  // For large maps, do a naive pre-check for OOM.
  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(sz);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  auto map = static_cast<BaseMap*>(obj);
  map->reserve(sz);
  if (sz >= Cfg::Server::UnserializationBigMapThreshold &&
      tryUnserializeStrIntMap(map, sz)) {
    return;
  }

  reserveForAdd(sz + sz); // keys + values
  for (int64_t i = 0; i < sz; ++i) {
    Variant k;
    unserializeVariant(k.asTypedValue(), UnserializeMode::ColKey);
    TypedValue* tv = nullptr;
    if (k.isInteger()) {
      auto h = k.toInt64();
      tv = map->findForUnserialize(h);
      // Be robust against manually crafted inputs with conflicting elements
      if (UNLIKELY(!tv)) {
        tv = k.asTypedValue();
        goto do_unserialize;
      }
    } else if (k.isString()) {
      auto key = k.getStringData();
      tv = map->findForUnserialize(key);
      // Be robust against manually crafted inputs with conflicting elements
      if (UNLIKELY(!tv)) {
        tv = k.asTypedValue();
        goto do_unserialize;
      }
    } else {
      throwInvalidKey();
    }
    tv->m_type = KindOfNull;
do_unserialize:
    unserializeVariant(tv);
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializeSet(ObjectData* obj, int64_t sz,
                                          char type) {
  if (type != 'V') throwBadFormat(obj, type);

  // For large maps, do a naive pre-check for OOM.
  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(sz);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  auto set = static_cast<BaseSet*>(obj);
  set->reserve(sz);

  reserveForAdd(sz);
  for (int64_t i = 0; i < sz; ++i) {
    // When unserializing an element of a Set, we use Mode::ColKey for now.
    // This will make the unserializer to reserve an id for the element
    // but won't allow referencing the element via 'r' or 'R'.
    Variant k;
    unserializeVariant(k.asTypedValue(), UnserializeMode::ColKey);
    if (k.isInteger()) {
      set->add(k.toInt64());
    } else if (k.isString()) {
      set->add(k.getStringData());
    } else {
      throwInvalidHashKey(obj);
    }
  }
}

template <class Source>
void VariableUnserializerImpl<Source>::unserializePair(ObjectData* obj, int64_t sz,
                                           char type) {
  assertx(sz == 2);
  if (type != 'V') throwBadFormat(obj, type);
  auto pair = static_cast<c_Pair*>(obj);
  unserializeVariant(pair->at(0));
  unserializeVariant(pair->at(1));
}

// Explicitly instantiate both byte-source variants: the default contiguous
// in-memory path and the streaming zstd-decompression path. Each is fully
// monomorphic, so neither pays for the other's dispatch.
template struct VariableUnserializerImpl<ContiguousSource>;
template struct VariableUnserializerImpl<ZStdDecompressSource>;

}
