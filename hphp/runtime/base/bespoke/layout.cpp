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

#include <fmt/format.h>

#include "hphp/runtime/base/bespoke/layout.h"

#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/bespoke/monotype-dict.h"
#include "hphp/runtime/base/bespoke/monotype-vec.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/util/trace.h"

#include <atomic>
#include <array>
#include <vector>
#include <sstream>

namespace HPHP::bespoke {

//////////////////////////////////////////////////////////////////////////////

using namespace jit;

namespace {

auto constexpr kMaxNumLayouts = (1 << 16) - 1;

std::atomic<size_t> s_topoIndex;
std::array<Layout*, kMaxNumLayouts> s_layoutTable;
size_t s_numStructLayouts; // For logging.
std::atomic<bool> s_hierarchyFinal = false;
std::mutex s_layoutCreationMutex;

LayoutFunctions s_emptyVtable;

constexpr LayoutIndex kBespokeTopIndex = {0};

bool checkLayoutTest(
    const std::vector<uint16_t>& liveVec,
    const std::vector<uint16_t>& deadVec,
    LayoutTest test) {
  for (auto const live : liveVec) {
    if (!test.accepts(live)) return false;
  }
  for (auto const dead : deadVec) {
    if (test.accepts(dead)) return false;
  }
  return true;
}

Optional<LayoutTest> compute2ByteTest(
    const std::vector<uint16_t>& liveVec,
    const std::vector<uint16_t>& deadVec) {
  assertx(!liveVec.empty());
  auto const val = liveVec[0];
  auto const cmp = LayoutTest { val, LayoutTest::Cmp2Byte };
  if (checkLayoutTest(liveVec, deadVec, cmp)) return cmp;

  auto imm = uint16_t(-1);
  for (auto const live : liveVec) {
    imm &= ~live;
  }
  auto const and_ = LayoutTest { imm, LayoutTest::And2Byte };
  if (checkLayoutTest(liveVec, deadVec, and_)) return and_;

  return std::nullopt;
}

std::string show(LayoutTest test) {
  switch (test.mode) {
    case LayoutTest::And1Byte:
      return fmt::format("AND {:02x}xx", test.imm & 0xff);
    case LayoutTest::And2Byte:
      return fmt::format("AND {:04x}", test.imm);
    case LayoutTest::Cmp1Byte:
      return fmt::format("CMP {:02x}xx", test.imm & 0xff);
    case LayoutTest::Cmp2Byte:
      return fmt::format("CMP {:04x}", test.imm);
  }
  always_assert(false);
}

}

LayoutFunctionsDispatch g_layout_funcs;

Layout::Layout(LayoutIndex index, std::string description, LayoutSet parents,
               const LayoutFunctions* vtable)
  : m_index(index)
  , m_description(std::move(description))
  , m_parents(std::move(parents))
  , m_vtable(vtable ? vtable : &s_emptyVtable)
{
  std::lock_guard<std::mutex> lock(s_layoutCreationMutex);
  assertx(!s_hierarchyFinal.load(std::memory_order_acquire));
  assertx(m_index.raw < kMaxNumLayouts);
  assertx(s_layoutTable[m_index.raw] == nullptr);
  s_layoutTable[m_index.raw] = this;
  m_topoIndex = s_topoIndex++;
  assertx(checkInvariants());
}

BespokeArray* Layout::coerce(ArrayData* ad) const {
  assertx(ad->isVanilla());
  auto const layout = ArrayLayout(this);

  if (layout.monotype()) {
    return maybeMonoify(ad);
  } else if (layout.is_struct()) {
    return StructDict::MakeFromVanilla(ad, StructLayout::As(this));
  } else if (layout.is_type_structure()) {
    return TypeStructure::MakeFromVanilla(ad);
  }
  always_assert(false);
}

/*
 * A BFS implementation used to traverse the bespoke type lattice.
 */
struct Layout::BFSWalker {
  BFSWalker(bool upward, LayoutIndex initIdx)
    : m_workQ({initIdx})
    , m_upward(upward)
  {}

  template <typename C>
  BFSWalker(bool upward, C&& col)
    : m_workQ(col.cbegin(), col.cend())
    , m_upward(upward)
  {}

  /*
   * Return the next new node in the graph discovered by BFS, or std::nullopt if
   * the BFS has terminated.
   */
  Optional<LayoutIndex> next() {
    while (!m_workQ.empty()) {
      auto const index = m_workQ.front();
      m_workQ.pop_front();
      if (m_processed.find(index) != m_processed.end()) continue;
      m_processed.insert(index);
      auto const layout = FromIndex(index);
      auto const nextSet = m_upward ? layout->m_parents : layout->m_children;
      for (auto const next : nextSet) {
        m_workQ.push_back(next);
      }
      return index;
    }
    return std::nullopt;
  }

