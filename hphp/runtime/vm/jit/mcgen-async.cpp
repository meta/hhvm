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

#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/vm/func-token.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-controller.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/prof-data-sb.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/runtime/base/perf-warning.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-prologue.h"
#include "hphp/runtime/vm/jit/tc-region.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/type-profile.h"
#include "hphp/runtime/vm/jit/mcgen-async.h"

#include "hphp/util/alloc.h"
#include "hphp/util/data-block.h"
#include "hphp/util/build-info.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/job-queue.h"
#include "hphp/util/managed-arena.h"
#include "hphp/util/match.h"
#include "hphp/util/trace.h"

#include <folly/AtomicHashMap.h>
#include <folly/system/ThreadName.h>

#include <tbb/concurrent_hash_map.h>

TRACE_SET_MOD(async_jit)

namespace HPHP::jit::mcgen {

namespace {

struct SrcKeySetAtomicBool final {
  explicit SrcKeySetAtomicBool(bool value) : b(value) {}
  SrcKeySetAtomicBool(const SrcKeySetAtomicBool& sksab): b(sksab.load()) {}
  SrcKeySetAtomicBool& operator=(const SrcKeySetAtomicBool& sksab) = delete;

  bool compare_exchange_strong(bool value, bool comparand) const {
      return b.compare_exchange_strong(value, comparand,
                                       std::memory_order_relaxed);
  }
  void store(bool value) const { b.store(value, std::memory_order_relaxed); }
  bool load() const { return b.load(std::memory_order_relaxed); }

private:
  mutable std::atomic<bool> b;
};

struct SrcKeySet {
  SrcKeySet() {}
  ~SrcKeySet() { assertx(!inited); }
  SrcKeySet(SrcKeySet&&) = delete;
  SrcKeySet& operator=(SrcKeySet&&) = delete;
  void init() {
    assertx(!inited);

    sks_type = static_cast<SrcKeySetType>(Cfg::Eval::CustomSrcKeyFilter);
    switch (sks_type) {
    case SrcKeySetType::TBB:
      new (&m_tbbm) TBBM();
      break;
    case SrcKeySetType::AHM:
      new (&m_ahm) AHM(Cfg::Eval::AtomicSrcKeyFilterSize);
      break;
    case SrcKeySetType::CHMTenShardBits:
      new (&m_chm10Shards) CHMTenShardBits();
      break;
    case SrcKeySetType::CHMNineShardBits:
      new (&m_chm9Shards) CHMNineShardBits();
      break;
    case SrcKeySetType::CHM:
    default:
      new (&m_chm) CHM();
    }
    inited = true;
  }

  void destroy() {
    assertx(inited);
    always_assert(static_cast<int>(sks_type) == Cfg::Eval::CustomSrcKeyFilter);

    inited = false;
    switch (sks_type) {
    case SrcKeySetType::TBB:
      m_tbbm.~TBBM();
      break;
    case SrcKeySetType::AHM:
      m_ahm.~AHM();
      break;
    case SrcKeySetType::CHMTenShardBits:
      m_chm10Shards.~CHMTenShardBits();
      break;
    case SrcKeySetType::CHMNineShardBits:
      m_chm9Shards.~CHMNineShardBits();
      break;
    case SrcKeySetType::CHM:
    default:
      m_chm.~CHM();
      break;
    }
  }

  bool exists(const SrcKey &sk) {
    assertx(inited);
    always_assert(static_cast<int>(sks_type) == Cfg::Eval::CustomSrcKeyFilter);

    auto const chmExists = [&](auto &map) {
      auto const it = map.find(sk);
      return it != map.end() && it->second;
    };

    auto const tbbExists = [&](auto &map) {
      TBBM::const_accessor cacc;
      return map.find(cacc, sk) && cacc->second.load();
    };

    auto const ahmExists = [&](auto &map) {
      auto enqueuedItr = map.find(sk.toAtomicInt());
      return enqueuedItr != map.end() && enqueuedItr->second;
    };

    switch (sks_type) {
    case SrcKeySetType::TBB:
      return tbbExists(m_tbbm);
    case SrcKeySetType::AHM:
      return ahmExists(m_ahm);
    case SrcKeySetType::CHMTenShardBits:
      return chmExists(m_chm10Shards);
    case SrcKeySetType::CHMNineShardBits:
      return chmExists(m_chm9Shards);
    case SrcKeySetType::CHM:
    default:
      return chmExists(m_chm);
    }
  }

