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

#ifndef HPHP_LOGGING_PROFILE_H_
#define HPHP_LOGGING_PROFILE_H_

#include "hphp/runtime/base/bespoke/entry-types.h"
#include "hphp/runtime/base/bespoke/key-order.h"
#include "hphp/runtime/base/bespoke-runtime.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/type-alias.h"
#include "hphp/runtime/vm/jit/array-layout.h"

#include <fmt/format.h>
#include <folly/String.h>

#include <folly/container/F14Map.h>

#include <algorithm>
#include <atomic>

namespace HPHP::bespoke {

struct LoggingArray;

// The second entry in these tuples is an "is read operation" flag.
// This flag is set for ops that are guaranteed to preserve the array's layout,
// even if - like with the ToVArray op - they may update the array due to COW.
#define ARRAY_OPS \
  X(Scan,               true)  \
  X(EscalateToVanilla,  true)  \
  X(ConvertToShared,    true)  \
  X(ReleaseShared,      true)  \
  X(Release,            true)  \
  X(IsVectorData,       true)  \
  X(GetInt,             true)  \
  X(GetStr,             true)  \
  X(GetIntPos,          true)  \
  X(GetStrPos,          true)  \
  X(LvalInt,            false) \
  X(LvalStr,            false) \
  X(ElemInt,            false) \
  X(ElemStr,            false) \
  X(SetPos,             false) \
  X(SetInt,             false) \
  X(SetStr,             false) \
  X(ConstructInt,       false) \
  X(ConstructStr,       false) \
  X(RemoveInt,          false) \
  X(RemoveStr,          false) \
  X(APCInitInt,         true)  \
  X(APCInitStr,         true)  \
  X(IterBegin,          true)  \
  X(IterLast,           true)  \
  X(IterEnd,            true)  \
  X(IterAdvance,        true)  \
  X(IterRewind,         true)  \
  X(Append,             false) \
  X(Pop,                false) \
  X(PreSort,            true)  \
  X(PostSort,           true)  \
  X(SetLegacyArray,     true)

enum class ArrayOp : uint8_t {
#define X(name, read) name,
ARRAY_OPS
#undef X
};

// Internal storage detail of EventMap.
struct EventKey;

struct APCKey { size_t hash; };

using EventMap = folly::F14FastMap<uint64_t, CopyAtomic<size_t>>;

// The type of location a profile is attached to.
enum class LocationType : uint8_t {
  SrcKey,
  APCKey,
  InstanceProperty,
  StaticProperty,
  TypeConstant,
  Runtime,
  TypeAlias,
};

struct LoggingProfileKey {
  explicit LoggingProfileKey(SrcKey sk)
    : sk(sk)
    , slot(kInvalidSlot)
    , locationType(LocationType::SrcKey)
  {}

  explicit LoggingProfileKey(APCKey ak)
    : ak(ak)
    , slot(kInvalidSlot)
    , locationType(LocationType::APCKey)
  {}

  explicit LoggingProfileKey(RuntimeStruct* runtimeStruct)
    : runtimeStruct(runtimeStruct)
    , slot(kInvalidSlot)
    , locationType(LocationType::Runtime)
  {}

  explicit LoggingProfileKey(const Class* cls, Slot slot, LocationType loc)
    : cls(cls)
    , slot(slot)
    , locationType(loc)
  {}

  explicit LoggingProfileKey(const TypeAlias* ta)
    : ta(ta)
    , slot(kInvalidSlot)
    , locationType(LocationType::TypeAlias)
  {}

  bool operator==(const LoggingProfileKey& o) const {
    return locationType == o.locationType && ptr == o.ptr && slot == o.slot;
  }

  bool checkInvariants() const {
    DEBUG_ONLY auto const propOrCns =
      locationType == LocationType::InstanceProperty ||
      locationType == LocationType::StaticProperty ||
      locationType == LocationType::TypeConstant;
    assertx(propOrCns == (slot != kInvalidSlot));
    assertx(IMPLIES(
      locationType == LocationType::Runtime,
      runtimeStruct != nullptr));
    return true;
  }

  bool isRuntimeLocation() const {
    return locationType == LocationType::Runtime;
  }

  Optional<Op> op() const {
    switch (locationType) {
      case LocationType::SrcKey:
        return {sk.op()};
      case LocationType::InstanceProperty:
        return {Op::NewObjD};
      case LocationType::APCKey:
      case LocationType::Runtime:
      case LocationType::StaticProperty:
      case LocationType::TypeConstant:
      case LocationType::TypeAlias:
        return std::nullopt;
    }
    always_assert(false);
  }

