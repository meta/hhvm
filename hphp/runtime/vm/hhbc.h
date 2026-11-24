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

#include <type_traits>

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/types.h"
#include "hphp/runtime/base/header-kind.h"
#include "hphp/runtime/vm/fcall-args-flags.h"
#include "hphp/runtime/vm/hhbc-shared.h"
#include "hphp/runtime/vm/iter-args-flags.h"
#include "hphp/runtime/vm/member-key.h"
#include "hphp/runtime/vm/opcodes.h"
#include "hphp/util/compact-vector.h"
#include "hphp/util/either.h"
#include "hphp/util/functional.h"
#include "hphp/util/hash-set.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct Unit;
struct UnitEmitter;
struct Func;
struct FuncEmitter;

constexpr size_t kMaxHhbcImms = 6;

// A contiguous range of locals. The count is the number of locals
// including the first. If the range is empty, count will be zero and
// first's value is arbitrary.
struct LocalRange {
  uint32_t first;
  uint32_t count;
};

/*
 * Arguments to IterInit / IterNext opcodes.
 * hhas format: <iterId>
 * hhbc format: <uint8:flags> <iva:iterId>
 *
 * We don't accept flags from hhas because our flags require analyses that we
 * currently only do in HHBBC.
 */
struct IterArgs {
  using Flags = IterArgsFlags;

  explicit IterArgs(Flags flags, int32_t iterId)
    : iterId(iterId), flags(flags) {}

  bool operator==(const IterArgs& other) const {
    return iterId == other.iterId && flags == other.flags;
  }

  int32_t iterId;
  Flags flags;

  template <typename SerDe> static IterArgs makeForSerde(SerDe& sd) {
    static_assert(SerDe::deserializing);
    int32_t iterId;
    Flags flags;
    sd(iterId)(flags);
    return IterArgs{flags, iterId};
  }

  template <typename SerDe> void serde(SerDe& sd) {
    sd(iterId)(flags);
  }
};

// Arguments to FCall opcodes.
// hhas format: <flags> <numArgs> <numRets> <inoutArgs> <readonlyArgs>
//              <namedArgNames vecid> <asyncEagerOffset>
// hhbc format: <uint16:flags> ?<iva:numArgs> ?<iva:numRets>
//              ?<boolvec:inoutArgs> ?<boolvec:readonlyArgs>
//              ?<vecid:argNames> ?<ba:asyncEagerOffset>
//   flags            = flags (hhas doesn't have HHBC-only flags)
//   numArgs          = flags >> kFirstNumArgsBit
//                        ? flags >> kFirstNumArgsBit - 1 : decode_iva()
//   numRets          = flags & HasInOut ? decode_iva() : 1
//   inoutArgs        = flags & EnforceInOut ? decode bool vec : nullptr
//   asyncEagerOffset = flags & HasAEO ? decode_ba() : kInvalidOffset
struct FCallArgsBase {
  using Flags = FCallArgsFlags;
  // The first (lowest) bit of numArgs.
  static constexpr uint16_t kFirstNumArgsBit = 13;

  // Flags that are valid on FCallArgsBase::flags struct (i.e. non-HHBC-only).
  static constexpr Flags kInternalFlags =
    Flags::HasUnpack |
    Flags::HasGenerics |
    Flags::LockWhileUnwinding |
    Flags::SkipRepack |
    Flags::SkipCoeffectsCheck |
    Flags::EnforceMutableReturn |
    Flags::EnforceReadonlyThis |
    Flags::HasNamedArgs;

  explicit FCallArgsBase(Flags flags, uint32_t numArgs, uint32_t numRets)
    : numArgs(numArgs)
    , numRets(numRets)
    , flags(flags)
  {
    assertx(!(flags & ~kInternalFlags));
  }
  bool hasUnpack() const { return flags & Flags::HasUnpack; }
  bool hasGenerics() const { return flags & Flags::HasGenerics; }
  bool lockWhileUnwinding() const { return flags & Flags::LockWhileUnwinding; }
  bool skipRepack() const { return flags & Flags::SkipRepack; }
  bool skipCoeffectsCheck() const { return flags & Flags::SkipCoeffectsCheck; }
  bool enforceMutableReturn() const {
    return flags & Flags::EnforceMutableReturn;
  }
  bool enforceReadonlyThis() const {
    return flags & Flags::EnforceReadonlyThis;
  }
  uint32_t numInputs() const {
    return numArgs + (hasUnpack() ? 1 : 0) + (hasGenerics() ? 1 : 0);
  }
  uint32_t numArgs;
  uint32_t numRets;
  Flags flags;

