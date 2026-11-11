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

#include <stdexcept>
#include <iostream>
#include <cstdlib>
#include <string>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <exception>
#include <utility>
#include <vector>
#include <filesystem>

#include <boost/program_options.hpp>

#include <folly/ScopeGuard.h>
#include <fmt/core.h>
#include <folly/system/HardwareConcurrency.h>
#include <folly/portability/Unistd.h>

#include "hphp/runtime/base/configs/configs.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/vm-worker.h"
#include "hphp/hhvm/process-init.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/treadmill.h"

#include "hphp/hhbbc/misc.h"
#include "hphp/hhbbc/options.h"
#include "hphp/hhbbc/parallel.h"
#include "hphp/hhbbc/representation.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/logger.h"
#include "hphp/util/rds-local.h"

namespace HPHP {

using namespace extern_worker;
namespace coro = folly::coro;

namespace HHBBC {

namespace {

//////////////////////////////////////////////////////////////////////

std::string output_repo;
std::string input_repo;
bool logging = true;

//////////////////////////////////////////////////////////////////////

void parse_options(int argc, char** argv) {
  namespace po = boost::program_options;

  auto const defaultThreadCount =
    std::max<long>(static_cast<long>(folly::available_concurrency()) - 1, 1);
  auto const defaultFinalThreadCount =
    std::max<long>(defaultThreadCount - 1, 1);

  std::vector<std::string> trace_fns;
  std::vector<std::string> trace_bcs;
  bool no_logging = false;
  bool no_cores = false;

  po::options_description basic("Options");
  basic.add_options()
    ("help", "display help message")
    ("output,o",
      po::value(&output_repo)->default_value("hhvm.hhbbc"),
      "output hhbc repo path")
    ("input",
      po::value(&input_repo)->default_value("hhvm.hhbc"),
      "input hhbc repo path")
    ("unit-order-file", po::value(&Cfg::Repo::UnitOrderFile),
      "source-relative unit paths in priority order, one per line")
    ("unit-order-buffer-size",
      po::value(&Cfg::Repo::UnitOrderBufferSize)->default_value(64 << 20),
      "unit ordering buffer limit in bytes")
    ("no-logging",
      po::bool_switch(&no_logging),
      "turn off logging")
    ("no-cores",
      po::bool_switch(&no_cores),
      "turn off core dumps (useful when running lots of tests in parallel)")
    ("profile-memory",
     po::value(&options.profileMemory)->default_value(""),
      "If non-empty, dump jemalloc memory profiles at key points")
    ("parallel-num-threads",
      po::value(&parallel::num_threads)->default_value(defaultThreadCount),
      "Number of threads to use for parallelism")
    ("parallel-final-threads",
      po::value(&parallel::final_threads)->default_value(
        defaultFinalThreadCount),
      "Number of threads to use for the final pass")
    ("trace",
      po::value(&trace_fns)->composing(),
      "Add a function to increase tracing level on (for debugging)")
    ("trace-bytecode",
      po::value(&trace_bcs)->composing(),
      "Add a bytecode to trace (for debugging)")
    ;

  // Some extra esoteric options that aren't exposed in --help for
  // now.
  po::options_description extended("Extended Options");
  extended.add_options()
    ("analyze-func-wlimit",  po::value(&options.analyzeFuncWideningLimit))
    ("analyze-class-wlimit", po::value(&options.analyzeClassWideningLimit))
    ("return-refine-limit",  po::value(&options.returnTypeRefineLimit))
    ("public-sprop-refine-limit", po::value(&options.publicSPropRefineLimit))
    ("local-hhbbc", po::value(&options.useLocalHHBBC))
    ;

  po::options_description oflags("Optimization Flags");
  oflags.add_options()
    ("context-sensitive-interp",  po::value(&options.ContextSensitiveInterp))
    ;

  po::options_description eflags("Extern-Worker Flags");
  eflags.add_options()
    ("extern-worker-use-case",                 po::value(&options.ExternWorkerUseCase))
    ("extern-worker-platform",                 po::value(&options.ExternWorkerPlatform))
    ("extern-worker-working-dir",              po::value(&options.ExternWorkerWorkingDir))
    ("extern-worker-timeout-secs",             po::value(&options.ExternWorkerTimeoutSecs))
    ("extern-worker-throttle-retries",         po::value(&options.ExternWorkerThrottleRetries))
    ("extern-worker-throttle-base-wait-msecs", po::value(&options.ExternWorkerThrottleBaseWaitMSecs))
    ("extern-worker-use-exec-cache",           po::value(&options.ExternWorkerUseExecCache))
    ("extern-worker-cleanup",                  po::value(&options.ExternWorkerCleanup))
    ("extern-worker-use-rich-client",          po::value(&options.ExternWorkerUseRichClient))
    ("extern-worker-use-p2p",                  po::value(&options.ExternWorkerUseP2P))
    ("extern-worker-verbose-logging",          po::value(&options.ExternWorkerVerboseLogging))
    ("extern-worker-async-cleanup",            po::value(&options.ExternWorkerAsyncCleanup))
    ("extern-worker-max-subprocess-memory",    po::value(&options.ExternWorkerMaxSubprocessMemory))
    ("extern-worker-use-subprocess-scheduler", po::value(&options.ExternWorkerUseSubprocessScheduler))
    ("extern-worker-zstd-dictionary",          po::value(&options.ExternWorkerZstdDictionaryPath))
    ;

  po::options_description all;
  all.add(basic).add(extended).add(oflags).add(eflags);

  po::positional_options_description pd;
  pd.add("input", 1);

  po::variables_map vm;
  po::store(
    po::command_line_parser(argc, argv)
      .options(all)
      .positional(pd)
      .extra_parser(
        [&] (const std::string& s) -> std::pair<std::string,std::string> {
          if (s.find("-f") == 0) {
            if (s.substr(2, 3) == "no-") {
              return { s.substr(5), "false" };
            }
            return { s.substr(2), "true" };
          }
          return {};
        })
      .run(),
    vm
  );
  po::notify(vm);

  if (vm.contains("help")) {
    std::cout << basic << "\n"
"Individual optimizations may be turned on and off using gcc-style -fflag\n"
"and -fno-flag arguments.  The various optimization flags are documented\n"
"in the code.\n";
    std::exit(0);
  }

  options.CoreDump = !no_cores;
  add_to_method_map(options.TraceFunctions, trace_fns);

  if (!options.profileMemory.empty()) {
    mallctlWrite("prof.active", true);
    mallctlWrite("prof.thread_active_init", true);
  }

  logging = !no_logging;
}

UNUSED void validate_options() {
  if (parallel::num_threads < 1) {
    std::cerr << "Invalid parallelism configuration.\n";
    std::exit(1);
  }
}

//////////////////////////////////////////////////////////////////////

RepoGlobalData get_global_data() {
  auto const now = std::chrono::high_resolution_clock::now();
  auto const nanos =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      now.time_since_epoch()
    );

