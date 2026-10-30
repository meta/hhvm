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

#if defined(__linux__) 

#include <fmt/format.h>
#include <folly/Demangle.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/portability/Unistd.h>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#if LLVM_VERSION_MAJOR > 20
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"
#else
#include "llvm/DebugInfo/DWARF/DWARFExpression.h"
#endif
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"

#include "hphp/util/assertions.h"
#include "hphp/util/job-queue.h"
#include "hphp/util/trace.h"

#include "hphp/tools/debug-parser/dwarf-context-manager.h"
#include "hphp/tools/debug-parser/dwarf-global-offset.h"
#include "hphp/tools/debug-parser/debug-parser.h"

/*
 * Debug parser for DWARF
 *
 * DWARF is structured as a forest of DIEs (Debug Information Entry). Each DIE
 * has a tag, which describes what kind of DIE it is, and a list of
 * attributes. Each attribute has a type, which identifies what it is, and a
 * value (the type of the value is implied by the attribute type). Furthermore,
 * a DIE can have other DIEs as children. The top-level DIEs correspond to
 * compilation-units, and all the children of these top-level DIEs correspond to
 * the information in that compilation-unit.
 *
 * The meaning and interpretation of the DIEs is deliberately left vague by the
 * standard, so different compilers can encode things in different ways (and no
 * implementation is bug free).
 */

namespace debug_parser { namespace {

TRACE_SET_MOD(trans)

////////////////////////////////////////////////////////////////////////////////

// Allow foreach on a range (as returned by equal_range)
template<typename It> It begin(std::pair<It,It> p) { return p.first; }
template<typename It> It end(std::pair<It,It> p) { return p.second; }

/*
 * Fully qualified names aren't represented explicitly in DWARF. Instead the
 * structure of the DIEs mimics the nesting structure in the source (IE, a
 * nested class within a class nested within a namespace). So, in order to
 * infer the fully qualified name for any given class, the current scope is
 * tracked as the DIEs are walked.
 *
 * Likewise, DWARF has no concept of linkage, but the linkage is needed to know
 * which types are actually equivalent. Luckily, a type's linkage is closely
 * related to its scope (except for templates, see below), so it can be inferred
 * the same way.
 *
 * The scope is tracked as a stack of contexts, pushing and popping off contexts
 * when a namespace or type is entered or exited.
 */

struct Scope {
  explicit Scope(GlobalOff cu_offset)
      : m_cu_offset{cu_offset}
  {
    m_scope.emplace_back(
      ObjectTypeName{std::string{}, ObjectTypeName::Linkage::external},
      true
    );
  }

  GlobalOff cuOffset() const { return m_cu_offset; }

  ObjectTypeName name() const;

  // Fix the name of a type to match where it is in the namespace/type
  // hierarchy.
  void fixName(ObjectTypeName newName);

  ObjectTypeName::Linkage linkage() const {
    return m_scope.back().name.linkage;
  }

  std::size_t unnamedTypeCount() const {
    return m_scope.back().unnamed_count;
  }

  bool isInNamespaceScope() const {
    return m_scope.back().in_namespace_scope;
  }

  void incUnnamedTypeCount() { ++m_scope.back().unnamed_count; }

  HPHP::Optional<GlobalOff> typeOffset() const {
    return m_scope.back().offset;
  }

  void pushType(std::string name, GlobalOff offset) {
    m_scope.emplace_back(
      ObjectTypeName{std::move(name), linkage()},
      false
    );
    m_scope.back().offset = offset;
  }

  void pushUnnamedType(std::string name, GlobalOff offset) {
    m_scope.emplace_back(
      ObjectTypeName{
        std::move(name),
        ObjectTypeName::Linkage::none
      },
      false
    );
    m_scope.back().offset = offset;
  }

  void pushNamespace(std::string ns) {
    m_scope.emplace_back(
      ObjectTypeName{std::move(ns), linkage()},
      true
    );
  }

  void pushUnnamedNamespace() {
    m_scope.emplace_back(
      ObjectTypeName{
        "(unnamed namespace)",
        ObjectTypeName::Linkage::internal
      },
      true
    );
  }

  void pop() { m_scope.pop_back(); }

private:
  struct Context {
    Context(ObjectTypeName name, bool in_namespace_scope)
        : name(std::move(name))
        , in_namespace_scope{in_namespace_scope} {}
    ObjectTypeName name;
    bool in_namespace_scope;
    std::size_t unnamed_count = 0;
    HPHP::Optional<GlobalOff> offset;
  };
  std::vector<Context> m_scope;
  GlobalOff m_cu_offset;

public:
  static const std::string s_pseudo_type_name;
};

/*
 * Actual implementation of TypeParser for DWARF.
 */

struct TypeParserImpl : TypeParser {
  explicit TypeParserImpl(const std::string& filename, int num_threads);

  Object getObject(ObjectTypeKey key) override;

  size_t getObjectBlockCount() const override;

protected:
  const std::vector<ObjectType>& getObjectBlock(size_t index) const override;

private:
  struct StateBlock;

  struct LinkageDependents {
    folly::F14FastSet<GlobalOff> template_uses;
    folly::F14FastSet<GlobalOff> children;
  };

  struct StaticSpec {
    static auto constexpr kNoAddress = std::numeric_limits<uint64_t>::max();
    std::string linkage_name;
    uint64_t address{kNoAddress};
    bool is_member{false};
  };

  struct Env {
    const DWARFContextManager* dwarfContext;
    std::unique_ptr<StateBlock> state;
    folly::F14FastMap<GlobalOff, GlobalOff> local_mappings;
    folly::F14FastMap<GlobalOff, LinkageDependents> linkage_dependents;
    std::vector<std::pair<GlobalOff, StaticSpec>> raw_static_definitions;
  };

  // Functions used while concurrently building state. Since these functions are
  // invoked from multiple threads, they are static and take all their state
  // explicitly as parameters.
  static void genNames(Env& env,
                       const DieContext& dieContext,
                       Scope& scope,
                       std::vector<GlobalOff>* template_params = nullptr);

  static HPHP::Optional<uintptr_t> interpretLocAddress(const DWARFContextManager& dwarfContext,
                                                       const DieContext& dieContext,
                                                       llvm::DWARFAttribute attr);
  static HPHP::Optional<GlobalOff> parseSpecification(const DWARFContextManager& dwarfContext,
                                                      const DieContext& dieContext,
                                                      bool first,
                                                      StaticSpec& spec);
  void fixTemplateLinkage();

  // Functions used after state is built. These are not thread-safe.
  Object genObject(const DieContext& dieContext,
                   ObjectTypeName name,
                   ObjectTypeKey key);
  Type genType(const DieContext& dieContext);
  Object::Member genMember(const DieContext& dieContext,
                           const ObjectTypeName& parent_name);
  Object::Function genFunction(const DieContext& dieContext);
  Object::Base genBase(const DieContext& dieContext, const ObjectTypeName& parent_name);
  Object::TemplateParam genTemplateParam(const DieContext& dieContext);
  HPHP::Optional<size_t> determineArrayBound(const llvm::DWARFDie& die);

  void fillFuncArgs(const DieContext& dieContext, FuncType& func);

  // Map a given offset to the state block which contains state for that offset
  // (see below).
  const StateBlock& stateForOffset(GlobalOff offset) const {
    assertx(!m_state_map.empty());
    auto it = std::upper_bound(
      m_state_map.begin(),
      m_state_map.end(),
      offset,
      [](GlobalOff offset, const std::pair<GlobalOff, StateBlock*>& p) {
        return offset < p.first;
      }
    );
    if (it != m_state_map.begin()) --it;
    return *it->second;
  }

  // All of the parser's persistent state is stored in some number of
  // blocks. All of the blocks are computed concurrently, one block per
  // thread. To avoid the overhead of merging the blocks together, they are kept
  // separated. Instead m_state_map is used to map a given offset into the block
  // which contains the state for that offset. It is a list of offset/state
  // pairs. Any offset between the offset given in the pair and the one in the
  // next pair is mapped to the state block in the pair.
  //
  // Note: this scheme only works because each compilation unit is
  // self-contained and does not reference data in another compilation
  // unit. However, nothing in DWARF prevents this and its not guaranteed to
  // always be true.
  struct StateBlock {
    std::vector<ObjectType> all_objs;
    folly::F14FastMap<GlobalOff, size_t> obj_offsets;
    std::multimap<GlobalOff, StaticSpec> static_definitions;
  };
  std::vector<std::unique_ptr<StateBlock>> m_states;
  std::vector<std::pair<GlobalOff, StateBlock*>> m_state_map;
  tbb::concurrent_hash_map<GlobalOff,
                           LinkageDependents,
                           GlobalOff::Hash> m_linkage_dependents;