  /*
   * Checks if the BFS has encountered a given node.
   */
  bool hasSeen(LayoutIndex index) const {
    return m_processed.find(index) != m_processed.end();
  }

  /*
   * Returns the full set of nodes seen during the BFS.
   */
  LayoutSet allSeen() const { return m_processed; }

private:
  LayoutSet m_processed;
  std::deque<LayoutIndex> m_workQ;
  bool m_upward;
};

/*
 * Computes whether the layout is a descendant of another layout. To do so, we
 * simply perform an upward BFS from the current layout until we encounter the
 * other layout or the BFS terminates.
 */
bool Layout::isDescendantOfDebug(const Layout* other) const {
  BFSWalker walker(true, index());
  while (auto const idx = walker.next()) {
    if (idx == other->index()) return true;
  }
  return false;
}

/*
 * Compute the set of ancestors by running upward DFS until termination.
 */
Layout::LayoutSet Layout::computeAncestors() const {
  BFSWalker walker(true, index());
  while (walker.next()) {}
  return walker.allSeen();
}

/*
 * Compute the set of descendants by running downward DFS until termination.
 */
Layout::LayoutSet Layout::computeDescendants() const {
  BFSWalker walker(false, index());
  while (walker.next()) {}
  return walker.allSeen();
}

bool Layout::checkInvariants() const {
  if (!allowBespokeArrayLikes()) return true;

  for (auto const DEBUG_ONLY parent : m_parents) {
    auto const DEBUG_ONLY layout = FromIndex(parent);
    assertx(layout);
    assertx(layout->m_topoIndex < m_topoIndex);
  }

  return true;
}

struct Layout::DescendantOrdering {
  bool operator()(const Layout* a, const Layout* b) const {
    return a->m_topoIndex < b->m_topoIndex;
  }
};

struct Layout::AncestorOrdering {
  bool operator()(const Layout* a, const Layout* b) const {
    return a->m_topoIndex > b->m_topoIndex;
  }
};

void Layout::ClearHierarchy() {
  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    if (i != kBespokeTopIndex.raw) s_layoutTable[i] = nullptr;
  }
  s_numStructLayouts = 0;
}

void Layout::FinalizeHierarchy() {
  assertx(allowBespokeArrayLikes());
  assertx(!s_hierarchyFinal.load(std::memory_order_acquire));
  s_numStructLayouts = 0;
  std::vector<Layout*> allLayouts;
  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    auto const layout = s_layoutTable[i];
    if (!layout) continue;
    if (ArrayLayout(layout).is_struct()) s_numStructLayouts++;
    allLayouts.push_back(layout);
    assertx(layout->checkInvariants());
    for (auto const pIdx : layout->m_parents) {
      auto const parent = s_layoutTable[pIdx.raw];
      assertx(parent);
      parent->m_children.insert(layout->index());
    }
  }

  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    auto layout = s_layoutTable[i];
    if (!layout) continue;
    auto const descendants = layout->computeDescendants();
    std::transform(
      descendants.begin(), descendants.end(),
      std::back_inserter(layout->m_descendants),
      [&] (LayoutIndex i) { return s_layoutTable[i.raw]; }
    );
    auto const ancestors = layout->computeAncestors();
    std::transform(
      ancestors.begin(), ancestors.end(),
      std::back_inserter(layout->m_ancestors),
      [&] (LayoutIndex i) { return s_layoutTable[i.raw]; }
    );

    std::sort(layout->m_descendants.begin(), layout->m_descendants.end(),
              DescendantOrdering{});
    std::sort(layout->m_ancestors.begin(), layout->m_ancestors.end(),
              AncestorOrdering{});
  }

  // Compute mask and compare sets in topological order so that they can use
  // their children's masks if necessary.
  std::sort(allLayouts.begin(), allLayouts.end(), AncestorOrdering{});
  for (auto const layout : allLayouts) {
    if (layout->index() == kBespokeTopIndex) continue;
    layout->m_layout_test = layout->computeLayoutTest();
  }

  s_hierarchyFinal.store(true, std::memory_order_release);
}

bool Layout::HierarchyFinalized() {
  return s_hierarchyFinal.load(std::memory_order_acquire);
}

bool Layout::operator<=(const Layout& other) const {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  auto const res = std::binary_search(m_ancestors.begin(), m_ancestors.end(),
                                      &other, AncestorOrdering{});
  assertx(isDescendantOfDebug(&other) == res);
  return res;
}