  auto gd                        = RepoGlobalData{};
  gd.Signature                   = nanos.count();

  Cfg::StoreToGlobalData(gd);

  gd.EvalCoeffectEnforcementLevels = RO::EvalCoeffectEnforcementLevels;
  gd.SourceRootForFileBC = options.SourceRootForFileBC;

  for (auto const& elm : RuntimeOption::ConstantFunctions) {
    auto const s = internal_serialize(tvAsCVarRef(elm.second));
    gd.ConstantFunctions.emplace_back(elm.first, s.toCppString());
  }
  std::sort(gd.ConstantFunctions.begin(), gd.ConstantFunctions.end());

  return gd;
}

//////////////////////////////////////////////////////////////////////

// Receives UnitEmitters and turns them into WholeProgramInput Key and
// Values. This "loads" them locally and uploads them into an
// extern_worker::Client in the way that whole_program expects.
struct LoadRepoJob {
  using W = WholeProgramInput;

  static std::string name() { return "hhbbc-load-repo"; }
  static void init(const Config& config) {
    process_init(config.o, config.gd, false);
  }
  static std::vector<W::Key> fini() {
    process_exit(false);
    return std::move(s_keys);
  }
  static Variadic<W::Value> run(UnitEmitterSerdeWrapper wrapper) {
    std::vector<W::Value> values;
    for (auto& [key, value] : W::make(std::move(wrapper.m_ue))) {
      s_keys.emplace_back(std::move(key));
      values.emplace_back(std::move(value));
    }
    return Variadic<W::Value>{std::move(values)};
  }

