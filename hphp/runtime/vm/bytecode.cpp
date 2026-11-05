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


#include "hphp/runtime/vm/bytecode.h"

#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <cinttypes>
#include <filesystem>

#include <fmt/format.h>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/portability/SysMman.h>

#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/portability.h"
#include "hphp/util/random.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/util/text-util.h"
#include "hphp/util/trace.h"

#include "hphp/system/systemlib.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/array-provenance.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/code-coverage.h"
#include "hphp/runtime/base/configs/configs.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/implicit-context.h"
#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/base/package.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/strings.h"
#include "hphp/runtime/base/tv-arith.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"

#include "hphp/runtime/ext/array/ext_array.h"
#include "hphp/runtime/ext/asio/ext_concurrent-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/asio/ext_waitable-wait-handle.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"
#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/ext/hh/ext_hh.h"
#include "hphp/runtime/ext/hh/ext_implicit_context.h"

#include "hphp/runtime/server/source-root-info.h"

#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/event-hook.h"
#include "hphp/runtime/ext/functioncredential/ext_functioncredential.h"
#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/interp-helpers.h"
#include "hphp/runtime/vm/iter.h"
#include "hphp/runtime/vm/live-func-stats.h"
#include "hphp/runtime/vm/member-operations.h"
#include "hphp/runtime/vm/memo-cache.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/vm/module.h"
#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/reified-generics.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/unwind.h"

#include "hphp/runtime/vm/jit/enter-tc.h"
#include "hphp/runtime/vm/jit/jit-resume-addr-defs.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/jit/service-request-handlers.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/translator-runtime.h"

#include "hphp/zend/zend-string.h"


