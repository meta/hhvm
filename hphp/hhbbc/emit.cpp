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
#include "hphp/hhbbc/emit.h"

#include <vector>
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <type_traits>

#include <folly/gen/Base.h>
#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/Memory.h>

#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/context.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/unit-util.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/repo-auth-type.h"

#include "hphp/runtime/ext/extension-registry.h"

#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/type-alias-emitter.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/configs/eval.h"

namespace HPHP::HHBBC {

TRACE_SET_MOD(hhbbc_emit)

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_invoke("__invoke");

//////////////////////////////////////////////////////////////////////

struct EmitUnitState {
  explicit EmitUnitState(const IIndex& index, const php::Unit* unit) :
      index(index), unit(unit) {}

  /*
   * Access to the Index for this program.
   */
  const IIndex& index;

  /*
   * Access to the unit we're emitting
   */
  const php::Unit* unit;

  /*
   * While emitting bytecode, we keep track of the classes and funcs
   * we emit.
   */
  std::vector<std::pair<php::Class*, PreClassEmitter*>> pces;

  /*
   * Whether a closure in a CreateCl opcode has been seen before
   * (CreateCls within the same func are allowed to refer to the same
   * closure, so we must avoid creating duplicate PreClassEmitters for
   * them).
   */
  hphp_fast_set<const php::Class*> seenClosures;
};

/*
 * Some bytecodes need to be mutated before being emitted. Pass those
 * bytecodes by value to their respective emit_op functions.
 */
template<typename T>
struct OpInfoHelper {
  static constexpr bool by_value = T::op == Op::CreateCl;
  using type = typename std::conditional<by_value, T, const T&>::type;
};

template<typename T>
using OpInfo = typename OpInfoHelper<T>::type;

/*
 * Helper to conditionally call fun on data provided data is of the
 * given type.
 */
template<Op op, typename F, typename Bc>
std::enable_if_t<std::remove_reference_t<Bc>::op == op>
caller(F&& fun, Bc&& data) { fun(std::forward<Bc>(data)); }

template<Op op, typename F, typename Bc>
std::enable_if_t<std::remove_reference_t<Bc>::op != op>
caller(F&&, Bc&&) {}

void recordClass(EmitUnitState& euState,
                 UnitEmitter& ue,
                 php::Class& cls) {
  euState.pces.emplace_back(
    &cls,
    ue.newPreClassEmitter(cls.name->toCppString())
  );
}

//////////////////////////////////////////////////////////////////////

php::SrcLoc srcLoc(const php::Unit& unit, const php::Func& func, int32_t ix) {
  if (ix < 0) return php::SrcLoc{};
  assertx(ix < unit.srcLocs.size());
  return unit.srcLocs[ix];
}

/*
 * Order the blocks for bytecode emission.
 *
 * Rules about block order:
 *
 *   - Each DV funclet must have all of its blocks contiguous, with the
 *     entry block first.
 *
 *   - Main entry point must be the first block.
 *
 * It is not a requirement, but we attempt to locate all the DV entry
 * points after the rest of the primary function body.  The normal
 * case for DV initializers is that each one falls through to the
 * next, with the block jumping back to the main entry point.
 */
std::vector<BlockId> order_blocks(const php::WideFunc& f) {
  auto sorted = rpoSortFromMain(f);

  // Get the DV blocks, without the rest of the primary function body,
  // and then add them to the end of sorted.
  auto const dvBlocks = [&] {
    auto withDVs = rpoSortAddDVs(f);
    withDVs.erase(
      std::find(begin(withDVs), end(withDVs), sorted.front()),
      end(withDVs)
    );
    return withDVs;
  }();
  sorted.insert(end(sorted), begin(dvBlocks), end(dvBlocks));

  FTRACE(2, "      block order:{}\n",
    [&] {
      std::string ret;
      for (auto const bid : sorted) {
        ret += " ";
        ret += folly::to<std::string>(bid);
      }
      return ret;
    }()
  );
  return sorted;
}

// While emitting bytecode, we learn about some metadata that will
// need to be registered in the FuncEmitter.
struct EmitBcInfo {
  struct JmpFixup { Offset instrOff; Offset jmpImmedOff; };

  struct BlockInfo {
    BlockInfo()
      : offset(kInvalidOffset)
      , past(kInvalidOffset)
      , regionsToPop(0)
    {}

    // The offset of the block, if we've already emitted it.
    // Otherwise kInvalidOffset.
    Offset offset;

    // The offset past the end of this block.
    Offset past;

    // How many catch regions the jump at the end of this block is leaving.
    // 0 if there is no jump or if the jump is to the same catch region or a
    // child
    int regionsToPop;

    // When we emit a forward jump to a block we haven't seen yet, we
    // write down where the jump was so we can fix it up when we get
    // to that block.
    std::vector<JmpFixup> forwardJumps;

    // When we see a forward jump to a block, we record the stack
    // depth at the jump site here.  This is needed to track
    // currentStackDepth correctly (and we also assert all the jumps
    // have the same depth).
    Optional<uint32_t> expectedStackDepth;
  };

