/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#include "hphp/runtime/ext/fb/ext_fb.h"

#include <fstream>

#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <folly/String.h>
#include <folly/portability/Sockets.h>

#ifndef HPHP_OSS
#include "common/serialize/FBSerialize.h"
#endif

#include "hphp/util/configs/eval.h"
#include "hphp/util/htonll.h"
#include "hphp/util/logger.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/code-coverage.h"
#include "hphp/runtime/base/file.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/intercept.h"
#include "hphp/runtime/base/stat-cache.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/code-coverage-util.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/ext/std/ext_std_function.h"
#include "hphp/runtime/ext/fb/VariantController.h"
#include "hphp/runtime/server/xbox-server.h"
#include "hphp/runtime/vm/lazy-class.h"
#include "hphp/runtime/vm/unwind.h"
#include "hphp/zend/zend-string.h"

namespace HPHP {

// fb_serialize options
const int64_t k_FB_SERIALIZE_HACK_ARRAYS = 1<<1;
const int64_t k_FB_SERIALIZE_VARRAY_DARRAY = 1<<2;
const int64_t k_FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS = 1<<3;
const int64_t k_FB_SERIALIZE_POST_HACK_ARRAY_MIGRATION = 1<<4;

// fb_compact_serialize options
const int64_t FB_COMPACT_SERIALIZE_FORCE_PHP_ARRAYS = 1 << 0;
const int64_t FB_COMPACT_SERIALIZE_KEYSETS = 1 << 1;
const int64_t FB_COMPACT_SERIALIZE_KEEP_CLASSES = 1 << 2;

///////////////////////////////////////////////////////////////////////////////

static const UChar32 SUBSTITUTION_CHARACTER = 0xFFFD;

#define FB_UNSERIALIZE_NONSTRING_VALUE           0x0001
#define FB_UNSERIALIZE_UNEXPECTED_END            0x0002
#define FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE  0x0003
#define FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE 0x0004
#define FB_UNSERIALIZE_MAX_DEPTH_EXCEEDED        0x0005

#ifdef HHVM_FACEBOOK
# define HHVM_FACEBOOK_FLAG true
#else
# define HHVM_FACEBOOK_FLAG false
#endif

///////////////////////////////////////////////////////////////////////////////

/* enum of thrift types */
enum TType {
  T_STOP    = 1,
  T_BYTE    = 2,
  T_U16     = 3,
  T_I16     = 4,
  T_U32     = 5,
  T_I32     = 6,
  T_U64     = 7,
  T_I64     = 8,
  T_STRING  = 9,
  T_STRUCT  = 10,
  T_MAP     = 11,
  T_SET     = 12,
  T_LIST    = 13,
  T_NULL    = 14,
  T_VARCHAR = 15,
  T_DOUBLE  = 16,
  T_BOOLEAN = 17,
};

/* Return the smallest size int that can store the value */
#define INT_SIZE(x) (((x) == ((int8_t)x))  ? 1 :    \
                     ((x) == ((int16_t)x)) ? 2 :    \
                     ((x) == ((int32_t)x)) ? 4 : 8)

/* Return the smallest (supported) unsigned length that can store the value */
#define LEN_SIZE(x) ((((unsigned)x) == ((uint8_t)x)) ? 1 : 4)

const StaticString s_invalidMethCallerSerde("Cannot serialize meth_caller");

Variant HHVM_FUNCTION(fb_serialize, const Variant& thing, int64_t options) {
#ifndef HPHP_OSS
  try {
    if (options & k_FB_SERIALIZE_POST_HACK_ARRAY_MIGRATION) {
      size_t len = HPHP::serialize
        ::FBSerializer<VariantControllerPostHackArrayMigration>
        ::serializedSize(thing);
      OptString s(len, ReserveString);
      HPHP::serialize
        ::FBSerializer<VariantControllerPostHackArrayMigration>
        ::serialize(thing, s.mutableData());
      s.setSize(len);
      return s;
    } else if (options & k_FB_SERIALIZE_HACK_ARRAYS) {
      size_t len = HPHP::serialize
        ::FBSerializer<VariantControllerUsingHackArrays>
        ::serializedSize(thing);
      OptString s(len, ReserveString);
      HPHP::serialize
        ::FBSerializer<VariantControllerUsingHackArrays>
        ::serialize(thing, s.mutableData());
      s.setSize(len);
      return s;
    } else if (options & k_FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS) {
      size_t len = HPHP::serialize
        ::FBSerializer<VariantControllerUsingHackArraysAndKeyset>
        ::serializedSize(thing);
      OptString s(len, ReserveString);
      HPHP::serialize
        ::FBSerializer<VariantControllerUsingHackArraysAndKeyset>
        ::serialize(thing, s.mutableData());
      s.setSize(len);
      return s;
    } else if (options & k_FB_SERIALIZE_VARRAY_DARRAY) {
      size_t len = HPHP::serialize
        ::FBSerializer<VariantControllerUsingVarrayDarray>
        ::serializedSize(thing);
      OptString s(len, ReserveString);
      HPHP::serialize
        ::FBSerializer<VariantControllerUsingVarrayDarray>
        ::serialize(thing, s.mutableData());
      s.setSize(len);
      return s;
    } else {
      size_t len =
        HPHP::serialize::FBSerializer<VariantController>::serializedSize(thing);
      OptString s(len, ReserveString);
      HPHP::serialize::FBSerializer<VariantController>::serialize(
        thing, s.mutableData());
      s.setSize(len);
      return s;
    }
  } catch (const HPHP::serialize::KeysetSerializeError&) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Serializing Keysets requires the FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS "
      "option to be provided"
    );
  } catch (const HPHP::serialize::HackArraySerializeError&) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Serializing Hack arrays requires the FB_SERIALIZE_HACK_ARRAYS "
      "option to be provided"
    );
  } catch (const HPHP::serialize::MethCallerSerializeError&) {
    SystemLib::throwInvalidOperationExceptionObject(
      VarNR{s_invalidMethCallerSerde.get()}
    );
  } catch (const HPHP::serialize::SerializeError&) {
    return init_null();
  }
#else
    return init_null();
#endif
}

Variant HHVM_FUNCTION(fb_unserialize,
                      const Variant& thing,
                      bool& success,
                      int64_t options) {
#ifndef HPHP_OSS
  if (thing.isString()) {
    OptString sthing = thing.toString();

    if (sthing.size() && (sthing.data()[0] & 0x80)) {
      Variant error;
      return fb_compact_unserialize(sthing.data(), sthing.size(),
                                    success, error);
    } else {
      return fb_unserialize(sthing.data(), sthing.size(), success, options);
    }
  }
#endif

  success = false;
  return false;
}

