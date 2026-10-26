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
#include "hphp/hhbbc/parse.h"

#include <map>

#include <variant>
#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <folly/gen/Base.h>
#include <fmt/core.h>
#include <folly/gen/String.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/sorted_vector_types.h>

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/ext/std/ext_std_misc.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/type-alias-emitter.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/debug.h"
#include "hphp/hhbbc/eval-cell.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/optimize.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/wide-func.h"

#include "hphp/util/configs/eval.h"

namespace HPHP::HHBBC {

TRACE_SET_MOD(hhbbc_parse)

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_toString("__toString");
const StaticString s_Stringish("Stringish");
const StaticString s_StringishObject("StringishObject");
const StaticString s_XHPChild("XHPChild");
const StaticString s_attr_Deprecated("__Deprecated");
const StaticString s___NoContextSensitiveAnalysis(
    "__NoContextSensitiveAnalysis");

//////////////////////////////////////////////////////////////////////

struct ParseUnitState {
  /*
   * This is computed once for each unit and stashed here.  We support
   * having either a SourceLocTable or a LineTable.  If we're
   * optimizing a repo that was already created by hphpc, it won't
   * have the full SourceLocTable information in it, so we're limited
   * to line numbers.
   */
  std::variant<SourceLocTable, LineTable> srcLocInfo;

  /*
   * Map from Closure name to the function containing the Closure's
   * associated CreateCl opcode.
   */
  TSStringToOneT<php::Func*> createClMap;

  /*
   * Possible identifiers that this unit references (either just in
   * cinit or elsewhere). This isn't exhaustive as it might miss some
   * references. This is fine, as it's only used to "prime" to
   * scheduler with possible dependencies.
   */
  SStringSet cinitPredeps;
  SStringSet predeps;