  static std::vector<W::Key> s_keys;
};
std::vector<WholeProgramInput::Key> LoadRepoJob::s_keys;
Job<LoadRepoJob> s_loadRepoJob;

// Load all UnitEmitters from the RepoFile and convert them into what
// whole_program expects as input.
std::pair<WholeProgramInput, Config> load_repo(TicketExecutor& executor,
                                               Client& client,
                                               StructuredLogEntry& sample) {
  trace_time timer("load repo", &sample);

  SCOPE_EXIT { RepoFile::destroy(); };
  RepoFile::loadGlobalTables(false);
  auto const units = RepoFile::enumerateUnits();
  if (logging) std::cout << fmt::format("{} units\n", units.size());

  // Start this as early as possible
  auto config = Config::get(RepoFile::globalData());
  CoroAsyncValue<Ref<Config>> storedConfig{
    [&client, config] () { return client.store(config); },
    executor.sticky()
  };

  // Shard the emitters using consistent hashing of their path. Force
  // all systemlib units into the first bucket (and nothing else in
  // there).
  constexpr size_t kLoadGroupSize = 500;
  auto const numBuckets = std::max<size_t>(
    (units.size() + kLoadGroupSize - 1) / kLoadGroupSize,
    2
  );

  std::vector<std::vector<const StringData*>> groups;
  groups.resize(numBuckets);

  for (auto const unit : units) {
    if (FileUtil::isSystemName(unit->slice())) {
      groups[0].emplace_back(unit);
      continue;
    }
    auto const idx = consistent_hash(unit->hash(), numBuckets - 1) + 1;
    assertx(idx < numBuckets);
    groups[idx].emplace_back(unit);
  }

  // Maintain deterministic ordering within each group
  for (auto& group : groups) {
    std::sort(
      group.begin(),
      group.end(),
      [] (const StringData* a, const StringData* b) {
        return strcmp(a->data(), b->data()) < 0;
      }
    );
  }

  using WPI = WholeProgramInput;
  WPI inputs;

  auto const load = [&] (size_t bucketIdx,
                         std::vector<const StringData*> units)
    -> coro::Task<void> {
    assertx(!units.empty());

    // Load the UnitEmitters from disk (deferring this until here cuts
    // down on the number of UnitEmitters we have to keep in memory at
    // once).
    std::vector<UnitEmitterSerdeWrapper> ues;
    ues.reserve(units.size());
    for (auto const unit : units) {
      assertx((bucketIdx == 0) == FileUtil::isSystemName(unit->slice()));
      ues.emplace_back(
        RepoFile::loadUnitEmitter(unit, nullptr, false)
      );
    }

    // Process the first bucket locally. Systemlib units need full
    // process init and we don't currently do that in extern-worker
    // jobs.
    if (bucketIdx == 0) {
      std::vector<WPI::Key> keys;
      std::vector<WPI::Value> values;
      for (auto& ue : ues) {
        assertx(FileUtil::isSystemName(ue.m_ue->m_filepath->slice()));
        for (auto& [key, value] : WPI::make(std::move(ue.m_ue))) {
          keys.emplace_back(std::move(key));
          values.emplace_back(std::move(value));
        }
      }
      if (keys.empty()) co_return;
      auto valueRefs = co_await client.storeMulti(std::move(values));
      auto const numKeys = keys.size();
      assertx(valueRefs.size() == numKeys);
      for (size_t i = 0; i < numKeys; ++i) {
        inputs.add(std::move(keys[i]), std::move(valueRefs[i]));
      }
      co_return;
    }

    // Store them (and the config we'll use).
    auto [refs, configRef] = co_await coro::collectAll(
      client.storeMulti(std::move(ues)),
      storedConfig.getCopy()
    );

    std::vector<std::tuple<Ref<UnitEmitterSerdeWrapper>>> tuplized;
    tuplized.reserve(refs.size());
    for (auto& r : refs) tuplized.emplace_back(std::move(r));

    auto metadata = make_exec_metadata("load-repo", units[0]->toCppString());

    // Run the job and get refs to the keys and values.
    auto [valueRefs, keyRefs] = co_await
      client.exec(
        s_loadRepoJob,
        std::move(configRef),
        std::move(tuplized),
        std::move(metadata)
      );

    // We need the keys locally, so load them. Values can stay as
    // Refs.
    auto keys = co_await client.load(std::move(keyRefs));

    auto const numKeys = keys.size();
    size_t keyIdx = 0;
    for (auto& v : valueRefs) {
      for (auto& r : v) {
        always_assert(keyIdx < numKeys);
        inputs.add(std::move(keys[keyIdx]), std::move(r));
        ++keyIdx;
      }
    }
    always_assert(keyIdx == numKeys);

    co_return;
  };

  std::vector<coro::TaskWithExecutor<void>> tasks;
  for (size_t i = 0; i < groups.size(); ++i) {
    auto& group = groups[i];
    if (group.empty()) continue;
    tasks.emplace_back(co_withExecutor(executor.sticky(), load(i, std::move(group))));
  }
  coro::blockingWait(coro::collectAllRange(std::move(tasks)));
  return std::make_pair(std::move(inputs), std::move(config));
}

extern_worker::Options make_extern_worker_options() {
  extern_worker::Options opts;
  opts
    .setApplication("hhbbc")
    .setUseCase(options.ExternWorkerUseCase)
    .setUseSubprocess(options.ExternWorkerUseCase.empty()
                      ? extern_worker::Options::UseSubprocess::Always
                      : extern_worker::Options::UseSubprocess::Never)
    .setCacheExecs(options.ExternWorkerUseExecCache)
    .setCleanup(options.ExternWorkerCleanup)
    .setUseRichClient(options.ExternWorkerUseRichClient)
    .setUseP2P(options.ExternWorkerUseP2P)
    .setVerboseLogging(options.ExternWorkerVerboseLogging)
    .setCasConnectionCount(options.ExternWorkerCASConnectionCount)
    .setEngineConnectionCount(options.ExternWorkerEngineConnectionCount)
    .setAcConnectionCount(options.ExternWorkerActionCacheConnectionCount)
    .setFeaturesFile(options.ExternWorkerFeaturesFile)
    .setUseSubprocessScheduler(options.ExternWorkerUseSubprocessScheduler);
  if (options.ExternWorkerTimeoutSecs > 0) {
    opts.setTimeout(std::chrono::seconds{options.ExternWorkerTimeoutSecs});
  }
  if (!options.ExternWorkerPlatform.empty()) {
    opts.setPlatform(options.ExternWorkerPlatform);
  }
  if (!options.ExternWorkerWorkingDir.empty()) {
    opts.setWorkingDir(options.ExternWorkerWorkingDir);
  }
  if (options.ExternWorkerThrottleRetries >= 0) {
    opts.setThrottleRetries(options.ExternWorkerThrottleRetries);
  }
  if (options.ExternWorkerThrottleBaseWaitMSecs >= 0) {
    opts.setThrottleBaseWait(
      std::chrono::milliseconds{options.ExternWorkerThrottleBaseWaitMSecs}
    );
  }
  if (options.ExternWorkerMaxSubprocessMemory > 0) {
    opts.setMaxSubprocessMemory(options.ExternWorkerMaxSubprocessMemory);
  }
  if (!options.ExternWorkerZstdDictionaryPath.empty()) {
    opts.setZSTDDictionaryPath(options.ExternWorkerZstdDictionaryPath);
  }
  return opts;
}

void compile_repo() {
  auto executor = std::make_unique<TicketExecutor>(
    "HHBBCWorker",
    0,
    parallel::num_threads,
    [] {
      hphp_thread_init();
      hphp_session_init(Treadmill::SessionKind::HHBBC);
    },
    [] {
      hphp_context_exit();
      hphp_session_exit();
      hphp_thread_exit();
    },
    std::chrono::minutes{15}
  );
  auto client = std::make_unique<Client>(
    executor->sticky(),
    make_extern_worker_options()
  );
  trace_time::register_client_stats(client->getStatsPtr());

  StructuredLogEntry sample;
  sample.setStr("debug", debug ? "true" : "false");
  sample.setStr("use_case", options.ExternWorkerUseCase);
  sample.setStr("platform", options.ExternWorkerPlatform);
  sample.setInt("use_rich_client", options.ExternWorkerUseRichClient);
  sample.setInt("use_p2p", options.ExternWorkerUseP2P);
  sample.setInt("force_subprocess", options.ExternWorkerForceSubprocess);
  sample.setInt("use_exec_cache", options.ExternWorkerUseExecCache);
  sample.setInt("timeout_secs", options.ExternWorkerTimeoutSecs);
  sample.setInt("cleanup", options.ExternWorkerCleanup);
  sample.setInt("throttle_retries", options.ExternWorkerThrottleRetries);
  sample.setInt("throttle_base_wait_ms",
                options.ExternWorkerThrottleBaseWaitMSecs);
  sample.setInt("use_subprocess_scheduler",
                options.ExternWorkerUseSubprocessScheduler);
  sample.setStr("working_dir", options.ExternWorkerWorkingDir);
  sample.setStr("use_hphpc", "false");
  sample.setStr("use_hhbbc", "true");
  sample.setInt("hhbbc_thread_count", executor->numThreads());
  sample.setInt("hhbbc_async_cleanup", options.ExternWorkerAsyncCleanup);
  sample.setStr("extern_worker_impl", client->implName());
  sample.setStr("extern_worker_session", client->session());

  // Package Info must be read prior to load_repo as loading the repo
  // destroys the repo file
  auto const packageInfo = RepoFile::packageInfo();

  auto [inputs, config] = load_repo(*executor, *client, sample);

  RepoAutoloadMapBuilder autoload;
  RepoFileBuilder repo{output_repo};
  if (!Cfg::Repo::UnitOrderFile.empty()) {
    repo.writeInSnOrder(Cfg::Repo::UnitOrderBufferSize);
  }
  std::mutex repoLock;
  std::atomic<size_t> numUnits{0};

  auto const emit = [&] (std::unique_ptr<UnitEmitter> ue) {
    ++numUnits;
    autoload.addUnit(*ue);
    RepoFileBuilder::EncodedUE encoded{*ue, false};
    std::scoped_lock<std::mutex> _{repoLock};
    repo.add(std::move(encoded));
  };

  std::thread asyncDispose;
  SCOPE_EXIT { if (asyncDispose.joinable()) asyncDispose.join(); };
  auto const dispose = [&] (std::unique_ptr<TicketExecutor> e,
                            std::unique_ptr<Client> c) {
    if (!options.ExternWorkerAsyncCleanup) {
      // If we don't want to cleanup asynchronously, do so now.
      c.reset();
      e.reset();
      return;
    }
    // All the thread does is reset the unique_ptr to run the dtor.
    asyncDispose = std::thread{
      [e = std::move(e), c = std::move(c)] () mutable {
        c.reset();
        e.reset();
      }
    };
  };

  whole_program(
    std::move(inputs),
    std::move(config),
    std::move(executor),
    std::move(client),
    emit,
    dispose,
    &sample
  );

  trace_time timer{"finalizing repo", &sample};
  repo.finish(get_global_data(), autoload, packageInfo);

  // Only log big builds.
  if (numUnits >= Cfg::Eval::HHBBCMinUnitsToLog) {
    sample.force_init = true;
    StructuredLog::log("hhvm_whole_program", sample);
  }
}

//////////////////////////////////////////////////////////////////////

}

void process_init(const Options& o,
                  const RepoGlobalData& gd,
                  bool fullInit) {
  if (!o.CoreDump) {
    struct rlimit rl{};
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);
  }

