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

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

#include <folly/Demangle.h>
#include <fmt/core.h>
#include <folly/Hash.h>
#include <folly/Memory.h>
#include <folly/Singleton.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>

#include <boost/program_options.hpp>
#include <utility>
#include <variant>

#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_vector.h>

#include "hphp/tools/debug-parser/debug-parser.h"

#include "hphp/util/assertions.h"
#include "hphp/util/file.h"
#include "hphp/util/match.h"
#include "hphp/util/type-scan.h"

/*
 * Program responsible for taking the parsed debug-information from TypeParser,
 * analyzing it (along with user-provided annotations), and generating C++ code
 * for GC type-scanners. These type-scanners will then be compiled into a shared
 * object which can be loaded at start-up if GC is enabled.
 *
 * Some general concepts:
 *
 * - "Scanner". A scanner is responsible for reporting pointer information about
 *    a set of types to the GC. Generally it will report a list of pointers to
 *    other potentially interesting types, as well as address ranges that should
 *    be conservatively scanned.
 *
 * - "Collectable". A type is collectable if MarkCollectable<> or
 *   MarkScannableCollectable<> is instantiated on it. Type-scanners are never
 *   generated for collectable types, it is assumed their scanners will be
 *   hand-written. The exception is if MarkScannableCollectable<> is used, in
 *   which case they'll be scanned if explicitly requested. The point of the
 *   type-scanners is to determine how to find pointers to collectable types
 *   from other types. Collectable types correspond to the set of types in HHVM
 *   which are explicitly managed by the GC.
 *
 * - "Indexed". An indexed type is a combination of a type and an action. These
 *   occur from an instantiation of Indexer<>. Any particular type can be part
 *   of multiple indexed types but with different actions. Every indexed type
 *   receives a unique type-index and will have a scanner generated for
 *   it. Belonging to an indexed type generally marks that the type is allocated
 *   on the req-heap (with one exception). The action influences the behavior of
 *   that particular scanner. They are:
 *
 *        (*) Ignore -- Scanner is trivial.
 *        (*) Auto -- The default. Attempt to generate scanner automatically.
 *        (*) Conservative<T...> -- If any of T... is "interesting" (see below),
 *                                  conservative scan. Otherwise, ignore.
 *                                  If T... is an empty list, always
 *                                  conservative scan.
 *        (*) WithSuffix<T> -- Attempt to generate scanner for type as normal.
 *                             However, any allocation of the type only contains
 *                             one instance of that type. Any remaining
 *                             allocated memory is assumed to be filled with
 *                             instances of T. So, a scanner for T will be
 *                             generated as well.
 *        (*) ForScan -- Similar to Auto, but this type should not be marked as
 *                       "pointer followable" (see below) if this is the only
 *                       indexed type for the type. (This is a marker that the
 *                       type is never actually req-heap allocated).
 *
 * - "Interesting". A type is interesting if the scanner generated for it would
 *   be non-trivial. IE, it would have at least one action. Interesting
 *   types either contain interesting types, pointers to "pointer
 *   followable" types, or have some custom action defined on it.
 *
 * - "Pointer followable". A type is pointer followable if it is a collectable
 *   type, it has a collectable type as a base, or if it is a member of at least
 *   one indexed type which has an action which is not "ForScan" and is
 *   interesting. By these conditions, a pointer followable type is one which is
 *   known to be allocated out of the request heap, and transitively leads to a
 *   collectable type via some chain of pointers. Pointers to pointer followable
 *   types are enqueued inside scanners. Pointers to non-pointer followable
 *   types are ignored. All base classes of pointer followable object types are
 *   also pointer followable (to handle polymorphism).
 *
 * - "Layout". Abstract description of a scanner. A layout contains a list of
 *   offsets and actions to perform at those offsets. Since many types have
 *   similar structure, multiple types can share the same layout.
 *
 * The goal of the scanner generator is find the maximal set of pointer
 * followable types, use those to compute the layout of each indexed type, then
 * output those layouts in the form of C++ code.
 *
 * Note: this entire scheme assumes that all pointer followable types can be
 * reached via some indexed type. If not, that pointer followable type is a
 * root, and must be dealt with specially by the GC.
 */

namespace {

// fast_map/set maps to F14{Value,Vector}Map/Set depending on K+V size.
// Entries are moved (if possible) or copied (if necessary) on rehash & erase.
template<class K, class V, class H=std::hash<K>, class C=std::equal_to<K>>
using fast_map = folly::F14FastMap<K,V,H,C>;
template<class T, class H=std::hash<T>, class C=std::equal_to<T>>
using fast_set = folly::F14FastSet<T,H,C>;

// node_map/set allocate K+V separately like std::unordered_map; K+V don't
// move during rehash. Saves memory compared to fast_map/set when when K+V
// is large.
template<class K, class V, class H=std::hash<K>, class C=std::equal_to<K>>
using node_map = folly::F14NodeMap<K,V,H,C>;
template<class T, class H=std::hash<T>, class C=std::equal_to<T>>
using node_set = folly::F14NodeSet<T,H,C>;

////////////////////////////////////////////////////////////////////////////////

using namespace debug_parser;

struct Generator {
 private:
  struct IndexedType;
  struct Layout;
  struct LayoutError;

  // Action is a description of any special behavior an object type needs when
  // generating a scanner for it. It abstracts away how this behavior is
  // communicated within the generator.
  struct Action {
    // Ignore everything, a trivial scanner
    bool ignore_all = false;

    // Conservative scan everything (not including bases)
    bool conservative_all = false;
    // Conservative scan all bases
    bool conservative_all_bases = false;

    // This type should be ignored (used for system types we can't modify). This
    // is different from ignore_all where ignore_all will still process base
    // classes as normal, but whitelisted will not process anything, even base
    // classes.
    bool whitelisted = false;

    // This type is a template which should not be instantiated on any req
    // allocated types.
    bool forbidden_template = false;

    // Symbol of the custom scanner function which handles the entire type. If
    // present, but empty, the scanner does not have linkage (which is an
    // error).
    HPHP::Optional<std::string> custom_all;

    // Symbol of the custom scanner function which handles scanning base
    // classes. If present, but empty, the scanner does not have linkage (which
    // is an error).
    HPHP::Optional<std::string> custom_bases_scanner;

    // If non-empty, the name of the field in the object which is a "flexible
    // array" member (a trailing array of unbound size). Each object can only
    // have one of these.
    std::string flexible_array_field;

    // If a custom scanner for the object type is specified, it will only be
    // invoked if any of the types in the custom guards list is interesting. If
    // the list is empty, the custom scanner is always invoked.
    fast_set<const Type*> custom_guards;

    // List of fields in the object which should be ignored.
    fast_set<std::string> ignore_fields;

    // List of fields in the object which should always be conservative scanned.
    fast_set<std::string> conservative_fields;

    // Map of field names to symbols of custom scanners for that field.
    fast_map<std::string, std::string> custom_fields;

    // List of immediate base classes which should be ignored.
    fast_set<const Object*> ignored_bases;

    // List of immediate bases which the "forbidden template" check should not
    // be applied to. Mainly used internally.
    fast_set<const Object*> silenced_bases;

    // If a custom scanner function for bases is specified, the list of
    // immediate bases which the scanner applies to.
    fast_set<const Object*> custom_bases;

    // For certain actions it can immediately be known that its associated
    // object will always be interesting. Therefore, any indexed type with such
    // an action can immediately be marked as pointer followable, which helps us
    // reach a fixed point faster. This is only an optimization, so the criteria
    // does not need to be exact.
    bool isAlwaysNonTrivial() const {
      return !ignore_all && !whitelisted && (
        conservative_all || conservative_all_bases ||
        (custom_all && custom_guards.empty()) ||
        (custom_bases_scanner && !custom_bases.empty()) ||
        !conservative_fields.empty() || !custom_fields.empty());
    }
  };

  struct ObjectNameHasher {
    size_t operator()(const ObjectTypeName& name) const {
      return folly::Hash()(name.name, name.linkage);
    }
  };
  struct ObjectNameEquals {
    bool operator()(const ObjectTypeName& n1,
                    const ObjectTypeName& n2) const {
      return std::tie(n1.name, n1.linkage) ==
        std::tie(n2.name, n2.linkage);
    }
  };

  // For initializing type indices, we can use either the symbol name of the
  // Indexer<> instantiation (preferred), or its raw memory address. We have to
  // use the raw memory address for Indexer<> instantiations which do not have
  // external linkage.
  using Address = std::variant<std::string, uintptr_t>;

 public:
  // Parse out all the debug information out of the specified file and do the
  // analysis generating the layouts.
  explicit Generator(const std::string&, bool skip);

  // Turn the layouts into C++ code, writing to the specified ostream.
  void operator()(std::ostream&, bool skip) const;
 private:
  static bool isTemplateName(const std::string& candidate,
                             const std::string& name);

  static bool isMarkCollectableName(const std::string&);
  static bool isMarkScannableCollectableName(const std::string&);
  static bool isIndexerName(const std::string&);
  static bool isConservativeActionName(const std::string&);
  static bool isWithSuffixActionName(const std::string&);

  static std::string splitFieldName(const std::string& input,
                                    const std::string& prefix);

  static const Type& stripModifiers(const Type&);

  static int compareTypes(const Type&, const Type&);

  static int compareIndexedTypes(const IndexedType&,
                                 const IndexedType&,
                                 bool for_merge = false);

  static void sanityCheckTemplateParams(const Object&);

  bool findMemberHelper(const std::string& field, const Object &a_object) const;
  void genAllLayouts();
  void checkForLayoutErrors() const;
  void assignUniqueLayouts();

  template <typename T, typename F, typename C> T extractFromMarkers(
    const C&, F&&
  ) const;

  size_t determineSize(const Type&) const;

  const Object& getObject(const ObjectType&) const;

  const Object& getMarkedCollectable(const Object&) const;

  void genLayout(const Type&, Layout&, size_t,
                 bool conservative_everything = false) const;
  void genLayout(const Object&, Layout&, size_t,
                 bool do_forbidden_check = true,
                 bool conservative_everything = false) const;
  bool checkMemberSpecialAction(const Object& base_object,
                                const Object::Member& member,
                                const Action& action,
                                Layout& layout,
                                size_t base_obj_offset,
                                size_t offset) const;

  IndexedType getIndexedType(const Object&) const;

  template <typename C>
  std::vector<IndexedType> getIndexedTypes(const C&) const;

  std::vector<IndexedType> mergeDupIndexedTypes(
    std::vector<Generator::IndexedType>
  ) const;

  bool hasCollectableBase(const Object& object) const;

  bool forbiddenTemplateCheck(const Type& type) const;
  bool forbiddenTemplateCheck(const Object& object) const;

  bool hasEmptyLayout(const Type& type) const;
  bool hasEmptyLayout(const Object& object) const;

  bool isPtrFollowable(const Type& type) const;
  bool isPtrFollowable(const Object& object) const {
    return m_ptr_followable.find(&object) != m_ptr_followable.end();
  }

  void makePtrFollowable(const Type& type);
  void makePtrFollowable(const Object& object);

  const Action& getAction(const Object& object,
                          bool conservative_everything = false) const;
  Action inferAction(const Object& object,
                     bool conservative_everything = false) const;

  void genMetrics(std::ostream&) const;
  void genForwardDecls(std::ostream&) const;
  void genDataTable(std::ostream&) const;
  void genIndexInit(std::ostream&) const;
  void genScannerFuncs(std::ostream&) const;
  void genBuiltinScannerFuncs(std::ostream&) const;
  void genScannerFunc(std::ostream&,
                      const Layout&,
                      size_t begin_offset = 0) const;

  std::unique_ptr<TypeParser> m_parser;

  /*
   * Maps manipulated by getObject(). These index the known object types in
   * various ways to allow for the proper lookup of an object type according to
   * its linkage. They also allow that getObject() will always return the same
   * Object& for the same type, even if the object types' keys are different,
   * which means we can use pointer equality to compare types.
   *
   * They are marked mutable so that getObject() can remain const.
   */

  mutable tbb::concurrent_unordered_map<
    ObjectTypeName,
    tbb::concurrent_vector<ObjectTypeKey>,
    ObjectNameHasher,
    ObjectNameEquals
  > m_objects_by_name;

  mutable node_map<
    std::string,
    Object
  > m_external_objects;

  mutable node_map<
    CompileUnitId,
    node_map<
      std::string,
      Object
    >
  > m_internal_objects;

  mutable node_map<
    ObjectTypeId,
    Object
  > m_unique_objects;

  // Mapping of object types to their computed actions. We could compute the
  // action every time we needed it, but they're stored in this table for
  // memoization. This table is mutable as well since its a cache.
  mutable node_map<const Object*, Action> m_actions; // XXX must be node

  // List of all indexed types in the debug information.
  std::vector<IndexedType> m_indexed_types;

