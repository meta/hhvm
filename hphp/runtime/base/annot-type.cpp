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

#include "hphp/runtime/base/annot-type.h"

#include <folly/MapUtil.h>
#include "hphp/runtime/base/isame-log.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/hash-map.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

using HhvmStrToTypeMap = hphp_hash_map<
  const StringData*, AnnotType, string_data_hash, string_data_tsame
>;

using StdStrToTypeMap = hphp_string_tmap<AnnotType>;

const StaticString
  s_HH_Traversable("HH\\Traversable"),
  s_HH_KeyedTraversable("HH\\KeyedTraversable"),
  s_HH_Container("HH\\Container"),
  s_HH_KeyedContainer("HH\\KeyedContainer"),
  s_XHPChild("XHPChild"),
  s_Stringish("Stringish");

/**
 * This is the authoritative map that determines which typehints require
 * special handling. Any typehint not on this list is assumed to be normal
 * "class-name" typehint.
 */
static const std::pair<HhvmStrToTypeMap, StdStrToTypeMap>& getAnnotTypeMaps() {
  static const std::pair<HhvmStrToTypeMap, StdStrToTypeMap> mapPair = []() {
    std::pair<HhvmStrToTypeMap, StdStrToTypeMap> mappedPairs;
    struct Pair {
      const char* name;
      AnnotType type;
    };
    std::vector<Pair> pairs = {
      { annotTypeName(AnnotType::Nothing),   AnnotType::Nothing },
      { annotTypeName(AnnotType::NoReturn),  AnnotType::NoReturn },
      { annotTypeName(AnnotType::Null),      AnnotType::Null },
      { "HH\\void",                          AnnotType::Null },
      { annotTypeName(AnnotType::Bool),      AnnotType::Bool },
      { annotTypeName(AnnotType::Int),       AnnotType::Int },
      { annotTypeName(AnnotType::Float),     AnnotType::Float },
      { annotTypeName(AnnotType::String),    AnnotType::String },
      { annotTypeName(AnnotType::Object),    AnnotType::Object },
      { annotTypeName(AnnotType::Resource),  AnnotType::Resource },
      { annotTypeName(AnnotType::Mixed),     AnnotType::Mixed },
      { annotTypeName(AnnotType::Nonnull),   AnnotType::Nonnull },
      { annotTypeName(AnnotType::Number),    AnnotType::Number },
      { annotTypeName(AnnotType::ArrayKey),  AnnotType::ArrayKey },
      { annotTypeName(AnnotType::This),      AnnotType::This },
      { annotTypeName(AnnotType::Callable),  AnnotType::Callable },
      { annotTypeName(AnnotType::Vec),       AnnotType::Vec },
      { annotTypeName(AnnotType::Dict),      AnnotType::Dict },
      { annotTypeName(AnnotType::Keyset),    AnnotType::Keyset },
      { kAnnotTypeVarrayStr,                 AnnotType::Vec },
      { kAnnotTypeDarrayStr,                 AnnotType::Dict },
      { kAnnotTypeVarrayOrDarrayStr,         AnnotType::VecOrDict },
      { annotTypeName(AnnotType::VecOrDict), AnnotType::VecOrDict },
      { annotTypeName(AnnotType::ArrayLike), AnnotType::ArrayLike },
      { annotTypeName(AnnotType::Classname), AnnotType::Classname },
      // TODO(T199611023) revisit when we enforce the inner type
      { annotTypeName(AnnotType::Class),     AnnotType::Class },
      { annotTypeName(AnnotType::ClassOrClassname),
                                             AnnotType::ClassOrClassname },
    };
    for (unsigned i = 0; i < pairs.size(); ++i) {
      mappedPairs.first[makeStaticString(pairs[i].name)] = pairs[i].type;
      mappedPairs.second[pairs[i].name] = pairs[i].type;
    }
    return mappedPairs;
  }();
  return mapPair;
}

