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

/*
 * The HHVM's ARM64 backend works with an early-truncation policy.
 * That means that:
 *
 *  A Vreg8 is an extended W-register with a u8 value.
 *  A Vreg16 is an extended W-register with a u16 value.
 *  A Vreg32 is a W-register with a u32 value.
 *  A Vreg64 is a X-register with a u64 value.
 *
 * This allows to omit truncation instructions for sub-32-bit
 * operations. E.g. a testb{Vreg8 s0, Vreg8 s1} has to truncate
 * s0 and s1 before emitting a tst instruction. When using the
 * early-truncation policy, the testb{} emitter can rely on the
 * fact, that s0 and s1 are already truncated and can emit a
 * cmp instruction without preceding uxtb's.
 *
 * Conversely any arithmetic instruction has to sign extend any
 * Vreg8 before operating on it. Vasm is light on these instructions,
 * with only the following, currently: csinc[bw]{} and cmp[bw][i]{}.
 *
 * Early-truncation has also consequences to extension/truncation
 * vasm instructions. The following list shows how to use them:
 *
 * movzbw: Vreg8 -> Vreg16:  mov wd, ws #nop if s==d
 * movzbl: Vreg8 -> Vreg32:  mov wd, ws #nop if s==d
 * movzbq: Vreg8 -> Vreg64:  mov wd, ws
 * movzwl: Vreg16 -> Vreg32  mov wd, ws #nop if s==d
 * movzwq: Vreg16 -> Vreg64  mov wd, ws
 * movzlq: Vreg32 -> Vreg64  mov wd, ws
 * movtqb: Vreg64 -> Vreg8:  uxtb w0, w0
 * movtql: Vreg64 -> Vreg32: mov wd, ws
 *
 * Note that movtqb writes a W-register. On AArch64, any W-register write also
 * clears the upper 32 bits of the corresponding X-register, so movtqb leaves
 * a byte-clean 64-bit architectural value as well.
 *
 * Early-truncation also implies, that instructions have to truncate
 * after performing the actual operation if it cannot guarantee that
 * the resulting VregN type matches. E.g. emitting code for the vasm
 * instruction andbi{Immed imm, Vreg8 s, Vreg8 d} has to truncate the
 * result to guarantee that register d indeed holds a u8 value.
 *
 * Note, that the early-truncation policy allows aarch64 specific
 * optimizations, which are not relevant on other architectures.
 * E.g. the x86_64 does not need this policy as the ISA allows
 * direct register accesses for Vreg8, Vreg16, Vreg32 and Vreg64
 * (e.g. AL, AX, EAX, RAX).
 *
 * The early-truncation policy relies on the following
 * requirements of the Vreg type-system:
 *
 *  * All VregNs are created for values of up to N bits
 *  * All conversions between VregNs are done via movz/movt vasm instructions
 */

#include "hphp/runtime/vm/jit/vasm-emit.h"

#include <algorithm>

#include "hphp/runtime/vm/jit/asm-info.h"
#include "hphp/runtime/vm/jit/abi-arm.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/relocation-arm.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/smashable-instr-arm.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-block-counters.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-internal.h"
#include "hphp/runtime/vm/jit/vasm-lower.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"
#include "hphp/runtime/vm/jit/vasm-util-arm.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/roar.h"

#include "hphp/vixl/hphp-compat.h"

#include <limits>

TRACE_SET_MOD(vasm)

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

using namespace arm;
using namespace vixl;

namespace arm { struct ImmFolder; }

namespace {
///////////////////////////////////////////////////////////////////////////////

static_assert(folly::kIsLittleEndian,
  "Code contains little-endian specific optimizations.");

vixl::Register X(Vreg64 r) {
  PhysReg pr(r.asReg());
  return x2a(pr);
}

vixl::Register W(Vreg64 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg32 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg16 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg8 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::VRegister D(Vreg r) {
  return x2f(r);
}

vixl::VRegister V(Vreg r) {
  return x2v(r);
}

uint8_t Log2(uint8_t value) {
  switch (value) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    default:
      always_assert(false);
  }
}

vixl::MemOperand M(Vptr p) {
  assertx(p.base.isValid());
  if (p.index.isValid()) {
    assertx(p.disp == 0);
    return MemOperand(X(p.base), X(p.index), LSL, Log2(p.scale));
  }
  return MemOperand(X(p.base), p.disp);
}

vixl::Condition C(ConditionCode cc) {
  return arm::convertCC(cc);
}

/*
 * Uses the flags from the Vinstr which defs SF to determine
 * whether or not the Vixl assembler should emit code which
 * sets the status flags.
 */
vixl::FlagsUpdate UF(Vflags flags) {
  return flags ? SetFlags : LeaveFlags;
}

/*
 * Helper: new VIXL's And/Bic don't accept FlagsUpdate. Use Ands/Bics when
 * flags are needed.
 */
template<typename Rd, typename Rn, typename Op>
void EmitAnd(vixl::MacroAssembler* a, Rd rd, Rn rn, Op op, Vflags fl) {
  if (fl) a->Ands(rd, rn, op);
  else    a->And(rd, rn, op);
}

/*
 * There are numerous ARM instructions that don't set status flags, and
 * therefore those flags must be set synthetically in the emitters. This
 * assertion is applied to the emitters which don't set all of the status
 * flags required by the Vinstr which defs SF. The flags field of the
 * Vinstr is used to determine which bits are required. Those required
 * bits are compared against the bits which are actually set by the
 * implementation.
 */
template<class Inst> void checkSF(const Inst& i, StatusFlags s) {
  Vflags required = i.fl;
  Vflags set = static_cast<Vflags>(s);
  always_assert_flog((required & set) == required,
                     "should def SF but does not: {}\n",
                     vinst_names[Vinstr(i).op]);
}

template<class Inst> void checkSF(const Inst& i) {
  checkSF(i, StatusFlags::None);
}

/*
 * Returns true if the queried flag(s) is in the set of required flags.
 */
bool flagRequired(Vflags flags, StatusFlags flag) {
  return (flags & static_cast<Vflags>(flag));
}

///////////////////////////////////////////////////////////////////////////////

struct Vgen {
  explicit Vgen(Venv& env)
    : env(env)
    , assem(*env.cb)
    , a(&assem)
    , base(a->frontier())
    , current(env.current)
    , next(env.next)
    , jmps(env.jmps)
    , jccs(env.jccs)
    , cmpbrs(env.cmpbrs)
    , catches(env.catches)
    , vveneers(env.vveneers)
  {}
  ~Vgen() {
    env.cb->sync(base);
  }

  static void emitVeneers(Venv& env);
  static void processVveneers(Venv& env);
  static void handleLiterals(Venv& env);
  static void retargetBinds(Venv& env);
  static void patch(Venv& env);

  static void pad(CodeBlock& cb) {
    vixl::MacroAssembler a { cb };
    auto const begin = cb.frontier();
    while (cb.available() >= 4) a.Brk(1);
    assertx(cb.available() == 0);
    cb.sync(begin);
  }

  /////////////////////////////////////////////////////////////////////////////

  template<class Inst> void emit(const Inst& i) {
    always_assert_flog(false, "unimplemented instruction: {} in B{}\n",
                       vinst_names[Vinstr(i).op], size_t(current));
  }

  void emitCompareAndBranch(vixl::Register r, const Vlabel targets[2],
                            bool branchOnZero);
  void emitTestAndBranch(vixl::Register r, unsigned bit,
                         const Vlabel targets[2], bool branchOnZero);

  // intrinsics
  void emit(const copy& i);
  void emit(const copy2& i);
  void emit(const debugtrap& /*i*/) { a->Brk(0); }
  void emit(const fallthru& /*i*/);
  void emit(const killeffects& /*i*/) {}
  void emit(const ldimmb& i);
  void emit(const ldimml& i);
  void emit(const ldimmq& i);
  void emit(const ldimmw& i);
  void emit(const ldimm128& i);
  void emit(const ldundefq& /*i*/) {}
  void emit(const load& i);
#define DECL_UPDATE(name, pre, post, ...) \
  void emit(const pre&); \
  void emit(const post&);
  VASM_LOAD_UPDATE_SINGLE_LIST(DECL_UPDATE)
  VASM_LOAD_UPDATE_PAIR_LIST(DECL_UPDATE)
  void emit(const store& i);
  VASM_STORE_UPDATE_SINGLE_LIST(DECL_UPDATE)
  VASM_STORE_UPDATE_PAIR_LIST(DECL_UPDATE)
#undef DECL_UPDATE
  void emit(const mcprep& i);

  // native function abi
  void emit(const call& i);
  void emit(const callr& i) {
    if (!i.stackUnaligned) trap_unaligned_stack();
    a->Blr(X(i.target));
  }
  void emit(const calls& i);
  void emit(const ret& /*i*/) { a->Ret(); }

  // stub function abi
  void emit(const callstub& i);
  void emit(const callfaststub& i);

  // php function abi
  void emit(const callphp& i) {
    emit(call{i.target, i.args});
    setCallFuncId(env, a->frontier());
  }
  void emit(const callphpr& i) {
    emit(callr{i.target, i.args});
    setCallFuncId(env, a->frontier());
  }
  void emit(const inlinesideexit& i) {
    emit(call{tc::ustubs().inlineSideExit, i.args});
  }
  void emit(const contenter& i);
  void emit(const phpret& i);

  // vm entry abi
  void emit(const inittc& /*i*/) {}

  // exceptions
  void emit(const landingpad& /*i*/) {}
  void emit(const nothrow& i);
  void emit(const syncpoint& i);
  void emit(const unwind& i);

