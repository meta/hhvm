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

#include "hphp/runtime/vm/jit/smashable-instr-arm.h"

#include "hphp/runtime/vm/jit/abi-arm.h"
#include "hphp/runtime/vm/jit/alignment.h"
#include "hphp/runtime/vm/jit/align-arm.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/vasm-util-arm.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/data-block.h"

#include "hphp/vixl/hphp-compat.h"

namespace HPHP::jit::arm {

namespace {

/*
 * Word-level decoding of an inline site's load.
 *
 * Each predicate takes a copy of the instruction rather than its address.  A
 * concurrent retirement NOPs the LDR and then the ADRP, so two reads of the
 * same site can straddle that and yield a decode describing neither state --
 * in particular a literal address belonging to neither.
 */
uint32_t readInstWord(TCA addr) {
  assert_flog(addr != nullptr, "readInstWord: null address\n");
  assert_flog(
    (reinterpret_cast<uintptr_t>(addr) & 3) == 0,
    "readInstWord: {} is not 4-byte aligned\n", addr);
  return __atomic_load_n(reinterpret_cast<const uint32_t*>(addr),
                         __ATOMIC_RELAXED);
}

/*
 * One instruction, read once.  The decoded view points into this object, so
 * the copy and the pointer share a lifetime; a free function handing back a
 * pointer into its argument reads like a cast but dangles the moment it is
 * passed a temporary.
 */
struct Word {
  explicit Word(TCA addr) : raw{readInstWord(addr)} {}

  const vixl::Instruction* operator->() const {
    return vixl::Instruction::CastConst(
      reinterpret_cast<const uint8_t*>(&raw));
  }

  uint32_t raw;
};

bool isNopWord(const Word& w) {
  using namespace vixl;
  return w->Mask(SystemHintFMask) == SystemHintFixed && w->ImmHint() == NOP;
}

bool isNearLdrWord(const Word& w) {
  using namespace vixl;
  return w->Mask(LoadLiteralMask) == LDR_w_lit;
}

bool isUnsignedLdrWord(const Word& w) {
  using namespace vixl;
  return w->Mask(LoadStoreUnsignedOffsetMask) == LDR_w_unsigned;
}

/*
 * The inline site named by `inst`, read once per word.
 *
 * `inst` is the site start (LDR near, ADRP far) or, for a far site reached
 * from a return address, its LDR.  Nothing here is read twice: a concurrent
 * retirement NOPs the LDR and then the ADRP, so a second read of the same
 * word can see a different site, and a decode mixing the two describes
 * neither -- in particular it yields a literal address belonging to neither.
 * Every address below is arithmetic on `inst`, never a load through it.
 *
 * Not a parse means either a NOP -- another thread part-way through retiring,
 * which is a legal state -- or a shape we do not emit.  Callers that know the
 * address must name a site report the difference; see
 * assertAlreadyCollapsedInline().
 */
struct InlineLoad {
  bool ok{false};
  bool far{false};
  TCA  start{nullptr};    // ADRP for far, LDR for near
  TCA  literal{nullptr};
  TCA  branch{nullptr};   // the B{L}R the load feeds