const Layout* Layout::operator|(const Layout& other) const {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  auto lIter = m_ancestors.cbegin();
  auto rIter = other.m_ancestors.cbegin();
  auto const lt = AncestorOrdering{};
  while (lIter != m_ancestors.cend() && rIter != other.m_ancestors.cend()) {
    if (*lIter == *rIter) {
      return *lIter;
    }
    if (lt(*lIter, *rIter)) {
      lIter++;
    } else {
      rIter++;
    }
  }
  not_reached();
}

const Layout* Layout::operator&(const Layout& other) const {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  auto lIter = m_descendants.cbegin();
  auto rIter = other.m_descendants.cbegin();
  auto const lt = DescendantOrdering{};
  while (lIter != m_descendants.cend() && rIter != other.m_descendants.cend()) {
    if (*lIter == *rIter) {
      return *lIter;
    }
    if (lt(*lIter, *rIter)) {
      lIter++;
    } else {
      rIter++;
    }
  }
  return nullptr;
}

const Layout* Layout::FromIndex(LayoutIndex index) {
  auto const layout = s_layoutTable[index.raw];
  assertx(layout != nullptr);
  assertx(layout->index() == index);
  return layout;
}

std::string Layout::dumpAllLayouts() {
  std::ostringstream ss;
  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    auto const layout = s_layoutTable[i];
    if (!layout) continue;
    ss << layout->dumpInformation();
  }
  return ss.str();
}

std::string Layout::dumpInformation() const {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));

  auto const concreteDesc = [&](const Layout* layout) {
    return layout->isConcrete() ? "Concrete" : "Abstract";
  };

  std::ostringstream ss;
  ss << fmt::format("{:04x}: {} [{}]\n", m_index.raw, describe(),
                       concreteDesc(this));

  auto const top = index() == kBespokeTopIndex;
  auto const type_test = top ? "kind() & 1" : show(m_layout_test);
  ss << fmt::format("  Type test: {}\n", type_test);

  ss << fmt::format("  Ancestors:\n");
  for (auto const ancestor : m_ancestors) {
    ss << fmt::format("  - {:04x}: {} [{}]\n",
                        ancestor->m_index.raw, ancestor->describe(),
                        concreteDesc(ancestor));
  }

  ss << fmt::format("  Descendants:\n");
  for (auto const descendant : m_descendants) {
    ss << fmt::format("  - {:04x}: {} [{}]\n",
                        descendant->m_index.raw, descendant->describe(),
                        concreteDesc(descendant));
  }

  ss << "\n";

  return ss.str();
}

LayoutTest Layout::computeLayoutTest() const {
  // The set of all concrete layouts that descend from the layout.
  std::vector<uint16_t> liveVec;
  for(auto const layout : m_descendants) {
    if (!layout->isConcrete()) continue;
    liveVec.push_back(layout->m_index.raw);
  }
  assertx(!liveVec.empty());
  std::sort(liveVec.begin(), liveVec.end());

  // The set of all possible concrete layouts.
  std::vector<uint16_t> allConcrete;
  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    auto const layout = s_layoutTable[i];
    if (!layout) continue;
    if (!layout->isConcrete()) continue;
    allConcrete.push_back(layout->m_index.raw);
  }

  // The set of all concrete layouts that do *not* descend from this layout.
  // This set includes the vanilla index, so that our test will exclude it.
  std::vector<uint16_t> deadVec;
  std::set_difference(
    allConcrete.cbegin(), allConcrete.cend(),
    liveVec.cbegin(), liveVec.cend(),
    std::back_inserter(deadVec)
  );
  static_assert(ArrayData::kVanillaLayoutIndex.raw == uint16_t(-1));
  deadVec.push_back(ArrayData::kVanillaLayoutIndex.raw);

  SCOPE_ASSERT_DETAIL("bespoke::Layout::computeLayoutTest") {
    std::string ret = fmt::format("{:04x}: {}\n", m_index.raw, describe());
    ret += fmt::format("  Live:\n");
    for (auto const live : liveVec) {
      auto const layout = s_layoutTable[live]->describe();
      ret += fmt::format("  - {:04x}: {}\n", live, layout);
    }
    ret += fmt::format("  Dead:\n");
    for (auto const dead : deadVec) {
      auto const layout = dead == ArrayData::kVanillaLayoutIndex.raw
        ? "Vanilla" : s_layoutTable[dead]->describe();
      ret += fmt::format("  - {:04x}: {}\n", dead, layout);
    }
    return ret;
  };

  // Try a 1-byte test on the layout's high byte. Fall back to a 2-byte test.
  auto const result = [&]{
    std::vector<uint16_t> live1ByteVec;
    for (auto const live : liveVec) {
      live1ByteVec.push_back(live >> 8);
    }
    std::vector<uint16_t> dead1ByteVec;
    for (auto const dead : deadVec) {
      dead1ByteVec.push_back(dead >> 8);
    }

    if (auto const test = compute2ByteTest(live1ByteVec, dead1ByteVec)) {
      auto one_byte = *test;
      one_byte.mode = [&]{
        switch (one_byte.mode) {
          case LayoutTest::And1Byte: always_assert(false);
          case LayoutTest::And2Byte: return LayoutTest::And1Byte;
          case LayoutTest::Cmp1Byte: always_assert(false);
          case LayoutTest::Cmp2Byte: return LayoutTest::Cmp1Byte;
        }
        always_assert(false);
      }();
      one_byte.imm = uint16_t(int8_t(one_byte.imm & 0xff));
      return one_byte;
    }

    if (auto const test = compute2ByteTest(liveVec, deadVec)) return *test;

    always_assert(false);
  }();

  SCOPE_ASSERT_DETAIL("selected layout test") { return show(result); };

  always_assert(checkLayoutTest(liveVec, deadVec, result));

  return result;
}

