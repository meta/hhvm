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

#include "hphp/runtime/vm/jit/bc-marker.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/string-tag.h"
#include "hphp/runtime/vm/jit/type.h"

#include <boost/intrusive/list.hpp>
#include <fmt/format.h>
#include <folly/Range.h>

#include <cstdint>
#include <limits>
#include <string>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

struct Block;
struct Edge;
struct IRUnit;
struct SSATmp;

///////////////////////////////////////////////////////////////////////////////

/*
 * Bytecode-relative position context for an IRInstruction.
 *
 * These are threaded around to construct IRInstructions.
 */
struct BCContext {
  BCMarker marker;
  uint16_t iroff;
};

///////////////////////////////////////////////////////////////////////////////

/*
 * An HHIR instruction.
 *
 * IRInstructions must be arena-allocatable.  (Destructors are not called when
 * they come from IRUnit.)
 */
struct IRInstruction {
  enum Id { kTransient = 0xffffffff };

  /////////////////////////////////////////////////////////////////////////////

  /*
   * Create an IRInstruction for the opcode `op'.
   *
   * IRInstruction creation is usually done through IRUnit or IRBuilder rather
   * than directly via the constructor.
   */
  explicit IRInstruction(Opcode op,
                         BCContext bcctx,
                         Edge* edges = nullptr,
                         uint32_t numSrcs = 0,
                         SSATmp** srcs = nullptr);

  /*
   * Construct an IRInstruction as a deep copy of `inst', using `arena' to
   * allocate memory for its srcs/dsts.
   */
  explicit IRInstruction(Arena& arena, const IRInstruction* inst, Id id);

  /*
   * Stringify the instruction.
   */
  std::string toString() const;

  IRInstruction(const IRInstruction&) = delete;
  IRInstruction& operator=(const IRInstruction&) = delete;


  /////////////////////////////////////////////////////////////////////////////
  // Replacement.

  /*
   * Turn this instruction into the target instruction, without changing
   * stable fields (id, current block, list fields).
   *
   * The existing destination SSATmp(s) will continue to think they came from
   * this instruction, and the instruction's marker will not change.
   *
   * The target instruction may be transient---we'll clone anything we need to
   * keep, using IRUnit for any needed memory.
   *
   * Pre: other->isTransient() || numDsts() == other->numDsts()
   */
  void become(IRUnit&, const IRInstruction* other);

  /*
   * Replace the instruction in-place with a Nop.
   *
   * This is less general than become(), but it is fairly common, and doesn't
   * require access to an IRUnit.
   */
  void convertToNop();


  /////////////////////////////////////////////////////////////////////////////
  // OpcodeFlag helpers.                                                [const]

  /*
   * Helper accessors for the OpcodeFlag bits for this instruction.
   */
  bool hasDst() const;
  bool naryDst() const;
  bool consumesReferences() const;
  bool isTerminal() const;
  bool hasEdges() const;
  bool isPassthrough() const;
  bool isLayoutAgnostic() const;
  bool isLayoutPreserving() const;

  /*
   * Returns false if this instruction cannot raise an error under any
   * circumstances (and thus does not require a catch block). True
   * otherwise. This only consults the HHIR op of the instruction, so
   * will not change.
   */
  bool mayRaiseError() const;

  /*
   * Returns false if this instruction cannot raise an error, taking
   * into account its current sources. True otherwise. Since this can
   * consult the types of the current sources, the result can change
   * as types are reflowed.
   */
  bool mayRaiseErrorWithSources() const;

  /*
   * Returns true if the instruction may perform a sync of the VMRegs. This
   * implies that the instruction can load AVMRegState. Further, it can store
   * to AVMRegAny if the VMRegState is DIRTY, and load from AVMRegAny if the
   * VMRegState is CLEAN. mayRaise implies maySync.
   */
  bool maySyncVMRegsWithSources() const;

  /*
   * consumesReference covers two similar conditions. Either it decRefs
   * the input, or it transfers ownership of the input to a new location.
   */
  bool consumesReference(int srcNo) const;

  /*
   * mayMoveReference implies consumesReference. When
   * consumesReference is true, and mayMoveReference is false, this
   * instruction will definitely decRef its input. This is used by dce
   * to determine where it needs to insert DecRefs after killing a
   * consumesReference instruction.
   */
  bool mayMoveReference(int srcNo) const;

  /*
   * movesReference implies mayMoveReference, and guarantees that there
   * is no change to the refCount as a result of this instruction. Since
   * the new owner of the location is not specified, you can only assume
   * it lives until the next thing that might modify refcounts in an
   * unknown way. This is used by refcount opts to preserve the lower
   * bound of an aset across such an instruction (using an
   * unsupportedRef, which will be killed at the next instruction that
   * modifies refcounts in an untracked way).
   */
  bool movesReference(int srcNo) const;