  template <typename SerDe> static FCallArgsBase makeForSerde(SerDe& sd) {
    static_assert(SerDe::deserializing);
    uint32_t numArgs;
    uint32_t numRets;
    Flags flags;
    sd(numArgs)(numRets)(flags);
    return FCallArgsBase{flags, numArgs, numRets};
  }

  template <typename SerDe> void serde(SerDe& sd) {
    static_assert(!SerDe::deserializing);
    sd(numArgs)(numRets)(flags);
  }
};

struct FCallArgs : FCallArgsBase {
  explicit FCallArgs(Flags flags, uint32_t numArgs, uint32_t numRets,
                     const uint8_t* inoutArgs, const uint8_t* readonlyArgs,
                     const Id namedArgNames, Offset asyncEagerOffset,
                     const StringData* context)
    : FCallArgsBase(flags, numArgs, numRets)
    , asyncEagerOffset(asyncEagerOffset)
    , inoutArgs(inoutArgs)
    , readonlyArgs(readonlyArgs)
    , namedArgNames(namedArgNames)
    , context(context) {
    assertx(IMPLIES(inoutArgs != nullptr || readonlyArgs != nullptr,
                    numArgs != 0));
    if (readonlyArgs && !anyReadonly(readonlyArgs, numArgs)) {
      readonlyArgs = nullptr;
    }
  }
  static bool isReadonlyArg(const uint8_t* readonlyArgs, uint32_t i) {
    assertx(readonlyArgs != nullptr);
    return readonlyArgs[i / 8] & (1 << (i % 8));
  }
  static bool anyReadonly(const uint8_t* readonlyArgs, uint32_t numArgs) {
    assertx(readonlyArgs != nullptr);
    for (size_t i = 0; i < numArgs; ++i) {
      if (isReadonlyArg(readonlyArgs, i)) return true;
    }
    return false;
  }
  bool enforceInOut() const { return inoutArgs != nullptr; }
  bool isInOut(uint32_t i) const {
    assertx(enforceInOut());
    return inoutArgs[i / 8] & (1 << (i % 8));
  }
  bool enforceReadonly() const {
    assertx(IMPLIES(readonlyArgs != nullptr,
                    anyReadonly(readonlyArgs, numArgs)));
    return readonlyArgs != nullptr;
  }
  bool isReadonly(uint32_t i) const {
    assertx(enforceReadonly());
    return isReadonlyArg(readonlyArgs, i);
  }
  FCallArgs withGenerics() const {
    assertx(!hasGenerics());
    return FCallArgs(
      static_cast<Flags>(flags | Flags::HasGenerics),
      numArgs, numRets, inoutArgs, readonlyArgs,
      namedArgNames, asyncEagerOffset, context);
  }
  bool hasNamedArgs() const {
    return namedArgNames != kInvalidId;
  }
  Offset asyncEagerOffset;
  const uint8_t* inoutArgs;
  const uint8_t* readonlyArgs;
  Id namedArgNames;
  const StringData* context;
};

static_assert(1 << FCallArgs::kFirstNumArgsBit == FCallArgsFlags::NumArgsStart, "");

using PrintLocal = std::function<std::string(int32_t local)>;
std::string show(const IterArgs&, PrintLocal);

std::string show(const LocalRange&);
std::string show(uint32_t numArgs, const uint8_t* boolVecArgs);
std::string show(const FCallArgsBase&, const uint8_t* inoutArgs,
                 const uint8_t* readonlyArgs, Id namedArgNameId,
                 std::string asyncEagerLabel, const StringData* ctx);