namespace HPHP {

TRACE_SET_MOD(bcinterp)

using jit::JitResumeAddr;
using jit::TCA;

// GCC 4.8 has some real problems with all the inlining in this file, so don't
// go overboard with that version.
#if !defined(NDEBUG) || ((__GNUC__ == 4) && (__GNUC_MINOR__ == 8))
#define OPTBLD_INLINE
#else
#define OPTBLD_INLINE       ALWAYS_INLINE
#endif

Class* arGetContextClass(const ActRec* ar) {
  if (ar == nullptr) {
    return nullptr;
  }
  return ar->func()->cls();
}

void frame_free_locals_no_hook(ActRec* fp) {
  frame_free_locals_inl_no_hook(fp, fp->func()->numLocals());
}

const StaticString s_file("file");
const StaticString s_line("line");

///////////////////////////////////////////////////////////////////////////////

//=============================================================================
// Miscellaneous decoders.

inline const char* prettytype(int) { return "int"; }
inline const char* prettytype(long) { return "long"; }
inline const char* prettytype(long long) { return "long long"; }
inline const char* prettytype(double) { return "double"; }
inline const char* prettytype(unsigned) { return "unsigned"; }
inline const char* prettytype(OODeclExistsOp) { return "OpDeclExistsOp"; }
inline const char* prettytype(FatalOp) { return "FatalOp"; }
inline const char* prettytype(IsTypeOp) { return "IsTypeOp"; }
inline const char* prettytype(SetOpOp) { return "SetOpOp"; }
inline const char* prettytype(IncDecOp) { return "IncDecOp"; }
inline const char* prettytype(ObjMethodOp) { return "ObjMethodOp"; }
inline const char* prettytype(BareThisOp) { return "BareThisOp"; }
inline const char* prettytype(InitPropOp) { return "InitPropOp"; }
inline const char* prettytype(SwitchKind) { return "SwitchKind"; }
inline const char* prettytype(MOpMode) { return "MOpMode"; }
inline const char* prettytype(QueryMOp) { return "QueryMOp"; }
inline const char* prettytype(SetRangeOp) { return "SetRangeOp"; }
inline const char* prettytype(TypeStructResolveOp) {
  return "TypeStructResolveOp";
}
inline const char* prettytype(VerifyRetKind) {
  return "VerifyRetKind";
}
inline const char* prettytype(TypeStructEnforceKind) {
  return "TypeStructEnforceKind";
}
inline const char* prettytype(AsTypeStructExceptionKind) {
  return "AsTypeStructExceptionKind";
}
inline const char* prettytype(ReadonlyOp) { return "ReadonlyOp"; }
inline const char* prettytype(ContCheckOp) { return "ContCheckOp"; }
inline const char* prettytype(SpecialClsRef) { return "SpecialClsRef"; }
inline const char* prettytype(ClassGetCMode) { return "ClassGetCMode"; }
inline const char* prettytype(CollectionType) { return "CollectionType"; }

// load a T value from *pc without incrementing
template<class T> T peek(PC pc) {
  T v;
  std::memcpy(&v, pc, sizeof v);
  TRACE(2, "decode:     Immediate %s %" PRIi64"\n", prettytype(v), int64_t(v));
  return v;
}

template<class T> T decode(PC& pc) {
  auto v = peek<T>(pc);
  pc += sizeof(T);
  return v;
}

inline const ArrayData* decode_litarr(PC& pc) {
  return liveUnit()->lookupArrayId(decode<Id>(pc));
}

ALWAYS_INLINE tv_lval decode_local(PC& pc) {
  auto la = decode_iva(pc);
  assertx(la < vmfp()->func()->numLocals());
  return frame_local(vmfp(), la);
}

ALWAYS_INLINE local_var decode_indexed_local(PC& pc) {
  auto la = decode_iva(pc);
  assertx(la < vmfp()->func()->numLocals());
  return local_var{frame_local(vmfp(), la), safe_cast<int32_t>(la)};
}

ALWAYS_INLINE named_local_var decode_named_local_var(PC& pc) {
  auto loc = decode_named_local(pc);
  assertx(0 <= loc.id);
  assertx(loc.id < vmfp()->func()->numLocals());
  assertx(kInvalidLocalName <= loc.name);
  assertx(loc.name < vmfp()->func()->numNamedLocals());
  return named_local_var{loc.name, frame_local(vmfp(), loc.id)};
}

ALWAYS_INLINE Iter* decode_iter(PC& pc) {
  auto ia = decode_iva(pc);
  return frame_iter(vmfp(), ia);
}

template<typename T>
OPTBLD_INLINE imm_array<T> decode_imm_array(PC& pc) {
  auto const size = decode_iva(pc);
  auto const arr_pc = pc;
  pc += size * sizeof(T);
  return imm_array<T>{size, arr_pc};
}

OPTBLD_INLINE RepoAuthType decode_rat(PC& pc) {
  if (debug) return decodeRAT(liveUnit(), pc);

  pc += encodedRATSize(pc);
  return RepoAuthType{};
}

//=============================================================================
// Miscellaneous helpers.

static inline Class* frameStaticClass(ActRec* fp) {
  if (!fp->func()->cls()) return nullptr;
  if (fp->hasThis()) {
    return fp->getThis()->getVMClass();
  }
  return fp->getClass();
}

//=============================================================================
// VarEnv.

namespace {
const StaticString
  s_argc("argc"),
  s_argv("argv"),
  s__SERVER("_SERVER"),
  s__GET("_GET"),
  s__POST("_POST"),
  s__FILES("_FILES"),
  s__ENV("_ENV"),
  s_HTTP_RAW_POST_DATA("HTTP_RAW_POST_DATA");
}

void createGlobalNVTable() {
  assertx(!g_context->m_globalNVTable);
  g_context->m_globalNVTable = req::make_raw<NameValueTable>();
  auto nvTable = g_context->m_globalNVTable;
  Variant arr(staticEmptyDictArray());
  nvTable->set(s_argc.get(),               init_null_variant.asTypedValue());
  nvTable->set(s_argv.get(),               init_null_variant.asTypedValue());
  nvTable->set(s__SERVER.get(),            arr.asTypedValue());
  nvTable->set(s__GET.get(),               arr.asTypedValue());
  nvTable->set(s__POST.get(),              arr.asTypedValue());
  nvTable->set(s__FILES.get(),             arr.asTypedValue());
  nvTable->set(s__ENV.get(),               arr.asTypedValue());
  nvTable->set(s_HTTP_RAW_POST_DATA.get(), init_null_variant.asTypedValue());
}

const StaticString s_reified_generics_var("0ReifiedGenerics");
const StaticString s_coeffects_var("0Coeffects");

Array getDefinedVariables() {
  Array ret = Array::CreateDict();

  NameValueTable::Iterator iter(g_context->m_globalNVTable);
  for (; iter.valid(); iter.next()) {
    auto const sd = iter.curKey();
    auto const val = iter.curVal();
    // Reified functions and functions with coeffects rules
    // have an internal variables
    if (s_reified_generics_var.equal(sd) || s_coeffects_var.equal(sd)) {
      continue;
    }
    ret.set(StrNR(sd).asString(), Variant{const_variant_ref{val}});
  }
  {
    // Make result independent of the hashtable implementation.
    ArrayData* sorted = ret->escalateForSort(SORTFUNC_KSORT);
    assertx(sorted == ret.get() ||
            sorted->empty() ||
            sorted->hasExactlyOneRef());
    SCOPE_EXIT {
      if (sorted != ret.get()) {
        ret = Array::attach(sorted);
      }
    };
    sorted->ksort(0, true);
  }
  return ret;
}

//=============================================================================
// Stack.

// Store actual stack elements array in a thread-local in order to amortize the
// cost of allocation.
struct StackElms {
  ~StackElms() { free(m_elms); }
  TypedValue* elms() {
    if (m_elms == nullptr) {
      // Cfg::Eval::VMStackElms-sized and -aligned.
      size_t algnSz = Cfg::Eval::VMStackElms * sizeof(TypedValue);
      if (posix_memalign((void**)&m_elms, algnSz, algnSz) != 0) {
        throw std::runtime_error(
          std::string("VM stack initialization failed: ") +
                      folly::errnoStr(errno).c_str());
      }
      madvise(m_elms, algnSz, MADV_DONTNEED);
    }
    return m_elms;
  }
  void flush() {
    if (m_elms != nullptr) {
      size_t algnSz = Cfg::Eval::VMStackElms * sizeof(TypedValue);
      madvise(m_elms, algnSz, MADV_DONTNEED);
    }
  }
private:
  TypedValue* m_elms{nullptr};
};
THREAD_LOCAL_FLAT(StackElms, t_se);

void flush_evaluation_stack() {
  if (!t_se.isNull()) {
    t_se->flush();
  }
}

const int Stack::sSurprisePageSize = sysconf(_SC_PAGESIZE);
// We reserve the bottom page of each stack for use as the surprise
// page, so the minimum useful stack size is the next power of two.
const uint32_t Stack::sMinStackElms =
  2 * sSurprisePageSize / sizeof(TypedValue);

void Stack::ValidateStackSize() {
  if (Cfg::Eval::VMStackElms < sMinStackElms) {
    throw std::runtime_error(fmt::format(
      "VM stack size of {:#x} is below the minimum of {:#x}",
      Cfg::Eval::VMStackElms,
      sMinStackElms
    ));
  }
  if (!folly::isPowTwo(Cfg::Eval::VMStackElms)) {
    throw std::runtime_error(fmt::format(
      "VM stack size of {:#x} is not a power of 2",
      Cfg::Eval::VMStackElms
    ));
  }
}

Stack::Stack()
  : m_elms(nullptr), m_top(nullptr), m_base(nullptr) {
}

Stack::~Stack() {
  requestExit();
}

void Stack::requestInit() {
  m_elms = t_se->elms();
  // Burn one element of the stack, to satisfy the constraint that
  // valid m_top values always have the same high-order (>
  // log(Cfg::Eval::VMStackElms)) bits.
  m_top = m_base = m_elms + Cfg::Eval::VMStackElms - 1;

  auto const limit = reinterpret_cast<uintptr_t>(
    reinterpret_cast<char*>(m_elms) + sSurprisePageSize +
      stackCheckPadding() * sizeof(TypedValue)
  );
  rds::header()->stackLimitAndSurprise.initialize(limit);

  // Because of the surprise page at the bottom of the stack we lose an
  // additional 256 elements which must be taken into account when checking for
  // overflow.
  UNUSED size_t maxelms =
    Cfg::Eval::VMStackElms - sSurprisePageSize / sizeof(TypedValue);
  assertx(!wouldOverflow(maxelms - 1));
  assertx(wouldOverflow(maxelms));
}

void Stack::requestExit() {
  m_elms = nullptr;
}

static std::string toStringElm(TypedValue tv) {
  std::ostringstream os;

  if (!isRealType(tv.m_type)) {
    os << " ??? type " << static_cast<data_type_t>(tv.m_type) << "\n";
    return os.str();
  }
  if (isRefcountedType(tv.m_type) &&
      !tv.m_data.pcnt->checkCount()) {
    // OK in the invoking frame when running a destructor.
    os << " ??? inner_count " << tvGetCount(tv) << " ";
    return os.str();
  }

  auto print_count = [&] {
    if (tv.m_data.pcnt->isStatic()) {
      os << ":c(static)";
    } else if (tv.m_data.pcnt->isShared()) {
      os << ":c(shared)";
    } else {
      os << ":c(" << tvGetCount(tv) << ")";
    }
  };

  os << "C:";

  do {
    switch (tv.m_type) {
    case KindOfUninit:
      os << "Uninit";
      continue;
    case KindOfNull:
      os << "Null";
      continue;
    case KindOfBoolean:
      os << (tv.m_data.num ? "True" : "False");
      continue;
    case KindOfInt64:
      os << "0x" << std::hex << tv.m_data.num << std::dec;
      continue;
    case KindOfDouble:
      os << tv.m_data.dbl;
      continue;
    case KindOfPersistentString:
    case KindOfString:
      {
        int len = tv.m_data.pstr->size();
        bool truncated = false;
        if (len > 128) {
          len = 128;
          truncated = true;
        }
        os << tv.m_data.pstr;
        print_count();
        os << ":\""
           << escapeStringForCPP(tv.m_data.pstr->data(), len)
           << "\"" << (truncated ? "..." : "");
      }
      continue;
    case KindOfPersistentVec:
    case KindOfVec:
      assertx(tv.m_data.parr->isVecType());
      assertx(tv.m_data.parr->checkCount());
      os << tv.m_data.parr;
      print_count();
      os << ":Vec";
      continue;
    case KindOfPersistentDict:
    case KindOfDict:
      assertx(tv.m_data.parr->isDictType());
      assertx(tv.m_data.parr->checkCount());
      os << tv.m_data.parr;
      print_count();
      os << ":Dict";
      continue;
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      assertx(tv.m_data.parr->isKeysetType());
      assertx(tv.m_data.parr->checkCount());
      os << tv.m_data.parr;
      print_count();
      os << ":Keyset";
      continue;
    case KindOfObject:
      assertx(tv.m_data.pobj->checkCount());
      os << tv.m_data.pobj;
      print_count();
      os << ":Object("
         << tv.m_data.pobj->getClassName().get()->data()
         << ")";
      continue;
    case KindOfResource:
      assertx(tv.m_data.pres->checkCount());
      os << tv.m_data.pres;
      print_count();
      os << ":Resource("
         << tv.m_data.pres->data()->o_getClassName().get()->data()
         << ")";
      continue;
    case KindOfEnumClassLabel:
      os << tv.m_data.pstr;
      print_count();
      os << ":EnumClassLabel("
         << tv.m_data.pstr->data()
         << ")";
      continue;
    case KindOfRFunc: // TODO(T63348446) serialize the reified generics
      assertx(tv.m_data.prfunc->checkCount());
      os << tv.m_data.prfunc;
      print_count();
      os << ":RFunc("
         << tv.m_data.prfunc->m_func->fullName()->data()
         << ")<"
         << tv.m_data.prfunc->m_arr
         << ">";
      continue;
    case KindOfFunc:
      os << ":Func("
         << tv.m_data.pfunc->fullName()->data()
         << ")";
      continue;
    case KindOfClass:
      os << ":Class("
         << tv.m_data.pclass->name()->data()
         << ")";
      continue;
    case KindOfLazyClass:
      os << ":LClass("
         << tv.m_data.plazyclass.name()->data()
         << ")";
      continue;
    case KindOfClsMeth:
      os << ":ClsMeth("
       << tv.m_data.pclsmeth->getCls()->name()->data()
       << ", "
       << tv.m_data.pclsmeth->getFunc()->fullName()->data()
       << ")";
       continue;
    case KindOfRClsMeth:
      os << ":RClsMeth("
         << tv.m_data.prclsmeth->m_cls->name()->data()
         << ", "
         << tv.m_data.prclsmeth->m_func->fullName()->data()
         << ")<"
         << tv.m_data.prclsmeth->m_arr
         << ">";
      continue;
    }
    not_reached();
  } while (0);

  return os.str();
}

/*
 * Return true if Offset o is inside the protected region of a fault
 * funclet for iterId, otherwise false.
 */
static bool checkIterScope(const Func* f, Offset o, Id iterId) {
  assertx(o >= 0 && o < f->bclen());
  for (auto const& eh : f->ehtab()) {
    if (eh.m_base <= o && o < eh.m_past &&
        eh.m_iterId == iterId) {
      return true;
    }
  }
  return false;
}

static void toStringFrame(std::ostream& os, const ActRec* fp,
                          int offset, const TypedValue* ftop,
                          const std::string& prefix, bool isTop = true) {
  assertx(fp);

  // Use depth-first recursion to output the most deeply nested stack frame
  // first.
  {
    Offset prevPc = 0;
    TypedValue* prevStackTop = nullptr;
    ActRec* prevFp = g_context->getPrevVMState(fp, &prevPc, &prevStackTop);
    if (prevFp != nullptr) {
      toStringFrame(os, prevFp, prevPc, prevStackTop, prefix, false);
    }
  }

  os << prefix;
  const Func* func = fp->func();
  assertx(func);
  func->validate();
  std::string funcName(func->fullName()->data());
  os << "{func:" << funcName
     << ",callOff:" << fp->callOffset()
     << ",this:0x"
     << std::hex << (func->cls() && fp->hasThis() ? fp->getThis() : nullptr)
     << std::dec << "}";

  if (func->numLocals() > 0) {
    // Don't print locals for parent frames on a Ret(C|V) since some of them
    // may already be destructed.
    if (isRet(func->getOp(offset)) && !isTop) {
      os << "<locals destroyed>";
    } else {
      os << "<";
      int n = func->numLocals();
      for (int i = 0; i < n; i++) {
        if (i > 0) {
          os << " ";
        }
        os << toStringElm(*frame_local(fp, i));
      }
      os << ">";
    }
  }

  if (func->numIterators() > 0) {
    os << "|";
    for (int i = 0; i < func->numIterators(); i++) {
      if (i > 0) {
        os << " ";
      }
      if (checkIterScope(func, offset, i)) {
        os << "I";
      } else {
        os << "I:Undefined";
      }
    }
    os << "|";
  }

  std::vector<std::string> stackElems;
  visitStackElems(
    fp, ftop,
    [&](const TypedValue* tv) {
      stackElems.push_back(toStringElm(*tv));
    }
  );
  std::reverse(stackElems.begin(), stackElems.end());
  os << ' ' << folly::join(' ', stackElems);

  os << '\n';
}

std::string Stack::toString(const ActRec* fp, int offset,
                       const std::string prefix/* = "" */) const {
  // The only way to figure out which stack elements are activation records is
  // to follow the frame chain. However, the goal for each stack frame is to
  // print stack fragments from deepest to shallowest -- a then b in the
  // following example:
  //
  //   {func:foo,callOff:51}<C:8> {func:bar} C:8 C:1 {func:biz} C:0
  //                              aaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbb
  //
  // Use depth-first recursion to get the output order correct.

  std::ostringstream os;
  auto unit = fp->unit();
  auto func = fp->func();
  os << prefix << "=== Stack at "
     << unit->filepath()->data() << ":"
     << func->getLineNumber(func->offsetOf(vmpc()))
     << " func " << func->fullName()->data() << " ===\n";

  toStringFrame(os, fp, offset, m_top, prefix);

  return os.str();
}

bool Stack::wouldOverflow(int numCells) const {
  // The funny approach here is to validate the translator's assembly
  // technique. We've aligned and sized the stack so that the high order
  // bits of valid cells are all the same. In the translator, numCells
  // can be hardcoded, and m_top is wired into a register,
  // so the expression requires no loads.
  intptr_t truncatedTop = intptr_t(m_top) / sizeof(TypedValue);
  truncatedTop &= Cfg::Eval::VMStackElms - 1;
  intptr_t diff = truncatedTop - numCells -
    sSurprisePageSize / sizeof(TypedValue);
  return diff < 0;
}

TypedValue* Stack::anyFrameStackBase(const ActRec* fp) {
  return isResumed(fp) ? Stack::resumableStackBase(fp)
                       : Stack::frameStackBase(fp);
}

TypedValue* Stack::frameStackBase(const ActRec* fp) {
  assertx(!isResumed(fp));
  return (TypedValue*)fp - fp->func()->numSlotsInFrame();
}

TypedValue* Stack::resumableStackBase(const ActRec* fp) {
  assertx(isResumed(fp));
  auto sfp = fp->sfp();
  if (sfp) {
    // The non-reentrant case occurs when a non-async or async generator is
    // resumed via ContEnter or ContRaise opcode. These opcodes leave a single
    // value on the stack that becomes part of the generator's stack. So we
    // find the caller's FP, compensate for its locals and iterators, and then
    // we've found the base of the generator's stack.
    assertx(fp->func()->isGenerator());
    assertx(!sfp->func()->isGenerator());
    return (TypedValue*)sfp - sfp->func()->numSlotsInFrame();
  } else {
    // The reentrant case occurs when asio scheduler resumes an async function
    // or async generator. We simply use the top of stack of the previous VM
    // frame (since the ActRec, locals, and iters for this frame do not reside
    // on the VM stack).
    assertx(fp->func()->isAsync());
    TypedValue* prevSp;
    DEBUG_ONLY auto const prevFp =
      g_context.getNoCheck()->getPrevVMState(fp, nullptr, &prevSp);
    assertx(prevFp != nullptr);
    return prevSp;
  }
}

Array getDefinedVariables(const ActRec* fp) {
  if (UNLIKELY(fp == nullptr || fp->isInlined())) return empty_dict_array();
  auto const func = fp->func();
  auto const numLocals = func->numNamedLocals();
  DictInit ret(numLocals);
  for (Id id = 0; id < numLocals; ++id) {
    auto const local = frame_local(fp, id);
    if (type(local) == KindOfUninit) {
      continue;
    }
    auto const localNameSd = func->localVarName(id);
    if (!localNameSd) continue;
    // this is basically just a convoluted const_cast :p
    Variant name(localNameSd, Variant::PersistentStrInit{});
    ret.set(name.getStringData(), Variant{variant_ref{local}});
  }
  return ret.toArray();
}

// Unpack or repack positionals as needed to match the function signature.
// The stack contains numPositionalArgs positionals plus an extra cell
// containing arguments to unpack.
uint32_t prepareUnpackArgs(const Func* func, uint32_t numPositionalArgs,
                           bool checkInOutAnnot) {
  auto& stack = vmStack();
  auto unpackArgs = *stack.topC();
  if (!isContainer(unpackArgs)) throwInvalidUnpackArgs();
  stack.discard();
  SCOPE_EXIT { tvDecRefGen(unpackArgs); };

  auto const numUnpackArgs = getContainerSize(unpackArgs);
  auto const numPositionalParams = func->numPositionalParams();
  if (LIKELY(numPositionalArgs == numPositionalParams)) {
    // Convert unpack args to the proper type.
    tvCastToVecInPlace(&unpackArgs);
    stack.pushVec(unpackArgs.m_data.parr);
    return numPositionalParams + 1;
  }

  ArrayIter iter(unpackArgs);
  if (LIKELY(numPositionalArgs < numPositionalParams)) {
    for (auto i = numPositionalArgs; iter && (i < numPositionalParams); ++i, ++iter) {
      if (UNLIKELY(checkInOutAnnot && func->isInOut(i))) {
        throwParamInOutMismatch(func, i);
      }
      auto const from = iter.secondValPlus();
      tvDup(from, *stack.allocTV());
    }

    if (LIKELY(!iter)) {
      // argArray was exhausted, so there are no "extra" arguments but there
      // may be a deficit of non-variadic arguments, and the need to push an
      // empty array for the variadic argument ... that work is left to
      // initFuncInputs.
      assertx(numPositionalArgs + numUnpackArgs <= numPositionalParams);
      return numPositionalArgs + numUnpackArgs;
    }
  }

  // there are "extra" arguments; passed as standard arguments prior to the
  // ... unpack operator and/or still remaining in argArray
  assertx(numPositionalArgs + numUnpackArgs > numPositionalParams);
  assertx(numPositionalArgs > numPositionalParams || !!iter);

  auto const numNewUnpackArgs = numPositionalArgs + numUnpackArgs - numPositionalParams;
  VecInit ai(numNewUnpackArgs);
  if (UNLIKELY(numPositionalArgs > numPositionalParams)) {
    // The arguments are pushed in order, so we should start from the bottom.
    auto ptr = stack.indTV(numPositionalArgs - numPositionalParams);
    for (auto i = numPositionalParams; i < numPositionalArgs; ++i) {
      ai.append(*--ptr);
    }
    for (auto i = numPositionalParams; i < numPositionalArgs; ++i) {
      stack.popTV();
    }
  }
  for (; iter; ++iter) {
    ai.append(iter.secondValPlus());
  }
  auto const ad = ai.create();
  assertx(ad->hasExactlyOneRef());
  assertx(ad->size() == numNewUnpackArgs);
  stack.pushArrayLikeNoRc(ad);
  return numPositionalParams + 1;
}

static void prepareFuncEntry(ActRec *ar, uint32_t posArgcInclUnpack,
                             bool allNamedParamsPassed) {
  assertx(!isResumed(ar));
  assertx(
    reinterpret_cast<TypedValue*>(ar) - vmStack().top() ==
      ar->func()->numParams()
      + (ar->func()->hasReifiedGenerics() ? 1U : 0U)
      + (ar->func()->hasCoeffectsLocal() ? 1U : 0U)
  );

  const Func* func = ar->func();

  vmStack().top() = reinterpret_cast<TypedValue*>(ar) - func->numSlotsInFrame();
  vmfp() = ar;
  vmpc() = func->entry() + func->getFuncEntryForNumPositionals(posArgcInclUnpack,
                                                               allNamedParamsPassed);
  vmJitReturnAddr() = nullptr;
}

void enterVMAtFunc(ActRec* enterFnAr, uint32_t posArgcInclUnpack,
                   bool allNamedParamsPassed) {
  assertx(enterFnAr);
  assertx(!isResumed(enterFnAr));
  Stats::inc(Stats::VMEnter);

  prepareFuncEntry(enterFnAr, posArgcInclUnpack, allNamedParamsPassed);
  assertx(vmfp()->func()->contains(vmpc()));
  auto const& stubs = jit::tc::ustubs();
  JitResumeAddr resumeAddr;
  // We use `>=` instead of `==` as the caller may supply unpack args, in which case
  // posArgcInclUnpack would be enterFnAr->func()->numPositionalParams() + 1,
  // and calling into the main func entry is safe.
  if (!RID().getJit()) {
    resumeAddr = JitResumeAddr::helper(stubs.resumeHelperNoTranslateFuncEntryFromInterp);
  } else if (posArgcInclUnpack >= enterFnAr->func()->numPositionalParams() &&
             allNamedParamsPassed) {
    resumeAddr = JitResumeAddr::transFuncEntry(enterFnAr->func()->getFuncEntry());
  } else {
    resumeAddr = JitResumeAddr::helper(stubs.resumeHelperFuncEntryFromInterp);
  }
  jit::enterTC(resumeAddr);
}

void enterVMAtCurPC() {
  assertx(vmfp());
  assertx(vmpc());
  assertx(vmfp()->func()->contains(vmpc()));
  Stats::inc(Stats::VMEnter);
  auto const resumeHelper = RID().getJit()
    ? jit::tc::ustubs().resumeHelperFromInterp
    : jit::tc::ustubs().resumeHelperNoTranslateFromInterp;
  jit::enterTC(JitResumeAddr::helper(resumeHelper));
}

static inline StringData* lookup_name(tv_rval key) {
  return prepareKey(*key);
}

static inline tv_lval lookup_gbl(ActRec* /*fp*/, StringData*& name,
                                 tv_rval key) {
  name = lookup_name(key);
  assertx(g_context->m_globalNVTable);
  return g_context->m_globalNVTable->lookup(name);
}

static inline tv_lval lookupd_gbl(ActRec* /*fp*/, StringData*& name,
                                  tv_rval key) {
  name = lookup_name(key);
  assertx(g_context->m_globalNVTable);
  auto env = g_context->m_globalNVTable;
  auto val = env->lookup(name);
  if (!val) {
    TypedValue tv;
    tvWriteNull(tv);
    env->set(name, &tv);
    val = env->lookup(name);
  }
  return val;
}
namespace {
void checkStaticPropertyInternal(
  const Class* cls,
  const StringData* name,
  const MemberLookupContext& ctx
) {
    auto const slot = cls->lookupSProp(name);
    auto const prop = cls->staticProperties()[slot];
    if (will_symbol_raise_module_boundary_violation(&prop, &ctx)) {
      raiseModulePropertyViolation(cls, name, ctx.moduleName(), true);
    }
}
}
static inline void lookup_sprop(ActRec* fp,
                                Class* cls,
                                StringData*& name,
                                TypedValue* key,
                                TypedValue*& val,
                                Slot& slot,
                                bool& visible,
                                bool& accessible,
                                bool& constant,
                                bool& readonly,
                                bool ignoreLateInit) {
  name = lookup_name(key);
  auto const ctx = MemberLookupContext(arGetContextClass(fp), fp->func());

  auto const lookup = ignoreLateInit
    ? cls->getSPropIgnoreLateInit(ctx, name)
    : cls->getSProp(ctx, name);

  if (lookup.internal) {
    checkStaticPropertyInternal(cls, name, ctx);
  }

  val = lookup.val;
  slot = lookup.slot;
  visible = lookup.val != nullptr;
  constant = lookup.constant;
  readonly = lookup.readonly;
  accessible = lookup.accessible;
}

namespace {

void checkModuleBoundaryViolation(const Class* ctx, const Func& callee) {
  auto const caller = vmfp()->func();
  if (will_symbol_raise_module_boundary_violation(&callee, caller)) {
    raiseModuleBoundaryViolation(ctx, &callee, caller->moduleName());
  }
}

void checkModuleBoundaryViolation(const Class& cls) {
  auto const caller = vmfp()->func();
  if (will_symbol_raise_module_boundary_violation(&cls, caller)) {
    raiseModuleBoundaryViolation(&cls, caller->moduleName(), caller->cls());
  }
}

} // namespace

struct ClassnameToClassChecks {
  bool dynamicallyReferenced{false};
  bool inStrictPackage{false};
};

static inline Class* classnameToClass(
    TypedValue* input,
    ClassnameToClassChecks checks) {
  auto const explicitFail = [&](const char* k, const char* n) {
    std::string msg;
    string_printf(msg, Strings::CLASSNAME_TO_CLASS_NOEXIST_EXCEPTION, k, n);
    SystemLib::throwInvalidArgumentExceptionObject(msg);
  };

  if (tvIsString(input)) {
    auto const name = input->m_data.pstr;
    if (Class* class_ = Class::resolve(name, vmfp()->func())) {
      if (checks.dynamicallyReferenced &&
          !class_->isDynamicallyReferenced()) {
        raiseMissingDynamicallyReferenced(class_);
      }
      if (checks.inStrictPackage) checkStrictPackageDynamicReference(class_);
      return class_;
    }
    explicitFail("string", name->data());
  }
  if (tvIsLazyClass(input)) {
    auto const name = input->m_data.plazyclass.name();
    // We skip module checks for lazy-classes
    if (Class* class_ = Class::load(name)) {
      return class_;
    }
    explicitFail("lazy class", name->data());
  }
  if (tvIsClass(input)) return input->m_data.pclass;

  SystemLib::throwInvalidArgumentExceptionObject(
    fmt::format(
      "Invalid argument type {} passed to classname_to_class", input->m_type)
  );
}

static inline Class* lookupClsRef(TypedValue* input, jit::StrToClassKind kind) {
  if (tvIsString(input)) {
    auto const name = input->m_data.pstr;
    if (Class* class_ = Class::resolve(name, vmfp()->func())) {
      raise_str_to_class_notice(name, kind);
      checkStrictPackageDynamicReference(class_);
      return class_;
    }
    raise_error(Strings::UNKNOWN_CLASS, name->data());
  }
  if (tvIsLazyClass(input)) {
    // We skip module checks for lazy-classes
    if (Class* class_ = Class::load(input->m_data.plazyclass.name())) {
      return class_;
    }
    raise_error(Strings::UNKNOWN_CLASS, input->m_data.plazyclass.name()->data());
  }
  if (tvIsObject(input)) return input->m_data.pobj->getVMClass();
  if (tvIsClass(input)) return input->m_data.pclass;
  raise_error("Cls: Expected string or object, got %s",
              describe_actual_type(input).c_str());
}

static UNUSED int innerCount(TypedValue tv) {
  return isRefcountedType(tv.m_type) ? tvGetCount(tv) : -1;
}

/*
 * One iop* function exists for every bytecode. They all take a single PC&
 * argument, which should be left pointing to the next bytecode to execute when
 * the instruction is complete. Most return void, though a few return a
 * jit::JitResumeAddr. The ones that return a JitReturnAddr return a true value
 * to indicate that the caller must resume execution in the TC at the returned
 * address. This is used to maintain certain invariants about how we get into
 * and out of VM frames in jitted code; see comments on jitReturnPre() for more
 * details.
 */

OPTBLD_INLINE void iopNop() {
}

OPTBLD_INLINE void iopPopC() {
  vmStack().popC();
}

OPTBLD_INLINE void iopPopU() {
  vmStack().popU();
}

OPTBLD_INLINE void iopPopU2() {
  assertx(vmStack().indC(1)->m_type == KindOfUninit);
  *vmStack().indC(1) = *vmStack().topC();
  vmStack().discard();
}

OPTBLD_INLINE void iopPopL(tv_lval to) {
  TypedValue* fr = vmStack().topC();
  tvMove(*fr, to);
  vmStack().discard();
}

OPTBLD_INLINE void iopDup() {
  vmStack().dup();
}

OPTBLD_INLINE void iopCGetCUNop() {
}

OPTBLD_INLINE void iopNull() {
  vmStack().pushNull();
}

OPTBLD_INLINE void iopNullUninit() {
  vmStack().pushNullUninit();
}

OPTBLD_INLINE void iopTrue() {
  vmStack().pushBool(true);
}

OPTBLD_INLINE void iopFalse() {
  vmStack().pushBool(false);
}

OPTBLD_INLINE void iopFile() {
  if (auto const ou = vmfp()->func()->originalUnit()) {
    vmStack().pushStaticString(ou);
    return;
  }
  auto s = vmfp()->func()->unit()->filepath();
  vmStack().pushStaticString(s);
}

OPTBLD_INLINE void iopDir() {
  auto const p = [&] {
    if (auto const ou = vmfp()->func()->originalUnit()) {
      return ou;
    }
    return vmfp()->func()->unit()->filepath();
  }();
  vmStack().pushStaticString(
    makeStaticString(FileUtil::dirname(StrNR{p}))
  );
}

OPTBLD_INLINE void iopMethod() {
  auto s = vmfp()->func()->fullName();
  vmStack().pushStaticString(s);
}

OPTBLD_INLINE void iopFuncCred() {
  if (UNLIKELY(vmfp()->func()->isNoInjection())) {
    const Func* func = nullptr;
    walkStack([&] (const BTFrame& f) {
      if (f.func()->isNoInjection()) return false;
      func = f.func();
      return true;
    });
    if (func) {
      vmStack().pushObjectNoRc(
        FunctionCredential::newInstance(func));
      return;
    }
  }
  vmStack().pushObjectNoRc(
    FunctionCredential::newInstance(vmfp()->func()));
}

OPTBLD_INLINE void iopClassName() {
  auto const cls  = vmStack().topC();
  if (!isClassType(cls->m_type)) {
    raise_error("Attempting to get name of non-class");
  }
  vmStack().replaceC<KindOfPersistentString>(
    cls->m_data.pclass->name()
  );
}

OPTBLD_INLINE void iopEnumClassLabelName() {
  auto const label  = vmStack().topC();
  if (!tvIsEnumClassLabel(label)) {
    raise_error("Attempting to get name of non enum class label");
  }
  vmStack().replaceC<KindOfPersistentString>(label->m_data.pstr);
}

OPTBLD_INLINE void iopInt(int64_t imm) {
  vmStack().pushInt(imm);
}

OPTBLD_INLINE void iopDouble(double imm) {
  vmStack().pushDouble(imm);
}

OPTBLD_INLINE void iopString(const StringData* s) {
  vmStack().pushStaticString(s);
}

namespace {

void profileArrLikePropsForInterp(ObjectData* obj) {
  if (g_context->doingInlineInterp()) return;
  bespoke::profileArrLikeProps(obj);
}

ArrayData* maybeMakeBespokeArray(ArrayData* ad) {
  return bespoke::makeArrayOfSelectedLayout(ad);
}

const ArrayData* maybeMakeBespokeArray(const ArrayData* ad) {
  return maybeMakeBespokeArray(const_cast<ArrayData*>(ad));
}

void maybeMakeBespokeArrayAfterCast(TypedValue* tv) {
  auto const oldArr = val(tv).parr;
  auto const newArr = maybeMakeBespokeArray(oldArr);
  if (newArr == oldArr) return;
  val(tv).parr = newArr;
  type(tv) = dt_with_rc(type(tv));
}

}

OPTBLD_INLINE void iopVec(const ArrayData* a) {
  assertx(a->isVecType());
  vmStack().pushStaticVec(maybeMakeBespokeArray(a));
}

OPTBLD_INLINE void iopDict(const ArrayData* a) {
  assertx(a->isDictType());
  vmStack().pushStaticDict(maybeMakeBespokeArray(a));
}

OPTBLD_INLINE void iopKeyset(const ArrayData* a) {
  assertx(a->isKeysetType());
  vmStack().pushStaticKeyset(maybeMakeBespokeArray(a));
}

OPTBLD_INLINE void iopNewDictArray(uint32_t capacity) {
  auto const ad = capacity ? VanillaDict::MakeReserveDict(capacity)
                           : staticEmptyDictArray();
  vmStack().pushDictNoRc(maybeMakeBespokeArray(ad));
}

namespace {

template <typename F>
ArrayData* newStructArrayImpl(imm_array<int32_t> ids, F f) {
  auto const n = ids.size;
  assertx(n > 0 && n <= ArrayData::MaxElemsOnStack);
  req::vector<const StringData*> names;
  names.reserve(n);
  auto unit = vmfp()->func()->unit();
  for (size_t i = 0; i < n; ++i) {
    auto name = unit->lookupLitstrId(ids[i]);
    names.push_back(name);
  }

  // This constructor moves values, no inc/decref is necessary.
  auto const a = f(n, names.data(), vmStack().topC())->asArrayData();
  vmStack().ndiscard(n);
  return a;
}

}

OPTBLD_INLINE void iopNewStructDict(imm_array<int32_t> ids) {
  auto const ad = newStructArrayImpl(ids, VanillaDict::MakeStructDict);
  vmStack().pushDictNoRc(maybeMakeBespokeArray(ad));
}

OPTBLD_INLINE void iopNewVec(uint32_t n) {
  // This constructor moves values, no inc/decref is necessary.
  auto const ad = VanillaVec::MakeVec(n, vmStack().topC());
  vmStack().ndiscard(n);
  vmStack().pushVecNoRc(maybeMakeBespokeArray(ad));
}

OPTBLD_INLINE void iopNewKeysetArray(uint32_t n) {
  // This constructor moves values, no inc/decref is necessary.
  auto const ad = VanillaKeyset::MakeSet(n, vmStack().topC());
  vmStack().ndiscard(n);
  vmStack().pushKeysetNoRc(maybeMakeBespokeArray(ad));
}

OPTBLD_INLINE void iopAddElemC() {
  TypedValue* c1 = vmStack().topC();
  auto key = tvClassToString(*vmStack().indC(1));
  TypedValue* c3 = vmStack().indC(2);
  if (!tvIsDict(c3)) {
    raise_error("AddElemC: $3 must be an array or dict");
  }
  tvAsVariant(*c3).asArrRef().set(tvAsCVarRef(key), tvAsCVarRef(c1));
  assertx(tvIsPlausible(*c3));
  vmStack().popC();
  vmStack().popC();
}

OPTBLD_INLINE void iopAddNewElemC() {
  TypedValue* c1 = vmStack().topC();
  TypedValue* c2 = vmStack().indC(1);
  if (!tvIsVec(c2) && !tvIsKeyset(c2)) {
    raise_error("AddNewElemC: $2 must be an varray, vec, or keyset");
  }
  tvAsVariant(*c2).asArrRef().append(tvAsCVarRef(c1));
  assertx(tvIsPlausible(*c2));
  vmStack().popC();
}

OPTBLD_INLINE void iopNewCol(CollectionType cType) {
  assertx(cType != CollectionType::Pair);
  // Incref the collection object during construction.
  auto obj = collections::alloc(cType);
  vmStack().pushObjectNoRc(obj);
}

OPTBLD_INLINE void iopNewPair() {
  TypedValue* c1 = vmStack().topC();
  TypedValue* c2 = vmStack().indC(1);
  // elements were pushed onto the stack in the order they should appear
  // in the pair, so the top of the stack should become the second element
  auto pair = collections::allocPair(*c2, *c1);
  // This constructor moves values, no inc/decref is necessary.
  vmStack().ndiscard(2);
  vmStack().pushObjectNoRc(pair);
}

OPTBLD_INLINE void iopColFromArray(CollectionType cType) {
  assertx(cType != CollectionType::Pair);
  auto const c1 = vmStack().topC();
  if (cType == CollectionType::Vector || cType == CollectionType::ImmVector) {
     if (UNLIKELY(!isVecType(c1->m_type))) {
       raise_error("ColFromArray: $1 must be a Vec when creating an "
                   "(Imm)Vector");
     }
   } else if (UNLIKELY(!isDictType(c1->m_type))) {
       raise_error("ColFromArray: $1 must be a Dict when creating an (Imm)Set "
                   "or an (Imm)Map");
   }
  // This constructor reassociates the ArrayData with the collection, so no
  // inc/decref is needed for the array. The collection object itself is
  // increfed.
  auto obj = collections::alloc(cType, c1->m_data.parr);
  vmStack().discard();
  vmStack().pushObjectNoRc(obj);
}

OPTBLD_INLINE void iopCnsE(const StringData* s) {
  auto const cns = Constant::load(s);
  if (type(cns) == KindOfUninit) {
    raise_error("Undefined constant '%s'", s->data());
  }
  auto const c1 = vmStack().allocC();
  tvCopy(cns, *c1);
}

OPTBLD_INLINE void iopClsCns(const StringData* clsCnsName) {
  auto const clsTV = vmStack().topC();
  if (!isClassType(clsTV->m_type)) {
    raise_error("Attempting class constant access on non-class");
  }

  auto const cls = clsTV->m_data.pclass;
  auto const clsCns = cls->clsCnsGet(clsCnsName);

  if (clsCns.m_type == KindOfUninit) {
    raise_error("Couldn't find constant %s::%s",
                cls->name()->data(), clsCnsName->data());
  }

  tvDup(clsCns, *clsTV);
}

OPTBLD_INLINE void iopClsCnsD(const StringData* clsCnsName, Id classId) {
  const NamedTypePair& classNamedType =
    vmfp()->func()->unit()->lookupNamedTypePairId(classId);
  auto const clsCns = g_context->lookupClsCns(classNamedType.second,
                                       classNamedType.first, clsCnsName);
  auto const c1 = vmStack().allocC();
  tvDup(clsCns, *c1);
}

OPTBLD_INLINE void iopClsCnsL(tv_lval local) {
  auto const clsTV = vmStack().topC();
  if (!isClassType(clsTV->m_type)) {
    raise_error("Attempting class constant access on non-class");
  }
  auto const cls = clsTV->m_data.pclass;
  if (!isStringType(type(local))) {
    raise_error("String expected for %s constant", cls->name()->data());
  }
  auto const clsCnsName = val(local).pstr;
  auto const clsCns = cls->clsCnsGet(clsCnsName);
  if (clsCns.m_type == KindOfUninit) {
    raise_error("Couldn't find constant %s::%s",
                cls->name()->data(), clsCnsName->data());
  }
  tvSet(clsCns, *clsTV);
}

OptString toStringWithNotice(const Variant& c) {
  return c.toString(ConvThrowMode::Throw);
}

OPTBLD_INLINE void iopConcat() {
  auto const c1 = vmStack().topC();
  auto const c2 = vmStack().indC(1);
  auto const s2 = toStringWithNotice(tvAsVariant(*c2));
  auto const s1 = toStringWithNotice(tvAsCVarRef(*c1));
  tvAsVariant(*c2) = concat(s2, s1);
  assertx(c2->m_data.pstr->checkCount());
  vmStack().popC();
}

OPTBLD_INLINE void iopConcatN(uint32_t n) {
  if (n == 2) return iopConcat();
  auto const c1 = vmStack().topC();
  auto const c2 = vmStack().indC(1);
  auto const c3 = vmStack().indC(2);

  if (n == 3) {
    auto const s3 = toStringWithNotice(tvAsVariant(*c3));
    auto const s2 = toStringWithNotice(tvAsCVarRef(*c2));
    auto const s1 = toStringWithNotice(tvAsCVarRef(*c1));
    tvAsVariant(*c3) = concat3(s3, s2, s1);
    assertx(c3->m_data.pstr->checkCount());
  } else {
    assertx(n == 4);
    auto const c4 = vmStack().indC(3);
    auto const s4 = toStringWithNotice(tvAsVariant(*c4));
    auto const s3 = toStringWithNotice(tvAsCVarRef(*c3));
    auto const s2 = toStringWithNotice(tvAsCVarRef(*c2));
    auto const s1 = toStringWithNotice(tvAsCVarRef(*c1));
    tvAsVariant(*c4) = concat4(s4, s3, s2, s1);
    assertx(c4->m_data.pstr->checkCount());
  }

  for (int i = 1; i < n; ++i) {
    vmStack().popC();
  }
}

OPTBLD_INLINE void iopNot() {
  TypedValue* c1 = vmStack().topC();
  tvAsVariant(*c1) = !tvAsVariant(*c1).toBoolean();
}

template<class Fn>
OPTBLD_INLINE void implTvBinOp(Fn fn) {
  auto const c1 = vmStack().topC();
  auto const c2 = vmStack().indC(1);
  auto const result = fn(*c2, *c1);
  tvDecRefGen(c2);
  *c2 = result;
  vmStack().popC();
}

template<class Fn>
OPTBLD_INLINE void implTvBinOpBool(Fn fn) {
  auto const c1 = vmStack().topC();
  auto const c2 = vmStack().indC(1);
  bool const result = fn(*c2, *c1);
  tvDecRefGen(c2);
  *c2 = make_tv<KindOfBoolean>(result);
  vmStack().popC();
}

template<class Fn>
OPTBLD_INLINE void implTvBinOpInt64(Fn fn) {
  auto const c1 = vmStack().topC();
  auto const c2 = vmStack().indC(1);
  auto const result = fn(*c2, *c1);
  tvDecRefGen(c2);
  *c2 = make_tv<KindOfInt64>(result);
  vmStack().popC();
}

OPTBLD_INLINE void iopAdd() {
  implTvBinOp(tvAdd);
}

OPTBLD_INLINE void iopSub() {
  implTvBinOp(tvSub);
}

OPTBLD_INLINE void iopMul() {
  implTvBinOp(tvMul);
}

OPTBLD_INLINE void iopDiv() {
  implTvBinOp(tvDiv);
}

OPTBLD_INLINE void iopPow() {
  implTvBinOp(tvPow);
}

OPTBLD_INLINE void iopMod() {
  implTvBinOp(tvMod);
}

OPTBLD_INLINE void iopBitAnd() {
  implTvBinOp(tvBitAnd);
}

OPTBLD_INLINE void iopBitOr() {
  implTvBinOp(tvBitOr);
}

OPTBLD_INLINE void iopBitXor() {
  implTvBinOp(tvBitXor);
}

OPTBLD_INLINE void iopSame() {
  implTvBinOpBool(tvSame);
}

OPTBLD_INLINE void iopNSame() {
  implTvBinOpBool([&] (TypedValue c1, TypedValue c2) {
    return !tvSame(c1, c2);
  });
}

OPTBLD_INLINE void iopEq() {
  implTvBinOpBool([&] (TypedValue c1, TypedValue c2) {
    return tvEqual(c1, c2);
  });
}

OPTBLD_INLINE void iopNeq() {
  implTvBinOpBool([&] (TypedValue c1, TypedValue c2) {
    return !tvEqual(c1, c2);
  });
}

OPTBLD_INLINE void iopLt() {
  implTvBinOpBool([&] (TypedValue c1, TypedValue c2) {
    return tvLess(c1, c2);
  });
}

OPTBLD_INLINE void iopLte() {
  implTvBinOpBool(tvLessOrEqual);
}

OPTBLD_INLINE void iopGt() {
  implTvBinOpBool([&] (TypedValue c1, TypedValue c2) {
    return tvGreater(c1, c2);
  });
}

OPTBLD_INLINE void iopGte() {
  implTvBinOpBool(tvGreaterOrEqual);
}

OPTBLD_INLINE void iopCmp() {
  implTvBinOpInt64([&] (TypedValue c1, TypedValue c2) {
    return tvCompare(c1, c2);
  });
}

OPTBLD_INLINE void iopShl() {
  implTvBinOp(tvShl);
}

OPTBLD_INLINE void iopShr() {
  implTvBinOp(tvShr);
}

OPTBLD_INLINE void iopBitNot() {
  tvBitNot(*vmStack().topC());
}

OPTBLD_INLINE void iopCastBool() {
  TypedValue* c1 = vmStack().topC();
  tvCastToBooleanInPlace(c1);
}

OPTBLD_INLINE void iopCastInt() {
  TypedValue* c1 = vmStack().topC();
  tvCastToInt64InPlace(c1);
}

OPTBLD_INLINE void iopCastDouble() {
  TypedValue* c1 = vmStack().topC();
  tvCastToDoubleInPlace(c1);
}

OPTBLD_INLINE void iopCastString() {
  TypedValue* c1 = vmStack().topC();
  tvCastToStringInPlace(c1);
}

OPTBLD_INLINE void iopCastVec() {
  TypedValue* c1 = vmStack().topC();
  if (tvIsVec(c1)) return;
  tvCastToVecInPlace(c1);
  maybeMakeBespokeArrayAfterCast(c1);
}

OPTBLD_INLINE void iopCastDict() {
  TypedValue* c1 = vmStack().topC();
  if (tvIsDict(c1)) return;
  tvCastToDictInPlace(c1);
  maybeMakeBespokeArrayAfterCast(c1);
}

OPTBLD_INLINE void iopCastKeyset() {
  TypedValue* c1 = vmStack().topC();
  if (tvIsKeyset(c1)) return;
  tvCastToKeysetInPlace(c1);
  maybeMakeBespokeArrayAfterCast(c1);
}

OPTBLD_INLINE void iopDblAsBits() {
  auto c = vmStack().topC();
  if (UNLIKELY(!isDoubleType(c->m_type))) {
    vmStack().replaceC<KindOfInt64>(0);
    return;
  }
  c->m_type = KindOfInt64;
}

ALWAYS_INLINE
bool implInstanceOfHelper(const StringData* str1, TypedValue* c2) {
  const NamedType* rhs = NamedType::getNoCreate(str1);
  // Because of other codepaths, an un-normalized name might enter the
  // table without a Class* so we need to check if it's there.
  if (LIKELY(rhs && rhs->getCachedClass() != nullptr)) {
    return tvInstanceOf(c2, rhs);
  }
  return false;
}

OPTBLD_INLINE void iopInstanceOf() {
  TypedValue* c1 = vmStack().topC();   // c2 instanceof c1
  TypedValue* c2 = vmStack().indC(1);
  bool r = false;
  if (isStringType(c1->m_type)) {
    r = implInstanceOfHelper(c1->m_data.pstr, c2);
  } else if (c1->m_type == KindOfObject) {
    if (c2->m_type == KindOfObject) {
      ObjectData* lhs = c2->m_data.pobj;
      ObjectData* rhs = c1->m_data.pobj;
      r = lhs->instanceof(rhs->getVMClass());
    }
  } else if (isClassType(c1->m_type)) {
    // TODO (T29639296) Exploit class pointer further
    r = implInstanceOfHelper(c1->m_data.pclass->name(), c2);
  } else {
    raise_error("Class name must be a valid object or a string");
  }
  vmStack().popC();
  vmStack().replaceC<KindOfBoolean>(r);
}

OPTBLD_INLINE void iopInstanceOfD(Id id) {
  const NamedType* ne = vmfp()->func()->unit()->lookupNamedTypeId(id);
  TypedValue* c1 = vmStack().topC();
  bool r = tvInstanceOf(c1, ne);
  vmStack().replaceC<KindOfBoolean>(r);
}

OPTBLD_INLINE void iopIsLateBoundCls() {
  auto const cls = frameStaticClass(vmfp());
  if (!cls) {
    raise_error(HPHP::Strings::THIS_OUTSIDE_CLASS);
  }
  if (isTrait(cls)) {
    raise_error("\"is\" and \"as\" operators cannot be used with a trait");
  }
  auto const c1 = vmStack().topC();
  bool r = tvInstanceOf(c1, cls);
  vmStack().replaceC<KindOfBoolean>(r);
}

namespace {

ArrayData* resolveAndVerifyTypeStructureHelper(
  uint32_t n, const TypedValue* values, bool suppress, bool isOrAsOp) {
  Class* declaringCls = nullptr;
  Class* calledCls = nullptr;
  auto const v = *values;
  isValidTSType(v, true);
  if (typeStructureCouldBeNonStatic(v.m_data.parr)) {
    auto const frame = vmfp();
    if (frame && frame->func()) {
      declaringCls = frame->func()->cls();
      if (declaringCls) {
        calledCls = frame->hasClass()
          ? frame->getClass()
          : frame->getThis()->getVMClass();
      }
    }
  }
  return jit::resolveTypeStructHelper(n, values, declaringCls,
                                      calledCls, suppress, isOrAsOp);
}

ALWAYS_INLINE Array maybeResolveAndErrorOnTypeStructure(
  TypeStructResolveOp op,
  bool suppress,
  bool isOrAsOp
) {
  auto const a = vmStack().topC();
  isValidTSType(*a, true);
  auto const arr = a->m_data.parr;

  if (op == TypeStructResolveOp::Resolve) {
    auto const result = resolveAndVerifyTypeStructureHelper(1, vmStack().topC(),
                                                            suppress, isOrAsOp);
    return Array::attach(result);

  }
  if (isOrAsOp) {
    errorOnIsAsExpressionInvalidTypes(ArrNR(arr), false);
  }
  return ArrNR(arr);
}

inline void checkThis(ActRec* fp) {
  if (!fp->func()->cls() || !fp->hasThis()) {
    raise_error(Strings::FATAL_NULL_THIS);
  }
}

} // namespace

OPTBLD_INLINE void iopIsTypeStructC(TypeStructResolveOp op, TypeStructEnforceKind kind) {
  auto const c = vmStack().indC(1);
  auto const ts = maybeResolveAndErrorOnTypeStructure(op, true, true);
  bool b;
  switch (kind) {
  case TypeStructEnforceKind::Deep:
    b = checkTypeStructureMatchesTV(ts, *c);
    break;
  case TypeStructEnforceKind::Shallow:
    // The checkTypeStructureMatchesTV overload which accepts a warn flag runs
    // checkTypeStructureMatchesTVImpl with isOrAsOp=false, giving the shallow
    // checking behavior used for param verification and which we want here.
    bool warn = false;
    b = checkTypeStructureMatchesTV(ts, *c, warn);
    break;
  }
  vmStack().popC(); // pop c
  vmStack().replaceC<KindOfBoolean>(b);
}

OPTBLD_INLINE void iopThrowAsTypeStructException(AsTypeStructExceptionKind kind) {
  auto const c = vmStack().indC(1);
  auto const ts =
    maybeResolveAndErrorOnTypeStructure(TypeStructResolveOp::Resolve, false, true);
  std::string givenType, expectedType, errorKey;
  if (!checkTypeStructureMatchesTV(ts, *c,
                                   givenType, expectedType, errorKey)) {
    vmStack().popC(); // pop c
    bool raiseError = false;
    switch(kind) {
      case AsTypeStructExceptionKind::Error:
        raiseError = true;
        break;
      case AsTypeStructExceptionKind::Typehint:
        raiseError = false;
        break;
    }
    throwTypeStructureDoesNotMatchTVException(
      givenType, expectedType, errorKey, raiseError);
  }
  always_assert(false && "Invalid bytecode sequence: Instruction must throw");
}

OPTBLD_INLINE void iopCombineAndResolveTypeStruct(uint32_t n) {
  assertx(n != 0);
  auto const resolved =
    resolveAndVerifyTypeStructureHelper(n, vmStack().topC(), false, false);
  vmStack().popC(); // pop the first TS
  vmStack().ndiscard(n-1);
  vmStack().pushArrayLikeNoRc(resolved);
}

OPTBLD_INLINE void iopRecordReifiedGeneric() {
  auto const tsList = vmStack().topC();
  if (!tvIsVec(tsList)) {
    raise_error("Invalid type-structure list in RecordReifiedGeneric");
  }
  // recordReifiedGenericsAndGetTSList decrefs the tsList
  auto const result =
    jit::recordReifiedGenericsAndGetTSList(tsList->m_data.parr);
  vmStack().discard();
  vmStack().pushStaticArrayLike(result);
}

OPTBLD_INLINE void iopCheckClsReifiedGenericMismatch() {
  Class* cls = arGetContextClass(vmfp());
  if (!cls) raise_error("No class scope is active");
  auto const c = vmStack().topC();
  if (!tvIsVec(c)) {
    raise_error("Invalid type-structure list in CheckClsReifiedGenericMismatch");
  }
  checkClassReifiedGenericMismatch(cls, c->m_data.parr);
  vmStack().popC();
}

OPTBLD_INLINE void iopClassHasReifiedGenerics() {
  auto const class_ = [&] {
    auto const cls = vmStack().topC();
    if (LIKELY(tvIsClass(cls))) return cls->m_data.pclass;
    if (tvIsLazyClass(cls)) return Class::load(cls->m_data.plazyclass.name());
    raise_error("ClassHasReifiedGenerics called on non-class object");
  }();
  vmStack().replaceC<KindOfBoolean>(class_->hasReifiedGenerics());
}

OPTBLD_INLINE void iopGetClsRGProp() {
  checkThis(vmfp());
  ObjectData* this_ = vmfp()->getThis();
  auto const cls = [&] {
    auto const cls = vmStack().topC();
    if (LIKELY(tvIsClass(cls))) return cls->m_data.pclass;
    if (tvIsLazyClass(cls)) return Class::load(cls->m_data.plazyclass.name());
    raise_error("GetClsRGProp called on non-class object");
  }();
  vmStack().replaceC<KindOfVec>(getClsReifiedGenericsProp(cls, this_));
}

OPTBLD_INLINE void iopHasReifiedParent() {
  auto const class_ = [&] {
    auto const cls = vmStack().topC();
    if (tvIsClass(cls)) return cls->m_data.pclass;
    if (tvIsLazyClass(cls)) return Class::load(cls->m_data.plazyclass.name());
    raise_error("HasReifiedParent called on non-class object");
  }();
  vmStack().replaceC<KindOfBoolean>(class_->hasReifiedParent());
}

OPTBLD_INLINE void iopCheckClsRGSoft() {
  auto const class_ = [&] {
    auto const cls = vmStack().topC();
    if (LIKELY(tvIsClass(cls))) return cls->m_data.pclass;
    if (tvIsLazyClass(cls)) return Class::load(cls->m_data.plazyclass.name());
    raise_error("CheckClsRGSoft called on non-class object");
  }();

  assertx(class_->hasReifiedGenerics());
  checkClassReifiedGenericsSoft(class_);
  vmStack().popC();
}

OPTBLD_INLINE void iopReifiedInit(tv_lval val) {
  auto const class_ = [&] {
    auto const cls = vmStack().topC();
    if (LIKELY(tvIsClass(cls))) return cls->m_data.pclass;
    if (tvIsLazyClass(cls)) return Class::load(cls->m_data.plazyclass.name());
    raise_error("ReifiedInit called on non-class object");
  }();
  auto genericsTv = val.tv();
  assertx(tvIsVec(genericsTv));
  auto generics = genericsTv.m_data.parr;
  auto objTv = vmStack().indC(1);
  assertx(tvIsObject(objTv));
  auto obj = objTv->m_data.pobj;
  tryClassReifiedInit(class_, generics, obj);
  vmStack().popC();
  vmStack().popC();
}

OPTBLD_INLINE void iopPrint() {
  TypedValue* c1 = vmStack().topC();
  g_context->write(tvAsVariant(*c1).toString());
  vmStack().replaceC<KindOfInt64>(1);
}

OPTBLD_INLINE void iopClone() {
  TypedValue* tv = vmStack().topTV();
  if (tv->m_type != KindOfObject) {
    raise_error("clone called on non-object");
  }
  auto newobj = tv->m_data.pobj->clone();
  vmStack().popTV();
  vmStack().pushObjectNoRc(newobj);
}

OPTBLD_INLINE void iopExit() {
  int exitCode = 0;
  TypedValue* c1 = vmStack().topC();
  if (c1->m_type == KindOfInt64) {
    exitCode = c1->m_data.num;
  } else {
    g_context->write(tvAsVariant(*c1).toString());
  }
  vmStack().popC();
  vmStack().pushNull();
  throw ExitException(exitCode);
}

OPTBLD_INLINE void iopFatal(FatalOp kind_char) {
  TypedValue* top = vmStack().topTV();
  std::string msg;
  if (isStringType(top->m_type)) {
    msg = top->m_data.pstr->data();
  } else {
    msg = "Fatal error message not a string";
  }
  vmStack().popTV();

  switch (kind_char) {
  case FatalOp::RuntimeOmitFrame:
    raise_error_without_first_frame(msg);
  case FatalOp::Runtime:
  case FatalOp::Parse:
    raise_error(msg);
  }
}

OPTBLD_INLINE void iopStaticAnalysisError() {
  always_assert(!Cfg::Eval::CrashOnStaticAnalysisError);

  jit::raiseStaticAnalysisError();
}

OPTBLD_INLINE void jmpSurpriseCheck(Offset offset) {
  if (offset <= 0 && UNLIKELY(stackLimitAndSurprise().hasSurprise())) {
    auto const flags = handle_request_surprise();

    // Memory Threhsold callback should also be fired here
    if (flags & MemThresholdFlag) {
      EventHook::DoMemoryThresholdCallback();
    }
    if (flags & TimedOutFlag) {
      RID().invokeUserTimeoutCallback();
    }
    if (flags & DebuggerSignalFlag) {
      markFunctionWithDebuggerIntr(vmfp()->func());
    }
  }
}

OPTBLD_INLINE void iopJmp(PC& pc, PC targetpc) {
  jmpSurpriseCheck(targetpc - pc);
  pc = targetpc;
}

template<Op op>
OPTBLD_INLINE void jmpOpImpl(PC& pc, PC targetpc) {
  static_assert(op == OpJmpZ || op == OpJmpNZ,
                "jmpOpImpl should only be used by JmpZ and JmpNZ");
  jmpSurpriseCheck(targetpc - pc);

  TypedValue* c1 = vmStack().topC();
  if (c1->m_type == KindOfInt64 || c1->m_type == KindOfBoolean) {
    int64_t n = c1->m_data.num;
    vmStack().popX();
    if (op == OpJmpZ ? n == 0 : n != 0) pc = targetpc;
  } else {
    auto const cond = tvAsCVarRef(*c1).toBoolean();
    vmStack().popC();
    if (op == OpJmpZ ? !cond : cond) pc = targetpc;
  }
}

OPTBLD_INLINE void iopJmpZ(PC& pc, PC targetpc) {
  jmpOpImpl<OpJmpZ>(pc, targetpc);
}

OPTBLD_INLINE void iopJmpNZ(PC& pc, PC targetpc) {
  jmpOpImpl<OpJmpNZ>(pc, targetpc);
}

OPTBLD_INLINE void iopSelect() {
  auto const cond = [&]{
    auto c = vmStack().topC();
    if (c->m_type == KindOfInt64 || c->m_type == KindOfBoolean) {
      auto const val = (bool)c->m_data.num;
      vmStack().popX();
      return val;
    } else {
      auto const val = tvAsCVarRef(*c).toBoolean();
      vmStack().popC();
      return val;
    }
  }();

  if (cond) {
    auto const t = *vmStack().topC();
    vmStack().discard();
    vmStack().replaceC(t);
  } else {
    vmStack().popC();
  }
}

OPTBLD_INLINE
void iopSwitch(PC origpc, PC& pc, SwitchKind kind, int64_t base,
               imm_array<Offset> jmptab) {
  auto const veclen = jmptab.size;
  assertx(veclen > 0);
  TypedValue* val = vmStack().topTV();
  if (kind == SwitchKind::Unbounded) {
    assertx(val->m_type == KindOfInt64);
    // Continuation switch: no bounds checking needed
    int64_t label = val->m_data.num;
    vmStack().popX();
    assertx(label >= 0 && label < veclen);
    pc = origpc + jmptab[label];
    return;
  }

  const auto num = val->m_data.num;
  const auto offset =
    !tvIsInt(val) || num < base || num >= (base + veclen - 2)
      ? veclen - 1
      : num - base;

  pc = origpc + jmptab[offset];
  vmStack().discard();
}

OPTBLD_INLINE
void iopSSwitch(PC origpc, PC& pc, imm_array<StrVecItem> jmptab) {
  auto const veclen = jmptab.size;
  assertx(veclen > 1);
  TypedValue* val = vmStack().topTV();

  if (tvIsString(val) || tvIsClass(val) || tvIsLazyClass(val)) {
    unsigned cases = veclen - 1; // the last vector item is the default case
    Unit* u = vmfp()->func()->unit();
    for (unsigned i = 0; i < cases; ++i) {
      auto item = jmptab[i];
      const StringData* str = u->lookupLitstrId(item.str);
      if (tvEqual(*val, str)) {
        pc = origpc + item.dest;
        vmStack().popC();
        return;
      }
    }
  }

  // default case
  pc = origpc + jmptab[veclen - 1].dest;
  vmStack().popC();
}

/*
 * jitReturnPre and jitReturnPost are used by RetC/V, CreateCont, NativeImpl,
 * Yield, and YieldK to perform a few tasks related to interpreting out of a
 * frame:
 *
 * - If the current frame was entered in the TC and the jit is now off, we
 *   throw a VMSwitchMode at the beginning of the bytecode to execute the
 *   call's catch block (if present) before performing the return.
 * - If the current frame was entered in the TC and the jit is still on,
 *   we wait until the end of the bytecode and throw a VMResumeTC, to return to
 *   our translated caller rather than interpreting back into it.
 * - If the current frame was entered by the interpreter but was active when
 *   the jit called MCGenerator::handleResume() (meaning it's the saved value
 *   of %rbp in handleResume()'s stack frame), throw a VMResumeTC to reenter
 *   handleResume(). This is necessary to update the value of %rbp in the TC
 *   frame, so the unwinder doesn't read from a dead VM frame if something
 *   throws from the interpreter later on.
 */
namespace {
struct JitReturn {
  uint64_t savedRip;
  ActRec* fp;
  ActRec* sfp;
  Offset callOff;
};

OPTBLD_INLINE JitReturn jitReturnPre(ActRec* fp) {
  assertx(fp->m_savedRip);
  return {fp->m_savedRip, fp, fp->sfp(), fp->callOffset()};
}

OPTBLD_INLINE JitResumeAddr jitReturnPost(JitReturn retInfo) {
  assertx(isCallToExit(retInfo.savedRip) == (retInfo.sfp == nullptr));
  assertx(isCallToExit(retInfo.savedRip) == (vmfp() == nullptr));

  if (!isReturnHelper(retInfo.savedRip)) {
    // This frame is either the first frame in this VM nesting level, or it was
    // called by a translated code so we can't interpret out of it. Either way,
    // use the saved rip, which will either use the callToExit helper to exit
    // the TC, or resume in the TC right after the call to us. This situation
    // most commonly happens when we interpOne a RetC due to some weird case.
    assertx(isCallToExit(retInfo.savedRip) || RID().getJit());
    return JitResumeAddr::ret(TCA(retInfo.savedRip));
  }

  // Consider a situation with a Hack function f() that calls another function
  // g(). If the call is interpreted, then we spend some time in the TC inside
  // g(), then eventually end in dispatchBB() (called by
  // MCGenerator::handleResume()) for g()'s RetC, the logic here kicks in.
  //
  // g()'s VM frame was in %rbp when the TC called handleResume(), so it's
  // saved somewhere in handleResume()'s stack frame. If we return out of that
  // frame and keep going in the interpreter, that saved %rbp will be pointing
  // to a garbage VM frame. This is a problem if something needs to throw an
  // exception up through handleResume() and the TC frames above it, since the
  // C++ unwinder will attempt to treat parts of the popped VM frame as
  // pointers and segfault.
  //
  // To avoid running with this dangling saved %rbp a few frames up, we
  // immediately throw an exception that is "caught" by the TC frame that
  // called handleResume(). We resume execution in the TC which reloads the new
  // vmfp() into %rbp, then handleResume() is called again, this time with a
  // live VM frame in %rbp.
  if (vmJitCalledFrame() == retInfo.fp) {
    FTRACE(1, "Returning from frame {}; resuming", vmJitCalledFrame());
    return JitResumeAddr::helper(jit::tc::ustubs().resumeHelperFromInterp);
  }

  // This frame was called from the interpreter, so it's ok to also return
  // using the interpreter.
  return JitResumeAddr::none();
}

OPTBLD_INLINE void returnToCaller(PC& pc, ActRec* sfp, Offset callOff,
                                  bool asyncEagerReturn = false) {
  vmfp() = sfp;
  pc = LIKELY(sfp != nullptr)
    ? asyncEagerReturn
      ? asyncEagerReturnTarget(sfp->func(), callOff)
      : skipCall(sfp->func()->entry() + callOff)
    : nullptr;
}

}

template <bool suspended>
OPTBLD_INLINE JitResumeAddr ret(PC& pc) {
  assertx(!suspended || vmfp()->func()->isAsyncFunction());
  assertx(!suspended || !isResumed(vmfp()));

  // Grab info from callee's ActRec.
  auto const fp = vmfp();
  auto const func = fp->func();
  assertx(!suspended || !func->isEagerOnly());
  auto const sfp = fp->sfp();
  auto const jitReturn = jitReturnPre(fp);
  auto const asyncEagerReturn =
    !suspended && func->isAsyncFunction() && fp->isAsyncEagerReturn();
  assertx(IMPLIES(asyncEagerReturn, sfp));

  // Get the return value.
  TypedValue retval = *vmStack().topTV();
  vmStack().discard();

  assertx(
    !suspended || (tvIsObject(retval) && retval.m_data.pobj->isWaitHandle())
  );

  // Free $this and local variables. Calls FunctionReturn hook. The return
  // value must be removed from the stack, or the unwinder would try to free it
  // if the hook throws---but the event hook routine decrefs the return value
  // in that case if necessary.
  fp->setLocalsDecRefd();
  frame_free_locals_inl(
    fp,
    func->numLocals(),
    &retval,
    EventHook::Source::Interpreter
  );

  if (LIKELY(!isResumed(fp))) {
    // If in an eagerly executed async function, wrap the return value into
    // succeeded StaticWaitHandle, unless the caller requested an async eager
    // return.
    if (UNLIKELY(func->isAsyncFunction()) &&
        !suspended &&
        !asyncEagerReturn) {
      auto const& retvalCell = *tvAssertPlausible(&retval);
      // Heads up that we're assuming CreateSucceeded can't throw, or we won't
      // decref the return value.  (It can't right now.)
      auto const waitHandle = c_StaticWaitHandle::CreateSucceeded(retvalCell);
      tvCopy(make_tv<KindOfObject>(waitHandle), retval);
    }

    // Free ActRec and store the return value.
    vmStack().ndiscard(func->numSlotsInFrame());
    vmStack().ret();
    *vmStack().topTV() = retval;
    assertx(vmStack().topTV() == fp->retSlot());
    vmStack().topTV()->m_aux.u_asyncEagerReturnFlag =
      asyncEagerReturn ? static_cast<uint32_t>(-1) : 0;
  } else if (func->isAsyncFunction()) {
    // Mark the async function as succeeded and store the return value.
    assertx(!sfp);
    auto wh = frame_afwh(fp);
    wh->ret(retval);
    decRefObj(wh);
  } else if (func->isAsyncGenerator()) {
    // Mark the async generator as finished.
    assertx(isNullType(retval.m_type));
    auto const gen = frame_async_generator(fp);
    auto const eagerResult = gen->ret();
    if (eagerResult) {
      // Eager execution => return StaticWaitHandle.
      assertx(sfp);
      vmStack().pushObjectNoRc(eagerResult);
    } else {
      // Resumed execution => return control to the scheduler.
      assertx(!sfp);
    }
  } else if (func->isNonAsyncGenerator()) {
    // Mark the generator as finished and store the return value.
    assertx(isNullType(retval.m_type));
    frame_generator(fp)->ret();

    // Push return value of next()/send()/raise().
    vmStack().pushNull();
  } else {
    not_reached();
  }

  // Return control to the caller.
  returnToCaller(
    pc, sfp, jitReturn.callOff,
    asyncEagerReturn && isReturnHelper(jitReturn.savedRip)
  );

  return jitReturnPost(jitReturn);
}

namespace {

OPTBLD_INLINE void verifyRetTypeImpl(size_t ind, HPHP::VerifyRetKind kind) {
  auto const func = vmfp()->func();

  if (func->trustReturnType()) {
    assertx(kind == VerifyRetKind::None);
    return;
  }

  if (kind == VerifyRetKind::None && !func->hasInheritedReturnTypes()) return;

  auto const& constraints = func->returnTypeConstraints();
  auto const& retVal = vmStack().indC(ind);

  auto verifyTc = [&](auto const& tc, bool checkNonNull) {
    if (!tc.isCheckable()) return;
    if (checkNonNull && tc.isNullable()) return;
    auto const ctx = tc.isThis() ? frameStaticClass(vmfp()) : nullptr;
    return checkNonNull
      ? tc.verifyReturnNonNull(retVal, ctx, func)
      : tc.verifyReturn(retVal, ctx, func);
  };

  for (auto const& tc : constraints.range()) {
    if (!tc.isInherited() && kind == VerifyRetKind::None) continue;
    verifyTc(tc, !tc.isInherited() && kind == VerifyRetKind::NonNull);
  }
}

} // namespace

OPTBLD_INLINE JitResumeAddr iopRetC(PC& pc, HPHP::VerifyRetKind kind) {
  verifyRetTypeImpl(0, kind);
  return ret<false>(pc);
}

OPTBLD_INLINE JitResumeAddr iopRetCSuspended(PC& pc) {
  assertx(vmfp()->func()->isAsyncFunction());
  assertx(!isResumed(vmfp()));
  return ret<true>(pc);
}

OPTBLD_INLINE JitResumeAddr iopRetM(PC& pc, uint32_t numRet, HPHP::VerifyRetKind kind) {
  verifyRetTypeImpl(numRet - 1, kind);
  auto const jitReturn = jitReturnPre(vmfp());

  req::vector<TypedValue> retvals;
  retvals.reserve(numRet);

  for (int i = numRet - 1; i >= 0; i--) {
    retvals.push_back(*vmStack().indC(i));
  }

  vmStack().ndiscard(numRet);

  // Free $this and local variables. Calls FunctionReturn hook. The return
  // value must be removed from the stack, or the unwinder would try to free it
  // if the hook throws---but the event hook routine decrefs the return value
  // in that case if necessary.
  frame_free_locals_inl(
    vmfp(),
    vmfp()->func()->numLocals(),
    &retvals[0],
    EventHook::Source::Interpreter
  );

  assertx(!vmfp()->func()->isGenerator() && !vmfp()->func()->isAsync());

  // Grab caller info from ActRec.
  ActRec* sfp = vmfp()->sfp();
  Offset callOff = vmfp()->callOffset();

  // Free ActRec and store the return value.
  vmStack().ndiscard(vmfp()->func()->numSlotsInFrame());
  vmStack().ret();

  // Discard scratch space for return values allocated for multi return FCall
  vmStack().ndiscard(numRet - 1);
  *vmStack().topTV() = retvals[1];

  for (int i = 2; i < numRet; i++) {
    *vmStack().allocTV() = retvals[i];
  }

  // Store the actual return value at the top of the stack
  *vmStack().allocTV() = retvals[0];

  // Return control to the caller.
  returnToCaller(pc, sfp, callOff);

  return jitReturnPost(jitReturn);
}

OPTBLD_INLINE JitResumeAddr iopEnter(PC& pc, PC targetpc) {
  auto const jitReturn = jitReturnPre(vmfp());
  auto const intercepted = !EventHook::FunctionCall(
    vmfp(), EventHook::NormalFunc, EventHook::Source::Interpreter);
  if (intercepted) {
    // The callee was intercepted, return to the caller.
    pc = vmpc();
    return jitReturnPost(jitReturn);
  }

  pc = targetpc;
  return JitResumeAddr::none();
}

OPTBLD_INLINE void iopThrow(PC&) {
  TypedValue* c1 = vmStack().topC();
  if (c1->m_type != KindOfObject ||
      !c1->m_data.pobj->instanceof(SystemLib::getThrowableClass())) {
    raise_error(
      "Exceptions must implement the Throwable interface, got %s.",
      describe_actual_type(c1).c_str()
    );
  }
  auto obj = Object::attach(c1->m_data.pobj);
  vmStack().discard();
  DEBUGGER_ATTACHED_ONLY(phpDebuggerExceptionThrownHook(obj.get()));
  if (UNLIKELY(Cfg::Debugger::EnableVSDebugger &&
               !RID().getVSDebugDisablesJit() &&
               !g_context->m_dbgNoBreak &&
               RID().getDebuggerStepIntr())) {
    walkStack([&] (const BTFrame& f) {
      auto const func = f.func();
      if (!func->isBuiltin() &&
          findExceptionHandler(func, f.bcOff()) != kInvalidOffset) {
        markFunctionWithDebuggerIntr(func);
      }
    });
  }
  throw req::root<Object>(std::move(obj));
}

OPTBLD_INLINE void iopThrowNonExhaustiveSwitch() {
  SystemLib::throwRuntimeExceptionObject(OptString(Strings::NONEXHAUSTIVE_SWITCH));
}

OPTBLD_INLINE void iopRaiseClassStringConversionNotice() {
  if (folly::Random::oneIn(Cfg::Eval::RaiseClassConversionNoticeSampleRate, threadLocalRng64())) {
    raise_class_to_string_conversion_notice("bytecode");
  }
}

OPTBLD_INLINE void iopResolveClass(Id id) {
  auto const cname = vmfp()->unit()->lookupLitstrId(id);
  auto const class_ = Class::resolve(cname, vmfp()->func());
  if (class_) vmStack().pushClass(class_);
  else raise_resolve_class_undefined(cname);
}

OPTBLD_INLINE void iopLazyClass(Id id) {
  auto const cname = vmfp()->unit()->lookupLitstrId(id);
  auto const lclass = LazyClassData::create(cname);
  vmStack().pushLazyClass(lclass);
}

OPTBLD_INLINE void iopEnumClassLabel(Id id) {
  auto const label = vmfp()->unit()->lookupLitstrId(id);
  vmStack().pushEnumClassLabel(label);
}

OPTBLD_INLINE void iopClassGetC(ClassGetCMode mode) {
  auto const cell = vmStack().topC();
  auto const cls = [&] {
    switch (mode) {
      case ClassGetCMode::Normal:
        return lookupClsRef(cell, jit::StrToClassKind::Expression);
      case ClassGetCMode::ExplicitConversion:
        return classnameToClass(cell, {
          .dynamicallyReferenced = true,
          .inStrictPackage = true,
        });
      case ClassGetCMode::UnsafeBackdoor:
        assertx(!Cfg::Repo::Authoritative);
        return classnameToClass(cell, {});
      case ClassGetCMode::StrictIsolationBackdoor:
        return classnameToClass(cell, {
          .dynamicallyReferenced = true,
        });
    }
  }();
  vmStack().popC();
  vmStack().pushClass(cls);
}

OPTBLD_INLINE void classGetTSImpl(bool pushGenerics) {
  auto const cell = vmStack().topC();
  if (!tvIsDict(cell)) {
    raise_error("Reified type must be a type structure");
  }
  auto const ts = cell->m_data.parr;
  auto const classname_field = ts->get(s_classname.get());
  if (!classname_field.is_init()) {
    raise_error("You cannot create a new instance of this type as "
                "it is not a class");
  }
  assertx(isStringType(classname_field.type()));
  auto const name = classname_field.val().pstr;

  ArrayData* reified_types = nullptr;
  if (pushGenerics) {
    auto const generics_field = ts->get(s_generic_types.get());
    if (generics_field.is_init()) {
      reified_types = generics_field.val().parr;
      auto const mangledTypeName =
        makeStaticString(mangleReifiedGenericsName(reified_types));
      reified_types->incRefCount();
      reified_types = addToTypeReifiedGenericsTable(mangledTypeName, reified_types);
    }
  }

  auto const cls = Class::load(name);
  if (cls == nullptr) {
    raise_error(Strings::UNKNOWN_CLASS, name->data());
  }

  vmStack().popC();
  vmStack().pushClass(cls);
  if (pushGenerics) {
    if (reified_types) {
      vmStack().pushStaticArrayLike(reified_types);
    } else {
      vmStack().pushStaticArrayLike(staticEmptyVec());
    }
  }
}

OPTBLD_INLINE void iopClassGetTS() {
  classGetTSImpl(false);
}

OPTBLD_INLINE void iopClassGetTSWithGenerics() {
  classGetTSImpl(true);
}

static void raise_undefined_local(ActRec* fp, LocalName pind) {
  assertx(pind < fp->func()->numNamedLocals());
  assertx(fp->func()->localVarName(pind));
  if (debug) {
    auto vm = &*g_context;
    always_assert_flog(
      pind != kInvalidLocalName,
      "HHBBC incorrectly removed name info for a local in {}:{}",
      vm->getContainingFileName()->data(),
      vm->getLine()
    );
  }
  SystemLib::throwUndefinedVariableExceptionObject(
    fmt::format("Undefined variable: {}",
                   fp->func()->localVarName(pind)->data()));
}

static inline void cgetl_inner_body(tv_rval fr, TypedValue* to) {
  assertx(type(fr) != KindOfUninit);
  tvDup(*fr, *to);
}

OPTBLD_INLINE void cgetl_body(ActRec* fp,
                              tv_rval fr,
                              TypedValue* to,
                              LocalName lname,
                              bool warn) {
  if (type(fr) == KindOfUninit) {
    // `to' is uninitialized here, so we need to tvWriteNull before
    // possibly causing stack unwinding.
    tvWriteNull(*to);
    if (warn) raise_undefined_local(fp, lname);
  } else {
    cgetl_inner_body(fr, to);
  }
}

OPTBLD_INLINE void iopCGetL(named_local_var fr) {
  TypedValue* to = vmStack().allocC();
  cgetl_body(vmfp(), fr.lval, to, fr.name, true);
}

OPTBLD_INLINE void iopCGetQuietL(tv_lval fr) {
  TypedValue* to = vmStack().allocC();
  cgetl_body(vmfp(), fr, to, kInvalidLocalName, false);
}

OPTBLD_INLINE void iopCUGetL(tv_lval fr) {
  auto to = vmStack().allocTV();
  tvDup(*fr, *to);
}

OPTBLD_INLINE void iopCGetL2(named_local_var fr) {
  TypedValue* oldTop = vmStack().topTV();
  TypedValue* newTop = vmStack().allocTV();
  memcpy(newTop, oldTop, sizeof *newTop);
  TypedValue* to = oldTop;
  cgetl_body(vmfp(), fr.lval, to, fr.name, true);
}

OPTBLD_INLINE void iopPushL(tv_lval locVal) {
  assertx(type(locVal) != KindOfUninit);
  TypedValue* dest = vmStack().allocTV();
  *dest = *locVal;
  type(locVal) = KindOfUninit;
}

OPTBLD_INLINE void iopCGetG() {
  StringData* name;
  TypedValue* to = vmStack().topTV();
  auto const fr = lookup_gbl(vmfp(), name, to);
  SCOPE_EXIT { decRefStr(name); };
  tvDecRefGen(to);
  if (!fr || type(fr) == KindOfUninit) {
    tvWriteNull(*to);
  } else {
    cgetl_inner_body(fr, to);
  }
}

struct SpropState {
  SpropState(Stack&, bool ignoreLateInit);
  ~SpropState();
  StringData* name;
  Class* cls;
  TypedValue* output;
  TypedValue* val;
  TypedValue oldNameCell;
  Slot slot;
  bool visible;
  bool accessible;
  bool constant;
  bool readonly;
  bool internal;
  Stack& vmstack;
};

SpropState::SpropState(Stack& vmstack, bool ignoreLateInit) : vmstack{vmstack} {
  auto const clsCell = vmstack.topC();
  auto const nameCell = output = vmstack.indTV(1);
  if (!isClassType(clsCell->m_type)) {
    raise_error("SpropState: expected class");
  }
  cls = clsCell->m_data.pclass;
  lookup_sprop(vmfp(), cls, name, nameCell, val,
               slot, visible, accessible, constant, readonly, ignoreLateInit);
  oldNameCell = *nameCell;
}

SpropState::~SpropState() {
  vmstack.discard();
  decRefStr(name);
  tvDecRefGen(oldNameCell);
}

OPTBLD_INLINE void iopCGetS(ReadonlyOp op) {
  SpropState ss(vmStack(), false);
  if (!(ss.visible && ss.accessible)) {
    raise_error("Invalid static property access: %s::%s",
                ss.cls->name()->data(),
                ss.name->data());
  }
  if (ss.readonly && op == ReadonlyOp::Mutable) {
    throw_must_be_enclosed_in_readonly(
      ss.cls->name()->data(), ss.name->data()
    );
  }
  tvDup(*ss.val, *ss.output);
}

OPTBLD_INLINE void iopBaseSC(uint32_t keyIdx,
                             uint32_t clsIdx,
                             MOpMode mode,
                             ReadonlyOp op) {
  auto& mstate = vmMInstrState();
  auto const clsCell = vmStack().indC(clsIdx);
  auto const key = vmStack().indTV(keyIdx);

  if (!isClassType(clsCell->m_type)) {
    raise_error("Attempting to obtain static base on non-class");
  }
  auto const class_ = clsCell->m_data.pclass;

  auto const name = lookup_name(key);
  SCOPE_EXIT { decRefStr(name); };
  auto const ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func()->moduleName());
  auto const lookup = class_->getSProp(ctx, name);
  if (!lookup.val || !lookup.accessible) {
    raise_error("Invalid static property access: %s::%s",
                class_->name()->data(),
                name->data());
  }
  if (lookup.internal) {
    checkStaticPropertyInternal(class_, name, ctx);
  }
  assertx(mode != MOpMode::InOut);
  auto const writeMode = mode == MOpMode::Define || mode == MOpMode::Unset;

