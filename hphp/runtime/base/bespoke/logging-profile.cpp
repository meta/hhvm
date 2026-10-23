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

#include "hphp/runtime/base/bespoke/logging-profile.h"

#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/bespoke/key-coloring.h"
#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/server/memory-stats.h"
#include "hphp/runtime/vm/vm-regs.h"
#include "hphp/util/hash-map.h"

#include <fmt/format.h>
#include <folly/SharedMutex.h>
#include <folly/String.h>

#include <algorithm>
#include <atomic>

namespace HPHP::bespoke {

TRACE_SET_MOD(bespoke)

//////////////////////////////////////////////////////////////////////////////

namespace {

template <typename T, typename U>
bool fits(U u) {
  return u >= std::numeric_limits<T>::min() &&
         u <= std::numeric_limits<T>::max();
}
constexpr int8_t kInt8Min = std::numeric_limits<int8_t>::min();

// The number of events we render inline before dropping key specializations.
constexpr size_t kMaxNumDetailedEvents = 32;

folly::SharedMutex s_profilingLock;
std::atomic<bool> s_profiling{true};

std::string arrayOpToString(ArrayOp op) {
  switch (op) {
#define X(name, read) case ArrayOp::name: return #name;
ARRAY_OPS
#undef X
  }
  not_reached();
}

bool arrayOpIsRead(ArrayOp op) {
  switch (op) {
#define X(name, read) case ArrayOp::name: return read;
ARRAY_OPS
#undef X
  }
  not_reached();
}

// Some SrcKeys cannot be used as bespoke sources or sinks, because they
// don't round-trip through serialization. We hit this case for the SrcKeys
// of eval'd functions, and possibly others.
bool serializable(SrcKey sk) {
  assertx(sk.valid());
  return !sk.unit()->origFilepath()->empty();
}

// SrcKey includes more information than just the (Func, Offset) pair,
// but we want all our logging to be grouped by these two fields alone.
SrcKey canonicalize(SrcKey sk) {
  assertx(sk.valid());
  assertx(serializable(sk));
  return SrcKey(sk.func(), sk.offset(), ResumeMode::None);
}

// If the given source always produces a particular static array, return it.
ArrayData* getStaticArray(LoggingProfileKey key) {
  switch (key.locationType) {
    case LocationType::APCKey:
    case LocationType::Runtime:
      return nullptr;

    case LocationType::InstanceProperty: {
      auto const index = key.cls->propSlotToIndex(key.slot);
      auto const tv = key.cls->declPropInit()[index].val.tv();
      return tvIsArrayLike(tv) ? tv.val().parr : nullptr;
    }

    case LocationType::StaticProperty: {
      auto const tv = key.cls->staticProperties()[key.slot].val;
      return tvIsArrayLike(tv) ? tv.val().parr : nullptr;
    }

    case LocationType::TypeConstant: {
      auto const tv = key.cls->constants()[key.slot].val;
      if (!tvIsArrayLike(tv)) return nullptr;
      return key.cls->resolvedTypeCnsGet(tv.val().parr);
    }

    case LocationType::TypeAlias: {
      auto const& ts = key.ta->resolvedTypeStructureRaw();
      return !ts.isNull() ? ts.get() : nullptr;
    }

    case LocationType::SrcKey: {
      auto const op = key.sk.op();
      if (op != Op::Vec && op != Op::Dict && op != Op::Keyset) {
        return nullptr;
      }
      auto const unit = key.sk.func()->unit();
      auto const result = unit->lookupArrayId(getImm(key.sk.pc(), 0).u_AA);
      return const_cast<ArrayData*>(result);
    }
  }
  always_assert(false);
}

template <typename M, typename L, typename K>
size_t incrementCounter(M& map, L& mutex, K key) {
  {
    std::shared_lock lock{mutex};
    auto it = map.find(key);
    if (it != map.end()) {
      it->second.value++;
      return it->second;
    }
  }
  std::unique_lock lock{mutex};
  auto [it, didInsert] = map.emplace(key, 0);
  it->second.value++;
  return it->second;
}

}

//////////////////////////////////////////////////////////////////////////////

// The key for a sampled event. The granularity we choose is important here:
// too fine, and our profiles will have too many entries; too coarse, and we
// won't be able to make certain optimizations.
struct alignas(8) EventKey {
  explicit EventKey(ArrayOp op);
  EventKey(ArrayOp op, int64_t k);
  EventKey(ArrayOp op, const StringData* k);
  EventKey(ArrayOp op, TypedValue v);
  EventKey(ArrayOp op, int64_t k, TypedValue v);
  EventKey(ArrayOp op, const StringData* k, TypedValue v);

  explicit EventKey(uint64_t value) {
    static_assert(sizeof(EventKey) == sizeof(uint64_t), "");
    memmove(this, &value, sizeof(EventKey));
  }

  EventKey dropKeySpecialization() const {
    auto result = *this;
    result.m_key = kInvalidId;
    return result;
  }

  uint64_t toUInt64() const {
    uint64_t value;
    memmove(&value, this, sizeof(EventKey));
    return value;
  }

  ArrayOp getOp() const {
    return m_op;
  }

  PackedStringPtr getStrKey() const {
    if (m_key_spec != Spec::Str32) return nullptr;
    return (const StringData*)safe_cast<uintptr_t>(m_key);
  }

  DataType getValType() const {
    assertx(m_val_type != kInvalidDataType);
    return m_val_type;
  }