  /*
   * Whether the dest produces a reference.
   */
  bool producesReference() const;
  
  /*
   * Whether the dest produces a completely new value with refcount 1
   */
  bool producesNewReference() const;

  /*
   * Get the src that is passed through.
   *
   * Currently, this is always src(0).
   */
  SSATmp* getPassthroughValue() const;


  /////////////////////////////////////////////////////////////////////////////
  // Opcode, marker, and type.

  /*
   * The ID assigned by the owning IRUnit.
   *
   * The instruction ID is stable and useful as an array index.  If the
   * Instruction is not arena-allocated by an IRUnit, the `m_id' field will be
   * kTransient (in which case this function should not be called).
   */
  uint32_t id() const;

  /*
   * Return true if the instruction is in a transient state.
   *
   * "Transient" means that the instruction is allocated on the stack and we
   * haven't yet committed to including it in the IRUnit's CFG.
   */
  bool isTransient() const;

  /*
   * The index of this instruction relative to its BCMarker.
   */
  uint16_t iroff() const;

  /*
   * Whether the instruction has one among a variadic list of opcodes.
   */
  template<typename... Args>
  bool is(Opcode op, Args&&... args) const;
  bool is() const;

  /*
   * Get or set the opcode of the instruction.
   */
  Opcode op() const;
  void setOpcode(Opcode);

  /*
   * Get or set the BCMarker of the instruction.
   */
  const BCMarker& marker() const;
  BCMarker& marker();

  /*
   * Get the BCContext of the instruction.
   *
   * This is only used for threading through to IRInstruction's constructor.
   */
  BCContext bcctx() const;

  /*
   * Return the current Func, or nullptr if not known (it should only
   * be unknown in test code).
   */
  const Func* func() const;

  /*
   * Return the current Func's cls(), or nullptr if not known.
   */
  const Class* ctx() const;

  /*
   * Check for, get, or set the instruction's optional type parameter.
   */
  bool hasTypeParam() const;
  Type typeParam() const;
  void setTypeParam(Type);


  /////////////////////////////////////////////////////////////////////////////
  // Srcs and dsts.

  /*
   * Initialize the source list for this IRInstruction.  We must not have
   * already had our sources initialized before this function is called.
   *
   * Memory for `srcs' is owned outside of this class and must outlive it.
   */
  void initializeSrcs(uint32_t numSrcs, SSATmp** srcs);

  /*
   * Number of srcs/dsts.
   */
  uint32_t numSrcs() const;
  uint32_t numDsts() const;

  /*
   * Return the ith src of this instruction.
   */
  SSATmp* src(uint32_t i) const;

  /*
   * Return the singular dst for this instruction.
   *
   * @requires: !naryDst()
   */
  SSATmp* dst() const;

  /*
   * Returns the ith dst of this instruction.
   *
   * If the instruction has no dsts, dst(0) will return nullptr, and if the
   * instruction is not NaryDst, dst(0) will return the single dst.  Otherwise,
   * it returns the first variadic dst.
   */
  SSATmp* dst(uint32_t i) const;

  /*
   * Returns the index where the given tmp lies in this instruction's
   * dst list, or numDsts() if it is not present.
   */
  uint32_t findDst(SSATmp* dst) const;

  /*
   * Get the srcs/dsts as a folly::Range.
   */
  folly::Range<SSATmp**> srcs() const;
  folly::Range<SSATmp**> dsts();
  folly::Range<SSATmp* const *> dsts() const;

  /*
   * Set a single src or all srcs.
   */
  void setSrc(uint32_t i, SSATmp* newSrc);
  void setSrcs(uint32_t numSrcs, SSATmp** newSrcs);
  void deleteSrc(uint32_t i);

  /*
   * Set the dsts, either as a single dst, or as `numDsts' dsts (if the
   * instruction has NaryDst).
   */
  void setDst(SSATmp* newDst);
  void setDst(SSATmp* newDst, uint32_t i);
  void setDsts(uint32_t numDsts, SSATmp** newDsts);
  void deleteDst(uint32_t i);


  /////////////////////////////////////////////////////////////////////////////
  // ExtraData.

  /*
   * Whether or not this instruction has an ExtraData.
   */
  bool hasExtra() const;

  /*
   * Return access to the ExtraData on this instruction, for the specified
   * opcode type.
   *
   * Pre: op() == opc
   */
  template<Opcode opc> const typename IRExtraDataType<opc>::type* extra() const;
  template<Opcode opc>       typename IRExtraDataType<opc>::type* extra();