  if (lookup.constant && writeMode) {
    throw_cannot_modify_static_const_prop(class_->name()->data(),
      name->data());
  }

  mstate.roProp = false;
  checkReadonly(lookup.val, class_, name, lookup.readonly, op, writeMode);
  mstate.base = tv_lval(lookup.val);
}

OPTBLD_INLINE void baseLImpl(named_local_var loc, MOpMode mode, ReadonlyOp op) {
  auto& mstate = vmMInstrState();
  auto const local = loc.lval;
  if (mode == MOpMode::Warn && type(local) == KindOfUninit) {
    raise_undefined_local(vmfp(), loc.name);
  }

  mstate.roProp = false;
  if (readonlyLocalShouldThrow(*local, op)) {
    assertx(loc.name < vmfp()->func()->numNamedLocals());
    assertx(vmfp()->func()->localVarName(loc.name));
    auto const name = vmfp()->func()->localVarName(loc.name);
    throw_local_must_be_value_type(name->data());
  }
  mstate.base = local;
}

OPTBLD_INLINE void iopBaseL(named_local_var loc, MOpMode mode, ReadonlyOp op) {
  baseLImpl(loc, mode, op);
}

OPTBLD_INLINE void iopBaseC(uint32_t idx, MOpMode) {
  auto& mstate = vmMInstrState();
  mstate.base = vmStack().indC(idx);
  mstate.roProp = false;
}

