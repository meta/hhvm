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
#include "hphp/hhbbc/hhbbc.h"

#include <vector>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <numeric>
#include <ranges>
#include <set>

#include <folly/AtomicLinkedList.h>
#include <fmt/core.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>

#ifdef HHVM_FACEBOOK
#include <strobelight/strobemeta/strobemeta_frames.h>
#else
#define SET_FRAME_METADATA(...)
#endif

#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/vm/constant.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/hhbbc/analyze.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/debug.h"
#include "hphp/hhbbc/emit.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/optimize.h"
#include "hphp/hhbbc/options.h"
#include "hphp/hhbbc/options-util.h"
#include "hphp/hhbbc/parallel.h"
#include "hphp/hhbbc/parse.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-system.h"
#include "hphp/hhbbc/wide-func.h"

#include "hphp/util/configs/repo.h"
#include "hphp/util/extern-worker.h"
#include "hphp/util/logger.h"
#include "hphp/util/struct-log.h"

namespace HPHP {

using namespace extern_worker;
namespace coro = folly::coro;

namespace HHBBC {

TRACE_SET_MOD(hhbbc)

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_invoke("__invoke");

//////////////////////////////////////////////////////////////////////

enum class LocalAnalysisMode { Constants, Full };
enum class WorkType { Class, Func };

struct WorkItem {
  explicit WorkItem(WorkType type, Context ctx)
    : type(type)
    , ctx(ctx)
  {}

  WorkType type;
  Context ctx;
};

struct WorkResult {
  explicit WorkResult(ClassAnalysis cls)
    : type(WorkType::Class)
    , cls(std::move(cls))
  {}

  explicit WorkResult(FuncAnalysisResult func)
    : type(WorkType::Func)
    , func(std::move(func))
  {}

  WorkResult(WorkResult&& wr) noexcept
    : type(wr.type)
  {
    switch (type) {
    case WorkType::Class:
      new (&cls) ClassAnalysis(std::move(wr.cls));
      break;
    case WorkType::Func:
      new (&func) FuncAnalysisResult(std::move(wr.func));
      break;
    }
  }

  WorkResult& operator=(WorkResult&& o) noexcept {
    this->~WorkResult();
    switch (o.type) {
    case WorkType::Class: new (this) WorkResult(std::move(o.cls)); break;
    case WorkType::Func:  new (this) WorkResult(std::move(o.func)); break;
    }
    return *this;
  }

  ~WorkResult() {
    switch (type) {
    case WorkType::Class:
      cls.~ClassAnalysis();
      break;
    case WorkType::Func:
      func.~FuncAnalysisResult();
      break;
    }
  }

