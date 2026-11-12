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

#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/srckey.h"

namespace HPHP {

struct ActRec;

namespace jit {

struct IRUnit;
struct Vunit;

namespace tc {

struct LocalTCBuffer;
struct FuncMetaInfo;

}

/*
 * Arguments for the translate() entry points in Translator.
 *
 * These include a variety of flags that help decide what to translate.
 */
struct TransArgs {
  explicit TransArgs(SrcKey sk) : sk{sk} {}

  SrcKey sk;
  Annotations annotations;
  TransID transId{kInvalidTransID};
  // A sequential per function index to identify optimized
  // translations in TRACE and StructuredLog output (in particular to
  // make it possible to cross reference between the two).
  int optIndex{0};
  TransKind kind{TransKind::Invalid};
  RegionDescPtr region{nullptr};
};

inline tracing::Props traceProps(const TransArgs& a) {
  return traceProps(a.sk.func())
    .add("sk", show(a.sk))
    .add("trans_kind", show(a.kind));
}

////////////////////////////////////////////////////////////////////////////////

namespace mcgen {

/*
 * Look up or translate a func prologue or func body.
 */
TranslationResult getFuncPrologue(Func* func, int nPassed);

/*
 * Create a live or profile retranslation based on args.
 *
 * Will return null if the write-lease could not be obtained or a translation
 * could not be generated.
 */
TranslationResult retranslate(TransArgs args, const RegionContext& ctx);

/*
 * Regionize and optimize the given function using profile data.
 *
 * Returns true iff the function has been successfully optimized.
 */
bool retranslateOpt(const Func* func);

/*
 * In JitPGO mode, run retranslateAll if its enabled, we haven't already run it,
 * and either force is true, or we've collected "enough" profile data.
 *
 * In CLI mode, or when force is true, wait for retranslateAll to
 * finish; otherwise let it run in parallel.
 */
void checkRetranslateAll(bool force = false, bool skipSerialize = false);

/*
 * If JIT optimized code profile-data serialization is enabled and scheduled to
 * trigger in the future, check if we hit one of the triggering conditions and,
 * of so, append the data to the file containing the serialized profile.
 */
void checkSerializeOptProf();

/*
 * Called once when the JIT is activated to initialize internal mcgen structures
 */
void processInit();

/*
 * Called once before process shutdown. May block to wait for any pending JIT
 * worker threads.
 */
void joinWorkerThreads();

/*
 * Wait until the retranslateAll workers are done with `info', whether or not
 * they optimized its function.
 */
void waitForTranslate(tc::FuncMetaInfo& info);

/*
 * True iff mcgen::processInit() has been called
 */
bool initialized();

/*
 * Return the timestamp at which mcgen::processInit was called
 */
int64_t jitInitTime();

/*
 * Whether we should dump TC annotations for translations of `transKind'.
 */
bool dumpTCAnnotation(TransKind transKind);

/*
 * How many JIT worker threads are active.
 */
int getActiveWorker();

std::string debug_translate_live(SrcKey sk,
                                 const std::vector<std::string>& types);

}}}
