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
#include "hphp/runtime/base/enum-cache.h"
#include "hphp/runtime/base/tv-type.h"

#include <memory>

namespace HPHP {

//////////////////////////////////////////////////////////////////////

// initialize the cache
static EnumCache s_cache;

const StaticString s_enumName("Enum");

const EnumValues* EnumCache::getValues(const Class* klass,
                                       bool recurse) {
  if (UNLIKELY(klass->classVecLen() == 1 ||
               !s_enumName.get()->same(klass->classVec()[0]->name()))) {
    std::string msg;
    msg += klass->name()->data();
    msg += " must derive from Enum";
    EnumCache::failLookup(msg);
  }
  if (LIKELY(!recurse)) {
    if (auto const values = klass->getEnumValues()) return values;
  }
  return s_cache.getEnumValues(klass, recurse);
}

const EnumValues* EnumCache::getValuesBuiltin(const Class* klass) {
  assertx(isAnyEnum(klass));
  if (auto const values = klass->getEnumValues()) return values;
  return s_cache.getEnumValues(klass, false);
}

const EnumValues* EnumCache::getValuesStatic(const Class* klass) {
  assertx(isAnyEnum(klass));
  auto const result = [&]() -> const EnumValues* {
    if (auto const values = klass->getEnumValues()) return values;
    return s_cache.getEnumValues(klass, false, true);
  }();
  if (!result) return nullptr;
  assertx(result->names->isStatic());
  assertx(result->values->isStatic());
  // Sizes may mismatch if there are duplicate names or values.
  if (result->names->size() != result->values->size()) return nullptr;
  return result;
}

void EnumCache::deleteValues(const Class* klass) {
  // it's unlikely a class is in the cache so check first
  // without write lock
  if (s_cache.getEnumValuesIfDefined(getKey(klass, false), false) != nullptr) {
    s_cache.deleteEnumValues(getKey(klass, false));
  }
  if (s_cache.getEnumValuesIfDefined(getKey(klass, true), false) != nullptr) {
    s_cache.deleteEnumValues(getKey(klass, true));
  }
}

void EnumCache::failLookup(const Variant& msg) {
  SystemLib::throwExceptionObject(msg);
}

EnumCache::~EnumCache() {
  m_enumValuesMap.clear();
}

const EnumValues* EnumCache::cachePersistentEnumValues(
  const Class* klass,
  bool recurse,
  Array&& names,
  Array&& values) {
  assertx(names.isDict());
  assertx(values.isDict());

  std::unique_ptr<EnumValues> enums(new EnumValues());
  enums->values = ArrayData::GetScalarArray(std::move(values));
  enums->names = ArrayData::GetScalarArray(std::move(names));
  if (!recurse) {
    return const_cast<Class*>(klass)->setEnumValues(enums.release());
  }
  intptr_t key = getKey(klass, recurse);
  EnumValuesMap::accessor acc;
  if (!m_enumValuesMap.insert(acc, key)) {
    return acc->second;
  }
  // add to the map the newly created values
  acc->second = enums.release();
  return acc->second;
}

const EnumValues* EnumCache::cacheRequestEnumValues(
  const Class* klass,
  bool recurse,
  Array&& names,
  Array&& values) {

  assertx(names.isDict());
  assertx(values.isDict());

  m_nonScalarEnumValuesMap.bind(rds::Mode::Normal, rds::LinkID{"EnumCache"});
  if (!m_nonScalarEnumValuesMap.isInit()) {
    m_nonScalarEnumValuesMap.initWith(req::make_raw<ReqEnumValuesMap>());
  }
  auto& enumValuesData = *m_nonScalarEnumValuesMap;

  auto enums = req::make_raw<EnumValues>();
  enums->values = std::move(values);
  enums->names = std::move(names);

  intptr_t key = getKey(klass, recurse);
  enumValuesData->emplace(key, enums);

  return enums;
}

const EnumValues* EnumCache::loadEnumValues(
    const Class* klass, bool recurse, bool require_static) {
  auto const numConstants = klass->numConstants();
  auto values = Array::CreateDict();
  auto names = Array::CreateDict();
  auto const consts = klass->constants();
  bool persist = true;
  for (size_t i = 0; i < numConstants; i++) {
    if (consts[i].isAbstractAndUninit()
        || consts[i].kind() != ConstModifierFlags::Kind::Value) {
      continue;
    }
    // The outer condition below enables caching of enum constants defined
    // in enums included by the current class.
    if (!(isAnyEnum(klass)
        && klass->hasIncludedEnums()
        && klass->allIncludedEnums().contains(consts[i].cls->name()))) {
      if (consts[i].cls != klass && !recurse) {
        continue;
      }
    }
    TypedValue value = consts[i].val;
    // Handle dynamically set constants. We can't get a static value here.
    if (value.m_type == KindOfUninit) {
      if (require_static) return nullptr;
      persist = false;
      value = klass->clsCnsGet(consts[i].name);
    }
    assertx(value.m_type != KindOfUninit);
    bool isEnumClass = klass->attrs() & AttrEnumClass;
    if (!isEnumClass &&
        !(isIntType(value.m_type) || tvIsString(&value) ||
          tvIsClass(&value) || tvIsLazyClass(&value))) {
      // Enum values must be ints or strings. We can't get a static value here.
      if (require_static) return nullptr;
      std::string msg;
      msg += klass->name()->data();
      msg += " enum can only contain string and int values";
      EnumCache::failLookup(msg);
    }
    values.set(StrNR(consts[i].name), tvAsCVarRef(value));
    if (isEnumClass) {
      // The enum values of enum classes are objects. This makes it
      // such that we can't build `names` (which requires enum values be
      // either int or string as they serve as array keys there).
      continue; // So, the `names` member of the calculated
                // `EnumValues` will be empty. This is OK, since we
                // don't care to support `getNames` for enum classes.
    }
    // Manually perform int-like key coercion even if names is a dict for
    // backwards compatibility.
    int64_t n;
    auto const source = "enum";
    if (tvIsString(&value) &&
        value.m_data.pstr->isStrictlyInteger(n)) {
      names.set(n, make_tv<KindOfString>(consts[i].name));
    } else if (tvIsClass(&value)) {
      names.set(StrNR{classToStringHelper(val(value).pclass, source)},
                make_tv<KindOfString>(consts[i].name), true);
    } else if (tvIsLazyClass(&value)) {
      names.set(StrNR{lazyClassToStringHelper(val(value).plazyclass, source)},
                make_tv<KindOfString>(consts[i].name), true);
    } else {
      names.set(value, make_tv<KindOfString>(consts[i].name), true);
    }
  }

  assertx(names.isDict());
  assertx(values.isDict());

  // If we saw dynamic constants we cannot cache the enum values across requests
  // as they may not be the same in every request.
  return persist
    ? cachePersistentEnumValues(
      klass,
      recurse,
      std::move(names),
      std::move(values))
    : cacheRequestEnumValues(
      klass,
      recurse,
      std::move(names),
      std::move(values));
}

const EnumValues* EnumCache::getEnumValuesIfDefined(
  intptr_t key, bool checkLocal) const {
  EnumValuesMap::const_accessor acc;
  if (m_enumValuesMap.find(acc, key)) {
    return acc->second;
  }
  if (!checkLocal ||
      !m_nonScalarEnumValuesMap.bound() ||
      !m_nonScalarEnumValuesMap.isInit()) {
    return nullptr;
  }
  auto data = *m_nonScalarEnumValuesMap;
  auto it = data->find(key);
  if (it != data->end()) {
    return it->second;
  }
  return nullptr;
}

const EnumValues* EnumCache::getEnumValues(
    const Class* klass, bool recurse, bool require_static) {
  auto const values = getEnumValuesIfDefined(getKey(klass, recurse));
  if (values && require_static && !values->names->isStatic()) return nullptr;
  return values ? values : loadEnumValues(klass, recurse, require_static);
}

void EnumCache::deleteEnumValues(intptr_t key) {
  EnumValuesMap::accessor acc;
  if (m_enumValuesMap.find(acc, key)) {
    delete acc->second;
    m_enumValuesMap.erase(acc);
  }
}

//////////////////////////////////////////////////////////////////////

}
