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

#include "hphp/runtime/vm/verifier/check.h"

#include "hphp/runtime/vm/verifier/cfg.h"
#include "hphp/runtime/vm/verifier/util.h"
#include "hphp/runtime/vm/verifier/pretty.h"

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/type-structure-helpers.h"

#include "hphp/runtime/ext/extension.h"

#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/preclass-emitter.h"

#include <fmt/format.h>
#include <folly/Range.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <stdatomic.h>
#include <stdexcept>
#include <type_traits>

namespace HPHP {
namespace Verifier {

/**
 * Facts about a Func's current frame at various program points
 */
struct State {
  FlavorDesc* stk{};  // Evaluation stack.
  bool* iters{};      // defined/not-defined state of each iter var.
  int stklen{0};       // length of evaluation stack.
  bool mbr_live{false};    // liveness of member base register
  Optional<MOpMode> mbr_mode; // mode of member base register
  bool guaranteedThis; // whether $this is guaranteed to be non-null
  // immediately following BaseL or BaseH in a minstr sequence, when the base
  // was known to be Rx mutable
  bool mbrMustContainMutableLocalOrThis;
  bool afterDim; // has there been a Dim in the current minstr seqence
  // has there been an instruction with CheckROCOW or CheckMutROCOW
  bool afterCheckCOW;
  bool afterProp; // has there been a BaseSC/prop-flavored Dim
};

/**
 * Facts about a specific block.
 */
struct BlockInfo {
  State state_in;  // state at the start of the block
};

struct FuncChecker {
  FuncChecker(const FuncEmitter* func,
              const UnitEmitter* unit,
              ErrorMode mode,
              StringToStringTMap& createCls);
  ~FuncChecker();
  bool checkOffsets();
  bool checkFlow();
  bool checkDef();
  bool checkInfo();

 private:
  struct unknown_length : std::runtime_error {
    unknown_length()
      : std::runtime_error("Unknown instruction length")
    {}
  };

  bool checkEdge(Block* b, const State& cur, Block* t);
  bool checkBlock(State& cur, Block* b);
  bool checkSuccEdges(Block* b, State* cur);
  bool checkOffset(const char* name, Offset o, const char* regionName,
                   Offset base, Offset past, bool check_instrs = true);
  bool checkRegion(const char* name, Offset b, Offset p,
                   const char* regionName, Offset base, Offset past,
                   bool check_instrs = true);
  bool checkPrimaryBody(Offset base, Offset past);
  bool checkImmediates(const char* name, PC instr);
  bool checkImmVec(PC& pc, size_t elemSize);
#define ARGTYPE(name, type) bool checkImm##name(PC& pc, PC instr);
#define ARGTYPEVEC(name, type) ARGTYPE(name, type)
  ARGTYPES
#undef ARGTYPE
#undef ARGTYPEVEC
  bool checkOp(State*, PC, Op, Block*, PC);
  template<typename Subop> bool checkImmOAImpl(PC& pc, PC instr);
  bool checkMemberKey(State* cur, PC, Op);
  bool checkInputs(State* cur, PC, Block* b);
  bool checkOutputs(State* cur, PC, Block* b);
  bool checkReadonlyOp(State* cur, PC, Op);
  bool checkSig(PC pc, int len, const FlavorDesc* args, const FlavorDesc* sig);
  bool checkTerminal(State* cur, Op op, Block* b);
  bool checkIter(State* cur, PC pc);
  bool checkLocal(PC pc, int val);
  bool checkArray(PC pc, Id id);
  bool checkString(PC pc, Id id);
  bool checkExnEdge(State cur, Op op, Block* b);
  bool checkItersDead(const State& cur, Op op, Block* b, const char* info);
  bool readOnlyImmNotSupported(ReadonlyOp rop, Op op);
  void reportStkUnderflow(Block*, const State& cur, PC);
  void reportStkOverflow(Block*, const State& cur, PC);
  void reportStkMismatch(Block* b, Block* target, const State& cur);
  void reportEscapeEdge(Block* b, Block* s);
  std::string stateToString(const State& cur) const;
  std::string sigToString(int len, const FlavorDesc* sig) const;
  std::string stkToString(int len, const FlavorDesc* args) const;
  std::string iterToString(const State& cur) const;
  std::string slotsToString(const boost::dynamic_bitset<>&) const;
  void copyState(State* to, const State* from);
  void initState(State* s);
  const FlavorDesc* sig(PC pc);
  Offset offset(PC pc) const { return pc - m_func->bc(); }
  PC at(Offset off) const { return m_func->bc() + off; }
  int maxStack() const { return m_func->maxStackCells; }
  int numIters() const { return m_func->numIterators(); }
  int numLocals() const { return m_func->numLocals(); }
  int numParams() const { return m_func->params.size(); }
  const UnitEmitter* unit() const { return m_unit; }

 private:
  template<class... Args>
  void error(const char* fmt, Args&&... args) {
    verify_error(
      unit(),
      m_func,
      m_errmode == kThrow,
      fmt,
      std::forward<Args>(args)...
    );
  }
  template<class... Args>
  void ferror(fmt::format_string<Args...> fmt, Args&&... args) {
    verify_error(unit(), m_func, m_errmode == kThrow, "%s",
                 fmt::format(fmt, std::forward<Args>(args)...).c_str());
  }