  WorkType type;
  union {
    ClassAnalysis cls;
    FuncAnalysisResult func;
  };
};

//////////////////////////////////////////////////////////////////////

std::vector<Context> all_unit_contexts(const Index& index,
                                       const php::Unit& u) {
  std::vector<Context> ret;
  index.for_each_unit_class(
    u,
    [&] (const php::Class& c) {
      for (auto const& m : c.methods) {
        ret.emplace_back(Context { u.filename, m.get(), &c });
      }
    }
  );
  index.for_each_unit_func(
    u,
    [&] (const php::Func& f) {
      // Folded constant initializers are neither optimized nor emitted.
      if (Constant::nameFromFuncName(f.name) &&
          !index.constant_init_needed(f)) {
        return;
      }
      ret.emplace_back(Context { u.filename, &f });
    }
  );
  return ret;
}

bool is_constant_work(const php::Func& func) {
  if (is_86init_func(func)) return true;
  auto const decl = func.cls ? func.cls->closureDeclFunc : func.name;
  return decl && Constant::nameFromFuncName(decl);
}

std::vector<Context> constant_work_contexts(const Index& index) {
  std::vector<Context> ret;
  for (auto const& cls : index.program().classes) {
    for (auto const& method : cls->methods) {
      if (!is_constant_work(*method)) continue;
      ret.emplace_back(Context { cls->unit, method.get(), cls.get() });
    }
  }
  for (auto const& func : index.program().funcs) {
    if (!is_constant_work(*func)) continue;
    ret.emplace_back(Context { func->unit, func.get() });
  }
  return ret;
}

std::vector<WorkItem> initial_work(const Index& index, LocalAnalysisMode mode) {
  std::vector<WorkItem> ret;

  if (mode == LocalAnalysisMode::Constants) {
    for (auto const ctx : constant_work_contexts(index)) {
      ret.emplace_back(WorkType::Func, ctx);
    }
    return ret;
  }

  auto const& program = index.program();
  for (auto const& c : program.classes) {
    assertx(!c->closureContextCls);
    if (is_used_trait(*c)) {
      for (auto const& f : c->methods) {
        ret.emplace_back(
          WorkType::Func,
          Context { c->unit, f.get(), f->cls }
        );
      }
    } else {
      ret.emplace_back(
        WorkType::Class,
        Context { c->unit, nullptr, c.get() }
      );
    }
  }
  for (auto const& f : program.funcs) {
    // Folded constant initializers have no executable work left.
    if (Constant::nameFromFuncName(f->name) &&
        !index.constant_init_needed(*f)) {
      continue;
    }
    ret.emplace_back(
      WorkType::Func,
      Context { f->unit, f.get() }
    );
  }
  return ret;
}

WorkItem work_item_for(const DependencyContext& d,
                       LocalAnalysisMode mode,
                       const Index& index) {
  switch (d.tag()) {
    case DependencyContextType::Class: {
      auto const cls = (const php::Class*)d.ptr();
      assertx(mode == LocalAnalysisMode::Full && !is_used_trait(*cls));
      return WorkItem {
        WorkType::Class,
        Context { cls->unit, nullptr, cls }
      };
    }
    case DependencyContextType::Func: {
      auto const func = (const php::Func*)d.ptr();
      auto const cls = func->cls
        ? index.lookup_closure_context(*func->cls)
        : nullptr;
      assertx(!cls ||
              mode == LocalAnalysisMode::Constants ||
              is_used_trait(*cls));
      return WorkItem {
        WorkType::Func,
        Context { func->unit, func, cls }
      };
    }
    case DependencyContextType::Prop:
    case DependencyContextType::FuncFamily:
      // We only record dependencies on these. We don't schedule any
      // work on their behalf.
      break;
  }
  always_assert(false);
}

/*
 * Algorithm:
 *
 * Start by running an analyze pass on every class or free function.
 * During analysis, information about functions or classes will be
 * requested from the Index, which initially won't really know much,
 * but will record a dependency.  This part is done in parallel: no
 * passes are mutating anything, just reading from the Index.
 *
 * After a pass, we do a single-threaded "update" step to prepare
 * for the next pass: for each function or class that was analyzed,
 * note the facts we learned that may aid analyzing other functions
 * in the program, and register them in the index.
 *
 * If any of these facts are more useful than they used to be, add
 * all the Contexts that had a dependency on the new information to
 * the work list again, in case they can do better based on the new
 * fact.  (This only applies to function analysis information right
 * now.)
 *
 * Repeat until the work list is empty.
 *
 */
void analyze_iteratively(Index& index, LocalAnalysisMode mode) {
  trace_time tracer(
    mode == LocalAnalysisMode::Constants
      ? "analyze constants fixed point"
      : "analyze iteratively",
    index.sample()
  );

  // Counters, just for debug printing.
  std::atomic<uint32_t> total_funcs{0};
  std::atomic<uint32_t> total_classes{0};
  auto round = uint32_t{0};

  SCOPE_EXIT {
    if (Trace::moduleEnabledRelease(Trace::hhbbc_time, 1)) {
      Trace::traceRelease("total class visits %u\n", total_classes.load());
      Trace::traceRelease("total function visits %u\n", total_funcs.load());
    }
  };

  std::vector<DependencyContextSet> deps_vec{parallel::num_threads};
  auto work = initial_work(index, mode);
  while (!work.empty()) {
    auto results = [&] {
      trace_time trace(
        "analyzing",
        fmt::format("round {} -- {} work items", round, work.size())
      );
      return parallel::map(
        work,
        // We have a Optional just to keep the result type
        // DefaultConstructible.
        [&] (const WorkItem& wi) -> Optional<WorkResult> {
          SET_FRAME_METADATA(wi.ctx.unit->toCppString());
          switch (wi.type) {
          case WorkType::Func: {
            ++total_funcs;
            auto const wf = php::WideFunc::cns(wi.ctx.func);
            auto const ctx = AnalysisContext { wi.ctx.unit, wf, wi.ctx.cls };
            IndexAdaptor adaptor{ index };
            return WorkResult { analyze_func(adaptor, ctx, CollectionOpts{}) };
          }
          case WorkType::Class: {
            ++total_classes;
            IndexAdaptor adaptor{ index };
            return WorkResult { analyze_class(adaptor, wi.ctx) };
          }
          }
          not_reached();
        }
      );
    }();

    ++round;
    trace_time update_time("updating");

    auto const update_func = [&] (FuncAnalysisResult& fa,
                                  DependencyContextSet& deps) {
      SCOPE_ASSERT_DETAIL("update_func") {
        return "Updating Func: " + show(fa.ctx);
      };
      // This const_cast is safe since no two threads update the same Func.
      auto func = php::WideFunc::mut(const_cast<php::Func*>(fa.ctx.func));
      index.refine_return_info(fa, deps);
      index.refine_constants(fa, deps);
      auto const bytecodeUpdate =
        update_bytecode(func, std::move(fa.blockUpdates));

      if (mode == LocalAnalysisMode::Full) {
        index.record_public_static_mutations(
          *func,
          std::move(fa.publicSPropMutations)
        );
      }

      // Keep changed initializer bytecode in the constants fixed point.
      if (mode == LocalAnalysisMode::Constants &&
          bytecodeUpdate != UpdateBCResult::None &&
          (bytecodeUpdate == UpdateBCResult::ChangedAnalyze ||
           func->name == s_86cinit.get())) {
        deps.insert(index.dependency_context(fa.ctx));
      }

      if (auto const l = fa.resolvedInitializers.left()) {
        index.refine_class_constants(fa.ctx, *l, deps);
      } else if (auto const r = fa.resolvedInitializers.right()) {
        index.update_prop_initial_values(fa.ctx, *r, deps);
      }

      for (auto const& [cls, vars] : fa.closureUseTypes) {
        assertx(is_closure(*cls));
        if (index.refine_closure_use_vars(cls, vars)) {
          auto const func = find_method(cls, s_invoke.get());
          always_assert_flog(
            func != nullptr,
            "Failed to find __invoke on {} during index update\n",
            cls->name
          );
          auto const ctx =
            Context { func->unit, func, cls };
          deps.insert(index.dependency_context(ctx));
        }
      }
    };

    auto const update_class = [&] (ClassAnalysis& ca,
                                   DependencyContextSet& deps) {
      {
        SCOPE_ASSERT_DETAIL("update_class") {
          return "Updating Class: " + show(ca.ctx);
        };
        index.refine_private_props(ca.ctx.cls,
                                   ca.privateProperties);
        index.refine_private_statics(ca.ctx.cls,
                                     ca.privateStatics);
        index.update_prop_initial_values(ca.ctx, ca.resolvedProps, deps);
      }
      for (auto& fa : ca.methods)  update_func(fa, deps);
      for (auto& fa : ca.closures) update_func(fa, deps);
    };

    parallel::for_each(
      results,
      [&] (auto& result, size_t worker) {
        assertx(worker < deps_vec.size());
        switch (result->type) {
          case WorkType::Func:
            update_func(result->func, deps_vec[worker]);
            break;
          case WorkType::Class:
            update_class(result->cls, deps_vec[worker]);
            break;
        }
        result.reset();
      }
    );

    {
      trace_time _("merging deps");
      for (auto& deps : deps_vec) {
        if (&deps == &deps_vec[0]) continue;
        for (auto& d : deps) deps_vec[0].insert(d);
        deps.clear();
      }
    }

    auto& deps = deps_vec[0];

    if (mode == LocalAnalysisMode::Full) {
      index.refine_public_statics(deps);
    }

    work.clear();
    work.reserve(deps.size());
    for (auto& d : deps) {
      auto item = work_item_for(d, mode, index);
      // Keep the preliminary pass scoped to initializer work.
      if (mode == LocalAnalysisMode::Constants &&
          (item.type != WorkType::Func ||
           !is_constant_work(*item.ctx.func))) {
        continue;
      }
      work.emplace_back(std::move(item));
    }
    deps.clear();
  }
}

/*
 * Return unit indices in emission order. Hints take priority, followed by
 * unlisted units ordered by filename. Empty if no ordering file is configured.
 */
std::vector<uint32_t> unit_emit_order(
    const std::vector<std::unique_ptr<php::Unit>>& units) {
  if (Cfg::Repo::UnitOrderFile.empty()) return {};
  size_t numHints = 0;
  SStringToOneT<uint32_t> ranks;
  std::ifstream input{Cfg::Repo::UnitOrderFile};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    always_assert(numHints <= UINT32_MAX);
    auto const rank = uint32_t(numHints++);
    if (auto const s = lookupStaticString(line)) ranks.emplace(s, rank);
  }

