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

#include "hphp/runtime/base/tv-shared.h"

#include "hphp/runtime/base/apc-stats.h"
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/func.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////////////

void* AllocShared(size_t bytes) {
  if (APCStats::IsCreated()) {
    APCStats::getAPCStats().addAPCSharedBlock();
  }
  return shared_malloc(bytes);
}

void FreeShared(void* ptr) {
  if (APCStats::IsCreated()) {
    APCStats::getAPCStats().removeAPCSharedBlock();
  }
  return shared_free(ptr);
}

void FreeShared(void* ptr, size_t bytes) {
  if (APCStats::IsCreated()) {
    APCStats::getAPCStats().removeAPCSharedBlock();
  }
  return shared_sized_free(ptr, bytes);
}

//////////////////////////////////////////////////////////////////////////////

void ConvertTvToShared(tv_lval source, const MakeSharedEnv& env) {
  auto& data = source.val();
  auto& type = source.type();

  switch (type) {
    case KindOfFunc:
      assertx(data.pfunc->isPersistent());
      break;

    case KindOfClass:
      if (data.pclass->isPersistent()) break;
      data.plazyclass = LazyClassData::create(data.pclass->name());
      type = KindOfLazyClass;
      break;

    case KindOfString:
      data.pstr = MakeSharedString(data.pstr, env);
      break;

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
      data.parr = MakeSharedArray(data.parr, env);
      break;

    case KindOfClsMeth:
      assertx(data.pclsmeth->getCls()->isPersistent());
      break;

    case KindOfUninit:
      type = KindOfNull;
      break;

    case KindOfLazyClass:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfEnumClassLabel:
      break;

    // DataWalker excludes these cases when it analyzes a value.
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfRClsMeth:
      always_assert(false);
  }
}

//////////////////////////////////////////////////////////////////////////////

ArrayData* MakeSharedArray(
    ArrayData* in, const MakeSharedEnv& env, bool hasApcTv) {
  if (in->persistentIncRef()) return in;

  if (in->empty()) {
    auto const legacy = in->isLegacyArray();
    switch (in->toDataType()) {
      case KindOfVec:
        return legacy ? staticEmptyMarkedVec() : staticEmptyVec();
      case KindOfDict:
        return legacy ? staticEmptyMarkedDictArray() : staticEmptyDictArray();
      case KindOfKeyset:
        return staticEmptyKeysetArray();
      default:
        always_assert(false);
    }
  }

  ArrayData** seenArr = nullptr;
  if (env.seenArrays && in->hasMultipleRefs()) {
    seenArr = &(*env.seenArrays)[in];
    auto arr = *seenArr;
    if (arr) {
      arr->sharedIncRef();
      return arr;
    }
  }

  auto const result = in->makeShared(env, hasApcTv);
  // NOTE: We may have mutated env.seen in makeShared, so we must redo
  // the hash table lookup here. We only use seenArr to test for presence.
  if (seenArr) (*env.seenArrays)[in] = result;
  return result;
}

StringData* MakeSharedString(StringData* in, const MakeSharedEnv& env) {
  if (in->persistentIncRef()) return in;
  if (in->empty()) return staticEmptyString();

  if (env.seenStrings) {
    auto it = env.seenStrings->find(in);
    if (it != env.seenStrings->end()) {
      auto st = *it;
      st->persistentIncRef();
      return st;
    }
  }

  auto st = lookupStaticString(in);
  if (!st) {
    st = StringData::MakeShared(in->slice());
  }

  if (env.seenStrings) env.seenStrings->insert(st);
  return st;
}

//////////////////////////////////////////////////////////////////////////////

void DecRefShared(TypedValue tv) {
  if (tvIsString(tv)) return DecRefSharedString(val(tv).pstr);
  if (tvIsArrayLike(tv)) return DecRefSharedArray(val(tv).parr);
  assertx(!isRefcountedType(type(tv)));
}

void DecRefSharedArray(ArrayData* ad) {
  assertx(!ad->isRefCounted());
  if (ad->isShared() && ad->sharedDecRef()) {
    ad->sharedFixCountForRelease();
    ad->releaseShared();
  }
}

void DecRefSharedString(StringData* sd) {
  assertx(!sd->isRefCounted());
  if (sd->isShared() && sd->sharedDecRef()) {
    sd->sharedFixCountForRelease();
    StringData::ReleaseShared(sd);
  }
}

//////////////////////////////////////////////////////////////////////////////

}