 private:
  Arena m_arena;
  BlockInfo* m_info; // one per block
  const FuncEmitter* const m_func;
  const UnitEmitter* const m_unit;
  Graph* m_graph;
  Bits m_instrs;
  ErrorMode m_errmode;
  FlavorDesc* m_tmp_sig;
  Id m_last_rpo_id; // rpo_id of the last block visited
  StringToStringTMap& m_createCls;
};

const StaticString s_invoke("__invoke");

bool checkNativeFunc(const FuncEmitter* func, const UnitEmitter* unit, ErrorMode mode) {
  if (!Cfg::Eval::VerifySystemLibHasNativeImpl) {
    return true;
  }

  auto const funcname = func->name;
  auto const pc = func->pce();
  auto const clsname = pc ? pc->name() : nullptr;
  always_assert(unit->m_extension);
  auto const& info = Native::getNativeFunction(unit->m_extension->nativeFuncs(),
                                               funcname, clsname,
                                               func->attrs & AttrStatic);

  if (!info.ptr) {
    verify_error(unit, func, mode == kThrow,
      "<<__Native>> function %s%s%s is missing native impl.\n",
      clsname ? clsname->data() : "",
      clsname ? "::" : "",
      funcname->data()
    );
    return false;
  }

  if (func->isAsync) {
    verify_error(unit, func, mode == kThrow,
      "<<__Native>> function %s%s%s is declared async; <<__Native>> functions "
      "can return Awaitable<T>, but can not be declared async.\n",
      clsname ? clsname->data() : "",
      clsname ? "::" : "",
      funcname->data()
    );
    return false;
  }

  auto const message = Native::checkTypeFunc(info.sig, func);

  if (message) {
    auto const tstr = info.sig.toString(clsname ? clsname->data() : nullptr,
                                        funcname->data());

    verify_error(unit, func, mode == kThrow,
      "<<__Native>> function %s%s%s does not match C++ function "
      "signature (%s): %s\n",
      clsname ? clsname->data() : "",
      clsname ? "::" : "",
      funcname->data(),
      tstr.c_str(),
      message
    );
    return false;
  }

  return true;
}

bool checkFunc(const FuncEmitter* func,
               const UnitEmitter* unit,
               StringToStringTMap& createCls,
               ErrorMode mode) {
  if (mode == kVerbose) {
    pretty_print(func, unit, std::cout);
    if (!func->pce()) {
      printf("  FuncId %d\n", func->id());
    }
  }
  FuncChecker v{func, unit, mode, createCls};
  return v.checkInfo() &&
         v.checkDef() &&
         v.checkOffsets() &&
         v.checkFlow();
}

bool isInitialized(const State& state) {
  return state.stk;
}

// This fn returns false for a subset of ops that are guaranteed not to take
// the exn edge for the block they appear in. For other ops, it pessimistically
// assumes that this edge may get taken.
//
// This list should include instructions that are emitted for gotos or for iter
// or local scope cleanup blocks. "IterFree", "UnsetL", and "Jmp"
// are all used in these blocks. The "Ret*" ops are used for returns inside
// loop bodies, as well. If HHBBC determines that a block is unreachable,
// it will replace its contents with "String ...; Fatal", which we also include.
//
// Some of the ops here require justification:
//  - Fatal: this op throws, but not in a way that can be caught by exn
//  - Jmp: this op may check suprise flags and reenter, but reentry for suprise
//    checks is guarded by try-catch blocks, so we won't take the exn edge
//  - Ret*: these ops check surprise flags and the return hook may throw, but
//    before we run it, we set localsDecRefd, so we won't run the exn edge
//
// TODO(#57576776): Modify emitter + HHBBC to use JmpNS, then drop Jmp here.
// TODO(#57576993): Modify emitter to jump out of loops before returning,
//                  then drop the Ret* instructions here.
bool mayTakeExnEdges(Op op) {
  switch (op) {
    case Op::AssertRATL:
    case Op::AssertRATStk:
    case Op::Enter:
    case Op::Jmp:
    case Op::Fatal:
    case Op::IterFree:
    case Op::RetC:
    case Op::RetCSuspended:
    case Op::RetM:
    case Op::String:
    case Op::UnsetL:
    case Op::StaticAnalysisError:
      return false;
    default:
      return true;
  }
}

FuncChecker::FuncChecker(const FuncEmitter* f,
                         const UnitEmitter* unit,
                         ErrorMode mode,
                         StringToStringTMap& createCls)
: m_func(f)
, m_unit(unit)
, m_graph(0)
, m_instrs(m_arena, f->bcPos() + 1)
, m_errmode(mode)
, m_createCls(createCls)
{}

FuncChecker::~FuncChecker() {
  // if checkOffsets() is false, checkFlow() will never run so these will be
  // uninitialized
  if (!m_graph || !m_info) return;
  for (int i = 0; i < m_graph->block_count; i++) m_info[i].~BlockInfo();
}

// Needs to be a sorted map so we can divide funcs into contiguous sections.
using SectionMap = std::map<Offset,Offset>;

/**
 * Return the start offset of the nearest enclosing section.  Caller must
 * ensure that off is at least within the entire func's bytecode region.
 */
Offset findSection(SectionMap& sections, Offset off) {
  assertx(!sections.empty());
  SectionMap::iterator i = sections.upper_bound(off);
  --i;
  return i->first;
}

const StaticString s_reified("__Reified");
const StaticString s_reified_var("0ReifiedGenerics");
const StaticString s_coeffects_var("0Coeffects");

bool FuncChecker::checkInfo() {
  if (numLocals() > std::numeric_limits<uint16_t>::max()) {
    error("too many locals: %d", numLocals());
    return false;
  }
  if (numIters() > std::numeric_limits<uint16_t>::max()) {
    error("too many iterators: %d", numIters());
    return false;
  }

  auto const it = m_func->userAttributes.find(s_reified.get());
  auto const hasReifiedGenerics = it != m_func->userAttributes.end();

  if (hasReifiedGenerics) {
    auto const localId = m_func->lookupVarId(s_reified_var.get());
    if (localId != m_func->params.size()) {
      ferror("functions with reified generics must have the first non parameter"
             " local to be reified generics local");
      return false;
    }
  }
  if (!m_func->coeffectRules.empty() &&
      !(m_func->coeffectRules.size() == 1 &&
        m_func->coeffectRules[0].isGeneratorThis())) {
    auto const localId = m_func->lookupVarId(s_coeffects_var.get());
    if (localId != m_func->params.size() + (hasReifiedGenerics ? 1 : 0)) {
      ferror("functions with coeffect rules must have the first non parameter"
             " local (also after reified generics local) to be coeffects local");
      return false;
    }
  }
  return true;
}

/**
 * Make sure that internally special functions are properly defined.
 */
bool FuncChecker::checkDef() {
  auto const s = m_func->name->toCppString();
  if (s.compare("86pinit") == 0 ||
      s.compare("86sinit") == 0 ||
      s.compare("86linit") == 0 ||
      s.compare("86cinit") == 0) {
    if (!(m_func->attrs & AttrStatic)) {
      error("%s functions must be static\n", s.data());
      return false;
    }
  }
  return true;
}

/**
 * Make sure all offsets are in-bounds.  Offset 0 is unit->m_bc,
 * for all functions.  Jump instructions use an Offset relative to
 * the start of the jump instruction.
 */
bool FuncChecker::checkOffsets() {
  bool ok = true;
  assertx(m_func->bcPos() >= 0);
  Offset base = 0;
  Offset past = m_func->bcPos();
  checkRegion("func", base, past, "unit", 0, past, false);
  // Get instruction boundaries and check branches within primary body
  ok &= checkPrimaryBody(base, past);
  // DV entry points must be in the primary function body
  for (auto& param : m_func->params) {
    if (param.hasDefaultValue()) {
      ok &= checkOffset("dv-entry", param.funcletOff, "func body", base,
                        past);
    }
  }
  return ok;
}

/**
 * Scan instructions in the given range to find valid instruction
 * boundaries, and check that branches a) land on valid boundaries,
 * b) do not escape the range.
 */
bool FuncChecker::checkPrimaryBody(Offset base, Offset past) {
  bool ok = true;
  using BranchList = std::list<PC>;
  BranchList branches;
  PC bc = m_func->bc();
  // Find instruction boundaries and branch instructions.
  for (InstrRange i(at(base), at(past)); !i.empty();) {
    auto pc = i.popFront();
    auto const op = peek_op(pc);
    if (!checkImmediates("primary body", pc)) {
      ferror("checkImmediates failed for {} @ {}\n",
             opcodeToName(op), offset(pc));
      return false;
    }
    m_instrs.set(offset(pc));
    if (!instrJumpTargets(bc, offset(pc)).empty()) {
      if (op == OpSwitch && getImm(pc, 0).u_IVA == int(SwitchKind::Bounded)) {
        int64_t switchBase = getImm(pc, 1).u_I64A;
        int32_t len = getImmVector(pc).size();
        if (len <= 2) {
          error("Bounded switch must have a vector of length > 2 [%d:%d]\n",
                base, past);
        }
        if (switchBase > std::numeric_limits<int64_t>::max() - len + 2) {
          error("Overflow in Switch bounds [%d:%d]\n", base, past);
        }
      } else if (op == Op::SSwitch) {
        foreachSSwitchString(pc, [&](Id id) {
          ok &= checkString(pc, id);
        });
      }
      branches.push_back(pc);
    }
    if (i.empty()) {
      if (offset(pc + instrLen(pc)) != past) {
        error("Last instruction in primary body at %d overflows [%d:%d]\n",
              offset(pc), base, past);
        ok = false;
      }
      if ((instrFlags(op) & TF) == 0) {
        error("Last instruction in primary body is not terminal %d:%s\n",
              offset(pc), instrToString(pc, m_func, m_unit).c_str());
        ok = false;
      }
    }
  }
  // Check each branch target lands on a valid instruction boundary
  // within this region.
  for (auto const branch : branches) {
    auto const targets = instrJumpTargets(bc, offset(branch));
    for (auto const& target : targets) {
      ok &= checkOffset("branch target", target, "primary body", base, past);
      if (peek_op(branch) == Op::Enter && target == offset(branch)) {
        error("Enter may not have zero offset in %s\n", "primary body");
        ok = false;
      }
    }
  }
  return ok;
}

Id decodeId(PC* ppc) {
  Id id = *(Id*)*ppc;
  *ppc += sizeof(Id);
  return id;
}

Offset decodeOffset(PC* ppc) {
  Offset offset = *(Offset*)*ppc;
  *ppc += sizeof(Offset);
  return offset;
}

bool FuncChecker::checkLocal(PC pc, int k) {
  if (!(0 <= k && k < numLocals())) {
    error("invalid local variable id %d at Offset %d\n", k, offset(pc));
    return false;
  }
  return true;
}

bool FuncChecker::checkString(PC /*pc*/, Id id) {
  return id < unit()->numLitstrs();
}

bool FuncChecker::checkArray(PC /*pc*/, Id id) {
  return id < unit()->numArrays();
}

bool FuncChecker::checkImmVec(PC& pc, size_t elemSize) {
  auto const len = decode_iva(pc);
  if (len < 1) {
    error("invalid length of immediate vector %d at Offset %d\n",
          len, offset(pc));
    throw unknown_length{};
  }

  pc += len * elemSize;
  return true;
}

bool FuncChecker::checkImmBLA(PC& pc, PC const /*instr*/) {
  return checkImmVec(pc, sizeof(Offset));
}

bool FuncChecker::checkImmSLA(PC& pc, PC const /*instr*/) {
  return checkImmVec(pc, sizeof(Id) + sizeof(Offset));
}

bool FuncChecker::checkImmIVA(PC& pc, PC const instr) {
  auto const k = decode_iva(pc);
  switch (peek_op(instr)) {
    case Op::ConcatN:
      return k >= 2 && k <= kMaxConcatN;
    case Op::CombineAndResolveTypeStruct:
      return k >= 1;
    default:
      return true;
  }
}

bool FuncChecker::checkImmI64A(PC& pc, PC const /*instr*/) {
  pc += sizeof(int64_t);
  return true;
}

bool FuncChecker::checkImmLA(PC& pc, PC const instr) {
  auto ok = true;
  auto const k = decode_iva(pc);
  ok &= checkLocal(pc, k);
  switch (peek_op(instr)) {
    case Op::VerifyParamType:
    case Op::VerifyParamTypeTS:
      if (k >= numParams()) {
        error("invalid parameter id %d at %d\n", k, offset(instr));
        ok = false;
      }
      break;
    default:
      break;
  }
  return ok;
}

bool FuncChecker::checkImmNLA(PC& pc, PC const instr) {
  decode_iva(pc);
  return checkImmLA(pc, instr);
}

bool FuncChecker::checkImmILA(PC& pc, PC const instr) {
  return checkImmLA(pc, instr);
}

bool FuncChecker::checkImmIA(PC& pc, PC const instr) {
  auto const k = decode_iva(pc);
  if (!(0 <= k && k < numIters())) {
    error("invalid iterator variable id %d at %d\n", k, offset(instr));
    return false;
  }
  return true;
}

bool FuncChecker::checkImmDA(PC& pc, PC const /*instr*/) {
  pc += sizeof(double);
  return true;
}

bool FuncChecker::checkImmSA(PC& pc, PC const /*instr*/) {
  auto const id = decodeId(&pc);
  return checkString(pc, id);
}

bool FuncChecker::checkImmAA(PC& pc, PC const /*instr*/) {
  auto const id = decodeId(&pc);
  if (!checkArray(pc, id)) {
    error("invalid array id %d\n", id);
    return false;
  }
  return true;
}

bool FuncChecker::checkImmRATA(PC& pc, PC const /*instr*/) {
  // Nothing to check at the moment.
  pc += encodedRATSize(pc);
  return true;
}

bool FuncChecker::checkImmBA(PC& pc, PC const instr) {
  // we check branch offsets in checkPrimaryBody(). ignore here.
  assertx(!instrJumpTargets(m_func->bc(), offset(instr)).empty());
  pc += sizeof(Offset);
  return true;
}

bool FuncChecker::checkImmVSA(PC& pc, PC const /*instr*/) {
  auto const len = decode_iva(pc);
  if (len < 1 || len > ArrayData::MaxElemsOnStack) {
    error("invalid length of immedate VSA vector %d at offset %d\n",
          len, offset(pc));
    throw unknown_length{};
  }

  auto ok = true;
  for (int i = 0; i < len; i++) {
    auto const id = decodeId(&pc);
    ok &= checkString(pc, id);
  }
  return ok;
}

template <typename Subop>
bool FuncChecker::checkImmOAImpl(PC& pc, PC const /*instr*/) {
  auto const subop = decode_oa<Subop>(pc);
  if (!subopValid(subop)) {
    ferror("Invalid subop {}\n", size_t(subop));
    return false;
  }
  return true;
}

bool FuncChecker::checkImmKA(PC& pc, PC const instr) {
  static_assert(
      std::is_unsigned<typename std::underlying_type<MemberCode>::type>::value,
      "MemberCode is expected to be unsigned.");

  auto const mcode = decode_raw<MemberCode>(pc);
  if (mcode >= NumMemberCodes) {
    ferror("Invalid MemberCode {}\n", uint8_t{mcode});
    return false;
  }

  ReadonlyOp rop = ReadonlyOp::Any;
  auto ok = true;
  switch (mcode) {
    case MW:
      break;
    case MEL: case MPL: {
      decode_iva(pc);
      auto const loc = decode_iva(pc);
      ok &= checkLocal(pc, loc);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    }
    case MEC: case MPC:
      decode_iva(pc);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    case MEI:
      pc += sizeof(int64_t);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    case MET: case MPT: case MQT:
      auto const id = decode_raw<Id>(pc);
      ok &= checkString(pc, id);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
  }

  auto const op = peek_op(instr);
  switch (op) {
    case Op::Dim:
      if (rop == ReadonlyOp::Readonly) return readOnlyImmNotSupported(rop, op);
      break;
    case Op::QueryM:
      if (rop != ReadonlyOp::Mutable && rop != ReadonlyOp::Any) {
        return readOnlyImmNotSupported(rop, op);
      }
      break;
    case Op::SetM:
      if (rop != ReadonlyOp::Readonly && rop != ReadonlyOp::Any) {
        return readOnlyImmNotSupported(rop, op);
      }
      break;
    case Op::SetRangeM:
    case Op::IncDecM:
    case Op::SetOpM:
    case Op::UnsetM:
      if (rop != ReadonlyOp::Any) return readOnlyImmNotSupported(rop, op);
      break;
    default:
      always_assert(false);
      return false;
  }

  return ok;
}

bool FuncChecker::checkImmLAR(PC& pc, PC const instr) {
  auto ok = true;
  auto const range = decodeLocalRange(pc);
  for (auto i = uint32_t{0}; i < range.count; ++i) {
    ok &= checkLocal(instr, range.first + i);
  }
  return ok;
}

bool FuncChecker::checkImmITA(PC& pc, PC const instr) {
  auto ok = true;
  auto ita = decodeIterArgs(pc);
  if (!(0 <= ita.iterId && ita.iterId < numIters())) {
    error("invalid iterator variable id %d at %d\n", ita.iterId, offset(instr));
    ok = false;
  }
  return ok;
}

bool FuncChecker::checkImmFCA(PC& pc, PC const instr) {
  auto fca = decodeFCallArgs(peek_op(instr), pc, nullptr /* StringDecoder */);
  if (fca.numRets == 0) {
    ferror("FCall at {} must return at least one value\n", offset(instr));
    return false;
  }
  bool hasNamedArgNames = fca.namedArgNames != -1;
  TypedValue prevName;
  if (hasNamedArgNames) {
    if (!checkArray(pc, fca.namedArgNames)) {
      ferror("FCall at {} has invalid named arg names array id {}\n",
             offset(instr),
             fca.namedArgNames);
     return false;
    }
    auto names = m_unit->lookupArrayId(fca.namedArgNames);
    for (int64_t i = 0; i < names->size(); ++i) {
      auto curName = names->get(i);
      if (!tvIsString(curName)) {
        ferror("FCall at {} has non-string {}'th named arg name\n", offset(instr), i);
        return false;
      }
      if (i > 0) {
        auto curStr = curName.m_data.pstr;
        auto prevStr = prevName.m_data.pstr;
        if (curStr->compare(prevStr) <= 0) {
          ferror("FCall at {} expected {} to be lexicographically greater than {}\n",
                 offset(instr),
                 curStr->toCppString(),
                 prevStr->toCppString());
          return false;
        }
      }
      prevName = curName;
    }
  }
  return true;
}

/**
 * Check instruction and its immediates. Returns false if we can't continue
 * because we don't know the length of this instruction or one of the
 * immediates was invalid.
 */
bool FuncChecker::checkImmediates(const char* name, PC const instr) {
  auto pc = instr;

  auto const op = decode_op(pc);
  if (!isValidOpcode(op)) {
    error("Invalid opcode %d in section %s at offset %d\n",
          size_t(op), name, offset(instr));
    return false;
  }
  bool ok = true;
  try {
    switch (op) {
#define NA
#define checkImmOA(ty) checkImmOAImpl<ty>
#define ONE(a) ok &= checkImm##a(pc, instr)
#define TWO(a, b) ONE(a); ok &= checkImm##b(pc, instr)
#define THREE(a, b, c) TWO(a, b); ok &= checkImm##c(pc, instr)
#define FOUR(a, b, c, d) THREE(a, b, c); ok &= checkImm##d(pc, instr)
#define FIVE(a, b, c, d, e) FOUR(a, b, c, d); ok &= checkImm##e(pc, instr)
#define SIX(a, b, c, d, e, f) FIVE(a, b, c, d, e); ok &= checkImm##f(pc, instr)
#define O(name, imm, in, out, flags) case Op::name: imm; break;
      OPCODES
#undef NA
#undef checkOA
#undef ONE
#undef TWO
#undef THREE
#undef FOUR
#undef FIVE
#undef SIX
#undef O
    }
  } catch (const unknown_length&) {
    return false;
  }

  auto const expectPC = instr + instrLen(instr);
  if (pc != expectPC) {
    ferror("PC after decoding {} at {} is {} instead of expected {}\n",
           opcodeToName(op), fmt::ptr(instr), fmt::ptr(pc), fmt::ptr(expectPC));
    return false;
  }
  return ok;
}

static const char* stkflav(FlavorDesc f) {
  switch (f) {
  case NOV:  return "N";
  case CV:   return "C";
  case UV:   return "U";
  case CUV:  return "C|U";
  }
  not_reached();
}

static bool checkArg(FlavorDesc expect, FlavorDesc check) {
  if (expect == check) return true;

  switch (expect) {
    case CUV:  return check == CV || check == UV;
    default:   return false;
  }
}

bool FuncChecker::checkSig(PC pc, int len, const FlavorDesc* args,
                           const FlavorDesc* sig) {
  for (int i = 0; i < len; ++i) {
    if (!checkArg(sig[i], args[i])) {
      error("flavor mismatch at %d, got %s expected %s\n",
             offset(pc), stkToString(len, args).c_str(),
             sigToString(len, sig).c_str());
      return false;
    }
  }
  return true;
}

const FlavorDesc* FuncChecker::sig(PC pc) {
  static const FlavorDesc inputSigs[][kMaxHhbcImms] = {
  #define NOV { },
  #define CUMANY { },
  #define FCALL(nin, nobj) { nobj ? CV : UV },
  #define CMANY { },
  #define SMANY { },
  #define ONE(a) { a },
  #define TWO(a,b) { b, a },
  #define THREE(a,b,c) { c, b, a },
  #define FOUR(a,b,c,d) { d, c, b, a },
  #define FIVE(a,b,c,d,e) { e, d, c, b, a },
  #define SIX(a,b,c,d,e,f) { f, e, d, c, b, a },
  #define MFINAL { },
  #define C_MFINAL(n) { },
  #define O(name, imm, pop, push, flags) pop
    OPCODES
  #undef O
  #undef MFINAL
  #undef C_MFINAL
  #undef CUMANY
  #undef FCALL
  #undef CMANY
  #undef SMANY
  #undef SIX
  #undef FIVE
  #undef FOUR
  #undef THREE
  #undef TWO
  #undef ONE
  #undef NOV
  };
  switch (peek_op(pc)) {
  case Op::QueryM:
  case Op::IncDecM:
  case Op::UnsetM:
  case Op::SetM:
  case Op::SetOpM:
  case Op::SetRangeM:
    for (int i = 0, n = instrNumPops(pc); i < n; ++i) {
      m_tmp_sig[i] = CV;
    }
    return m_tmp_sig;
  case Op::FCallClsMethodM:
  case Op::FCallClsMethodD:
  case Op::FCallClsMethodSD:
  case Op::FCallCtor:
  case Op::FCallFunc:
  case Op::FCallFuncD:
  case Op::FCallObjMethod:
  case Op::FCallObjMethodD: {  // FCA..., FCALL, FCALL
    auto const fca = getImm(pc, 0).u_FCA;
    auto const numPops = instrNumPops(pc);
    assertx(fca.numRets != 0);
    auto idx = 0;
    for (int i = 0; i < fca.numRets - 1; ++i) m_tmp_sig[idx++] = UV;
    m_tmp_sig[idx++] = inputSigs[size_t(peek_op(pc))][0];
    m_tmp_sig[idx++] = UV;
    for (int i = 0; i < fca.numArgs; ++i) m_tmp_sig[idx++] = CV;
    if (fca.hasUnpack()) m_tmp_sig[idx++] = CV;
    if (fca.hasGenerics()) m_tmp_sig[idx++] = CV;
    assertx(idx == numPops || idx + 1 == numPops || idx + 2 == numPops);
    while (idx < numPops) m_tmp_sig[idx++] = CV;
    return m_tmp_sig;
  }
  case Op::CreateCl:  // TWO(IVA,SA),  CUMANY,   ONE(CV)
    for (int i = 0, n = instrNumPops(pc); i < n; ++i) {
      m_tmp_sig[i] = CUV;
    }
    return m_tmp_sig;
  case Op::NewStructDict:   // ONE(VSA),     SMANY,   ONE(CV)
  case Op::NewVec:          // ONE(IVA),     CMANY,   ONE(CV)
  case Op::NewKeysetArray:  // ONE(IVA),     CMANY,   ONE(CV)
  case Op::ConcatN:         // ONE(IVA),     CMANY,   ONE(CV)
  case Op::CombineAndResolveTypeStruct:
                            // ONE(IVA),     CMANY,   ONE(CV)
  case Op::RetM:            // ONE(IVA),     CMANY,   NA
    for (int i = 0, n = instrNumPops(pc); i < n; ++i) {
      m_tmp_sig[i] = CV;
    }
    return m_tmp_sig;
  default:
    return &inputSigs[size_t(peek_op(pc))][0];
  }
}

bool FuncChecker::checkMemberKey(State* cur, PC pc, Op op) {
  MemberCode mcode;

  switch(op){
    case Op::IncDecM:
    case Op::SetOpM:
    case Op::QueryM:  //THREE(IVA, OA, KA)
      decode_op(pc);
      decode_iva(pc);
      decode_byte(pc);
      mcode = static_cast<MemberCode>(decode_byte(pc));
      break;
    case Op::UnsetM:
    case Op::SetM:   //TWO(IVA, KA)
      decode_op(pc);
      decode_iva(pc);
      mcode = static_cast<MemberCode>(decode_byte(pc));
      break;
    case Op::SetRangeM:
      return true;

    default:
      always_assert(false);
      return false;
  }

  uint32_t iva = 0;
  ReadonlyOp rop = ReadonlyOp::Any;

  switch (mcode) {
    case MET: case MPT: case MQT: {
      auto const id = decode_raw<Id>(pc);
      if (!checkString(pc, id)) {
        error("Member Key in op %s contains an invalid string id\n",
              opcodeToName(op));
        return false;
      }
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    }

    case MEC: case MPC:
      iva = decode_iva(pc);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    case MEL: case MPL:
      decode_iva(pc);
      decode_iva(pc);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    case MEI:
      decode_raw<int64_t>(pc);
      rop = decode_oa<ReadonlyOp>(pc);
      break;
    case MW:
      break;
  }

  if (op == Op::UnsetM && !mcodeIsElem(mcode)) {
    ferror("UnsetM only supports elem-flavored member keys.\n");
    return false;
  }

  if (op == Op::SetM || op == Op::UnsetM) {
    if (!cur->afterCheckCOW) {
      if (!mcodeIsProp(mcode) && cur->afterProp) {
        ferror("Check(Mut)ROCOW must appear at least once for member-op"
          " sequences ending with an elem-flavored FinalOp.\n");
        return false;
      }
    } else if (mcodeIsProp(mcode)) {
      ferror("Check(Mut)ROCOW must only appear on the last prop access.\n");
      return false;
    }
  }

  if ((mcode == MemberCode::MEC || mcode == MemberCode::MPC) &&
      iva + 1 > cur->stklen) {
    MemberKey key{mcode, static_cast<int32_t>(iva), rop};
    error("Member Key %s in op %s has stack offset greater than stack"
          " depth %d\n", show(key).c_str(), opcodeToName(op), cur->stklen);
    return false;
  }

  return true;
}

bool FuncChecker::checkInputs(State* cur, PC pc, Block* b) {
  auto const numPops = instrNumPops(pc);
  if (numPops > cur->stklen) {
    reportStkUnderflow(b, *cur, pc);
    cur->stklen = 0;
    return false;
  }
  cur->stklen -= numPops;
  auto ok = checkSig(pc, numPops, &cur->stk[cur->stklen], sig(pc));
  auto const op = peek_op(pc);
  auto const need_live = isMemberDimOp(op) || isMemberFinalOp(op);
  auto const live_ok = need_live || isTypeAssert(op);
  if ((need_live && !cur->mbr_live) || (cur->mbr_live && !live_ok)) {
    error("Member base register %s coming into %s\n",
          cur->mbr_live ? "live" : "dead", opcodeToName(op));
    ok = false;
  }

  if (cur->mbr_live && isMemberOp(op)) {
    Optional<MOpMode> op_mode;
    auto new_pc = pc;
    if (op == Op::QueryM) {
      decode_op(new_pc);
      decode_iva(new_pc);
      op_mode = getQueryMOpMode(decode_oa<QueryMOp>(new_pc));
    } else if (isMemberFinalOp(op)) {
      op_mode = finalMemberOpMode(op);
    } else if (op == Op::Dim) {
      decode_op(new_pc);
      op_mode = decode_oa<MOpMode>(new_pc);
    }

    if (op_mode == MOpMode::InOut) {
      if (op_mode == MOpMode::InOut && op != Op::Dim && op != Op::QueryM) {
        error("Member instruction %s is incompatible with mode InOut\n",
              opcodeToName(op));
        ok = false;
      } else {
        auto const c = static_cast<MemberCode>(decode_byte(new_pc));
        if (!mcodeIsElem(c)) {
          error("Member instruction with mode InOut and incompatible member "
                "code %s, must be Elem\n", memberCodeString(c));
          ok = false;
        }
      }
    }

    if(cur->mbr_mode && cur->mbr_mode != op_mode) {
      if (cur->mbr_mode != MOpMode::Warn ||
        (op_mode != MOpMode::InOut && op_mode != MOpMode::None)) {
        error("Member base register mode at %s is %s when it should be %s\n",
              opcodeToName(op),
              op_mode ? subopToName(op_mode.value()) : "Unknown",
              subopToName(cur->mbr_mode.value()));
        ok = false;
      }
    }

    cur->mbr_mode = op_mode;
  }

  return ok;
}

bool FuncChecker::checkItersDead(const State& cur, Op op,
                                 Block* b, const char* info) {
  auto ok = true;
  for (auto i = 0; i < numIters(); i++) {
    ok &= !cur.iters[i];
  }
  if (ok) return true;

  auto liveIters = std::vector<int>{};
  for (auto i = 0; i < numIters(); i++) {
    if (cur.iters[i]) liveIters.push_back(i);
  }
  error("Block B%d %s at op %s with live iterators: [%s]\n",
        b->id, info, opcodeToName(op), folly::join(", ", liveIters).data());
  return false;
}

bool FuncChecker::checkTerminal(State* cur, Op op, Block* b) {
  if (!isRet(op)) return true;
  auto ok = checkItersDead(*cur, op, b, "terminates");
  if (cur->stklen != 0) {
    error("stack depth must equal 0 after Ret*; got %d\n", cur->stklen);
    ok = false;
  }
  return ok;
}

// Check that the initialization state of the iterator referenced by the current
// iterator instruction is valid. For *IterInit, it must not already be
// initialized; for *IterNext and *IterFree, it must be initialized.
bool FuncChecker::checkIter(State* cur, PC const pc) {
  bool ok = true;
  auto op = peek_op(pc);
  auto const id = getIterId(pc);
  if (op == Op::IterInit) {
    if (cur->iters[id]) {
      error("IterInit* <%d> trying to double-initialize\n", id);
      ok = false;
    }
  } else {
    if (!cur->iters[id]) {
      error("Cannot access un-initialized iter %d\n", id);
      ok = false;
    }
    if (op == Op::IterFree) {
      cur->iters[id] = false;
    }
  }
  return ok;
}

/* Returns the set of local IDs that appear in the op's immediates */
std::set<int> localIds(Op op, PC pc) {
  std::set<int> result;
  switch (op) {
#define NA
#define ONE(a) a(0)
#define TWO(a, b) ONE(a) b(1)
#define THREE(a, b, c) TWO(a, b) c(2)
#define FOUR(a, b, c, d) THREE(a, b, c) d(3)
#define FIVE(a, b, c, d, e) FOUR(a, b, c, d) e(4)
#define SIX(a, b, c, d, e, f) FIVE(a, b, c, d, f) f(5)
#define LA(n) result.insert(getImm(pc, n).u_LA);
#define NLA(n) result.insert(getImm(pc, n).u_NLA.id);
#define ILA(n) result.insert(getImm(pc, n).u_ILA);
#define MA(n)
#define BLA(n)
#define SLA(n)
#define IVA(n)
#define I64A(n)
#define IA(n)
#define DA(n)
#define SA(n)
#define AA(n)
#define RATA(n)
#define BA(n)
#define EMPTY_OA(n)
#define OA(op) EMPTY_OA
#define VSA(n)
#define KA(n)
#define LAR(n)
#define ITA(n)
#define FCA(n)
#define O(name, imm, in, out, flags) case Op::name: imm; break;
    OPCODES
#undef NA
#undef ONE
#undef TWO
#undef THREE
#undef FOUR
#undef FIVE
#undef SIX
#undef LA
#undef NLA
#undef ILA
#undef MA
#undef BLA
#undef SLA
#undef IVA
#undef I64A
#undef IA
#undef DA
#undef SA
#undef AA
#undef RATA
#undef BA
#undef EMPTY_OA
#undef OA
#undef VSA
#undef KA
#undef LAR
#undef ITA
#undef FCA
#undef O
  }
  return result;
}

bool FuncChecker::checkOp(State* cur, PC pc, Op op, Block* b, PC prev_pc) {
  switch (op) {
    case Op::InitProp:
    case Op::CheckProp: {
        auto fname = m_func->name->toCppString();
        if (fname.compare("86pinit") != 0 &&
            fname.compare("86sinit") != 0 &&
            fname.compare("86linit") != 0) {
          ferror("{} cannot appear in {} function\n", opcodeToName(op), fname);
          return false;
        }
        auto const prop = m_unit->lookupLitstrIdCopy(getImm(pc, 0).u_SA);
        if (!m_func->pce() || !m_func->pce()->hasProp(prop.get())){
             ferror("{} references non-existent property {}\n",
                    opcodeToName(op), prop);
             return false;
        }
        break;
    }
    case Op::This:
    case Op::CheckThis: {
      cur->guaranteedThis = true;
      break;
    }
    case Op::BareThis: {
      auto new_pc = pc;
      decode_op(new_pc);
      if (decode_oa<BareThisOp>(new_pc) != BareThisOp::NeverNull) break;
      [[fallthrough]];
    }
    case Op::BaseH: {
      // In HHBBC we can track the $this across loads into locals and pushes
      // onto the stack. If the types of those equivalent locations are refined
      // we may end up knowing that $this is never null. The verifier doesn't
      // currently support this type of sophisticated tracking and it's doubtful
      // there would be much value in adding it.
      if (!Cfg::Repo::Authoritative && !cur->guaranteedThis) {
        ferror("{} required that $this be guaranteed to be non-null\n",
        opcodeToName(op));
        return false;
      }
      break;
    }
    case Op::CreateCl: {
      auto const name = m_unit->lookupLitstrIdCopy(getImm(pc, 1).u_SA);
      auto const preCls = [&] () -> const PreClassEmitter* {
        for (auto const pce : unit()->preclasses()) {
          if (pce->name()->tsame(name.get())) return pce;
        }
        return nullptr;
      }();
      if (!preCls) {
        ferror("CreateCl references non-existent class {}\n", name);
        return false;
      }
      if (!preCls->parentName() ||
          preCls->parentName()->toCppString() != "Closure") {
        ferror("CreateCl references non-closure class {}\n", preCls->name()->slice());
        return false;
      }
      auto const [existing, emplaced] =
        m_createCls.emplace(preCls->name(), m_func->name);
      if (!emplaced && !existing->second->fsame(m_func->name)) {
        ferror("Closure {} referenced in multiple funcs {} and {}\n",
               preCls->name()->slice(), existing->second->slice(), m_func->name);
        return false;
      }
      auto const numBound = getImm(pc, 0).u_IVA;
      auto const invoke = preCls->lookupMethod(s_invoke.get());
      if (invoke && numBound != preCls->numProperties()) {
        ferror("CreateCl bound Closure {} with {} params instead of {}\n",
               preCls->name()->slice(), numBound, preCls->numProperties());
        return false;
      }
      if (!m_func->pce() || (m_func->attrs & AttrStatic)) {
        if (!(invoke->attrs & AttrStatic)) {
          ferror("CreateCl bound Closure {} without AttrStatic in a {}\n",
                 preCls->name()->slice(), m_func->pce() ? "static method" : "function");
          return false;
        }
      }
      break;
    }
    #define O(name) \
    case Op::name: { \
      auto const id = getImm(pc, 0).u_AA; \
      if (!checkArray(pc, id)) { \
        ferror("{} references array data that is not a \n", \
                #name, #name); \
        return false; \
      } \
      auto const dt = unit()->lookupArrayIdCopy(id)->toDataType(); \
      if (dt != KindOf##name) { \
        ferror("{} references array data that is a {}\n", #name, tname(dt)); \
        return false; \
      } \
      break; \
    }
    O(Keyset)
    O(Dict)
    O(Vec)
    #undef O
    case Op::GetMemoKeyL: {
      auto const name = folly::to<std::string>(
        m_func && m_func->pce() ? m_func->pce()->name()->data() : "",
        m_func && m_func->pce() ? "::" : "",
        m_func ? m_func->name->data() : "");
      if (!m_func->isMemoizeWrapper) {
        ferror("GetMemoKeyL can only appear within memoize wrappers\n");
        return false;
      }
      break;
    }
    case Op::MemoGet:
    case Op::MemoGetEager:
    case Op::MemoSet:
    case Op::MemoSetEager:
      if (!m_func->isMemoizeWrapper) {
        ferror("{} can only appear within memoize wrappers\n",
               opcodeToName(op));
        return false;
      }
      if (op == Op::MemoGetEager || op == Op::MemoSetEager) {
        if (!m_func->isAsync || m_func->isGenerator) {
          ferror("{} can only appear within async functions\n",
                 opcodeToName(op));
          return false;
        }
      }
      break;
    case Op::NewCol:
    case Op::ColFromArray: {
      auto new_pc = pc;
      decode_op(new_pc);
      auto colType = decode_oa<CollectionType>(new_pc);
      if (colType == CollectionType::Pair) {
        ferror("Immediate of {} must not be a pair\n", opcodeToName(op));
        return false;
      }
      break;
    }
    case Op::AssertRATL:
    case Op::AssertRATStk: {
      if (pc == b->last){
        ferror("{} cannot appear at the end of a block\n", opcodeToName(op));
        return false;
      }
      if (op == Op::AssertRATL) break;
      [[fallthrough]];
    }
    case Op::BaseC: {
      auto const stackIdx = getImm(pc, 0).u_IVA;
      if (stackIdx >= cur->stklen) {
        ferror("{} indexes ({}) past end of stack ({})\n", opcodeToName(op),
               stackIdx, cur->stklen);
        return false;
      }
      break;
    }
    case Op::BaseSC: {
      auto const keyIdx = getImm(pc, 0).u_IVA;
      auto const clsIdx = getImm(pc, 1).u_IVA;
      if (keyIdx >= cur->stklen) {
        ferror("{} indexes key index ({}) past end of stack ({})\n",
               opcodeToName(op), keyIdx, cur->stklen);
        return false;
      }
      if (clsIdx >= cur->stklen) {
        ferror("{} indexes class index ({}) past end of stack ({})\n",
               opcodeToName(op), clsIdx, cur->stklen);
        return false;
      }
      break;
    }
    case Op::CreateCont:
    case Op::YieldK:
    case Op::Yield:
      if (!m_func->isGenerator) {
        ferror("{} may only appear in a generator\n", opcodeToName(op));
        return false;
      }
      break;
    case Op::ContEnter: // Only in non-static generator methods
    case Op::ContRaise: {
      auto cls = m_func->pce();
      if ((m_func->attrs & AttrStatic) || !cls ||
          (cls->name()->toCppString() != std::string("Generator") &&
           cls->name()->toCppString() != std::string("HH\\AsyncGenerator"))) {
        ferror("{} may only appear in non-static methods of the "
               "[Async]Generator class\n", opcodeToName(op));
        return false;
      }
      break;
    }
    case Op::AwaitAll: {
      auto const& range = getImm(pc, 0).u_LAR;
      if (range.count == 0) {
        ferror("{} must have a non-empty local range\n", opcodeToName(op));
        return false;
      }
      [[fallthrough]];
    }
    case Op::Await: {
      if (!m_func->isAsync) {
        ferror("{} may only appear in an async function\n", opcodeToName(op));
        return false;
      }
      if (cur->stklen != instrNumPops(pc)) {
        ferror("{} may not be used with non-empty stack\n", opcodeToName(op));
        return false;
      }
      break;
    }
    case Op::AwaitLowPri: {
      if (!m_func->isAsync) {
        ferror("{} may only appear in an async function\n", opcodeToName(op));
        return false;
      }
      if (m_func->isGenerator) {
        ferror("{} may not appear in a generator\n", opcodeToName(op));
        return false;
      }
      if (cur->stklen != instrNumPops(pc)) {
        ferror("{} may not be used with non-empty stack\n", opcodeToName(op));
        return false;
      }
      break;
    }
    case Op::Fatal:
      // fatals don't do exception handling
      break;

    case Op::StaticAnalysisError:
      cur->mbr_live = false;
      break;

    case Op::NewVec:
    case Op::NewKeysetArray: {
      auto new_pc = pc;
      decode_op(new_pc);
      auto const elems = decode_iva(new_pc);
      static_assert(std::is_unsigned<decltype(elems)>::value,
                    "IVA should be unsigned");
      if (elems == 0 || elems > ArrayData::MaxElemsOnStack) {
        ferror("{} has an illegal element count\n", opcodeToName(op));
        return false;
      }
      break;
    }

    case Op::RetCSuspended:
      if (!m_func->isAsync || m_func->isGenerator) {
        ferror("{} can only appear within async functions\n",
               opcodeToName(op));
        return false;
      }
      break;

    case Op::FCallClsMethodD:
    case Op::FCallClsMethodSD:
    case Op::FCallCtor:
    case Op::FCallFuncD:
    case Op::FCallObjMethodD: {
      auto const fca = getImm(pc, 0).u_FCA;
      if (prev_pc && fca.hasGenerics()) {
        auto const prev_op = peek_op(prev_pc);
        if (prev_op == Op::Vec) {
          auto const id = getImm(prev_pc, 0).u_AA;
          if (!checkArray(prev_pc, id)) {
            ferror("Generics passed to {} don't exist\n", opcodeToName(op));
            return false;
          }
          auto const arr = unit()->lookupArrayIdCopy(id);
          if (doesTypeStructureContainTUnresolved(arr.get())) {
            ferror("Generics passed to {} contain unresolved generics. "
                   "Call CombineAndResolveTypeStruct to resolve them\n",
                   opcodeToName(op));
            return false;
          }
        }
      }
      break;
    }
    case Op::SetS: {
      auto new_pc = pc;
      decode_op(new_pc);
      auto const rop = decode_oa<ReadonlyOp>(new_pc);
      if (rop != ReadonlyOp::Readonly && rop != ReadonlyOp::Any) {
        return readOnlyImmNotSupported(rop, op);
      }
      break;
    }
    case Op::CGetS: {
      auto new_pc = pc;
      decode_op(new_pc);
      auto const rop = decode_oa<ReadonlyOp>(new_pc);
      if (rop != ReadonlyOp::Mutable && rop != ReadonlyOp::Any) {
        return readOnlyImmNotSupported(rop, op);
      }
      break;
    }
    default:
      break;
  }

  return true;
}

bool FuncChecker::checkOutputs(State* cur, PC pc, Block* b) {
  static const FlavorDesc outputSigs[][kMaxHhbcImms] = {
  #define NOV { },
  #define CMANY { },
  #define FCALL { },
  #define ONE(a) { a },
  #define TWO(a,b) { a, b },
  #define THREE(a,b,c) { a, b, c },
  #define FOUR(a,b,c,d) { a, b, c, d },
  #define FIVE(a,b,c,d,e) { a, b, c, d, e },
  #define SIX(a,b,c,d,e,f) { a, b, c, d, e, f },
  #define O(name, imm, pop, push, flags) push
    OPCODES
  #undef O
  #undef SIX
  #undef FIVE
  #undef FOUR
  #undef THREE
  #undef TWO
  #undef ONE
  #undef CMANY
  #undef FCALL
  #undef NOV
  };
  bool ok = true;
  auto const op = peek_op(pc);
  auto const pushes = instrNumPushes(pc);
  if (cur->stklen + pushes > maxStack()) reportStkOverflow(b, *cur, pc);
  FlavorDesc *outs = &cur->stk[cur->stklen];
  cur->stklen += pushes;
  if (isFCall(op)) {
    for (int i = 0; i < pushes; ++i) {
      outs[i] = CV;
    }
  } else {
    for (int i = 0; i < pushes; ++i) {
      outs[i] = outputSigs[size_t(op)][i];
    }
  }

  if (isMemberBaseOp(op)) {
    cur->mbr_live = true;
    if (op == Op::BaseL)  {
      auto new_pc = pc;
      decode_op(new_pc);
      if (op == Op::BaseL) decode_iva(new_pc);
      decode_iva(new_pc);
      cur->mbr_mode = decode_oa<MOpMode>(new_pc);
      decode_oa<ReadonlyOp>(new_pc);
    }
  } else if (isMemberFinalOp(op)) {
    cur->mbr_live = false;
    cur->mbr_mode.reset();
  }

  return ok;
}

bool FuncChecker::readOnlyImmNotSupported(ReadonlyOp rop, Op op) {
  ferror("{} immediate not supported on {}.\n",
    subopToName(rop), opcodeToName(op));
  return false;
}

bool FuncChecker::checkReadonlyOp(State* cur, PC pc, Op op) {
  auto new_pc = pc;
  if (isMemberBaseOp(op)) {
    if (op == Op::BaseSC) {
      cur->afterProp = true;
      decode_op(new_pc);
      decode_iva(new_pc);
      decode_iva(new_pc);
      decode_oa<MOpMode>(new_pc);
      auto const rop = decode_oa<ReadonlyOp>(new_pc);
      if (rop == ReadonlyOp::Readonly) return readOnlyImmNotSupported(rop, op);
      cur->afterCheckCOW = rop == ReadonlyOp::CheckROCOW || rop == ReadonlyOp::CheckMutROCOW;
      return true;
    } else if (op == Op::BaseL) {
      cur->afterProp = false;
      decode_op(new_pc);
      decode_iva(new_pc);
      decode_iva(new_pc);
      decode_oa<MOpMode>(new_pc);
      auto const rop = decode_oa<ReadonlyOp>(new_pc);
      if (rop != ReadonlyOp::CheckROCOW && rop != ReadonlyOp::Any) {
        return readOnlyImmNotSupported(rop, op);
      }
      cur->afterCheckCOW = rop == ReadonlyOp::CheckROCOW;
      return true;
    } else {
      cur->afterProp = false;
      cur->afterCheckCOW = false;
      return true;
    }
  } else if (isMemberDimOp(op)) {
    decode_op(new_pc);
    decode_oa<MOpMode>(new_pc);
    auto const mcode = decode_raw<MemberCode>(new_pc);
    switch (mcode) {
      case MW:
        return true;
      case MEL: case MPL: {
        decode_iva(new_pc);
        decode_iva(new_pc);
        break;
      }
      case MEC: case MPC: {
        decode_iva(new_pc);
        break;
      }
      case MEI: {
        new_pc += sizeof(int64_t);
        break;
      }
      case MET: case MPT: case MQT:
        decode_raw<Id>(new_pc);
        break;
    }
    auto const is_prop_flavor = mcodeIsProp(mcode);
    cur->afterProp |= is_prop_flavor;
    if (is_prop_flavor && cur->afterCheckCOW) {
      ferror("Check(Mut)ROCOW must only appear on the last prop access.\n");
      return false;
    }
    auto const rop = decode_oa<ReadonlyOp>(new_pc);
    if (rop == ReadonlyOp::CheckMutROCOW || rop == ReadonlyOp::CheckROCOW) {
      if (!is_prop_flavor) {
        ferror("Only property-flavored member keys may be marked Check(Mut)ROCOW.\n");
        return false;
      } else {
        cur->afterCheckCOW = true;
      }
    }
  }
  return true;
}

std::string FuncChecker::stkToString(int len, const FlavorDesc* args) const {
  std::stringstream out;
  out << '[';
  for (int i = 0; i < len; ++i) {
    out << stkflav(args[i]);
  }
  out << ']';
  return out.str();
}

std::string FuncChecker::sigToString(int len, const FlavorDesc* sig) const {
  std::stringstream out;
  out << '[';
  for (int i = 0; i < len; ++i) {
    out << stkflav(sig[i]);
  }
  out << ']';
  return out.str();
}

std::string FuncChecker::iterToString(const State& cur) const {
  int n = numIters();
  if (!n) return "";
  std::stringstream out;
  out << '[';
  for (int i = 0; i < n; ++i) {
    out << (cur.iters[i] ? '1' : '0');
  }
  out << ']';
  return out.str();
}

std::string
FuncChecker::slotsToString(const boost::dynamic_bitset<>& slots) const {
  std::ostringstream oss;
  oss << slots;
  return oss.str();
}

std::string FuncChecker::stateToString(const State& cur) const {
  return fmt::format(
    "{}{}",
    iterToString(cur),
    stkToString(cur.stklen, cur.stk)
  );
}

void FuncChecker::initState(State* s) {
  s->stk = new (m_arena) FlavorDesc[maxStack()];
  s->iters = new (m_arena) bool[numIters()];
  for (int i = 0, n = numIters(); i < n; ++i) s->iters[i] = false;
  s->stklen = 0;
  s->mbr_live = false;
  s->mbr_mode.reset();
  s->guaranteedThis = m_func->pce() && !(m_func->attrs & AttrStatic);
  s->mbrMustContainMutableLocalOrThis = false;
  s->afterDim = false;
  s->afterCheckCOW = false;
  s->afterProp = false;
}

void FuncChecker::copyState(State* to, const State* from) {
  assertx(isInitialized(*from));
  if (!to->stk) initState(to);
  memcpy(to->stk, from->stk, from->stklen * sizeof(*to->stk));
  memcpy(to->iters, from->iters, numIters() * sizeof(*to->iters));
  to->stklen = from->stklen;
  to->mbr_live = from->mbr_live;
  to->mbr_mode = from->mbr_mode;
  to->guaranteedThis = from->guaranteedThis;
  to->mbrMustContainMutableLocalOrThis = from->mbrMustContainMutableLocalOrThis;
  to->afterDim = from->afterDim;
  to->afterCheckCOW = from->afterCheckCOW;
  to->afterProp = from->afterProp;
}

bool FuncChecker::checkExnEdge(State cur, Op op, Block* b) {
  // Any live iterators must be guarded by exception edges. So, if there
  // isn't an exception edge for a given block, all iters must be dead.
  if (!b->exn) return checkItersDead(cur, op, b, "is unguarded");

  // Reachable catch blocks have just the exception on the
  // stack. Checking an edge to the catch block right before every
  // instruction is unnecessary since not every instruction can throw;
  // there is room for improvement here if we want to note in the
  // bytecode table which instructions can actually throw.
  auto save_stklen = cur.stklen;
  auto save_stktop = cur.stklen ? cur.stk[0] : CV;
  cur.stklen = 1;
  cur.stk[0] = CV;
  auto const ok = checkEdge(b, cur, b->exn);
  cur.stklen = save_stklen;
  cur.stk[0] = save_stktop;
  return ok;
}

bool FuncChecker::checkBlock(State& cur, Block* b) {
  bool ok = true;
  if (m_errmode == kVerbose) {
    std::cout << blockToString(b, m_graph, m_func) << std::endl;
  }
  PC prev_pc = nullptr;
  for (InstrRange i = blockInstrs(b); !i.empty(); ) {
    PC pc = i.popFront();
    if (m_errmode == kVerbose) {
      std::cout << "  " << std::setw(5) << offset(pc) << ":" <<
                   stateToString(cur) << " " <<
                   instrToString(pc, m_func, m_unit) << std::endl;
    }
    auto const op = peek_op(pc);
    if (mayTakeExnEdges(op)) ok &= checkExnEdge(cur, op, b);
    if (isMemberFinalOp(op)) ok &= checkMemberKey(&cur, pc, op);
    ok &= checkOp(&cur, pc, op, b, prev_pc);
    ok &= checkInputs(&cur, pc, b);
    auto const flags = instrFlags(op);
    if (flags & TF) ok &= checkTerminal(&cur, op, b);
    if (isIter(pc)) ok &= checkIter(&cur, pc);
    ok &= checkOutputs(&cur, pc, b);
    ok &= checkReadonlyOp(&cur, pc, op);
    prev_pc = pc;
  }
  ok &= checkSuccEdges(b, &cur);
  return ok;
}

/**
 * Verify stack depth along all control flow paths
 */
bool FuncChecker::checkFlow() {
  bool ok = true;
  GraphBuilder builder(m_arena, m_func);
  m_graph = builder.build();
  m_info = new (m_arena) BlockInfo[m_graph->block_count];
  m_tmp_sig = new (m_arena) FlavorDesc[maxStack()];
  sortRpo(m_graph);
  State cur;
  initState(&cur);
  // initialize state at all entry points
  for (BlockPtrRange i = entryBlocks(m_graph); !i.empty();) {
    ok &= checkEdge(0, cur, i.popFront());
  }
  for (Block* b = m_graph->first_rpo; b; b = b->next_rpo) {
    m_last_rpo_id = b->rpo_id;
    auto const& state = m_info[b->id].state_in;
    if (isInitialized(state)) {
      copyState(&cur, &state);
      ok &= checkBlock(cur, b);
    }
  }

  return ok;
}

bool FuncChecker::checkSuccEdges(Block* b, State* cur) {
  bool ok = true;

  if (isIter(b->last) && b->succ_count == 2) {
    // IterInit* and IterNext*, Both implicitly free their iterator variable
    // on the loop-exit path.  Compute the iterator state on the "taken" path;
    // the fall-through path has the opposite state.
    auto const id = getIterId(b->last);
    auto const last_op = peek_op(b->last);
    bool taken_state = last_op == OpIterNext;
    bool save = cur->iters[id];
    cur->iters[id] = taken_state;
    if (m_errmode == kVerbose) {
      std::cout << "        " << stateToString(*cur) <<
        " -> B" << b->succs[1]->id << std::endl;
    }
    ok &= checkEdge(b, *cur, b->succs[1]);
    cur->iters[id] = !taken_state;
    if (m_errmode == kVerbose) {
      std::cout << "        " << stateToString(*cur) <<
                   " -> B" << b->succs[0]->id << std::endl;
    }
    ok &= checkEdge(b, *cur, b->succs[0]);
    cur->iters[id] = save;
  } else if (peek_op(b->last) == OpMemoGet && b->succ_count == 2) {
    ok &= checkEdge(b, *cur, b->succs[0]);
    --cur->stklen;
    ok &= checkEdge(b, *cur, b->succs[1]);
  } else if (peek_op(b->last) == OpMemoGetEager && b->succ_count == 3) {
    ok &= checkEdge(b, *cur, b->succs[0]);
    --cur->stklen;
    ok &= checkEdge(b, *cur, b->succs[1]);
    ++cur->stklen;
    ok &= checkEdge(b, *cur, b->succs[2]);
  } else {
    // Other branch instructions send the same state to all successors.
    if (m_errmode == kVerbose) {
      std::cout << "        " << stateToString(*cur) << std::endl;
    }
    for (BlockPtrRange i = succBlocks(b); !i.empty(); ) {
      ok &= checkEdge(b, *cur, i.popFront());
    }
  }

  if (peek_op(b->last) == OpEnter) {
    assertx(b->succ_count == 1);
    auto const t = b->succs[0];
    if (offset(t->start) != 0) {
      error("Enter target offset %d must point to the entry block\n",
            offset(t->start));
      ok = false;
    } else {
      boost::dynamic_bitset<> visited(m_graph->block_count);
      if (Block::reachable(t, b, visited)) {
        error("DV initializer at B%d can't be reachable from the entry block\n",
              b->id);
        ok = false;
      }
    }
  }

  if (cur->mbr_live) {
    // MBR must not be live across control flow edges.
    error("Member base register live at end of B%d\n", b->id);
    ok = false;
  }

  return ok;
}

/**
 * Check the edge b->t, given the current state at the end of b.
 * If this is the first edge ->t we've seen, copy the state to t.
 * Otherwise, unify the state and require the nonunifiable parts
 * to exactly match.
 */
bool FuncChecker::checkEdge(Block* b, const State& cur, Block *t) {
  State& state = m_info[t->id].state_in;

  // If we already passed this block in RPO order, but we just modified its in
  // state, we must revisit it. This pass terminates because each block's state
  // can change at most twice:
  //
  //  - Uninitialized -> initialized
  //  - Initialized, guaranteedThis -> Initialized, !guaranteedThis
  //
  auto const maybe_revisit = [&]{
    if (m_last_rpo_id < t->rpo_id) return true;
    State tmp;
    copyState(&tmp, &state);
    return checkBlock(tmp, t);
  };

  // We call checkEdge with a null block b to initialize entry blocks;
  // don't visit these states initially because we'll get them in RPO order.
  if (!isInitialized(state)) {
    copyState(&state, &cur);
    return b == nullptr || maybe_revisit();
  }

  // Check that the stacks agree on depth and flavor.
  if (state.stklen != cur.stklen) {
    reportStkMismatch(b, t, cur);
    return false;
  }
  for (int i = 0, n = cur.stklen; i < n; i++) {
    if (state.stk[i] != cur.stk[i]) {
      error("mismatch on edge B%d->B%d, current %s target %s\n",
             b->id, t->id, stkToString(n, cur.stk).c_str(),
             stkToString(n, state.stk).c_str());
      return false;
    }
  }

  // Check that iterator initialization state matches.
  for (int i = 0, n = numIters(); i < n; i++) {
    if (state.iters[i] != cur.iters[i]) {
      error("mismatched iterator state on edge B%d->B%d, "
             "current %s target %s\n", b->id, t->id,
             iterToString(cur).c_str(), iterToString(state).c_str());
      return false;
    }
  }

  // Conservatively propagate guarantees about $this.
  if (state.guaranteedThis && !cur.guaranteedThis) {
    state.guaranteedThis = false;
    return maybe_revisit();
  }
  return true;
}

void FuncChecker::reportStkUnderflow(Block*, const State& cur, PC pc) {
  error("Rule2: Stack underflow at PC %d\n", offset(pc));
}

void FuncChecker::reportStkOverflow(Block*, const State& /*cur*/, PC pc) {
  error("Rule2: Stack overflow at PC %d\n", offset(pc));
}

void FuncChecker::reportStkMismatch(Block* b, Block* t, const State& cur) {
  const State& st = m_info[t->id].state_in;
  error("Rule1: Stack mismatch on edge B%d->B%d; depth %d->%d\n",
          b->id, t->id, cur.stklen, st.stklen);
}

void FuncChecker::reportEscapeEdge(Block* b, Block* s) {
  error("Edge from B%d to offset %d escapes function\n",
         b->id, offset(s->start));
}

/**
 * Check that the offset is within the region and it lands on an exact
 * instruction start.
 */
bool FuncChecker::checkOffset(const char* name, Offset off,
                              const char* regionName, Offset base,
                              Offset past, bool check_instrs) {
  assertx(past >= base);
  if (off < base || off >= past) {
    error("Offset %s %d is outside region %s %d:%d\n",
           name, off, regionName, base, past);
    return false;
  }
  if (check_instrs && !m_instrs.get(off)) {
    error("label %s %d is not on a valid instruction boundary\n",
           name, off);
    return false;
  }
  return true;
}

/**
 * Check that the given inner region is valid, within the outer region, and
 * the inner region boundaries are exact instructions.
 */
bool FuncChecker::checkRegion(const char* name, Offset b, Offset p,
                               const char* regionName, Offset base,
                               Offset past, bool check_instrs) {
  assertx(past >= base);
  if (p < b) {
    error("region %s %d:%d has negative length\n",
           name, b, p);
    return false;
  }
  if (b < base || p > past) {
    error("region %s %d:%d is not inside region %s %d:%d\n",
           name, b, p, regionName, base, past);
    return false;
  } else if (check_instrs &&
             (!m_instrs.get(b) ||
              (p < past && !m_instrs.get(p)))) {
    error("region %s %d:%d boundaries are inbetween instructions\n",
           name, b, p);
    return false;
  }
  return true;
}

}} // HPHP::Verifier