  // instructions
  void emit(const absdbl& i) { a->Fabs(D(i.d), D(i.s)); }
  void emit(const addl& i) { a->Add(W(i.d), W(i.s1), W(i.s0), UF(i.fl)); }
  void emit(const addli& i) { a->Add(W(i.d), W(i.s1), i.s0.l(), UF(i.fl)); }
  void emit(const addq& i) { a->Add(X(i.d), X(i.s1), X(i.s0), UF(i.fl));}
  void emit(const addqi& i) { a->Add(X(i.d), X(i.s1), i.s0.q(), UF(i.fl)); }
  void emit(const addsd& i) { a->Fadd(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const andb& i) { EmitAnd(a, W(i.d), W(i.s1), W(i.s0), i.fl); }
  void emit(const andbi& i) { EmitAnd(a, W(i.d), W(i.s1), i.s0.ub(), i.fl); }
  void emit(const andw& i) { EmitAnd(a, W(i.d), W(i.s1), W(i.s0), i.fl); }
  void emit(const andwi& i) { EmitAnd(a, W(i.d), W(i.s1), i.s0.uw(), i.fl); }
  void emit(const andl& i) { EmitAnd(a, W(i.d), W(i.s1), W(i.s0), i.fl); }
  void emit(const andli& i) { EmitAnd(a, W(i.d), W(i.s1), i.s0.l(), i.fl); }
  void emit(const andq& i) { EmitAnd(a, X(i.d), X(i.s1), X(i.s0), i.fl); }
  void emit(const andqi& i) { EmitAnd(a, X(i.d), X(i.s1), i.s0.q(), i.fl); }
  void emit(const andqi64& i) { EmitAnd(a, X(i.d), X(i.s1), i.s0.q(), i.fl); }
  void emit(const btrq& i) {
    // NB: We can't directly store the result to i.d because, in case i.s1 is
    // dead after the btrq, the register allocator may allocated both i.d and
    // i.s1 to the same physical register, thus breaking the comparison
    // (subtraction) that is done below.
    a->Bic(rVixlScratch0, X(i.s1), 1 << i.s0.q());

    // Subtract the original value from the result to set the carry bit based on
    // whether they differ.
    a->Sub(vixl::xzr, rVixlScratch0, X(i.s1), UF(i.fl));
    a->Mov(X(i.d), rVixlScratch0);
  }
  void emit(const cbzl& i);
  void emit(const cbnzl& i);
  void emit(const cbzq& i);
  void emit(const cbnzq& i);
  void emit(const tbzl& i);
  void emit(const tbnzl& i);
  void emit(const tbzq& i);
  void emit(const tbnzq& i);
  void emit(const cmovb& i) { a->Csel(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const cmovw& i) { a->Csel(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const cmovl& i) { a->Csel(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const cmovq& i) { a->Csel(X(i.d), X(i.t), X(i.f), C(i.cc)); }
  // note: cmp{bw}[i] are emitted only for narrow comparisons and _do not_ sign
  // extend their arguments--these instructions are lowered to cmp{lq}[i] if
  // the comparison is not narrow or not equality/inequality
  void emit(const cmpb& i) { a->Cmp(W(i.s1), W(i.s0)); }
  void emit(const cmpbi& i) { a->Cmp(W(i.s1), i.s0.ub()); }
  void emit(const cmpw& i) { a->Cmp(W(i.s1), W(i.s0)); }
  void emit(const cmpwi& i) { a->Cmp(W(i.s1), i.s0.uw()); }
  void emit(const cmpl& i) { a->Cmp(W(i.s1), W(i.s0)); }
  void emit(const cmpli& i) { a->Cmp(W(i.s1), i.s0.l()); }
  void emit(const cmpq& i) { a->Cmp(X(i.s1), X(i.s0)); }
  void emit(const cmpqi& i) { a->Cmp(X(i.s1), i.s0.q()); }
  void emit(const cmpsd& i);
  void emit(const cmpsdz& i);
  // TODO(CDE): csinc[bw]{} Should a) sign extend and b) set SF for overflow
  void emit(const csincb& i) { a->Csinc(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const csincw& i) { a->Csinc(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const csincl& i) { a->Csinc(W(i.d), W(i.t), W(i.f), C(i.cc)); }
  void emit(const csincq& i) { a->Csinc(X(i.d), X(i.t), X(i.f), C(i.cc)); }
  void emit(const cvtsi2sd& i) { a->Scvtf(D(i.d), X(i.s)); }
  void emit(const decl& i) { a->Sub(W(i.d), W(i.s), 1, UF(i.fl)); }
  void emit(const decq& i) { a->Sub(X(i.d), X(i.s), 1, UF(i.fl)); }
  void emit(const decqmlock& i);
  void emit(const decqmlocknosf& i);
  void emit(const divint& i) { a->Sdiv(X(i.d), X(i.s0), X(i.s1)); }
  void emit(const divsd& i) { a->Fdiv(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const imul& i);
  void emit(const incl& i) { a->Add(W(i.d), W(i.s), 1, UF(i.fl)); }
  void emit(const incq& i) { a->Add(X(i.d), X(i.s), 1, UF(i.fl)); }
  void emit(const incw& i) { a->Add(W(i.d), W(i.s), 1, UF(i.fl)); }
  void emit(const jcc& i);
  void emit(const jcci& i);
  void emit(const jmp& i);
  void emit(const jmpi& i);
  void emit(const jmpr& i) { a->Br(X(i.target)); }
  void emit(const ldbindretaddr& i);
  void emit(const lea& i);
  void emit(const leap& i);
  void emit(const lead& i);
  void emit(const loadb& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadl& i) { a->Ldr(W(i.d), M(i.s)); }
  void emit(const loadsd& i) { a->Ldr(D(i.d), M(i.s)); }
  void emit(const loadtqb& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadtql& i) { a->Ldr(W(i.d), M(i.s)); }
  void emit(const loadups& i);
  void emit(const loadw& i) { a->Ldrh(W(i.d), M(i.s)); }
  void emit(const loadzbl& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadzbq& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadsbq& i) { a->Ldrsb(X(i.d), M(i.s)); }
  void emit(const loadsbl& i) { a->Ldrsb(W(i.d), M(i.s)); }
  void emit(const loadzwq& i) { a->Ldrh(W(i.d), M(i.s)); }
  void emit(const loadzlq& i) { a->Ldr(W(i.d), M(i.s)); }
  void emit(const movb& i) { if (i.d != i.s) a->Mov(W(i.d), W(i.s)); }
  void emit(const movw& i) { if (i.d != i.s) a->Mov(W(i.d), W(i.s)); }
  void emit(const movl& i) { if (i.d != i.s) a->Mov(W(i.d), W(i.s)); }
  void emit(const movsbl& i) { a->Sxtb(W(i.d), W(i.s)); }
  void emit(const movsbq& i) { a->Sxtb(X(i.d), W(i.s).X()); }
  void emit(const movswl& i) { a->Sxth(W(i.d), W(i.s)); }
  void emit(const movtqb& i) { a->Uxtb(W(i.d), W(i.s)); }
  void emit(const movtqw& i) { a->Uxth(W(i.d), W(i.s)); }
  void emit(const movzbq& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movzwq& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movzlq& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const mulsd& i) { a->Fmul(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const neg& i) {
    if (i.fl) a->Negs(X(i.d), X(i.s));
    else      a->Neg(X(i.d), X(i.s));
  }
  void emit(const nop& /*i*/) { a->Nop(); }
  void emit(const notb& i) { a->Mvn(W(i.d), W(i.s)); }
  void emit(const not_& i) { a->Mvn(X(i.d), X(i.s)); }
  void emit(const orbi& i);
  void emit(const orq& i);
  void emit(const orwi& i);
  void emit(const orli& i);
  void emit(const orqi& i);
  void emit(const pop& i);
  void emit(const popp& i);
  void emit(const push& i);
  void emit(const pushp& i);
  void emit(const roundsd& i);
  void emit(const sar& i);
  void emit(const sarqi& i);
  void emit(const setcc& i) { a->Cset(W(i.d), C(i.cc)); }
  void emit(const shl& i);
  void emit(const shlli& i);
  void emit(const shlqi& i);
  void emit(const shrli& i);
  void emit(const shrqi& i);
  void emit(const sqrtsd& i) { a->Fsqrt(D(i.d), D(i.s)); }
  void emit(const srem& i);
  void emit(const storeb& i) { a->Strb(W(i.s), M(i.m)); }
  void emit(const storel& i) { a->Str(W(i.s), M(i.m)); }
  void emit(const storesd& i) { emit(store{i.s, i.m}); }
  void emit(const storeups& i);
  void emit(const storew& i) { a->Strh(W(i.s), M(i.m)); }
  void emit(const subl& i) { a->Sub(W(i.d), W(i.s1), W(i.s0), UF(i.fl)); }
  void emit(const subli& i) { a->Sub(W(i.d), W(i.s1), i.s0.l(), UF(i.fl)); }
  void emit(const subq& i) { a->Sub(X(i.d), X(i.s1), X(i.s0), UF(i.fl)); }
  void emit(const subqi& i) { a->Sub(X(i.d), X(i.s1), i.s0.q(), UF(i.fl)); }
  void emit(const subsd& i) { a->Fsub(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const testb& i){ a->Tst(W(i.s1), W(i.s0)); }
  void emit(const testbi& i){ a->Tst(W(i.s1), i.s0.ub()); }
  void emit(const testw& i){ a->Tst(W(i.s1), W(i.s0)); }
  void emit(const testwi& i){ a->Tst(W(i.s1), i.s0.uw()); }
  void emit(const testl& i) { a->Tst(W(i.s1), W(i.s0)); }
  void emit(const testli& i) { a->Tst(W(i.s1), i.s0.l()); }
  void emit(const testq& i) { a->Tst(X(i.s1), X(i.s0)); }
  void emit(const testqi& i) { a->Tst(X(i.s1), i.s0.q()); }
  void emit(const testqi64& i) { a->Tst(X(i.s1), i.s0.q()); }
  void emit(const trap& /*i*/);
  void emit(const ucomisd& i) { a->Fcmp(D(i.s0), D(i.s1)); }
  void emit(const ucomisdz& i) { a->Fcmp(D(i.s), 0.0); }
  void emit(const unpcklpd&);
  void emit(const pack2q&);
  void emit(const xorb& i);
  void emit(const xorbi& i);
  void emit(const xorw& i);
  void emit(const xorwi& i);
  void emit(const xorl& i);
  void emit(const xorq& i);
  void emit(const xorqi& i);
  void emit(const xorqi64& i);
  void emit(const crc32q& i) { a->Crc32cx(W(i.d), W(i.s1), X(i.s0)); }

  // arm intrinsics
  void emit(const prefetch& i) { a->Prfm(PLDL1KEEP, M(i.m)); }
  void emit(const fcvtzs& i) { a->Fcvtzs(X(i.d), D(i.s)); }
  void emit(const mrs& i) { a->Mrs(X(i.r), vixl::SystemRegister(i.s.l())); }
  void emit(const msr& i) { a->Msr(vixl::SystemRegister(i.s.l()), X(i.r)); }
  void emit(const ubfmli& i) { a->ubfm(W(i.d), W(i.s), i.mr.w(), i.ms.w()); }
  void emit(const ubfmliq& i) { a->ubfm(X(i.d), X(i.s), i.mr.l(), i.ms.l()); }
  void emit(const sbfizq& i) { a->Sbfiz(X(i.d), X(i.s), i.shift.l(), i.width.l()); }
  void emit(const storepair& i) {
    // storepair addresses base+disp only: STP can't index, and the two-store
    // fallback below would form an unencodable base+index+disp. Creators
    // (storeTV, vasm-simplify) guarantee this.
    assertx(!i.d.index.isValid());
    Vptr hi = i.d;
    hi.disp += 8;
    // Emit a single STP only when both sources are in the same register bank,
    // neither source is sp (STP's register field encodes sp as xzr, silently
    // storing 0), and the displacement is STP-encodable. Otherwise fall back to
    // two single stores: emit(store) lowers each correctly, including the sp
    // special-case and out-of-range displacements.
    if (i.s0.isGP() == i.s1.isGP() &&
        i.s0 != arm::rsp() && i.s1 != arm::rsp() &&
        arm::encodablePair64(i.d)) {
      if (i.s0.isGP()) a->Stp(X(i.s0), X(i.s1), M(i.d));
      else             a->Stp(D(i.s0), D(i.s1), M(i.d));
    } else {
      emit(store{i.s0, i.d});
      emit(store{i.s1, hi});
    }
  }
  void emit(const storepairl& i) { a->Stp(W(i.s0), W(i.s1), M(i.d)); }
  void emit(const storepairups& i) { a->Stp(V(i.s0), V(i.s1), M(i.d)); }
  void emit(const loadpair& i) {
    // loadpair addresses base+disp only: LDP can't index, and the two-load
    // fallback below would form an unencodable base+index+disp.
    assertx(!i.s.index.isValid());
    Vptr hi = i.s;
    hi.disp += 8;
    // Emit a single LDP only when both destinations are in the same register
    // bank and the displacement is LDP-encodable. Otherwise fall back to two
    // single loads, which emit(load) lowers correctly for any bank and
    // displacement. (No sp case here: loadpair never targets sp, matching
    // emit(load).)
    if (i.d0.isGP() == i.d1.isGP() && arm::encodablePair64(i.s)) {
      if (i.d0.isGP()) a->Ldp(X(i.d0), X(i.d1), M(i.s));
      else             a->Ldp(D(i.d0), D(i.d1), M(i.s));
    } else {
      emit(load{i.s, i.d0});
      emit(load{hi, i.d1});
    }
  }
  void emit(const loadpairl& i) { a->Ldp(W(i.d0), W(i.d1), M(i.s)); }
  void emit(const loadpairups& i) {
    assertx(i.d0 != i.d1);
    a->Ldp(V(i.d0), V(i.d1), M(i.s));
  }

  void emit_nop() { a->Nop(); }

private:
  static bool validSimpleUpdateOffset(int32_t offset) {
    return offset >= -256 && offset <= 255;
  }

  static bool validPairUpdateOffset(int32_t offset, int laneSize) {
    if (laneSize <= 0 || (offset % laneSize) != 0) return false;
    auto const scaled = offset / laneSize;
    return scaled >= -64 && scaled <= 63;
  }

  static int32_t checkedSimpleOffset(Immed imm) {
    auto const value = imm.l();
    always_assert_flog(
      value >= std::numeric_limits<int32_t>::min() &&
      value <= std::numeric_limits<int32_t>::max(),
      "Immediate {} out of 32-bit range for simple update", value
    );
    auto const offset = static_cast<int32_t>(value);
    always_assert_flog(
      validSimpleUpdateOffset(offset),
      "Immediate {} out of range for simple pre/post-index update", value
    );
    return offset;
  }

  static int32_t checkedPairOffset(Immed imm, int laneSize) {
    auto const value = imm.l();
    always_assert_flog(
      value >= std::numeric_limits<int32_t>::min() &&
      value <= std::numeric_limits<int32_t>::max(),
      "Immediate {} out of 32-bit range for pair update", value
    );
    auto const offset = static_cast<int32_t>(value);
    always_assert_flog(
      validPairUpdateOffset(offset, laneSize),
      "Immediate {} out of range for pair pre/post-index update with lane {}",
      value, laneSize
    );
    return offset;
  }

  template<class EmitFn>
  void emitMemUpdate(Vreg64 s, Vreg64 base, int32_t offset,
                     AddrMode mode, EmitFn emit_fn) {
    auto const baseReg = X(s);
    auto const mem = MemOperand(baseReg, offset, mode);
    emit_fn(mem);
    if (base != s) {
      FTRACE(1, "emitMemUpdate: base hint missed, emitting Mov {} => {}\n",
             show(s), show(base));
      a->Mov(X(base), baseReg);
    }
  }

  CodeBlock& frozen() { return env.text.frozen().code; }
  static void recordAddressImmediate(Venv& env, TCA addr) {
    env.meta.addressImmediates.insert(addr);
  }
  void recordAddressImmediate() {
    env.meta.addressImmediates.insert(env.cb->frontier());
  }

  void trap_unaligned_stack();

private:
  Venv& env;
  vixl::MacroAssembler assem;
  vixl::MacroAssembler* a;
  Address base;

  const Vlabel current;
  const Vlabel next;
  jit::vector<Venv::LabelPatch>& jmps;
  jit::vector<Venv::LabelPatch>& jccs;
  jit::vector<Venv::LabelPatch>& cmpbrs;
  jit::vector<Venv::LabelPatch>& catches;
  jit::vector<Venv::LabelPatch>& vveneers;
};

///////////////////////////////////////////////////////////////////////////////

void Vgen::trap_unaligned_stack() {
  if (!Cfg::Jit::TrapUnalignedStackCalls) return;

  // We can't test rsp directly, so copy it to rVixlScratch0.
  a->Mov(rVixlScratch0, X(arm::rsp()));
  a->Tst(rVixlScratch0, 0xf); // should be 0 => 16-byte aligned
  vixl::Label Call;
  a->B(&Call, C(jit::CC_Z));
  env.meta.trapReasons.emplace_back(a->frontier(), "unaligned native call");
  a->Brk(1);
  a->bind(&Call);
}

static CodeBlock* getBlock(Venv& env, CodeAddress a) {
  for (auto const& area : env.text.areas()) {
    if (area.code.contains(a)) {
      return &area.code;
    }
  }
  return nullptr;
}

void patchFarLiteralLoad(Instruction* adrpActual,
                         CodeAddress adrpLogical,
                         CodeAddress literalAddress) {
  auto const load = arm::LoadLiteral::at(adrpActual);
  assertx(load && load.isFar());
  auto const setTarget =
    load.setTarget(Instruction::Cast(literalAddress),
                   Instruction::Cast(adrpLogical));
  always_assert_flog(
    setTarget,
    "patchFarLiteralLoad(): cannot encode ADRP/LDR at {} for literal {}\n",
    adrpLogical, literalAddress
  );
  auto const start = reinterpret_cast<CodeAddress>(adrpActual);
  DataBlock::syncDirect(
    start, start + 2 * kInstructionSize
  );
}

void Vgen::emitVeneers(Venv& env) {
  auto& meta = env.meta;
  decltype(env.meta.veneers) notEmitted;

  // Emit non-smashable veneers first so they are placed closer to the code
  // they originate from, then smashable veneers.
  std::stable_sort(meta.veneers.begin(), meta.veneers.end(),
                   [](const CGMeta::VeneerData& a, const CGMeta::VeneerData& b) {
                     return !a.smashable && b.smashable;
                   });

  for (auto const& veneer : meta.veneers) {
    auto cb = getBlock(env, veneer.source);
    if (!cb) {
      // If we can't find the code block, it must have been emitted by a Vunit
      // wrapping this one (bindjmp emits a Vunit within a Vunit).
      notEmitted.push_back(veneer);
      continue;
    }
    auto const vaddr = cb->frontier();

    FTRACE(1, "emitVeneers: source = {}, target = {}, veneer at {}"
           " (smashable={})\n",
           veneer.source, veneer.target, vaddr, veneer.smashable);

    meta.veneerAddrs.insert(vaddr);
    MacroAssembler av{*cb};
    meta.addressImmediates.insert(vaddr);

    int64_t veneerSize;
    if (veneer.smashable) {
      // Emit the veneer code: LDR + BR normally, or ADRP + LDR + BR when far
      // literals are enabled for local TC emission.
      // Keep the target in the literal pool so runtime smashing can patch the
      // loaded value, regardless of whether the load is direct or ADRP + LDR.
      auto const emitFarLiteral = env.unit.farLiteralEnabled();
      emitPooledLiteralLoad(
        av,
        *cb,
        meta,
        (uint64_t)makeTarget32(veneer.target),
        rAsm_w,
        32,
        true,
        emitFarLiteral
      );
      av.Br(rAsm);
      veneerSize = (emitFarLiteral ? 3 : 2) * kInstructionSize;
    } else {
      // Emit the veneer code: MOVZ/MOVK + BR.
      // Always emit exactly 2 mov instructions so the relocator has a
      // fixed-size sequence to rewrite.
      auto const target32 = makeTarget32(veneer.target);
      av.movz(rAsm_w, target32 & 0xFFFF, 0);
      av.movk(rAsm_w, (target32 >> 16) & 0xFFFF, 16);
      av.Br(rAsm);
      veneerSize = 3 * kInstructionSize;
    }
    auto const veneerInstrCount =
        static_cast<int64_t>(veneerSize / kInstructionSize);

    // Update the veneer source instruction to jump/call the veneer.
    auto const realSource = env.text.toDestAddress(veneer.source);
    CodeBlock tmpBlock;
    tmpBlock.init(realSource, kInstructionSize, "emitVeneers");
    MacroAssembler at{tmpBlock};
    int64_t offset = vaddr - veneer.source;
    auto sourceInst = Instruction::Cast(realSource);

    if (sourceInst->Mask(UnconditionalBranchMask) == B) {
      always_assert(is_int28(offset));
      at.b(offset >> kInstructionSizeLog2);

    } else if (sourceInst->Mask(UnconditionalBranchMask) == BL) {
      always_assert(is_int28(offset));
      at.bl(offset >> kInstructionSizeLog2);

    } else if (sourceInst->IsCondBranchImm() || sourceInst->IsCompareBranch()) {
      if (is_int21(offset)) {
        if (sourceInst->IsCompareBranch()) {
          auto const details = getCompareAndBranchDetails(sourceInst);
          if (details.isCbnz) {
            at.cbnz(details.reg, offset >> kInstructionSizeLog2);
          } else {
            at.cbz(details.reg, offset >> kInstructionSizeLog2);
          }
        } else {
          auto const cond = static_cast<Condition>(sourceInst->ConditionBranch());
          at.b(offset >> kInstructionSizeLog2, cond);
        }
      } else {
        // The veneer is outside the conditional branch's range. Use an
        // unconditional branch to an appendix that evaluates the condition.
        // The appendix must be within the unconditional branch's range.
        offset += veneerSize;
        always_assert(is_int28(offset));
        // Add an appendix to the veneer, and jump to it instead.  The full
        // veneer in this case looks like:
        //   VENEER:
        //      LDR/MOV RX, target
        //      BR  RX
        //   APPENDIX:
        //      B.CC VENEER
        //      B NEXT
        // And the conditional jump into the veneer is turned into a jump to the
        // appendix:
        //      B APPENDIX
        //   NEXT:

        // Emit appendix.
        auto const appendix = cb->frontier();
        int imm19 = -veneerInstrCount;
        if (sourceInst->IsCondBranchImm()) {
          auto const cond = static_cast<Condition>(sourceInst->ConditionBranch());
          av.b(imm19, cond);
        } else {
          auto const details = getCompareAndBranchDetails(sourceInst);
          if (details.isCbnz) {
            av.cbnz(details.reg, imm19);
          } else {
            av.cbz(details.reg, imm19);
          }
        }
        const int64_t nextOffset = (veneer.source + kInstructionSize) - // NEXT
          (vaddr + veneerSize + kInstructionSize); // addr of "B NEXT"
        always_assert(is_int28(nextOffset));
        av.b(nextOffset >> kInstructionSizeLog2);

        // Turn the original conditional branch into an unconditional one.
        at.b(offset >> kInstructionSizeLog2);

        // Replace veneer.source with appendix in the relevant metadata.
        meta.smashableLocations.erase(veneer.source);
        meta.smashableLocations.insert(appendix);
        for (auto& tj : meta.inProgressTailJumps) {
          if (tj.toSmash() == veneer.source) tj.adjust(appendix);
        }
        for (auto& bind : meta.smashableBinds) {
          if (bind.smashable.toSmash() == veneer.source) {
            bind.smashable.adjust(appendix);
          }
        }
      }
    } else {
      always_assert_flog(0, "emitVeneers: invalid source instruction at source"
                         " {} (realSource = {})",
                         veneer.source, realSource);
    }
  }

  env.meta.veneers.swap(notEmitted);
}

void Vgen::handleLiterals(Venv& env) {
  decltype(env.meta.literalsToPool) notEmitted;
  for (auto const& pl : env.meta.literalsToPool) {
    auto const cb = getBlock(env, pl.patchAddress);
    if (!cb) {
      // If we can't find the code block it must have been emitted by a Vunit
      // wrapping this one.  (bindjmp emits a Vunit within a Vunit)
      notEmitted.push_back(pl);
      continue;
    }

    // Emit the literal.
    auto literalAddress = cb->frontier();
    if (pl.width == 32) {
      cb->dword(static_cast<uint32_t>(pl.value));
    } else if (pl.width == 64) {
      if (pl.smashable) {
        // Although the region is actually dead, we mark it as live, so that
        // the relocator can remove the padding.
        align(*cb, &env.meta, Alignment::QuadWordSmashable, AlignContext::Live);
        literalAddress = cb->frontier();
      }
      cb->qword(pl.value);
    } else {
      not_reached();
    }

    // Patch the literal load.
    auto const patchStartActual =
      Instruction::Cast(env.text.toDestAddress(pl.patchAddress));

    if (!pl.far) {
      always_assert_flog(
        patchStartActual->IsLoadLiteral(),
        "handleLiterals(): expected a direct literal load at {}\n",
        pl.patchAddress
      );
      auto const imm = static_cast<int64_t>(literalAddress - pl.patchAddress);
      always_assert_flog(
        is_int21(imm),
        "handleLiterals(): literalAddress ({}) is too far from LDR ({})\n",
        literalAddress, pl.patchAddress
      );
      patchStartActual->SetImmPCOffsetTarget(
        Instruction::Cast(literalAddress),
        Instruction::Cast(pl.patchAddress)
      );
      auto const start = reinterpret_cast<CodeAddress>(patchStartActual);
      DataBlock::syncDirect(start, start + kInstructionSize);
      continue;
    }

    patchFarLiteralLoad(patchStartActual, pl.patchAddress, literalAddress);
    continue;
  }

  if (env.meta.fallthru) {
    auto const fallthru = *env.meta.fallthru;
    auto const cb = getBlock(env, fallthru);
    if (!cb) {
      always_assert_flog(false,
                         "Fallthrus shouldn't be used in nested Vunits.");
    }
    auto const blockEndAddr = cb->frontier();
    auto const startAddr = cb->toDestAddress(fallthru);
    CodeBlock tmp;
    tmp.init(startAddr, kInstructionSize, "Tmp");
    // Write the jmp.
    Assembler a { tmp };
    recordAddressImmediate(env, fallthru);
    a.b((blockEndAddr - fallthru) >> kInstructionSizeLog2);
  }

  env.meta.literalsToPool.swap(notEmitted);
}

void Vgen::retargetBinds(Venv& env) {
}

void Vgen::processVveneers(Venv& env) {
  for (auto& vv : env.vveneers) {
    FTRACE(3, "processVveneers: source: {}  target: {} ({})\n",
           vv.instr, env.addrs[vv.target], vv.target);
    addNonSmashableVeneer(env.meta, vv.instr, env.addrs[vv.target]);
  }
}

void Vgen::patch(Venv& env) {
  // Patch the 32 bit target of the LDR
  auto patch = [&env](TCA instr, TCA logicalLoadStart, TCA target) {
    auto const start = Instruction::Cast(instr);
    auto const logicalStart = Instruction::Cast(logicalLoadStart);
    auto const load = arm::LoadLiteral::at(start);
    always_assert_flog(
      load && load.width() == 32,
      "Vgen::patch(): expected 32-bit literal load at {} (logical = {})\n",
      instr,
      logicalLoadStart
    );
    DEBUG_ONLY auto const br = load.ldr()->GetNextInstruction();
    assertx(br->Mask(UnconditionalBranchToRegisterMask) == BR &&
            load.destReg() == br->Rn());
    auto const targetAddr =
      env.text.toDestAddress(load.literalAddress(logicalStart));
    // Patch the 32 bit target following the LDR and BR
    patchTarget32(targetAddr, target);
  };

  for (auto const& p : env.jmps) {
    auto addr = env.text.toDestAddress(p.instr);
    auto logicalLoadStart = p.instr;
    auto const target = env.addrs[p.target];
    assertx(target);
    // Only the veneer form needs the redirect: its site is a B to the
    // veneer that holds the literal.  An inline site already is the load and
    // BR that patch() expects, so it is patched where it stands.
    if (env.meta.smashableLocations.contains(p.instr) &&
        possiblySmashableJmpVeneer(addr)) {
      auto const source = addr;
      // Update `addr' to point to the veneer.
      addr = TCA(vixl::Instruction::Cast(addr)->ImmPCOffsetTarget());
      // Keep the logical address at the same byte offset into the veneer.
      logicalLoadStart += addr - source;
    }
    // Patch the address we are jumping to.
    patch(addr, logicalLoadStart, target);
  }
  for (auto const& p : env.jccs) {
    auto addr = env.text.toDestAddress(p.instr);
    auto logicalLoadStart = p.instr;
    auto const target = env.addrs[p.target];
    assertx(target);
    if (env.meta.smashableLocations.contains(p.instr)) {
      assertx(possiblySmashableJcc(addr));
      auto const source = addr;
      // Update `addr' to point to the veneer.
      addr = TCA(vixl::Instruction::Cast(addr)->ImmPCOffsetTarget());
      // Keep the logical address at the same byte offset into the veneer.
      logicalLoadStart += addr - source;
    }
    patch(addr, logicalLoadStart, target);
  }
  for (auto const& p : env.cmpbrs) {
    auto const addr = env.text.toDestAddress(p.instr);
    auto const target = env.addrs[p.target];
    assertx(target);
    // Compare-branches are never smashable (there is no emitSmashableCb), so
    // unlike jccs the load start never needs redirecting through a veneer.
    patch(addr, p.instr, target);
  }
  for (auto const& p : env.leas) {
    auto addr = env.text.toDestAddress(p.instr);
    auto const target = env.vaddrs[p.target];

    auto const load = arm::LoadLiteral::at(Instruction::Cast(addr));
    auto const literalAddr = env.text.toDestAddress(
      load.literalAddress(Instruction::Cast(p.instr))
    );
    patchTarget32(literalAddr, target);
  }
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const copy& i) {
  if (i.s == i.d) return;
  if (i.s.isGP() && i.d.isGP()) {
    a->Mov(X(i.d), X(i.s));
  } else if (i.s.isSIMD() && i.d.isGP()) {
    a->Fmov(X(i.d), D(i.s));
  } else if (i.s.isGP() && i.d.isSIMD()) {
    a->Fmov(D(i.d), X(i.s));
  } else {
    assertx(i.s.isSIMD() && i.d.isSIMD());
    a->mov(V(i.d), V(i.s));
  }
}

void Vgen::emit(const copy2& i) {
  assertx(i.s0.isValid() && i.s1.isValid() && i.d0.isValid() && i.d1.isValid());
  auto s0 = i.s0, s1 = i.s1, d0 = i.d0, d1 = i.d1;
  assertx(d0 != d1);
  if (d0 == s1) {
    if (d1 == s0) {
      a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);
      a->Mov(rVixlScratch0, X(d0));
      a->Mov(X(d0), X(s0));
      a->Mov(X(s0), rVixlScratch0);
      a->SetScratchRegisters(rVixlScratch0, rVixlScratch1);
    } else {
      // could do this in a simplify pass
      if (s1 != d1) a->Mov(X(d1), X(s1)); // save s1 first; d1 != s0
      if (s0 != d0) a->Mov(X(d0), X(s0));
    }
  } else {
    if (s0 != d0) a->Mov(X(d0), X(s0));
    if (s1 != d1) a->Mov(X(d1), X(s1));
  }
}

void emitSimdImmInt(vixl::MacroAssembler* a, uint64_t val, Vreg d) {
  // Assembler::fmov emits a ldr from a literal pool if IsImmFP64 is false.
  // In that case, emit the raw bits into a GPR first and then move them
  // unmodified into destination SIMD
  union { double dval; uint64_t ival; };
  ival = val;
  if (vixl::Assembler::IsImmFP64(dval)) {
    a->Fmov(D(d), dval);
  } else if (ival == 0) {
    a->Fmov(D(d), vixl::xzr);
  } else {
    a->Mov(rAsm, ival);
    a->Fmov(D(d), rAsm);
  }
}
void Vgen::emit(const fallthru& /*i*/) {
  always_assert(!env.meta.fallthru);
  env.meta.fallthru = a->frontier();
  a->nop();
}

#define Y(vasm_opc, simd_w, vr_w, gpr_w, imm) \
void Vgen::emit(const vasm_opc& i) {          \
  if (i.d.isSIMD()) {                         \
    emitSimdImmInt(a, static_cast<uint##vr_w##_t>(i.s.simd_w()), i.d);     \
  } else {                                    \
    Vreg##vr_w d = i.d;                       \
    a->Mov(gpr_w(d), imm);                    \
  }                                           \
}

Y(ldimmb, ub, 8, W, i.s.ub())
Y(ldimmw, uw, 16, W, i.s.uw())
Y(ldimml, l, 32, W, i.s.l())
Y(ldimmq, q, 64, X, i.s.q())

#undef Y

void Vgen::emit(const ldimm128& i) {
  auto const lo = i.s0.q();
  auto const hi = i.s1.q();
  auto const byte = static_cast<uint8_t>(lo);
  if (lo == splat8x8(byte) && hi == splat8x8(byte)) {
    a->Movi(V(i.d), byte);
    return;
  }
  // Build {0, byte} with three SIMD ops when only the low byte of the upper
  // 64-bit lane is set.
  if (lo == 0 && hi != 0 && (hi & ~uint64_t{0xff}) == 0) {
    auto const hiByte = static_cast<uint8_t>(hi);
    a->Movi(V(i.d).V8B(), hiByte);
    a->ushr(D(i.d), D(i.d), 56);
    a->Ext(V(i.d).V16B(), V(i.d).V16B(), V(i.d).V16B(), 8);
    return;
  }

  emitSimdImmInt(a, lo, i.d);
  if (hi != 0) {
    a->Mov(rAsm, hi);
    a->Ins(V(i.d).V2D(), 1, rAsm);
  }
}

void Vgen::emit(const load& i) {
  if (i.d.isGP()) {
    a->Ldr(X(i.d), M(i.s));
  } else {
    a->Ldr(D(i.d), M(i.s));
  }
}

#define VASM_LOAD_UPDATE_SINGLE_BODY_load(mem, inst)                \
  do {                                                              \
    if ((inst).d.isGP()) {                                          \
      a->Ldr(X((inst).d), (mem));                                   \
    } else {                                                        \
      a->Ldr(D((inst).d), (mem));                                   \
    }                                                               \
  } while (false)
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadb(mem, inst) \
  a->Ldrb(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadw(mem, inst) \
  a->Ldrh(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadl(mem, inst) \
  a->Ldr(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadsd(mem, inst) \
  a->Ldr(D((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadzbl(mem, inst) \
  a->Ldrb(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadzbq(mem, inst) \
  a->Ldrb(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadzwq(mem, inst) \
  a->Ldrh(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadzlq(mem, inst) \
  a->Ldr(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadsbl(mem, inst) \
  a->Ldrsb(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadsbq(mem, inst) \
  a->Ldrsb(X((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadtqb(mem, inst) \
  a->Ldrb(W((inst).d), (mem))
#define VASM_LOAD_UPDATE_SINGLE_BODY_loadtql(mem, inst) \
  a->Ldr(W((inst).d), (mem))

#define VASM_LOAD_UPDATE_PAIR_BODY_loadpair(mem, inst)               \
  do {                                                               \
    assertx((inst).d0 != (inst).d1);                                 \
    a->Ldp(X((inst).d0), X((inst).d1), (mem));                       \
  } while (false)
#define VASM_LOAD_UPDATE_PAIR_BODY_loadpairl(mem, inst)              \
  do {                                                               \
    assertx((inst).d0 != (inst).d1);                                 \
    a->Ldp(W((inst).d0), W((inst).d1), (mem));                       \
  } while (false)
#define VASM_LOAD_UPDATE_PAIR_BODY_loadpairups(mem, inst)            \
  do {                                                               \
    assertx((inst).d0 != (inst).d1);                                 \
    a->Ldp(V((inst).d0), V((inst).d1), (mem));                       \
  } while (false)

#define DEFINE_LOAD_UPDATE_SINGLE(name, pre, post, reg, size, ptr)             \
void Vgen::emit(const pre& i) {                                           \
  auto const offset = checkedSimpleOffset(i.s0);                          \
  emitMemUpdate(i.s, i.base, offset, PreIndex,                      \
                [&](const MemOperand& mem) {                              \
                  VASM_LOAD_UPDATE_SINGLE_BODY_##name(mem, i);            \
                });                                                       \
}                                                                         \
void Vgen::emit(const post& i) {                                          \
  auto const offset = checkedSimpleOffset(i.s0);                          \
  emitMemUpdate(i.s, i.base, offset, PostIndex,                     \
                [&](const MemOperand& mem) {                              \
                  VASM_LOAD_UPDATE_SINGLE_BODY_##name(mem, i);            \
                });                                                       \
}
VASM_LOAD_UPDATE_SINGLE_LIST(DEFINE_LOAD_UPDATE_SINGLE)
#undef DEFINE_LOAD_UPDATE_SINGLE

#define DEFINE_LOAD_UPDATE_PAIR(name, pre, post, reg, laneSize, lanes, ptr)    \
void Vgen::emit(const pre& i) {                                           \
  auto const offset = checkedPairOffset(i.s0, laneSize);                  \
  emitMemUpdate(i.s, i.base, offset, PreIndex,                      \
                [&](const MemOperand& mem) {                              \
                  VASM_LOAD_UPDATE_PAIR_BODY_##name(mem, i);              \
                });                                                       \
}                                                                         \
void Vgen::emit(const post& i) {                                          \
  auto const offset = checkedPairOffset(i.s0, laneSize);                  \
  emitMemUpdate(i.s, i.base, offset, PostIndex,                     \
                [&](const MemOperand& mem) {                              \
                  VASM_LOAD_UPDATE_PAIR_BODY_##name(mem, i);              \
                });                                                       \
}
VASM_LOAD_UPDATE_PAIR_LIST(DEFINE_LOAD_UPDATE_PAIR)
#undef DEFINE_LOAD_UPDATE_PAIR

#undef VASM_LOAD_UPDATE_SINGLE_BODY_load
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadb
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadw
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadl
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadsd
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadzbl
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadzbq
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadzwq
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadzlq
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadsbl
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadsbq
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadtqb
#undef VASM_LOAD_UPDATE_SINGLE_BODY_loadtql
#undef VASM_LOAD_UPDATE_PAIR_BODY_loadpair
#undef VASM_LOAD_UPDATE_PAIR_BODY_loadpairl
#undef VASM_LOAD_UPDATE_PAIR_BODY_loadpairups

void Vgen::emit(const store& i) {
  if (i.s.isGP()) {
    if (i.s == arm::rsp()) {
      a->Mov(rAsm, X(i.s));
      a->Str(rAsm, M(i.d));
    } else {
      a->Str(X(i.s), M(i.d));
    }
  } else {
    a->Str(D(i.s), M(i.d));
  }
}

#define VASM_STORE_UPDATE_SINGLE_BODY_store(mem, inst)                    \
  do {                                                                    \
    if ((inst).v.isGP()) {                                                \
      if ((inst).v == arm::rsp()) {                                       \
        a->Mov(rAsm, X((inst).v));                                        \
        a->Str(rAsm, (mem));                                              \
      } else {                                                            \
        a->Str(X((inst).v), (mem));                                       \
      }                                                                   \
    } else {                                                              \
      a->Str(D((inst).v), (mem));                                         \
    }                                                                     \
  } while (false)
#define VASM_STORE_UPDATE_SINGLE_BODY_storeb(mem, inst) \
  a->Strb(W((inst).v), (mem))
#define VASM_STORE_UPDATE_SINGLE_BODY_storew(mem, inst) \
  a->Strh(W((inst).v), (mem))
#define VASM_STORE_UPDATE_SINGLE_BODY_storel(mem, inst) \
  a->Str(W((inst).v), (mem))
#define VASM_STORE_UPDATE_SINGLE_BODY_storesd(mem, inst) \
  a->Str(D((inst).v), (mem))

#define VASM_STORE_UPDATE_PAIR_BODY_storepair(mem, inst) \
  a->Stp(X((inst).v0), X((inst).v1), (mem))
#define VASM_STORE_UPDATE_PAIR_BODY_storepairl(mem, inst) \
  a->Stp(W((inst).v0), W((inst).v1), (mem))
#define VASM_STORE_UPDATE_PAIR_BODY_storepairups(mem, inst) \
  a->Stp(V((inst).v0), V((inst).v1), (mem))

#define DEFINE_STORE_UPDATE_SINGLE(name, pre, post, reg, size, ptr)            \
void Vgen::emit(const pre& i) {                                           \
  auto const offset = checkedSimpleOffset(i.s0);                          \
  emitMemUpdate(i.s, i.base, offset, PreIndex,                      \
                [&](const MemOperand& mem) {                              \
                  VASM_STORE_UPDATE_SINGLE_BODY_##name(mem, i);           \
                });                                                       \
}                                                                         \
void Vgen::emit(const post& i) {                                          \
  auto const offset = checkedSimpleOffset(i.s0);                          \
  emitMemUpdate(i.s, i.base, offset, PostIndex,                     \
                [&](const MemOperand& mem) {                              \
                  VASM_STORE_UPDATE_SINGLE_BODY_##name(mem, i);           \
                });                                                       \
}
VASM_STORE_UPDATE_SINGLE_LIST(DEFINE_STORE_UPDATE_SINGLE)
#undef DEFINE_STORE_UPDATE_SINGLE

#define DEFINE_STORE_UPDATE_PAIR(name, pre, post, reg, laneSize, lanes, ptr)   \
void Vgen::emit(const pre& i) {                                           \
  auto const offset = checkedPairOffset(i.s0, laneSize);                  \
  emitMemUpdate(i.s, i.base, offset, PreIndex,                      \
                [&](const MemOperand& mem) {                              \
                  VASM_STORE_UPDATE_PAIR_BODY_##name(mem, i);             \
                });                                                       \
}                                                                         \
void Vgen::emit(const post& i) {                                          \
  auto const offset = checkedPairOffset(i.s0, laneSize);                  \
  emitMemUpdate(i.s, i.base, offset, PostIndex,                     \
                [&](const MemOperand& mem) {                              \
                  VASM_STORE_UPDATE_PAIR_BODY_##name(mem, i);             \
                });                                                       \
}
VASM_STORE_UPDATE_PAIR_LIST(DEFINE_STORE_UPDATE_PAIR)
#undef DEFINE_STORE_UPDATE_PAIR

#undef VASM_STORE_UPDATE_SINGLE_BODY_store
#undef VASM_STORE_UPDATE_SINGLE_BODY_storeb
#undef VASM_STORE_UPDATE_SINGLE_BODY_storew
#undef VASM_STORE_UPDATE_SINGLE_BODY_storel
#undef VASM_STORE_UPDATE_SINGLE_BODY_storesd
#undef VASM_STORE_UPDATE_PAIR_BODY_storepair
#undef VASM_STORE_UPDATE_PAIR_BODY_storepairl
#undef VASM_STORE_UPDATE_PAIR_BODY_storepairups

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const mcprep& i) {
  /*
   * Initially, we set the cache to hold (addr << 1) | 1 (where `addr' is the
   * address of the movq) so that we can find the movq from the handler.
   *
   * We set the low bit for two reasons: the Class* will never be a valid
   * Class*, so we'll always miss the inline check before it's smashed, and
   * MethodCache::handleStaticCall can tell it's not been smashed yet
   */

  auto const emitFarLiteral = env.unit.farLiteralEnabled();
  align(*env.cb, &env.meta, Alignment::SmashMovq, AlignContext::Live);
  auto const movAddr =
    ::HPHP::jit::emitSmashableMovq(
      *env.cb,
      env.meta,
      0,
      r64(i.d),
      emitFarLiteral
    );
  auto const movAddrInt = reinterpret_cast<uint64_t>(movAddr);
  // emitSmashableMovq() immediately pools a placeholder literal. The mcprep
  // initial value depends on the movq address, so patch the entry it just
  // appended instead of threading a self-reference through the emitter.
  auto& literal = env.meta.literalsToPool.back();
  assertx(literal.patchAddress == movAddr);
  assertx(literal.far == emitFarLiteral);
  literal.value = (movAddrInt << 1) | 1;

  env.meta.addressImmediates.insert(
    reinterpret_cast<TCA>(~reinterpret_cast<uintptr_t>(movAddr))
  );
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const call& i) {
  if (!i.stackUnaligned) trap_unaligned_stack();
  recordAddressImmediate();
  a->Mov(rAsm, reinterpret_cast<uint64_t>(i.target));
  a->Blr(rAsm);
  if (i.watch) {
    *i.watch = a->frontier();
    env.meta.watchpoints.push_back(i.watch);
  }
}

void Vgen::emit(const calls& i) {
  trap_unaligned_stack();
  auto const addr = emitSmashableCall(*env.cb, env.meta, i.target, env.unit.farLiteralEnabled());
  // When using ROAR, track the native call so we can register them with ROAR
  // after the code is relocated into its final place in the code cache.
  //
  // ROAR accepts the sequence start: BL for a veneer call, LDR or ADRP
  // for an inline call. Its final BL occupies the original branch slot so
  // the return address stays unchanged.
  //
  // ROAR patches a registered site for the life of the process, but a reusable
  // TC frees translations and hands their memory to new code.
  if (use_roar && !Cfg::Eval::EnableReusableTC) {
    env.meta.nativeCalls[addr] = i.target;
  }
  if (i.watch) {
    *i.watch = a->frontier();
    env.meta.watchpoints.push_back(i.watch);
  }
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const callstub& i) {
  emit(call{i.target, i.args});
}

void Vgen::emit(const callfaststub& i) {
  emit(call{i.target, i.args});
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const phpret& i) {
  // prefer load-pair instruction
  if (!i.noframe) {
    a->ldp(X(arm::rvmfp()), X(rlr()), MemOperand(X(i.fp), AROFF(m_sfp)));
  } else {
    a->Ldr(X(rlr()), MemOperand(X(i.fp), AROFF(m_savedRip)));
  }
  emit(ret{});
}

void Vgen::emit(const contenter& i) {
  vixl::Label stub, end;

  // Jump past the stub below.
  recordAddressImmediate();
  a->B(&end);

  // We call into this stub from the end below. Take that LR and store it in
  // m_savedRip. Then jump to the target.
  a->bind(&stub);
  a->Str(X(rlr()), M(i.fp[AROFF(m_savedRip)]));
  a->Br(X(i.target));

  // Call to stub above and then unwind.
  a->bind(&end);
  recordAddressImmediate();
  a->Bl(&stub);
  emit(unwind{{i.targets[0], i.targets[1]}});
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const nothrow& /*i*/) {
  env.meta.catches.emplace_back(a->frontier(), nullptr);
}

void Vgen::emit(const syncpoint& i) {
  FTRACE(5, "IR recordSyncPoint: {} {}\n", a->frontier(), i.fix.show());
  env.meta.fixups.emplace_back(a->frontier(), i.fix);
  env.record_inline_stack(a->frontier());
}

void Vgen::emit(const unwind& i) {
  catches.push_back({a->frontier(), i.targets[1]});
  env.record_inline_stack(a->frontier());
  emit(jmp{i.targets[0]});
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Flags
 *   SF should be set to MSB of the result
 *   CF, OF should be set to (1, 1) if the result is truncated, (0, 0) otherwise
 *   ZF, AF, PF are undefined
 *
 * In the following implementation,
 *   N, Z, V are updated according to result
 *   C is cleared (FIXME)
 */
void Vgen::emit(const imul& i) {
  auto const needV = i.fl && flagRequired(i.fl, StatusFlags::V);

  // Do the multiplication for the upper 64 bits of a 128 bit result. This has
  // to happen before the Mul below, because i.d may be allocated to the same
  // register as i.s0 or i.s1, and the Mul would then clobber the operand.
  if (needV) a->smulh(rAsm, X(i.s0), X(i.s1));

  // Do the multiplication
  a->Mul(X(i.d), X(i.s0), X(i.s1));

  // If we have to set any flags, then always set N and Z since it's cheap.
  // Only set V when absolutely necessary. C is not supported.
  if (i.fl) {
    vixl::Label after;

    checkSF(i, StatusFlags::NotC);

    if (needV) {
      vixl::Label checkSign;
      vixl::Label Overflow;

      // rAsm holds the upper 64 bits, computed above.
      // If the result is not all zeroes or all ones, then we have overflow.
      // If the result is all zeroes or all ones, and the sign is the same,
      // for both hi and low, then there is no overflow.

      // If hi is all 0's or 1's, then check the sign, else overflow
      // (fallthrough).
      recordAddressImmediate();
      a->Cbz(rAsm, &checkSign);
      a->Cmp(rAsm, -1);
      recordAddressImmediate();
      a->B(&checkSign, vixl::eq);

      // Overflow, so conditionally set N and Z bits and then or in V bit.
      a->Bind(&Overflow);
      a->Bics(vixl::xzr, X(i.d), vixl::xzr);
      a->Mrs(rAsm, NZCV);
      a->Orr(rAsm, rAsm, 1<<28);
      a->Msr(NZCV, rAsm);
      recordAddressImmediate();
      a->B(&after);

      // Check the signs of hi and lo.
      a->Bind(&checkSign);
      a->Eor(rAsm, rAsm, X(i.d));
      recordAddressImmediate();
      a->Tbnz(rAsm, 63, &Overflow);
    }

    // No Overflow, so conditionally set the N and Z only
    a->Bics(vixl::xzr, X(i.d), vixl::xzr);

    a->bind(&after);
  }
}

void Vgen::emit(const decqmlock& i) {
  auto adr = M(i.m);
  /* Use VIXL's macroassembler scratch regs. */
  a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);
  if (Cfg::Jit::ArmLse) {
    a->Mov(rVixlScratch0, -1);
    a->ldadd(rVixlScratch0, rVixlScratch0, adr);
    a->Sub(rAsm, rVixlScratch0, 1, SetFlags);
  } else {
    vixl::Label again;
    a->bind(&again);
    a->ldxr(rAsm, adr);
    a->Sub(rAsm, rAsm, 1, SetFlags);
    a->stxr(rVixlScratch0, rAsm, adr);
    recordAddressImmediate();
    a->Cbnz(rVixlScratch0, &again);
  }
  /* Restore VIXL's scratch regs. */
  a->SetScratchRegisters(rVixlScratch0, rVixlScratch1);
}

void Vgen::emit(const decqmlocknosf& i) {
  auto adr = M(i.m);
  /* Use VIXL's macroassembler scratch regs. */
  a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);
  if (Cfg::Jit::ArmLse) {
    a->Mov(rVixlScratch0, -1);
    a->ldadd(rVixlScratch0, rVixlScratch0, adr);
    a->Sub(rAsm, rVixlScratch0, 1, LeaveFlags);
  } else {
    vixl::Label again;
    a->bind(&again);
    a->ldxr(rAsm, adr);
    a->Sub(rAsm, rAsm, 1, LeaveFlags);
    a->stxr(rVixlScratch0, rAsm, adr);
    recordAddressImmediate();
    a->Cbnz(rVixlScratch0, &again);
  }
  /* Restore VIXL's scratch regs. */
  a->SetScratchRegisters(rVixlScratch0, rVixlScratch1);
}

void Vgen::emitCompareAndBranch(vixl::Register r, const Vlabel targets[2],
                                bool branchOnZero) {
  if (targets[1] != targets[0]) {
    if (next == targets[1]) {
      const Vlabel new_targets[2] = {targets[1], targets[0]};
      return emitCompareAndBranch(r, new_targets, !branchOnZero);
    }
    auto taken = targets[1];

    // If the taken block is in a different code area than the compare-branch,
    // we emit a veneer and jump through it to the taken block.  This avoids
    // having to flip the branch and penalizing the fall-through path.
    // Otherwise, we flip the branch and emit a far compare-branch sequence that
    // should be optimized later during relocation's optimizeFarCondBranch,
    // which will flip the branch back.
    if (env.unit.blocks[env.current].area_idx != env.unit.blocks[taken].area_idx) {
      auto source = a->frontier();
      vveneers.push_back({source, taken});
      vixl::Label veneer_addr;
      a->bind(&veneer_addr);
      // Use the raw assembler (like jcc's a->b) so the recorded veneer source
      // is exactly one CB instruction: the MacroAssembler Cbz/Cbnz can flush a
      // literal pool first, which would leave source pointing at the pool guard.
      if (branchOnZero) {
        a->cbz(r, &veneer_addr);
      } else {
        a->cbnz(r, &veneer_addr);
      }
      // NB: this will be patched later.
    } else {
      vixl::Label skip;

      // Emit a "far JCC" sequence for easy patching later.  Static relocation
      // might be able to simplify this later (see optimizeFarCondBranch()).
      recordAddressImmediate();
      if (branchOnZero) {
        a->Cbnz(r, &skip);
      } else {
        a->Cbz(r, &skip);
      }
      auto const loadStart = a->frontier();
      cmpbrs.push_back({loadStart, taken});
      recordAddressImmediate(env, loadStart);
      emitPooledLiteralLoad(
        *a,
        *env.cb,
        env.meta,
        makeTarget32(loadStart),
        rAsm_w,
        32,
        false,
        env.unit.farLiteralEnabled()
      );
      a->Br(rAsm);
      a->bind(&skip);
    }
  }
  emit(jmp{targets[0]});
}

void Vgen::emit(const cbzl& i) {
  emitCompareAndBranch(W(i.s), i.targets, true);
}

void Vgen::emit(const cbnzl& i) {
  emitCompareAndBranch(W(i.s), i.targets, false);
}

void Vgen::emit(const cbzq& i) {
  emitCompareAndBranch(X(i.s), i.targets, true);
}

void Vgen::emit(const cbnzq& i) {
  emitCompareAndBranch(X(i.s), i.targets, false);
}

void Vgen::emitTestAndBranch(vixl::Register r,
                             unsigned bit,
                             const Vlabel targets[2],
                             bool branchOnZero) {
  if (targets[1] == targets[0]) return emit(jmp{targets[0]});
  assertx(bit < static_cast<unsigned>(r.GetSizeInBits()));

  // Keep the flag-setting form until relocation knows the final layout.
  // Relocation can shrink it to TBZ/TBNZ when the imm14 target fits, while
  // jcc's existing far-branch path handles longer targets without padding.
  env.meta.testBranches.insert(a->frontier());
  a->Tst(r, uint64_t{1} << bit);
  emit(jcc{
    branchOnZero ? CC_E : CC_NE,
    VregSF{InvalidReg},
    {targets[0], targets[1]},
    StringTag{}
  });
}

void Vgen::emit(const tbzl& i) {
  emitTestAndBranch(W(i.s), i.bit.l(), i.targets, true);
}

void Vgen::emit(const tbnzl& i) {
  emitTestAndBranch(W(i.s), i.bit.l(), i.targets, false);
}

void Vgen::emit(const tbzq& i) {
  emitTestAndBranch(X(i.s), i.bit.l(), i.targets, true);
}

void Vgen::emit(const tbnzq& i) {
  emitTestAndBranch(X(i.s), i.bit.l(), i.targets, false);
}

void Vgen::emit(const jcc& i) {
  if (i.targets[1] != i.targets[0]) {
    if (next == i.targets[1]) {
      return emit(jcc{ccNegate(i.cc), i.sf, {i.targets[1], i.targets[0]}});
    }
    auto taken = i.targets[1];

    // If the taken block is in a different code area than the jcc, we emit a
    // veneer and jump through it to the taken block.  This avoids having to
    // flip the branch and penalizing the fall-through path.  Otherwise, we flip
    // the branch and emit a "far JCC" sequence that should be optimized later
    // during relocation's optimizeFarCondBranch, which will flip the branch back.
    if (env.unit.blocks[env.current].area_idx != env.unit.blocks[taken].area_idx) {
      auto source = a->frontier();
      vveneers.push_back({source, taken});
      vixl::Label veneer_addr;
      a->bind(&veneer_addr);
      a->b(&veneer_addr, arm::convertCC(i.cc)); // NB: this will be patched later.
    } else {
      vixl::Label skip;

      // Emit a "far JCC" sequence for easy patching later.  Static relocation
      // might be able to simplify this later (see optimizeFarCondBranch()).
      recordAddressImmediate();
      a->B(&skip, vixl::InvertCondition(C(i.cc)));
      auto const loadStart = a->frontier();
      jccs.push_back({loadStart, taken});
      recordAddressImmediate(env, loadStart);
      emitPooledLiteralLoad(
        *a,
        *env.cb,
        env.meta,
        makeTarget32(loadStart),
        rAsm_w,
        32,
        false,
        env.unit.farLiteralEnabled()
      );
      a->Br(rAsm);
      a->bind(&skip);
    }
  }
  emit(jmp{i.targets[0]});
}

void Vgen::emit(const jcci& i) {
  vixl::Label skip;

  recordAddressImmediate();
  a->B(&skip, vixl::InvertCondition(C(i.cc)));
  emit(jmpi{i.taken});
  a->bind(&skip);
}

void Vgen::emit(const jmp& i) {
  if (next == i.target) return;
  auto const loadStart = a->frontier();
  jmps.push_back({loadStart, i.target});

  // Emit a "far JMP" sequence for easy patching later.  Static relocation
  // might be able to simplify this (see optimizeFarJmp()).
  recordAddressImmediate(env, loadStart);
  emitPooledLiteralLoad(
    *a,
    *env.cb,
    env.meta,
    makeTarget32(loadStart),
    rAsm_w,
    32,
    false,
    env.unit.farLiteralEnabled()
  );
  a->Br(rAsm);
}

void Vgen::emit(const jmpi& i) {
  // Cannot use simple a->Mov() since such a sequence cannot be
  // adjusted while live following a relocation.
  auto const loadStart = a->frontier();
  recordAddressImmediate(env, loadStart);
  emitPooledLiteralLoad(
    *a,
    *env.cb,
    env.meta,
    makeTarget32(i.target),
    rAsm_w,
    32,
    false,
    env.unit.farLiteralEnabled()
  );
  a->Br(rAsm);
}

void Vgen::emit(const ldbindretaddr& i) {
  auto const addr = a->frontier();
  emit(leap{reg::rip[(intptr_t)addr], i.d});
  env.ldbindretaddrs.push_back({addr, i.target, i.spOff});
}

void Vgen::emit(const lea& i) {
  auto p = i.s;
  assertx(p.seg == DS);
  if (p.base.isValid()) {
    if (p.index.isValid()) {
      a->Add(X(i.d), X(p.base), Operand(X(p.index), LSL, Log2(p.scale)));
      if (p.disp != 0) a->Add(X(i.d), X(i.d), p.disp);
    } else {
      a->Add(X(i.d), X(p.base), p.disp);
    }
  } else if (p.index.isValid()) {
    assertx(p.scale > 1);
    a->Lsl(X(i.d), X(p.index), Log2(p.scale));
    if (p.disp != 0) a->Add(X(i.d), X(i.d), p.disp);
  } else {
    a->Mov(X(i.d), p.disp);
  }
}

void Vgen::emit(const leap& i) {
  // Cannot use simple a->Mov() since such a sequence cannot be
  // adjusted while live following a relocation.
  auto const loadStart = a->frontier();
  recordAddressImmediate(env, loadStart);
  emitPooledLiteralLoad(
    *a,
    *env.cb,
    env.meta,
    makeTarget32(i.s.r.disp),
    W(i.d),
    32,
    false,
    env.unit.farLiteralEnabled()
  );
}

void Vgen::emit(const lead& i) {
  recordAddressImmediate();
  a->Mov(X(i.d), reinterpret_cast<uint64_t>(i.s.get()));
}

void Vgen::emit(const loadups& i) {
  a->Ldr(D(i.d).Q(), M(i.s));
}

void Vgen::emit(const storeups& i) {
  a->Str(D(i.s).Q(), M(i.m));
}

/*
 * Flags
 *   SF, ZF, PF should be updated according to result
 *   CF, OF should be cleared
 *   AF is undefined
 *
 * In the following implementation,
 *   N, Z are updated according to result
 *   C, V are cleared
 */
#define Y(vasm_opc, arm_opc, gpr_w, s0, zr)           \
void Vgen::emit(const vasm_opc& i) {                  \
  a->arm_opc(gpr_w(i.d), gpr_w(i.s1), s0);            \
  if (i.fl) {                                         \
    a->Bics(vixl::zr, gpr_w(i.d), vixl::zr); \
  }                                                   \
}

Y(orbi, Orr, W, i.s0.ub(), wzr)
Y(orwi, Orr, W, i.s0.uw(), xzr)
Y(orli, Orr, W, i.s0.l(), xzr)
Y(orqi, Orr, X, i.s0.q(), xzr)
Y(orq, Orr, X, X(i.s0), xzr)
Y(xorb, Eor, W, W(i.s0), wzr)
Y(xorbi, Eor, W, i.s0.ub(), wzr)
Y(xorw, Eor, W, W(i.s0), wzr)
Y(xorwi, Eor, W, i.s0.uw(), wzr)
Y(xorl, Eor, W, W(i.s0), wzr)
Y(xorq, Eor, X, X(i.s0), xzr)
Y(xorqi, Eor, X, i.s0.q(), xzr)
Y(xorqi64, Eor, X, i.s0.q(), xzr)

#undef Y

void Vgen::emit(const pop& i) {
  // SP access must be 8 byte aligned. Use rAsm instead.
  a->Mov(rAsm, sp);
  a->Ldr(X(i.d), MemOperand(rAsm, 8, PostIndex));
  a->Mov(sp, rAsm);
}

void Vgen::emit(const push& i) {
  // SP access must be 8 byte aligned. Use rAsm instead.
  a->Mov(rAsm, sp);
  a->Str(X(i.s), MemOperand(rAsm, -8, PreIndex));
  a->Mov(sp, rAsm);
}

void Vgen::emit(const roundsd& i) {
  switch (i.dir) {
    case RoundDirection::nearest: {
      a->frintn(D(i.d), D(i.s));
      break;
    }

    case RoundDirection::floor: {
      a->frintm(D(i.d), D(i.s));
      break;
    }

    case RoundDirection:: ceil: {
      a->frintp(D(i.d), D(i.s));
      break;
    }

    default: {
      assertx(i.dir == RoundDirection::truncate);
      a->frintz(D(i.d), D(i.s));
    }
  }
}

void Vgen::emit(const srem& i) {
  a->Sdiv(rAsm, X(i.s0), X(i.s1));
  a->Msub(X(i.d), rAsm, X(i.s1), X(i.s0));
}

void Vgen::emit(const trap& i) {
  env.meta.trapReasons.emplace_back(a->frontier(), i.reason);
  if (i.fix.isValid()) {
    env.meta.trapFixups.emplace_back(a->frontier(), i.fix);
    env.record_inline_stack(a->frontier());
  }
  // UDF #1 — permanently undefined instruction that raises SIGILL, matching
  // x86_64's ud2 behavior.
  a->udf(1);
}

void Vgen::emit(const unpcklpd& i) {
  // s0 is an across use: d may equal s1, but must not equal s0 because
  // inserting lane 1 reads s0 after d is initialized from s1.
  if (i.d != i.s1) a->fmov(D(i.d), D(i.s1));
  a->Ins(V(i.d).V2D(), 1, V(i.s0).V2D(), 0);
}

void Vgen::emit(const pack2q& i) {
  if (i.s0 == i.s1) {
    a->dup(V(i.d).V2D(), X(i.s0));
    return;
  }

  a->fmov(D(i.d), X(i.s0));
  // Select VIXL's INS (general) form: ins vD.d[1], xS1.
  a->Ins(V(i.d).V2D(), 1, X(i.s1));
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const cmpsd& i) {
  a->fcmeq(D(i.d), D(i.s0), D(i.s1));
  switch (i.pred) {
  case ComparisonPred::eq_ord:
    // Do nothing
    break;
  case ComparisonPred::ne_unord:
    a->mvn(D(i.d), D(i.d));
    break;
  default:
    always_assert(false);
  }
}

void Vgen::emit(const cmpsdz& i) {
  a->fcmeq(D(i.d), D(i.s), 0.0);
  switch (i.pred) {
  case ComparisonPred::eq_ord:
    break;
  case ComparisonPred::ne_unord:
    a->mvn(D(i.d), D(i.d));
    break;
  default:
    always_assert(false);
  }
}


///////////////////////////////////////////////////////////////////////////////

/*
 * For the shifts:
 *
 * C is set through inspection
 * N, Z are updated according to result
 * V is cleared (FIXME)
 * PF, AF are not available
 *
 * Only set the flags if there are any required flags (i.fl).
 * Setting the C flag is particularly expensive, so when setting
 * flags check this flag specifically.
 */
#define Y(vasm_opc, arm_opc, gpr_w, zr)                      \
void Vgen::emit(const vasm_opc& i) {                         \
  if (!i.fl) {                                               \
    /* Just perform the shift. */                            \
    a->arm_opc(gpr_w(i.d), gpr_w(i.s1), gpr_w(i.s0));        \
  } else {                                                   \
    checkSF(i, StatusFlags::NotV);                           \
    if (!flagRequired(i.fl, StatusFlags::C)) {               \
      /* Perform the shift and set N and Z. */               \
      a->arm_opc(gpr_w(i.d), gpr_w(i.s1), gpr_w(i.s0));      \
      a->Bics(vixl::zr, gpr_w(i.d), vixl::zr);      \
    } else {                                                 \
      /* Use VIXL's macroassembler scratch regs. */          \
      a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);      \
      /* Perform the shift using temp and set N and Z. */    \
      a->arm_opc(rVixlScratch0, gpr_w(i.s1), gpr_w(i.s0));   \
      a->Bics(vixl::zr, rVixlScratch0, vixl::zr);   \
      /* Read the flags into a temp. */                      \
      a->Mrs(rAsm, NZCV);                                    \
      /* Reshift right leaving the last bit as bit 0. */     \
      a->Sub(rVixlScratch1, gpr_w(i.s0), 1);                 \
      a->Lsr(rVixlScratch1, gpr_w(i.s1), rVixlScratch1);     \
      /* Negate the bits, including bit 0 to match X64. */   \
      a->Mvn(rVixlScratch1, rVixlScratch1);                  \
      /* Copy bit zero into bit 29 of the flags. */          \
      a->bfm(rAsm, rVixlScratch1, 35, 0);                    \
      /* Copy the flags back to the system register. */      \
      a->Msr(NZCV, rAsm);                                    \
      /* Copy the result to the destination. */              \
      a->Mov(gpr_w(i.d), rVixlScratch0);                     \
      /* Restore VIXL's scratch regs. */                     \
      a->SetScratchRegisters(rVixlScratch0, rVixlScratch1);  \
    }                                                        \
  }                                                          \
}

Y(sar, Asr, X, xzr)

#undef Y

#define Y(vasm_opc, arm_opc, gpr_w, sz, zr)                 \
void Vgen::emit(const vasm_opc& i) {                        \
  if (!i.fl) {                                              \
    /* Just perform the shift. */                           \
    a->arm_opc(gpr_w(i.d), gpr_w(i.s1), gpr_w(i.s0));       \
  } else {                                                  \
    checkSF(i, StatusFlags::NotV);                          \
    if (!flagRequired(i.fl, StatusFlags::C)) {              \
      /* Perform the shift and set N and Z. */              \
      a->arm_opc(gpr_w(i.d), gpr_w(i.s1), gpr_w(i.s0));     \
      a->Bics(vixl::zr, gpr_w(i.d), vixl::zr);     \
    } else {                                                \
      /* Use VIXL's macroassembler scratch regs. */         \
      a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);     \
      /* Perform the shift using temp and set N and Z. */   \
      a->arm_opc(rVixlScratch0, gpr_w(i.s1), gpr_w(i.s0));  \
      a->Bics(vixl::zr, rVixlScratch0, vixl::zr);  \
      /* Read the flags into a temp. */                     \
      a->Mrs(rAsm, NZCV);                                   \
      /* Reshift right leaving the last bit as bit 0. */    \
      a->Mov(rVixlScratch1, sz);                            \
      a->Sub(rVixlScratch1, rVixlScratch1, gpr_w(i.s0));    \
      a->Lsr(rVixlScratch1, gpr_w(i.s1), rVixlScratch1);    \
      /* Negate the bits, including bit 0 to match X64. */  \
      a->Mvn(rVixlScratch1, rVixlScratch1);                 \
      /* Copy bit zero into bit 29 of the flags. */         \
      a->bfm(rAsm, rVixlScratch1, 35, 0);                   \
      /* Copy the flags back to the system register. */     \
      a->Msr(NZCV, rAsm);                                   \
      /* Copy the result to the destination. */             \
      a->Mov(gpr_w(i.d), rVixlScratch0);                    \
      /* Restore VIXL's scratch regs. */                    \
      a->SetScratchRegisters(rVixlScratch0, rVixlScratch1); \
    }                                                       \
  }                                                         \
}

Y(shl, Lsl, X, 64, xzr)

#undef Y

#define Y(vasm_opc, arm_opc, gpr_w, zr)                     \
void Vgen::emit(const vasm_opc& i) {                        \
  if (!i.fl) {                                              \
    /* Just perform the shift. */                           \
    a->arm_opc(gpr_w(i.d), gpr_w(i.s1), i.s0.l());          \
  } else {                                                  \
    checkSF(i, StatusFlags::NotV);                          \
    if (!flagRequired(i.fl, StatusFlags::C)) {              \
      /* Perform the shift and set N and Z. */              \
      a->arm_opc(gpr_w(i.d), gpr_w(i.s1), i.s0.l());        \
      a->Bics(vixl::zr, gpr_w(i.d), vixl::zr);     \
    } else {                                                \
      /* Use VIXL's macroassembler scratch regs. */         \
      a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);     \
      /* Perform the shift using temp and set N and Z. */   \
      a->arm_opc(rVixlScratch0, gpr_w(i.s1), i.s0.l());     \
      a->Bics(vixl::zr, rVixlScratch0, vixl::zr);  \
      /* Read the flags into a temp. */                     \
      a->Mrs(rAsm, NZCV);                                   \
      /* Reshift right leaving the last bit as bit 0. */    \
      a->Lsr(rVixlScratch1, gpr_w(i.s1), i.s0.l() - 1);     \
      /* Negate the bits, including bit 0 to match X64. */  \
      a->Mvn(rVixlScratch1, rVixlScratch1);                 \
      /* Copy bit zero into bit 29 of the flags. */         \
      a->bfm(rAsm, rVixlScratch1, 35, 0);                   \
      /* Copy the flags back to the system register. */     \
      a->Msr(NZCV, rAsm);                                   \
      /* Copy the result to the destination. */             \
      a->Mov(gpr_w(i.d), rVixlScratch0);                    \
      /* Restore VIXL's scratch regs. */                    \
      a->SetScratchRegisters(rVixlScratch0, rVixlScratch1); \
    }                                                       \
  }                                                         \
}

Y(sarqi, Asr, X, xzr)
Y(shrli, Lsr, W, wzr)
Y(shrqi, Lsr, X, xzr)

#undef Y

#define Y(vasm_opc, arm_opc, gpr_w, sz, zr)                  \
void Vgen::emit(const vasm_opc& i) {                         \
  if (!i.fl) {                                               \
    /* Just perform the shift. */                            \
    a->arm_opc(gpr_w(i.d), gpr_w(i.s1), i.s0.l());           \
  } else {                                                   \
    checkSF(i, StatusFlags::NotV);                           \
    if (!flagRequired(i.fl, StatusFlags::C)) {               \
      /* Perform the shift and set N and Z. */               \
      a->arm_opc(gpr_w(i.d), gpr_w(i.s1), i.s0.l());         \
      a->Bics(vixl::zr, gpr_w(i.d), vixl::zr);      \
    } else {                                                 \
      /* Use VIXL's macroassembler scratch regs. */          \
      a->SetScratchRegisters(vixl::NoReg, vixl::NoReg);      \
      /* Perform the shift using temp and set N and Z. */    \
      a->arm_opc(rVixlScratch0, gpr_w(i.s1), i.s0.l());      \
      a->Bics(vixl::zr, rVixlScratch0, vixl::zr);   \
      /* Read the flags into a temp. */                      \
      a->Mrs(rAsm, NZCV);                                    \
      /* Reshift right leaving the last bit as bit 0. */     \
      a->Lsr(rVixlScratch1, gpr_w(i.s1), sz - i.s0.l());     \
      /* Negate the bits, including bit 0 to match X64. */   \
      a->Mvn(rVixlScratch1, rVixlScratch1);                  \
      /* Copy bit zero into bit 29 of the flags. */          \
      a->bfm(rAsm, rVixlScratch1, 35, 0);                    \
      /* Copy the flags back to the system register. */      \
      a->Msr(NZCV, rAsm);                                    \
      /* Copy the result to the destination. */              \
      a->Mov(gpr_w(i.d), rVixlScratch0);                     \
      /* Restore VIXL's scratch regs. */                     \
      a->SetScratchRegisters(rVixlScratch0, rVixlScratch1);  \
    }                                                        \
  }                                                          \
}

Y(shlli, Lsl, W, 32, wzr)
Y(shlqi, Lsl, X, 64, xzr)

#undef Y

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const popp& i) {
  // ldp x0, x0 is unpredictable on ARM
  always_assert(i.d0 != i.d1);
  a->Ldp(X(i.d0), X(i.d1), MemOperand(sp, 16, PostIndex));
}

void Vgen::emit(const pushp& i) {
  a->Stp(X(i.s1), X(i.s0), MemOperand(sp, -16, PreIndex));
}

///////////////////////////////////////////////////////////////////////////////

template<typename Lower>
void lower_impl(Vunit& unit, Vlabel b, size_t i, Lower lower) {
  vmodify(unit, b, i, [&] (Vout& v) { lower(v); return 1; });
}

template <typename Inst>
void lower(const VLS& /*env*/, Inst& /*inst*/, Vlabel /*b*/, size_t /*i*/) {}

///////////////////////////////////////////////////////////////////////////////

enum ImmediateStyle : uint16_t {
  kUnscaledSignedImmediate9 = 0,
  kXPositiveImmediate12,
  kWPositiveImmediate12,
  kHPositiveImmediate12,
  kBPositiveImmediate12,
  kLegacyStyle = kUnscaledSignedImmediate9,
};

static const struct {
  const int16_t immMin;
  const int16_t immMax;
  const uint8_t immStep;
  const bool assertOnOutOfRange;
} ImmediateCharacteristics[] = {
  // kUnscaledSignedImmediate9
  {
    .immMin = -256,
    .immMax = 255,
    .immStep = 1,
    .assertOnOutOfRange = false,
  },
  // kXPositiveImmediate12
  {
    .immMin = 0,
    .immMax = 32760,
    .immStep = 8,
    .assertOnOutOfRange = false,
  },
  // kWPositiveImmediate12
  {
    .immMin = 0,
    .immMax = 16380,
    .immStep = 4,
    .assertOnOutOfRange = false,
  },
  // kHPositiveImmediate12
  {
    .immMin = 0,
    .immMax = 8190,
    .immStep = 2,
    .assertOnOutOfRange = false,
  },
  // kBPositiveImmediate12
  {
    .immMin = 0,
    .immMax = 4095,
    .immStep = 1,
    .assertOnOutOfRange = false,
  },
};

/*
 * TODO: Using load size (ldr[bh]?), apply scaled address if 'disp' is unsigned
 */
void lowerVptr(Vptr& p, Vout& v, ImmediateStyle is = kLegacyStyle, ImmediateStyle alt = kLegacyStyle) {
  enum {
    BASE = 1,
    INDEX = 2,
    DISP = 4
  };

  uint8_t mode = (((p.base.isValid()  & 0x1) << 0) |
                  ((p.index.isValid() & 0x1) << 1) |
                  (((p.disp != 0)     & 0x1) << 2));
  switch (mode) {
    case BASE:
      // ldr/str allow [base], nothing to lower.
      break;

    case BASE | INDEX:
      if (p.scale != 1 && p.scale != uint8_t(p.width)) {
        auto t = v.makeReg();
        v << shlqi{Log2(p.scale), p.index, t, v.makeReg()};
        p.index = t;
        p.scale = 1;
      }
      break;

    case INDEX:
      // Not supported, convert to [base].
      if (p.scale > 1) {
        auto t = v.makeReg();
        v << shlqi{Log2(p.scale), p.index, t, v.makeReg()};
        p.base = t;
      } else {
        p.base = p.index;
      }
      p.index = Vreg{};
      p.scale = 1;
      break;

    case BASE | DISP: {
      // if the immediate value can be directly encoded we have nothing to do
      if (p.disp >= ImmediateCharacteristics[is].immMin &&
          p.disp <= ImmediateCharacteristics[is].immMax &&
          (p.disp % ImmediateCharacteristics[is].immStep) == 0)
        break;
      if (is != alt &&
          p.disp >= ImmediateCharacteristics[alt].immMin &&
          p.disp <= ImmediateCharacteristics[alt].immMax &&
          (p.disp % ImmediateCharacteristics[alt].immStep) == 0)
        break;
      if (ImmediateCharacteristics[is].assertOnOutOfRange) {
        always_assert(false && "Immediate value out of range");
      }

      // #imm is out of range, convert to [base, index]
      auto index = v.makeReg();
      v << ldimmq{Immed64(p.disp), index};
      p.index = index;
      p.scale = 1;
      p.disp = 0;
      break;
    }

    case DISP: {
      // Not supported, convert to [base].
      auto base = v.makeReg();
      v << ldimmq{Immed64(p.disp), base};
      p.base = base;
      p.index = Vreg{};
      p.scale = 1;
      p.disp = 0;
      break;
    }

    case INDEX | DISP:
      // Not supported, convert to [base, #imm] or [base, index].
      if (p.scale > 1) {
        auto t = v.makeReg();
        v << shlqi{Log2(p.scale), p.index, t, v.makeReg()};
        p.base = t;
      } else {
        p.base = p.index;
      }
      if (p.disp >= -256 && p.disp <= 255) {
        p.index = Vreg{};
        p.scale = 1;
      } else {
        auto index = v.makeReg();
        v << ldimmq{Immed64(p.disp), index};
        p.index = index;
        p.scale = 1;
        p.disp = 0;
      }
      break;

    case BASE | INDEX | DISP: {
      // Not supported, convert to [base, index].
      auto index = v.makeReg();
      if (p.scale > 1) {
        auto addr = p;
        addr.base = Vreg{};
        v << lea{addr, index};
      } else {
        v << addqi{p.disp, p.index, index, v.makeReg()};
      }
      p.index = index;
      p.scale = 1;
      p.disp = 0;
      break;
    }
  }
}

void lowerVptrByte(Vptr& p, Vout& v) {
  lowerVptr(p, v, kBPositiveImmediate12, kUnscaledSignedImmediate9);
}

void lowerVptrWord(Vptr& p, Vout& v) {
  lowerVptr(p, v, kHPositiveImmediate12, kUnscaledSignedImmediate9);
}

void lowerVptrLong(Vptr& p, Vout& v) {
  lowerVptr(p, v, kWPositiveImmediate12, kUnscaledSignedImmediate9);
}

void lowerVptrQuad(Vptr& p, Vout& v) {
  lowerVptr(p, v, kXPositiveImmediate12, kUnscaledSignedImmediate9);
}

#define Y(vasm_opc, m)                                      \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lowerVptrQuad(i.m, v);                                  \
    v << i;                                                 \
  });                                                       \
}

Y(load, s)
Y(store, d)

#undef Y

#define Y(vasm_opc, m)                                      \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lowerVptrLong(i.m, v);                                  \
    v << i;                                                 \
  });                                                       \
}

Y(loadl, s)
Y(loadtql, s)
Y(loadzlq, s)
Y(storel, m)

#undef Y

#define Y(vasm_opc, m)                                      \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lowerVptrByte(i.m, v);                                  \
    v << i;                                                 \
  });                                                       \
}

Y(loadb, s)
Y(loadtqb, s)
Y(loadzbl, s)
Y(loadzbq, s)
Y(storeb, m)
Y(loadsbq, s)
Y(loadsbl, s)

#undef Y

#define Y(vasm_opc, m)                                      \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lowerVptrWord(i.m, v);                                  \
    v << i;                                                 \
  });                                                       \
}

Y(loadw, s)
Y(loadzwq, s)
Y(storew, m)

#undef Y

#define Y(vasm_opc, m, lower_vptr)                          \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lower_vptr(i.m, v);                                     \
    v << i;                                                 \
  });                                                       \
}

Y(decqmlock, m, lowerVptr)
Y(decqmlocknosf, m, lowerVptr)
Y(loadsd, s, lowerVptrQuad)
Y(loadups, s, lowerVptr)
Y(storesd, m, lowerVptrQuad)
Y(storeups, m, lowerVptr)

#undef Y

void lower(const VLS& e, lea& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const p = i.s;
    if (!p.base.isValid() && p.index.isValid() && p.scale == 1) {
      if (p.disp != 0) {
        v << addqi{p.disp, p.index, i.d, v.makeReg()};
      } else {
        v << copy{p.index, i.d};
      }
    } else {
      // Unlike memory operands, lea materializes an address. The emitter can
      // keep scaled indexes and macro-expand arbitrary add/sub immediates, so
      // only the baseless scale-1 form needs lowering here.
      v << i;
    }
  });
}

// storepair/loadpair lower to STP/LDP, which address base+disp only -- they can't
// index. Flatten any index into a temp here (lea computes base+index*scale; the
// disp rides along on the pair), so emit only ever sees base+disp. Doing this in
// the lower pass (rather than rejecting indexed pairs at vasm emission) lets
// storeTV/loadTV form pairs optimistically: an index present here may still be
// folded into the displacement by a later pass.
void lower(const VLS& e, storepair& i, Vlabel b, size_t z) {
  if (!i.d.index.isValid()) return;
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const t = v.makeReg();
    auto addr = i.d;
    addr.disp = 0;
    v << lea{addr, t};
    i.d.base = t;
    i.d.index = Vreg{};
    i.d.scale = 1;
    v << i;
  });
}

