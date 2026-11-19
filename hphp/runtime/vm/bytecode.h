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

#pragma once

#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/rds-util.h"
#include "hphp/runtime/base/tv-arith.h"
#include "hphp/runtime/base/tv-array-like.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-mutate.h"
#include "hphp/runtime/base/tv-variant.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/type-string.h"

#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/iter.h"
#include "hphp/runtime/vm/name-value-table.h"
#include "hphp/runtime/vm/prologue-flags.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/runtime/vm/jit/jit-resume-addr.h"
#include "hphp/runtime/vm/jit/types.h"

#include "hphp/util/arena.h"
#include "hphp/util/type-traits.h"

#include <type_traits>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct ActRec;
struct Func;
struct Resumable;

///////////////////////////////////////////////////////////////////////////////

#define EVAL_FILENAME_SUFFIX ") : eval()'d code"

// perform the set(op) operation on lhs & rhs, leaving the result in lhs.
// The old value of lhs is decrefed.
ALWAYS_INLINE
void setopBody(tv_lval lhs, SetOpOp op, TypedValue* rhs) {
  assertx(tvIsPlausible(*lhs));
  assertx(tvIsPlausible(*rhs));

  switch (op) {
  case SetOpOp::PlusEqual:      tvAddEq(lhs, *rhs); return;
  case SetOpOp::MinusEqual:     tvSubEq(lhs, *rhs); return;
  case SetOpOp::MulEqual:       tvMulEq(lhs, *rhs); return;
  case SetOpOp::DivEqual:       tvDivEq(lhs, *rhs); return;
  case SetOpOp::PowEqual:       tvPowEq(lhs, *rhs); return;
  case SetOpOp::ModEqual:       tvModEq(lhs, *rhs); return;
  case SetOpOp::ConcatEqual:    tvConcatEq(lhs, *rhs); return;
  case SetOpOp::AndEqual:       tvBitAndEq(lhs, *rhs); return;
  case SetOpOp::OrEqual:        tvBitOrEq(lhs, *rhs);  return;
  case SetOpOp::XorEqual:       tvBitXorEq(lhs, *rhs); return;
  case SetOpOp::SlEqual:        tvShlEq(lhs, *rhs); return;
  case SetOpOp::SrEqual:        tvShrEq(lhs, *rhs); return;
  }
  not_reached();
}

///////////////////////////////////////////////////////////////////////////////

// Creates and initializes the global NamedValue table
void createGlobalNVTable();
// Collects the variables defined on the global NamedValue table
Array getDefinedVariables();

///////////////////////////////////////////////////////////////////////////////

/*
 * Returns true iff `ar` represents a frame on the VM eval stack or a Resumable
 * object on the PHP heap.
 *
 * The `may_be_non_runtime` flag should be set if we aren't guaranteed to be
 * running in a "Hack runtime" context---e.g., if we're in the JIT or an
 * extension thread, etc.  This function is pretty much guaranteed to return
 * false if we're not in the runtime, but the caller might be runtime-agnostic.
 */
bool isVMFrame(const ActRec* ar, bool may_be_non_runtime = false);

///////////////////////////////////////////////////////////////////////////////

void frame_free_locals_no_hook(ActRec* fp);

#define tvReturn(x)                                                     \
  ([&] {                                                                \
    TypedValue val_;                                                    \
    new (&val_) Variant(x);                                             \
    assertx(val_.m_type != KindOfUninit);                               \
    return val_;                                                        \
  }())

Class* arGetContextClass(const ActRec* ar);

///////////////////////////////////////////////////////////////////////////////

// Used by extension functions that take a PHP "callback", since they need to
// figure out the callback context once and call it multiple times. (e.g.
// array_map, array_filter, ...)
struct CallCtx {
  const Func* func;
  ObjectData* this_;
  Class* cls;
  bool dynamic;
};

