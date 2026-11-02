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

#include <cstdint>
#include <cstdio>
#include <string>

#include <fmt/format.h>
#include <folly/lang/Bits.h>

#include "hphp/util/assertions.h"
#include "hphp/util/optional.h"
#include "hphp/util/portability.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

constexpr size_t kDataTypePopCount = 3;

// udt meaning "unordered DataType": compute an encoding of DataTypes into a
// 3-of-7 balanced (and thus, unordered) error-correcting code.
//
// This function returns the `index`th codeword, setting the lowest bit based
// on the bool `counted`. To construct a persistent/counted DataType pair,
// call it twice with the same index but different values for counted.
constexpr int8_t udt(size_t index, bool counted) {
  for (auto i = 0; i <= std::numeric_limits<uint8_t>::max(); i += 2) {
    if (folly::popcount(i) != kDataTypePopCount) continue;
    if (index == 0) return static_cast<int8_t>(i | (counted ? 1 : 0));
    index--;
  }
  // We've run out of codewords. clang allows us to use an always_assert here.
  // GCC does not - if we use an assert, the function is no longer constexpr.
#ifdef __clang__
  always_assert(false);
#else
  return 0;
#endif
}

/*
 * DataType is the type tag for a TypedValue (see typed-value.h).
 *
 * If you want to add a new type, make sure you understand how the current
 * encoding works. A DataType is a uint8_t. Its low bit indicates countedness;
 * if this bit is unset, the value is definitely not refcounted. (If the bit
 * is set, the value may or may not be counted.)
 *
 * We encode different types with a 3-of-7 unordered code on the upper 7 bits
 * of a DataType. This encoding allows us to efficiently test ANY type by
 * checking that the other 4 bits are unset. We can include or exclude the
 * counted value by including the low bit in this test, too.
 *
 * In addition, we support a few efficient tests by doing unsigned LT or GT
 * comparisons on the type byte:
 *  - To check for "vec or dict", check that dt is <= KindOfVec.
 *  - To check for "has persistent flavor", check that dt is <= KindOfString.
 *  - To check for "null or uninit", check that the dt is >= KindOfUninit.
 *
 * If you think you need to change any of these restrictions, be prepared to
 * deal with subtle bugs and/or performance regressions while you sort out the
 * consequences. At a minimum, you must:
 * - Audit every helper function in this file.
 * - Audit jit::emitTypeTest().
 *
 * Manually computed bitmasks are provided for convenience of debugging
 * assembly code of jitted type checks.
 */
#define DATATYPES \
  DT(PersistentDict,   udt(0,  false), 0b00001110) \
  DT(Dict,             udt(0,  true),  0b00001111) \
  DT(PersistentVec,    udt(1,  false), 0b00010110) \
  DT(Vec,              udt(1,  true),  0b00010111) \
  DT(PersistentKeyset, udt(2,  false), 0b00011010) \
  DT(Keyset,           udt(2,  true),  0b00011011) \
  DT(PersistentString, udt(3,  false), 0b00011100) \
  DT(String,           udt(3,  true),  0b00011101) \
  DT(Object,           udt(4,  true),  0b00100111) \
  DT(Resource,         udt(5,  true),  0b00101011) \
  DT(RFunc,            udt(6,  true),  0b00101101) \
  DT(RClsMeth,         udt(7,  true),  0b00110011) \
  DT(ClsMeth,          udt(8,  false), 0b00110100) \
  DT(Boolean,          udt(9,  false), 0b00111000) \
  DT(Int64,            udt(10, false), 0b01000110) \
  DT(Double,           udt(11, false), 0b01001010) \
  DT(Func,             udt(12, false), 0b01001100) \
  DT(Class,            udt(13, false), 0b01010010) \
  DT(LazyClass,        udt(14, false), 0b01010100) \
  DT(EnumClassLabel,   udt(15, false), 0b01011000) \
  DT(Uninit,           udt(16, false), 0b01100010) \
  DT(Null,             udt(17, false), 0b01100100)

#define DT(name, value1, value2) static_assert(value1 == value2, "bad bitmask");
DATATYPES
#undef DT

enum class DataType : int8_t {
#define DT(name, value, ...) name = value,
DATATYPES
#undef DT
};

using data_type_t = typename std::underlying_type<DataType>::type;

// Macro so we can limit its scope to this file. Anyone else doing this cast
// should have to write out the whole thing and think about their life choices.
#define dt_t(t) static_cast<data_type_t>(t)
#define ut_t(t) static_cast<std::make_unsigned<data_type_t>::type>(t)