  std::vector<BlockId> blockOrder;
  uint32_t maxStackDepth;
  bool containsCalls;
  std::vector<BlockInfo> blockInfo;
};

using ExnNodePtr = php::ExnNode*;

bool handleEquivalent(const php::Func& func, ExnNodeId eh1, ExnNodeId eh2) {
  auto entry = [&] (ExnNodeId eid) {
    return func.exnNodes[eid].region.catchEntry;
  };

  while (eh1 != eh2) {
    assertx(eh1 != NoExnNodeId &&
            eh2 != NoExnNodeId &&
            func.exnNodes[eh1].depth == func.exnNodes[eh2].depth);
    if (entry(eh1) != entry(eh2)) return false;
    eh1 = func.exnNodes[eh1].parent;
    eh2 = func.exnNodes[eh2].parent;
  }

  return true;
}

// The common parent P of eh1 and eh2 is the deepest region such that
// eh1 and eh2 are both handle-equivalent to P or a child of P
ExnNodeId commonParent(const php::Func& func, ExnNodeId eh1, ExnNodeId eh2) {
  if (eh1 == NoExnNodeId || eh2 == NoExnNodeId) return NoExnNodeId;
  while (func.exnNodes[eh1].depth > func.exnNodes[eh2].depth) {
    eh1 = func.exnNodes[eh1].parent;
  }
  while (func.exnNodes[eh2].depth > func.exnNodes[eh1].depth) {
    eh2 = func.exnNodes[eh2].parent;
  }
  while (!handleEquivalent(func, eh1, eh2)) {
    eh1 = func.exnNodes[eh1].parent;
    eh2 = func.exnNodes[eh2].parent;
  }
  return eh1;
}

const StaticString
  s_hhbbc_fail_verification("__hhvm_intrinsics\\hhbbc_fail_verification");

EmitBcInfo emit_bytecode(EmitUnitState& euState, UnitEmitter& ue, FuncEmitter& fe,
                         const php::WideFunc& func) {
  EmitBcInfo ret = {};
  auto& blockInfo = ret.blockInfo;
  blockInfo.resize(func.blocks().size());

  // Track the stack depth while emitting to determine maxStackDepth.
  int32_t currentStackDepth { 0 };

  // Temporary buffer for vector immediates.  (Hoisted so it's not
  // allocated in the loop.)
  std::vector<uint8_t> immVec;

  // Offset of the last emitted bytecode.
  Offset lastOff { 0 };

  auto const unit = euState.index.lookup_func_original_unit(*func);

  SCOPE_ASSERT_DETAIL("emit") {
    std::string ret;
    for (auto bid : func.blockRange()) {
      auto const block = show(*func, *func.blocks()[bid]);
      ret += fmt::format("block #{}\n{}", bid, block);
    }

    return ret;
  };

  auto const map_local = [&] (LocalId id) {
    if (id >= func->locals.size()) return id;
    auto const loc = func->locals[id];
    assertx(!loc.killed);
    assertx(loc.id <= id);
    id = loc.id;
    return id;
  };

  auto const map_local_name = [&] (NamedLocal nl) {
    nl.id = map_local(nl.id);
    if (nl.name == kInvalidLocalName) return nl;
    if (nl.name >= func->locals.size()) return nl;
    auto const loc = func->locals[nl.name];
    if (!loc.name) {
      nl.name = kInvalidLocalName;
      return nl;
    }
    assertx(!loc.unusedName);
    nl.name = loc.nameId;
    return nl;
  };

  auto const set_expected_depth = [&] (BlockId block) {
    auto& info = blockInfo[block];

    if (info.expectedStackDepth) {
      assertx(*info.expectedStackDepth == currentStackDepth);
    } else {
      info.expectedStackDepth = currentStackDepth;
    }
  };

  auto const make_member_key = [&] (MKey mkey) {
    switch (mkey.mcode) {
      case MEC: case MPC:
        return MemberKey{mkey.mcode, static_cast<int32_t>(mkey.idx), mkey.rop};
      case MEL: case MPL:
        return MemberKey{mkey.mcode, map_local_name(mkey.local), mkey.rop};
      case MET: case MPT: case MQT:
        return MemberKey{mkey.mcode, mkey.litstr, mkey.rop};
      case MEI:
        return MemberKey{mkey.mcode, mkey.int64, mkey.rop};
      case MW:
        return MemberKey{};
    }
    not_reached();
  };

  auto const emit_inst = [&] (const Bytecode& inst) {
    auto const startOffset = fe.bcPos();
    lastOff = startOffset;

    FTRACE(4, " emit: {} -- {} @ {}\n", currentStackDepth, show(*func, inst),
           show(srcLoc(*unit, *func, inst.srcLoc)));

    auto const emit_vsa = [&] (const CompactVector<LSString>& keys) {
      auto n = keys.size();
      fe.emitIVA(n);
      for (size_t i = 0; i < n; ++i) {
        fe.emitInt32(ue.mergeLitstr(keys[i]));
      }
    };

    auto const emit_branch = [&] (BlockId id) {
      auto& info = blockInfo[id];
      if (info.offset != kInvalidOffset) {
        fe.emitInt32(info.offset - startOffset);
      } else {
        info.forwardJumps.push_back({ startOffset, fe.bcPos() });
        fe.emitInt32(0);
      }
    };

    auto const emit_switch = [&] (const SwitchTab& targets) {
      fe.emitIVA(targets.size());
      for (auto t : targets) {
        set_expected_depth(t);
        emit_branch(t);
      }
    };

    auto const emit_sswitch = [&] (const SSwitchTab& targets) {
      fe.emitIVA(targets.size());
      for (size_t i = 0; i < targets.size() - 1; ++i) {
        set_expected_depth(targets[i].second);
        fe.emitInt32(ue.mergeLitstr(targets[i].first));
        emit_branch(targets[i].second);
      }
      fe.emitInt32(-1);
      set_expected_depth(targets[targets.size() - 1].second);
      emit_branch(targets[targets.size() - 1].second);
    };

    auto const emit_srcloc = [&] {
      auto const sl = srcLoc(*unit, *func, inst.srcLoc);
      auto const loc = sl.isValid() ?
        Location::Range(sl.start.line, sl.start.col, sl.past.line, sl.past.col)
        : Location::Range(-1,-1,-1,-1);
      fe.recordSourceLocation(loc, startOffset);
    };

    auto const pop = [&] (int32_t n) {
      currentStackDepth -= n;
      assertx(currentStackDepth >= 0);
    };
    auto const push = [&] (int32_t n) {
      currentStackDepth += n;
      ret.maxStackDepth =
        std::max<uint32_t>(ret.maxStackDepth, currentStackDepth);
    };

    auto const ret_assert = [&] { assertx(currentStackDepth == inst.numPop()); };

    auto const createcl = [&] (auto const& data) {
      auto const cls = euState.index.lookup_class(data.str2);
      always_assert_flog(
        cls,
        "A closure class ({}) failed to resolve",
        data.str2
      );
      assertx(cls->unit == euState.unit->filename);
      assertx(is_closure(*cls));
      // Skip closures we've already recorded
      if (!euState.seenClosures.emplace(cls).second) return;
      recordClass(euState, ue, const_cast<php::Class&>(*cls));
    };

    auto const emit_lar  = [&](const LocalRange& range) {
      encodeLocalRange(fe, HPHP::LocalRange{
        map_local(range.first), range.count
      });
    };

    auto const emit_named_args = [&](const NamedArgNameVec* names) {
      assertx(names != nullptr);
      assertx(names->size() > 0);
      VecInit nameVec(names->size());
      for (auto name : *names) {
        nameVec.append(make_tv<KindOfString>(name));
      }
      auto nameTV = nameVec.create();
      ArrayData::GetScalarArray(&nameTV);
      fe.emitInt32(ue.mergeArray(nameTV));
    };



#define IMM_BLA(n)     emit_switch(data.targets);
#define IMM_SLA(n)     emit_sswitch(data.targets);
#define IMM_IVA(n)     fe.emitIVA(data.arg##n);
#define IMM_I64A(n)    fe.emitInt64(data.arg##n);
#define IMM_LA(n)      fe.emitIVA(map_local(data.loc##n));
#define IMM_NLA(n)     fe.emitNamedLocal(map_local_name(data.nloc##n));
#define IMM_ILA(n)     fe.emitIVA(map_local(data.loc##n));
#define IMM_IA(n)      fe.emitIVA(data.iter##n);
#define IMM_DA(n)      fe.emitDouble(data.dbl##n);
#define IMM_SA(n)      fe.emitInt32(ue.mergeLitstr(data.str##n));
#define IMM_RATA(n)    encodeRAT(fe, ue, data.rat);
#define IMM_AA(n)      fe.emitInt32(ue.mergeArray(data.arr##n));
#define IMM_OA_IMPL(n) fe.emitByte(static_cast<uint8_t>(data.subop##n));
#define IMM_OA(type)   IMM_OA_IMPL
#define IMM_BA(n)      targets[numTargets++] = data.target##n; \
                       emit_branch(data.target##n);
#define IMM_VSA(n)     emit_vsa(data.keys);
#define IMM_KA(n)      encode_member_key(make_member_key(data.mkey), fe, ue);
#define IMM_LAR(n)     emit_lar(data.locrange);
#define IMM_ITA(n)     encodeIterArgs(fe, data.ita);
#define IMM_FCA(n)     encodeFCallArgs(                                 \
                         fe, data.fca.base(),                           \
                         data.fca.enforceInOut(),                       \
                         [&] {                                          \
                           data.fca.applyIO(                            \
                             [&] (int numBytes, const uint8_t* inOut) { \
                               encodeFCallArgsBoolVec(fe, numBytes, inOut); \
                             }                                          \
                           );                                           \
                         },                                             \
                         data.fca.enforceReadonly(),                    \
                         [&] {                                          \
                           data.fca.applyReadonly(                     \
                             [&] (int numBytes, const uint8_t* readonly) { \
                               encodeFCallArgsBoolVec(fe, numBytes, readonly); \
                             }                                          \
                           );                                           \
                         },                                             \
                         data.fca.namedArgNames() != nullptr,           \
                         [&] {                                          \
                           emit_named_args(data.fca.namedArgNames());   \
                         },                                             \
                         data.fca.asyncEagerTarget() != NoBlockId,      \
                         [&] {                                          \
                           set_expected_depth(data.fca.asyncEagerTarget()); \
                           emit_branch(data.fca.asyncEagerTarget());    \
                         },                                             \
                         data.fca.context() != nullptr,                 \
                         [&] {                                          \
                           fe.emitInt32(ue.mergeLitstr(data.fca.context()));\
                         });                                            \
                       if (!data.fca.hasUnpack()) ret.containsCalls = true;

#define IMM_NA
#define IMM_ONE(x)                IMM_##x(1)
#define IMM_TWO(x, y)             IMM_##x(1); IMM_##y(2);
#define IMM_THREE(x, y, z)        IMM_TWO(x, y); IMM_##z(3);
#define IMM_FOUR(x, y, z, n)      IMM_THREE(x, y, z); IMM_##n(4);
#define IMM_FIVE(x, y, z, n, m)   IMM_FOUR(x, y, z, n); IMM_##m(5);
#define IMM_SIX(x, y, z, n, m, o) IMM_FIVE(x, y, z, n, m); IMM_##o(6);

#define POP_NOV
#define POP_ONE(x)            pop(1);
#define POP_TWO(x, y)         pop(2);
#define POP_THREE(x, y, z)    pop(3);

#define POP_MFINAL     pop(data.arg1);
#define POP_C_MFINAL(n) pop(n); pop(data.arg1);
#define POP_CMANY      pop(data.arg##1);
#define POP_SMANY      pop(data.keys.size());
#define POP_CUMANY     pop(data.arg##1);
#define POP_FCALL(nin, nobj) \
                       pop(nin + data.fca.numInputs() + 1 + data.fca.numRets());

#define PUSH_NOV
#define PUSH_ONE(x)            push(1);
#define PUSH_TWO(x, y)         push(2);
#define PUSH_THREE(x, y, z)    push(3);
#define PUSH_CMANY             push(data.arg1);
#define PUSH_FCALL             push(data.fca.numRets());

#define O(opcode, imms, inputs, outputs, flags)                  \
    case Op::opcode: {                                           \
      if (Op::opcode == Op::Nop) break;                          \
      OpInfo<bc::opcode> data{inst.opcode};                      \
      if (Cfg::Eval::EnableIntrinsicsExtension) {            \
        if (Op::opcode == Op::FCallFuncD &&                      \
            inst.FCallFuncD.str2 == s_hhbbc_fail_verification.get()) {\
          fe.emitOp(Op::CheckProp);                              \
          fe.emitInt32(                                          \
            ue.mergeLitstr(inst.FCallFuncD.str2));               \
          fe.emitOp(Op::PopC);                                   \
          ret.maxStackDepth++;                                   \
        }                                                        \
      }                                                          \
      caller<Op::CreateCl>(createcl, data);                      \
                                                                 \
      if (isRet(Op::opcode)) ret_assert();                       \
      fe.emitOp(Op::opcode);                                     \
      POP_##inputs                                               \
                                                                 \
      size_t numTargets = 0;                                     \
      std::array<BlockId, kMaxHhbcImms> targets;                 \
                                                                 \
      if (Op::opcode == Op::MemoGet) {                           \
        IMM_##imms                                               \
        assertx(numTargets == 1);                                \
        set_expected_depth(targets[0]);                          \
        PUSH_##outputs                                           \
      } else if (Op::opcode == Op::MemoGetEager) {               \
        IMM_##imms                                               \
        assertx(numTargets == 2);                                \
        set_expected_depth(targets[0]);                          \
        PUSH_##outputs                                           \
        set_expected_depth(targets[1]);                          \
      } else {                                                   \
        PUSH_##outputs                                           \
        IMM_##imms                                               \
        for (size_t i = 0; i < numTargets; ++i) {                \
          set_expected_depth(targets[i]);                        \
        }                                                        \
      }                                                          \
                                                                 \
      if (flags & TF) currentStackDepth = 0;                     \
      emit_srcloc();                                             \
      break;                                                     \
    }
    switch (inst.op) { OPCODES }
#undef O

#undef IMM_MA
#undef IMM_BLA
#undef IMM_SLA
#undef IMM_IVA
#undef IMM_I64A
#undef IMM_LA
#undef IMM_NLA
#undef IMM_ILA
#undef IMM_IA
#undef IMM_DA
#undef IMM_SA
#undef IMM_RATA
#undef IMM_AA
#undef IMM_BA
#undef IMM_OA_IMPL
#undef IMM_OA
#undef IMM_VSA
#undef IMM_KA
#undef IMM_LAR
#undef IMM_FCA

#undef IMM_NA
#undef IMM_ONE
#undef IMM_TWO
#undef IMM_THREE
#undef IMM_FOUR
#undef IMM_FIVE
#undef IMM_SIX

#undef POP_NOV
#undef POP_ONE
#undef POP_TWO
#undef POP_THREE

#undef POP_CMANY
#undef POP_SMANY
#undef POP_CUMANY
#undef POP_FCALL
#undef POP_MFINAL
#undef POP_C_MFINAL

#undef PUSH_NOV
#undef PUSH_ONE
#undef PUSH_TWO
#undef PUSH_THREE
#undef PUSH_CMANY
#undef PUSH_FCALL

  };

  ret.blockOrder        = order_blocks(func);
  auto blockIt          = begin(ret.blockOrder);
  auto const endBlockIt = end(ret.blockOrder);
  for (; blockIt != endBlockIt; ++blockIt) {
    auto bid = *blockIt;
    auto& info = blockInfo[bid];
    auto const b = func.blocks()[bid].get();

    info.offset = fe.bcPos();
    FTRACE(2, "      block {}: {}\n", bid, info.offset);

    for (auto& fixup : info.forwardJumps) {
      fe.emitInt32(info.offset - fixup.instrOff, fixup.jmpImmedOff);
    }

    if (!info.expectedStackDepth) {
      // unreachable, or entry block
      info.expectedStackDepth = b->catchEntry ? 1 : 0;
    }

    currentStackDepth = *info.expectedStackDepth;

    auto fallthrough = b->fallthrough;
    auto end = b->hhbcs.end();
    auto flip = false;

    if (!is_single_nop(*b)) {
      // If the block ends with JmpZ or JmpNZ to the next block, flip
      // the condition to make the fallthrough the next block
      if (b->hhbcs.back().op == Op::JmpZ ||
          b->hhbcs.back().op == Op::JmpNZ) {
        auto const& bc = b->hhbcs.back();
        auto const target =
          bc.op == Op::JmpNZ ? bc.JmpNZ.target1 : bc.JmpZ.target1;
        if (std::next(blockIt) != endBlockIt && blockIt[1] == target) {
          fallthrough = target;
          --end;
          flip = true;
        }
      }

      for (auto iit = b->hhbcs.begin(); iit != end; ++iit) emit_inst(*iit);
      if (flip) {
        if (end->op == Op::JmpNZ) {
          emit_inst(bc_with_loc(end->srcLoc, bc::JmpZ { b->fallthrough }));
        } else {
          emit_inst(bc_with_loc(end->srcLoc, bc::JmpNZ { b->fallthrough }));
        }
      }
    }

    info.past = fe.bcPos();

    if (fallthrough != NoBlockId) {
      set_expected_depth(fallthrough);
      if (std::next(blockIt) == endBlockIt ||
          blockIt[1] != fallthrough) {
        emit_inst(bc::Jmp { fallthrough });

        auto const nextExnId = func.blocks()[fallthrough]->exnNodeId;
        auto const parent = commonParent(*func, nextExnId, b->exnNodeId);

        auto depth = [&] (ExnNodeId eid) {
          return eid == NoExnNodeId ? 0 : func->exnNodes[eid].depth;
        };
        // If we are in an exn region we pop from the current region to the
        // common parent. If the common parent is null, we pop all regions
        info.regionsToPop = depth(b->exnNodeId) - depth(parent);
        assertx(info.regionsToPop >= 0);
        FTRACE(4, "      popped catch regions: {}\n", info.regionsToPop);
      }
    }

    if (b->throwExit != NoBlockId) {
      FTRACE(4, "      throw: {}\n", b->throwExit);
    }
    if (fallthrough != NoBlockId) {
      FTRACE(4, "      fallthrough: {}\n", fallthrough);
    }
    FTRACE(2, "      block {} end: {}\n", bid, info.past);
  }

  return ret;
}

void emit_locals_and_params(UnitEmitter& ue, FuncEmitter& fe, const php::Func& func,
                            const EmitBcInfo& info) {
  Id id = 0;
  for (auto const& loc : func.locals) {
    if (loc.id < func.params.size()) {
      assertx(loc.name);
      assertx(!loc.killed);
      assertx(!loc.unusedName);
      auto& param = func.params[id];
      Func::ParamInfo pinfo;
      pinfo.defaultValue = param.defaultValue;
      pinfo.typeConstraints = param.typeConstraints;
      pinfo.userType = ue.mergeLitstr(param.userTypeConstraint);
      pinfo.phpCode = ue.mergeLitstr(param.phpCode);
      pinfo.userAttributes = param.userAttributes;
      if (param.inout) pinfo.setFlag(Func::ParamInfo::Flags::InOut);
      if (param.outOnly) pinfo.setFlag(Func::ParamInfo::Flags::OutOnly);
      if (param.readonly) pinfo.setFlag(Func::ParamInfo::Flags::Readonly);
      if (param.isVariadic) pinfo.setFlag(Func::ParamInfo::Flags::Variadic);
      if (param.isOptional) pinfo.setFlag(Func::ParamInfo::Flags::Optional);
      if (param.isNamed) pinfo.setFlag(Func::ParamInfo::Flags::Named);
      fe.appendParam(func.locals[id].name, pinfo);
      auto const dv = param.dvEntryPoint;
      if (dv != NoBlockId) {
        fe.params[id].funcletOff = info.blockInfo[dv].offset;
      }
      ++id;
    } else {
      if (loc.killed) continue;
      if (loc.name && !loc.unusedName && loc.name) {
        fe.allocVarId(loc.name);
        assertx(fe.lookupVarId(loc.name) == id);
        assertx(loc.id == id);
        ++id;
      } else {
        fe.allocUnnamedLocal();
        ++id;
      }
    }
  }
  for (auto const& loc : func.locals) {
    if (loc.killed && !loc.unusedName && loc.name) {
      fe.allocVarId(loc.name, true);
    }
  }

  if (debug) {
    for (auto const& loc : func.locals) {
      if (!loc.killed) {
        assertx(loc.id < fe.numLocals());
      }
      if (!loc.unusedName && loc.name) {
        assertx(loc.nameId < fe.numNamedLocals());
      }
    }
  }

  assertx(fe.numLocals() == id);
  fe.setNumIterators(func.numIters);
}

struct EHRegion {
  const php::ExnNode* node;
  EHRegion* parent;
  Offset start;
  Offset past;
};

template<class BlockInfo, class ParentIndexMap>
void emit_eh_region(FuncEmitter& fe,
                    const EHRegion* region,
                    const BlockInfo& blockInfo,
                    ParentIndexMap& parentIndexMap) {
  FTRACE(2,  "    func {}: ExnNode {}\n", fe.name, region->node->idx);

  auto const unreachable = [&] (const php::ExnNode& node) {
    return blockInfo[node.region.catchEntry].offset == kInvalidOffset;
  };

  // A region on a single empty block.
  if (region->start == region->past) {
    FTRACE(2, "    Skipping\n");
    return;
  } else if (unreachable(*region->node)) {
    FTRACE(2, "    Unreachable\n");
    return;
  }

  FTRACE(2, "    Process @ {}-{}\n", region->start, region->past);

  auto& eh = fe.addEHEnt();
  eh.m_base = region->start;
  eh.m_past = region->past;
  assertx(eh.m_past >= eh.m_base);
  assertx(eh.m_base != kInvalidOffset && eh.m_past != kInvalidOffset);

  // An unreachable parent won't be emitted (and thus its offset won't be set),
  // so find the closest reachable one.
  auto parent = region->parent;
  while (parent && unreachable(*parent->node)) parent = parent->parent;
  if (parent) {
    auto parentIt = parentIndexMap.find(parent);
    assertx(parentIt != end(parentIndexMap));
    eh.m_parentIndex = parentIt->second;
  } else {
    eh.m_parentIndex = -1;
  }
  parentIndexMap[region] = fe.ehtab.size() - 1;

  auto const& cr = region->node->region;
  eh.m_handler = blockInfo[cr.catchEntry].offset;
  eh.m_end = kInvalidOffset;
  eh.m_iterId = cr.iterId;

  assertx(eh.m_handler != kInvalidOffset);
}

void exn_path(const php::Func& func,
              std::vector<const php::ExnNode*>& ret,
              ExnNodeId id) {
  if (id == NoExnNodeId) return;
  auto const& n = func.exnNodes[id];
  exn_path(func, ret, n.parent);
  ret.push_back(&n);
}

// Return the count of shared elements in the front of two forward
// ranges.
template<class ForwardRange1, class ForwardRange2>
size_t shared_prefix(ForwardRange1& r1, ForwardRange2& r2) {
  auto r1it = begin(r1);
  auto r2it = begin(r2);
  auto const r1end = end(r1);
  auto const r2end = end(r2);
  auto ret = size_t{0};
  while (r1it != r1end && r2it != r2end && *r1it == *r2it) {
    ++ret; ++r1it; ++r2it;
  }
  return ret;
}

/*
 * Traverse the actual block layout, and find out the intervals for
 * each exception region in the tree.
 *
 * The basic idea here is that we haven't constrained block layout
 * based on the exception tree, but adjacent blocks are still
 * reasonably likely to have the same ExnNode.  Try to coalesce the EH
 * regions we create for in those cases.
 */
void emit_ehent_tree(FuncEmitter& fe, const php::WideFunc& func,
                     const EmitBcInfo& info) {
  hphp_fast_map<
    const php::ExnNode*,
    std::vector<std::unique_ptr<EHRegion>>
  > exnMap;

  /*
   * While walking over the blocks in layout order, we track the set
   * of "active" exnNodes.  This are a list of exnNodes that inherit
   * from each other.  When a new active node is pushed, begin an
   * EHEnt, and when it's popped, it's done.
   */
  std::vector<const php::ExnNode*> activeList;

  auto pop_active = [&] (Offset past) {
    auto p = activeList.back();
    activeList.pop_back();
    exnMap[p].back()->past = past;
  };

  auto push_active = [&] (const php::ExnNode* p, Offset start) {
    auto const parent = activeList.empty()
      ? nullptr
      : exnMap[activeList.back()].back().get();
    exnMap[p].push_back(
      std::make_unique<EHRegion>(
        EHRegion { p, parent, start, kInvalidOffset }
      )
    );
    activeList.push_back(p);
  };

  /*
   * Walk over the blocks, and compare the new block's exnNode path to
   * the active one.  Find the least common ancestor of the two paths,
   * then modify the active list by popping and then pushing nodes to
   * set it to the new block's path.
   */
  for (auto const bid : info.blockOrder) {
    auto const b = func.blocks()[bid].get();
    auto const offset = info.blockInfo[bid].offset;

    if (b->exnNodeId == NoExnNodeId) {
      while (!activeList.empty()) pop_active(offset);
      continue;
    }

    std::vector<const php::ExnNode*> current;
    exn_path(*func, current, b->exnNodeId);

    auto const prefix = shared_prefix(current, activeList);
    for (size_t i = prefix, sz = activeList.size(); i < sz; ++i) {
      pop_active(offset);
    }
    for (size_t i = prefix, sz = current.size(); i < sz; ++i) {
      push_active(current[i], offset);
    }

    for (int i = 0; i < info.blockInfo[bid].regionsToPop; i++) {
      // If the block ended in a jump out of the catch region, this effectively
      // ends all catch regions deeper than the one we are jumping to
      pop_active(info.blockInfo[bid].past);
    }

    if (debug && !activeList.empty()) {
      current.clear();
      exn_path(*func, current, activeList.back()->idx);
      assertx(current == activeList);
    }
  }

  while (!activeList.empty()) {
    pop_active(info.blockInfo[info.blockOrder.back()].past);
  }

  /*
   * We've created all our regions, but we need to sort them instead
   * of trying to get the UnitEmitter to do it.
   *
   * The UnitEmitter expects EH regions that look a certain way
   * (basically the way emitter.cpp likes them).  There are some rules
   * about the order it needs to have at runtime, which we set up
   * here.
   *
   * Essentially, an entry a is less than an entry b iff:
   *
   *   - a starts before b
   *   - a starts at the same place, but encloses b entirely
   *   - a has the same extents as b, but is a parent of b
   */
  std::vector<EHRegion*> regions;
  for (auto& mapEnt : exnMap) {
    for (auto& region : mapEnt.second) {
      regions.push_back(region.get());
    }
  }
  std::sort(
    begin(regions), end(regions),
    [&] (const EHRegion* a, const EHRegion* b) {
      if (a == b) return false;
      if (a->start == b->start) {
        if (a->past == b->past) {
          // When regions exactly overlap, the parent is less than the
          // child.
          for (auto p = b->parent; p != nullptr; p = p->parent) {
            if (p == a) return true;
          }
          // If a is not a parent of b, and they have the same region;
          // then b better be a parent of a.
          if (debug) {
            auto p = a->parent;
            for (; p != b && p != nullptr; p = p->parent) continue;
            assertx(p == b);
          }
          return false;
        }
        return a->past > b->past;
      }
      return a->start < b->start;
    }
  );

  hphp_fast_map<const EHRegion*,uint32_t> parentIndexMap;
  for (auto& r : regions) {
    emit_eh_region(fe, r, info.blockInfo, parentIndexMap);
  }
  fe.setEHTabIsSorted();
}

void emit_finish_func(EmitUnitState& state, UnitEmitter& ue, FuncEmitter& fe,
                      php::WideFunc& wf, const EmitBcInfo& info) {
  auto const& func = *wf;
  if (info.containsCalls) fe.containsCalls = true;

  emit_locals_and_params(ue, fe, func, info);
  emit_ehent_tree(fe, wf, info);
  wf.blocks().clear();

  fe.userAttributes = func.userAttributes;
  fe.retUserType = ue.mergeLitstr(func.returnUserType);
  fe.retTypeConstraints = func.retTypeConstraints;
  fe.typeParamNames = std::vector<PackedStringPtr>(
    func.typeParamNames.begin(),
    func.typeParamNames.end()
  );
  fe.originalUnit =
    func.originalUnit ? func.originalUnit : nullptr;
  fe.originalModuleName = func.originalModuleName;
  fe.requiresFromOriginalModule = func.requiresFromOriginalModule;
  fe.isClosureBody = func.isClosureBody;
  fe.isAsync = func.isAsync;
  fe.isGenerator = func.isGenerator;
  fe.isPairGenerator = func.isPairGenerator;
  fe.isNative = func.isNative;
  fe.isMemoizeWrapper = func.isMemoizeWrapper;
  fe.isMemoizeWrapperLSB = func.isMemoizeWrapperLSB;

  for (auto& name : func.staticCoeffects) fe.staticCoeffects.push_back(name);
  for (auto& rule : func.coeffectRules)   fe.coeffectRules.push_back(rule);

  auto const retTy = state.index.lookup_return_type_raw(&func).first.t;
  if (!retTy.subtypeOf(BBottom)) {
    fe.repoReturnType = make_repo_type(retTy);
  }

  if (is_specialized_wait_handle(retTy)) {
    auto const awaitedTy = wait_handle_inner(retTy);
    if (!awaitedTy.subtypeOf(BBottom)) {
      fe.repoAwaitedReturnType = make_repo_type(awaitedTy);
    }
  }

  fe.maxStackCells = info.maxStackDepth +
                     fe.numLocals() +
                     fe.numIterators() * kNumIterCells;

  fe.finish();
}

void renumber_locals(php::Func& func) {
  Id id = 0;
  Id nameId = 0;

  // We initialise local name ids in two passes, since locals that have not
  // been remapped may require their name be at the same offset as the local.
  // That's true for params, volatile locals, or locals in funcs with VarEnvs.
  //
  // In the first pass, we assume that all local names are used. Only in the
  // second pass do we apply the fact that some local names are never used.

  for (auto& loc : func.locals) {
    if (loc.killed) {
      // Make sure it's out of range, in case someone tries to read it.
      loc.id = INT_MAX;
    } else {
      loc.nameId = nameId++;
      loc.id = id++;
    }
  }
  for (auto& loc : func.locals) {
    if (loc.unusedName || !loc.name) {
      // Make sure it's out of range, in case someone tries to read it.
      loc.nameId = INT_MAX;
    } else if (loc.killed) {
      // The local uses a shared local slot, but its name is still used.
      loc.nameId = nameId++;
    }
  }
}

void emit_init_func(UnitEmitter& ue,
                    FuncEmitter& fe,
                    const php::Func& func,
                    Attr attrs) {
  fe.init(
    std::get<0>(func.srcInfo.loc),
    std::get<1>(func.srcInfo.loc),
    attrs | (func.sampleDynamicCalls ? AttrDynamicallyCallable : AttrNone),
    func.srcInfo.docComment,
    ue.isSystemLib()
  );
}

void emit_func(EmitUnitState& state, UnitEmitter& ue,
               FuncEmitter& fe, php::Func& f) {
  FTRACE(2,  "    func {}\n", f.name->data());
  assertx(f.attrs & AttrPersistent);
  auto attrs = f.attrs;
  attrSetter(
    attrs,
    f.isAsync &&
      !f.isGenerator &&
      !(f.attrs & AttrAbstract) &&
      !state.index.lookup_return_type_raw(&f).first.canSuspend,
    AttrEagerOnly
  );
  renumber_locals(f);
  emit_init_func(ue, fe, f, attrs);
  auto func = php::WideFunc::mut(&f);
  auto const info = emit_bytecode(state, ue, fe, func);
  emit_finish_func(state, ue, fe, func, info);
}

void emit_class(EmitUnitState& state, UnitEmitter& ue, PreClassEmitter* pce,
                php::Class& cls) {
  FTRACE(2, "    class: {}\n", cls.name->data());
  assertx(cls.attrs & AttrPersistent);
  pce->init(
    std::get<0>(cls.srcInfo.loc),
    std::get<1>(cls.srcInfo.loc),
    cls.attrs |
      (cls.sampleDynamicConstruct ? AttrDynamicallyConstructible : AttrNone),
    cls.parentName ? cls.parentName : staticEmptyString(),
    cls.srcInfo.docComment,
    ue.isSystemLib()
  );
  pce->setUserAttributes(cls.userAttributes);

  for (auto& x : cls.interfaceNames)     pce->addInterface(x);
  for (auto& x : cls.includedEnumNames)  pce->addEnumInclude(x);
  for (auto& x : cls.usedTraitNames)     pce->addUsedTrait(x);
  for (auto& x : cls.requirements)       pce->addClassRequirement(x);

  pce->setIfaceVtableSlot(state.index.lookup_iface_vtable_slot(&cls));

  bool needs86cinit = false;

  auto const nativeConsts = cls.attrs & AttrBuiltin ?
    Native::getClassConstants(cls.name) : nullptr;

  for (auto& cconst : cls.constants) {
    if (nativeConsts && nativeConsts->count(cconst.name)) {
      continue;
    }
    if (cconst.kind == ConstModifierFlags::Kind::Context) {
      assertx(cconst.cls->tsame(cls.name));
      assertx(!cconst.resolvedTypeStructure);
      assertx(cconst.invariance == php::Const::Invariance::None);
      pce->addContextConstant(
        cconst.name,
        std::vector<PackedStringPtr>(cconst.coeffects),
        cconst.isAbstract,
        cconst.isFromTrait
      );
    } else if (!cconst.val.has_value()) {
      assertx(cconst.cls->tsame(cls.name));
      assertx(!cconst.resolvedTypeStructure);
      assertx(cconst.invariance == php::Const::Invariance::None);
      pce->addAbstractConstant(
        cconst.name,
        cconst.kind,
        cconst.isFromTrait
      );
    } else {
      needs86cinit |= cconst.val->m_type == KindOfUninit;
      pce->addConstant(
        cconst.name,
        cconst.cls->tsame(cls.name) ? nullptr : cconst.cls,
        &cconst.val.value(),
        ArrNR{cconst.resolvedTypeStructure},
        cconst.kind,
        cconst.invariance,
        cconst.isFromTrait,
        cconst.isAbstract
      );
    }
  }

  for (auto& m : cls.methods) {
    if (!m) continue; // Removed
    if (!needs86cinit && m->name == s_86cinit.get()) continue;
    FTRACE(2, "    method: {}\n", m->name);
    auto const fe = ue.newMethodEmitter(m->name, pce);
    emit_func(state, ue, *fe, *m);
    pce->addMethod(fe);
  }

  CompactVector<Type> useVars;
  if (is_closure(cls)) {
    auto f = find_method(&cls, s_invoke.get());
    always_assert(f);
    useVars = state.index.lookup_closure_use_vars_raw(*f);
  }
  auto uvIt = useVars.begin();

  auto const privateProps   = state.index.lookup_private_props(&cls, true);
  auto const privateStatics = state.index.lookup_private_statics(&cls, true);
  auto const publicStatics  = state.index.lookup_public_statics(&cls);
  for (auto const& prop : cls.properties) {
    auto const makeRat = [&] (const Type& ty) -> RepoAuthType {
      if (!ty.subtypeOf(BCell)) return RepoAuthType{};
      if (ty.subtypeOf(BBottom)) {
        // A property can be TBottom if no sets (nor its initial value) is
        // compatible with its type-constraint, or if its LateInit and there's
        // no sets to it. The repo auth type here doesn't particularly matter,
        // since such a prop will be inaccessible.
        return RepoAuthType{};
      }
      return make_repo_type(ty);
    };

    auto const privPropTy = [&] (const PropState& ps) -> std::pair<Type, bool> {
      if (is_closure(cls)) {
        // For closures use variables will be the first properties of the
        // closure object, in declaration order
        if (uvIt != useVars.end()) return std::make_pair(*uvIt++, true);
        return std::make_pair(TCell, true);
      }

      auto it = ps.find(prop.name);
      if (it == end(ps)) return std::make_pair(TCell, true);
      return std::make_pair(it->second.ty, it->second.everModified);
    };

    auto propTy = TCell;
    auto everModified = true;
    auto attrs = prop.attrs;
    if (attrs & AttrPrivate) {
      std::tie(propTy, everModified) =
        privPropTy((attrs & AttrStatic) ? privateStatics : privateProps);
    } else if ((attrs & (AttrPublic|AttrProtected)) && (attrs & AttrStatic)) {
      std::tie(propTy, everModified) = [&] {
        auto const it = publicStatics.find(prop.name);
        if (it == end(publicStatics)) return std::make_pair(TCell, true);
        return std::make_pair(it->second.ty, it->second.everModified);
      }();
    }

    if (!everModified && (attrs & AttrStatic) && !propTy.is(BBottom)) {
      attrs |= AttrPersistent;
    }

    pce->addProperty(
      prop.name,
      attrs,
      prop.userType,
      TypeIntersectionConstraint(prop.typeConstraints),
      prop.docComment,
      &prop.val,
      makeRat(propTy),
      prop.userAttributes
    );
  }
  assertx(uvIt == useVars.end());

  pce->setEnumBaseTy(cls.enumBaseTy);
  std::vector<PackedStringPtr> names;
  for (auto const& name : cls.typeParamNames) {
    names.emplace_back(name);
  }
  pce->setTypeParamNames(std::move(names));
}

void emit_typealias(UnitEmitter& ue, const php::TypeAlias& alias) {
  assertx(alias.attrs & AttrPersistent);
  auto const te = ue.newTypeAliasEmitter(alias.name->toCppString());

  te->init(
    std::get<0>(alias.srcInfo.loc),
    std::get<1>(alias.srcInfo.loc),
    alias.attrs,
    alias.value,
    alias.kind,
    Array::attach(const_cast<ArrayData*>(alias.typeStructure)),
    Array::attach(const_cast<ArrayData*>(alias.resolvedTypeStructure))
  );
  te->setUserAttributes(alias.userAttrs);
}

void emit_constant(UnitEmitter& ue, const php::Constant& constant) {
  assertx(constant.attrs & AttrPersistent);
  Constant c {
    constant.name,
    constant.val,
    constant.attrs,
  };
  ue.addConstant(c);
}

void emit_module(UnitEmitter& ue, const php::Module& module) {
  Module m {
    module.name,
    module.srcInfo.docComment,
    (int)std::get<0>(module.srcInfo.loc),
    (int)std::get<1>(module.srcInfo.loc),
    module.attrs,
    module.userAttributes,
  };
  ue.addModule(m);
}

//////////////////////////////////////////////////////////////////////

}