/*
 * Variable-size immediates are implemented as follows: To determine which size
 * the immediate is, examine the first byte where the immediate is expected,
 * and examine its high-order bit.  If it is zero, it's a 1-byte immediate
 * and the byte is the value. Otherwise, it's 4 bytes, and bits 8..31 must be
 * logical-shifted to the right by one to get rid of the flag bit.
 *
 * The types in this macro for BLA, SLA, and VSA are meaningless since they
 * are never read out of ArgUnion (they use ImmVector).
 *
 * There are several different local immediate types:
 *   - LA immediates are for bytecodes that only require the TypedValue* to
 *     perform their operation.
 *   - ILA immediates are used by bytecodes that need both the TypedValue* and
 *     the slot index to implement their operation.  This could be used by
 *     opcodes that print an error message including this slot info.
 *   - NLA immediates are used by bytecodes that need both the TypedValue* and
 *     the name of the local to be implemented.  This is commonly used for
 *     ops that raise warnings for undefined local uses.
 *
 * ArgTypes and their various decoding helpers should be kept in sync with the
 * `hhx' bytecode inspection GDB command.
 */
#define ARGTYPES                                                               \
  ARGTYPE(NA,     void*)         /* unused */                                  \
  ARGTYPEVEC(BLA, Offset)        /* Bytecode offset vector immediate */        \
  ARGTYPEVEC(SLA, Id)            /* String id/offset pair vector */            \
  ARGTYPE(IVA,    uint32_t)      /* Variable size: 8 or 32-bit uint */         \
  ARGTYPE(I64A,   int64_t)       /* 64-bit Integer */                          \
  ARGTYPE(LA,     int32_t)       /* Local: 8 or 32-bit int */                  \
  ARGTYPE(NLA,    NamedLocal)    /* Local w/ name: 2x 8 or 32-bit int */       \
  ARGTYPE(ILA,    int32_t)       /* Local w/ ID: 8 or 32-bit int */            \
  ARGTYPE(IA,     int32_t)       /* Iterator ID: 8 or 32-bit int */            \
  ARGTYPE(DA,     double)        /* Double */                                  \
  ARGTYPE(SA,     Id)            /* Static string ID */                        \
  ARGTYPE(AA,     Id)            /* Static array ID */                         \
  ARGTYPE(RATA,   RepoAuthType)  /* Statically inferred RepoAuthType */        \
  ARGTYPE(BA,     Offset)        /* Bytecode offset */                         \
  ARGTYPE(OA,     unsigned char) /* Sub-opcode, untyped */                     \
  ARGTYPE(KA,     MemberKey)     /* Member key: local, stack, int, str */      \
  ARGTYPE(LAR,    LocalRange)    /* Contiguous range of locals */              \
  ARGTYPE(ITA,    IterArgs)      /* Iterator arguments */                      \
  ARGTYPE(FCA,    FCallArgs)     /* FCall arguments */                         \
  ARGTYPEVEC(VSA, Id)            /* Vector of static string IDs */

enum ArgType {
#define ARGTYPE(name, type) name,
#define ARGTYPEVEC(name, type) name,
  ARGTYPES
#undef ARGTYPE
#undef ARGTYPEVEC
};

union ArgUnion {
  ArgUnion() : u_LA{0} {}
  uint8_t bytes[0];
#define ARGTYPE(name, type) type u_##name;
#define ARGTYPEVEC(name, type) type u_##name;
  ARGTYPES
#undef ARGTYPE
#undef ARGTYPEVEC
};

enum FlavorDesc {
  NOV,  // None
  CV,   // TypedValue
  UV,   // Uninit
  CUV,  // TypedValue, or Uninit argument
};

enum InstrFlags {
  /* No flags. */
  NF = 0x0,

  /* Terminal: next instruction is not reachable via fall through or the callee
   * returning control. This includes instructions like Throw that always throw
   * exceptions. */
  TF = 0x1,

  /* Control flow: If this instruction finishes executing (doesn't throw an
   * exception), vmpc() is not guaranteed to point to the next instruction in
   * the bytecode stream. This does not take VM reentry into account, as that
   * operation is part of the instruction that performed the reentry, and does
   * not affect what vmpc() is set to after the instruction completes. */
  CF = 0x2,

  /* Shorthand for common combinations. */
  CF_TF = (CF | TF),
};

