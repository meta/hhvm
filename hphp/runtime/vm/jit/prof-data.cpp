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

#include "hphp/runtime/vm/jit/prof-data.h"

#include <vector>

#include <folly/MapUtil.h>

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/logger.h"

#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/vasm-block-counters.h"
#include "hphp/runtime/vm/prologue-id.h"
#include "hphp/runtime/vm/verifier/cfg.h"

namespace HPHP::jit {

TRACE_SET_MOD(pgo)

////////////////////////////////////////////////////////////////////////////////

ProfTransRec::ProfTransRec(SrcKey lastSk, SrcKey sk, RegionDescPtr region,
                           uint32_t asmSize)
    : m_kind(TransKind::Profile)
    , m_asmSize(asmSize)
    , m_lastSk(lastSk)
    , m_sk(sk)
    , m_region(region) {
  assertx(region != nullptr && !region->empty() && region->start() == sk);
}

ProfTransRec::ProfTransRec(SrcKey sk, uint32_t nArgs, uint32_t asmSize)
    : m_kind(TransKind::ProfPrologue)
    , m_asmSize(asmSize)
    , m_prologueArgs(nArgs)
    , m_sk(sk)
    , m_callers{}
{
  m_callers = std::make_unique<CallerRec>();
}

ProfTransRec::~ProfTransRec() {
  if (m_kind == TransKind::Profile) {
    m_region.~RegionDescPtr();
    return;
  }
  assertx(m_kind == TransKind::ProfPrologue);
  m_callers.~CallerRecPtr();
}

////////////////////////////////////////////////////////////////////////////////

template<typename Map>
typename Map::Config makeAHMConfig() {
  typename Map::Config config;
  config.growthFactor = 1;
  config.entryCountThreadCacheSize = 10;
  return config;
}

ProfData::ProfData()
  : m_counters(Cfg::Server::Mode
                 ? std::numeric_limits<int64_t>::max()
                 : Cfg::Jit::PGOThreshold)
  , m_profilingFuncs(Cfg::Eval::PGOFuncCountHint,
                     makeAHMConfig<decltype(m_profilingFuncs)>())
  , m_optimizedSKs(Cfg::Eval::PGOFuncCountHint,
                   makeAHMConfig<decltype(m_optimizedSKs)>())
  , m_proflogueDB(Cfg::Eval::PGOFuncCountHint * 10,
                  makeAHMConfig<decltype(m_proflogueDB)>())
  , m_dvFuncletDB(Cfg::Eval::PGOFuncCountHint * 2,
                  makeAHMConfig<decltype(m_dvFuncletDB)>())
  , m_jmpToTransID(Cfg::Eval::PGOFuncCountHint * 10,
                   makeAHMConfig<decltype(m_jmpToTransID)>())
  , m_blockEndOffsets(Cfg::Eval::PGOFuncCountHint,
                      makeAHMConfig<decltype(m_blockEndOffsets)>())
{}

TransID ProfData::allocTransID() {
  std::unique_lock lock{m_transLock};
  m_transRecs.emplace_back();
  return m_transRecs.size() - 1;
}

TransID ProfData::proflogueTransId(const Func* func, uint32_t nArgs) const {
  auto const numParams = func->numNonVariadicParams();
  if (nArgs > numParams) nArgs = numParams + 1;

  return folly::get_default(
    m_proflogueDB,
    PrologueID{func, nArgs},
    kInvalidTransID
  );
}

TransID ProfData::dvFuncletTransId(SrcKey sk) const {
  assertx(sk.anyFuncEntry());
  return folly::get_default(
    m_dvFuncletDB,
    sk.toAtomicInt(),
    kInvalidTransID
  );
}

void ProfData::addTransProfile(TransID transID,
                               const RegionDescPtr& region,
                               const PostConditions& pconds,
                               uint32_t asmSize) {
  auto const lastSk = region->lastSrcKey();

  assertx(region);
  DEBUG_ONLY auto const nBlocks = region->blocks().size();
  assertx(nBlocks == 1);
  region->renumberBlock(region->entry()->id(), transID);
  for (auto& b : region->blocks()) b->setProfTransID(transID);
  region->blocks().back()->setPostConds(pconds);
  auto const startSk = region->start();

  // If the translation corresponds to a DV Funclet, then add an entry
  // into dvFuncletDB.
  auto const func = startSk.func();

  if (startSk.anyFuncEntry() && startSk.entryOffset() != 0) {
    assertx(func->isDVEntry(startSk.entryOffset()));
    // Normal DV funclets don't have type guards, and thus have a single
    // translation.  However, some special functions written in hhas
    // (e.g. array_map) have complex DV funclets that get retranslated for
    // different types.  For those functions, m_dvFuncletDB keeps the TransID
    // for their first translation.
    FuncCleanup::addProfDataSks(startSk);
    m_dvFuncletDB.emplace(startSk.toAtomicInt(), transID);
  }

  {
    std::unique_lock lock{m_transLock};
    m_transRecs[transID].reset(new ProfTransRec(lastSk, startSk, region,
                                                asmSize));
  }

  // Putting transID in m_funcProfTrans makes it visible to other threads, so
  // this has to happen after we've already put its metadata in m_transRecs.
  std::unique_lock lock{m_funcProfTransLock};
  m_funcProfTrans[func].push_back(transID);
}

void ProfData::addTransProfPrologue(TransID transID, SrcKey sk, uint32_t nArgs,
                                    uint32_t asmSize) {
  auto prologueID = PrologueID{sk.funcID(), nArgs};
  FuncCleanup::addProfDataPrologueID(prologueID);

  {
    std::unique_lock lock{m_transLock};
    m_transRecs[transID].reset(new ProfTransRec(sk, nArgs, asmSize));
  }

  m_proflogueDB.emplace(prologueID, transID);
}

void ProfData::addProfTrans(TransID transID,
                            std::unique_ptr<ProfTransRec> ptr) {
  assertx(transID >= m_transRecs.size());
  if (transID > m_transRecs.size()) m_transRecs.resize(transID);
  auto const sk = ptr->srcKey();
  if (ptr->kind() == TransKind::Profile) {
    if (sk.anyFuncEntry() && sk.entryOffset() != 0) {
      assertx(sk.func()->isDVEntry(sk.entryOffset()));
      FuncCleanup::addProfDataSks(sk);
      m_dvFuncletDB.emplace(sk.toAtomicInt(), transID);
    }
    m_funcProfTrans[sk.func()].push_back(transID);
  } else {
    auto prologueID = PrologueID{sk.funcID(), ptr->prologueArgs()};
    FuncCleanup::addProfDataPrologueID(prologueID);
    m_proflogueDB.emplace(prologueID, transID);
  }
  m_transRecs.emplace_back(std::move(ptr));
}

bool ProfData::anyBlockEndsAt(const Func* func, Offset offset) {
  auto it = m_blockEndOffsets.find(func);
  if (it == m_blockEndOffsets.end()) {
    Arena arena;
    Verifier::GraphBuilder builder{arena, func};
    auto cfg = builder.build();
    jit::fast_set<Offset> offsets;

    for (auto blocks = linearBlocks(cfg); !blocks.empty(); ) {
      auto last = blocks.popFront()->last - func->entry();
      offsets.insert(last);
    }

    it = m_blockEndOffsets.emplace(func, std::move(offsets)).first;
  }

  return it->second.contains(offset);
}

////////////////////////////////////////////////////////////////////////////////

namespace {
std::atomic<ProfData*> s_profData{nullptr};
struct ProfDataShutdownDeleter {
  ~ProfDataShutdownDeleter() {
    delete s_profData.load();
  }
} s_profDataShutdownDeleter;

// In case we want to keep ProfData till end of the process.
std::unique_ptr<ProfData> s_profDataHolder{nullptr};

/*
 * Used to free ProfData from the Treadmill.
 */
struct ProfDataTreadmillDeleter {
  void operator()() {
    if (Cfg::Server::Mode) {
      Logger::Info("Deleting JIT ProfData");
    }
  }