std::unique_ptr<UnitEmitter> emit_unit(IIndex& index, php::Unit& unit) {
  SCOPE_ASSERT_DETAIL("emit_unit") { return unit.filename->toCppString(); };

  Trace::Bump bumper{
    Trace::hhbbc_emit, kSystemLibBump, is_systemlib_part(unit)
  };

  ContextPusher _{index, Context{ unit.filename, nullptr, nullptr }};

  assertx(!is_native_unit(unit));
  assertx(check(unit, index));

  auto extension = [&]() -> Extension* {
    if (unit.extName->empty()) return nullptr;
    auto const ext = ExtensionRegistry::get(unit.extName->data(), false);
    always_assert(ext);
    return ext;
  }();

  auto ue = std::make_unique<UnitEmitter>(SHA1{},
                                          SHA1{},
                                          unit.packageInfo,
                                          unit.attributes);
  FTRACE(1, "  unit {}\n", unit.filename->data());
  ue->m_sn = 0; // Will be set before writing to repo
  ue->m_filepath = unit.filename;
  ue->m_extension = extension;
  ue->m_metaData = unit.metaData;
  ue->m_fileAttributes = unit.fileAttributes;
  ue->m_moduleName = unit.moduleName;
  ue->m_softDeployedRepoOnly = unit.softDeployed;

  if (unit.fatalInfo) {
    // We should have dealt with verifier failures long ago.
    assertx(unit.fatalInfo->fatalLoc.has_value());
    ue->m_fatalUnit = true;
    ue->m_fatalOp = unit.fatalInfo->fatalOp;
    ue->m_fatalLoc = *unit.fatalInfo->fatalLoc;
    ue->m_fatalMsg = unit.fatalInfo->fatalMsg;
  }

  EmitUnitState state { index, &unit };

  // Go through all constants and see if they still need their
  // matching 86cinit func. In repo mode we are able to optimize away
  // most of them away. If the const doesn't need them anymore we
  // should not emit them.
  hphp_fast_set<const StringData*> const_86cinit_funcs;
  for (size_t id = 0; id < unit.constants.size(); ++id) {
    auto const& c = unit.constants[id];
    if (type(c->val) != KindOfUninit) {
      const_86cinit_funcs.emplace(Constant::funcNameFromName(c->name));
    }
  }

  index.for_each_unit_class_mutable(
    unit,
    [&] (php::Class& c) {
      // No reason to include closures unless there's a reachable
      // CreateCl.
      if (is_closure(c)) return;
      recordClass(state, *ue, c);
    }
  );

  index.for_each_unit_func_mutable(
    unit,
    [&] (php::Func& f) {
      if (const_86cinit_funcs.contains(f.name)) return;
      auto fe = ue->newFuncEmitter(f.name);
      emit_func(state, *ue, *fe, f);
    }
  );

  // Note that state.pces can grow inside the loop due to discovering
  // more closures.
  for (size_t idx = 0; idx < state.pces.size(); ++idx) {
    auto const [cls, pce] = state.pces[idx];
    emit_class(state, *ue, pce, *cls);
  }

  for (size_t id = 0; id < unit.typeAliases.size(); ++id) {
    emit_typealias(*ue, *unit.typeAliases[id]);
  }

  for (size_t id = 0; id < unit.constants.size(); ++id) {
    emit_constant(*ue, *unit.constants[id]);
  }

  for (size_t id = 0; id < unit.modules.size(); ++id) {
    emit_module(*ue, *unit.modules[id]);
  }

  ue->finish();
  return ue;
}

//////////////////////////////////////////////////////////////////////

}
