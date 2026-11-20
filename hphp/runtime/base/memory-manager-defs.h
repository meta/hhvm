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

#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec-defs.h"

#include "hphp/runtime/vm/class-meth-data.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/rfunc-data.h"
#include "hphp/runtime/vm/rclass-meth-data.h"

#include "hphp/runtime/ext/asio/ext_asio.h"
#include "hphp/runtime/ext/asio/ext_await-all-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_concurrent-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_condition-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_external-thread-event-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_priority-bridge-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_reschedule-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_sleep-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/ext/collections/hash-collection.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/util/bitset-utils.h"
#include "hphp/util/configs/gc.h"

#include <algorithm>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

/*
 * Struct Slab encapsulates the header attached to each large block of memory
 * used for allocating smaller blocks. The header contains a start-bit map,
 * used for quickly locating the start of an object, given an interior pointer
 * (a pointer to somewhere in the body). The start-bit map marks the location
 * of the start of each object. One bit represents kSmallSizeAlign bytes.
 */
struct Slab : HeapObject {

  char* init() {
    auto const off = align(this + 1) - (char*)this + Cfg::GC::SlabStartOffset;
    assertx(off % kSmallSizeAlign == 0 && off + kMaxSmallSize <= kSlabSize);
    initHeader_32(HeaderKind::Slab, uint32_t(off));
    clearStarts();
    return start();
  }

  void setCachedStart(const void* p) {
    bitvec_set(startsCache_, start_index(p));
  }

  bool isCachedStart(const void* p) const {
    auto const i = start_index(p);
    auto const res = bitvec_test(startsCache_, i);
    if (debug && res) {
      auto const obj = static_cast<const HeapObject*>(p);
      always_assert_flog(
        ((unsigned)obj->kind()) < NumHeaderKinds,
        "Start bits should be unset for heap object with uninitialized header");
    }
    return res;
  }

  HeapObject* find(const void* ptr) const;

  static Slab* fromHeader(HeapObject* h) {
    assertx(h->kind() == HeaderKind::Slab);
    return reinterpret_cast<Slab*>(h);
  }

  size_t size() const {
    return kSlabSize;
  }

  static Slab* fromPtr(const void* p) {
    static_assert(kSlabAlign == kSlabSize, "");
    auto slab = reinterpret_cast<Slab*>(uintptr_t(p) & ~(kSlabAlign - 1));
    assertx(slab->kind() == HeaderKind::Slab);
    return slab;
  }

  static char* align(const void* addr) {
    auto const a = reinterpret_cast<intptr_t>(addr);
    auto const mask = Cfg::GC::SlabAllocAlign - 1;
    assertx((mask & (mask + 1)) == 0);
    return reinterpret_cast<char*>(a + (-a & mask));
  }

  char* start() { return (char*)this + m_aux32; }
  char* end() { return (char*)this + size(); }
  const char* start() const { return (const char*)this + m_aux32; }
  const char* end() const { return (const char*)this + size(); }

  struct InitMasks;

private:
  void clearStarts() {
    memset(startsCache_, 0, sizeof(startsCache_));
  }

  static size_t start_index(const void* p) {
    return ((size_t)p & (kSlabSize - 1)) / kSmallSizeAlign;
  }

private:
  static constexpr size_t kBitsPerStart = 64; // bits per starts_[] entry
  static constexpr auto kNumStarts = kSlabSize / kBitsPerStart /
                                     kSmallSizeAlign;

  // tables for bulk-initializing start bits in setStarts(). kNumMasks
  // covers the small size classes that span <= 64 start bits (up to 1K).
  static constexpr size_t kNumMasks = 20;
  static std::array<uint64_t,kNumMasks> masks_;
  static std::array<uint8_t,kNumMasks> shifts_;

  // Start-bit state:
  // 1 = a HeapObject starts at corresponding address AND it has had its header
  //     kind initialized at least once (not necessarily currently accurate but
  //     definitely does not contain never initialized data)
  // 0 = in the middle of an object OR free space OR a heap object with
  //     uninitialized headers OR a heap object not created through split tail
  uint64_t startsCache_[kNumStarts];
};

static_assert(kMaxSmallSize < kSlabSize - sizeof(Slab),
              "kMaxSmallSize must fit in Slab");