OPTBLD_INLINE void iopBaseH() {
  auto& mstate = vmMInstrState();
  mstate.tvTempBase = make_tv<KindOfObject>(vmfp()->getThis());
  mstate.base = &mstate.tvTempBase;
  mstate.roProp = false;
}

static OPTBLD_INLINE void propDispatch(MOpMode mode, TypedValue key, ReadonlyOp op) {
  auto& mstate = vmMInstrState();
  auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());

  mstate.base = [&]{
    switch (mode) {
      case MOpMode::None:
        return Prop<MOpMode::None>(mstate.tvTempBase, ctx, *mstate.base, key, op);
      case MOpMode::Warn:
        return Prop<MOpMode::Warn>(mstate.tvTempBase, ctx, *mstate.base, key, op);
      case MOpMode::Define:
        return Prop<MOpMode::Define,KeyType::Any>(
          mstate.tvTempBase, ctx, *mstate.base, key, op
        );
      case MOpMode::Unset:
        return Prop<MOpMode::Unset>(mstate.tvTempBase, ctx, *mstate.base, key, op);
      case MOpMode::InOut:
        always_assert_flog(false, "MOpMode::InOut can only occur on Elem");
    }
    always_assert(false);
  }();
}

static OPTBLD_INLINE void propQDispatch(MOpMode mode, TypedValue key, ReadonlyOp op) {
  auto& mstate = vmMInstrState();
  auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());

  assertx(mode == MOpMode::None || mode == MOpMode::Warn);
  assertx(key.m_type == KindOfPersistentString);
  if (mode == MOpMode::None) {
    mstate.base = nullSafeProp<MOpMode::None>(mstate.tvTempBase, ctx,
                                              *mstate.base, key.m_data.pstr, op);
  } else {
    mstate.base = nullSafeProp<MOpMode::Warn>(mstate.tvTempBase, ctx,
                                              *mstate.base, key.m_data.pstr, op);
  }
}

static OPTBLD_INLINE
void elemDispatch(MOpMode mode, TypedValue key) {
  auto& mstate = vmMInstrState();
  auto const b = mstate.base;

  auto const baseValueToLval = [&](TypedValue base) {
    mstate.tvTempBase = base;
    return tv_lval { &mstate.tvTempBase };
  };

  auto const checkDimForReadonly = [&](DataType dt) {
    if (mstate.roProp && dt == KindOfObject) {
      throw_cannot_modify_readonly_collection();
    }
  };

  mstate.base = [&]{
    switch (mode) {
      case MOpMode::None:
        return baseValueToLval(Elem<MOpMode::None>(*b, key));
      case MOpMode::Warn:
        return baseValueToLval(Elem<MOpMode::Warn>(*b, key));
      case MOpMode::InOut:
        return baseValueToLval(Elem<MOpMode::InOut>(*b, key));
      case MOpMode::Define: {
        auto const result = ElemD(b, key);
        checkDimForReadonly(result.type());
        return result;
      }
      case MOpMode::Unset: {
        auto const result = ElemU(b, key);
        checkDimForReadonly(result.type());
        return result;
      }
    }
    always_assert(false);
  }();
}

static inline TypedValue key_tv(MemberKey key) {
  switch (key.mcode) {
    case MW:
      return TypedValue{};
    case MEL: case MPL: {
      auto const local = frame_local(vmfp(), key.local.id);
      if (type(local) == KindOfUninit) {
        raise_undefined_local(vmfp(), key.local.name);
        return make_tv<KindOfNull>();
      }
      return tvClassToString(*local);
    }
    case MEC: case MPC:
      return tvClassToString(*vmStack().indTV(key.iva));
    case MEI:
      return make_tv<KindOfInt64>(key.int64);
    case MET: case MPT: case MQT:
      return make_tv<KindOfPersistentString>(key.litstr);
  }
  not_reached();
}

static OPTBLD_INLINE
void maybeLogDynamicProp(MemberKey mk, TypedValue key) {
  if (!Cfg::Eval::LogDynamicPropAccessSampleRate) return;
  if (mk.mcode != MPC && mk.mcode != MPL) return;
  logDynamicPropAccess(*vmMInstrState().base, key);
}

static OPTBLD_INLINE
void dimDispatch(MOpMode mode, MemberKey mk) {
  auto const key = key_tv(mk);
  maybeLogDynamicProp(mk, key);
  if (mk.mcode == MQT) {
    propQDispatch(mode, key, mk.rop);
  } else if (mcodeIsProp(mk.mcode)) {
    propDispatch(mode, key, mk.rop);
  } else if (mcodeIsElem(mk.mcode)) {
    elemDispatch(mode, key);
  } else {
    if (mode == MOpMode::Warn) raise_error("Cannot use [] for reading");
    auto& mstate = vmMInstrState();
    mstate.base = NewElem(mstate.base);
  }
}

OPTBLD_INLINE void iopDim(MOpMode mode, MemberKey mk) {
  dimDispatch(mode, mk);
}

static OPTBLD_INLINE void mFinal(MInstrState& mstate,
                                 int32_t nDiscard,
                                 Optional<TypedValue> result) {
  auto& stack = vmStack();
  for (auto i = 0; i < nDiscard; ++i) stack.popTV();
  if (result) tvCopy(*result, *stack.allocTV());
}

static OPTBLD_INLINE
void queryMImpl(MemberKey mk, int32_t nDiscard, QueryMOp op) {
  auto& mstate = vmMInstrState();
  TypedValue result;
  switch (op) {
    case QueryMOp::InOut:
      always_assert_flog(
        mcodeIsElem(mk.mcode), "QueryM InOut is only compatible with Elem"
      );
      [[fallthrough]];
    case QueryMOp::CGet:
    case QueryMOp::CGetQuiet:
      dimDispatch(getQueryMOpMode(op), mk);
      tvDup(*mstate.base, result);
      break;

    case QueryMOp::Isset:
      result.m_type = KindOfBoolean;
      auto const key = key_tv(mk);
      if (mcodeIsProp(mk.mcode)) {
        maybeLogDynamicProp(mk, key);
        auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());
        result.m_data.num = IssetProp(ctx, *mstate.base, key);
      } else {
        assertx(mcodeIsElem(mk.mcode));
        result.m_data.num = IssetElem(*mstate.base, key);
      }
      break;
  }
  mFinal(mstate, nDiscard, result);
}

OPTBLD_INLINE void iopQueryM(uint32_t nDiscard, QueryMOp subop, MemberKey mk) {
  queryMImpl(mk, nDiscard, subop);
}

OPTBLD_INLINE void iopSetM(uint32_t nDiscard, MemberKey mk) {
  auto& mstate = vmMInstrState();
  auto const topC = vmStack().topC();

  if (mk.mcode == MW) {
    SetNewElem<true>(mstate.base, topC);
  } else {
    auto const key = key_tv(mk);
    maybeLogDynamicProp(mk, key);
    if (mcodeIsElem(mk.mcode)) {
      auto const result = SetElem<true>(mstate.base, key, topC);
      if (result) {
        tvDecRefGen(topC);
        topC->m_type = KindOfString;
        topC->m_data.pstr = result;
      }
    } else {
      auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());
      try {
        SetProp(ctx, *mstate.base, key, *topC, mk.rop);
      } catch (const InvalidSetMException& exn) {
        assertx(!isRefcountedType(type(exn.tv())));
        vmStack().popC();
        mFinal(mstate, nDiscard, exn.tv());
        return;
      }
    }
  }

  auto const result = *topC;
  vmStack().discard();
  mFinal(mstate, nDiscard, result);
}

OPTBLD_INLINE void iopSetRangeM( uint32_t nDiscard, uint32_t size, SetRangeOp op) {
  auto& mstate = vmMInstrState();
  auto const count = tvCastToInt64(*vmStack().indC(0));
  auto const src = *vmStack().indC(1);
  auto const offset = tvCastToInt64(*vmStack().indC(2));

  if (op == SetRangeOp::Forward) {
    SetRange<false>(mstate.base, offset, src, count, size);
  } else {
    SetRange<true>(mstate.base, offset, src, count, size);
  }

  mFinal(mstate, nDiscard + 3, std::nullopt);
}

OPTBLD_INLINE void iopIncDecM(uint32_t nDiscard, IncDecOp subop, MemberKey mk) {
  auto const key = key_tv(mk);
  maybeLogDynamicProp(mk, key);

  auto& mstate = vmMInstrState();
  auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());

  auto const result = [&]{
    if (mcodeIsProp(mk.mcode)) {
      return IncDecProp(ctx, subop, *mstate.base, key);
    } else if (mcodeIsElem(mk.mcode)) {
      return IncDecElem(subop, mstate.base, key);
    } else {
      return IncDecNewElem(subop, mstate.base);
    }
  }();

  mFinal(mstate, nDiscard, result);
}

OPTBLD_INLINE void iopSetOpM(uint32_t nDiscard, SetOpOp subop, MemberKey mk) {
  auto const key = key_tv(mk);
  maybeLogDynamicProp(mk, key);
  auto const rhs = vmStack().topC();

  auto& mstate = vmMInstrState();
  auto ctx = MemberLookupContext(arGetContextClass(vmfp()), vmfp()->func());
  auto const result = [&]{
    if (mcodeIsProp(mk.mcode)) {
      return *SetOpProp(mstate.tvTempBase, ctx,
                        subop, *mstate.base, key, rhs);
    } else if (mcodeIsElem(mk.mcode)) {
      return SetOpElem(subop, mstate.base, key, rhs);
    } else {
      return SetOpNewElem(subop, mstate.base, rhs);
    }
  }();

  vmStack().popC();
  tvIncRefGen(result);
  mFinal(mstate, nDiscard, result);
}

OPTBLD_INLINE void iopUnsetM(uint32_t nDiscard, MemberKey mk) {
  assertx(mcodeIsElem(mk.mcode));
  auto& mstate = vmMInstrState();
  UnsetElem(mstate.base, key_tv(mk));

  mFinal(mstate, nDiscard, std::nullopt);
}

namespace {

OPTBLD_INLINE const TypedValue* memoGetImpl(LocalRange keys) {
  auto const fp = vmfp();
  auto const func = fp->func();
  assertx(func->isMemoizeWrapper());
  assertx(keys.first + keys.count <= func->numLocals());

  for (auto i = 0; i < keys.count; ++i) {
    auto const key = frame_local(fp, keys.first + i);
    if (!isIntType(type(key)) && !isStringType(type(key))) {
      raise_error("Memoization keys can only be ints or strings");
    }
  }

  auto const c = [&] () -> const TypedValue* {
    if (!func->isMethod() || func->isStatic()) {
      auto const lsbCls =
        func->isMemoizeWrapperLSB() ? fp->getClass() : nullptr;
      if (keys.count > 0) {
        auto cache =
          lsbCls ? rds::bindLSBMemoCache(lsbCls, func)
                 : rds::bindStaticMemoCache(func);
        if (!cache.isInit()) return nullptr;
        auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
        if (auto getter = memoCacheGetForKeyCount(keys.count)) {
          return getter(*cache, keysBegin);
        }
        return memoCacheGetGeneric(
          *cache,
          GenericMemoId{func->getFuncId(), keys.count}.asParam(),
          keysBegin
        );
      }

      auto cache =
        lsbCls ? rds::bindLSBMemoValue(lsbCls, func)
               : rds::bindStaticMemoValue(func);
      return cache.isInit() ? cache.get() : nullptr;
    }

    checkThis(fp);
    auto const this_ = fp->getThis();
    auto const cls = func->cls();
    assertx(this_->instanceof(cls));
    assertx(cls->hasMemoSlots());

    auto const memoInfo = cls->memoSlotForFunc(func->getFuncId());

    auto const slot = UNLIKELY(this_->hasNativeData())
      ? this_->memoSlotNativeData(memoInfo.first, cls->getNativeDataInfo()->sz)
      : this_->memoSlot(memoInfo.first);

    if (keys.count == 0 && !memoInfo.second) {
      auto const val = slot->getValue();
      return val->m_type != KindOfUninit ? val : nullptr;
    }

    auto const cache = slot->getCache();
    if (!cache) return nullptr;

    if (memoInfo.second) {
      if (keys.count == 0) {
        return memoCacheGetSharedOnly(
          cache,
          makeSharedOnlyKey(func->getFuncId())
        );
      }
      auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
      if (auto const getter = sharedMemoCacheGetForKeyCount(keys.count)) {
        return getter(cache, func->getFuncId(), keysBegin);
      }
      return memoCacheGetGeneric(
        cache,
        GenericMemoId{func->getFuncId(), keys.count}.asParam(),
        keysBegin
      );
    }

    assertx(keys.count > 0);
    auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
    if (auto const getter = memoCacheGetForKeyCount(keys.count)) {
      return getter(cache, keysBegin);
    }
    return memoCacheGetGeneric(
      cache,
      GenericMemoId{func->getFuncId(), keys.count}.asParam(),
      keysBegin
    );
  }();

  assertx(!c || tvIsPlausible(*c));
  assertx(!c || c->m_type != KindOfUninit);
  return c;
}

}

OPTBLD_INLINE void iopMemoGet(PC& pc, PC notfound, LocalRange keys) {
  if (auto const c = memoGetImpl(keys)) {
    tvDup(*c, *vmStack().allocC());
  } else {
    pc = notfound;
  }
}

OPTBLD_INLINE void iopMemoGetEager(PC& pc,
                                   PC notfound,
                                   PC suspended,
                                   LocalRange keys) {
  assertx(vmfp()->func()->isAsyncFunction());
  assertx(!isResumed(vmfp()));

  if (auto const c = memoGetImpl(keys)) {
    tvDup(*c, *vmStack().allocC());
    if (!c->m_aux.u_asyncEagerReturnFlag) {
      assertx(tvIsObject(c) && c->m_data.pobj->isWaitHandle());
      pc = suspended;
    }
  } else {
    pc = notfound;
  }
}

namespace {

OPTBLD_INLINE void memoSetImpl(LocalRange keys, TypedValue val) {
  auto const fp = vmfp();
  auto const func = fp->func();
  assertx(func->isMemoizeWrapper());
  assertx(keys.first + keys.count <= func->numLocals());
  assertx(tvIsPlausible(val));

  for (auto i = 0; i < keys.count; ++i) {
    auto const key = frame_local(fp, keys.first + i);
    if (!isIntType(type(key)) && !isStringType(type(key))) {
      raise_error("Memoization keys can only be ints or strings");
    }
  }

  if (!func->isMethod() || func->isStatic()) {
    auto const lsbCls = func->isMemoizeWrapperLSB() ? fp->getClass() : nullptr;
    if (keys.count > 0) {
      auto cache =
        lsbCls ? rds::bindLSBMemoCache(lsbCls, func)
               : rds::bindStaticMemoCache(func);
      if (!cache.isInit()) cache.initWith(nullptr);
      auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
      if (auto setter = memoCacheSetForKeyCount(keys.count)) {
        return setter(*cache, keysBegin, val);
      }
      return memoCacheSetGeneric(
        *cache,
        GenericMemoId{func->getFuncId(), keys.count}.asParam(),
        keysBegin,
        val
      );
    }

    auto cache =
      lsbCls ? rds::bindLSBMemoValue(lsbCls, func)
             : rds::bindStaticMemoValue(func);
    if (!cache.isInit()) {
      tvWriteUninit(*cache);
      cache.markInit();
    }

    tvSetWithAux(val, *cache);
    return;
  }

  checkThis(fp);
  auto const this_ = fp->getThis();
  auto const cls = func->cls();
  assertx(this_->instanceof(cls));
  assertx(cls->hasMemoSlots());

  this_->setAttribute(ObjectData::UsedMemoCache);

  auto const memoInfo = cls->memoSlotForFunc(func->getFuncId());

  auto slot = UNLIKELY(this_->hasNativeData())
    ? this_->memoSlotNativeData(memoInfo.first, cls->getNativeDataInfo()->sz)
    : this_->memoSlot(memoInfo.first);

  if (keys.count == 0 && !memoInfo.second) {
    tvSetWithAux(val, *slot->getValue());
    return;
  }

  auto& cache = slot->getCacheForWrite();

  if (memoInfo.second) {
    if (keys.count == 0) {
      return memoCacheSetSharedOnly(
        cache,
        makeSharedOnlyKey(func->getFuncId()),
        val
      );
    }
    auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
    if (auto const setter = sharedMemoCacheSetForKeyCount(keys.count)) {
      return setter(cache, func->getFuncId(), keysBegin, val);
    }
    return memoCacheSetGeneric(
      cache,
      GenericMemoId{func->getFuncId(), keys.count}.asParam(),
      keysBegin,
      val
    );
  }

  assertx(keys.count > 0);
  auto const keysBegin = frame_local(fp, keys.first + keys.count - 1);
  if (auto const setter = memoCacheSetForKeyCount(keys.count)) {
    return setter(cache, keysBegin, val);
  }
  return memoCacheSetGeneric(
    cache,
    GenericMemoId{func->getFuncId(), keys.count}.asParam(),
    keysBegin,
    val
  );
}

}

OPTBLD_INLINE void iopMemoSet(LocalRange keys) {
  auto val = *vmStack().topC();
  assertx(val.m_type != KindOfUninit);
  if (vmfp()->func()->isAsyncFunction()) {
    assertx(tvIsObject(val) && val.m_data.pobj->isWaitHandle());
    val.m_aux.u_asyncEagerReturnFlag = 0;
  }
  memoSetImpl(keys, val);
}

OPTBLD_INLINE void iopMemoSetEager(LocalRange keys) {
  assertx(vmfp()->func()->isAsyncFunction());
  assertx(!isResumed(vmfp()));
  auto val = *vmStack().topC();
  assertx(val.m_type != KindOfUninit);
  val.m_aux.u_asyncEagerReturnFlag = static_cast<uint32_t>(-1);
  memoSetImpl(keys, val);
}