void lower(const VLS& e, loadpair& i, Vlabel b, size_t z) {
  if (!i.s.index.isValid()) return;
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const t = v.makeReg();
    auto addr = i.s;
    addr.disp = 0;
    v << lea{addr, t};
    i.s.base = t;
    i.s.index = Vreg{};
    i.s.scale = 1;
    v << i;
  });
}

#define Y(vasm_opc, lower_opc, load_opc, store_opc, arg, m, lower_vptr) \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                  \
    lower_vptr(i.m, v);                                     \
    auto r0 = v.makeReg(), r1 = v.makeReg();                \
    v << load_opc{i.m, r0};                                 \
    v << lower_opc{arg, r0, r1, i.sf, i.fl};                \
    v << store_opc{r1, i.m};                                \
  });                                                       \
}

Y(addlim, addli, loadl, storel, i.s0, m, lowerVptrLong)
Y(addlm, addl, loadl, storel, i.s0, m, lowerVptrLong)
Y(addwm, addl, loadw, storew, Reg32(i.s0), m, lowerVptrWord)
Y(addqim, addqi, load, store, i.s0, m, lowerVptrQuad)
Y(andbim, andbi, loadb, storeb, i.s, m, lowerVptrByte)
Y(subqim, subqi, load, store, i.s0, m, lowerVptrQuad)
Y(orbim, orqi, loadb, storeb, i.s0, m, lowerVptrByte)
Y(orqim, orqi, load, store, i.s0, m, lowerVptrQuad)
Y(orwim, orqi, loadw, storew, i.s0, m, lowerVptrWord)
Y(orlim, orqi, loadl, storel, i.s0, m, lowerVptrLong)