#ifndef HPHP_OSS
Variant fb_unserialize(const char* str,
                       int len,
                       bool& success,
                       int64_t options) {
  try {
    if (options & k_FB_SERIALIZE_POST_HACK_ARRAY_MIGRATION) {
      auto res = HPHP::serialize
        ::FBUnserializer<VariantControllerPostHackArrayMigration>
        ::unserialize(folly::StringPiece(str, len));
      success = true;
      return res;
    } else if (options & k_FB_SERIALIZE_HACK_ARRAYS) {
      auto res = HPHP::serialize
        ::FBUnserializer<VariantControllerUsingHackArrays>
        ::unserialize(folly::StringPiece(str, len));
      success = true;
      return res;
    } else if (options & k_FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS) {
      auto res = HPHP::serialize
        ::FBUnserializer<VariantControllerUsingHackArraysAndKeyset>
        ::unserialize(folly::StringPiece(str, len));
      success = true;
      return res;
    } else if (options & k_FB_SERIALIZE_VARRAY_DARRAY) {
      auto res = HPHP::serialize
        ::FBUnserializer<VariantControllerUsingVarrayDarray>
        ::unserialize(folly::StringPiece(str, len));
      success = true;
      return res;
    } else {
      auto res = HPHP::serialize::FBUnserializer<VariantController>
        ::unserialize(folly::StringPiece(str, len));
      success = true;
      return res;
    }
  } catch (const HPHP::serialize::UnserializeError&) {
    success = false;
    return false;
  }
}

///////////////////////////////////////////////////////////////////////////////

/**
 *                         FB Compact Serialize
 *                         ====================
 *
 * === Compatibility with fb_unserialize ===
 *
 * Check the high bit in the first byte of the serialized string.
 * If it's set, the string is fb_compact_serialize'd, otherwise it's
 * fb_serialize'd.
 *
 * === Format ===
 *
 * A value is serialized as a string <c> <data> where c is a byte (0xf0 | code),
 * code being one of:
 *
 *  0 (INT16): data is 2 bytes, network order signed int16
 *  1 (INT32): data is 4 bytes, network order signed int32
 *  2 (INT64): data is 8 bytes, network order signed int64
 *      All of these represent an int64 value.
 *
 *  3 (NULL): no data, null value
 *
 *  4 (TRUE),
 *  5 (FALSE): no data, boolean value
 *
 *  6 (DOUBLE): data is 8 bytes, double value
 *
 *  7 (STRING_0): no data
 *  8 (STRING_1): one char of data
 *  9 (STRING_N): followed by n as a serialized int64, followed by n characters
 *      All of these represent a string value.
 *
 *  10 (LIST_MAP): followed by serialized values until STOP is seen.
 *      Represents a map with numeric keys 0, 1, ..., n-1 (but see SKIP below).
 *
 *  11 (MAP): followed by serialized key/value pairs until STOP
 *      is seen.  Represents a map with arbitrary int64 or string keys.
 *
 *  12 (STOP): no data
 *      Marks the end of a LIST or a MAP.
 *
 *  13 (SKIP): no data
 *      If seen as an entry in a LIST_MAP, the next index in the sequence will
 *       be skipped.  E.g. array(0 => 'a', 1 => 'b', 3 => 'c) will be encoded as
 *      (LIST_MAP, 'a', 'b', SKIP, 'c') instead of
 *      (MAP, 0, 'a', 1, 'b', 3, 'c').
 *
 *  14 (VECTOR): followed by n serialized values until STOP is seen.
 *      Represents a vector of n values.
 *
 *  15 (EXT): extension code. Followed by a 1-byte sub-type, then sub-type
 *      specific data. Sub-types:
 *        0 (EXT_KEYSET): followed by serialized values (int or string) until
 *          STOP is seen. Represents a keyset.
 *        1 (EXT_CLASS): followed by a serialized string (the class name).
 *          Represents a class pointer (deserialized as a lazy class).
 *
 *  In addition, if <c> & 0xf0 != 0xf0, most significant bits of <c> mean:
 *
 *  - 0....... 7-bit unsigned int
 *      (NOTE: not used for the sole int value due to the compatibility
 *       requirement above)
 *  - 10...... + 6 more bytes, 54-bit unsigned int
 *  - 110..... + 1 more byte,  13-bit unsigned int
 *  - 1110.... + 2 more bytes, 20-bit unsigned int
 *
 *  All of these represent an int64 value.
 */

enum FbCompactSerializeCode {
  FB_CS_INT16      = 0,
  FB_CS_INT32      = 1,
  FB_CS_INT64      = 2,
  FB_CS_NULL       = 3,
  FB_CS_TRUE       = 4,
  FB_CS_FALSE      = 5,
  FB_CS_DOUBLE     = 6,
  FB_CS_STRING_0   = 7,
  FB_CS_STRING_1   = 8,
  FB_CS_STRING_N   = 9,
  FB_CS_LIST_MAP   = 10,
  FB_CS_MAP        = 11,
  FB_CS_STOP       = 12,
  FB_CS_SKIP       = 13,
  FB_CS_VECTOR     = 14,
  FB_CS_EXT        = 15,
  FB_CS_MAX_CODE   = 16,
};

// Extension sub-type codes (used with FB_CS_EXT / 0xFF prefix)
enum FbCompactSerializeExtCode : uint8_t {
  FB_CS_EXT_KEYSET = 0,
  FB_CS_EXT_CLASS  = 1,
};

// 1 byte: 0<7 bits>
const uint64_t kInt7Mask            = 0x7f;
const uint64_t kInt7Prefix          = 0x00;

// 2 bytes: 110<13 bits>
const uint64_t kInt13Mask           = (1ULL << 13) - 1;
const uint64_t kInt13PrefixMsbMask  = 0xe0;
const uint64_t kInt13PrefixMsb      = 0xc0;
const uint64_t kInt13Prefix         = kInt13PrefixMsb << (1 * 8);

// 3 bytes: 1110<20 bits>
const uint64_t kInt20Mask           = (1ULL << 20) - 1;
const uint64_t kInt20PrefixMsbMask  = 0xf0;
const uint64_t kInt20PrefixMsb      = 0xe0;
const uint64_t kInt20Prefix         = kInt20PrefixMsb << (2 * 8);

// 7 bytes: 10<54 bits>
const uint64_t kInt54Mask           = (1ULL << 54) - 1;
const uint64_t kInt54PrefixMsbMask  = 0xc0;
const uint64_t kInt54PrefixMsb      = 0x80;
const uint64_t kInt54Prefix         = kInt54PrefixMsb << (6 * 8);

// 1 byte: 1111<4 bits>
const uint64_t kCodeMask            = 0x0f;
const uint64_t kCodePrefix          = 0xf0;

static void fb_compact_serialize_code(StringBuffer& sb,
                                      FbCompactSerializeCode code) {
  assertx(code == (code & kCodeMask));
  uint8_t v = (kCodePrefix | code);
  sb.append(reinterpret_cast<char*>(&v), 1);
}

static void fb_compact_serialize_ext_code(StringBuffer& sb,
                                          FbCompactSerializeExtCode ext) {
  fb_compact_serialize_code(sb, FB_CS_EXT);
  sb.append(reinterpret_cast<const char*>(&ext), 1);
}