  struct SrcLocHash {
    size_t operator()(const php::SrcLoc& sl) const {
      auto const h1 = ((size_t)sl.start.col << 32) | sl.start.line;
      auto const h2 = ((size_t)sl.past.col << 32) | sl.past.line;
      return hash_int64_pair(h1, h2);
    }
  };
  hphp_fast_map<php::SrcLoc, int32_t, SrcLocHash> srcLocs;
};

//////////////////////////////////////////////////////////////////////

std::vector<Offset> findBasicBlocks(const FuncEmitter& fe) {
  std::vector<Offset> blockStarts;
  auto markBlock = [&] (Offset off) { blockStarts.emplace_back(off); };

  // Each entry point for a DV funclet is the start of a basic
  // block.
  for (auto& param : fe.params) {
    if (param.hasDefaultValue()) markBlock(param.funcletOff);
  }

  // The main entry point is also a basic block start.
  markBlock(0);

  /*
   * For each instruction, add it to the set if it must be the start
   * of a block.  It is the start of a block if it is:
   *
   *   - A jump target
   *
   *   - Immediatelly following a control flow instruction, other than
   *     a call.
   */
  auto offset = 0;
  for (;;) {
    auto const bc = fe.bc();
    auto const pc = bc + offset;
    auto const nextOff = offset + instrLen(pc);
    auto const atLast = nextOff == fe.bcPos();
    auto const op = peek_op(pc);
    auto const breaksBB =
      instrIsNonCallControlFlow(op) ||
      instrFlags(op) & TF ||
      (isFCall(op) && !instrJumpTargets(bc, offset).empty());

    if (breaksBB && !atLast) {
      markBlock(nextOff);
    }

    auto const targets = instrJumpTargets(bc, offset);
    for (auto const& target : targets) markBlock(target);

    offset = nextOff;
    if (atLast) break;
  }

  /*
   * Find blocks associated with exception handlers.
   *
   *   - The start of each EH protected region begins a block.
   *
   *   - The instruction immediately after the end of any
   *     EH protected region begins a block.
   *
   *   - Each catch entry point begins a block.
   *
   *   - The instruction immediately after the end of any
   *     catch region begins a block.
   */
  for (auto& eh : fe.ehtab) {
    markBlock(eh.m_base);
    markBlock(eh.m_past);
    markBlock(eh.m_handler);
    if (eh.m_end != kInvalidOffset) {
      markBlock(eh.m_end);
    }
  }

  // Now, each interval in blockStarts delinates a basic block.
  blockStarts.emplace_back(fe.bcPos());

  std::sort(blockStarts.begin(), blockStarts.end());
  blockStarts.erase(
    std::unique(blockStarts.begin(), blockStarts.end()),
    blockStarts.end()
  );
  return blockStarts;
}

struct ExnTreeInfo {
  /*
   * Map from EHEnt to the ExnNode that will represent exception
   * behavior in that region.
   */
  hphp_fast_map<const EHEnt*,ExnNodeId> ehMap;
};

template<class FindBlock>
ExnTreeInfo build_exn_tree(const FuncEmitter& fe,
                           php::Func& func,
                           FindBlock findBlock) {
  ExnTreeInfo ret;
  func.exnNodes.reserve(fe.ehtab.size());
  for (auto& eh : fe.ehtab) {
    auto const catchBlk = findBlock(eh.m_handler, true);
    auto node = php::ExnNode{};
    node.idx = func.exnNodes.size();
    node.parent = NoExnNodeId;
    node.depth = 1; // 0 depth means no ExnNode
    node.region = php::CatchRegion { catchBlk, eh.m_iterId };
    ret.ehMap[&eh] = node.idx;

    if (eh.m_parentIndex != -1) {
      auto it = ret.ehMap.find(&fe.ehtab[eh.m_parentIndex]);
      assertx(it != end(ret.ehMap));
      assertx(it->second < node.idx);
      node.parent = it->second;
      auto& parent = func.exnNodes[node.parent];
      node.depth = parent.depth + 1;
      parent.children.emplace_back(node.idx);
    }
    func.exnNodes.emplace_back(std::move(node));
  }

  return ret;
}

template<class T> T decode(PC& pc) {
  auto const ret = *reinterpret_cast<const T*>(pc);
  pc += sizeof ret;
  return ret;
}

template<class T> void decode(PC& pc, T& val) {
  val = decode<T>(pc);
}

MKey make_mkey(const php::Func& /*func*/, MemberKey mk) {
  switch (mk.mcode) {
    case MEL: case MPL:
      return MKey{mk.mcode, mk.local, mk.rop};
    case MEC: case MPC:
      return MKey{mk.mcode, mk.iva, mk.rop};
    case MET: case MPT: case MQT:
      return MKey{mk.mcode, mk.litstr, mk.rop};
    case MEI:
      return MKey{mk.mcode, mk.int64, mk.rop};
    case MW:
      return MKey{};
  }
  not_reached();
}

// Whether some string might be a valid class or func name. This
// doesn't have to be perfect as it's just an optimization.
bool is_possible_identifier(SString s) {
  return
    !s->empty() &&
    s->size() <= 96 &&
    std::isalpha(*s->data()) &&
    std::strspn(
      s->data(),
      "0123456789_\\"
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    ) == s->size();
}

template<class FindBlock>
void populate_block(ParseUnitState& puState,
                    const FuncEmitter& fe,
                    const UnitEmitter& ue,
                    php::Func& func,
                    php::Block& blk,
                    PC pc,
                    PC const past,
                    bool& sawCreateCl,
                    FindBlock findBlock) {
  auto decode_stringvec = [&] {
    auto const vecLen = decode_iva(pc);
    CompactVector<LSString> keys;
    keys.reserve(vecLen);
    for (auto i = size_t{0}; i < vecLen; ++i) {
      keys.push_back(ue.lookupLitstrId(decode<int32_t>(pc)));
    }
    return keys;
  };

  auto decode_switch = [&] (PC opPC) {
    SwitchTab ret;
    auto const vecLen = decode_iva(pc);
    for (int32_t i = 0; i < vecLen; ++i) {
      ret.push_back(findBlock(
        opPC + decode<Offset>(pc) - fe.bc()
      ));
    }
    return ret;
  };

  auto decode_sswitch = [&] (PC opPC) {
    SSwitchTab ret;

    auto const vecLen = decode_iva(pc);
    for (int32_t i = 0; i < vecLen - 1; ++i) {
      auto const id = decode<Id>(pc);
      auto const offset = decode<Offset>(pc);
      ret.emplace_back(ue.lookupLitstrId(id),
                       findBlock(opPC + offset - fe.bc()));
    }

    // Final case is the default, and must have a litstr id of -1.
    DEBUG_ONLY auto const defId = decode<Id>(pc);
    auto const defOff = decode<Offset>(pc);
    assertx(defId == -1);
    ret.emplace_back(nullptr, findBlock(opPC + defOff - fe.bc()));
    return ret;
  };

  auto createcl = [&] (const Bytecode& b) {
    sawCreateCl = true;
    auto const [existing, emplaced] =
      puState.createClMap.emplace(b.CreateCl.str2, &func);
    always_assert_flog(
      emplaced || existing->second == &func,
      "Closure {} used in CreateCl by two different functions '{}' and '{}'",
      b.CreateCl.str2,
      func_fullname(*existing->second),
      func_fullname(func)
    );
  };

  auto const add_predep = [&] (SString n) {
    is_86init_func(func)
      ? puState.cinitPredeps.emplace(n)
      : puState.predeps.emplace(n);
  };
  auto decodeNamedArgs = [&] (const Id nameId) ->
    std::unique_ptr<NamedArgNameVec> {
    if (nameId == kInvalidId) {
      return nullptr;
    }
    auto nameArr = ue.lookupArrayId(nameId);
    NamedArgNameVec nameVec;

    assertx(nameArr != nullptr);
    auto size = nameArr->size();
    for (auto i = 0; i < size; ++i) {
      auto name = nameArr->at(i);
      assertx(name.type() == KindOfString || name.type() == KindOfPersistentString);
      nameVec.push_back(name.val().pstr);
    }

    return std::make_unique<NamedArgNameVec>(std::move(nameVec));
  };

#define IMM_BLA(n)     auto targets = decode_switch(opPC);
#define IMM_SLA(n)     auto targets = decode_sswitch(opPC);
#define IMM_IVA(n)     auto arg##n = decode_iva(pc);
#define IMM_I64A(n)    auto arg##n = decode<int64_t>(pc);
#define IMM_LA(n)      auto loc##n = [&] {                       \
                         LocalId id = decode_iva(pc);            \
                         always_assert(id < func.locals.size()); \
                         return id;                              \
                       }();
#define IMM_NLA(n)     auto nloc##n = [&] {                         \
                         NamedLocal loc = decode_named_local(pc);   \
                         always_assert(loc.id < func.locals.size());\
                         return loc;                                \
                       }();
#define IMM_ILA(n)     auto loc##n = [&] {                       \
                         LocalId id = decode_iva(pc);            \
                         always_assert(id < func.locals.size()); \
                         return id;                              \
                       }();
#define IMM_IA(n)      auto iter##n = [&] {                      \
                         IterId id = decode_iva(pc);             \
                         always_assert(id < func.numIters);      \
                         return id;                              \
                       }();
#define IMM_DA(n)      auto dbl##n = decode<double>(pc);
#define IMM_SA(n)      auto str##n = ue.lookupLitstrId(decode<Id>(pc)); \
                       add_predep(str##n);
#define IMM_RATA(n)    auto rat = decodeRAT(ue, pc);
#define IMM_AA(n)      auto arr##n = ue.lookupArrayId(decode<Id>(pc));
#define IMM_BA(n)      assertx(next == past);     \
                       auto target##n = findBlock(  \
                         opPC + decode<Offset>(pc) - fe.bc());