#undef Y

#define Y(vasm_opc, lower_opc, movs_opc)                                \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) {             \
  if (!i.fl || (i.fl & static_cast<Vflags>(StatusFlags::NV))) {         \
    lower_impl(e.unit, b, z, [&] (Vout& v) {                            \
      auto r0 = v.makeReg(), r1 = v.makeReg();                          \
      v << movs_opc{i.s0, r0};                                          \
      v << movs_opc{i.s1, r1};                                          \
      v << lower_opc{r0, r1, i.sf, i.fl};                               \
    });                                                                 \
  }                                                                     \
}

Y(cmpb, cmpl, movsbl)
Y(cmpw, cmpl, movswl)

#undef Y

#define Y(vasm_opc, lower_opc, movs_opc)                                \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) {             \
  if (!i.fl || (i.fl & static_cast<Vflags>(StatusFlags::NV))) {         \
    lower_impl(e.unit, b, z, [&] (Vout& v) {                            \
      auto r = v.makeReg();                                             \
      v << movs_opc{i.s1, r};                                           \
      v << lower_opc{i.s0, r, i.sf, i.fl};                              \
    });                                                                 \
  }                                                                     \
}

Y(cmpbi, cmpli, movsbl)
Y(cmpwi, cmpli, movswl)

#undef Y

