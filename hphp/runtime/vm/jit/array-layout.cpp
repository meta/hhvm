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

#include "hphp/runtime/vm/jit/array-layout.h"

#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/bespoke/monotype-dict.h"
#include "hphp/runtime/base/bespoke/monotype-vec.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"

namespace HPHP::jit {

//////////////////////////////////////////////////////////////////////////////

namespace {

using Sort = ArrayLayout::Sort;

auto constexpr kBasicSortMask    = 0b11;
auto constexpr kBasicSortShift   = 0b11;
auto constexpr kBasicSortUnshift = 0b01;

// A "basic sort" is just one of the four named Sort enum values. If `sort`
// is non-basic, then Sort::Bottom < sort < Sort::Bespoke.
constexpr bool isBasicSort(Sort sort) {
  return sort <= Sort::Bespoke;
}

// Converts non-basic sorts (which are subtypes of Bespoke) to Bespoke.
constexpr Sort toBasicSort(Sort sort) {
  return std::min(sort, Sort::Bespoke);
}

// If we mask a basic sort, we'll get a value such that | and & bit ops on
// that value correspond to | and & type operations on the original sort.
constexpr int maskBasicSort(Sort sort) {
  assertx(isBasicSort(sort));
  return kBasicSortMask & (int(sort) + kBasicSortShift);
}

static_assert(maskBasicSort(Sort::Top)     == 0b11);
static_assert(maskBasicSort(Sort::Vanilla) == 0b01);
static_assert(maskBasicSort(Sort::Bespoke) == 0b10);
static_assert(maskBasicSort(Sort::Bottom)  == 0b00);

// This operation is the inverse of the maskBasicSort operation above.
constexpr Sort unmaskBasicSort(int masked) {
  auto const result = Sort(kBasicSortMask & (masked + kBasicSortUnshift));
  assertx(isBasicSort(result));
  return result;
}

static_assert(unmaskBasicSort(maskBasicSort(Sort::Top))     == Sort::Top);
static_assert(unmaskBasicSort(maskBasicSort(Sort::Vanilla)) == Sort::Vanilla);
static_assert(unmaskBasicSort(maskBasicSort(Sort::Bespoke)) == Sort::Bespoke);
static_assert(unmaskBasicSort(maskBasicSort(Sort::Bottom))  == Sort::Bottom);

// Returns the basic sort that is the intersection of the given basic sorts.
constexpr Sort intersectBasicSort(Sort a, Sort b) {
  return unmaskBasicSort(maskBasicSort(a) & maskBasicSort(b));
}

// Returns the basic sort that is the union of the given basic sorts.
constexpr Sort unionBasicSort(Sort a, Sort b) {
  return unmaskBasicSort(maskBasicSort(a) | maskBasicSort(b));
}

// Returns the sort (either Bespoke, or non-basic) for this bespoke layout.
Sort sortFromLayoutIndex(bespoke::LayoutIndex index) {
  return Sort(index.raw + int(Sort::Bespoke));
}

const bespoke::Layout& assertBespoke(ArrayLayout layout) {
  auto const result = layout.bespokeLayout();
  assertx(result != nullptr);
  return *result;
}

}

//////////////////////////////////////////////////////////////////////////////

ArrayLayout::ArrayLayout(bespoke::LayoutIndex index)
  : sort(sortFromLayoutIndex(index))
{
  assertx(bespoke::Layout::FromIndex(*layoutIndex()));
}

ArrayLayout::ArrayLayout(const bespoke::Layout* layout)
  : sort(sortFromLayoutIndex(layout->index()))
{
  assertx(bespoke::Layout::FromIndex(*layoutIndex()));
}

bool ArrayLayout::operator<=(const ArrayLayout& o) const {
  if (*this == o) return true;
  if (o == Top()) return true;
  if (*this == Bottom()) return true;

  // The max chain length on basic sorts alone is three:
  //
  //   Bottom < {Vanilla,Bespoke} < Top
  //
  // We took care of the Bottom, Top, and equality cases above. Further, if o
  // is non-basic, it's a strict subtype of Bespoke. So we can return here.
  if (isBasicSort(sort)) return false;

  if (isBasicSort(o.sort)) return o == Bespoke();
  return assertBespoke(*this) <= assertBespoke(o);
}

ArrayLayout ArrayLayout::operator|(const ArrayLayout& o) const {
  if (*this == o) return o;
  if (o == Bottom()) return *this;
  if (*this == Bottom()) return o;

  // If either side is captured as a basic sort, then the result is, too.
  if (isBasicSort(sort) || isBasicSort(o.sort)) {
    return ArrayLayout(unionBasicSort(toBasicSort(sort), toBasicSort(o.sort)));
  }

  return ArrayLayout(assertBespoke(*this) | assertBespoke(o));
}

ArrayLayout ArrayLayout::operator&(const ArrayLayout& o) const {
  if (*this == o) return o;
  if (o == Top()) return *this;
  if (*this == Top()) return o;

  // We only intersect bespoke layouts if toBasicSort is Bespoke for both.
  auto const meet = intersectBasicSort(toBasicSort(sort), toBasicSort(o.sort));
  if (meet != Sort::Bespoke) return ArrayLayout(meet);

  // If either type is Bespoke (i.e. "bespoke top"), return the other type.
  if (o == Bespoke()) return *this;
  if (*this == Bespoke()) return o;
  auto const result = assertBespoke(*this) & assertBespoke(o);
  return result ? ArrayLayout(result) : Bottom();
}

bool ArrayLayout::logging() const {
  auto const index = layoutIndex();
  return index && *index == bespoke::LoggingArray::GetLayoutIndex();
}

bool ArrayLayout::monotype() const {
  auto const index = layoutIndex();
  if (!index) return false;
  return bespoke::isMonotypeVecLayout(*index) ||
         bespoke::isMonotypeDictLayout(*index);
}

bool ArrayLayout::is_struct() const {
  auto const index = layoutIndex();
  return index && bespoke::StructLayout::IsStructLayout(*index);
}

bool ArrayLayout::is_type_structure() const {
  auto const index = layoutIndex();
  return index && *index == bespoke::TypeStructure::GetLayoutIndex();
}

bool ArrayLayout::is_concrete() const {
  auto const layout = bespokeLayout();
  return layout && layout->isConcrete();
}

const bespoke::Layout* ArrayLayout::bespokeLayout() const {
  auto const index = layoutIndex();
  if (!index) return nullptr;
  return bespoke::Layout::FromIndex(*index);
}

Optional<bespoke::LayoutIndex> ArrayLayout::layoutIndex() const {
  auto const index = int(sort) - int(Sort::Bespoke);
  if (index < 0) return {};
  return bespoke::LayoutIndex { safe_cast<uint16_t>(index) };
}

LayoutTest ArrayLayout::bespokeLayoutTest() const {
  assertx(!isBasicSort(sort));
  auto const& layout = assertBespoke(*this);
  return layout.getLayoutTest();
}

const bespoke::Layout* ArrayLayout::irgenLayout() const {
  auto const index = std::max(int(sort) - int(Sort::Bespoke), 0);
  return bespoke::Layout::FromIndex({safe_cast<uint16_t>(index)});
}

std::string ArrayLayout::describe() const {
  if (isBasicSort(sort)) {
    switch (sort) {
      case Sort::Top:     return "Top";
      case Sort::Vanilla: return "Vanilla";
      case Sort::Bespoke: return "Bespoke";
      case Sort::Bottom:  return "Bottom";
    }
  }
  return assertBespoke(*this).describe();
}

ArrayData* ArrayLayout::apply(ArrayData* ad) const {
  assertx(ad->isStatic());

  if (bespoke::TypeStructure::isBespokeTypeStructure(ad)) return ad;
  assertx(ad->isVanilla());

  auto const result = [&]() -> ArrayData* {
    if (vanilla() || logging()) return ad;
    return bespokeLayout()->coerce(ad);
  }();

  SCOPE_ASSERT_DETAIL("ArrayLayout::apply") { return describe(); };
  always_assert(result != nullptr);
  return result;
}

//////////////////////////////////////////////////////////////////////////////

ArrayLayout ArrayLayout::appendType(Type val) const {
  if (vanilla()) return ArrayLayout::Vanilla();
  if (isBasicSort(sort)) return ArrayLayout::Top();
  return bespokeLayout()->appendType(val);
}

ArrayLayout ArrayLayout::removeType(Type key) const {
  if (vanilla()) return ArrayLayout::Vanilla();
  if (isBasicSort(sort)) return ArrayLayout::Top();
  return bespokeLayout()->removeType(key);
}

ArrayLayout ArrayLayout::setType(Type val) const {
  if (vanilla()) return ArrayLayout::Vanilla();
  if (isBasicSort(sort)) return ArrayLayout::Top();
  return bespokeLayout()->setType(val);
}

ArrayLayout ArrayLayout::setType(Type key, Type val) const {
  if (vanilla()) return ArrayLayout::Vanilla();
  if (isBasicSort(sort)) return ArrayLayout::Top();
  return bespokeLayout()->setType(key, val);
}

std::pair<Type, bool> ArrayLayout::elemType(Type key) const {
  if (isBasicSort(sort)) return {TInitCell, false};
  return bespokeLayout()->elemType(key);
}

bool ArrayLayout::slotAlwaysPresent(const Type& slot) const {
  if (!is_struct()) return false;
  return bespokeLayout()->slotAlwaysPresent(slot);
}

std::pair<Type, bool> ArrayLayout::firstLastType(
    bool isFirst, bool isKey) const {
  if (isBasicSort(sort)) return {isKey ? (TInt | TStr) : TInitCell, false};
  return bespokeLayout()->firstLastType(isFirst, isKey);
}

Type ArrayLayout::iterPosType(Type pos, bool isKey) const {
  if (isBasicSort(sort)) return isKey ? (TInt | TStr) : TInitCell;
  return bespokeLayout()->iterPosType(pos, isKey);
}

Type ArrayLayout::getTypeBound(Type slot) const {
  if (isBasicSort(sort)) return TCell;
  return bespokeLayout()->getTypeBound(slot);
}

Optional<int64_t> ArrayLayout::numElements() const {
  if (isBasicSort(sort)) return std::nullopt;
  return bespokeLayout()->numElements();
}

//////////////////////////////////////////////////////////////////////////////

namespace {

using bespoke::LoggingProfileKey;
using bespoke::SinkProfileKey;
using bespoke::StructLayout;
using FieldVector = bespoke::StructLayout::FieldVector;

void write_field_vector(ProfDataSerializer& ser, const StructLayout* layout) {
  auto const num = layout->numFields();
  write_raw(ser, num);
  for (auto slot = 0; slot < num; ++slot) {
    auto const& f = layout->field(slot);
    assertx(f.key->color() != StringData::kInvalidColor);
    write_string(ser, f.key);
    write_raw(ser, f.required);
    write_raw(ser, f.type_mask);
  }
}

FieldVector read_field_vector(ProfDataDeserializer& des) {
  auto data = FieldVector{};
  auto const num = read_raw<size_t>(des);
  for (auto i = 0; i < num; i++) {
    auto const key = read_string(des);
    auto const required = read_raw<bool>(des);
    auto const type_mask = read_raw<uint8_t>(des);
    data.push_back({key, required, type_mask});
  }
  return data;
}

void write_source_key(ProfDataSerializer& ser, const LoggingProfileKey& key) {
  write_raw(ser, key.locationType);
  switch (key.locationType) {
    case bespoke::LocationType::SrcKey:
      write_srckey(ser, key.sk);
      break;
    case bespoke::LocationType::APCKey:
      write_raw(ser, key.ak);
      break;
    case bespoke::LocationType::TypeAlias:
      write_typealias(ser, key.ta);
      break;
    case bespoke::LocationType::InstanceProperty:
    case bespoke::LocationType::StaticProperty:
    case bespoke::LocationType::TypeConstant:
      write_class(ser, key.cls);
      write_raw(ser, key.slot);
      break;
    case bespoke::LocationType::Runtime:
      write_string(ser, key.runtimeStruct->getStableIdentifier());
      break;
    default: always_assert(false);
  }
}

LoggingProfileKey read_source_key(ProfDataDeserializer& des) {
  LoggingProfileKey key(SrcKey{});
  read_raw(des, key.locationType);
  switch (key.locationType) {
    case bespoke::LocationType::SrcKey:
      key.sk = read_srckey(des);
      break;
    case bespoke::LocationType::APCKey:
      read_raw(des, key.ak);
      break;
    case bespoke::LocationType::TypeAlias:
      key.ta = read_typealias(des);
      break;
    case bespoke::LocationType::InstanceProperty:
    case bespoke::LocationType::StaticProperty:
    case bespoke::LocationType::TypeConstant:
      key.cls = read_class(des);
      read_raw(des, key.slot);
      break;
    case bespoke::LocationType::Runtime: {
      auto const stableIdentifier = read_string(des);
      key.runtimeStruct = RuntimeStruct::findById(stableIdentifier);
      break;
    }
    default: always_assert(false);
  }
  return key;
}

void write_sink_key(ProfDataSerializer& ser, const SinkProfileKey& key) {
  write_raw(ser, key.first);
  write_srckey(ser, key.second);
}

SinkProfileKey read_sink_key(ProfDataDeserializer& des) {
  auto const trans = read_raw<TransID>(des);
  return SinkProfileKey(trans, read_srckey(des));
}

void write_sink_layout(ProfDataSerializer& ser, bespoke::SinkLayout sl) {
  write_raw(ser, sl.layout);
  write_raw(ser, sl.coverage);
}

void write_sink_layouts(ProfDataSerializer& ser, bespoke::SinkLayouts sls) {
  write_raw(ser, safe_cast<size_t>(sls.layouts.size()));
  for (auto const &sl : sls.layouts) {
    write_sink_layout(ser, sl);
  }
  write_raw(ser, sls.sideExit);
}

bespoke::SinkLayout read_sink_layout(ProfDataDeserializer& des) {
  auto result = bespoke::SinkLayout{};
  result.layout = read_layout(des);
  result.coverage = read_raw<double>(des);
  return result;
}

bespoke::SinkLayouts read_sink_layouts(ProfDataDeserializer& des) {
  auto result = bespoke::SinkLayouts{};
  auto const layouts = read_raw<size_t>(des);
  for (size_t i = 0; i < layouts; i++) {
    result.layouts.push_back(read_sink_layout(des));
  }
  read_raw(des, result.sideExit);
  return result;
}

}

struct RuntimeStructSerde {
  static void write_runtime_struct(
      ProfDataSerializer& ser, const RuntimeStruct* runtimeStruct) {
    write_string(ser, runtimeStruct->m_stableIdentifier);

    write_raw(ser, runtimeStruct->m_fields.size());
    for (auto const key : runtimeStruct->m_fields) {
      if (key) {
        write_raw(ser, true);
        write_string(ser, key);
      } else {
        write_raw(ser, false);
      }
    }

    auto const layout = runtimeStruct->m_assignedLayout.load();
    if (layout) {
      write_raw(ser, true);
      write_raw(ser, layout->index());
    } else {
      write_raw(ser, false);
    }
  }