  DWARFContextManager m_dwarfContext;
};

// Purposefully fake name to avoid confusion with an actual type.
const std::string Scope::s_pseudo_type_name = "@_PSEUDO_TY";

ObjectTypeName Scope::name() const {
  auto iter = m_scope.begin();
  std::string str = iter->name.name;
  ++iter;
  for (; iter != m_scope.end(); ++iter) {
    if (str.empty()) str = iter->name.name;
    else str = fmt::format("{}::{}", str, iter->name.name);
  }
  return ObjectTypeName{std::move(str), linkage()};
}

void Scope::fixName(ObjectTypeName newName) {
  if (m_scope.size() == 1) {
    m_scope.back().name = std::move(newName);
    return;
  }

  auto context = std::move(m_scope.back());
  m_scope.pop_back();
  auto outerName = name();
  assertx(newName.name.size() > outerName.name.size());
  if (outerName.name.size()) {
    assertx(!outerName.name.compare(0, outerName.name.size(), newName.name));
    newName.name = newName.name.substr(outerName.name.size() + 2);
  }
  context.name = std::move(newName);
  m_scope.push_back(std::move(context));
}

TypeParserImpl::TypeParserImpl(const std::string& filename, int num_threads)
    : m_dwarfContext{filename}
{
  // Processing each compilation unit is very expensive, as it involves walking
  // a large part of the debug information. To speed things up (a lot), we build
  // up the state concurrently. Create a job corresponding to each compilation
  // unit in the file and enqueue the jobs with a thread pool. We'll find the
  // offsets of the compilation unit in the main thread, enqueuing them as we
  // find them. This lets us not only exploit concurrency between processing
  // compilation units, but between finding them and processing them.
  //
  // Each worker maintains its own private state which it populates for all the
  // compilation units its assigned (each worker can process multiple
  // compilation units). Once done, all the different states are kept separate
  // (merging them would be too expensive), but a mapping is constructed to map
  // offsets to the appropriate state block.
  //
  // This whole scheme is only viable because (right now), debug information in
  // a given compilation unit doesn't reference anything outside of that unit,
  // so the state for any given compilation unit can be processed
  // independently.

  // The context serves as the link between a worker and the TypeParserImpl
  // state (this is forced by the JobQueueWorker interface).
  struct Context {
    const decltype(m_dwarfContext)& dwarfContext;
    decltype(m_states)& states;
    decltype(m_state_map)& state_map;
    decltype(m_linkage_dependents)& linkage_dependents;
    // The lock protects states, state_map, and the exception field (but only
    // when the workers are running).
    std::mutex lock;
    // Set to the exception if any of the workers threw (first one wins).
    std::exception_ptr exception;
  };

  // Thread worker. We'll end up with a state block for each one of these.
  struct Worker : HPHP::JobQueueWorker<GlobalOff, Context*> {
    Env env;

    // Remember each offset we processed so we can record it the global state
    // map when we finish.
    std::vector<GlobalOff> offsets;

    void doJob(GlobalOff globalOff) override {
      // Process a compilation unit at the given offset.
      try {
        // We're going to use it so let's mark this worker active.
        if (!env.dwarfContext) {
          env.dwarfContext = &m_context->dwarfContext;
          env.state = std::make_unique<StateBlock>();
        }

        auto dwarfUnit = env.dwarfContext->findUnitForGlobalOffset(globalOff);
        // Force load the unit which will be further processed in this worker
        auto unitDie = dwarfUnit->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
        offsets.emplace_back(globalOff);

        // Do the actual processing, adding to the state block:
        Scope scope{globalOff};
        genNames(env, {unitDie, globalOff.isInfo()}, scope);

        auto const remap = [&] (GlobalOff o) {
          auto const it = env.local_mappings.find(o);
          if (it != env.local_mappings.end()) {
            return it->second;
          }
          return o;
        };

        // Generate static_definitions by updating their keys collected during
        // genNames. Some keys refer back to a DW_AT_member that belongs to a
        // struct whose definition was in another type-unit. We want to add an
        // entry for the member in the definition.
        std::transform(
            env.raw_static_definitions.begin(),
            env.raw_static_definitions.end(),
            std::inserter(
                env.state->static_definitions,
                env.state->static_definitions.end()),
            [&](const auto& elem) {
              return std::make_pair(remap(elem.first), std::move(elem.second));
            });
        env.raw_static_definitions.clear();

        for (auto& linkage : env.linkage_dependents) {
          if (!linkage.second.template_uses.size()) continue;

          std::decay_t<decltype(m_context->linkage_dependents)>::accessor acc;

          auto const inserted =
            m_context->linkage_dependents.insert(acc, remap(linkage.first));
          if (inserted && !env.local_mappings.size()) {
            acc->second = std::move(linkage.second);
          } else {
            auto const process = [&] (auto const& from, auto& to) {
              for (auto& elm : from) {
                to.insert(remap(elm));
              }
            };
            process(linkage.second.template_uses, acc->second.template_uses);
            process(linkage.second.children, acc->second.children);
          }
        }
        env.linkage_dependents.clear();
        env.local_mappings.clear();
      } catch (...) {
        // Store any exception thrown so it can be rethrown in the main
        // thread. We only bother to store the first one.
        stop();
        std::lock_guard<std::mutex> guard{m_context->lock};
        if (!m_context->exception) {
          m_context->exception = std::current_exception();
        }
      }
    }

    void onThreadExit() override {
      // The worker is done (we've been told to stop). Now that we know we won't
      // be processing anymore offsets, do the needed post-processing on the
      // rest of the state.
      if (!env.dwarfContext) return;
      try {
        // Compute a mapping of an object type's offset to its location in the
        // all_objs vector.
        env.state->obj_offsets.reserve(env.state->all_objs.size());
        for (auto i = size_t{0}; i < env.state->all_objs.size(); ++i) {
          env.state->obj_offsets.emplace(
            GlobalOff::fromRaw(env.state->all_objs[i].key.object_id), i
          );
        }

        // Record all the offsets this worker processed (along with the state
        // block) in the global state map. This is done using a lock because its
        // quick and only done when the thread is finishing.
        std::lock_guard<std::mutex> guard{m_context->lock};
        auto const state = env.state.get();
        m_context->states.emplace_back(std::move(env.state));
        for (auto offset : offsets) {
          m_context->state_map.emplace_back(offset, state);
        }
      } catch (...) {
        // Store any exception thrown so it can be rethrown in the main
        // thread. We only bother to store the first one.
        stop();
        std::lock_guard<std::mutex> guard{m_context->lock};
        if (!m_context->exception) {
          m_context->exception = std::current_exception();
        }
      }
    }
  };

  // Create the thread pool
  Context context{m_dwarfContext, m_states, m_state_map, m_linkage_dependents};
  HPHP::JobQueueDispatcher<Worker> dispatcher{
    num_threads, num_threads, 0, false, &context
  };

  size_t infoUnits = 0;
  size_t typeUnits = 0;
  FTRACE(1, "Adding units to dispatcher...\n");

  m_dwarfContext.forEachUnit([&](const std::unique_ptr<llvm::DWARFUnit>& unit, bool isInfo){
    // getNonSkeletonUnitDIE in general is not thread safe, so call it here and
    // then parse the unit in the worker thread.
    auto unitDie = unit->getNonSkeletonUnitDIE(/*ExtractUnitDIEOnly=*/true);
    dispatcher.enqueue(m_dwarfContext.getGlobalOffset(unitDie.getOffset(), isInfo));
    isInfo ? ++infoUnits : ++typeUnits;
    return true;
  });

  FTRACE(1, "... {} info units added.\n", infoUnits);
  FTRACE(1, "... {} type units added.\n", typeUnits);
  dispatcher.start();
  FTRACE(1, "Waiting for dispatcher...\n");
  dispatcher.stop();

  FTRACE(1, "Finished with genNames\n");

  // If any of the workers caught an exception, rethrow here in the main
  // thread. We don't need to bother taking the lock because all the workers are
  // gone.
  if (context.exception) std::rethrow_exception(context.exception);

  // Since the state map was appended to by the workers in a non-deterministic
  // order, we need to sort it by offset so we can do efficient lookups later.
  std::sort(
    m_state_map.begin(), m_state_map.end(),
    [&](const std::pair<GlobalOff, StateBlock*>& p1,
        const std::pair<GlobalOff, StateBlock*>& p2) {
      return p1.first < p2.first;
    }
  );

  // Some of the static_definitions entries need to be moved to the
  // correct block; eg they were seen when processing the cu
  // containing the definition of the static member, but need to be
  // moved to the state for the tu which contains the definition of
  // the struct (which may or may not be the same state block).
  folly::F14FastSet<void*> seen;
  for (auto const& p : m_state_map) {
    if (!seen.insert(p.second).second) continue;
    auto curOff = p.first;
    auto curState = p.second;
    for (auto it = p.second->static_definitions.begin();
         it != p.second->static_definitions.end(); ) {
      if (it->first != curOff) {
        curOff = it->first;
        curState = const_cast<decltype(p.second)>(&stateForOffset(curOff));
      }
      if (curState == p.second) {
        ++it;
        continue;
      }
      curState->static_definitions.insert(*it);
      it = p.second->static_definitions.erase(it);
    }
  }

  fixTemplateLinkage();
  m_linkage_dependents.clear();
}

size_t TypeParserImpl::getObjectBlockCount() const {
  return m_states.size();
}

const std::vector<ObjectType>&
TypeParserImpl::getObjectBlock(size_t index) const {
  return m_states[index]->all_objs;
}

/*
 * As stated above, the linkage of templates is tricky. The linkage of a
 * template is the most restrictive linkage of its original linkage and the
 * linkage of its template parameters. Since some of the template parameters may
 * not yet be parsed when we parse the template, the inference of the correct
 * template linkage is deferred until all the types' linkages are computed.
 *
 * However, since templates can be parameters to other templates, this process
 * must be repeated until the linkage of no types are changed.
 *
 * As an additional complication, the linkage of any nested class is inherited
 * from its parent, so when a template's linkage changes, it must be bubbled
 * down to any of its nested classes.
 *
 * When the name and initial linkages of all the types was generated, the
 * relationship between templates, their parameters, and nested classes is
 * recorded in linkage_dependents, which is used here.
 */
void TypeParserImpl::fixTemplateLinkage() {
  using ChangedSet = folly::F14FastSet<GlobalOff>;
  ChangedSet changed;

  for (const auto& pair : m_linkage_dependents) {
    if (pair.second.template_uses.empty()) continue;
    changed.emplace(pair.first);
  }

  ChangedSet old_changed;
  while (!changed.empty()) {
    std::swap(changed, old_changed);

    // For every type which has its linkage changed, update its dependents
    // (templates where the type is used as a parameter, or nested classes) with
    // the new linkage, and mark as being changed as well.
    for (auto changed_offset : old_changed) {
      decltype(m_linkage_dependents)::const_accessor acc;
      if (!m_linkage_dependents.find(acc, changed_offset)) continue;

      auto const& children = acc->second.children;
      auto const& template_uses = acc->second.template_uses;

      auto const& changed_state = stateForOffset(changed_offset);

      auto const it = changed_state.obj_offsets.find(changed_offset);
      if (it == changed_state.obj_offsets.end()) {
        // This isn't right - if (eg) its a pointer to an object type
        // with internal linkage, we need to mark the dependents
        // internal; but we don't track pointer types at all - so just
        // assume this type doesn't matter. The same goes for other
        // things like const struct types etc.
        continue;
      }

      auto const& changed_obj = changed_state.all_objs[it->second];

      // Only update and mark if we actually make the linkage more restrictive.
      if (changed_obj.name.linkage != ObjectTypeName::Linkage::external) {
        const auto process = [&](GlobalOff dependent_offset) {
          auto& dep_state = const_cast<StateBlock&>(
            stateForOffset(dependent_offset)
          );
          auto const it = dep_state.obj_offsets.find(dependent_offset);
          if (it == dep_state.obj_offsets.end()) return;
          auto& dependent_obj = dep_state.all_objs[it->second];
          if (dependent_obj.name.linkage < changed_obj.name.linkage) {
            FTRACE(4,
                   "Reducing linkage for {}({}) from {} to {} due to {}({})\n",
                   dependent_obj.name.name,
                   GlobalOff::fromRaw(dependent_obj.key.object_id),
                   show(dependent_obj.name.linkage),
                   show(changed_obj.name.linkage),
                   changed_obj.name.name,
                   GlobalOff::fromRaw(changed_obj.key.object_id));
            dependent_obj.name.linkage = changed_obj.name.linkage;
            changed.emplace(dependent_offset);
          }
        };
        for (auto template_offset : template_uses) process(template_offset);
        for (auto child_offset : children) process(child_offset);
      }
    }

    old_changed.clear();
  }
}

Object TypeParserImpl::getObject(ObjectTypeKey key) {
  auto const& state = stateForOffset(GlobalOff::fromRaw(key.object_id));
  auto iter = state.obj_offsets.find(GlobalOff::fromRaw(key.object_id));

  // If we don't know of an object type at the given location, assume its
  // referring to something we never parsed in the first place, so return the
  // pseudo-type.
  if (iter == state.obj_offsets.end()) {
    return Object{
      ObjectTypeName{
        Scope::s_pseudo_type_name,
        ObjectTypeName::Linkage::pseudo,
      },
      0,
      key,
      Object::Kind::k_other,
      true
    };
  }

  auto const offset = GlobalOff::fromRaw(key.object_id);
  auto dieContext = m_dwarfContext.getDieContextAtGlobalOffset(offset);
  return genObject(dieContext, state.all_objs[iter->second].name, key);
}

// For static members, determine how that member's address can be
// determined. In theory, this can be any arbitrary expression, but we only
// support constant addresses right now.
HPHP::Optional<uintptr_t>
TypeParserImpl::interpretLocAddress(const DWARFContextManager& dwarfContext,
                                    const DieContext& dieContext,
                                    llvm::DWARFAttribute attr) {
  const auto form = attr.Value.getForm();
  if (form != llvm::dwarf::DW_FORM_exprloc) return std::nullopt;

  llvm::DataExtractor extractor(
    *attr.Value.getAsBlock(),
    dwarfContext.getDWARFContext()->isLittleEndian()
#if LLVM_VERSION_MAJOR < 23
    ,
    dieContext.die.getDwarfUnit()->getAddressByteSize()
#endif
  );

  if (extractor.size() != 1) return std::nullopt;

  llvm::DWARFExpression expression(
    extractor,
    dieContext.die.getDwarfUnit()->getAddressByteSize()
  );

  const auto& ex = *expression.begin();
  if (ex.getCode() != llvm::dwarf::DW_OP_addr) return std::nullopt;
  return HPHP::Optional<uintptr_t>{ex.getRawOperand(0)};
}

uint64_t getRefOffset(const DieContext &dieContext,
                      const llvm::DWARFAttribute &attr) {
#if LLVM_VERSION_MAJOR >= 19
  if (auto ref = attr.Value.getAsRelativeReference()) {
    return dieContext.die.getDwarfUnit()->getOffset() + *ref;
  } else if (auto ref = attr.Value.getAsDebugInfoReference()) {
    return *ref;
  } else if (auto ref = attr.Value.getAsSignatureReference()) {
    return *ref;
  } else {
    // Unsupported reference type
    always_assert(0);
  }
#else
  return *attr.Value.getAsReference();
#endif
}

HPHP::Optional<GlobalOff>
TypeParserImpl::parseSpecification(const DWARFContextManager& dwarfContext,
                                   const DieContext& dieContext,
                                   bool first,
                                   StaticSpec &spec) {
  HPHP::Optional<GlobalOff> offset;
  bool is_inline = false;
  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_abstract_origin: {
        const auto attrOffset = getRefOffset(dieContext, attr);
        const auto globalOff = dwarfContext.getGlobalOffset(attrOffset, dieContext.isInfo);
        const auto die2Context = dwarfContext.getDieContextAtGlobalOffset(globalOff);
        offset = parseSpecification(dwarfContext, die2Context, false, spec);
        break;
      }
      case llvm::dwarf::DW_AT_specification: {
        const auto attrOffset = getRefOffset(dieContext, attr);
        offset = dwarfContext.getGlobalOffset(attrOffset, dieContext.isInfo);
        break;
      }
      case llvm::dwarf::DW_AT_linkage_name: {
        if (spec.linkage_name.empty()) {
          if (auto val = llvm::dwarf::toString(attr.Value)) {
            spec.linkage_name = *val;
          }
        }
        break;
      }
      case llvm::dwarf::DW_AT_location: {
        if (spec.address == StaticSpec::kNoAddress) {
          if (auto const address = interpretLocAddress(dwarfContext, dieContext, attr)) {
            spec.address = *address;
          }
        }
        break;
      }
      case llvm::dwarf::DW_AT_low_pc: {
        if (spec.address == StaticSpec::kNoAddress) {
          // TODO: check the func here
          spec.address = *attr.Value.getAsCStringOffset();
          // Sometimes GCC and Clang will emit invalid function
          // addresses. Usually zero, but sometimes a very low
          // number. These numbers have the appearance of being
          // un-relocated addresses, but its in the final executable. As
          // a safety net, if an address is provided, but its abnormally
          // low, ignore it.
          if (spec.address < 4096) spec.address = StaticSpec::kNoAddress;
        }
        break;
      }
      case llvm::dwarf::DW_AT_object_pointer:
        // Just in case we actually have a definition, use it to infer
        // member-ness.
        spec.is_member = true;
        break;
      default:
        break;
    }
  }
  if (first && (is_inline ||
                (spec.linkage_name.empty() &&
                 spec.address == StaticSpec::kNoAddress &&
                 !spec.is_member))) {
    return std::nullopt;
  }
  return offset;
}