static void fb_compact_serialize_int64(StringBuffer& sb, int64_t val) {
  if (val >= 0 && (uint64_t)val <= kInt7Mask) {
    uint8_t nval = val;
    sb.append(reinterpret_cast<char*>(&nval), 1);

  } else if (val >= 0 && (uint64_t)val <= kInt13Mask) {
    uint16_t nval = htons(kInt13Prefix | val);
    sb.append(reinterpret_cast<char*>(&nval), 2);

  } else if (val == (int64_t)(int16_t)val) {
    fb_compact_serialize_code(sb, FB_CS_INT16);
    uint16_t nval = htons(val);
    sb.append(reinterpret_cast<char*>(&nval), 2);

  } else if (val >= 0 && (uint64_t)val <= kInt20Mask) {
    uint32_t nval = htonl(kInt20Prefix | val);
    // Skip most significant byte
    sb.append(reinterpret_cast<char*>(&nval) + 1, 3);

  } else if (val == (int64_t)(int32_t)val) {
    fb_compact_serialize_code(sb, FB_CS_INT32);
    uint32_t nval = htonl(val);
    sb.append(reinterpret_cast<char*>(&nval), 4);

  } else if (val >= 0 && (uint64_t)val <= kInt54Mask) {
    uint64_t nval = htonll(kInt54Prefix | val);
    // Skip most significant byte
    sb.append(reinterpret_cast<char*>(&nval) + 1, 7);

  } else {
    fb_compact_serialize_code(sb, FB_CS_INT64);
    uint64_t nval = htonll(val);
    sb.append(reinterpret_cast<char*>(&nval), 8);
  }
}

static void fb_compact_serialize_string(StringBuffer& sb, const OptString& str) {
  int len = str.size();
  if (len == 0) {
    fb_compact_serialize_code(sb, FB_CS_STRING_0);
  } else {
    if (len == 1) {
      fb_compact_serialize_code(sb, FB_CS_STRING_1);
    } else {
      fb_compact_serialize_code(sb, FB_CS_STRING_N);
      fb_compact_serialize_int64(sb, len);
    }
    sb.append(str.data(), len);
  }
}

static bool fb_compact_serialize_is_list(
    const Array& arr, int64_t& index_limit, int64_t options) {
  index_limit = arr->size();
  auto const dt = arr->toDataType();
  assertx(isVecType(dt) || isDictType(dt));

  // fb_compact_serialize has slightly different formats for dicts and darrays,
  // and for vecs and varrays. We can address these format differences either
  // by monomorphizing the sink (via the option) or by marking the input array.
  auto const force_legacy_format =
    (options & FB_COMPACT_SERIALIZE_FORCE_PHP_ARRAYS) ||
    arr->isLegacyArray();

  if (isVecType(dt)) {
    // Encode empty vecs as lists and empty varrays as maps. It doesn't affect
    // the encoding size or the decoding result so it doesn't matter too much.
    return !arr->empty() || !force_legacy_format;
  }

  int64_t max_index = 0;
  for (ArrayIter it(arr); it; ++it) {
    Variant key = it.first();
    if (!key.isNumeric()) {
      return false;
    }
    int64_t index = key.toInt64();
    if (index < max_index) {
      return false;
    }
    if (index > max_index) {
      max_index = index;
    }
  }

  if (max_index >= arr.size() * 2) {
    // Might as well store it as a map
    return false;
  }

  index_limit = max_index + 1;
  return force_legacy_format;
}

static int fb_compact_serialize_variant(
  StringBuffer& sd, const Variant& var, int depth, int64_t options);

static void fb_compact_serialize_array_as_list_map(
    StringBuffer& sb, const Array& arr, int64_t index_limit,
    int depth, int64_t options) {
  fb_compact_serialize_code(sb, FB_CS_LIST_MAP);
  for (int64_t i = 0; i < index_limit; ++i) {
    if (arr.exists(i)) {
      fb_compact_serialize_variant(sb, arr[i], depth + 1, options);
    } else {
      fb_compact_serialize_code(sb, FB_CS_SKIP);
    }
  }
  fb_compact_serialize_code(sb, FB_CS_STOP);
}

static void fb_compact_serialize_array_as_map(
    StringBuffer& sb, const Array& arr, int depth, int64_t options) {
  fb_compact_serialize_code(sb, FB_CS_MAP);
  IterateKV(
    arr.get(),
    [&](TypedValue k, TypedValue v) {
      if (isStringType(k.m_type)) {
        fb_compact_serialize_string(sb, StrNR{k.m_data.pstr});
      } else {
        assertx(isIntType(k.m_type));
        fb_compact_serialize_int64(sb, k.m_data.num);
      }
      fb_compact_serialize_variant(sb, VarNR(v), depth + 1, options);
    }
  );
  fb_compact_serialize_code(sb, FB_CS_STOP);
}

static void fb_compact_serialize_keyset(
    StringBuffer& sb, const Array& arr, int64_t options) {
  if (options & FB_COMPACT_SERIALIZE_KEYSETS) {
    // Compact encoding: EXT_KEYSET, values..., STOP
    fb_compact_serialize_ext_code(sb, FB_CS_EXT_KEYSET);
    IterateV(
      arr.get(),
      [&](TypedValue v) {
        if (isStringType(v.m_type)) {
          fb_compact_serialize_string(sb, StrNR{v.m_data.pstr});
        } else {
          assertx(v.m_type == KindOfInt64);
          fb_compact_serialize_int64(sb, v.m_data.num);
        }
      }
    );
    fb_compact_serialize_code(sb, FB_CS_STOP);
  } else {
    // Legacy encoding: MAP with key=key pairs
    fb_compact_serialize_code(sb, FB_CS_MAP);
    IterateV(
      arr.get(),
      [&](TypedValue v) {
        if (isStringType(v.m_type)) {
          fb_compact_serialize_string(sb, StrNR{v.m_data.pstr});
          fb_compact_serialize_string(sb, StrNR{v.m_data.pstr});
        } else {
          assertx(v.m_type == KindOfInt64);
          fb_compact_serialize_int64(sb, v.m_data.num);
          fb_compact_serialize_int64(sb, v.m_data.num);
        }
      }
    );
    fb_compact_serialize_code(sb, FB_CS_STOP);
  }
}