  size_t matched = 0;
  for (auto const& unit : units) {
    matched += ranks.count(unit->filename);
  }
  Logger::FInfo("Unit ordering: {} hints in '{}', {} of {} units matched",
                numHints, Cfg::Repo::UnitOrderFile, matched, units.size());

  always_assert(units.size() <= (uint64_t{1} << 32));
  std::vector<uint32_t> order(units.size());
  std::iota(order.begin(), order.end(), uint32_t{0});
  auto const rank = [&] (uint32_t i) {
    auto const it = ranks.find(units[i]->filename);
    return it != ranks.end() ? it->second : numHints;
  };
  std::sort(
    order.begin(), order.end(),
    [&] (uint32_t a, uint32_t b) {
      auto const rankA = rank(a);
      auto const rankB = rank(b);
      if (rankA != rankB) return rankA < rankB;
      return string_data_lt{}(units[a]->filename, units[b]->filename);
    }
  );
  return order;
}

/*
 * Finally, use the results of all these iterations to perform
 * optimization.  This reanalyzes every function using our
 * now-very-updated Index, and then runs optimize_func with the
 * results.
 *
 * We do this in parallel: all the shared information is queried out
 * of the index, and each thread is allowed to modify the bytecode
 * for the function it is looking at.
 *
 * NOTE: currently they can't modify anything other than the
 * bytecode/Blocks, because other threads may be doing unlocked
 * queries to php::Func and php::Class structures.
 */
template<typename F>
void final_pass(Index& index, F emitUnit) {
  trace_time _{"final pass", index.sample()};
  _.ignore_client_stats();
  index.freeze();
  auto const& units = index.program().units;
  auto const order = unit_emit_order(units);
  IndexAdaptor adaptor{ index };
  auto const dump_dir = debug_dump_to();
  parallel::for_each(
    std::views::iota(size_t{0}, units.size()),
    [&] (size_t pos) {
      auto const& unit = units[order.empty() ? pos : order[pos]];
      SET_FRAME_METADATA(unit->filename->toCppString());
      // optimize_func can remove 86*init methods from classes, so we
      // have to save the contexts for now.
      for (auto const& context : all_unit_contexts(index, *unit)) {
        // This const_cast is safe since no two threads update the same Func.
        auto func = php::WideFunc::mut(const_cast<php::Func*>(context.func));
        auto const ctx = AnalysisContext { context.unit, func, context.cls };
        optimize_func(adaptor, analyze_func(adaptor, ctx, CollectionOpts{}), func);
      }
      state_after("optimize", *unit, adaptor);
      if (!dump_dir.empty()) {
        if (Trace::moduleEnabledRelease(Trace::hhbbc_dump, 2)) {
          auto const d = dump_representation(adaptor, *unit);
          if (!d.empty()) {
            write_representation_dump(
              dump_dir,
              unit->filename->toCppString(),
              d
            );
          }
        }
        auto const d = dump_index(adaptor, *unit);
        if (!d.empty()) {
          write_index_dump(
            dump_dir,
            unit->filename->toCppString(),
            d
          );
        }
      }
      emitUnit(*unit, order.empty() ? -1 : int64_t(pos));
    }
  );
}

//////////////////////////////////////////////////////////////////////

// Extern-worker job to run full analysis

struct AnalyzeJob {
  static std::string name() { return "hhbbc-analyze"; }
  static void init(const Config& config) {
    process_init(config.o, config.gd, true);
    AnalysisIndex::start();
  }
  static void fini() { AnalysisIndex::stop(); }

  template<typename T> using V = Variadic<T>;

  using Output = AnalysisIndex::Output;

  static Output run(V<AnalysisIndexBundle> reportBundles,
                    V<AnalysisIndexBundle> noReportBundles,
                    AnalysisInput::Meta meta) {
    // AnalysisIndex ctor will initialize the worklist appropriately.
    AnalysisWorklist worklist;
    AnalysisIndex index{
      worklist,
      std::move(reportBundles.vals),
      std::move(noReportBundles.vals),
      std::move(meta),
      AnalysisMode::Full
    };

    // Keep processing work until we reach a fixed-point (nothing new
    // gets put on the worklist).
    while (process(index, worklist)) {}
    // Freeze the index. Nothing is allowed to update the index after
    // this. This will also re-load the worklist with all of the
    // classes which will end up in the output.
    index.freeze();
    // Now do a pass through the original work items again. Now that
    // the index is frozen, we'll gather up any dependencies from the
    // analysis. Since we already reached a fixed point, this should
    // not cause any updates (and if it does, we'll assert).
    while (process(index, worklist)) {}
    // Everything is analyzed and dependencies are recorded. Turn the
    // index data into AnalysisIndex::Output and return it from this
    // job.
    return index.finish();
  }

private:
  // Analyze the work item at the front of the worklist (returning
  // false if the list is empty).
  static bool process(AnalysisIndex& index,
                      AnalysisWorklist& worklist) {
    auto const w = worklist.next();
    if (auto const c = w.cls()) {
      update(analyze(*c, index), index);
    } else if (auto const f = w.func()) {
      update(analyze(*f, index), index);
      for (auto const i : index.lookup_func_closure_invokes(*f)) {
        update(analyze(*i, index), index);
      }
    } else if (auto const u = w.unit()) {
      update(analyze(*u, index), index);
    } else {
      return false;
    }
    return true;
  }

  static FuncAnalysisResult analyze(const php::Func& f,
                                    const AnalysisIndex& index) {
    auto const wf = php::WideFunc::cns(&f);
    AnalysisContext ctx{ f.unit, wf, f.cls };
    return analyze_func(AnalysisIndexAdaptor{ index }, ctx, CollectionOpts{});
  }