#define IMM_OA_IMPL(n) subop##n; decode(pc, subop##n);
#define IMM_OA(type)   type IMM_OA_IMPL
#define IMM_VSA(n)     auto keys = decode_stringvec();
#define IMM_KA(n)      auto mkey = make_mkey(func, decode_member_key(pc, &ue));
#define IMM_LAR(n)     auto locrange = [&] {                             \
                         auto const range = decodeLocalRange(pc);        \
                         always_assert(range.first + range.count         \
                                       <= func.locals.size());           \
                         return LocalRange { range.first, range.count }; \
                       }();
#define IMM_ITA(n)     auto ita = decodeIterArgs(pc);
#define IMM_FCA(n)     auto fca = [&] {                                      \
                         auto const fca = decodeFCallArgs(op, pc, &ue);      \
                         auto const numBytes = (fca.numArgs + 7) / 8;        \
                         auto inoutArgs = fca.enforceInOut()                 \
                           ? std::make_unique<uint8_t[]>(numBytes)           \
                           : nullptr;                                        \
                         if (inoutArgs) {                                    \
                           memcpy(inoutArgs.get(), fca.inoutArgs, numBytes); \
                         }                                                   \
                         auto readonlyArgs = fca.enforceReadonly()           \
                           ? std::make_unique<uint8_t[]>(numBytes)           \
                           : nullptr;                                        \
                         if (readonlyArgs) {                                 \
                           memcpy(readonlyArgs.get(), fca.readonlyArgs, numBytes); \
                         }                                                   \
                         auto const aeOffset = fca.asyncEagerOffset;         \
                         auto const aeTarget = aeOffset != kInvalidOffset    \
                           ? findBlock(opPC + aeOffset - fe.bc())            \
                           : NoBlockId;                                      \
                         assertx(aeTarget == NoBlockId || next == past);     \
                         if (fca.context) add_predep(fca.context);           \
                         auto namedArgs = decodeNamedArgs(fca.namedArgNames);\
                         return FCallArgs(fca.flags, fca.numArgs,            \
                                          fca.numRets, std::move(inoutArgs), \
                                          std::move(readonlyArgs),           \
                                          std::move(namedArgs),              \
                                          aeTarget, fca.context);            \
                       }();

#define IMM_NA
#define IMM_ONE(x)                IMM_##x(1)
#define IMM_TWO(x, y)             IMM_##x(1) IMM_##y(2)
#define IMM_THREE(x, y, z)        IMM_TWO(x, y) IMM_##z(3)
#define IMM_FOUR(x, y, z, n)      IMM_THREE(x, y, z) IMM_##n(4)
#define IMM_FIVE(x, y, z, n, m)   IMM_FOUR(x, y, z, n) IMM_##m(5)
#define IMM_SIX(x, y, z, n, m, o) IMM_FIVE(x, y, z, n, m) IMM_##o(6)

#define IMM_ARG(which, n)         IMM_NAME_##which(n)
#define IMM_ARG_NA
#define IMM_ARG_ONE(x)                IMM_ARG(x, 1)
#define IMM_ARG_TWO(x, y)             IMM_ARG(x, 1), IMM_ARG(y, 2)
#define IMM_ARG_THREE(x, y, z)        IMM_ARG(x, 1), IMM_ARG(y, 2), \
                                      IMM_ARG(z, 3)
#define IMM_ARG_FOUR(x, y, z, l)      IMM_ARG(x, 1), IMM_ARG(y, 2), \
                                      IMM_ARG(z, 3), IMM_ARG(l, 4)
#define IMM_ARG_FIVE(x, y, z, l, m)   IMM_ARG(x, 1), IMM_ARG(y, 2), \
                                      IMM_ARG(z, 3), IMM_ARG(l, 4), \
                                      IMM_ARG(m, 5)
#define IMM_ARG_SIX(x, y, z, l, m, n) IMM_ARG(x, 1), IMM_ARG(y, 2), \
                                      IMM_ARG(z, 3), IMM_ARG(l, 4), \
                                      IMM_ARG(m, 5), IMM_ARG(n, 6)