#define Y(vasm_opc, lower_opc, load_opc, lower_vptr)            \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) {      \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                       \
    lower_vptr(i.s1, v);                                         \
    auto r = e.allow_vreg() ? v.makeReg() : Vreg(PhysReg(rAsm)); \
    v << load_opc{i.s1, r};                                      \
    v << lower_opc{i.s0, r, i.sf, i.fl};                         \
  });                                                            \
}

Y(cmpbim, cmpbi, loadb, lowerVptrByte)
Y(cmplim, cmpli, loadl, lowerVptrLong)
Y(cmpbm, cmpb, loadb, lowerVptrByte)
Y(cmpwm, cmpw, loadb, lowerVptrByte)
Y(cmplm, cmpl, loadl, lowerVptrLong)
Y(cmpqim, cmpqi, load, lowerVptrQuad)
Y(cmpqm, cmpq, load, lowerVptrQuad)
Y(cmpwim, cmpwi, loadw, lowerVptrWord)
Y(testbim, testli, loadb, lowerVptrByte)
Y(testlim, testli, loadl, lowerVptrLong)
Y(testqim, testqi, load, lowerVptrQuad)
Y(testbm, testb, loadb, lowerVptrByte)
Y(testwm, testw, loadw, lowerVptrWord)
Y(testlm, testl, loadl, lowerVptrLong)
Y(testqm, testq, load, lowerVptrQuad)
Y(testwim, testli, loadw, lowerVptrWord)

