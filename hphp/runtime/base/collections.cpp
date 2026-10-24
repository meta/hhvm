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
#include "hphp/runtime/base/collections.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/variable-unserializer.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"

namespace HPHP::collections {
/////////////////////////////////////////////////////////////////////////////

#define X(type) const StaticString s_##type("HH\\" #type);
COLLECTIONS_ALL_TYPES(X)
#undef X

/////////////////////////////////////////////////////////////////////////////
// Constructor/Initializer

#define X(type)                                    \
ObjectData* allocEmpty##type() {                   \
  return req::make<c_##type>().detach();           \
}                                                  \
ObjectData* allocFromArray##type(ArrayData* arr) { \
  return req::make<c_##type>(arr).detach();        \
}
COLLECTIONS_PAIRED_TYPES(X)
#undef X

newEmptyInstanceFunc allocEmptyFunc(CollectionType ctype) {
  switch (ctype) {
#define X(type) case CollectionType::type: return allocEmpty##type;
COLLECTIONS_PAIRED_TYPES(X)
#undef X
    case CollectionType::Pair: not_reached();
  }
  not_reached();
}

newFromArrayFunc allocFromArrayFunc(CollectionType ctype) {
  switch (ctype) {
#define X(type) case CollectionType::type: return allocFromArray##type;
COLLECTIONS_PAIRED_TYPES(X)
#undef X
    case CollectionType::Pair: not_reached();
  }
  not_reached();
}

ObjectData* alloc(CollectionType ctype, ArrayData* arr) {
  if (!arr->isVanilla()) {
    auto const vanilla = BespokeArray::ToVanilla(arr, "collections::alloc");
    decRefArr(arr);
    arr = vanilla;
  }
  return allocFromArrayFunc(ctype)(arr);
}

ObjectData* allocPair(TypedValue c1, TypedValue c2) {
  return req::make<c_Pair>(c1, c2, c_Pair::NoIncRef{}).detach();
}

/////////////////////////////////////////////////////////////////////////////
// Misc

bool isType(const Class* cls, CollectionType ctype) {
  switch (ctype) {
#define X(type) case CollectionType::type: return cls == c_##type::classof();
COLLECTIONS_ALL_TYPES(X)
#undef X
  }
  not_reached();
}

/////////////////////////////////////////////////////////////////////////////
// Casting and Copying

template <IntishCast IC>
Array toArray(const ObjectData* obj) {
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
#define X(type) case CollectionType::type: return c_##type::ToArray<IC>(obj);
COLLECTIONS_ALL_TYPES(X)
#undef X
  }
  not_reached();
}

template
Array toArray<IntishCast::None>(const ObjectData*);
template
Array toArray<IntishCast::Cast>(const ObjectData*);

bool toBool(const ObjectData* obj) {
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
#define X(type) case CollectionType::type: return c_##type::ToBool(obj);
COLLECTIONS_ALL_TYPES(X)
#undef X
  }
  not_reached();
}

ObjectData* clone(ObjectData* obj) {
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
#define X(type) case CollectionType::type: return c_##type::Clone(obj);
COLLECTIONS_ALL_TYPES(X)
#undef X
  }
  not_reached();
}

ArrayData* asArray(ObjectData* obj) {
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
  case CollectionType::ImmVector:
  case CollectionType::Vector:
    return static_cast<BaseVector*>(obj)->arrayData();
  case CollectionType::ImmMap:
  case CollectionType::Map:
  case CollectionType::ImmSet:
  case CollectionType::Set:
    return static_cast<HashCollection*>(obj)->arrayData()->asArrayData();
  case CollectionType::Pair:
    return nullptr;
  }
  not_reached();
}

void replaceArray(ObjectData* obj, ArrayData* ad) {
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
  case CollectionType::ImmVector:
  case CollectionType::Vector: {
    auto const collection = static_cast<BaseVector*>(obj);
    decRefArr(collection->arrayData());
    return collection->setArrayData(ad);
  }
  case CollectionType::ImmMap:
  case CollectionType::Map:
  case CollectionType::ImmSet:
  case CollectionType::Set: {
    auto const collection = static_cast<HashCollection*>(obj);
    decRefArr(collection->arrayData()->asArrayData());
    return collection->setArrayData(VanillaDict::as(ad));
  }
  case CollectionType::Pair:
    always_assert(false);
  }
  not_reached();
}