OPTBLD_INLINE void iopIssetG() {
  StringData* name;
  TypedValue* tv1 = vmStack().topTV();
  auto const lval = lookup_gbl(vmfp(), name, tv1);
  SCOPE_EXIT { decRefStr(name); };
  auto const e = lval && !tvIsNull(lval);
  vmStack().replaceC<KindOfBoolean>(e);
}

OPTBLD_INLINE void iopIssetS() {
  SpropState ss(vmStack(), true);
  bool e;
  if (!(ss.visible && ss.accessible)) {
    e = false;
  } else {
    e = !tvIsNull(ss.val);
  }
  ss.output->m_data.num = e;
  ss.output->m_type = KindOfBoolean;
}

OPTBLD_INLINE void iopIssetL(tv_lval val) {
  bool ret = !is_null(val);
  TypedValue* topTv = vmStack().allocTV();
  topTv->m_data.num = ret;
  topTv->m_type = KindOfBoolean;
}

OPTBLD_INLINE void iopIsUnsetL(tv_lval val) {
  bool ret = type(val) == KindOfUninit;
  TypedValue* topTv = vmStack().allocTV();
  topTv->m_data.num = ret;
  topTv->m_type = KindOfBoolean;
}

OPTBLD_INLINE static bool isTypeHelper(TypedValue val, IsTypeOp op) {
  assertx(tvIsPlausible(val));

  switch (op) {
  case IsTypeOp::Null:   return is_null(&val);
  case IsTypeOp::Bool:   return is_bool(&val);
  case IsTypeOp::Int:    return is_int(&val);
  case IsTypeOp::Dbl:    return is_double(&val);
  case IsTypeOp::Vec:    return is_vec(&val);
  case IsTypeOp::Dict:   return is_dict(&val);
  case IsTypeOp::Keyset: return is_keyset(&val);
  case IsTypeOp::Obj:    return is_object(&val);
  case IsTypeOp::Str:    return is_string(&val);
  case IsTypeOp::Res:    return tvIsResource(val);
  case IsTypeOp::Scalar: return tvAsCVarRef(val).isScalar();
  case IsTypeOp::ArrLike: return is_any_array(&val);
  case IsTypeOp::LegacyArrLike: {
    auto const& v = tvAsCVarRef(val);
    return v.isArray() && v.asCArrRef()->isLegacyArray();
  }
  case IsTypeOp::ClsMeth: return is_clsmeth(&val);
  case IsTypeOp::Func: return is_fun(&val);
  case IsTypeOp::Class: return is_class(&val);
  }
  not_reached();
}

OPTBLD_INLINE void iopIsTypeL(named_local_var loc, IsTypeOp op) {
  if (type(loc.lval) == KindOfUninit) {
    raise_undefined_local(vmfp(), loc.name);
  }
  vmStack().pushBool(isTypeHelper(*loc.lval, op));
}

OPTBLD_INLINE void iopIsTypeC(IsTypeOp op) {
  auto val = vmStack().topC();
  vmStack().replaceC(make_tv<KindOfBoolean>(isTypeHelper(*val, op)));
}

OPTBLD_INLINE void iopAssertRATL(local_var loc, RepoAuthType rat) {
  if (debug) {
    auto const val = *loc.lval;
    auto const func = vmfp()->func();
    auto vm = &*g_context;
    always_assert_flog(
      tvMatchesRepoAuthType(val, rat),
      "failed assert RATL on local slot {}: maybe ${} in {}:{}, expected {},"
      " got {}",
      loc.index,
      loc.index < func->numNamedLocals() && func->localNames()[loc.index]
      ? func->localNames()[loc.index]->data()
      : "<unnamed/unknown>",
      vm->getContainingFileName()->data(),
      vm->getLine(),
      show(rat),
      toStringElm(val)
    );
  }
}

OPTBLD_INLINE void iopAssertRATStk(uint32_t stkSlot, RepoAuthType rat) {
  if (debug) {
    auto const tv = *vmStack().indTV(stkSlot);
    auto vm = &*g_context;
    always_assert_flog(
      tvMatchesRepoAuthType(tv, rat),
      "failed assert RATStk {} in {}:{}, expected {}, got {}",
      stkSlot,
      vm->getContainingFileName()->data(),
      vm->getLine(),
      show(rat),
      toStringElm(tv)
    );
  }
}

OPTBLD_INLINE void iopAKExists() {
  TypedValue* arr = vmStack().topTV();
  auto key = tvClassToString(*(arr + 1));
  bool result = HHVM_FN(array_key_exists)(tvAsCVarRef(key), tvAsCVarRef(arr));
  vmStack().popTV();
  vmStack().replaceTV<KindOfBoolean>(result);
}

OPTBLD_INLINE void iopGetMemoKeyL(named_local_var loc) {
  DEBUG_ONLY auto const func = vmfp()->func();
  assertx(func->isMemoizeWrapper());
  assertx(tvIsPlausible(*loc.lval));

  if (UNLIKELY(type(loc.lval) == KindOfUninit)) {
    tvWriteNull(loc.lval);
    raise_undefined_local(vmfp(), loc.name);
  }

  // Use the generic scheme, which is performed by
  // serialize_memoize_param.
  auto const key = HHVM_FN(serialize_memoize_param)(*loc.lval);
  tvCopy(key, *vmStack().allocC());
}

OPTBLD_INLINE void iopIdx() {
  TypedValue* def = vmStack().topTV();
  auto const  key = tvClassToString(*vmStack().indTV(1));
  TypedValue* arr = vmStack().indTV(2);

  if (isNullType(key.m_type)) {
    tvDecRefGen(arr);
    *arr = *def;
    vmStack().ndiscard(2);
    return;
  }

  TypedValue result;
  if (isArrayLikeType(arr->m_type)) {
    result = HHVM_FN(hphp_array_idx)(tvAsCVarRef(arr),
                                     tvAsCVarRef(&key),
                                     tvAsCVarRef(def));
    vmStack().popTV();
  } else if (arr->m_type == KindOfObject) {
    auto obj = arr->m_data.pobj;
    if (obj->isCollection() && collections::contains(obj, tvAsCVarRef(&key))) {
      result = collections::at(obj, &key).tv();
      tvIncRefGen(result);
      vmStack().popTV();
    } else {
      result = *def;
      vmStack().discard();
    }
  } else if (isStringType(arr->m_type)) {
    // This replicates the behavior of the hack implementation of idx, which
    // first checks isset($arr[$idx]), then returns $arr[(int)$idx]
    auto str = arr->m_data.pstr;
    if (IssetElemString<KeyType::Any>(str, key)) {
      auto idx = tvCastToInt64(key);
      assertx(idx >= 0 && idx < str->size());
      result = make_tv<KindOfPersistentString>(str->getChar(idx));
      vmStack().popTV();
    } else {
      result = *def;
      vmStack().discard();
    }
  } else {
    result = *def;
    vmStack().discard();
  }
  vmStack().popTV();
  tvDecRefGen(arr);
  *arr = result;
}

OPTBLD_INLINE void iopArrayIdx() {
  TypedValue* def = vmStack().topTV();
  auto const  key = tvClassToString(*vmStack().indTV(1));
  TypedValue* arr = vmStack().indTV(2);
  if (isClsMethType(type(arr))) {
    tvCastToVecInPlace(arr);
  }
  auto const result = HHVM_FN(hphp_array_idx)(tvAsCVarRef(arr),
                                              tvAsCVarRef(&key),
                                              tvAsCVarRef(def));
  vmStack().popTV();
  vmStack().popTV();
  tvDecRefGen(arr);
  *arr = result;
}

namespace {
void implArrayMarkLegacy(bool legacy) {
  auto const recursive = *vmStack().topTV();
  if (!tvIsBool(recursive)) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format("$recursive must be a bool; got {}",
                     getDataTypeString(type(recursive))));
  }

  auto const input = vmStack().indTV(1);
  auto const output = val(recursive).num
    ? arrprov::markTvRecursively(*input, legacy)
    : arrprov::markTvShallow(*input, legacy);

  vmStack().popTV();
  tvMove(output, input);
}
}

OPTBLD_INLINE void iopArrayMarkLegacy() {
  implArrayMarkLegacy(true);
}

OPTBLD_INLINE void iopArrayUnmarkLegacy() {
  implArrayMarkLegacy(false);
}

OPTBLD_INLINE void iopSetL(tv_lval to) {
  TypedValue* fr = vmStack().topC();
  tvSet(*fr, to);
}

OPTBLD_INLINE void iopPopG() {
  StringData* name;
  TypedValue* fr = vmStack().topC();
  TypedValue* tv2 = vmStack().indTV(1);
  auto const to = lookupd_gbl(vmfp(), name, tv2);
  SCOPE_EXIT { decRefStr(name); };
  assertx(to);
  tvMove(*fr, to);
  vmStack().discard();
  vmStack().discard();
}

OPTBLD_INLINE void iopSetS(ReadonlyOp op) {
  TypedValue* tv1 = vmStack().topTV();
  TypedValue* clsCell = vmStack().indC(1);
  TypedValue* propn = vmStack().indTV(2);
  TypedValue* output = propn;
  StringData* name;
  TypedValue* val;
  bool visible, accessible, readonly, constant;
  Slot slot;

  if (!isClassType(clsCell->m_type)) {
    raise_error("Attempting static property access on non class");
  }
  auto const cls = clsCell->m_data.pclass;

  lookup_sprop(vmfp(), cls, name, propn, val, slot, visible,
               accessible, constant, readonly, true);

  SCOPE_EXIT { decRefStr(name); };

  if (!readonly && op == ReadonlyOp::Readonly) {
    throw_must_be_readonly(cls->name()->data(), name->data());
  }

  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                cls->name()->data(),
                name->data());
  }
  if (constant) {
    throw_cannot_modify_static_const_prop(cls->name()->data(), name->data());
  }
  if (Cfg::Eval::CheckPropTypeHints > 0) {
    auto const& sprop = cls->staticProperties()[slot];
    sprop.typeConstraints.verifyStaticProperty(tv1, cls, sprop.cls, name);
  }
  always_assert(cls->sPropLink(slot).isLocal());
  tvSet(*tv1, *val);
  tvDecRefGen(propn);
  memcpy(output, tv1, sizeof(TypedValue));
  vmStack().ndiscard(2);
}

OPTBLD_INLINE void iopSetOpL(tv_lval to, SetOpOp op) {
  TypedValue* fr = vmStack().topC();
  setopBody(to, op, fr);
  tvDecRefGen(fr);
  tvDup(*to, *fr);
}

OPTBLD_INLINE void iopSetOpS(SetOpOp op) {
  TypedValue* fr = vmStack().topC();
  TypedValue* clsCell = vmStack().indC(1);
  TypedValue* propn = vmStack().indTV(2);
  TypedValue* output = propn;
  StringData* name;
  TypedValue* val;
  bool visible, accessible, readonly, constant;
  Slot slot;

  if (!isClassType(clsCell->m_type)) {
    raise_error("Attempting static property access on non class");
  }
  auto const cls = clsCell->m_data.pclass;

  lookup_sprop(vmfp(), cls, name, propn, val, slot, visible,
               accessible, constant, readonly, false);
  SCOPE_EXIT { decRefStr(name); };
  if (!(visible && accessible)) {
    raise_error("Invalid static property access: %s::%s",
                cls->name()->data(),
                name->data());
  }
  if (constant) {
    throw_cannot_modify_static_const_prop(cls->name()->data(), name->data());
  }
  auto const& sprop = cls->staticProperties()[slot];
  if (setOpNeedsTypeCheck(sprop.typeConstraints, op, val)) {
    TypedValue temp;
    tvDup(*val, temp);
    SCOPE_FAIL { tvDecRefGen(&temp); };
    setopBody(&temp, op, fr);
    sprop.typeConstraints.verifyStaticProperty(
      &temp, cls, sprop.cls, name
    );
    always_assert(cls->sPropLink(slot).isLocal());
    tvMove(temp, *val);
  } else {
    always_assert(cls->sPropLink(slot).isLocal());
    setopBody(val, op, fr);
  }

  tvDecRefGen(propn);
  tvDecRefGen(fr);
  tvDup(*val, *output);
  vmStack().ndiscard(2);
}

OPTBLD_INLINE void iopIncDecL(named_local_var fr, IncDecOp op) {
  TypedValue* to = vmStack().allocTV();
  tvWriteUninit(*to);
  if (UNLIKELY(type(fr.lval) == KindOfUninit)) {
    raise_undefined_local(vmfp(), fr.name);
    tvWriteNull(fr.lval);
  }
  tvCopy(IncDecBody(op, fr.lval), *to);
}

OPTBLD_INLINE void iopIncDecS(IncDecOp op) {
  SpropState ss(vmStack(), false);
  if (!(ss.visible && ss.accessible)) {
    raise_error("Invalid static property access: %s::%s",
                ss.cls->name()->data(),
                ss.name->data());
  }
  if (ss.constant) {
    throw_cannot_modify_static_const_prop(ss.cls->name()->data(),
                                          ss.name->data());
  }
  auto const checkable_sprop = [&]() -> const Class::SProp* {
    if (Cfg::Eval::CheckPropTypeHints <= 0) return nullptr;
    auto const& sprop = ss.cls->staticProperties()[ss.slot];
    return sprop.typeConstraints.isCheckable() ? &sprop : nullptr;
  }();

  auto const val = ss.val;
  if (checkable_sprop) {
    TypedValue temp;
    tvDup(*val, temp);
    SCOPE_FAIL { tvDecRefGen(&temp); };
    auto result = IncDecBody(op, &temp);
    SCOPE_FAIL { tvDecRefGen(&result); };
    checkable_sprop->typeConstraints.verifyStaticProperty(
      &temp,
      ss.cls,
      checkable_sprop->cls,
      ss.name
    );
    always_assert(ss.cls->sPropLink(ss.slot).isLocal());
    tvMove(temp, *val);
    tvCopy(result, *ss.output);
  } else {
    always_assert(ss.cls->sPropLink(ss.slot).isLocal());
    tvCopy(IncDecBody(op, val), *ss.output);
  }
}

OPTBLD_INLINE void iopUnsetL(tv_lval loc) {
  tvUnset(loc);
}

OPTBLD_INLINE void iopUnsetG() {
  TypedValue* tv1 = vmStack().topTV();
  StringData* name = lookup_name(tv1);
  SCOPE_EXIT { decRefStr(name); };
  auto env = g_context->m_globalNVTable;
  assertx(env != nullptr);
  env->unset(name);
  vmStack().popC();
}

namespace {

void initClosureLocals() {
  auto const ar = vmfp();
  if (!ar->func()->isClosureBody()) return;
  c_Closure::initActRecFromClosure(ar);
}

void initRegularLocals() {
  auto const ar = vmfp();
  auto const func = ar->func();
  auto const firstRegularLocal = func->firstRegularLocalId();
  auto const numLocals = func->numLocals();
  for (auto i = firstRegularLocal; i < numLocals; ++i) {
    tvWriteUninit(frame_local(ar, i));
  }
}

}

bool funcEntry() {
  assertx(!isResumed(vmfp()));
  assertx(
    reinterpret_cast<TypedValue*>(vmfp()) - vmStack().top() ==
      vmfp()->func()->numSlotsInFrame()
  );

  initClosureLocals();
  initRegularLocals();

  // DV initializers delay the function call event hook until the Enter opcode.
  if (vmpc() != vmfp()->func()->entry()) return true;

  // If this returns false, the callee was intercepted and should be skipped.
  return EventHook::FunctionCall(
    vmfp(), EventHook::NormalFunc, EventHook::Source::Interpreter);
}

void doFCall(PrologueFlags prologueFlags, const Func* func,
             const ArrayData* namedArgNames,
             uint32_t numPositionalArgs, void* ctx, TCA retAddr) {
  TRACE(3, "FCall: pc %p func %p\n", vmpc(), vmfp()->func()->entry());
  if (Cfg::Eval::LogJitFunctionLivenessSampleRate > 0) {
    if (!func->atomicFlags().check(Func::Flags::WasCalled)
        && !func->atomicFlags().set(Func::Flags::WasCalled)
        && folly::Random::oneIn(Cfg::Eval::LogJitFunctionLivenessSampleRate)) {
      markFunctionAsLive(func);
    }
  }

  assertx(numPositionalArgs <= func->numPositionalParams() + 1);
  assertx(kNumActRecCells == 2);
  auto numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  ActRec* ar = vmStack().indA(
    numPositionalArgs + numNamedArgs + (prologueFlags.hasGenerics() ? 1 : 0));
  // Callee checks and input initialization.
  calleeGenericsChecks(func, prologueFlags.hasGenerics());
  calleeNamedArgChecks(func, numPositionalArgs, namedArgNames);
  calleeArgumentArityChecks(func, numPositionalArgs);
  calleeArgumentTypeChecks(func, numPositionalArgs, namedArgNames, ctx);
  calleeDynamicCallChecks(func, prologueFlags.isDynamicCall());
  calleeCoeffectChecks(func, prologueFlags.coeffects(), numPositionalArgs, ctx);
  func->recordCall();
  initFuncInputs(func, numPositionalArgs);

  ar->m_sfp = vmfp();
  ar->setFunc(func);
  ar->setJitReturn(retAddr);
  ar->m_callOffAndFlags = ActRec::encodeCallOffsetAndFlags(
    prologueFlags.callOffset(),
    prologueFlags.asyncEagerReturn() ? (1 << ActRec::AsyncEagerRet) : 0
  );
  ar->setThisOrClassAllowNull(ctx);

  // A `named t...' parameter absorbs argument names matching no named
  // parameter, so the raw count of named arguments does not tell us whether
  // every named parameter was given a value.
  prepareFuncEntry(ar, numPositionalArgs,
                   numMatchedNamedArgs(func, namedArgNames) ==
                     func->numNamedParams());
}

namespace {

enum class NoCtx {};

void* takeCtx(Class* cls) { return cls; }
void* takeCtx(Object& obj) = delete;
void* takeCtx(Object&& obj) { return obj.detach(); }
void* takeCtx(NoCtx) {
  if (debug) return reinterpret_cast<void*>(ActRec::kTrashedThisSlot);
  return nullptr;
}

template<bool dynamic, typename Ctx>
JitResumeAddr fcallImpl(PC origpc, PC& pc, const FCallArgs& fca,
                        const Func* func, Ctx&& ctx,
                        bool logAsDynamicCall = true, bool isCtor = false) {
  const ArrayData* nameArr = nullptr;
  if (fca.hasNamedArgs()) {
    nameArr = vmfp()->func()->unit()->lookupArrayId(fca.namedArgNames);
  }

  if (fca.enforceInOut()) checkInOutMismatch(func, fca.numArgs, fca.inoutArgs);
  if (fca.enforceReadonly()) {
    checkReadonlyMismatch(func, fca.numArgs, fca.readonlyArgs, nameArr);
  }
  if (fca.enforceMutableReturn() && (func->attrs() & AttrReadonlyReturn)) {
    throwReadonlyMismatch(func, kReadonlyReturnId);
  }
  if (fca.enforceReadonlyThis() && !(func->attrs() & AttrReadonlyThis)) {
    throwReadonlyMismatch(func, kReadonlyThisId);
  }
  if (dynamic && logAsDynamicCall) callerDynamicCallChecks(func);
  checkStack(vmStack(), func, 0);

  auto const numPositionalArgs = [&] {
    uint32_t numNamedArgs = nameArr ? nameArr->size() : 0;
    assertx(fca.numArgs >= numNamedArgs);
    auto numPositionalArgs = fca.numArgs - numNamedArgs;
    auto numPositionalParams = func->numPositionalParams();
    if (UNLIKELY(fca.hasUnpack())) {
      GenericsSaver gs{fca.hasGenerics()};
      return prepareUnpackArgs(func, numPositionalArgs, true);
    }
    if (UNLIKELY(numPositionalArgs > numPositionalParams)) {
      GenericsSaver gs{fca.hasGenerics()};
      uint32_t delta = numPositionalArgs - numPositionalParams;
      iopNewVec(delta);
      return numPositionalParams + 1;
    }

    return numPositionalArgs;
  }();

  auto const prologueFlags = PrologueFlags(
    fca.hasGenerics(),
    dynamic,
    fca.namedArgNames != kInvalidId,
    fca.asyncEagerOffset != kInvalidOffset && func->supportsAsyncEagerReturn(),
    Offset(origpc - vmfp()->func()->entry()),
    0,  // generics bitmap not used by interpreter
    vmfp()->providedCoeffectsForCall(isCtor)
  );

  doFCall(prologueFlags, func, nameArr, numPositionalArgs,
          takeCtx(std::forward<Ctx>(ctx)), jit::tc::ustubs().retHelper);

  // Let JIT handle FuncEntry.
  pc = vmpc();
  return
    JitResumeAddr::helper(jit::tc::ustubs().resumeHelperFuncEntryFromInterp);
}

const StaticString s___invoke("__invoke");

// This covers both closures and functors.
OPTBLD_INLINE JitResumeAddr fcallFuncObj(PC origpc, PC& pc,
                                         const FCallArgs& fca) {
  assertx(tvIsObject(vmStack().topC()));
  auto obj = Object::attach(vmStack().topC()->m_data.pobj);
  vmStack().discard();

  auto const cls = obj->getVMClass();
  auto const func = cls->lookupMethod(s___invoke.get());

  if (func == nullptr) {
    raise_error(Strings::FUNCTION_NAME_MUST_BE_STRING);
  }

  if (func->isStaticInPrologue()) {
    obj.reset();
    return fcallImpl<false>(origpc, pc, fca, func, cls);
  } else {
    return fcallImpl<false>(origpc, pc, fca, func, std::move(obj));
  }
}

/*
 * Supports callables:
 *   array($instance, 'method')
 *   array('Class', 'method'),
 *   vec[$instance, 'method'],
 *   vec['Class', 'method'],
 *   dict[0 => $instance, 1 => 'method'],
 *   dict[0 => 'Class', 1 => 'method'],
 *   array(Class*, Func*),
 *   array(ObjectData*, Func*),
 */
OPTBLD_INLINE JitResumeAddr fcallFuncArr(PC origpc, PC& pc,
                                         const FCallArgs& fca) {
  assertx(tvIsArrayLike(vmStack().topC()));
  auto arr = Array::attach(vmStack().topC()->m_data.parr);
  vmStack().discard();

  ObjectData* thiz = nullptr;
  HPHP::Class* cls = nullptr;
  bool dynamic = false;

  auto const func = vm_decode_function(const_variant_ref{arr}, vmfp(), thiz,
                                       cls, dynamic, DecodeFlags::NoWarn);
  assertx(dynamic);
  if (UNLIKELY(func == nullptr)) {
    raise_error("Invalid callable (array)");
  }

  checkModuleBoundaryViolation(cls, *func);
  if (cls) checkModuleBoundaryViolation(*cls);

  Object thisRC(thiz);
  arr.reset();

  if (thisRC) {
    return fcallImpl<true>(origpc, pc, fca, func, std::move(thisRC));
  } else if (cls) {
    return fcallImpl<true>(origpc, pc, fca, func, cls);
  } else {
    return fcallImpl<true>(origpc, pc, fca, func, NoCtx{});
  }
}

/*
 * Supports callables:
 *   'func_name'
 *   'class::method'
 */
OPTBLD_INLINE JitResumeAddr fcallFuncStr(PC origpc, PC& pc,
                                         const FCallArgs& fca) {
  assertx(tvIsString(vmStack().topC()));
  auto str = OptString::attach(vmStack().topC()->m_data.pstr);
  vmStack().discard();

  ObjectData* thiz = nullptr;
  HPHP::Class* cls = nullptr;
  bool dynamic = false;

  auto const func = vm_decode_function(const_variant_ref{str}, vmfp(), thiz,
                                       cls, dynamic, DecodeFlags::NoWarn);
  assertx(dynamic);
  if (UNLIKELY(func == nullptr)) {
    raise_call_to_undefined(str.get());
  }

  checkModuleBoundaryViolation(cls, *func);
  if (cls) checkModuleBoundaryViolation(*cls);

  Object thisRC(thiz);
  str.reset();

  if (thisRC) {
    return fcallImpl<true>(origpc, pc, fca, func, std::move(thisRC));
  } else if (cls) {
    return fcallImpl<true>(origpc, pc, fca, func, cls);
  } else {
    return fcallImpl<true>(origpc, pc, fca, func, NoCtx{});
  }
}

OPTBLD_INLINE JitResumeAddr fcallFuncFunc(PC origpc, PC& pc,
                                          const FCallArgs& fca) {
  assertx(tvIsFunc(vmStack().topC()));
  auto func = vmStack().topC()->m_data.pfunc;
  vmStack().discard();

  if (func->cls()) {
    raise_error(Strings::CALL_ILLFORMED_FUNC);
  }

  return fcallImpl<false>(origpc, pc, fca, func, NoCtx{});
}

OPTBLD_INLINE JitResumeAddr fcallFuncRFunc(PC origpc, PC& pc,
                                           const FCallArgs& fca) {
  assertx(tvIsRFunc(vmStack().topC()));
  auto const rfunc = vmStack().topC()->m_data.prfunc;
  auto const func = rfunc->m_func;
  vmStack().discard();
  vmStack().pushArrayLike(rfunc->m_arr);
  decRefRFunc(rfunc);

  return
    fcallImpl<false>(origpc, pc, fca.withGenerics(), func, NoCtx{});
}

OPTBLD_INLINE JitResumeAddr fcallFuncClsMeth(PC origpc, PC& pc,
                                             const FCallArgs& fca) {
  assertx(tvIsClsMeth(vmStack().topC()));
  auto const clsMeth = vmStack().topC()->m_data.pclsmeth;
  vmStack().discard();

  const Func* func = clsMeth->getFunc();
  auto const cls = clsMeth->getCls();
  assertx(func && cls);

  return fcallImpl<false>(origpc, pc, fca, func, cls);
}

OPTBLD_INLINE JitResumeAddr fcallFuncRClsMeth(PC origpc, PC& pc,
                                              const FCallArgs& fca) {
  assertx(tvIsRClsMeth(vmStack().topC()));
  auto const rclsMeth = vmStack().topC()->m_data.prclsmeth;
  auto const cls = rclsMeth->m_cls;
  auto const func = rclsMeth->m_func;
  vmStack().discard();
  vmStack().pushArrayLike(rclsMeth->m_arr);
  decRefRClsMeth(rclsMeth);

  return fcallImpl<false>(origpc, pc, fca.withGenerics(), func, cls);
}

Func* resolveFuncImpl(Id id) {
  auto unit = vmfp()->func()->unit();
  auto const nep = unit->lookupNamedFuncPairId(id);
  auto func = Func::resolve(nep.second, nep.first, vmfp()->func());
  if (func) return func->unwrap();
  raise_resolve_func_undefined(unit->lookupLitstrId(id));
}

OPTBLD_INLINE void iopResolveFunc(Id id) {
  auto func = resolveFuncImpl(id);
  vmStack().pushFunc(func);
}

OPTBLD_INLINE void iopResolveMethCaller(Id id) {
  auto unit = vmfp()->func()->unit();
  auto const nep = unit->lookupNamedFuncPairId(id);
  auto func = Func::resolve(nep.second, nep.first, vmfp()->func());
  assertx(func && func->isMethCaller());
  checkMethCaller(func, arGetContextClass(vmfp()));
  vmStack().pushFunc(func);
}

RFuncData* newRFuncImpl(Func* func, ArrayData* reified_generics) {
  auto rfunc = RFuncData::newInstance(func, reified_generics);
  TRACE(2, "ResolveRFunc: just created new rfunc %s: %p\n",
        func->name()->data(), rfunc);
  return rfunc;
}

} // namespace

