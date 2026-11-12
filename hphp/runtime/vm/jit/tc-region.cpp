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

#include "hphp/runtime/vm/jit/tc-region.h"

#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc-prologue.h"
#include "hphp/runtime/vm/jit/tc-record.h"

#include "hphp/runtime/base/perf-warning.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/func-order.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/relocation.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/trans-db.h"
#include "hphp/runtime/vm/jit/trans-rec.h"
#include "hphp/runtime/vm/jit/translate-region.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vtune-jit.h"
#include "hphp/runtime/vm/func-token.h"
#include "hphp/runtime/vm/treadmill.h"

#include "hphp/util/boot-stats.h"
#include "hphp/util/data-block.h"
#include "hphp/util/configs/codecache.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/hardware-counter.h"
#include "hphp/util/roar.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/trace.h"

#include <folly/Range.h>
#include <fmt/format.h>

TRACE_SET_MOD(mcg)

namespace HPHP::jit::tc {

namespace {

using PrologueTCAMap = jit::hash_map<PrologueID,TCA,PrologueID::Hasher>;
using SrcKeyTransMap = jit::hash_map<SrcKey,jit::vector<TCA>,
                                     SrcKey::Hasher>;

constexpr size_t kROARNativeCallBatchSize = 4096;

bool checkLimit(TransKind kind, const size_t numTrans) {
  auto const limit = kind == TransKind::Profile
    ? Cfg::Jit::MaxProfileTranslations
    : Cfg::Jit::MaxTranslations;

  // Once numTrans has reached limit + 1 we know that an interp translation
  // has already been emitted. Prior to that if numTrans == limit only allow
  // interp translations to avoid a race where multiple threads believe there
  // are still available translations.
  if (numTrans == limit + 1) return false;
  if (numTrans == limit && kind != TransKind::Interp) return false;
  return true;
}

void invalidateSrcKey(SrcKey sk, SBInvOffset spOff) {
  assertx(!Cfg::Repo::Authoritative || Cfg::Jit::PGO);
  /*
   * Reroute existing translations for SrcKey to an as-yet indeterminate
   * new one.
   */
  auto const sr = srcDB().find(sk);
  if (!sr) {
    always_assert(profData()->wasDeserialized());
    return;
  }

  // Retranslate stub must exist, as createSrcRec() created it.
  auto const transStub = svcreq::getOrEmitStub(
    svcreq::StubType::Translate, sk, spOff);
  always_assert(transStub);

  /*
   * Since previous translations aren't reachable from here, we know we
   * just created some garbage in the TC. We currently have no mechanism
   * to reclaim this.
   */
  FTRACE_MOD(Trace::reusetc, 1,
             "Replacing translations from sk: {} " "to SrcRec addr={}\n",
             showShort(sk), (void*)sr);
  Trace::Indent _i;
  sr->replaceOldTranslations(transStub);
}

void invalidateFuncProfSrcKeys(const Func* func) {
  assertx(profData());
  for (auto tid : profData()->funcProfTransIDs(func)) {
    auto const transRec = profData()->transRec(tid);
    invalidateSrcKey(transRec->srcKey(), transRec->startSpOff());
  }
}

size_t infoSize(const FuncMetaInfo& info) {
  size_t sz = 0;
  for (auto& trans : info.translators) {
    auto const& range = trans->range();
    sz += range.main.size() + range.cold.size() + range.frozen.size();
  }
  return sz;
}

void relocateOptFunc(const Func* func, FuncMetaInfo& info,
                     PrologueTCAMap& prologueTCAs,
                     SrcKeyTransMap& srcKeyTrans,
                     size_t* failedBytes = nullptr) {
  unsigned nRegions = 0;
  // Relocate/emit all prologues and translations for func in order.
  for (auto& translator : info.translators) {
    assertx(func == translator->sk.func());
    auto const prologueTranslator =
      dynamic_cast<PrologueTranslator*>(translator.get());
    auto const regionTranslator =
      dynamic_cast<RegionTranslator*>(translator.get());

    if (regionTranslator) {
      auto const it = srcKeyTrans.find(translator->sk);
      // We don't publish translations one at a time during optimized
      // retranslation.  Use the srcKeyTransMap to establish if we hit the
      // translation limit.
      if (it != srcKeyTrans.end() &&
          !checkLimit(regionTranslator->kind, it->second.size())) {
        translator->reset();
        continue;
      }
    }
    if (!translator->translateSuccess()) continue;
    auto const& range = translator->range();
    auto const bytes = range.main.size() + range.cold.size() +
                       range.frozen.size();
    if (regionTranslator) nRegions++;
    // We don't want to align the first region, to enable fall-through from the
    // last emitted prologue.
    const bool alignMain = !regionTranslator || nRegions != 1;
    translator->viewTid = pthread_self();
    translator->relocate(alignMain);
    if (!translator->translateSuccess()) {
      if (failedBytes) *failedBytes += bytes;
      continue;
    }

    translator->bindOutgoingEdges();
    if (!translator->translateSuccess()) continue;

    always_assert(code().inMain(translator->entry()));
    if (prologueTranslator) {
      const auto pid = PrologueID(func, prologueTranslator->paramIndex());
      prologueTCAs[pid] = translator->entry();
    } else if (regionTranslator) {
      srcKeyTrans[regionTranslator->sk].emplace_back(translator->entry());
    }
  }
}

void publishOptFuncMeta(FuncMetaInfo& info) {
  for (auto const& translator : info.translators) {
    if (translator->translateSuccess()) translator->publishMetaInternal();
  }
}

void processOptFuncsPidMapEntries(folly::Range<FuncMetaInfo*> infos) {
  if (!use_roar) return;

  CGMeta batch;
  for (auto& info : infos) {
    for (auto const& translator : info.translators) {
      if (!translator->translateSuccess()) continue;

      auto& entries = translator->meta().roarPidMapEntries;
      batch.roarPidMapEntries.insert(
        batch.roarPidMapEntries.end(),
        std::make_move_iterator(entries.begin()),
        std::make_move_iterator(entries.end())
      );
      entries.clear();
    }
  }
  batch.processPidMapEntries();
}

void processSortedOptFuncsNativeCalls(std::vector<FuncMetaInfo>& infos) {
  if (!use_roar) return;

  CGMeta batch;
  for (auto& finfo : infos) {
    for (auto const& translator : finfo.translators) {
      if (!translator->translateSuccess()) continue;

      auto& nativeCalls = translator->meta().nativeCalls;
      for (auto const& call : nativeCalls) {
        if (batch.nativeCalls.size() == kROARNativeCallBatchSize) {
          batch.processNativeCalls();
        }
        batch.nativeCalls.emplace(call.first, call.second);
      }
      nativeCalls.clear();
    }
  }
  batch.processNativeCalls();
}

void publishOptFuncCode(FuncMetaInfo& info,
                        jit::hash_set<TCA>* publishedSet = nullptr) {

  // Publish all prologues and translations for func in order.
  for (auto const& translator : info.translators) {
    if (translator->translateSuccess()) {
      translator->publishCodeInternal();
      auto const tca = translator->entry();
      if (publishedSet) publishedSet->insert(tca);
    } else {
      // If we failed to emit the prologue (e.g. the TC filled up), redirect
      // all the callers to the fcallHelperThunk so that they stop calling
      // the profile code.
      translator->smashBackup();
    }
  }
}

void relocateSortedOptFuncs(std::vector<FuncMetaInfo>& infos,
                            PrologueTCAMap& prologueTCAs,
                            SrcKeyTransMap& srcKeyTrans) {
  BootStats::Block timer("RTA_relocate_sorted_opt_funcs", Cfg::Server::Mode,
                         true);
  size_t failedBytes = 0;

  bool shouldLog = Cfg::Server::Mode;
  for (auto& finfo : infos) {
    // make sure we don't get ahead of the translation threads
    mcgen::waitForTranslate(finfo);

    // We clear the translations that are not relocated to ensure no one tries
    // smashing or publishing such translations. The later phases only look at
    // the translators, so this is the only place that checks the token.
    auto const func = finfo.funcToken->getFunc();
    if (func == nullptr) {
      finfo.clear();
      continue;
    }

    if (tcIsFull()) {
      FTRACE(1, "relocateSortedOptFuncs: ran out of space in the TC. "
             "Skipping function {} {}\n", func->getFuncId(),
             func->fullName());
      failedBytes += infoSize(finfo);
      // Reset the translators.  This will cause the callers of the OptPrologues
      // that are being skipped to be smashed to stop calling the corresponding
      // ProfPrologues (see publishOptFuncCode).
      for (auto& translator : finfo.translators) {
        translator->reset();
      }
      continue;
    }
    if (shouldLog) {
      shouldLog = false;
      Logger::Info("retranslateAll: starting to relocate functions");
    }
    relocateOptFunc(func, finfo, prologueTCAs, srcKeyTrans, &failedBytes);
  }

  if (failedBytes) {
    FTRACE(1, "relocateSortedOptFuncs: failedBytes = {}\n", failedBytes);
    logPerfWarning("opt_translation_overflow", 1, [&] (StructuredLogEntry& e) {
      e.setInt("bytes_dropped", failedBytes);
    });
  }
}

/*
 * Smash and optimize the calls in `meta' to prologues in `srcKeyTrans'.
 */
void smashOptCalls(CGMeta& meta,
                   const PrologueTCAMap& prologueTCAs) {
  auto const oldSmashableCallData = std::move(meta.smashableCallData);
  for (auto& pair : oldSmashableCallData) {
    TCA call = pair.first;
    auto const& pid = pair.second;
    auto it = prologueTCAs.find(pid);
    if (it == prologueTCAs.end()) {
      // insert non-smashed call back into transInfo.meta.smashableCallData
      meta.smashableCallData.emplace(pair);
      continue;
    }

    const TCA target = it->second;
    FTRACE(1, "smashedOptCalls: found candidate call @ {}, "
           "target prologue @ {} (funcId={}, nArgs={})\n",
           call, target, pid.funcId(), pid.nargs());
    assertx(code().inMainOrColdOrFrozen(call));
    assertx(code().inMainOrColdOrFrozen(target));

    smashCall(call, target);
    optimizeSmashedCall(call);

    meta.smashableLocations.erase(call);
  }
}

/*
 * Find the jump target for jumps within the translation corresponding to
 * `curEntry' going to the SrcRec corresponding to `vec'.  `curEntry' is the TCA
 * of the start of the current tracelet.  For prologues this is nullptr.  It is
 * used while smashing retranslations of the same srckey.  `isRetrans` indicate
 * whether this is a fallback jump to the next translation.
 */
TCA findJumpTarget(TCA curEntry,
                   const jit::vector<TCA>& vec,
                   bool isRetrans) {
  always_assert(vec.size() > 0);

  // It may seem like !isRetrans should imply that none of the elements in the
  // retranslation chain vector is the curEntry.  In practice self loops may
  // make that happen.
  always_assert(IMPLIES(curEntry == nullptr, !isRetrans));

  // Case 1: when the jump is in a prologue (curEntry == nullptr) or it jumps to a
  //         different SrcKey, we jump to the first translation in the list.
  if (!isRetrans) {
    return vec.front();
  }

  // Case 2: when jumping to the same SrcKey, we jump to the translation
  //         following this one (`curEntry').
  for (size_t i = 0; i < vec.size() - 1; i++) {
    if (vec[i] == curEntry) {
      return vec[i + 1];
    }
  }

  // We should only get here if `curEntry' is the last translation.
  always_assert(curEntry == vec.back());
  return nullptr;
}

/*
 * Smash and optimize the jumps in the translation starting at TCA entry (with
 * tracked metadata in meta)  going to the optimized translations in
 * `srcKeyTrans'.
 */
void smashOptBinds(CGMeta& meta,
                   TCA entry,  // nullptr for prologues
                   const SrcKeyTransMap& srcKeyTrans) {
  jit::hash_set<TCA> smashed;
  decltype(meta.smashableBinds) newBinds;

  for (auto& bind : meta.smashableBinds) {
    auto it = srcKeyTrans.find(bind.sk);
    if (it == srcKeyTrans.end()) {
      newBinds.emplace_back(bind);
      continue;
    }
    const auto& transVec = it->second;
    TCA succTCA = findJumpTarget(entry, transVec, bind.fallback);
    if (succTCA == nullptr) {
      newBinds.emplace_back(bind);
      continue;
    }

    FTRACE(3, "smashOptBinds: found candidate bind @ {}, target SrcKey {}, "
           "fallback = {}\n",
           bind.smashable.show(), showShort(bind.sk), bind.fallback);
    // The bound ADDR pointer may not belong to the data segment, as is the case
    // with SSwitchMap (see #10347945)
    assertx(code().inMainOrColdOrFrozen(bind.smashable.toSmash()) ||
            bind.smashable.type() == IncomingBranch::Tag::ADDR);
    assertx(code().inMainOrColdOrFrozen(succTCA));
    bind.smashable.patch(succTCA);
    bind.smashable.optimize();
    smashed.insert(bind.smashable.toSmash());
    meta.smashableLocations.erase(bind.smashable.toSmash());
  }

  // Remove jumps that were smashed from meta.smashableJumpData.
  meta.smashableBinds.swap(newBinds);

  // If any of the jumps we smashed was a tail jump (i.e. going to a
  // retranslation of the entry SrcKey), then remove it from the set of
  // inProgressTailJumps as it has already been smashed.
  GrowableVector<IncomingBranch> newTailJumps;
  for (auto& jump : meta.inProgressTailJumps) {
    if (!smashed.contains(jump.toSmash())) {
      newTailJumps.push_back(jump);
    } else {
      FTRACE(3, "smashOptBinds: removing {} from inProgressTailJumps\n",
             jump.toSmash());
    }
  }
  meta.inProgressTailJumps.swap(newTailJumps);
}

/*
 * Smash and optimize the calls and jumps between the translations/prologues in
 * `infos'.
 */
void smashOptSortedOptFuncs(std::vector<FuncMetaInfo>& infos,
                            const PrologueTCAMap& prologueTCAs,
                            const SrcKeyTransMap& srcKeyTrans) {
  BootStats::Block timer("RTA_smash_opt_funcs",
                         Cfg::Server::Mode);
  for (auto& finfo : infos) {
    for (auto& translator : finfo.translators) {
      // Skip if the translation wasn't relocated (e.g. ran out of TC space).
      if (!translator->entry()) continue;
      assertx(code().inMainOrColdOrFrozen(translator->entry()));

      if (isPrologue(translator->kind)) {
        smashOptBinds(translator->meta(), nullptr, srcKeyTrans);
      } else {
        smashOptCalls(translator->meta(), prologueTCAs);
        smashOptBinds(translator->meta(), translator->entry(), srcKeyTrans);
      }
    }
  }
}

void invalidateFuncsProfSrcKeys() {
  BootStats::Block timer("RTA_invalidate_prof_srckeys",
                         Cfg::Server::Mode);
  auto const pd = profData();
  assertx(pd);
  pd->forEachProfilingFunc([&](auto const& func) {
    always_assert(func);

    invalidateFuncProfSrcKeys(func);

    // clear the func body and prologues
    const_cast<Func*>(func)->resetFuncEntry();
    auto const numPrologues = func->numPrologues();
    for (int p = 0; p < numPrologues; p++) {
      const_cast<Func*>(func)->resetPrologue(p);
    }
  });
}

void publishSortedOptFuncsMeta(std::vector<FuncMetaInfo>& infos) {
  BootStats::Block timer("RTA_publish_meta",
                         Cfg::Server::Mode);
  for (auto& finfo : infos) {
    publishOptFuncMeta(finfo);
  }
}

void publishSortedOptFuncsCode(std::vector<FuncMetaInfo>& infos,
                               jit::hash_set<TCA>* publishedSet) {
  BootStats::Block timer("RTA_publish_code",
                         Cfg::Server::Mode);
  for (auto& finfo : infos) {
    publishOptFuncCode(finfo, publishedSet);
  }
}

std::string show(const SrcKeyTransMap& map) {
  std::string ret;
  for (auto& skt : map) {
    fmt::format_to(std::back_inserter(ret), "  - [{}]:", showShort(skt.first));
    for (auto tca : skt.second) {
      fmt::format_to(std::back_inserter(ret), " {},", fmt::ptr(tca));
    }
    ret += "\n";
  }
  return ret;
}

void checkPublishedAddr(TCA tca, const jit::hash_set<TCA>& publishedSet) {
  always_assert_flog(code().inMainOrColdOrFrozen(tca),
                     "srcKeyTrans has address not in hot/main: {}", tca);
  always_assert_flog(publishedSet.contains(tca),
                     "srcKeyTrans has unpublished translation @ {}", tca);
}

/*
 * Make sure that every address that we may have smashed to was published
 * somewhere in the translation cache.
 */
void checkPublishedAddresses(const PrologueTCAMap&     prologueTCAs,
                             const SrcKeyTransMap&     srcKeyTrans,
                             const jit::hash_set<TCA>& publishedSet) {
  for (auto& prologueTCA : prologueTCAs) {
      checkPublishedAddr(prologueTCA.second, publishedSet);
  }

  for (auto& skt : srcKeyTrans) {
    auto& vec = skt.second;
    for (auto tca : vec) {
      if (tca) {
        checkPublishedAddr(tca, publishedSet);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
}

SrcRec* findSrcRec(SrcKey sk) {
  return srcDB().find(sk);
}

SrcRec* createSrcRec(SrcKey sk, SBInvOffset spOff) {
  if (auto const sr = srcDB().find(sk)) return sr;

  // invalidateSrcKey() depends on existence of this stub.
  auto const transStub = svcreq::getOrEmitStub(
    svcreq::StubType::Translate, sk, spOff);
  if (!transStub) return nullptr;

  auto metaLock = lockMetadata();
  if (auto const sr = srcDB().find(sk)) return sr;

  SKTRACE(1, sk, "inserting SrcRec\n");

  auto const sr = srcDB().insert(sk);
  if (Cfg::Eval::EnableReusableTC) {
    recordFuncSrcRec(sk.func(), sr);
  }

  return sr;
}


////////////////////////////////////////////////////////////////////////////////

void publishOptFunc(const Func* func, FuncMetaInfo info) {
  PrologueTCAMap prologueTCAs;
  SrcKeyTransMap srcKeyTrans;
  relocateOptFunc(func, info, prologueTCAs, srcKeyTrans);

  {
    auto codeLock = lockCode();
    auto metaLock = lockMetadata();
    invalidateFuncProfSrcKeys(func);
    publishOptFuncMeta(info);
    publishOptFuncCode(info);

    updateCodeSizeCounters();
  }
  processOptFuncsPidMapEntries({&info, 1});
}

void relocatePublishSortedOptFuncs(std::vector<FuncMetaInfo> infos) {
  // Grab the session now (which includes a Treadmill::Session) so
  // that no Func's get destroyed during this step
  ProfData::Session pds(Treadmill::SessionKind::RetranslateAll);
  const bool serverMode = Cfg::Server::Mode;

  PrologueTCAMap prologueTCAs;
  SrcKeyTransMap srcKeyTrans;

  relocateSortedOptFuncs(infos, prologueTCAs, srcKeyTrans);

  if (serverMode) {
    Logger::Info(
      "retranslateAll: finished optimizing and relocating functions"
    );
  }

  processSortedOptFuncsNativeCalls(infos);

  auto codeLock = lockCode();
  auto metaLock = lockMetadata();

  FTRACE(3,
         "relocatePublishSortedOptFuncs: after relocateSortedOptFuncs:\n{}\n",
         show(srcKeyTrans));

  smashOptSortedOptFuncs(infos, prologueTCAs, srcKeyTrans);

  FTRACE(3,
         "relocatePublishSortedOptFuncs: after smashOptSortedOptFuncs:\n{}\n",
         show(srcKeyTrans));

  if (serverMode) {
    Logger::Info("retranslateAll: starting to publish functions");
  }

  invalidateFuncsProfSrcKeys();

  // Publish the metadata for all the translations/prologues before actually
  // publishing any of their code.  This is necessary because we've smashed
  // calls and jumps across translations so, once any of them is made reachable,
  // we have to assume that all of them are already reachable, even before
  // they're all published.
  publishSortedOptFuncsMeta(infos);
  processOptFuncsPidMapEntries(folly::range(infos));

  jit::hash_set<TCA> publishedSet;
  publishSortedOptFuncsCode(infos, debug ? &publishedSet : nullptr);

  FTRACE(3,
         "relocatePublishSortedOptFuncs: after publishSortedOptFuncsCode:\n"
         "{}\n", show(srcKeyTrans));

  if (debug) {
    checkPublishedAddresses(prologueTCAs, srcKeyTrans, publishedSet);
  }

  updateCodeSizeCounters();
}

void RegionTranslator::computeKind() {
  // Update the translation kind if it is invalid, or if it may
  // have changed (original kind was a profiling kind)
  if (kind == TransKind::Invalid || kind == TransKind::Profile) {
    kind = profileFunc(sk.func()) ? TransKind::Profile
                                  : TransKind::Live;
  }
}

Optional<TranslationResult> RegionTranslator::getCached() {
  auto const srcRec = srcDB().find(sk);
  auto const numTrans = srcRec->numTrans();
  if (prevNumTranslations != -1 && prevNumTranslations != numTrans) {
    // A new translation was generated before we grabbed the lock.  Force
    // execution to rerun through the retranslation chain.
    return TranslationResult{srcRec->getTopTranslation()};
  }
  prevNumTranslations = numTrans;

  // An optimize retranslation request may be in flight that will
  // remove profiling translations from the srcRec.  Wait till
  // getCached is called while the lease is held to check the
  // number of translations in the srcRec is not over max
  // capacity.
  if (!m_lease || !(*m_lease)) return std::nullopt;
  // Check for potential interp anchor translation
  if (kind == TransKind::Profile) {
    if (numTrans > Cfg::Jit::MaxProfileTranslations) {
      always_assert(numTrans ==
                    Cfg::Jit::MaxProfileTranslations + 1);
      return TranslationResult{srcRec->getTopTranslation()};
    }
  } else if (numTrans > Cfg::Jit::MaxTranslations) {
    always_assert(numTrans == Cfg::Jit::MaxTranslations + 1);
    return TranslationResult{srcRec->getTopTranslation()};
  }
  return std::nullopt;
}

void RegionTranslator::resetCached() {
  invalidateSrcKey(sk, spOff);
}

void RegionTranslator::gen() {
  // Per-request hardware counters (such as instructions, load/store) should not
  // include the JIT.
  UNUSED HardwareCounter::ExcludeScope stopCount;
  auto const srcRec = srcDB().find(sk);
  auto const emitInterpStub = [&] {
    FTRACE(1, "emitting dispatchBB interp request for failed "
           "translation (spOff = {})\n", spOff.offset);
    vunit = std::make_unique<Vunit>();
    Vout vmain{*vunit, vunit->makeBlock(AreaIndex::Main)};
    vunit->entry = Vlabel(vmain);

    emitInterpReq(vmain, sk, spOff);

    irlower::optimize(*vunit, CodeKind::Helper);
  };
  auto const fail = [&] {
    if (isProfiling(kind)) return;

    kind = TransKind::Interp;
    if (!checkLimit(kind, srcRec->numTrans())) return;
    emitInterpStub();
  };

  // Only start profiling new functions at their entry point. This reduces the
  // chances of profiling the body of a function but not its entry (where we
  // trigger retranslation) and helps remove bias towards larger functions that
  // can cause variations in the size of code.prof.
  if (kind == TransKind::Profile &&
      !profData()->profiling(sk.funcID()) &&
      !sk.anyFuncEntry()) {
    return;
  }
  if (!checkLimit(kind, srcRec->numTrans())) return fail();

  if (!region) {
    region = selectRegion(regionContext(), kind);
  }
  if (!region) return fail();

  INC_TPC(translate);
  Timer timer(Timer::mcg_translate, nullptr);

  tracing::Block _{"translate", [&] {
		return traceProps(sk.func())
			.add("sk", show(sk))
			.add("trans_kind", show(kind));
	}};
  tracing::annotateBlock(
    [&] {
      return tracing::Props{}
      .add("region_size", region->instrSize());
    }
  );

  rqtrace::ScopeGuard trace{"JIT_TRANSLATE"};
  trace.annotate("func_name", sk.func()->fullName()->data());
  trace.annotate("trans_kind", show(kind));
  trace.setEventPrefix("JIT_");
  trace.annotate(
    "region_size", folly::to<std::string>(region->instrSize()));

  TransContext ctx {
    transId == kInvalidTransID ? TransIDSet{} : TransIDSet{transId},
    optIndex,
    kind,
    sk,
    region.get(),
    sk.packageInfo(),
    PrologueID(),
  };
  unit = irGenRegion(*region, ctx, pconds);
  assertx(unit);
  auto const& uas = unit->annotationData->getAllAnnotations();
  annotations.insert(annotations.end(), uas.begin(), uas.end());
  hasLoop = cfgHasLoop(*unit);

  vunit = irlower::lowerUnit(*unit);
  // vasm gen can punt, and when it does it's important we don't retry
  // translating the same thing again and again as it will potentially consume
  // more and more RDS space.
  if (!vunit) emitInterpStub();
}

/*
 * Record various metadata about the translation into the global data
 * structures.
 */
void RegionTranslator::publishMetaImpl() {
  auto& fixups = transMeta->fixups;
  const auto& view = transMeta->view;
  const auto& loc = transMeta->range.loc();
  assertx(!loc.empty());

  auto const srcRec = srcDB().find(sk);
  always_assert(srcRec);

  if (Cfg::Eval::ProfileBC) {
    TransBCMapping prev{};
    for (auto& cur : fixups.bcMap) {
      if (!cur.aStart) continue;
      if (prev.aStart) {
        recordBCInstr(
          uint32_t(prev.sk.op()), prev.aStart, cur.aStart, false, fixups
        );
      } else {
        recordBCInstr(
          OpTraceletGuard, loc.mainStart(), cur.aStart, false, fixups
        );
      }
      prev = cur;
    }
  }

  recordGdbTranslation(sk, view.main(), loc.mainStart(), loc.mainEnd(), fixups);
  recordGdbTranslation(
    sk, view.cold(), loc.coldCodeStart(), loc.coldEnd(), fixups
  );

  TransRec tr{sk, transId, kind, loc.mainStart(), loc.mainSize(),
              loc.coldCodeStart(), loc.coldCodeSize(),
              loc.frozenCodeStart(), loc.frozenCodeSize(),
              std::move(annotations), region, fixups.bcMap, hasLoop};
  transdb::addTranslation(tr);
  FuncOrder::recordTranslation(tr);
  if (Cfg::Jit::UseVtuneAPI) {
    reportTraceletToVtune(sk.unit(), sk.func(), tr);
  }
  recordTranslationSizes(tr);

  fixups.process(&tailBranches);

  if (kind == TransKind::Live) {
    incrementLiveFuncTransBytes(sk.func()->getFuncId(), 
                                loc.mainSize() + loc.coldCodeSize() + loc.frozenCodeSize());
  }
}

/*
 * Add a translation to the corresponding SrcRec, effectively making it
 * reachable.  This should be done after the metadata for the translation has
 * been published.
 */
void RegionTranslator::publishCodeImpl() {
  const auto& loc = transMeta->range.loc();
  const auto srcRec = srcDB().find(sk);
  always_assert(srcRec);
  assert_flog(checkLimit(kind, srcRec->numTrans()), "sk: {}", show(sk));

  if (kind == TransKind::Profile) {
    always_assert(region);
    profData()->addTransProfile(transId, region, pconds,
                                transMeta->range.main.size());
  }

  TRACE(1, "newTranslation: %p  sk: %s\n",
        entry(), showShort(sk).c_str());

  srcRec->newTranslation(loc, tailBranches);
  auto const func = sk.func();
  auto const numPosParams = func->numPositionalParams();
  if (sk == SrcKey{func, numPosParams, false, SrcKey::FuncEntryTag{}}) {
    const_cast<Func*>(func)->setFuncEntry(entry());
  }

  TRACE(1, "mcg: %u-byte translation (%u main, %u cold, %u frozen)\n",
        loc.mainSize() + loc.coldCodeSize() + loc.frozenCodeSize(),
        loc.mainSize(), loc.coldCodeSize(), loc.frozenCodeSize());
  if (Trace::moduleEnabledRelease(Trace::tcspace, 1)) {
    Trace::traceRelease("%s", getTCSpace().c_str());
  }
  checkFreeProfData();
}

void RegionTranslator::setCachedForProcessFail() {
  auto const srcRec = srcDB().find(sk);
  always_assert(srcRec);

  auto const func = sk.func();
  auto const numPosParams = func->numPositionalParams();
  if (sk == SrcKey{func, numPosParams, false, SrcKey::FuncEntryTag{}}) {
    const_cast<Func*>(func)->setFuncEntry(
      tc::ustubs().handleInterpMainFuncEntryNoTranslate);
  }
  auto const stub = svcreq::emit_interp_no_translate_stub(spOff, sk);
  TRACE(1, "setCachedForProcessFail: stub: %p  sk: %s\n",
        stub, showShort(sk).c_str());

  if (!stub) return;
  srcRec->smashFallbacksToStub(stub);
}

bool RegionTranslator::exceededMaxLiveTranslations(int numTrans) {
  always_assert(isLive(kind));
  // After hitting limit, jit will generate one
  // more translation that will call the interpreter.
  always_assert(numTrans <= Cfg::Jit::MaxTranslations + 1);
  return numTrans >= Cfg::Jit::MaxTranslations;
}
}