/////////////////////////////////////////////////////////////////////////////
// Deep Copy

namespace {

ArrayData* deepCopyArrayLike(ArrayData* arr) {
  assertx(!arr->isKeysetType());
  Array ar(arr);
  IterateKV(
    arr,
    [&](TypedValue k, TypedValue v) {
      if (!isRefcountedType(v.m_type)) return false;
      Variant value{tvAsCVarRef(&v)};
      deepCopy(value.asTypedValue());
      if (value.asTypedValue()->m_data.num != v.m_data.num) {
        ar.set(k, *value.asTypedValue(), true);
      }
      return false;
    }
  );
  return ar.detach();
}

}  // namespace

void deepCopy(tv_lval lval) {
  switch (type(lval)) {
    DT_UNCOUNTED_CASE:
    case KindOfString:
    case KindOfResource:
    case KindOfKeyset:
    case KindOfRClsMeth:
    case KindOfRFunc:
      return;

    case KindOfVec:
    case KindOfDict: {
      auto& original = val(lval).parr;
      if (!original->isRefCounted()) return;
      auto arr = deepCopyArrayLike(original);
      decRefArr(original);
      original = arr;
      return;
    }

    case KindOfObject: {
      auto& original = val(lval).pobj;
      auto obj = original;
      if (!obj->isCollection()) return;
      const auto copyVector = [](BaseVector* vec) {
        const auto size = vec->size();
        if (size > 0 && vec->arrayData()->isRefCounted()) {
          vec->mutate();
          int64_t i = 0;
          do {
            deepCopy(vec->lvalAt(i));
          } while (++i < size);
        }
        return vec;
      };
      const auto copyMap = [](BaseMap* mp) {
        if (mp->size() > 0 && mp->arrayData()->isRefCounted()) {
          mp->mutate();
          auto used = mp->posLimit();
          for (uint32_t i = 0; i < used; ++i) {
            if (mp->isTombstone(i)) continue;
            auto* e = &mp->data()[i];
            deepCopy(&e->data);
          }
        }
        return mp;
      };
      switch (obj->collectionType()) {
        case CollectionType::Pair: {
          auto pair = c_Pair::Clone(static_cast<c_Pair*>(obj));
          Object o = Object::attach(pair);
          deepCopy(&pair->elm0);
          deepCopy(&pair->elm1);
          obj = o.detach();
          break;
        }
        case CollectionType::Vector:
          obj = copyVector(c_Vector::Clone(static_cast<c_Vector*>(obj)));
          break;
        case CollectionType::ImmVector:
          obj = copyVector(c_ImmVector::Clone(static_cast<c_ImmVector*>(obj)));
          break;
        case CollectionType::Map:
          obj = copyMap(c_Map::Clone(static_cast<c_Map*>(obj)));
          break;
        case CollectionType::ImmMap:
          obj = copyMap(c_ImmMap::Clone(static_cast<c_ImmMap*>(obj)));
          break;
        case CollectionType::Set:
          obj = c_Set::Clone(static_cast<c_Set*>(obj));
          break;
        case CollectionType::ImmSet:
          obj = c_ImmSet::Clone(static_cast<c_ImmSet*>(obj));
          break;
        default:
          assertx(false);
      }
      assertx(obj != original || original->hasMultipleRefs());
      decRefObj(original);
      original = obj;
      return;
    }
  }
  not_reached();
}

/////////////////////////////////////////////////////////////////////////////
// Read/Write access

namespace {

// Value types (everything except Object and Resource) are handled specially:
// any value-type members of an immutable collection are also immutable.
inline bool isValueType(DataType type) {
  return type != KindOfObject && type != KindOfResource;
}

template <bool throwOnMiss>
inline tv_lval atImpl(ObjectData* obj, const TypedValue* key) {
  switch (obj->collectionType()) {
#define X(type) case CollectionType::type: \
                  return c_##type::OffsetAt<throwOnMiss>(obj, key);
COLLECTIONS_ALL_TYPES(X)
#undef X
  }
  return nullptr;
}

}  // namespace