#define O(opcode, imms, inputs, outputs, flags)                    \
  case Op::opcode:                                                 \
    {                                                              \
      auto b = [&] () -> Bytecode {                                \
        IMM_##imms /*these two macros advance the pc as required*/ \
        if (isTypeAssert(op)) return bc::Nop {};                   \
        return bc::opcode { IMM_ARG_##imms };                      \
      }();                                                         \
      b.srcLoc = srcLocIx;                                         \
      if (Op::opcode == Op::CreateCl)    createcl(b);              \
      blk.hhbcs.push_back(std::move(b));                           \
      assertx(pc == next);                                         \
    }                                                              \
    break;

  assertx(pc != past);
  do {
    auto const opPC = pc;
    auto const next = pc + instrLen(opPC);
    assertx(next <= past);

    auto const srcLoc = match<php::SrcLoc>(
      puState.srcLocInfo,
      [&] (const SourceLocTable& tab) {
        SourceLoc sloc;
        if (SourceLocation::getLoc(tab, opPC - fe.bc(), sloc)) {
          return php::SrcLoc {
            { static_cast<uint32_t>(sloc.line0),
              static_cast<uint32_t>(sloc.char0) },
            { static_cast<uint32_t>(sloc.line1),
              static_cast<uint32_t>(sloc.char1) }
          };
        }
        return php::SrcLoc{};
      },
      [&] (const LineTable& tab) {
        auto const line = SourceLocation::getLineNumber(tab, opPC - fe.bc());
        if (line != -1) {
          return php::SrcLoc {
            { static_cast<uint32_t>(line), 0 },
            { static_cast<uint32_t>(line), 0 },
          };
        };
        return php::SrcLoc{};
      }
    );

    auto const srcLocIx = puState.srcLocs.emplace(
      srcLoc, puState.srcLocs.size()).first->second;

    auto op = decode_op(pc);
    switch (op) { OPCODES }

    if (next == past) {
      if (instrAllowsFallThru(op)) {
        blk.fallthrough = findBlock(next - fe.bc());
      }
    }

    pc = next;
  } while (pc != past);

#undef O

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
#undef IMM_LAR
#undef IMM_ITA
#undef IMM_FCA

#undef IMM_NA
#undef IMM_ONE
#undef IMM_TWO
#undef IMM_THREE
#undef IMM_FOUR
#undef IMM_FIVE

#undef IMM_ARG
#undef IMM_ARG_NA
#undef IMM_ARG_ONE
#undef IMM_ARG_TWO
#undef IMM_ARG_THREE
#undef IMM_ARG_FOUR
#undef IMM_ARG_FIVE
#undef IMM_ARG_SIX

  /*
   * If a block ends with an unconditional jump, change it to a
   * fallthrough edge.
   *
   * If the jmp is the only instruction, convert it to a Nop, to avoid
   * creating an empty block (we have an invariant that no blocks are
   * empty).
   */

  auto make_fallthrough = [&] {
    blk.fallthrough = blk.hhbcs.back().Jmp.target1;
    if (blk.hhbcs.size() == 1) {
      blk.hhbcs.back() = bc_with_loc(blk.hhbcs.back().srcLoc, bc::Nop{});
    } else {
      blk.hhbcs.pop_back();
    }
  };

  switch (blk.hhbcs.back().op) {
  case Op::Jmp:   make_fallthrough();                              break;
  default:                                                         break;
  }
}

template<class FindBlk>
void link_entry_points(php::Func& func,
                       const FuncEmitter& fe,
                       FindBlk findBlock) {
  func.dvEntries.resize(fe.params.size(), NoBlockId);
  for (size_t i = 0, sz = fe.params.size(); i < sz; ++i) {
    if (fe.params[i].hasDefaultValue()) {
      auto const dv = findBlock(fe.params[i].funcletOff);
      func.params[i].dvEntryPoint = dv;
      func.dvEntries[i] = dv;
    }
  }
  func.mainEntry = findBlock(0);
}

void build_cfg(ParseUnitState& puState,
               php::Func& func,
               const FuncEmitter& fe,
               const UnitEmitter& ue) {
  auto const blockStarts = findBasicBlocks(fe);

  FTRACE(3, "    blocks are at: {}\n",
    [&]() -> std::string {
      using namespace folly::gen;
      return from(blockStarts)
        | eachTo<std::string>()
        | unsplit<std::string>(" ");
    }()
  );

  hphp_fast_map<Offset,std::pair<BlockId, copy_ptr<php::Block>>> blockMap;
  auto const bc = fe.bc();

  auto findBlock = [&] (Offset off, bool catchEntry = false) {
    auto& ent = blockMap[off];
    if (!ent.second) {
      auto blk         = php::Block{};
      ent.first        = blockMap.size() - 1;
      blk.exnNodeId    = NoExnNodeId;
      blk.catchEntry   = catchEntry;
      ent.second.emplace(std::move(blk));
    } else if (catchEntry) {
      ent.second.mutate()->catchEntry = true;
    }
    return ent.first;
  };

  auto exnTreeInfo = build_exn_tree(fe, func, findBlock);

  hphp_fast_map<BlockId, std::pair<int, int>> predSuccCounts;

  bool sawCreateCl = false;
  for (auto it = begin(blockStarts);
       std::next(it) != end(blockStarts);
       ++it) {
    auto const bid     = findBlock(*it);
    auto const block   = blockMap[*it].second.mutate();
    auto const bcStart = bc + *it;
    auto const bcStop  = bc + *std::next(it);

    if (auto const eh = Func::findEH(fe.ehtab, *it)) {
      auto it = exnTreeInfo.ehMap.find(eh);
      assertx(it != end(exnTreeInfo.ehMap));
      block->exnNodeId = it->second;
      block->throwExit = func.exnNodes[it->second].region.catchEntry;
    }

    populate_block(puState, fe, ue, func, *block, bcStart, bcStop,
                   sawCreateCl, findBlock);
    forEachNonThrowSuccessor(*block, [&] (BlockId blkId) {
        predSuccCounts[blkId].first++;
        predSuccCounts[bid].second++;
    });
  }
  func.hasCreateCl = sawCreateCl;

  link_entry_points(func, fe, findBlock);

  auto mf = php::WideFunc::create(func);
  mf.blocks().resize(blockMap.size());
  for (auto& kv : blockMap) {
    auto const blk = kv.second.second.mutate();
    auto const id = kv.second.first;
    blk->multiSucc = predSuccCounts[id].second > 1;
    blk->multiPred = predSuccCounts[id].first > 1;
    blk->hhbcs.shrink_to_fit();
    mf.blocks()[id] = std::move(kv.second.second);
  }
}