OPTBLD_INLINE void iopResolveRFunc(Id id) {
  auto const tsList = vmStack().topC();

  // Should I refactor this out with iopNewObj*?
  auto const reified = [&] () -> ArrayData* {
    if (!tvIsVec(tsList)) {
      raise_error("Attempting ResolveRFunc with invalid reified generics");
    }
    return tsList->m_data.parr;
  }();

  auto func = resolveFuncImpl(id);
  if (!func->hasReifiedGenerics()) {
    vmStack().popC();
    vmStack().pushFunc(func);
  } else {
    checkFunReifiedGenericMismatch(func, reified);
    auto rfunc = newRFuncImpl(func, reified);
    vmStack().discard();
    vmStack().pushRFuncNoRc(rfunc);
  }
}

OPTBLD_INLINE JitResumeAddr iopFCallFunc(PC origpc, PC& pc, FCallArgs fca) {
  auto const type = vmStack().topC()->m_type;
  if (isObjectType(type)) return fcallFuncObj(origpc, pc, fca);
  if (isArrayLikeType(type)) return fcallFuncArr(origpc, pc, fca);
  if (isStringType(type)) return fcallFuncStr(origpc, pc, fca);
  if (isFuncType(type)) return fcallFuncFunc(origpc, pc, fca);
  if (isRFuncType(type)) return fcallFuncRFunc(origpc, pc, fca);
  if (isClsMethType(type)) return fcallFuncClsMeth(origpc, pc, fca);
  if (isRClsMethType(type)) return fcallFuncRClsMeth(origpc, pc, fca);

  raise_error(Strings::FUNCTION_NAME_MUST_BE_STRING);
}

OPTBLD_INLINE JitResumeAddr iopFCallFuncD(PC origpc, PC& pc, FCallArgs fca,
                                          Id id) {
  auto const nep = vmfp()->unit()->lookupNamedFuncPairId(id);
  auto const func = [&]() {
    auto const f = Func::resolve(nep.second, nep.first, vmfp()->func());
    if (UNLIKELY(f == nullptr)) {
      raise_call_to_undefined(vmfp()->unit()->lookupLitstrId(id));
    }
    return f->unwrap();
  }();

  return fcallImpl<false>(origpc, pc, fca, func, NoCtx{});
}

namespace {

const StaticString
  s_DynamicContextOverrideUnsafe("__SystemLib\\DynamicContextOverrideUnsafe");

template<bool dynamic>
JitResumeAddr fcallObjMethodImpl(PC origpc, PC& pc, const FCallArgs& fca,
                                 StringData* methName) {
  const Func* func;
  LookupResult res;
  assertx(tvIsObject(vmStack().indC(fca.numInputs() + (kNumActRecCells - 1))));
  auto const obj =
    vmStack().indC(fca.numInputs() + (kNumActRecCells - 1))->m_data.pobj;
  auto cls = obj->getVMClass();
  auto const ctx = [&] {
    if (!fca.context) return arGetContextClass(vmfp());
    if (fca.context->tsame(s_DynamicContextOverrideUnsafe.get())) {
      if (Cfg::Repo::Authoritative) {
        raise_error("Cannot use dynamic_meth_caller_force() in repo-mode");
      }
      return cls;
    }
    return Class::load(fca.context);
  }();
  auto const callCtx = MemberLookupContext(ctx, vmfp()->func());
  // if lookup throws, obj will be decref'd via stack
  res = lookupObjMethod(func, cls, methName, callCtx, MethodLookupErrorOptions::RaiseOnNotFound);
  assertx(func);
  decRefStr(methName);
  if (res == LookupResult::MethodFoundNoThis) {
    throw_has_this_need_static(func);
  }
  assertx(res == LookupResult::MethodFoundWithThis);

  if (func->hasReifiedGenerics() && !fca.hasGenerics() &&
      !func->getGenericsInfo().allGenericsSoft()) {
    throw_call_reified_func_without_generics(func);
  }

  // fcallImpl() will do further checks before spilling the ActRec. If any
  // of these checks fail, make sure it gets decref'd only via ctx.
  tvWriteNull(*vmStack().indC(fca.numInputs() + (kNumActRecCells - 1)));
  return
    fcallImpl<dynamic>(origpc, pc, fca, func, Object::attach(obj));
}

static void raise_resolve_non_object(const char* methodName,
                                     const char* typeName = nullptr) {
  auto const msg = fmt::format(
    "Cannot resolve a member function {}() on a non-object ({})",
    methodName, typeName
  );

  raise_fatal_error(msg.c_str());
}

static void throw_call_non_object(const char* methodName,
                                  const char* typeName = nullptr) {
  std::string msg;
  fmt::format_to(std::back_inserter(msg), "Call to a member function {}() on a non-object ({})",
    methodName, typeName);

  if (Cfg::ErrorHandling::ThrowExceptionOnBadMethodCall) {
    SystemLib::throwBadMethodCallExceptionObject(OptString(msg));
  }
  raise_fatal_error(msg.c_str());
}

ALWAYS_INLINE bool
fcallObjMethodHandleInput(const FCallArgs& fca, ObjMethodOp op,
                          const StringData* methName, bool extraStk) {
  TypedValue* obj = vmStack().indC(fca.numInputs()
                                   + (kNumActRecCells - 1)
                                   + (extraStk ? 1 : 0));
  if (LIKELY(isObjectType(obj->m_type))) return false;

  if (UNLIKELY(op == ObjMethodOp::NullThrows || !isNullType(obj->m_type))) {
    auto const dataTypeStr = getDataTypeString(obj->m_type).get();
    throw_call_non_object(methName->data(), dataTypeStr->data());
  }

  // null?->method(...), pop extra stack input, all arguments and two uninits,
  // the null "object" and all uninits for inout returns, then push null.
  auto& stack = vmStack();
  if (extraStk) stack.popC();
  if (fca.hasGenerics()) stack.popC();
  if (fca.hasUnpack()) stack.popC();

  // Save any inout arguments, as those will be pushed unchanged as
  // the output.
  std::vector<TypedValue> inOuts;
  for (uint32_t i = 0; i < fca.numArgs; ++i) {
    if (fca.enforceInOut() && fca.isInOut(fca.numArgs - i - 1)) {
      inOuts.emplace_back(*stack.top());
      stack.discard();
    } else {
      stack.popTV();
    }
  }
  stack.popU();
  stack.popC();
  for (uint32_t i = 0; i < fca.numRets - 1; ++i) stack.popU();

  assertx(inOuts.size() == fca.numRets - 1);
  for (auto const tv : inOuts) *stack.allocC() = tv;
  stack.pushNull();

  // Handled.
  return true;
}

} // namespace

OPTBLD_INLINE JitResumeAddr
iopFCallObjMethod(PC origpc, PC& pc, FCallArgs fca, const StringData*,
                  ObjMethodOp op) {
  TypedValue* c1 = vmStack().topC(); // Method name.
  if (!isStringType(c1->m_type)) {
    raise_error(Strings::METHOD_NAME_MUST_BE_STRING);
  }

  StringData* methName = c1->m_data.pstr;
  if (fcallObjMethodHandleInput(fca, op, methName, true)) {
    return JitResumeAddr::none();
  }

  // We handle decReffing method name in fcallObjMethodImpl
  vmStack().discard();
  return fcallObjMethodImpl<true>(origpc, pc, fca, methName);
}

OPTBLD_INLINE JitResumeAddr
iopFCallObjMethodD(PC origpc, PC& pc, FCallArgs fca,
                   const StringData*, ObjMethodOp op,
                   const StringData* methName) {
  if (fcallObjMethodHandleInput(fca, op, methName, false)) {
    return JitResumeAddr::none();
  }
  auto const methNameC = const_cast<StringData*>(methName);
  return fcallObjMethodImpl<false>(origpc, pc, fca, methNameC);
}

Class* specialClsRefToCls(SpecialClsRef ref) {
  switch (ref) {
    case SpecialClsRef::LateBoundCls:
      if (auto const cls = frameStaticClass(vmfp())) return cls;
      raise_error(HPHP::Strings::CANT_ACCESS_STATIC);
    case SpecialClsRef::SelfCls:
      if (auto const cls = arGetContextClass(vmfp())) return cls;
      raise_error(HPHP::Strings::CANT_ACCESS_SELF);
    case SpecialClsRef::ParentCls:
      if (auto const cls = arGetContextClass(vmfp())) {
        if (auto const parent = cls->parent()) return parent;
        raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_PARENT);
      }
      raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_CLASS);
  }
  always_assert(false);
}

namespace {

const Func* resolveClsMethodFunc(Class* cls, const StringData* methName) {
  const Func* func;
  auto const ctx = arGetContextClass(vmfp());
  auto const callCtx = MemberLookupContext(ctx, vmfp()->func());
  auto const res = lookupClsMethod(func, cls, methName, nullptr,
                                   callCtx,
                                   MethodLookupErrorOptions::NoErrorOnModule);
  if (res == LookupResult::MethodNotFound) {
    raise_error("Failure to resolve method name \'%s::%s\'",
                cls->name()->data(), methName->data());
  }
  assertx(res == LookupResult::MethodFoundNoThis);
  assertx(func);
  checkClsMethFuncHelper(func);
  checkModuleBoundaryViolation(cls, *func);
  return func;
}

template<bool extraStk = false>
void resolveClsMethodImpl(Class* cls, const StringData* methName) {
  const Func* func = resolveClsMethodFunc(cls, methName);
  auto clsmeth = ClsMethDataRef::create(cls, const_cast<Func*>(func));
  if (extraStk) vmStack().popC();
  vmStack().pushClsMethNoRc(clsmeth);
}

} // namespace

OPTBLD_INLINE void iopResolveClsMethod(const StringData* methName) {
  auto const c = vmStack().topC();
  if (!isClassType(c->m_type)) {
    raise_error("Attempting ResolveClsMethod with non-class");
  }
  resolveClsMethodImpl<true>(c->m_data.pclass, methName);
}

OPTBLD_INLINE void iopResolveClsMethodD(Id classId,
                                        const StringData* methName) {
  auto const nep = vmfp()->func()->unit()->lookupNamedTypePairId(classId);
  auto cls = Class::resolve(nep.second, nep.first, vmfp()->func());
  if (UNLIKELY(cls == nullptr)) {
    raise_error("Failure to resolve class name \'%s\'", nep.first->data());
  }
  resolveClsMethodImpl(cls, methName);
}

OPTBLD_INLINE void iopResolveClsMethodS(SpecialClsRef ref,
                                        const StringData* methName) {
  resolveClsMethodImpl(specialClsRefToCls(ref), methName);
}

namespace {

template<bool extraStk = false>
void resolveRClsMethodImpl(Class* cls, const StringData* methName) {
  const Func* func = resolveClsMethodFunc(cls, methName);

  auto const tsList = vmStack().topC();
  auto const reified = [&] () -> ArrayData* {
    if (!tvIsVec(tsList)) {
      raise_error("Invalid reified generics when resolving class method");
    }
    return tsList->m_data.parr;
  }();

  if (func->hasReifiedGenerics()) {
    checkFunReifiedGenericMismatch(func, reified);
    auto rclsmeth = RClsMethData::create(cls, const_cast<Func*>(func), reified);
    vmStack().discard();
    if (extraStk) vmStack().popC();
    vmStack().pushRClsMethNoRc(rclsmeth);
  } else {
    auto clsmeth = ClsMethDataRef::create(cls, const_cast<Func*>(func));
    vmStack().popC();
    if (extraStk) vmStack().popC();
    vmStack().pushClsMethNoRc(clsmeth);
  }
}

} // namespace

OPTBLD_INLINE void iopResolveRClsMethod(const StringData* methName) {
  auto const c = vmStack().indC(1);
  if (!isClassType(c->m_type)) {
    raise_error("Attempting ResolveRClsMethod with non-class");
  }
  resolveRClsMethodImpl<true>(c->m_data.pclass, methName);
}

OPTBLD_INLINE void iopResolveRClsMethodD(Id classId,
                                         const StringData* methName) {
  auto const nep = vmfp()->func()->unit()->lookupNamedTypePairId(classId);
  auto cls = Class::resolve(nep.second, nep.first, vmfp()->func());
  if (UNLIKELY(cls == nullptr)) {
    raise_error("Failure to resolve class name \'%s\'", nep.first->data());
  }
  resolveRClsMethodImpl<false>(cls, methName);
}

OPTBLD_INLINE void iopResolveRClsMethodS(SpecialClsRef ref,
                                         const StringData* methName) {
  resolveRClsMethodImpl<false>(specialClsRefToCls(ref), methName);
}

namespace {

JitResumeAddr fcallClsMethodImpl(PC origpc, PC& pc,
                                 const FCallArgs& fca, Class* cls,
                                 const StringData* methName, bool forwarding) {
  auto const ctx = [&] {
    if (!fca.context) return liveClass();
    if (fca.context->tsame(s_DynamicContextOverrideUnsafe.get())) {
      if (Cfg::Repo::Authoritative) {
        raise_error("Cannot use dynamic_meth_caller_force() in repo-mode");
      }
      return cls;
    }
    return Class::load(fca.context);
  }();
  auto obj = liveClass() && vmfp()->hasThis() ? vmfp()->getThis() : nullptr;
  const Func* func;
  auto const callCtx = MemberLookupContext(ctx, vmfp()->func());
  auto const res = lookupClsMethod(func, cls, methName, obj, callCtx,
                                   MethodLookupErrorOptions::RaiseOnNotFound);
  assertx(func);

  if (res == LookupResult::MethodFoundNoThis) {
    if (!func->isStaticInPrologue()) {
      throw_missing_this(func);
    }
    obj = nullptr;
  } else {
    assertx(obj);
    assertx(res == LookupResult::MethodFoundWithThis);
  }

  if (func->hasReifiedGenerics() && !fca.hasGenerics() &&
      !func->getGenericsInfo().allGenericsSoft()) {
    throw_call_reified_func_without_generics(func);
  }

  if (obj) {
    return fcallImpl<false>(origpc, pc, fca, func, Object(obj), false);
  } else {
    if (forwarding && ctx) {
      /* Propagate the current late bound class if there is one, */
      /* otherwise use the class given by this instruction's input */
      if (vmfp()->hasThis()) {
        cls = vmfp()->getThis()->getVMClass();
      } else {
        cls = vmfp()->getClass();
      }
    }
    return fcallImpl<false>(origpc, pc, fca, func, cls, false);
  }
}

} // namespace

OPTBLD_INLINE JitResumeAddr
iopFCallClsMethodM(PC origpc, PC& pc, FCallArgs fca,
                   const StringData*,
                   const StringData* methName) {
  auto const cell = vmStack().topC();
  auto const cls = lookupClsRef(cell, jit::StrToClassKind::StaticMethod);
  vmStack().popC();
  assertx(cls && methName);
  return fcallClsMethodImpl(origpc, pc, fca, cls, methName, false);
}

OPTBLD_INLINE JitResumeAddr
iopFCallClsMethodD(PC origpc, PC& pc, FCallArgs fca,
                   Id classId, const StringData* methName) {
  const NamedTypePair &nep =
    vmfp()->func()->unit()->lookupNamedTypePairId(classId);
  Class* cls = Class::resolve(nep.second, nep.first, vmfp()->func());
  if (cls == nullptr) {
    raise_error(Strings::UNKNOWN_CLASS, nep.first->data());
  }
  return fcallClsMethodImpl(origpc, pc, fca, cls, methName, false);
}

OPTBLD_INLINE JitResumeAddr
iopFCallClsMethodSD(PC origpc, PC& pc, FCallArgs fca,
                    const StringData*, SpecialClsRef ref,
                    const StringData* methName) {
  auto const cls = specialClsRefToCls(ref);
  auto const fwd = ref == SpecialClsRef::SelfCls ||
                   ref == SpecialClsRef::ParentCls;
  return fcallClsMethodImpl(origpc, pc, fca, cls, methName, fwd);
}

namespace {

ObjectData* newObjImpl(Class* cls) {
  // Replace input with uninitialized instance.
  auto this_ = ObjectData::newInstance<true>(cls);
  TRACE(2, "NewObj: just new'ed an instance of class %s: %p\n",
        cls->name()->data(), this_);
  profileArrLikePropsForInterp(this_);
  return this_;
}

} // namespace

OPTBLD_INLINE void iopNewObj() {
  auto const clsCell = vmStack().topC();
  if (!isClassType(clsCell->m_type)) {
    raise_error("Attempting NewObj with non-class");
  }
  auto const cls = clsCell->m_data.pclass;

  callerDynamicConstructChecks(cls);
  auto this_ = newObjImpl(cls);
  vmStack().popC();
  vmStack().pushObjectNoRc(this_);
}

OPTBLD_INLINE void iopNewObjD(Id id) {
  const NamedTypePair &nep =
    vmfp()->func()->unit()->lookupNamedTypePairId(id);
  auto cls = Class::load(nep.second, nep.first);
  if (cls == nullptr) {
    raise_error(Strings::UNKNOWN_CLASS,
                vmfp()->func()->unit()->lookupLitstrId(id)->data());
  }
  auto this_ = newObjImpl(cls);
  vmStack().pushObjectNoRc(this_);
}

OPTBLD_INLINE void iopNewObjS(SpecialClsRef ref) {
  auto const cls = specialClsRefToCls(ref);
  if (ref == SpecialClsRef::LateBoundCls && cls->hasReifiedGenerics()) {
    raise_error(Strings::NEW_STATIC_ON_REIFIED_CLASS, cls->name()->data());
  }
  auto this_ = newObjImpl(cls);
  vmStack().pushObjectNoRc(this_);
}

OPTBLD_INLINE JitResumeAddr iopFCallCtor(PC origpc, PC& pc, FCallArgs fca,
                                         const StringData*) {
  assertx(fca.numRets == 1);
  assertx(fca.asyncEagerOffset == kInvalidOffset);
  assertx(tvIsObject(vmStack().indC(fca.numInputs() + (kNumActRecCells - 1))));
  auto const obj =
    vmStack().indC(fca.numInputs() + (kNumActRecCells - 1))->m_data.pobj;

  const Func* func;
  auto const ctx = arGetContextClass(vmfp());
  auto const callCtx = MemberLookupContext(ctx, vmfp()->func());
  auto const res UNUSED = lookupCtorMethod(func, obj->getVMClass(), callCtx,
                            MethodLookupErrorOptions::RaiseOnNotFound);
  assertx(res == LookupResult::MethodFoundWithThis);

  // fcallImpl() will do further checks before spilling the ActRec. If any
  // of these checks fail, make sure it gets decref'd only via ctx.
  tvWriteNull(*vmStack().indC(fca.numInputs() + (kNumActRecCells - 1)));
  return fcallImpl<false>(
    origpc, pc, fca, func, Object::attach(obj), true, true);
}

OPTBLD_INLINE void iopLockObj() {
  auto c1 = vmStack().topC();
  if (!tvIsObject(*c1)) raise_error("LockObj: expected an object");
  c1->m_data.pobj->lockObject();
}

OPTBLD_INLINE void iopIterBase() {
  TypedValue* base = vmStack().topC();
  if (LIKELY(isArrayLikeType(type(base)))) return;
  tvMove(Iter::extractBase(*base, arGetContextClass(vmfp())), base);
}

OPTBLD_INLINE void iopIterGetKey(const IterArgs& ita, tv_lval base) {
  if (isArrayLikeType(type(base))) {
    auto const arr = val(base).parr;
    auto const pos = frame_iter(vmfp(), ita.iterId)->getPos();
    auto const key = iter_select(iter_get_key_array, ita.flags)(arr, pos);
    tvDup(key, *vmStack().allocTV());
    return;
  }

  assertx(isObjectType(type(base)));
  auto const key = iter_get_key_object(val(base).pobj);
  tvCopy(key, *vmStack().allocTV());
}

OPTBLD_INLINE void iopIterGetValue(const IterArgs& ita, tv_lval base) {
  if (isArrayLikeType(type(base))) {
    auto const arr = val(base).parr;
    auto const pos = frame_iter(vmfp(), ita.iterId)->getPos();
    auto const value = iter_select(iter_get_value_array, ita.flags)(arr, pos);
    tvDup(value, *vmStack().allocTV());
    return;
  }

  assertx(isObjectType(type(base)));
  auto const value = iter_get_value_object(val(base).pobj);
  tvCopy(value, *vmStack().allocTV());
}

OPTBLD_INLINE void iopIterSetValue(const IterArgs& ita, tv_lval base) {
  assertx(isArrayLikeType(type(base)));
  assertx(!has_flag(ita.flags, IterArgs::Flags::BaseConst));

  auto it = frame_iter(vmfp(), ita.iterId);
  type(base) = dt_with_rc(type(base));
  val(base).parr = val(base).parr->setPosMove(it->getPos(), *vmStack().topC());
  vmStack().discard();
}

OPTBLD_INLINE void iopIterInit(PC& pc, const IterArgs& ita,
                               tv_lval base, PC targetpc) {
  assertx(isArrayLikeType(type(base)) || isObjectType(type(base)));
  auto it = frame_iter(vmfp(), ita.iterId);

  auto const more = isArrayLikeType(type(base))
    ? iter_select(iter_init_array, ita.flags)(it, val(base).parr)
    : iter_init_object(val(base).pobj);

  if (!more) pc = targetpc;
}

OPTBLD_INLINE void iopIterNext(PC& pc, const IterArgs& ita,
                               tv_lval base, PC targetpc) {
  assertx(isArrayLikeType(type(base)) || isObjectType(type(base)));
  auto it = frame_iter(vmfp(), ita.iterId);

  auto const more = isArrayLikeType(type(base))
    ? iter_select(iter_next_array, ita.flags)(it, val(base).parr)
    : iter_next_object(val(base).pobj);

  if (more) {
    vmpc() = targetpc;
    jmpSurpriseCheck(targetpc - pc);
    pc = targetpc;
  }
}

OPTBLD_INLINE void iopIterFree(Iter* it) {
  it->kill();
}

OPTBLD_INLINE void inclOp(InclOpFlags flags, const char* opName) {
  TypedValue* c1 = vmStack().topC();
  auto path = OptString::attach(prepareKey(*c1));
  bool initial;
  TRACE(2, "inclOp %s %s %s %s \"%s\"\n",
        flags & InclOpFlags::Once ? "Once" : "",
        flags & InclOpFlags::DocRoot ? "DocRoot" : "",
        flags & InclOpFlags::Relative ? "Relative" : "",
        flags & InclOpFlags::Fatal ? "Fatal" : "",
        path.data());

  auto curUnitFilePath = [&] {
    std::filesystem::path currentUnit(
      vmfp()->func()->unit()->filepath()->data()
    );
    return currentUnit.parent_path().string();
  };

  auto const unit = [&] {
    if (flags & InclOpFlags::Relative) {
      OptString absPath = curUnitFilePath() + '/';
      absPath += path;
      return lookupUnit(absPath.get(), "", &initial, nullptr, false);
    }
    if (flags & InclOpFlags::DocRoot) {
      return lookupUnit(
        SourceRootInfo::RelativeToPhpRoot(path).get(), "", &initial,
        nullptr, false);
    }
    return lookupUnit(path.get(), curUnitFilePath().c_str(), &initial,
                      nullptr, false);
  }();

  if (unit == nullptr) {
    if (flags & InclOpFlags::Fatal) {
      raise_error("%s(%s): File not found", opName, path.data());
    } else {
      raise_warning("%s(%s): File not found", opName, path.data());
    }
    vmStack().popC();
    vmStack().pushBool(false);
    return;
  }

  if (!(flags & InclOpFlags::Once) || initial) {
    unit->merge();
  }
  vmStack().popC();
  vmStack().pushBool(true);
}

OPTBLD_INLINE void iopIncl() {
  inclOp(InclOpFlags::Default, "include");
}

OPTBLD_INLINE void iopInclOnce() {
  inclOp(InclOpFlags::Once, "include_once");
}

OPTBLD_INLINE void iopReq() {
  inclOp(InclOpFlags::Fatal, "require");
}

OPTBLD_INLINE void iopReqOnce() {
  inclOp(InclOpFlags::Fatal | InclOpFlags::Once, "require_once");
}

OPTBLD_INLINE void iopReqDoc() {
  inclOp(
    InclOpFlags::Fatal | InclOpFlags::Once | InclOpFlags::DocRoot,
    "require_once"
  );
}

OPTBLD_INLINE void iopEval() {
  TypedValue* c1 = vmStack().topC();

  if (UNLIKELY(Cfg::Eval::AuthoritativeMode)) {
    // Ahead of time whole program optimizations need to assume it can
    // see all the code, or it really can't do much.
    raise_error("You can't use eval in RepoAuthoritative mode");
  }

  if (UNLIKELY(Cfg::Eval::DisableEval)) {
    // Fatal when program attempts to use eval() when disabled.
    raise_error("You can't use eval when disabled");
  }

  auto code = OptString::attach(prepareKey(*c1));
  OptString prefixedCode = concat("<?hh ", code);

  auto evalFilename = std::string();
  auto vm = &*g_context;
  string_printf(
    evalFilename,
    "%s(%d)(%s" EVAL_FILENAME_SUFFIX,
    vm->getContainingFileName()->data(),
    vm->getLine(),
    string_md5(code.slice()).c_str()
  );
  auto unit = compileEvalString(prefixedCode.get(), evalFilename.c_str());
  if (!Cfg::Jit::EvaledCode) {
    unit->setInterpretOnly();
  }

  vmStack().popC();
  if (auto const info = unit->getFatalInfo()) {
    auto const errnum = static_cast<int>(ErrorMode::WARNING);
    if (vm->errorNeedsLogging(errnum)) {
      // manual call to Logger instead of logError as we need to use
      // evalFileName and line as the exception doesn't track the eval()
      Logger::Error(
        "\nFatal error: %s in %s on line %d",
        info->m_fatalMsg.c_str(),
        evalFilename.c_str(),
        info->m_fatalLoc.line1
      );
    }

    vmStack().pushBool(false);
    return;
  }
  unit->merge();
  vmStack().pushBool(true);
}