inline bool isPre(IncDecOp op) {
  return op == IncDecOp::PreInc || op == IncDecOp::PreDec;
}

inline bool isInc(IncDecOp op) {
  return op == IncDecOp::PreInc || op == IncDecOp::PostInc;
}

constexpr uint32_t kMaxConcatN = 4;

#define O(...) + 1
constexpr size_t Op_count = OPCODES;
#undef O

enum class Op : std::conditional<Op_count <= 256, uint8_t, uint16_t>::type {
#define O(name, ...) name,
  OPCODES
#undef O
};

/*
 * Also put Op* in the enclosing namespace, to avoid having to change every
 * existing usage site of the enum values.
 */
#define O(name, ...) UNUSED auto constexpr Op##name = Op::name;
  OPCODES
#undef O

inline constexpr bool operator<(Op a, Op b) { return size_t(a) < size_t(b); }
inline constexpr bool operator>(Op a, Op b) { return size_t(a) > size_t(b); }
inline constexpr bool operator<=(Op a, Op b) {
  return size_t(a) <= size_t(b);
}
inline constexpr bool operator>=(Op a, Op b) {
  return size_t(a) >= size_t(b);
}

constexpr bool isValidOpcode(Op op) {
  return size_t(op) < Op_count;
}

inline MOpMode getQueryMOpMode(QueryMOp op) {
  switch (op) {
    case QueryMOp::CGet:  return MOpMode::Warn;
    case QueryMOp::CGetQuiet:
    case QueryMOp::Isset: return MOpMode::None;
    case QueryMOp::InOut: return MOpMode::InOut;
  }
  always_assert(false);
}

#define HIGH_OPCODES \
  O(FuncPrologue) \
  O(TraceletGuard)

enum HighOp {
  OpHighStart = Op_count-1,
#define O(name) Op##name,
  HIGH_OPCODES
#undef O
};

struct StrVecItem {
  Id str;
  Offset dest;
};

struct ImmVector {
  explicit ImmVector() : m_start(0) {}

  explicit ImmVector(const uint8_t* start,
                     int32_t length,
                     int32_t numStack)
    : m_length(length)
    , m_numStack(numStack)
    , m_start(start)
  {}

  bool isValid() const { return m_start != 0; }

  const int32_t* vec32() const {
    return reinterpret_cast<const int32_t*>(m_start);
  }
  folly::Range<const int32_t*> range32() const {
    auto base = vec32();
    return {base, base + size()};
  }
  const StrVecItem* strvec() const {
    return reinterpret_cast<const StrVecItem*>(m_start);
  }

  /*
   * Returns the length of the immediate vector in bytes (for M
   * vectors) or elements (for switch vectors)
   */
  int32_t size() const { return m_length; }

  /*
   * Returns the number of elements on the execution stack that this vector
   * will need to access.
   */
  int numStackValues() const { return m_numStack; }

private:
  int32_t m_length;
  int32_t m_numStack;
  const uint8_t* m_start;
};

// Must be an opcode that actually has an ImmVector.
ImmVector getImmVector(PC opcode);

// Some decoding helper functions.
int numImmediates(Op opcode);
ArgType immType(Op opcode, int idx);
bool hasImmVector(Op opcode);
int instrLen(PC opcode);

PC skipCall(PC pc);
PC asyncEagerReturnTarget(const Func* caller, Offset callOff);

/*
 * The returned struct has normalized variable-sized immediates. u must be
 * provided unless you know that the immediate is not of type KA.
 *
 * Don't use with RATA immediates.
 */
ArgUnion getImm(PC opcode, int idx, const Unit* u = nullptr);

// Don't use this with variable-sized immediates!
ArgUnion* getImmPtr(PC opcode, int idx);

void staticStreamer(const TypedValue* tv, std::string& out);
std::string staticStreamer(const TypedValue* tv);

std::string instrToString(PC it, const Func* f);
std::string instrToString(PC it, Either<const Func*, const FuncEmitter*> f,
                          Either<const Unit*, const UnitEmitter*> u);

void staticArrayStreamer(const ArrayData*, std::string&);
std::string staticArrayStreamer(const ArrayData* ad);

/*
 * Convert subopcodes or opcodes into strings.
 */
