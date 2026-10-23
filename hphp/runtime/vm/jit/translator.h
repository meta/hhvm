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

#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/tracing.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/prologue-id.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/srckey.h"

#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/prof-src-key.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/trans-rec.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/types.h"

#include "hphp/util/hash-map.h"
#include "hphp/util/mutex.h"

#include <fmt/format.h>

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Class;
struct Func;

namespace Debug { struct DebugInfo; }

namespace jit {
///////////////////////////////////////////////////////////////////////////////

struct Block;
struct IRTranslator;
struct NormalizedInstruction;
struct ProfData;
namespace irgen { struct IRGS; }

constexpr uint32_t transCountersPerChunk = 1024 * 1024 / 8;


///////////////////////////////////////////////////////////////////////////////
// Translator exceptions.

struct TranslationFailedExc : std::runtime_error {
  TranslationFailedExc(const char* file, int line)
    : std::runtime_error(fmt::format("TranslationFailedExc @ {}:{}",
                                       file, line))
  {}
};

struct UnknownInputExc : std::runtime_error {
  UnknownInputExc(const char* file, int line)
    : std::runtime_error(fmt::format("UnknownInputExc @ {}:{}",
                                       file, line))
    , m_file(file)
    , m_line(line)
  {}

  const char* m_file; // must be static
  const int m_line;
};

#define punt() do { \
  throw TranslationFailedExc(__FILE__, __LINE__); \
} while(0)

#define throwUnknownInput() do { \
  throw UnknownInputExc(__FILE__, __LINE__); \
} while(0);


///////////////////////////////////////////////////////////////////////////////
// Translator auxiliary types.

using BlockIdToIRBlockMap = hphp_hash_map<RegionDesc::BlockId, Block*>;

/*
 * The information about the context a translation is occurring in.
 *
 * These fields are fixed for the whole translation.  Many objects in the JIT
 * need access to this.
 */
struct TransContext {
  /*
   * Data members.
   *
   * The contents of SrcKey are re-laid out to avoid func table lookups.
   */
  TransIDSet transIDs;  // May be empty if not for a real translation.
  int optIndex;
  TransKind kind{TransKind::Invalid};
  SrcKey initSrcKey;
  const RegionDesc* region{nullptr};
  const PackageInfo* packageInfo{nullptr};
  PrologueID pid;
};

inline tracing::Props traceProps(const TransContext& c) {
  return traceProps(c.initSrcKey.func())
    .add("sk", show(c.initSrcKey))
    .add("trans_kind", show(c.kind));
}

///////////////////////////////////////////////////////////////////////////////
// Stack information.

/*
 * Number of stack values popped by the opcode at `pc'.
 */
int64_t getStackPopped(PC pc);

/*
 * Number of stack values pushed by the opcode at `pc'.
 */
int64_t getStackPushed(PC pc);


///////////////////////////////////////////////////////////////////////////////
// Control flow information.

enum class ControlFlowInfo {
  None,
  ChangesPC,
  BreaksBB
};

/*
 * Return the ControlFlowInfo for `instr'.
 */
ControlFlowInfo opcodeControlFlowInfo(const Op op, bool inlining);

/*
 * Return true if the instruction can potentially set PC to point to something
 * other than the next instruction in the bytecode.
 */
bool opcodeChangesPC(const Op op);

/*
 * Return true if the instruction always breaks a tracelet.
 *
 * Most instructions that change PC will break the tracelet, though some do not
 * (e.g., FCall).
 */
bool opcodeBreaksBB(const Op op, bool inlining);

/*
 * Return true if the instruction doesn't care about the inner types.
 */
bool opcodeIgnoresInnerType(const Op op);

/*
 * Similar to opcodeBreaksBB but more strict.  We break profiling blocks after
 * any instruction that can side exit, including instructions with predicted
 * output, and before any control flow merge point.
 */
bool instrBreaksProfileBB(const NormalizedInstruction& inst);


///////////////////////////////////////////////////////////////////////////////
// Input and output information.

/*
 * Location and metadata for an instruction's input.
 */
struct InputInfo {
  explicit InputInfo(const Location& l) : loc(l) {}

  std::string pretty() const;

public:
  // Location specifier for the input.
  Location loc;

  // Never break the tracelet nor generate a guard on account of this input.
  bool dontGuard{false};
};

/*
 * Vector of InputInfo with a pretty-printer.
 */
struct InputInfoVec : public std::vector<InputInfo> {
  std::string pretty() const;
};

/*
 * Get input location info and flags for a NormalizedInstruction.
 */
InputInfoVec getInputs(const NormalizedInstruction&, SBInvOffset bcSPOff);

/*
 * Get the list of local output operands written by the `ni' instruction.
 */
jit::fast_set<uint32_t> getLocalOutputs(const NormalizedInstruction& ni);

/*
 * Return the index of op's local immediate.
 */
size_t localImmIdx(Op op);

/*
 * Return the local id referenced by the instruction immediate.
 */
uint32_t getLocalOperand(SrcKey sk);