void add_frame_variables(php::Func& func, const FuncEmitter& fe, const UnitEmitter& ue) {
  for (auto& param : fe.params) {
    func.params.push_back(
      php::Param {
        param.defaultValue,
        NoBlockId,
        param.userType.get(ue),
        param.typeConstraints,
        param.userAttributes,
        param.phpCode.get(ue),
        param.isInOut(),
        param.isOutOnly(),
        param.isReadonly(),
        param.isVariadic(),
        param.isOptional(),
        param.isNamed(),
      }
    );
  }

  func.locals.reserve(fe.numLocals());
  for (LocalId id = 0; id < fe.numLocals(); ++id) {
    func.locals.push_back({
      .name = nullptr,
      .id = id,
      .killed = false,
      .nameId = id,
      .unusedName = false
    });
  }
  for (auto& kv : fe.localNameMap()) {
    func.locals[kv.second].name = kv.first;
  }

  func.numIters = fe.numIterators();
}

const StaticString
  s_construct("__construct"),
  s_DynamicallyCallable("__DynamicallyCallable"),
  s_ModuleLevelTrait("__ModuleLevelTrait");

std::unique_ptr<php::Func> parse_func(ParseUnitState& puState,
                                      php::Unit* unit,
                                      php::Class* cls,
                                      const FuncEmitter& fe,
                                      const UnitEmitter& ue) {
  if (fe.hasSourceLocInfo()) {
    puState.srcLocInfo = fe.createSourceLocTable();
  } else {
    puState.srcLocInfo = fe.lineTable();
  }

  FTRACE(2, "  func: {}\n",
    fe.name->data() && *fe.name->data() ? fe.name->data() : "pseudomain");

  auto ret         = std::make_unique<php::Func>();
  ret->name        = fe.name;
  ret->srcInfo     = php::SrcInfo { fe.getLocation(),
                                    fe.docComment };
  ret->unit        = unit->filename;
  ret->cls         = cls;

  ret->attrs       = static_cast<Attr>(
    (fe.attrs & ~AttrNoOverride & ~AttrInterceptable & ~AttrEagerOnly) |
    AttrPersistent
  );

  ret->userAttributes     = fe.userAttributes;
  ret->returnUserType     = fe.retUserType.get(ue);
  ret->retTypeConstraints = fe.retTypeConstraints;
  for (auto const& typeParam : fe.typeParamNames) {
    ret->typeParamNames.emplace_back(typeParam);
  }
  ret->originalUnit       = fe.originalUnit;
  ret->originalModuleName = unit->moduleName;

  ret->isClosureBody       = fe.isClosureBody;
  ret->isAsync             = fe.isAsync;
  ret->isGenerator         = fe.isGenerator;
  ret->isPairGenerator     = fe.isPairGenerator;
  ret->isMemoizeWrapper    = fe.isMemoizeWrapper;
  ret->isMemoizeWrapperLSB = fe.isMemoizeWrapperLSB;
  ret->isMemoizeImpl       = Func::isMemoizeImplName(fe.name);
  ret->isNative            = fe.isNative;
  ret->isReified           = fe.userAttributes.find(s___Reified.get()) !=
                             fe.userAttributes.end();
  ret->isReadonlyReturn    = fe.attrs & AttrReadonlyReturn;
  ret->isReadonlyThis      = fe.attrs & AttrReadonlyThis;
  ret->noContextSensitiveAnalysis = fe.userAttributes.find(
    s___NoContextSensitiveAnalysis.get()) != fe.userAttributes.end();
  ret->fromModuleLevelTrait = cls && cls->userAttributes.find(
    s_ModuleLevelTrait.get()) != cls->userAttributes.end();
  ret->hasInOutArgs        = [&] {
    for (auto& a : fe.params) if (a.isInOut()) return true;
    return false;
  }();
  ret->hasNamedParams = [&] {
    // A `named t...' parameter declares no named parameter, but it does mean
    // the call may carry named arguments, so the same conservatism applies.
    if (fe.attrs & AttrVariadicNamedParam) return true;
    for (auto& a : fe.params) if (a.isNamed()) return true;
    return false;
  }();

  // Assume true, will be updated in build_cfg().
  ret->hasCreateCl = true;

  auto const coeffectsInfo = getCoeffectsInfoFromList(
    fe.staticCoeffects, cls && fe.name == s_construct.get());
  ret->requiredCoeffects = coeffectsInfo.first.toRequired();
  ret->coeffectEscapes = coeffectsInfo.second;

  for (auto& name : fe.staticCoeffects) ret->staticCoeffects.push_back(name);
  for (auto& rule : fe.coeffectRules) ret->coeffectRules.push_back(rule);

  ret->sampleDynamicCalls = [&] {
    if (!(fe.attrs & AttrDynamicallyCallable)) return false;

    auto const it = fe.userAttributes.find(s_DynamicallyCallable.get());
    if (it == fe.userAttributes.end()) return false;

    assertx(isArrayLikeType(type(it->second)));
    auto const rate = val(it->second).parr->get(int64_t(0));
    if (!isIntType(type(rate)) || val(rate).num < 0) return false;

    ret->attrs = Attr(ret->attrs & ~AttrDynamicallyCallable);
    return true;
  }();

  add_frame_variables(*ret, fe, ue);

  if (!RuntimeOption::ConstantFunctions.empty()) {
    auto const name = [&] {
      if (!cls) return fe.name->toCppString();
      return fmt::format("{}::{}", cls->name->data(), ret->name->data());
    }();
    auto const it = RuntimeOption::ConstantFunctions.find(func_fullname(*ret));
    if (it != RuntimeOption::ConstantFunctions.end()) {
      ret->locals.resize(fe.params.size());
      ret->numIters = 0;
      ret->attrs |= AttrIsFoldable;

      auto const mainEntry = BlockId{0};

      auto blk         = php::Block{};
      blk.exnNodeId    = NoExnNodeId;
      blk.hhbcs = {gen_constant(it->second), bc::RetC {HPHP::VerifyRetKind::None}};

      auto mf = php::WideFunc::create(*ret);
      mf.blocks().emplace_back(std::move(blk));

      ret->dvEntries.resize(fe.params.size(), NoBlockId);
      ret->mainEntry = mainEntry;

      for (size_t i = 0, sz = fe.params.size(); i < sz; ++i) {
        if (fe.params[i].hasDefaultValue()) {
          ret->params[i].dvEntryPoint = mainEntry;
          ret->dvEntries[i] = mainEntry;
        }
      }
      return ret;
    }
  }

  /*
   * Builtin functions get some extra information.  The returnType flag is only
   * non-std::nullopt for these, but note that something may be a builtin and
   * still have a std::nullopt return type.
   */
  if (fe.isNative) {
    // We shouldn't be processing native functions in an extern-worker
    // job right now.
    assertx(!extern_worker::g_in_job);

    auto const f = [&] () -> HPHP::Func* {
      if (ret->cls) {
        auto const cls = Class::lookup(ret->cls->name);
        return cls ? cls->lookupMethod(ret->name) : nullptr;
      } else {
        return Func::lookupBuiltin(ret->name);
      }
    }();

    if (f && ret->params.size()) {
      for (auto i = 0; i < ret->params.size(); i++) {
        auto& pi = ret->params[i];
        if (pi.isVariadic || !f->params()[i].hasDefaultValue()) continue;
        if (pi.defaultValue.m_type == KindOfUninit &&
            pi.phpCode != nullptr) {
          auto res = eval_cell_value([&] {
              auto val = HHVM_FN(constant)(StrNR(pi.phpCode));
              val.setEvalScalar();
              return *val.asTypedValue();
            });
          if (!res) {
            FTRACE(4, "Argument {} to {}: Failed to evaluate {}\n",
                   i, f->fullName(), pi.phpCode);
            continue;
          }
          pi.defaultValue = *res;
        }
      }
    }
    if (!f || !f->nativeFuncPtr() ||
        (f->userAttributes().count(
          PackedStringPtr(s_attr_Deprecated.get())))) {
      ret->attrs |= AttrNoFCallBuiltin;
    }
  }

  build_cfg(puState, *ret, fe, ue);

  return ret;
}