  // Set of all types which are currently known to be pointer followable or
  // collectable. The collectable set is set once, and should never change,
  // while the pointer followable set can grow as more pointer followable types
  // are discovered (it must grow monotonically, never removing anything).
  fast_set<const Object*> m_ptr_followable;
  fast_set<const Object*> m_collectable;
  fast_set<const Object*> m_scannable_collectable;

  // List of all layouts. Once computed, the indexed types will have an index
  // into this table for its associated layout.
  std::vector<Layout> m_layouts;

  // Set of objects whose layout is currently being generated. Used to detect
  // possible infinite recursion.
  mutable fast_set<const Object*> m_layout_being_generated;

  // Static strings used to identify certain special types in the debug info,
  // which serve as markers for special actions. These strings should stay in
  // sync with the types in type-scan.h.
  static constexpr const char* const s_mark_collectable_name =
    "HPHP::type_scan::MarkCollectable";
  static constexpr const char* const s_mark_scannable_collectable_name =
    "HPHP::type_scan::MarkScannableCollectable";
  static constexpr const char* const s_indexer_name =
    "HPHP::type_scan::detail::Indexer";
  static constexpr const char* const s_auto_action_name =
    "HPHP::type_scan::Action::Auto";
  static constexpr const char* const s_scan_action_name =
    "HPHP::type_scan::detail::ScanAction";
  static constexpr const char* const s_with_suffix_action_name =
    "HPHP::type_scan::Action::WithSuffix";
  static constexpr const char* const s_ignore_action_name =
    "HPHP::type_scan::Action::Ignore";
  static constexpr const char* const s_conservative_action_name =
    "HPHP::type_scan::Action::Conservative";
  static constexpr const char* const s_scanner_name =
    "HPHP::type_scan::Scanner";
};

/*
 * As stated above, layout is a description of a particular scanner. It is a
 * list of offsets, along with a certain action to perform at that
 * offset. Computing layouts for indexed types is the purpose of the scanner
 * generator. A layout is not inherently tied to any particular type, as many
 * different types can share the same layout (this is especially common for
 * templated types, like req::ptr<>).
 *
 * A layout can currently contain three different types of actions. The first is
 * to report the value of a pointer at a certain offset. The second is to
 * conservative scan a range given by a start offset and a length. The last is
 * to invoke a custom function (with a certain symbol), passing a pointer formed
 * by the offset as the first parameter (which serves as the this pointer).
 *
 * A layout can also have a suffix layout. Normally, for a given block of
 * memory, the layout will be applied contiguously until the entire block is
 * processed. However, if the layout has a suffix layout, the base layout is
 * only applied once on the beginning of the block. The suffix layout will be
 * applied contiguously on the remaining portion of the block. This is to handle
 * object types with "flexible array members".
 */

struct Generator::Layout {
  struct Ptr {
    size_t offset;

    bool operator==(const Ptr& other) const {
      return offset == other.offset;
    }
    bool operator!=(const Ptr& other) const {
      return !(*this == other);
    }
    bool operator<(const Ptr& other) const {
      return offset < other.offset;
    }
  };

  struct Conservative {
    size_t offset;
    size_t size;

    bool operator==(const Conservative& other) const {
      return std::tie(offset, size) == std::tie(other.offset, other.size);
    }
    bool operator!=(const Conservative& other) const {
      return !(*this == other);
    }
    bool operator<(const Conservative& other) const {
      return std::tie(offset, size) < std::tie(other.offset, other.size);
    }
  };

  struct Custom {
    size_t offset;
    std::string linkage_name;

    bool operator==(const Custom& other) const {
      return std::tie(offset, linkage_name) ==
        std::tie(other.offset, other.linkage_name);
    }
    bool operator!=(const Custom& other) const {
      return !(*this == other);
    }
    bool operator<(const Custom& other) const {
      return std::tie(offset, linkage_name) <
        std::tie(other.offset, other.linkage_name);
    }
  };

  explicit Layout(size_t size) : size{size} {}

  Layout(const Layout& other);
  Layout(Layout&&) = default;

  Layout& operator=(const Layout& other);
  Layout& operator=(Layout&&) = default;

  void addPtr(size_t offset) {
    ptrs.emplace_back(Ptr{offset});
  }
  void addConservative(size_t offset, size_t size);
  void addCustom(size_t offset, std::string linkage) {
    custom.emplace_back(Custom{offset, linkage});
  }

  // A sub-layout is used to insert a layout into another layout N times. Used
  // for arrays, where we compute the layout once, but want to insert it for the
  // number of elements in the array.
  void addSubLayout(size_t offset,
                    size_t count,
                    const Layout& layout) {
    for (size_t i = 0; i < count; ++i) {
      merge(layout, offset + i*layout.size);
    }
  }

  void setSuffix(Layout other) {
    suffix = std::make_unique<Layout>(std::move(other));
  }

  void merge(const Layout& other, size_t offset);

  bool isEmpty() const {
    return ptrs.empty() && conservative.empty() &&
      custom.empty() && (!suffix || suffix->isEmpty());
  }

  // Is this layout made up of nothing but conservative scans?
  bool isAllConservative() const;

  void clear();

  size_t totalCount() const;

  bool operator==(const Layout& other) const;
  bool operator!=(const Layout& other) const { return !(*this == other); }
  bool operator<(const Layout& other) const;

  size_t size;
  std::vector<Ptr> ptrs;
  std::vector<Conservative> conservative;
  std::vector<Custom> custom;
  std::unique_ptr<Layout> suffix;
  // Offset where the suffix begins. This may be different than the size because
  // of flexible array members (which sometimes start within the object).
  size_t suffix_begin = size;
};

/*
 * If the generator cannot automatically create a layout, it will throw
 * LayoutError. These LayoutErrors are gathered and reported together at the end
 * of processing. They include context about why and where the error occurred.
 */
struct Generator::LayoutError {
  explicit LayoutError(std::string reason): reason{std::move(reason)} {}

  void addContext(std::string str) {
    context.emplace_back(std::move(str));
  }

  std::string reason;
  std::vector<std::string> context;
};

/*
 * An indexed type is a combination of a type and a set of actions associated
 * with. Every indexed type is associated a layout (hence a scanner), but a
 * given type can be part of multiple indexed types. Type-indices are handed out
 * on a per indexed type basis.
 *
 * Its important to distinguish the actions that are inherent to a type
 * (embodied in the Action class), with the actions assigned inside an indexed
 * type. The actions inherent to the type are used when automatically generating
 * a scanner for that type, while the actions here override those. However,
 * since indexed types are keyed by a type index, they are only applied on
 * specific instances of the type with that type index at runtime. So, for
 * example, a type T can have an automatically generated scanner for some
 * instances of T, but a conservative scanner for others depending on which type
 * index the instance has.
 */
struct Generator::IndexedType {
  IndexedType(const Type& type, size_t type_size, Address address)
    : type{&type}
    , layout{type_size} { addresses.emplace_back(address); }

  // Underlying type
  const Type* type;

  // List of addresses for all the associated Indexer<> instances.
  std::vector<Address> addresses;

  // Marker that instances of the type with this type index do not represent
  // actual heap allocations (we only want the scanner).
  bool scan = false;

  // Ignore instances of the type with this type index. If this is true, and
  // conservative is true as well, it means that the conservative scan is still
  // conditional.
  bool ignore = false;

  // Conservative scan instances of the type with this type index.
  bool conservative = false;

  // If conservative is true, only actually conservative scan if any of these
  // types are interesting. If not, ignore. If the list is empty, always
  // conservative scan.
  std::vector<const Type*> conservative_guards;

  // A suffix type is used as the basis for a suffix layout as described above.
  const Type* suffix_type = nullptr;

  // Generated layout. This is used when generating layouts, but after that,
  // layout_index will index into the layout table for the actual layout to use.
  Layout layout;

  // Index into the layout table for the assigned layout.
  size_t layout_index = 0;

