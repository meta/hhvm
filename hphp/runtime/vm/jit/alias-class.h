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

#include "hphp/runtime/vm/minstr-state.h"

#include "hphp/runtime/vm/jit/alias-id-set.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"

#include <bitset>
#include <string>
#include <cstdint>

namespace HPHP { struct StringData; }
namespace HPHP::jit {

struct SSATmp;

//////////////////////////////////////////////////////////////////////

/*
 * AliasClass represents a lattice of abstract memory locations.  These support
 * type-system-like operations (operator<= for subset, operator| for union, and
 * maybe() to test for non-zero intersections).
 *
 * AUnknown is the top of the lattice (is the class of all possible memory
 * locations we care about).  AEmpty is the bottom.  AUnknownTV is a union of
 * all the classes that contain TypedValue-style storage of PHP values.  The
 * various special location types AFoo all have an AFooAny, which is the
 * superset of all AFoos.
 *
 * Part of the lattice currently looks like this:
 *
 *                         Unknown
 *                            |
 *                            |
 *                    +-------+-------+
 *                    |               |
 *                 UnknownTV         Iter
 *                    |
 *                    |
 *                    |
 *      +---------+---+---------------+-------------------+----------+
 *      |         |                   |           |       |          |
 *      |         |                   |           |       |          |
 *      |         |                   |           |       |       RdsAny
 *      |         |                   |         Other     |          |
 *      |         |                HeapAny*               |         ...
 *      |         |                   |                   |
 *      |         |            +------+------+            |
 *      |         |            |             |            |
 *   FrameAny  StackAny     ElemAny       PropAny     MIStateAny
 *      |         |          /    \          |            |
 *     ...       ...   ElemIAny  ElemSAny   ...           |
 *                        |         |                     |
 *                       ...       ...             +------+------+
 *                                                 |             |
 *                                             MITempBase     MIBase**
 *
 *
 *   (*) AHeapAny contains some things other than ElemAny, and PropAny
 *       that don't have explicit nodes in the lattice yet.  (Like the
 *       lvalBlackhole, etc.)  It's hard for this to matter to client code for
 *       now because we don't expose an intersection or difference operation.
 *
 *  (**) MIBase is a pointer, so it's not UnknownTV, but it's hard to find
 *       the right spot for it in this diagram.
 */
struct AliasClass;

//////////////////////////////////////////////////////////////////////

struct ALocal;
struct AIter;

namespace detail {

static constexpr uint32_t kAIterPosOffset = 0;
static constexpr uint32_t kAIterEndOffset = 1;
static constexpr uint32_t kSlotsPerAIter = 2;

}

struct UFrameBase {
  explicit UFrameBase(SSATmp* fp) : frameIdx{frameDepthIndex(fp)} {}
  explicit UFrameBase(uint32_t frameIdx) : frameIdx{frameIdx} {}
  uint32_t frameIdx;
};

#define FRAME_RELATIVE(Name)                                                   \
  struct Name : UFrameBase {                                                   \
    Name(SSATmp* fp, AliasIdSet ids) : UFrameBase{fp}, ids{ids} {}             \
    Name(uint32_t frameIdx, AliasIdSet ids) : UFrameBase{frameIdx}, ids{ids} {}\
    AliasIdSet ids;                                                            \
  }

#define FRAME_RELATIVE0(Name)                                                  \
  struct Name : UFrameBase {                                                   \
    using UFrameBase::UFrameBase;                                              \
  }

/*
 * Special data for locations known to be a set of locals on the frame `fp'.
 */
FRAME_RELATIVE(ALocal);

/*
 * Iterator state. We model the different fields of the iterator as 4
 * adjacent slots (with each iterator starting at a multiple of 4).
 */
FRAME_RELATIVE(AIter);

inline AIter aiter_pos(SSATmp* fp, uint32_t id) {
  using namespace detail;
  return AIter { fp, id * kSlotsPerAIter + kAIterPosOffset };
}
inline AIter aiter_end(SSATmp* fp, uint32_t id) {
  using namespace detail;
  return AIter { fp, id * kSlotsPerAIter + kAIterEndOffset };
}

inline AIter aiter_all(SSATmp* fp, uint32_t id) {
  using namespace detail;
  return AIter {
    fp,
    AliasIdSet::IdRange(id * kSlotsPerAIter, (id + 1) * kSlotsPerAIter)
  };
}

// Returns an AliasClass capturing all iterator states for iterators in the
// range [start, end)
inline AIter aiter_range(SSATmp* fp, uint32_t start, uint32_t end) {
  using namespace detail;
  return AIter {
    fp,
    AliasIdSet::IdRange(start * kSlotsPerAIter, end * kSlotsPerAIter)
  };
}

/*
 * These singleton values are each associated with a given frame. AFContext
 * is the m_thisUnsafe field of ActRec, AFFunc is m_func, AFMeta holds the
 * flags, hardware, and VM return fields (m_savedRip and m_callOffAndFlags).
 */
FRAME_RELATIVE0(AFContext);
FRAME_RELATIVE0(AFFunc);
FRAME_RELATIVE0(AFMeta);

/*
 * AActRec is a union of AFContext, AFFunc, and AFMeta
 */
FRAME_RELATIVE0(AActRec);

/*
 * A location inside of an object property, with base `obj' at physical index
 * `index' from the ObjectData*.
 */
struct AProp  { SSATmp* obj; uint16_t offset; };

struct AClosureArg { SSATmp* closure; uint16_t offset; };

/*
 * A integer index inside of an array, with base `arr'.  The `arr' tmp is any
 * kind of array (not necessarily a vec).
 */
struct AElemI { SSATmp* arr; int64_t idx; };

/*
 * A location inside of an array, with base `arr', with a string key.  The
 * `arr' tmp is any kind of array.
 */
struct AElemS { SSATmp* arr; const StringData* key; };

/*
 * A range of the stack located between the `low' and `high' offsets from
 * the IRSP. The `low' offset may be set to INT32_MIN to refer to the class
 * of all stack locations below the `high' depth. The stack pointer is the same
 * for all stack ranges in the IR unit, and thus is not stored here.
 *
 * As an example, `acls = AStack::range(IRSPRelOffset{-3}, IRSPRelOffset{-1})'
 * represents the following:
 *
 *         ___________________
 *  high  |   irspoff  0      |
 *    ^   |___________________| ___...sp points here
 *    |   |   irspoff -1      |
 *    |   |___________________| ___...acls.high             __
 *    |   |   ifspoff -2      |                               \
 *    |   |___________________|                                | acls
 *    |   |   irspoff -3      |                                |
 *    |   |___________________| ___...acls.low              __/
 *   low  |   irspoff -4      |
 *        |___________________|
 *
 * Some notes on how the evaluation stack is treated for alias analysis:
 *
 *   o Since AStack locations are always canonicalized, different AStack
 *     locations must not alias if there is no overlap in the ranges.
 *
 *   o In situations with inlined calls, we may in fact have ALocal locations
 *     that refer to the same concrete memory locations (in the evaluation
 *     stack) as other AStack locations in the same HHIR program.  However, the
 *     portions of the HHIR program that may use that memory location in either
 *     of those ways may not overlap, based on the HHBC semantics of function
 *     calls and eval stack usage.  (This may seem odd, but it's not really any
 *     different from knowing that a physical portion of the heap may be
 *     treated as both an AElemI or AProp, but not at the same time based on
 *     HHBC-level semantics.)
 */
struct AStack {
  // Return AStack representing a single stack cell at `offset`.
  static AStack at(IRSPRelOffset offset) {
    return AStack{offset, offset + 1};
  }