  std::string toString() const {
    switch (locationType) {
      case LocationType::APCKey:
        return fmt::format("APC:{:08x}", ak.hash);
      case LocationType::SrcKey:
        return sk.getSymbol();
      case LocationType::InstanceProperty: {
        auto const& prop = cls->declProperties()[slot];
        return fmt::format("{}->{}", cls->name()->slice(), prop.name);
      }
      case LocationType::StaticProperty: {
        auto const& prop = cls->staticProperties()[slot];
        return fmt::format("{}::{}", cls->name()->slice(), prop.name);
      }
      case LocationType::TypeConstant: {
        auto const& cns = cls->constants()[slot];
        return fmt::format("{}::{}", cls->name()->slice(), cns.name);
      }
      case LocationType::TypeAlias:
        return fmt::format("{}", ta->name()->slice());
      case LocationType::Runtime:
        return runtimeStruct->toString()->toCppString();
    }
    always_assert(false);
  }

  std::string toStringDetail() const {
    switch (locationType) {
      case LocationType::SrcKey:
        return sk.showInst();
      case LocationType::InstanceProperty:
        return fmt::format("NewObjD \"{}\"", cls->name()->slice());
      case LocationType::APCKey:
      case LocationType::Runtime:
      case LocationType::StaticProperty:
      case LocationType::TypeConstant:
      case LocationType::TypeAlias:
        return toString();
    }
    always_assert(false);
  }

  size_t stableHash() const {
    auto const base = [&]{
      switch (locationType) {
        case LocationType::APCKey:
          return ak.hash;
        case LocationType::SrcKey:
          return SrcKey::StableHasher()(sk);
        case LocationType::InstanceProperty:
        case LocationType::StaticProperty:
        case LocationType::TypeConstant:
          return cls->stableHash() ^ slot;
        case LocationType::Runtime:
          return runtimeStruct->stableHash();
        case LocationType::TypeAlias:
          return ta->stableHash();
      }
      always_assert(false);
    }();
    return folly::hash::hash_combine(static_cast<size_t>(locationType), base);
  }

  union {
    SrcKey sk;
    APCKey ak;
    const Class* cls;
    RuntimeStruct* runtimeStruct;
    const TypeAlias* ta;
    uintptr_t ptr;
  };

  // The logical slot of a property on cls. kInvalidSlot for other types.
  Slot slot;
  LocationType locationType;
};

// We'll store a LoggingProfile for each array construction site SrcKey.
// It tracks the operations that happen on arrays coming from that site.
struct LoggingProfile {
  // Values in the event map are sampled event counts.
  using EventMap = folly::F14FastMap<uint64_t, CopyAtomic<size_t>>;

  // The first element of the key is the EntryTypes before the operation;
  // the second element is the EntryTypes after it.
  using EntryTypesMapKey = std::pair<uint16_t, uint16_t>;
  using EntryTypesMapHasher = pairHashCompare<uint16_t, uint16_t,
                                              integralHashCompare<uint16_t>,
                                              integralHashCompare<uint16_t>>;
  using EntryTypesMap = folly::F14FastMap<EntryTypesMapKey, CopyAtomic<size_t>,
                                          EntryTypesMapHasher>;

  // The content of the logging profile that can be freed after layout selection.
  struct LoggingProfileData {
    mutable folly::SharedMutex mapLock;
    std::atomic<uint64_t> sampleCount = 0;
    std::atomic<uint64_t> loggingArraysEmitted = 0;
    LoggingArray* staticLoggingArray = nullptr;
    std::atomic<ArrayData*> staticMonotypeArray{nullptr};
    std::atomic<ArrayData*> staticStructDict{nullptr};
    ArrayData* staticSampledArray = nullptr;
    EventMap events;
    EntryTypesMap entryTypes;
    KeyOrderMap keyOrders;
  };

  explicit LoggingProfile(LoggingProfileKey key);
  LoggingProfile(LoggingProfileKey key, jit::ArrayLayout layout);

  void releaseData() { data.reset(); }

  double getSampleCountMultiplier() const;
  uint64_t getTotalEvents() const;
  double getProfileWeight() const;

  // We take specific inputs rather than templated inputs because we're going
  // to follow up soon with limitations on the number of arguments we can log.
  void logEvent(ArrayOp op);
  void logEvent(ArrayOp op, int64_t k);
  void logEvent(ArrayOp op, const StringData* k);
  void logEvent(ArrayOp op, TypedValue v);
  void logEvent(ArrayOp op, int64_t k, TypedValue v);
  void logEvent(ArrayOp op, const StringData* k, TypedValue v);

  void logEntryTypes(EntryTypes before, EntryTypes after);
  void logKeyOrders(const KeyOrder&);