void parse_methods(ParseUnitState& puState,
                   php::Class* ret,
                   php::Unit* unit,
                   const PreClassEmitter& pce,
                   const UnitEmitter& ue) {
  std::unique_ptr<php::Func> cinit;
  for (auto& me : pce.methods()) {
    auto f = parse_func(puState, unit, ret, *me, ue);
    if (f->name == s_86cinit.get()) {
      cinit = std::move(f);
    } else {
      f->clsIdx = ret->methods.size();
      ret->methods.emplace_back(std::move(f));
    }
  }
  if (cinit) {
    cinit->clsIdx = ret->methods.size();
    ret->methods.emplace_back(std::move(cinit));
  }
}

void add_stringish(php::Class* cls) {
  // The runtime adds StringishObject to any class providing a
  // __toString() function, so we mirror that here to make sure
  // analysis of interfaces is correct.  All StringishObjects are also
  // XHPChild, so handle it here as well.
  if (cls->attrs & AttrInterface && cls->name->tsame(s_StringishObject.get())) {
    return;
  }

  bool hasXHP = false;
  for (auto& iface : cls->interfaceNames) {
    if (iface->tsame(s_StringishObject.get())) return;
    if (iface->tsame(s_XHPChild.get())) { hasXHP = true; }
  }

  const auto has_toString = std::any_of(
    begin(cls->methods),
    end(cls->methods),
    [](const auto& func) { return func->name == s_toString.get(); });
  if (has_toString) {
    FTRACE(2, "Adding Stringish, StringishObject and XHPChild to {}\n",
           cls->name->data());
    cls->interfaceNames.push_back(s_StringishObject.get());
    if (!hasXHP && !cls->name->tsame(s_XHPChild.get())) {
      cls->interfaceNames.push_back(s_XHPChild.get());
    }
  }
}

const StaticString s_DynamicallyConstructible("__DynamicallyConstructible");

