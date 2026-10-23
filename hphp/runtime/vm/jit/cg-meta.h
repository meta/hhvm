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

#include <fmt/format.h>

#include "hphp/runtime/base/backtrace.h"

#include "hphp/runtime/vm/prologue-id.h"
#include "hphp/runtime/vm/jit/alignment.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/trans-rec.h"
#include "hphp/runtime/vm/jit/types.h"

#include "hphp/util/growable-vector.h"

#include <map>
#include <string>
#include <vector>

namespace HPHP::jit {

/*
 * CGMeta contains a variety of different metadata information that is
 * collected during code generation.
 *
 * A major use case of this class is to expose metadata to the code relocator
 * for adjustment before it is written to global data structures (e.g., in
 * MCGenerator).
 */
struct CGMeta {
  void process(GrowableVector<IncomingBranch>* inProgressTailBranches);
  void process_only(GrowableVector<IncomingBranch>* inProgressTailBranches);
  void process_literals();
  void processNativeCalls();
  void processPidMapEntries();

  bool empty() const;
  void clear();

  void setJmpTransID(TCA jmp, TransID transID, TransKind kind);

  void setCallFuncId(TCA callRetAddr, FuncId funcId, TransKind kind);

  void setInterceptJccTCA(TCA jccAddr, FuncId funcId);

  /*
   * Code addresses of interest to the code generator.
   *
   * At emit-time, these should be set to point to TCAs of interest, and will
   * be updated if those addresses change.  A watchpoint set immediately
   * following an instruction is guaranteed to still follow it immediately,
   * even if updated.
   */
  std::vector<TCA*> watchpoints;

  /*
   * Pending MCGenerator table entries.
   */
  std::vector<std::pair<TCA,Fixup>> fixups;
  std::vector<std::pair<TCA,Fixup>> trapFixups;
  std::vector<std::pair<TCA,TCA>> catches;
  std::vector<std::pair<TCA,TransID>> jmpTransIDs;
  std::vector<std::pair<TCA,FuncId>> callFuncIds;
  std::vector<std::pair<FuncId,TCA>> interceptTCAs;
  std::vector<std::pair<TCA,Reason>> trapReasons;
  std::vector<IFrame> inlineFrames;
  std::vector<std::pair<TCA,IStack>> inlineStacks;

  /*
   * On some architectures (like ARM) we want to pool up literals and emit them
   * at once.  This allows us to accumulate literals until the end of a
   * Vunit, where we can emit them together.
   */
  struct PoolLiteralMeta {
    uint64_t value;
    CodeAddress patchAddress; // sequence start: ldr or adrp+ldr
    bool smashable;
    uint8_t width;
    bool far; // true when patchAddress starts an ADRP/LDR pair
  };
  std::vector<PoolLiteralMeta> literalsToPool;

  /*
   * We want to collapse emitting literals multiple times.  This map allows us
   * to find the addresses of already emitted literals.
   */
  jit::fast_map<uint64_t, const uint64_t*> literalAddrs;

  struct VeneerData {
    TCA source; // address of instruction that jumps/calls the veneer
    TCA target; // address that the veneer jumps to
    bool smashable; // whether the veneer target can be patched at runtime
  };
  std::vector<VeneerData> veneers;

  /*
   * Addresses of veneers.
   */
  std::set<TCA> veneerAddrs;

  /*
   * Native calls.  Maps addresses in the JIT code cache to the target function being called.
   */
  jit::fast_map<TCA, TCA> nativeCalls;

  struct ROARPidMapEntry {
    uint64_t address;
    uint64_t size;
    std::string name;
  };
  // Populated while publishing metadata, after process()/clear(). It is
  // consumed and cleared explicitly by processPidMapEntries().
  std::vector<ROARPidMapEntry> roarPidMapEntries;

  /*
   * All the alignment constraints on each code address.
   */
  std::multimap<TCA,std::pair<Alignment,AlignContext>> alignments;

  /*
   * Address immediates in the generated code.
   *
   * Also contains the addresses of any mcprep{} instructions that were emitted.
   */
  std::set<TCA> addressImmediates;

  /*
   * Single-bit TST+B.cond pairs whose flags have no other uses. Relocation may
   * shrink these to TBZ/TBNZ when the final target is in range.
   */
  std::set<TCA> testBranches;