  std::string toString() const;

private:
  // We specialize on certain subtypes that we can represent efficiently.
  // Str32 is a static string with a 4-byte pointer, even in non-lowptr builds.
  //
  // Spec is strictly more specific than DataType, because we drop persistence
  // when saving types to the event frequency map.
  enum class Spec : uint8_t { None, Int8, Int16, Int32, Int64, Str32, Str };

  Spec getSpec(TypedValue v) {
    if (tvIsString(v)) {
      if (!val(v).pstr->isStatic()) return Spec::Str;
      auto const str = uintptr_t(val(v).pstr);
      return fits<uint32_t>(str) ? Spec::Str32 : Spec::Str;
    }
    if (tvIsInt(v)) {
      auto const num = val(v).num;
      if (fits<int8_t>(num))  return Spec::Int8;
      if (fits<int16_t>(num)) return Spec::Int16;
      if (fits<int32_t>(num)) return Spec::Int32;
      return Spec::Int64;
    }
    return Spec::None;
  }

  void setOp(ArrayOp op) {
    m_op = op;
  }
  void setKey(int64_t k) {
    m_key_spec = getSpec(make_tv<KindOfInt64>(k));
    if (m_key_spec == Spec::Int8) m_key = safe_cast<uint32_t>(k - kInt8Min);
  }
  void setKey(const StringData* k) {
    m_key_spec = getSpec(make_tv<KindOfString>(const_cast<StringData*>(k)));
    if (m_key_spec == Spec::Str32) m_key = safe_cast<uint32_t>(uintptr_t(k));
  }
  void setVal(TypedValue v) {
    m_val_spec = getSpec(v);
    m_val_type = dt_modulo_persistence(type(v));
  }

