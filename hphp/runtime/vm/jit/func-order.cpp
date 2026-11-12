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

#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/runtime/vm/jit/trans-rec.h"
#include "hphp/runtime/vm/jit/types.h"

#include "hphp/util/assertions.h"
#include "hphp/util/boot-stats.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/hfsort.h"
#include "hphp/util/logger.h"
#include "hphp/util/trace.h"

#include <tbb/concurrent_hash_map.h>

#include <vector>

TRACE_SET_MOD(funcorder)

namespace HPHP::jit::FuncOrder {

////////////////////////////////////////////////////////////////////////////////

namespace {

// Cached function order.
std::vector<FuncId> s_funcOrder;

// Cached function callsite counts.
jit::hash_map<FuncId, uint32_t> s_funcTargetCounts;

// Map from calls' return address to the the FuncId of the top-level function
// containing that call.
using CallAddrFuncs = tbb::concurrent_hash_map<TCA,FuncId>;
CallAddrFuncs s_callToFuncId;

using FuncPair = std::pair<FuncId::Int,FuncId::Int>;
using CallCounters = tbb::concurrent_hash_map<FuncPair,uint32_t>;
CallCounters s_callCounters;

// Map that keeps track of the size of optimized translations/prologues for each
// function.
using FuncSizes = tbb::concurrent_hash_map<FuncId::Int,uint32_t>;
FuncSizes s_funcSizes;

////////////////////////////////////////////////////////////////////////////////

uint32_t getFuncSize(FuncId funcId) {
  FuncSizes::const_accessor acc;
  return s_funcSizes.find(acc, funcId.toInt()) ? acc->second : 1; // avoid div by 0
}

hfsort::TargetGraph
createCallGraphFromProfCode(jit::hash_map<hfsort::TargetId, FuncId>& funcID) {
  ProfData::Session pds;

  using hfsort::TargetId;

  hfsort::TargetGraph cg;
  jit::hash_map<FuncId, TargetId> targetID;
  auto const pd = profData();
  assertx(pd);

  // Create one node (aka target) for each function that was profiled.
  FTRACE(1, "createCallGraph\n");
  pd->forEachProfilingFunc([&](auto const& func) {
    always_assert(func);
    auto const fid = func->getFuncId();
    auto const transIds = pd->funcProfTransIDs(func);
    uint32_t size = 1; // avoid zero-sized functions
    uint32_t profCount = 0;
    for (auto transId : transIds) {
      const auto trec = pd->transRec(transId);
      assertx(trec->kind() == TransKind::Profile);
      size += trec->asmSize();
      if (trec->srcKey().anyFuncEntry()) {
        profCount += pd->transCounter(transId);
      }
    }
    // NB: avoid division by 0
    const auto targetId = cg.addTarget(size, profCount ? profCount : 1);
    targetID[fid] = targetId;
    funcID[targetId] = fid;
    FTRACE(1, "  - adding node FuncId = {} => TargetId = {}\n", fid, targetId);
  });

  // Add arcs with weights

  auto addCallerCount = [&] (TCA callAddr,
                             TargetId calleeTargetId,
                             TransID callerTransId,
                             uint64_t& totalCalls) {
    assertx(callerTransId != kInvalidTransID);
    auto const callerFuncId = pd->transRec(callerTransId)->funcId();
    if (!Func::isFuncIdValid(callerFuncId)) return;
    auto const callerTargetId = targetID[callerFuncId];
    auto const callCount = pd->transCounter(callerTransId);
    // Don't create arcs with zero weight
    if (callCount) {
      cg.incArcWeight(callerTargetId, calleeTargetId, callCount);
      totalCalls += callCount;
      FTRACE(1, "  - adding arc @ {} : {} => {} [weight = {}] \n",
             callAddr, callerTargetId, calleeTargetId, callCount);
    }
  };

  auto addCallersCount = [&] (TargetId calleeTargetId,
                              const auto& callerAddrs,
                              uint64_t& totalCalls) {
    for (auto callAddr : callerAddrs) {
      auto const callerTransId = pd->jmpTransID(callAddr);
      if (callerTransId != kInvalidTransID) {
        addCallerCount(callAddr, calleeTargetId, callerTransId, totalCalls);
      }
    }
  };

  jit::hash_map<TargetId, uint64_t> totalSampleCounts;
  pd->forEachProfilingPrologue([&] (auto const& func, auto const _nargs, auto const& transId) {
    always_assert(func);
    auto const fid = func->getFuncId();
    auto const calleeTargetId = targetID[fid];

    uint64_t totalCalls = 0;
    FTRACE(1, "  - processing ProfPrologue w/ transId = {}\n", transId);
    const auto trec = pd->transRec(transId);
    assertx(trec->kind() == TransKind::ProfPrologue);
    {
      auto lock = trec->lockCallerList();
      for (auto const callerTransId : trec->profCallers()) {
        addCallerCount(nullptr, calleeTargetId, callerTransId, totalCalls);
      }
      addCallersCount(calleeTargetId, trec->mainCallers(),  totalCalls);
    }
    totalSampleCounts[calleeTargetId] += totalCalls;
  });

  for (auto const& it : totalSampleCounts) {
    auto samples = cg.getSamples(it.first);
    cg.setSamples(it.first, std::max(it.second, samples));
  }

  cg.normalizeArcWeights();
  return cg;
}

hfsort::TargetGraph
createCallGraphFromOptCode(jit::hash_map<hfsort::TargetId, FuncId>& funcID) {
  using hfsort::TargetId;

  hfsort::TargetGraph cg;
  jit::hash_map<FuncId, TargetId> targetID;

  auto getTargetID = [&] (FuncId fid) {
    auto it = targetID.find(fid);
    if (it != targetID.end()) return it->second;
    auto const funcSize = getFuncSize(fid);
    auto tid = cg.addTarget(funcSize);
    targetID[fid] = tid;
    funcID[tid] = fid;
    return tid;
  };

  // Set Arc weights, adding nodes to the call graph on demand.
  FTRACE(1, "createCallGraphFromOptCode:\n");
  for (auto const& it : s_callCounters) {
    FTRACE(1, "  - weight({} -> {}) = {}\n",
           it.first.first, it.first.second, it.second);
    auto const weight = it.second;
    if (weight == 0) continue; // don't create arcs with zero weight
    auto const callerFid = FuncId::fromInt(it.first.first);
    auto const calleeFid = FuncId::fromInt(it.first.second);
    auto const callerTid = getTargetID(callerFid);
    auto const calleeTid = getTargetID(calleeFid);
    cg.incArcWeight(callerTid, calleeTid, weight);
    cg.addSamples(calleeTid, weight);
  }

  cg.normalizeArcWeights();
  return cg;
}

hfsort::TargetGraph
createCallGraph(jit::hash_map<hfsort::TargetId, FuncId>& funcID) {
  BootStats::Block timer("RTA_create_callgraph",
                         Cfg::Server::Mode);

  // If we have the call counters collected from optimized code use them to
  // build the call graph; otherwise, use the estimates based on profile code.
  return s_callCounters.empty() ? createCallGraphFromProfCode(funcID)
                                : createCallGraphFromOptCode(funcID);
}

void print(const char* fileName,
           const std::vector<hfsort::Cluster>& clusters,
           jit::hash_map<hfsort::TargetId, FuncId>& target2FuncId) {
  FILE* outfile = fopen(fileName, "wt");
  if (!outfile) return;

  for (auto const& cluster : clusters) {
    fprintf(outfile,
            "-------- density = %.3lf (%u / %u) --------\n",
            (double) cluster.samples / cluster.size,
            cluster.samples, cluster.size);
    for (auto const targetId : cluster.targets) {
      auto funcId = target2FuncId[targetId];
      if (!Func::isFuncIdValid(funcId)) continue;
      fprintf(outfile, "%s\n", Func::fromFuncId(funcId)->fullName()->data());
    }
  }
  fclose(outfile);
}

std::pair<std::vector<FuncId>, uint64_t> hfsortFuncs() {
  // Return ordered function id, as well as the base profile count used by
  // inlining.
  std::pair<std::vector<FuncId>, uint64_t> ret;

  const bool serverMode = Cfg::Server::Mode;
  jit::hash_map<hfsort::TargetId, FuncId> target2FuncId;
  auto cg = createCallGraph(target2FuncId);
  for (auto& arc: cg.arcs) {
    auto callee = target2FuncId[arc.dst];
    s_funcTargetCounts[callee]++;
  }

  if (Cfg::Jit::PGODumpCallGraph) {
    Treadmill::Session ts(Treadmill::SessionKind::Retranslate);
    cg.printDot("/tmp/cg-pgo.dot",
                [&](hfsort::TargetId targetId) -> const char* {
                  const auto fid = target2FuncId[targetId];
                  if (!Func::isFuncIdValid(fid)) return "<invalid>";
                  const auto func = Func::fromFuncId(fid);
                  return func->fullName()->data();
                });
    if (serverMode) {
      Logger::Info("retranslateAll: saved call graph at /tmp/cg-pgo.dot");
    }
  }

  std::vector<hfsort::Cluster> clusters;
  if (Cfg::Jit::PGOHFSortPlus) {
    clusters = hfsort::hfsortPlus(cg);
  } else {
    clusters = hfsort::clusterize(cg);
    sort(clusters.begin(), clusters.end(), hfsort::compareClustersDensity);
  }
  if (serverMode) {
    Logger::Info("retranslateAll: finished clusterizing the functions");
  }

  if (Cfg::Jit::PGODumpCallGraph) {
    Treadmill::Session ts(Treadmill::SessionKind::Retranslate);

    print("/tmp/hotfuncs-pgo.txt", clusters, target2FuncId);
    if (serverMode) {
      Logger::Info("retranslateAll: saved sorted list of hot functions at "
                   "/tmp/hotfuncs-pgo.txt");
    }
  }

  uint64_t total = 0;
  for (auto const& target : cg.targets) {
    total += target.samples;
  }
  if (cg.targets.size() > 0) {
    ret.second = total / cg.targets.size();
  }

  ret.first.reserve(cg.targets.size());

  auto const addFuncId = [&] (int tid) {
    auto const funcId = target2FuncId[tid];
    if (!Func::isFuncIdValid(funcId)) return;
    ret.first.push_back(funcId);
  };

  jit::fast_set<hfsort::TargetId> seen;
  for (auto& cluster : clusters) {
    for (auto tid : cluster.targets) {
      seen.emplace(tid);
      addFuncId(tid);
    }
  }
  if (auto const extra = cg.targets.size() - seen.size()) {
    if (serverMode) {
      Logger::Info("retranslateAll: %lu functions had no samples!", extra);
    }
    for (int i = 0; i < cg.targets.size(); ++i) {
      if (!seen.contains(i)) {
        addFuncId(i);
      }
    }
  }
  // assert that there is no duplicate in sortedFuncIds.
  assertx(ret.first.size() ==
          std::set<FuncId>(ret.first.begin(), ret.first.end()).size());
  return ret;
}

////////////////////////////////////////////////////////////////////////////////

}

const std::vector<FuncId>& get() {
  return s_funcOrder;
}

const jit::hash_map<FuncId, uint32_t>& getTargetCounts() {
  return s_funcTargetCounts;
}

uint64_t compute() {
  auto ret = hfsortFuncs();
  // Append any functions previously in s_funcOrder that are missing in the
  // optimized order, in the same order that s_funcOrder contains them.
  hphp_hash_set<FuncId> optSet;
  optSet.insert(ret.first.begin(), ret.first.end());
  for (auto fid : s_funcOrder) {
    if (!optSet.contains(fid)) ret.first.push_back(fid);
  }
  s_funcOrder = std::move(ret.first);
  return ret.second;
}

void serialize(ProfDataSerializer& ser) {
  write_raw(ser, safe_cast<uint32_t>(s_funcOrder.size()));
  for (auto const funcId : s_funcOrder) {
    write_func_id(ser, funcId);
    write_raw(ser, s_funcTargetCounts[funcId]);
  }
}

void deserialize(ProfDataDeserializer& des) {
  auto const sz = read_raw<uint32_t>(des);
  s_funcOrder.clear();
  s_funcOrder.reserve(sz);
  s_funcTargetCounts.clear();
  s_funcTargetCounts.reserve(sz);
  for (auto i = sz; i > 0; --i) {
    auto funcId = read_func_id(des);
    s_funcOrder.push_back(funcId);
    s_funcTargetCounts[funcId] = read_raw<uint32_t>(des);
  }
}

////////////////////////////////////////////////////////////////////////////////

void setCallFuncId(TCA callRetAddr, FuncId funcId) {
  CallAddrFuncs::accessor acc;
  CallAddrFuncs::value_type val(callRetAddr, funcId);
  if (!s_callToFuncId.insert(acc, val)) {
    always_assert(acc->second == funcId);
  }
}

FuncId getCallFuncId(TCA callRetAddr) {
  CallAddrFuncs::const_accessor acc;
  return s_callToFuncId.find(acc, callRetAddr)
    ? acc->second : FuncId::Invalid;
}

void clearCallFuncId(TCA callRetAddr) {
  s_callToFuncId.erase(callRetAddr);
}

////////////////////////////////////////////////////////////////////////////////

void incCount(const Func* callee, const ActRec* fp) {
  auto const callerRip = reinterpret_cast<TCA>(fp->m_savedRip);
  if (callerRip == nullptr) return;
  auto const caller = getCallFuncId(callerRip);
  if (caller.isInvalid()) return;

  auto pair = FuncPair(caller.toInt(), callee->getFuncId().toInt());
  {
    CallCounters::accessor acc;
    if (!s_callCounters.insert(acc, CallCounters::value_type(pair, 1))) {
      acc->second++;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void recordTranslation(const TransRec& transRec) {
  auto const kind = transRec.kind;
  if (kind != TransKind::Optimize && kind != TransKind::OptPrologue) return;

  auto const funcId = transRec.src.funcID();
  auto const size = transRec.aLen ? transRec.aLen
                                  : transRec.acoldLen ? transRec.acoldLen
                                                      : transRec.afrozenLen;
  FuncSizes::accessor acc;
  if (!s_funcSizes.insert(acc, FuncSizes::value_type(funcId.toInt(), size))) {
    acc->second += size;
  }
}

////////////////////////////////////////////////////////////////////////////////

}