const char* opcodeToName(Op op);
const char* subopToName(InitPropOp);
const char* subopToName(IsTypeOp);
const char* subopToName(FatalOp);
const char* subopToName(CollectionType);
const char* subopToName(SetOpOp);
const char* subopToName(IncDecOp);
const char* subopToName(BareThisOp);
const char* subopToName(OODeclExistsOp);
const char* subopToName(ObjMethodOp);
const char* subopToName(SwitchKind);
const char* subopToName(MOpMode);
const char* subopToName(QueryMOp);
const char* subopToName(SetRangeOp);
const char* subopToName(TypeStructResolveOp);
const char* subopToName(VerifyRetKind);
const char* subopToName(TypeStructEnforceKind);
const char* subopToName(AsTypeStructExceptionKind);
const char* subopToName(ContCheckOp);
const char* subopToName(SpecialClsRef);
const char* subopToName(ClassGetCMode);
const char* subopToName(ReadonlyOp);

/*
 * Returns true iff the given SubOp is in the valid range for its type.
 */
template<class Subop>
bool subopValid(Subop);

/*
 * Try to parse a string into a subop name of a given type.
 *
 * Returns std::nullopt if the string is not recognized as that type of
 * subop.
 */
template<class SubOpType>
Optional<SubOpType> nameToSubop(const char*);

using OffsetList = std::vector<Offset>;

// returns absolute address of targets, or nothing if instruction
// cannot jump
OffsetList instrJumpTargets(PC instrs, Offset pos);

/*
 * Returns the set of bytecode offsets for the instructions that may
 * be executed immediately after opc.
 */
using OffsetSet = hphp_hash_set<Offset>;
OffsetSet instrSuccOffsets(PC opc, const Func* func);

/*
 * Some CF instructions can be treated as non-CF instructions for most analysis
 * purposes, such as bytecode verification and HHBBC. These instructions change
 * vmpc() to point somewhere in a different function, but the runtime
 * guarantees that if execution ever returns to the original frame, it will be
 * at the location immediately following the instruction in question. This
 * creates the illusion that the instruction fell through normally to the
 * instruction after it, within the context of its execution frame.
 *
 * The canonical example of this behavior are the FCall* instructions, so we use
 * "non-call control flow" to describe the set of CF instruction that do not
 * exhibit this behavior. This function returns true if `opcode' is a non-call
 * control flow instruction.
 */
bool instrIsNonCallControlFlow(Op opcode);

bool instrAllowsFallThru(Op opcode);

constexpr InstrFlags instrFlagsData[] = {
#define O(unusedName, unusedImm, unusedPop, unusedPush, flags) flags,
  OPCODES
#undef O
};

constexpr InstrFlags instrFlags(Op opcode) {
  return instrFlagsData[size_t(opcode)];
}

constexpr bool instrIsControlFlow(Op opcode) {
  return (instrFlags(opcode) & CF) != 0;
}

constexpr bool isAwait(Op opcode) {
  return
    opcode == Op::Await ||
    opcode == Op::AwaitAll ||
    opcode == Op::AwaitLowPri;
}

constexpr bool isUnconditionalJmp(Op opcode) {
  return opcode == Op::Jmp;
}

constexpr bool isConditionalJmp(Op opcode) {
  return opcode == Op::JmpZ || opcode == Op::JmpNZ;
}

constexpr bool isInterceptableJmp(Op opcode) {
  return opcode == Op::Enter;
}

constexpr bool isJmp(Op opcode) {
  return
    opcode == Op::Enter ||
    opcode == Op::Jmp   ||
    opcode == Op::JmpZ  ||
    opcode == Op::JmpNZ;
}

constexpr bool isObjectConstructorOp(Op opcode) {
  return
    opcode == Op::NewObj ||
    opcode == Op::NewObjD ||
    opcode == Op::NewObjS;
}

constexpr bool isArrLikeConstructorOp(Op opcode) {
  return
    opcode == Op::Dict ||
    opcode == Op::Keyset ||
    opcode == Op::Vec ||
    opcode == Op::NewDictArray ||
    opcode == Op::NewStructDict ||
    opcode == Op::NewVec ||
    opcode == Op::NewKeysetArray;
}