/*
 * Given a DIE, and the current scope, recursively generate the names/linkages
 * for all the object types in this DIE and children. If template_params is
 * provided, the parent DIE is an object type, so template_params should be
 * filled with any template parameters in the child DIE.
 */
void TypeParserImpl::genNames(Env& env,
                              const DieContext& dieContext,
                              Scope& scope,
                              std::vector<GlobalOff>* template_params) {
  auto& dwarfContext = *env.dwarfContext;
  auto& state = *env.state;

  const auto recurse = [&](std::vector<GlobalOff>* params = nullptr){
    for (const auto& child : dieContext.die.children()) {
      genNames(env, {child, dieContext.isInfo}, scope, params);
    }
  };

  auto tag = dieContext.die.getTag();
  switch (tag) {
    case llvm::dwarf::DW_TAG_base_type:
    case llvm::dwarf::DW_TAG_union_type:
    case llvm::dwarf::DW_TAG_enumeration_type:
    case llvm::dwarf::DW_TAG_structure_type:
    case llvm::dwarf::DW_TAG_class_type:
    case llvm::dwarf::DW_TAG_unspecified_type: {
      // Object-types. These have names and linkages, so we must record them.

      // If this is a type-unit definition with a separate declaration
      // in the same tu, declarationOffset will point to the
      // declaration.
      HPHP::Optional<GlobalOff> declarationOffset;

      // If this is a declaration in a cu, referring back to a
      // tu-definition, definitionOffset will point to that
      // definition. Such declarations are emitted for the
      // *definitions* of static members (which always happen in cus,
      // not tus)
      HPHP::Optional<GlobalOff> definitionOffset;

      // Determine the base name, whether this type was unnamed, and whether
      // this is an incomplete type or not from the DIE's attributes.
      auto get_info = [&](DieContext curContext,
                          bool updateOffsets) ->
        std::tuple<std::string, bool, bool> {
        std::string name;
        std::string linkage_name;
        auto incomplete = false;

      for (const auto& attr : curContext.die.attributes()) {
        switch(attr.Attr) {
          case llvm::dwarf::DW_AT_name:
            if (auto val = llvm::dwarf::toString(attr.Value)) {
              name = *val;
            }
            break;
          case llvm::dwarf::DW_AT_linkage_name:
            if (auto val = llvm::dwarf::toString(attr.Value)) {
              linkage_name = *val;
            }
            break;
          case llvm::dwarf::DW_AT_declaration:
            incomplete = *attr.Value.getAsUnsignedConstant() != 0;
            break;
          case llvm::dwarf::DW_AT_specification:
            // The compiler can spit out a declaration for a
            // struct, followed later by the full definition. The
            // full definition has a DW_AT_specification pointing
            // back to the declaration - but note that the full
            // definition may not be defined in the correct
            // namespace - so we're going to keep the declaration,
            // and update it based on the definition ignoring the
            // definition's name (this feels a little backwards,
            // but its how dwarf works).
            if (updateOffsets) {
              declarationOffset = dwarfContext.getGlobalOffset(
                  getRefOffset(dieContext, attr), curContext.isInfo);
            }
            break;
          case llvm::dwarf::DW_AT_signature: {
            if (updateOffsets && attr.Value.getForm() == llvm::dwarf::DW_FORM_ref_sig8) {
              const auto sig8 = getRefOffset(dieContext, attr);
              // The actual definition is in another type-unit, we
              // can ignore this declaration.
              definitionOffset = dwarfContext.getTypeUnitOffset(sig8);
              break;
            }
            break;
          }
          default:
            break;
        }
      }

        // If there's an explicit name, just use that.
        if (!name.empty()) return std::make_tuple(name, false, incomplete);

        // Otherwise, if there's a linkage name, demangle it, and strip off
        // everything except the last section, and use that as the base
        // name. For types which have external linkage, this lets us use
        // whatever naming scheme the compiler has chosen for unnamed types.
        if (!linkage_name.empty()) {
          auto demangled = folly::demangle(linkage_name.c_str()).toStdString();
          auto index = demangled.rfind("::");
          if (index != decltype(demangled)::npos) demangled.erase(0, index+2);
          return std::make_tuple(demangled, false, incomplete);
        }

        // No explicit name and no linkage name to use, so we have to try to
        // infer one ourself (making it a synthetic name).

        // Try the first named member
        auto const first_member = [&](const char* type) {
          std::string first_member;
          for (const auto& child : curContext.die.children()) {
            if ((tag == llvm::dwarf::DW_TAG_enumeration_type &&
                 child.getTag() == llvm::dwarf::DW_TAG_enumerator) ||
                (tag != llvm::dwarf::DW_TAG_enumeration_type &&
                 (child.getTag() == llvm::dwarf::DW_TAG_member ||
                  child.getTag() == llvm::dwarf::DW_TAG_variable))) {
              first_member = dwarfContext.getDIEName(dieContext.die);
            }
            if (!first_member.empty()) break;
          }
          if (!first_member.empty()) {
            return fmt::format(
              "(unnamed {} containing '{}')", type, first_member
            );
          }
          return std::string{};
        };

        auto const type_name = [&]{
          if (tag == llvm::dwarf::DW_TAG_enumeration_type) return "enumeration";
          if (tag == llvm::dwarf::DW_TAG_union_type) return "union";
          if (tag == llvm::dwarf::DW_TAG_structure_type) return "struct";
          if (tag == llvm::dwarf::DW_TAG_class_type) return "class";
          return "type";
        };

        auto first_member_name = first_member(type_name());
        if (!first_member_name.empty()) {
          return std::make_tuple(
            std::move(first_member_name), true, incomplete
          );
        }

        // If this is within a namespace, don't infer any name at all, keep it
        // nameless. If its not within a namespace (IE, within a class), give it
        // a unique name based on how many unnamed types we've seen so far. We
        // can't do this for types within a namespace because namespaces are
        // open and thus we can't force a global numbering of all types within
        // it.
        if (!scope.isInNamespaceScope()) {
          scope.incUnnamedTypeCount();
          return std::make_tuple(
            fmt::format(
              "(unnamed {} #{})",
              type_name(),
              scope.unnamedTypeCount()
            ),
            true,
            incomplete
          );
        }

        return std::make_tuple(
          fmt::format("(unnamed {})", type_name()),
          true,
          incomplete
        );
      };

      const auto info = get_info(dieContext, /*updateOffsets=*/true);

      auto offset = dwarfContext.getGlobalOffset(dieContext.die.getOffset(), dieContext.isInfo);
      if (definitionOffset) {
        // This is a declaration which refers to the definition via
        // DW_AT_signature. We'll see one of these for a class in the
        // cu where its static members are defined.  Later
        // DW_TAG_variable nodes will refer back to the ones here,
        // rather than the ones in the definition, so we need to
        // record a map from any members defined here back to the
        // original definition. We could also see them for parent
        // classes, or for template param (a template param can refer
        // to an out-of-unit type either by using a ref_sig8 directly,
        // in which case we will have resolved the offset correctly,
        // or it could have an offset to a type with a
        // DW_AT_signature, in which case we'll need to fix it up
        // later). In any case, add an entry to map our offset to the
        // true definition, and entries to map any members to their
        // true definitions.
        env.local_mappings.emplace(offset, *definitionOffset);

        folly::F14FastMap<std::string, GlobalOff> map;
        for (const auto& child : dieContext.die.children()) {
          if (child.getTag() == llvm::dwarf::DW_TAG_member ||
              child.getTag() == llvm::dwarf::DW_TAG_variable) {
            auto name = dwarfContext.getDIEName(child);
            map.emplace(
              std::move(name),
              dwarfContext.getGlobalOffset(child.getOffset(), dieContext.isInfo)
            );
          }
        }
        if (!map.empty()) {
          const auto orig = dwarfContext.getDieContextAtGlobalOffset(*definitionOffset);
          for (const auto& child : orig.die.children()) {
            auto const name = dwarfContext.getDIEName(child);
            auto it = map.find(name);
            if (it != map.end()) {
              env.local_mappings.emplace(
                it->second,
                dwarfContext.getGlobalOffset(child.getOffset(), definitionOffset->isInfo())
              );
            }
          }
        }
      }

      auto parent_offset = scope.typeOffset();

      // If we inferred a base name, use that to form the fully qualified name,
      // otherwise treat it as an unnamed type.
      if (!definitionOffset) {
        std::get<1>(info) ?
          scope.pushUnnamedType(std::get<0>(info), offset) :
          scope.pushType(std::get<0>(info), offset);
      } else {
        // Push the name of the definition, not of the declaration
        const auto defContext = dwarfContext.getDieContextAtGlobalOffset(*definitionOffset);
        const auto info_def = get_info(defContext, /*updateOffsets=*/false);
        std::get<1>(info_def) ?
          scope.pushUnnamedType(std::get<0>(info_def), offset) :
          scope.pushType(std::get<0>(info_def), offset);
      }
      SCOPE_EXIT { scope.pop(); };

      if (declarationOffset) {
        // This completes a previous declaration. search backwards for
        // it, which should be fine because its normally right after
        // the declaration (and its always in the same cu/tu).
        auto i = state.all_objs.size();
        while (true) {
          assert(i);
          auto& obj = state.all_objs[--i];
          if (obj.key.object_id == declarationOffset->raw()) {
            assert(obj.incomplete);
            FTRACE(5,
                   "Completing previous definition of {}.\n"
                   "  Was {}, Now {}, Linkage: {}\n",
                   obj.name.name,
                   GlobalOff::fromRaw(obj.key.object_id), offset,
                   show(obj.name.linkage)
                  );
            obj.incomplete = false;
            obj.key.object_id = offset.raw();
            // map declarationOffset to offset, because any ref_sig8s
            // will point to the definition, not the declaration.
            env.local_mappings.emplace(*declarationOffset, offset);

            // Fixup the name in the scope stack
            scope.fixName(obj.name);
            assertx(scope.name().name == obj.name.name);
            break;
          }
        }
      } else {
        // Record this object type, with fully qualified name, key, and linkage.
        auto obj = ObjectType{
          scope.name(),
          ObjectTypeKey{offset.raw(), scope.cuOffset().raw()},
          std::get<2>(info)
        };
        FTRACE(5,
               "{} {} at {} Linkage: {}\n",
               obj.incomplete ? "Declaring" : "Defining",
               obj.name.name,
               offset,
               show(obj.name.linkage)
              );
        state.all_objs.emplace_back(std::move(obj));
      }

      // This object type is done, so recurse into any nested classes. Provide a
      // list of template parameters to be filled in case this is a template. If
      // it is, we'll record the linkage dependence for the later template
      // linkage fix-up.
      std::vector<GlobalOff> recurse_template_params;
      recurse(&recurse_template_params);

      for (auto param_offset : recurse_template_params) {
        FTRACE(9, "linkage: {} depends on template param {}\n",
               offset, param_offset);
        env.linkage_dependents[param_offset].template_uses.emplace(offset);
      }
      if (parent_offset) {
        FTRACE(9, "linkage: {} depends on child {}\n",
               *parent_offset, offset);
        env.linkage_dependents[*parent_offset].children.emplace(offset);
      }
      break;
    }
    case llvm::dwarf::DW_TAG_namespace: {
      // Record the namespace in the scope and recurse. If this is an unnamed
      // namespace, that means any type found in child DIEs will have internal
      // linkage.
      auto const name = dieContext.die.getShortName() ? std::string(dieContext.die.getShortName()) : "";
      name.empty() ?
        scope.pushUnnamedNamespace() :
        scope.pushNamespace(std::move(name));
      SCOPE_EXIT { scope.pop(); };
      recurse();
      break;
    }
    case llvm::dwarf::DW_TAG_variable: {
      // Normally we don't care about variables since we're only looking for
      // types. However, certain aspects of object types can't be completely
      // inferred at the declaration site (mainly static variable linkage
      // related things like linkage name and address). We need a definition for
      // that, so record all the variable definitions along with their
      // specification, which we can consult later.

      // Neither GCC nor Clang record a name for a variable which is a static
      // definition, so ignore any that do have a name. This speeds things up.
      const auto name = dwarfContext.getDIEName(dieContext.die);
      if (!name.empty()) break;

      StaticSpec spec;
      if (auto off = parseSpecification(dwarfContext, dieContext, true, spec)) {
        env.raw_static_definitions.emplace_back(*off, spec);
      }
      // Note that we don't recurse into any child DIEs here. There shouldn't be
      // anything interesting in them.
      break;
    }
    case llvm::dwarf::DW_TAG_subprogram: {
      // For the same reason we care about llvm::dwarf::DW_TAG_variables, we examine
      // llvm::dwarf::DW_TAG_subprogram as well. Certain interesting aspects of a static
      // function are only present in its definition.
      auto const name = dwarfContext.getDIEName(dieContext.die);
      if (!name.empty()) break;

      StaticSpec spec;
      if (auto off = parseSpecification(dwarfContext, dieContext, true, spec)) {
        env.raw_static_definitions.emplace_back(*off, spec);
      }

      // Don't recurse. There might be valid types within a subprogram
      // definition, but we deliberately ignore those. A large portion of the
      // debug information lies within subprogram definitions, and scanning all
      // of that consumes a large amount of time. Moreover, these types usually
      // aren't very interesting, so we deliberately ignore them for
      // efficiency. If there's actually any reference to these types, they'll
      // be reported as the pseudo-type.
      break;
    }
    case llvm::dwarf::DW_TAG_template_type_parameter: {
      // Template type parameters are represented using child DIEs, not
      // attributes. If the parent DIE was an object type, fill the supplied
      // vector with the template parameters. Don't recurse because there
      // shouldn't be anything interesting in the children.
      if (template_params) {
        for (const auto& attr : dieContext.die.attributes()) {
          if (attr.Attr == llvm::dwarf::DW_AT_type) {
            auto offset = dwarfContext.getGlobalOffset(
                getRefOffset(dieContext, attr), dieContext.isInfo);
            // Check this type to see if it is a declaration and use the
            // real type instead
            const auto typeDieContext = dwarfContext.getDieContextAtGlobalOffset(offset);
            for (const auto& typeAttr : typeDieContext.die.attributes()) {
              if (typeAttr.Attr == llvm::dwarf::DW_AT_signature &&
                  typeAttr.Value.getForm() == llvm::dwarf::DW_FORM_ref_sig8) {
                const auto sig8 = getRefOffset(dieContext, typeAttr);
                offset = dwarfContext.getTypeUnitOffset(sig8);
                break;
              }
            }
            template_params->emplace_back(offset);
            break;
          }
        }
      }
      break;
    }
    default:
      recurse();
      break;
  }
}