  rds::threadInit();
  rds::local::init();
  SCOPE_FAIL { rds::local::fini(); };

  Hdf config;
  IniSetting::Map ini = IniSetting::Map::object;

  // We need to write correct coeffects before we load
  for (auto const& [name, value] : gd.EvalCoeffectEnforcementLevels) {
    config["Eval"]["CoeffectEnforcementLevels"][name] = value;
  }

  // These need to be set before RO::Load, because it triggers the
  // table resizing.
  if (gd.Eval_InitialTypeTableSize) {
    Cfg::Eval::InitialTypeTableSize  = gd.Eval_InitialTypeTableSize;
  }
  if (gd.Eval_InitialFuncTableSize) {
    Cfg::Eval::InitialFuncTableSize  = gd.Eval_InitialFuncTableSize;
  }
  if (gd.Eval_InitialStaticStringTableSize) {
    Cfg::Eval::InitialStaticStringTableSize = gd.Eval_InitialStaticStringTableSize;
  }

  RO::Load(ini, config);
  Cfg::Repo::Authoritative                  = false;
  Cfg::Jit::Enabled                         = false;
  Cfg::Eval::LowStaticArrays                = false;
  Cfg::Repo::DebugInfo                      = false;
  Cfg::Eval::PerfPidMap                     = false;
  Logger::LogLevel                          = Logger::LogError;

