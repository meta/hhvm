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

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/trans-rec.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-text.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"

#include "hphp/util/configs/jit.h"
#include "hphp/util/data-block.h"

#include <folly/Random.h>

#include <vector>

namespace HPHP::jit {

struct IRInstruction;

///////////////////////////////////////////////////////////////////////////////

namespace vasm_detail {

///////////////////////////////////////////////////////////////////////////////

/*
 * Nouned verb class used to hide mostly-debug metadata updates from the main
 * body of vasm_emit().
 *
 * This is invoked on every Vinstr encountered in order to accumulate mappings
 * from higher-level representations.
 */
struct IRMetadataUpdater {
  IRMetadataUpdater(const Venv& env, AsmInfo* asm_info);

  /*
   * Update IR mappings for a Vinstr.
   */
  void register_inst(const Vinstr& inst);

  /*
   * Update IR mappings at the end of a block.
   */
  void register_block_end();

  /*
   * Update AsmInfo after the Vunit has been fully emitted.
   */
  void finish(const jit::vector<Vlabel>& labels);

private:
  struct Snippet {
    const IRInstruction* origin;
    TcaRange range;
  };

  /*
   * Get HHIR mapping info for the current block in `m_env'.
   */
  jit::vector<Snippet>& block_info();

private:
  const Venv& m_env;
  AsmInfo* m_asm_info;
  const IRInstruction* m_origin{nullptr};
  jit::vector<jit::vector<jit::vector<Snippet>>> m_area_to_blockinfos;
  std::vector<TransBCMapping>* m_bcmap{nullptr};
};

///////////////////////////////////////////////////////////////////////////////

/*
 * Is `block' an empty catch block?
 */
bool is_empty_catch(const Vblock& block);

/*
 * Register catch blocks for fixups.
 */
void register_catch_block(const Venv& env, const Venv::LabelPatch& p);

/*
 * Arch-independent emitters.
 *
 * Return true if the instruction was supported.
 */
template <class Inst>
bool emit(Venv& /*env*/, const Inst&) {
  return false;
}
bool emit(Venv& env, const callphpfe& i);
bool emit(Venv& env, const callphps& i);
bool emit(Venv& env, const bindjmp& i);
bool emit(Venv& env, const bindjcc& i);
bool emit(Venv& env, const bindaddr& i);
bool emit(Venv& env, const ldbindaddr& i);
bool emit(Venv& env, const fallback& i);
bool emit(Venv& env, const fallbackcc& i);

inline bool emit(Venv& env, const recordbasenativesp& i) {
  return true;
}

inline bool emit(Venv& env, const unrecordbasenativesp& i) {
  return true;
}

inline bool emit(Venv& env, const recordstack& i) {
  env.record_inline_stack(i.fakeAddress);
  return true;
}

inline bool emit(Venv&, const inlinestart&) {
  return true;
}

inline bool emit(Venv&, const inlineend&) {
  return true;
}

inline void record_frame(Venv& env, TCA start) {
  if (!env.origin ||
      !env.origin->marker().valid() ||
      !env.origin->marker().fp()) return;

  auto const& block = env.unit.blocks[env.current];
  auto const frameIt = env.unit.fpToFrame.find(env.origin->marker().fp());
  if (frameIt == env.unit.fpToFrame.end()) {
    assertx(env.origin->marker().sk().prologue());
    return;
  }

  auto const frame = frameIt->second;
  auto& frames = env.unit.frames;
  auto const size = env.cb->frontier() - start;
  // It's possible that (a) this block is empty, and (b) cb is full, so the
  // frontier from the start of the block is actually the first byte after the
  // block. This is particularly likely when RetranslateAll is in use as
  // ephemeral code blocks are resizable.
  always_assert(!size || env.cb->contains(start));
  always_assert((int64_t)size >= 0);
  auto const area = static_cast<uint8_t>(block.area_idx);
  frames[frame].sections[area].exclusive += size;
  for (auto f = frame; f != Vframe::Top; f = frames[f].parent) {
    frames[f].sections[area].inclusive += size;
  }
}

template<class Vemit>
void check_nop_interval(Venv& env, const Vinstr& inst,
                        uint32_t& nop_counter, uint32_t nop_interval) {
  if (LIKELY(nop_interval == 0)) return;

  if (nop_counter == 0) {
    // Initialize start position randomly.
    nop_counter = folly::Random::rand32(nop_interval) + 1;
  }

  switch (inst.op) {
    // These instructions are for exception handling or state syncing and do
    // not represent any actual work, so they're excluded from the nop counter.
    case Vinstr::landingpad:
    case Vinstr::nothrow:
    case Vinstr::syncpoint:
    case Vinstr::unwind:
      break;

    default:
      if (--nop_counter == 0) {
        // We use a special emit_nop() function rather than emit(nop{}) because
        // many performance counters exclude nops from their count of retired
        // instructions. It's up the to arch-specific backends to emit some
        // real work with no visible side-effects.
        Vemit(env).emit_nop();
        nop_counter = nop_interval;
      }
      break;
  }
}

template<class Vemit>
void emitLdBindRetAddrStubs(Venv& env) {
  jit::fast_map<SrcKey, CodeAddress, SrcKey::Hasher> stubs;
  env.cb = &env.text.areas().back().code;

  for (auto const& ldbindretaddr : env.ldbindretaddrs) {
    CodeAddress stub = [&] {
      auto const i = stubs.find(ldbindretaddr.target);
      if (i != stubs.end()) return i->second;

      auto const start = env.cb->frontier();
      stubs.insert({ldbindretaddr.target, start});

      // Store return value to the stack.
      Vemit(env).emit(store{rret_data(), rvmsp()[TVOFF(m_data)]});
      Vemit(env).emit(store{rret_type(), rvmsp()[TVOFF(m_type)]});

      // Bind jump to the translation.
      emit(env, bindjmp{
        ldbindretaddr.target,
        ldbindretaddr.spOff,
        cross_trace_regs_resumed()
      });

      return start;
    }();

    auto const addr = env.unit.makeAddr();
    assertx(env.vaddrs.size() == addr);
    env.vaddrs.push_back(stub);
    env.leas.push_back({ldbindretaddr.instr, addr});
  }
}

///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

inline Venv::Venv(Vunit& unit, Vtext& text, CGMeta& meta)
  : unit(unit)
  , text(text)
  , meta(meta)
{
  vaddrs.resize(unit.next_vaddr);
}

inline void Venv::record_inline_stack(TCA addr) {
  // Do not record stack if we are not inlining
  if (!origin || !origin->marker().valid() ||
      !origin->marker().fp()->inst()->is(DefCalleeFP)) return;

  auto const sk = origin->marker().sk();
  auto const callOff = sk.anyFuncEntry() ? sk.entryOffset() : sk.offset();
  auto const it = unit.fpToFrame.find(origin->marker().fp());
  assertx(it != unit.fpToFrame.end());

  stacks.emplace_back(addr, IStack{it->second - 1, callOff});
}

template<class Vemit>
void vasm_emit(Vunit& unit, Vtext& text, CGMeta& fixups,
               AsmInfo* asm_info) {
  using namespace vasm_detail;

  // Lower inlinestart and inlineend instructions to jmps, and annotate blocks
  // with inlined function parents

  Venv env { unit, text, fixups };
  env.addrs.resize(unit.blocks.size());

  auto labels = layoutBlocks(unit);

  IRMetadataUpdater irmu(env, asm_info);

  auto const area_start = [&] (Vlabel b) {
    auto area = unit.blocks[b].area_idx;
    return text.area(area).start;
  };

  // We don't want to put nops in Vunits representing stubs, and those Vunits
  // don't have a context set.
  auto const nop_interval =
    unit.context ? Cfg::Jit::NopInterval : uint32_t{0};
  auto nop_counter = uint32_t{0};

  for (int i = 0, n = labels.size(); i < n; ++i) {
    assertx(checkBlockEnd(unit, labels[i]));

    auto b = labels[i];
    auto& block = unit.blocks[b];

    env.cb = &text.area(block.area_idx).code;
    env.addrs[b] = env.cb->frontier();

    { // Compute the next block we will emit into the current area.
      auto const cur_start = area_start(labels[i]);
      auto j = i + 1;
      while (j < labels.size() &&
             cur_start != area_start(labels[j])) {
        j++;
      }
      env.next = j < labels.size() ? labels[j] : Vlabel(unit.blocks.size());
      env.current = b;
    }

    // We'll replace exception edges to empty catch blocks with the catch
    // helper unique stub.
    if (is_empty_catch(block)) continue;

    for (auto& inst : block.code) {
      irmu.register_inst(inst);
      env.origin = inst.origin;

      check_nop_interval<Vemit>(env, inst, nop_counter, nop_interval);

      auto const start = env.cb->frontier();
      switch (inst.op) {
#define O(name, imms, uses, defs)               \
        case Vinstr::name:                      \
          if (emit(env, inst.name##_)) break;   \
          Vemit(env).emit(inst.name##_);        \
          break;
        VASM_OPCODES
#undef O
      }
      record_frame(env, start);
    }

    irmu.register_block_end();
  }

  emitLdBindRetAddrStubs<Vemit>(env);

  Vemit::processVveneers(env);

  Vemit::emitVeneers(env);

  Vemit::handleLiterals(env);

  // Retarget smashable binds.
  Vemit::retargetBinds(env);

  // Patch up jump targets and friends.
  Vemit::patch(env);

  // Register catch blocks.
  for (auto& p : env.catches) register_catch_block(env, p);

  // Register inline frames.
  for (auto& f : unit.frames) {
    if (f.parent == Vframe::Top) continue; // skip the top frame
    auto const parent = f.parent != Vframe::Root
      ? f.parent - 1 : kRootIFrameID;
    fixups.inlineFrames.emplace_back(
      IFrame{f.func, f.callOff, f.sbToRootSbOff.offset, parent});
  }

  // Register inline stacks.
  fixups.inlineStacks = std::move(env.stacks);

  if (unit.padding) Vemit::pad(text.main().code);

  irmu.finish(labels);
}

///////////////////////////////////////////////////////////////////////////////

}