  // If there was an error while automatically generating the layout for this
  // underlying type, it is stored here until they are all reported.
  HPHP::Optional<LayoutError> errors;
};

size_t NumThreads = 24;

Generator::Generator(const std::string& filename, bool skip) {
  // Either this platform has no support for parsing debug information, or the
  // preprocessor symbol to enable actually building scanner isn't
  // enabled. Either way, just bail out. Everything will get a conservative
  // scanner by default if someone actually tries to use the scanners at
  // runtime.
  if (skip) return;

  m_parser = TypeParser::make(filename, NumThreads);

  tbb::concurrent_vector<ObjectType> indexer_types;
  tbb::concurrent_vector<ObjectType> collectable_markers;
  tbb::concurrent_vector<ObjectType> scannable_collectable_markers;

  // Iterate through all the objects the debug info parser found, storing the
  // MarkCollectable<> markers, and the Indexer<> instances. For everything,
  // store in the appropriate getObject() maps, which will allow us to do
  // getObject() lookups afterwards.
  //
  // There can be a lot of objects to iterate over, so do it concurrently.
  {
    auto const block_count = m_parser->getObjectBlockCount();
    std::atomic<size_t> next_block{0};

    auto const run = [&]{
      while (true) {
        auto const block = next_block++;
        if (block >= block_count) break;

        m_parser->forEachObjectInBlock(
          block,
          [&](const ObjectType& type) {
            if (isIndexerName(type.name.name)) {
              indexer_types.push_back(type);
            } else if (isMarkCollectableName(type.name.name)) {
              collectable_markers.push_back(type);
            } else if (isMarkScannableCollectableName(type.name.name)) {
              collectable_markers.push_back(type);
              scannable_collectable_markers.push_back(type);
            }

            // Incomplete types are useless for our purposes, so just ignore
            // them.
            if (type.incomplete) return;

            m_objects_by_name[type.name].push_back(type.key);
          }
        );
      }
    };

    std::vector<std::thread> threads;
    // No point in creating more threads than there are blocks.
    for (auto i = size_t{0}; i < std::min(block_count, NumThreads); ++i) {
      threads.emplace_back(std::thread(run));
    }
    for (auto& t : threads) t.join();
  }

  // Complain if it looks like we don't have any debug info enabled.
  // (falls back to conservative scanning for everything)
  if (collectable_markers.empty() && indexer_types.empty()) {
    throw Exception("No collectable or indexed types found. "
                    "Is debug-info enabled?");
  }

  // Extract all the types that Mark[Scannable]Collectable<> was instantiated on
  // to obtain all the types which are collectable. Since all collectable types
  // are automatically pointer followable, mark them as such.
  m_collectable = extractFromMarkers<decltype(m_collectable)>(
    collectable_markers,
    [&](const Object& o) { return &getMarkedCollectable(o); }
  );
  m_scannable_collectable =
    extractFromMarkers<decltype(m_scannable_collectable)>(
      scannable_collectable_markers,
      [&](const Object& o) { return &getMarkedCollectable(o); }
  );
  for (const auto* obj : m_collectable) {
    makePtrFollowable(*obj);
  }

  // Extract all the IndexedType information out of the Indexer<>
  // instantiations.
  m_indexed_types = getIndexedTypes(indexer_types);

  // Before beginning the actual layout generation, we can speed things up a bit
  // by marking any types which we know are always pointer followable. This will
  // let us reach the fixed point in fewer iterations.
  for (const auto& indexed : m_indexed_types) {
    // Indexed types just for scanning are never pointer followable (because
    // they're not actually heap allocated).
    if (indexed.scan) continue;

    // If the underlying type is an object type, and its associated action is
    // always non-trivial, or if the object type has a collectable type as a
    // base class, then the object type is always pointer followable. Same logic
    // for any suffix type.
    if (const auto* obj = stripModifiers(*indexed.type).asObject()) {
      const auto& object = getObject(*obj);
      if (getAction(object).isAlwaysNonTrivial() ||
          hasCollectableBase(object)) {
        makePtrFollowable(object);
      }
    }

    if (indexed.suffix_type) {
      const auto& suffix_type = stripModifiers(*indexed.suffix_type);
      if (const auto* obj = suffix_type.asObject()) {
        const auto& object = getObject(*obj);
        if (getAction(object).isAlwaysNonTrivial()) {
          makePtrFollowable(*indexed.type);
        }
      }
    }

    // If this indexed type is always going to be a complete conservative scan,
    // than we're always going to have a non-trivial action for its scanner, so
    // it's always pointer followable.
    if (indexed.conservative && indexed.conservative_guards.empty()) {
      makePtrFollowable(*indexed.type);
    }
  }

  genAllLayouts();
  checkForLayoutErrors();
  assignUniqueLayouts();
}

// Helper function. Match a name against another name, ignoring the template
// parameter portion.
bool Generator::isTemplateName(const std::string& candidate,
                               const std::string& name) {
  auto index = candidate.find(name);
  if (index != 0) return false;
  if (name.size() >= candidate.size()) return false;
  if (candidate[name.size()] != '<') return false;
  if (candidate.rfind('>') == std::string::npos) return false;
  return true;
}

// Helper functions to check if an object type's name is that of a special
// marker type:

bool Generator::isMarkCollectableName(const std::string& name) {
  return isTemplateName(name, s_mark_collectable_name);
}
bool Generator::isMarkScannableCollectableName(const std::string& name) {
  return isTemplateName(name, s_mark_scannable_collectable_name);
}
bool Generator::isIndexerName(const std::string& name) {
  return isTemplateName(name, s_indexer_name);
}
bool Generator::isConservativeActionName(const std::string& name) {
  return isTemplateName(name, s_conservative_action_name);
}
bool Generator::isWithSuffixActionName(const std::string& name) {
  return isTemplateName(name, s_with_suffix_action_name);
}

// Split a member function of the form "[prefix][suffix]_" into just "suffix"
// and return it. Returns the empty string if the input doesn't match that
// format.
std::string Generator::splitFieldName(const std::string& input,
                                      const std::string& prefix) {
  auto index = input.find(prefix);
  if (index != 0) return std::string{};
  if (prefix.size()+1 >= input.size()) return std::string{};
  if (input[input.size()-1] != '_') return std::string{};
  return input.substr(prefix.size(), input.size()-prefix.size()-1);
}

// Helper function to remove any cv (and restrict) qualifiers from a type.
const Type& Generator::stripModifiers(const Type& type) {
  if (auto modifier = type.asConst()) {
    return stripModifiers(modifier->modified);
  } else if (auto modifier = type.asVolatile()) {
    return stripModifiers(modifier->modified);
  } else if (auto modifier = type.asRestrict()) {
    return stripModifiers(modifier->modified);
  } else {
    return type;
  }
}

// Compare two types, return -1 if t1 < t2, 1 if t1 > t2, and 0 if they are
// equal. Type doesn't have a builtin comparison operator because the ordering
// is rather arbitrary and application dependent.
int Generator::compareTypes(const Type& t1, const Type& t2) {
  // First order types according to what category of type they are, using the
  // below arbitrary ranking.
  const auto rank = [](const Type& t) {
    return t.match<int>(
      [](const ObjectType*) { return 0; },
      [](const PtrType*) { return 1; },
      [](const RefType*) { return 2; },
      [](const RValueRefType*) { return 3; },
      [](const ArrType*) { return 4; },
      [](const ConstType*) { return 5; },
      [](const VolatileType*) { return 6; },
      [](const RestrictType*) { return 7; },
      [](const FuncType*) { return 8; },
      [](const MemberType*) { return 9; },
      [](const VoidType*) { return 10; }
    );
  };
  const auto rank1 = rank(t1);
  const auto rank2 = rank(t2);
  if (rank1 < rank2) return -1;
  else if (rank1 > rank2) return 1;

  // At this point, both t1 and t2 are of the same type category. We can now do
  // member-wise comparison of the types within these types.

  return t1.match<int>(
    [&](const ObjectType* t) {
      const auto* other = t2.asObject();

      if (t->name.name < other->name.name) return -1;
      else if (t->name.name > other->name.name) return 1;

      if (t->incomplete < other->incomplete) return -1;
      else if (t->incomplete > other->incomplete) return 1;

      if (t->name.linkage < other->name.linkage) return -1;
      else if (t->name.linkage > other->name.linkage) return 1;

      // The two object types have the same linkage and same name, so now
      // examine which linkage that actually is to determine how to check for
      // object equality.
      switch (t->name.linkage) {
        case ObjectTypeName::Linkage::external:
        case ObjectTypeName::Linkage::pseudo:
          // Same name and external linkage means they're the same.
          return 0;
        case ObjectTypeName::Linkage::internal:
          // Objects types with internal linkage with the same name are only the
          // same if they're in the same compile unit.
          if (t->key.compile_unit_id < other->key.compile_unit_id) return -1;
          if (t->key.compile_unit_id > other->key.compile_unit_id) return 1;
          return 0;
        case ObjectTypeName::Linkage::none: {
          // Object types with no linkage are only the same if they have
          // identical keys.
          const auto& tie1 = std::tie(
            t->key.compile_unit_id,
            t->key.object_id
          );
          const auto& tie2 = std::tie(
            other->key.compile_unit_id,
            other->key.object_id
          );
          if (tie1 < tie2) return -1;
          if (tie1 > tie2) return 1;
          return 0;
        }
      }

      return 0;
    },
    [&](const PtrType* t) {
      return compareTypes(t->pointee, t2.asPtr()->pointee);
    },
    [&](const RefType* t) {
      return compareTypes(t->referenced, t2.asRef()->referenced);
    },
    [&](const RValueRefType* t) {
      return compareTypes(t->referenced, t2.asRValueRef()->referenced);
    },
    [&](const ArrType* t) {
      const auto* other = t2.asArr();
      const auto cmp = compareTypes(t->element, other->element);
      if (cmp != 0) return cmp;
      if (t->count < other->count) return -1;
      if (t->count > other->count) return 1;
      return 0;
    },
    [&](const ConstType* t) {
      return compareTypes(t->modified, t2.asConst()->modified);
    },
    [&](const VolatileType* t) {
      return compareTypes(t->modified, t2.asVolatile()->modified);
    },
    [&](const RestrictType* t) {
      return compareTypes(t->modified, t2.asRestrict()->modified);
    },
    [&](const FuncType* t) {
      const auto* other = t2.asFunc();
      const auto cmp = compareTypes(t->ret, other->ret);
      if (cmp != 0) return cmp;
      if (t->args.size() < other->args.size()) return -1;
      if (t->args.size() > other->args.size()) return 1;
      for (size_t i = 0; i < t->args.size(); ++i) {
        const auto cmp = compareTypes(t->args[i], other->args[i]);
        if (cmp != 0) return cmp;
      }
      return 0;
    },
    [&](const MemberType* t) {
      const auto* other = t2.asMember();
      const auto cmp = compareTypes(t->obj, other->obj);
      if (cmp != 0) return cmp;
      return compareTypes(t->member, other->member);
    },
    [&](const VoidType*) { return 0; }
  );
}

// Compare two indexed types, return -1 if t1 < t2, 1 if t1 > t2, and 0 if
// equal. If this comparison is for the purpose of trying to merge two indexed
// type together (see below), skip comparing certain fields.
int Generator::compareIndexedTypes(const IndexedType& t1,
                                   const IndexedType& t2,
                                   bool for_merge) {
  const auto cmp = compareTypes(*t1.type, *t2.type);
  if (cmp != 0) return cmp;

  if (t1.ignore < t2.ignore) return -1;
  else if (t1.ignore > t2.ignore) return 1;

  if (t1.conservative < t2.conservative) return -1;
  else if (t1.conservative > t2.conservative) return 1;

  const auto compare_guards = [](const IndexedType& type1,
                                 const IndexedType& type2) {
    return std::lexicographical_compare(
      type1.conservative_guards.begin(),
      type1.conservative_guards.end(),
      type2.conservative_guards.begin(),
      type2.conservative_guards.end(),
      [](const Type* tp1, const Type* tp2) {
        return compareTypes(*tp1, *tp2) < 0;
      }
    );
  };
  if (compare_guards(t1, t2)) return -1;
  else if (compare_guards(t2, t1)) return 1;

  if (!t1.suffix_type) {
    if (t2.suffix_type) return -1;
  } else if (!t2.suffix_type) {
    if (t1.suffix_type) return 1;
  } else {
    const auto cmp = compareTypes(*t1.suffix_type, *t2.suffix_type);
    if (cmp != 0) return cmp;
  }

  // The whole point of merging two indexed types together are to coalesce
  // Indexer<> addresses and to combine a "scan-only" one with a non-scan-only
  // one, so don't compare these fields when checking for equality.
  if (!for_merge) {
    // This ordering is important! We want indexed types with scan set to false
    // to come first. We always merge right into left, so scan-only should be
    // merged into no-scan-only.
    if (t1.scan < t2.scan) return -1;
    else if (t1.scan > t2.scan) return 1;

    if (t1.addresses < t2.addresses) return -1;
    else if (t1.addresses > t2.addresses) return 1;
  }

  return 0;
}

// Helper function to extract types that markers are instantiated on from the
// markers. Given a list of a specific marker object type, apply the given
// callable f to it (which should extract the marked type out of it), sort and
// uniquify the resultant list, and return it.
template <typename T, typename F, typename C>
T Generator::extractFromMarkers(const C& types, F&& f) const {
  std::vector<const Object*> objects;
  std::transform(
    types.begin(),
    types.end(),
    std::back_inserter(objects),
    [&](const ObjectType& t){ return &getObject(t); }
  );
  std::sort(objects.begin(), objects.end());

  T out;
  auto ins = std::inserter(out, out.end());
  std::string msg;
  std::for_each(
    objects.begin(),
    std::unique(objects.begin(), objects.end()),
    [&](const Object* o) {
      try {
        ins = f(*o);
      } catch (Exception& e) {
        msg += fmt::format(" => {}\n", e.what());
      }
    }
  );
  if (!msg.empty()) throw Exception(msg);
  return out;
}

// Given a list of Indexer<> instantiation types, extract the marked types, and
// create the appropriate IndexedType struct for them, merging duplicates
// together.
template <typename C>
std::vector<Generator::IndexedType> Generator::getIndexedTypes(
  const C& indexers
) const {
  auto indexed = extractFromMarkers<std::vector<IndexedType>>(
    indexers,
    [&](const Object& o) { return getIndexedType(o); }
  );
  return mergeDupIndexedTypes(std::move(indexed));
}

// Merge "duplicate" indexed types together using some definition of duplicate,
// returning the merged types.
std::vector<Generator::IndexedType> Generator::mergeDupIndexedTypes(
  std::vector<Generator::IndexedType> indexed
) const {

  // First sort the types. The ordering has been set up so that indexed types
  // that are identical except for the "scan" field will have the indexed types
  // with scan set to false first.
  std::sort(
    indexed.begin(),
    indexed.end(),
    [](const IndexedType& t1, const IndexedType& t2)
    { return compareIndexedTypes(t1, t2) < 0; }
  );

  // Only insert an indexed type into the unique vector if its not equal (for
  // the purposes of merging) as the last indexed type in the vector. If they
  // are equal, merge their addresses and their scan field anded together. Since
  // indexed types with scan set to false come first, this will ensure that
  // false is kept over true.
  std::vector<IndexedType> unique;
  for (const auto& i : indexed) {
    if (unique.empty() || compareIndexedTypes(unique.back(), i, true) != 0) {
      unique.emplace_back(std::move(i));
    } else {
      auto& back = unique.back();
      back.scan &= i.scan;
      back.addresses.insert(
        back.addresses.end(),
        i.addresses.begin(),
        i.addresses.end()
      );
    }
  }

  return unique;
}

/*
 * Certain compilers (usually Clang) will emit debug information for template
 * instantiations, but fail to emit information about the template
 * parameters. This usually happens if it thinks the template isn't "used". This
 * is bad for marker types like Conservative<...> because it will be interpreted
 * as an empty type list (which implies always conservative scan). Unfortunately
 * we cannot always detect this, but we can for certain cases.
 *
 * If we know a-priori that an object-type is a template, and the debug
 * information indicates the object type has no template parameters, do a hacky
 * check on the object type's name. Look for the name to end with "<>",
 * indicating an empty parameter list. If not, the template actually has
 * parameters but is failing to emit them.
 *
 * Unfortunately there's not much that can be done about this, but we want to
 * catch it quickly so that we can try to work around it.
 */
void Generator::sanityCheckTemplateParams(const Object& object) {
  if (!object.template_params.empty()) return;

  const auto index = object.name.name.rfind("<>");
  if (index == std::string::npos ||
      index != object.name.name.size()-2) {
    throw Exception{
      fmt::format(
        "Object type '{}' at ({},{}) is reported as having "
        "no template parameters, but its name indicates "
        "otherwise. This usually indicates the compiler "
        "is not generating complete enough debug information.",
        object.name.name,
        object.key.object_id,
        object.key.compile_unit_id
      )
    };
  }
}

// Given a Mark[Scannable]Collectable<> marker instantiation, extract
// the object-type its marking. Actually very simple, but do a lot of sanity
// checking on the result.
const Object& Generator::getMarkedCollectable(const Object& mark) const {
  if (mark.incomplete) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) is an incomplete type",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  if (mark.kind != Object::Kind::k_class) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) isn't a class type",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  if (!mark.bases.empty()) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) has base classes",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  if (!mark.members.empty()) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) has members",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  if (mark.name.linkage != ObjectTypeName::Linkage::external) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) does not have external linkage",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  if (mark.template_params.size() != 1) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) does not have exactly "
        "one template parameter",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id
      )
    };
  }

  const auto& type = mark.template_params[0].type;

  const auto* obj_type = stripModifiers(type).asObject();
  if (!obj_type) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) is instantiated on type '{}', "
        "which is not an object",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id,
        type.toString()
      )
    };
  }

  if (obj_type->name.linkage != ObjectTypeName::Linkage::external) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) is instantiated on object type '{}'"
        " at ({}, {}), which does not have external linkage",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id,
        obj_type->name.name,
        obj_type->key.object_id,
        obj_type->key.compile_unit_id
      )
    };
  }

  const auto& obj = getObject(*obj_type);
  if (obj.incomplete) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) is instantiated on object type '{}'"
        " at ({}, {}), which is an incomplete type",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id,
        obj_type->name.name,
        obj_type->key.object_id,
        obj_type->key.compile_unit_id
      )
    };
  }
  if (obj.kind != Object::Kind::k_class) {
    throw Exception{
      fmt::format(
        "Collectable marker '{}' at ({},{}) is instantiated on object type '{}'"
        " at ({}, {}), which is not a class type",
        mark.name.name,
        mark.key.object_id,
        mark.key.compile_unit_id,
        obj_type->name.name,
        obj_type->key.object_id,
        obj_type->key.compile_unit_id
      )
    };
  }

  return obj;
}