OPTBLD_INLINE void iopThis() {
  checkThis(vmfp());
  ObjectData* this_ = vmfp()->getThis();
  vmStack().pushObject(this_);
}

OPTBLD_INLINE void iopBareThis(BareThisOp bto) {
  if (vmfp()->func()->cls() && vmfp()->hasThis()) {
    ObjectData* this_ = vmfp()->getThis();
    vmStack().pushObject(this_);
  } else {
    vmStack().pushNull();
    switch (bto) {
    case BareThisOp::Notice:   raise_notice(Strings::WARN_NULL_THIS); break;
    case BareThisOp::NoNotice: break;
    case BareThisOp::NeverNull:
      assertx(false && "$this cannot be null in BareThis with NeverNull option");
      break;
    }
  }
}

OPTBLD_INLINE void iopCheckThis() {
  checkThis(vmfp());
}

OPTBLD_INLINE void iopChainFaults() {
  auto const current = *vmStack().indC(1);
  auto const prev = *vmStack().indC(0);
  if (!isObjectType(current.m_type) ||
      !current.m_data.pobj->instanceof(SystemLib::getThrowableClass()) ||
      !isObjectType(prev.m_type) ||
      !prev.m_data.pobj->instanceof(SystemLib::getThrowableClass())) {
    raise_error(
      "Inputs to ChainFault must be objects that implement Throwable"
    );
  }

  // chainFaultObjects takes ownership of a reference to prev.
  vmStack().discard();
  chainFaultObjects(current.m_data.pobj, prev.m_data.pobj);
}

OPTBLD_INLINE void iopLateBoundCls() {
  auto const cls = frameStaticClass(vmfp());
  if (!cls) raise_error(HPHP::Strings::CANT_ACCESS_STATIC);
  vmStack().pushClass(cls);
}

OPTBLD_INLINE void iopVerifyParamType(local_var param) {
  verifyParamType(
    vmfp()->func(),
    param.index,
    vmStack().topTV(),
    [] { return frameStaticClass(vmfp()); }
  );
}

OPTBLD_INLINE void iopVerifyParamTypeTS(local_var param) {
  auto const cell = vmStack().topC();
  assertx(tvIsDict(cell));
  auto isTypeVar = tcCouldBeReified(vmfp()->func(), param.index);
  bool warn = false;
  if ((isTypeVar || tvIsObject(param.lval)) &&
      !verifyReifiedLocalType(
        param.lval, cell->m_data.parr, frameStaticClass(vmfp()), vmfp()->func(),
        isTypeVar, warn)) {
    raise_reified_typehint_error(
      fmt::format(
        "Argument {} passed to {}() must be an instance of {}, {} given",
        param.index + 1,
        vmfp()->func()->fullName()->data(),
        TypeStructure::toString(ArrNR(cell->m_data.parr),
          TypeStructure::TSDisplayType::TSDisplayTypeUser).c_str(),
        describe_actual_type(param.lval)
      ), warn
    );
  }
  vmStack().popC();
}

OPTBLD_INLINE void iopVerifyOutType(local_var param) {
  auto const func = vmfp()->func();
  auto const paramId = param.index;
  assertx(paramId < func->numParams());
  assertx(func->numParams() == int(func->params().size()));
  for (auto const& tc : func->params()[paramId].typeConstraints.range()) {
    if (tc.isCheckable()) {
      auto const ctx = tc.isThis() ? frameStaticClass(vmfp()) : nullptr;
      tc.verifyOutParam(vmStack().topTV(), ctx, func, paramId);
    }
  }
}

OPTBLD_INLINE void iopVerifyRetTypeTS() {
  auto const ts = vmStack().topC();
  assertx(tvIsDict(ts));
  auto const cell = vmStack().indC(1);
  bool isTypeVar = tcCouldBeReified(vmfp()->func(), TypeConstraint::ReturnId);
  bool warn = false;
  if ((isTypeVar || tvIsObject(cell)) &&
      !verifyReifiedLocalType(
        cell, ts->m_data.parr, frameStaticClass(vmfp()), vmfp()->func(),
        isTypeVar, warn)) {
    raise_reified_typehint_error(
      fmt::format(
        "Value returned from function {}() must be of type {}, {} given",
        vmfp()->func()->fullName()->data(),
        TypeStructure::toString(ArrNR(ts->m_data.parr),
          TypeStructure::TSDisplayType::TSDisplayTypeUser).c_str(),
        describe_actual_type(cell)
      ), warn
    );
  }
  vmStack().popC();
}

// Verify that the type of an expression matches a type structure
OPTBLD_INLINE void iopVerifyTypeTS() {
  auto const cell = vmStack().indC(1);
  if (folly::Random::oneIn(Cfg::Eval::CheckedUnsafeCastSampleRate, threadLocalRng64())) {
    assertx(Cfg::Eval::CheckedUnsafeCast);
    auto const ts = maybeResolveAndErrorOnTypeStructure(TypeStructResolveOp::Resolve, true, false);
    std::string givenType, expectedType, errorKey;
    if (!checkForVerifyTypeStructureMatchesTV(ts, *cell, givenType, expectedType, errorKey)) {
      raise_inline_typehint_error(givenType, expectedType, errorKey);
    }
  }
  vmStack().popC();
}

OPTBLD_INLINE JitResumeAddr iopNativeImpl(PC& pc) {
  auto const fp = vmfp();
  auto const func = vmfp()->func();
  auto const sfp = fp->sfp();
  auto const jitReturn = jitReturnPre(fp);
  auto const native = func->arFuncPtr();
  assertx(native != nullptr);
  // Actually call the native implementation. This will handle freeing the
  // locals in the normal case. In the case of an exception, the VM unwinder
  // will take care of it.
  native(fp);

  // Adjust the stack; the native implementation put the return value in the
  // right place for us already
  vmStack().ndiscard(func->numSlotsInFrame());
  vmStack().ret();

  // Return control to the caller.
  returnToCaller(pc, sfp, jitReturn.callOff);
  return jitReturnPost(jitReturn);
}

OPTBLD_INLINE void iopSelfCls() {
  auto const clss = arGetContextClass(vmfp());
  if (!clss) raise_error(HPHP::Strings::CANT_ACCESS_SELF);
  vmStack().pushClass(clss);
}

OPTBLD_INLINE void iopParentCls() {
  auto const clss = arGetContextClass(vmfp());
  if (!clss) raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_CLASS);
  auto const parent = clss->parent();
  if (!parent) raise_error(HPHP::Strings::CANT_ACCESS_PARENT_WHEN_NO_PARENT);
  vmStack().pushClass(parent);
}

OPTBLD_INLINE void iopCreateCl(uint32_t numArgs, const StringData* name) {
  auto const func = vmfp()->func();
  auto const preCls = func->unit()->lookupPreClass(name);
  assertx(preCls);
  assertx(preCls->attrs() & AttrIsClosureClass);
  auto const c = Class::defClosure(preCls, true);

  auto const cls = c->rescope(const_cast<Class*>(func->cls()));
  assertx(!cls->needInitialization());
  // In repo mode, rescoping should always use the template class
  // (except if we're in a trait).
  assertx(
    IMPLIES(
      Cfg::Repo::Authoritative &&
        !(func->preClass() && func->preClass()->attrs() & AttrTrait),
      cls == c
    )
  );

  auto obj = Cfg::Repo::Authoritative
    ? createClosureRepoAuth(cls) : createClosure(cls);
  c_Closure::fromObject(obj)->init(numArgs, vmfp(), vmStack().top());
  vmStack().ndiscard(numArgs);
  vmStack().pushObjectNoRc(obj);
}

static inline BaseGenerator* this_base_generator(const ActRec* fp) {
  auto const obj = fp->getThis();
  assertx(obj->getVMClass() == AsyncGenerator::classof() ||
         obj->getVMClass() == Generator::classof());
  return obj->getVMClass() == Generator::classof()
    ? static_cast<BaseGenerator*>(Generator::fromObject(obj))
    : static_cast<BaseGenerator*>(AsyncGenerator::fromObject(obj));
}

static inline Generator* this_generator(const ActRec* fp) {
  auto const obj = fp->getThis();
  return Generator::fromObject(obj);
}

const StaticString s_this("this");

OPTBLD_INLINE JitResumeAddr iopCreateCont(PC origpc, PC& pc) {
  auto const jitReturn = jitReturnPre(vmfp());

  auto const fp = vmfp();
  auto const func = fp->func();
  auto const numSlots = func->numSlotsInFrame();
  auto const suspendOffset = func->offsetOf(origpc);
  assertx(!isResumed(fp));
  assertx(func->isGenerator());

  // Create the {Async,}Generator object. Create takes care of copying local
  // variables and iterators.
  auto const obj = func->isAsync()
    ? AsyncGenerator::Create(fp, numSlots, nullptr, suspendOffset)
    : Generator::Create(fp, numSlots, nullptr, suspendOffset);

  auto const genData = func->isAsync() ?
    static_cast<BaseGenerator*>(AsyncGenerator::fromObject(obj)) :
    static_cast<BaseGenerator*>(Generator::fromObject(obj));

  EventHook::FunctionSuspendCreateCont(
    fp,
    genData->actRec(),
    EventHook::Source::Interpreter
  );

  // Grab caller info from ActRec.
  ActRec* sfp = fp->sfp();
  Offset callOff = fp->callOffset();

  // Free ActRec and store the return value.
  vmStack().ndiscard(numSlots);
  vmStack().ret();
  tvCopy(make_tv<KindOfObject>(obj), *vmStack().topTV());
  assertx(vmStack().topTV() == fp->retSlot());

  // Return control to the caller.
  returnToCaller(pc, sfp, callOff);

  return jitReturnPost(jitReturn);
}

OPTBLD_INLINE JitResumeAddr contEnterImpl(PC origpc) {
  // The stack must have one cell! Or else resumableStackBase() won't work!
  assertx(vmStack().top() + 1 ==
         (TypedValue*)vmfp() - vmfp()->func()->numSlotsInFrame());

  auto const gen = this_base_generator(vmfp());
  auto const genAR = gen->actRec();

  // Stack overflow check.
  checkStack(vmStack(), genAR->func(), 0);

  // Point of no return, set the generator state to Running.
  assertx(!gen->isRunning() && !gen->isDone());
  gen->setState(BaseGenerator::State::Running);

  // Set up previous FP and return address.
  auto const retHelper = genAR->func()->isAsync()
    ? jit::tc::ustubs().asyncGenRetHelper
    : jit::tc::ustubs().genRetHelper;
  genAR->setReturn(vmfp(), origpc, retHelper, false);

  // Enter the generator frame.
  vmfp() = genAR;
  vmpc() = genAR->func()->at(gen->resumable()->resumeFromYieldOffset());

  EventHook::FunctionResumeYield(vmfp(), EventHook::Source::Interpreter);

  return JitResumeAddr::trans(gen->resumable()->resumeAddr());
}

OPTBLD_INLINE JitResumeAddr iopContEnter(PC origpc, PC& pc) {
  auto const retAddr = contEnterImpl(origpc);
  pc = vmpc();
  return retAddr;
}

OPTBLD_INLINE void iopContRaise(PC origpc, PC& pc) {
  contEnterImpl(origpc);
  pc = vmpc();
  iopThrow(pc);
}

OPTBLD_INLINE JitResumeAddr yield(PC origpc, PC& pc, const TypedValue* key,
                                  const TypedValue value) {
  auto const jitReturn = jitReturnPre(vmfp());

  auto const fp = vmfp();
  auto const func = fp->func();
  auto const suspendOffset = func->offsetOf(origpc);
  assertx(isResumed(fp));
  assertx(func->isGenerator());

  EventHook::FunctionSuspendYield(fp, EventHook::Source::Interpreter);

  auto const sfp = fp->sfp();
  auto const callOff = fp->callOffset();

  if (!func->isAsync()) {
    // Non-async generator.
    assertx(fp->sfp());
    frame_generator(fp)->yield(suspendOffset, key, value);

    // Push return value of next()/send()/raise().
    vmStack().pushNull();
  } else {
    // Async generator.
    auto const gen = frame_async_generator(fp);
    auto const eagerResult = gen->yield(suspendOffset, key, value);
    if (eagerResult) {
      // Eager execution => return StaticWaitHandle.
      assertx(sfp);
      vmStack().pushObjectNoRc(eagerResult);
    } else {
      // Resumed execution => return control to the scheduler.
      assertx(!sfp);
    }
  }

  returnToCaller(pc, sfp, callOff);

  return jitReturnPost(jitReturn);
}

OPTBLD_INLINE JitResumeAddr iopYield(PC origpc, PC& pc) {
  auto const value = *vmStack().topC();
  vmStack().discard();
  return yield(origpc, pc, nullptr, value);
}

OPTBLD_INLINE JitResumeAddr iopYieldK(PC origpc, PC& pc) {
  auto const key = *vmStack().indC(1);
  auto const value = *vmStack().topC();
  vmStack().ndiscard(2);
  return yield(origpc, pc, &key, value);
}

OPTBLD_INLINE void iopContCheck(ContCheckOp subop) {
  this_base_generator(vmfp())->checkNext(subop == ContCheckOp::CheckStarted);
}

OPTBLD_INLINE void iopContValid() {
  vmStack().pushBool(
    this_generator(vmfp())->getState() != BaseGenerator::State::Done);
}

OPTBLD_INLINE void iopContKey() {
  Generator* cont = this_generator(vmfp());
  cont->startedCheck();
  tvDup(cont->m_key, *vmStack().allocC());
}

OPTBLD_INLINE void iopContCurrent() {
  Generator* cont = this_generator(vmfp());
  cont->startedCheck();
  tvDup(cont->m_value, *vmStack().allocC());
}

template <bool lowPri>
OPTBLD_INLINE void asyncSuspendE(PC origpc, PC& pc) {
  auto const fp = vmfp();
  auto const func = fp->func();
  auto const numSlots = func->numSlotsInFrame();
  auto const suspendOffset = func->offsetOf(origpc);
  assertx(func->isAsync());
  assertx(resumeModeFromActRec(fp) != ResumeMode::Async);

  // Pop the dependency we are blocked on unless we are in low-pri mode.
  c_WaitableWaitHandle* child = nullptr;
  if constexpr (!lowPri) {
    child = wait_handle<c_WaitableWaitHandle>(*vmStack().topC());
    assertx(!child->isFinished());
    vmStack().discard();
  }

  if (!func->isGenerator()) {  // Async function.
    assertx(!func->isEagerOnly());
    // Create the AsyncFunctionWaitHandle object. Create takes care of
    // copying local variables and iterators.
    auto waitHandle = c_AsyncFunctionWaitHandle::Create<lowPri>(
      fp, numSlots, nullptr, suspendOffset, child);

    // Call the suspend hook. It will decref the newly allocated waitHandle
    // if it throws.
    EventHook::FunctionSuspendAwaitEF(
      fp,
      waitHandle->actRec(),
      EventHook::Source::Interpreter
    );

    // Grab caller info from ActRec.
    ActRec* sfp = fp->sfp();
    Offset callOff = fp->callOffset();

    // Free ActRec and store the return value. In case async eager return was
    // requested by the caller, let it know that we did not finish eagerly.
    vmStack().ndiscard(numSlots);
    vmStack().ret();
    tvCopy(make_tv<KindOfObject>(waitHandle), *vmStack().topTV());
    vmStack().topTV()->m_aux.u_asyncEagerReturnFlag = 0;
    assertx(vmStack().topTV() == fp->retSlot());

    // Return control to the caller.
    returnToCaller(pc, sfp, callOff);
  } else {  // Async generator.
    assertx(!lowPri);
    // Create new AsyncGeneratorWaitHandle.
    auto waitHandle = c_AsyncGeneratorWaitHandle::Create(
      fp, nullptr, suspendOffset, child);

    // Call the suspend hook. It will decref the newly allocated waitHandle
    // if it throws.
    EventHook::FunctionSuspendAwaitEG(fp, EventHook::Source::Interpreter);

    // Store the return value.
    vmStack().pushObjectNoRc(waitHandle);

    // Return control to the caller (AG::next()).
    assertx(fp->sfp());
    returnToCaller(pc, fp->sfp(), fp->callOffset());
  }
}

OPTBLD_INLINE void asyncSuspendR(PC origpc, PC& pc) {
  auto const fp = vmfp();
  auto const func = fp->func();
  auto const suspendOffset = func->offsetOf(origpc);
  assertx(!fp->sfp());
  assertx(func->isAsync());
  assertx(resumeModeFromActRec(fp) == ResumeMode::Async);

  // Pop the dependency we are blocked on.
  auto child = req::ptr<c_WaitableWaitHandle>::attach(
    wait_handle<c_WaitableWaitHandle>(*vmStack().topC()));
  assertx(!child->isFinished());
  vmStack().discard();

  // Before adjusting the stack or doing anything, check the suspend hook.
  // This can throw.
  EventHook::FunctionSuspendAwaitR(
    fp,
    child.get(),
    EventHook::Source::Interpreter
  );

  // Await child and suspend the async function/generator. May throw.
  if (!func->isGenerator()) {  // Async function.
    assertx(!func->isEagerOnly());
    frame_afwh(fp)->await(suspendOffset, std::move(child));
  } else {  // Async generator.
    auto const gen = frame_async_generator(fp);
    gen->resumable()->setResumeAddr(nullptr, suspendOffset);
    gen->getWaitHandle()->await(std::move(child));
  }

  // Return control to the scheduler.
  pc = nullptr;
  vmfp() = nullptr;
}

namespace {

JitResumeAddr suspendStack(PC origpc, PC &pc) {
  auto const jitReturn = jitReturnPre(vmfp());
  if (resumeModeFromActRec(vmfp()) == ResumeMode::Async) {
    // suspend resumed execution
    asyncSuspendR(origpc, pc);
  } else {
    // suspend eager execution
    asyncSuspendE<false>(origpc, pc);
  }
  return jitReturnPost(jitReturn);
}

}

OPTBLD_INLINE JitResumeAddr iopAwait(PC origpc, PC& pc) {
  auto const awaitable = vmStack().topC();
  auto wh = c_Awaitable::fromTV(*awaitable);
  if (UNLIKELY(wh == nullptr)) {
    SystemLib::throwBadMethodCallExceptionObject("Await on a non-Awaitable");
  }
  if (LIKELY(wh->isFailed())) {
    throw req::root<Object>{wh->getException()};
  }
  if (wh->isSucceeded()) {
    tvSet(wh->getResult(), *vmStack().topC());
    return JitResumeAddr::none();
  }
  return suspendStack(origpc, pc);
}

OPTBLD_INLINE JitResumeAddr iopAwaitLowPri(PC origpc, PC& pc) {
  if (!Cfg::Eval::EnableLowPriorityAwaitables) {
    vmStack().pushNull();
    return JitResumeAddr::none();
  }
  auto const jitReturn = jitReturnPre(vmfp());
  asyncSuspendE<true>(origpc, pc);
  return jitReturnPost(jitReturn);
}

OPTBLD_INLINE JitResumeAddr iopAwaitAll(PC origpc, PC& pc, LocalRange locals) {
  uint32_t cnt = 0;
  for (auto i = locals.first; i < locals.first + locals.count; ++i) {
    auto const local = *frame_local(vmfp(), i);
    if (tvIsNull(local)) continue;
    auto const awaitable = c_Awaitable::fromTV(local);
    if (UNLIKELY(awaitable == nullptr)) {
      SystemLib::throwBadMethodCallExceptionObject("Await on a non-Awaitable");
    }
    if (!awaitable->isFinished()) {
      ++cnt;
    }
  }

  if (!cnt) {
    vmStack().pushNull();
    return JitResumeAddr::none();
  }

  auto obj = Object::attach(
    c_ConcurrentWaitHandle::fromFrameNoCheck(vmfp(), locals.first,
                                             locals.first + locals.count, cnt)
  );
  assertx(obj->isWaitHandle());
  if (UNLIKELY(static_cast<c_Awaitable*>(obj.get())->isFinished())) {
    // A profiler hook may have finished the CCWH.
    vmStack().pushNull();
    return JitResumeAddr::none();
  }

  vmStack().pushObjectNoRc(obj.detach());
  return suspendStack(origpc, pc);
}

OPTBLD_INLINE void iopWHResult() {
  // we should never emit this bytecode for non-waithandle
  auto const wh = c_Awaitable::fromTV(*vmStack().topC());
  if (UNLIKELY(!wh)) {
    raise_error("WHResult input was not a subclass of Awaitable");
  }

  // the failure condition is likely since we punt to this opcode
  // in the JIT when the state is failed.
  if (wh->isFailed()) {
    throw_object(Object{wh->getException()});
  }
  if (wh->isSucceeded()) {
    tvSet(wh->getResult(), *vmStack().topC());
    return;
  }
  SystemLib::throwInvalidOperationExceptionObject(
    "Request for result on pending wait handle, "
    "must await or join() before calling result()");
  not_reached();
}

OPTBLD_INLINE void iopSetImplicitContextByValue() {
  assertx(*ImplicitContext::activeCtx);
  std::swap((*vmStack().topC()).m_data.pobj, (*ImplicitContext::activeCtx));
}

OPTBLD_INLINE void iopGetMemoAgnosticImplicitContext() {
  assertx(*ImplicitContext::activeCtx);
  auto context = Native::data<ImplicitContext>(*ImplicitContext::activeCtx);
  vmStack().pushObject(context->m_memoAgnosticIC);
}

OPTBLD_INLINE void iopGetWholeImplicitContext() {
  assertx(*ImplicitContext::activeCtx);
  vmStack().pushObject(*ImplicitContext::activeCtx);
}

OPTBLD_INLINE void iopCheckProp(const StringData* propName) {
  auto* cls = vmfp()->getClass();
  auto* propVec = cls->getPropData();
  always_assert(propVec);

  auto* ctx = arGetContextClass(vmfp());
  auto slot = ctx->lookupDeclProp(propName);
  auto index = cls->propSlotToIndex(slot);

  auto const val = (*propVec)[index].val;
  vmStack().pushBool(type(val) != KindOfUninit);
}

OPTBLD_INLINE void iopInitProp(const StringData* propName, InitPropOp propOp) {
  auto* cls = vmfp()->getClass();

  auto* ctx = arGetContextClass(vmfp());
  auto* fr = vmStack().topC();

  auto lval = [&] () -> tv_lval {
    switch (propOp) {
      case InitPropOp::Static: {
        auto const slot = ctx->lookupSProp(propName);
        assertx(slot != kInvalidSlot);
        auto ret = cls->getSPropData(slot);
        if (Cfg::Eval::CheckPropTypeHints > 0) {
          auto const& sprop = cls->staticProperties()[slot];
          sprop.typeConstraints.verifyStaticProperty(
            fr, cls, sprop.cls, sprop.name
          );
        }
        return ret;
      }

      case InitPropOp::NonStatic: {
        auto* propVec = cls->getPropData();
        always_assert(propVec);
        auto const slot = ctx->lookupDeclProp(propName);
        auto const index = cls->propSlotToIndex(slot);
        assertx(slot != kInvalidSlot);
        auto ret = (*propVec)[index].val;
        if (Cfg::Eval::CheckPropTypeHints > 0) {
          auto const& prop = cls->declProperties()[slot];
          prop.typeConstraints.verifyProperty(fr, cls, prop.cls, prop.name);
        }
        return ret;
      }
    }
    always_assert(false);
  }();

  tvDup(*fr, lval);
  vmStack().popC();
}

OPTBLD_INLINE void iopOODeclExists(OODeclExistsOp subop) {
  TypedValue* aloadTV = vmStack().topTV();
  if (aloadTV->m_type != KindOfBoolean) {
    raise_error("OODeclExists: Expected Bool on top of stack, got %s",
          tname(aloadTV->m_type).c_str());
  }

  bool autoload = aloadTV->m_data.num;
  vmStack().popX();

  TypedValue* name = vmStack().topTV();
  if (!isStringType(name->m_type)) {
    raise_error("OODeclExists: Expected String on stack, got %s",
          tname(aloadTV->m_type).c_str());
  }

  ClassKind kind;
  switch (subop) {
    case OODeclExistsOp::Class : kind = ClassKind::Class; break;
    case OODeclExistsOp::Trait : kind = ClassKind::Trait; break;
    case OODeclExistsOp::Interface : kind = ClassKind::Interface; break;
  }
  tvAsVariant(name) = Class::exists(name->m_data.pstr, autoload, kind);
}

std::string prettyStack(const std::string& prefix) {
  if (!vmfp()) return "__Halted";
  int offset = (vmfp()->func()->unit() != nullptr)
               ? pcOff() : 0;
  auto begPrefix = prefix + "__";
  auto midPrefix = prefix + "|| ";
  auto endPrefix = prefix + "\\/";
  auto stack = vmStack().toString(vmfp(), offset, midPrefix);
  return begPrefix + "\n" + stack + endPrefix;
}

// callable from gdb
void DumpStack() {
  fprintf(stderr, "%s\n", prettyStack("").c_str());
}

// callable from gdb
void DumpCurUnit(int skip) {
  ActRec* fp = vmfp();
  Offset pc = fp->func()->unit() ? pcOff() : 0;
  while (skip--) {
    fp = g_context->getPrevVMState(fp, &pc);
  }
  if (fp == nullptr) {
    std::cout << "Don't have a valid fp\n";
    return;
  }

  printf("Offset = %d, in function %s\n", pc, fp->func()->name()->data());
  Unit* u = fp->func()->unit();
  if (u == nullptr) {
    std::cout << "Current unit is NULL\n";
    return;
  }
  printf("Dumping bytecode for %s(%p)\n", u->filepath()->data(), u);
  std::cout << u->toString();
}

// callable from gdb
void PrintTCCallerInfo() {
  VMRegAnchor _;

  auto const f = vmfp()->func();
  auto const u = f->unit();
  auto const rip = []() -> jit::TCA {
    DECLARE_FRAME_POINTER(reg_fp);
    // NB: We can't directly mutate the register-mapped `reg_fp'.
    for (ActRec* fp = reg_fp; fp; fp = fp->m_sfp) {
      auto const rip = jit::TCA(fp->m_savedRip);
      if (jit::tc::isValidCodeAddress(rip)) return rip;
    }
    return nullptr;
  }();

  fprintf(stderr, "Called from TC address %p\n", rip);
  std::cerr << u->filepath()->data() << ':'
            << f->getLineNumber(f->offsetOf(vmpc())) << '\n';
}

// thread-local cached coverage info
static __thread Unit* s_prev_unit;
static __thread int s_prev_line;