/*
 * Given the DIE representing an object type, its name, and its key, return the
 * detailed specification of the object.
 */
Object TypeParserImpl::genObject(const DieContext& dieContext,
                                 ObjectTypeName name,
                                 ObjectTypeKey key) {
  const auto& die = dieContext.die;
  const auto& isInfo = dieContext.isInfo;

  const auto kind = [&]{
    switch (die.getTag()) {
      case llvm::dwarf::DW_TAG_structure_type: return Object::Kind::k_class;
      case llvm::dwarf::DW_TAG_class_type: return Object::Kind::k_class;
      case llvm::dwarf::DW_TAG_union_type: return Object::Kind::k_union;
      case llvm::dwarf::DW_TAG_base_type: return Object::Kind::k_primitive;
      case llvm::dwarf::DW_TAG_enumeration_type: return Object::Kind::k_enum;
      // Strange things like "decltype(nullptr_t)"
      case llvm::dwarf::DW_TAG_unspecified_type: return Object::Kind::k_other;
      // Shouldn't happen because we only call genObject() on offsets already
      // visited and verified to be an object type.
      default: always_assert(0);
    }
  }();

  HPHP::Optional<std::size_t> size;
  bool incomplete = false;
  HPHP::Optional<GlobalOff> definition_offset;

  for (const auto& attr : die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_byte_size:
        size = attr.Value.getAsUnsignedConstant().value();
        break;
      case llvm::dwarf::DW_AT_declaration:
        incomplete = *attr.Value.getAsUnsignedConstant() != 0;
        break;
      case llvm::dwarf::DW_AT_signature: {
        const auto sig8 = getRefOffset(dieContext, attr);
        definition_offset = m_dwarfContext.getTypeUnitOffset(sig8);
        break;
      }
      default:
        break;
    }
  }

  if (definition_offset) {
    const auto die2Context = m_dwarfContext.getDieContextAtGlobalOffset(*definition_offset);
    return genObject(die2Context, name, key);
  }

  // No size was provided. This is expected for incomplete types or the strange
  // "other" types sometimes seen, but an error otherwise.
  if (!size) {
    if (incomplete || kind == Object::Kind::k_other) {
      size = 0;
    } else {
      throw Exception{
        fmt::format(
          "Object type '{}' at offset {} is a complete definition, "
          "but has no size!",
          name.name,
          key.object_id
        )
      };
    }
  }

  Object obj{std::move(name), *size, key, kind, incomplete};

  for (const auto& child : die.children()) {
    switch (child.getTag()) {
      case llvm::dwarf::DW_TAG_inheritance:
        obj.bases.emplace_back(genBase({child, isInfo}, obj.name));
        break;
      case llvm::dwarf::DW_TAG_member:
      case llvm::dwarf::DW_TAG_variable:
        obj.members.emplace_back(genMember({child, isInfo}, obj.name));
        if (obj.name.linkage != ObjectTypeName::Linkage::external) {
          // Clang gives linkage names to things that don't actually have
          // linkage. Don't let any members have linkage names if the object
          // type doesn't have external linkage.
          obj.members.back().linkage_name.clear();
        }
        break;
      case llvm::dwarf::DW_TAG_template_type_parameter:
        obj.template_params.emplace_back(genTemplateParam({child, isInfo}));
        break;
      case llvm::dwarf::DW_TAG_GNU_template_parameter_pack:
        // Flatten parameter packs as if they were just a normally provided
        // parameter list. This is enough for our purposes.
        for (const auto& template_die : child.children()) {
          if (template_die.getTag() ==
              llvm::dwarf::DW_TAG_template_type_parameter) {
            obj.template_params.emplace_back(
              genTemplateParam({template_die, isInfo})
            );
          }
        }
        break;
      case llvm::dwarf::DW_TAG_subprogram:
        obj.functions.emplace_back(genFunction({child, isInfo}));
        if (obj.name.linkage != ObjectTypeName::Linkage::external) {
          // Clang gives linkage names to things that don't actually have
          // linkage. Don't let any functions have linkage names if the object
          // type doesn't have external linkage.
          obj.functions.back().linkage_name.clear();
        }
        break;
      default:
        break;
    }
  }

  // The base classes and members aren't always reported in DWARF in offset
  // order, but make the output deterministic here to simplify consumers of the
  // information.
  std::sort(
    obj.bases.begin(),
    obj.bases.end(),
    [&](const Object::Base& b1, const Object::Base& b2) {
      return std::tie(b1.offset, b1.type.name.name) <
        std::tie(b2.offset, b2.type.name.name);
    }
  );

  std::sort(
    obj.members.begin(),
    obj.members.end(),
    [&](const Object::Member& m1, const Object::Member& m2) {
      return std::tie(m1.offset, m1.name) <
        std::tie(m2.offset, m2.name);
    }
  );

  return obj;
}