  static ClassAnalysis analyze(const php::Class& c,
                               const AnalysisIndex& index) {
    assertx(!is_closure(c));
    return is_used_trait(c)
      ? analyze_class_separate(index, Context { c.unit, nullptr, &c })
      : analyze_class(AnalysisIndexAdaptor { index },
                      Context { c.unit, nullptr, &c });
  }

  static UnitAnalysis analyze(const php::Unit& u, const AnalysisIndex& index) {
    return analyze_unit(index, Context { u.filename, nullptr, nullptr });
  }

  static void update(FuncAnalysisResult fa, AnalysisIndex& index) {
    SCOPE_ASSERT_DETAIL("update func") {
      return "Updating Func: " + show(fa.ctx);
    };

    auto const UNUSED bump =
      trace_bump(fa.ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    AnalysisIndexAdaptor adaptor{index};
    ContextPusher _{adaptor, fa.ctx};
    index.refine_return_info(fa);
    index.refine_closure_use_vars(fa);
    index.refine_constants(fa);
    index.refine_class_constants(fa);
    index.update_prop_initial_values(fa);
    index.update_bytecode(fa);
  }

  static void update(ClassAnalysis ca, AnalysisIndex& index) {
    SCOPE_ASSERT_DETAIL("update class") {
      return "Updating Class: " + show(ca.ctx);
    };

    {
      auto const UNUSED bump =
        trace_bump(ca.ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
      AnalysisIndexAdaptor adaptor{index};
      ContextPusher _{adaptor, ca.ctx};
      index.update_type_consts(ca);
      index.refine_private_props(ca);
      index.refine_private_statics(ca);
    }
    for (auto& fa : ca.methods)  update(std::move(fa), index);
    for (auto& fa : ca.closures) update(std::move(fa), index);
  }

  static void update(UnitAnalysis ua, AnalysisIndex& index) {
    SCOPE_ASSERT_DETAIL("update unit") {
      return "Updating Unit: " + show(ua.ctx);
    };
    auto const UNUSED bump =
      trace_bump(ua.ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    AnalysisIndexAdaptor adaptor{index};
    ContextPusher _{adaptor, ua.ctx};
    index.update_type_aliases(ua);
  }
};

Job<AnalyzeJob> s_analyzeJob;

constexpr size_t kMaxBucketWeight =
  1UL*1024UL*1024UL*1024UL + 512UL*1024UL*1024UL;

AnalysisScheduler analyze_full(Index& index) {
  trace_time tracer{"analyze", index.sample()};

  using namespace folly::gen;

  // Register every program entity for distributed full analysis.
  AnalysisScheduler scheduler{index};
  {
    trace_time trace2{"analyze init"};
    trace2.ignore_client_stats();

    scheduler.reserveForRegistration(
      index.all_classes_to_analyze().size(),
      index.all_funcs().size(),
      index.all_units().size()
    );
    scheduler.registerAllBulk(
      index.all_classes_to_analyze(),
      index.all_funcs(),
      index.all_units()
    );
  }

  auto const run = [&] (AnalysisInput input, size_t round) -> coro::Task<void> {
    co_await coro::co_reschedule_on_current_executor;

    if (input.empty()) co_return;

    auto metadata = make_exec_metadata(
      "analyze",
      round,
      input.key()->toCppString()
    );

    auto bundleNames = input.reportBundleNames();

    auto [inputMeta, config] = co_await coro::collectAll(
      index.client().store(input.takeMeta()),
      index.configRef().getCopy()
    );

    auto inputRefs = input.toInput(std::move(inputMeta));

    // Run the job
    auto outputs = co_await [&, config = &config] {
      return index.client().exec(
        s_analyzeJob,
        std::move(*config),
        singleton_vec(std::move(inputRefs)),
        std::move(metadata)
      );
    }();

    always_assert(outputs.size() == 1);
    auto& [bundleRefs, metaRef] = outputs[0];

    auto meta = co_await index.client().load(std::move(metaRef));
    always_assert(bundleRefs.size() == bundleNames.size());

    // Inform the scheduler
    scheduler.record(
      AnalysisOutput{
        std::move(bundleNames),
        std::move(bundleRefs),
        std::move(meta)
      }
    );

    co_return;
  };

  size_t round{0};
  while (auto const workItems = scheduler.workItems()) {
    trace_time trace{
      "analyze round",
      fmt::format("round {} -- {} work items", round, workItems)
    };
    // Get the work buckets from the scheduler.
    auto work = [&] {
      trace_time trace2{"analyze schedule", fmt::format("round {}", round)};
      trace2.ignore_client_stats();
      // TODO: Investigate enabling trace scheduling for later rounds
      // (few work items) to converge faster and eliminate tail rounds.
      // Initial attempt with depth=10 caused scheduler explosion on
      // highly-connected nodes.
      return scheduler.schedule(AnalysisMode::Full, 0, 0, kMaxBucketWeight);
    }();
    // Work shouldn't be empty because we add non-zero work items this
    // round.
    assertx(!work.empty());

    {
      // Process the work buckets in individual analysis jobs. These record
      // their results as each one finishes.
      trace_time trace2{
        "analyze run",
        fmt::format("round {} -- {} jobs", round, work.size())
      };
      trace2.ignore_client_stats();

      coro::blockingWait(coro::collectAllRange(
        from(work)
          | move
          | map([&] (AnalysisInput&& input) {
              return co_withExecutor(
                index.executor().sticky(),
                run(std::move(input), round)
              );
            })
          | as<std::vector>()
      ));
    }

    {
      // All the jobs recorded their results in the scheduler. Now let
      // the scheduler know that all jobs are done, so it can
      // determine what needs to be run in the next round.
      trace_time trace2{"analyze deps", fmt::format("round {}", round)};
      trace2.ignore_client_stats();
      scheduler.recordingDone();
      ++round;
    }
  }

  return scheduler;
}

// Run preliminary constants analysis, then prepare its results for the full
// pass.
void analyze_constants(Index& index) {
  trace_time tracer{"analyze constants", index.sample()};
  tracer.ignore_client_stats();

  index.use_class_dependencies(false);
  {
    index.set_constant_analysis(true);
    SCOPE_EXIT { index.set_constant_analysis(false); };
    analyze_iteratively(index, LocalAnalysisMode::Constants);
  }
  index.preresolve_type_structures();
}

//////////////////////////////////////////////////////////////////////

// Extern-worker job to run final pass optimizations and UnitEmitter
// emission.

struct FinalPassJob {
  static std::string name() { return "hhbbc-final-pass"; }
  static void init(const Config& config) {
    process_init(config.o, config.gd, true);
    AnalysisIndex::start();
  }
  static void fini() { AnalysisIndex::stop(); }

  template<typename T> using V = Variadic<T>;

  struct Output {
    std::vector<UnitEmitterSerdeWrapper> units;
    hphp_fast_map<std::string, std::string> indexDump;
    hphp_fast_map<std::string, std::string> repDump;
    template <typename SerDe> void serde(SerDe& sd) {
      ScopedStringDataIndexer _;
      sd(units)
        (indexDump, std::less<>{})
        (repDump, std::less<>{})
        ;
    }
  };

  static Output run(V<AnalysisIndexBundle> bundles,
                    V<AnalysisIndexBundle> depBundles,
                    AnalysisInput::Meta meta) {
    // AnalysisIndex ctor will initialize the worklist appropriately.
    AnalysisWorklist worklist;
    AnalysisIndex index{
      worklist,
      std::move(bundles.vals),
      std::move(depBundles.vals),
      std::move(meta),
      AnalysisMode::Final
    };
    while (process(index, worklist)) {}

    AnalysisIndexAdaptor adaptor{ index };
    Output out;
    for (auto u : index.units_to_emit()) {
      if (meta.dumpRep) {
        auto d = dump_representation(adaptor, *u);
        if (!d.empty()) {
          out.repDump.emplace(u->filename->toCppString(), std::move(d));
        }
      }
      if (meta.dumpIndex) {
        auto d = dump_index(adaptor, *u);
        if (!d.empty()) {
          out.indexDump.emplace(u->filename->toCppString(), std::move(d));
        }
      }
      out.units.emplace_back(emit_unit(adaptor, *u));
    }
    return out;
  }
private:
  // Analyze the work item at the front of the worklist (returning
  // false if the list is empty).
  static bool process(AnalysisIndex& index,
                      AnalysisWorklist& worklist) {
    auto w = worklist.next();
    if (auto c = w.cls()) {
      optimize(*c, index);
    } else if (auto f = w.func()) {
      optimize(*f, index);
      for (auto i : index.lookup_func_closure_invokes(*f)) {
        optimize(*i, index);
      }
    } else if (w.unit()) {
      // Nothing to do
    } else {
      return false;
    }
    return true;
  }

  static void optimize(php::Func& f, const AnalysisIndex& index) {
    auto wf = php::WideFunc::mut(&f);
    AnalysisContext ctx{ f.unit, wf, f.cls };
    AnalysisIndexAdaptor adaptor{ index };
    optimize_func(adaptor, analyze_func(adaptor, ctx, CollectionOpts{}), wf);
  }

  static void optimize(php::Class& c, const AnalysisIndex& index) {
    assertx(!is_closure(c));
    for (auto& m : c.methods) optimize(*m, index);
    for (auto& clo : c.closures) {
      for (auto& m : clo->methods) optimize(*m, index);
    }
  }
};

Job<FinalPassJob> s_finalPassJob;

using UnitEmitterRef = Ref<FinalPassJob::Output>;
using UnitEmitterRefs = RefVec<FinalPassJob::Output>;

UnitEmitterRefs final_pass(Index& index,
                           AnalysisScheduler scheduler,
                           bool debugDump) {
  trace_time tracer{"final-pass", index.sample()};
  tracer.ignore_client_stats();

  auto const run = [&] (AnalysisInput input) -> coro::Task<UnitEmitterRef> {
    co_await coro::co_reschedule_on_current_executor;

    always_assert(!input.empty());

    auto metadata =
      make_exec_metadata("final-pass", input.key()->toCppString());

    auto inputMeta = input.takeMeta();
    if (debugDump) {
      if (Trace::moduleEnabledRelease(Trace::hhbbc_dump, 2)) {
        inputMeta.dumpRep = true;
      }
      inputMeta.dumpIndex = true;
    }

    auto [inputMetaRef, config] = co_await coro::collectAll(
      index.client().store(std::move(inputMeta)),
      index.configRef().getCopy()
    );
    auto inputRefs = input.toInput(std::move(inputMetaRef));

    auto outputs = co_await index.client().exec(
      s_finalPassJob,
      std::move(config),
      singleton_vec(std::move(inputRefs)),
      std::move(metadata)
    );
    always_assert(outputs.size() == 1);
    co_return outputs[0];
  };

  scheduler.enableAll();
  auto const workItems = scheduler.workItems();
  always_assert(workItems > 0);

  auto work = [&] {
    trace_time trace2{
      "final-pass schedule",
      fmt::format("{} work items", workItems)
    };
    trace2.ignore_client_stats();
    // No traces here, we only need the immediate deps
    return scheduler.schedule(AnalysisMode::Final, 0, 0, kMaxBucketWeight);
  }();
  assertx(!work.empty());

  trace_time trace2{
    "final-pass run",
    fmt::format("{} jobs", work.size())
  };
  using namespace folly::gen;
  return coro::blockingWait(coro::collectAllRange(
    from(work)
      | move
      | map([&] (AnalysisInput&& input) {
          return co_withExecutor(
            index.executor().sticky(),
            run(std::move(input))
          );
        })
      | as<std::vector>()
  ));
}

//////////////////////////////////////////////////////////////////////

void emit_units(Index& index,
                UnitEmitterRefs refs,
                const EmitCallback& callback,
                const std::string& debugDump) {
  trace_time _{
    "emit-units",
    fmt::format("{} units", refs.size()),
    index.sample()
  };

  std::atomic<uint64_t> next{0};
  auto const run = [&] (UnitEmitterRef r) -> coro::Task<void> {
    co_await coro::co_reschedule_on_current_executor;

    auto output = co_await index.client().load(std::move(r));

    if (!debugDump.empty()) {
      for (auto const& [p, d] : output.repDump) {
        write_representation_dump(debugDump, p, d);
      }
      for (auto const& [p, d] : output.indexDump) {
        write_index_dump(debugDump, p, d);
      }
    }

    for (auto& ue : output.units) {
      auto const idx = next++;
      ue.m_ue->m_sn = idx;
      ue.m_ue->setSha1(SHA1 { idx });
      if (Cfg::Eval::AbortBuildOnVerifyError && !ue.m_ue->check(false)) {
        fprintf(
          stderr,
          "The optimized unit for %s did not pass verification, "
          "bailing because Eval.AbortBuildOnVerifyError is set\n",
          ue.m_ue->m_filepath->data()
        );
        _Exit(HPHP_EXIT_FAILURE);
      }
      callback(std::move(ue.m_ue));
    }

    co_return;
  };

  using namespace folly::gen;
  coro::blockingWait(
    coro::collectAllRange(
      from(refs)
        | move
        | map([&] (UnitEmitterRef&& r) {
            return co_withExecutor(
              index.executor().sticky(),
              run(std::move(r))
            );
          })
        | as<std::vector>()
    )
  );
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

struct WholeProgramInput::Key::Impl {
  enum class Type {
    None,
    Fail,
    Unit,
    Func,
    FuncBytecode,
    Class,
    ClassBytecode
  };

  using UnresolvedTypes = TSStringSet;

  struct FailInfo {
    LSString message;
    template <typename SerDe> void serde(SerDe& sd) { sd(message); }
  };
  struct UnitInfo {
    LSString name;
    std::vector<TypeMapping> typeMappings;
    std::vector<std::pair<SString, bool>> constants;
    SStringSet cinitPredeps;
    SStringSet predeps;
    template <typename SerDe> void serde(SerDe& sd) {
      sd(name)
        (typeMappings)
        (constants)
        (cinitPredeps, string_data_lt{})
        (predeps, string_data_lt{})
        ;
    }
  };
  struct FuncInfo {
    LSString name;
    LSString unit;
    bool methCaller;
    UnresolvedTypes unresolvedTypes;
    template <typename SerDe> void serde(SerDe& sd) {
      sd(name)
        (unit)
        (methCaller)
        (unresolvedTypes, string_data_lt_type{})
        ;
    }
  };
  struct FuncBytecodeInfo {
    LSString name;
    LSString unit;
    bool methCaller;
    template <typename SerDe> void serde(SerDe& sd) {
      sd(name)(unit)(methCaller);
    }
  };
  struct ClassInfo {
    LSString name;
    LSString context;
    LSString unit;
    std::vector<SString> closures;
    std::vector<SString> dependencies;
    Optional<TypeMapping> typeMapping;
    UnresolvedTypes unresolvedTypes;
    template <typename SerDe> void serde(SerDe& sd) {
      sd(name)
        (context)
        (unit)
        (closures)
        (dependencies)
        (typeMapping)
        (unresolvedTypes, string_data_lt_type{})
        ;
    }
  };
  struct ClassBytecodeInfo {
    LSString name;
    template <typename SerDe> void serde(SerDe& sd) {
      sd(name);
    }
  };

  Type type;
  union {
    FailInfo fail;
    UnitInfo unit;
    FuncInfo func;
    FuncBytecodeInfo funcBC;
    ClassInfo cls;
    ClassBytecodeInfo clsBC;
  };

  Impl() : type{Type::None} {}

  explicit Impl(FailInfo i)  : type{Type::Fail},  fail{std::move(i)} {}
  explicit Impl(UnitInfo i)  : type{Type::Unit},  unit{std::move(i)} {}
  explicit Impl(FuncInfo i)  : type{Type::Func},  func{std::move(i)} {}
  explicit Impl(ClassInfo i) : type{Type::Class}, cls{std::move(i)}  {}
  explicit Impl(FuncBytecodeInfo i)
    : type{Type::FuncBytecode}, funcBC{std::move(i)} {}
  explicit Impl(ClassBytecodeInfo i)
    : type{Type::ClassBytecode}, clsBC{std::move(i)} {}

  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;

  void destroyInfo() {
    switch (type) {
      case Type::None:  break;
      case Type::Fail:  fail.~FailInfo();  break;
      case Type::Unit:  unit.~UnitInfo();  break;
      case Type::Func:  func.~FuncInfo();  break;
      case Type::Class: cls.~ClassInfo();  break;
      case Type::FuncBytecode:
        funcBC.~FuncBytecodeInfo();
        break;
      case Type::ClassBytecode:
        clsBC.~ClassBytecodeInfo();
        break;
    }
  }

  ~Impl() { destroyInfo(); }

  template <typename SerDe> void serde(SerDe& sd) {
    if constexpr (SerDe::deserializing) {
      destroyInfo();
      sd(type);
      switch (type) {
        case Type::None:  break;
        case Type::Fail:  new (&fail) FailInfo();  break;
        case Type::Unit:  new (&unit) UnitInfo();  break;
        case Type::Func:  new (&func) FuncInfo();  break;
        case Type::Class: new (&cls)  ClassInfo(); break;
        case Type::FuncBytecode:
          new (&funcBC) FuncBytecodeInfo();
          break;
        case Type::ClassBytecode:
          new (&clsBC) ClassBytecodeInfo();
          break;
      }
    } else {
      sd(type);
    }

    switch (type) {
      case Type::None:  break;
      case Type::Fail:  sd(fail); break;
      case Type::Unit:  sd(unit); break;
      case Type::Func:  sd(func); break;
      case Type::Class: sd(cls);  break;
      case Type::FuncBytecode:
        sd(funcBC);
        break;
      case Type::ClassBytecode:
        sd(clsBC);
        break;
    }
  }
};

struct WholeProgramInput::Value::Impl {
  std::unique_ptr<php::Func> func;
  std::unique_ptr<php::Class> cls;
  std::unique_ptr<php::Unit> unit;
  std::unique_ptr<php::FuncBytecode> funcBC;
  std::unique_ptr<php::ClassBytecode> clsBC;

  explicit Impl(std::nullptr_t) {}
  explicit Impl(std::unique_ptr<php::Func> func) : func{std::move(func)} {}
  explicit Impl(std::unique_ptr<php::Class> cls) : cls{std::move(cls)} {}
  explicit Impl(std::unique_ptr<php::Unit> unit) : unit{std::move(unit)} {}
  explicit Impl(std::unique_ptr<php::FuncBytecode> b)  : funcBC{std::move(b)} {}
  explicit Impl(std::unique_ptr<php::ClassBytecode> b) : clsBC{std::move(b)} {}
};

struct WholeProgramInput::Impl {
  folly::AtomicLinkedList<std::pair<Key, Ref<Value>>> values;
};

WholeProgramInput::WholeProgramInput() : m_impl{new Impl} {}

void WholeProgramInput::add(Key k, extern_worker::Ref<Value> v) {
  assertx(m_impl);
  m_impl->values.insertHead(std::make_pair(std::move(k), std::move(v)));
}

std::vector<std::pair<WholeProgramInput::Key, WholeProgramInput::Value>>
WholeProgramInput::make(std::unique_ptr<UnitEmitter> ue) {
  assertx(ue);

  auto parsed = parse_unit(*ue);

  std::vector<std::pair<Key, Value>> out;

  using KeyI = Key::Impl;
  using ValueI = Value::Impl;

  auto const add = [&] (auto k, auto v) {
    Key key;
    Value value;
    key.m_impl.reset(new KeyI{std::move(k)});
    value.m_impl.reset(new ValueI{std::move(v)});
    out.emplace_back(std::move(key), std::move(value));
  };

  auto const addType =
    [&] (KeyI::UnresolvedTypes& u,
         const folly::Range<const TypeConstraint*>& tcs,
         const php::Class* cls = nullptr) {
    // Skip names which match the current class name. We don't need to
    // report these as it's implicit.
    for (auto const& tc : tcs) {
      if (tc.isUnresolved() && (!cls || !cls->name->tsame(tc.typeName()))) {
        u.emplace(tc.typeName());
      }
    }
  };

  auto const addFuncTypes = [&] (KeyI::UnresolvedTypes& u,
                                 const php::Func& f,
                                 const php::Class* cls = nullptr) {
    for (auto const& p : f.params) {
      addType(u, p.typeConstraints.range(), cls);
    }
    addType(u, f.retTypeConstraints.range(), cls);
  };

  if (parsed.unit) {
    if (auto const& fi = parsed.unit->fatalInfo) {
      auto const msg = makeStaticString(fi->fatalMsg);
      if (!fi->fatalLoc) {
        add(KeyI::FailInfo{msg}, nullptr);
        return out;
      }
    }

    KeyI::UnitInfo info{parsed.unit->filename};
    info.cinitPredeps = std::move(parsed.cinitPredeps);
    info.predeps = std::move(parsed.predeps);

    for (auto const& typeAlias : parsed.unit->typeAliases) {
      info.typeMappings.emplace_back(
        TypeMapping{
          typeAlias->name,
          typeAlias->value,
          true,
          false,
          parsed.unit->filename,
          typeAlias->typeStructure
        }
      );
    }
    for (auto const& cns : parsed.unit->constants) {
      info.constants.emplace_back(
        cns->name,
        type(cns->val) == KindOfUninit
      );
    }
    add(std::move(info), std::move(parsed.unit));
  }

  auto const onCls = [&] (std::unique_ptr<php::Class>& c,
                          php::ClassBytecode& bc,
                          KeyI::UnresolvedTypes& types,
                          std::vector<SString>& deps) {
    assertx(IMPLIES(is_closure(*c), c->methods.size() == 1));

    for (auto& m : c->methods) {
      addFuncTypes(types, *m, c.get());
      bc.methodBCs.emplace_back(m->name, std::move(m->rawBlocks));
    }
    for (auto const& p : c->properties) {
      addType(types, p.typeConstraints.range(), c.get());
    }

    auto const d = Index::Input::makeDeps(*c);
    deps.insert(end(deps), begin(d), end(d));

    assertx(IMPLIES(is_closure(*c), !(c->attrs & AttrEnum)));
  };

  for (auto& c : parsed.classes) {
    auto const name = c->name;
    auto const declFunc = c->closureDeclFunc;
    auto const unit = c->unit;

    assertx(IMPLIES(is_closure(*c), !c->closureContextCls));
    assertx(IMPLIES(is_closure(*c), declFunc));

    php::ClassBytecode bc{name};
    KeyI::UnresolvedTypes types;
    std::vector<SString> deps;
    std::vector<SString> closures;

    onCls(c, bc, types, deps);
    for (auto& clo : c->closures) {
      assertx(is_closure(*clo));
      onCls(clo, bc, types, deps);
      closures.emplace_back(clo->name);
    }

    Optional<TypeMapping> typeMapping;
    if (c->attrs & AttrEnum) {
      assertx(!is_closure(*c));
      auto tc = c->enumBaseTy;
      assertx(!tc.isNullable());
      addType(types, folly::Range<const TypeConstraint*>(&tc, &tc + 1), nullptr);
      if (tc.isMixed()) tc.setType(AnnotType::ArrayKey);
      typeMapping.emplace(
        TypeMapping{c->name, tc, false, true, unit, nullptr}
      );
    }

    std::sort(begin(deps), end(deps), string_data_lt_type{});
    deps.erase(
      std::unique(begin(deps), end(deps), string_data_tsame{}),
      end(deps)
    );

    std::sort(begin(closures), end(closures), string_data_lt_type{});

    add(
      KeyI::ClassBytecodeInfo{name},
      std::make_unique<php::ClassBytecode>(std::move(bc))
    );
    add(
      KeyI::ClassInfo{
        name,
        declFunc,
        unit,
        std::move(closures),
        std::move(deps),
        std::move(typeMapping),
        std::move(types)
      },
      std::move(c)
    );
  }

  for (auto& f : parsed.funcs) {
    auto const name = f->name;
    auto const unit = f->unit;
    auto const methCaller = bool(f->attrs & AttrIsMethCaller);

    KeyI::UnresolvedTypes types;
    addFuncTypes(types, *f);

    add(
      KeyI::FuncBytecodeInfo{name, unit, methCaller},
      std::make_unique<php::FuncBytecode>(name, std::move(f->rawBlocks))
    );
    add(
      KeyI::FuncInfo{name, unit, methCaller, std::move(types)},
      std::move(f)
    );
  }
  return out;
}

void WholeProgramInput::Key::serde(BlobEncoder& sd) const {
  assertx(m_impl);
  sd(*m_impl);
}

void WholeProgramInput::Key::serde(BlobDecoder& sd) {
  m_impl.reset(new Impl());
  sd(*m_impl);
}

void WholeProgramInput::Value::serde(BlobEncoder& sd) const {
  assertx(m_impl);
  assertx(
    (bool)m_impl->func + (bool)m_impl->cls + (bool)m_impl->unit +
    (bool)m_impl->funcBC + (bool)m_impl->clsBC <= 1
  );
  if (m_impl->func) {
    sd(m_impl->func, nullptr);
  } else if (m_impl->cls) {
    sd(m_impl->cls);
  } else if (m_impl->unit) {
    sd(m_impl->unit);
  } else if (m_impl->funcBC) {
    sd(m_impl->funcBC);
  } else if (m_impl->clsBC) {
    sd(m_impl->clsBC);
  }
}

void WholeProgramInput::Key::Deleter::operator()(Impl* i) const {
  delete i;
}
void WholeProgramInput::Value::Deleter::operator()(Impl* i) const {
  delete i;
}
void WholeProgramInput::Deleter::operator()(Impl* i) const {
  delete i;
}

//////////////////////////////////////////////////////////////////////

namespace {

Index::Input make_index_input(WholeProgramInput input) {
  Index::Input out;

  using WPI = WholeProgramInput;
  using Key = WPI::Key::Impl;

  input.m_impl->values.sweep(
    [&] (std::pair<WPI::Key, Ref<WPI::Value>>&& p) {
      switch (p.first.m_impl->type) {
        case Key::Type::None:
          break;
        case Key::Type::Fail:
          // An unit which failed the verifier. This causes us
          // to exit immediately with an error.
          fprintf(stderr, "%s", p.first.m_impl->fail.message->data());
          _Exit(HPHP_EXIT_FAILURE);
        case Key::Type::Class:
          out.classes.emplace_back(
            Index::Input::ClassMeta{
              p.second.cast<std::unique_ptr<php::Class>>(),
              p.first.m_impl->cls.name,
              std::move(p.first.m_impl->cls.dependencies),
              p.first.m_impl->cls.context,
              std::move(p.first.m_impl->cls.closures),
              p.first.m_impl->cls.unit,
              std::move(p.first.m_impl->cls.typeMapping),
              std::vector<SString>{
                begin(p.first.m_impl->cls.unresolvedTypes),
                end(p.first.m_impl->cls.unresolvedTypes)
              }
            }
          );
          break;
        case Key::Type::Func:
          out.funcs.emplace_back(
            Index::Input::FuncMeta{
              p.second.cast<std::unique_ptr<php::Func>>(),
              p.first.m_impl->func.name,
              p.first.m_impl->func.unit,
              p.first.m_impl->func.methCaller,
              std::vector<SString>{
                begin(p.first.m_impl->func.unresolvedTypes),
                end(p.first.m_impl->func.unresolvedTypes)
              }
            }
          );
          break;
        case Key::Type::Unit:
          out.units.emplace_back(
            Index::Input::UnitMeta{
              p.second.cast<std::unique_ptr<php::Unit>>(),
              p.first.m_impl->unit.name,
              std::move(p.first.m_impl->unit.typeMappings),
              std::move(p.first.m_impl->unit.constants),
              std::move(p.first.m_impl->unit.cinitPredeps),
              std::move(p.first.m_impl->unit.predeps)
            }
          );
          break;
        case Key::Type::FuncBytecode:
          out.funcBC.emplace_back(
            Index::Input::FuncBytecodeMeta{
              p.second.cast<std::unique_ptr<php::FuncBytecode>>(),
              p.first.m_impl->funcBC.name,
              p.first.m_impl->funcBC.unit,
              p.first.m_impl->funcBC.methCaller
            }
          );
          break;
        case Key::Type::ClassBytecode:
          out.classBC.emplace_back(
            Index::Input::ClassBytecodeMeta{
              p.second.cast<std::unique_ptr<php::ClassBytecode>>(),
              p.first.m_impl->clsBC.name
            }
          );
          break;
      }
    }
  );

  return out;
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

void whole_program(WholeProgramInput inputs,
                   Config config,
                   std::unique_ptr<TicketExecutor> executor,
                   std::unique_ptr<Client> client,
                   const EmitCallback& callback,
                   DisposeCallback dispose,
                   StructuredLogEntry* sample,
                   int num_threads) {
  trace_time tracer("whole program", sample);

  if (sample) {
    sample->setInt("hhbbc_thread_count", executor->numThreads());
  }

  if (num_threads > 0) {
    parallel::num_threads = num_threads;
    // Leave a thread free for cleanup
    parallel::final_threads = (num_threads > 1) ? (num_threads - 1) : 1;
  }

  Index index{
    make_index_input(std::move(inputs)),
    std::move(config),
    std::move(executor),
    std::move(client),
    std::move(dispose),
    sample
  };

  if (options.useExternWorkerForFullAnalysis) {
    auto scheduler = analyze_full(index);

    auto const debugDump = debug_dump_to();
    auto emitters = final_pass(index, std::move(scheduler), !debugDump.empty());
    emit_units(index, std::move(emitters), callback, debugDump);

    if (sample) {
      sample->setInt("hhbbc_num_units", index.all_units().size());
      sample->setInt("hhbbc_num_classes", index.all_classes_to_analyze().size());
      sample->setInt("hhbbc_num_funcs", index.all_funcs().size());
    }
  } else {
    index.make_local();
    analyze_constants(index);

    assertx(check(index.program()));

    IndexAdaptor adaptor{ index };

    std::atomic<uint64_t> next{0};
    auto const emitUnit = [&] (php::Unit& unit, int64_t sn) {
      auto ue = emit_unit(adaptor, unit);
      auto const idx = sn >= 0 ? uint64_t(sn) : next++;
      ue->m_sn = idx;
      ue->setSha1(SHA1 { idx });
      if (Cfg::Eval::AbortBuildOnVerifyError && !ue->check(false)) {
        fprintf(
          stderr,
          "The optimized unit for %s did not pass verification, "
          "bailing because Eval.AbortBuildOnVerifyError is set\n",
          ue->m_filepath->data()
        );
        _Exit(HPHP_EXIT_FAILURE);
      }
      callback(std::move(ue));
    };

    index.use_class_dependencies(true);
    analyze_iteratively(index, LocalAnalysisMode::Full);
    auto cleanup_for_final = std::thread([&] { index.cleanup_for_final(); });
    parallel::num_threads = parallel::final_threads;
    final_pass(index, emitUnit);
    cleanup_for_final.join();

    if (sample) {
      sample->setInt("hhbbc_num_units", index.program().units.size());
      sample->setInt("hhbbc_num_classes", index.program().classes.size());
      sample->setInt("hhbbc_num_funcs", index.program().funcs.size());
    }

    index.cleanup_post_emit();
  }

  summarize_memory(sample);
}

//////////////////////////////////////////////////////////////////////

}}
