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

#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/perf-warning.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/vm-regs.h"
#include "hphp/runtime/vm/live-func-stats.h"
#include "hphp/runtime/vm/workload-stats.h"

#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/code-view.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/guard-type-profile.h"
#include "hphp/runtime/vm/jit/mcgen-async.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/relocation.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/tc-intercept.h"
#include "hphp/runtime/vm/jit/tc-record.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/trans-db.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/unwind-itanium.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/write-lease.h"

#include "hphp/util/configs/codecache.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/disasm.h"
#include "hphp/util/logger.h"
#include "hphp/util/mutex.h"
#include "hphp/util/rds-local.h"
#include "hphp/util/service-data.h"
#include "hphp/util/trace.h"

#include <folly/container/Array.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <type_traits>

TRACE_SET_MOD(mcg)

namespace HPHP::jit::tc {

__thread bool tl_is_jitting = false;
__thread bool tl_threadIsFull = false;

CodeCache* g_code{nullptr};
CodeCacheViews* g_views{nullptr};
SrcDB g_srcDB;
UniqueStubs g_ustubs;
ServiceData::ExportedCounter* g_JitMaturityCounter{nullptr};
int g_maxJitMaturity = 100; // may be reduced to 99 when workload changes
folly::ConcurrentHashMap<FuncId, uint32_t> g_funcLiveTransBytes;


///////////////////////////////////////////////////////////////////////////////

namespace {

const StaticString s_AlwaysInterp("__ALWAYS_INTERP");

std::atomic<uint64_t> s_numTrans;
SimpleMutex s_codeLock{false, RankCodeCache};
SimpleMutex s_metadataLock{false, RankCodeMetadata};
RDS_LOCAL_NO_CHECK(size_t, s_initialTCSize);

bool shouldPGOFunc(const Func* func) {
  return profData() != nullptr;
}

// A code reuse block owns temporary code blocks used to emit into a reused
// segment of another view.
struct CodeReuseBlock {
  CodeBlock reusedMain, reusedCold, reusedFrozen;

  // Get a view into possibly reused code blocks (if there is space, and
  // reusable TC is enabled).
  CodeCache::View getMaybeReusedView(CodeCache::View& src,
                                     const TransRange& range) {
    if (!Cfg::Eval::EnableReusableTC) return src;
    auto main = &src.main();
    auto cold = &src.cold();
    auto frozen = &src.frozen();

    auto const pad = Cfg::Eval::ReusableTCPadding;
    size_t mainSize   = range.main.size() + pad;
    size_t coldSize   = range.cold.size() + pad;
    size_t frozenSize = range.frozen.size() + pad;
    if (auto const s = (TCA)main->allocInner(mainSize)) {
      reusedMain.init(s, mainSize, "Reused main");
      main = &reusedMain;
    }
    if (auto const s = (TCA)cold->allocInner(coldSize)) {
      reusedCold.init(s, coldSize, "Reused cold");
      cold = &reusedCold;
    }
    if (cold != frozen) {
      if (auto const s = (TCA)frozen->allocInner(frozenSize)) {
        reusedFrozen.init(s, frozenSize, "Reused frozen");
        frozen = &reusedFrozen;
      }
    }

    return CodeCache::View(*main, *cold, *frozen, src.data(), false, src.isOwned());
  }
};

class LiveStreak {
 public:
  size_t size() const { return m_size; }

  bool contains(FuncId::Int id) const {
    auto const end = m_ids + m_size;
    return std::find(m_ids, end, id) != end;
  }

  void clear() { m_size = 0; }

  void add(FuncId::Int id) {
    if (Cfg::Jit::LiveStreakMaxSize == 0) return;
    if (m_size < Cfg::Jit::LiveStreakMaxSize) {
      m_ids[m_size++] = id;
      return;
    }
    // Drop the oldest id
    std::memmove(m_ids, m_ids + 1, (m_size - 1) * sizeof(FuncId::Int));
    m_ids[m_size - 1] = id;
  }

