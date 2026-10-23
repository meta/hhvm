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

#include <fmt/format.h>

#include "hphp/runtime/vm/jit/mutation.h"

#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/state-vector.h"
#include "hphp/runtime/vm/jit/timer.h"

#include "hphp/util/dataflow-worklist.h"

namespace HPHP::jit {

TRACE_SET_MOD(hhir)

namespace {

//////////////////////////////////////////////////////////////////////

bool retypeDst(IRInstruction* inst, int num) {
  auto const ssa = inst->dst(num);
  auto const oldType = ssa->type();

  /*
   * The type of a tmp defined by DefLabel is the union of the types of the
   * tmps at each incoming Jmp.
   */
  if (inst->op() == DefLabel) {
    auto type = TBottom;
    inst->block()->forEachSrc(num, [&] (IRInstruction*, SSATmp* tmp) {
      type = type | tmp->type();
    });
    ssa->setType(type);
    return type != oldType;
  }

  auto const newType = outputType(inst, num);
  ssa->setType(newType);

  return newType != oldType;
}

//////////////////////////////////////////////////////////////////////

struct RefineTmps {
  TRACE_SET_MOD(hhir_refineTmps)

  explicit RefineTmps(IRUnit& unit)
    : unit{unit}
    , rpoBlocks{rpoSortCfg(unit)}
    , blockState{unit, State{}} {}

