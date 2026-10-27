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

#include "hphp/runtime/vm/native-data.h"

#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/memo-cache.h"
#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"

namespace HPHP::Native {
//////////////////////////////////////////////////////////////////////////////

using NativeDataInfoMap = std::unordered_map<const StringData*,NativeDataInfo>;
static NativeDataInfoMap s_nativedatainfo;

namespace {

// return the full native header size, which is also the distance from
// the allocated pointer to the ObjectData*.
size_t ndsize(size_t dataSize, size_t nMemoSlots) {
  if (UNLIKELY(nMemoSlots > 0)) {
    return alignTypedValue(
      alignTypedValue(sizeof(NativeNode)) +
      nMemoSlots * sizeof(MemoSlot) +
      dataSize
    );
  } else {
    return alignTypedValue(dataSize + sizeof(NativeNode));
  }
}

}

size_t ndsize(const ObjectData* obj, const NativeDataInfo* ndi) {
  auto cls = obj->getVMClass();
  if (cls == Generator::classof()) {
    assertx(!cls->hasMemoSlots());
    return Native::data<Generator>(obj)->resumable()->size() -
           sizeof(ObjectData);
  }
  if (cls == AsyncGenerator::classof()) {
    assertx(!cls->hasMemoSlots());
    return Native::data<AsyncGenerator>(obj)->resumable()->size() -
           sizeof(ObjectData);
  }
  return ndsize(ndi->sz, cls->numMemoSlots());
}

void registerNativeDataInfo(const StringData* name,
                            size_t sz,
                            NativeDataInfo::InitFunc init,
                            NativeDataInfo::CopyFunc copy,
                            NativeDataInfo::DestroyFunc destroy,
                            NativeDataInfo::SweepFunc sweep,
                            NativeDataInfo::SleepFunc sleep,
                            NativeDataInfo::WakeupFunc wakeup,
                            type_scan::Index tyindex,
                            uint8_t rt_attrs,
                            bool ctor_throws) {
  assertx(s_nativedatainfo.find(name) == s_nativedatainfo.end());
  assertx((sleep == nullptr && wakeup == nullptr) ||
         (sleep != nullptr && wakeup != nullptr));
  NativeDataInfo info;
  info.sz = sz;
  info.rt_attrs = rt_attrs;
  info.tyindex = tyindex;
  info.init = init;
  info.copy = copy;
  info.destroy = destroy;
  info.sweep = sweep;
  info.sleep = sleep;
  info.wakeup = wakeup;
  info.ctor_throws = ctor_throws;
  s_nativedatainfo[name] = info;
}

NativeDataInfo* getNativeDataInfo(const StringData* name) {
  auto it = s_nativedatainfo.find(name);
  if (it == s_nativedatainfo.end()) {
    return nullptr;
  }
  return &it->second;
}

/* Classes with NativeData structs allocate extra memory prior
 * to the ObjectData.
 *
 * [NativeNode][padding][memo slots][NativeData][ObjectData](prop0)...(propN)
 *                                              /\
 *                                              ObjectData* points here
 *
 * padding is added by alignTypedValue(sizeof(NativeData)) to ensure
 * that ObjectData* falls on a 16-aligned boundary. NativeData is
 * sizeof(NativeData) (NativeDataInfo.sz) bytes for the custom struct.
 * NativeNode is a link in the NativeData sweep list for this ND block
 */
template <bool Unlocked>
ObjectData* nativeDataInstanceCtor(Class* cls) {
  auto const ndi = cls->getNativeDataInfo();
  assertx(ndi);
  auto const nativeDataSize = ndsize(ndi->sz, cls->numMemoSlots());
  auto const nProps = cls->numDeclProperties();
  auto const size = ObjectData::sizeForNProps(nProps) + nativeDataSize;

  auto node = reinterpret_cast<NativeNode*>(
    tl_heap->objMalloc(size)
  );
  node->obj_offset = nativeDataSize;
  assertx(type_scan::isKnownType(ndi->tyindex));
  node->initHeader_32_16(HeaderKind::NativeData, 0, ndi->tyindex);
  auto const flags = Unlocked
    ? ObjectData::IsBeingConstructed
    : ObjectData::NoAttrs;
  auto obj = new (reinterpret_cast<char*>(node) + nativeDataSize)
    ObjectData(cls, flags, HeaderKind::NativeObject);
  assertx(obj->hasExactlyOneRef());

  if (UNLIKELY(cls->hasMemoSlots())) {
    auto cur = reinterpret_cast<MemoSlot*>(
        reinterpret_cast<char*>(node) + sizeof(NativeNode));
    auto end = reinterpret_cast<MemoSlot*>(
        reinterpret_cast<char*>(node) + nativeDataSize);
    while (cur < end) {
      (cur++)->init();
    }
  }

  if (ndi->init) {
    ndi->init(obj);
  }
  if (ndi->sweep) {
    tl_heap->addNativeObject(node);
  }
  return obj;
}

template ObjectData* nativeDataInstanceCtor<false>(Class*);
template ObjectData* nativeDataInstanceCtor<true>(Class*);

ObjectData* nativeDataInstanceCopyCtor(ObjectData* src, Class* cls,
                                       size_t nProps) {
  auto const ndi = cls->getNativeDataInfo();
  assertx(ndi);
  if (!ndi->copy) {
    throw_not_implemented("NativeDataInfoCopy");
  }
  auto const nativeDataSize = ndsize(src, ndi);
  auto node = reinterpret_cast<NativeNode*>(
    tl_heap->objMalloc(ObjectData::sizeForNProps(nProps) + nativeDataSize)
  );
  node->obj_offset = nativeDataSize;
  assertx(type_scan::isKnownType(ndi->tyindex));
  node->initHeader_32_16(HeaderKind::NativeData, 0, ndi->tyindex);
  auto obj = new (reinterpret_cast<char*>(node) + nativeDataSize)
    ObjectData(cls, ObjectData::InitRaw{}, ObjectData::NoAttrs,
      HeaderKind::NativeObject);
  assertx(obj->hasExactlyOneRef());

  if (UNLIKELY(cls->hasMemoSlots())) {
    auto cur = reinterpret_cast<MemoSlot*>(
        reinterpret_cast<char*>(node) + sizeof(NativeNode));
    auto end = reinterpret_cast<MemoSlot*>(
        reinterpret_cast<char*>(node) + nativeDataSize);
    while (cur < end) {
      (cur++)->init();
    }
  }

  if (ndi->init) {
    ndi->init(obj);
  }
  ndi->copy(obj, src);
  if (ndi->sweep) {
    tl_heap->addNativeObject(node);
  }
  return obj;
}

void nativeDataInstanceDtor(ObjectData* obj, const Class* cls) {
  obj->~ObjectData();

  auto const nProps = size_t{cls->numDeclProperties()};
  obj->props()->foreach(nProps, [&](tv_lval lval) {
    tvDecRefGen(lval);
  });

  auto ndi = cls->getNativeDataInfo();

  if (UNLIKELY(obj->getAttribute(ObjectData::UsedMemoCache))) {
    assertx(cls->hasMemoSlots());
    auto const nSlots = cls->numMemoSlots();
    for (Slot i = 0; i < nSlots; ++i) {
      auto slot = obj->memoSlotNativeData(i, ndi->sz);
      if (slot->isCache()) {
        if (auto cache = slot->getCache()) req::destroy_raw(cache);
      } else {
        tvDecRefGen(*slot->getValue());
      }
    }
  }

  if (ndi->destroy) {
    ndi->destroy(obj);
  }
  auto node = getNativeNode(obj, ndi);
  if (ndi->sweep) {
    tl_heap->removeNativeObject(node);
  }

  tl_heap->objFree(node, ObjectData::sizeForNProps(nProps) + ndsize(obj, ndi));
}

Variant nativeDataSleep(const ObjectData* obj) {
  auto ndi = obj->getVMClass()->getNativeDataInfo();
  assertx(ndi);
  assertx(ndi->sleep);
  return ndi->sleep(obj);
}

void nativeDataWakeup(ObjectData* obj, const Variant& data) {
  auto ndi = obj->getVMClass()->getNativeDataInfo();
  assertx(ndi);
  assertx(ndi->wakeup);
  ndi->wakeup(obj, data);
}

//////////////////////////////////////////////////////////////////////////////
} // namespace HPHP::Native