std::unique_ptr<php::Class> parse_class(ParseUnitState& puState,
                                        php::Unit* unit,
                                        const PreClassEmitter& pce,
                                        const UnitEmitter& ue) {
  FTRACE(2, "  class: {}\n", pce.name()->data());

  auto ret                = std::make_unique<php::Class>();
  ret->name               = pce.name();
  ret->srcInfo            = php::SrcInfo { pce.getLocation(),
                                           pce.docComment() };
  ret->unit               = unit->filename;
  ret->closureContextCls  = nullptr;
  ret->parentName         = pce.parentName()->empty() ? nullptr
                                                      : pce.parentName();
  ret->attrs              = static_cast<Attr>(
    (pce.attrs() & ~(AttrNoOverride | AttrNoOverrideRegular)) |
    AttrPersistent);
  ret->userAttributes     = pce.userAttributes();
  ret->hasReifiedGenerics = ret->userAttributes.find(s___Reified.get()) !=
                            ret->userAttributes.end();
  ret->hasConstProp       = false;
  ret->moduleName         = unit->moduleName;

  ret->sampleDynamicConstruct = [&] {
    if (!(ret->attrs & AttrDynamicallyConstructible)) return false;

    auto const it = ret->userAttributes.find(s_DynamicallyConstructible.get());
    if (it == ret->userAttributes.end()) return false;

    assertx(isArrayLikeType(type(it->second)));
    auto const rate = val(it->second).parr->get(int64_t(0));
    if (!isIntType(type(rate)) || val(rate).num < 0) return false;

    ret->attrs = Attr(ret->attrs & ~AttrDynamicallyConstructible);
    return true;
  }();

  if (ret->parentName) puState.predeps.emplace(ret->parentName);

  for (auto& iface : pce.interfaces()) {
    ret->interfaceNames.push_back(iface);
    puState.predeps.emplace(iface);
  }
  for (auto& enumInclude : pce.enumIncludes()) {
    ret->includedEnumNames.push_back(enumInclude);
    puState.predeps.emplace(enumInclude);
  }

  copy(ret->usedTraitNames,  pce.usedTraits());
  copy(ret->requirements,    pce.requirements());

  parse_methods(puState, ret.get(), unit, pce, ue);
  add_stringish(ret.get());

  auto& propMap = pce.propMap();
  for (size_t idx = 0; idx < propMap.size(); ++idx) {
    auto& prop = propMap[idx];
    assertx(prop.typeConstraints().validForProp());
    ret->properties.push_back(
      php::Prop {
        prop.name(),
        static_cast<Attr>(
          prop.attrs() & ~(AttrNoBadRedeclare |
                           AttrNoImplicitNullable |
                           AttrInitialSatisfiesTC)
        ),
        prop.userAttributes(),
        prop.docComment(),
        prop.userType(),
        TypeIntersectionConstraint(prop.typeConstraints()),
        prop.val()
      }
    );
    if ((prop.attrs() & (AttrStatic | AttrIsConst)) == AttrIsConst) {
      ret->hasConstProp = true;
    }
  }

  auto const getTypeStructureConst = [&] (const PreClassEmitter::Const& cconst) {
    auto const val = cconst.valOption();
    if (!val.has_value() || !isArrayLikeType(val->type())) return val;
    auto const ad = val->val().parr;
    type_structure_references(ad, puState.cinitPredeps);
    if (!Cfg::Eval::EmitBespokeTypeStructures) return val;
    if (!bespoke::TypeStructure::isValidTypeStructure(ad)) return val;
    auto const ts = bespoke::TypeStructure::MakeFromVanillaStatic(ad, true);
    return make_optional(make_tv<KindOfPersistentDict>(ts));
  };

  auto& constMap = pce.constMap();
  for (size_t idx = 0; idx < constMap.size(); ++idx) {
    auto& cconst = constMap[idx];
    auto const cconstValue = (cconst.kind() == ConstModifierFlags::Kind::Type)
      ? getTypeStructureConst(cconst)
      : cconst.valOption();

    ret->constants.push_back(
      php::Const {
        cconst.name(),
        ret->name,
        cconstValue,
        cconst.coeffects(),
        nullptr,
        cconst.kind(),
        php::Const::Invariance::None,
        cconst.isAbstract(),
        cconst.isFromTrait(),
        false
      }
    );
  }

  if (ret->attrs & AttrBuiltin) {
    // We shouldn't be processing any builtins in an extern-worker job
    // right now.
    assertx(!extern_worker::g_in_job);

    if (auto nativeConsts = Native::getClassConstants(ret->name)) {
      for (auto const& cnsMap : *nativeConsts) {
        TypedValueAux tvaux;
        tvCopy(cnsMap.second, tvaux);
        tvaux.constModifiers() = {};
        ret->constants.push_back(
          php::Const {
            cnsMap.first,
            ret->name,
            tvaux,
            {},
            nullptr,
            ConstModifierFlags::Kind::Value,
            php::Const::Invariance::None,
            false,
            false,
            false
          }
        );
      }
    }
  }

  ret->enumBaseTy = pce.enumBaseTy();
  for (auto const& name : pce.getTypeParamNames()) {
    ret->typeParamNames.push_back(name);
  }

  return ret;
}

//////////////////////////////////////////////////////////////////////

void assign_closure_context(const ParseUnitState&, php::Class*);

std::pair<LSString, bool>
find_closure_context(const ParseUnitState& puState,
                     php::Func* createClFunc) {
  if (auto const cls = createClFunc->cls) {
    if (is_closure(*cls)) {
      // We have a closure created by a closure's invoke method, which
      // means it should inherit the outer closure's context, so we
      // have to know that first.
      assign_closure_context(puState, cls);
      if (cls->closureContextCls) {
        return std::make_pair(cls->closureContextCls, true);
      }
      if (cls->closureDeclFunc) {
        return std::make_pair(cls->closureDeclFunc, false);
      }
      always_assert_flog(
        false,
        "Closure {} defined in unit {} lacks an enclosing context",
        cls->name,
        cls->unit
      );
    }
    return std::make_pair(cls->name, true);
  }
  return std::make_pair(createClFunc->name, false);
}

void assign_closure_context(const ParseUnitState& puState,
                            php::Class* clo) {
  assertx(is_closure(*clo));

  if (clo->closureContextCls || clo->closureDeclFunc) return;

  auto const clIt = puState.createClMap.find(clo->name);
  if (clIt == end(puState.createClMap)) {
    // Unused closure class.  Technically not prohibited by the spec.
    return;
  }

  auto const [ctx, isClass] = find_closure_context(puState, clIt->second);
  if (isClass) {
    clo->closureContextCls = ctx;
    clo->closureDeclFunc = nullptr;
  } else {
    clo->closureContextCls = nullptr;
    clo->closureDeclFunc = ctx;
  }
}

//////////////////////////////////////////////////////////////////////

}

std::unique_ptr<php::Constant> parse_constant(const Constant& c) {
  return std::unique_ptr<php::Constant>(new php::Constant{
    c.name,
    c.val,
    c.attrs | AttrPersistent
  });
}

std::unique_ptr<php::Module> parse_module(const Module& m) {
  return std::unique_ptr<php::Module>(new php::Module{
    m.name,
    php::SrcInfo {
      {m.line0, m.line1},
      m.docComment
    },
    m.attrs | AttrPersistent,
    m.userAttributes,
  });
}