  // Load RepoGlobalData first because hphp_process_init can read
  // RuntimeOptions.
  gd.load(false);

  register_process_init();
  hphp_process_init(!fullInit, true);
  SCOPE_FAIL { hphp_process_exit(); };

  options = o;
  // Now that process state is set up, we can safely do a full load of
  // RepoGlobalData.
  gd.load();

  // When running hhbbc, these options are loaded from GD, and will
  // override CLI. When running hhvm, these options are not loaded
  // from GD, but read from CLI. NB: These are only needed if
  // RepoGlobalData::load() does not currently write them which is
  // called in hphp_process_init().
  Cfg::LoadFromGlobalDataOnlyHHBBC(gd);

  options.SourceRootForFileBC = gd.SourceRootForFileBC;

  if (fullInit) hphp_session_init(Treadmill::SessionKind::HHBBC);
}

void process_exit(bool full) {
  if (full) {
    hphp_context_exit();
    hphp_session_exit();
  }
  hphp_process_exit();
  rds::local::fini();
}

int main(int argc, char** argv) try {
  parse_options(argc, argv);

  if (std::filesystem::exists(output_repo)) {
    std::cout << "output repo already exists; removing it\n";
    if (unlink(output_repo.c_str())) {
      std::cerr << "failed to unlink output repo: "
                << strerror(errno) << '\n';
      return 1;
    }
  }
  if (!std::filesystem::exists(input_repo)) {
    std::cerr << "input repo `" << input_repo << "' not found\n";
    return 1;
  }

  RepoFile::init(input_repo);

  auto const& gd = RepoFile::globalData();
  gd.load(false);

  process_init(options, gd, true);
  SCOPE_EXIT { process_exit(true); };

  Logger::LogLevel = logging ? Logger::LogInfo : Logger::LogError;
  Logger::Escape = false;
  Logger::AlwaysEscapeLog = false;

  Trace::BumpRelease bumper(Trace::hhbbc_time, -1, logging);
  compile_repo();
  return 0;
}

catch (std::exception& e) {
  Logger::Error("std::exception: %s", e.what());
  return 1;
}

//////////////////////////////////////////////////////////////////////

}}