  // TODO(kshaunak): Refactor this class so that we automatically construct
  // this cached array when we set the layout. (We should make layout.apply
  // a LayoutFunction - MakeFromVanilla - to do so as cleanly as possible.)
  BespokeArray* getStaticBespokeArray() const;
  void setStaticBespokeArray(BespokeArray* array);

  jit::ArrayLayout getLayout() const;
  void setLayout(jit::ArrayLayout layout);

  // List of static string keys guaranteed to be present at a StructDict source.
  // Note that this list is not exhaustive. For example, it does not capture
  // the keys in a RuntimeStruct source.
  std::vector<const StringData*> knownStructKeys() const;

private:
  void logEventImpl(const EventKey& key);

public:
  const LoggingProfileKey key;
  std::unique_ptr<LoggingProfileData> data;

private:
  std::atomic<jit::ArrayLayout> layout = jit::ArrayLayout::Bottom();
  BespokeArray* staticBespokeArray = nullptr;
};

// The decision we make at each layout-sensitive sink.
struct SinkLayout {
  jit::ArrayLayout layout = jit::ArrayLayout::Bottom();
  double coverage = 0.0;
};

struct SinkLayouts {
  std::vector<SinkLayout> layouts;
  bool sideExit = true;
};

// We split sinks by profiling tracelet so we can condition on array type.
using SinkProfileKey = std::pair<TransID, SrcKey>;

// We'll store a SinkProfile for each place where an array is used.
struct SinkProfile {
  using SourceMap = folly::F14FastMap<LoggingProfile*, CopyAtomic<size_t>>;

  static constexpr size_t kNumArrTypes = ArrayData::kNumKinds / 2;
  static constexpr size_t kNumKeyTypes = int(KeyTypes::Any) + 1;
  static constexpr size_t kNumValTypes = kMaxDataType - kMinDataType + 3;

  static constexpr size_t kNoValTypes = kNumValTypes - 2;
  static constexpr size_t kAnyValType = kNumValTypes - 1;

  // The content of the sink profile that can be released after layout
  // selection.
  struct SinkProfileData {
    mutable folly::SharedMutex mapLock;

    std::atomic<uint64_t> arrCounts[kNumArrTypes] = {};
    std::atomic<uint64_t> keyCounts[kNumKeyTypes] = {};
    std::atomic<uint64_t> valCounts[kNumValTypes] = {};

    std::atomic<uint64_t> typeStructureCount = 0;
    std::atomic<uint64_t> sampledCount = 0;
    std::atomic<uint64_t> unsampledCount = 0;
    SourceMap sources;
    KeyOrderMap keyOrders;
  };

  SinkLayouts getLayouts() const;
  void setLayouts(SinkLayouts layouts);

  void update(const ArrayData* ad);

  explicit SinkProfile(SinkProfileKey key);
  SinkProfile(SinkProfileKey key, SinkLayouts layouts);

  void releaseData() { data.reset(); }

public:
  const SinkProfileKey key;
  std::unique_ptr<SinkProfileData> data;

private:
  folly::Synchronized<SinkLayouts> layouts = {};
};

// Return a profile for the given (valid) SrcKey. If no profile for the SrcKey
// exists, a new one is made. If we're done profiling or it's not useful to
// profile this bytecode, this function will return nullptr.
LoggingProfile* getLoggingProfile(SrcKey sk);
LoggingProfile* getLoggingProfile(APCKey ak);
LoggingProfile* getLoggingProfile(RuntimeStruct* runtimeStruct);
LoggingProfile* getLoggingProfile(const Class* cls, Slot slot,
                                  LocationType loc);
LoggingProfile* getLoggingProfile(const TypeAlias* ta);

// Return a profile for the given profiling tracelet and (valid) sink SrcKey.
// If no profile for the sink exists, a new one is made. May return nullptr.
SinkProfile* getSinkProfile(TransID id, SrcKey sk);

// Attempt to get the current SrcKey. May fail and return an invalid SrcKey.
SrcKey getSrcKey();

// Hooks called by layout selection at the appropriate times.
void stopProfiling();
void startExportProfiles();

// Global views, used for layout selection and serialization.
void eachSource(std::function<void(LoggingProfile& profile)> fn);
void eachSink(std::function<void(SinkProfile& profile)> fn);
void deserializeSource(LoggingProfileKey key, jit::ArrayLayout layout);
void deserializeSink(SinkProfileKey key, SinkLayouts layout);
size_t countSources();
size_t countSinks();

// Accessors for logged events. TODO(kshaunak): Expose a better API.
ArrayOp getEventArrayOp(uint64_t key);
PackedStringPtr getEventStrKey(uint64_t key);
DataType getEventValType(uint64_t key);

}

#endif // HPHP_LOGGING_PROFILE_H_