// Given an Indexer<> marker instantiation, extract the type and action the
// marker refers to, returning the appropriate IndexedType instance. Do a lot of
// sanity checking on the result.
Generator::IndexedType Generator::getIndexedType(const Object& indexer) const {
  if (indexer.incomplete) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) is an incomplete type",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }

  if (indexer.kind != Object::Kind::k_class) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) is not a class type",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }

  if (!indexer.bases.empty()) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) has base classes",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }

  if (indexer.members.size() != 2) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) does not have exactly two members ({})",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id,
        indexer.members.size()
      )
    };
  }

  if (indexer.template_params.size() != 2) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) does not have exactly two "
        "template parameters ({})",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id,
        indexer.template_params.size()
      )
    };
  }

  const auto index_iter = std::find_if(
    indexer.members.begin(),
    indexer.members.end(),
    [](const Object::Member& m) { return m.name == "s_index"; }
  );
  if (index_iter == indexer.members.end()) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) does not a s_index member",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }
  const auto& index_member = *index_iter;

  if (index_member.offset) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) has a non-static s_index member",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }

  // Since we want to put the assigned type index into Indexer<>::s_index, it
  // had better either have a symbol (preferred), or an absolute address.
  if (!index_member.address && index_member.linkage_name.empty()) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) has a s_index member which "
        "has neither a linkage name, nor an address",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id
      )
    };
  }

  // Extract the underlying type from the Indexer<>, doing sanity checking on
  // the type.
  const auto* type = [&] {
    /*
     * The type we want is just the first template parameter of Indexer<>, but
     * we want to do sanity checking on it, which involves walking the type
     * chain.
     *
     * To avoid recursion, use a loop instead. Each invocation of the match
     * method returns the next type to check, returning nullptr if there's no
     * more types to check. This works because each type category only has at
     * most one sub-type to check.
     */
    const auto* current = &indexer.template_params[0].type;
    while (current) {
      current = current->match<const Type*>(
        [&](const ObjectType* t) -> const Type* {
          if (t->name.linkage == ObjectTypeName::Linkage::pseudo) {
            throw Exception{
              fmt::format(
                "Indexer '{}' at ({},{}) is instantiated on "
                "object type '{}' which is the pseudo-type",
                indexer.name.name,
                indexer.key.object_id,
                indexer.key.compile_unit_id,
                current->toString()
              )
            };
          }

          const auto& obj = getObject(*t);
          if (obj.incomplete) {
            throw Exception{
              fmt::format(
                "Indexer '{}' at ({},{}) is instantiated on "
                "object type '{}' which is an incomplete type",
                indexer.name.name,
                indexer.key.object_id,
                indexer.key.compile_unit_id,
                current->toString()
              )
            };
          }

          if (obj.kind == Object::Kind::k_other) {
            throw Exception{
              fmt::format(
                "Indexer '{}' at ({},{}) is instantiated on "
                "object type '{}' which has an 'other' kind",
                indexer.name.name,
                indexer.key.object_id,
                indexer.key.compile_unit_id,
                current->toString()
              )
            };
          }

          return nullptr;
        },
        [&](const PtrType*) -> const Type* { return nullptr; },
        [&](const RefType*) -> const Type* { return nullptr; },
        [&](const RValueRefType*) -> const Type* { return nullptr; },
        [&](const ArrType* t) {
          if (!t->count) {
            throw Exception{
              fmt::format(
                "Indexer '{}' at ({},{}) is instantiated on "
                "unbounded array type '{}'",
                indexer.name.name,
                indexer.key.object_id,
                indexer.key.compile_unit_id,
                current->toString()
              )
            };
          }
          return &t->element;
        },
        [&](const ConstType* t) { return &t->modified; },
        [&](const VolatileType* t) { return &t->modified; },
        [&](const RestrictType* t) { return &t->modified; },
        [&](const FuncType*) -> const Type* {
          throw Exception{
            fmt::format(
              "Indexer '{}' at ({},{}) is instantiated on function type '{}'",
              indexer.name.name,
              indexer.key.object_id,
              indexer.key.compile_unit_id,
              current->toString()
            )
          };
          return nullptr;
        },
        [&](const MemberType*) -> const Type* {
          throw Exception{
            fmt::format(
              "Indexer '{}' at ({},{}) is instantiated on member type '{}'",
              indexer.name.name,
              indexer.key.object_id,
              indexer.key.compile_unit_id,
              current->toString()
            )
          };
          return nullptr;
        },
        [&](const VoidType*) -> const Type* {
          throw Exception{
            fmt::format(
              "Indexer '{}' at ({},{}) is instantiated on void type",
              indexer.name.name,
              indexer.key.object_id,
              indexer.key.compile_unit_id
            )
          };
          return nullptr;
        }
      );
    }

    return &stripModifiers(indexer.template_params[0].type);
  }();

  IndexedType indexed_type{
    *type,
    determineSize(*type),
    index_member.linkage_name.empty() ?
      Address{*index_member.address} :
      Address{index_member.linkage_name}
  };

  // Now examine the action component:

  const auto* action_type =
    stripModifiers(indexer.template_params[1].type).asObject();
  if (!action_type) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) action type '{}' isn't an object type",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id,
        indexer.template_params[1].type.toString()
      )
    };
  }
  const auto& action = getObject(*action_type);

  if (action.incomplete) {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) action type '{}' at ({},{}) is incomplete",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id,
        action.name.name,
        action.key.object_id,
        action.key.compile_unit_id
      )
    };
  }

  // Use the name of the action to determine what it is:
  if (action.name.name == s_auto_action_name) {
    // Nothing
  } else if (action.name.name == s_scan_action_name) {
    indexed_type.scan = true;
  } else if (action.name.name == s_ignore_action_name) {
    indexed_type.ignore = true;
  } else if (isConservativeActionName(action.name.name)) {
    indexed_type.conservative = true;

    // Conservative<> is a variadic template, so we'd better sanity check the
    // template parameters.
    sanityCheckTemplateParams(action);

    if (!action.template_params.empty()) {
      // If it has template parameters, the conservative scan is
      // conditional. Indicate this fact by setting ignore to true as well. Once
      // the guards have been evaluated, ignore will either be cleared (if any
      // guards pass), or conservative will be cleared (if none pass).
      indexed_type.ignore = true;
      for (const auto& param : action.template_params) {
        indexed_type.conservative_guards.emplace_back(
          &stripModifiers(param.type)
        );
      }
    }
  } else if (isWithSuffixActionName(action.name.name)) {
    if (action.template_params.size() != 1) {
      throw Exception{
        fmt::format(
          "Indexer '{}' at ({},{}) action type '{}' at ({},{}) does not "
          "have exactly one template parameter",
          indexer.name.name,
          indexer.key.object_id,
          indexer.key.compile_unit_id,
          action.name.name,
          action.key.object_id,
          action.key.compile_unit_id
        )
      };
    }
    indexed_type.suffix_type = &stripModifiers(action.template_params[0].type);
  } else {
    throw Exception{
      fmt::format(
        "Indexer '{}' at ({},{}) action type '{}' at ({},{}) is unknown",
        indexer.name.name,
        indexer.key.object_id,
        indexer.key.compile_unit_id,
        action.name.name,
        action.key.object_id,
        action.key.compile_unit_id
      )
    };
  }

  return indexed_type;
}

// Retrieve the action associated with the given object type, computing a new
// one if it isn't already present.
const Generator::Action& Generator::getAction(const Object& object,
                                              bool conservative) const {
  auto iter = m_actions.find(&object);
  if (iter != m_actions.end()) return iter->second;
  return m_actions.emplace(
    &object,
    inferAction(object, conservative)
  ).first->second;
}

bool Generator::findMemberHelper(const std::string& field,
                                 const Object &a_object) const {
  return std::any_of(
    a_object.members.begin(),
    a_object.members.end(),
    [&](const Object::Member& m) {
      if (m.type.isObject()) {
        const Object &inner_object = getObject(*m.type.asObject());
        if (inner_object.kind == Object::Kind::k_union) {
          return findMemberHelper(field, inner_object);
        }
      }
      return m.offset && m.name == field;
    }
  );
}