/*
 * Also define KindOf<Foo> for each type, to avoid having to change thousands
 * of existing usage sites.
 */
#define DT(name, ...) auto constexpr KindOf##name = DataType::name;
DATATYPES
#undef DT

/*
 * Sentinel invalid DataTypes.
 *
 * These values must differ from that of any real DataType.  A live TypedValue
 * should never have these as its type tag, so we keep them out of the enum to
 * keep switches cleaner.
 *
 * These should only be used where MaybeDataType cannot be (e.g., in
 * TypedValues, such as for VanillaDict tombstones).
 */
constexpr DataType kInvalidDataType = static_cast<DataType>(-128);
constexpr DataType kExtraInvalidDataType = static_cast<DataType>(0);

/*
 * DataType limits.
 */
auto constexpr kMinDataType = ut_t(KindOfPersistentDict);
auto constexpr kMaxDataType = ut_t(KindOfNull);
auto constexpr kMinRefCountedDataType = ut_t(KindOfDict);
auto constexpr kMaxRefCountedDataType = ut_t(KindOfRClsMeth);

/*
 * A DataType is a refcounted type if and only if it has this bit set.
 */
constexpr int kRefCountedBit = 0x1;

/*
 * Whether a type is refcounted.
 */
constexpr bool isRefcountedType(DataType t) {
  return ut_t(t) & kRefCountedBit;
}

/*
 * Whether a type is or has a persistent version.
 */
constexpr bool hasPersistentFlavor(DataType t) {
  return ut_t(t) <= ut_t(KindOfString);
}

/*
 * Return `dt` with or without the refcount bit set.
 */
constexpr DataType dt_with_rc(DataType dt) {
  assertx(hasPersistentFlavor(dt) || isRefcountedType(dt));
  return static_cast<DataType>(dt_t(dt) | kRefCountedBit);
}
constexpr DataType dt_with_persistence(DataType dt) {
  assertx(hasPersistentFlavor(dt) || !isRefcountedType(dt));
  return static_cast<DataType>(dt_t(dt) & ~kRefCountedBit);
}

/*
 * Return the ref-counted flavor of `dt` if it has both a KindOf$x and a
 * KindOfPersistent$x flavor
 */
constexpr DataType dt_modulo_persistence(DataType dt) {
  return hasPersistentFlavor(dt) ? dt_with_rc(dt) : dt;
}

///////////////////////////////////////////////////////////////////////////////
/*
 * Optional DataType.
 *
 * Used for (DataType|KindOfNoneType) or (DataType|KindOfAnyType), depending on
 * context.  Users who wish to use (DataType|KindOfNoneType|KindOfAnyType)
 * should consider dying in a fire.
 */
using MaybeDataType = Optional<DataType>;

///////////////////////////////////////////////////////////////////////////////
// DataTypeCategory

// These categories must be kept in order from least to most specific.
#define DT_CATEGORIES(func)                     \
  func(Generic)                                 \
  func(CountnessInit)                           \
  func(Specific)                                \
  func(Specialized)

enum class DataTypeCategory : uint8_t {
#define DT(name) name,
  DT_CATEGORIES(DT)
#undef DT
};

#define DT(name) auto constexpr DataType##name = DataTypeCategory::name;
DT_CATEGORIES(DT)
#undef DT

///////////////////////////////////////////////////////////////////////////////
// Names.

inline std::string tname(DataType t) {
  switch (t) {
#define DT(name, ...) case KindOf##name: return #name;
DATATYPES
#undef DT
    default: {
      if (t == kInvalidDataType) return "Invalid";
      return fmt::format("Unknown:{}", static_cast<int>(t));
    }
  }
}

inline std::string typeCategoryName(DataTypeCategory c) {
  switch (c) {
# define CASE(name) case DataType##name: return "DataType" #name;
  DT_CATEGORIES(CASE)
#undef CASE
  }
  not_reached();
}

/*
 * These are used in type-variant.cpp.
 */
constexpr int kDestrTableSize =
  (kMaxRefCountedDataType - kMinRefCountedDataType) / 2 + 1;

constexpr unsigned typeToDestrIdx(DataType t) {
  // t must be a refcounted type, but we can't actually assert that and still
  // be constexpr.
  return (static_cast<int64_t>(t) - kMinRefCountedDataType) / 2;
}

///////////////////////////////////////////////////////////////////////////////
// Is-a macros.

/*
 * Whether a type is valid.
 */
constexpr bool isRealType(DataType t) {
  return ut_t(t) >= kMinDataType && ut_t(t) <= kMaxDataType &&
         folly::popcount(ut_t(t) & ~kRefCountedBit) == kDataTypePopCount;
}