std::unique_ptr<php::TypeAlias> parse_type_alias(ParseUnitState& pu,
                                                 const TypeAliasEmitter& te) {
  FTRACE(2, "  type alias: {}\n", te.name()->data());

  auto const ts = [&] () -> SArray {
    auto const a = te.typeStructure();
    if (a.isNull()) return nullptr;
    assertx(a->isStatic());
    if (!Cfg::Eval::EmitBespokeTypeStructures) return a.get();
    if (!bespoke::TypeStructure::isValidTypeStructure(a.get())) return a.get();
    return bespoke::TypeStructure::MakeFromVanillaStatic(a.get(), true);
  }();
  if (ts) type_structure_references(ts, pu.cinitPredeps);

  return std::unique_ptr<php::TypeAlias>(new php::TypeAlias {
    php::SrcInfo { te.getLocation() },
    te.name(),
    te.attrs() | AttrPersistent,
    te.value(),
    te.kind(),
    te.userAttributes(),
    ts,
    nullptr
  });
}

ParsedUnit parse_unit(const UnitEmitter& ue) {
  Trace::Bump bumper{Trace::hhbbc_parse, kSystemLibBump, ue.isSystemLib()};
  FTRACE(2, "parse_unit {}\n", ue.m_filepath->data());

  ParsedUnit ret;
  ret.unit = std::make_unique<php::Unit>();

  ret.unit->filename       = ue.m_filepath;
  ret.unit->metaData       = ue.m_metaData;
  ret.unit->fileAttributes = ue.m_fileAttributes;
  ret.unit->moduleName     = ue.m_moduleName;
  ret.unit->packageInfo    = ue.m_packageInfo;
  ret.unit->attributes     = ue.m_attributes;
  ret.unit->softDeployed   = ue.m_softDeployedRepoOnly;

  ret.unit->extName        = [&]{
    if (ue.m_extension) {
      return makeStaticString(ue.m_extension->getName());
    }
    return staticEmptyString();
  }();

  if (Cfg::Eval::AbortBuildOnVerifyError && !ue.check(false)) {
    // Record a FatalInfo without a location. This represents a
    // verifier failure.
    php::FatalInfo fi{
      std::nullopt,
      FatalOp::Parse,
      fmt::format(
        "The unoptimized unit for {} did not pass verification, "
        "bailing because Eval.AbortBuildOnVerifyError is set\n",
        ue.m_filepath->data()
      )
    };
    ret.unit->fatalInfo = std::make_unique<php::FatalInfo>(std::move(fi));
    return ret;
  }

  if (ue.m_fatalUnit) {
    php::FatalInfo fi{ue.m_fatalLoc, ue.m_fatalOp, ue.m_fatalMsg};
    ret.unit->fatalInfo = std::make_unique<php::FatalInfo>(std::move(fi));
  }

  ParseUnitState puState;

  for (auto const pce : ue.preclasses()) {
    auto cls = parse_class(puState, ret.unit.get(), *pce, ue);
    ret.unit->classes.emplace_back(cls->name);
    ret.classes.emplace_back(std::move(cls));
  }

  for (auto const& fe : ue.fevec()) {
    assertx(!fe->pce());
    auto func = parse_func(puState, ret.unit.get(), nullptr, *fe, ue);
    ret.unit->funcs.emplace_back(func->name);
    ret.funcs.emplace_back(std::move(func));
  }

  ret.unit->srcLocs.resize(puState.srcLocs.size());
  for (auto const& srcInfo : puState.srcLocs) {
    ret.unit->srcLocs[srcInfo.second] = srcInfo.first;
  }

  for (auto const& te : ue.typeAliases()) {
    ret.unit->typeAliases.emplace_back(parse_type_alias(puState, *te));
  }

  for (auto const& c : ue.constants()) {
    ret.unit->constants.emplace_back(parse_constant(c));
  }

  for (auto const& m : ue.modules()) {
    ret.unit->modules.emplace_back(parse_module(m));
  }

  TSStringToOneT<php::Class*> classMap;
  classMap.reserve(ret.classes.size());
  for (auto const& c : ret.classes) {
    classMap.try_emplace(c->name, c.get());
  }

  // Remove closures declared in other classes from the unit's class
  // list. These are owned by their declaring classes, not the unit.
  ret.classes.erase(
    std::remove_if(
      begin(ret.classes), end(ret.classes),
      [&] (std::unique_ptr<php::Class>& c) {
        if (!is_closure(*c)) return false;
        assign_closure_context(puState, c.get());
        if (!c->closureContextCls) return false;
        auto const ctx = folly::get_default(classMap, c->closureContextCls);
        always_assert(ctx);
        ctx->closures.emplace_back(std::move(c));
        return true;
      }
    ),
    end(ret.classes)
  );

  if (debug) {
    // Make sure all closures in our createClMap (which are just
    // strings) actually exist in this unit (CreateCls should not be
    // referring to classes outside of their unit).
    TSStringSet classes;
    for (auto const& c : ret.classes) {
      classes.emplace(c->name);
      for (auto const& clo : c->closures) classes.emplace(clo->name);
    }
      for (auto const [name, _] : puState.createClMap) {
      always_assert(classes.contains(name));
    }

    for (auto const& f : ret.funcs)   always_assert(check(*f));
    for (auto const& c : ret.classes) always_assert(check(*c));
  }
  state_after("parse", ret);

  folly::erase_if(
    puState.cinitPredeps,
    [] (SString s) { return !is_possible_identifier(s); }
  );
  folly::erase_if(
    puState.predeps,
    [] (SString s) { return !is_possible_identifier(s); }
  );
  ret.cinitPredeps = std::move(puState.cinitPredeps);
  ret.predeps = std::move(puState.predeps);

  return ret;
}

//////////////////////////////////////////////////////////////////////

}