  bool enqueue(const SrcKey &sk) {
    assertx(inited);
    always_assert(static_cast<int>(sks_type) == Cfg::Eval::CustomSrcKeyFilter);

    auto const chmEnqueue = [&](auto &map) {
      return map.insert(sk, true).second ||
             map.assign_if_equal(sk, false, true);
    };

    auto const tbbEnqueue = [&](auto &map) {
      TBBM::const_accessor cacc;
      TBBM::value_type val(sk, false);
      map.insert(cacc, val);

      bool expected = false;
      return cacc->second.compare_exchange_strong(expected, true);
    };

    auto const ahmEnqueue = [&](auto &map) {
      auto [it, ins] = map.emplace(sk.toAtomicInt(), true);
      static std::atomic<bool> signaled{false};
      checkAHMSubMaps(map, "jit queue map", signaled);
      bool expected = false;

      return ins || const_cast<std::atomic<bool> &>(it->second)
                        .compare_exchange_strong(expected, true,
                                                 std::memory_order_relaxed);
    };

    switch (sks_type) {
    case SrcKeySetType::TBB:
      return tbbEnqueue(m_tbbm);
    case SrcKeySetType::AHM:
      return ahmEnqueue(m_ahm);
    case SrcKeySetType::CHMTenShardBits:
      return chmEnqueue(m_chm10Shards);
    case SrcKeySetType::CHMNineShardBits:
      return chmEnqueue(m_chm9Shards);
    case SrcKeySetType::CHM:
    default:
      return chmEnqueue(m_chm);
    }
  }

  void dequeue(const SrcKey &sk) {
    assertx(inited);
    always_assert(static_cast<int>(sks_type) == Cfg::Eval::CustomSrcKeyFilter);

    auto const chmDequeue = [&](auto &map) { return map.assign(sk, false); };
    auto const tbbDequeue = [&](auto &map) {
      TBBM::const_accessor cacc;
      always_assert(map.find(cacc, sk) &&
                    cacc->second.load());
      cacc->second.store(false);
    };

    auto const ahmDequeue = [&](auto &map) {
      auto it = map.find(sk.toAtomicInt());
      always_assert(it->second.load(std::memory_order_relaxed));
      it->second.store(false, std::memory_order_relaxed);
    };

    switch (sks_type) {
    case SrcKeySetType::TBB:
      tbbDequeue(m_tbbm);
      break;
    case SrcKeySetType::AHM:
      ahmDequeue(m_ahm);
      break;
    case SrcKeySetType::CHMTenShardBits:
      chmDequeue(m_chm10Shards);
      break;
    case SrcKeySetType::CHMNineShardBits:
      chmDequeue(m_chm9Shards);
      break;
    case SrcKeySetType::CHM:
    default:
      chmDequeue(m_chm);
      break;
    }
  }

private:
  using CHM =
      folly_concurrent_hash_map_simd<SrcKey, bool,
                                     SrcKey::Hasher>; // normal shardBits = 8
  using CHMNineShardBits =
      folly_concurrent_hash_map_simd<SrcKey, bool, SrcKey::Hasher,
                                     std::equal_to<SrcKey>,
                                     std::allocator<uint8_t>, 9>;
  using CHMTenShardBits =
      folly_concurrent_hash_map_simd<SrcKey, bool, SrcKey::Hasher,
                                     std::equal_to<SrcKey>,
                                     std::allocator<uint8_t>, 10>;
  using AHM = folly::AtomicHashMap<SrcKey::AtomicInt, std::atomic<bool>>;
  using TBBM = tbb::concurrent_hash_map<SrcKey, SrcKeySetAtomicBool,
                                        SrcKey::TbbHashCompare>;


  enum class SrcKeySetType : int {
    CHM = 0,
    CHMNineShardBits = 1,
    CHMTenShardBits = 2,
    AHM = 3,
    TBB = 4,
  };