  // Return AStack representing a range of stack cells between low (inclusive)
  // and high (exclusive) offsets.
  static AStack range(IRSPRelOffset low, IRSPRelOffset high) {
    return AStack{low, high};
  }

  // Return AStack representing all stack cells below the high offset (not
  // including the cell at the high offset, as that one is above).
  static AStack below(IRSPRelOffset high) {
    auto constexpr low = IRSPRelOffset{std::numeric_limits<int32_t>::min()};
    return AStack{low, high};
  }

  uint32_t size() const {
    return safe_cast<uint32_t>(int64_t{high.offset} - int64_t{low.offset});
  }

  IRSPRelOffset low;
  IRSPRelOffset high;

private:
  explicit AStack(IRSPRelOffset l, IRSPRelOffset h) : low(l), high(h) {}
};

/*
 * A TypedValue stored in rds.
 *
 * Assumes this handle uniquely identifies a TypedValue in rds - it's
 * not required that the tv is at the start of the rds storage.
 */
struct ARds { rds::Handle handle; };

#undef FRAME_RELATIVE
#undef FRAME_RELATIVE0

#define ALIAS_CLASS_SPEC          \
  O(None,     BEmpty)             \
  O(Local,    (BLocal | BActRec)) \
  O(Iter,     (BIter | BActRec))  \
  O(ClosureArg, BClosureArg)      \
  O(Prop,     BProp)              \
  O(ElemI,    BElemI)             \
  O(ElemS,    BElemS)             \
  O(Stack,    BStack)             \
  O(Rds,      BRds)               \
  O(FrameAll, BActRec)            \
/**/

//////////////////////////////////////////////////////////////////////

struct AliasClass {
  enum rep : uint32_t {  // bits for various location classes
    BEmpty    = 0,
    // The relative order of the values are used in operator| to decide
    // which specialization is more useful.
    BLocal      = 1U << 0,
    BIter       = 1U << 1,
    BClosureArg = 1U << 2,
    BProp       = 1U << 3,
    BElemI      = 1U << 4,
    BElemS      = 1U << 5,
    BStack      = 1U << 6,
    BRds        = 1U << 7,
    BFContext   = 1U << 8,
    BFFunc      = 1U << 9,
    BFMeta      = 1U << 10,