  ArrayOp m_op;
  Spec m_key_spec = Spec::None;
  Spec m_val_spec = Spec::None;
  DataType m_val_type = kInvalidDataType;
  uint32_t m_key = kInvalidId; // May be set for Spec::Int8 and Spec::Str32
};

// Dispatch to the setters above. There must be a better way...
EventKey::EventKey(ArrayOp op) { setOp(op); }
EventKey::EventKey(ArrayOp op, int64_t k) { setOp(op); setKey(k); }
EventKey::EventKey(ArrayOp op, const StringData* k) { setOp(op); setKey(k); }
EventKey::EventKey(ArrayOp op, TypedValue v) { setOp(op); setVal(v); }
EventKey::EventKey(ArrayOp op, int64_t k, TypedValue v)
  { setOp(op); setKey(k); setVal(v); }
EventKey::EventKey(ArrayOp op, const StringData* k, TypedValue v)
  { setOp(op); setKey(k); setVal(v); }

std::string EventKey::toString() const {
  auto const op = arrayOpToString(m_op);
  auto const specToStr = [](EventKey::Spec spec) {
    switch (spec) {
      case Spec::None:  return "none";
      case Spec::Int8:  return "i8";
      case Spec::Int16: return "i16";
      case Spec::Int32: return "i32";
      case Spec::Int64: return "i64";
      case Spec::Str32: return "s32";
      case Spec::Str:   return "str";
    }
    not_reached();
  };
  auto const key = [&]() -> std::string {
    auto const spec = m_key_spec;
    if (spec == Spec::None) return "";
    if (m_key != kInvalidId) {
      assertx(spec == Spec::Int8 || spec == Spec::Str32);
      if (spec == Spec::Int8) {
        auto const i = safe_cast<int8_t>(safe_cast<int16_t>(m_key) + kInt8Min);
        return fmt::format(" key=[i8:{}]", i);
      } else {
        auto const s = ((StringData*)safe_cast<uintptr_t>(m_key))->data();
        return fmt::format(" key=[s32:\"{}\"]", folly::cEscape<std::string>(s));
      }
    }
    return fmt::format(" key=[{}]", specToStr(spec));
  }();
  auto const val = [&]() -> std::string {
    if (m_val_type == kInvalidDataType) return "";
    auto const spec = m_val_spec;
    return spec == Spec::None ? fmt::format(" val=[{}]", tname(m_val_type))
                              : fmt::format(" val=[{}]", specToStr(spec));
  }();
  return fmt::format("{}{}{}", op, key, val);
}

//////////////////////////////////////////////////////////////////////////////

LoggingProfile::LoggingProfile(LoggingProfileKey key)
  : key(key)
  , data(std::make_unique<LoggingProfileData>())
{
  assertx(s_profiling.load(std::memory_order_acquire));
}

LoggingProfile::LoggingProfile(LoggingProfileKey key, jit::ArrayLayout layout)
  : key(key)
  , layout(layout)
  , staticBespokeArray(nullptr)
{
  assertx(!s_profiling.load(std::memory_order_acquire));
  if (layout.vanilla()) return;
  if (auto const ad = getStaticArray(key)) {
    setStaticBespokeArray(BespokeArray::asBespoke(layout.apply(ad)));
  }
}

double LoggingProfile::getSampleCountMultiplier() const {
  assertx(data);
  if (data->loggingArraysEmitted == 0) return 0;
  return data->sampleCount / (double) data->loggingArraysEmitted;
}

uint64_t LoggingProfile::getTotalEvents() const {
  assertx(data);
  size_t total = 0;
  for (auto const& event : data->events) total += event.second;
  return total;
}

double LoggingProfile::getProfileWeight() const {
  return getTotalEvents() * getSampleCountMultiplier();
}

void LoggingProfile::logEvent(ArrayOp op) {
  logEventImpl(EventKey(op));
}
void LoggingProfile::logEvent(ArrayOp op, int64_t k) {
  logEventImpl(EventKey(op, k));
}
void LoggingProfile::logEvent(ArrayOp op, const StringData* k) {
  logEventImpl(EventKey(op, k));
}
void LoggingProfile::logEvent(ArrayOp op, TypedValue v) {
  logEventImpl(EventKey(op, v));
}
void LoggingProfile::logEvent(ArrayOp op, int64_t k, TypedValue v) {
  logEventImpl(EventKey(op, k, v));
}
void LoggingProfile::logEvent(ArrayOp op, const StringData* k, TypedValue v) {
  logEventImpl(EventKey(op, k, v));
}

void LoggingProfile::logEventImpl(const EventKey& key) {
  // Hold the read mutex for the duration of the mutation so that profiling
  // cannot be interrupted until the mutation is complete.
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return;

  assertx(data);

  DEBUG_ONLY auto const count =
    incrementCounter(data->events, data->mapLock, key.toUInt64());

  if (Trace::moduleEnabled(Trace::bespoke, 6)) {
    DEBUG_ONLY auto const op = key.getOp();
    DEBUG_ONLY auto const has_sink =
      op != ArrayOp::Release && op != ArrayOp::ReleaseShared;
    DEBUG_ONLY auto const sink = has_sink ? getSrcKey() : SrcKey{};
    FTRACE(6, "{} -> {}: {} [count={}]\n", key.toString(),
           (sink.valid() ? sink.getSymbol() : "<unknown>"),
           EventKey(key).toString(), count);
  }
}

void LoggingProfile::logEntryTypes(EntryTypes before, EntryTypes after) {
  if (!Cfg::Eval::EmitBespokeMonotypes) return;
  // Hold the read mutex for the duration of the mutation so that profiling
  // cannot be interrupted until the mutation is complete.
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return;

  assertx(data);

  auto const key = std::make_pair(before.asInt16(), after.asInt16());
  incrementCounter(data->entryTypes, data->mapLock, key);
}

void LoggingProfile::logKeyOrders(const KeyOrder& ko) {
  // Hold the read mutex for the duration of the mutation so that profiling
  // cannot be interrupted until the mutation is complete.
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return;

  assertx(data);

  incrementCounter(data->keyOrders, data->mapLock, ko);
}

BespokeArray* LoggingProfile::getStaticBespokeArray() const {
  return staticBespokeArray;
}

void LoggingProfile::setStaticBespokeArray(BespokeArray* bad) {
  assertx(bad != nullptr);
  assertx(staticBespokeArray == nullptr);
  assertx(getStaticArray(key) != nullptr);

  staticBespokeArray = bad;

  if (key.locationType == LocationType::InstanceProperty) {
    auto const index = key.cls->propSlotToIndex(key.slot);
    auto props = const_cast<Class::PropInitVec*>(&key.cls->declPropInit());
    auto const lval = (*props)[index].val;
    lval.val().parr = bad;
    assertx(tvIsPlausible(*lval));
  }

  if (key.locationType == LocationType::StaticProperty) {
    auto const sprops = key.cls->staticProperties();
    auto const tv = const_cast<TypedValue*>(&sprops[key.slot].val);
    tv->m_data.parr = bad;
    assertx(tvIsPlausible(*tv));
  }

  if (key.locationType == LocationType::TypeConstant) {
    auto const consts = key.cls->constants();
    auto const tv = const_cast<TypedValueAux*>(&consts[key.slot].val);
    auto const rawData = reinterpret_cast<intptr_t>(bad);
    auto const ad = reinterpret_cast<ArrayData*>(rawData | 0x1);
    tv->m_data.parr = ad;
  }

  if (key.locationType == LocationType::TypeAlias) {
    auto const ta = const_cast<TypeAlias*>(key.ta);
    ta->setResolvedTypeStructure(bad);
  }
}

jit::ArrayLayout LoggingProfile::getLayout() const {
  return layout.load(std::memory_order_acquire);
}

void LoggingProfile::setLayout(jit::ArrayLayout layout) {
  assertx(layout.vanilla() || layout.bespokeLayout()->isConcrete());
  this->layout.store(layout, std::memory_order_release);
  if (key.isRuntimeLocation() && layout.is_struct()) {
    // TODO(mcolavita): setLayout should trigger setStaticBespokeArray.
    key.runtimeStruct->applyLayout(StructLayout::As(layout.bespokeLayout()));
  }
}

std::vector<const StringData*> LoggingProfile::knownStructKeys() const {
  std::vector<const StringData*> ret;
  if (key.op() == Op::NewStructDict) {
    auto const imms = getImmVector(key.sk.pc());
    auto const size = safe_cast<uint32_t>(imms.size());
    for (auto i = 0; i < size; ++i) {
      auto const keyStr = key.sk.unit()->lookupLitstrId(imms.vec32()[i]);
      ret.push_back(keyStr);
    }
    return ret;
  }
  auto const ad = data->staticSampledArray;
  if (ad && ad->isVanillaDict()) {
    VanillaDict::IterateKV(VanillaDict::as(ad), [&](auto k, auto) {
      if (tvIsString(k)) ret.push_back(val(k).pstr);
    });
    return ret;
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////

SinkProfile::SinkProfile(SinkProfileKey key)
  : key(key)
  , data(std::make_unique<SinkProfileData>())
{
  assertx(s_profiling.load(std::memory_order_acquire));
}

SinkProfile::SinkProfile(SinkProfileKey key, SinkLayouts layouts)
  : key(key)
  , layouts(layouts)
{
  assertx(!s_profiling.load(std::memory_order_acquire));
}

SinkLayouts SinkProfile::getLayouts() const {
  return this->layouts.copy();
}

void SinkProfile::setLayouts(SinkLayouts sls) {
  assertx(sls.layouts.size() > 0);
  this->layouts.swap(sls);
}

void SinkProfile::update(const ArrayData* ad) {
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return;

  assertx(data);

  // Because profiling hasn't stopped yet, any bespoke arrays should be
  // LoggingArrays at this point. Bail out if we get a non-logging bespoke.
  const LoggingArray* lad = nullptr;
  if (!ad->isVanilla()) {
    auto const index = BespokeArray::asBespoke(ad)->layoutIndex();
    // Update count of bespoke type structures before bailing out
    if (index == TypeStructure::GetLayoutIndex()) {
      data->typeStructureCount++;
      data->sampledCount++;
      return;
    }
    if (index != LoggingArray::GetLayoutIndex()) return;
    lad = LoggingArray::As(ad);
  }

  // Update array-like-generic fields: the sampled bit and the array type.

  if (lad != nullptr || ad->isSampledArray()) {
    data->sampledCount++;
  } else {
    data->unsampledCount++;
  }

  auto const kind = ad->kind() / 2;
  assertx(kind < kNumArrTypes);
  data->arrCounts[kind]++;

  if (lad == nullptr) return;

  // Update LoggingArray-only fields: key type, val type, and array source.
  auto const& et = lad->entryTypes;
  auto const key = size_t(et.keyTypes);
  auto const val = [&]{
    if (et.valueTypes == ValueTypes::Empty) return kNoValTypes;
    if (et.valueTypes != ValueTypes::Monotype) return kAnyValType;
    auto const dt = dt_modulo_persistence(et.valueDatatype);
    return size_t(static_cast<data_type_t>(dt) - kMinDataType);
  }();

  assertx(key < kNumKeyTypes);
  assertx(val < kNumValTypes);
  data->keyCounts[key]++;
  data->valCounts[val]++;

  incrementCounter(data->sources, data->mapLock, lad->profile);
  incrementCounter(data->keyOrders, data->mapLock, lad->keyOrder);
}

//////////////////////////////////////////////////////////////////////////////

namespace {

struct EventOutputData {
  EventOutputData(EventKey event, uint64_t count): event(event), count(count) {}

  bool operator<(const EventOutputData& other) const {
    return count > other.count;
  }

  EventKey event;
  uint64_t count;
};

struct OperationOutputData {
  explicit OperationOutputData(ArrayOp operation)
    : operation(operation)
    , totalCount(0)
  {}

  bool operator<(const OperationOutputData& other) const {
    return totalCount > other.totalCount;
  }

  void registerEvent(EventKey key, uint64_t count) {
    events.emplace_back(key, count);
    totalCount += count;
  }

  void formatEvents() {
    if (events.size() > kMaxNumDetailedEvents) {
      std::map<uint64_t, uint64_t> deduped;
      for (auto const& event : events) {
        deduped[event.event.dropKeySpecialization().toUInt64()] += event.count;
      }
      events.clear();
      for (auto const& pair : deduped) {
        events.emplace_back(EventKey(pair.first), pair.second);
      }
    }
    std::sort(events.begin(), events.end());
  }

  ArrayOp operation;
  std::vector<EventOutputData> events;
  uint64_t totalCount;
};

struct EntryTypesUseOutputData {
  EntryTypesUseOutputData(EntryTypes state, uint64_t count)
    : state(state)
    , count(count)
  {}

  bool operator<(const EntryTypesUseOutputData& other) const {
    return count > other.count;
  }

  EntryTypes state;
  uint64_t count;
};

struct KeyOrderOutputData {
  KeyOrderOutputData(const KeyOrder& ko, uint64_t count)
    : ko(ko)
    , count(count)
  {}

  bool operator<(const KeyOrderOutputData& other) const {
    return count > other.count;
  }

  KeyOrder ko;
  uint64_t count;
};

struct EntryTypesEscalationOutputData {
  EntryTypesEscalationOutputData(EntryTypes before, EntryTypes after,
                               uint64_t count)
    : before(before)
    , after(after)
    , count(count)
  {}

  bool operator<(const EntryTypesEscalationOutputData& other) const {
    return count > other.count;
  }

  EntryTypes before;
  EntryTypes after;
  uint64_t count;
};

struct SourceFrequencyData {
  SourceFrequencyData(const std::string& source, uint64_t count)
    : source(source), count(count) {}

  bool operator<(const SourceFrequencyData& other) const {
    return count > other.count;
  }

  std::string source;
  uint64_t count;
};

struct SinkTypeData {
  SinkTypeData(const char* name, uint64_t count) : name(name), count(count) {}

  bool operator<(const SinkTypeData& other) const {
    return count > other.count;
  }

  const char* name;
  uint64_t count;
};

// NOTE: These helpers undo the transformations in SinkProfile::update.

const char* getArrTypeStr(size_t type) {
  assertx(type <= SinkProfile::kNumArrTypes);
  return ArrayData::kindToString(ArrayData::ArrayKind(type * 2));
}

const char* getKeyTypeStr(size_t type) {
  assertx(type <= SinkProfile::kNumKeyTypes);
  return show(KeyTypes(type));
}

const char* getValTypeStr(size_t type) {
  assertx(type <= SinkProfile::kNumValTypes);
  if (type == SinkProfile::kNoValTypes) return "Empty";
  if (type == SinkProfile::kAnyValType) return "Any";
  auto const dt = DataType(safe_cast<int8_t>(type) + kMinDataType);
  switch (dt) {
#define DT(name, ...) case KindOf##name: return #name;
DATATYPES
#undef DT
  }
  always_assert(false);
}

template <size_t N, typename Fn>
std::vector<SinkTypeData> populateSortedCounts(
    const std::atomic<uint64_t> (&counts)[N], Fn fn) {
  std::vector<SinkTypeData> result;
  for (auto i = 0; i < N; i++) {
    auto const count = counts[i].load(std::memory_order_acquire);
    if (count) result.push_back({fn(i), count});
  }
  std::sort(result.begin(), result.end());
  return result;
}

struct SourceOutputData {
  SourceOutputData(const LoggingProfile* profile,
                   std::vector<OperationOutputData>&& operations,
                   std::vector<EntryTypesEscalationOutputData>&& monoEscalations,
                   std::vector<EntryTypesUseOutputData>&& monoUses,
                   std::vector<KeyOrderOutputData>&& kos)
    : profile(profile)
    , monotypeEscalations(std::move(monoEscalations))
    , monotypeUses(std::move(monoUses))
    , keyOrders(std::move(kos))
  {
    std::sort(monotypeEscalations.begin(), monotypeEscalations.end());
    std::sort(monotypeUses.begin(), monotypeUses.end());
    std::sort(operations.begin(), operations.end());
    std::sort(keyOrders.begin(), keyOrders.end());

    readCount = 0;
    writeCount = 0;
    for (auto const& operationData : operations) {
      if (arrayOpIsRead(operationData.operation)) {
        readCount += operationData.totalCount;
        readOperations.push_back(operationData);
      } else {
        writeCount += operationData.totalCount;
        writeOperations.push_back(operationData);
      }
    }

    weight = profile->getProfileWeight();
  }

  bool operator<(const SourceOutputData& other) const {
    return weight > other.weight;
  }

  const LoggingProfile* profile;
  std::vector<OperationOutputData> readOperations;
  std::vector<OperationOutputData> writeOperations;
  std::vector<EntryTypesEscalationOutputData> monotypeEscalations;
  std::vector<EntryTypesUseOutputData> monotypeUses;
  std::vector<KeyOrderOutputData> keyOrders;
  uint64_t readCount;
  uint64_t writeCount;
  double weight;
};

struct SinkOutputData {
  explicit SinkOutputData(const SinkProfile* profile)
    : profile(profile)
  {
    assertx(profile->data);

    std::map<std::string, uint64_t> sourceCounts;
    for (auto const& it : profile->data->sources) {
      sourceCounts[it.first->key.toString()] += it.second;
      loggedCount += it.second;
    }
    for (auto const& it : sourceCounts) {
      sources.push_back({it.first, it.second});
    }
    std::sort(sources.begin(), sources.end());

    arrCounts = populateSortedCounts(profile->data->arrCounts, getArrTypeStr);
    keyCounts = populateSortedCounts(profile->data->keyCounts, getKeyTypeStr);
    valCounts = populateSortedCounts(profile->data->valCounts, getValTypeStr);

    sampledCount = profile->data->sampledCount.load(std::memory_order_acquire);
    unsampledCount = profile->data->unsampledCount.load(std::memory_order_acquire);

    weight = sampledCount + unsampledCount;

    for (auto const& keyOrderAndCount : profile->data->keyOrders) {
      keyOrders.push_back({keyOrderAndCount.first, keyOrderAndCount.second});
    }
    std::sort(keyOrders.begin(), keyOrders.end());
  }

  bool operator<(const SinkOutputData& other) const {
    return weight > other.weight;
  }

  const SinkProfile* profile;
  std::vector<SinkTypeData> arrCounts;
  std::vector<SinkTypeData> keyCounts;
  std::vector<SinkTypeData> valCounts;
  std::vector<SourceFrequencyData> sources;
  std::vector<KeyOrderOutputData> keyOrders;
  uint64_t loggedCount = 0;
  uint64_t sampledCount = 0;
  uint64_t unsampledCount = 0;
  uint64_t weight = 0;
};

struct LoggingProfileKeyHash {
  size_t operator()(const LoggingProfileKey& key) const {
    return folly::hash::hash_combine(
      hash_int64(key.ptr), key.slot, key.locationType);
  }
};

struct SinkProfileKeyHash {
  size_t operator()(const SinkProfileKey& key) const {
    return folly::hash::hash_combine(
      hash_int64(key.first), key.second.toAtomicInt());
  }
};

using ProfileOutputData = std::vector<SourceOutputData>;
using ProfileMap = folly_concurrent_hash_map_simd<
  LoggingProfileKey, LoggingProfile*, LoggingProfileKeyHash,
  std::equal_to<LoggingProfileKey>>;
using SinkMap = folly_concurrent_hash_map_simd<
  SinkProfileKey, SinkProfile*, SinkProfileKeyHash,
  std::equal_to<SinkProfileKey>>;

std::thread s_exportProfilesThread;
ProfileMap s_profileMap;
SinkMap s_sinkMap;

template<typename... Ts>
bool logToFile(FILE* file, fmt::format_string<Ts...> fmt, Ts&&... args) {
  auto const entry = fmt::format(fmt, std::forward<Ts>(args)...);
  return fwrite(entry.data(), 1, entry.length(), file) == entry.length();
}

#define LOG_OR_RETURN(...) if (!logToFile(__VA_ARGS__)) return false;

bool exportOperationSet(FILE* file,
                        const std::vector<OperationOutputData>& operations) {
  for (auto const& operationData : operations) {
    if (operationData.events.size() == 1) {
      // There's only one distinct event for this op, print it at this level.
      auto const eventData = *operationData.events.begin();
      assertx(operationData.totalCount == eventData.count);
      LOG_OR_RETURN(file, "  {: >6}x {}\n", eventData.count,
                    eventData.event.toString());
      continue;
    }

    LOG_OR_RETURN(file, "  {: >6}x {}\n", operationData.totalCount,
                  arrayOpToString(operationData.operation));

    for (auto const& eventData : operationData.events) {
      LOG_OR_RETURN(file, "        {: >6}x {}\n", eventData.count,
                    eventData.event.toString());
    }
  }

  return true;
}

bool exportTypeCounts(FILE* file, const char* label,
                      const std::vector<SinkTypeData>& counts) {
  LOG_OR_RETURN(file, "  {} Type Counts:\n", label);
  for (auto const& count : counts) {
    LOG_OR_RETURN(file, "  {: >6}x {}\n", count.count, count.name);
  }
  return true;
}

bool exportSortedProfiles(FILE* file, const ProfileOutputData& profileData) {
  LOG_OR_RETURN(file, "========================================================================\n");
  LOG_OR_RETURN(file, "Sources:\n\n");

  for (auto const& sourceData : profileData) {
    auto const sourceProfile = sourceData.profile;
    assertx(sourceProfile->data);

    LOG_OR_RETURN(file, "{} [{}/{} sampled, {:.2f} weight]\n",
                  sourceProfile->key.toString(),
                  sourceProfile->data->loggingArraysEmitted.load(),
                  sourceProfile->data->sampleCount.load(), sourceData.weight);
    LOG_OR_RETURN(file, "  {}\n", sourceProfile->key.toStringDetail());
    LOG_OR_RETURN(file, "  Selected Layout: {}\n",
                  sourceProfile->getLayout().describe());
    LOG_OR_RETURN(file, "  {} reads, {} writes\n",
                  sourceData.readCount, sourceData.writeCount);

    LOG_OR_RETURN(file, "  Read operations:\n");
    if (!exportOperationSet(file, sourceData.readOperations)) return false;

    LOG_OR_RETURN(file, "  Write operations:\n");
    if (!exportOperationSet(file, sourceData.writeOperations)) return false;

    LOG_OR_RETURN(file, "  Entry Type Escalations:\n");
    for (auto const& escData : sourceData.monotypeEscalations) {
      LOG_OR_RETURN(file, "  {: >6}x {} -> {}\n", escData.count,
                    escData.before.toString(), escData.after.toString());
    }

    LOG_OR_RETURN(file, "  Entry Type Operations:\n");
    for (auto const& useData : sourceData.monotypeUses) {
      LOG_OR_RETURN(file, "  {: >6}x {}\n", useData.count,
                    useData.state.toString());
    }

    LOG_OR_RETURN(file, "  Static String Key Insertion Order:\n");
    for (auto const koData : sourceData.keyOrders) {
      LOG_OR_RETURN(file, "  {: >6}x {}\n", koData.count, koData.ko.toString());
    }

    LOG_OR_RETURN(file, "\n");
  }

  return true;
}

bool exportSortedSinks(FILE* file, const std::vector<SinkOutputData>& sinks) {
  LOG_OR_RETURN(file, "========================================================================\n");
  LOG_OR_RETURN(file, "Sinks:\n\n");

  for (auto const& sink : sinks) {
    auto const sk = sink.profile->key.second;
    LOG_OR_RETURN(file, "{} [{}/{} sampled, {} logged]\n",
                  sk.getSymbol(), sink.sampledCount,
                  sink.weight, sink.loggedCount);
    LOG_OR_RETURN(file, "  {}\n", sk.showInst());

    auto const sls = sink.profile->getLayouts();
    LOG_OR_RETURN(file, "  Selected Layouts (n={}):\n", sls.layouts.size());
    for (auto const& sl : sls.layouts) {
      LOG_OR_RETURN(file, "    Layout (coverage={}): {}\n", sl.coverage, sl.layout.describe());
    }
    LOG_OR_RETURN(file, "  Layout Guard: {}\n", sls.sideExit ? "side-exit" : "diamond");

    if (!exportTypeCounts(file, "Array", sink.arrCounts)) return false;
    if (!exportTypeCounts(file, "Key",   sink.keyCounts)) return false;
    if (!exportTypeCounts(file, "Value", sink.valCounts)) return false;

    LOG_OR_RETURN(file, "  Sources:\n");
    for (auto const& edge : sink.sources) {
      LOG_OR_RETURN(file, "  {: >6}x {}\n", edge.count, edge.source);
    }

    LOG_OR_RETURN(file, "  Static String Key Insertion Order:\n");
    for (auto const koData : sink.keyOrders) {
      LOG_OR_RETURN(file, "  {: >6}x {}\n", koData.count, koData.ko.toString());
    }

    LOG_OR_RETURN(file, "\n");
  }

  return true;
}

bool exportLayouts(FILE* file) {
  LOG_OR_RETURN(file, "========================================================================\n");
  LOG_OR_RETURN(file, "Layouts:\n\n");
  LOG_OR_RETURN(file, "{}", Layout::dumpAllLayouts());

  LOG_OR_RETURN(file, "========================================================================\n");
  LOG_OR_RETURN(file, "Coloring info:\n\n");
  LOG_OR_RETURN(file, "{}", dumpColoringInfo());

  return true;
}

SourceOutputData sortSourceData(const LoggingProfile* profile) {
  assertx(profile->data);
  // Group events by their operation
  std::map<ArrayOp, OperationOutputData> opsGrouped;
  for (auto const& eventAndCount : profile->data->events) {
    auto const event = EventKey(eventAndCount.first);
    auto const count = eventAndCount.second;

    auto const op = event.getOp();
    auto const it = opsGrouped.try_emplace(op, op);
    it.first->second.registerEvent(event, count);
  }

  for (auto& operation : opsGrouped) {
    operation.second.formatEvents();
  }

  // Flatten to vectors
  std::vector<OperationOutputData> operations;
  operations.reserve(opsGrouped.size());
  std::transform(
    opsGrouped.begin(), opsGrouped.end(), std::back_inserter(operations),
    [](const std::pair<ArrayOp, OperationOutputData>& operationData) {
      return operationData.second;
    }
  );

  // Determine monotype operations
  std::vector<EntryTypesEscalationOutputData> escalations;
  std::map<uint16_t, uint64_t> usesMap;
  for (auto const& statesAndCount : profile->data->entryTypes) {
    auto const before = statesAndCount.first.first;
    auto const after = statesAndCount.first.second;
    auto const count = statesAndCount.second;

    if (before != after) {
      escalations.emplace_back(EntryTypes(before), EntryTypes(after), count);
    }

    usesMap[after] += count;
  }

  std::vector<EntryTypesUseOutputData> uses;
  uses.reserve(usesMap.size());
  std::transform(
    usesMap.begin(), usesMap.end(), std::back_inserter(uses),
    [](const std::pair<uint16_t, uint64_t>& useData) {
      auto const state = EntryTypes(useData.first);
      return EntryTypesUseOutputData(state, useData.second);
    }
  );

  std::vector<KeyOrderOutputData> keyOrders;
  for (auto const& keyOrderAndCount : profile->data->keyOrders) {
    keyOrders.push_back({keyOrderAndCount.first, keyOrderAndCount.second});
  }

  return SourceOutputData(profile, std::move(operations),
                          std::move(escalations), std::move(uses),
                          std::move(keyOrders));
}

ProfileOutputData sortProfileData() {
  ProfileOutputData profileData;
  profileData.reserve(s_profileMap.size());

  std::transform(
    s_profileMap.begin(), s_profileMap.end(), std::back_inserter(profileData),
    [](auto const& pair) { return sortSourceData(pair.second); }
  );
  std::sort(profileData.begin(), profileData.end());

  return profileData;
}

std::vector<SinkOutputData> sortSinkData() {
  std::vector<SinkOutputData> sinkData;
  sinkData.reserve(s_sinkMap.size());

  for (auto const& it : s_sinkMap) {
    sinkData.push_back(SinkOutputData(it.second));
  }
  std::sort(sinkData.begin(), sinkData.end());

  return sinkData;
}

}

void stopProfiling() {
  assertx(allowBespokeArrayLikes());
  assertx(s_profiling.load(std::memory_order_acquire));
  std::unique_lock lock{s_profilingLock};
  s_profiling.store(false, std::memory_order_release);
}

namespace {
void freeProfileData() {
  // When testing bespoke array-likes, we cache logging and monotype arrays
  // in LoggingProfileData, so we do not free them after layout selection.
  if (shouldTestBespokeArrayLikes()) return;
  eachSource([](auto& x) { x.releaseData(); });
  eachSink([](auto& x) { x.releaseData(); });
  Treadmill::enqueue([] { KeyOrder::ReleaseProfilingKeyOrders(); });
}
}

void startExportProfiles() {
  if (Cfg::Eval::ExportLoggingArrayDataPath.empty()) {
    freeProfileData();
    return;
  }

  s_exportProfilesThread = std::thread([] {
    SCOPE_EXIT { freeProfileData(); };

    auto const sources = sortProfileData();
    auto const sinks = sortSinkData();
    auto const file = fopen(Cfg::Eval::ExportLoggingArrayDataPath.c_str(), "w");
    if (!file) return;

    SCOPE_EXIT { fclose(file); };

    exportLayouts(file);
    exportSortedProfiles(file, sources);
    exportSortedSinks(file, sinks);
  });
}

void waitOnExportProfiles() {
  if (s_exportProfilesThread.joinable()) {
    s_exportProfilesThread.join();
  }
}

//////////////////////////////////////////////////////////////////////////////

namespace {
void freeStaticArray(ArrayData* ad) {
  assertx(ad->isStatic());
  auto const extra = sharedAllocExtra(ad, /*apc_tv=*/false);
  auto const alloc = reinterpret_cast<char*>(ad) - extra;
  ArrayData::FreeStatic(alloc);
}

bool shouldLogAtSrcKey(SrcKey sk) {
  if (!sk.valid()) {
    FTRACE(5, "VMRegAnchor failed for maybeMakeLoggingArray.\n");
    return false;
  }

  if (!serializable(sk)) {
    FTRACE(5, "Cannot create bespoke source for non-serializable source.\n");
    return false;
  }

  // Don't profile static dicts used for TypeStruct tests. Rather than using
  // these dicts, we almost always just do a DataType check on the value.
  if (sk.op() == Op::Dict && sk.advanced().op() == Op::IsTypeStructC) {
    FTRACE(5, "Skipping static array used for TypeStruct test.\n");
    return false;
  }

  return true;
}

LoggingProfile* getLoggingProfile(LoggingProfileKey key) {
  assertx(key.checkInvariants());
  assertx(allowBespokeArrayLikes());
  auto const it = s_profileMap.find(key);
  if (it != s_profileMap.end()) {
    assertx(it->second->key == key);
    return it->second;
  }
  auto const ad = getStaticArray(key);
  if (ad && !ad->isVanilla()) return nullptr;

  // Hold the read mutex while we're constructing the new profile so that we
  // cannot stop profiling until this mutation is complete.
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return nullptr;

  auto profile = std::make_unique<LoggingProfile>(key);
  if (ad) {
    profile->data->staticLoggingArray = LoggingArray::MakeStatic(ad, profile.get());
    profile->data->staticSampledArray = ad->makeSampledStaticArray();
  }

  auto const pair = s_profileMap.insert({key, profile.get()});
  if (pair.second) profile.release();

  // If the array was static, we must either log the new static memory we used
  // or free that memory, depending on whether we won the race to set profile.
  if (ad) {
    if (profile) {
      freeStaticArray(profile->data->staticSampledArray);
      freeStaticArray(profile->data->staticLoggingArray);
    } else {
      MemoryStats::LogAlloc(AllocKind::StaticArray, sizeof(LoggingArray));
      MemoryStats::LogAlloc(AllocKind::StaticArray, ad->heapSize());
    }
  }

  auto const result = pair.first->second;
  assertx(result->key == key);
  return result;
}
}

LoggingProfile* getLoggingProfile(SrcKey sk) {
  if (!shouldLogAtSrcKey(sk)) return nullptr;
  return getLoggingProfile(LoggingProfileKey(canonicalize(sk)));
}

LoggingProfile* getLoggingProfile(APCKey ak) {
  return getLoggingProfile(LoggingProfileKey(ak));
}

LoggingProfile* getLoggingProfile(RuntimeStruct* runtimeStruct) {
  auto const profile = runtimeStruct->m_profile.load(std::memory_order_acquire);
  if (profile) return profile;
  auto const newProfile = getLoggingProfile(LoggingProfileKey(runtimeStruct));
  runtimeStruct->m_profile.store(newProfile, std::memory_order_release);
  return newProfile;
}

LoggingProfile* getLoggingProfile(const Class* cls, Slot slot,
                                  LocationType loc) {
  assertx(
    loc == LocationType::InstanceProperty ||
    loc == LocationType::StaticProperty ||
    loc == LocationType::TypeConstant
  );
  if (loc == LocationType::InstanceProperty &&
      cls->declProperties()[slot].name == s_86reified_prop.get()) {
    return nullptr;
  }
  return getLoggingProfile(LoggingProfileKey(cls, slot, loc));
}

LoggingProfile* getLoggingProfile(const TypeAlias* ta) {
  return getLoggingProfile(LoggingProfileKey(ta));
}

SinkProfile* getSinkProfile(TransID id, SrcKey sk) {
  assertx(allowBespokeArrayLikes());
  if (!serializable(sk)) return nullptr;
  auto const key = SinkProfileKey { id, canonicalize(sk) };
  auto const it = s_sinkMap.find(key);
  if (it != s_sinkMap.end()) {
    assertx(it->second->key == key);
    return it->second;
  }

  // Hold the read mutex while we're constructing the new profile so that we
  // cannot stop profiling until this mutation is complete.
  std::shared_lock lock{s_profilingLock};
  if (!s_profiling.load(std::memory_order_acquire)) return nullptr;

  auto profile = std::make_unique<SinkProfile>(key);
  auto const pair = s_sinkMap.insert({key, profile.get()});
  if (pair.second) profile.release();

  auto const result = pair.first->second;
  assertx(result->key == key);
  return result;
}

SrcKey getSrcKey() {
  // If there are no VM frames, don't drop an anchor
  if (!rds::header()) return SrcKey();

  VMRegAnchor _(VMRegAnchor::Soft);
  if (regState() != VMRegState::CLEAN || vmfp() == nullptr) {
    return SrcKey();
  }
  return fromLeaf([&](const BTFrame& frm) {
    auto const result = SrcKey(frm.func(), frm.bcOff(), ResumeMode::None);
    if (!serializable(result)) return SrcKey();
    assertx(canonicalize(result) == result);
    return result;
  });
}

void eachSource(std::function<void(LoggingProfile& profile)> fn) {
  assertx(!s_profiling.load(std::memory_order_acquire));
  for (auto& it : s_profileMap) {
    assertx(it.first == it.second->key);
    fn(*it.second);
  }
}

void eachSink(std::function<void(SinkProfile& profile)> fn) {
  assertx(!s_profiling.load(std::memory_order_acquire));
  for (auto& it : s_sinkMap) {
    assertx(it.first == it.second->key);
    fn(*it.second);
  }
}

void deserializeSource(LoggingProfileKey key, jit::ArrayLayout layout) {
  assertx(key.checkInvariants());
  auto const profile = new LoggingProfile(key, layout);
  always_assert(s_profileMap.insert({key, profile}).second);
}

void deserializeSink(SinkProfileKey key, SinkLayouts sls) {
  auto const profile = new SinkProfile(key, sls);
  always_assert(s_sinkMap.insert({key, profile}).second);
}

size_t countSources() {
  return s_profileMap.size();
}

size_t countSinks() {
  return s_sinkMap.size();
}

ArrayOp getEventArrayOp(uint64_t key) {
  return EventKey(key).getOp();
}

PackedStringPtr getEventStrKey(uint64_t key) {
  return EventKey(key).getStrKey();
}

DataType getEventValType(uint64_t key) {
  return EventKey(key).getValType();
}

//////////////////////////////////////////////////////////////////////////////

}