  explicit operator bool() const { return ok; }
};

/*
 * What smashableCallFromRet() relies on: the LDR is always the word before the
 * branch, so subtracting the site length from a return address lands on it for
 * both forms -- a far site's ADRP is a prefix and does not change the distance.
 * The backward parse in readInlineLoad() only works while this holds.
 */
static_assert(smashableCallLenInline() == 2 * vixl::kInstructionSize);
static_assert(smashableJmpLenInline() == 2 * vixl::kInstructionSize);

InlineLoad readInlineLoad(TCA inst, uint32_t branchMask, uint32_t branchOp) {
  using namespace vixl;
  InlineLoad r;

  auto const w1 = Word{inst};
  if (isNopWord(w1)) return r;

  // The LDR, wherever it turned out to be; its Rt has to be what the branch
  // indirects through.
  uint32_t ldrReg = 0;

  if (isNearLdrWord(w1)) {
    r.far = false;
    r.start = inst;
    ldrReg = w1->GetRt();
    r.literal = inst + w1->GetImmLLiteral() * kInstructionSize;
  } else if (w1->IsAdrp()) {
    auto const w2 = Word{inst + kInstructionSize};
    if (!isUnsignedLdrWord(w2)) return r;
    if (w1->GetRd() != w2->GetRn() || w1->GetRd() != w2->GetRt()) return r;
    r.far = true;
    r.start = inst;
    ldrReg = w2->GetRt();
    auto const page = reinterpret_cast<uintptr_t>(
      w1->GetImmPCOffsetTarget(Instruction::CastConst(inst)));
    r.literal = reinterpret_cast<TCA>(
      page + (w2->GetImmLSUnsigned() << w2->GetSizeLS()));
  } else if (isUnsignedLdrWord(w1)) {
    // A far site named by its LDR, which is what subtracting a fixed length
    // from a return address gives -- see smashableCallFromRet().  The ADRP is
    // identified by the register the LDR indexes, not by assuming a length.
    auto const adrpAddr = inst - kInstructionSize;
    auto const w0 = Word{adrpAddr};
    if (!w0->IsAdrp()) return r;
    if (w0->GetRd() != w1->GetRn() || w0->GetRd() != w1->GetRt()) return r;
    r.far = true;
    r.start = adrpAddr;
    ldrReg = w1->GetRt();
    auto const page = reinterpret_cast<uintptr_t>(
      w0->GetImmPCOffsetTarget(Instruction::CastConst(adrpAddr)));
    r.literal = reinterpret_cast<TCA>(
      page + (w1->GetImmLSUnsigned() << w1->GetSizeLS()));
  } else {
    return r;
  }

  r.branch = r.start + (r.far ? 2 : 1) * kInstructionSize;
  auto const wb = Word{r.branch};
  if (wb->Mask(branchMask) != branchOp) return r;
  if (wb->Rn() != ldrReg) return r;

  r.ok = true;
  return r;
}

void assertInlineLiteralOutsideSite(const InlineLoad& load,
                                    const char* what) {
  assert_flog(
    load.literal + sizeof(uint32_t) <= load.start ||
      load.literal >= load.branch + vixl::kInstructionSize,
    "{}: site {} has its literal at {}, inside the site itself\n",
    what, load.start, load.literal
  );
}

}

///////////////////////////////////////////////////////////////////////////////

static bool useSmashedCallInline() { return Cfg::Jit::ArmInlineSmashableCall; }
static bool useSmashedJmpInline() { return Cfg::Jit::ArmInlineSmashableJmp; }

/*
 * Literal-only smashing: patch the pooled literal and stop there, leaving the
 * indirect B{L}R (or the B{L} to the veneer) in place.
 *
 * Collapsing a site to a direct branch is one-way -- the literal stops being
 * what the branch reads, and for the inline form retirement then NOPs the load
 * that named it -- so a later re-smash of a still-tracked site has nothing to
 * write and is dropped.  Staying indirect keeps every site re-smashable for as
 * long as it exists, at the cost of the direct branch.
 */
static bool smashLiteralOnly() { return Cfg::Jit::ArmSmashLiteralOnly; }

size_t smashableCallLenActual() {
  return useSmashedCallInline() ? smashableCallLenInline()
                                : smashableCallLenVeneer();
}

TCA smashableCallStart(TCA inst) {
  if (!useSmashedCallInline()) return inst;
  auto const load = readInlineLoad(
    inst, vixl::UnconditionalBranchToRegisterMask, vixl::BLR);
  return load ? load.start : nullptr;
}

size_t smashableJmpLenActual() {
  return useSmashedJmpInline() ? smashableJmpLenInline()
                               : smashableJmpLenVeneer();
}

///////////////////////////////////////////////////////////////////////////////

/*
 * For smashable jmps and calls in ARM, we emit the target address straight
 * into the instruction stream, and then do a pc-relative load to read it.
 *
 * This neatly sidesteps the problem of concurrent modification and execution,
 * as well as the problem of 19- and 26-bit jump offsets (not big enough).  It
 * does, however, entail an indirect jump.
 */

void emitPooledLiteralLoad(vixl::MacroAssembler& a,
                           CodeBlock& cb,
                           CGMeta& meta,
                           uint64_t val,
                           vixl::Register dst,
                           uint8_t width,
                           bool smashable,
                           bool emitFarLiteral) {
  auto const loadStart = cb.frontier();

  if (emitFarLiteral) {
    auto const base = vixl::Register(dst.GetCode(), 64);
    a.adrp(base, int64_t{0});
    a.Ldr(dst, vixl::MemOperand(base, 0));
  } else {
    vixl::Label data;
    a.bind(&data);
    a.Ldr(dst, &data);
  }

  poolLiteral(
    cb,
    meta,
    val,
    width,
    smashable,
    loadStart,
    emitFarLiteral
  );
}

TCA emitSmashableMovq(CodeBlock& cb, CGMeta& meta, uint64_t imm,
                      PhysReg d, bool emitFarLiteral) {
  align(cb, &meta, Alignment::SmashMovq, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const sync_start = cb.frontier();
  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);

  emitPooledLiteralLoad(a, cb, meta, imm, x2a(d), 64, true, emitFarLiteral);

  cb.sync(sync_start);
  return the_start;
}

///////////////////////////////////////////////////////////////////////////////
// emitSmashableCall

TCA emitSmashableCallInline(CodeBlock& cb, CGMeta& meta, TCA target,
                            bool emitFarLiteral) {
  align(cb, &meta, Alignment::SmashCall, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);
  // The literal holds an address, so relocation has to adjust it.  Only the
  // inline form pools its own literal, so only it records that here.
  meta.addressImmediates.insert(the_start);

  assertx((makeTarget32(target) & 3) == 0);
  // Inline: (ADRP+)LDR + BLR.  The 32-bit target lives in the literal pool and
  // is reached either by a near LDR-literal or, when far literals are enabled
  // (e.g. before relocation), by an ADRP+LDR pair so the load can be relocated.
  // Smashing the target is always a single store to the pooled literal.
  emitPooledLiteralLoad(a, cb, meta, (uint64_t)makeTarget32(target),
                        rAsm_w, 32, true /* smashable */, emitFarLiteral);
  a.Blr(rAsm);

  cb.sync(the_start);
  return the_start;
}

// No emitFarLiteral parameter: the literal lives in the veneer, which Vgen
// emits later and which picks near vs far itself.
TCA emitSmashableCallVeneer(CodeBlock& cb, CGMeta& meta, TCA target) {
  align(cb, &meta, Alignment::SmashCall, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);

  assertx((makeTarget32(target) & 3) == 0);
  addSmashableVeneer(meta, the_start, target);
  vixl::Label veneer_addr;
  a.bind(&veneer_addr);
  a.bl(&veneer_addr);

  cb.sync(the_start);
  return the_start;
}

TCA emitSmashableCall(CodeBlock& cb, CGMeta& meta, TCA target,
                      bool emitFarLiteral) {
  return useSmashedCallInline()
    ? emitSmashableCallInline(cb, meta, target, emitFarLiteral)
    : emitSmashableCallVeneer(cb, meta, target);
}

///////////////////////////////////////////////////////////////////////////////
// emitSmashableJmp

TCA emitSmashableJmpInline(CodeBlock& cb, CGMeta& meta, TCA target,
                           bool emitFarLiteral) {
  align(cb, &meta, Alignment::SmashJmp, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);
  // See emitSmashableCallInline(): the pooled literal is an address and only
  // this marks it for relocation.
  meta.addressImmediates.insert(the_start);

  assertx((makeTarget32(target) & 3) == 0);
  // Inline: (ADRP+)LDR + BR.  See emitSmashableCallInline() for the near/far
  // literal-load rationale.
  emitPooledLiteralLoad(a, cb, meta, (uint64_t)makeTarget32(target),
                        rAsm_w, 32, true /* smashable */, emitFarLiteral);
  a.Br(rAsm);

  cb.sync(the_start);
  return the_start;
}

TCA emitSmashableJmpVeneer(CodeBlock& cb, CGMeta& meta, TCA target) {
  align(cb, &meta, Alignment::SmashJmp, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);

  assertx((makeTarget32(target) & 3) == 0);
  addSmashableVeneer(meta, the_start, target);
  vixl::Label veneer_addr;
  a.bind(&veneer_addr);
  a.b(&veneer_addr);

  cb.sync(the_start);
  return the_start;
}

TCA emitSmashableJmp(CodeBlock& cb, CGMeta& meta, TCA target,
                     bool emitFarLiteral) {
  return useSmashedJmpInline()
    ? emitSmashableJmpInline(cb, meta, target, emitFarLiteral)
    : emitSmashableJmpVeneer(cb, meta, target);
}

///////////////////////////////////////////////////////////////////////////////
// emitSmashableJcc

TCA emitSmashableJcc(CodeBlock& cb, CGMeta& meta, TCA target,
                     ConditionCode cc) {
  align(cb, &meta, Alignment::SmashJcc, AlignContext::Live);

  vixl::MacroAssembler a { cb };

  auto const the_start = cb.frontier();
  meta.smashableLocations.insert(the_start);

  assertx((makeTarget32(target) & 3) == 0);
  addSmashableVeneer(meta, the_start, target);
  vixl::Label veneer_addr;
  a.bind(&veneer_addr);
  a.b(&veneer_addr, arm::convertCC(cc));

  cb.sync(the_start);
  return the_start;
}

///////////////////////////////////////////////////////////////////////////////

namespace {
// The vasm instruction after a smashable may have had nops inserted before it.
// In that case the branch jumping over its immediate will point after the nop
// sequence rather than immediately after the smashable.  This helper is used
// to check the branch is pointing to an appropriate address.
//
// Returns true if target points to inst or a chain of Nops leading to inst.
// Otherwise it returns false.
bool targetsInst(const vixl::Instruction* target,
                 const vixl::Instruction* inst) {
  while (target != inst && inst->IsNop()) {
    inst = inst->GetNextInstruction();
  }
  return target == inst;
}

arm::LoadLiteral smashableMovqLoad(const vixl::Instruction* start) {
  auto const load =
    arm::LoadLiteral::at(const_cast<vixl::Instruction*>(start));
  if (!load || load.width() != 64) return {};
  return load;
}

/*
 * A literal load we are able to patch: parsed, and holding a 32-bit target.
 * Everything that rebinds a site writes the literal with a single aligned
 * 32-bit store, so a load of any other width is not ours to touch.
 */
bool isPatchableLiteralLoad(const arm::LoadLiteral& load) {
  return load && load.width() == 32;
}

arm::LoadLiteral smashableVeneerLoad(const vixl::Instruction* start) {
  using namespace vixl;

  auto const load =
    arm::LoadLiteral::at(const_cast<vixl::Instruction*>(start));
  if (!isPatchableLiteralLoad(load)) return {};

  auto const br = load.ldr()->GetNextInstruction();
  if (br->Mask(UnconditionalBranchToRegisterMask) != BR ||
      br->Rn() != load.destReg()) {
    return {};
  }

  return load;
}

}

bool possiblySmashableMovq(TCA inst) {
  using namespace vixl;
  auto const start = Instruction::CastConst(inst);
  return bool(smashableMovqLoad(start));
}

bool isSmashableVeneer(const vixl::Instruction* start) {
  return bool(smashableVeneerLoad(start));
}

namespace {

bool canOptimizeSmashedBranch(TCA target) {
  // Direct branches cannot be resmashed or guarded. Stubs are temporary, and
  // a smashable veneer may itself be retargeted concurrently.
  return Cfg::Repo::Authoritative &&
         !svcreq::isStub(target) &&
         !isSmashableVeneer(vixl::Instruction::CastConst(target));
}

}

///////////////////////////////////////////////////////////////////////////////
// possiblySmashable* — Inline variants

// The inline load is either a near LDR-literal (1 instr) or an ADRP+LDR far
// pair (2 instrs), followed by the indirect branch (BLR for call, BR for jmp).
// Returns the parsed load when `inst` starts a 32-bit inline load whose branch
// (at inst + instrCount instructions) matches `branchMask`/`branchOp` and reads
// the loaded register; otherwise returns an invalid LoadLiteral.
namespace {
}

bool possiblySmashableCallInline(TCA inst) {
  using namespace vixl;
  return bool(readInlineLoad(inst, UnconditionalBranchToRegisterMask, BLR));
}

bool possiblySmashableJmpInline(TCA inst) {
  using namespace vixl;
  return bool(readInlineLoad(inst, UnconditionalBranchToRegisterMask, BR));
}

///////////////////////////////////////////////////////////////////////////////
// possiblySmashable* — Veneer variants (exact original code)

bool possiblySmashableCallVeneer(TCA inst) {
  using namespace vixl;

  auto const bl = Instruction::Cast(inst);

  return (bl->Mask(UnconditionalBranchMask) == BL &&
          isSmashableVeneer(bl->ImmPCOffsetTarget()));
}

bool possiblySmashableJmpVeneer(TCA inst) {
  using namespace vixl;

  auto const b = Instruction::Cast(inst);

  return (b->Mask(UnconditionalBranchMask) == B &&
          isSmashableVeneer(b->ImmPCOffsetTarget()));
}

bool possiblySmashableJcc(TCA inst) {
  using namespace vixl;

  auto const b = Instruction::Cast(inst);

  return b->IsCondBranchImm() && isSmashableVeneer(b->ImmPCOffsetTarget());
}

///////////////////////////////////////////////////////////////////////////////
// possiblySmashable* — Dispatchers

bool possiblySmashableCall(TCA inst) {
  // One form is live per process, so ask about that one.  Accepting either
  // would report a site as smashable that the configured path cannot smash.
  return useSmashedCallInline() ? possiblySmashableCallInline(inst)
                                : possiblySmashableCallVeneer(inst);
}

bool possiblySmashableJmp(TCA inst) {
  // See possiblySmashableCall().
  return useSmashedJmpInline() ? possiblySmashableJmpInline(inst)
                               : possiblySmashableJmpVeneer(inst);
}

///////////////////////////////////////////////////////////////////////////////
// smash* — Inline variants

void smashMovq(TCA inst, uint64_t target) {
  using namespace vixl;
  assert_flog(possiblySmashableMovq(inst),
    "smashMovq: {} is not a smashable movq site\n", inst);

  auto const load = smashableMovqLoad(Instruction::Cast(inst));
  assert_flog(load, "smashMovq: no literal load at {}\n", inst);
  patchTarget64(
    reinterpret_cast<TCA>(load.literalAddress()),
    reinterpret_cast<TCA>(target)
  );
}

void smashCmpq(TCA /*inst*/, uint32_t /*target*/) {
  not_implemented();
}

///////////////////////////////////////////////////////////////////////////////
// Smash-site validation.
//
// Every smash stores into live code.  Before each one, check the destination
// is a patchable TC word and holds the instruction we parsed -- getting either
// wrong surfaces much later as an illegal instruction somewhere unrelated.

namespace {

void checkSmashDestination(TCA site, TCA addr, const char* what) {
  assert_flog(
    (reinterpret_cast<uintptr_t>(addr) & 3) == 0,
    "smash {}: destination {} is not 4-byte aligned\n", what, addr);
  // Key off the site, not the destination: unit tests smash their own buffers,
  // where a wild destination would look just like a test buffer.
  if (!tc::isValidCodeAddress(site)) return;
  assert_flog(
    tc::isValidCodeAddress(addr),
    "smash {}: site {} is TC code but destination {} is not\n",
    what, site, addr);
}

// Replace one instruction, having first confirmed the word being replaced is
// the one we parsed.
void smashInstExpecting(TCA site, TCA addr, uint32_t mask, uint32_t op,
                        uint32_t newInst, const char* what) {
  checkSmashDestination(site, addr, what);
  DEBUG_ONLY auto const cur = vixl::Instruction::Cast(addr);
  // The word already holding what we are about to write is not a failure.
  // Two threads can bind the same site at once -- tc-bind.cpp takes no lock
  // when the callee prologue is not profiled -- and they compute the same
  // instruction, so the loser of the race arrives to find its own rewrite
  // already done.  That was always benign; only the expectation is new.
  // Anything else means we are looking at the wrong site.
  assert_flog(
    cur->Mask(mask) == op || cur->InstructionBits() == newInst,
    "smash {}: expected to replace a {:#010x}-shaped instruction at {}, "
    "found {:#010x}\n",
    what, op, addr, cur->InstructionBits());
  smashInst(addr, newInst);
}

}

namespace {

bool isDirectBranch(TCA addr) {
  using namespace vixl;
  auto const op = Instruction::Cast(addr)->Mask(UnconditionalBranchMask);
  return op == B || op == BL;
}

bool isNop(TCA addr) {
  return isNopWord(Word{addr});
}

}

namespace {

/*
 * The one legitimate reason a smash finds no live site: it was already
 * collapsed, which only happens under repo authoritative mode.  A silent
 * no-op here is indistinguishable from a successful bind.
 */

void assertCollapsed(bool collapsed, TCA inst, const char* what) {
  assert_flog(
    Cfg::Repo::Authoritative,
    "smash {}: no live site at {}, and sites are only collapsed under repo "
    "authoritative mode\n", what, inst);
  assert_flog(
    collapsed,
    "smash {}: {} is neither a live site nor an already-collapsed one\n",
    what, inst);
}

/*
 * Inline: the branch is last -- inst + 4 when the site is named by its load,
 * inst + 8 when a far site is named by its ADRP.  Retirement may NOP the
 * prefix, so only the branch tells us anything.
 */
void assertAlreadyCollapsedInline(TCA inst, const char* what) {
  using namespace vixl;
  assertCollapsed(isDirectBranch(inst + kInstructionSize) ||
                  isDirectBranch(inst + 2 * kInstructionSize),
                  inst, what);
}

/* Veneer: the site is the single branch, retargeted in place. */
void assertAlreadyCollapsedVeneer(TCA inst, const char* what) {
  assertCollapsed(isDirectBranch(inst), inst, what);
}

/*
 * As above, but a jmp whose target is the next instruction collapses to a NOP
 * rather than a branch to it -- see smashJmpVeneer(), which picks the
 * cheaper encoding.  A call has no such form, so it keeps the stricter check.
 */
void assertAlreadyCollapsedJmpVeneer(TCA inst, const char* what) {
  assertCollapsed(isDirectBranch(inst) || isNop(inst), inst, what);
}

}

namespace {

/*
 * After a rebind the literal must hold what we wrote, and any collapsed branch
 * must agree with it.  A site where they disagree is what a misdirected smash
 * leaves behind, and nothing notices until it executes.
 */
void verifyLiteralWrite(TCA inst, TCA literal, TCA target,
                        const char* what) {
  DEBUG_ONLY auto const got = *reinterpret_cast<uint32_t*>(literal);
  assert_flog(
    got == makeTarget32(target),
    "smash {}: literal for site {} holds {:#x} after writing {:#x}\n",
    what, inst, got, makeTarget32(target));
}

void verifyCollapsedBranch(TCA inst, TCA branchAddr, TCA target,
                           const char* what) {
  DEBUG_ONLY auto const branch = vixl::Instruction::Cast(branchAddr);
  assert_flog(
    branch->ImmPCOffsetTarget() == vixl::Instruction::Cast(target),
    "smash {}: site {} collapsed to a branch to {}, expected {}\n",
    what, inst, reinterpret_cast<const void*>(branch->ImmPCOffsetTarget()), target);
}


}

void smashCallInline(TCA inst, TCA target) {
  using namespace vixl;
  // Snapshot the load before claiming the site; same-target contenders lose.
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BLR);
  if (!load) {
    // Either a NOP, meaning another thread is part-way through retiring this
    // site, or a shape we never emit.  Only the branch tells them apart.
    assertAlreadyCollapsedInline(inst, "call inline");
    return;
  }
  assertInlineLiteralOutsideSite(load, "call inline");
  auto const start = load.start;
  auto const branchAddr = load.branch;
  auto const literal = load.literal;

