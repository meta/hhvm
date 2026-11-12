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

#include <fmt/format.h>

#include "hphp/runtime/vm/func.h"

#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/server/memory-stats.h"
#include "hphp/runtime/vm/as-shared.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func-cleanup.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/reverse-data-map.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/source-location.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/system/systemlib.h"

#include "hphp/util/atomic-vector.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/fixed-vector.h"
#include "hphp/util/functional.h"
#include "hphp/util/ptr.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/trace.h"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbc)
const StaticString s_TrivialHHVMBuiltinWrapper("TrivialHHVMBuiltinWrapper");

std::atomic<bool> Func::s_treadmill;

InheritedRetTypesMap s_inheritedRetTypes;

Mutex g_funcsMutex;

/*
 * FuncId high water mark and FuncId -> Func* table.
 * We can't start with 0 since that's used for special sentinel value
 * in TreadHashMap
 */
static std::atomic<FuncId::Int> s_nextFuncId{1};
folly::ConcurrentHashMap<uint32_t, uint32_t> s_stableFuncIDs;

static ServiceData::ExportedCounter* s_funcid_counter =
  ServiceData::createCounter("admin.func_ids");
static InitFiniNode s_funcidCounterInit([]{
  // see comment on s_nextFuncId for why we start at 1.
  s_funcid_counter->setValue(1);
}, InitFiniNode::When::PostRuntimeOptions, "func_id counter init");

namespace {
inline int numProloguesForNumPositionals(int numPositionalParams) {
  // The number of prologues is numPositionalParams + 2. The extra 2 are needed for
  // the following cases:
  //   - arguments passed > numPositionalParams
  //   - no arguments passed
  return numPositionalParams + 2;
}
}

///////////////////////////////////////////////////////////////////////////////
// Creation and destruction.

Func::Func(Unit& unit, const StringData* name, Attr attrs)
  : m_name(name)
  , m_unit(&unit)
  , m_shared(nullptr)
  , m_attrs(attrs)
{
  m_allFlags.m_allFlags = 0;
  m_allFlags.m_shouldSampleJit = StructuredLog::coinflip(Cfg::Jit::SampleRate);
}

Func::Func(
  Unit& unit, const StringData* name, Attr attrs,
  const StringData *methCallerCls, const StringData *methCallerMeth)
  : m_name(name)
  , m_unit(&unit)
  , m_shared(nullptr)
  , m_attrs(attrs)
{
  m_allFlags.m_allFlags = 0;
  m_allFlags.m_shouldSampleJit = StructuredLog::coinflip(Cfg::Jit::SampleRate);
  assertx(methCallerCls != nullptr);
  assertx(methCallerMeth != nullptr);
  methCallerData().m_methName = methCallerMeth;
  methCallerData().m_clsName = methCallerCls;
}

Func::~Func() {
  // Should've deregistered in Func::destroy() or Func::freeClone()
  assertx(!m_allFlags.m_registeredInDataMap);
#ifndef NDEBUG
  validate();
  m_magic = ~m_magic;
#endif
}

void* Func::allocFuncMem(int numPositionalParams) {
  int numPrologues = numProloguesForNumPositionals(numPositionalParams);

  auto const funcSize =
    sizeof(Func) + numPrologues * sizeof(m_prologueTable[0])
    - sizeof(m_prologueTable);

  MemoryStats::LogAlloc(AllocKind::Func, funcSize);
  return low_malloc(funcSize);
}

void Func::startDestroy() {
  atomicFlags().set(Func::Flags::Zombie);
}

void Func::finishDestroy() {
  NamedFunc::removeFunc(this);

  if (hasInheritedReturnTypes()) {
    s_inheritedRetTypes.erase(getFuncId().toStableInt());
  }

  if (m_allFlags.m_registeredInDataMap) {
    deregisterInDataMap();
  }

  auto lambda = [this](){
    {
      auto metaLock = jit::tc::lockMetadata();
      auto fc = FuncCleanup::get(this);
      if (fc) fc->cleanup();
    }
    this->~Func();
    low_free(this);
  };
  if (s_treadmill.load(std::memory_order_acquire)) {
    Treadmill::enqueue(std::move(lambda));
    return;
  }

  lambda();
}

void Func::freeClone() {
  assertx(isPreFunc());
  assertx(m_cloned.flag.test_and_set());

  if (m_allFlags.m_registeredInDataMap) {
    deregisterInDataMap();
  }

  if (hasInheritedReturnTypes()) {
    m_attrs = static_cast<Attr>(m_attrs & ~AttrHasInheritedReturnTypes);
    s_inheritedRetTypes.erase(getFuncId().toStableInt());
  }

  {
    auto metaLock = jit::tc::lockMetadata();
    auto fc = FuncCleanup::get(this);
    if (fc) fc->cleanup();
  }

  m_cloned.flag.clear();
  assertx(!hasInheritedReturnTypes());
}

Func* Func::clone(Class* cls, const StringData* name) const {
  auto numPositionalParams = this->numPositionalParamsVariadic();

  // If this is a PreFunc (i.e., a Func on a PreClass) that is not already
  // being used as a regular Func by a Class, and we aren't trying to change
  // its name (since the name is part of the template for later clones), we can
  // reuse this same Func as the clone.
  bool const can_reuse =
    m_allFlags.m_isPreFunc && !name && !m_cloned.flag.test_and_set();

  Func* f = !can_reuse
    ? new (allocFuncMem(numPositionalParams)) Func(*this)
    : const_cast<Func*>(this);

  f->m_cloned.flag.test_and_set();
  f->initProloguesAndFuncEntry(numPositionalParams);
  if (name) f->m_name = name;
  f->clsData().m_impl = cls;
  f->setFullName();

  if (f != this) {
    f->m_allFlags.m_isPreFunc = false;
    f->m_allFlags.m_registeredInDataMap = false;
  }
  // We may be cloning from a prefunc which has this attribute set in the
  // context of the class it was cloned into, so unset it for the new clone.
  f->m_attrs = Attr(f->m_attrs & ~AttrHasInheritedReturnTypes);

  f->setNewFuncId();
  f->atomicFlags().unset(Func::Flags::Zombie);
  return f;
}