/*
 * Whether a type's value is an integral value in m_data.num.
 */
constexpr bool hasNumData(DataType t) {
  return t == KindOfBoolean || t == KindOfInt64;
}

/*
 * Whether a type is KindOfUninit or KindOfNull.
 */
constexpr bool isNullType(DataType t) {
  return ut_t(t) >= ut_t(KindOfUninit);
}

/*
 * Whether a type is any kind of string or array.
 */
constexpr bool isStringType(DataType t) {
  return !(ut_t(t) & ~ut_t(KindOfString));
}
inline bool isStringType(MaybeDataType t) {
  return t && isStringType(*t);
}

constexpr bool isArrayLikeType(DataType t) {
  return ut_t(t) <= ut_t(KindOfKeyset);
}
inline bool isArrayLikeType(MaybeDataType t) {
  return t && isArrayLikeType(*t);
}

constexpr bool isVecType(DataType t) {
  return !(ut_t(t) & ~ut_t(KindOfVec));
}
inline bool isVecType(MaybeDataType t) {
  return t && isVecType(*t);
}

constexpr bool isDictType(DataType t) {
  return !(ut_t(t) & ~ut_t(KindOfDict));
}
inline bool isDictType(MaybeDataType t) {
  return t && isDictType(*t);
}

constexpr bool isKeysetType(DataType t) {
  return !(ut_t(t) & ~ut_t(KindOfKeyset));
}
inline bool isKeysetType(MaybeDataType t) {
  return t && isKeysetType(*t);
}

/*
 * Other type-check functions.
 */
constexpr bool isIntType(DataType t) { return t == KindOfInt64; }
constexpr bool isBoolType(DataType t) { return t == KindOfBoolean; }
constexpr bool isDoubleType(DataType t) { return t == KindOfDouble; }
constexpr bool isObjectType(DataType t) { return t == KindOfObject; }
constexpr bool isResourceType(DataType t) { return t == KindOfResource; }
constexpr bool isRFuncType(DataType t) { return t == KindOfRFunc; }
constexpr bool isFuncType(DataType t) { return t == KindOfFunc; }
constexpr bool isClassType(DataType t) { return t == KindOfClass; }
constexpr bool isClsMethType(DataType t) { return t == KindOfClsMeth; }
constexpr bool isRClsMethType(DataType t) { return t == KindOfRClsMeth; }
constexpr bool isLazyClassType(DataType t) { return t == KindOfLazyClass; }
constexpr bool isEnumClassLabelType(DataType t) { return t == KindOfEnumClassLabel; }

/*
 * Return whether two DataTypes for primitive types are "equivalent" as far as
 * user-visible PHP types are concerned (i.e. the same modulo countedness).
 * Note that KindOfUninit and KindOfNull are not considered equivalent.
 */
constexpr bool equivDataTypes(DataType t1, DataType t2) {
  return !((ut_t(t1) ^ ut_t(t2)) & ~kRefCountedBit);
}

/*
 * If you think you need to do any of these operations, you should instead add
 * a helper function up above and call that, to keep any knowledge about the
 * relative values of DataTypes in this file.
 */
bool operator<(DataType, DataType) = delete;
bool operator>(DataType, DataType) = delete;
bool operator<=(DataType, DataType) = delete;
bool operator>=(DataType, DataType) = delete;

#undef ut_t
#undef dt_t

///////////////////////////////////////////////////////////////////////////////
// Switch case macros.

/*
 * Covers all DataTypes `dt' such that !isRefcountedType(dt) holds.
 */
#define DT_UNCOUNTED_CASE   \
  case KindOfUninit:        \
  case KindOfNull:          \
  case KindOfBoolean:       \
  case KindOfInt64:         \
  case KindOfDouble:        \
  case KindOfPersistentString:  \
  case KindOfPersistentVec: \
  case KindOfPersistentDict: \
  case KindOfPersistentKeyset: \
  case KindOfClsMeth:       \
  case KindOfFunc:          \
  case KindOfClass:         \
  case KindOfLazyClass:     \
  case KindOfEnumClassLabel
}

///////////////////////////////////////////////////////////////////////////////

template <>
struct fmt::formatter<HPHP::DataTypeCategory>
    : fmt::formatter<std::string_view> {
  auto format(HPHP::DataTypeCategory val, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        HPHP::typeCategoryName(val), ctx);
  }
};

template <>
struct fmt::formatter<HPHP::DataType> : fmt::formatter<std::string_view> {
  auto format(HPHP::DataType dt, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(HPHP::tname(dt), ctx);
  }
};

///////////////////////////////////////////////////////////////////////////////