static_assert(kMaxSmallSize <= kSlabSize - 4096 -
              ((sizeof(Slab) + 4095) & ~size_t(4095)),
              "kMaxSmallSize must fit in Slab with SlabStartOffset");

inline const Resumable* resumable(const HeapObject* h) {
  assertx(h->kind() == HeaderKind::AsyncFuncFrame);
  auto native = static_cast<const NativeNode*>(h);
  return reinterpret_cast<const Resumable*>(
    (const char*)native + native->obj_offset - sizeof(Resumable)
  );
}

inline Resumable* resumable(HeapObject* h) {
  assertx(h->kind() == HeaderKind::AsyncFuncFrame);
  auto native = static_cast<NativeNode*>(h);
  return reinterpret_cast<Resumable*>(
    (char*)native + native->obj_offset - sizeof(Resumable)
  );
}

inline const c_Awaitable* asyncFuncWH(const HeapObject* h) {
  assertx(resumable(h)->actRec()->func()->isAsyncFunction());
  auto native = static_cast<const NativeNode*>(h);
  auto obj = reinterpret_cast<const c_Awaitable*>(
    (const char*)native + native->obj_offset
  );
  assertx(obj->headerKind() == HeaderKind::AsyncFuncWH);
  return obj;
}

inline c_Awaitable* asyncFuncWH(HeapObject* h) {
  assertx(resumable(h)->actRec()->func()->isAsyncFunction());
  auto native = static_cast<NativeNode*>(h);
  auto obj = reinterpret_cast<c_Awaitable*>(
    (char*)native + native->obj_offset
  );
  assertx(obj->headerKind() == HeaderKind::AsyncFuncWH);
  return obj;
}

inline const ObjectData* closureObj(const HeapObject* h) {
  assertx(h->kind() == HeaderKind::ClosureHdr);
  auto closure_hdr = static_cast<const ClosureHdr*>(h);
  auto obj = reinterpret_cast<const ObjectData*>(closure_hdr + 1);
  assertx(obj->headerKind() == HeaderKind::Closure);
  return obj;
}

inline ObjectData* closureObj(HeapObject* h) {
  assertx(h->kind() == HeaderKind::ClosureHdr);
  auto closure_hdr = static_cast<ClosureHdr*>(h);
  auto obj = reinterpret_cast<ObjectData*>(closure_hdr + 1);
  assertx(obj->headerKind() == HeaderKind::Closure);
  return obj;
}

inline ObjectData* memoObj(HeapObject* h) {
  assertx(h->kind() == HeaderKind::MemoData);
  auto hdr = static_cast<MemoNode*>(h);
  auto obj = reinterpret_cast<ObjectData*>((char*)hdr + hdr->objOff());
  assertx(obj->headerKind() == HeaderKind::Object);
  assertx(obj->getVMClass()->hasMemoSlots());
  assertx(hdr->objOff() == ObjectData::objOffFromMemoNode(obj->getVMClass()));
  return obj;
}

inline const ObjectData* memoObj(const HeapObject* h) {
  return memoObj(const_cast<HeapObject*>(h));
}

// if this header is one of the types that contains an ObjectData,
// return the (possibly inner ptr) ObjectData*
inline const ObjectData* innerObj(const HeapObject* h) {
  return isObjectKind(h->kind()) ? static_cast<const ObjectData*>(h) :
         h->kind() == HeaderKind::AsyncFuncFrame ? asyncFuncWH(h) :
         h->kind() == HeaderKind::NativeData ?
           Native::obj(static_cast<const NativeNode*>(h)) :
         h->kind() == HeaderKind::ClosureHdr ? closureObj(h) :
         h->kind() == HeaderKind::MemoData ? memoObj(h) :
         nullptr;
}

// constexpr version of MemoryManager::sizeClass.
// requires sizeof(T) <= kMaxSmallSizeLookup
template<class T> constexpr size_t sizeClass() {
  return kSizeIndex2Size[
    kSmallSize2Index[(sizeof(T)-1) >> kLgSmallSizeQuantum]
  ];
}

/*
 * Return the size of h in bytes, rounded up to size class if necessary.
 */