  /*
   * Overview of algorithm:
   *
   * For each block we maintain a map of "canonical" SSATmps to what
   * they should be rewritten to.
   *
   * A SSATmp is canonical if it's produced by a non-passthrough
   * instruction. SSATmps are canonicalized by chasing passthrough
   * instructions which define them, until we hit a
   * non-passthrough. DefLabels have special handling. If all sources
   * of the DefLabel canonicalize to the same SSATmp, we treat it as a
   * passthrough. If not, they're considered "normal" defining
   * instructions. Canonical values for DefLabels are pre-calculated
   * using dataflow.
   *
   * A SSATmp can be rewritten to another SSATmp, or to a phi. For
   * each block, union together the rewrite maps in each
   * predecessor. If two predecessors disagree, then the rewrite
   * becomes a phi at the current block. A SSATmp not present in the
   * map implicitly means it rewrites to itself.
   *
   * Once the rewrites for the block start have been set up, we walk
   * through each instruction in the block. If the instruction uses a
   * (canonicalized) SSATmp in the rewrite map, and the rewrite target
   * is a SSATmp, rewrite the use to the target SSATmp. If the rewrite
   * target is a phi (whether this block or another), record the usage
   * in a side map, but don't modify the use.
   *
   * If the instruction is a passthrough, store the instruction's
   * (canonicalized) source in the rewrite map, with the dest of the
   * instruction as the rewrite target. Again, DefLabel's are treated
   * as passthrough or not depending on how its sources canonicalize.
   *
   * Store the rewrite map (updated from processing the instructions)
   * in the block's out state (next and/or taken). Do the block
   * processing in the typical dataflow manner, scheduling
   * predecessors and rescheduling a block every time one of its
   * successor's changes state. This is guaranteed to terminate
   * because the rewrite map (for every canonical SSATmp) can either
   * stay the same, or turn into a phi. Once it becomes a phi, it
   * stays that way.
   *
   * Once the dataflow is done, we have the rewrite maps for every
   * block, and all SSATmp uses (non-phi) have been rewritten. We then
   * materialize all *used* phis. A phi might have been created in the
   * rewrite maps, but never used. During dataflow we recorded which
   * phis were actually used.
   *
   * Phis are materialized by creating a DefLabel at their associated
   * block. The inputs for the phi are done by looking at the rewrite
   * maps in each predecessor. If the rewrite is to a SSATmp, or to an
   * already materialized phi, we simply use the SSATmp. If not, we
   * materialize the non-materialized phi from the pred
   * recursively. This requires a bit of trickery because DefLabels
   * might be mutually recursive and we can end up in an infinite
   * loop. We store "dummy" SSATmps in the DefLabel, then recursively
   * materialize the dependent phis, then fixup the dummy SSATmps.
   *
   * Once all (used) phis are materialized, we have SSATmps for all of
   * them. Iterate over the instructions which use those phis
   * (gathered during dataflow), and rewrite the remaining uses.
   */
  bool operator()() {
    ITRACE(1, "refine-tmps:vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\n");
    SCOPE_EXIT {
      ITRACE(1, "refine-tmps:^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
    };
    Trace::Indent indenter;

    // First calculate canonical values for DefLabels
    calcDefLabelCanons();

    // Then calculate the dataflow to see which rewrites are present
    // at each block. Non-phi SSATmps are rewritten as we go. If no
    // phis are created, this is sufficient.
    auto const changed = dataflow();
    if (usedPhis.empty()) {
      ITRACE(2, "no phis to insert\n");
      return changed;
    }

    // Otherwise we have phis we need to create. Sort them according
    // to RPO order. This lets us tend to create phis before the
    // usages of the phis, which makes for less fixups needed.
    jit::vector<Phi> toMaterialize;
    for (auto const& phi : usedPhis) {
      assertx(!phi.second);
      toMaterialize.emplace_back(phi.first);
    }
    std::sort(
      toMaterialize.begin(), toMaterialize.end(),
      [&] (auto const& p1, auto const& p2) {
        auto const order1 = blockState[p1.block].order;
        auto const order2 = blockState[p2.block].order;
        if (order1 != order2) return order1 < order2;
        return p1.canon->id() < p2.canon->id();
      }
    );

    ONTRACE(
      2,
      ITRACE(2, "To Materialize:\n");
      for (auto const& phi : toMaterialize) {
        ITRACE(2, "  B{} for {}\n", phi.block->id(), *phi.canon);
      }
    );

    // Materialize phis:
    for (auto const& phi : toMaterialize) materializePhi(phi);

    // At this point everything is created. We only need to rewrite
    // any instructions which use one of the materialized phis. We
    // kept an explicit list of these, so we don't need to walk the
    // entire unit.
    for (auto const& use : phiUses) {
      auto inst = use.first.inst;
      auto const idx = use.first.srcIdx;
      auto const block = use.second.block;
      auto const canon = use.second.canon;
      assertx(idx < inst->numSrcs());
      auto const it = usedPhis.find(Phi{block, canon});
      assertx(it != usedPhis.end());
      assertx(it->second);
      ITRACE(2, "Rewriting {} -> {} in {}\n",
             *inst->src(idx), *it->second, inst->toString());
      inst->setSrc(idx, it->second);
    }

    return true;
  }

  // Use dataflow to calculate the appropriate rewrites for each
  // block. If any phis need to be created, record them in
  // `usedPhis'. Record any instructions which will use a phi SSATmp,
  // record it in `phiUses'.
  bool dataflow() {
    dataflow_worklist<size_t> worklist{rpoBlocks.size()};
    for (uint32_t i = 0; i < rpoBlocks.size(); ++i) {
      worklist.push(i);
      blockState[rpoBlocks[i]].order = i;
    }

    auto changed = false;

    do {
      auto const block = rpoBlocks[worklist.pop()];
      auto& state = blockState[block];

      ITRACE(2, "Processing B{}:\n", block->id());
      Trace::Indent indenter;

      // Build up the in state from predecessors:
      auto remaps = [&] {
        if (block == unit.entry()) return Remaps{};

        Optional<Remaps> ret;
        block->forEachPred(
          [&] (Block* pred) {
            assertx(block == pred->next() || block == pred->taken());
            auto const& predState = block == pred->next()
              ? blockState[pred].outNext
              : blockState[pred].outTaken;
            // If the predecessor wasn't visited yet, ignore it. If we
            // visit it later, we'll reschedule this block.
            if (!predState) return;
            if (!ret) {
              ret = *predState;
              return;
            }

            // Merge together the remaps between the current state and
            // this pred. If there's any mismatch, we produce a phi at
            // this block.
            for (auto& remap : *ret) {
              assertx(RemapTo{remap.first} != remap.second);
              auto const o = [&] {
                auto const it = predState->find(remap.first);
                if (it == predState->end()) return RemapTo{remap.first};
                return it->second;
              }();
              remap.second.merge(o, block);
            }

            for (auto const& remap : *predState) {
              assertx(RemapTo{remap.first} != remap.second);
              auto const it = ret->find(remap.first);
              if (it != ret->end()) continue;
              ret->emplace(remap.first, RemapTo{block});
            }
          }
        );

        assertx(ret.has_value());
        return *ret;
      }();

      ONTRACE(
        4,
        ITRACE(4, "Remaps (initial):\n");
        for (auto const& remap : remaps) {
          ITRACE(4, "  {} -> {}\n", *remap.first, remap.second.toString());
        }
      );

      // Now visit each instruction in the block, rewriting sources,
      // and updating rewrites if we see a passthrough instruction.
      for (auto& inst : *block) {
        // If this instruction has a taken edge (which means it's a
        // block end), we need to push the state before processing
        // this instruction.
        if (auto const taken = inst.taken()) {
          if (state.outTaken != remaps) {
            ITRACE(4, "Scheduling taken block B{}\n", taken->id());
            worklist.push(blockState[taken].order);
            if (!inst.next() && inst.numSrcs() == 0) {
              // Optimization. If this instruction doesn't have a next
              // edge (so just taken), and has no sources, we can move
              // the state and not process the instruction.
              state.outTaken = std::move(remaps);
              continue;
            } else {
              // Otherwise we need to copy it, since we might need to
              // use it.
              state.outTaken = remaps;
            }
          } else if (!inst.next() && inst.numSrcs() == 0) {
            continue;
          }
        }

        // Perform rewrites. If we have none, we don't have to visit
        // the sources.
        if (!remaps.empty()) {
          for (uint32_t i = 0; i < inst.numSrcs(); ++i) {
            auto const from = canonicalize(inst.src(i));
            auto const it = remaps.find(from);
            if (it == remaps.end()) continue;
            if (auto const to = it->second.ssatmp()) {
              // Rewrites to a SSATmp. Change the source.
              assertx(from != to);
              if (inst.src(i) != to) {
                ITRACE(2, "Rewriting {} -> {} in {}\n",
                       *inst.src(i), *to, inst.toString());
                inst.setSrc(i, to);
                changed = true;
              }
            } else {
              // Rewrites to a phi. Record the phi as used and record
              // the instruction so it can be rewritten later.
              auto const phi = it->second.block();
              assertx(phi);
              usedPhis.emplace(Phi{phi, from}, nullptr);
              phiUses.insert_or_assign(PhiUse{&inst, i}, Phi{phi, from});
              ITRACE(2, "Would rewrite {} to phi B{} in {}\n",
                     *inst.src(i), phi->id(), inst.toString());
            }
          }
        }

        if (inst.isPassthrough()) {
          // Record a rewrite from the canonical source of this
          // passthrough to its dest.
          auto const from = canonicalize(inst.src(0));
          // Ignore constants. They're perfect as they are.
          if (!from->inst()->is(DefConst)) {
            auto const to = inst.dst();
            ITRACE(
              2, "{} now maps to {} (via {})\n",
              *from, *to, inst.toString()
            );
            remaps.insert_or_assign(from, RemapTo{to});
          }
        } else if (inst.is(DefLabel)) {
          // Check if the DefLabel's sources all canonicalize the
          // same. If it does, treat this like a passthrough. If not,
          // treat it as a non-passthrough.
          for (uint32_t i = 0; i < inst.numDsts(); ++i) {
            auto const to = inst.dst(i);
            auto const from = canonicalize(to);
            if (from == to) {
              remaps.erase(to);
            } else if (!from->inst()->is(DefConst)) {
              ITRACE(
                2, "{} now maps to {} (via DefLabel {})\n",
                *from, *to, inst.toString()
              );
              remaps.insert_or_assign(from, RemapTo{to});
            }
          }
        } else {
          // Normal instruction. If this instruction produces a SSATmp
          // we have a rewrite to, erase that rewrite. This might seem
          // odd, but it can happen in a loop.
          for (uint32_t i = 0; i < inst.numDsts(); ++i) {
            remaps.erase(inst.dst(i));
          }
        }

        if (auto const next = inst.next()) {
          if (state.outNext != remaps) {
            ITRACE(4, "Scheduling next block B{}\n", next->id());
            worklist.push(blockState[next].order);
            state.outNext = std::move(remaps);
          }
        }
      }

      ONTRACE(
        4,
        ITRACE(4, "Remaps (next):\n");
        if (state.outNext) {
          for (auto const& remap : *state.outNext) {
            ITRACE(4, "  {} -> {}\n", *remap.first, remap.second.toString());
          }
        }
      );
      ONTRACE(
        4,
        ITRACE(4, "Remaps (taken):\n");
        if (state.outTaken) {
          for (auto const& remap : *state.outTaken) {
            ITRACE(4, "  {} -> {}\n", *remap.first, remap.second.toString());
          }
        }
      );
    } while (!worklist.empty());

    return changed;
  }

  struct Phi {
    Block* block;
    SSATmp* canon;
    bool operator==(const Phi& o) const {
      return block == o.block && canon == o.canon;
    }
  };

  // Create a DefLabel at the given block for the given canonical
  // SSATmp. This includes recursively materializing any phis this phi
  // might depend on.
  SSATmp* materializePhi(const Phi& phiToMaterialize) {
    {
      auto const it = usedPhis.find(phiToMaterialize);
      assertx(it != usedPhis.end());
      if (it->second) return it->second;
    }

    auto const block = phiToMaterialize.block;
    auto const canon = phiToMaterialize.canon;

    ITRACE(2, "Materializing phi at B{} for {}\n", block->id(), *canon);
    Trace::Indent indenter;

    jit::hash_map<Block*, SSATmp*> inputs;
    jit::fast_map<Block*, Block*> fixups;

    // Visit each pred to determine what the input for the DefLabel
    // should be
    block->forEachPred(
      [&] (Block* pred) {
        assertx(block == pred->next() || block == pred->taken());
        auto const& predState = block == pred->next()
          ? blockState[pred].outNext
          : blockState[pred].outTaken;
        assertx(predState);

        auto const it = predState->find(canon);
        if (it == predState->end()) {
          ITRACE(4, "Pred B{} has no mapping for {}\n", pred->id(), *canon);
          inputs.emplace(pred, canon);
        } else if (auto const tmp = it->second.ssatmp()) {
          assertx(tmp != canon);
          ITRACE(4, "Pred B{} maps {} to {}\n", pred->id(), *canon, *tmp);
          inputs.emplace(pred, tmp);
        } else if (auto const predPhi = it->second.block()) {
          // The input is another DefLabel. If the phi has a SSATmp,
          // then it's already resolved and we can treat it as
          // normal. If not, we don't have a SSATmp yet for that
          // phi. We use a Bottom constant as a placeholder
          // SSATmp. Afterwards we'll resolve the phi and then fixup
          // the input.
          auto const phiIt =
            usedPhis.emplace(Phi{predPhi, canon}, nullptr).first;
          if (phiIt->second) {
            ITRACE(
              4, "Pred B{} maps {} to resolved phi {}\n",
              pred->id(), *canon, *phiIt->second
            );
            inputs.emplace(pred, phiIt->second);
          } else {
            ITRACE(
              4, "Pred B{} maps {} to unresolved phi at B{}\n",
              pred->id(), *canon, predPhi->id()
            );
            inputs.emplace(pred, unit.cns(TBottom));
            fixups.emplace(pred, predPhi);
          }
        } else {
          always_assert(false);
        }
      }
    );

    // Create the DefLabel
    auto const phi = insertPhi(unit, block, inputs, !fixups.empty());
    ITRACE(
      4, "Phi at B{} for {} materialized as {}\n",
      block->id(), *canon, *phi
    );

    // Store the SSATmp we created for the DefLabel. Note: we do this
    // before resolving the placeholders below. This means if the
    // DefLabel has mutual recursion, we won't hit an infinite
    // loop. We'll just return this SSATmp immediately.
    auto const it = usedPhis.find(Phi{block, canon});
    assertx(it != usedPhis.end());
    assertx(!it->second);
    it->second = phi;

    if (!fixups.empty()) {
      // If we made use of any Bottom placeholders above, materialize
      // the phi for it, and then update the placeholder.
      ITRACE(
        4, "Resolving fixups for phi at B{} for {}\n",
        block->id(), *canon
      );
      assertx(phi->inst()->findDst(phi) == phi->inst()->numDsts() - 1);
      block->forEachPred(
        [&] (Block* pred) {
          auto& jmp = pred->back();
          assertx(jmp.is(Jmp));
          assertx(jmp.numSrcs() == phi->inst()->numDsts());
          auto const fixupIt = fixups.find(pred);
          if (fixupIt == fixups.end()) return;
          assertx(jmp.src(phi->inst()->numDsts() - 1)->isA(TBottom));
          auto const fixup = materializePhi(Phi{fixupIt->second, canon});
          ITRACE(4, "Fixup for phi B{} for {} resolves to {}\n",
                 fixupIt->second->id(), *canon, *fixup);
          jmp.setSrc(phi->inst()->numDsts() - 1, fixup);
        }
      );
    }

    // Set up the output type for the DefLabel
    retypeDests(phi->inst(), &unit);
    return phi;
  }

  /*
   * Canonicalizing DefLabels on the fly is tricky. Doing it naively
   * can lead to exponential path blowup, and caching schemes run into
   * problems when mutually recursive DefLabels are involved. Instead
   * of doing it on demand, pre-calculate all of the canonical values
   * for every DefLabel in the unit.
   *
   * This is essentially a dataflow problem, and it's solved in the
   * typical way. First we record all DefLabels, and record the
   * dependencies between DefLabels (a DefLabel depends on another if
   * the dst of one is the src of another).
   *
   * We then use a worklist algorithm. For every DefLabel on the list,
   * calculate the canonical value for it. This is is done by
   * examining the DefLabel sources and looking up their canonical
   * values. If the source's canonical value isn't a DefLabel, then
   * that's the source's final canonical value. Otherwise we lookup
   * that DefLabel's current canonical value. If all sources
   * canonicalize to the same value, that ends up being the canonical
   * value of the DefLabel. If not (they differ), the DefLabel
   * canonicalizes to itself. If this round of processing has changed
   * the canonical value, we then reschedule any other DefLabels which
   * depend on this value. The process repeats until the worklist
   * empties.
   *
   * During the above process, when we go to lookup a DefLabel's
   * canonical value, one might not be present. This can happen for
   * two reasons. The first is the DefLabel hasn't been processed at
   * all yet, or the DefLabel is entirely defined recursively. If we
   * encounter such a situation, we simply skip that source (in this
   * round). The dataflow ensures that we eventually will calculate a
   * value and revisit this DefLabel. This is the equivalent of a
   * Bottom.
   */

  // State for DefLabel canonicalization. The canonical value and the
  // other DefLabels which use this value.
  struct DefLabelCanon {
    SSATmp* canon{nullptr};
    jit::fast_set<SSATmp*> usedBy;
  };
  jit::fast_map<SSATmp*, DefLabelCanon> defLabelCanons;

  void calcDefLabelCanons() {
    jit::fast_set<SSATmp*> onWorklist;
    jit::deque<SSATmp*> worklist;

    // Iterate over every block, record all DefLabels and build the
    // "usedBy" dependencies between them. Initially put all on the
    // worklist.
    for (auto const block : rpoBlocks) {
      auto const& defLabel = block->front();
      if (!defLabel.is(DefLabel)) continue;

      auto const numDsts = defLabel.numDsts();
      for (uint32_t dstIdx = 0; dstIdx < numDsts; ++dstIdx) {
        auto const dst = defLabel.dst(dstIdx);
        block->forEachSrc(
          dstIdx,
          [&] (IRInstruction*, SSATmp* src) {
            // NB: "chase", not "canonical" here since we don't want
            // to consult the DefLabel state yet.
            src = chase(src);
            if (!src->inst()->is(DefLabel)) return;
            defLabelCanons[src].usedBy.emplace(dst);
          }
        );

        auto const DEBUG_ONLY inserted = onWorklist.emplace(dst).second;
        assertx(inserted);
        worklist.emplace_back(dst);
      }
    }

    // Now do the dataflow. For each DefLabel on the worklist,
    // calculate its canonical value from its sources. If it changes,
    // reschedule any DefLabels using this canonical value onto the
    // worklist.
    while (!worklist.empty()) {
      // Remove from worklist
      auto const defLabel = worklist.front();
      worklist.pop_front();
      onWorklist.erase(defLabel);

      auto const dstIdx = defLabel->inst()->findDst(defLabel);
      assertx(dstIdx != defLabel->inst()->numDsts());

      auto& state = defLabelCanons[defLabel];
      // Canonical value being the DefLabel itself is most pessimistic
      // state. The value cannot change at this point, so no point in
      // trying to recalculate it.
      if (state.canon == defLabel) continue;

      // Calculate the new canonical value.
      SSATmp* canon = nullptr;
      defLabel->inst()->block()->forEachSrc(
        dstIdx,
        [&] (IRInstruction*, SSATmp* src) {
          // If we reach the most pessimistic state, we can stop
          // looking at sources.
          if (canon == defLabel) return;

          // Chase back to defining instruction for this source and
          // find associated canonical value. We want "chase" here,
          // not "canonical" because they differ in how nullptr
          // state.canon values are handled.
          src = chase(src);
          auto const srcCanon = [&] {
            if (!src->inst()->is(DefLabel)) return src;
            auto const it = defLabelCanons.find(src);
            assertx(it != defLabelCanons.end());
            return it->second.canon;
          }();
          // A nullptr means we don't yet have a canonical value for
          // the DefLabel associated with src. Ignore this src for
          // now.
          if (!srcCanon) return;

          // If we haven't gotten a value yet, take the first one we
          // get.
          if (!canon) {
            canon = srcCanon;
            return;
          }

          // Otherwise check if this value matches the one found in
          // previous loop iteration. If not, pessimize (which causes
          // us to short-circuit out of this loop).
          if (srcCanon != canon) {
            canon = defLabel;
            return;
          }
        }
      );

      // The newly calculated canonical value is different. Update the
      // state and reschedule any dependent DefLabels.
      if (state.canon != canon) {
        state.canon = canon;
        for (auto const used : state.usedBy) {
          if (onWorklist.emplace(used).second) {
            worklist.emplace_back(used);
          }
        }
      }
    }

    ONTRACE(
      4,
      ITRACE(4, "DefLabel canonical values:\n");
      Trace::Indent indenter;
      for (auto const& kv : defLabelCanons) {
        ITRACE(
          4,
          "{} -> {} [{}]\n",
          *kv.first,
          [&] () -> std::string {
            if (!kv.second.canon) return "*";
            return kv.second.canon->toString();
          }(),
          [&] {
            std::string str;
            auto first = true;
            for (auto const u : kv.second.usedBy) {
              fmt::format_to(std::back_inserter(str), "{}{}", first ? "" : ",", *u);
              first = false;
            }
            return str;
          }()
        );
      }
    );
  }

  // "Chase" a SSATmp's definition back to the first non passthrough
  // value. We don't use canonical for this, because it includes
  // ConvPtrToLval (which isn't a passthrough for our purposes).
  static SSATmp* chase(SSATmp* v) {
    while (v->inst()->isPassthrough()) v = v->inst()->getPassthroughValue();
    return v;
  }

  // "Canonicalize" a SSATmp by chasing its definition back to the
  // first non passthrough value. If this hits a DefLabel, consult the
  // already calculated canonical value for it.
  SSATmp* canonicalize(SSATmp* v) {
    v = chase(v);

    // Not a DefLabel. This is canonical.
    if (!v->inst()->is(DefLabel)) return v;

    // DefLabel. Look up the state to get its canonical value. We
    // should have state for every possible DefLabel encountered here.
    auto const it = defLabelCanons.find(v);
    assertx(it != defLabelCanons.end());

    if (!it->second.canon) return v;
    return it->second.canon;
  }

  struct RemapTo {
    explicit RemapTo(SSATmp* t) : to{t} {}
    explicit RemapTo(Block* b) : to{b} {}

    SSATmp* ssatmp() const { return to.left(); }
    Block* block() const { return to.right(); }

    void merge(const RemapTo& o, Block* current) {
      assertx(!to.isNull());
      assertx(!o.to.isNull());
      if (to != o.to) to = current;
    }

    std::string toString() const {
      return to.match(
        [] (SSATmp* s) { return s->toString(); },
        [] (Block* b) { return fmt::format("Phi B{}", b->id()); }
      );
    }

    bool operator==(const RemapTo& o) const { return to == o.to; }
    bool operator!=(const RemapTo& o) const { return to != o.to; }

    Either<SSATmp*, Block*> to;
  };

  IRUnit& unit;
  BlockList rpoBlocks;

  using Remaps = jit::fast_map<SSATmp*, RemapTo>;
  struct State {
    size_t order;
    Optional<Remaps> outNext;
    Optional<Remaps> outTaken;
  };
  StateVector<Block, State> blockState{unit, State{}};

  struct PhiUse {
    IRInstruction* inst;
    uint32_t srcIdx;
    bool operator==(const PhiUse& o) const {
      return inst == o.inst && srcIdx == o.srcIdx;
    }
  };

  struct Hasher {
    size_t operator()(const Phi& p) const {
      return folly::hash::hash_combine(
        pointer_hash<Block>{}(p.block),
        pointer_hash<SSATmp>{}(p.canon)
      );
    }
    size_t operator()(const PhiUse& p) const {
      return folly::hash::hash_combine(
        pointer_hash<IRInstruction>{}(p.inst),
        p.srcIdx
      );
    }
  };

  jit::fast_map<Phi, SSATmp*, Hasher> usedPhis;
  jit::fast_map<PhiUse, Phi, Hasher> phiUses;
};

//////////////////////////////////////////////////////////////////////

}

void cloneToBlock(const BlockList& /*rpoBlocks*/, IRUnit& unit,
                  Block::iterator const first, Block::iterator const last,
                  Block* const target) {
  StateVector<SSATmp,SSATmp*> rewriteMap(unit, nullptr);

  auto rewriteSources = [&] (IRInstruction* inst) {
    for (int i = 0; i < inst->numSrcs(); ++i) {
      if (auto newTmp = rewriteMap[inst->src(i)]) {
        FTRACE(5, "  rewrite: {} -> {}\n",
               inst->src(i)->toString(),
               newTmp->toString());
        inst->setSrc(i, newTmp);
      }
    }
  };

  auto targetIt = target->skipHeader();
  for (auto it = first; it != last; ++it) {
    assertx(!it->isControlFlow());

    FTRACE(5, "cloneToBlock({}): {}\n", target->id(), it->toString());
    auto const newInst = unit.clone(&*it);

    if (auto const numDests = newInst->numDsts()) {
      for (int i = 0; i < numDests; ++i) {
        FTRACE(5, "  add rewrite: {} -> {}\n",
               it->dst(i)->toString(),
               newInst->dst(i)->toString());
        rewriteMap[it->dst(i)] = newInst->dst(i);
      }
    }

    target->insert(targetIt, newInst);
    targetIt = ++target->iteratorTo(newInst);
  }

  postorderWalk(
    unit,
    [&](Block* block) {
      FTRACE(5, "cloneToBlock: rewriting block {}\n", block->id());
      for (auto& inst : *block) {
        FTRACE(5, " rewriting {}\n", inst.toString());
        rewriteSources(&inst);
      }
    },
    target
  );
}

void moveToBlock(Block::iterator const first,
                 Block::iterator const last,
                 Block* const target) {
  if (first == last) return;

  auto const srcBlock = first->block();

  auto targetIt = target->skipHeader();
  for (auto it = first; it != last;) {
    auto const inst = &*it;
    assertx(!inst->isControlFlow());

    FTRACE(5, "moveToBlock({}): {}\n",
           target->id(),
           inst->toString());

    it = srcBlock->erase(it);
    target->insert(targetIt, inst);
    targetIt = ++target->iteratorTo(inst);
  }
}

bool retypeDests(IRInstruction* inst, const IRUnit* /*unit*/) {
  auto changed = false;
  for (auto i = uint32_t{0}; i < inst->numDsts(); ++i) {
    DEBUG_ONLY auto const oldType = inst->dst(i)->type();
    if (retypeDst(inst, i)) {
      changed = true;
      ITRACE(5, "reflowTypes: retyped {} in {}\n", oldType.toString(),
             inst->toString());
    }
  }
  return changed;
}

/*
 * Algorithm for reflow:
 *   1) Set DefLabel dsts to TBottom.  We will widen their types from there.
 *   2) Flow types forward through the unit using retypeDests. Propagate types
 *   through Jmps unioning them into the target DefLabel's dst types.  If the
 *   target was earlier in RPO we have to schedule another loop to start at or
 *   earlier than that target block.
 */
bool reflowTypes(IRUnit& unit) {
  using RPOId = uint32_t;
  auto const rpoBlocks = rpoSortCfg(unit);
  auto const ids = numberBlocks(unit, rpoBlocks);
  auto const endRPOId = safe_cast<RPOId>(rpoBlocks.size());

  for (auto const block : rpoBlocks) {
    auto const inst = &block->front();
    if (inst->is(DefLabel)) {
      for (auto const dst : inst->dsts()) {
        dst->setType(TBottom);
      }
    }
  }

  RPOId firstUnstable = 0;
  RPOId firstBottom = endRPOId;
  while (firstUnstable < endRPOId) {
    auto nextFirstUnstable = endRPOId;
    if (firstBottom >= firstUnstable) firstBottom = endRPOId;
    FTRACE(5, "reflowTypes: starting iteration at {}/{})\n",
           firstUnstable, endRPOId);
    for (auto id = firstUnstable; id < endRPOId; ++id) {
      auto const block = rpoBlocks[id];
      FTRACE(5, "reflowTypes: visiting block {} "
                "(rpo: {}, firstUnstable: {}, firstBottom: {})\n",
             block->id(), ids[block], nextFirstUnstable, firstBottom);

      for (auto& inst : *block) {
        if (!inst.is(DefLabel)) retypeDests(&inst, &unit);

        for (auto const src : inst.srcs()) {
          if (src->type() == TBottom) {
            FTRACE(5, "reflowTypes: found bottom src {} (at rpo: {})\n",
                   src->toString(), id);
            firstBottom = std::min(firstBottom, id);
          }
        }
        // If this instruction reachable, but its next block unreachable it is
        // okay for it to def a Bottom tmp.  It would be better for such
        // instructions to have a better simplification option to an always
        // taken variant, but not necessarily all throwing operations have such
        // an alternate representation.
        auto const unreachableNext = inst.next() && inst.taken()
                                     && inst.next()->isUnreachable()
                                     && !inst.taken()->isUnreachable();
        if (unreachableNext) continue;

        for (auto const dst : inst.dsts()) {
          if (dst->type() == TBottom) {
            FTRACE(5, "reflowTypes: found bottom dst {} (at rpo: {})\n",
                   dst->toString(), id);
            firstBottom = std::min(firstBottom, id);
          }
        }
      }

      auto const jmp = &block->back();
      auto const n = jmp->numSrcs();
      if (jmp->is(Jmp) && n > 0) {
        // If we pass a wider type to a label, we need to update its dst type
        // and then restart the next iteration at that block (or earlier).
        auto srcs = jmp->srcs();
        auto const taken = jmp->taken();
        auto dsts = taken->front().dsts();
        for (auto i = 0; i < n; ++i) {
          auto const type = srcs[i]->type() | dsts[i]->type();
          if (type == dsts[i]->type()) continue;
          FTRACE(5, "reflowTypes: widening phi {} to {} (at rpo: {})\n",
                 dsts[i]->toString(), type.toString(), ids[taken]);
          auto const takenId = ids[taken];
          if (takenId <= id) {
            nextFirstUnstable = std::min(nextFirstUnstable, takenId);
          }
          dsts[i]->setType(type);
        }
      }
    }
    firstUnstable = nextFirstUnstable;
  }
  return firstBottom != endRPOId;
}

void insertNegativeAssertTypes(IRUnit& unit, const BlockList& blocks) {
  for (auto& blk : blocks) {
    auto const& inst = blk->back();
    if (!inst.is(CheckType)) continue;

    // Note that we can't assert the type if this isn't the only predecessor,
    // but also, it's currently not the case that it would ever be useful to do
    // so.  If a taken edge coming out of a CheckType is critical, even if we
    // split it so we could insert the AssertType, there would be no use of the
    // tmp in the edge-splitting-block to rewrite to the dst of the AssertType.
    if (inst.taken()->numPreds() != 1) continue;

    auto const checkTy = inst.typeParam();
    auto const srcTy   = inst.src(0)->type();
    auto const takenTy = negativeCheckType(srcTy, checkTy);
    if (takenTy < srcTy) {
      inst.taken()->prepend(
        unit.gen(AssertType, inst.bcctx(), takenTy, inst.src(0))
      );
    }
  }
}

void refineTmps(IRUnit& unit) {
  TRACE_SET_MOD(hhir_refineTmps)
  PassTracer tracer{&unit, Trace::hhir_refineTmps, "refineTmps"};
  Timer timer{Timer::optimize_refineTmps, unit.logEntry().get_pointer()};

  RefineTmps refiner{unit};
  auto const changed = refiner();
  if (changed) reflowTypes(unit);
  assertx(checkEverything(unit));
}

SSATmp* insertPhi(IRUnit& unit, Block* blk,
                  const jit::hash_map<Block*, SSATmp*>& inputs,
                  bool forceNew) {
  assertx(blk->numPreds() > 1);
  auto label = &blk->front();
  if (!label->is(DefLabel)) {
    label = unit.defLabel(1, blk, label->bcctx());
  } else if (forceNew) {
    unit.expandLabel(label, 1);
  } else {
    for (auto d = label->numDsts(); d--; ) {
      auto result = label->dst(d);
      blk->forEachPred([&](Block* pred) {
          if (result) {
            auto& jmp = pred->back();
            auto it = inputs.find(pred);
            assertx(it != inputs.end());
            if (jmp.src(d) != it->second) {
              result = nullptr;
            }
          }
        });
      if (result) return result;
    }
    unit.expandLabel(label, 1);
  }

  blk->forEachPred([&](Block* pred) {
      auto it = inputs.find(pred);
      assertx(it != inputs.end());
      unit.expandJmp(&pred->back(), it->second);
    });
  retypeDests(label, &unit);
  return label->dst(label->numDsts() - 1);
}

SSATmp* deletePhiDest(IRInstruction* label, unsigned i) {
  assertx(label->is(DefLabel));
  auto dest = label->dst(i);
  label->block()->forEachSrc(
    i, [&](IRInstruction* jmp, SSATmp* /*src*/) { jmp->deleteSrc(i); });
  label->deleteDst(i);
  return dest;
}

//////////////////////////////////////////////////////////////////////

}