constexpr size_t kNumIterCells = sizeof(Iter) / sizeof(TypedValue);
constexpr size_t kNumActRecCells = sizeof(ActRec) / sizeof(TypedValue);

///////////////////////////////////////////////////////////////////////////////

/*
 * We pad all stack overflow checks by a small amount to allow for three
 * things:
 *
 *   - inlining functions without having to either do another stack
 *     check (or chase down prologues to smash checks to be bigger).
 *
 *   - omitting stack overflow checks on leaf functions
 *
 *   - delaying stack overflow checks on reentry
 */
constexpr int kStackCheckReenterPadding = 9;

ALWAYS_INLINE
int stackCheckPadding() {
  return Cfg::Eval::StackCheckLeafPadding + kStackCheckReenterPadding;
}

// Interpreter evaluation stack.
struct Stack {
private:
  TypedValue* m_elms;
  TypedValue* m_top;
  TypedValue* m_base; // Stack grows down, so m_base is beyond the end of
                      // m_elms.

public:
  bool isAllocated() { return m_elms != nullptr; }
  void* getStackLowAddress() const { return m_elms; }
  void* getStackHighAddress() const { return m_base; }
  bool isValidAddress(uintptr_t v) {
    return v >= uintptr_t(m_elms) && v < uintptr_t(m_base);
  }
  void requestInit();
  void requestExit();

  static const int sSurprisePageSize;
  static const unsigned sMinStackElms;
  static void ValidateStackSize();
  Stack();
  ~Stack();

  std::string toString(const ActRec* fp, int offset,
                       std::string prefix="") const;

  bool wouldOverflow(int numCells) const;

  /*
   * top --
   * topOfStackOffset --
   *
   *   Accessors for the x64 translator. Do not play on or around.
   */
  TypedValue*& top() {
    return m_top;
  }

  static constexpr size_t topOfStackOffset() {
    return offsetof(Stack, m_top);
  }

  static TypedValue* anyFrameStackBase(const ActRec* fp);
  static TypedValue* frameStackBase(const ActRec* fp);
  static TypedValue* resumableStackBase(const ActRec* fp);

  ALWAYS_INLINE
  size_t count() const {
    return ((uintptr_t)m_base - (uintptr_t)m_top) / sizeof(TypedValue);
  }

  // Same as discard(), but meant to replace popC() iff the interpreter knows
  // for certain that decrementing a refcount is unnecessary.
  ALWAYS_INLINE
  void popX() {
    assertx(m_top != m_base);
    assertx(!isRefcountedType(m_top->m_type));
    tvDebugTrash(m_top);
    m_top++;
  }

  ALWAYS_INLINE
  void popC() {
    assertx(m_top != m_base);
    assertx(tvIsPlausible(*m_top));
    tvDecRefGen(m_top);
    tvDebugTrash(m_top);
    m_top++;
  }

  ALWAYS_INLINE
  void popU() {
    assertx(m_top != m_base);
    assertx(m_top->m_type == KindOfUninit);
    tvDebugTrash(m_top);
    ++m_top;
  }

  ALWAYS_INLINE
  void popTV() {
    assertx(m_top != m_base);
    assertx(tvIsPlausible(*m_top));
    tvDecRefGen(m_top);
    tvDebugTrash(m_top);
    m_top++;
  }

  // popAR() should only be used to tear down a pre-live ActRec. Once
  // an ActRec is live, it should be torn down using frame_free_locals()
  // followed by discardAR() or ret().
  ALWAYS_INLINE
  void popAR() {
    assertx(m_top != m_base);
    ActRec* ar = (ActRec*)m_top;
    if (ar->func()->cls() && ar->hasThis()) decRefObj(ar->getThis());
    discardAR();
  }