  // Claim the site by moving the literal to our target.  Losing means another
  // thread is smashing it and owns the collapse and the retirement below.
  checkSmashDestination(start, literal, "inline literal");
  auto const expected = readTarget32(literal);
  if (!casTarget32(literal, expected, target)) return;
  verifyLiteralWrite(start, literal, target, "call inline");

  // Smash BLR → BL (single instruction).  The dead (ADRP+)LDR remains but is
  // harmless — it loads into rAsm which the BL ignores.
  int64_t offset = target - branchAddr;
  if (!smashLiteralOnly() && canOptimizeSmashedBranch(target) &&
      is_int28(offset)) {
    CodeBlock cb;
    uint32_t newInst;
    cb.init((TCA)&newInst, kInstructionSize, "smashCall");
    MacroAssembler a{cb};
    a.bl(offset >> kInstructionSizeLog2);
    smashInstExpecting(start, branchAddr, UnconditionalBranchToRegisterMask,
                       BLR, newInst, "call inline BLR->BL");
    verifyCollapsedBranch(start, branchAddr, target, "call inline");
  }
}

void smashJmpInline(TCA inst, TCA target) {
  using namespace vixl;
  // See smashCallInline(): snapshot before the same-target claim.
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BR);
  if (!load) {
    assertAlreadyCollapsedInline(inst, "jmp inline");
    return;
  }
  assertInlineLiteralOutsideSite(load, "jmp inline");
  auto const start = load.start;
  auto const branchAddr = load.branch;
  auto const literal = load.literal;

  // If the target is within the smashable jmp itself, redirect it to the end
  // so we don't branch into the middle of our own sequence.
  auto const seqEnd = branchAddr + kInstructionSize;
  if (target > start && target < seqEnd) target = seqEnd;

  checkSmashDestination(start, literal, "inline literal");
  auto const expected = readTarget32(literal);
  if (!casTarget32(literal, expected, target)) return;
  verifyLiteralWrite(start, literal, target, "jmp inline");

  // Smash BR → B (single instruction).  The (ADRP+)LDR before it becomes dead.
  int64_t offset = target - branchAddr;
  if (!smashLiteralOnly() && canOptimizeSmashedBranch(target) &&
      is_int28(offset)) {
    CodeBlock cb;
    uint32_t newInst;
    cb.init((TCA)&newInst, kInstructionSize, "smashJmp");
    MacroAssembler a{cb};
    a.b(offset >> kInstructionSizeLog2);
    smashInstExpecting(start, branchAddr, UnconditionalBranchToRegisterMask,
                       BR, newInst, "jmp inline BR->B");
    verifyCollapsedBranch(start, branchAddr, target, "jmp inline");
  }
}