const AnnotType* nameToAnnotType(const StringData* typeName) {
  assertx(typeName);
  auto const& mapPair = getAnnotTypeMaps();
  return folly::get_ptr(mapPair.first, typeName);
}

namespace {

bool tsame(folly::StringPiece a, folly::StringPiece b) {
  return a.size() == b.size() && !tstrcmp(a.data(), b.data());
}

}

bool interface_supports_non_objects(const StringData* s) {
  return interface_supports_non_objects(s->slice());
}

bool interface_supports_non_objects(folly::StringPiece s) {
  return interface_supports_arrlike(s) ||
         tsame(s, s_Stringish.slice());
}

bool interface_supports_arrlike(const StringData* s) {
  return interface_supports_arrlike(s->slice());
}

bool interface_supports_arrlike(folly::StringPiece s) {
  return tsame(s, s_HH_Traversable.slice()) ||
         tsame(s, s_HH_KeyedTraversable.slice()) ||
         tsame(s, s_HH_Container.slice()) ||
         tsame(s, s_HH_KeyedContainer.slice()) ||
         tsame(s, s_XHPChild.slice());
}

bool interface_supports_string(const StringData* s) {
  return interface_supports_string(s->slice());
}

bool interface_supports_string(folly::StringPiece s) {
  return tsame(s, s_XHPChild.slice()) ||
         tsame(s, s_Stringish.slice());
}

bool interface_supports_int(const StringData* s) {
  return interface_supports_int(s->slice());
}

bool interface_supports_int(folly::StringPiece s) {
  return tsame(s, s_XHPChild.slice());
}

bool interface_supports_double(const StringData* s) {
  return interface_supports_double(s->slice());
}

bool interface_supports_double(folly::StringPiece s) {
  return tsame(s, s_XHPChild.slice());
}

///////////////////////////////////////////////////////////////////////////////

TypedValue annotDefaultValue(AnnotType at) {
  switch (at) {
    case AnnotType::Mixed:
    case AnnotType::This:
    case AnnotType::Callable:
    case AnnotType::Resource:
    case AnnotType::Object:
    case AnnotType::SubObject:
    case AnnotType::Unresolved:
    case AnnotType::Nothing:
    case AnnotType::NoReturn:
    case AnnotType::Classname:
    case AnnotType::Class:
    case AnnotType::ClassOrClassname:
    case AnnotType::Null:     return make_tv<KindOfNull>();
    case AnnotType::Nonnull:
    case AnnotType::Number:
    case AnnotType::ArrayKey:
    case AnnotType::Int:      return make_tv<KindOfInt64>(0);
    case AnnotType::Bool:     return make_tv<KindOfBoolean>(false);
    case AnnotType::Float:    return make_tv<KindOfDouble>(0);
    case AnnotType::ArrayLike:
    case AnnotType::VecOrDict:
    case AnnotType::Vec:
      return make_tv<KindOfPersistentVec>(staticEmptyVec());
    case AnnotType::String:
      return make_tv<KindOfPersistentString>(staticEmptyString());
    case AnnotType::Dict:
      return make_tv<KindOfPersistentDict>(staticEmptyDictArray());
    case AnnotType::Keyset:
      return make_tv<KindOfPersistentKeyset>(staticEmptyKeysetArray());
  }
  always_assert(false);
}