static int fb_compact_serialize_variant(
    StringBuffer& sb, const Variant& var, int depth, int64_t options) {
  if (depth > 256) {
    return 1;
  }

  switch (var.getType()) {
    case KindOfUninit:
    case KindOfNull:
      fb_compact_serialize_code(sb, FB_CS_NULL);
      return 0;

    case KindOfBoolean:
      if (var.toInt64()) {
        fb_compact_serialize_code(sb, FB_CS_TRUE);
      } else {
        fb_compact_serialize_code(sb, FB_CS_FALSE);
      }
      return 0;

    case KindOfInt64:
      fb_compact_serialize_int64(sb, var.toInt64());
      return 0;

    case KindOfDouble: {
      fb_compact_serialize_code(sb, FB_CS_DOUBLE);
      double d = var.toDouble();
      sb.append(reinterpret_cast<char*>(&d), 8);
      return 0;
    }

    case KindOfFunc:
      if (var.toFuncVal()->isMethCaller()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerSerde.get()}
        );
      }
      [[fallthrough]];
    case KindOfPersistentString:
    case KindOfString:
      fb_compact_serialize_string(sb, var.toString());
      return 0;

    case KindOfClass:
    case KindOfLazyClass:
      if (options & FB_COMPACT_SERIALIZE_KEEP_CLASSES) {
        fb_compact_serialize_ext_code(sb, FB_CS_EXT_CLASS);
        fb_compact_serialize_string(sb, var.toString());
      } else {
        fb_compact_serialize_string(sb, var.toString());
      }
      return 0;

    case KindOfPersistentKeyset:
    case KindOfKeyset: {
      Array arr = var.toArray();
      assertx(arr->isKeysetType());
      fb_compact_serialize_keyset(sb, std::move(arr), options);
      return 0;
    }

    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentVec:
    case KindOfVec: {
      Array arr = var.toArray();
      int64_t index_limit;
      if (fb_compact_serialize_is_list(arr, index_limit, options)) {
        fb_compact_serialize_array_as_list_map(
          sb, std::move(arr), index_limit, depth, options);
      } else {
        fb_compact_serialize_array_as_map(sb, std::move(arr), depth, options);
      }
      return 0;
    }

    case KindOfClsMeth: {
      Array arr = var.toArray();
      fb_compact_serialize_variant(sb, VarNR(arr), depth, options);
      return 0;
    }

    case KindOfRClsMeth: {
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize reified class method pointer"
      );
    }

    case KindOfObject:
      if (Cfg::Eval::ForbidMethCallerHelperSerialize &&
          var.asCObjRef().get()->getVMClass() ==
            SystemLib::getMethCallerHelperClass()) {
        if (Cfg::Eval::ForbidMethCallerHelperSerialize == 1) {
          fb_compact_serialize_code(sb, FB_CS_NULL);
          raise_warning("Serializing MethCallerHelper");
        } else {
          SystemLib::throwInvalidOperationExceptionObject(
            VarNR{s_invalidMethCallerSerde.get()}
          );
        }
        break;
      }
      [[fallthrough]];
    case KindOfResource:
      fb_compact_serialize_code(sb, FB_CS_NULL);
      raise_warning(
        "fb_compact_serialize(): unable to serialize "
        "object/resource/ref/func/class"
      );
      break;
    case KindOfRFunc:
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize reified function pointer"
      );
    case KindOfEnumClassLabel:
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize enum class label"
      );
  }

  return 1;
}

OptString fb_compact_serialize(const Variant& thing, int64_t options) {
  /**
   * If thing is a single int value [0, 127] normally we would serialize
   * it as a single byte (7 bit unsigned int).
   *
   * However, we want highest bit of the first byte to always be set so
   * that we can tell if the string is fb_serialize'd or fb_compact_serialize'd.
   *
   * So we force to serialize it as 13 bit unsigned int instead.
   */
  if (thing.getType() == KindOfInt64) {
    int64_t val = thing.toInt64();
    if (val >= 0 && (uint64_t)val <= kInt7Mask) {
      OptString s(2, ReserveString);
      *(uint16_t*)(s.mutableData()) = (uint16_t)htons(kInt13Prefix | val);
      s.setSize(2);
      return s;
    }
  }

  StringBuffer sb;
  if (fb_compact_serialize_variant(sb, thing, 0, options)) {
    return OptString();
  }

  return sb.detach();
}

#endif // HPHP_OSS

Variant HHVM_FUNCTION(
    fb_compact_serialize, const Variant& thing, int64_t options) {
#ifndef HPHP_OSS
  return fb_compact_serialize(thing, options);
#else
  return OptString();
#endif
}

#ifndef HPHP_OSS

/* Check if there are enough bytes left in the buffer */
#define CHECK_ENOUGH(bytes, pos, num) do {                                \
    if ((int64_t)(bytes) > (int64_t)((num) - (pos)) || bytes < 0) {       \
      return FB_UNSERIALIZE_UNEXPECTED_END;                               \
    }                                                                     \
  } while (0)


int fb_compact_unserialize_int64_from_buffer(
  int64_t& out, const char* buf, int n, int& p) {

  CHECK_ENOUGH(1, p, n);
  uint64_t first = (unsigned char)buf[p];
  if ((first & ~kInt7Mask) == kInt7Prefix) {
    p += 1;
    out = first & kInt7Mask;

  } else if ((first & kInt13PrefixMsbMask) == kInt13PrefixMsb) {
    CHECK_ENOUGH(2, p, n);
    uint16_t val = (uint16_t)ntohs(*reinterpret_cast<const uint16_t*>(buf + p));
    p += 2;
    out = val & kInt13Mask;

  } else if (first == (kCodePrefix | FB_CS_INT16)) {
    p += 1;
    CHECK_ENOUGH(2, p, n);
    int16_t val = (int16_t)ntohs(*reinterpret_cast<const int16_t*>(buf + p));
    p += 2;
    out = val;

  } else if ((first & kInt20PrefixMsbMask) == kInt20PrefixMsb) {
    CHECK_ENOUGH(3, p, n);
    uint32_t b = 0;
    memcpy(&b, buf + p, 3);
    uint32_t val = ntohl(b);
    p += 3;
    out = (val >> 8) & kInt20Mask;

  } else if (first == (kCodePrefix | FB_CS_INT32)) {
    p += 1;
    CHECK_ENOUGH(4, p, n);
    int32_t val = (int32_t)ntohl(*reinterpret_cast<const int32_t*>(buf + p));
    p += 4;
    out = val;

  } else if ((first & kInt54PrefixMsbMask) == kInt54PrefixMsb) {
    CHECK_ENOUGH(7, p, n);
    uint64_t b = 0;
    memcpy(&b, buf + p, 7);
    uint64_t val = ntohll(b);
    p += 7;
    out = (val >> 8) & kInt54Mask;

  } else if (first == (kCodePrefix | FB_CS_INT64)) {
    p += 1;
    CHECK_ENOUGH(8, p, n);
    int64_t val = (int64_t)ntohll(*reinterpret_cast<const int64_t*>(buf + p));
    p += 8;
    out = val;

  } else {
    return FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE;
  }

  return 0;
}

const StaticString s_empty("");