///////////////////////////////////////////////////////////////////////////////
// smash* — Veneer variants (exact original code)

void smashCallVeneer(TCA inst, TCA target) {
  using namespace vixl;

  if (!possiblySmashableCallVeneer(inst)) {
    // The optimization at the end of this function may already have turned
    // this site into a direct branch, for which the probe returns false.
    assertAlreadyCollapsedVeneer(inst, "call veneer");
    return;
  }

  auto const bl = Instruction::Cast(inst);
  auto const veneer = bl->ImmPCOffsetTarget();
  auto const veneerLoad = smashableVeneerLoad(veneer);
  assert_flog(veneerLoad,
                     "smash call veneer: {} has no veneer load at {}\n",
                     inst, reinterpret_cast<const void*>(veneer));
  patchTarget32(veneerLoad.literalAddress(veneer), target);
  {
    DEBUG_ONLY auto const got =
      *reinterpret_cast<uint32_t*>(veneerLoad.literalAddress(veneer));
    assert_flog(
      got == makeTarget32(target),
      "smash call veneer: veneer literal for {} holds {:#x} after "
      "writing {:#x}\n", inst, got, makeTarget32(target));
  }

  // If the target can be reached through a direct call, patch the original.
  int64_t offset = target - inst;
  if (!smashLiteralOnly() && canOptimizeSmashedBranch(target) &&
      is_int28(offset)) {
    CodeBlock cb;
    uint32_t newInst;
    cb.init((TCA)&newInst, kInstructionSize, "smashCall");
    MacroAssembler a{cb};
    a.bl(offset >> kInstructionSizeLog2);
    smashInstExpecting(inst, inst, UnconditionalBranchMask, BL, newInst,
                       "call veneer BL retarget");
    verifyCollapsedBranch(inst, inst, target, "call veneer");
  }
}