  ALWAYS_INLINE
  void discardAR() {
    assertx(m_top != m_base);
    if (debug) {
      for (int i = 0; i < kNumActRecCells; ++i) {
        tvDebugTrash(m_top + i);
      }
    }
    m_top += kNumActRecCells;
    assertx((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  ALWAYS_INLINE
  void retNoTrash() {
    m_top += kNumActRecCells - 1;
    assertx((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  ALWAYS_INLINE
  void ret() {
    // Leave part of the activation on the stack, since the return value now
    // resides there.
    if (debug) {
      for (int i = 0; i < kNumActRecCells - 1; ++i) {
        tvDebugTrash(m_top + i);
      }
    }
    m_top += kNumActRecCells - 1;
    assertx((uintptr_t)m_top <= (uintptr_t)m_base);
  }

  ALWAYS_INLINE
  void discard() {
    assertx(m_top != m_base);
    tvDebugTrash(m_top);
    m_top++;
  }

  ALWAYS_INLINE
  void ndiscard(size_t n) {
    assertx((uintptr_t)&m_top[n] <= (uintptr_t)m_base);
    if (debug) {
      for (int i = 0; i < n; ++i) {
        tvDebugTrash(m_top + i);
      }
    }
    m_top += n;
  }

  ALWAYS_INLINE
  void trim(TypedValue* c) {
    assertx(c <= m_base);
    assertx(m_top <= c);
    if (debug) {
      while (m_top < c) tvDebugTrash(m_top++);
    } else {
      m_top = c;
    }
  }

  ALWAYS_INLINE
  void dup() {
    assertx(m_top != m_base);
    assertx(m_top != m_elms);
    TypedValue* fr = m_top;
    m_top--;
    TypedValue* to = m_top;
    tvDup(*fr, *to);
  }

  ALWAYS_INLINE
  void pushUninit() {
    assertx(m_top != m_elms);
    m_top--;
    tvWriteUninit(*m_top);
  }

  ALWAYS_INLINE
  void pushNull() {
    assertx(m_top != m_elms);
    m_top--;
    tvWriteNull(*m_top);
  }

  ALWAYS_INLINE
  void pushNullUninit() {
    assertx(m_top != m_elms);
    m_top--;
    m_top->m_data.num = 0;
    m_top->m_type = KindOfUninit;
  }

  template<DataType t, class T> void pushVal(T v) {
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<t>(v);
  }
  ALWAYS_INLINE void pushBool(bool v) { pushVal<KindOfBoolean>(v); }
  ALWAYS_INLINE void pushInt(int64_t v) { pushVal<KindOfInt64>(v); }
  ALWAYS_INLINE void pushDouble(double v) { pushVal<KindOfDouble>(v); }
  ALWAYS_INLINE void pushClass(Class* v) { pushVal<KindOfClass>(v); }

  // This should only be called directly when the caller has
  // already adjusted the refcount appropriately
  ALWAYS_INLINE
  void pushStringNoRc(StringData* s) {
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfString>(s);
  }

  ALWAYS_INLINE
  void pushStaticString(const StringData* s) {
    assertx(s->isStatic()); // No need to call s->incRefCount().
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfString>(s);
  }

  ALWAYS_INLINE
  void pushLazyClass(LazyClassData l) {
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfLazyClass>(l);
  }

  ALWAYS_INLINE
  void pushEnumClassLabel(const StringData* s) {
    assertx(s->isStatic()); // No need to call s->incRefCount().
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfEnumClassLabel>(s);
  }

  ALWAYS_INLINE
  void pushArrayLikeNoRc(ArrayData* a) {
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_array_like_tv(a);
  }

  ALWAYS_INLINE
  void pushVecNoRc(ArrayData* a) {
    assertx(a->isVecType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfVec>(a);
  }

  ALWAYS_INLINE
  void pushDictNoRc(ArrayData* a) {
    assertx(a->isDictType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfDict>(a);
  }

  ALWAYS_INLINE
  void pushKeysetNoRc(ArrayData* a) {
    assertx(a->isKeysetType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfKeyset>(a);
  }

  ALWAYS_INLINE
  void pushArrayLike(ArrayData* a) {
    assertx(a);
    pushArrayLikeNoRc(a);
    a->incRefCount();
  }

  ALWAYS_INLINE
  void pushVec(ArrayData* a) {
    assertx(a);
    pushVecNoRc(a);
    a->incRefCount();
  }

  ALWAYS_INLINE
  void pushDict(ArrayData* a) {
    assertx(a);
    pushDictNoRc(a);
    a->incRefCount();
  }

  ALWAYS_INLINE
  void pushKeyset(ArrayData* a) {
    assertx(a);
    pushKeysetNoRc(a);
    a->incRefCount();
  }

  ALWAYS_INLINE
  void pushStaticArrayLike(const ArrayData* a) {
    assertx(a->isStatic()); // No need to call a->incRefCount().
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_array_like_tv(const_cast<ArrayData*>(a));
  }

  ALWAYS_INLINE
  void pushStaticVec(const ArrayData* a) {
    assertx(a->isStatic()); // No need to call a->incRefCount().
    assertx(a->isVecType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfVec>(a);
  }

  ALWAYS_INLINE
  void pushStaticDict(const ArrayData* a) {
    assertx(a->isStatic()); // No need to call a->incRefCount().
    assertx(a->isDictType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfDict>(a);
  }

  ALWAYS_INLINE
  void pushStaticKeyset(const ArrayData* a) {
    assertx(a->isStatic()); // No need to call a->incRefCount().
    assertx(a->isKeysetType());
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfKeyset>(a);
  }

  // This should only be called directly when the caller has
  // already adjusted the refcount appropriately
  ALWAYS_INLINE
  void pushObjectNoRc(ObjectData* o) {
    assertx(m_top != m_elms);
    m_top--;
    *m_top = make_tv<KindOfObject>(o);
  }

  ALWAYS_INLINE
  void pushObject(ObjectData* o) {
    pushObjectNoRc(o);
    o->incRefCount();
  }

  ALWAYS_INLINE
  void pushFunc(Func* f) {
    m_top--;
    *m_top = make_tv<KindOfFunc>(f);
  }

  ALWAYS_INLINE
  void pushRFuncNoRc(RFuncData* f) {
    m_top--;
    *m_top = make_tv<KindOfRFunc>(f);
  }

  ALWAYS_INLINE
  void pushRFunc(RFuncData* f) {
    pushRFuncNoRc(f);
    f->incRefCount();
  }

  ALWAYS_INLINE
  void pushClsMethNoRc(ClsMethDataRef clsMeth) {
    m_top--;
    *m_top = make_tv<KindOfClsMeth>(clsMeth);
  }

  ALWAYS_INLINE
  void pushRClsMethNoRc(RClsMethData* clsMeth) {
    m_top--;
    *m_top = make_tv<KindOfRClsMeth>(clsMeth);
  }

  ALWAYS_INLINE
  void nalloc(size_t n) {
    assertx((uintptr_t)(m_top - n) <= (uintptr_t)m_base);
    m_top -= n;
  }

  ALWAYS_INLINE
  TypedValue* allocC() {
    assertx(m_top != m_elms);
    m_top--;
    return (TypedValue*)m_top;
  }

  ALWAYS_INLINE
  TypedValue* allocTV() {
    assertx(m_top != m_elms);
    m_top--;
    return m_top;
  }

  ALWAYS_INLINE
  ActRec* allocA() {
    assertx((uintptr_t)(m_top - kNumActRecCells) >= (uintptr_t)m_elms);
    assertx(kNumActRecCells * sizeof(TypedValue) == sizeof(ActRec));
    m_top -= kNumActRecCells;
    return (ActRec*)m_top;
  }

  ALWAYS_INLINE
  void allocI() {
    assertx(kNumIterCells * sizeof(TypedValue) == sizeof(Iter));
    assertx((uintptr_t)(m_top - kNumIterCells) >= (uintptr_t)m_elms);
    m_top -= kNumIterCells;
  }

  ALWAYS_INLINE
  void replaceC(const TypedValue c) {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = c;
  }

  template <DataType DT>
  ALWAYS_INLINE
  void replaceC() {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = make_tv<DT>();
  }

  template <DataType DT, typename T>
  ALWAYS_INLINE
  void replaceC(T value) {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = make_tv<DT>(value);
  }

  ALWAYS_INLINE
  void replaceTV(const TypedValue& tv) {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = tv;
  }

  template <DataType DT>
  ALWAYS_INLINE
  void replaceTV() {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = make_tv<DT>();
  }

  template <DataType DT, typename T>
  ALWAYS_INLINE
  void replaceTV(T value) {
    assertx(m_top != m_base);
    tvDecRefGen(m_top);
    *m_top = make_tv<DT>(value);
  }

  ALWAYS_INLINE
  TypedValue* topC() {
    assertx(m_top != m_base);
    return tvAssertPlausible(m_top);
  }

  ALWAYS_INLINE
  TypedValue* topTV() {
    assertx(m_top != m_base);
    return m_top;
  }

  ALWAYS_INLINE
  ActRec* indA(size_t ind) {
    assertx(m_top != m_base);
    assertx(count() >= ind + kNumActRecCells);
    return (ActRec*)&m_top[ind];
  }

  ALWAYS_INLINE
  TypedValue* indC(size_t ind) {
    assertx(m_top != m_base);
    return tvAssertPlausible(&m_top[ind]);
  }

  ALWAYS_INLINE
  TypedValue* indTV(size_t ind) {
    assertx(m_top != m_base);
    return &m_top[ind];
  }
};

//////////////////////////////////////////////////////////////////////

void flush_evaluation_stack();

/*
 * Visit all the slots on a live eval stack, stopping when we reach
 * the supplied activation record.
 *
 * The stack elements are visited from lower address to higher.
 *
 * This will not read the VM registers (pc, fp, sp), so it will
 * perform the requested visitation independent of modifications to
 * the VM stack or frame pointer.
 */
template<class TV, class TVFun>
typename maybe_const<TV, TypedValue>::type
visitStackElems(const ActRec* const fp, TV* const stackTop, TVFun tvFun) {
  const TypedValue* const base = Stack::anyFrameStackBase(fp);
  auto cursor = stackTop;
  assertx(cursor <= base);

  while (cursor < base) {
    tvFun(cursor++);
  }
}

void resetCoverageCounters();

// Resolve a Static / Self / Parent class reference to a class.
Class* specialClsRefToCls(SpecialClsRef ref);

// The interpOne*() methods implement individual opcode handlers.
using InterpOneFunc = jit::JitResumeAddr (*) (ActRec*, TypedValue*, Offset);
extern InterpOneFunc interpOneEntryPoints[];

void doFCall(PrologueFlags prologueFlags, const Func* func,
             const ArrayData* namedArgNames,
             uint32_t numPositionalArgs, void* ctx, jit::TCA retAddr);
bool funcEntry();
jit::JitResumeAddr dispatchBB();
Array getDefinedVariables(const ActRec*);

/*
 * Start a new nested instance of VM either at the beginning of a function
 * determined by the enterFnAr frame, or at the current vmpc() location.
 *
 * Execution finishes by either returning, awaiting or yielding from the top
 * level frame, in which case vmfp()/vmpc() are set to nullptr, or by throwing
 * an exception, which callers usually process via exception_handler().
 */
void enterVMAtFunc(ActRec* enterFnAr, uint32_t posArgcInclUnpack,
                   bool allNamedParamsPassed);
void enterVMAtCurPC();
uint32_t prepareUnpackArgs(const Func* func, uint32_t numPositionalArgs,
                           bool checkInOutAnnot);

///////////////////////////////////////////////////////////////////////////////

}

#define incl_HPHP_VM_BYTECODE_INL_H_
#include "hphp/runtime/vm/bytecode-inl.h"
#undef incl_HPHP_VM_BYTECODE_INL_H_