    // Have no specialization, put them last.
    BMITempBase = 1U << 11,
    BMIBase     = 1U << 12,
    BMIROProp   = 1U << 13,
    BFBasePtr   = 1U << 14,
    BVMFP       = 1U << 15,
    BVMSP       = 1U << 16,
    BVMPC       = 1U << 17,
    BVMRetAddr  = 1U << 18,
    BVMRegState = 1U << 19,
    BOther      = 1U << 20,

    BVMReg     = BVMFP | BVMSP | BVMPC | BVMRetAddr,
    BElem      = BElemI | BElemS,
    BHeap      = BElem | BProp | BClosureArg,
    BMIState   = BMITempBase | BMIBase | BMIROProp,

    BActRec = BFContext | BFFunc | BFMeta,

    BUnknownTV = ~(BIter | BMIBase | BActRec | BVMReg | BVMRegState),

    BUnknown   = static_cast<uint32_t>(-1),
  };

  /*
   * A hashing function for alias classes.
   */
  struct Hash { size_t operator()(AliasClass) const; };

  /*
   * Create an alias class from a bit representation.  Usually you should use
   * one of the other functions to create these.
   */
  explicit AliasClass(rep bits) : m_bits{bits} {}

  /*
   * Create an alias class with more precise specialized information about
   * where it is.
   */
  /* implicit */ AliasClass(ALocal);
  /* implicit */ AliasClass(AIter);
  /* implicit */ AliasClass(AClosureArg);
  /* implicit */ AliasClass(AProp);
  /* implicit */ AliasClass(AElemI);
  /* implicit */ AliasClass(AElemS);
  /* implicit */ AliasClass(AStack);
  /* implicit */ AliasClass(ARds);
  /* implicit */ AliasClass(AFContext);
  /* implicit */ AliasClass(AFFunc);
  /* implicit */ AliasClass(AFMeta);
  /* implicit */ AliasClass(AActRec);