int fb_compact_unserialize_from_buffer(
    Variant& out, const char* buf, int n, int& p, size_t depth) {
  if (UNLIKELY(depth > 1024)) {
    return FB_UNSERIALIZE_MAX_DEPTH_EXCEEDED;
  }

  CHECK_ENOUGH(1, p, n);
  int code = (unsigned char)buf[p];
  if ((code & ~kCodeMask) != kCodePrefix ||
      (code & kCodeMask) == FB_CS_INT16 ||
      (code & kCodeMask) == FB_CS_INT32 ||
      (code & kCodeMask) == FB_CS_INT64) {

    int64_t val;
    int err = fb_compact_unserialize_int64_from_buffer(val, buf, n, p);
    if (err) {
      return err;
    }
    out = (int64_t)val;
    return 0;
  }
  p += 1;
  code &= kCodeMask;
  switch (code) {
    case FB_CS_NULL:
      out = uninit_null();
      break;

    case FB_CS_TRUE:
      out = true;
      break;

    case FB_CS_FALSE:
      out = false;
      break;

    case FB_CS_DOUBLE:
    {
      CHECK_ENOUGH(8, p, n);
      double d = *reinterpret_cast<const double*>(buf + p);
      p += 8;
      out = d;
      break;
    }

    case FB_CS_STRING_0:
    {
      out = s_empty;
      break;
    }

    case FB_CS_STRING_1:
    case FB_CS_STRING_N:
    {
      int64_t len = 1;
      if (code == FB_CS_STRING_N) {
        int err = fb_compact_unserialize_int64_from_buffer(len, buf, n, p);
        if (err) {
          return err;
        }
      }

    CHECK_ENOUGH(len, p, n);
      out = Variant::attach(StringData::Make(buf + p, len, CopyString));
      p += len;
      break;
    }

    case FB_CS_VECTOR:
    {
      Array arr = Array::CreateVec();
      while (p < n && buf[p] != (char)(kCodePrefix | FB_CS_STOP)) {
        Variant value;
        int err =
          fb_compact_unserialize_from_buffer(value, buf, n, p, depth + 1);
        if (err) {
          return err;
        }
        arr.append(value);
      }

      // Consume STOP
      CHECK_ENOUGH(1, p, n);
      p += 1;

      out = arr;
      break;
    }

    case FB_CS_LIST_MAP:
    {
      Array arr = Array::CreateDict();
      int64_t i = 0;
      while (p < n && buf[p] != (char)(kCodePrefix | FB_CS_STOP)) {
        if (buf[p] == (char)(kCodePrefix | FB_CS_SKIP)) {
          ++i;
          ++p;
        } else {
          Variant value;
          int err =
            fb_compact_unserialize_from_buffer(value, buf, n, p, depth + 1);
          if (err) {
            return err;
          }
          arr.set(i++, value);
        }
      }

      // Consume STOP
      CHECK_ENOUGH(1, p, n);
      p += 1;

      out = arr;
      break;
    }

    case FB_CS_MAP:
    {
      Array arr = Array::CreateDict();
      while (p < n && buf[p] != (char)(kCodePrefix | FB_CS_STOP)) {
        Variant key;
        int err = fb_compact_unserialize_from_buffer(key, buf, n, p, depth + 1);
        if (err) {
          return err;
        }
        Variant value;
        err = fb_compact_unserialize_from_buffer(value, buf, n, p, depth + 1);
        if (err) {
          return err;
        }
        if (key.getType() == KindOfInt64) {
          arr.set(key.toInt64(), value);
        } else if (key.getType() == KindOfString ||
                   key.getType() == KindOfPersistentString) {
          mapSetConvertStatic<IntishCast::Cast>(
            arr, key.asStrRef().get(), std::move(value));
        } else {
          return FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE;
        }
      }

      // Consume STOP
      CHECK_ENOUGH(1, p, n);
      p += 1;

      out = arr;
      break;
    }

    case FB_CS_EXT:
    {
      CHECK_ENOUGH(1, p, n);
      uint8_t ext_code = (unsigned char)buf[p];
      p += 1;
      switch (ext_code) {
        case FB_CS_EXT_KEYSET: {
          Array arr = Array::CreateKeyset();
          while (p < n && buf[p] != (char)(kCodePrefix | FB_CS_STOP)) {
            Variant value;
            int err =
              fb_compact_unserialize_from_buffer(value, buf, n, p, depth + 1);
            if (err) {
              return err;
            }
            if (value.getType() == KindOfInt64 ||
                value.getType() == KindOfString ||
                value.getType() == KindOfPersistentString) {
              arr.append(value);
            } else {
              return FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE;
            }
          }
          // Consume STOP
          CHECK_ENOUGH(1, p, n);
          p += 1;
          out = arr;
          break;
        }
        case FB_CS_EXT_CLASS: {
          Variant name;
          int err =
            fb_compact_unserialize_from_buffer(name, buf, n, p, depth + 1);
          if (err) {
            return err;
          }
          if (!name.isString()) {
            return FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE;
          }
          auto const sname = makeStaticString(name.asCStrRef().get());
          out = Variant{LazyClassData::create(sname)};
          break;
        }
        default:
          return FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE;
      }
      break;
    }

    default:
      return FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE;
  }

  return 0;
}

Variant fb_compact_unserialize(const char* str, int len,
                               bool& success,
                               Variant& errcode) {

  Variant ret;
  int p = 0;
  int err = fb_compact_unserialize_from_buffer(ret, str, len, p, 0);
  if (err) {
    success = false;
    errcode = err;
    return false;
  }
  success = true;
  errcode = init_null();
  return ret;
}

#endif // HPHP_OSS

Variant HHVM_FUNCTION(fb_compact_unserialize,
                      const Variant& thing, bool& success,
                      Variant& errcode) {
#ifndef HPHP_OSS
  if (!thing.isString()) {
    success = false;
    errcode = FB_UNSERIALIZE_NONSTRING_VALUE;
    return false;
  }

  OptString s = thing.toString();
  return fb_compact_unserialize(s.data(), s.size(), success, errcode);
#else
    success = false;
    errcode = 0;
    return false;
#endif
}

///////////////////////////////////////////////////////////////////////////////

bool HHVM_FUNCTION(fb_utf8ize, Variant& input) {
  OptString s = input.toString();
  const char* const srcBuf = s.data();
  int32_t srcLenBytes = s.size();

  if (s.size() < 0 || s.size() > INT_MAX) {
    return false; // Too long.
  }

  // Preflight to avoid allocation if the entire input is valid.
  int32_t srcPosBytes;
  for (srcPosBytes = 0; srcPosBytes < srcLenBytes; /* U8_NEXT increments */) {
    // This is lame, but gcc doesn't optimize U8_NEXT very well
    if (srcBuf[srcPosBytes] != 0 && !(srcBuf[srcPosBytes] & 0x80)) {
      srcPosBytes++; // U8_NEXT would increment this
      continue;
    }
    UChar32 curCodePoint;
    // U8_NEXT() always advances srcPosBytes; save in case curCodePoint invalid
    int32_t savedSrcPosBytes = srcPosBytes;
    U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);
    if (curCodePoint <= 0) {
      // curCodePoint invalid; back up so we'll fix it in the loop below.
      srcPosBytes = savedSrcPosBytes;
      break;
    }
  }

  if (srcPosBytes == srcLenBytes) {
    // it's all valid
    return true;
  }

  // There are invalid bytes. Allocate memory, then copy the input, replacing
  // invalid sequences with either the substitution character or nothing,
  // depending on the value of Cfg::Server::Utf8izeReplace.
  //
  // Worst case, every remaining byte is invalid, taking a 3-byte substitution.
  int32_t bytesRemaining = srcLenBytes - srcPosBytes;
  uint64_t dstMaxLenBytes = srcPosBytes + (Cfg::Server::Utf8izeReplace ?
    bytesRemaining * U8_LENGTH(SUBSTITUTION_CHARACTER) :
    bytesRemaining);
  if (dstMaxLenBytes > INT_MAX) {
    return false; // Too long.
  }
  OptString dstStr(dstMaxLenBytes, ReserveString);
  char *dstBuf = dstStr.mutableData();

  // Copy valid bytes found so far as one solid block.
  memcpy(dstBuf, srcBuf, srcPosBytes);

  // Iterate through the remaining bytes.
  int32_t dstPosBytes = srcPosBytes; // already copied srcPosBytes
  for (/* already init'd */; srcPosBytes < srcLenBytes; /* see U8_NEXT */) {
    UChar32 curCodePoint;
    // This is lame, but gcc doesn't optimize U8_NEXT very well
    if (srcBuf[srcPosBytes] != 0 && !(srcBuf[srcPosBytes] & 0x80)) {
      curCodePoint = srcBuf[srcPosBytes++]; // U8_NEXT would increment
    } else {
      U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);
    }
    if (curCodePoint <= 0) {
      // Invalid UTF-8 sequence.
      // N.B. We consider a null byte an invalid sequence.
      if (!Cfg::Server::Utf8izeReplace) {
        continue; // Omit invalid sequence
      }
      curCodePoint = SUBSTITUTION_CHARACTER; // Replace invalid sequences
    }
    // We know that resultBuffer > total possible length.
    U8_APPEND_UNSAFE(dstBuf, dstPosBytes, curCodePoint);
  }
  assertx(dstPosBytes <= dstMaxLenBytes);
  input = dstStr.shrink(dstPosBytes);
  return true;
}