tv_lval at(ObjectData* obj, const TypedValue* key) {
  return atImpl<true>(obj, key);
}
tv_lval get(ObjectData* obj, const TypedValue* key) {
  return atImpl<false>(obj, key);
}

tv_lval atLval(ObjectData* obj, const TypedValue* key) {
  tv_lval ret;
  switch (obj->collectionType()) {
    case CollectionType::Pair:
      ret = c_Pair::OffsetAt<true>(obj, key);
      break;

    case CollectionType::Vector: {
      ret = BaseVector::OffsetAt<true>(obj, key);
      // We're about to expose an element of a Vector in an lvalue context;
      // if the element is a value-type (anything other than objects and
      // resources) we need to sever any buffer sharing that might be going on
      auto* vec = static_cast<c_Vector*>(obj);
      if (UNLIKELY(!vec->canMutateBuffer() && isValueType(type(ret)))) {
        vec->mutate();
        ret = BaseVector::OffsetAt<true>(obj, key);
      }
      return ret;
    }
    case CollectionType::ImmVector:
      ret = BaseVector::OffsetAt<true>(obj, key);
      break;

    case CollectionType::Map: {
      ret = BaseMap::OffsetAt<true>(obj, key);
      // We're about to expose an element of a Map in an lvalue context;
      // if the element is a value-type (anything other than objects and
      // resources) we need to sever any buffer sharing that might be going on
      auto* mp = static_cast<c_Map*>(obj);
      if (UNLIKELY(!mp->canMutateBuffer() && isValueType(type(ret)))) {
        mp->mutate();
        ret = BaseMap::OffsetAt<true>(obj, key);
      }
      return ret;
    }
    case CollectionType::ImmMap:
      ret = BaseMap::OffsetAt<true>(obj, key);
      break;

    case CollectionType::Set:
    case CollectionType::ImmSet:
      BaseSet::throwNoMutableIndexAccess();

  }
  // Value-type elements (anything other than objects and resources) of
  // an immutable collection "inherit" the collection's immutable status.
  // We do not allow value-type elements of an immutable collection to
  // be read in an "lvalue" context in order to prevent null->array
  // promotion, null->stdClass promotion, and mutating strings or arrays
  // in place (see "test/slow/collection_classes/invalid-operations.php"
  // for examples).
  if (isValueType(type(ret))) {
    throw_cannot_modify_immutable_object(obj->getClassName().data());
  }
  return ret;
}

tv_lval atRw(ObjectData* obj, const TypedValue* key) {
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      // Since we're exposing an element of a Vector in an read/write context,
      // we need to sever any buffer sharing that might be going on.
      static_cast<c_Vector*>(obj)->mutate();
      return BaseVector::OffsetAt<true>(obj, key);
    case CollectionType::Map:
      static_cast<c_Map*>(obj)->mutate();
      return BaseMap::OffsetAt<true>(obj, key);
    case CollectionType::Set:
    case CollectionType::ImmSet:
      BaseSet::throwNoMutableIndexAccess();
    case CollectionType::ImmVector:
    case CollectionType::ImmMap:
    case CollectionType::Pair:
      throw_cannot_modify_immutable_object(obj->getClassName().data());
  }
  return nullptr;
}

bool contains(ObjectData* obj, const Variant& offset) {
  auto* key = offset.asTypedValue();
  switch (obj->collectionType()) {
    case CollectionType::Vector:
    case CollectionType::ImmVector:
      return BaseVector::OffsetContains(obj, key);
    case CollectionType::Map:
    case CollectionType::ImmMap:
      return BaseMap::OffsetContains(obj, key);
    case CollectionType::Set:
    case CollectionType::ImmSet:
      return BaseSet::OffsetContains(obj, key);
    case CollectionType::Pair:
      return c_Pair::OffsetContains(obj, key);
  }
  not_reached();
}