#undef Y

void lower(const VLS& e, cvtsi2sdm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    lowerVptrQuad(i.s, v);
    auto r = v.makeReg();
    v << load{i.s, r};
    v << cvtsi2sd{r, i.d};
  });
}

#define Y(vasm_opc, lower_opc, load_opc, store_opc, m, lower_vptr) \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) {       \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                        \
    lower_vptr(i.m, v);                                           \
    auto r0 = e.allow_vreg() ? v.makeReg() : Vreg(PhysReg(rAsm)); \
    auto r1 = e.allow_vreg() ? v.makeReg() : Vreg(PhysReg(rAsm)); \
    v << load_opc{i.m, r0};                                       \
    v << lower_opc{r0, r1, i.sf, i.fl};                           \
    v << store_opc{r1, i.m};                                      \
  });                                                             \
}

Y(declm, decl, loadl, storel, m, lowerVptrLong)
Y(decqm, decq, load, store, m, lowerVptrQuad)
Y(inclm, incl, loadl, storel, m, lowerVptrLong)
Y(incqm, incq, load, store, m, lowerVptrQuad)
Y(incwm, incw, loadw, storew, m, lowerVptrWord)

#undef Y

void lower(const VLS& e, cvttsd2siq& i, Vlabel b, size_t idx) {
  lower_impl(e.unit, b, idx, [&] (Vout& v) {
    // Move i.s to a GP register verbatim.
    auto const dbl_bits = v.makeReg();
    v << copy{i.s, dbl_bits};

    // Extract the exponent of the double value.
    auto const dbl_exp = v.makeReg();
    v << ubfmliq(52, 52 + 11 - 1, dbl_bits, dbl_exp);

    // Compare against 0x43e, which is 63 if unbiased (0x43e - 1023 = 63).
    auto const sf = v.makeReg();
    v << cmpqi(0x43e, dbl_exp, sf);

    // Do ARM64's double to signed int64 conversion.
    auto const res = v.makeReg();
    v << fcvtzs{i.s, res};

    // Load error value (-2^63)
    auto const err = v.cns(0x8000000000000000L);

    // Move converted value or error.
    // If exp < 63, the double value is finite and within the range of
    // -2^63 and 2^63 - 1, so fcvtzs always succeeds and the value is
    // chosen as the result.
    // Otherwise, below are all the cases:
    // 1. If dbl is positive, then dbl is >= 2^63, or dbl is an infinity
    //    or NaN. Converting those values to int64 certainly fails and
    //    we choose the error value.
    // 2. If dbl is negative, then dbl is <= -2^63, or dbl is an infinity
    //    or NaN. If dbl is -2^63, the converted integer value is still
    //    -2^63, so the error value is actually correct. The other cases
    //    will lead to conversion failure, which will pick the error value.
    v << cmovq{CC_AE, sf, res, err, i.d};
  });
}

