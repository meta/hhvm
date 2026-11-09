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

#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-record.h"

#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/util/trace.h"

TRACE_SET_MOD(mcg)

namespace HPHP::jit::tc {

void bindCall(TCA toSmash, TCA start, Func* callee, int nArgs) {
  if (start == nullptr) return;

  toSmash = smashableCallStart(toSmash);
  // Return if already smashed.  Note that smashableCallTarget returns nullptr
  // when the target was smashed if the call was able to be optimized in place
  // (so that it doesn't look like a smashable call anymore).
  if (toSmash == nullptr || !smashableCallTarget(toSmash) ||
      smashableCallTarget(toSmash) == start) {
    return;
  }

  // For functions to be PGO'ed, if their current prologues are still profiling
  // ones, then save toSmash as a caller to the prologue, so that it can later
  // be smashed to call a new prologue when it's generated.
  int calleeNumParams = callee->numNonVariadicParams();
  int calledPrologNumArgs = (nArgs <= calleeNumParams ?
                             nArgs :  calleeNumParams + 1);
  auto const profData = jit::profData();

  // Ensure that the smash is performed while holding the lock, this way if
  // the list of prologue callers is resmashed/cleared there won't be a race.
  // Right now we never reset non-profiled prologues so we only need the lock
  // if the prologue being smashed is in Aprof.
  ProfTransRec* rec{nullptr};
  std::unique_lock<Mutex> recLock;
  if (profData) {
    rec = profData->prologueTransRec(callee, calledPrologNumArgs);
    if (rec != nullptr) {
      recLock = rec->lockCallerList();

      // It's possible that the callee prologue was reset before we acquired the
      // lock. Make sure we have the right one.
      auto const trans = mcgen::getFuncPrologue(callee, nArgs);
      start = trans.isProcessPersistentFailure()
        ? tc::ustubs().fcallHelperNoTranslateThunk
        : trans.addr();

      // Do these checks again with the lock.
      if (start == nullptr || !smashableCallTarget(toSmash) ||
          smashableCallTarget(toSmash) == start) {
        return;
      }

      rec->addMainCaller(toSmash);
    }
  }

  assertx(smashableCallTarget(toSmash));
  TRACE(2, "bindCall smash %p -> %p\n", toSmash, start);
  smashCall(toSmash, start);

  // We need to be able to reclaim the function prologues once the unit
  // associated with this function is treadmilled-- so record all of the
  // callers that will need to be re-smashed
  //
  // Additionally for profiled calls we need to remove them from the main
  // caller map.
  if (Cfg::Eval::EnableReusableTC) {
    if (debug || rec) {
      recordFuncCaller(callee, toSmash, rec);
    }
  }
}

}
