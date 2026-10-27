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

#include "hphp/runtime/vm/jit/reg-alloc.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/native-calls.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"

#include "hphp/util/arch.h"
#include "hphp/util/configs/hhir.h"

namespace HPHP::jit {


TRACE_SET_MOD(hhir)

//////////////////////////////////////////////////////////////////////

namespace {

/*
 * Return true if this instruction can load a TypedValue using a 16-byte load
 * into a SIMD register.
 */
bool loadsCell(const IRInstruction& inst) {
  auto const arch_allows = ARCH_MATCH(
    [](arch::X64) { return true; },
    [](arch::ARM) { return true; }
  );

  switch (inst.op()) {
  case LdMem:
    return arch_allows && inst.src(0)->isA(TPtr);

  case LdStk:
  case LdLoc:
  case LdClsCns:
  case LdSubClsCns:
  case CGetProp:
  case DictGet:
  case DictGetQuiet:
  case DictGetK:
  case KeysetGet:
  case KeysetGetQuiet:
  case KeysetGetK:
  case MapGet:
  case CGetElem:
  case DictIdx:
  case KeysetIdx:
  case MemoGetStaticValue:
  case MemoGetStaticCache:
  case MemoGetLSBValue:
  case MemoGetLSBCache:
  case MemoGetInstanceValue:
  case MemoGetInstanceCache:
    return arch_allows;

  default:
    return false;
  }
}

/*
 * Returns true if the instruction can store source operand srcIdx to
 * memory as a cell using a 16-byte store.  (implying its okay to
 * clobber TypedValue.m_aux)
 */
bool storesCell(const IRInstruction& inst, uint32_t srcIdx) {
  // If this function returns true for an operand, then the register allocator
  // may give it an XMM register, and the instruction will store the whole 16
  // bytes into memory.  Therefore it's important *not* to return true if the
  // TypedValue.m_aux field in memory has important data.  This is the case for
  // VanillaDict elements, and Map elements.
  switch (inst.op()) {
  case StLoc:
  case StLocMeta:
    return srcIdx == 1;
  case StStk:
  case StStkMeta:
    return srcIdx == 1;
  case StClsInitElem:
    return srcIdx == 1;
  case StMem:
  case StMemMeta:
    return srcIdx == 1 && inst.src(0)->isA(TPtr);
  default:
    return false;
  }
}

}

//////////////////////////////////////////////////////////////////////

// Assign virtual registers to all SSATmps used or defined in reachable
// blocks. This assigns a value register to constants defined by DefConst,
// because some HHIR instructions require them. Ordinary Gen values with
// a known DataType only get one register. Assign "wide" locations when
// possible (when all uses and defs can be wide). These will be assigned
// SIMD registers later.
void assignRegs(const IRUnit& unit, Vunit& vunit, irlower::IRLS& state,
                const BlockList& blocks) {
  // visit instructions to find tmps eligible to use SIMD registers
  auto const try_wide = Cfg::HHIR::AllocSIMDRegs;
  boost::dynamic_bitset<> not_wide(unit.numTmps());
  StateVector<SSATmp,SSATmp*> tmps(unit, nullptr);
  for (auto block : blocks) {
    for (auto& inst : *block) {
      for (uint32_t i = 0, n = inst.numSrcs(); i < n; i++) {
        auto s = inst.src(i);
        tmps[s] = s;
        if (!try_wide || !storesCell(inst, i)) {
          not_wide.set(s->id());
        }
      }
      for (auto& d : inst.dsts()) {
        tmps[d] = d;
        if (!try_wide || inst.isControlFlow() || !loadsCell(inst)) {
          not_wide.set(d->id());
        }
      }
    }
  }
  // visit each tmp, assign 1 or 2 registers to each.
  for (auto tmp : tmps) {
    if (!tmp) continue;
    if (tmp->inst()->is(DefConst)) {
      auto const loc = make_const(vunit, tmp->type());
      state.locs[tmp] = loc;
      FTRACE(kVasmRegAllocDetailLevel, "const t{} in %{}\n", tmp->id(),
             size_t(loc.reg(0)), size_t(loc.reg(1)));
    } else {
      if (tmp->numWords() == 2) {
        if (!not_wide.test(tmp->id())) {
          auto r = vunit.makeReg();
          state.locs[tmp] = Vloc{Vloc::kWide, r};
          FTRACE(kVasmRegAllocDetailLevel,
                 "def t{} in wide %{}\n", tmp->id(), size_t(r));
        } else {
          auto data = vunit.makeReg();
          auto type = vunit.makeReg();
          state.locs[tmp] = Vloc{data, type};
          FTRACE(kVasmRegAllocDetailLevel,
                 "def t{} in %{},%{}\n", tmp->id(),
                 size_t(data), size_t(type));
        }
      } else {
        auto data = vunit.makeReg();
        state.locs[tmp] = Vloc{data};
        FTRACE(kVasmRegAllocDetailLevel,
               "def t{} in %{}\n", tmp->id(), size_t(data));
      }
    }
  }
}

void getEffects(const Abi& abi, const Vinstr& i,
                RegSet& uses, RegSet& across, RegSet& defs) {
  uses = defs = across = RegSet();
  switch (i.op) {
    case Vinstr::call:
    case Vinstr::callm:
    case Vinstr::callr:
    case Vinstr::calls:
      defs = abi.all() - (abi.calleeSaved | rvmfp() | rsp());
      break;

    case Vinstr::callstub:
      defs =
        (abi.all() - (abi.calleeSaved | rvmfp() | rsp()))
        | jit::abi(CodeKind::CrossTrace).unreserved();
      break;

    case Vinstr::callfaststub:
      defs = abi.all() - abi.calleeSaved - abi.gpUnreserved - (rvmfp() | rsp());
      break;

    case Vinstr::callphp:
    case Vinstr::callphpfe:
    case Vinstr::callphpr:
    case Vinstr::callphps:
    case Vinstr::contenter:
    case Vinstr::inlinesideexit:
      defs = abi.all() - (rvmfp() | rvmtl() | rsp());
      break;

    case Vinstr::cqo:
      uses = RegSet(reg::rax);
      defs = reg::rax | reg::rdx;
      break;
    case Vinstr::idiv:
      uses = defs = reg::rax | reg::rdx;
      break;
    case Vinstr::shlq:
    case Vinstr::sarq:
      across = RegSet(reg::rcx);
      break;

    case Vinstr::pushvmfp:
      defs = RegSet(rvmfp());
      break;

    case Vinstr::pop:
    case Vinstr::popf:
    case Vinstr::popm:
    case Vinstr::popp:
    case Vinstr::poppm:
    case Vinstr::push:
    case Vinstr::pushf:
    case Vinstr::pushm:
    case Vinstr::pushp:
    case Vinstr::pushpm:
      uses = defs = RegSet(rsp());
      break;

    case Vinstr::vcall:
    case Vinstr::vinvoke:
      always_assert(false && "Unsupported instruction in vxls");

    default:
      break;
  }
}

//////////////////////////////////////////////////////////////////////

}