/**
 * Private utf8_strlen implementation.
 *
 * Returns count of code points in input, substituting 1 code point per invalid
 * sequence.
 *
 * deprecated=true: instead return byte count on invalid UTF-8 sequence.
 */
static int fb_utf8_strlen_impl(const OptString& input, bool deprecated) {
  // Count, don't modify.
  int32_t sourceLength = input.size();
  const char* const sourceBuffer = input.data();
  int64_t num_code_points = 0;

  for (int32_t sourceOffset = 0; sourceOffset < sourceLength; ) {
    UChar32 sourceCodePoint;
    // U8_NEXT() is guaranteed to advance sourceOffset by 1-4 each time it's
    // invoked.
    U8_NEXT(sourceBuffer, sourceOffset, sourceLength, sourceCodePoint);
    if (deprecated && sourceCodePoint < 0) {
      return sourceLength; // return byte count on invalid sequence
    }
    num_code_points++;
  }
  return num_code_points;
}

int64_t HHVM_FUNCTION(fb_utf8_strlen, const OptString& input) {
  return fb_utf8_strlen_impl(input, /* deprecated */ false);
}

int64_t HHVM_FUNCTION(fb_utf8_strlen_deprecated, const OptString& input) {
  return fb_utf8_strlen_impl(input, /* deprecated */ true);
}

StaticString s_substitution_str("\ufffd");

Array HHVM_FUNCTION(fb_utf8_decompose, StringArg input) {
  const size_t len = input->size();
  if (!len) return empty_vec_array();

  VecInit ret{len};
  auto const bufp = input->data();
  int32_t next = 0;
  do {
    auto off = next;
    UChar32 codePoint;
    U8_NEXT(bufp, next, len, codePoint);

    if (UNLIKELY(codePoint < 0)) {
      ret.append(StrNR{s_substitution_str.get()}.asString());
    } else if (LIKELY(next == off + 1)) {
      ret.append(StrNR{precomputed_chars[uint8_t(bufp[off])]}.asString());
    } else {
      ret.append(OptString{&bufp[off], size_t(next - off), CopyString});
    }
  } while (next < len);

  return ret.toArray();
}

/**
 * Private helper; requires non-negative firstCodePoint and desiredCodePoints.
 */
static OptString fb_utf8_substr_simple(const OptString& str,
                                       int32_t firstCodePoint,
                                       int32_t numDesiredCodePoints) {
  const char* const srcBuf = str.data();
  int32_t srcLenBytes = str.size(); // May truncate; checked before use below.

  assertx(firstCodePoint >= 0); // Wrapper fixes up negative starting positions.
  assertx(numDesiredCodePoints > 0); // Wrapper fixes up negative/zero length.
  if (str.size() <= 0 ||
      str.size() > INT_MAX ||
      firstCodePoint >= srcLenBytes) {
    return String::Empty();
  }

  // Cannot be more code points than bytes in input.  This typically reduces
  // the INT_MAX default value to something more reasonable.
  numDesiredCodePoints = std::min(numDesiredCodePoints,
                                  srcLenBytes - firstCodePoint);

  // Pre-allocate the result.
  // the worst case can come from one of two sources:
  //  - every code point could be the substitution char (3 bytes)
  //    giving us numDesiredCodePoints * 3
  //  - every code point could be 4 bytes long, giving us
  //    numDesiredCodePoints * 4 - but capped by the length of the input
  uint64_t dstMaxLenBytes =
    std::min((uint64_t)numDesiredCodePoints * 4,
             (uint64_t)srcLenBytes - firstCodePoint);
  dstMaxLenBytes = std::max(dstMaxLenBytes,
                            (uint64_t)numDesiredCodePoints *
                            U8_LENGTH(SUBSTITUTION_CHARACTER));
  if (dstMaxLenBytes > INT_MAX) {
    return String::Empty(); // Too long.
  }
  OptString dstStr(dstMaxLenBytes, ReserveString);
  char* dstBuf = dstStr.mutableData();
  int32_t dstPosBytes = 0;

  // Iterate through src's codepoints; srcPosBytes is incremented by U8_NEXT.
  for (int32_t srcPosBytes = 0, srcPosCodePoints = 0;
       srcPosBytes < srcLenBytes && // more available
       srcPosCodePoints < firstCodePoint + numDesiredCodePoints; // want more
       srcPosCodePoints++) {

    // U8_NEXT() advances sourceBytePos by 1-4 each time it's invoked.
    UChar32 curCodePoint;
    U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);

    if (srcPosCodePoints >= firstCodePoint) {
      // Copy this code point into the result.
      if (curCodePoint < 0) {
        curCodePoint = SUBSTITUTION_CHARACTER; // replace invalid sequences
      }
      // We know that resultBuffer > total possible length.
      // U8_APPEND_UNSAFE updates dstPosBytes.
      U8_APPEND_UNSAFE(dstBuf, dstPosBytes, curCodePoint);
    }
  }

  assertx(dstPosBytes <= dstMaxLenBytes);
  if (dstPosBytes > 0) {
    dstStr.shrink(dstPosBytes);
    return dstStr;
  }
  return String::Empty();
}

OptString HHVM_FUNCTION(fb_utf8_substr, const OptString& str, int64_t start,
                                        int64_t length /* = INT_MAX */) {
  if (length > INT_MAX) {
    length = INT_MAX;
  }
  // For negative start or length, calculate start and length values
  // based on total code points.
  if (start < 0 || length < 0) {
    // Get number of code points assuming we substitute invalid sequences.
    Variant utf8StrlenResult = HHVM_FN(fb_utf8_strlen)(str);
    auto sourceNumCodePoints = (int)utf8StrlenResult.toInt64();

    if (start < 0) {
      // Negative means first character is start'th code point from end.
      // e.g., -1 means start with the last code point.
      start = sourceNumCodePoints + start; // adding negative start
    }
    if (length < 0) {
      // Negative means omit last abs(length) code points.
      length = sourceNumCodePoints - start + length; // adding negative length
    }
  }
  if (start < 0 || length <= 0) {
    return String::Empty(); // Empty result
  }

  return fb_utf8_substr_simple(str, start, length);
}

///////////////////////////////////////////////////////////////////////////////

bool HHVM_FUNCTION(fb_intercept2, const OptString& name, const Variant& handler) {
  return register_intercept(name, handler);
}

bool HHVM_FUNCTION(fb_intercept2_named_args, const OptString& name, const Variant& handler) {
  return register_intercept(name, handler);
}