bool isset(ObjectData* obj, const TypedValue* key) {
  switch (obj->collectionType()) {
    case CollectionType::Vector:
    case CollectionType::ImmVector:
      return BaseVector::OffsetIsset(obj, key);
    case CollectionType::Map:
    case CollectionType::ImmMap:
      return BaseMap::OffsetIsset(obj, key);
    case CollectionType::Set:
    case CollectionType::ImmSet:
      return BaseSet::OffsetIsset(obj, key);
    case CollectionType::Pair:
      return c_Pair::OffsetIsset(obj, key);
  }
  not_reached();
}

void unset(ObjectData* obj, const TypedValue* key) {
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      c_Vector::OffsetUnset(obj, key);
      break;
    case CollectionType::Map:
      c_Map::OffsetUnset(obj, key);
      break;
    case CollectionType::Set:
      BaseSet::OffsetUnset(obj, key);
      break;
    case CollectionType::ImmVector:
    case CollectionType::ImmMap:
    case CollectionType::ImmSet:
    case CollectionType::Pair:
      throw_cannot_modify_immutable_object(obj->getClassName().data());
  }
}

void append(ObjectData* obj, TypedValue* val) {
  assertx(val->m_type != KindOfUninit);
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      static_cast<c_Vector*>(obj)->add(*val);
      break;
    case CollectionType::Map:
      SystemLib::throwInvalidArgumentExceptionObject("Cannot append to Map");
    case CollectionType::Set:
      static_cast<c_Set*>(obj)->add(tvClassToString(*val));
      break;
    case CollectionType::ImmVector:
    case CollectionType::ImmMap:
    case CollectionType::ImmSet:
    case CollectionType::Pair:
      throw_cannot_modify_immutable_object(obj->getClassName().data());
  }
}

void set(ObjectData* obj, const TypedValue* key, const TypedValue* val) {
  assertx(val->m_type != KindOfUninit);
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      c_Vector::OffsetSet(obj, key, val);
      break;
    case CollectionType::Map:
      BaseMap::OffsetSet(obj, key, val);
      break;
    case CollectionType::Set:
    case CollectionType::ImmSet:
      BaseSet::throwNoMutableIndexAccess();
    case CollectionType::ImmVector:
    case CollectionType::ImmMap:
    case CollectionType::Pair:
      throw_cannot_modify_immutable_object(obj->getClassName().data());
  }
}

bool equals(const ObjectData* obj1, const ObjectData* obj2) {
  assertx(isValidCollection(obj1->collectionType()));
  if (!obj2->isCollection()) return false;
  auto ct1 = obj1->collectionType();
  auto ct2 = obj2->collectionType();

  // we intentionally allow mutable/immutable versions of the same collection
  // type to compare equal
  if (isMapCollection(ct1)) {
    return isMapCollection(ct2) && BaseMap::Equals(obj1, obj2);
  } else if (isVectorCollection(ct1)) {
    return isVectorCollection(ct2) && BaseVector::Equals(obj1, obj2);
  } else if (isSetCollection(ct1)) {
    return isSetCollection(ct2) && BaseSet::Equals(obj1, obj2);
  } else {
    assertx(ct1 == CollectionType::Pair);
    return (ct2 == CollectionType::Pair) && c_Pair::Equals(obj1, obj2);
  }
}

Variant pop(ObjectData* obj) {
  assertx(obj->isCollection());
  assertx(isMutableCollection(obj->collectionType()));
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      return static_cast<c_Vector*>(obj)->pop();
    case CollectionType::Map:
      return static_cast<c_Map*>(obj)->pop();
    case CollectionType::Set:
      return static_cast<c_Set*>(obj)->pop();
    default:
      assertx(false);
  }
  not_reached();
}

Variant shift(ObjectData* obj) {
  assertx(obj->isCollection());
  assertx(isMutableCollection(obj->collectionType()));
  switch (obj->collectionType()) {
    case CollectionType::Vector:
      return static_cast<c_Vector*>(obj)->popFront();
    case CollectionType::Map:
      return static_cast<c_Map*>(obj)->popFront();
    case CollectionType::Set:
      return static_cast<c_Set*>(obj)->popFront();
    default:
      assertx(false);
  }
  not_reached();
}

/////////////////////////////////////////////////////////////////////////////
}