LayoutTest Layout::getLayoutTest() const {
  assertx(index() != kBespokeTopIndex);
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  return m_layout_test;
}

struct Layout::Initializer {
  Initializer() {
    AbstractLayout::InitializeLayouts();
    LoggingArray::InitializeLayouts();
    MonotypeVec::InitializeLayouts();
    EmptyMonotypeDict::InitializeLayouts();
    TypeStructure::InitializeLayouts();
  }
};
Layout::Initializer Layout::s_initializer;

//////////////////////////////////////////////////////////////////////////////

ArrayLayout Layout::appendType(Type /*val*/) const {
  return ArrayLayout::Top();
}

ArrayLayout Layout::removeType(Type /*key*/) const {
  return ArrayLayout::Top();
}

ArrayLayout Layout::setType(Type /*val*/) const {
  return ArrayLayout::Top();
}

ArrayLayout Layout::setType(Type /*key*/, Type /*val*/) const {
  return ArrayLayout::Top();
}

std::pair<Type, bool> Layout::elemType(Type /*key*/) const {
  return {TInitCell, false};
}

bool Layout::slotAlwaysPresent(const Type& /*slot*/) const {
  return false;
}

std::pair<Type, bool> Layout::firstLastType(bool /*isFirst*/, bool isKey) const {
  return {isKey ? (TInt | TStr) : TInitCell, false};
}

Type Layout::iterPosType(Type /*pos*/, bool isKey) const {
  return isKey ? (TInt | TStr) : TInitCell;
}

Type Layout::getTypeBound(Type /*slot*/) const {
  return TCell;
}

Optional<int64_t> Layout::numElements() const {
  return std::nullopt;
}

//////////////////////////////////////////////////////////////////////////////

AbstractLayout::AbstractLayout(LayoutIndex index,
                               std::string description,
                               LayoutSet parents,
                               const LayoutFunctions* vtable /*=nullptr*/)
  : Layout(index, std::move(description), std::move(parents), vtable)
{}

void AbstractLayout::InitializeLayouts() {
  new AbstractLayout(kBespokeTopIndex, "BespokeTop", {});
}

LayoutIndex AbstractLayout::GetBespokeTopIndex() {
  return kBespokeTopIndex;
}