bool HHVM_FUNCTION(fb_rename_function, const OptString& orig_func_name,
                                       const OptString& new_func_name) {
  if (orig_func_name.empty() || new_func_name.empty() ||
      orig_func_name.get()->fsame(new_func_name.get())) {
    raise_invalid_argument_warning("unable to rename %s", orig_func_name.data());
    return false;
  }

  if (!function_exists(orig_func_name)) {
    raise_warning("fb_rename_function(%s, %s) failed: %s does not exist!",
                  orig_func_name.data(), new_func_name.data(),
                  orig_func_name.data());
    return false;
  }

  if (function_exists(new_func_name)) {
    if (new_func_name.data()[0] != '1') {
      raise_warning("fb_rename_function(%s, %s) failed: %s already exists!",
                    orig_func_name.data(), new_func_name.data(),
                    new_func_name.data());
      return false;
    }
  }

  rename_function(orig_func_name, new_func_name);
  return true;
}

///////////////////////////////////////////////////////////////////////////////

Variant HHVM_FUNCTION(fb_get_code_coverage, bool flush) {
  RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
  if (ti->m_reqInjectionData.getCoverage()) {
    Array ret = ti->m_coverage.Report();
    if (flush) {
      ti->m_coverage.Reset();
    }
    return ret;
  }
  return false;
}

void HHVM_FUNCTION(fb_enable_code_coverage) {
  RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
  ti->m_coverage.Reset();
  ti->m_reqInjectionData.setCoverage(true);
  if (g_context->isNested()) {
    raise_notice("Calling fb_enable_code_coverage from a nested "
                 "VM instance may cause unpredicable results");
  }
  if (Cfg::Eval::EnableCodeCoverage == 0) {
    SystemLib::throwRuntimeExceptionObject(
      "Calling fb_enable_code_coverage without enabling the setting "
      "Eval.EnableCodeCoverage");
  }
  if (Cfg::Eval::EnableCodeCoverage == 1) {
    if (!isEnableCodeCoverageReqParamTrue()) {
      SystemLib::throwRuntimeExceptionObject(
        "Calling fb_enable_code_coverage without adding "
        "'enable_code_coverage' in request params");
    }
  }
  if (isEnablePerFileCoverageReqParamTrue()) {
    SystemLib::throwRuntimeExceptionObject(
      "Calling fb_enable_code_coverage with "
      "'enable_per_file_coverage' in request params");
  }
  if (Cfg::Eval::EnablePerFileCoverage == 2) {
    SystemLib::throwRuntimeExceptionObject(
      "Calling fb_enable_code_coverage with "
      "Eval.EnablePerFileCoverage=2");
  }
}

Array disable_code_coverage_helper(bool report_frequency) {
  RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
  ti->m_reqInjectionData.setCoverage(false);
  auto ret = ti->m_coverage.Report(report_frequency);
  ti->m_coverage.Reset();
  return ret;
}

Array HHVM_FUNCTION(fb_disable_code_coverage) {
  return disable_code_coverage_helper(/* report frequency */ false);
}

Array HHVM_FUNCTION(HH_disable_code_coverage_with_frequency) {
  return disable_code_coverage_helper(/* report frequency */ true);
}

///////////////////////////////////////////////////////////////////////////////

bool HHVM_FUNCTION(fb_output_compression, bool new_value) {
  Transport *transport = g_context->getTransport();
  if (transport) {
    bool rv = transport->isCompressionEnabled();
    if (new_value) {
      transport->enableCompression();
    } else {
      transport->disableCompression();
    }
    return rv;
  }
  return false;
}

void HHVM_FUNCTION(fb_set_exit_callback, const Variant& function) {
  g_context->setExitCallback(function);
}

const StaticString
  s_flush_stats("flush_stats"),
  s_chunk_stats("chunk_stats"),
  s_total("total"),
  s_sent("sent"),
  s_time("time");

int64_t HHVM_FUNCTION(fb_get_last_flush_size) {
  Transport *transport = g_context->getTransport();
  return transport ? transport->getLastChunkSentSize() : 0;
}

extern Array stat_impl(struct stat*); // ext_file.cpp

template<class Function>
static Variant do_lazy_stat(Function dostat, const OptString& filename) {
  struct stat sb;
  if (dostat(File::TranslatePathWithFileCache(filename).c_str(), &sb)) {
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,
                    folly::errnoStr(errno).c_str());
    return false;
  }
  return stat_impl(&sb);
}

Variant HHVM_FUNCTION(fb_lazy_lstat, const OptString& filename) {
  if (!FileUtil::checkPathAndWarn(filename, __FUNCTION__ + 2, 1)) {
    return false;
  }
  return do_lazy_stat(lstatSyscall, filename);
}

Variant HHVM_FUNCTION(fb_lazy_realpath, const OptString& filename) {
  if (!FileUtil::checkPathAndWarn(filename, __FUNCTION__ + 2, 1)) {
    return false;
  }

  return realpathLibc(filename.c_str());
}

int64_t HHVM_FUNCTION(HH_non_crypto_md5_upper, StringArg str) {
  Md5Digest md5(str.get()->data(), str.get()->size());
  int64_t pre_decode;
  // Work around "strict aliasing" with memcpy
  memcpy(&pre_decode, md5.digest, sizeof(pre_decode));
  // When PHP/Hack users decode MD5 hex, they treat it as big endian.
  // Replicate that here.
  return folly::Endian::big(pre_decode);
}

int64_t HHVM_FUNCTION(HH_non_crypto_md5_lower, StringArg str) {
  Md5Digest md5(str.get()->data(), str.get()->size());
  int64_t pre_decode;
  // Work around "strict aliasing" with memcpy
  memcpy(&pre_decode, md5.digest + 8, sizeof(pre_decode));
  // When PHP/Hack users decode MD5 hex, they treat it as big endian.
  // Replicate that here.
  return folly::Endian::big(pre_decode);
}

int64_t HHVM_FUNCTION(HH_int_mul_overflow, int64_t a, int64_t b) {
  // On x86_64, this compiles down to mov+mul+mov+retq
  uint64_t ua = a;
  uint64_t ub = b;
  __uint128_t full_product =
    static_cast<__uint128_t>(ua) * static_cast<__uint128_t>(ub);
  // Return only the part that would overflow 64-bit multiplication.
  return static_cast<int64_t>(full_product >> 64);
}

int64_t HHVM_FUNCTION(HH_int_mul_add_overflow,
                      int64_t a, int64_t b, int64_t bias) {
  uint64_t ua = a;
  uint64_t ub = b;
  uint64_t umbias = static_cast<uint64_t>(-1 - bias);
  __uint128_t full =
    static_cast<__uint128_t>(ua) * static_cast<__uint128_t>(ub);
  // The assembly for this looks faster than 128-bit add to 'full'
  // (8 instructions vs. 11)
  uint64_t full_lower = static_cast<uint64_t>(full);
  return static_cast<int64_t>(full >> 64) + (full_lower > umbias);
}

///////////////////////////////////////////////////////////////////////////////
// Product attribution id

void HHVM_FUNCTION(set_product_attribution_id, int64_t) {
  SystemLib::throwInvalidArgumentExceptionObject(
    "Unsupported dynamic call of set_product_attribution_id()");
}

void HHVM_FUNCTION(set_product_attribution_id_deferred, const Variant&) {
  SystemLib::throwInvalidArgumentExceptionObject(
    "Unsupported dynamic call of set_product_attribution_id_deferred()");
}