/*
 * Given a DIE representing an arbitrary type, return its equivalent Type. This
 * can involve chasing a chain of such type DIEs.
 */
Type TypeParserImpl::genType(const DieContext& dieContext) {
  // Offset of a different type this type refers to. If not present, that type
  // is implicitly "void".
  HPHP::Optional<GlobalOff> type_offset;
  // For pointers to members, the type referring to the object the member
  // belongs to.
  HPHP::Optional<GlobalOff> containing_type_offset;

  // A struct can have a declaration which refers to the definition
  // via a DW_AT_signature.
  HPHP::Optional<GlobalOff> definition_offset;

  const auto& die = dieContext.die;
  const auto isInfo = dieContext.isInfo;

  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_type:
        type_offset = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), isInfo);
        break;
      case llvm::dwarf::DW_AT_containing_type:
        containing_type_offset = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), isInfo);
        break;
      case llvm::dwarf::DW_AT_signature: {
        const auto sig8 = getRefOffset(dieContext, attr);
        definition_offset = m_dwarfContext.getTypeUnitOffset(sig8);
        break;
      }
      default:
        break;
    }
  }

  const auto recurse = [&](GlobalOff offset) {
    const auto die2Context = m_dwarfContext.getDieContextAtGlobalOffset(offset);
    return genType(die2Context);
  };

  // Pointers to member functions aren't represented in DWARF. Instead the
  // compiler creates a struct internally which stores all the information.

  switch (die.getTag()) {
    case llvm::dwarf::DW_TAG_base_type:
    case llvm::dwarf::DW_TAG_structure_type:
    case llvm::dwarf::DW_TAG_class_type:
    case llvm::dwarf::DW_TAG_union_type:
    case llvm::dwarf::DW_TAG_enumeration_type:
    case llvm::dwarf::DW_TAG_unspecified_type: {
      if (definition_offset) return recurse(*definition_offset);
      auto offset = m_dwarfContext.getGlobalOffset(die.getOffset(), isInfo);
      auto const& state = stateForOffset(offset);
      auto iter = state.obj_offsets.find(offset);
      if (iter == state.obj_offsets.end()) {
        // Must be the pseudo-type.
        return ObjectType{
          ObjectTypeName{
            Scope::s_pseudo_type_name,
            ObjectTypeName::Linkage::pseudo
          },
          ObjectTypeKey{offset.raw(), 0},
          true
        };
      } else {
        return state.all_objs[iter->second];
      }
    }
    case llvm::dwarf::DW_TAG_pointer_type:
      return PtrType{type_offset ? recurse(*type_offset) : VoidType{}};
    case llvm::dwarf::DW_TAG_reference_type: {
      if (!type_offset) {
        throw Exception{
          fmt::format(
            "Encountered reference to void at offset {}",
            die.getOffset()
          )
        };
      }
      return RefType{recurse(*type_offset)};
    }
    case llvm::dwarf::DW_TAG_rvalue_reference_type: {
      if (!type_offset) {
        throw Exception{
          fmt::format(
            "Encountered rvalue reference to void at offset {}",
            die.getOffset()
          )
        };
      }
      return RValueRefType{recurse(*type_offset)};
    }
    case llvm::dwarf::DW_TAG_array_type: {
      if (!type_offset) {
        throw Exception{
          fmt::format(
            "Encountered array of voids at offset {}",
            die.getOffset()
          )
        };
      }
      return ArrType{recurse(*type_offset), determineArrayBound(die)};
    }
    case llvm::dwarf::DW_TAG_const_type:
      return ConstType{type_offset ? recurse(*type_offset) : VoidType{}};
    case llvm::dwarf::DW_TAG_volatile_type:
      return VolatileType{type_offset ? recurse(*type_offset) : VoidType{}};
    case llvm::dwarf::DW_TAG_restrict_type:
      return RestrictType{type_offset ? recurse(*type_offset) : VoidType{}};
    case llvm::dwarf::DW_TAG_typedef:
      return type_offset ? recurse(*type_offset) : VoidType{};
    case llvm::dwarf::DW_TAG_subroutine_type: {
      FuncType func{type_offset ? recurse(*type_offset) : VoidType{}};
      fillFuncArgs({die, isInfo}, func);
      return std::move(func);
    }
    case llvm::dwarf::DW_TAG_ptr_to_member_type: {
      if (!containing_type_offset) {
        throw Exception{
          fmt::format(
            "Encountered ptr-to-member at offset {} without a "
            "containing object",
            die.getOffset()
          )
        };
      }

      auto containing = recurse(*containing_type_offset);
      if (auto obj = containing.asObject()) {
        return PtrType{
          MemberType{std::move(*obj), recurse(*type_offset)}
        };
      } else {
        throw Exception{
          fmt::format(
            "Encountered ptr-to-member at offset {} with a "
            "containing object of type '{}'",
            die.getOffset(),
            containing.toString()
          )
        };
      }
    }
    default:
      throw Exception{
        fmt::format(
          "Encountered non-type tag '{}' at offset {} while "
          "traversing type description",
          fmt::underlying(die.getTag()),
          die.getOffset()
        )
      };
  }
}