AnnotAction
annotCompat(DataType dt, AnnotType at, const StringData* annotClsName) {
  assertx(IMPLIES(at == AnnotType::SubObject, annotClsName != nullptr));
  assertx(IMPLIES(at == AnnotType::Unresolved, annotClsName != nullptr));

  auto const metatype = getAnnotMetaType(at);
  switch (metatype) {
    case AnnotMetaType::Mixed:
      return AnnotAction::Pass;
    case AnnotMetaType::Nonnull:
      return (dt == KindOfNull) ? AnnotAction::Fail : AnnotAction::Pass;
    case AnnotMetaType::Number:
      return (isIntType(dt) || isDoubleType(dt))
        ? AnnotAction::Pass : AnnotAction::Fail;
    case AnnotMetaType::ArrayKey:
      if (isClassType(dt)) {
        return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
          ? AnnotAction::WarnClassToString : AnnotAction::ConvertClassToString;
      }
      if (isLazyClassType(dt)) {
        return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
          ? AnnotAction::WarnLazyClassToString
          : AnnotAction::ConvertLazyClassToString;
      }
      return (isIntType(dt) || isStringType(dt))
        ? AnnotAction::Pass : AnnotAction::Fail;
    case AnnotMetaType::This:
      // For "this", if `dt' is not an object we know
      // it's not compatible, otherwise more checks are required
      return (dt == KindOfObject)
        ? AnnotAction::ObjectCheck
        : AnnotAction::Fail;
    case AnnotMetaType::Callable:
      // For "callable", if `dt' is not string/array/object/func we know
      // it's not compatible, otherwise more checks are required
      return (isStringType(dt) || isVecType(dt) || isDictType(dt) ||
              isFuncType(dt) || dt == KindOfObject || isClsMethType(dt) ||
              isRFuncType(dt) || isRClsMethType(dt))
        ? AnnotAction::CallableCheck : AnnotAction::Fail;
    case AnnotMetaType::VecOrDict:
      return (isVecType(dt) || isDictType(dt))
        ? AnnotAction::Pass
        : AnnotAction::Fail;
    case AnnotMetaType::ArrayLike:
      return isArrayLikeType(dt) ? AnnotAction::Pass : AnnotAction::Fail;
    case AnnotMetaType::Classname:
      if (isStringType(dt)) return AnnotAction::Pass;
      if (isClassType(dt) || isLazyClassType(dt)) {
        if (!Cfg::Eval::ClassPassesClassname) {
          return AnnotAction::Fail;
        }
        return Cfg::Eval::ClassnameNoticesSampleRate > 0 ?
          AnnotAction::WarnClassname : AnnotAction::Pass;
      }
      return AnnotAction::Fail;
    case AnnotMetaType::Class:
      // TODO(T199611023) add more levels
      if (isClassType(dt) || isLazyClassType(dt)) return AnnotAction::Pass;
      if (isStringType(dt)) {
        if (Cfg::Eval::ClassTypeLevel > 0) {
          return AnnotAction::Fail;
        }
        return Cfg::Eval::ClassNoticesSampleRate > 0 ?
          AnnotAction::WarnClass : AnnotAction::Pass;
      }
      return AnnotAction::Fail;
    case AnnotMetaType::ClassOrClassname:
      if (isStringType(dt)|| isLazyClassType(dt) || isClassType(dt))
        return AnnotAction::Pass;
      return AnnotAction::Fail;
    case AnnotMetaType::Nothing:
    case AnnotMetaType::NoReturn:
      return AnnotAction::Fail;
    case AnnotMetaType::Precise:
    case AnnotMetaType::SubObject:
    case AnnotMetaType::Unresolved:
      break;
  }

  assertx(metatype == AnnotMetaType::Precise ||
          metatype == AnnotMetaType::SubObject ||
          metatype == AnnotMetaType::Unresolved);
  if (at == AnnotType::String && dt == KindOfClass) {
    return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
      ? AnnotAction::WarnClassToString : AnnotAction::ConvertClassToString;
  }
  if (at == AnnotType::String && dt == KindOfLazyClass) {
    return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
      ? AnnotAction::WarnLazyClassToString
      : AnnotAction::ConvertLazyClassToString;
  }

  if (metatype == AnnotMetaType::Precise) {
    // If `at' is "bool", "int", "float", "string", "vec", "dict", "keyset",
    // "object", or "resource", then equivDataTypes() can definitively tell us
    // whether or not `dt' is compatible.
    return equivDataTypes(getAnnotDataType(at), dt)
      ? AnnotAction::Pass : AnnotAction::Fail;
  }

  assertx(annotClsName != nullptr);
  if (dt == KindOfObject) return AnnotAction::ObjectCheck;

  // If `dt' is not an object, check for "magic" interfaces that
  // support non-object datatypes
  if (interface_supports_non_objects(annotClsName)) {
    switch (dt) {
      case KindOfInt64:
        return interface_supports_int(annotClsName)
          ? AnnotAction::Pass : AnnotAction::Fail;
      case KindOfDouble:
        return interface_supports_double(annotClsName)
          ? AnnotAction::Pass : AnnotAction::Fail;
      case KindOfPersistentString:
      case KindOfString:
        return interface_supports_string(annotClsName)
          ? AnnotAction::Pass : AnnotAction::Fail;
      case KindOfPersistentVec:
      case KindOfVec:
      case KindOfPersistentDict:
      case KindOfDict:
      case KindOfPersistentKeyset:
      case KindOfKeyset:
        return interface_supports_arrlike(annotClsName)
          ? AnnotAction::Pass : AnnotAction::Fail;
      case KindOfClass:
        if (interface_supports_string(annotClsName)) {
          return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
            ? AnnotAction::WarnClassToString : AnnotAction::ConvertClassToString;
        }
        return AnnotAction::Fail;
      case KindOfLazyClass:
        if (interface_supports_string(annotClsName)) {
          return Cfg::Eval::ClassStringHintNoticesSampleRate > 0
            ? AnnotAction::WarnLazyClassToString
            : AnnotAction::ConvertLazyClassToString;
        }
        return AnnotAction::Fail;
      case KindOfClsMeth:
        return AnnotAction::Fail;
      case KindOfFunc:
      case KindOfRClsMeth:
      case KindOfRFunc:
      case KindOfUninit:
      case KindOfNull:
      case KindOfBoolean:
      case KindOfResource:
      case KindOfEnumClassLabel:
        return AnnotAction::Fail;
      case KindOfObject:
        not_reached();
    }
  }

  if (at == AnnotType::SubObject) return AnnotAction::Fail;

  assertx(at == AnnotType::Unresolved);
  return isClassType(dt) || isLazyClassType(dt)
    ? AnnotAction::FallbackCoerce : AnnotAction::Fallback;
}