 private:
  uint8_t m_size;
  FuncId::Int m_ids[UINT8_MAX];
};

static_assert(std::is_same_v<decltype(Cfg::Jit::LiveStreakMaxSize), uint8_t>);
static_assert(std::is_trivial_v<LiveStreak>);

__thread LiveStreak tl_liveStreak;

}
///////////////////////////////////////////////////////////////////////////////

TransLoc TransRange::loc() const {
  TransLoc loc;
  loc.setMainStart(main.begin());
  loc.setColdStart(cold.begin());
  loc.setFrozenStart(frozen.begin());
  loc.setMainSize(main.size());
  loc.setColdSize(cold.size());
  loc.setFrozenSize(frozen.size());

  assertx(loc.coldCodeSize() == cold.size());
  assertx(loc.frozenCodeSize() == frozen.size());
  return loc;
}

bool canTranslate() {
  return s_numTrans.load(std::memory_order_acquire) <
    Cfg::Jit::GlobalTranslationLimit;
}

static RDS_LOCAL_NO_CHECK(bool, s_jittingTimeLimitExceeded);

uint8_t adjustedLiveThreshold() {
  if (Cfg::Jit::LiveThreshold == 0) return 0;

  static std::atomic<uint8_t> s_cached{Cfg::Jit::LiveThreshold};
  static std::atomic<int64_t> s_lastUpdate{0};

  auto const elapsed = mcgen::liveSecondsElapsed();
  auto lastUpdate = s_lastUpdate.load(std::memory_order_relaxed);
  if (elapsed - lastUpdate > Cfg::Jit::LiveThresholdUpdateIntervalSecs &&
      s_lastUpdate.compare_exchange_strong(
          lastUpdate, elapsed, std::memory_order_relaxed)) {
    auto const bytesUsed = getLiveTotalUsage();
    auto const expectedBytesUsed =
      (bytesUsed + code().totalUnassigned()) * elapsed / Cfg::Jit::MaxLiveSeconds;
    auto const nextLiveThreshold =
      static_cast<int>(s_cached.load(std::memory_order_relaxed)) +
      (bytesUsed < expectedBytesUsed ? -Cfg::Jit::LiveThresholdStep : Cfg::Jit::LiveThresholdStep);
    s_cached.store(static_cast<uint8_t>(std::clamp(
      nextLiveThreshold, 1, static_cast<int>(Cfg::Jit::LiveThreshold))),
      std::memory_order_relaxed);
  }
  return s_cached.load(std::memory_order_relaxed);
}

static double liveStreakScale(size_t streakSize) {
  static_assert(
    std::is_same_v<decltype(Cfg::Jit::LiveStreakMaxSize), uint8_t>);
  static const auto scales = folly::make_array_with<256>([](size_t i) {
    return std::pow(Cfg::Jit::LiveStreakRatio, i);
  });
  return scales[std::min(streakSize, scales.size() - 1)];
}

TranslationResult::Scope shouldTranslateNoSizeLimit(SrcKey sk, TransKind kind,
                                                    bool noTheshold) {
  // If we've hit Eval.JitGlobalTranslationLimit, then we stop translating.
  if (!canTranslate()) return TranslationResult::Scope::Process;

  if (*s_jittingTimeLimitExceeded) return TranslationResult::Scope::Request;

  auto const maxTransTime = Cfg::Jit::MaxRequestTranslationTime;
  if (maxTransTime >= 0 &&
      Cfg::Server::Mode &&
      !mcgen::isAsyncJitEnabled(kind)) {
    auto const transCounter = Timer::CounterValue(Timer::mcg_translate);
    if (transCounter.wall_time_elapsed >= maxTransTime) {
      if (Trace::moduleEnabledRelease(Trace::mcg, 1)) {
        Trace::traceRelease("Skipping translation. "
                            "Time budget of %" PRId64 " exceeded. "
                            "%" PRId64 "us elapsed. "
                            "%" PRId64 " translations completed\n",
                            maxTransTime,
                            transCounter.wall_time_elapsed,
                            transCounter.count);
      }
      *s_jittingTimeLimitExceeded = true;
      return TranslationResult::Scope::Request;
    }
  }

  auto const func = sk.func();
  if (!Cfg::HHIR::AlwaysInterpIgnoreHint &&
      func->userAttributes().count(s_AlwaysInterp.get())) {
    SKTRACE(2, sk,
            "punting because function is annotated with __ALWAYS_INTERP\n");
    return TranslationResult::Scope::Process;
  }


  // Do not translate functions from units marked as interpret-only.
  if (func->unit()->isInterpretOnly()) {
    return TranslationResult::Scope::Transient;
  }

  // Never translate functions in user-supplied blocklist.
  if (!Cfg::Jit::FuncBlockList.empty()) {
    // Calling func->fullName() could create a static string. So do our own
    // concatenation here to avoid wasting low memory.
    std::string fullName;
    if (auto cls = func->cls()) {
      fullName = cls->name()->toCppString() + "::";
    }
    fullName.append(func->name()->data());
    if (Cfg::Jit::FuncBlockList.contains(fullName)) {
      return TranslationResult::Scope::Process;
    }
  }

  // Refuse to JIT Live translations if Eval.JitPGOOnly is enabled.
  if (Cfg::Jit::PGOOnly &&
      (kind == TransKind::Live || kind == TransKind::LivePrologue)) {
    return TranslationResult::Scope::Transient;
  }

  // Refuse to JIT Live / Profile translations for a function until
  // Eval.JitLiveThreshold / Eval.JitProfileThreshold is hit.
  auto const isLive = kind == TransKind::Live ||
                      kind == TransKind::LivePrologue;
  auto const isProf = kind == TransKind::Profile ||
                      kind == TransKind::ProfPrologue;
  auto const hasLiveTransBytesLimit = Cfg::Jit::FuncLiveTranslationByteLimit != std::numeric_limits<uint32_t>::max();
  if (isLive && hasLiveTransBytesLimit) {
    auto const it = g_funcLiveTransBytes.find(func->getFuncId());
    if (it != g_funcLiveTransBytes.cend() && it->second > Cfg::Jit::FuncLiveTranslationByteLimit) {
      return TranslationResult::Scope::Process;
    }
  }

  auto const funcId = func->getFuncId().toInt();
  auto const inStreak = isLive && tl_liveStreak.contains(funcId);

  auto const softMaxTransTime = Cfg::Jit::SoftMaxRequestTranslationTime;
  if (softMaxTransTime >= 0 &&
      !inStreak &&
      Cfg::Server::Mode &&
      !mcgen::isAsyncJitEnabled(kind)) {
    auto const transCounter = Timer::CounterValue(Timer::mcg_translate);
    if (transCounter.wall_time_elapsed >= softMaxTransTime) {
      if (Trace::moduleEnabledRelease(Trace::mcg, 1)) {
        Trace::traceRelease("Skipping first-time translation. "
                            "Soft time budget of %" PRId64 " exceeded. "
                            "%" PRId64 "us elapsed. "
                            "%" PRId64 " translations completed\n",
                            softMaxTransTime,
                            transCounter.wall_time_elapsed,
                            transCounter.count);
      }
      return TranslationResult::Scope::Request;
    }
  }

  if (!noTheshold && (isLive || isProf)) {
    auto const baseThreshold = isLive ? adjustedLiveThreshold()
                                      : Cfg::Jit::ProfileThreshold;
    auto const funcThreshold = inStreak ? 0 : isLive
        ? static_cast<uint8_t>(
              baseThreshold * liveStreakScale(tl_liveStreak.size()))
        : baseThreshold;
    if (func->incJitReqCount() < funcThreshold) {
      tl_liveStreak.clear();
      return TranslationResult::Scope::Transient;
    }
  }

  if (isLive && !inStreak) {
    tl_liveStreak.add(funcId);
  }
  return TranslationResult::Scope::Success;
}

static std::atomic<bool> s_TCisFull{false};

TranslationResult::Scope shouldTranslate(SrcKey sk, TransKind kind,
                                         bool noThreshold) {
  if (threadIsFull() || tcIsFull()) {
    return TranslationResult::Scope::Process;
  }

  if (*s_jittingTimeLimitExceeded) {
    return TranslationResult::Scope::Request;
  }

  const bool reachedMaxLiveMainLimit =
    getLiveMainUsage() >= Cfg::Jit::MaxLiveMainUsage || mcgen::liveMaxSecondsExpired();
  if (!reachedMaxLiveMainLimit) {
    return shouldTranslateNoSizeLimit(sk, kind, noThreshold);
  }

    // Set this so that next checks are faster.
  s_TCisFull.store(true, std::memory_order_release);

  return TranslationResult::Scope::Process;
}

bool newTranslation() {
  if (s_numTrans.fetch_add(1, std::memory_order_acq_rel) >=
      Cfg::Jit::GlobalTranslationLimit) {
    return false;
  }
  return true;
}

std::unique_lock<SimpleMutex> lockCode(bool lock) {
  if (lock) return std::unique_lock<SimpleMutex>{ s_codeLock };
  return std::unique_lock<SimpleMutex>{s_codeLock, std::defer_lock};
}

std::unique_lock<SimpleMutex> lockMetadata(bool lock) {
  if (lock) return std::unique_lock<SimpleMutex>{s_metadataLock};
  return std::unique_lock<SimpleMutex>{s_metadataLock, std::defer_lock};
}

CodeMetaLock::CodeMetaLock(bool f) :
    m_code(lockCode(f)),
    m_meta(lockMetadata(f)) {
}

void CodeMetaLock::lock() {
  m_code.lock();
  m_meta.lock();
}

void CodeMetaLock::unlock() {
  m_meta.unlock();
  m_code.unlock();
}

void assertOwnsCodeLock(OptView v) {
  if (!v || (!v->isLocal() && !v->isOwned())) s_codeLock.assertOwnedBySelf();
}
void assertOwnsMetadataLock() { s_metadataLock.assertOwnedBySelf(); }

void requestInit() {
  regState() = VMRegState::CLEAN;
  Timer::RequestInit();
  memset(rl_perf_counters.getCheck(), 0, sizeof(PerfCounters));
  Stats::init();
  requestInitProfData();
  *s_initialTCSize.getCheck() = g_code->totalUsed();
  assertx(!g_unwind_rds.isInit());
  memset(g_unwind_rds.get(), 0, sizeof(UnwindRDS));
  g_unwind_rds.markInit();
  *s_jittingTimeLimitExceeded.getCheck() = false;
}

void requestExit() {
  Stats::dump();
  Stats::clear();
  if (Cfg::Jit::ProfileGuardTypes) {
    logGuardProfileData();
  }
  Timer::RequestExit();
  if (profData()) profData()->maybeResetCounters();
  requestExitProfData();

  reportJitMaturity();

  if (Trace::moduleEnabledRelease(Trace::mcgstats, 1)) {
    Trace::traceRelease("MCGenerator perf counters for %s:\n",
                        g_context->getRequestUrl(50).c_str());
    for (int i = 0; i < tpc_num_counters; i++) {
      Trace::traceRelease("%-20s %10" PRId64 "\n",
                          kPerfCounterNames[i], rl_perf_counters[i]);
    }
    Trace::traceRelease("\n");
  }
}

void codeEmittedThisRequest(size_t& requestEntry, size_t& now) {
  requestEntry = *s_initialTCSize;
  now = g_code->totalUsed();
}

void processInit() {
  auto codeLock = lockCode();
  auto metaLock = lockMetadata();

  g_code = new(low_malloc(sizeof(CodeCache))) CodeCache();
  g_views = new CodeCacheViews(Cfg::Eval::MaxConcurrentCodeViews);
  g_ustubs.emitAll(*g_code, *Debug::DebugInfo::Get());
  g_JitMaturityCounter = ServiceData::createCounter("jit.maturity");

  // Write an .eh_frame section that covers the whole TC.
  initUnwinder(g_code->base(), g_code->codeSize());

  Disasm::ExcludedAddressRange(g_code->base(), g_code->codeSize());

  recycleInit();
}

void processExit() {
  recycleStop();
}

void recordWorkloadChange() {
  if (Cfg::Eval::WorkloadAwareMaturity && getJitMaturity() < g_maxJitMaturity) {
    g_maxJitMaturity = 99;
  }
}

int getJitMaturity() {
  if (!g_JitMaturityCounter) return 0;
  return g_JitMaturityCounter->getValue();
}

bool isValidCodeAddress(TCA addr) {
  return g_code->isValidCodeAddress(addr);
}

void checkFreeProfData() {
  // In PGO mode, we free all the profiling data once the main code area reaches
  // its maximum usage.
  //
  // However, we keep the data around indefinitely in a few special modes:
  // * Eval.EnableReusableTC
  // * TC dumping enabled (Eval.DumpTC/DumpIR/etc.)
  // * Continuous profiling active
  //
  // Finally, when the RetranslateAll mode is enabled, the ProfData is discarded
  // via a different mechanism, after all the optimized translations are
  // generated.

  if (profData() &&
      !Cfg::Eval::EnableReusableTC &&
      !cprof::contProfActive() &&
      (tcIsFull() ||
       getLiveMainUsage() >= Cfg::Jit::MaxLiveMainUsage) &&
      !transdb::enabled() &&
      !mcgen::retranslateAllEnabled()) {
    discardProfData();
  }
}

bool shouldProfileNewFuncs() {
  if (profData() == nullptr) return false;

  // We have one knob to control the number of functions we're allowed to
  // profile:Eval.JitProfileBCSize. We profile new functions till this limit is exceeded.
  int64_t limit = Cfg::Jit::EnableProfileBCSizeMultiplier
                  ? Cfg::Jit::ProfileBCSizeMultiplier * Cfg::Jit::ProfileBCSize
                  : Cfg::Jit::ProfileBCSize;
  return profData()->profilingBCSize() < limit;
}

bool profileFunc(const Func* func) {
  // If retranslateAll has been scheduled (including cases when it is going on,
  // or has finished), we can't emit more Profile translations.  This is to
  // ensure that, when retranslateAll() runs, no more Profile translations are
  // being added to ProfData.
  if (mcgen::retranslateAllScheduled()) return false;

  if (threadIsFull()) return false;

  if (getLiveMainUsage() >= Cfg::Jit::MaxLiveMainUsage) {
    return false;
  }

  if (!shouldPGOFunc(func)) return false;

  if (profData()->optimized(func->getFuncId())) return false;

  // If we already started profiling `func', then we return true and skip the
  // other checks below.
  if (profData()->profiling(func->getFuncId())) return true;

  return shouldProfileNewFuncs();
}

///////////////////////////////////////////////////////////////////////////////

LocalTCBuffer::LocalTCBuffer(Address start, size_t initialSize) {
  TCA fakeStart = code().threadLocalStart();
  auto const sz = initialSize / 4;
  auto initBlock = [&] (DataBlock& block, size_t mxSz, const char* nm) {
    always_assert_flog(sz <= mxSz, "sz: {} mxSz: {}\n", sz, mxSz);
    block.init(fakeStart, start, sz, mxSz, nm);
    fakeStart += mxSz;
    start += sz;
  };
  initBlock(m_main, Cfg::Eval::ThreadTCMainBufferSize,
            "thread local main");
  initBlock(m_cold, Cfg::Eval::ThreadTCColdBufferSize,
            "thread local cold");
  initBlock(m_frozen, Cfg::Eval::ThreadTCFrozenBufferSize,
            "thread local frozen");
  initBlock(m_data, Cfg::Eval::ThreadTCDataBufferSize,
            "thread local data");
}

OptView LocalTCBuffer::view() {
  if (!valid()) return std::nullopt;
  return CodeCache::View(m_main, m_cold, m_frozen, m_data, true, true);
}

////////////////////////////////////////////////////////////////////////////////
// Translator internals
Translator::Translator(SrcKey sk, TransKind kind)
  : sk(sk), kind(kind), unit(), vunit()
{}

Translator::~Translator() = default;

bool Translator::shouldEmitLiveTranslation() {
  computeKind();
  // Avoid a race where we would create a Live translation while
  // retranslateAll is in flight and we haven't generated an
  // Optimized translation yet.
  return !mcgen::retranslateAllPending() || isProfiling(kind);
}

Optional<TranslationResult>
Translator::acquireLeaseAndRequisitePaperwork() {
  if (!shouldEmitLiveTranslation()) {
    return TranslationResult::failTransiently();
  }

  if (auto const p = getCached()) return *p;
  if (threadIsFull()) {
    setCachedForProcessFail();
    return TranslationResult{TranslationResult::Scope::Process};
  }

  // Acquire the appropriate lease otherwise bail to a fallback
  // execution mode (eg. interpreter) by returning a nullptr
  // translation address.
  m_lease.emplace(sk.func(), kind);
  if (!(*m_lease)) return TranslationResult::failTransiently();
  computeKind();  // Recompute the kind in case we are no longer profiling.
  if (!m_lease->checkKind(kind)) return TranslationResult::failTransiently();

  // Check again if we can emit live translations for the given
  // func now that we have the lock.
  if (!shouldEmitLiveTranslation()) {
    return TranslationResult::failTransiently();
  }

  if (auto const s = shouldTranslate(false /*noTheshold*/,
                                     false /*noSizeLimit*/);
      s != TranslationResult::Scope::Success) {
    if (s == TranslationResult::Scope::Process) setCachedForProcessFail();
    if (s == TranslationResult::Scope::Request) {
      m_lease->setDropHint(std::chrono::milliseconds{0});
    }
    return TranslationResult{s};
  }

  if (UNLIKELY(RID().isJittingDisabled())) {
    TRACE(2, "punting because jitting code was disabled\n");
    return TranslationResult::failTransiently();
  }

  // Check for cached one last time since we have all the locks now.
  return getCached();
}

TranslationResult::Scope Translator::shouldTranslate(bool noThreshold,
                                                     bool noSizeLimit) {
  if (kind == TransKind::Invalid) computeKind();
  if (noSizeLimit) {
    return shouldTranslateNoSizeLimit(sk, kind, noThreshold);
  }
  return ::HPHP::jit::tc::shouldTranslate(sk, kind, noThreshold);
}

Optional<TranslationResult>
Translator::translate(Optional<CodeCache::View> view) {
  // If the TransDB is enabled, we allocate TransIDs for all translations in
  // ProfData to keep each translation's ID the same in both ProfData and
  // TransDB.
  if (profData() && (isProfiling(kind) || transdb::enabled())) {
    transId = profData()->allocTransID();
  }

  if (!newTranslation()) return TranslationResult::failForProcess();

  WorkloadStats::EnsureInit();
  WorkloadStats guard(WorkloadStats::InTrans);
  SCOPE_EXIT {
    unit.reset();
    vunit.reset();
  };

  {
    tl_is_jitting = true;
    this->gen();
    SCOPE_EXIT {
      tl_is_jitting = false;
    };
  }

  // Check for translation failure.
  // TODO: categorize failure
  if (!vunit) return TranslationResult::failTransiently();

  Timer timer(Timer::mcg_finishTranslation, nullptr);

  tracing::Block _b{
    "emit-translation",
      [&] {
        return traceProps(*vunit);
      }
  };

  if (!view.has_value()) {
    auto const initialSize = 512;
    m_localBuffer = std::make_unique<uint8_t[]>(initialSize);
    m_localTCBuffer =
      std::make_unique<LocalTCBuffer>(m_localBuffer.get(), initialSize);
    view = m_localTCBuffer->view();
  }
  assertx(view->isLocal());

  // Tag the translation start, and build the trans meta.
  // Generate vasm into the code view, retrying if we fill hot.
  while (true) {
    CGMeta fixups;
    TransLocMaker maker{*view};
    const bool align = isPrologue(kind);
    try {
      view->alignForTranslation(align);
      maker.markStart();
      emitVunit(*vunit, unit.get(), *view, fixups,
                mcgen::dumpTCAnnotation(kind) ? getAnnotations()
                                              : nullptr);
    } catch (const DataBlockFull& dbFull) {
      // Rollback so the area can be used by something else.
      auto const range = maker.rollback();
      auto const bytes = range.main.size() + range.cold.size() +
                         range.frozen.size();
      // There should be few of these.  They mean there is wasted work
      // performing translation for functions that don't have space in the TC.
      logPerfWarning("translation_overflow", 1, [&] (StructuredLogEntry& e) {
        e.setStr("kind", show(kind));
        e.setStr("srckey", show(sk));
        e.setStr("data_block", dbFull.name);
        e.setInt("bytes_dropped", bytes);
      });
      if (Cfg::Server::Mode) {
        Logger::Warning("TC area %s filled up!", dbFull.name.c_str());
      }
      reset();
      return TranslationResult::failForProcess();
    }
    transMeta.emplace(*view);
    transMeta->fixups = std::move(fixups);
    transMeta->range = maker.markEnd();
    break;
  }

  Timer metaTimer(Timer::mcg_finishTranslation_metadata, nullptr);
  if (unit && unit->logEntry()) {
    auto metaLock = lockMetadata();
    logTranslation(this, transMeta->range);
  }

  if (!Cfg::Jit::LogAllInlineRegions.empty()) {
    logFrames(*vunit);
  }

  return std::nullopt;
}

bool Translator::translateSuccess() const {
  return transMeta.has_value();
}

Optional<TranslationResult> Translator::relocate(bool alignMain) {
  assertx(transMeta.has_value());
  // Code emitted directly is relocated during emission (or emitted
  // directly in place).
  if (!transMeta->view.isLocal()) {
    assertx(!Cfg::Eval::EnableReusableTC);
    return std::nullopt;
  }

  WorkloadStats::EnsureInit();
  WorkloadStats guard(WorkloadStats::InTrans);

  auto const& range = transMeta->range;
  auto& fixups = transMeta->fixups;

  RelocationInfo rel;
  {
    auto codeLock = lockCode(!Cfg::Jit::EnableConcurrentCodeViews);
    auto viewLock = Cfg::Jit::EnableConcurrentCodeViews
      ? codeViews().lockView(viewTid)
      : std::unique_lock<std::mutex>();
    std::optional<TransLocMaker> maker;
    std::optional<CodeCache::View> finalView;
    while (true) {
      try {
        finalView = codeViews().view(kind, &range, viewTid,
                                     Cfg::Jit::EnableConcurrentCodeViews);
        CodeReuseBlock crb;
        auto dstView = crb.getMaybeReusedView(finalView.value(), range);
        auto& srcView = transMeta->view;

        maker.emplace(dstView);
        dstView.alignForTranslation(alignMain);
        maker->markStart();
        auto origin = range.data;
        if (!origin.empty()) {
          dstView.data().bytes(origin.size(),
                               srcView.data().toDestAddress(origin.begin()));

          auto dest = maker->dataRange();
          auto oAddr = origin.begin();
          auto dAddr = dest.begin();
          while (oAddr != origin.end()) {
            assertx(dAddr != dest.end());
            rel.recordAddress(oAddr++, dAddr++, 0);
          }
        }

        jit::relocate(rel, dstView.main(), range.main.begin(), range.main.end(),
                      srcView.main(), fixups, AreaIndex::Main);
        jit::relocate(rel, dstView.cold(), range.cold.begin(), range.cold.end(),
                      srcView.cold(), fixups, AreaIndex::Cold);
        if (&srcView.cold() != &srcView.frozen()) {
          jit::relocate(rel, dstView.frozen(), range.frozen.begin(),
                        range.frozen.end(), srcView.frozen(), fixups,
                        AreaIndex::Frozen);
        }

      } catch (const DataBlockFull& dbFull) {
        // Set a flag so we quickly bail from trying to generate new
        // translations next time.
        setTcIsFull(viewTid);
        // Rollback so the area can be used by something else.
        if (maker) {
          maker->rollback();
        }
        auto const bytes = range.main.size() + range.cold.size() +
                           range.frozen.size();
        // There should be few of these.  They mean there is wasted work
        // performing translation for functions that don't have space in the TC.
        logPerfWarning("translation_overflow", 1, [&] (StructuredLogEntry& e) {
          e.setStr("kind", show(kind));
          e.setStr("srckey", show(sk));
          e.setStr("data_block", dbFull.name);
          e.setInt("bytes_dropped", bytes);
        });
        reset();
        return TranslationResult::failForProcess();
      }
      transMeta->range = maker->markEnd();
      transMeta->view = *finalView;
      break;
    }
  }
  adjustForRelocation(rel);
  adjustMetaDataForRelocation(rel, nullptr, fixups);
  adjustCodeForRelocation(rel, fixups);

  return std::nullopt;
}

Optional<TranslationResult> Translator::bindOutgoingEdges() {
  assertx(transMeta.has_value());
  auto& meta = transMeta->fixups;
  for (auto& b : meta.smashableBinds) {
    // We can't bind to fallback translations, so use the stub. they do not
    // exist yet outside of retranslate all, which has a different mechanism
    // to achieve that.
    if (b.fallback) {
      auto const stub = svcreq::getOrEmitStub(
        svcreq::StubType::Retranslate, b.sk, b.spOff);
      if (stub == nullptr) {
        reset();
        return TranslationResult::failForProcess();
      }
      b.smashable.patch(stub);
      continue;
    }

    auto sr = createSrcRec(b.sk, b.spOff);
    if (sr == nullptr) {
      reset();
      return TranslationResult::failForProcess();
    }

    // If the target is translated, bind it.
    if (sr->getTopTranslation()) {
      auto srLock = sr->writelock();
      if (sr->getTopTranslation()) {
        sr->chainFrom(b.smashable);
        continue;
      }
    }

    // May need a stub, so create it.
    auto const stub = svcreq::getOrEmitStub(
      svcreq::StubType::Translate, b.sk, b.spOff);
    if (stub == nullptr) {
      reset();
      return TranslationResult::failForProcess();
    }

    auto srLock = sr->writelock();
    sr->chainFrom(b.smashable, stub);
  }

  return std::nullopt;
}

TranslationResult Translator::publish() {
  assertx(transMeta.has_value());
  transMeta->fixups.processNativeCalls();
  {
    auto metaLock = lockMetadata();
    publishMetaInternal();
    publishCodeInternal();
  }
  transMeta->fixups.processPidMapEntries();
  return TranslationResult{transMeta->range.loc().entry()};
}

void Translator::publishMetaInternal() {
  assertx(transMeta.has_value());
  if (!transMeta->fixups.interceptTCAs.empty()) {
    auto interceptLock = lockGlobalIntercept();
    for (auto& it : transMeta->fixups.interceptTCAs) {
      tc::recordInterceptTCA(it.first, it.second);
    }
    this->publishMetaImpl();
  } else {
    this->publishMetaImpl();
  }
  if (isProfiling(kind)) {
    profData()->setProfiling(sk.func());
  }
  if (Cfg::Eval::LogJitFunctionLivenessSampleRate > 0) {
    auto const prevWasCalled =
      sk.func()->atomicFlags().check(Func::Flags::WasCalled)
      || sk.func()->atomicFlags().set(Func::Flags::WasCalled);
    bool markKindAsLive = isLive(kind) || Cfg::Eval::LogAllTranslationsForLiveness;
    if (markKindAsLive && !prevWasCalled &&
        folly::Random::oneIn(Cfg::Eval::LogJitFunctionLivenessSampleRate)) {
      markFunctionAsLive(sk.func());
    }
  }
}

void Translator::publishCodeInternal() {
  assertx(transMeta.has_value());
  this->publishCodeImpl();
  updateCodeSizeCounters();
}

void incrementLiveFuncTransBytes(FuncId funcId, uint32_t bytes) {
  auto const hasLiveTransBytesLimit = Cfg::Jit::FuncLiveTranslationByteLimit != std::numeric_limits<uint32_t>::max();
  if (!hasLiveTransBytesLimit) return;

  auto prev = g_funcLiveTransBytes[funcId];
  while (!g_funcLiveTransBytes.assign_if_equal(funcId, prev, prev + bytes)) {
    prev = g_funcLiveTransBytes[funcId];
  }
}

bool tcIsFull() {
  if (Cfg::Jit::DataBlockSizeRatio > 0) {
    return s_TCisFull.load(std::memory_order_acquire);
  }

  return code().isAnySectionFull();
}

void setTcIsFull(pthread_t viewTid) {
  if (viewTid == pthread_self()) {
    tl_threadIsFull = true;
  }
  codeViews().setFull(viewTid);

  if (codeViews().numThreadsFull() >= Cfg::Eval::MaxConcurrentCodeViews) {
    s_TCisFull.store(true, std::memory_order_release);
  }
}

bool threadIsFull() {
  return tl_threadIsFull;
}

void cleanupSrcDBKeys(const std::vector<SrcKey>& srcKeys) {
  auto& s = srcDB();
  for (auto const sk : srcKeys) {
    s.erase(sk);
  }
}

////////////////////////////////////////////////////////////////////////////////

}