  /*
   * Certain addresses are fallthrough to the next vunit.
   * We tag such an address so we can patch a jump over any inserted literal
   * pools/veneers.  This metadata is kept around so the relocator can properly
   * adjust or remove the jump to keep it pointing directly after the Vunit.
   */
  Optional<TCA> fallthru;

  /*
   * Code addresses of interest to other code.
   *
   * These are like `watchpoints', except that the pointers point into the TC
   * data segment rather than, e.g., code generator data structures.  Used for
   * REQ_BIND_ADDR service requests.
   *
   * These also omit the "stickiness" guarantee w.r.t. previous instructions.
   */
  std::set<TCA*> codePointers;

  /*
   * Smash targets of fallback{} and fallbackcc{} instructions, currently
   * pointing to the StubType::Retranslate service request stub).
   *
   * These always correspond to the initial SrcKey of the current IR unit---
   * see cgReqRetranslate().
   */
  GrowableVector<IncomingBranch> inProgressTailJumps;

  /*
   * Smashable locations. Used on relocation to be sure a smashable instruction
   * is not optimized.
   */
  std::set<TCA> smashableLocations;

  /*
   * Extra data kept for smashable calls.  Used to pre-smash calls before the
   * code is published.
   */
  jit::fast_map<TCA,PrologueID> smashableCallData;

  /*
   * List of cross-translation smashable binds, populated by bindjmp, bindjcc,
   * bindaddr, fallback and fallbackcc instructions.
   *
   * In vasm_emit(), we lower these service request instructions to their inline
   * functionality (e.g., a smashable jump), and add a record here so that we
   * can emit the requisite stub and patch in its address after the rest of the
   * unit is emitted. These stubs are emitted separately because they are not
   * truly part of the unit and might be shared across use sites. It is also
   * impossible to emit a code while emitting a code into the same section.
   *
   * Before the code is published, retranslate all uses this list to pre-smash
   * binds to targets that were also translated.
   */
  struct BindData {
    IncomingBranch smashable;
    SrcKey sk;
    SBInvOffset spOff;
    bool fallback;
  };
  std::vector<BindData> smashableBinds;

  /*
   * Debug-only map from bytecode to machine code address.
   */
  std::vector<TransBCMapping> bcMap;
};

/*
 * If the literal val has already been written into the TCA return its address,
 * otherwise return nullptr
 */
const uint64_t* addrForLiteral(uint64_t val);

/*
 * Look up a TCA-to-landingpad mapping.
 */
Optional<TCA> getCatchTrace(CTCA ip);

/*
 * Return the number of registered catch traces
 */
size_t numCatchTraces();

/*
 * Erase catch trace at addr. If no trace is registered at addr no action is
 * performed.
 */
void eraseCatchTrace(CTCA addr);

/*
 * Get the reason for the jit::trap instruction (UD2 on x64) at given address.
 * The returned pointer refers to memory subject to the treadmill, so should
 * only be used in the current request.
 */
Reason* getTrapReason(CTCA addr);

/*
 * Pool up literal to be emitted at patch time.
 */
void poolLiteral(CodeBlock& cb, CGMeta& meta, uint64_t val, uint8_t width,
                 bool smashable, CodeAddress patchAddress = nullptr,
                 bool far = false);

void addSmashableVeneer(CGMeta& meta, TCA source, TCA target);
void addNonSmashableVeneer(CGMeta& meta, TCA source, TCA target);

Optional<IStack> inlineStackAt(CTCA addr);
IFrame getInlineFrame(IFrameID id);
void eraseInlineStack(CTCA addr);
void eraseInlineStacksInRange(CTCA start, CTCA end);

}

namespace folly {
template<> class FormatValue<HPHP::IFrame> {
public:
  explicit FormatValue(HPHP::IFrame ifr) : m_ifr(ifr) {}

  template<typename Callback>
  void format(FormatArg& arg, Callback& cb) const {
    auto str = fmt::format(
      "IFrame{{func = {}, callOff = {}, parent = {}}}",
      m_ifr.func->fullName()->data(),
      m_ifr.callOff,
      m_ifr.parent
    );
    format_value::formatString(str, arg, cb);
  }

private:
  HPHP::IFrame m_ifr;
};
}

template <>
struct fmt::formatter<HPHP::IFrame> : fmt::formatter<std::string_view> {
  auto format(const HPHP::IFrame& ifr, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        fmt::format(
            "IFrame{{func = {}, callOff = {}, parent = {}}}",
            ifr.func->fullName()->data(),
            ifr.callOff,
            ifr.parent),
        ctx);
  }
};