Object::Member TypeParserImpl::genMember(const DieContext& dieContext,
                                         const ObjectTypeName& parent_name) {
  std::string name;
  std::string linkage_name;
  std::size_t offset = 0;
  HPHP::Optional<GlobalOff> die_offset;
  HPHP::Optional<uintptr_t> address;
  bool is_static = false;

  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_name:
        if (auto val = llvm::dwarf::toString(attr.Value)) {
          name = *val;
        }
        break;
      case llvm::dwarf::DW_AT_linkage_name:
        if (auto val = llvm::dwarf::toString(attr.Value)) {
          linkage_name = *val;
        }
        break;
      case llvm::dwarf::DW_AT_location:
        address = interpretLocAddress(m_dwarfContext, dieContext, attr);
        break;
      case llvm::dwarf::DW_AT_data_member_location:
        offset = *attr.Value.getAsUnsignedConstant();
        break;
      case llvm::dwarf::DW_AT_type:
        die_offset = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), dieContext.isInfo);
        break;
      case llvm::dwarf::DW_AT_declaration:
        is_static = *attr.Value.getAsUnsignedConstant() != 0;
        break;
      default:
        break;
    }
  }

  if (!die_offset) {
    // No DW_AT_type means "void", but you can't have void members!
    throw Exception{
      fmt::format(
        "Encountered member (name: '{}') of type void "
        "in object type '{}' at offset {}",
        name,
        parent_name.name,
        dieContext.die.getOffset()
      )
    };
  }

  if (is_static) {
    // If this is a static member, look up any definitions which refer to this
    // member, and pull any additional information out of it.
    // auto const static_offset = m_dwarf.getDIEOffset(die);
    auto const static_offset = m_dwarfContext.getGlobalOffset(dieContext.die.getOffset(), dieContext.isInfo);
    auto const& state = stateForOffset(static_offset);
    auto const range = state.static_definitions.equal_range(static_offset);

    for (auto const& elm : range) {
      if (linkage_name.empty() && !elm.second.linkage_name.empty()) {
        linkage_name = elm.second.linkage_name;
      }
      if (!address && elm.second.address != StaticSpec::kNoAddress) {
        address = elm.second.address;
      }
    }
  }

  const auto die2Context = m_dwarfContext.getDieContextAtGlobalOffset(*die_offset);
  auto type = genType(die2Context);

  if (name.empty()) {
    name = is_static
      ? fmt::format("(unnamed static member of type '{}')", type.toString())
      : fmt::format("(unnamed member of type '{}')", type.toString());
  }

  return Object::Member{
    name,
    is_static ? std::nullopt : HPHP::Optional<std::size_t>{offset},
    linkage_name,
    address,
    std::move(type)
  };
}

