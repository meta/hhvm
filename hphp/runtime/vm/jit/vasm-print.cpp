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

#include "hphp/runtime/vm/jit/vasm-print.h"

#include <sstream>
#include <type_traits>

#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/abi-cxx.h"
#include "hphp/util/arch.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/util/stack-trace.h"

TRACE_SET_MOD(vasm)

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

using Trace::RingBufferType;
using Trace::ringbufferName;

const char* area_names[] = { "main", "cold", "frozen" };

namespace {

#ifdef __aarch64__
const char* vixl_ccs[] = {
  "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
  "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
};
#endif

// Visitor class to format the operands of a Vinstr.  There are imm()
// overloaded methods for each type of operand used by any Vinstr.  If you add
// new imm types, you must add a printer for it here.
struct FormatVisitor {
  FormatVisitor(const Vunit& unit, std::ostringstream& str)
    : unit(unit), str(str)
  {}

  template<class T>
  typename std::enable_if<
    std::is_integral<T>::value && !std::is_same<T,bool>::value
  >::type imm(T t) {
    str << sep() << t;
  }

  template<class T>
  typename std::enable_if<
    std::is_same<T,bool>::value
  >::type imm(T b) { str << sep() << (b ? 'T' : 'F'); }

  template<class T>
  typename std::enable_if<
    std::is_same<T,Immed>::value
  >::type imm(T s) { str << sep() << s.l(); }

  template<class T>
  typename std::enable_if<
    std::is_same<T,Immed64>::value
  >::type imm(T s) {
    str << sep();
    if (s.fits(sz::byte)) str << s.l();
    else str << fmt::format("0x{:08x}", s.q());
  }

  void imm(SBInvOffset off) { str << sep() << off.offset; }
  void imm(ConditionCode cc) { str << sep() << cc_names[cc]; }
#ifdef __aarch64__
  void imm(vixl::Condition cc) { str << sep() << vixl_ccs[cc]; }
#endif
  void imm(TCA addr) {
    str << sep() << getNativeFunctionName(addr);
  }
  void imm(TCA* addr) {
    str << sep() << fmt::format("{}", fmt::ptr(addr));
  }
  template<typename T>
  void imm(VdataPtr<T> ptr) {
    str << fmt::format("{}{}{}",
                         sep(), fmt::ptr(ptr.getRaw()), ptr.bound() ? "" : "(unbound)");
  }
  void imm(const CallSpec& call) {
    switch (call.kind()) {
    default:
      str << sep() << "<unknown>";
      break;
    case CallSpec::Kind::Direct:
    case CallSpec::Kind::Smashable:
      return imm((TCA)call.address());
    case CallSpec::Kind::Destructor:
      str << sep() << fmt::format("destructor({})", show(call.reg()));
      break;
    case CallSpec::Kind::ObjDestructor:
      str << sep() << fmt::format("obj-destructor({})", show(call.reg()));
      break;
    case CallSpec::Kind::Stub:
      return imm(call.stubAddr());
    }
  }
  void imm(RingBufferType t) { str << sep() << ringbufferName(t); }
  void imm(SrcKey k) { str << sep() << showShort(k); }
  void imm(Fixup fix) {
    str << sep() << fix.show();
  }
  void imm(Stats::StatCounter c) { str << sep() << Stats::g_counterNames[c]; }
  void imm(Vlabel b) { str << sep() << "B" << size_t(b); }
  void imm(const Func* func) {
    str << sep();
    if (func) {
      str << fmt::format("{}(id {:#x})", func->fullName()->slice(),
                           func->getFuncId());
    } else {
      str << "nullptr";
    }
  }
  void imm(DestType dt) {
    str << sep() << destTypeName(dt);
  }
  void imm(RIPRelativeRef r) {
    str << sep() << fmt::format("ip[{:#x}]", r.r.disp);
  }
  void imm(RoundDirection rd) {
    str << sep() << show(rd);
  }
  void imm(Vflags /*fl*/) {}

  void imm(Reason r) {
    str << sep() << show(r);
  }

