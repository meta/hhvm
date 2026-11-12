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

#include "hphp/runtime/vm/jit/inlining-decider.h"

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/trans-cfg.h"
#include "hphp/runtime/vm/srckey.h"

#include "hphp/util/configs/hhir.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/trace.h"

#include "hphp/zend/zend-strtod.h"

#include <fmt/format.h>
#include <folly/Synchronized.h>
#include <cmath>
#include <vector>
#include <sstream>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(inlining)

namespace {
///////////////////////////////////////////////////////////////////////////////

std::string nameAndReason(int bcOff, std::string caller, std::string callee,
                          std::string why) {
  return fmt::format("BC {}: {} -> {}: {}\n", bcOff, caller, callee, why);
}

bool traceRefusal(SrcKey callerSk, const Func* callee, std::string why,
                  AnnotationData* annotations) {
  // This is not under Trace::enabled so that we can collect the data in prod.
  const Func* caller = callerSk.func();
  int bcOff = callerSk.offset();
  auto calleeName = callee ? callee->fullName()->data() : "(unknown)";
  if (annotations && Cfg::Eval::DumpInlDecision > 0) {
    annotations->inliningDecisions.emplace_back(false, bcOff, caller, callee,
                                                why);
  }
  if (Trace::enabled) {
    assertx(caller);
    FTRACE(2, "Inlining decider: refusing {}() <- {}{}\t<reason: {}>\n",
           caller->fullName()->data(), calleeName, callee ? "()" : "", why);
  }
  if (caller->shouldSampleJit() || (callee && callee->shouldSampleJit())) {
    StructuredLogEntry inlLog;
    auto bcStr = [&] {
      std::ostringstream bcStrn;
      bcStrn << bcOff;
      return bcStrn.str();
    } ();
    inlLog.setStr("bc_off", bcStr);
    inlLog.setStr("caller", caller->fullName()->data());
    inlLog.setStr("callee", calleeName);
    inlLog.setStr("reason", why);
    StructuredLog::log("hhvm_inline_refuse", inlLog);
  }
  return false;
}

std::atomic<uint64_t> s_baseProfCount{0};
folly::ConcurrentHashMap<FuncId, uint32_t> s_funcTargetCounts;


///////////////////////////////////////////////////////////////////////////////
// canInlineAt() helpers.

const StaticString
  s_AlwaysInline("__ALWAYS_INLINE"),
  s_NeverInline("__NEVER_INLINE"),
  s_HH_Coeffects_Backdoor("HH\\Coeffects\\backdoor"),
  s_HH_Coeffects_Backdoor_Async("HH\\Coeffects\\backdoor_async"),
  s_HH_Coeffects_FB_Backdoor_to_globals_leak_safe__DO_NOT_USE(
    "HH\\Coeffects\\fb\\backdoor_to_globals_leak_safe__DO_NOT_USE"
  ),
  s_HH_Coeffects_FB_Backdoor_to_globals_leak_safe_async__DO_NOT_USE(
    "HH\\Coeffects\\fb\\backdoor_to_globals_leak_safe_async__DO_NOT_USE"
  );

#define COEFFECTS_BACKDOOR_WRAPPERS \
  X(pure)                           \
  X(pure_async)                     \
  X(write_props)                    \
  X(write_props_async)              \
  X(read_globals)                   \
  X(read_globals_async)             \
  X(zoned)                          \
  X(leak_safe)                      \
  X(leak_safe_async)

#define X(x)                                             \
  const StaticString                                     \
  s_HH_Coeffects_FB_Backdoor_from_##x                    \
  ("HH\\Coeffects\\fb\\backdoor_from_"#x"__DO_NOT_USE");
COEFFECTS_BACKDOOR_WRAPPERS
#undef X

/*
 * Check if the `callee' has any characteristics which prevent inlining,
 * without peeking into its bytecode or regions.
 */
bool isCalleeInlinable(SrcKey callSK, const Func* callee,
                       AnnotationData* annotations) {
  auto refuse = [&] (const char* why) {
    return traceRefusal(callSK, callee, why, annotations);
  };

  if (!callee) {
    return refuse("callee not known");
  }
  if (callee == callSK.func()) {
    return refuse("call is recursive");
  }
  if (callee->isGenerator()) {
    return refuse("callee is generator");
  }
  if (callee->maxStackCells() >= Cfg::Eval::StackCheckLeafPadding) {
    return refuse("function stack depth too deep");
  }
  if (callee->userAttributes().contains(s_NeverInline.get())) {
    return refuse("callee marked __NEVER_INLINE");
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////
}

bool canInlineAt(SrcKey callSK, SrcKey entry, AnnotationData* annotations) {
  assertx(entry.func());
  assertx(entry.anyFuncEntry());
  auto const callee = entry.func();

  if (!Cfg::HHIR::EnableGenTimeInlining) {
    return traceRefusal(callSK, callee, "disabled via runtime option",
                        annotations);
  }
  if (RuntimeOption::funcIsRenamable(callee->name())) {
    return traceRefusal(callSK, callee, "function is renamable",
                        annotations);
  }
  if (callee->isInterceptable()) {
    return traceRefusal(callSK, callee, "callee is interceptable", annotations);
  }

  if (!isCalleeInlinable(callSK, callee, annotations)) {
    return false;
  }

  return true;
}

namespace {
///////////////////////////////////////////////////////////////////////////////
// shouldInline() helpers.

/*
 * Check if a builtin is inlinable.
 */
bool isInlinableCPPBuiltin(const Func* f) {
  assertx(f->isCPPBuiltin());

  // The callee needs to be callable with FCallBuiltin, because NativeImpl
  // requires a frame.
  if (!Cfg::Eval::EnableCallBuiltin ||
      (f->attrs() & AttrNoFCallBuiltin) ||
      (f->numParams() > Native::maxFCallBuiltinArgs()) ||
      !f->nativeFuncPtr()) {
    return false;
  }

  return true;
}

struct InlineRegionKey {
  InlineRegionKey(SrcKey entryKey,
                  Type ctxType,
                  TinyVector<Type, 4> argTypes)
    : entryKey{std::move(entryKey)}
    , ctxType{std::move(ctxType)}
    , argTypes(std::move(argTypes)) {}

  explicit InlineRegionKey(const RegionDesc& region)
    : entryKey(region.entry()->start())
    , ctxType(region.inlineCtxType())
  {
    for (auto const ty : region.inlineInputTypes()) {
      argTypes.push_back(ty);
    }
  }

  InlineRegionKey(const InlineRegionKey& irk)
    : entryKey(irk.entryKey)
    , ctxType(irk.ctxType)
  {
    for (auto ty : irk.argTypes) argTypes.push_back(ty);
  }

  InlineRegionKey(InlineRegionKey&& irk) noexcept
    : entryKey(std::move(irk.entryKey))
    , ctxType(std::move(irk.ctxType))
  {
    for (auto ty : irk.argTypes) argTypes.push_back(ty);
    irk.argTypes.clear();
  }

  InlineRegionKey& operator=(const InlineRegionKey& irk) {
    entryKey = irk.entryKey;
    ctxType = irk.ctxType;
    argTypes.clear();
    for (auto ty : irk.argTypes) argTypes.push_back(ty);
    return *this;
  }

  InlineRegionKey& operator=(InlineRegionKey&& irk) noexcept {
    entryKey = irk.entryKey;
    ctxType = irk.ctxType;
    argTypes.clear();
    for (auto ty : irk.argTypes) argTypes.push_back(ty);
    irk.argTypes.clear();
    return *this;
  }

  struct Eq {
    size_t operator()(const InlineRegionKey& k1,
                      const InlineRegionKey& k2) const {
      return
        k1.entryKey == k2.entryKey &&
        k1.ctxType == k2.ctxType &&
        k1.argTypes == k2.argTypes;
    }
  };

  struct Hash {
    size_t operator()(const InlineRegionKey& key) const {
      size_t h = 0;
      h = hash_combine(h, key.entryKey.toAtomicInt());
      h = hash_combine(h, key.ctxType.hash());
      for (auto const ty : key.argTypes) {
        h = hash_combine(h, ty.hash());
      }
      return h;
    }

  private:
    template<class T>
    static size_t hash_combine(size_t base, T other) {
      return folly::hash::hash_128_to_64(
          base, folly::hash::hash_combine(other));
    }
  };

  SrcKey entryKey;
  Type ctxType;
  TinyVector<Type, 4> argTypes;
};

using InlineCostCache = folly::ConcurrentHashMap<
  InlineRegionKey,
  unsigned,
  InlineRegionKey::Hash,
  InlineRegionKey::Eq
>;

Vcost computeTranslationCostSlow(SrcKey at,
                                 const irgen::RegionAndLazyUnit& regionAndLazyUnit,
                                 AnnotationData* annotationData) {
  auto const unit = regionAndLazyUnit.unit();
  if (!unit) return {0, true};

  // TODO(T52856776) - annotations should be copied from unit into outer unit
  // via annotationData
  rqtrace::DisableTracing notrace;
  auto const unbumper = mcgen::unbumpFunctions();
  SCOPE_ASSERT_DETAIL("Inline-IRUnit") { return show(*unit); };
  return irlower::computeIRUnitCost(*unit);
}

InlineCostCache s_inlCostCache;

int computeTranslationCost(SrcKey at,
                           const irgen::RegionAndLazyUnit& regionAndLazyUnit,
                           AnnotationData* annotationData) {
  auto const region = *regionAndLazyUnit.region();
  InlineRegionKey irk{region};
  auto f = s_inlCostCache.find(irk);
  if (f != s_inlCostCache.end()) return f->second;

  auto const info = computeTranslationCostSlow(at, regionAndLazyUnit, annotationData);
  auto cost = info.cost;

  // We normally store the computed cost into the cache.  However, if the region
  // is incomplete, and it's cost is still within the maximum allowed cost, and
  // we're still profiling that function, then we don't want to cache that
  // result yet.  The reason for this exception is that we may still gather
  // additional profiling information that will allow us to create a complete
  // region with acceptable cost.
  bool cacheResult = true;

  if (info.incomplete) {
    if (info.cost <= Cfg::HHIR::InliningMaxVasmCostLimit) {
      auto const fid = region.entry()->func()->getFuncId();
      auto const profData = jit::profData();
      auto const profiling = profData && profData->profiling(fid);
      if (profiling) cacheResult = false;
    }

    // Set cost very high to prevent inlining of incomplete regions.
    cost = std::numeric_limits<int>::max();
  }
  if (cacheResult) {
    s_inlCostCache.emplace(irk, cost);
  }
  FTRACE(3, "computeTranslationCost(at {}) = {}\n", showShort(at), cost);
  return cost;
}

uint64_t adjustedMaxVasmCost(const irgen::IRGS& env,
                             const RegionDesc& calleeRegion,
                             uint32_t depth) {
  auto const maxDepth = Cfg::HHIR::InliningMaxDepth;
  if (depth >= maxDepth) return 0;
  const auto baseVasmCost = Cfg::HHIR::InliningVasmCostLimit;
  const auto baseProfCount = s_baseProfCount.load();
  if (baseProfCount == 0) return baseVasmCost;
  auto const callerProfCount = irgen::curProfCount(env);
  auto adjustedCost = baseVasmCost *
    std::pow((double)callerProfCount / baseProfCount,
             Cfg::HHIR::InliningVasmCallerExp);
  auto const passesConcreteCallable = [&] {
    // If we pass a concrete callable to a callee and the callee calls this
    // callable (likely), inlining the callee is beneficial, as it converts
    // the runtime call to the callable to a known one and also makes it
    // eligible for further inlining.
    //
    // Treat calls to callees that pass a concrete callables as unique callees
    // to increase their chance of being inlined.
    //
    // This makes HHVM inline various HSL helpers such as Vec\map() more
    // aggressively, which are often used with trivial lambdas.
    for (auto const& ty : calleeRegion.inlineInputTypes()) {
      if (ty.hasConstVal(TFunc)) return true;
      if (ty.hasConstVal(TClsMeth)) return true;
      if (ty <= TObj) {
        auto const cls = ty.clsSpec().exactCls();
        if (cls != nullptr && cls->classof(c_Closure::classof())) return true;
      }
    }
    return false;
  }();
  auto const calleeProfCount = passesConcreteCallable
    ? 0U : irgen::calleeProfCount(env, calleeRegion);
  if (calleeProfCount) {
    adjustedCost *= std::pow((double)callerProfCount / calleeProfCount,
                             Cfg::HHIR::InliningVasmCalleeExp);
  }
  auto const numCallsites = passesConcreteCallable
    ? 1U
    : std::max(1U, s_funcTargetCounts[calleeRegion.entry()->func()->getFuncId()]);
  adjustedCost *= std::max(Cfg::HHIR::InliningVasmMinCallsiteFactor,
                           std::pow(1.0 / numCallsites, Cfg::HHIR::InliningVasmCallsiteExp));
  adjustedCost *= std::pow(1.0 / (1 + depth),
                           Cfg::HHIR::InliningDepthExp);
  if (adjustedCost < Cfg::HHIR::AlwaysInlineVasmCostLimit) {
    adjustedCost = Cfg::HHIR::AlwaysInlineVasmCostLimit;
  }
  if (adjustedCost > Cfg::HHIR::InliningMaxVasmCostLimit) {
    adjustedCost = Cfg::HHIR::InliningMaxVasmCostLimit;
  }
  if (calleeProfCount) {
    FTRACE(3, "adjustedMaxVasmCost: adjustedCost ({}) = baseVasmCost ({}) * "
           "(callerProfCount ({}) / baseProfCount ({})) ^ {} * "
           "(callerProfCount ({}) / calleeProfCount ({})) ^ {} * "
           "(1.0 / (1 + depth ({}))) ^ {}\n",
           adjustedCost, baseVasmCost,
           callerProfCount, baseProfCount,
           Cfg::HHIR::InliningVasmCallerExp,
           callerProfCount, calleeProfCount,
           Cfg::HHIR::InliningVasmCalleeExp,
           depth, Cfg::HHIR::InliningDepthExp);
  } else {
    FTRACE(3, "adjustedMaxVasmCost: adjustedCost ({}) = baseVasmCost ({}) * "
           "(callerProfCount ({}) / baseProfCount ({})) ^ {} * "
           "(1.0 / (1 + depth ({}))) ^ {}\n",
           adjustedCost, baseVasmCost,
           callerProfCount, baseProfCount,
           Cfg::HHIR::InliningVasmCallerExp,
           depth, Cfg::HHIR::InliningDepthExp);
  }
  return adjustedCost;
}

///////////////////////////////////////////////////////////////////////////////
}

/*
 * Return the cost of inlining the given callee.
 */
int costOfInlining(SrcKey callerSk,
                   const Func* callee,
                   const irgen::RegionAndLazyUnit& regionAndLazyUnit,
                   AnnotationData* annotationData) {
  auto const alwaysInl =
    (!Cfg::HHIR::InliningIgnoreHints &&
    callee->userAttributes().count(s_AlwaysInline.get())) ||
    (callee->isMemoizeWrapper() && callee->numParams() == 0);

  // Functions marked as always inline don't contribute to overall cost
  return alwaysInl ?
    0 :
    computeTranslationCost(callerSk, regionAndLazyUnit, annotationData);
}

bool isCoeffectsBackdoor(SrcKey callerSk, const Func* callee) {
  auto const callee_name = callee->fullName();
#define X(x)                                                                \
  if (callee_name->fsame(s_HH_Coeffects_FB_Backdoor_from_##x.get())) {      \
    return true;                                                            \
  }
  COEFFECTS_BACKDOOR_WRAPPERS
#undef X

  if (callee_name ==
        s_HH_Coeffects_FB_Backdoor_to_globals_leak_safe__DO_NOT_USE.get() ||
      callee_name ==
        s_HH_Coeffects_FB_Backdoor_to_globals_leak_safe_async__DO_NOT_USE.get()) {
    return true;
  }

  if (callee_name == s_HH_Coeffects_Backdoor.get() ||
      callee_name == s_HH_Coeffects_Backdoor_Async.get() ||
      (callee->isClosureBody() &&
       (callerSk.func()->fullName() == s_HH_Coeffects_Backdoor.get() ||
        callerSk.func()->fullName() == s_HH_Coeffects_Backdoor_Async.get()))) {
    return true;
  }

  return false;
}

bool shouldInline(const irgen::IRGS& irgs,
                  SrcKey callerSk,
                  const Func* callee,
                  const irgen::RegionAndLazyUnit& regionAndUnit,
                  int& cost) {
  auto const region = *regionAndUnit.region();
  auto sk = region.empty() ? SrcKey() : region.start();
  assertx(callee);
  assertx(sk.func() == callee);

  auto annotationsPtr = mcgen::dumpTCAnnotation(irgs.context.kind) ?
                        irgs.unit.annotationData.get() : nullptr;

  // Tracing return lambdas.
  auto refuse = [&] (const std::string& why) {
    FTRACE(2, "shouldInline: rejecting callee region: {}", show(region));
    return traceRefusal(callerSk, callee, why, annotationsPtr);
  };

  auto accept = [&] (std::string why) {
    auto static inlineAccepts = ServiceData::createTimeSeries(
      "jit.inline.accepts", {ServiceData::StatsType::COUNT});
    inlineAccepts->addValue(1);

    if (annotationsPtr && Cfg::Eval::DumpInlDecision >= 2) {
      auto const decision = AnnotationData::InliningDecision{
        true, callerSk.offset(), callerSk.func(), callee, why
      };
      annotationsPtr->inliningDecisions.push_back(decision);
    }

    UNUSED auto const topFunc = [&] {
      return irgs.inlineState.frames.empty()
        ? irgs.bcState.func()
        : irgs.inlineState.frames[0].callerSk.func();
    };

    FTRACE(2, "Inlining decider: inlining {}() <- {}()\t<reason: {}>\n",
           topFunc()->fullName()->data(), callee->fullName()->data(), why);
    return true;
  };

  auto const stackDepth = irgs.inlineState.stackDepth;
  if (stackDepth + callee->maxStackCells() >= Cfg::Eval::StackCheckLeafPadding) {
    return refuse("inlining stack depth limit exceeded");
  }

  // Try to inline CPP builtin functions. Inline regions for these functions
  // must end with a unique NativeImpl, which may not be true:
  //  - If we only include the initial Asserts in the region, we may have zero
  //  - If the NativeImpl guards its inputs, we may have multiple
  if (callee->isCPPBuiltin()) {
    if (!isInlinableCPPBuiltin(callee)) {
      return refuse("non-inlinable CPP builtin");
    }
    auto const count = std::count_if(
      std::begin(region.blocks()), std::end(region.blocks()),
      [](auto const b) {
        return
          !b->empty() &&
          !b->last().anyFuncEntry() &&
          b->last().op() == OpNativeImpl;
      }
    );
    switch (count) {
      case 0:  return refuse("inlinable CPP builtin without a NativeImpl");
      case 1:  return accept("inlinable CPP builtin with a unique NativeImpl");
      default: return refuse("inlinable CPP builtin with multiple NativeImpls");
    }
  }

  bool hasRet = false;

  // Iterate through the region, checking its suitability for inlining.
  for (auto const& block : region.blocks()) {
    sk = block->start();

    for (auto i = 0, n = block->length(); i < n; ++i, sk.advance()) {
      if (sk.anyFuncEntry()) continue;

      // We don't allow inlined functions in the region.  The client is
      // expected to disable inlining for the region it gives us to peek.
      if (sk.func() != callee) {
        return refuse("got region with inlined calls");
      }

      // Detect that the region contains a return.
      if (isReturnish(sk.op())) {
        hasRet = true;
      }

      // In optimized regions consider an await to be a returnish instruction,
      // if no returns appeared in the region then we likely suspend on all
      // calls to the callee.
      if (block->profTransID() != kInvalidTransID) {
        if (region.isExit(block->id()) && i + 1 == n && isAwait(sk.op())) {
          hasRet = true;
        }
      }
    }
  }

  if (!hasRet) {
    return refuse(
      fmt::format("region has no returns: callee BC instrs = {} : {}",
                     region.instrSize(), show(region)));
  }

  if (isCoeffectsBackdoor(callerSk, callee)) {
    return accept("coeffect backdoor is always inlined");
  }

  // Ignore cost computation for functions marked __ALWAYS_INLINE
  if (!Cfg::HHIR::InliningIgnoreHints &&
      callee->userAttributes().count(s_AlwaysInline.get())) {
    // In debug builds compute the cost anyway to catch bugs in the inlining
    // machinery. Many inlining tests utilize the __ALWAYS_INLINE attribute.
    if (debug) {
      computeTranslationCost(callerSk, regionAndUnit, annotationsPtr);
    }
    return accept("callee marked as __ALWAYS_INLINE");
  }

  // Refuse if the cost exceeds our thresholds.
  // We measure the cost of inlining each callstack and stop when it exceeds a
  // certain threshold.  (Note that we do not measure the total cost of all the
  // inlined calls for a given caller---just the cost of each nested stack.)
  cost = costOfInlining(callerSk, callee, regionAndUnit, annotationsPtr);
  if (cost <= Cfg::HHIR::AlwaysInlineVasmCostLimit) {
    return accept(fmt::format("cost={} within always-inline limit", cost));
  }

  if (region.instrSize() > irgs.budgetBCInstrs) {
    return refuse(fmt::format(
      "exhausted bytecode budget: budgetBCInstrs={}, regionSize={}",
      irgs.budgetBCInstrs, region.instrSize()));
  }
  auto maxTotalCost = adjustedMaxVasmCost(irgs, region, inlineDepth(irgs));
  int maxCost = maxTotalCost;
  if (Cfg::HHIR::InliningUseStackedCost) {
    maxCost -= irgs.inlineState.cost;
  }
  const auto baseProfCount = s_baseProfCount.load();
  const auto callerProfCount = irgen::curProfCount(irgs);
  const auto calleeProfCount = irgen::calleeProfCount(irgs, region);
  if (cost > maxCost) {
    auto const depth = inlineDepth(irgs);
    return refuse(fmt::format(
      "too expensive: cost={} : maxCost={} : "
      "baseProfCount={} : callerProfCount={} : calleeProfCount={} : depth={}",
      cost, maxCost, baseProfCount, callerProfCount, calleeProfCount, depth));
  }

  return accept(fmt::format("small region with return: cost={} : "
                               "maxTotalCost={} : maxCost={} : baseProfCount={}"
                               " : callerProfCount={} : calleeProfCount={}",
                               cost, maxTotalCost, maxCost, baseProfCount,
                               callerProfCount, calleeProfCount));
}

///////////////////////////////////////////////////////////////////////////////

namespace {
RegionDescPtr selectCalleeTracelet(SrcKey entry,
                                   Type ctxType,
                                   std::vector<Type>& inputTypes,
                                   int32_t maxBCInstrs) {
  assertx(entry.nonTrivialFuncEntry());

  // Set up the RegionContext for the tracelet selector.
  RegionContext ctx{entry, SBInvOffset{0}};

  for (uint32_t i = 0; i < inputTypes.size(); ++i) {
    auto type = inputTypes[i];
    assertx(type <= TCell);
    ctx.liveTypes.push_back({Location::Local{i}, type});
  }

  // Produce a tracelet for the callee.
  auto r = selectTracelet(
    ctx,
    TransKind::Live,
    maxBCInstrs,
    true /* inlining */
  );
  if (r) {
    r->setInlineContext(ctxType, inputTypes);
  }
  return r;
}

TransIDSet findTransIDsForCallee(const ProfData* profData, SrcKey entry,
                                 Type ctxType, std::vector<Type>& inputTypes) {
  assertx(entry.nonTrivialFuncEntry());
  auto const idvec = profData->funcProfTransIDs(entry.func());

  TransIDSet ret;
  FTRACE(2, "findTransIDForCallee: entry={}\n", showShort(entry));
  for (auto const id : idvec) {
    auto const rec = profData->transRec(id);
    if (rec->srcKey() != entry) continue;
    auto const region = rec->region();

    auto const isvalid = [&] () {
      if (rec->srcKey().hasThis() != ctxType.maybe(TObj)) {
        return false;
      }
      for (auto const& typeloc : region->entry()->typePreConditions()) {
        if (typeloc.location.tag() != LTag::Local) continue;
        auto const locId = typeloc.location.localId();

        if (locId < inputTypes.size() &&
            !(inputTypes[locId].maybe(typeloc.type))) {
          return false;
        }
      }
      return true;
    }();

    if (isvalid) ret.insert(id);
  }
  return ret;
}

RegionDescPtr selectCalleeCFG(SrcKey callerSk, SrcKey entry,
                              Type ctxType, std::vector<Type>& inputTypes,
                              int32_t maxBCInstrs,
                              AnnotationData* annotations) {
  assertx(entry.nonTrivialFuncEntry());
  auto const callee = entry.func();

  auto const profData = jit::profData();
  if (!profData) {
    traceRefusal(callerSk, callee, "no profData", annotations);
    return nullptr;
  }

  if (!profData->profiling(callee->getFuncId())) {
    traceRefusal(callerSk, callee,
                 fmt::format("no profiling data for callee FuncId: {}",
                                callee->getFuncId()),
                 annotations);
    return nullptr;
  }

  auto const dvIDs = findTransIDsForCallee(profData, entry, ctxType,
                                           inputTypes);

  if (dvIDs.empty()) {
    traceRefusal(callerSk, callee, "didn't find entry TransID for callee",
                 annotations);
    return nullptr;
  }

  TransCFG cfg(callee, profData, true /* inlining */);

  HotTransContext ctx;
  ctx.entries = dvIDs;
  ctx.cfg = &cfg;
  ctx.profData = profData;
  ctx.maxBCInstrs = maxBCInstrs;
  ctx.inlining = true;
  ctx.inputTypes = &inputTypes;

  bool truncated = false;
  auto r = selectHotCFG(ctx, &truncated);
  if (truncated) {
    traceRefusal(callerSk, callee, "callee region truncated due to BC size",
                 annotations);
    return nullptr;
  }
  if (r) {
    r->setInlineContext(ctxType, inputTypes);
  } else {
    traceRefusal(callerSk, callee, "failed selectHotCFG for callee",
                 annotations);
  }
  return r;
}
}

irgen::RegionAndLazyUnit selectCalleeRegion(
  const irgen::IRGS& irgs,
  SrcKey entry,
  Type ctxType,
  SrcKey callerSk,
  bool addedUninitNamed,
  std::vector<Type> inputTypes
) {
  assertx(entry.anyFuncEntry());
  auto const callee = entry.func();

  auto static inlineAttempts = ServiceData::createTimeSeries(
    "jit.inline.attempts", {ServiceData::StatsType::COUNT});
  inlineAttempts->addValue(1);

  auto kind = irgs.context.kind;
  auto annotationsPtr = mcgen::dumpTCAnnotation(kind) ?
                        irgs.unit.annotationData.get() : nullptr;

  if (ctxType == TBottom) {
    traceRefusal(callerSk, callee, "ctx is TBottom", annotationsPtr);
    return {callerSk, nullptr};
  }
  if (callee->isClosureBody()) {
    if (!callee->cls()) {
      ctxType = TNullptr;
    } else if (callee->hasThisInBody()) {
      ctxType = TObj;
    } else {
      ctxType = TCls;
    }
  } else {
    // Bail out if calling a static methods with an object ctx.
    if (ctxType.maybe(TObj) &&
        (callee->isStaticInPrologue() ||
         (!callerSk.hasThis() && isFCallClsMethod(callerSk.op())))) {
      traceRefusal(callerSk, callee, "calling static method with an object",
                   annotationsPtr);
      return {callerSk, nullptr};
    }
  }

  if (callee->cls()) {
    if (callee->isStatic() && !ctxType.maybe(TCls)) {
      traceRefusal(callerSk, callee, "calling a static method with an instance",
                   annotationsPtr);
      return {callerSk, nullptr};
    }
    if (!callee->isStatic() && !ctxType.maybe(TObj)) {
      traceRefusal(callerSk, callee,
                   "calling an instance method without an instance",
                   annotationsPtr);
      return {callerSk, nullptr};
    }
  }

  FTRACE(2, "selectCalleeRegion: callee = {}\n", callee->fullName()->data());
  assertx(inputTypes.size() == callee->numFuncEntryInputs());

  for (int i = 0; i < inputTypes.size(); ++i) {
    // If we don't have sufficient type information to inline the region return
    // early
    if (inputTypes[i] == TBottom) return {callerSk, nullptr};
    FTRACE(2, "input {}: {}\n", i + 1, inputTypes[i]);
  }

  while (entry.trivialDVFuncEntry()) {
    auto const param = entry.numEntryArgs();
    auto const paramIdx = param + callee->numNamedParams();
    assertx(paramIdx < inputTypes.size());
    inputTypes[paramIdx] = Type::cns(callee->params()[paramIdx].defaultValue);
    entry = SrcKey{callee, param + 1, addedUninitNamed, SrcKey::FuncEntryTag {}};
  }

  auto const maxBCInstrs = kind == TransKind::Live
    ? Cfg::Jit::MaxLiveRegionInstrs
    : Cfg::Jit::MaxRegionInstrs;

  if (profData()) {
    auto region = selectCalleeCFG(callerSk, entry, ctxType, inputTypes,
                                  maxBCInstrs, annotationsPtr);
    if (region) return {callerSk, region};
    // Special case: even if we don't have prof data for this func, if
    // it takes no arguments and returns a constant, it might be a
    // trivial function (IE, "return 123;"). Attempt to inline it
    // anyways using the tracelet selector.
    if (callee->numFuncEntryInputs() > 0) return {callerSk, nullptr};
    auto const retType =
      typeFromRAT(callee->repoReturnType(), callerSk.func()->cls());
    // Deliberately using hasConstVal, not admitsSingleVal, since we
    // don't want TInitNull, etc.
    if (!retType.hasConstVal()) return {callerSk, nullptr};
  }

  auto region = selectCalleeTracelet(entry, ctxType, inputTypes,
                                     maxBCInstrs);
  return {callerSk, region};
}

void setInliningMetadata(uint64_t baseProfCount,
                         const jit::hash_map<FuncId, uint32_t>& funcTargetCounts) {
  s_baseProfCount.store(baseProfCount);
  for (auto& [funcId, count] : funcTargetCounts) {
    s_funcTargetCounts.emplace(funcId, count);
  }
  FTRACE(1, "setInliningMetadata: {}\n", baseProfCount);
}

///////////////////////////////////////////////////////////////////////////////

void clearCachedInliningMetadata() {
  s_inlCostCache.clear();
  s_funcTargetCounts.clear();
}

void serializeCachedInliningCost(ProfDataSerializer& ser) {
  tl_heap.getCheck()->init();
  zend_get_bigint_data();
  auto size = s_inlCostCache.size();
  write_raw(ser, safe_cast<uint32_t>(size));
  for (auto const& p : s_inlCostCache) {
    write_srckey(ser, p.first.entryKey);
    p.first.ctxType.serialize(ser);
    write_raw(ser, safe_cast<uint32_t>(p.first.argTypes.size()));
    for (auto const& arg : p.first.argTypes) arg.serialize(ser);
    write_raw(ser, safe_cast<uint32_t>(p.second));
    if (--size == 0) break;
  }
}

void deserializeCachedInliningCost(ProfDataDeserializer& ser) {
  auto const numEntries = read_raw<uint32_t>(ser);
  for (uint32_t i = 0; i < numEntries; ++i) {
    auto srcKey = read_srckey(ser);
    auto ctxType = Type::deserialize(ser);
    auto const numArgs = read_raw<uint32_t>(ser);
    TinyVector<Type, 4> args;
    for (int64_t j = 0; j < numArgs; j++) {
      args.emplace_back(Type::deserialize(ser));
    }
    auto const cost = read_raw<uint32_t>(ser);

    s_inlCostCache.emplace(
      InlineRegionKey{srcKey, ctxType, std::move(args)},
      cost
    );
  }
}

///////////////////////////////////////////////////////////////////////////////
}