  /*
   * Return access to ExtraData of type T.  Requires that
   * IRExtraDataType<opc>::type is T for this instruction's opcode.
   *
   * It's normally preferable to use the version of this function that takes
   * the opcode instead of this one.  This is for writing code that is supposed
   * to be able to handle multiple opcode types that share the same kind of
   * extra data.
   */
  template<typename T> const T* extra() const;
  template<typename T> T* extra();

  /*
   * Return the raw ExtraData pointer, for pretty-printing.
   */
  const IRExtraData* rawExtra() const;

  /*
   * Adjust IRExtraData's stack offsets by delta.
   */
  void updateStackOffsetsExtra(int32_t delta);

  /*
   * Set the ExtraData for this instruction.
   *
   * The lifetime of the ExtraData must outlast this IRInstruction (and any of
   * its clones).
   */
  void setExtra(IRExtraData*);

  /*
   * Clear the extra data pointer in a IRInstruction.
   *
   * Used during IRUnit::gen to avoid having dangling IRExtraData*'s into stack
   * memory.
   */
  void clearExtra();


  /////////////////////////////////////////////////////////////////////////////
  // Blocks and edges.

  /*
   * Get/set the block containing this instruction.
   *
   * The block is also where the taken edge is coming from, if this instruction
   * has a taken edge.
   */
  Block* block() const;
  void setBlock(Block* b);

  /*
   * Optional fall-through edge.
   *
   * If present, this instruction must also have a taken edge.
   */
  Block* next() const;
  Edge* nextEdge();
  void setNext(Block* b);

  /*
   * Optional control flow edge.
   *
   * If present, this instruction must be the last one in the block.
   */
  Block* taken() const;
  Edge* takenEdge();
  void setTaken(Block* b);

  /*
   * Whether this instruction is a control flow instruction, or ends a block.
   */
  bool isControlFlow() const;
  bool isBlockEnd() const;

  /*
   * Clear any outgoing edges this instruction has, if any.
   */
  void clearEdges();

  /*
   * Whether the next() edge is unreachable based on type information.
   */
  bool isNextEdgeUnreachable() const;

private:
  /*
   * Block/edge implementations.
   */
  Block* succ(int i) const;
  Edge* succEdge(int i);
  void setSucc(int i, Block* b);


  /////////////////////////////////////////////////////////////////////////////
  // Data members.

private:
  Type m_typeParam;  // garbage unless m_hasTypeParam is true
  Opcode m_op;
  uint16_t m_iroff{std::numeric_limits<uint16_t>::max()};
  uint16_t m_numSrcs;
  uint16_t m_numDsts : 15;
  bool m_hasTypeParam : 1;
  BCMarker m_marker;
  const Id m_id{kTransient};
  // 4-byte hole
  SSATmp** m_srcs{nullptr};
  union {
    SSATmp* m_dest;  // if HasDest
    SSATmp** m_dsts; // if NaryDest
  };
  Block* m_block{nullptr};  // what block owns this instruction
  Edge* m_edges{nullptr};   // outgoing edges, if this is a block-end
  IRExtraData* m_extra{nullptr};

public:
  boost::intrusive::list_member_hook<> m_listNode; // for InstructionList
};

///////////////////////////////////////////////////////////////////////////////

using IRInstructionHookOption = boost::intrusive::member_hook<
  IRInstruction,
  boost::intrusive::list_member_hook<>,
  &IRInstruction::m_listNode
>;

using InstructionList = boost::intrusive::list<IRInstruction,
                                               IRInstructionHookOption>;

///////////////////////////////////////////////////////////////////////////////

/*
 * Return the output type from a given IRInstruction.
 *
 * The destination type is always predictable from the types of the inputs, any
 * type parameters to the instruction, and the id of the dst.
 */
Type outputType(const IRInstruction*, int dstId = 0);

///////////////////////////////////////////////////////////////////////////////

/*
 * Return a type appropriate for $this given a function.
 */
Type thisTypeFromFunc(const Func* func);

/*
 * Return a type of $this/static::class context at the time of function call
 * (prologue and func entry).
 */
Type callCtxType(const Func* func);

///////////////////////////////////////////////////////////////////////////////

}

template <>
struct fmt::formatter<HPHP::jit::IRInstruction>
    : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::IRInstruction& inst, fmt::format_context& ctx)
      const {
    return fmt::formatter<std::string_view>::format(inst.toString(), ctx);
  }
};

#define incl_HPHP_VM_JIT_INSTRUCTION_INL_H_
#include "hphp/runtime/vm/jit/ir-instruction-inl.h"
#undef incl_HPHP_VM_JIT_INSTRUCTION_INL_H_