  void imm(RegSet x) { print(x); }
  void imm(ComparisonPred x) {
    str << sep();
    switch (x) {
    case ComparisonPred::eq_ord:   str << "eq_ord"; break;
    case ComparisonPred::ne_unord: str << "ne_unord"; break;
    }
  }

  template<class R> void across(R r) { print(r); }
  template<class R> void use(R r) { print(r); }
  template<class R, class H> void useHint(R r, H) { print(r); }
  void use(Vptr m) { print(m); }
  void use(Vtuple uses) { print(uses); }

  void defSep() {
    if (!seen_def) {
      if (comma) str << " => ";
      else str << "=> ";
      seen_def = true;
      comma = false;
    }
  }
  void def(Vtuple defs) { defSep(); print(defs); }
  template<class R> void def(R r) { defSep(); print(r); }
  template<class R, class H> void defHint(R r, H) { defSep(); print(r); }

  void print(Vptr p) {
    str << sep() << show(p);
  }

  void print(Vtuple t) {
    print(unit.tuples[t]);
  }

  void print(const VregList& regs, const VcallArgs::Spills* spills = nullptr) {
    str << sep() << '{';
    comma = false;
    for (int i = 0; i < regs.size(); ++i) {
      print(regs[i]);
      if (!spills) continue;
      auto const it = spills->find(i);
      if (it == spills->end()) continue;
      str << '+';
      comma = false;
      print(it->second);
    }
    comma = true;
    str << '}';
  }

  void print(VcallArgsId id) {
    auto& args = unit.vcallArgs[id];
    print(args.args, &args.argSpills);
    print(args.simdArgs);
    print(args.stkArgs, &args.stkSpills);
    print(args.indRetArgs);
  }

  void print(RegSet regs) {
    str << sep() << show(regs);
  }

  void print(Vreg r) {
    str << sep() << show(r);

    auto it = unit.regToConst.find(r);
    if (it != unit.regToConst.end()) {
      str << '(' << show(it->second) << ')';
    }
  }

  const char* sep() { return comma ? ", " : (comma = true, ""); }
  const Vunit& unit;

  std::ostringstream& str;
  bool seen_def{false};
  bool comma{false};
};
}

std::string show(Vreg r) {
  if (!r.isValid()) return "%?";
  std::ostringstream str;
  if (r.isPhys()) {
    str << show(r.physReg());
  } else {
    str << "%" << size_t(r);
  }
  return str.str();
}

std::string show(const VregSet& s) {
  std::ostringstream str;
  auto comma = false;
  str << '{';
  for (auto const r : s) {
    if (comma) str << ", ";
    comma = true;
    str << show(r);
  }
  str << '}';
  return str.str();
}

std::string show(const VregList& l) {
  using namespace folly::gen;
  return fmt::format(
    "[{}]",
    from(l)
    | map([] (Vreg r) { return show(r); })
    | unsplit<std::string>(", ")
  );
}

std::string show(Vptr p) {
  std::string str;
  {
    // [%fs + %base + disp + %index * scale]
    str = "[";
    auto prefix = false;
    if (p.seg == Segment::FS) {
      str += "%fs";
      prefix = true;
    }
    if (p.seg == Segment::GS) {
      str += "%gs";
      prefix = true;
    }
      if (p.base.isValid()) {
        folly::toAppend(prefix ? " + " : "", show(p.base), &str);
        prefix = true;
      }
      if (p.disp) {
        fmt::format_to(std::back_inserter(str), "{}{:#x}",
                      prefix ? p.disp < 0 ? " - " : " + " : "",
                      prefix ? std::abs(p.disp) : p.disp);
        prefix = true;
      }
      if (p.index.isValid()) {
        folly::toAppend(prefix ? " + " : "", show(p.index), &str);
        if (p.scale != 1) folly::toAppend(" * ", p.scale, &str);
      }
    str += ']';
    return str;
  }
}

std::string show(Vconst c) {
  auto str = c.isUndef ? "undef:" : folly::to<std::string>(c.val);
  switch (c.kind) {
    case Vconst::Quad:
      str += 'q';
      break;
    case Vconst::Long:
      str += 'l';
      break;
    case Vconst::Byte:
      str += 'b';
      break;
    case Vconst::Double:
      str += 'd';
      break;
  }
  return str;
}