void lower(const VLS& e, callm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    lowerVptrQuad(i.target, v);

    auto const scratch = v.makeReg();

    // Load the target from memory and then call it.
    v << load{i.target, scratch};
    v << callr{scratch, i.args};
  });
}

void lower(const VLS& e, jmpm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    lowerVptrQuad(i.target, v);

    auto const scratch = v.makeReg();

    v << load{i.target, scratch};
    v << jmpr{scratch, i.args};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, restoreripm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    lowerVptrQuad(i.s, v);
    v << load{i.s, rlr()};
  });
}

void lower(const VLS& e, saverips& /*i*/, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Push LR twice to keep stack aligned.
    v << pushp{rlr(), rlr()};
  });
}

void lower(const VLS& e, restorerips& /*i*/, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Pop LR and the stack alignment padding.
    v << popp{PhysReg(rAsm), rlr()};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, stublogue& /*i*/, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Push both the LR and FP regardless of i.saveframe to align SP.
    v << pushp{rlr(), arm::rvmfp()};
  });
}

void lower(const VLS& e, unstublogue& /*i*/, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Pop LR and remove FP from the stack.
    v << popp{PhysReg(rAsm), rlr()};
  });
}

void lower(const VLS& e, stubret& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Pop LR and (optionally) FP.
    if (i.saveframe) {
      v << popp{arm::rvmfp(), rlr()};
    } else {
      v << popp{PhysReg(rAsm), rlr()};
    }

    v << ret{i.args};
  });
}