// Given an object type, examine it to infer all the needed actions for that
// type. The actions are inferred by looking for member functions with special
// names, and static members with special names.
Generator::Action Generator::inferAction(const Object& object,
                                         bool conservative_everything) const {
  if (object.incomplete) {
    throw Exception{
      fmt::format(
        "Trying to infer actions on object type '{}' at ({},{}) "
        "which is incomplete",
        object.name.name,
        object.key.object_id,
        object.key.compile_unit_id
      )
    };
  }

  Action action;

  if (conservative_everything) {
    action.conservative_all = true;
    action.conservative_all_bases = true;
    return action;
  }

  // White-listing and forbidden templates are determined by just checking the
  // name against explicit lists.
  if (HPHP::type_scan::detail::isIgnoredType(object.name.name)) {
    action.whitelisted = true;
    return action;
  }

  if (HPHP::type_scan::detail::isForbiddenTemplate(object.name.name)) {
    sanityCheckTemplateParams(object);
    action.forbidden_template = true;
    return action;
  }

  if (HPHP::type_scan::detail::isForcedConservativeTemplate(object.name.name)) {
    sanityCheckTemplateParams(object);
    action.conservative_all = true;
    action.conservative_all_bases = true;
    return action;
  }

  const auto find_member = [&](const std::string& field) {
    return findMemberHelper(field, object);
  };

  const auto find_base = [&](const Object& base) {
    return std::any_of(
      object.bases.begin(),
      object.bases.end(),
      [&](const Object::Base& b) { return &getObject(b.type) == &base; }
    );
  };

  for (const auto& fun : object.functions) {
    // Sanity check special member function. All the functions should take a
    // const pointer to the contained object type as the first parameter (the
    // this pointer), and a non-const reference to HPHP::type_scan::Scanner as
    // the second (and nothing else). The return type should be void.
    auto verify_func = [&](const Object::Function& func) {
      if (func.kind != Object::Function::Kind::k_member) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "which is not a non-static, non-virtual member",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name
          )
        };
      }

      if (!func.ret_type.isVoid()) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "which does not have a void return type",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name
          )
        };
      }

      if (func.arg_types.size() != 2) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "which does not take exactly two parameter ({})",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            func.arg_types.size()
          )
        };
      }

      const auto& this_arg = func.arg_types[0];
      const auto* this_ptr_arg = this_arg.asPtr();
      if (!this_ptr_arg) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose first parameter isn't a pointer type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            this_arg.toString()
          )
        };
      }

      const auto* this_const_arg = this_ptr_arg->pointee.asConst();
      if (!this_const_arg) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose first parameter isn't a const pointer type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            this_arg.toString()
          )
        };
      }

      const auto* this_obj_arg = this_const_arg->modified.asObject();
      if (!this_obj_arg) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose first parameter isn't a pointer type to object type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            this_arg.toString()
          )
        };
      }

      if (&getObject(*this_obj_arg) != &object) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose first parameter isn't a valid this pointer '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            this_arg.toString()
          )
        };
      }

      const auto& scanner_arg = func.arg_types[1];
      const auto* scanner_ref_arg = scanner_arg.asRef();
      if (!scanner_ref_arg) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose second parameter isn't a reference '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            scanner_arg.toString()
          )
        };
      }

      const auto* scanner_obj_arg = scanner_ref_arg->referenced.asObject();
      if (!scanner_obj_arg) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose second parameter isn't a reference to object-type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            scanner_arg.toString()
          )
        };
      }

      const auto& scanner_obj = getObject(*scanner_obj_arg);
      if (scanner_obj.name.name != s_scanner_name) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains scanner func '{}' "
            "whose second parameter isn't a reference to "
            "{} '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            func.name,
            std::string{s_scanner_name},
            scanner_arg.toString()
          )
        };
      }
    };

    // Custom scanner for particular field.
    auto custom_field = splitFieldName(
      fun.name,
      HPHP::type_scan::detail::kCustomFieldName
    );
    if (!custom_field.empty()) {
      verify_func(fun);

      if (!find_member(custom_field)) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains custom field marker "
            "referring to unknown non-static field '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            custom_field
          )
        };
      }

      action.custom_fields.emplace(
        std::move(custom_field),
        fun.linkage_name
      );
    }

    // Custom scanner for entire object.
    if (fun.name == HPHP::type_scan::detail::kCustomName) {
      verify_func(fun);
      action.custom_all = fun.linkage_name;
      continue;
    }

    // Custom scanner for base classes.
    if (fun.name == HPHP::type_scan::detail::kCustomBasesScannerName) {
      verify_func(fun);
      action.custom_bases_scanner = fun.linkage_name;
      continue;
    }
  }

  for (const auto& member : object.members) {
    // All special member markers should be static, so ignore anything that's
    // not.
    if (member.offset) continue;

    // Ignore a field.
    auto ignore_field = splitFieldName(
      member.name,
      HPHP::type_scan::detail::kIgnoreFieldName
    );
    if (!ignore_field.empty()) {
      if (!find_member(ignore_field)) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains ignore field marker "
            "referring to unknown non-static field '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            ignore_field
          )
        };
      }

      action.ignore_fields.emplace(std::move(ignore_field));
      continue;
    }

    // Scan field conservatively.
    auto conservative_field = splitFieldName(
      member.name,
      HPHP::type_scan::detail::kConservativeFieldName
    );
    if (!conservative_field.empty()) {
      if (!find_member(conservative_field)) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains conservative field marker "
            "referring to unknown non-static field '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            conservative_field
          )
        };
      }

      action.conservative_fields.emplace(std::move(conservative_field));
      continue;
    }

    // Marks flexible array field. There can only be one of these per object
    // type.
    auto flexible_array_field = splitFieldName(
      member.name,
      HPHP::type_scan::detail::kFlexibleArrayFieldName
    );
    if (!flexible_array_field.empty()) {
      if (!action.flexible_array_field.empty()) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains more than one flexible "
            "array field marker",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id
          )
        };
      }

      if (!find_member(flexible_array_field)) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains flexible array field marker "
            "referring to unknown non-static field '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            flexible_array_field
          )
        };
      }

      action.flexible_array_field = std::move(flexible_array_field);
    }

    // Ignore entire object.
    if (member.name == HPHP::type_scan::detail::kIgnoreName) {
      action.ignore_all = true;
      continue;
    }

    // Conservative scan entire object.
    if (member.name == HPHP::type_scan::detail::kConservativeName) {
      action.conservative_all = true;
      continue;
    }

    // Ignore specific base.
    if (member.name == HPHP::type_scan::detail::kIgnoreBaseName) {
      const auto* ignore_type = stripModifiers(member.type).asObject();
      if (!ignore_type) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains an ignore base marker "
            "for a non-object type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            member.type.toString()
          )
        };
      }

      const auto& ignore = getObject(*ignore_type);
      // This is a variadic template, so sanity check it.
      sanityCheckTemplateParams(ignore);
      for (const auto& param : ignore.template_params) {
        const auto* ignored_type = stripModifiers(param.type).asObject();
        if (!ignored_type) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains an ignore base marker "
              "instantiated on non-object type '{}'",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              param.type.toString()
            )
          };
        }

        const auto& ignored = getObject(*ignored_type);
        if (!find_base(ignored)) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains an ignore base marker "
              "instantiated on object-type '{}' which isn't a base class",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              ignored.name.name
            )
          };
        }
        action.ignored_bases.emplace(&ignored);
      }
      continue;
    }

    // Don't complain about a particular base class violating a forbidden
    // template check.
    if (member.name == HPHP::type_scan::detail::kSilenceForbiddenBaseName) {
      const auto* silence_type = stripModifiers(member.type).asObject();
      if (!silence_type) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains a silence base marker "
            "for a non-object type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            member.type.toString()
          )
        };
      }

      const auto& silence = getObject(*silence_type);
      // This is a variadic template, so sanity check it.
      sanityCheckTemplateParams(silence);
      for (const auto& param : silence.template_params) {
        const auto* silenced_type = stripModifiers(param.type).asObject();
        if (!silenced_type) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains a silence base marker "
              "instantiated on non-object type '{}'",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              param.type.toString()
            )
          };
        }

        const auto& silenced = getObject(*silenced_type);
        if (!find_base(silenced)) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains a silence base marker "
              "instantiated on object-type '{}' which isn't a base class",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              silenced.name.name
            )
          };
        }
        action.silenced_bases.emplace(&silenced);
      }
      continue;
    }

    // List of base classes to apply the custom bases scan to.
    if (action.custom_bases_scanner &&
        member.name == HPHP::type_scan::detail::kCustomBasesName) {
      const auto* custom_list_type = stripModifiers(member.type).asObject();
      if (!custom_list_type) {
        throw Exception{
          fmt::format(
            "Object type '{}' at ({},{}) contains a custom base marker "
            "for a non-object type '{}'",
            object.name.name,
            object.key.object_id,
            object.key.compile_unit_id,
            member.type.toString()
          )
        };
      }

      const auto& custom_list = getObject(*custom_list_type);
      // This is a variadic template, so sanity check it.
      sanityCheckTemplateParams(custom_list);
      for (const auto& param : custom_list.template_params) {
        const auto* custom_type = stripModifiers(param.type).asObject();
        if (!custom_type) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains a custom base marker "
              "instantiated on non-object type '{}'",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              param.type.toString()
            )
          };
        }

        const auto& custom = getObject(*custom_type);
        if (!find_base(custom)) {
          throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains a custom base marker "
              "instantiated on object-type '{}' which isn't a base class",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              custom.name.name
            )
          };
        }
        action.custom_bases.emplace(&custom);
      }
      continue;
    }

    // If there's a custom scanner for the entire object, list of types to guard
    // on.
    if (action.custom_all &&
        member.name == HPHP::type_scan::detail::kCustomGuardName) {
      const auto* guard_type = stripModifiers(member.type).asObject();
      if (!guard_type) {
        throw Exception{
            fmt::format(
              "Object type '{}' at ({},{}) contains a custom guard marker "
              "instantiated on non-object type '{}'",
              object.name.name,
              object.key.object_id,
              object.key.compile_unit_id,
              member.type.toString()
            )
        };
      }

      const auto& guard = getObject(*guard_type);
      // This is a variadic template, so sanity check it.
      sanityCheckTemplateParams(guard);
      for (const auto& param : guard.template_params) {
        action.custom_guards.emplace(&param.type);
      }
      continue;
    }
  }

  return action;
}

// Given an object type, return the matching Object representation. Even for
// object types with different keys, if the underlying object type is the "same"
// (according to the linkage rules), the same Object representation will be
// returned. This means that one can then use pointer equality to check for
// equality between two object types.
const Object& Generator::getObject(const ObjectType& type) const {
  // First attempt to lookup the Object in our internal maps. Use the
  // appropriate map according to the object type's linkage. This ensures that
  // object types that represent the same underlying object always returns the
  // same Object&.
  switch (type.name.linkage) {
    case ObjectTypeName::Linkage::external: {
      auto iter = m_external_objects.find(type.name.name);
      if (iter != m_external_objects.end()) return iter->second;
      break;
    }
    case ObjectTypeName::Linkage::internal: {
      auto cu_iter = m_internal_objects.find(type.key.compile_unit_id);
      if (cu_iter != m_internal_objects.end()) {
        auto iter = cu_iter->second.find(type.name.name);
        if (iter != cu_iter->second.end()) return iter->second;
      }
      break;
    }
    case ObjectTypeName::Linkage::none:
    case ObjectTypeName::Linkage::pseudo: {
      auto iter = m_unique_objects.find(type.key.object_id);
      if (iter != m_unique_objects.end()) return iter->second;
      break;
    }
  }

  const auto insert = [&](Object object) -> const Object& {
    switch (object.name.linkage) {
      case ObjectTypeName::Linkage::external: {
        return m_external_objects.emplace(
          type.name.name,
          std::move(object)
        ).first->second;
      }
      case ObjectTypeName::Linkage::internal: {
        return m_internal_objects[type.key.compile_unit_id].emplace(
          type.name.name,
          std::move(object)
        ).first->second;
      }
      case ObjectTypeName::Linkage::none:
      case ObjectTypeName::Linkage::pseudo: {
        return m_unique_objects.emplace(
          type.key.object_id,
          std::move(object)
        ).first->second;
      }
    }
    not_reached();
  };

  // Check if the object is a valid indexer. LLVM for clang 15/16 will sometimes
  // produce a DW_TAG_structure_type for the same Indexer<...> type in a
  // compile_unit without a corresponding DW_TAG_variable. This check will skip
  // such cases and end up pick a different type.
  const auto isValid = [&](const Object& obj) -> bool {
    if (isIndexerName(type.name.name)) {
      const auto index_iter = std::find_if(
        obj.members.begin(),
        obj.members.end(),
        [](const Object::Member& m) { return m.name == "s_index"; }
      );

      if (index_iter == obj.members.end()) {
        return false;
      }

      const auto& index_member = *index_iter;
      if (!index_member.address && index_member.linkage_name.empty()) {
        return false;
      }
    }

    return true;
  };

  // No direct matches in our internal maps, so we need to retrieve it from the
  // type parser. If the type is complete we can just retrieve it and use it
  // directly. If this type has no linkage or pseudo-linkage, it matches nothing
  // else, so just retrieve it. Store it in our maps for later lookup.
  if (isValid(m_parser->getObject(type.key)) && (!type.incomplete ||
      type.name.linkage == ObjectTypeName::Linkage::none ||
      type.name.linkage == ObjectTypeName::Linkage::pseudo)) {
    return insert(m_parser->getObject(type.key));
  }

  // The object type is incomplete, but has internal or external linkage. We
  // only want to return an incomplete object as a last resort, so let's look
  // for any possible definitions of this type elsewhere.

  auto const name_iter = m_objects_by_name.find(type.name);
  if (name_iter != m_objects_by_name.end()) {
    auto const& keys = name_iter->second;
    // First look for a type with the same name in the same compilation unit. If
    // there's one that's a complete definition, use that.
    for (auto const& key : keys) {
      if (key.object_id == type.key.object_id) continue;
      if (key.compile_unit_id != type.key.compile_unit_id) continue;
      auto other = m_parser->getObject(key);
      if (!isValid(other)) continue;
      if (other.incomplete) continue;
      return insert(std::move(other));
    }
    // Otherwise if the type has external linkage, look for any type in any
    // compilation unit (with external linkage) with the same name and having a
    // complete definition.
    if (type.name.linkage == ObjectTypeName::Linkage::internal) {
      // Newer clang seems to split some types into different units,
      // or at least we are not able to tell that they are the same.
      std::cout << "gen-type-scanners: warning: "
        "No matching type found for internal linkage type " <<
        type.name.name << " in same compile unit.  "
        "Trying other compile units." << std::endl;
    }
    for (auto const& key : keys) {
      if (key.object_id == type.key.object_id) continue;
      auto other = m_parser->getObject(key);
      if (!isValid(other)) continue;
      if (other.incomplete) continue;
      return insert(std::move(other));
    }
  }

  // There doesn't appear to be a complete definition of this type anywhere, so
  // just return the incomplete object representation. This will probably error
  // elsewhere, but there's nothing we can do.
  return insert(m_parser->getObject(type.key));
}

// Given a type, fill the given layout (starting at the specified offset)
// appropriate for that type. LayoutError will be thrown if an ambiguous
// construct is encountered.
void Generator::genLayout(const Type& type,
                          Layout& layout,
                          size_t offset,
                          bool conservative_everything) const {
  return type.match<void>(
    [&](const ObjectType* t) {
      genLayout(getObject(*t), layout, offset, true, conservative_everything);
    },
    [&](const PtrType* t) {
      // Don't care about pointers to non-pointer followable types.
      if (!isPtrFollowable(type)) return;
      if (t->pointee.isVoid()) {
        throw LayoutError{
          "Generic pointer to void. Add annotation to disambiguate."
        };
      }
      layout.addPtr(offset);
    },
    [&](const RefType* t) {
      // Don't care about pointers to non-pointer followable types.
      if (!isPtrFollowable(type)) return;
      if (t->referenced.isVoid()) {
        throw LayoutError{
          "Generic pointer to void. Add annotation to disambiguate."
        };
      }
      layout.addPtr(offset);
    },
    [&](const RValueRefType* t) {
      // Don't care about pointers to non-pointer followable types.
      if (!isPtrFollowable(type)) return;
      if (t->referenced.isVoid()) {
        throw LayoutError{
          "Generic pointer to void. Add annotation to disambiguate."
        };
      }
      layout.addPtr(offset);
    },
    [&](const ArrType* t) {
      if (!t->count) {
        throw LayoutError{
          "Array of indeterminate size. Add annotation to disambiguate."
        };
      }
      Layout sublayout{determineSize(t->element)};
      genLayout(t->element, sublayout, 0, conservative_everything);
      layout.addSubLayout(
        offset,
        *t->count,
        sublayout
      );
    },
    [&](const ConstType* t) {
      genLayout(t->modified, layout, offset, conservative_everything);
    },
    [&](const VolatileType* t) {
      genLayout(t->modified, layout, offset, conservative_everything);
    },
    [&](const RestrictType* t) {
      genLayout(t->modified, layout, offset, conservative_everything);
    },
    [&](const FuncType*) {},
    [&](const MemberType*) {},
    [&](const VoidType*) {}
  );
}