void Func::rescope(Class* ctx) {
  clsData().m_impl = ctx;
  setFullName();
}

///////////////////////////////////////////////////////////////////////////////
// Initialization.

void Func::init(int numPositionalParams) {
#ifndef NDEBUG
  m_magic = kMagic;
#endif
  setNewFuncId();
  // For methods, we defer setting the full name until m_cls is initialized
  if (!preClass()) {
    setFullName();
  } else {
    m_fullName = nullptr;
  }
  if (isSpecial(m_name)) {
    /*
     * We dont want these compiler generated functions to
     * appear in backtraces.
     */
    m_attrs = m_attrs | AttrNoInjection;
  }
  assertx(m_name);
  initProloguesAndFuncEntry(numPositionalParams);
}

void Func::initProloguesAndFuncEntry(int numPositionalParams) {
  int numPrologues = numProloguesForNumPositionals(numPositionalParams);

  if (!jit::mcgen::initialized()) {
    for (int i = 0; i < numPrologues; i++) {
      m_prologueTable[i] = nullptr;
    }
    return;
  }

  auto const& stubs = jit::tc::ustubs();

  TRACE(4, "initProloguesAndFuncEntry func %p %d\n", this, numPrologues);
  for (int i = 0; i < numPrologues; i++) {
    m_prologueTable[i] = stubs.fcallHelperThunk;
  }
  m_funcEntry = stubs.handleTranslateMainFuncEntry;
}

void Func::setFullName() {
  assertx(m_name->isStatic());
  Class *clazz = cls();
  if (clazz) {
    m_fullName = PackedStringPtr::fromRaw(kNeedsFullName);
  } else {
    m_fullName = m_name.get();

    // A scoped closure may not have a `cls', but we still need to preserve its
    // `methodSlot', which refers to its slot in its `baseCls' (which still
    // points to a subclass of Closure).
    if (!isMethod()) {
      setNamedFunc(NamedFunc::getOrCreate(m_name));
    }
  }
}

/* This function is expected to be called after all calls to appendParam
 * are complete. After, m_paramCounts is initialized such that the least
 * significant bit of this->m_paramCounts indicates whether the last
 * positional param is (non)variadic; and the rest of the bits are the
 * number of params.
 */
void Func::finishedEmittingParams(std::vector<ParamInfo>& fParams) {
  assertx(m_paramCounts == 0);
  assertx(m_inoutBits == 0);

  // Initialize m_paramCounts.
  shared()->m_params = fParams;
  m_paramCounts = fParams.size() << 1;
  if (!(m_attrs & AttrVariadicParam)) {
    m_paramCounts |= 1;
  }
  assertx(numParams() == fParams.size());

  // Build m_inoutBits.
  for (auto i = 0u; i < fParams.size(); ++i) {
    if (LIKELY(!fParams[i].isInOut())) continue;
    m_inoutBits |= 1u << std::min(i, kInoutFastCheckBits);
  }
}

void Func::registerInDataMap() {
  assertx((!m_allFlags.m_isPreFunc || m_cloned.flag.test_and_set()));
  assertx(!m_allFlags.m_registeredInDataMap);
  assertx(mallocEnd());
  data_map::register_start(this);
  m_allFlags.m_registeredInDataMap = true;
}

void Func::deregisterInDataMap() {
  assertx(m_allFlags.m_registeredInDataMap);
  assertx((!m_allFlags.m_isPreFunc || m_cloned.flag.test_and_set()));
  data_map::deregister(this);
  m_allFlags.m_registeredInDataMap = false;
}

bool Func::isMemoizeImplName(const StringData* name) {
  return name->size() > 13 && !memcmp(name->data() + name->size() - 13,
                                      "$memoize_impl", 13);
}

const StringData* Func::genMemoizeImplName(const StringData* origName) {
  return makeStaticString(fmt::format("{}$memoize_impl", origName->data()));
}

std::pair<const StringData*, const StringData*> Func::getMethCallerNames(
  const StringData* name) {
  assertx(name->size() > 11 && !memcmp(name->data(), "MethCaller$", 11));
  auto clsMethName = name->slice();
  clsMethName.uncheckedAdvance(11);
  auto const sep = folly::qfind(clsMethName, folly::StringPiece("$"));
  assertx(sep != std::string::npos);
  auto cls = clsMethName.uncheckedSubpiece(0, sep);
  auto meth = clsMethName.uncheckedSubpiece(sep + 1);
  return std::make_pair(makeStaticString(cls), makeStaticString(meth));
}

///////////////////////////////////////////////////////////////////////////////
// FuncId manipulation.

FuncId::Int Func::maxFuncIdNum() {
  return s_nextFuncId.load(std::memory_order_acquire);
}

void Func::setNewFuncId() {
  auto const id = s_nextFuncId.fetch_add(1, std::memory_order_acq_rel);
  s_funcid_counter->increment();

  if (!Cfg::Repo::Authoritative) {
    s_stableFuncIDs.insert_or_assign(getFuncId().toInt(), id);
  }
}

uint32_t Func::getStableId() const {
  if (Cfg::Repo::Authoritative) return getFuncId().toInt();
  auto const it = s_stableFuncIDs.find(getFuncId().toInt());
  assertx(it != s_stableFuncIDs.end());
  return it->second;
}

const Func* Func::fromFuncId(FuncId id) {
  auto const func = id.getFunc();
  func->validate();
  return func;
}

bool Func::isFuncIdValid(FuncId id) {
  return !id.isInvalid() && !id.isDummy();
}

///////////////////////////////////////////////////////////////////////////////
// Bytecode.

bool Func::isEntry(Offset offset) const {
  return offset == 0 || isDVEntry(offset);
}