void smashJmpVeneer(TCA inst, TCA target) {
  using namespace vixl;
  if (!possiblySmashableJmpVeneer(inst)) {
    // The optimization at the end of this function may already have turned
    // this site into a direct branch, or into a NOP when the target was the
    // next instruction; the probe returns false for both.
    assertAlreadyCollapsedJmpVeneer(inst, "jmp veneer");
    return;
  }

  // If the target is within the smashable jmp, then set the target to the
  // end. This mirrors logic in x86_64 with the exception that ARM cannot
  // replace the entire smashable jmp with nops.
  if (target > inst && target - inst <= smashableJmpLenVeneer()) {
    target = inst + smashableJmpLenVeneer();
  }

  auto const b = Instruction::Cast(inst);
  auto const veneer = b->ImmPCOffsetTarget();
  auto const veneerLoad = smashableVeneerLoad(veneer);
  assert_flog(veneerLoad,
                     "smash jmp veneer: {} has no veneer load at {}\n",
                     inst, reinterpret_cast<const void*>(veneer));
  patchTarget32(veneerLoad.literalAddress(veneer), target);
  {
    DEBUG_ONLY auto const got =
      *reinterpret_cast<uint32_t*>(veneerLoad.literalAddress(veneer));
    assert_flog(
      got == makeTarget32(target),
      "smash jmp veneer: veneer literal for {} holds {:#x} after "
      "writing {:#x}\n", inst, got, makeTarget32(target));
  }

  // If the target can be reached through a direct jump, patch the original.
  int64_t offset = target - inst;
  if (!smashLiteralOnly() && canOptimizeSmashedBranch(target) &&
      is_int28(offset)) {
    CodeBlock cb;
    uint32_t newInst;
    cb.init((TCA)&newInst, kInstructionSize, "smashJmp");
    MacroAssembler a{cb};
    // offset == 4 means the target is the instruction after the site, so a NOP
    // and a branch there are the same thing; the NOP is the cheaper encoding.
    auto const fallsThrough = offset == kInstructionSize;
    if (fallsThrough) {
      a.nop();
    } else {
      a.b(offset >> kInstructionSizeLog2);
    }
    smashInstExpecting(inst, inst, UnconditionalBranchMask, B, newInst,
                       "jmp veneer B retarget");
    if (!fallsThrough) {
      verifyCollapsedBranch(inst, inst, target, "jmp veneer");
    }
  }
}