// Check if the given object member is associated with a special action,
// recording it into the given Layout as needed. Unnamed unions are recursed
// into with their members being treated as members of the enclosing
// object. Returns true if the layout was modified, false otherwise.
bool Generator::checkMemberSpecialAction(const Object& base_object,
                                         const Object::Member& member,
                                         const Action& action,
                                         Layout& layout,
                                         size_t base_obj_offset,
                                         size_t offset) const {
  if (member.type.isObject()) {
    auto const& object = getObject(*member.type.asObject());
    // Treat members of an unnamed union as members
    // of the enclosing struct.
    if (object.kind == Object::Kind::k_union) {
      for (auto const& obj_member : object.members) {
        if (!obj_member.offset) continue;
        // Recurse: the unions themselves might contain unnamed unions.
        if (checkMemberSpecialAction(base_object, obj_member, action,
                                     layout, base_obj_offset,
                                     offset + *obj_member.offset)) {
          return true;
        }
      }
    }
  }

  // The sole purpose of marking the flexible array member is so we know
  // where the suffix begins. The suffix usually begins at the end of the
  // object, but sometimes within it.
  if (member.type.isArr() && action.flexible_array_field == member.name) {
    layout.suffix_begin = offset;
    return true;
  }

  if (action.ignore_fields.contains(member.name)) return true;

  if (action.conservative_fields.contains(member.name)) {
    layout.addConservative(offset, determineSize(member.type));
    return true;
  }

  auto custom_iter = action.custom_fields.find(member.name);
  if (custom_iter != action.custom_fields.end()) {
    if (custom_iter->second.empty()) {
      throw LayoutError{
        fmt::format(
          "'{}' needs to have external linkage (not in unnamed namespace)"
          " to use custom field scanner. If a template, template parameters"
          " must have external linkage as well.",
          base_object.name.name
        )
      };
    }
    layout.addCustom(base_obj_offset, custom_iter->second);
    return true;
  }

  return false;
}

// Given an object type representation, fill the given Layout (starting at
// specified offset) with the appropriate layout for that object
// type. LayoutError will be thrown if an ambiguous construct is encountered.
void Generator::genLayout(const Object& object,
                          Layout& layout,
                          size_t offset,
                          bool do_forbidden_check,
                          bool conservative_everything) const {
  // Never generate layout for collectable types, unless it was marked as
  // scannable.
  if (m_collectable.contains(&object) &&
      !m_scannable_collectable.count(&object)) {
    return;
  }

  if (!m_layout_being_generated.emplace(&object).second) {
    throw LayoutError{
      fmt::format(
        "'{}' is contained within a recursive definition. "
        "This can only happen with invalid debug information "
        "or a type-scanner generator bug.",
        object.name.name
      )
    };
  }
  SCOPE_EXIT { m_layout_being_generated.erase(&object); };

  const auto& action = getAction(object, conservative_everything);

  // A whitelisted type should be ignored entirely.
  if (action.whitelisted) return;

  // If this is a forbidden template (and forbidden template checking has been
  // enabled), check if any of the template's type parameters are
  // interesting. If so, this is an error, as the user shouldn't be using such
  // types in this template.
  if (do_forbidden_check &&
      action.forbidden_template &&
      forbiddenTemplateCheck(object)) {
    throw LayoutError{
      fmt::format(
        "'{}' shouldn't be used with potentially req-heap "
        "allocated objects. Use req:: equivalents instead or add "
        "annotations.",
        object.name.name
      )
    };
  }

  // Process the base classes first to maintain rough offset ordering.
  for (const auto& base : object.bases) {
    try {
      const auto& obj = getObject(base.type);
      if (action.ignored_bases.contains(&obj)) continue;

      // Any base which has been included with the custom base scanner should be
      // ignored here, as we'll do one call to the custom scanner.
      if (action.custom_bases_scanner && action.custom_bases.count(&obj)) {
        continue;
      }

      // Virtual inheritance. The generator doesn't know how to get the base
      // class from the derived (though in theory this could be inferred from
      // the debug information), so punt and make the user specify a custom base
      // scanner.
      if (!base.offset) {
        throw LayoutError{
          "Base is inherited virtually. "
          "Add annotations to convert manually."
        };
      }

      // Recursively generate layout for the base.
      genLayout(
        obj,
        layout,
        offset + *base.offset,
        !action.silenced_bases.contains(&obj),
        action.conservative_all_bases
      );
    } catch (LayoutError& exn) {
      exn.addContext(
        fmt::format(
          "from base class '{}'",
          base.type.name.name
        )
      );
      throw;
    }
  }

  // Do the single call to the custom bases scanner if there is one.
  if (action.custom_bases_scanner && !action.custom_bases.empty()) {
    if (action.custom_bases_scanner->empty()) {
      throw LayoutError{
        fmt::format(
          "'{}' needs to have external linkage (not in unnamed namespace)"
          " to use custom base scanner. If a template, template parameters"
          " must have external linkage as well.",
          object.name.name
        )
      };
    }
    layout.addCustom(offset, *action.custom_bases_scanner);
  }

  if (action.ignore_all) return;

  if (action.custom_all) {
    // We'll use the custom scanner if there's no guards, or if at least one of
    // the guards is interesting.
    if (action.custom_guards.empty() ||
        std::any_of(
          action.custom_guards.begin(),
          action.custom_guards.end(),
          [this](const Type* guard) { return !hasEmptyLayout(*guard); }
        )
       ) {

      // Ooops, the custom scanner function doesn't have a symbol, which
      // probably means it doesn't have external linkage. We can't reliably call
      // such things, so error out.
      if (action.custom_all->empty()) {
        throw LayoutError{
          fmt::format(
            "'{}' needs to have external linkage (not in unnamed namespace)"
            " to use custom scanner. If a template, template parameters must"
            " have external linkage as well.",
            object.name.name
          )
        };
      }
      layout.addCustom(offset, *action.custom_all);
    }
    return;
  }

  if (action.conservative_all) {
    // Determine the begin and end offsets of this type and set up a
    // conservative scan for that range. We can't simply use (0, object size)
    // because we do not want to include base classes, nor padding which we know
    // can't contain any fields.
    size_t begin = std::numeric_limits<size_t>::max();
    size_t end = std::numeric_limits<size_t>::min();
    for (const auto& member : object.members) {
      if (!member.offset) continue;
      begin = std::min(begin, *member.offset);
      end = std::max(end, *member.offset + determineSize(member.type));
    }
    if (begin < end) {
      layout.addConservative(begin + offset, end - begin);
    }
    return;
  }

  // Unions are special. If all the members of the union have the same layout,
  // we can just use that. If not, its a LayoutError, as a custom scanner is
  // needed to disambiguate it.
  if (object.kind == Object::Kind::k_union) {
    Layout first_layout{object.size};
    bool first = true;
    for (const auto& member : object.members) {
      if (!member.offset) continue;

      if (first) {
        genLayout(member.type, first_layout, 0, conservative_everything);
        first = false;
      } else {
        Layout other_layout{object.size};
        genLayout(member.type, other_layout, 0, conservative_everything);
        if (first_layout != other_layout) {
          throw LayoutError{
            fmt::format(
              "'{}' is a union containing potentially req-heap allocated "
              "objects with different layouts. Add annotation to disambiguate "
              "contents. (Conflicting members: '{}' and '{}')",
              object.name.name,
              object.members.front().name,
              member.name
            )
          };
        }
      }
    }

    layout.merge(first_layout, offset);
    return;
  }

  for (const auto& member : object.members) {
    // Only non-static members.
    if (!member.offset) continue;

    // Check if this member has a special action. If it does, we're done
    // processing it.
    if (checkMemberSpecialAction(object, member, action,
                                 layout, offset,
                                 offset + *member.offset)) {
      continue;
    }

    // Otherwise generate its layout recursively.
    try {
      genLayout(
        member.type,
        layout,
        offset + *member.offset,
        conservative_everything
      );
    } catch (LayoutError& exn) {
      exn.addContext(
        fmt::format(
          "from member '{}' of type '{}'",
          member.name,
          member.type.toString()
        )
      );
      throw;
    }
  }
}

// Given a type, determine if it is pointer followable. Only object types (which
// are recorded as being pointer followable) pass, as do void pointers (though
// we cannot generate layout for them).
bool Generator::isPtrFollowable(const Type& type) const {
  return type.match<bool>(
    [&](const ObjectType* t) { return isPtrFollowable(getObject(*t)); },
    [&](const PtrType* t) {
      if (t->pointee.isVoid()) return true;
      return isPtrFollowable(t->pointee);
    },
    [&](const RefType* t) {
      if (t->referenced.isVoid()) return true;
      return isPtrFollowable(t->referenced);
    },
    [&](const RValueRefType* t) {
      if (t->referenced.isVoid()) return true;
      return isPtrFollowable(t->referenced);
    },
    [&](const ArrType* t) { return isPtrFollowable(t->element); },
    [&](const ConstType* t) { return isPtrFollowable(t->modified); },
    [&](const VolatileType* t) { return isPtrFollowable(t->modified); },
    [&](const RestrictType* t) { return isPtrFollowable(t->modified); },
    [&](const FuncType*) { return false; },
    [&](const MemberType*) { return false; },
    [&](const VoidType*) { return false; }
  );
}

// Make a given type pointer followable. This just walks the type hierarchy,
// marking the contained object type (if any) as being pointer followable.
void Generator::makePtrFollowable(const Type& type) {
  return type.match<void>(
    [&](const ObjectType* t) { makePtrFollowable(getObject(*t)); },
    [&](const PtrType*) {},
    [&](const RefType*) {},
    [&](const RValueRefType*) {},
    [&](const ArrType* t) { makePtrFollowable(t->element); },
    [&](const ConstType* t) { makePtrFollowable(t->modified); },
    [&](const VolatileType* t) { makePtrFollowable(t->modified); },
    [&](const RestrictType* t) { makePtrFollowable(t->modified); },
    [&](const FuncType*) {},
    [&](const MemberType*) {},
    [&](const VoidType*) {}
  );
}

// Mark a given object type pointer followable. If an object type is marked
// pointer followable, all its bases must be as well (because a pointer to a
// base could be pointing towards this object type).
void Generator::makePtrFollowable(const Object& obj) {
  m_ptr_followable.emplace(&obj);
  for (const auto& base : obj.bases) {
    makePtrFollowable(getObject(base.type));
  }
}

// Recursive function to check if a given object has a collectable base
// somewhere in its type hierarchy.
bool Generator::hasCollectableBase(const Object& object) const {
  if (m_collectable.contains(&object)) return true;
  return std::any_of(
    object.bases.begin(),
    object.bases.end(),
    [this](const Object::Base& b) {
      return hasCollectableBase(getObject(b.type));
    }
  );
}

// Given a type, check if this is an object type with any template parameters
// being a pointer followable type.
bool Generator::forbiddenTemplateCheck(const Type& type) const {
  return type.match<bool>(
    [&](const ObjectType* t) { return forbiddenTemplateCheck(getObject(*t)); },
    [&](const PtrType* t) { return forbiddenTemplateCheck(t->pointee); },
    [&](const RefType* t) { return forbiddenTemplateCheck(t->referenced); },
    [&](const RValueRefType* t) {
      return forbiddenTemplateCheck(t->referenced);
    },
    [&](const ArrType* t) { return forbiddenTemplateCheck(t->element); },
    [&](const ConstType* t) { return forbiddenTemplateCheck(t->modified); },
    [&](const VolatileType* t) {
      return forbiddenTemplateCheck(t->modified);
    },
    [&](const RestrictType* t) {
      return forbiddenTemplateCheck(t->modified);
    },
    [&](const FuncType*) { return false; },
    [&](const MemberType*) { return false; },
    [&](const VoidType*) { return false; }
  );
}

