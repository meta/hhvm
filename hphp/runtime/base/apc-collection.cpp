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
#include "hphp/runtime/base/apc-collection.h"

#include "hphp/runtime/base/apc-object.h"
#include "hphp/runtime/base/apc-array.h"
#include "hphp/runtime/base/apc-stats.h"
#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/base/type-object.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/base/data-walker.h"

namespace HPHP {

namespace {

//////////////////////////////////////////////////////////////////////

void fillMap(BaseMap* map, const APCArray* apc, bool pure) {
  assertx(apc->isHashed());
  for (auto i = uint32_t{0}; i < apc->size(); ++i) {
    map->set(*apc->getHashedKey(i)->toLocal(true /* irrelevant for arraykey */).asTypedValue(),
             *apc->getHashedVal(i)->toLocal(pure).asTypedValue());
  }
}

void fillSet(BaseSet* coll, const APCArray* apc, bool pure) {
  assertx(apc->isHashed());
  for (auto i = uint32_t{0}; i < apc->size(); ++i) {
    coll->add(*apc->getHashedVal(i)->toLocal(pure).asTypedValue());
  }
}

void fillVector(BaseVector* coll, const APCArray* apc, bool pure) {
  assertx(apc->isPacked());
  for (auto i = uint32_t{0}; i < apc->size(); ++i) {
    coll->add(*apc->getPackedVal(i)->toLocal(pure).asTypedValue());
  }
}

// Deserializing an array could give back a different ArrayKind than we need,
// so we have to go with the slow case of calling a collection constructor.
NEVER_INLINE
Object createFromSerialized(CollectionType colType, APCHandle* handle, bool pure) {
  auto const col = Object::attach(collections::alloc(colType));
  auto const arr = handle->toLocal(pure);
  switch (colType) {
  case CollectionType::ImmVector:
  case CollectionType::Vector:
    static_cast<BaseVector*>(col.get())->init(arr);
    break;
  case CollectionType::ImmSet:
  case CollectionType::Set:
    static_cast<BaseSet*>(col.get())->init(arr);
    break;
  case CollectionType::ImmMap:
  case CollectionType::Map:
    static_cast<BaseMap*>(col.get())->init(arr);
    break;
  case CollectionType::Pair:
    not_reached();
  }
  return col;
}

//////////////////////////////////////////////////////////////////////

}

APCHandle::Pair APCCollection::Make(const ObjectData* obj,
                                    APCHandleLevel level,
                                    bool unserializeObj,
                                    bool pure) {
  auto const ad = const_cast<ArrayData*>(collections::asArray(obj));
  if (!ad) {
    auto const ser = apc_serialize(Variant(const_cast<ObjectData*>(obj)), pure);
    return APCString::MakeSerializedObject(ser);
  }

  if (auto const value = APCTypedValue::HandlePersistent(ad)) {
    return WrapArray(value, obj->collectionType());
  }

  /*
   * Create an uncounted array if we can.
   *
   * If this collection is an OuterHandle, then we need to do a full check on
   * this array for things like circularity.  If we're an InnerHandle, someone
   * already checked that, but we want to check for whether it's uncounted to
   * use a better representation.  For the OuterHandle case, we just delegate
   * to APCArray below (which will do the full DataWalker pass).
   */
  if (level == APCHandleLevel::Inner && apcExtension::UseUncounted) {
    DataWalker walker(DataWalker::LookupFeature::DetectNonPersistable);
    auto const features = walker.traverseData(ad);
    assertx(!features.isCircular);
    if (!features.hasNonPersistable) {
      auto const arr = APCArray::MakeSharedArray(ad, /*PointerMap*/nullptr, nullptr);
      auto const size = getMemSize(ad) + sizeof(APCTypedValue);
      return WrapArray({arr, size}, obj->collectionType());
    }
  }

  auto const arr = isVectorCollection(obj->collectionType())
    ? APCArray::MakeCopiedVec(ad, level, unserializeObj, pure)
    : APCArray::MakeCopiedDict(ad, level, unserializeObj, pure);
  return WrapArray(arr, obj->collectionType());
}

void APCCollection::Delete(APCHandle* h) {
  assertx(offsetof(APCCollection, m_handle) == 0);
  delete reinterpret_cast<APCCollection*>(h);
}

APCCollection::APCCollection()
  : m_handle(APCKind::CopiedCollection)
{}

APCCollection::~APCCollection() {
  // Zero for size is correct, because when this APCCollection was unreferenced
  // it already included the size of the inner handle.
  m_arrayHandle->unreferenceRoot(0);
}

APCHandle::Pair APCCollection::WrapArray(APCHandle::Pair inner,
                                         CollectionType colType) {
  auto const col = new APCCollection;
  col->m_arrayHandle = inner.handle;
  col->m_colType = colType;
  return { &col->m_handle, inner.size + sizeof(APCCollection) };
}

Object APCCollection::createObject(bool pure) const {
  if (m_arrayHandle->isTypedValue()) {
    Variant local(m_arrayHandle->toLocal(pure));
    assertx(local.isArray());
    return Object::attach(
      collections::alloc(m_colType, local.getArrayData())
    );
  }

  if (UNLIKELY(m_arrayHandle->kind() == APCKind::SerializedVec ||
               m_arrayHandle->kind() == APCKind::SerializedDict)) {
    return createFromSerialized(m_colType, m_arrayHandle, pure);
  }

  // We had a counted inner array---we need to do an O(N) copy to get the
  // collection into the request local heap.
  auto const apcArr = APCArray::fromHandle(m_arrayHandle);
  auto const col = Object::attach(collections::alloc(m_colType));
  switch (m_colType) {
  case CollectionType::ImmVector:
  case CollectionType::Vector:
    fillVector(static_cast<BaseVector*>(col.get()), apcArr, pure);
    break;
  case CollectionType::ImmSet:
  case CollectionType::Set:
    fillSet(static_cast<BaseSet*>(col.get()), apcArr, pure);
    break;
  case CollectionType::ImmMap:
  case CollectionType::Map:
    fillMap(static_cast<BaseMap*>(col.get()), apcArr, pure);
    break;
  case CollectionType::Pair:
    always_assert(0);
    break;
  }
  return col;
}

bool APCCollection::toLocalMayRaise() const {
  return m_arrayHandle->toLocalMayRaise();
}

//////////////////////////////////////////////////////////////////////

}