Variant HHVM_FUNCTION(get_product_attribution_id_internal) {
  // The caller of this function always eagerly syncs the vmregs, so in
  // non-debug mode this anchor should be a no-op.
  VMRegAnchor _;

  Variant result{Variant::NullInit()};
  walkStack([&] (const BTFrame& frm) {
    auto const func = frm.func();
    if (!(func->attrs() & AttrHasAttributionData)) return false;
    if (!frm.localsAvailable()) return false; // can this be an assert instead?
    auto local = func->lookupVarId(s_86productAttributionData.get());
    assertx(local != kInvalidId);
    auto const val = frm.local(local);
    assertx(tvIsPlausible(*val));
    if (val->type() == KindOfUninit) return false; // skip over uninit vals
    result = Variant{variant_ref{val}};
    return true;
  });
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// xbox APIs

namespace {

Array fb_call_user_func_message(const OptString& initialDoc,
                                const Variant& function,
                                const Array& _argv) {
  if (function.isArray()) {
    Array farr = function.toArray();
    if (!array_is_valid_callback(farr)) {
      raise_warning("function must be a valid callback");
      return Array();
    }
  } else if (!function.isString() && !function.isFunc() &&
             !function.isClsMeth()) {
    raise_warning("function must be a valid callback");
    return Array();
  }
  if (initialDoc.empty()) {
    raise_warning("initialDoc must be a non-empty string");
    return Array();
  }

  Array msg;
  msg.append(function);
  msg.append(_argv);

  return msg;
}

Variant HHVM_FUNCTION(fb_call_user_func_array_async, const OptString& initialDoc,
                      const Variant& function, const Array& params) {
  Array msg = fb_call_user_func_message(initialDoc, function, params);
  if (msg.empty()) {
    return init_null();
  }
  
  RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
  RequestId root_req_id = ti->getRootRequestId();
  root_req_id = root_req_id.unallocated() ? ti->m_id : root_req_id;
  return XboxServer::TaskStart(internal_serialize(msg), initialDoc, nullptr, root_req_id);
}

Variant HHVM_FUNCTION(fb_check_user_func_async, const OptResource& handle) {
  return XboxServer::TaskStatus(handle);
}

Variant HHVM_FUNCTION(fb_end_user_func_async, const OptResource& handle) {
  Variant ret;
  int code = XboxServer::TaskResult(handle, 0, &ret);
  if (code != 200) {
    SystemLib::throwExceptionObject(ret);
  }
  return ret;
}

Variant HHVM_FUNCTION(fb_gen_user_func_array, const OptString& initialDoc,
                      const Variant& function, const Array& _argv) {
  Array msg = fb_call_user_func_message(initialDoc, function, _argv);
  if (msg.empty()) {
    return init_null();
  }

  ServerTaskEvent<XboxServer, XboxTransport> *event;
  event = new ServerTaskEvent<XboxServer, XboxTransport>();

  try {
    RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
    RequestId root_req_id = ti->getRootRequestId();
    root_req_id = root_req_id.unallocated() ? ti->m_id : root_req_id;
    
    XboxServer::TaskStart(internal_serialize(msg), initialDoc, event, root_req_id);
  } catch (...) {
    event->abandon();
    throw;
  }

  return Variant{event->getWaitHandle()};
}

Variant HHVM_FUNCTION(extension_warmup_data, const OptString& extension_name) {
  auto ext = ExtensionRegistry::get(extension_name.data(), true);
  if (!ext) return init_null();
  auto data = ext->getWarmupData();
  if (!data) return init_null();
  return Array::attach(data);
}

}

///////////////////////////////////////////////////////////////////////////////

struct FBExtension : Extension {
  FBExtension(): Extension("fb", "1.0.0", NO_ONCALL_YET) {}

  void moduleRegisterNative() override {
    HHVM_RC_BOOL(HHVM_FACEBOOK, HHVM_FACEBOOK_FLAG);
    HHVM_RC_INT_SAME(FB_UNSERIALIZE_NONSTRING_VALUE);
    HHVM_RC_INT_SAME(FB_UNSERIALIZE_UNEXPECTED_END);
    HHVM_RC_INT_SAME(FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE);
    HHVM_RC_INT_SAME(FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE);
    HHVM_RC_INT_SAME(FB_UNSERIALIZE_MAX_DEPTH_EXCEEDED);

    HHVM_RC_INT(FB_SERIALIZE_HACK_ARRAYS, k_FB_SERIALIZE_HACK_ARRAYS);
    HHVM_RC_INT(FB_SERIALIZE_VARRAY_DARRAY, k_FB_SERIALIZE_VARRAY_DARRAY);
    HHVM_RC_INT(FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS,
                k_FB_SERIALIZE_HACK_ARRAYS_AND_KEYSETS);
    HHVM_RC_INT(FB_SERIALIZE_POST_HACK_ARRAY_MIGRATION,
                k_FB_SERIALIZE_POST_HACK_ARRAY_MIGRATION);

    HHVM_RC_INT_SAME(FB_COMPACT_SERIALIZE_FORCE_PHP_ARRAYS);
    HHVM_RC_INT_SAME(FB_COMPACT_SERIALIZE_KEYSETS);
    HHVM_RC_INT_SAME(FB_COMPACT_SERIALIZE_KEEP_CLASSES);

    HHVM_FE(fb_serialize);
    HHVM_FE(fb_unserialize);
    HHVM_FE(fb_compact_serialize);
    HHVM_FE(fb_compact_unserialize);
    HHVM_FE(fb_utf8ize);
    HHVM_FE(fb_utf8_strlen);
    HHVM_FE(fb_utf8_strlen_deprecated);
    HHVM_FE(fb_utf8_substr);
    HHVM_FE(fb_utf8_decompose);
    HHVM_FE(fb_intercept2);
    HHVM_FE(fb_intercept2_named_args);
    HHVM_FE(fb_rename_function);
    HHVM_FE(fb_get_code_coverage);
    HHVM_FE(fb_enable_code_coverage);
    HHVM_FE(fb_disable_code_coverage);
    HHVM_FE(fb_output_compression);
    HHVM_FE(fb_set_exit_callback);
    HHVM_FE(fb_get_last_flush_size);
    HHVM_FE(fb_lazy_lstat);
    HHVM_FE(fb_lazy_realpath);

    HHVM_FALIAS(HH\\disable_code_coverage_with_frequency,
                HH_disable_code_coverage_with_frequency);
    HHVM_FALIAS(HH\\non_crypto_md5_upper, HH_non_crypto_md5_upper);
    HHVM_FALIAS(HH\\non_crypto_md5_lower, HH_non_crypto_md5_lower);
    HHVM_FALIAS(HH\\int_mul_overflow, HH_int_mul_overflow);
    HHVM_FALIAS(HH\\int_mul_add_overflow, HH_int_mul_add_overflow);

    HHVM_FALIAS(HH\\set_product_attribution_id, set_product_attribution_id);
    HHVM_FALIAS(HH\\set_product_attribution_id_deferred, set_product_attribution_id_deferred);
    HHVM_FALIAS(HH\\get_product_attribution_id_internal, get_product_attribution_id_internal);

    HHVM_FE(fb_call_user_func_array_async);
    HHVM_FE(fb_check_user_func_async);
    HHVM_FE(fb_end_user_func_async);
    HHVM_FE(fb_gen_user_func_array);

    HHVM_FE(extension_warmup_data);
  }
} s_fb_extension;

///////////////////////////////////////////////////////////////////////////////
}