// Given an object type, check if any template parameters are a pointer
// followable type.
bool Generator::forbiddenTemplateCheck(const Object& object) const {
  if (isPtrFollowable(object)) return true;
  for (const auto& param : object.template_params) {
    if (forbiddenTemplateCheck(param.type)) return true;
  }
  return false;
}

// Given a type, determine if it is non-interesting, IE, its generated layout is
// empty.
bool Generator::hasEmptyLayout(const Type& type) const {
  try {
    Layout layout{determineSize(type)};
    genLayout(type, layout, 0);
    return layout.isEmpty();
  } catch (const LayoutError&) {
    return false;
  }
}

// Given an object type, determine if it is non-interesting, IE, its generated
// layout is empty.
bool Generator::hasEmptyLayout(const Object& object) const {
  try {
    Layout layout{object.size};
    genLayout(object, layout, 0);
    return layout.isEmpty();
  } catch (const LayoutError&) {
    return false;
  }
}

// Given a type, determine how many bytes an instance of that type occupies.
size_t Generator::determineSize(const Type& type) const {
  return type.match<size_t>(
    [&](const ObjectType* t) { return getObject(*t).size; },
    // This is valid because we run on architectures where all pointers are the
    // same size, and we always generate the heap scanners at the same time
    // we're building everything else.
    [&](const PtrType*) { return sizeof(void*); },
    [&](const RefType*) { return sizeof(void*); },
    [&](const RValueRefType*) { return sizeof(void*); },
    [&](const ArrType* t) {
      if (!t->count) return size_t{0};
      return determineSize(t->element) * *t->count;
    },
    [&](const ConstType* t) { return determineSize(t->modified); },
    [&](const VolatileType* t) { return determineSize(t->modified); },
    [&](const RestrictType* t) { return determineSize(t->modified); },
    // These are somewhat dubious, and shouldn't really occur:
    [&](const FuncType*) { return size_t{0}; },
    [&](const MemberType*) { return size_t{0}; },
    [&](const VoidType*) { return size_t{0}; }
  );
}

/*
 * Generate layouts for all indexed types. This is an iterative method since the
 * layout depends on which types are pointer followable, and what types are
 * pointer followable depends on which types are interesting, and what types are
 * interesting depends on their layout.
 *
 * For every indexed type, we generate its layout. If the type was previously
 * uninteresting, but the new layout makes it interesting, mark it as pointer
 * followable. Continue this process until we take a full pass through the
 * indexed type list without computing a different layout for all types.
 */
void Generator::genAllLayouts() {
  bool changed;
  do {
    changed = false;
    for (auto& indexed : m_indexed_types) {
      try {
        // No point in continuing with this one if we already have a
        // LayoutError.
        if (indexed.errors) continue;

        // If this indexed type's action is conservative, examine guards (if
        // any) to see if we want to ignore or conservative scan it.
        if (indexed.conservative) {
          // If ignore isn't set, the issue has already been decided
          // (conservative scan).
          if (!indexed.ignore) continue;
          // Otherwise, iterate over all the conservative guards, seeing if any
          // are interesting.
          for (const auto* guard : indexed.conservative_guards) {
            if (!hasEmptyLayout(*guard)) {
              indexed.ignore = false;
              makePtrFollowable(*indexed.type);
              changed = true;
              break;
            }
          }
          continue;
        }

        if (indexed.ignore) continue;

        // Generate the new layout for this type, including any suffix.
        Layout new_layout{determineSize(*indexed.type)};
        genLayout(*indexed.type, new_layout, 0);
        if (indexed.suffix_type) {
          Layout suffix_layout{determineSize(*indexed.suffix_type)};
          genLayout(*indexed.suffix_type, suffix_layout, 0);
          new_layout.setSuffix(std::move(suffix_layout));
        }

        if (indexed.layout != new_layout) {
          // This new layout is different. If this isn't a "scan-only" indexed
          // type (which can't be pointer followable), and the type was
          // previously un-interesting, make this pointer followable.
          if (!indexed.scan && indexed.layout.isEmpty()) {
            makePtrFollowable(*indexed.type);
          }
          changed = true;
          indexed.layout = std::move(new_layout);
        }
      } catch (LayoutError& exn) {
        indexed.errors = std::move(exn);
      }
    }
  } while (changed);
}

// At this point, the layouts in the indexed types are correct. However, some of
// the indexed types may have the same layout. Put all the unique and sorted
// layouts into the m_layouts list. Assign each indexed type its appropriate
// index into the list.
void Generator::assignUniqueLayouts() {
  // First fix up some of the indexed types so that they're in a consistent
  // state. These transformations are safe to perform as we know the layouts at
  // maximal.
  for (auto& indexed : m_indexed_types) {
    // At this point, we definitely know which types will be conservative
    // scanned or not, so we do not need the guards anymore. Clearing these lets
    // us merge together more duplicates.
    indexed.conservative_guards.clear();

    if (indexed.ignore) {
      // If the indexed type is still marked as ignored, it cannot be
      // conservative scanned.
      indexed.conservative = false;
      continue;
    }
    if (indexed.conservative) {
      // Likewise, if the indexed type is still marked for conservative
      // scanning, it cannot be ignored.
      indexed.ignore = false;
      continue;
    }
    if (indexed.layout.isEmpty()) {
      // If this type isn't interesting, mark it as if it was explicitly
      // ignored.
      indexed.ignore = true;
      indexed.layout.clear();
      continue;
    }
    if (indexed.layout.isAllConservative()) {
      // If the layout contains nothing but conservative scans, mark it as if it
      // was explicitly marked for conservative scans.
      indexed.conservative = true;
      indexed.layout.clear();
      continue;
    }
    // Finally, if there's a suffix layout, and the suffix begins at offset 0,
    // than the suffix layout can completely subsume the original layout.
    if (indexed.layout.suffix && indexed.layout.suffix_begin == 0) {
      // avoid indeterminate evaluation order by moving indexed.layout.suffix
      // to a temp before overwriting indexed.layout
      auto suffix = std::move(*indexed.layout.suffix);
      indexed.layout = std::move(suffix);
    }
  }

  // Now that the indexed types are fixed up to be more consistent, merge
  // duplicates together.
  m_indexed_types = mergeDupIndexedTypes(std::move(m_indexed_types));

  // Record all generated layouts in m_layouts (ignoring ignored or conservative
  // ones since those have hard-coded scanners).
  for (const auto& indexed : m_indexed_types) {
    if (indexed.ignore || indexed.conservative) continue;
    m_layouts.emplace_back(indexed.layout);
  }
  // Sort them and make them unique.
  std::sort(m_layouts.begin(), m_layouts.end());
  m_layouts.erase(
    std::unique(m_layouts.begin(), m_layouts.end()),
    m_layouts.end()
  );

  // Record the appropriate offset into m_layouts for each indexed type to refer
  // to its layout.
  for (auto& indexed : m_indexed_types) {
    if (indexed.ignore || indexed.conservative) continue;
    auto result = std::equal_range(
      m_layouts.begin(),
      m_layouts.end(),
      indexed.layout
    );
    assert(result.first != result.second);
    indexed.layout_index = std::distance(m_layouts.begin(), result.first);
  }
}

// Check for any errors while generating layouts. We don't want to report these
// errors immediately, as we want to gather them up and report them all at the
// end. This is helpful when there's several things wrong at once.
void Generator::checkForLayoutErrors() const {
  std::ostringstream oss;
  size_t error_count = 0;
  for (const auto& indexed : m_indexed_types) {
    if (indexed.errors) {
      // Don't go overboard....
      if (++error_count > 15) break;
      const auto& errors = *indexed.errors;
      oss << "error: " << errors.reason << "\n";
      for (const auto& context : errors.context) {
        oss << "\t- " << context << "\n";
      }
      oss << "\t- from type '" << *indexed.type << "'\n"
             "\t- if annotations are needed, see definitions in "
             "hphp/util/type-scan.h.\n\n";
    }
    // Error if an indexed type had internal linkage.
    for (const auto& address : indexed.addresses) {
      HPHP::match<void>(address,
        [&](const std::string&) { /* ok */ },
        [&](uintptr_t) {
          ++error_count;
          oss << "error: type " << *indexed.type << " has internal linkage.\n"
                 " Indexed types need external linkage.\n";
        }
      );
    }
  }
  if (error_count > 0) throw Exception{oss.str()};
}

/*
 * C++ code generation functions:
 */

/*
 * Output all the needed C++ forward declarations. Any called custom scanners
 * need to forward declared, as well as any Indexer<>::s_index static instances
 * which have a symbol. Normally to forward declare these, we would have to
 * forward declare a lot of other types, but we employ a dirty trick to avoid
 * this. We forward declare the mangled names, wrapped in an extern C block.
 */
void Generator::genForwardDecls(std::ostream& os) const {
  std::set<std::string> decls;

  for (const auto& layout : m_layouts) {
    for (const auto& custom : layout.custom) {
      decls.emplace(custom.linkage_name);
    }
    if (layout.suffix) {
      for (const auto& custom : layout.suffix->custom) {
        decls.emplace(custom.linkage_name);
      }
    }
  }

  for (const auto& decl : decls) {
    os << "/* " << folly::demangle(decl.c_str()) << " */\n"
       << "extern \"C\" void " << decl << "(const void*, Scanner&);\n";
  }
  os << "\n";

  decls.clear();
  for (const auto& indexed : m_indexed_types) {
    for (const auto& addr : indexed.addresses) {
      if (auto* decl = std::get_if<std::string>(&addr)) {
        decls.emplace(*decl);
      }
    }
  }

  for (const auto& decl : decls) {
    os << "/* " << folly::demangle(decl.c_str()) << " */\n"
       << "extern \"C\" Index " << decl << ";\n";
  }
  os << "\n";
}

// Output the initialization of the metadata table mapping the type indices to
// the type name and scanner.
void Generator::genDataTable(std::ostream& os) const {
  os << "const HPHP::type_scan::detail::Metadata g_table[] = {\n";
  os << "  {\"(UNKNOWN)\", scanner_conservative},\n";
  os << "  {\"(UNKNOWN NO-PTRS)\", scanner_noptrs},\n";
  for (const auto& indexed : m_indexed_types) {
    const auto get_scanner_name = [&]() -> std::string {
      if (indexed.ignore) return "scanner_noptrs";
      if (indexed.conservative) return "scanner_conservative";
      return fmt::format("scanner_{}", indexed.layout_index);
    };
    os << "  {\"" << *indexed.type << "\", "
       << get_scanner_name() << "},\n";
  }
  os << "};\n\n";
}

// Output the initialization function which inserts the type indices to the
// appropriate Indexer<>::s_index static instances.
void Generator::genIndexInit(std::ostream& os) const {
  os << "void init_indices() {\n";
  size_t index = 2;
  for (const auto& indexed : m_indexed_types) {
    os << "  /* " << *indexed.type << " */\n";
    for (const auto& address : indexed.addresses) {
      HPHP::match<void>(address,
                        [&](const std::string& s) {
                          os << "  " << s << " = " << index << ";\n";
                        },
                        [&](uintptr_t p) {
                          os << "  *reinterpret_cast<Index*>(0x" << std::hex
                             << p << std::dec << ") = " << index << ";\n";
                        });
    }
    ++index;
  }
  os << "\n  static_assert(" << index-1
     << " <= std::numeric_limits<Index>::max(), "
     << "\"type_scan::Index is too small for all types\");\n";
  os << "}\n\n";
}

void Generator::genBuiltinScannerFuncs(std::ostream& os) const {
  os << "void scanner_conservative(Scanner& scanner, "
     << "const void* ptr, size_t size) {\n"
     << "  scanner.m_conservative.emplace_back(ptr, size);\n"
     << "}\n\n";
  os << "void scanner_noptrs(Scanner& scanner, "
     << "const void* ptr, size_t size) {\n"
     << "}\n\n";
}

void Generator::genScannerFuncs(std::ostream& os) const {
  genBuiltinScannerFuncs(os);
  std::vector<std::vector<std::string>> types(m_layouts.size());
  for (const auto& indexed : m_indexed_types) {
    if (indexed.ignore || indexed.conservative) continue;
    types[indexed.layout_index].push_back(indexed.type->toString());
  }
  for (size_t i = 0; i < m_layouts.size(); ++i) {
    auto& type_list = types[i];
    std::sort(type_list.begin(), type_list.end());
    for (auto& t : type_list) {
      os << "// " << t << "\n";
    }
    os << "void scanner_" << i << "(Scanner& scanner, "
       << "const void* ptr, size_t size) {\n";
    genScannerFunc(os, m_layouts[i]);
    os << "}\n\n";
  }
}