void lower(const VLS& e, tailcallstub& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Restore LR from native stack and adjust SP.
    v << popp{PhysReg(rAsm), rlr()};

    // Then directly jump to the target.
    v << jmpi{i.target, i.args};
  });
}

void lower(const VLS& e, tailcallstubr& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Restore LR from native stack and adjust SP.
    v << popp{PhysReg(rAsm), rlr()};

    v << jmpr{i.target, i.args};
  });
}

void lower(const VLS& e, stubunwind& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Pop the call frame.
    v << popp{PhysReg(rAsm), i.d};
  });
}

void lower(const VLS& e, stubtophp& /*i*/, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Pop the call frame
    v << lea{arm::rsp()[16], arm::rsp()};
  });
}

void lower(const VLS& e, loadstubret& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Load the LR to the destination.
    v << load{arm::rsp()[AROFF(m_savedRip)], i.d};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, phplogue& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    v << store{rlr(), i.fp[AROFF(m_savedRip)]};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, resumetc& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    // Jump to the translation target.
    v << jmpr{i.target, i.args};
  });
}

  ///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, leavetc& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    v << jmpi{i.exittc};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower(const VLS& e, popm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto r = v.makeReg();
    v << pop{r};
    lowerVptrQuad(i.d, v);
    v << store{r, i.d};
  });
}

void lower(const VLS& e, poppm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto r0 = v.makeReg();
    auto r1 = v.makeReg();
    v << popp{r0, r1};
    lowerVptrQuad(i.d0, v);
    lowerVptrQuad(i.d1, v);
    v << store{r0, i.d0};
    v << store{r1, i.d1};
  });
}

void lower(const VLS& e, pushm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto r = v.makeReg();
    lowerVptrQuad(i.s, v);
    v << load{i.s, r};
    v << push{r};
  });
}

void lower(const VLS& e, pushpm& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto r0 = v.makeReg();
    auto r1 = v.makeReg();
    lowerVptrQuad(i.s0, v);
    lowerVptrQuad(i.s1, v);
    v << load{i.s0, r0};
    v << load{i.s1, r1};
    v << pushp{r0, r1};
  });
}

template<typename movz>
void lower_movz(const VLS& e, movz& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    v << copy{i.s, i.d};
  });
}

void lower(const VLS& e, movzbw& i, Vlabel b, size_t z) {
  lower_movz(e, i, b, z);
}

void lower(const VLS& e, movzbl& i, Vlabel b, size_t z) {
  lower_movz(e, i, b, z);
}

void lower(const VLS& e, movzwl& i, Vlabel b, size_t z) {
  lower_movz(e, i, b, z);
}

void lower(const VLS& e, movtql& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    v << copy{i.s, i.d};
  });
}

void lower(const VLS& e, movtdb& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto d = v.makeReg();
    v << copy{i.s, d};
    v << movtqb{d, i.d};
  });
}

void lower(const VLS& e, movtdq& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    v << copy{i.s, i.d};
  });
}

#define Y(vasm_opc, lower_opc, load_opc, imm, zr, sz, lower_vptr) \
void lower(const VLS& e, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(e.unit, b, z, [&] (Vout& v) {                   \
    lower_vptr(i.m, v);                                 \
    if (imm.sz() == 0u) {                               \
      v << lower_opc{PhysReg(vixl::zr), i.m};           \
    } else {                                            \
      auto r = v.makeReg();                             \
      v << load_opc{imm, r};                            \
      v << lower_opc{r, i.m};                           \
    }                                                   \
  });                                                   \
}

Y(storebi, storeb, ldimmb, i.s, wzr, b, lowerVptrByte)
Y(storewi, storew, ldimmw, i.s, wzr, w, lowerVptrWord)
Y(storeli, storel, ldimml, i.s, wzr, l, lowerVptrLong)
//storeqi only supports 32-bit immediates
Y(storeqi, store, ldimmq, Immed64(i.s.l()), wzr, q, lowerVptrQuad)

#undef Y

void lower(const VLS& e, cloadq& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const scratch = v.makeReg();

    lowerVptrQuad(i.t, v);

    v << load{i.t, scratch};
    v << cmovq{i.cc, i.sf, i.f, scratch, i.d};
  });
}

void lower(const VLS& e, loadqp& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const scratch = v.makeReg();

    v << leap{i.s, scratch};
    v << load{scratch[0], i.d};
  });
}

void lower(const VLS& e, loadqd& i, Vlabel b, size_t z) {
  lower_impl(e.unit, b, z, [&] (Vout& v) {
    auto const scratch = v.makeReg();

    v << lead{i.s.getRaw(), scratch};
    v << load{scratch[0], i.d};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lowerForARM(Vunit& unit) {
  vasm_lower(unit, [&] (const VLS& env, Vinstr& inst, Vlabel b, size_t i) {
    switch (inst.op) {
#define O(name, ...)                      \
      case Vinstr::name:                  \
        lower(env, inst.name##_, b, i);   \
        break;

      VASM_OPCODES
#undef O
    }
  });
}

///////////////////////////////////////////////////////////////////////////////
}

namespace arm {

void optimize(Vunit& unit, const Abi& abi, bool regalloc) {
  Timer timer(Timer::vasm_optimize, unit.log_entry);

  removeTrivialNops(unit);
  optimizePhis(unit);
  fuseBranches(unit);
  optimizeJmps(unit, false, true);

  assertx(checkWidths(unit));

  simplify(unit);

  annotateSFUses(unit);
  lowerForARM(unit);
  eliminateRedundantLoads(unit, abi);

  simplify(unit);

  if (!unit.constToReg.empty()) {
    foldImms<arm::ImmFolder>(unit);
    // foldImms can change flag producers, so refresh SF uses before simplify.
    annotateSFUses(unit);
    simplify(unit);
  }
  reuseImmq(unit);
  sinkDefs(unit, abi);

  optimizeCopies(unit, abi);

  annotateSFUses(unit);
  if (unit.needsRegAlloc()) {
    removeDeadCode(unit);
    if (regalloc) {
      splitCriticalEdges(unit);

      VasmBlockCounters::profileGuidedUpdate(unit);

      if (Cfg::Eval::UseGraphColor &&
          unit.context &&
          (unit.context->kind == TransKind::Optimize ||
           unit.context->kind == TransKind::OptPrologue)) {
        allocateRegistersWithGraphColor(unit, abi);
      } else {
        allocateRegistersWithXLS(unit, abi);
      }

      postRASimplify(unit, abi);
    }
  }

  optimizeExits(unit);
  optimizeJmps(unit, true, false);
}

void emit(Vunit& unit, Vtext& text, CGMeta& fixups,
          AsmInfo* asmInfo) {
  vasm_emit<Vgen>(unit, text, fixups, asmInfo);
}

}

///////////////////////////////////////////////////////////////////////////////
}