inline size_t allocSize(const HeapObject* h) {
  // Ordering depends on ext_wait-handle.h.
  static constexpr uint16_t waithandle_sizes[] = {
    sizeClass<c_StaticWaitHandle>(),
    0, /* AsyncFunction */
    sizeClass<c_AsyncGeneratorWaitHandle>(),
    0, /* AwaitAll */
    0, /* Concurrent */
    sizeClass<c_ConditionWaitHandle>(),
    sizeClass<c_RescheduleWaitHandle>(),
    sizeClass<c_SleepWaitHandle>(),
    sizeClass<c_ExternalThreadEventWaitHandle>(),
    sizeClass<c_PriorityBridgeWaitHandle>(),
  };

  // Ordering depends on header-kind.h.
  static constexpr uint16_t kind_sizes[] = {
    0, /* Vec */
    0, /* BespokeVec */
    0, /* Dict */
    0, /* BespokeDict */
    0, /* Keyset */
    0, /* BespokeKeyset */
    0, /* String */
    0, /* Resource */
    sizeClass<ClsMethData>(),
    sizeClass<RClsMethData>(),
    sizeClass<RFuncData>(),
    0, /* Object */
    0, /* NativeObject */
    0, /* WaitHandle */
    sizeClass<c_AsyncFunctionWaitHandle>(),
    0, /* AwaitAllWH */
    0, /* ConcurrentWH */
    0, /* Closure */
    sizeClass<c_Vector>(),
    sizeClass<c_Map>(),
    sizeClass<c_Set>(),
    sizeClass<c_Pair>(),
    sizeClass<c_ImmVector>(),
    sizeClass<c_ImmMap>(),
    sizeClass<c_ImmSet>(),
    0, /* AsyncFuncFrame */
    0, /* NativeData */
    0, /* ClosureHdr */
    0, /* MemoData */
    0, /* Cpp */
    0, /* SmallMalloc */
    0, /* BigMalloc */
    0, /* Free */
    0, /* Hole */
    sizeof(Slab), /* Slab */
  };
#define CHECKSIZE(knd, type) \
  static_assert(kind_sizes[(int)HeaderKind::knd] == sizeClass<type>(), #knd);
  CHECKSIZE(ClsMeth, ClsMethData)
  CHECKSIZE(RClsMeth, RClsMethData)
  CHECKSIZE(AsyncFuncWH, c_AsyncFunctionWaitHandle)
  CHECKSIZE(Vector, c_Vector)
  CHECKSIZE(Map, c_Map)
  CHECKSIZE(Set, c_Set)
  CHECKSIZE(Pair, c_Pair)
  CHECKSIZE(ImmVector, c_ImmVector)
  CHECKSIZE(ImmMap, c_ImmMap)
  CHECKSIZE(ImmSet, c_ImmSet)
  CHECKSIZE(RFunc, RFuncData)
  static_assert(kind_sizes[(int)HeaderKind::Slab] == sizeof(Slab), "");
#undef CHECKSIZE
#define CHECKSIZE(knd)\
  static_assert(kind_sizes[(int)HeaderKind::knd] == 0, #knd);
  CHECKSIZE(Vec)
  CHECKSIZE(Dict)
  CHECKSIZE(Keyset)
  CHECKSIZE(String)
  CHECKSIZE(Resource)
  CHECKSIZE(Object)
  CHECKSIZE(NativeObject)
  CHECKSIZE(WaitHandle)
  CHECKSIZE(AwaitAllWH)
  CHECKSIZE(ConcurrentWH)
  CHECKSIZE(Closure)
  CHECKSIZE(AsyncFuncFrame)
  CHECKSIZE(NativeData)
  CHECKSIZE(ClosureHdr)
  CHECKSIZE(MemoData)
  CHECKSIZE(Cpp)
  CHECKSIZE(SmallMalloc)
  CHECKSIZE(BigMalloc)
  CHECKSIZE(Free)
  CHECKSIZE(Hole)
#undef CHECKSIZE

  auto kind = h->kind();
  if (auto size = kind_sizes[(int)kind]) {
    return size;
  }

  // In the cases below, kinds that don't need size-class rounding will
  // return their size immediately; the rest break to the bottom, then
  // return a rounded-up size.
  size_t size = 0;
  switch (kind) {
    case HeaderKind::Vec:
    case HeaderKind::Dict:
    case HeaderKind::Keyset:
    case HeaderKind::BespokeVec:
    case HeaderKind::BespokeDict:
    case HeaderKind::BespokeKeyset:
      // An array's sizeIndex is m_aux16 >> 8. heapSize is computed from it.
      return static_cast<const ArrayData*>(h)->heapSize();
    case HeaderKind::String:
      // size = isRefCounted ? table[aux16] : m_size+C
      size = static_cast<const StringData*>(h)->heapSize();
      break;
    case HeaderKind::Resource:
      // size = h->aux16
      // [ResourceHdr][ResourceData subclass]
      size = static_cast<const ResourceHdr*>(h)->heapSize();
      break;
    case HeaderKind::Object:
    case HeaderKind::Closure:
      // size = h->m_cls->m_declProperties.m_extra * sz(TV) + sz(OD)
      // [ObjectData][props]
      size = static_cast<const ObjectData*>(h)->heapSize();
      break;
    case HeaderKind::WaitHandle: {
      // size = table[h->whkind & mask]
      // [ObjectData][subclass]
      auto obj = static_cast<const ObjectData*>(h);
      auto whKind = wait_handle<c_Awaitable>(obj)->getKind();
      size = waithandle_sizes[(int)whKind];
      assertx(size != 0); // AsyncFuncFrame, AwaitAllWH or ConcurrentWH
      assertx(size == MemoryManager::sizeClass(size));
      return size;
    }
    case HeaderKind::AsyncFuncFrame:
      // size = h->obj_offset + C // 32-bit
      // [NativeNode][locals][Resumable][c_AsyncFunctionWaitHandle]
      size = static_cast<const NativeNode*>(h)->obj_offset +
             sizeof(c_AsyncFunctionWaitHandle);
      break;
    case HeaderKind::AwaitAllWH:
      // size = h->m_cap * sz(Node) + sz(c_AAWH)
      // [ObjectData][children...]
      size = static_cast<const c_AwaitAllWaitHandle*>(h)->heapSize();
      break;
    case HeaderKind::ConcurrentWH:
      // size = h->m_cap * sz(Node) + sz(c_CCWH)
      // [ObjectData][children...]
      size = static_cast<const c_ConcurrentWaitHandle*>(h)->heapSize();
      break;
    case HeaderKind::NativeData: {
      // h->obj_offset + (h+h->obj_offset)->m_cls->m_extra * sz(TV) + sz(OD)
      // [NativeNode][Memo Slots][NativeData][ObjectData][props] is one alloc.
      // Generators -
      // [NativeNode][NativeData<locals><Resumable><GeneratorData>][ObjectData]
      auto native = static_cast<const NativeNode*>(h);
      size = native->obj_offset + Native::obj(native)->heapSize();
      break;
    }
    case HeaderKind::ClosureHdr:
      // size = h->aux32
      // [ClosureHdr][ObjectData][use vars]
      size = static_cast<const ClosureHdr*>(h)->size();
      break;
    case HeaderKind::MemoData:
      size = static_cast<const MemoNode*>(h)->objOff() + memoObj(h)->heapSize();
      break;
    case HeaderKind::Cpp:
    case HeaderKind::SmallMalloc: // [MallocNode][bytes...]
      // size = h->nbytes // 64-bit
      size = static_cast<const MallocNode*>(h)->nbytes;
      break;
    // the following sizes are intentionally not rounded up to size class.
    case HeaderKind::BigMalloc: // [MallocNode][raw bytes...]
      size = static_cast<const MallocNode*>(h)->nbytes;
      assertx(size != 0);
      // not rounded up to size class, because we never need to iterate over
      // more than one allocated block. avoid calling nallocx().
      return size;
    case HeaderKind::Free:
    case HeaderKind::Hole:
      size = static_cast<const FreeNode*>(h)->size();
      assertx(size != 0);
      // Free objects are guaranteed to be already size-class aligned.
      // Holes are not size-class aligned, so neither need to be rounded up.
      return size;
    case HeaderKind::Slab:
    case HeaderKind::NativeObject:
    case HeaderKind::AsyncFuncWH:
    case HeaderKind::Vector:
    case HeaderKind::Map:
    case HeaderKind::Set:
    case HeaderKind::Pair:
    case HeaderKind::ImmVector:
    case HeaderKind::ImmMap:
    case HeaderKind::ImmSet:
    case HeaderKind::ClsMeth:
    case HeaderKind::RClsMeth:
    case HeaderKind::RFunc:
      not_reached();
  }
  return MemoryManager::sizeClass(size);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Search from ptr backwards, for a HeapObject that contains ptr.
 * Returns the HeapObject* for the containing object, else nullptr.
 */
inline HeapObject* Slab::find(const void* ptr) const {
  auto const i = start_index(ptr);
  if (bitvec_test(startsCache_, i)) {
    return reinterpret_cast<HeapObject*>(
        uintptr_t(ptr) & ~(kSmallSizeAlign - 1)
    );
  }
  // compute a mask with 0s at i+1 and higher
  auto const mask = ~0ull >> (kBitsPerStart - 1 - i % kBitsPerStart);
  auto cursor = startsCache_ + i / kBitsPerStart;
  HeapObject* h = nullptr;
  for (auto bits = *cursor & mask;; bits = *cursor) {
    if (bits) {
      auto k = fls64(bits);
      h = reinterpret_cast<HeapObject*>(
        (char*)this +
        ((cursor - startsCache_) * kBitsPerStart + k) * kSmallSizeAlign
      );
      auto size = allocSize(h);
      if ((char*)ptr >= (char*)h + size) break;
      return h;
    }
    if (--cursor < startsCache_) break;
  }

  // `ptr` is past the end of the allocation pointed to by `h`.  Walk forward
  // until we pass `ptr` checking to see if we possibly have missing start
  // bits we should fill in.
  if (!h) return nullptr;
  auto size = allocSize(h);
  do {
    h = (HeapObject*)((char*)h + size);
    if ((char*)h >= end()) break;
    const_cast<Slab*>(this)->setCachedStart(h);
    size = allocSize(h);

    if ((char*) ptr < (char*)h + size) {
      return h;
    }
  } while ((char*)h < (char*)ptr);
  return nullptr;
}

inline HeapObject* MemoryManager::find(const void* p) {
  if (m_lastInitFreeAllocated != m_stats.mmAllocated()
      || m_lastInitFreeFreed != m_stats.mm_freed) {
    initFree();
  }
  if (auto const ptr = m_heap.find(p)) {
    if (UNLIKELY(ptr->kind() != HeaderKind::Slab)) return ptr;
    auto const slab = static_cast<Slab*>(ptr);
    auto const obj = slab->find(p);
    if (obj) return obj;
    return slab;
  }
  return nullptr;
}

template<class OnBig, class OnSlab>
void SparseHeap::iterate(OnBig onBig, OnSlab onSlab) {
  // slabs and bigs are sorted; walk through both in address order
  m_bigs.iterate([&](HeapObject* h, size_t size) {
    if (h->kind() == HeaderKind::Slab) {
      onSlab(h, size);
    } else {
      onBig(h, size);
    }
  });
}

template<class Fn> void SparseHeap::iterate(Fn fn) {
  // slabs and bigs are sorted; walk through both in address order
  m_bigs.iterate([&](HeapObject* big, size_t big_size) {
    HeapObject *h, *end;
    if (big->kind() == HeaderKind::Slab) {
      assertx((char*)big + big_size == Slab::fromHeader(big)->end());
      h = (HeapObject*)Slab::fromHeader(big)->start();
      end = (HeapObject*)Slab::fromHeader(big)->end();
    } else {
      h = big;
      end = nullptr; // ensure we don't loop below
    }
    do {
      auto size = allocSize(h);
      fn(h, size);
      h = (HeapObject*)((char*)h + size);
    } while (h < end);
    assertx(!end || h == end); // otherwise, last object was truncated
  });
}

template<class Fn> void MemoryManager::iterate(Fn fn) {
  m_heap.iterate([&](HeapObject* h, size_t allocSize) {
    assertx(m_stats.mm_freed == m_lastInitFreeFreed);
    if (h->kind() >= HeaderKind::Hole) {
      assertx(unsigned(h->kind()) < NumHeaderKinds);
      // no valid pointer can point here.
      return; // continue iterating
    }
    fn(h, allocSize);
  });
}

template<class Fn> void MemoryManager::forEachHeapObject(Fn fn) {
  initFree();
  iterate(fn);
}

template<class Fn> void MemoryManager::forEachObject(Fn fn) {
  if (debug) checkHeap("MemoryManager::forEachObject");
  std::vector<const ObjectData*> ptrs;
  forEachHeapObject([&](HeapObject* h, size_t) {
    if (auto obj = innerObj(h)) {
      ptrs.push_back(obj);
    }
  });
  for (auto ptr : ptrs) {
    fn(ptr);
  }
}

///////////////////////////////////////////////////////////////////////////////
}