Object::Function TypeParserImpl::genFunction(const DieContext& dieContext) {
  std::string name;
  Type ret_type{VoidType{}};
  std::string linkage_name;
  bool is_virtual = false;
  bool is_member = false;

  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_name:
        if (auto val = llvm::dwarf::toString(attr.Value)) {
          name = *val;
        }
        break;
      case llvm::dwarf::DW_AT_type: {
        const auto globalOff = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), dieContext.isInfo);
        const auto tyDieContext = m_dwarfContext.getDieContextAtGlobalOffset(globalOff);
        ret_type = genType(tyDieContext);
        break;
      }
      case llvm::dwarf::DW_AT_linkage_name:
        if (auto val = llvm::dwarf::toString(attr.Value)) {
          linkage_name = *val;
        }
        break;
      case llvm::dwarf::DW_AT_virtuality:
        is_virtual = *attr.Value.getAsUnsignedConstant() != llvm::dwarf::DW_VIRTUALITY_none;
        break;
      case llvm::dwarf::DW_AT_object_pointer:
        is_member = true;
        break;
      default:
        break;
    }
  }

  /*
   * We need to determine if this function is a static function or a member
   * function. The straight-forward way is to look for the DW_AT_object_pointer
   * attribute (which is only present for member functions). This works fine for
   * GCC, but not Clang.
   *
   * On Clang, the DW_AT_object_pointer is only present in a function's
   * definition, not its declaration. Moreover, it doesn't reliably emit
   * function declarations if it thinks the function isn't used. As a result, we
   * can't reliably distinguish member functions from static functions on clang.
   *
   * As an alternative, if the first formal parameter of a function is marked as
   * being "artificial" (which means its not present in the actual source),
   * assume its actually the this pointer, and that the function is a member
   * function.
   */
  std::vector<Type> arg_types;
  for (const auto& child : dieContext.die.children()) {
    if (child.getTag() != llvm::dwarf::DW_TAG_formal_parameter) {
      continue;
    }

    bool is_artificial = false;
    Type arg_type{VoidType()};

    for (const auto& attr : child.attributes()) {
      switch (attr.Attr) {
        case llvm::dwarf::DW_AT_type: {
          const auto globalOff = m_dwarfContext.getGlobalOffset(
              getRefOffset(dieContext, attr), dieContext.isInfo);
          const auto ty_die = m_dwarfContext.getDieContextAtGlobalOffset(globalOff);
          arg_type = genType(ty_die);
          break;
        }
        case llvm::dwarf::DW_AT_artificial:
          is_artificial = *attr.Value.getAsUnsignedConstant() != 0;
          break;
        default:
          break;
      }
    }

    // Only consider this a member function if this arg if the first and its
    // artificial.
    if (is_artificial && arg_types.empty()) {
      is_member = true;
    }
    arg_types.emplace_back(std::move(arg_type));
  }

  HPHP::Optional<std::uintptr_t> address;

  // Similar to static variables, find any definitions which refer to this
  // function in order to extract linkage information.
  auto const offset = m_dwarfContext.getGlobalOffset(dieContext.die.getOffset(), dieContext.isInfo);
  auto const& state = stateForOffset(offset);
  auto range = state.static_definitions.equal_range(offset);
  for (auto const& elm : range) {
    if (linkage_name.empty() && !elm.second.linkage_name.empty()) {
      linkage_name = elm.second.linkage_name;
    }
    if (!address && elm.second.address != StaticSpec::kNoAddress) {
      address = elm.second.address;
    }
    if (elm.second.is_member) is_member = true;
  }

  return Object::Function{
    name,
    std::move(ret_type),
    std::move(arg_types),
    is_virtual ?
      Object::Function::Kind::k_virtual :
      (is_member ? Object::Function::Kind::k_member :
       Object::Function::Kind::k_static),
    linkage_name,
    address,
  };
}