void smashJcc(TCA inst, TCA target) {
  using namespace vixl;

  if (!possiblySmashableJcc(inst)) {
    // Same shape as the call and jmp paths: the only legitimate reason a jcc
    // stops matching is that it was already optimized into a direct
    // conditional branch.  Prove it rather than assume it -- a stale address
    // would otherwise be silently "smashed" into nothing.
    assertCollapsed(
      Instruction::Cast(inst)->Mask(ConditionalBranchMask) == B_cond,
      inst, "jcc");
    return;
  }

  auto const b = Instruction::Cast(inst);
  auto const veneer = b->ImmPCOffsetTarget();
  patchTarget32(smashableVeneerLoad(veneer).literalAddress(veneer), target);

  // If the target can be reached through a direct branch, patch the original.
  int64_t offset = target - inst;
  if (!smashLiteralOnly() && canOptimizeSmashedBranch(target) &&
      is_int21(offset)) {
    CodeBlock cb;
    uint32_t newInst;
    cb.init((TCA)&newInst, kInstructionSize, "smashJcc");
    MacroAssembler a{cb};
    auto const cond = static_cast<Condition>(b->ConditionBranch());
    a.b(offset >> kInstructionSizeLog2, cond);
    smashInst(inst, newInst);
  }
}

///////////////////////////////////////////////////////////////////////////////
// smash* — Dispatchers