std::string show(const Vunit& unit, const Vinstr& inst) {
  std::ostringstream out;
  out << fmt::format("{: <10} ", vinst_names[inst.op]);
  FormatVisitor pv(unit, out);
  visitOperands(inst, pv);
  auto labels = succs(inst);
  if (labels.size() == 1) {
    out << pv.sep() << fmt::format("B{}", size_t(labels[0]));
  } else if (labels.size() == 2) {
    out << pv.sep() << fmt::format("B{}, else B{}", size_t(labels[1]),
                                     size_t(labels[0]));
  } else {
    for (auto succ : succs(inst)) {
      out << fmt::format("->B{} ", size_t(succ));
    }
  }
  return out.str();
}

void printBlock(std::ostream& out, const Vunit& unit,
                const PredVector& preds, Vlabel b,
                const IRInstruction*& origin) {
  auto& block = unit.blocks[b];
  out << '\n' << color(ANSI_COLOR_MAGENTA);
  out << fmt::format(" B{: <6} {} ({})", size_t(b),
                       area_names[int(block.area_idx)],
                       block.weight);
  for (auto p : preds[b]) out << ", B" << size_t(p);
  out << color(ANSI_COLOR_END) << '\n';

  if (block.code.empty()) {
    out << "        <empty>\n";
    return;
  }

  for (auto& inst : block.code) {
    out << "      ";
    if (origin != inst.origin && inst.origin) {
      origin = inst.origin;
      out << fmt::format("{:<45} # {}\n", show(unit, inst), *origin);
    } else {
      out << show(unit, inst) << '\n';
    }
  }
}

void printInstrs(std::ostream& out,
                 const Vunit& unit,
                 const jit::vector<Vinstr>& code) {
  for (auto& inst : code) {
    out << "        " << show(unit, inst) << '\n';
  }
}

void printCfg(std::ostream& out, const Vunit& unit,
              const jit::vector<Vlabel>& blocks) {
  out << "digraph G {\n";
  for (auto b : blocks) {
    auto& block = unit.blocks[b];
    auto succlist = succs(block);
    if (succlist.empty()) continue;
    auto sep = "";
    for (auto s: succlist) {
      out << sep << 'B' << size_t(b) << " -> B" << size_t(s);
      sep = "; ";
    }
    out << "\n";
  }
  out << "}\n";
}

void printCfg(const Vunit& unit, const jit::vector<Vlabel>& blocks) {
  std::ostringstream out;
  out << "vunit cfg\n";
  printCfg(out, unit, blocks);
  HPHP::Trace::traceRelease("%s\n", out.str().c_str());
}

std::string show(const Vunit& unit) {
  std::ostringstream out;
  auto preds = computePreds(unit);
  boost::dynamic_bitset<> reachableSet(unit.blocks.size());

  // Print reachable blocks first.
  auto reachableBlocks = sortBlocks(unit);
  const IRInstruction* origin = nullptr;
  for (auto b : reachableBlocks) {
    printBlock(out, unit, preds, b, origin);
    reachableSet.set(b);
  }

  // Print unreachable blocks last.
  auto const numUnreachable = reachableSet.size() - reachableSet.count();
  if (numUnreachable == 0) return out.str();

  if (Trace::moduleEnabledRelease(Trace::vasm, kVasmUnreachableLevel)) {
    out << "\nUnreachable blocks:\n";
    for (size_t b = 0; b < unit.blocks.size(); b++) {
      if (!reachableSet.test(b)) {
        printBlock(out, unit, preds, Vlabel{b}, origin);
      }
    }
  } else {
    out << fmt::format("\n{} unreachable blocks not shown. "
                         "Set TRACE=vasm:{} or greater to print them.\n",
                         numUnreachable, kVasmUnreachableLevel);
  }

  return out.str();
}

void printUnit(int level, const std::string& caption, const Vunit& unit) {
  if (!Trace::moduleEnabledRelease(HPHP::Trace::vasm, level)) return;
  Trace::ftraceRelease(
    "\n{}{}\n{}",
    banner(caption.c_str()),
    show(unit),
    banner("")
  );
}

///////////////////////////////////////////////////////////////////////////////

}