  bool inited{false};
  SrcKeySetType sks_type;

  union {
    CHM m_chm;
    CHMNineShardBits m_chm9Shards;
    CHMTenShardBits m_chm10Shards;
    AHM m_ahm;
    TBBM m_tbbm;
  };
};

SrcKeySet s_enqueuedSKs;
SrcKeySet& enqueuedSKs() {
  static const auto enqueuedSKs = [] {
      s_enqueuedSKs.init();
      return &s_enqueuedSKs;
    }();
  always_assert(enqueuedSKs);

  return *enqueuedSKs;
}

struct AsyncRegionTranslationContext {
  AsyncRegionTranslationContext(TransKind kind,
                                RegionContext&& ctx,
                                int currNumTranslations,
                                pthread_t requestTid)
    : kind(kind)
    , ctx(ctx)
    , currNumTranslations(currNumTranslations)
    , requestTid(requestTid)
  {}

  TransKind kind;
  RegionContext ctx;
  int currNumTranslations;
  pthread_t requestTid;
};

struct AsyncPrologueContext {
  AsyncPrologueContext(
    TransKind kind,
    FuncId funcId,
    int nPassed,
    bool forJumpstart,
    pthread_t requestTid
  ) : kind(kind)
    , funcId(funcId)
    , nPassed(nPassed)
    , forJumpstart(forJumpstart)
    , requestTid(requestTid)
  {}