  static void deserialize_runtime_struct(ProfDataDeserializer& des) {
    auto const stableIdentifier = read_string(des);
    auto const fieldSize = read_raw<size_t>(des);

    auto fields = RuntimeStruct::FieldKeys(fieldSize, nullptr);
    for (int i = 0; i < fieldSize; i++) {
      auto const present = read_raw<bool>(des);
      if (present) fields[i] = read_string(des);
    }

    auto const runtimeStruct =
      RuntimeStruct::deserialize(stableIdentifier, std::move(fields));

    auto const hasLayout = read_raw<bool>(des);
    if (hasLayout) {
      auto const layoutIdx = read_raw<bespoke::LayoutIndex>(des);
      auto const layout = bespoke::Layout::FromIndex(layoutIdx);
      runtimeStruct->applyLayout(bespoke::StructLayout::As(layout));
    }
  }
};

void serializeBespokeLayouts(ProfDataSerializer& ser) {
  // For now, we only need to serialize and deserialize StructLayouts,
  // because they are the only dynamically-constructed layouts.
  std::vector<const StructLayout*> layouts;
  bespoke::eachLayout([&](auto const& layout) {
    if (!layout.isConcrete() || !ArrayLayout(&layout).is_struct()) return;
    layouts.push_back(StructLayout::As(&layout));
  });
  // TopStructLayout is not concrete and is not serialized.
  assertx(layouts.size() == 0 ||
          layouts.size() == bespoke::numStructLayoutsCreated() - 1);
  write_raw(ser, layouts.size());
  write_raw(ser, StructLayout::maxColoredFields());
  for (auto const layout : layouts) {
    write_raw(ser, layout->index());
    write_field_vector(ser, layout);
  }

  // Serialize the runtime sources.
  std::vector<const RuntimeStruct*> runtimeStructs;
  RuntimeStruct::eachRuntimeStruct([&](RuntimeStruct* runtimeStruct) {
    runtimeStructs.push_back(runtimeStruct);
  });
  write_raw(ser, runtimeStructs.size());
  for (auto const runtimeStruct : runtimeStructs) {
    RuntimeStructSerde::write_runtime_struct(ser, runtimeStruct);
  }

  // Serialize the decisions we made at all sources and sinks.
  write_raw(ser, bespoke::countSources());
  bespoke::eachSource([&](auto const& profile) {
    write_source_key(ser, profile.key);
    write_raw(ser, profile.getLayout());
  });
  write_raw(ser, bespoke::countSinks());
  bespoke::eachSink([&](auto const& profile) {
    write_sink_key(ser, profile.key);
    write_sink_layouts(ser, profile.getLayouts());
  });
}

void deserializeBespokeLayouts(ProfDataDeserializer& des) {
  always_assert(bespoke::countSources() == 0);
  always_assert(bespoke::countSinks() == 0);
  bespoke::setLoggingEnabled(false);
  always_assert(bespoke::countSources() == 0);
  always_assert(bespoke::countSinks() == 0);

  auto const layouts = read_raw<size_t>(des);
  auto const maxColoredFields = read_raw<size_t>(des);
  for (auto i = 0; i < layouts; i++) {
    auto const index = read_raw<bespoke::LayoutIndex>(des);
    auto const fields = read_field_vector(des);
    auto const layout = StructLayout::Deserialize(index, fields);
    layout->createColoringHashMap(maxColoredFields);
  }
  auto const runtimeStructs = read_raw<size_t>(des);
  for (auto i = 0; i < runtimeStructs; i++) {
    RuntimeStructSerde::deserialize_runtime_struct(des);
  }
  // Read every key before applying any layout. Reading a key can define its
  // class, and a class copies its parent's property and constant values when
  // it is defined. Applying a layout replaces the source's value in place with
  // a bespoke array, so a subclass defined after its parent's source was
  // applied would inherit that bespoke array as its own source's value.
  auto const sources = read_raw<size_t>(des);
  std::vector<std::pair<bespoke::LoggingProfileKey, ArrayLayout>> decisions;
  decisions.reserve(sources);
  for (auto i = 0; i < sources; i++) {
    auto const key = read_source_key(des);
    decisions.emplace_back(key, read_layout(des));
  }
  for (auto const& [key, layout] : decisions) {
    bespoke::deserializeSource(key, layout);
  }
  assertx(bespoke::countSources() == sources);
  auto const sinks = read_raw<size_t>(des);
  for (auto i = 0; i < sinks; i++) {
    assertx(bespoke::countSinks() == i);
    auto const key = read_sink_key(des);
    bespoke::deserializeSink(key, read_sink_layouts(des));
    assertx(bespoke::countSinks() == i + 1);
  }
  bespoke::Layout::FinalizeHierarchy();
}

//////////////////////////////////////////////////////////////////////////////

}