bool Func::isDVEntry(Offset offset) const {
  auto const nparams = numNonVariadicParams();
  for (int i = 0; i < nparams; i++) {
    const ParamInfo& pi = params()[i];
    if (pi.hasDefaultValue() && pi.funcletOff == offset) return true;
  }
  return false;
}

bool Func::hasNonTrivialDVFuncEntry() const {
  auto const nparams = numNonVariadicParams();
  for (auto i = numNamedParams() + numRequiredPositionalParams(); i < nparams; ++i) {
    if (!params()[i].hasTrivialDefaultValue()) return true;
  }
  return false;
}


Offset Func::getPrologueEntryForNumPositionals(int numPositionalsPassed) const {
  assertx(numPositionalsPassed >= 0);
  auto const nparams = numNonVariadicParams();
  for (unsigned i = numPositionalsPassed + numNamedParams(); i < nparams; i++) {
    const Func::ParamInfo& pi = params()[i];
    if (pi.hasDefaultValue()) {
      return pi.funcletOff;
    }
  }
  return 0;
}

Offset Func::getFuncEntryForNumPositionals(int posArgc,
                                           bool allNamedParamsPassed) const {
  assertx(posArgc >= 0);
  auto const nparams = numNonVariadicParams();
  for (unsigned i = numNamedParams() + posArgc; i < nparams; i++) {
    const Func::ParamInfo& pi = params()[i];
    if (pi.hasDefaultValue()) {
      return pi.funcletOff;
    }
  }
  if (!allNamedParamsPassed) {
    return getNamedParamsFuncEntry();
  }
  return 0;
}

