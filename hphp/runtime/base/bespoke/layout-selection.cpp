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

#include "hphp/runtime/base/bespoke/layout-selection.h"

#include <hphp/runtime/vm/jit/prof-data.h>
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/bespoke/key-coloring.h"
#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/bespoke/monotype-dict.h"
#include "hphp/runtime/base/bespoke/monotype-vec.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/union-find.h"

namespace HPHP::bespoke {

TRACE_SET_MOD(bespoke)

//////////////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////////////

struct StructGroup {
  StructGroup(std::vector<const LoggingProfile*>&& profiles, double weight)
    : profiles(std::move(profiles))
    , weight(weight)
  {}

  std::vector<const LoggingProfile*> profiles;
  double weight;
  const StructLayout* layout;
};

//////////////////////////////////////////////////////////////////////////////

using jit::ArrayLayout;

// This type represents a decision of type T that we make based on profiling.
// It covers a fraction `coverage` of the profiled distribution.
template <typename T>
struct Decision {
  T value;
  double coverage;
};

uint64_t load(const std::atomic<uint64_t>& x) {
  return x.load(std::memory_order_acquire);
}

double probabilityOfEscalation(const LoggingProfile& profile) {
  auto const logging = load(profile.data->loggingArraysEmitted);
  if (!logging) return 1.0;

  uint64_t escalated = 0;
  for (auto const& it : profile.data->events) {
    auto const op = getEventArrayOp(it.first);
    if (op == ArrayOp::EscalateToVanilla) escalated += it.second;
  }
  return 1.0 * std::min(escalated, logging) / logging;
}

double probabilityOfSampled(const SinkProfile& profile) {
  if (profile.data->sources.empty()) return 0.0;
  auto const sampled = load(profile.data->sampledCount);
  auto const unsampled = load(profile.data->unsampledCount);
  return 1.0 * sampled / (sampled + unsampled);
}

//////////////////////////////////////////////////////////////////////////////
// Monotype helpers

// Returns KeyTypes::Any if there's no good key type to specialize on.
Decision<KeyTypes> selectKeyType(const SinkProfile& profile, double p_cutoff) {
  assertx(profile.data);

  auto const empty = load(profile.data->keyCounts[int(KeyTypes::Empty)]);
  auto const ints  = load(profile.data->keyCounts[int(KeyTypes::Ints)]);
  auto const strs  = load(profile.data->keyCounts[int(KeyTypes::Strings)]);
  auto const sstrs = load(profile.data->keyCounts[int(KeyTypes::StaticStrings)]);
  auto const any   = load(profile.data->keyCounts[int(KeyTypes::Any)]);

  auto const total = empty + ints + strs + sstrs + any;
  if (!total) return {KeyTypes::Any, 1.0};

  auto const p_ints  = 1.0 * (empty + ints) / total;
  auto const p_strs  = 1.0 * (empty + strs + sstrs) / total;
  auto const p_sstrs = 1.0 * (empty + sstrs) / total;

  if (p_sstrs >= p_cutoff) return {KeyTypes::StaticStrings, p_sstrs};
  if (p_strs >= p_cutoff)  return {KeyTypes::Strings, p_strs};
  if (p_ints >= p_cutoff)  return {KeyTypes::Ints, p_ints};
  return {KeyTypes::Any, 1.0};
}

// Returns kKindOfUninit if we should specialize on "monotype of unknown type".
// Returns kInvalidDataType if we should not specialize this sink on a monotype.
Decision<DataType> selectValType(const SinkProfile& profile, double p_cutoff) {
  assertx(profile.data);

  auto const empty = load(profile.data->valCounts[SinkProfile::kNoValTypes]);
  auto const any   = load(profile.data->valCounts[SinkProfile::kAnyValType]);

  uint64_t total = empty + any;
  uint64_t max_count = 0;
  auto max_index = safe_cast<int>(SinkProfile::kNumValTypes);

  static_assert(SinkProfile::kNoValTypes == SinkProfile::kNumValTypes - 2);
  static_assert(SinkProfile::kAnyValType == SinkProfile::kNumValTypes - 1);
  for (auto i = 0; i < SinkProfile::kNoValTypes; i++) {
    auto const count = load(profile.data->valCounts[i]);
    total += count;
    if (count > max_count) {
      max_count = count;
      max_index = i;
    }
  }

  if (!total) return {kInvalidDataType, 1.0};

  auto const p_empty = 1.0 * empty / total;
  auto const p_max   = 1.0 * (empty + max_count) / total;
  auto const p_mono  = 1.0 * (total - any) / total;
  auto const max = safe_cast<DataType>(max_index + kMinDataType);

  // TODO(kshaunak): We may want to specialize on empty in the future.
  if (p_empty >= p_cutoff) return {KindOfUninit, p_mono};
  if (p_max >= p_cutoff)   return {max, p_max};
  if (p_mono >= p_cutoff)  return {KindOfUninit, p_mono};
  return {kInvalidDataType, 1.0};
}

//////////////////////////////////////////////////////////////////////////////
// Struct helpers

using KeyFrequencies = folly::F14FastMap<const StringData*, int64_t>;
using KeyBounds = folly::F14FastMap<const StringData*, uint8_t>;
using KeySet = folly::F14FastSet<const StringData*>;

struct StructAnalysis {
  std::vector<KeyOrder> key_orders;
  std::vector<const SinkProfile*> merge_sinks;
  UnionFind<const LoggingProfile*> union_find;
};

struct StructAnalysisResult {
  folly::F14FastMap<const LoggingProfile*, const StructLayout*> sources;
  folly::F14FastMap<const SinkProfile*, Decision<ArrayLayout>> sinks;
};

double keyCoverageThreshold() {
  return Cfg::Eval::BespokeStructDictKeyCoverageThreshold / 100;
}

template <typename T, typename I, typename F>
void mergeValid(I&& begin, I&& end, UnionFind<T>& uf, F&& elem) {
  auto iter = begin;
  while (iter != end && !uf.lookup(elem(*iter))) iter++;
  if (iter == end) return;
  auto const& root = elem(*iter);
  for (; iter!= end; iter++) {
    auto const& val = elem(*iter);
    if (uf.lookup(val)) uf.merge(root, val);
  }
}

// Sort keys in KeyOrder by descending order of frequency.
KeyOrder sortKeyOrder(const KeyOrder& ko, const KeyFrequencies& keys) {
  if (ko.empty() || !ko.valid()) return ko;
  auto const frequency = [&](PackedStringPtr key) {
    auto const it = keys.find(key.get());
    return it == keys.end() ? 0 : it->second;
  };
  KeyOrderData sorted;
  for (auto const key : ko) {
    sorted.push_back(key);
  }
  std::sort(sorted.begin(), sorted.end(),
            [&](auto a, auto b) { return frequency(a) > frequency(b); });
  return KeyOrder::Make(sorted);
}

using KeyCountMap = folly::F14FastMap<const StringData*, size_t>;
using KeyOrderFrequencyList = std::vector<std::pair<KeyOrder, size_t>>;

bool opSetsStringKey(ArrayOp op) {
  using AO = ArrayOp;
  return op == AO::APCInitStr || op == AO::ConstructStr || op == AO::SetStr;
}

// Returns a map from keys to counts, indicating a conservative approximation
// of the number of logged arrays containing (the static version of) each key.
KeyCountMap arraysContainingKeys(
    const std::vector<const LoggingProfile*>& profiles, size_t bound) {
  auto arrayCounts = KeyCountMap();
  for (auto const& profile : profiles) {
    for (auto const& it : profile->data->events) {
      if (opSetsStringKey(getEventArrayOp(it.first))) {
        auto const key = getEventStrKey(it.first);
        if (key != nullptr) {
          auto& count = arrayCounts[key.get()];
          count = std::min(count + it.second, bound);
        }
      }
    }

    // Count static keys
    auto const ad = profile->data->staticSampledArray;
    if (!ad) continue;
    assertx(ad->isVanillaDict());
    VanillaDict::IterateKV(
      VanillaDict::as(ad),
      [&](auto k, auto) {
        if (!tvIsString(k)) return;
        auto& count = arrayCounts[val(k).pstr];
        count = std::min(
          size_t{count + load(profile->data->loggingArraysEmitted)}, bound);
      }
    );
  }
  return arrayCounts;
}

size_t countArrays(const std::vector<const LoggingProfile*>& profiles) {
  return std::accumulate(
    profiles.begin(),
    profiles.end(),
    size_t{0},
    [](auto const& a, auto const& b) {
      return a + load(b->data->loggingArraysEmitted);
    }
  );
}

// Returns a map indicating, for each key in the KeyOrderMap, how many key
// order instances would be invalidated by removing it.
KeyCountMap countKeyInstances(const KeyOrderFrequencyList& frequencyList) {
  auto keyInstances = KeyCountMap{};
  for (auto const& [keyOrder, count] : frequencyList) {
    for (auto const& key : keyOrder) {
      keyInstances[key] += count;
    }
  }

  return keyInstances;
}

// Removes unlikely keys from the overall KeyOrderMap, until removing the next
// key would bring the total probability remaining below the cutoff.
KeyOrder pruneKeyOrder(
    const std::vector<const LoggingProfile*>& profiles, double cutoff) {
  auto const keyOrderMap = [&]{
    auto kom = KeyOrderMap();
    for (auto const profile : profiles) {
      mergeKeyOrderMap(kom, profile->data->keyOrders);
    }
    return kom;
  }();

  auto workingSet = KeyOrderFrequencyList();
  workingSet.reserve(keyOrderMap.size());
  for (auto const& [keyOrder, count] : keyOrderMap) {
    workingSet.emplace_back(keyOrder, count);
  }

  auto const sumCounts = [](auto begin, auto end) {
    return std::accumulate(
      begin, end,
      size_t{0},
      [](auto const& a, auto const& b) { return a + b.second; }
    );
  };

  // Compute the total number of key order instances we begin with.
  auto const totalOrders = sumCounts(workingSet.begin(), workingSet.end());
  auto const totalArrays = countArrays(profiles);
  auto const arraysContainingKey = arraysContainingKeys(profiles, totalArrays);

  // Immediately prune invalid key orders.
  auto acceptedOrders = totalOrders;
  auto acceptedArrays = totalArrays;

  {
    auto const removeAt = std::partition(
      workingSet.begin(), workingSet.end(),
      [&](auto const& a) { return a.first.valid(); }
    );
    auto const discardedOrders = sumCounts(removeAt, workingSet.end());
    acceptedOrders -= discardedOrders;
    workingSet.erase(removeAt, workingSet.end());
    assertx(acceptedOrders == sumCounts(workingSet.begin(), workingSet.end()));

    // Conservatively invalidate the same number of arrays.
    if (discardedOrders > acceptedArrays) return KeyOrder::MakeInvalid();
    acceptedArrays -= discardedOrders;

    FTRACE(2, "Prune invalid. Remain: ({} / {} ko), ({} / {} arrays)\n",
           acceptedOrders, totalOrders, acceptedArrays, totalArrays);
  }

  // If we're already below our cutoff, abort.
  if (acceptedOrders < totalOrders * cutoff ||
      acceptedArrays < totalArrays * cutoff) {
    return KeyOrder::MakeInvalid();
  }

  // Greedily remove the key which invalidates the least number of key order
  // instances. Stop when doing so again would cross the cutoff.
  auto keyInstances = countKeyInstances(workingSet);
  while (!keyInstances.empty()) {
    auto const keyAndCount = std::min_element(
      keyInstances.cbegin(), keyInstances.cend(),
      [](auto const& a, auto const& b) { return a.second < b.second; }
    );

    FTRACE(2, "Suggest prune key \"{}\".\n", keyAndCount->first);

    {
      acceptedOrders -= keyAndCount->second;
      auto const iter = arraysContainingKey.find(keyAndCount->first);
      // If we do not have any arrays registered as containing the key, then
      // we'll just rely on the sink-side counts to detect if it's common.
      auto const attributed =
        iter == arraysContainingKey.end() ? 0 : iter->second;
      if (attributed > acceptedArrays) break;
      acceptedArrays -= attributed;
      if (acceptedOrders < totalOrders * cutoff ||
          acceptedArrays < totalArrays * cutoff) {
        break;
      }
    }

    FTRACE(2, "Prune key \"{}\". Remain: ({} / {} ko), ({} / {} arrays)\n",
           keyAndCount->first, acceptedOrders, totalOrders,
           acceptedArrays, totalArrays);

    // Remove all key orders invalidated by this removal, and update the
    // instance counts.
    auto removeAt = std::partition(
      workingSet.begin(), workingSet.end(),
      [&](auto const& a) { return !a.first.contains(keyAndCount->first); }
    );
    std::for_each(
      removeAt, workingSet.end(),
      [&](auto const& pair) {
        for (auto const& key : pair.first) {
          auto const iter = keyInstances.find(key);
          assertx(iter != keyInstances.end());
          iter->second -= pair.second;
          if (iter->second == 0) {
            keyInstances.erase(iter);
          }
        }
      }
    );

    workingSet.erase(removeAt, workingSet.end());

    assertx(acceptedOrders == sumCounts(workingSet.begin(), workingSet.end()));
    assertx(keyInstances == countKeyInstances(workingSet));
  }

  // Assemble the final pruned key order.
  auto prunedResult = KeyOrderMap();
  for (auto const& [keyOrder, count] : workingSet) {
    prunedResult.emplace(keyOrder, count);
  }

  auto const result = collectKeyOrder(prunedResult);
  return result;
}

// Computes a set of keys that are guaranteed to be present in the array at
// construction time, or "invalid" if the source doesn't provide that info.
KeyOrder computeRequiredKeysAtConstruction(const LoggingProfile& source) {
  auto const vad = source.data->staticSampledArray;
  if (vad) return KeyOrder::ForArray(vad);
  if (source.key.op() != Op::NewStructDict) return KeyOrder::MakeInvalid();

  auto result = KeyOrderData();
  auto const unit = source.key.sk.unit();
  auto const imms = getImmVector(source.key.sk.pc());
  for (auto i = 0; i < imms.size(); i++) {
    auto const key = unit->lookupLitstrId(imms.vec32()[i]);
    assertx(key && key->isStatic());
    result.push_back(key);
  }
  return KeyOrder::Make(result);
}

// Computes a set of keys that are present in all the given sources when the
// array is initialized and that are never removed from the array.
KeySet computeRequiredKeys(
    const KeyOrder& keys, const std::vector<const LoggingProfile*>& profiles) {
  auto const keyOrderMap = [&]{
    auto kom = KeyOrderMap();
    for (auto const profile : profiles) {
      mergeKeyOrderMap(kom, profile->data->keyOrders);
      kom[computeRequiredKeysAtConstruction(*profile)].value++;
    }
    return kom;
  }();

  assertx(keys.valid());
  auto result = KeySet(keys.begin(), keys.end());
  auto samples = 0;

  for (auto const& pair : keyOrderMap) {
    if (!pair.first.valid()) continue;
    auto here = KeySet(pair.first.begin(), pair.first.end());
    auto remove = std::vector<const StringData*>{};
    for (auto const key : result) {
      if (!here.contains(key)) remove.push_back(key);
    }
    for (auto const key : remove) {
      result.erase(key);
    }
    samples++;
  }

  return samples ? result : KeySet();
}

// Computes a good bound on the type of each field of a StructDict layout that
// covers all of the given sources. For now, we take a simple, conservative
// approach and just union the types of all values stored to each field.
KeyBounds computeTypeBounds(
    const std::vector<const LoggingProfile*>& profiles) {
  auto result = KeyBounds{};
  for (auto const profile : profiles) {
    if (auto const ad = profile->data->staticSampledArray) {
      IterateKV(ad, [&](auto const key, auto const val){
        if (!tvIsString(key)) return;
        auto const str = key.val().pstr;
        assertx(str->isStatic());
        result[str] |= static_cast<uint8_t>(val.type());
      });
    }
    for (auto const& it : profile->data->events) {
      if (opSetsStringKey(getEventArrayOp(it.first))) {
        auto const key = getEventStrKey(it.first);
        auto const val = getEventValType(it.first);
        if (key) result[key] |= static_cast<uint8_t>(val);
      }
    }
  }

  // So far, result contains the union of the bits of any types stored to this
  // field during profiling. There are two fixes we need to make to turn it
  // into a valid StructDict field type mask:
  //
  //  1. We need to negate its bits; a type mask records the bits that must
  //     *not* be set for values of this field.
  //
  //  2. We need to relax it to a checkable type, which for now is one of:
  //     a) any single DataType; b) TUncounted; c) TCell (the trivial bound).
  //
  for (auto& it : result) {
    auto const fixed = [&]() -> uint8_t {
      auto const bound = it.second;
      if (isRealType(static_cast<DataType>(bound))) return ~bound;
      if (!(bound & kRefCountedBit)) return kRefCountedBit;
      return 0;
    }();
    it.second = fixed;
  }

  return result;
}

// Computes a list of fields, with all information required - both the keys
// and the "required" bit - that we need to construct a StructLayout.
StructLayout::FieldVector collectFieldVector(
    const KeyOrder& keys, const KeySet& requiredKeys, const KeyBounds& typeBounds) {
  assertx(keys.valid());
  StructLayout::FieldVector result;
  for (auto const key : keys) {
    auto const it = typeBounds.find(key);
    auto const type_mask = it == typeBounds.end()
      ? static_cast<uint8_t>(0) : it->second;
    result.push_back({key, requiredKeys.contains(key), type_mask});
  }
  return result;
}

// Returns true if we treat the given sink as a "merge point" and union the
// sets of sinks that are incident to that merge. These points are also the
// only ones at which we'll JIT struct access code.
bool mergeStructsAtSink(const SinkProfile& profile, const StructAnalysis& sa) {
  // 1. We must have sufficient LoggingArray coverage for the given sink.
  auto const p_cutoff = Cfg::Eval::BespokeArraySinkSpecializationThreshold / 100;
  auto const p_sampled = probabilityOfSampled(profile);
  if (p_sampled < p_cutoff) return false;

  // 2. All sources must be struct-like sources.
  KeyOrderMap kom;
  for (auto const& pair : profile.data->sources) {
    auto const index = sa.union_find.lookup(pair.first);
    if (!index) return false;
    kom.emplace(sa.key_orders[*index], 0).first->second.value++;
  }

  // 3. We must be able to construct a struct layout that covers all sources.
  auto const ko = collectKeyOrder(kom);
  if (ko.empty() || !ko.valid()) return false;

  // 4. We must not be able to split sources into two disjoint struct layouts.
  UnionFind<const StringData*> uf;
  for (auto const key : ko) {
    uf.insert(key);
  }
  for (auto const& pair : kom) {
    if (pair.first.empty()) continue;
    auto const root = *pair.first.begin();
    for (auto const key : pair.first) {
      uf.merge(root, key);
    }
  }
  return uf.countGroups() == 1;
}

void initStructAnalysis(const LoggingProfile& profile, StructAnalysis& sa) {
  // We only use a struct layout for a dict/darray array sources. We don't
  // include casts here because non-trivial casts (from vec/varray/keyset
  // dict/darray) will rarely produce useful struct-like arrays.
  auto const type_okay = [&]{
    switch (profile.key.locationType) {
      case LocationType::APCKey:
      case LocationType::Runtime:
        return true;
      case LocationType::InstanceProperty:
      case LocationType::StaticProperty:
      case LocationType::TypeConstant:
      case LocationType::TypeAlias:
      case LocationType::SrcKey:
        auto const vad = profile.data->staticSampledArray;
        if (vad != nullptr && vad->isDictType()) return true;
        auto const op = profile.key.op();
        return op == OpNewDictArray || op == OpNewStructDict;
    }
    always_assert(false);
  }();

  // Add a node for this source to our union-find table if the type matches
  // and there's a struct layout that's a good match for this source alone.
  if (!type_okay || profile.data->keyOrders.empty()) return;
  auto const threshold = keyCoverageThreshold();
  auto const ko = pruneKeyOrder({&profile}, threshold);
  if (!ko.valid()) return;
  DEBUG_ONLY auto const index = sa.union_find.insert(&profile);
  assertx(index == sa.key_orders.size());
  sa.key_orders.push_back(ko);
}

void updateStructAnalysis(const SinkProfile& profile, StructAnalysis& sa) {
  auto const& sources = profile.data->sources;
  if (sources.empty() || !mergeStructsAtSink(profile, sa)) return;
  mergeValid(sources.begin(), sources.end(), sa.union_find,
             [&](auto const& x) { return x.first; });
  sa.merge_sinks.push_back(&profile);
}

StructAnalysisResult finishStructAnalysis(StructAnalysis& sa) {
  auto const p_cutoff = Cfg::Eval::BespokeArraySourceSpecializationThreshold / 100;
  auto groups = std::vector<StructGroup>{};

  // Sort groups by weight to preferentially create hot struct layouts.
  // We also filter out groups that are likely to need escalation here and
  // groups that have a type structure as a source.
  sa.union_find.forEachGroup([&](auto& group) {
    double weight = 0;
    double p_escalated = 0;
    double type_structs = 0;
    for (auto const source : group) {
      if (source->getLayout().is_type_structure()) {
        type_structs++;
      }
      auto const source_weight = source->getProfileWeight();
      weight += source_weight;
      p_escalated += source_weight * probabilityOfEscalation(*source);
    }
    if (type_structs == 0 && weight > 0 && !(p_escalated / weight > 1 - p_cutoff)) {
      groups.emplace_back(std::move(group), weight);
    }
  });
  std::sort(groups.begin(), groups.end(),
            [](auto const& a, auto const& b) { return a.weight > b.weight; });

  using LayoutWeightMap = folly::F14FastMap<const StructLayout*, double>;
  auto layoutWeights = LayoutWeightMap{};

  // Create potential layout set.
  for (auto& group : groups) {
    KeyFrequencies frequencies;
    for (auto const source : group.profiles) {
      auto const multiplier = source->getSampleCountMultiplier();
      for (auto const& it : source->data->events) {
        auto const key = getEventStrKey(it.first);
        if (key != nullptr) frequencies[key.get()] += it.second * multiplier;
      }
    }
    group.layout = [&]() -> const StructLayout* {
      auto const pruned = pruneKeyOrder(group.profiles, keyCoverageThreshold());
      auto const keys = sortKeyOrder(pruned, frequencies);
      if (!keys.valid()) return nullptr;
      auto const typeBounds = computeTypeBounds(group.profiles);
      auto const requiredKeys = computeRequiredKeys(keys, group.profiles);
      auto const fields = collectFieldVector(keys, requiredKeys, typeBounds);
      return StructLayout::GetLayout(fields, true);
    }();
    if (group.layout) layoutWeights[group.layout] += group.weight;
  }

  auto layoutVector =
    LayoutWeightVector(layoutWeights.begin(), layoutWeights.end());

  // Find a colorable subset of the selected layouts.
  auto coloring = findKeyColoring(layoutVector);

  // Remove groups with missing StructLayouts.
  {
    groups.erase(
      std::remove_if(
        groups.begin(),
        groups.end(),
        [&](auto const& a) {
          return a.layout == nullptr;
        }
      ), groups.end()
    );
  }

  // Apply coloring to strings, and then have each layout create its hash map.
  applyColoring(coloring, layoutVector);
  std::for_each(
    layoutVector.cbegin(), layoutVector.cend(),
    [&](auto const& layout) {
      layout.first->createColoringHashMap(coloring.numColoredFields);
    }
  );

  // Assign sources to layouts.
  StructAnalysisResult result;
  for (auto const& group : groups) {
    assertx(group.layout);
    for (auto const source : group.profiles) {
      // TODO(mcolavita): exclude sources likely to escalate with final layout
      result.sources.insert({source, group.layout});
    }
  }

  // Assign sinks to layouts.
  for (auto const& sink : sa.merge_sinks) {
    // TODO(mcolavita): exclude sinks likely to escalate with final layout
    assertx(!sink->data->sources.empty());
    auto const it = result.sources.find(sink->data->sources.begin()->first);
    if (it == result.sources.end()) continue;
    auto const p_sampled = probabilityOfSampled(*sink);
    result.sinks.insert({sink, {ArrayLayout(it->second), p_sampled}});
  }
  return result;
}

//////////////////////////////////////////////////////////////////////////////
// Generic dispatch

ArrayLayout selectSourceLayout(
    LoggingProfile& profile, const StructAnalysisResult& sar) {
  assertx(profile.data);
  auto const mode = Cfg::Eval::BespokeArraySpecializationMode;
  if (mode == 1 || mode == 2) return ArrayLayout::Vanilla();

  // 1. Use a struct layout if the union-find algorithm chose one.

  auto const it = sar.sources.find(&profile);
  if (it != sar.sources.end()) {
    auto const vad = profile.data->staticSampledArray;
    if (vad != nullptr) {
      auto const sad = StructDict::MakeFromVanilla(vad, it->second);
      // If the selected layout is incompatible with the static array, produce
      // vanilla instead.
      if (sad == nullptr) return ArrayLayout::Vanilla();
      profile.setStaticBespokeArray(sad);
    }
    return ArrayLayout(it->second);
  }

  // 2. If we aren't emitting monotypes, use a vanilla layout.

  if (!Cfg::Eval::EmitBespokeMonotypes) return ArrayLayout::Vanilla();

  // 3. If the array is a runtime source, use a vanilla layout.
  // TODO(mcolavita): We can eventually support more general runtime sources.

  if (profile.key.isRuntimeLocation()) return ArrayLayout::Vanilla();

  // 4. If we escalate too often, use a vanilla layout.

  auto const p_cutoff = Cfg::Eval::BespokeArraySourceSpecializationThreshold / 100;
  auto const p_escalated = probabilityOfEscalation(profile);
  if (p_escalated > 1 - p_cutoff) return ArrayLayout::Vanilla();

  // 5. If the array is likely to stay monotyped, use a monotype layout.

  uint64_t monotype = 0;
  uint64_t total = 0;
  for (auto const& it : profile.data->entryTypes) {
    if (EntryTypes(it.first.second).isMonotypeState()) {
      monotype += it.second;
    }
    total += it.second;
  }
  auto const p_monotype = 1.0 * monotype / total;

  if (p_monotype >= p_cutoff) {
    auto const vad = profile.data->staticSampledArray;
    if (vad == nullptr) return ArrayLayout::Vanilla();
    auto const mad = maybeMonoify(vad);
    if (mad == nullptr) return ArrayLayout::Vanilla();
    profile.setStaticBespokeArray(mad);
    return ArrayLayout(mad->layoutIndex());
  }

  return ArrayLayout::Vanilla();
}

std::vector<Decision<ArrayLayout>> makeSinkDecisions(
    const SinkProfile& profile, const StructAnalysisResult& sar) {
  assertx(profile.data);
  using DAL = Decision<ArrayLayout>;
  auto const mode = Cfg::Eval::BespokeArraySpecializationMode;
  if (mode == 1) return {{ArrayLayout::Vanilla(), 1.0}};
  if (mode == 2) return {{ArrayLayout::Top(), 1.0}};

  auto const it = sar.sinks.find(&profile);
  if (it != sar.sinks.end()) return {it->second};

  auto const sampled = load(profile.data->sampledCount);
  auto const unsampled = load(profile.data->unsampledCount);
  auto const type_structures = load(profile.data->typeStructureCount);
  if (!sampled) {
    return unsampled ? std::vector<DAL>({DAL{ArrayLayout::Vanilla(), 1.0}})
                     : std::vector<DAL>({DAL{ArrayLayout::Top(), 1.0}});
  }

  uint64_t vanilla = 0;
  uint64_t monotype = 0;
  uint64_t is_struct = 0;
  uint64_t total = type_structures;

  std::unordered_map<const bespoke::Layout*, uint64_t> structs;

  for (auto const& it : profile.data->sources) {
    auto const layout = it.first->getLayout();
    auto const count = it.second;
    if (layout.vanilla()) {
      vanilla += count;
    } else if (layout.monotype()) {
      monotype += count;
    } else if (layout.is_struct()) {
      is_struct += count;
      structs[layout.bespokeLayout()] += count;
    }
    total += count;
  }

  auto const p_cutoff = isIteratorBaseAccess(profile.key.second.op()) ?
    Cfg::Eval::BespokeArraySinkIteratorSpecializationThreshold / 100 :
    Cfg::Eval::BespokeArraySinkSpecializationThreshold / 100;

  auto const layoutThreshold = Cfg::Eval::BespokeArraySinkMultiLayoutThreshold;

  auto const p_sampled = 1.0 * sampled / (sampled + unsampled);

  if (!total) {
    auto const p_vanilla = 1 - p_sampled;
    if (p_vanilla >= p_cutoff) return {{ArrayLayout::Vanilla(), p_vanilla}};
    return {{ArrayLayout::Top(), 1.0}};
  }

  auto const p_vanilla = p_sampled * vanilla / total + (1 - p_sampled);
  auto const p_monotype = p_sampled * monotype / total;
  auto const p_is_struct = p_sampled * is_struct / total;
  auto const p_is_type_structures = p_sampled * type_structures / total;

  const auto sortedStructs = [&](){
    std::vector<std::pair<const bespoke::Layout*, double>> structVec;
    structVec.reserve(structs.size());

    for (auto const& pair : structs) {
      auto const p_this = p_sampled * pair.second / total;
      structVec.push_back(std::make_pair(pair.first, p_this));
    }

    std::sort(
      structVec.begin(),
      structVec.end(),
      [](auto const& p1, auto const& p2) {
        return p1.second > p2.second;
      }
    );

    return structVec;
  }();

  // Handle vanilla layouts
  if (p_vanilla >= p_cutoff) {

    // When vanilla + top struct cover all sources, return both
    if (p_is_struct > 0 &&
        p_vanilla + p_is_struct >= layoutThreshold &&
        !isIteratorBaseAccess(profile.key.second.op())) {

      // if single struct, return it
      if (sortedStructs.size() == 1) {
        auto const [layout, prob] = sortedStructs[0];
        return {
          {ArrayLayout::Vanilla(), p_vanilla},
          {ArrayLayout(layout), prob}
        };
      }

      return {
        {ArrayLayout::Vanilla(), p_vanilla},
        {ArrayLayout(TopStructLayout::Index()), p_is_struct}
      };
    }

    // Otherwise return just vanilla
    return {{ArrayLayout::Vanilla(), p_vanilla}};
  }

  // Handle monotype layouts
  if (p_monotype >= p_cutoff) {
    using AK = ArrayData::ArrayKind;
    auto const vec = load(profile.data->arrCounts[AK::kVecKind / 2]);
    auto const dict = load(profile.data->arrCounts[AK::kDictKind / 2]);
    auto const keyset = load(profile.data->arrCounts[AK::kKeysetKind / 2]);

    assertx(vec || dict || keyset);
    if (bool(vec) + bool(dict) + bool(keyset) != 1) {
      return {{ArrayLayout::Top(), 1.0}};
    }

    auto const p_cutoff_val = 1 + p_cutoff - p_monotype;
    auto const dt_decision = selectValType(profile, p_cutoff_val);
    auto const dt = dt_decision.value;
    if (dt == kInvalidDataType) return {{ArrayLayout::Top(), 1.0}};
    auto const p_monotype_val = p_monotype + dt_decision.coverage - 1;

    if (vec) {
      return dt == KindOfUninit ?
        std::vector<DAL>({
          {ArrayLayout(TopMonotypeVecLayout::Index()), p_monotype_val}
        }) :
        std::vector<DAL>({
          {ArrayLayout(MonotypeVecLayout::Index(dt)), p_monotype_val}}
        );
    }

    if (dict) {
      auto const p_cutoff_key = 1 + p_cutoff - p_monotype_val;
      auto const kt_decision = selectKeyType(profile, p_cutoff_key);
      auto const kt = kt_decision.value;
      if (kt == KeyTypes::Any) return {{ArrayLayout::Bespoke(), p_monotype_val}};
      auto const p_monotype_key = p_monotype_val + kt_decision.coverage - 1;
      return dt == KindOfUninit ?
        std::vector<DAL>({
          {ArrayLayout(TopMonotypeDictLayout::Index(kt)), p_monotype_key}
        }) :
        std::vector<DAL>({
          {ArrayLayout(MonotypeDictLayout::Index(kt, dt)), p_monotype_key}
        });
    }
  }

  if (p_is_type_structures >= p_cutoff) {
    return {{ArrayLayout(TypeStructure::GetLayoutIndex()), p_is_type_structures}};
  }

  auto const selectMultipleLayouts = [&](double p1, double p2){
    // Do not select multiple layouts for iterator ops
    if (isIteratorBaseAccess(profile.key.second.op())) return false;

    // Require at least cutoff for multiple layouts
    if (p1 + p2 < p_cutoff) return false;

    // Require at least 1 of the layouts to be above the min threshold to gate
    // multiple layouts to only those that have a significant skew.
    auto const minThreshold =
      Cfg::Eval::BespokeArraySinkSpecializationMinThreshold / 100;
    if (p1 < minThreshold && p2 < minThreshold) return false;

    return true;
  };

  // Handle struct layouts
  if (p_is_struct >= p_cutoff) {
    auto const& [layout_first, p_first] = sortedStructs[0];

    if (sortedStructs.size() > 1 &&
        !isIteratorBaseAccess(profile.key.second.op())) {
      auto const& [layout_second, p_second] = sortedStructs[1];

      if (p_first + p_second >= p_cutoff &&
          selectMultipleLayouts(p_first, p_second)) {
        return {
          {ArrayLayout(layout_first), p_first},
          {ArrayLayout(layout_second), p_second},
        };
      }
    }

    if (p_first >= p_cutoff) {
      return {{ArrayLayout(layout_first), p_first}};
    }

    // When top struct + vanilla cover all sources, return both
    if (p_vanilla > 0 &&
        p_is_struct + p_vanilla >= layoutThreshold &&
        !isIteratorBaseAccess(profile.key.second.op())) {
      return {
        {ArrayLayout(TopStructLayout::Index()), p_is_struct},
        {ArrayLayout::Vanilla(), p_vanilla}
      };
    }

    return {{ArrayLayout(TopStructLayout::Index()), p_is_struct}};
  }

  // Handle combined vanilla + struct layouts
  if (selectMultipleLayouts(p_vanilla, p_is_struct)) {
    auto const structLayout = sortedStructs.size() == 1 ?
      DAL({ArrayLayout(sortedStructs[0].first), sortedStructs[0].second}) :
      DAL({ArrayLayout(TopStructLayout::Index()), p_is_struct});

    return p_vanilla > p_is_struct ?
      std::vector<DAL>({{ArrayLayout::Vanilla(), p_vanilla}, structLayout}) :
      std::vector<DAL>({structLayout, {ArrayLayout::Vanilla(), p_vanilla}});
    }

  return {{ArrayLayout::Top(), 1.0}};
}

SinkLayouts selectSinkLayouts(
    const SinkProfile& profile, const StructAnalysisResult& sar) {
  auto const decisions = makeSinkDecisions(profile, sar);
  assertx(decisions.size() > 0);

  auto const sideExit = [&] {
    // We do not specialize iterators for bespoke layouts unless we side exit.
    if (isIteratorBaseAccess(profile.key.second.op())) return true;

    auto const coverage = std::accumulate(
      decisions.begin(),
      decisions.end(),
      0.0,
      [](double acc, const bespoke::Decision<ArrayLayout>& decision){
        return acc + decision.coverage;
      }
    );

    if (coverage < Cfg::Eval::BespokeArraySinkSideExitThreshold / 100) {
      return false;
    }

    auto const count = profile.data->sources.size();
    if (count > Cfg::Eval::BespokeArraySinkSideExitMaxSources) return false;

    auto const min_samples = Cfg::Eval::BespokeArraySinkSideExitMinSampleCount;
    for (auto const& it : profile.data->sources) {
      if (it.second < min_samples) return false;
    }

    return true;
  }();

  SinkLayouts sls;
  sls.sideExit = sideExit;

  for (auto const& decision : decisions) {
    sls.layouts.push_back({decision.value, decision.coverage});
  }

  return sls;
}

//////////////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////////////

ArrayLayout layoutForSource(SrcKey sk) {
  auto const profile = getLoggingProfile(sk);
  return profile ? profile->getLayout() : ArrayLayout::Vanilla();
}

// Computes the layouts for a given sink by looking at the layouts for each
// TransID. If there are <= 2 total layouts then they are returned in frequency
// order. If there are more than 2 layouts, they will be unioned and returned.
SinkLayouts layoutsForSink(
    const jit::TransIDSet& ids, SrcKey sk, ArrayLayout knownLayout) {
  auto result = SinkLayouts{};

  // Track layout frequencies across the multiple layouts
  folly::F14FastMap<uint16_t, double> layoutFrequencies;

  for (auto const& id : ids) {
    auto const transCounter = jit::profData()->transCounter(id);
    if (auto const profile = getSinkProfile(id, sk)) {
      auto sls = profile->getLayouts();
      result.sideExit &= sls.sideExit;

      // Record the frequencies for each layout
      for (auto const& layout : sls.layouts) {
        auto const l = layout.layout & knownLayout;
        if (l == ArrayLayout::Bottom()) continue;
        layoutFrequencies[l.toUint16()] += layout.coverage * transCounter;
      }
    }
  }

  // For <= 2 layouts just return them in sorted order
  if (layoutFrequencies.size() <= 2 && !isIteratorBaseAccess(sk.op())) {
    std::vector<std::pair<uint16_t, double>>
      sortedFrequencies(layoutFrequencies.begin(), layoutFrequencies.end());
    std::sort(
      sortedFrequencies.begin(),
      sortedFrequencies.end(),
      [](auto const& p1, auto const& p2){
        return p1.second > p2.second;
      }
    );

    for (auto const& [layout, _] : sortedFrequencies) {
      result.layouts.push_back(SinkLayout{ArrayLayout::FromUint16(layout)});
    }
  } else {
    // Otherwise union all of the layouts and return the result
    SinkLayout sl;
    for (auto const& [layout, _] : layoutFrequencies) {
      sl.layout = sl.layout | ArrayLayout::FromUint16(layout);
    }

    if (sl.layout == ArrayLayout::Bottom()) {
      return {{{knownLayout, 1.0}}, false};
    }

    result.layouts = {std::move(sl)};
  }

  if (result.layouts.empty()) {
    return {{{knownLayout, 1.0}}, false};
  }

  return result;
}

void selectBespokeLayouts() {
  // On successfully deserializing layout decisions, a jumpstart consumer
  // will take care of finalizing the layout hierarchy.
  if (Layout::HierarchyFinalized()) return;

  setLoggingEnabled(false);

  auto const sar = []{
    if (!Cfg::Eval::EmitBespokeStructDicts) return StructAnalysisResult();
    StructAnalysis sa;
    eachSource([&](auto const& x) { initStructAnalysis(x, sa); });
    eachSink([&](auto const& x) { updateStructAnalysis(x, sa); });
    return finishStructAnalysis(sa);
  }();
  eachSource([&](auto& x) { x.setLayout(selectSourceLayout(x, sar)); });
  eachSink([&](auto& x) { x.setLayouts(selectSinkLayouts(x, sar)); });
  Layout::FinalizeHierarchy();
  startExportProfiles();
}

//////////////////////////////////////////////////////////////////////////////

}