// The form is fixed for the life of the process -- the flag is read only when
// emitting -- so the dispatchers select on the flag rather than re-deriving the
// form from the bytes.  Do not call a form-specific entry point on a site the
// flag did not produce: the inline parser returns nothing for a veneer site
// and vice versa, which reads as "not smashable" rather than as an error.

void smashCall(TCA inst, TCA target) {
  if (useSmashedCallInline()) return smashCallInline(inst, target);
  smashCallVeneer(inst, target);
}

void smashJmp(TCA inst, TCA target) {
  if (useSmashedJmpInline()) return smashJmpInline(inst, target);
  smashJmpVeneer(inst, target);
}

void smashInterceptJcc(TCA /*inst*/) {
  not_implemented();
}

void smashInterceptJmp(TCA /*inst*/) {
  not_implemented();
}

///////////////////////////////////////////////////////////////////////////////

uint64_t smashableMovqImm(TCA inst) {
  using namespace vixl;

  assertx(possiblySmashableMovq(inst));
  auto const load = smashableMovqLoad(Instruction::Cast(inst));
  assertx(load);
  return *reinterpret_cast<uint64_t*>(load.literalAddress());
}

uint32_t smashableCmpqImm(TCA /*inst*/) {
  not_implemented();
}

///////////////////////////////////////////////////////////////////////////////
// smashable*Target — Inline variants

TCA smashableCallTargetInline(TCA inst) {
  using namespace vixl;
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BLR);
  if (!load) return nullptr;
  auto const target32 = readTarget32(load.literal);
  assertx((target32 & 3) == 0);
  return reinterpret_cast<TCA>(target32);
}

TCA smashableJmpTargetInline(TCA inst) {
  using namespace vixl;
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BR);
  if (!load) return nullptr;
  auto const target32 = readTarget32(load.literal);
  assertx((target32 & 3) == 0);
  return reinterpret_cast<TCA>(target32);
}

///////////////////////////////////////////////////////////////////////////////
// smashable*Target — Veneer variants (exact original code)

TCA smashableCallTargetVeneer(TCA inst) {
  if (!possiblySmashableCallVeneer(inst)) return nullptr;
  using namespace vixl;
  auto const bl = Instruction::Cast(inst);
  auto const veneer = bl->ImmPCOffsetTarget();
  assertx(isSmashableVeneer(veneer));
  auto const target32 =
    *reinterpret_cast<uint32_t*>(
      smashableVeneerLoad(veneer).literalAddress(veneer)
    );
  assertx((target32 & 3) == 0);
  return reinterpret_cast<TCA>(target32);
}

TCA smashableJmpTargetVeneer(TCA inst) {
  if (!possiblySmashableJmpVeneer(inst)) return nullptr;
  using namespace vixl;
  auto const b = Instruction::Cast(inst);
  auto const veneer = b->ImmPCOffsetTarget();
  assertx(isSmashableVeneer(veneer));
  auto const target32 =
    *reinterpret_cast<uint32_t*>(
      smashableVeneerLoad(veneer).literalAddress(veneer)
    );
  assertx((target32 & 3) == 0);
  return reinterpret_cast<TCA>(target32);
}

TCA smashableJccTarget(TCA inst) {
  if (!possiblySmashableJcc(inst)) return nullptr;
  using namespace vixl;
  auto const b = Instruction::Cast(inst);
  auto const veneer = b->ImmPCOffsetTarget();
  assertx(isSmashableVeneer(veneer));
  auto const target32 =
    *reinterpret_cast<uint32_t*>(
      smashableVeneerLoad(veneer).literalAddress(veneer)
    );
  assertx((target32 & 3) == 0);
  return reinterpret_cast<TCA>(target32);
}

///////////////////////////////////////////////////////////////////////////////
// smashable*Target — Dispatchers

TCA smashableCallTarget(TCA inst) {
  return useSmashedCallInline() ? smashableCallTargetInline(inst)
                                : smashableCallTargetVeneer(inst);
}

TCA smashableCallLiteral(TCA inst) {
  using namespace vixl;
  if (!possiblySmashableCall(inst)) return nullptr;
  auto const veneer = Instruction::Cast(inst)->ImmPCOffsetTarget();
  auto const load = smashableVeneerLoad(veneer);
  if (!load) return nullptr;
  return load.literalAddress(veneer);
}

TCA smashableJmpTarget(TCA inst) {
  return useSmashedJmpInline() ? smashableJmpTargetInline(inst)
                               : smashableJmpTargetVeneer(inst);
}

///////////////////////////////////////////////////////////////////////////////
// smashableJccCond

ConditionCode smashableJccCond(TCA inst) {
  using namespace vixl;
  Instruction* b = Instruction::Cast(inst);
  assertx(possiblySmashableJcc(inst));
  return arm::convertCC(static_cast<Condition>(b->Bits(3, 0)));
}

///////////////////////////////////////////////////////////////////////////////
// optimizeSmashed* — Inline variants