namespace InstrFlags {
///////////////////////////////////////////////////////////////////////////////

/*
 * Type of the output(s) of an instruction.
 *
 * May be dependent on the input type.
 */
enum OutTypeConstraints {
  OutNull,
  OutNullUninit,
  OutString,
  OutStringImm,         // String w/ precisely known immediate.
  OutDouble,
  OutBoolean,
  OutBooleanImm,
  OutInt64,
  OutVec,
  OutVecImm,
  OutDict,
  OutDictImm,
  OutKeyset,
  OutKeysetImm,
  OutObject,
  OutResource,
  OutThisObject,        // Object from current environment
  OutFDesc,             // Blows away the current function desc

  OutUnknown,           // Not known at tracelet compile-time
  OutPredBool,          // Boolean value predicted to be True or False
  OutCns,               // Constant; may be known at compile-time

  OutSameAsInput1,      // type is the same as the first stack input
  OutSameAsInput2,      // type is the same as the second stack input
  OutModifiedInput2,    // type is the same as the second stack input, but
                        // counted and unspecialized
  OutModifiedInput3,    // type is the same as the third stack input, but
                        // counted and unspecialized
  OutCInput,            // type is C(input)
  OutCInputL,           // type is C(type) of local input

  OutArith,             // For Add, Sub, Mul
  OutBitOp,             // For BitAnd, BitOr, BitXor
  OutSetOp,             // For SetOpL
  OutIncDec,            // For IncDecL

  OutIsTypeL,           // output for IsTypeL instructions

  OutFunc,              // for function pointers
  OutFuncLike,          // For ResolveRFunc instruction
  OutClass,             // for class pointers
  OutClsMeth,           // For ClsMeth pointers
  OutClsMethLike,       // For ResolveRClsMeth* instructions
  OutLazyClass,         // For lazy classes

  OutEnumClassLabel,    // For enum class labels

  OutNone,
};

/*
 * Input codes indicate what an instruction reads, and some other things about
 * their behavior.
 *
 * The order these show up in the inputs vector is given in getInputs(), and is
 * relevant in a few cases (e.g. instructions taking both stack inputs and
 * MVectors).
 */
enum Operands {
  None            = 0,
  Stack3          = 1 << 0,
  Stack2          = 1 << 1,
  Stack1          = 1 << 2,
  StackIns1       = 1 << 3,  // Insert an element under top of stack
  Local           = 1 << 6,  // Writes to a local
  Iter            = 1 << 7,  // Iterator in imm[0]
  DontGuardStack1 = 1 << 9, // Dont force a guard on behalf of stack1 input
  DontGuardAny    = 1 << 11, // Dont force a guard for any input
  This            = 1 << 12, // Input to CheckThis
  StackN          = 1 << 13, // pop N cells from stack; n = imm[0].u_IVA
  BStackN         = 1 << 14, // consume N cells from stack for builtin call;
                             // n = imm[0].u_IVA
  StackI          = 1 << 15, // consume 1 cell at index imm[0].u_IVA
  MBase           = 1 << 16, // member operation base
  MKey            = 1 << 17, // member lookup key
  LocalRange      = 1 << 18, // read range of locals
  DontGuardBase   = 1 << 19, // Dont force a guard for the base
  StackI2         = 1 << 20, // Consume 1 cell at index imm_[1].u_IVA
  StackTop2 = Stack1 | Stack2,
  StackTop3 = Stack1 | Stack2 | Stack3,
};

Operands operator|(const Operands& l, const Operands& r);

///////////////////////////////////////////////////////////////////////////////
}

/*
 * Metadata describing an instruction's inputs and outputs, including their
 * number and constraints.
 *
 * Encoded in a sparse table in translator.cpp.
 */
struct InstrInfo {
  InstrFlags::Operands in;
  InstrFlags::Operands out;
  InstrFlags::OutTypeConstraints type; // How are outputs related to inputs?
};

/*
 * Get the InstrInfo for `op'.
 */
const InstrInfo& getInstrInfo(Op op);

/*
 * If this returns true, we dont generate guards for any of the inputs to this
 * instruction.
 *
 * This is used to avoid generating guards for interpreted instructions.
 */
bool dontGuardAnyInputs(const NormalizedInstruction& ni);

///////////////////////////////////////////////////////////////////////////////
// Other instruction information.

/*
* Some bytecodes are always no-ops but kept around for various reasons (mostly
* stack flavor safety).
 */
bool isAlwaysNop(const NormalizedInstruction& ni);

///////////////////////////////////////////////////////////////////////////////
// Completely unrelated functionality.

/*
 * Initialize the instruction table queried by the various functions in this
 * module.
 */
void initInstrInfo();

/*
 * The offset, in cells, of this location from the frame pointer.
 */
int locPhysicalOffset(int32_t localIndex);

/*
 * Take a NormalizedInstruction and turn it into a call to the appropriate
 * irgen emit functions. Assumes the bytecode marker has been updated.
 */
void translateInstr(irgen::IRGS&, const NormalizedInstruction&);

///////////////////////////////////////////////////////////////////////////////
}}

#define incl_HPHP_TRANSLATOR_INL_H_
#include "hphp/runtime/vm/jit/translator-inl.h"
#undef incl_HPHP_TRANSLATOR_INL_H_