///////////////////////////////////////////////////////////////////////////////

const char* annotName(AnnotType at) {
  switch (at) {
    case AnnotType::Mixed:      return "mixed";
    case AnnotType::This:       return "this";
    case AnnotType::Callable:   return "callable";
    case AnnotType::Resource:   return "resource";
    case AnnotType::Object:     return "object";
    case AnnotType::SubObject:  return "subobject";
    case AnnotType::Unresolved: return "unresolved";
    case AnnotType::Nothing:    return "nothing";
    case AnnotType::NoReturn:   return "noreturn";
    case AnnotType::Classname:  return "classname";
    case AnnotType::Class:      return "class";
    case AnnotType::ClassOrClassname: return "class_or_classname";
    case AnnotType::Null:       return "null";
    case AnnotType::Nonnull:    return "nonnull";
    case AnnotType::Number:     return "number";
    case AnnotType::ArrayKey:   return "arraykey";
    case AnnotType::Int:        return "int";
    case AnnotType::Bool:       return "bool";
    case AnnotType::Float:      return "float";
    case AnnotType::ArrayLike:  return "arraylike";
    case AnnotType::VecOrDict:  return "vec-or-dict";
    case AnnotType::Vec:        return "vec";
    case AnnotType::String:     return "string";
    case AnnotType::Dict:       return "dict";
    case AnnotType::Keyset:     return "keyset";
  }
  always_assert(false);
}

///////////////////////////////////////////////////////////////////////////////

}