  /*
   * Exact equality.
   */
  bool operator==(AliasClass) const;
  bool operator!=(AliasClass o) const { return !(*this == o); }

  /*
   * Return an AliasClass that is the precise union of this class and another
   * class, or std::nullopt if that precise union cannot be represented.
   *
   * Guaranteed to be commutative.
   */
  Optional<AliasClass> precise_union(AliasClass) const;

  /*
   * Create an alias class that is at least as big as the true union of this
   * alias class and another one.
   *
   * If this.precise_union(o) is not std::nullopt, this function is guaranteed
   * to return *this.precise_union(o).  Otherwise it may return an arbitrary
   * AliasClass bigger than the (unrepresentable) true union---callers should
   * not rely on specifics about how much bigger it is.
   *
   * Guaranteed to be commutative.
   */
  AliasClass operator|(AliasClass o) const;
  AliasClass& operator|=(AliasClass o) { return *this = *this | o; }

  /*
   * Returns whether this alias class is a non-strict-subset of another one.
   */
  bool operator<=(AliasClass) const;
  bool operator<(AliasClass o) const { return *this <= o && *this != o; }

  /*
   * Returns whether an alias class could possibly refer to the same concrete
   * memory location as another one.  Basically, do they have a non-empty
   * intersection.
   */
  bool maybe(AliasClass) const;

  /*
   * Returns whether the alias class contains a single location.
   */
  bool isSingleLocation() const;

  /*
   * Conditionally access specific known information of various kinds.
   *
   * Returns std::nullopt if this alias class has no specialization in that way.
   */
  Optional<ALocal>          local() const;
  Optional<AIter>           iter() const;
  Optional<AClosureArg>     closureArg() const;
  Optional<AProp>           prop() const;
  Optional<AElemI>          elemI() const;
  Optional<AElemS>          elemS() const;
  Optional<AStack>          stack() const;
  Optional<ARds>            rds() const;
  Optional<AFContext>       fcontext() const;
  Optional<AFFunc>          ffunc() const;
  Optional<AFMeta>          fmeta() const;
  Optional<AActRec>         actrec() const;

  /*
   * Conditionally access specific known information, but also checking that
   * that is the only major category contained in this AliasClass.
   *
   * I.e., cls.is_foo() is semantically equivalent to:
   *
   *   cls <= AFooAny ? cls.foo() : std::nullopt
   */
  Optional<ALocal>          is_local() const;
  Optional<AIter>           is_iter() const;
  Optional<AClosureArg>     is_closureArg() const;
  Optional<AProp>           is_prop() const;
  Optional<AElemI>          is_elemI() const;
  Optional<AElemS>          is_elemS() const;
  Optional<AStack>          is_stack() const;
  Optional<ARds>            is_rds() const;
  Optional<AFContext>       is_fcontext() const;
  Optional<AFFunc>          is_ffunc() const;
  Optional<AFMeta>          is_fmeta() const;
  Optional<AActRec>         is_actrec() const;

  /*
   * Like the other foo() and is_foo() methods, but since we don't have an
   * AMIState anymore, these return AliasClass instead.
   */
  Optional<AliasClass> mis() const;
  Optional<AliasClass> is_mis() const;
  Optional<AliasClass> frame_base() const;
  Optional<AliasClass> is_frame_base() const;
  Optional<AliasClass> vm_reg() const;
  Optional<AliasClass> is_vm_reg() const;
  Optional<AliasClass> vm_reg_state() const;
  Optional<AliasClass> is_vm_reg_state() const;

  Optional<AliasClass> exclude_vm_reg() const;

private:
  enum class STag {
#define O(name, ...) name,
    ALIAS_CLASS_SPEC
#undef O
  };
private:
  friend std::string show(AliasClass);
  friend AliasClass canonicalize(AliasClass);

  void framelike_union_set(ALocal);
  void framelike_union_set(AIter);