Offset Func::getNamedParamsFuncEntry() const {
  for (unsigned i = 0; i < numNamedParams(); i++) {
    const Func::ParamInfo& pi = params()[i];
    if (pi.hasDefaultValue()) {
      return pi.funcletOff;
    }
  }
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Parameters.

bool Func::isInOut(int32_t arg) const {
  assertx(arg >= 0);
  if (LIKELY(arg < kInoutFastCheckBits)) {
    return m_inoutBits & (1ull << arg);
  }

  if (arg >= numParams()) return false;
  return params()[arg].isInOut();
}

bool Func::isReadonly(int32_t arg) const {
  assertx(arg >= 0);
  if (arg >= numParams()) return false;
  return params()[arg].isReadonly();
}

bool Func::isNamed(int32_t arg) const {
  assertx(arg >= 0);
  if (arg >= numParams()) return false;
  return params()[arg].isNamed();
}

uint32_t Func::numInOutParams() const {
  uint32_t count = folly::popcount(m_inoutBits);
  if (LIKELY(static_cast<int32_t>(m_inoutBits) >= 0)) {
    // The sign bit is not set, so only args 0..31 can possibly be inout.
    return count;
  }

  assertx(numParams() > kInoutFastCheckBits);
  for (auto i = numParams() - 1; i >= kInoutFastCheckBits; --i) {
    count += params()[i].isInOut() ? 1 : 0;
  }
  // Minus one for the sign bit.
  return count - 1;
}

uint32_t Func::numInOutParamsForArgs(int32_t numArgs) const {
  if (!takesInOutParams()) return 0;
  uint32_t i = 0;
  for (int p = 0; p < numArgs; ++p) i += isInOut(p);
  return i;
}

///////////////////////////////////////////////////////////////////////////////
// Locals, iterators, and stack.

Id Func::lookupVarId(const StringData* name) const {
  assertx(name != nullptr);
  return shared()->m_localNames.findIndex(name);
}

uint32_t Func::numClosureUseLocals() const {
  assertx(isClosureBody());
  auto const cls = implCls();
  return cls->numDeclProperties() - (cls->hasClosureCoeffectsProp() ? 1 : 0);
}

///////////////////////////////////////////////////////////////////////////////
// Persistence.

bool Func::isImmutableFrom(const Class* cls) const {
  if (!Cfg::Repo::Authoritative) return false;
  assertx(cls && cls->lookupMethod(name()) == this);
  if (attrs() & AttrNoOverride) {
    return true;
  }
  if (cls->preClass()->attrs() & AttrNoOverride) {
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// JIT data.

int Func::numPrologues() const {
  return numProloguesForNumPositionals(numPositionalParamsVariadic());
}

void Func::resetPrologue(int numParams) {
  auto const& stubs = jit::tc::ustubs();
  m_prologueTable[numParams] = stubs.fcallHelperThunk;
}

void Func::resetFuncEntry() {
  m_funcEntry = jit::tc::ustubs().handleTranslateMainFuncEntry;
}

///////////////////////////////////////////////////////////////////////////////
// Reified Generics

const GenericsInfo& Func::getGenericsInfo() const {
  auto const ex = extShared();
  assertx(ex);
  return ex->m_genericsInfo;
}

///////////////////////////////////////////////////////////////////////////////
// Pretty printer.

void Func::print_attrs(std::ostream& out, Attr attrs) {
  if (attrs & AttrStatic)    { out << " static"; }
  if (attrs & AttrPublic)    { out << " public"; }
  if (attrs & AttrProtected) { out << " protected"; }
  if (attrs & AttrPrivate)   { out << " private"; }
  if (attrs & AttrInternal)  { out << " internal"; }
  if (attrs & AttrAbstract)  { out << " abstract"; }
  if (attrs & AttrFinal)     { out << " final"; }
  if (attrs & AttrNoOverride){ out << " (no-override)"; }
  if (attrs & AttrInterceptable) { out << " (interceptable)"; }
  if (attrs & AttrPersistent) { out << " (persistent)"; }
  if (attrs & AttrBuiltin) { out << " (builtin)"; }
  if (attrs & AttrIsFoldable) { out << " (foldable)"; }
  if (attrs & AttrNoInjection) { out << " (no_injection)"; }
  if (attrs & AttrSupportsAsyncEagerReturn) { out << " (can_async_eager_ret)"; }
  if (attrs & AttrEagerOnly) { out << " (eager-only)"; }
  if (attrs & AttrDynamicallyCallable) { out << " (dyn_callable)"; }
  if (attrs & AttrIsMethCaller) { out << " (is_meth_caller)"; }
  if (attrs & AttrNoRecording) { out << " (no_recording)"; }
}

void Func::prettyPrint(std::ostream& out, const PrintOpts& opts) const {
  if (opts.name) {
    if (preClass() != nullptr) {
      out << "Method";
      print_attrs(out, m_attrs);
      if (isPhpLeafFn()) out << " (leaf)";
      if (isMemoizeWrapper()) out << " (memoize_wrapper)";
      if (isMemoizeWrapperLSB()) out << " (memoize_wrapper_lsb)";
      if (cls() != nullptr) {
        out << ' ' << fullName()->data();
      } else {
        out << ' ' << preClass()->name()->data() << "::" << m_name->data();
      }
    } else {
      out << "Function";
      print_attrs(out, m_attrs);
      if (isPhpLeafFn()) out << " (leaf)";
      if (isMemoizeWrapper()) out << " (memoize_wrapper)";
      if (isMemoizeWrapperLSB()) out << " (memoize_wrapper_lsb)";
      out << ' ' << m_name->data();
    }

    out << std::endl;
  }

  if (opts.metadata) {
    const ParamInfoVec& params = shared()->m_params;
    for (uint32_t i = 0; i < params.size(); ++i) {
      auto const& param = params[i];
      out << " Param: " << localVarName(i)->data();
      for (auto const& tc : param.typeConstraints.range()) {
        if (tc.hasConstraint()) {
          out << " " << tc.displayName(cls(), true);
        }
      }
      if (auto userType = param.userType.get(unit())) {
        out << " (" << userType->data() << ")";
      }
      if (param.funcletOff != kInvalidOffset) {
        out << " DV" << " at " << param.funcletOff;
        if (auto phpCode = param.phpCode.get(unit())) {
          out << " = " << phpCode->data();
        }
      }
      out << std::endl;
    }

    if (returnUserType() && !returnUserType()->empty()) {
      out << " Ret: ";
      for (auto const& tc : returnTypeConstraints().range()) {
        if (tc.hasConstraint()) {
          out << " " << tc.displayName(cls(), true);
        }
      }
      if (returnUserType() && !returnUserType()->empty()) {
        out << " (" << returnUserType()->data() << ")";
      }
      out << std::endl;
    }

    if (repoReturnType().tag() != RepoAuthType::Tag::Cell) {
      out << "repoReturnType: " << show(repoReturnType()) << '\n';
    }
    if (repoAwaitedReturnType().tag() != RepoAuthType::Tag::Cell) {
      out << "repoAwaitedReturnType: " << show(repoAwaitedReturnType()) << '\n';
    }
    out << "maxStackCells: " << maxStackCells() << '\n'
        << "numLocals: " << numLocals() << '\n'
        << "numIterators: " << numIterators() << '\n';

    const EHEntVec& ehtab = shared()->m_ehtab;
    size_t ehId = 0;
    for (auto it = ehtab.begin(); it != ehtab.end(); ++it, ++ehId) {
      out << " EH " << ehId << " Catch for " <<
        it->m_base << ":" << it->m_past;
      if (it->m_parentIndex != -1) {
        out << " outer EH " << it->m_parentIndex;
      }
      if (it->m_iterId != -1) {
        out << " iterId " << it->m_iterId;
      }
      out << " handle at " << it->m_handler;
      if (it->m_end != kInvalidOffset) {
        out << ":" << it->m_end;
      }
      if (it->m_parentIndex != -1) {
        out << " parentIndex " << it->m_parentIndex;
      }
      out << std::endl;
    }
  }

  if (opts.startOffset != kInvalidOffset) {
    auto startOffset = std::max(0, opts.startOffset);
    auto stopOffset = std::min(bclen(), opts.stopOffset);

    if (startOffset >= stopOffset) {
      return;
    }

    auto it = at(startOffset);
    auto const stop = at(stopOffset);
    int prevLineNum = -1;
    while (it < stop) {
      if (opts.showLines) {
        auto const lineNum =
          SourceLocation::getLineNumber(getOrLoadLineTable(), offsetOf(it));
        if (lineNum != prevLineNum) {
          out << "  // line " << lineNum << std::endl;
          prevLineNum = lineNum;
        }
      }

      out << std::string(opts.indentSize, ' ')
          << std::setw(4) << offsetOf(it) << ": "
          << instrToString(it, this)
          << std::endl;
      it += instrLen(it);
    }
  }
}


///////////////////////////////////////////////////////////////////////////////
// SharedData.

Func::SharedData::SharedData(BCPtr bc, Offset bclen,
                             PreClass* preClass, int sn, int line1,
                             int line2, bool isPhpLeafFn)
  : m_bc(bc.isPtr() ? BCPtr::FromPtr(allocateBCRegion(bc.ptr(), bclen)) : bc)
  , m_preClass(preClass)
  , m_line1(line1)
  , m_originalUnit(nullptr)
  , m_numLocals(0)
  , m_numIterators(0)
{
  m_allFlags.m_isClosureBody = false;
  m_allFlags.m_isAsync = false;
  m_allFlags.m_isGenerator = false;
  m_allFlags.m_isPairGenerator = false;
  m_allFlags.m_isGenerated = false;
  m_allFlags.m_hasExtendedSharedData = false;
  m_allFlags.m_returnByValue = false;
  m_allFlags.m_isUntrustedReturnType = false;
  m_allFlags.m_isMemoizeWrapper = false;
  m_allFlags.m_isMemoizeWrapperLSB = false;
  m_allFlags.m_memoizeICType = Func::MemoizeICType::MakeICInaccessible;
  m_allFlags.m_isPhpLeafFn = isPhpLeafFn;
  m_allFlags.m_hasReifiedGenerics = false;
  m_bclenSmall = std::min<uint32_t>(bclen, kSmallDeltaLimit);
  m_line2Delta = std::min<uint32_t>(line2 - line1, kSmallDeltaLimit);
  m_sn = std::min<uint32_t>(sn, kSmallDeltaLimit);
}

Func::SharedData::~SharedData() {
  if (auto bc = m_bc.copy(); bc.isPtr()) {
    freeBCRegion(bc.ptr(), bclen());
  }
  if (auto table = m_lineTable.copy(); table.isPtr()) {
    delete table.ptr();
  }
  Func::s_extendedLineInfo.erase(this);
}

void Func::SharedData::atomicRelease() {
  if (UNLIKELY(m_allFlags.m_hasExtendedSharedData)) {
    delete (ExtendedSharedData*)this;
  } else {
    delete this;
  }
}

Func::ExtendedSharedData::~ExtendedSharedData() {
}

///////////////////////////////////////////////////////////////////////////////

void logFunc(const Func* func, StructuredLogEntry& ent) {
  auto const attrs = attrs_to_vec(AttrContext::Func, func->attrs());
  std::set<folly::StringPiece> attrSet(attrs.begin(), attrs.end());

  if (func->isMemoizeWrapper()) attrSet.emplace("memoize_wrapper");
  if (func->isMemoizeWrapperLSB()) attrSet.emplace("memoize_wrapper_lsb");
  if (func->isMemoizeImpl()) attrSet.emplace("memoize_impl");
  if (func->isAsync()) attrSet.emplace("async");
  if (func->isGenerator()) attrSet.emplace("generator");
  if (func->isClosureBody()) attrSet.emplace("closure_body");
  if (func->isPairGenerator()) attrSet.emplace("pair_generator");
  if (func->hasVariadicCaptureParam()) attrSet.emplace("variadic_param");
  if (func->isPhpLeafFn()) attrSet.emplace("leaf_function");
  if (func->cls() && func->cls()->isPersistent()) attrSet.emplace("persistent");

  ent.setSet("func_attributes", attrSet);

  ent.setInt("num_params", func->numNonVariadicParams());
  ent.setInt("num_locals", func->numLocals());
  ent.setInt("num_iterators", func->numIterators());
  ent.setInt("frame_cells", func->numSlotsInFrame());
  ent.setInt("max_stack_cells", func->maxStackCells());
}

///////////////////////////////////////////////////////////////////////////////
// Lookup

const StaticString s_DebuggerMain("__DebuggerMain");

void Func::def(Func* func) {
  assertx(!func->isMethod());

  // Don't define the __debugger_main() function
  DEBUGGER_ATTACHED_ONLY(if (func->userAttributes().contains(s_DebuggerMain.get())) { return; });

  auto const ne = func->getNamedFunc();

  Func* f = ne->getCachedFunc();
  if (f == nullptr) {
    if (Cfg::Eval::LockFreeFuncDef) {
      auto const persistent = func->isPersistent();
      assertx(!persistent ||
              (Cfg::Eval::ForceAllPersistent ||
               Cfg::Repo::Authoritative || func->unit()->isSystemLib()));

      // Binding an RDS handle is an atomic operation. For persistent handles an
      // initial value can ve specified when binding succeeds which is also
      // atomically assigned. For normal handles the initial value is ignored.
      PackedPtr<Func> init = func;
      ne->m_cachedFunc.bind(
        persistent ? rds::Mode::Persistent : rds::Mode::Normal,
        rds::LinkName{"Func", func->name()},
        &init
      );

      f = ne->getCachedFunc();
      if (!f) ne->m_cachedFunc.initWith(func);
      if (!f || f == func) ne->setFunc(func);

      DEBUGGER_ATTACHED_ONLY(phpDebuggerDefFuncHook(func));
    } else {
      std::unique_lock<Mutex> l(g_funcsMutex);
      f = ne->getCachedFunc();
      if (f == nullptr) {
        auto const persistent = func->isPersistent();
        assertx(!persistent || (Cfg::Eval::ForceAllPersistent || Cfg::Repo::Authoritative || func->unit()->isSystemLib()));

        if (!ne->m_cachedFunc.bound()) {
          ne->m_cachedFunc.bind(
            persistent ? rds::Mode::Persistent : rds::Mode::Normal,
            rds::LinkName{"Func", func->name()}
          );
        }
        ne->m_cachedFunc.initWith(func);
        ne->setFunc(func);
        l.unlock();

        DEBUGGER_ATTACHED_ONLY(phpDebuggerDefFuncHook(func));
      }
    }
  }

  // If the function didn't exist before we are good
  if (f == nullptr) {
    return;
  }

  // Otherwise check if we match or show the right error message
  if (f == func) {
    assertx(!Cfg::Repo::Authoritative ||
            (f->isPersistent() && ne->m_cachedFunc.isPersistent()));
    return;
  }
  if (func->attrs() & AttrIsMethCaller) return;
  if (f->isBuiltin()) {
    assertx(!func->isBuiltin());
    raise_error(Strings::REDECLARE_BUILTIN, func->name()->data());
  }
  raise_error(Strings::FUNCTION_ALREADY_DEFINED, func->name()->data());
}

Func* Func::lookup(const StringData* name) {
  const NamedFunc* ne = NamedFunc::getNoCreate(name);
  return ne ? ne->getCachedFunc() : nullptr;
}

Func* Func::lookupBuiltin(const StringData* name) {
  // Builtins are either persistent (the normal case), or defined at the
  // beginning of every request (if JitEnableRenameFunction or interception is
  // enabled). In either case, they're unique, so they should be present in the
  // NamedFunc.
  auto const f = Func::lookup(name);
  return (f && f->isPersistent() && f->isBuiltin()) ? f : nullptr;
}

// If the function is a trivial builtin, that is not interceptable, and
// is persistent, then unwrap it.
Func* Func::unwrap() {
  if (!Cfg::Eval::ReplaceTrivialBuiltins) return this;
  // Already unwrapped
  if (Cfg::Repo::Authoritative) return this;
  if (RO::funcIsRenamable(name())) return this;

  auto const it = userAttributes().find(s_TrivialHHVMBuiltinWrapper.get());
  if (it != userAttributes().end()) {
    assertx(tvIsVec(it->second));
    auto const args = it->second.m_data.parr;
    assertx(args->size() == 1);
    auto const wrappedFunc = args->at(int64_t{0});
    assertx(tvIsString(wrappedFunc));
    assertx(wrappedFunc.m_data.pstr->isStatic());
    auto const resolved =
      NamedFunc::getOrCreate(wrappedFunc.m_data.pstr)->func();
    // The target may be undefined; the wrapper body does a checked call.
    if (resolved) return resolved;
  }
  return this;
}

/*
 * Find a function which always uniquely maps to the given name in the context
 * of the given unit. A function so returned can be used directly in the TC as
 * it will not change.
 */
const Func::FuncLookup Func::lookupKnownMaybe(const StringData* name, const Unit* unit) {
  auto const func = [&]() -> const Func* {
    auto const ne = NamedFunc::getOrCreate(name);
    auto const f = ne->func();
    if (!f) return nullptr;
    return f->unwrap();
  }();

  if (!func || RO::funcIsRenamable(func->name())) return Func::none();
  // In non-repo mode while the function must be available in this unit, it
  // may be de-duplication on load. This may mean that while the func is
  // available it is not immutable in the current compilation unit. The order
  // of the de-duplication can also differ between requests.
  if (func->isMethCaller() && !Cfg::Repo::Authoritative) return Func::none();
  assertx(!func->cls());
  if (func->isPersistent()) return Func::exact(func);
  if (func->unit() == unit) return Func::exact(func);
  return Cfg::Sandbox::Speculate ? Func::maybe(func) : Func::none();
}

inline const Func* Func::lookupKnown(const StringData* name, const Unit* unit) {
  auto const res = Func::lookupKnownMaybe(name, unit);
  switch (res.tag) {
    case Func::FuncLookupResult::Exact:
      return res.func;
    case Func::FuncLookupResult::Maybe:
    case Func::FuncLookupResult::None:
      return nullptr;
  }
}


Func* Func::load(const NamedFunc* ne, const StringData* name) {
  Func* func = ne->getCachedFunc();
  if (LIKELY(func != nullptr)) return func;
  if (AutoloadHandler::s_instance->autoloadFunc(
        const_cast<StringData*>(name))) {
    func = ne->getCachedFunc();
  }
  return func;
}

Func* Func::load(const StringData* name) {
  OptString normStr;
  auto ne = NamedFunc::getNoCreate(name, &normStr);
  if (ne != nullptr) {
    // Try to fetch from NamedFunc cache
    Func* func_ = ne->getCachedFunc();
    if (LIKELY(func_ != nullptr)) return func_;
  }

  // Normalize the namespace
  if (normStr) {
    name = normStr.get();
  }

  // Autoload the function
  auto& handler = AutoloadHandler::s_instance;
  if (handler->autoloadFunc(const_cast<StringData*>(name))) {
    if (!ne) ne = NamedFunc::getNoCreate(name, &normStr);
    if (ne) return ne->getCachedFunc();
  }
  return nullptr;
}

namespace {
void handleModuleBoundaryViolation(const Func* callee, const Func* caller) {
  if (!Cfg::Eval::EnforceModules || !callee || !caller) return;
  if (will_symbol_raise_module_boundary_violation(callee, caller)) {
    raiseModuleBoundaryViolation(nullptr, callee, caller->moduleName());
  }
}
} // namespace

Func* Func::resolve(const NamedFunc* ne, const StringData* name,
                    const Func* callerFunc) {
  Func* func = load(ne, name);
  handleModuleBoundaryViolation(func, callerFunc);
  return func;
}

Func* Func::resolve(const StringData* name, const Func* callerFunc) {
  Func* func = load(name);
  handleModuleBoundaryViolation(func, callerFunc);
  return func;
}

///////////////////////////////////////////////////////////////////////////////
// Code locations.

Func::ExtendedLineInfoCache Func::s_extendedLineInfo;

void Func::setLineTable(LineTable lineTable) {
  auto& table = shared()->m_lineTable;
  auto lock = table.lock_for_update();
  assertx(table.copy().isPtr() && !table.copy().ptr());
  lock.update(
    LineTablePtr::FromPtr(new LineTable{std::move(lineTable)})
  );
}

void Func::setLineTable(LineTablePtr::Token token) {
  assertx(Cfg::Repo::Authoritative);
  auto& table = shared()->m_lineTable;
  auto lock = table.lock_for_update();
  assertx(table.copy().isPtr() && !table.copy().ptr());
  lock.update(LineTablePtr::FromToken(token));
}

void Func::stashExtendedLineTable(SourceLocTable table) const {
  ExtendedLineInfoCache::accessor acc;
  if (s_extendedLineInfo.insert(acc, shared())) {
    acc->second.sourceLocTable = std::move(table);
  }
}

/*
 * Return the Unit's SourceLocTable, extracting it from the repo if
 * necessary.
 */
const SourceLocTable& Func::getLocTable() const {
  auto const sharedData = shared();
  {
    ExtendedLineInfoCache::const_accessor acc;
    if (s_extendedLineInfo.find(acc, sharedData)) {
      return acc->second.sourceLocTable;
    }
  }
  static SourceLocTable empty;
  return empty;
}

/*
 * Return a copy of the Func's line to OffsetRangeVec table.
 */
LineToOffsetRangeVecMap Func::getLineToOffsetRangeVecMap() const {
  auto const sharedData = shared();
  {
    ExtendedLineInfoCache::const_accessor acc;
    if (s_extendedLineInfo.find(acc, sharedData)) {
      if (!acc->second.lineToOffsetRange.empty()) {
        return acc->second.lineToOffsetRange;
      }
    }
  }

  LineToOffsetRangeVecMap map;
  auto const& srcLocTable = getLocTable();
  SourceLocation::generateLineToOffsetRangesMap(srcLocTable, map);

  ExtendedLineInfoCache::accessor acc;
  if (!s_extendedLineInfo.find(acc, sharedData)) {
    always_assert_flog(0, "ExtendedLineInfoCache was not found when it should "
      "have been");
  }
  if (acc->second.lineToOffsetRange.empty()) {
    acc->second.lineToOffsetRange = std::move(map);
  }
  return acc->second.lineToOffsetRange;
}

const LineTable* Func::getLineTable() const {
  auto const table = shared()->m_lineTable.copy();
  if (table.isPtr()) {
    assertx(table.ptr());
    return table.ptr();
  }
  return nullptr;
}

const LineTable& Func::getOrLoadLineTable() const {
  if (auto const table = getLineTable()) return *table;

  assertx(Cfg::Repo::Authoritative);

  auto& wrapper = shared()->m_lineTable;
  auto lock = wrapper.lock_for_update();

  auto const table = wrapper.copy();
  if (table.isPtr()) return *table.ptr();

  auto newTable = new LineTable{
    FuncEmitter::loadLineTableFromRepo(m_unit->sn(), table.token())
  };
  lock.update(LineTablePtr::FromPtr(newTable));
  return *newTable;
}

LineTable Func::getOrLoadLineTableCopy() const {
  auto const table = shared()->m_lineTable.copy();
  if (table.isPtr()) {
    assertx(table.ptr());
    return *table.ptr();
  }
  assertx(Cfg::Repo::Authoritative);
  return FuncEmitter::loadLineTableFromRepo(m_unit->sn(), table.token());
}

int Func::getLineNumber(Offset offset) const {
  auto const findLine = [&] {
    // lineMap is an atomically acquired bitwise copy of m_lineMap,
    // with no destructor
    auto lineMap(shared()->m_lineMap.get());
    if (lineMap->empty()) return INT_MIN;
    auto const it = std::upper_bound(
      lineMap->begin(), lineMap->end(),
      offset,
      [] (Offset info, const LineInfo& elm) {
        return info < elm.first.past;
      }
    );
    if (it != lineMap->end() && it->first.base <= offset) return it->second;
    return INT_MIN;
  };

  auto line = findLine();
  if (line != INT_MIN) return line;

  // Updating m_lineMap while coverage is enabled can cause the
  // treadmill to fill with an enormous number of resized maps.
  if (UNLIKELY(g_context && (m_unit->isCoverageEnabled() ||
                             RID().getCoverage()))) {
    return SourceLocation::getLineNumber(getOrLoadLineTable(), offset);
  }

  auto lock = shared()->m_lineMap.lock_for_update();
  line = findLine();
  if (line != INT_MIN) return line;

  auto const info = SourceLocation::getLineInfo(getOrLoadLineTable(), offset);
  auto copy = shared()->m_lineMap.copy();
  auto const it = std::upper_bound(
    copy.begin(), copy.end(),
    info,
    [&] (const LineInfo& a, const LineInfo& b) {
      return a.first.base < b.first.past;
    }
  );
  assertx(it == copy.end() ||
          (it->first.past > offset && it->first.base > offset));
  copy.insert(it, info);
  auto old = lock.update(std::move(copy));
  Treadmill::enqueue([old = std::move(old)] () mutable { old.clear(); });
  return info.second;
}

bool Func::getSourceLoc(Offset offset, SourceLoc& sLoc) const {
  auto const& sourceLocTable = getLocTable();
  return SourceLocation::getLoc(sourceLocTable, offset, sLoc);
}

bool Func::getOffsetRange(Offset offset, OffsetRange& range) const {
  auto line = getLineNumber(offset);
  if (line == -1) return false;

  auto map = getLineToOffsetRangeVecMap();
  auto it = map.find(line);
  if (it != map.end()) {
    for (auto o : it->second) {
      if (offset >= o.base && offset < o.past) {
        range = o;
        return true;
      }
    }
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// Bytecode

unsigned char*
allocateBCRegion(const unsigned char* bc, size_t bclen) {
  g_hhbc_size->addValue(bclen);
  auto arena = get_swappable_readonly_arena();
  auto mem = static_cast<unsigned char*>(
    arena ? arena->allocate(bclen) : malloc(bclen));
  std::copy(bc, bc + bclen, mem);
  return mem;
}

void freeBCRegion(const unsigned char* bc, size_t bclen) {
  // Can't free bytecode arena memory.
  if (Cfg::Repo::Authoritative) return;

  if (debug) {
    // poison released bytecode
    memset(const_cast<unsigned char*>(bc), 0xff, bclen);
  }
  free(const_cast<unsigned char*>(bc));
  g_hhbc_size->addValue(-int64_t(bclen));
}

PC Func::loadBytecode() {
  assertx(Cfg::Repo::Authoritative);
  auto& wrapper = shared()->m_bc;
  auto lock = wrapper.lock_for_update();
  auto const bc = wrapper.copy();
  if (bc.isPtr()) return bc.ptr();
  auto const length = bclen();
  g_hhbc_size->addValue(length);
  auto arena = get_swappable_readonly_arena();
  auto mem = static_cast<unsigned char*>(
    arena ? arena->allocate(length) : malloc(length));
  RepoFile::readRawFromUnit(m_unit->sn(), bc.token(), mem, length);
  lock.update(BCPtr::FromPtr(mem));
  return mem;
}

///////////////////////////////////////////////////////////////////////////////
// Coverage

namespace {
RDS_LOCAL(uint32_t, tl_saved_coverage_index);
RDS_LOCAL_NO_CHECK(Array, tl_called_functions);
rds::Link<uint32_t, rds::Mode::Local> s_coverage_index;

using CoverageLinkMap = tbb::concurrent_hash_map<
  const StringData*,
  rds::Link<uint32_t, rds::Mode::Local>
>;

struct EmbeddedCoverageLinkMap {
  explicit operator bool() const { return inited; }

  CoverageLinkMap* operator->() {
    assertx(inited);
    return reinterpret_cast<CoverageLinkMap*>(&data);
  }
  CoverageLinkMap& operator*() { assertx(inited); return *operator->(); }

  void emplace(uint32_t size) {
    assertx(!inited);
    new (&data) CoverageLinkMap(size);
    inited = true;
  }

  void clear() {
    if (inited) {
      operator*().~CoverageLinkMap();
      inited = false;
    }
  }

private:
  typename std::aligned_storage<
    sizeof(CoverageLinkMap),
    alignof(CoverageLinkMap)
  >::type data;
  bool inited;
};

EmbeddedCoverageLinkMap s_covLinks;
static InitFiniNode s_covLinksReinit([]{
  if (Cfg::Repo::Authoritative || !Cfg::Eval::EnableFuncCoverage) return;
  s_covLinks.emplace(Cfg::Eval::FuncCountHint);
}, InitFiniNode::When::PostRuntimeOptions, "s_funcVec reinit");

InitFiniNode s_clear_called_functions([]{
  tl_called_functions.nullOut();
  if (s_coverage_index.bound()) *s_coverage_index = 0;
}, InitFiniNode::When::RequestFini, "tl_called_functions clear");
}

rds::Handle Func::GetCoverageIndex() {
  if (!s_coverage_index.bound()) {
    s_coverage_index.bind(rds::Mode::Local, rds::LinkID{"FuncCoverageIndex"});
  }
  return s_coverage_index.handle();
}

rds::Handle Func::getCoverageHandle() const {
  assertx(!Cfg::Repo::Authoritative && Cfg::Eval::EnableFuncCoverage);
  assertx(!isNoInjection() && !isMethCaller());

  CoverageLinkMap::const_accessor cnsAcc;
  if (s_covLinks->find(cnsAcc, fullName())) {
    assertx(cnsAcc->second.bound());
    return cnsAcc->second.handle();
  }

  CoverageLinkMap::accessor acc;
  if (s_covLinks->insert(acc, fullName())) {
    assertx(!acc->second.bound());
    acc->second.bind(
      rds::Mode::Local, rds::LinkName{"FuncCoverageFlag", fullName()}
    );
  }
  assertx(acc->second.bound());
  return acc->second.handle();
}

void Func::EnableCoverage() {
  assertx(g_context);

  if (Cfg::Repo::Authoritative) {
    SystemLib::throwInvalidOperationExceptionObject(
      "Cannot enable function call coverage in repo authoritative mode"
    );
  }
  if (!Cfg::Eval::EnableFuncCoverage) {
    SystemLib::throwInvalidOperationExceptionObject(
      "Cannot enable function call coverage (you must set "
      "Eval.EnableFuncCoverage = true)"
    );
  }
  if (!tl_called_functions.isNull()) {
    SystemLib::throwInvalidOperationExceptionObject(
      "Function call coverage already enabled"
    );
  }

  GetCoverageIndex(); // bind the handle
  if (!*tl_saved_coverage_index) *tl_saved_coverage_index = 1;
  *s_coverage_index = (*tl_saved_coverage_index)++;
  tl_called_functions.emplace(Array::CreateDict());
}

rds::Handle Func::debuggerIntrSetHandle() const {
  assertx(shared()->m_funcHasDebuggerIntr.bound());
  return shared()->m_funcHasDebuggerIntr.handle();
}

rds::Link<bool, rds::Mode::Normal> Func::debuggerIntrSetLink() const {
  assertx(shared()->m_funcHasDebuggerIntr.bound());
  return shared()->m_funcHasDebuggerIntr;
}

void Func::ensureDebuggerIntrSetLinkBound() const {
  if (!shared()->m_funcHasDebuggerIntr.bound()) {
    shared()->m_funcHasDebuggerIntr.bind(
      rds::Mode::Normal,
      rds::LinkName{"FunctionDebuggerIntr", name()}
    );
  }
}

Array Func::GetCoverage() {
  if (tl_called_functions.isNull()) {
    SystemLib::throwInvalidOperationExceptionObject(
      "Function call coverage not enabled"
    );
  }

  auto const ret = std::move(*tl_called_functions.get());
  *s_coverage_index = 0;
  tl_called_functions.destroy();
  return ret;
}

void Func::recordCall() const {
  if (Cfg::Repo::Authoritative || !Cfg::Eval::EnableFuncCoverage) return;
  if (tl_called_functions.isNull()) return;
  if (isNoInjection() || isMethCaller()) return;

  auto const path = unit()->isSystemLib()
    ? String::Empty()
    : StrNR{unit()->filepath()}.asString();

  tl_called_functions->set(fullNameStr().asString(), std::move(path), true);
}

void Func::recordCallNoCheck() const {
  assertx(!Cfg::Repo::Authoritative && Cfg::Eval::EnableFuncCoverage);
  assertx(!tl_called_functions.isNull());
  assertx(tl_called_functions->isDict());
  assertx(!isNoInjection() && !isMethCaller());
  assertx(!tl_called_functions->exists(fullNameStr().asString(), true));

  auto const path = unit()->isSystemLib()
    ? String::Empty()
    : StrNR{unit()->filepath()}.asString();

  tl_called_functions->set(fullNameStr().asString(), std::move(path), true);
}

const TypeIntersectionConstraint& Func::lookupInheritedReturnTypes() const {
  assertx(hasInheritedReturnTypes());
  auto it = s_inheritedRetTypes.find(getFuncId().toStableInt());
  always_assert(it != s_inheritedRetTypes.end());
  return it->second;
}

void Func::storeInheritedReturnTypes(TypeIntersectionConstraint&& tcs) {
  always_assert(s_inheritedRetTypes.insert(getFuncId().toStableInt(), std::move(tcs)).second);
  m_attrs = m_attrs | AttrHasInheritedReturnTypes;
}

bool Func::trustReturnType() const {
  return isCPPBuiltin()
    || isMemoizeWrapper()
    || isMemoizeWrapperLSB()
    || isGenerator();
}

///////////////////////////////////////////////////////////////////////////////
}