  TransKind kind;
  FuncId funcId;
  int nPassed;
  bool forJumpstart;
  pthread_t requestTid;
};

struct AsyncOptimizeContext {
  AsyncOptimizeContext(const Func* func): funcToken(func->getFuncToken()) {}
  std::shared_ptr<FuncToken> funcToken;
};

namespace {
std::atomic<size_t> s_successTrans;
std::atomic<size_t> s_failTrans;
std::atomic<size_t> s_failSrcRec;
std::atomic<size_t> s_permFailTrans;
std::atomic<size_t> s_rejectTrans;
std::atomic<size_t> s_activeTrans;
std::atomic<size_t> s_interpTrans;

std::atomic<size_t> s_successPrologue;
std::atomic<size_t> s_failPrologue;
std::atomic<size_t> s_permFailPrologue;
std::atomic<size_t> s_rejectPrologue;
std::atomic<size_t> s_activePrologue;

InitFiniNode s_logJitStats([]{
  if ((!Cfg::Eval::EnableAsyncJIT && !Cfg::Eval::EnableAsyncJITLive &&
       !Cfg::Eval::EnableAsyncJITProfile) ||
      !isStandardRequest() ||
      !StructuredLog::coinflip(Cfg::Eval::AsyncJitLogStatsRate)) return;
  StructuredLogEntry ent;

  constexpr auto MO = std::memory_order_relaxed;
  ent.setInt("succeeded_trans", s_successTrans.load(MO));
  ent.setInt("failed_src_rec", s_failSrcRec.load(MO));
  ent.setInt("failed_trans", s_failTrans.load(MO));
  ent.setInt("permanent_failed_trans", s_permFailTrans.load(MO));
  ent.setInt("rejected_trans", s_rejectTrans.load(MO));
  ent.setInt("active_trans", s_activeTrans.load(MO));
  ent.setInt("interp_trans", s_interpTrans.load(MO));

  ent.setInt("succeeded_prologue", s_successPrologue.load(MO));
  ent.setInt("failed_prologue", s_failPrologue.load(MO));
  ent.setInt("permanent_failed_prologue", s_permFailPrologue.load(MO));
  ent.setInt("rejected_prologue", s_rejectPrologue.load(MO));
  ent.setInt("active_prologue", s_activePrologue.load(MO));

  ent.setInt("main_used_bytes", tc::code().mainUsed());
  ent.setInt("cold_used_bytes", tc::code().coldUsed());
  ent.setInt("frozen_used_bytes", tc::code().frozenUsed());
  ent.setInt("data_used_bytes", tc::code().dataUsed());

  ent.setInt("request_index", requestCount());
  ent.setInt("reached_limit", tc::canTranslate() ? 0 : 1);

  ent.setInt("sample_rate", Cfg::Eval::AsyncJitLogStatsRate);

  StructuredLog::log("hhvm_async_jit_stats", ent);
}, InitFiniNode::When::RequestFini, "logJitStats");
}

using AsyncTranslationContext = std::variant<
  AsyncRegionTranslationContext,
  AsyncPrologueContext,
  AsyncOptimizeContext
>;

constexpr int kIgnoreNumTrans = -1;

struct AsyncTranslationWorker
  : JobQueueWorker<AsyncTranslationContext, void*, true, true> {

  AsyncTranslationWorker()
    : m_codeBuffer(std::make_unique<uint8_t[]>(initialSize))
  {}

  void doAsyncOptimize(AsyncOptimizeContext& ctx) {
    ProfileNonVMThread nonVM;
    HphpSession hps{Treadmill::SessionKind::TranslateWorker};
    VMProtect _;
    ViewHolder viewer{pthread_self(), true};

    auto func = ctx.funcToken->getFunc();
    if (func == nullptr) return;
    SCOPE_EXIT { func->atomicFlags().unset(Func::Flags::LockedForAsyncJit); };

    if (!profData()) return;
    assertx(!profData()->optimized(func));

    profData()->setOptimized(func);

  cprof::maybeCaptureContProfProfile(*profData(), *func, true /* async */);

    optimizeFunc(const_cast<Func*>(func));
  }

  void doAsyncRegionTranslation(AsyncRegionTranslationContext& rctx) {
    auto const ctx = rctx.ctx;

    s_activeTrans++;
    SCOPE_EXIT {
      if (!Cfg::Repo::Authoritative) {
        assertx(Cfg::Eval::EnableAsyncJIT);
        // Dequeue for both normal and jumpstart requests so the SrcKey can be
        // re-enqueued if a future retranslation is needed.
        enqueuedSKs().dequeue(ctx.sk);
      } else {
        assertx(Cfg::Eval::EnableAsyncJITLive ||
                Cfg::Eval::EnableAsyncJITProfile);
        ctx.sk.func()->atomicFlags().unset(Func::Flags::LockedForAsyncJit);
      }
      s_activeTrans--;
    };

    ProfileNonVMThread nonVM;
    HphpSession hps{Treadmill::SessionKind::TranslateWorker};
    VMProtect _;
    auto const viewTid = isProfiling(rctx.kind) ? pthread_self()
                                                : rctx.requestTid;
    ViewHolder viewer{viewTid, true};

    if (!Func::isFuncIdValid(ctx.sk.funcID())) {
      FTRACE(2, "Invalid func id {}\n", ctx.sk.funcID().toInt());
      return;
    }

    if (ctx.sk.func()->atomicFlags().check(Func::Flags::Zombie)) {
      FTRACE(2, "Zombie function {}\n", ctx.sk.func()->fullName());
      return;
    }

    auto const srcRec = tc::createSrcRec(ctx.sk, ctx.spOffset);
    if (!srcRec) {
      FTRACE(2, "createSrcRec failed for sk {}\n", show(ctx.sk));
      s_failSrcRec++;
      return;
    }
    auto const numTrans = srcRec->numTrans();
    auto const ctxNumTrans = rctx.currNumTranslations;
    // If new translations were created since this request was enqueued,
    // let the next execution run through the retranslation chain.
    // ctxNumTrans < 0 indicates that the request was enqueued during jumpstart
    // and therefore should be processed regardless.
    if (ctxNumTrans >= 0 && ctxNumTrans != numTrans) {
      FTRACE(2, "New translation found for sk {}\n", show(ctx.sk));
      return;
    }

    tc::RegionTranslator translator(ctx.sk, rctx.kind);
    translator.viewTid = viewTid;
    translator.spOff = ctx.spOffset;
    translator.liveTypes = ctx.liveTypes;
    auto const noThreshold = ctxNumTrans == kIgnoreNumTrans;
    if (!translator.shouldEmitLiveTranslation()) {
      FTRACE(2, "shouldEmitLiveTranslation failed for sk {}\n", show(ctx.sk));
      s_rejectTrans++;
      return;
    }

    if (isLive(translator.kind) &&
        translator.exceededMaxLiveTranslations(numTrans)) {
      FTRACE(2, "Max translations reached for sk {}\n", show(ctx.sk));
      translator.setCachedForProcessFail();
      s_permFailTrans++;
      return;
    }

    if (auto const s = translator.shouldTranslate(noThreshold,
                                                  false /*noSizeLimit*/);
      s != TranslationResult::Scope::Success) {
      s_rejectTrans++;
      FTRACE(2, "shouldTranslate failed for sk {}\n", show(ctx.sk));
      if (s == TranslationResult::Scope::Process) {
        translator.setCachedForProcessFail();
        s_permFailTrans++;
      }
      return;
    }

    tc::LocalTCBuffer buf {m_codeBuffer.get(), initialSize};
    auto const result = [&] {
      if (auto const res = translator.translate(buf.view())) return *res;
      if (auto const res = translator.relocate(false)) return *res;
      if (auto const res = translator.bindOutgoingEdges()) return *res;
      return  translator.publish();
    }();
    if (result.scope() == TranslationResult::Scope::Success) {
      if (translator.region && !translator.region->empty()) {
        FTRACE(2, "start: {} | end: {}\n",
               show(translator.region->start()),
               show(translator.region->lastSrcKey()));
        s_successTrans++;
      }
      if (translator.kind == TransKind::Interp) s_interpTrans++;
      FTRACE(2, "Successfully generated translation for sk {}\n", show(ctx.sk));

      if (Cfg::Eval::EnableSBProfSerialize) {
        addToSBProfile(rctx.ctx);
      }
    } else {
      FTRACE(2, "Background jitting failed for sk {}\n", show(ctx.sk));
      s_failTrans++;
    }
  }

  void doAsyncPrologueGen(AsyncPrologueContext& ctx) {
    ProfileNonVMThread nonVM;
    HphpSession hps{Treadmill::SessionKind::TranslateWorker};
    VMProtect _;
    auto const viewTid = isProfiling(ctx.kind) ? pthread_self()
                                               : ctx.requestTid;
    ViewHolder viewer{viewTid, true};

    if (!Func::isFuncIdValid(ctx.funcId)) {
      FTRACE(2, "Invalid func id {}\n", ctx.funcId.toInt());
      return;
    }

    auto const func = const_cast<Func*>(Func::fromFuncId(ctx.funcId));

    SCOPE_EXIT {
      if (!ctx.forJumpstart) {
        func->atomicFlags().unset(Func::Flags::LockedForPrologueGen);
      }
      s_activePrologue--;
    };

    s_activePrologue++;

    if (func->atomicFlags().check(Func::Flags::Zombie)) {
      FTRACE(2, "Zombie function {}\n", func->fullName());
      return;
    }

    tc::PrologueTranslator translator(func, ctx.nPassed, ctx.kind);
    translator.viewTid = viewTid;

    auto const tcAddr = translator.getCached();
    if (tcAddr) {
      FTRACE(2, "getCached returned true for prologue of func {}\n",
             func->name());
      return;
    }

    if (!translator.shouldEmitLiveTranslation()) {
      FTRACE(2, "shouldEmitLiveTranslation failed for func prologue {}\n",
             func->name());
      s_rejectPrologue++;
      return;
    }

    if (auto const s =
        translator.shouldTranslate(ctx.forJumpstart /*noThreshold */,
                                   false /*noSizeLimit*/);
        s != TranslationResult::Scope::Success) {
      FTRACE(2, "shouldTranslate failed for func prologue {}\n",
             func->name());
      s_rejectPrologue++;
      if (s == TranslationResult::Scope::Process) {
        translator.setCachedForProcessFail();
        s_permFailPrologue++;
      }
      return;
    }

    tc::LocalTCBuffer buf {m_codeBuffer.get(), initialSize};
    auto const result = [&] {
      if (auto const res = translator.translate(buf.view())) return *res;
      if (auto const res = translator.relocate(true)) return *res;
      if (auto const res = translator.bindOutgoingEdges()) return *res;
      return translator.publish();
    }();
    if (result.scope() == TranslationResult::Scope::Success) {
      FTRACE(2, "Successfully generated prologue for func {}\n",
             func->name());
      s_successPrologue++;

      if (Cfg::Eval::EnableSBProfSerialize) {
        addToSBProfile(func, ctx.nPassed);
      }
    } else {
      FTRACE(2, "Background prologue generation failed for func {}\n",
             func->name());
      s_failPrologue++;
    }
  }

  void doJob(AsyncTranslationContext ctx) override {
    match(
      ctx,
      [&] (AsyncRegionTranslationContext& r) { doAsyncRegionTranslation(r); },
      [&] (AsyncPrologueContext& p) { doAsyncPrologueGen(p); },
      [&] (AsyncOptimizeContext& o) { doAsyncOptimize(o); }
    );
  }

  void onThreadEnter() override {
    folly::setThreadName("asyncjitworker");
#if USE_JEMALLOC
    if (auto arena = next_extra_arena(s_numaNode)) {
      arena->bindCurrentThread();
    }
#endif
  }

  static constexpr int initialSize = 4096;
  std::unique_ptr<uint8_t[]> m_codeBuffer;
};
} // namespace

using AsyncTranslationDispatcher = JobQueueDispatcher<AsyncTranslationWorker>;
AsyncTranslationDispatcher& dispatcher() {
  static const auto dispatcher = [] {
    auto d = std::make_unique<AsyncTranslationDispatcher>(
      Cfg::Eval::AsyncJitWorkerThreads,
      Cfg::Eval::AsyncJitWorkerThreads, 0, false, nullptr
    );
    d->start();
    return d;
  }();
  always_assert(dispatcher);
  return *dispatcher;
}
void joinAsyncTranslationWorkerThreads() {
  FTRACE(2, "Waiting for background jit worker threads to finish\n");
  dispatcher().waitEmpty(true);
  enqueuedSKs().destroy();
}
void waitForAsyncTranslationWorkerThreadsToEmpty() {
  FTRACE(2, "Waiting for background jit worker threads to empty\n");
  dispatcher().waitEmpty(false);
}
void enqueueAsyncTranslateRequestForJumpstart(RegionContext&& ctx) {
  // Use the SrcKeySet to deduplicate jumpstart requests for the same SrcKey.
  // The serialized SBProf data may contain multiple entries for the same
  // SrcKey (e.g. with different live types).  Without dedup, multiple workers
  // can concurrently translate and publish for the same SrcKey, pushing
  // numTrans past MaxTranslations and violating the translation limit
  // invariant.
 if (detail::mayEnqueueAsyncTranslateRequest(ctx.sk)) {
  dispatcher().enqueue(AsyncRegionTranslationContext {
    TransKind::Live, std::move(ctx), kIgnoreNumTrans, pthread_self()
  });
  FTRACE(2, "Enqueued sk {} for jitting in jumpstart\n", show(ctx.sk));
 }
}

namespace {

void logImpl(StructuredLogEntry& ent, const Func* f) {
  ent.setStr("repo_schema", repoSchemaId());
  ent.setStr("file", f->unit()->filepath()->slice());
  ent.setStr("function_name", f->fullName()->slice());
  ent.setInt("did_jumpstart", didDeserializeSBProfData());
  if (auto const cls = f->cls()) ent.setStr("class_name", cls->name()->slice());
}

void log(const Func* f, int nPassed) {
  if (!Cfg::Eval::DumpJitProfileStats) return;

  StructuredLogEntry ent;
  logImpl(ent, f);
  ent.setStr("kind", "prologue");
  ent.setInt("num_passed", nPassed);
  StructuredLog::log("hhvm_async_jit", ent);
}

void log(const RegionContext& ctx) {
  if (!Cfg::Eval::DumpJitProfileStats) return;

  StructuredLogEntry ent;
  logImpl(ent, ctx.sk.func());
  ent.setStr("kind", "region");
  ent.setInt("line", ctx.sk.lineNumber());
  ent.setInt("has_this", ctx.sk.hasThis());
  ent.setStr("resume_mode", resumeModeShortName(ctx.sk.resumeMode()));
  ent.setInt("func_entry", ctx.sk.funcEntry());
  ent.setInt("named_params_func_entry", ctx.sk.namedParamsFuncEntry());
  ent.setInt("offset", ctx.sk.offset());
  ent.setInt("hash", ctx.sk.stableHash());
  if (ctx.sk.anyFuncEntry()) {
    ent.setInt("entry_offset", ctx.sk.entryOffset());
    ent.setInt("entry_num_args", ctx.sk.numEntryArgs());
  }
  ent.setInt("num_live", ctx.liveTypes.size());
  ent.setInt("stack_offset", ctx.spOffset.offset);
  StructuredLog::log("hhvm_async_jit", ent);
}

}

namespace detail {

bool tryReserveView(pthread_t tid) {
  if (!Cfg::Jit::EnableConcurrentCodeViews) return true;
  ViewHolder viewer{tid, false};
  return viewer.isValid();
}

bool mayEnqueueAsyncTranslateRequest(const SrcKey& sk) {
  if (!Cfg::Repo::Authoritative) {
    assertx(Cfg::Eval::EnableAsyncJIT);
    if (!Cfg::Eval::CheckValueBeforeEnqueue) {
      return enqueuedSKs().enqueue(sk);
    } else {
      if (enqueuedSKs().exists(sk)) return false;
      return enqueuedSKs().enqueue(sk);
    }
  } else {
    assertx(Cfg::Eval::EnableAsyncJITLive || Cfg::Eval::EnableAsyncJITProfile);
    auto const func = sk.func();
    return !func->atomicFlags().set(Func::Flags::LockedForAsyncJit);
  }
  return false;
}

void enqueueAsyncTranslateRequest(TransKind kind,
                                  RegionContext&& ctx,
                                  int currNumTranslations,
                                  pthread_t requestTid) {
  dispatcher().enqueue(
    AsyncRegionTranslationContext {kind, std::move(ctx), currNumTranslations,
                                   requestTid}
  );
  FTRACE(2, "Enqueued sk {} for jitting\n", show(ctx.sk));
  log(ctx);
}

}

namespace {
template<bool forJumpstart>
void enqueueAsyncPrologueRequestImpl(TransKind kind, Func* func, int nPassed,
                                     pthread_t requestTid) {
  if (forJumpstart ||
      !func->atomicFlags().set(Func::Flags::LockedForPrologueGen)) {
    dispatcher().enqueue(
      AsyncPrologueContext {kind, func->getFuncId(), nPassed, forJumpstart,
                            requestTid});
    FTRACE(2, "Enqueued func {} for prologue generation\n", func->name());
  } else {
    FTRACE(2,
      "In progress prologue generation, skipping enqueue for func {}\n",
      func->name()
    );
  }
}
}

void enqueueAsyncPrologueRequest(TransKind kind, Func* func, int nPassed,
                                 pthread_t requestTid) {
  if (!isProfiling(kind) && !detail::tryReserveView(requestTid)) {
    FTRACE(2, "No code view slot available, skipping prologue enqueue for func {}\n",
           func->name());
    return;
  }
  enqueueAsyncPrologueRequestImpl<false>(kind, func, nPassed, requestTid);
  log(func, nPassed);
}

void enqueueAsyncPrologueRequestForJumpstart(Func* func, int nPassed) {
  enqueueAsyncPrologueRequestImpl<true>(TransKind::Live, func, nPassed,
                                        pthread_self());
}

void enqueueAsyncTranslateOptRequest(const Func* func) {
  auto const id = func->getFuncId();
  if (!profData() || profData()->optimized(id)) return;

  if (!func->atomicFlags().set(Func::Flags::LockedForAsyncJit)) {
    dispatcher().enqueue(AsyncOptimizeContext { func });
  }
}

bool isAsyncJitEnabled(TransKind kind) {
  if (!Cfg::Repo::Authoritative) {
    return Cfg::Eval::EnableAsyncJIT;
  } else {
    return (isLive(kind) && Cfg::Eval::EnableAsyncJITLive) ||
           (isProfiling(kind) && Cfg::Eval::EnableAsyncJITProfile);
  }
}

}