constexpr bool isArrLikeCastOp(Op opcode) {
  return
    opcode == Op::CastVec ||
    opcode == Op::CastDict ||
    opcode == Op::CastKeyset;
}

constexpr bool isComparisonOp(Op opcode) {
  return
    opcode == Op::Cmp ||
    opcode == Op::Eq ||
    opcode == Op::Neq ||
    opcode == Op::Gt ||
    opcode == Op::Gte ||
    opcode == Op::Lt ||
    opcode == Op::Lte ||
    opcode == Op::Same ||
    opcode == Op::NSame;
}

constexpr bool isFCallClsMethod(Op opcode) {
  return
    opcode == OpFCallClsMethodM ||
    opcode == OpFCallClsMethodD ||
    opcode == OpFCallClsMethodSD;
}

constexpr bool isFCallFunc(Op opcode) {
  return
    opcode == OpFCallFunc ||
    opcode == OpFCallFuncD;
}

constexpr bool isFCallObjMethod(Op opcode) {
  return
    opcode == OpFCallObjMethod ||
    opcode == OpFCallObjMethodD;
}

constexpr bool isFCall(Op opcode) {
  return
    opcode == OpFCallCtor ||
    isFCallClsMethod(opcode) ||
    isFCallFunc(opcode) ||
    isFCallObjMethod(opcode);
}

constexpr bool isRet(Op op) {
  return op == OpRetC || op == OpRetCSuspended || op == OpRetM;
}

constexpr bool isReturnish(Op op) {
  return isRet(op) || op == Op::NativeImpl;
}

constexpr bool isSwitch(Op op) {
  return op == OpSwitch || op == OpSSwitch;
}

constexpr bool isTypeAssert(Op op) {
  return op == OpAssertRATL || op == OpAssertRATStk;
}

constexpr bool isIteratorControlFlow(Op op) {
  return op == Op::IterInit || op == Op::IterNext;
}

constexpr bool isIteratorBaseAccess(Op op) {
  switch (op) {
    case Op::IterGetKey:
    case Op::IterGetValue:
    case Op::IterSetValue:
    case Op::IterInit:
    case Op::IterNext:
      return true;

    default:
      return false;
  }
}

inline bool isMemberBaseOp(Op op) {
  switch (op) {
    case Op::BaseSC:
    case Op::BaseL:
    case Op::BaseC:
    case Op::BaseH:
      return true;

    default:
      return false;
  }
}

inline bool isMemberDimOp(Op op) {
  return op == Op::Dim;
}

inline bool isMemberFinalOp(Op op) {
  switch (op) {
    case Op::QueryM:
    case Op::SetM:
    case Op::SetRangeM:
    case Op::IncDecM:
    case Op::SetOpM:
    case Op::UnsetM:
      return true;

    default:
      return false;
  }
}

inline bool isMemberOp(Op op) {
  return isMemberBaseOp(op) || isMemberDimOp(op) || isMemberFinalOp(op);
}

inline MOpMode finalMemberOpMode(Op op) {
  switch(op){
    case Op::SetM:
    case Op::SetRangeM:
    case Op::IncDecM:
    case Op::SetOpM:
      return MOpMode::Define;
    case Op::UnsetM:
      return MOpMode::Unset;
    case Op::QueryM:
      return MOpMode::None;
    default:
      always_assert_flog(
        false, "Unknown final member op {}", opcodeToName(op)
      );
  }
}

// true if the opcode body can set pc=0 to halt the interpreter.
constexpr bool instrCanHalt(Op op) {
  return isAwait(op) || op == OpRetC || op == OpNativeImpl ||
         op == OpCreateCont || op == OpYield || op == OpYieldK ||
         op == OpRetM || op == OpRetCSuspended;
}

int instrNumPops(PC opcode);
int instrNumPushes(PC opcode);
FlavorDesc instrInputFlavor(PC op, uint32_t idx);

}

//////////////////////////////////////////////////////////////////////

namespace std {
template<>
struct hash<HPHP::Op> {
  size_t operator()(HPHP::Op op) const {
    return HPHP::hash_int64(size_t(op));
  }
};
}

//////////////////////////////////////////////////////////////////////