void recordCodeCoverage(PC pc) {
  if (!CodeCoverage::isCoverable(peek_op(pc))) return;

  auto const func = vmfp()->func();

  auto shouldRecord = [&]() {
    // If there is any Default value initilizer we don't want to record
    // if we are running that code
    for (auto const& param : func->params()) {
      if (param.funcletOff != kInvalidOffset) {
        return pcOff() < param.funcletOff;
      }
    }

    return true;
  };

  Unit* unit = func->unit();
  assertx(unit != nullptr);
  if (RI().m_coverage.m_should_use_per_file_coverage) {
    if (unit->isCoverageEnabled() && shouldRecord()) {
      unit->recordCoverage(func->getLineNumber(pcOff()));
    }
    return;
  }

  if (!shouldRecord()) return;

  int line = func->getLineNumber(pcOff());
  assertx(line != -1);
  if (unit != s_prev_unit || line != s_prev_line) {
    s_prev_unit = unit;
    s_prev_line = line;
    const StringData* filepath = unit->filepath();
    assertx(filepath->isStatic());
    RI().m_coverage.Record(filepath->data(), line);
  }
}

void resetCoverageCounters() {
  s_prev_line = -1;
  s_prev_unit = nullptr;
}

static inline void
condStackTraceSep(Op opcode) {
  TRACE(3, "%s "
        "========================================"
        "========================================\n",
        opcodeToName(opcode));
}

#define COND_STACKTRACE(pfx)\
  ONTRACE(3, auto stack = prettyStack(pfx);\
          Trace::trace("%s\n", stack.c_str());)

namespace {

/*
 * iopWrapReturn() calls a function pointer and forwards its return value if it
 * returns JitResumeAddr, or JitResumeAddr::none() if returns void.
 * Some opcodes need the original PC by value, and some do not. We have wrappers
 * for both flavors. Some opcodes (FCall*) may want to return to the JIT in the
 * middle of an instruction. If their implementation returns JitResumeAddr,
 * we are guaranteed to use the returned JitResumeAddr to return to the JIT and
 * so it is safe to return in the middle of an instruction.
 */
template<typename... Params, typename... Args>
OPTBLD_INLINE JitResumeAddr
iopWrapReturn(void(fn)(Params...), PC, Args&&... args) {
  fn(std::forward<Args>(args)...);
  return JitResumeAddr::none();
}

template<typename... Params, typename... Args>
OPTBLD_INLINE JitResumeAddr
iopWrapReturn(JitResumeAddr(fn)(Params...), PC, Args&&... args) {
  return fn(std::forward<Args>(args)...);
}

template<typename... Params, typename... Args>
OPTBLD_INLINE JitResumeAddr
iopWrapReturn(void(fn)(PC, Params...), PC origpc, Args&&... args) {
  fn(origpc, std::forward<Args>(args)...);
  return JitResumeAddr::none();
}

template<typename... Params, typename... Args>
OPTBLD_INLINE JitResumeAddr
iopWrapReturn(JitResumeAddr(fn)(PC, Params...), PC origpc, Args&&... args) {
  return fn(origpc, std::forward<Args>(args)...);
}

/*
 * Some bytecodes with SA immediates want the raw Id to look up a NamedType
 * or NamedFunc quickly, and some want the const StringData*. Support both by
 * decoding to this struct and implicitly converting to what the callee wants.
 */
struct litstr_id {
  /* implicit */ ALWAYS_INLINE operator const StringData*() const {
    return liveUnit()->lookupLitstrId(id);
  }
  /* implicit */ ALWAYS_INLINE operator Id() const {
    return id;
  }

  Id id{kInvalidId};
};

/*
 * These macros are used to generate wrapper functions for the iop*() functions
 * defined earlier in this file. iopWrapFoo() decodes immediates from the
 * bytecode stream according to the signature of Foo (in hhbc.h), then calls
 * iopFoo() with those decoded arguments.
 */
#define FLAG_NF
#define FLAG_TF
#define FLAG_CF , pc
#define FLAG_CF_TF FLAG_CF

#define DECODE_IVA decode_iva(pc)
#define DECODE_I64A decode<int64_t>(pc)
#define DECODE_LA decode_local(pc)
#define DECODE_NLA decode_named_local_var(pc)
#define DECODE_ILA decode_indexed_local(pc)
#define DECODE_IA decode_iter(pc)
#define DECODE_DA decode<double>(pc)
#define DECODE_SA decode<litstr_id>(pc)
#define DECODE_AA decode_litarr(pc)
#define DECODE_RATA decode_rat(pc)
#define DECODE_BA origpc + decode_ba(pc)
#define DECODE_OA(ty) decode<ty>(pc)
#define DECODE_KA decode_member_key(pc, liveUnit())
#define DECODE_LAR decodeLocalRange(pc)
#define DECODE_ITA decodeIterArgs(pc)
#define DECODE_FCA decodeFCallArgs(op, pc, liveUnit())
#define DECODE_BLA decode_imm_array<Offset>(pc)
#define DECODE_SLA decode_imm_array<StrVecItem>(pc)
#define DECODE_VSA decode_imm_array<Id>(pc)

#define DECODE_NA
#define DECODE_ONE(a) auto const imm1 = DECODE_##a;
#define DECODE_TWO(a, b) DECODE_ONE(a) auto const imm2 = DECODE_##b;
#define DECODE_THREE(a, b, c) DECODE_TWO(a, b) auto const imm3 = DECODE_##c;
#define DECODE_FOUR(a, b, c, d) \
  DECODE_THREE(a, b, c) auto const imm4 = DECODE_##d;
#define DECODE_FIVE(a, b, c, d, e) \
  DECODE_FOUR(a, b, c, d) auto const imm5 = DECODE_##e;
#define DECODE_SIX(a, b, c, d, e, f) \
  DECODE_FIVE(a, b, c, d, e) auto const imm6 = DECODE_##f;

#define PASS_NA
#define PASS_ONE(...) , imm1
#define PASS_TWO(...) , imm1, imm2
#define PASS_THREE(...) , imm1, imm2, imm3
#define PASS_FOUR(...) , imm1, imm2, imm3, imm4
#define PASS_FIVE(...) , imm1, imm2, imm3, imm4, imm5
#define PASS_SIX(...) , imm1, imm2, imm3, imm4, imm5, imm6

#define O(name, imm, in, out, flags)                                 \
  OPTBLD_INLINE JitResumeAddr iopWrap##name(PC& pc) {                \
    UNUSED auto constexpr op = Op::name;                             \
    UNUSED auto const origpc = pc - encoded_op_size(op);             \
    DECODE_##imm                                                     \
    return iopWrapReturn(                                            \
      iop##name, origpc FLAG_##flags PASS_##imm);                    \
  }
OPCODES

#undef FLAG_NF
#undef FLAG_TF
#undef FLAG_CF
#undef FLAG_CF_TF

#undef DECODE_IVA
#undef DECODE_I64A
#undef DECODE_LA
#undef DECODE_NLA
#undef DECODE_ILA
#undef DECODE_IA
#undef DECODE_DA
#undef DECODE_SA
#undef DECODE_AA
#undef DECODE_RATA
#undef DECODE_BA
#undef DECODE_OA
#undef DECODE_KA
#undef DECODE_LAR
#undef DECODE_FCA
#undef DECODE_BLA
#undef DECODE_SLA
#undef DECODE_VSA

#undef DECODE_NA
#undef DECODE_ONE
#undef DECODE_TWO
#undef DECODE_THREE
#undef DECODE_FOUR
#undef DECODE_FIVE

#undef PASS_NA
#undef PASS_ONE
#undef PASS_TWO
#undef PASS_THREE
#undef PASS_FOUR
#undef PASS_FIVE

#undef O

}

/**
 * The interpOne functions are fat wrappers around the iop* functions, mostly
 * adding a bunch of debug-only logging and stats tracking.
 */
#define O(opcode, imm, push, pop, flags)                                       \
JitResumeAddr interpOne##opcode(ActRec* fp, TypedValue* sp, Offset pcOff) {    \
  interp_set_regs(fp, sp, pcOff);                                              \
  SKTRACE(5, liveSK(),                                                         \
          "%40s %p %p\n",                                                      \
          "interpOne" #opcode " before (fp,sp)", vmfp(), vmsp());              \
  if (Stats::enableInstrCount()) {                                             \
    Stats::inc(Stats::Instr_Transl##opcode, -1);                               \
    Stats::inc(Stats::Instr_InterpOne##opcode);                                \
  }                                                                            \
  if (Trace::moduleEnabled(Trace::interpOne, 1)) {                             \
    static const StringData* cat = makeStaticString("interpOne");              \
    static const StringData* name = makeStaticString(#opcode);                 \
    Stats::incStatGrouped(cat, name, 1);                                       \
  }                                                                            \
  if (Trace::moduleEnabled(Trace::ringbuffer)) {                               \
    auto sk = liveSK().toAtomicInt();                                          \
    Trace::ringbufferEntry(Trace::RBTypeInterpOne, sk, 0);                     \
  }                                                                            \
  INC_TPC(interp_one)                                                          \
  /* Correct for over-counting in TC-stats. */                                 \
  Stats::inc(Stats::Instr_TC, -1);                                             \
  condStackTraceSep(Op##opcode);                                               \
  COND_STACKTRACE("op"#opcode" pre:  ");                                       \
  PC pc = vmpc();                                                              \
  ONTRACE(1, auto offset = vmfp()->func()->offsetOf(pc);                       \
          Trace::trace("op"#opcode" offset: %d\n", offset));                   \
  assertx(peek_op(pc) == Op::opcode);                                          \
  pc += encoded_op_size(Op::opcode);                                           \
  auto const retAddr = iopWrap##opcode(pc);                                    \
  vmpc() = pc;                                                                 \
  COND_STACKTRACE("op"#opcode" post: ");                                       \
  condStackTraceSep(Op##opcode);                                               \
  /*
   * Only set regstate back to dirty if an exception is not
   * propagating.  If an exception is throwing, regstate for this call
   * is actually still correct, and we don't have information in the
   * fixup map for interpOne calls anyway.
   */                                                                          \
  regState() = VMRegState::DIRTY;                                              \
  return retAddr;                                                              \
}
OPCODES
#undef O

InterpOneFunc interpOneEntryPoints[] = {
#define O(opcode, imm, push, pop, flags) &interpOne##opcode,
OPCODES
#undef O
};

// Whether the bytecode interpreter should collect per-bytecode code coverage
// for the current opcode. Shared between the computed-goto dispatcher
// (dispatchImpl) and the tail-call dispatcher (dispatchTailCall).
static ALWAYS_INLINE bool interpCollectCoverage() {
  return !RI().m_coverage.m_should_use_per_file_coverage
    ? RID().getCoverage()
    : vmfp() && vmfp()->unit()->isCoverageEnabled();
}

///////////////////////////////////////////////////////////////////////////////
// Tail-call (musttail) threaded dispatch.
//
// dispatchImpl() below is a computed-goto direct-threaded interpreter: every
// opcode body ends in `goto *optab[next]`. The implementation here is an
// alternative dispatcher in which each opcode handler instead ends by
// *tail-calling* the next opcode's handler. With a guaranteed tail call
// ([[clang::musttail]] / [[gnu::musttail]]) every step reuses a single native
// stack frame, and each handler gets its own return/branch site. Semantics are
// identical to dispatchImpl(); selection is controlled at runtime by
// Cfg::Eval::InterpTailCallDispatch.
//
// This path is only compiled when the toolchain guarantees a tail call. It
// works on any architecture the compiler targets (notably x86-64 and arm64).

#if defined(__has_cpp_attribute)
#  if __has_cpp_attribute(clang::musttail)
#    define HHVM_MUSTTAIL [[clang::musttail]]
#    define HHVM_HAS_MUSTTAIL 1
#  elif __has_cpp_attribute(gnu::musttail)
#    define HHVM_MUSTTAIL [[gnu::musttail]]
#    define HHVM_HAS_MUSTTAIL 1
#  endif
#endif
#ifndef HHVM_HAS_MUSTTAIL
#  define HHVM_HAS_MUSTTAIL 0
#  define HHVM_MUSTTAIL
#endif

#if defined(__has_attribute)
#  if __has_attribute(preserve_none)
#    define HHVM_TAIL_CALL_CC __attribute__((preserve_none))
#  endif
#endif
#ifndef HHVM_TAIL_CALL_CC
#  define HHVM_TAIL_CALL_CC
#endif

#if HHVM_HAS_MUSTTAIL

namespace {

enum class TCDispatchMode : uint8_t {
  Coverage,
  Debugger,
  DebuggerCoverage,
};

struct TCDispatchTable;
using TCDispatchFn =
  JitResumeAddr (HHVM_TAIL_CALL_CC *)(PC, const TCDispatchTable*);
struct TCDispatchTable {
  TCDispatchFn entries[Op_count];
};
extern const TCDispatchTable tcDispatchDirectTable;
extern const TCDispatchTable tcDispatchCoverageTable;
extern const TCDispatchTable tcDispatchDebuggerTable;
extern const TCDispatchTable tcDispatchDebuggerCoverageTable;

// Mode changes are sampled at basic-block boundaries, including simple jumps
// which remain in the handler chain when the JIT is disabled.
ALWAYS_INLINE const TCDispatchTable* tcDispatchTable(bool collectCoverage) {
  auto table = collectCoverage
    ? &tcDispatchCoverageTable
    : &tcDispatchDirectTable;
  DEBUGGER_ATTACHED_ONLY(
    table = collectCoverage
      ? &tcDispatchDebuggerCoverageTable
      : &tcDispatchDebuggerTable
  );
  return table;
}

// Instrumented tables add a small prelude before entering the common opcode
// body. This lets all modes share the body's production PGO profile while
// keeping hook checks off the direct path.
template<TCDispatchMode mode>
ALWAYS_INLINE void tcDispatchInstrument() {
  auto const opPC = vmpc();
  if constexpr (mode != TCDispatchMode::Coverage) {
    phpDebuggerOpcodeHook(opPC);
  }
  if constexpr (mode != TCDispatchMode::Debugger) {
    recordCodeCoverage(opPC);
  }
}

#ifdef HPHP_TRACE
// Keep the trace path shared so debug builds do not duplicate its formatting
// and stack-walking code in every opcode handler. That keeps the tail-call
// dispatch working set small when tracing is available but disabled at runtime.
NEVER_INLINE void tcDispatchTrace() {
  COND_STACKTRACE("dispatch:                    ");
  FTRACE(1, "dispatch: {}: {}\n", pcOff(),
         instrToString(vmpc(), vmfp()->func()));
}
#else
ALWAYS_INLINE void tcDispatchTrace() {}
#endif

// Post-opcode bookkeeping. Mirrors the tail of OPCODE_MAIN_BODY. Returns
// nullopt if dispatch should continue to the next opcode; otherwise the
// JitResumeAddr that the dispatcher must return to its caller.
template<Op op>
constexpr bool tcDispatchCanContinue() {
  return op == Op::Jmp || op == Op::JmpZ || op == Op::JmpNZ;
}

template<Op op>
ALWAYS_INLINE Optional<JitResumeAddr>
tcDispatchPost(PC pc, JitResumeAddr retAddr) {
  vmpc() = pc;
  if (instrCanHalt(op) && UNLIKELY(!pc)) {
    vmfp() = nullptr;
    vmpc() = nullptr;
    // We returned from the top VM frame in this nesting level; m_savedRip in
    // our ActRec must have been callToExit. (The computed-goto path returns
    // here without the "Halt dispatch" trace, so we match it.)
    assertx(isCallToExit((uint64_t)retAddr.arg));
    return retAddr;
  }
  if (instrIsControlFlow(op)) {
    if constexpr (tcDispatchCanContinue<op>()) {
      assertx(!retAddr);
      if (!RID().getJit()) return std::nullopt;
    }
    ONTRACE(1, Trace::trace("dispatch: Halt dispatch(%p)\n", vmfp()));
    return retAddr;
  }
  assertx(!retAddr);
  return std::nullopt;
}

// Generate one common body per opcode plus small coverage and debugger
// preludes. The actual opcode body runs inside iopWrap##name().
#define O(name, imm, push, pop, flags)                                  \
  NEVER_INLINE JitResumeAddr HHVM_TAIL_CALL_CC                          \
  tcDispatchBody_##name(PC pc, const TCDispatchTable* table) {          \
    tcDispatchTrace();                                                  \
    if (Stats::enableInstrCount()) {                                    \
      Stats::inc(Stats::Instr_InterpBB##name);                          \
    }                                                                   \
    auto const retAddr = iopWrap##name(pc);                             \
    if (auto const out = tcDispatchPost<Op::name>(pc, retAddr)) {       \
      return *out;                                                      \
    }                                                                   \
    if constexpr (tcDispatchCanContinue<Op::name>()) {                  \
      table = tcDispatchTable(interpCollectCoverage());                 \
    }                                                                   \
    auto const next = decode_op(pc);                                    \
    HHVM_MUSTTAIL return table->entries[size_t(next)](                  \
      pc, table);                                                       \
  }                                                                     \
  template<TCDispatchMode mode>                                         \
  NEVER_INLINE JitResumeAddr HHVM_TAIL_CALL_CC                          \
  tcDispatchInstrumented_##name(                                       \
      PC pc, const TCDispatchTable* table) {                            \
    tcDispatchInstrument<mode>();                                       \
    HHVM_MUSTTAIL return tcDispatchBody_##name(pc, table);              \
  }
OPCODES
#undef O

// Clang lays out internal functions in order of first reference, so this
// table, referenced before the dispatch tables, places these bodies first and
// contiguously; the remaining bodies follow in opcode order. The order is C3
// (hfsort) clustering of the opcode-to-opcode transitions recovered from the
// computed-goto dispatchImpl in the production aarch64 BOLT profile.
#define TC_DISPATCH_HOT_OPCODES(X)                                            \
  X(SetM) X(PopC) X(AssertRATL) X(AssertRATStk) X(PushL) X(CGetQuietL)        \
  X(PopL) X(RetC) X(BaseH) X(Null) X(UnsetL) X(Dup) X(IterGetValue)           \
  X(FCallFuncD) X(BaseC) X(BareThis) X(NullUninit) X(FCallObjMethodD)         \
  X(CreateCl) X(LazyClass) X(NewPair) X(AddElemC) X(String)                   \
  X(FCallClsMethodD) X(Int) X(Dict) X(Same) X(Eq) X(False) X(BaseL) X(QueryM) \
  X(NewObjD) X(SetL) X(IsTypeC) X(JmpZ) X(JmpNZ) X(IterNext) X(CastString)    \
  X(CastInt) X(True) X(InstanceOfD) X(NewStructDict) X(Vec)                   \
  X(FCallClsMethodSD) X(Jmp) X(Double) X(FCallCtor) X(CastDouble) X(Await)    \
  X(NewVec) X(WHResult) X(CGetL2) X(Mul) X(IterGetKey) X(Dim) X(Lt) X(Add)    \
  X(IterSetValue) X(Idx) X(CastBool) X(IncDecL) X(NSame) X(IterInit)          \
  X(MemoGetEager) X(ClsCnsD) X(MemoGet) X(Sub) X(Enter) X(RetCSuspended)      \
  X(SetOpL) X(MemoSet) X(FCallFunc)

[[gnu::used]] const TCDispatchFn tcDispatchHotLayout[] = {
#define X(name) &tcDispatchBody_##name,
  TC_DISPATCH_HOT_OPCODES(X)
#undef X
};

#undef TC_DISPATCH_HOT_OPCODES

const TCDispatchTable tcDispatchDirectTable{{
#define O(name, imm, push, pop, flags) \
  &tcDispatchBody_##name,
  OPCODES
#undef O
}};

const TCDispatchTable tcDispatchCoverageTable{{
#define O(name, imm, push, pop, flags) \
  &tcDispatchInstrumented_##name<TCDispatchMode::Coverage>,
  OPCODES
#undef O
}};

const TCDispatchTable tcDispatchDebuggerTable{{
#define O(name, imm, push, pop, flags) \
  &tcDispatchInstrumented_##name<TCDispatchMode::Debugger>,
  OPCODES
#undef O
}};

const TCDispatchTable tcDispatchDebuggerCoverageTable{{
#define O(name, imm, push, pop, flags) \
  &tcDispatchInstrumented_##name<TCDispatchMode::DebuggerCoverage>,
  OPCODES
#undef O
}};

JitResumeAddr dispatchTailCall(bool collectCoverage) {
  ONTRACE(1, Trace::trace("dispatch: Enter tail-call dispatch(%p)\n", vmfp()));
  auto pc = vmpc();
  auto const op = decode_op(pc);
  auto const table = tcDispatchTable(collectCoverage);
  // Root call (intentionally NOT a tail call): when the handler chain reaches a
  // control-flow/halt opcode it returns here, and we forward that to our caller.
  return table->entries[size_t(op)](pc, table);
}

} // namespace

#endif // HHVM_HAS_MUSTTAIL

#undef HHVM_TAIL_CALL_CC

JitResumeAddr dispatchImpl() {
  auto const checkCoverage = [&] {
    return interpCollectCoverage();
  };
  bool collectCoverage = checkCoverage();

#if HHVM_HAS_MUSTTAIL
  auto const mode = Cfg::interpDispatchMode;
  auto useTailCall = mode == Cfg::InterpDispatchMode::TailCall;
  if (mode == Cfg::InterpDispatchMode::Alternate) {
    static thread_local bool tailCall = false;
    useTailCall = tailCall = !tailCall;
  }
  FTRACE(1, "dispatch: selected {} interpreter (alternate={})\n",
         useTailCall ? "tail-call" : "computed-goto",
         mode == Cfg::InterpDispatchMode::Alternate);
  if (useTailCall) {
    return dispatchTailCall(collectCoverage);
  }
#endif

  static const void* const optabDirect[] = {
#define O(name, imm, push, pop, flags) \
    &&Label##name,
    OPCODES
#undef O
  };
  static const void* const optabDbg[] = {
#define O(name, imm, push, pop, flags) \
    &&LabelDbg##name,
    OPCODES
#undef O
  };
  static const void* const optabCover[] = {
#define O(name, imm, push, pop, flags) \
    &&LabelCover##name,
    OPCODES
#undef O
  };
  assertx(sizeof(optabDirect) / sizeof(const void *) == Op_count);
  assertx(sizeof(optabDbg) / sizeof(const void *) == Op_count);
  const void* const* optab = optabDirect;
  if (collectCoverage) {
    optab = optabCover;
  }
  DEBUGGER_ATTACHED_ONLY(optab = optabDbg);

  bool isCtlFlow = false;
  auto retAddr = JitResumeAddr::none();
  Op op;

#define DISPATCH_ACTUAL() goto *optab[size_t(op)]

#define DISPATCH() do {                                                 \
    if (isCtlFlow) {                                                    \
      ONTRACE(1,                                                        \
              Trace::trace("dispatch: Halt dispatch(%p)\n",             \
                           vmfp()));                                    \
      return retAddr;                                                   \
    }                                                                   \
    opPC = pc;                                                          \
    op = decode_op(pc);                                                 \
    COND_STACKTRACE("dispatch:                    ");                   \
    FTRACE(1, "dispatch: {}: {}\n", pcOff(),                            \
           instrToString(opPC, vmfp()->func()));                        \
    DISPATCH_ACTUAL();                                                  \
} while (0)

  ONTRACE(1, Trace::trace("dispatch: Enter dispatch(%p)\n",
          vmfp()));
  PC pc = vmpc();
  PC opPC;
  DISPATCH();

#define OPCODE_DBG_BODY(name, imm, push, pop, flags)          \
  phpDebuggerOpcodeHook(opPC)
#define OPCODE_COVER_BODY(name, imm, push, pop, flags)        \
  if (collectCoverage) {                                      \
    recordCodeCoverage(opPC);                                 \
  }
#define OPCODE_MAIN_BODY(name, imm, push, pop, flags)         \
  {                                                           \
    if (Stats::enableInstrCount()) {                          \
      Stats::inc(Stats::Instr_InterpBB##name);                \
    }                                                         \
    retAddr = iopWrap##name(pc);                              \
    vmpc() = pc;                                              \
    if (isFCallFunc(Op::name) ||                              \
        Op::name == Op::NativeImpl) {                         \
      collectCoverage = checkCoverage();                      \
      optab = !collectCoverage ? optabDirect : optabCover;    \
      DEBUGGER_ATTACHED_ONLY(optab = optabDbg);               \
    }                                                         \
    isCtlFlow = instrIsControlFlow(Op::name);                 \
    if (instrCanHalt(Op::name) && UNLIKELY(!pc)) {            \
      vmfp() = nullptr;                                       \
      vmpc() = nullptr;                                       \
      /* We returned from the top VM frame in this nesting level. This means
       * m_savedRip in our ActRec must have been callToExit, which should've
       * been returned by jitReturnPost(), whether or not we were called from
       * the TC. We only actually return callToExit to our caller if that
       * caller is dispatchBB(). */                             \
      assertx(isCallToExit((uint64_t)retAddr.arg));             \
      return retAddr;                                           \
    }                                                           \
    assertx(isCtlFlow || !retAddr);                             \
    DISPATCH();                                                 \
  }

#define O(name, imm, push, pop, flags)                        \
  LabelDbg##name:                                             \
    OPCODE_DBG_BODY(name, imm, push, pop, flags);             \
  LabelCover##name:                                           \
    OPCODE_COVER_BODY(name, imm, push, pop, flags)            \
  Label##name:                                                \
    OPCODE_MAIN_BODY(name, imm, push, pop, flags)

  OPCODES

#undef O
#undef DISPATCH
#undef DISPATCH_ACTUAL
#undef OPCODE_DBG_BODY
#undef OPCODE_COVER_BODY
#undef OPCODE_MAIN_BODY

  not_reached();
}

JitResumeAddr dispatchBB() {
  auto sk = [] {
    return SrcKey(vmfp()->func(), vmpc(), resumeModeFromActRec(vmfp()));
  };

  if (Trace::moduleEnabled(Trace::dispatchBB)) {
    static auto cat = makeStaticString("dispatchBB");
    auto name = makeStaticString(show(sk()));
    Stats::incStatGrouped(cat, name, 1);
  }
  if (Trace::moduleEnabled(Trace::ringbuffer)) {
    Trace::ringbufferEntry(Trace::RBTypeDispatchBB, sk().toAtomicInt(), 0);
  }
  auto const retAddr = dispatchImpl();
  // When dispatchBB returns with debugger interrupt set, the next check in JIT
  // for debugger interrupt must bring the execution back to interpreter.
  if (UNLIKELY(Cfg::Debugger::EnableVSDebugger &&
               !RID().getVSDebugDisablesJit() &&
               !g_context->m_dbgNoBreak &&
               RID().getDebuggerStepIntr() &&
               vmfp())) {
    markFunctionWithDebuggerIntr(vmfp()->func());
  }
  return retAddr;
}

}