  template <typename T>
  static AliasClass framelike_union(rep newBits, rep frm, T a, T b);

  bool checkInvariants() const;
  bool equivData(AliasClass) const;
  bool subclassData(AliasClass) const;
  bool maybeData(AliasClass) const;
  bool diffSTagSubclassData(rep relevant_bits, AliasClass) const;
  bool diffSTagMaybeData(rep relevant_bits, AliasClass) const;
  Optional<UFrameBase> asUFrameBase() const;
  rep unionSTagBits(AliasClass) const;
  static AliasClass unionData(rep newBits, AliasClass, AliasClass);
  static STag stagFor(rep bits);

private:
  rep m_bits;
  rep m_stagBits{BEmpty};
  union {
    ALocal          m_local;
    AIter           m_iter;
    AClosureArg     m_closureArg;
    AProp           m_prop;
    AElemI          m_elemI;
    AElemS          m_elemS;
    AStack          m_stack;
    ARds            m_rds;

    UFrameBase      m_frameAll;
  };
};

//////////////////////////////////////////////////////////////////////

/* General alias classes. */
auto const AEmpty             = AliasClass{AliasClass::BEmpty};
auto const ALocalAny          = AliasClass{AliasClass::BLocal};
auto const AIterAny           = AliasClass{AliasClass::BIter};
auto const AClosureArgAny     = AliasClass{AliasClass::BClosureArg};
auto const APropAny           = AliasClass{AliasClass::BProp};
auto const AHeapAny           = AliasClass{AliasClass::BHeap};
auto const AStackAny          = AliasClass{AliasClass::BStack};
auto const ARdsAny            = AliasClass{AliasClass::BRds};
auto const AElemIAny          = AliasClass{AliasClass::BElemI};
auto const AElemSAny          = AliasClass{AliasClass::BElemS};
auto const AElemAny           = AliasClass{AliasClass::BElem};
auto const AMIStateAny        = AliasClass{AliasClass::BMIState};
auto const AFContextAny       = AliasClass{AliasClass::BFContext};
auto const AFFuncAny          = AliasClass{AliasClass::BFFunc};
auto const AFMetaAny          = AliasClass{AliasClass::BFMeta};
auto const AActRecAny         = AliasClass{AliasClass::BActRec};
auto const AUnknownTV         = AliasClass{AliasClass::BUnknownTV};
auto const AUnknown           = AliasClass{AliasClass::BUnknown};

/* Alias classes for specific MInstrState fields. */
auto const AMIStateBase       = AliasClass{AliasClass::BMIBase};
auto const AMIStateROProp     = AliasClass{AliasClass::BMIROProp};
auto const AMIStateTempBase   = AliasClass{AliasClass::BMITempBase};

/* Alias class for the frame base register */
auto const AFBasePtr          = AliasClass{AliasClass::BFBasePtr};

/* Alias class for the VM register state */
auto const AVMFP              = AliasClass{AliasClass::BVMFP};
auto const AVMSP              = AliasClass{AliasClass::BVMSP};
auto const AVMPC              = AliasClass{AliasClass::BVMPC};
auto const AVMRetAddr         = AliasClass{AliasClass::BVMRetAddr};
auto const AVMRegAny          = AliasClass{AliasClass::BVMReg};
auto const AVMRegState        = AliasClass{AliasClass::BVMRegState};

/* For misc things which we don't care to distinguish */
auto const AOther             = AliasClass{AliasClass::BOther};

//////////////////////////////////////////////////////////////////////

/*
 * Replace any SSATmps in an AliasClass with their canonical name (chasing
 * passthrough instructions as with canonical() from analysis.h.)
 */
AliasClass canonicalize(AliasClass);
jit::vector<AliasClass> canonicalize(jit::vector<AliasClass>);

/*
 * Produce a debug string for an alias class.
 */
std::string show(AliasClass);
std::string show(const jit::vector<AliasClass>&);

//////////////////////////////////////////////////////////////////////

}