// For a given layout, output the matching C++ code to implement the scanner.
void Generator::genScannerFunc(std::ostream& os,
                               const Layout& layout,
                               size_t begin_offset) const {
  // Assert that the size passed into the scanner is a multiple of the type
  // size.
  if (layout.size > 0) {
    if (begin_offset > 0) {
      os << "  assert((size - " << begin_offset << ") % "
         << layout.size << " == 0);\n";
    } else if (!layout.suffix) {
      os << "  assert(size % " << layout.size << " == 0);\n";
    }
  }

  // If there's no suffix, the scanner is wrapped within a for loop which loops
  // over the entire allocation given by the size parameter.
  if (!layout.suffix) {
    os << "  for (size_t offset = "
       << begin_offset << "; offset < size; offset += "
       << layout.size << ") {\n";
  }

  // Ident appropriately depending on whether we're inside a for loop or not.
  auto indent = [&](int level = 0) -> std::ostream& {
    if (!layout.suffix) level += 2;
    for (size_t i = 0; i < level; ++i) {
      os << " ";
    }
    return os;
  };

  // If we're in a for loop, the offsets need to be biased by the loop
  // iteration.
  const auto* offset_str = layout.suffix ? "" : "+offset";

  // First generate calls to the scanner to record all the pointers. We use the
  // version of insert() which takes an initializer list because it is more
  // efficient.
  if (layout.ptrs.size() == 1) {
    indent(2) << "scanner.m_addrs.emplace_back(\n";
    const auto& ptr = layout.ptrs.back();
    indent(4) << "((const void**)(uintptr_t(ptr)"
              << offset_str << "+" << ptr.offset << "))\n";
    indent(2) << ");\n";
  } else if (!layout.ptrs.empty()) {
    indent(2) << "scanner.m_addrs.insert(scanner.m_addrs.end(), {\n";
    for (const auto& ptr : layout.ptrs) {
      indent(4) << "((const void**)(uintptr_t(ptr)"
                << offset_str << "+" << ptr.offset
                << ((&ptr == &layout.ptrs.back()) ? "))\n" : ")),\n");
    }
    indent(2) << "});\n";
  }

  // In a similar manner, insert conservative ranges.
  if (layout.conservative.size() == 1) {
    indent(2) << "scanner.m_conservative.emplace_back(\n";
    const auto& conservative = layout.conservative.back();
    indent(4) << "(const void*)(uintptr_t(ptr)"
              << offset_str << "+" << conservative.offset << "), "
              << conservative.size << "\n";
    indent(2) << ");\n";
  } else if (!layout.conservative.empty()) {
    indent(2) << "scanner.m_conservative.insert(scanner.m_conservative.end(), "
              << "{\n";
    for (const auto& conservative : layout.conservative) {
      indent(4) << "{(const void*)(uintptr_t(ptr)"
                << offset_str << "+" << conservative.offset << "), "
                << conservative.size
                << ((&conservative == &layout.conservative.back()) ?
                    "}\n" : "},\n");
    }
    indent(2) << "});\n";
  }

  // Finally generate calls to all custom functions. Use the current offset to
  // form the this pointer for the method call.
  for (const auto& custom : layout.custom) {
    indent(2) << custom.linkage_name << "((const void*)(uintptr_t(ptr)"
              << offset_str << "+" << custom.offset << "), scanner);\n";
  }

  if (!layout.suffix) {
    os << "  }\n";
  } else {
    // If we have a suffix, we didn't generate a for loop, just straightline
    // code. Now generate code for the suffix portion.
    genScannerFunc(os, *layout.suffix, layout.suffix_begin);
  }
}

void Generator::genMetrics(std::ostream& os) const {
  os << "// type_scan Metrics:\n";
  os << "// unique layouts: " << m_layouts.size() << std::endl;
  os << "// indexed types: " << m_indexed_types.size() << std::endl;
  os << "// pointer followable types: " << m_ptr_followable.size() << std::endl;
  os << "// collectable types: " << m_collectable.size() << std::endl;
  os << "// scannable collectable types: " << m_scannable_collectable.size()
     << std::endl;

  size_t conservative_fields{0};
  size_t conservative_types{0};
  size_t custom_fields{0};
  size_t custom_types{0};
  size_t custom_bases{0};
  size_t ignored_fields{0};
  size_t ignored_types{0};
  size_t whitelisted_types{0};
  size_t forbidden_templates{0};
  size_t flexible_arrays{0};
  for (auto& e : m_actions) {
    auto& action = e.second;
    if (action.ignore_all) ignored_types++;
    if (action.conservative_all) conservative_types++;
    if (action.whitelisted) whitelisted_types++;
    if (action.forbidden_template) forbidden_templates++;
    if (action.custom_all) custom_types++;
    if (!action.flexible_array_field.empty()) flexible_arrays++;
    // count custom guards?
    ignored_fields += action.ignore_fields.size();
    conservative_fields += action.conservative_fields.size();
    custom_fields += action.custom_fields.size();
    custom_bases += action.custom_bases.size();
  }

  os << "// object types: " << m_actions.size() << std::endl;
  os << "// conservative-scanned types: " << conservative_types << std::endl;
  os << "// conservative-scanned fields: " << conservative_fields << std::endl;
  os << "// custom-scanned types: " << custom_types << std::endl;
  os << "// custom-scanned fields: " << custom_fields << std::endl;
  os << "// custom-scanned bases: " << custom_bases << std::endl;
  os << "// ignored types: " << ignored_types << std::endl;
  os << "// ignored fields: " << ignored_fields << std::endl;
  os << "// whitelisted types: " << whitelisted_types << std::endl;
  os << "// forbidden templates: " << forbidden_templates << std::endl;
  os << "// flexible arrays: " << flexible_arrays << std::endl;
  os << std::endl;
}

// Generate the entire C++ file.
void Generator::operator()(std::ostream& os, bool skip) const {
  if (!skip && m_layouts.empty()) {
    throw Exception{"No layouts generated"};
  }

  os << "#include <limits>\n\n";

  os << "#include \"hphp/util/assertions.h\"\n";
  os << "#include \"hphp/util/portability.h\"\n";
  os << "#include \"hphp/util/type-scan.h\"\n\n";

  os << "using namespace HPHP::type_scan;\n";
  os << "using namespace HPHP::type_scan::detail;\n\n";

  genMetrics(os);
  genForwardDecls(os);

  os << "namespace {\n\n";
  genScannerFuncs(os);
  genDataTable(os);
  genIndexInit(os);
  os << "}\n\n";

  os << "extern \"C\" {\n\n"
     << "EXTERNALLY_VISIBLE const Metadata* "
     << HPHP::type_scan::detail::kInitFuncName
     << "(size_t& table_size) {\n"
     << "  init_indices();\n"
     << "  table_size = " << m_indexed_types.size()+2 << ";\n"
     << "  return g_table;\n"
     << "}\n\n"
     << "}" << std::endl;
}

Generator::Layout::Layout(const Layout& other)
  : size{other.size}
  , ptrs{other.ptrs}
  , conservative{other.conservative}
  , custom{other.custom}
  , suffix{other.suffix ? std::make_unique<Layout>(*other.suffix) : nullptr}
  , suffix_begin{other.suffix_begin}
{
}

Generator::Layout& Generator::Layout::operator=(const Layout& other) {
  size = other.size;
  ptrs = other.ptrs;
  conservative = other.conservative;
  custom = other.custom;
  suffix = other.suffix ? std::make_unique<Layout>(*other.suffix) : nullptr;
  suffix_begin = other.suffix_begin;
  return *this;
}

void Generator::Layout::addConservative(size_t offset, size_t size) {
  if (!size) return;
  // Eagerly coalesce adjacent conservative ranges.
  if (!conservative.empty()) {
    auto& back = conservative.back();
    if (back.offset + back.size == offset) {
      back.size += size;
      return;
    }
  }
  conservative.emplace_back(Conservative{offset, size});
}

bool Generator::Layout::isAllConservative() const {
  if (!ptrs.empty() || !custom.empty()) return false;
  if (conservative.size() != 1) return false;
  if (suffix && !suffix->isAllConservative()) return false;
  const auto& back = conservative.back();
  return back.offset == 0 && back.size == size;
}

void Generator::Layout::clear() {
  ptrs.clear();
  conservative.clear();
  custom.clear();
  suffix.reset();
  suffix_begin = 0;
}

size_t Generator::Layout::totalCount() const {
  return ptrs.size() +
    conservative.size() +
    custom.size() +
    (suffix ? suffix->totalCount() : 0);
}

void Generator::Layout::merge(const Layout& other, size_t offset) {
  for (const auto& entry : other.ptrs) {
    addPtr(offset + entry.offset);
  }
  for (const auto& entry : other.conservative) {
    addConservative(offset + entry.offset, entry.size);
  }
  for (const auto& entry : other.custom) {
    addCustom(offset + entry.offset, entry.linkage_name);
  }
}

bool Generator::Layout::operator==(const Layout& other) const {
  if (std::tie(size, ptrs, conservative,
               custom, suffix_begin) !=
      std::tie(other.size, other.ptrs, other.conservative,
               other.custom, other.suffix_begin)) {
    return false;
  }
  if (!suffix) return !other.suffix;
  if (!other.suffix) return !suffix;
  return *suffix == *other.suffix;
}

// Arbitrary ordering of layouts. This ordering was chosen to satisfy my
// aesthetics of having "simpler" scanners come first.
bool Generator::Layout::operator<(const Layout& other) const {
  const auto count1 = totalCount();
  const auto count2 = other.totalCount();
  if (count1 != count2) return count1 < count2;

  if (ptrs.size() != other.ptrs.size()) {
    return ptrs.size() < other.ptrs.size();
  }
  if (conservative.size() != other.conservative.size()) {
    return conservative.size() < other.conservative.size();
  }
  if (custom.size() != other.custom.size()) {
    return custom.size() < other.custom.size();
  }

  if (ptrs != other.ptrs) return ptrs < other.ptrs;
  if (conservative != other.conservative) {
    return conservative < other.conservative;
  }
  if (custom != other.custom) return custom < other.custom;

  if (size != other.size) return size < other.size;

  if (suffix) {
    if (!other.suffix) return false;
    if (*suffix != *other.suffix) return *suffix < *other.suffix;
    return suffix_begin < other.suffix_begin;
  } else {
    return static_cast<bool>(other.suffix);
  }
}

const std::string kProgramDescription =
  "Generate type-scanners from debug-info";

////////////////////////////////////////////////////////////////////////////////

}

int main(int argc, char** argv) {
  folly::SingletonVault::singleton()->registrationComplete();

  namespace po = boost::program_options;

  po::options_description desc{"Allowed options"};
  desc.add_options()
    ("help", "produce help message")
    ("install_dir",
     po::value<std::string>(),
     "directory to put generated scanners")
    ("fbcode_dir", po::value<std::string>(), "ignored")
    ("source_file",
     po::value<std::string>()->required(),
     "filename to read debug-info from")
    ("dep",
      po::value<std::vector<std::string>>(),
      "just here so we can add dependencies")
    ("output_file",
     po::value<std::string>()->required(),
     "filename of generated scanners")
    ("skip", "do not scan dwarf, generate conservative scanners")
    ("num_threads", po::value<int>(), "number of parallel threads")
    ("print", "dump the dwarf to stdout")
    ;

  try {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.contains("help")) {
      std::cout << kProgramDescription << "\n\n"
                << desc << std::endl;
      return 1;
    }

#if defined(__clang__) && !defined(CLANG_STANDALONE_DEBUG)
    // Doesn't work with older Clang that don't support attribute used
    // in member functions of template classes.
    // Fixed in https://reviews.llvm.org/D56928
    // Doesn't work with Clang without -fstandalone-debug
  auto skip = true;
#else
  auto skip = vm.count("skip") || getenv("HHVM_DISABLE_TYPE_SCANNERS");
#endif

    po::notify(vm);
    auto const print = vm.contains("print");

    const auto output_filename =
      vm.contains("install_dir") ?
      fmt::format(
        "{}{}{}",
        vm["install_dir"].as<std::string>(),
        HPHP::FileUtil::getDirSeparator(),
        vm["output_file"].as<std::string>()
      ) :
      vm["output_file"].as<std::string>();

    if (vm.contains("num_threads")) {
      auto n = vm["num_threads"].as<int>();
      if (n > 0) {
        NumThreads = n;
      } else {
        std::cerr << "\nIllegal num_threads=" << n << "\n";
        return 1;
      }
    }

    try {
      const auto source_executable = vm["source_file"].as<std::string>();
      if (print) {
        auto const printer = debug_parser::Printer::make(source_executable);
        (*printer)(std::cout);
      }
      Generator generator{source_executable, skip};
      std::ofstream output_file{output_filename};
      generator(output_file, skip);
    } catch (const debug_parser::Exception& exn) {
      std::cerr << "\nError generating type scanners:\n"
                << exn.what() << std::endl << std::endl;
      return 1;
    }
  } catch (const po::error& e) {
    std::cerr << e.what() << "\n\n"
              << kProgramDescription << "\n\n"
              << desc << std::endl;
    return 1;
  }

  return 0;
}