Object::Base TypeParserImpl::genBase(const DieContext& dieContext,
                                     const ObjectTypeName& parent_name) {
  std::string name;
  HPHP::Optional<std::size_t> offset;
  HPHP::Optional<GlobalOff> die_offset;
  bool is_virtual = false;

  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_name:
        if (auto val = llvm::dwarf::toString(attr.Value)) {
          name = *val;
        }
        break;
      case llvm::dwarf::DW_AT_type:
        die_offset = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), dieContext.isInfo);
        break;
      case llvm::dwarf::DW_AT_virtuality:
        is_virtual = *attr.Value.getAsUnsignedConstant() != llvm::dwarf::DW_VIRTUALITY_none;
        break;
      default:
        break;
    }
  }

  if (!is_virtual) {
    offset = 0;

    for (const auto& attr : dieContext.die.attributes()) {
      switch (attr.Attr) {
        case llvm::dwarf::DW_AT_data_member_location:
          offset = *attr.Value.getAsUnsignedConstant();
          break;
        default:
          break;
      }
    }
  }

  if (!die_offset) {
    throw Exception{
      fmt::format(
        "Encountered base '{}' of object type '{}' without "
        "type information at offset {}",
        name,
        parent_name.name,
        dieContext.die.getOffset()
      )
    };
  }

  const auto die2Context = m_dwarfContext.getDieContextAtGlobalOffset(*die_offset);
  auto type = genType(die2Context);

  if (auto obj = type.asObject()) {
    // Base class better be an actual class!
    return Object::Base{*obj, offset};
  } else {
    throw Exception{
      fmt::format(
        "Encountered base '{}' of object type '{}' of "
        "non-object type '{}' at offset {}",
        name,
        parent_name.name,
        type.toString(),
        dieContext.die.getOffset()
      )
    };
  }
}

Object::TemplateParam TypeParserImpl::genTemplateParam(const DieContext& dieContext) {
  HPHP::Optional<GlobalOff> die_offset;

  for (const auto& attr : dieContext.die.attributes()) {
    switch (attr.Attr) {
      case llvm::dwarf::DW_AT_type: {
        die_offset = m_dwarfContext.getGlobalOffset(
            getRefOffset(dieContext, attr), dieContext.isInfo);
        break;
      }
      default:
        break;
    }
  }

  return Object::TemplateParam{
    die_offset ?
      genType(m_dwarfContext.getDieContextAtGlobalOffset(*die_offset)) :
      VoidType{}
  };
}

HPHP::Optional<std::size_t>
TypeParserImpl::determineArrayBound(const llvm::DWARFDie& die) {
  HPHP::Optional<std::size_t> bound;

  for (const auto& child : die.children()) {
    switch (child.getTag()) {
      case llvm::dwarf::DW_TAG_subrange_type: {
        for (const auto& attr : child.attributes()) {
              switch (attr.Attr) {
                case llvm::dwarf::DW_AT_count:
                  bound = attr.Value.getRawUValue();
                  break;
                case llvm::dwarf::DW_AT_upper_bound:
                  bound = attr.Value.getRawUValue() + 1;
                  break;
                default:
                  break;
              }
        }
        break;
      }
      default:
        break;
    }
  }

  if (bound && !*bound) bound.reset();
  return bound;
}

void TypeParserImpl::fillFuncArgs(const DieContext& dieContext, FuncType& func) {
  for (const auto& child : dieContext.die.children()) {
    switch (child.getTag()) {
      case llvm::dwarf::DW_TAG_formal_parameter: {
        HPHP::Optional<GlobalOff> type_offset;

        for (const auto& attr : child.attributes()) {
          switch (attr.Attr) {
            case llvm::dwarf::DW_AT_type:
              type_offset = m_dwarfContext.getGlobalOffset(
                  getRefOffset(dieContext, attr), dieContext.isInfo);
              break;
            default:
              break;
          }
        }

        if (!type_offset) {
          throw Exception{
            fmt::format(
              "Encountered function at offset {} taking a void parameter",
              dieContext.die.getOffset()
            )
          };
        }

        const auto typeDieContext = m_dwarfContext.getDieContextAtGlobalOffset(*type_offset);
        func.args.push_back(genType(typeDieContext));
        break;
      }
      default:
        break;
    }
  }
}

struct PrinterImpl : Printer {
  explicit PrinterImpl(const std::string &filename)
      : m_filename{filename}, m_dwarfContext{filename} {}

  void operator()(std::ostream &os, std::size_t begin = 0,
                  std::size_t end = std::numeric_limits<std::size_t>::max(),
                  bool dwp = false) const override {

    auto printUnits = [&](auto &&unitFunction) {
      // If a non-default begin parameter was specified, first iterate over all
      // the compilation units. Find the first compilation unit which at least
      // partially lies within the range given by the begin parameter. This is
      // the first compilation unit to begin printing from.
      HPHP::Optional<uint64_t> last;
      unitFunction([&](const std::unique_ptr<llvm::DWARFUnit> &unit) {
        const auto offset = unit->getOffset();
        if (offset > end)
          return false;
        if (offset <= begin)
          last = offset;
        return true;
      });

      // Now iterate over all the compilation units again. Only actually print
      // out dies that lie within the last/end parameter range.
      unitFunction([&](const std::unique_ptr<llvm::DWARFUnit> &unit) {
        const auto offset = unit->getOffset();
        if (offset > end)
          return false;
        if (!last || offset >= *last) {
          unit->dump(llvm::outs(), llvm::DIDumpOptions{});
          llvm::DWARFDie unitDie =
              unit->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
          for (auto const &die : unitDie.children()) {
            printDie(os, die, last ? *last : begin, end, 1);
          }
        }
        return true;
      });
    };

    if (!dwp) {
      printUnits([&](auto &&func) { m_dwarfContext.forEachNormalUnit(func); });
    } else {
      printUnits([&](auto &&func) { m_dwarfContext.forEachDwoUnit(func); });
    }
  }

private:
  /*
   * Print out the given DIE (including children) in textual format to the given
   * ostream. Only actually print out DIEs which begin in the range between the
   * begin and end parameters.
   */
  void printDie(std::ostream &os, const llvm::DWARFDie &dwarfDie, size_t begin,
                size_t end, unsigned level = 0) const {
    if (dwarfDie.getOffset() >= begin && dwarfDie.getOffset() <= end) {
      dwarfDie.dump(llvm::outs(), level);
    }

    for (auto const &child : dwarfDie.children()) {
      printDie(os, child, begin, end, level + 1);
    }
  }

  std::string m_filename;
  DWARFContextManager m_dwarfContext;
};

////////////////////////////////////////////////////////////////////////////////

}

std::unique_ptr<TypeParser>
make_dwarf_type_parser(const std::string& filename, int num_threads) {
  // Support calling with main binary or dwp, but process main binary since
  // TypeParser handles dwp automatically
  auto main_binary = filename;
  if (main_binary.substr(main_binary.size() - 4) == ".dwp") {
    main_binary = main_binary.substr(0, main_binary.size() - 4);
  }
  return std::make_unique<TypeParserImpl>(main_binary, num_threads);
}

std::unique_ptr<Printer> make_dwarf_printer(const std::string& filename) {
  return std::make_unique<PrinterImpl>(filename);
}

////////////////////////////////////////////////////////////////////////////////

}

#endif

namespace folly {
template<> class FormatValue<llvm::dwarf::Tag> {
 public:
  explicit FormatValue(llvm::dwarf::Tag tag) : m_tag(tag) {}

  template<typename Callback>
  void format(FormatArg& arg, Callback& cb) const {
    format_value::formatString(fmt::format("{}", fmt::underlying(m_tag)), arg, cb);
  }

 private:
  llvm::dwarf::Tag m_tag;
};
}