ConcreteLayout::ConcreteLayout(LayoutIndex index,
                               std::string description,
                               LayoutSet parents,
                               const LayoutFunctions* vtable)
  : Layout(index, std::move(description), std::move(parents), vtable)
{
  assertx(vtable);
  auto const vindex = index.byte() & kBespokeVtableMask;
#define X(Return, Name, Args...) {                          \
    assertx(vtable->fn##Name);                              \
    auto& entry = g_layout_funcs.fn##Name[vindex];          \
    assertx(entry == nullptr || entry == vtable->fn##Name); \
    entry = vtable->fn##Name;                               \
  }
  BESPOKE_LAYOUT_FUNCTIONS(ArrayData)
  BESPOKE_SYNTHESIZED_LAYOUT_FUNCTIONS(ArrayData)
#undef X
}

const ConcreteLayout* ConcreteLayout::FromConcreteIndex(LayoutIndex index) {
  auto const layout = s_layoutTable[index.raw];
  assertx(layout != nullptr);
  assertx(layout->index() == index);
  assertx(layout->isConcrete());
  return reinterpret_cast<ConcreteLayout*>(layout);
}

//////////////////////////////////////////////////////////////////////////////

namespace {

template<typename Bespokify>
ArrayData* maybeBespokifyForTesting(ArrayData* ad,
                                    LoggingProfile* profile,
                                    std::atomic<ArrayData*>& cache,
                                    Bespokify bespokify) {
  if (!profile->data) return ad;
  auto const bad = cache.load(std::memory_order_acquire);
  if (bad) return bad;

  auto const result = bespokify(ad, profile);
  if (!result) return ad;
  ad->decRefAndRelease();

  // We should cache a static bespoke array iff this profile is for a static
  // array constructor or static prop - i.e. iff staticLoggingArray is set.
  if (!profile->data->staticLoggingArray) return result;

  ArrayData* current = nullptr;
  if (cache.compare_exchange_strong(current, result)) return result;
  ArrayData::FreeStatic(result);
  return current;
}

}

void logBespokeDispatch(const BespokeArray* bad, const char* fn) {
  if (Trace::moduleEnabled(Trace::bespoke, 6)) {
    DEBUG_ONLY auto const sk = getSrcKey();
    DEBUG_ONLY auto const layout = Layout::FromIndex(bad->layoutIndex());
    TRACE_MOD(Trace::bespoke, 6, "Bespoke dispatch: %s: %s::%s\n",
              sk.valid() ? sk.getSymbol().data() : "(unknown)",
              layout->describe().data(), fn);
  }
}

BespokeArray* maybeMonoify(ArrayData* ad) {
  if (!ad->isVanilla() || ad->isKeysetType()) return nullptr;

  auto const et = EntryTypes::ForArray(ad);
  if (!et.isMonotypeState()) return nullptr;

  auto const legacy = ad->isLegacyArray();

  if (et.valueTypes == ValueTypes::Empty) {
    switch (ad->toDataType()) {
      case KindOfVec:  return EmptyMonotypeVec::GetVec(legacy);
      case KindOfDict: return EmptyMonotypeDict::GetDict(legacy);
      default: always_assert(false);
    }
  }

  auto const dt = et.valueDatatype;
  return ad->isDictType()
    ? MakeMonotypeDictFromVanilla(ad, dt, et.keyTypes)
    : MonotypeVec::MakeFromVanilla(ad, dt);
}

StructLayout::FieldVector collectFieldVector(const KeyOrder& ko) {
  if (!ko.valid()) return {};
  StructLayout::FieldVector result;
  for (auto const key : ko) result.push_back({key, false, 0});
  return result;
}

BespokeArray* maybeStructify(ArrayData* ad, const LoggingProfile* profile) {
  if (!ad->isVanilla() || ad->isKeysetType()) return nullptr;

  assertx(profile->data);
  auto const& koMap = profile->data->keyOrders;
  if (koMap.empty()) return nullptr;

  auto const ko = collectKeyOrder(koMap);
  auto const create = !s_hierarchyFinal.load(std::memory_order_acquire);
  auto const fields = collectFieldVector(ko);
  auto const layout = StructLayout::GetLayout(fields, create);
  return layout ? StructDict::MakeFromVanilla(ad, layout) : nullptr;
}

ArrayData* makeBespokeForTesting(ArrayData* ad, LoggingProfile* profile) {
  if (!jit::mcgen::retranslateAllEnabled()) {
    return bespoke::maybeMakeLoggingArray(ad, profile);
  }
  auto const mod = requestCount() % 4;
  if (mod == 1) return bespoke::maybeMakeLoggingArray(ad, profile);
  if (mod == 2) {
    return bespoke::maybeBespokifyForTesting(
      ad, profile, profile->data->staticMonotypeArray,
      [](auto ad, auto /*profile*/) { return maybeMonoify(ad); }
    );
  }
  return ad;
}

void eachLayout(std::function<void(Layout& layout)> fn) {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  for (size_t i = 0; i < kMaxNumLayouts; i++) {
    auto const layout = s_layoutTable[i];
    if (layout) fn(*layout);
  }
}

Layout** layoutsForJIT() {
  assertx(s_hierarchyFinal.load(std::memory_order_acquire));
  return s_layoutTable.data();
}

size_t numStructLayouts() {
  if (!s_hierarchyFinal.load(std::memory_order_acquire)) return 0;
  return s_numStructLayouts;
}

//////////////////////////////////////////////////////////////////////////////

}