bool optimizeSmashedCallInline(TCA inst) {
  using namespace vixl;
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BLR);
  if (!load) return false;
  if (smashLiteralOnly()) return false;
  assertInlineLiteralOutsideSite(load, "optimize call inline");
  auto const target32 = readTarget32(load.literal);
  if (!target32) return false;
  assertx((target32 & 3) == 0);
  auto const target = reinterpret_cast<TCA>(target32);
  if (!canOptimizeSmashedBranch(target)) return false;
  auto const start = load.start;
  // NOP(s) + B{L}, branch left in the slot the B{L}R occupied -- the shape
  // smashCall/JmpInline() also leave.  The branch has to stay last: fixups,
  // catch traces and FuncOrder are keyed on the return address.  Range-check
  // the displacement actually encoded, which is measured from that slot and
  // not from the site start.
  auto const total = load.far ? size_t{3} : size_t{2};
  auto const branchAddr = load.branch;
  int64_t const offset = target - branchAddr;
  if (is_int28(offset)) {
    CodeBlock cb;
    uint32_t insts[3];
    cb.init((TCA)insts, total * kInstructionSize, "optimizeSmashedCall");
    MacroAssembler a{cb};
    for (auto i = size_t{1}; i < total; ++i) a.nop();
    a.bl(offset >> kInstructionSizeLog2);
    for (auto i = size_t{0}; i < total; ++i) {
      // A multi-instruction rewrite, before publish and without core
      // serialization: a wrong address corrupts the whole sequence, so
      // validate every slot.
      checkSmashDestination(start, start + i * kInstructionSize,
                            "optimize call inline");
      smashInst(start + i * kInstructionSize, insts[i]);
    }
    verifyCollapsedBranch(start, branchAddr, target, "optimize call inline");
    return true;
  }
  return false;
}

bool optimizeSmashedJmpInline(TCA inst) {
  using namespace vixl;
  auto const load =
    readInlineLoad(inst, UnconditionalBranchToRegisterMask, BR);
  if (!load) return false;
  if (smashLiteralOnly()) return false;
  assertInlineLiteralOutsideSite(load, "optimize jmp inline");
  auto const target32 = readTarget32(load.literal);
  if (!target32) return false;
  assertx((target32 & 3) == 0);
  auto const target = reinterpret_cast<TCA>(target32);
  if (!canOptimizeSmashedBranch(target)) return false;
  auto const start = load.start;
  // Replace the whole sequence ((ADRP+)LDR + BR) with NOP(s) + B, keeping the
  // branch in the slot the B{L}R occupied -- see optimizeSmashedCallInline()
  // for why the branch stays last and why the range check uses the branch's
  // own displacement.
  auto const total = load.far ? size_t{3} : size_t{2};
  auto const branchAddr = load.branch;
  int64_t const offset = target - branchAddr;
  if (is_int28(offset)) {
    CodeBlock cb;
    uint32_t insts[3];
    cb.init((TCA)insts, total * kInstructionSize, "optimizeSmashedJmp");
    MacroAssembler a{cb};
    for (auto i = size_t{1}; i < total; ++i) a.nop();
    a.b(offset >> kInstructionSizeLog2);
    for (auto i = size_t{0}; i < total; ++i) {
      // See optimizeSmashedCallInline(): a multi-instruction rewrite.
      checkSmashDestination(start, start + i * kInstructionSize,
                            "optimize jmp inline");
      smashInst(start + i * kInstructionSize, insts[i]);
    }
    verifyCollapsedBranch(start, branchAddr, target, "optimize jmp inline");
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// optimizeSmashed* — Veneer variants (original code)

bool optimizeSmashedJcc(TCA inst) {
  using namespace vixl;

  if (!possiblySmashableJcc(inst)) return false;

  // Notice that this optimization prevents a debugger guard from being
  // installed later, so we only perform it in repo authoritative mode.
  if (smashLiteralOnly()) return false;
  if (!Cfg::Repo::Authoritative) return false;

  auto const b = Instruction::Cast(inst);
  auto const target = smashableJccTarget(inst);
  assertx(target);
  auto offset = (intptr_t)target - (intptr_t)b;
  assertx(!is_int21(offset)); // otherwise smashJcc would have optimized it

  auto const veneer = b->ImmPCOffsetTarget();
  auto const load = smashableVeneerLoad(veneer);
  assertx(load);

  offset = (intptr_t)target - (intptr_t)veneer;

  if (is_int28(offset)) {
    auto const veneerInstrs = load.instrCount() + 1; // load + BR
    CodeBlock tmpBlock;
    tmpBlock.init(
      (TCA)veneer,
      veneerInstrs * kInstructionSize,
      "optimizeSmashedJcc"
    );
    MacroAssembler a{tmpBlock};
    a.b(offset >> kInstructionSizeLog2);
    for (auto i = size_t{1}; i < veneerInstrs; ++i) {
      a.nop();
    }
    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////
// optimizeSmashed* — Dispatchers

// Unlike the other dispatchers there is no flag to read: the veneer form
// has no optimization, so the inline attempt is the whole answer.  It parses
// the site, so a veneer site simply fails to match and returns false.
bool optimizeSmashedCall(TCA inst) {
  return optimizeSmashedCallInline(inst);
}

bool optimizeSmashedJmp(TCA inst) {
  return optimizeSmashedJmpInline(inst);
}

///////////////////////////////////////////////////////////////////////////////

}