  std::unique_ptr<ProfData> data;
};
}

std::atomic_bool ProfData::s_triedDeserialization{false};
std::atomic_bool ProfData::s_wasDeserialized{false};
std::atomic<StringData*> ProfData::s_buildHost{nullptr};
std::atomic<StringData*> ProfData::s_tag{nullptr};
std::atomic<int64_t> ProfData::s_buildTime{0};
std::atomic<size_t> ProfData::s_prevProfSize{0};

ServiceData::ExportedCounter* ProfData::s_optimized_funcs_counter =
  ServiceData::createCounter("jit.optimized_funcs");
ServiceData::ExportedCounter* ProfData::s_tried_deserialze =
  ServiceData::createCounter("jit.tried_deser");
ServiceData::ExportedCounter* ProfData::s_deserialize_succ =
  ServiceData::createCounter("jit.succeeded_deser");

RDS_LOCAL_NO_CHECK(ProfData*, rl_profData)(nullptr);

void processInitProfData() {
  if (!Cfg::Jit::PGO) return;

  s_profData.store(new ProfData(), std::memory_order_release);
}

void requestInitProfData() {
  *rl_profData = s_profData.load(std::memory_order_acquire);
}

void requestExitProfData() {
  *rl_profData = nullptr;
}

const ProfData* globalProfData() {
  return s_profData.load(std::memory_order_acquire);
}

void discardProfData() {
  if (s_profData.load(std::memory_order_acquire) == nullptr) return;

  // Make sure s_profData is nullptr so any new requests won't try to use the
  // object we're deleting, then send it to the Treadmill for deletion.
  auto data = s_profData.exchange(nullptr, std::memory_order_acq_rel);
  if (data != nullptr) {
    s_profDataHolder.reset(data);
    if (!Cfg::Eval::KeepProfData) {
      if (Cfg::Server::Mode) {
        Logger::Info("Putting JIT ProfData on Treadmill");
      }
      Treadmill::enqueue(ProfDataTreadmillDeleter{std::move(s_profDataHolder)});
    }
  }
  // Only free VasmBlockCounters when no JIT translations reference them.
  // During jit-serialize, Optimize translations contain atomic decrements
  // with hardcoded counter addresses that must remain valid. Freeing
  // while these translations are live causes use-after-free corruption.
  if (!isJitSerializing()) {
    Treadmill::enqueue(VasmBlockCounters::free);
  }
}

void ProfData::maybeResetCounters() {
  if (m_countersReset.load(std::memory_order_acquire)) return;
  if (requestCount() < Cfg::Jit::ResetProfCountersRequest) return;

  std::unique_lock lock{m_transLock};
  if (m_countersReset.load(std::memory_order_acquire)) return;
  m_counters.resetAllCounters(Cfg::Jit::PGOThreshold);
  m_countersReset.store(true, std::memory_order_release);
}

void ProfData::addTargetProfile(const ProfData::TargetProfileInfo& info) {
  std::unique_lock lock{m_targetProfilesLock};
  m_targetProfiles[info.key.transId].push_back(info);
}

std::vector<ProfData::TargetProfileInfo> ProfData::getTargetProfiles(
  TransID transID) const {
  std::shared_lock lock{m_targetProfilesLock};
  auto it = m_targetProfiles.find(transID);
  if (it != m_targetProfiles.end()) {
    return it->second;
  } else {
    return std::vector<TargetProfileInfo>{};
  }
}

void ProfData::cleanupFunc(const Func* func,
                           const std::vector<SrcKey>& srcKeys,
                           const std::vector<PrologueID>& prologueIDs) {
  std::unique_lock lock{m_funcProfTransLock};

  m_funcProfTrans.erase(func);
  m_blockEndOffsets.erase(func);
  m_profilingFuncs.erase(func);

  for (auto const sk : srcKeys) {
    m_optimizedSKs.erase(sk.toAtomicInt());
    m_dvFuncletDB.erase(sk.toAtomicInt());
  }

  for (auto const prologueID : prologueIDs) {
    m_proflogueDB.erase(prologueID);
  }
}

////////////////////////////////////////////////////////////////////////////////

}
