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

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/code-view.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/tc-recycle.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/vm-protect.h"
#include "hphp/runtime/vm/jit/write-lease.h"
#include "hphp/util/growable-vector.h"

#include <memory>
#include <string>
#include <vector>

namespace HPHP::jit {

struct AsmInfo;
struct IncomingBranch;
struct IRUnit;
struct ProfTransRec;
struct TransEnv;
struct TransLoc;
struct Vunit;

using OptView = Optional<CodeCache::View>;

////////////////////////////////////////////////////////////////////////////////

namespace tc {

extern __thread bool tl_is_jitting;

struct TransRange {
  TcaRange main;
  TcaRange cold;
  TcaRange frozen;
  TcaRange data;

  TransLoc loc() const;
};

struct LocalTCBuffer {
  LocalTCBuffer() = default;
  explicit LocalTCBuffer(Address start, size_t initialSize);
  LocalTCBuffer(LocalTCBuffer&&) = default;
  LocalTCBuffer& operator=(LocalTCBuffer&&) = default;

  OptView view();
  bool valid() const { return m_main.base() != nullptr; }

private:
  CodeBlock m_main;
  CodeBlock m_cold;
  CodeBlock m_frozen;
  DataBlock m_data;
};

struct Translator {
  // Live translation is achieved through the following steps (things annotated
  // with a * are generic and do not need to be implemented by individual
  // translator types):
  //   0) Initialize the kind and check if we should translate.  This includes
  //      actions like verifying there isn't already a suitable translation
  //   *) Acquiring a suitable lock for translation
  //   *) Repeat step 0 now that we have the lock
  //   1) Generate machine code into code buffers (these may be thread local)
  //      this takes several steps, generally:
  //        - Generate IR (possibly selecting a region to translate first)
  //        - Optimize
  //        - Lower to vasm
  //        - Optimize
  //        - Emit
  //   *) Relocate the machine code into its final resting place (this is
  //      necessarily done sequentially as code space is bump allocated).  This
  //      step is a noop if it was emitted into the global code cache directly.
  //   3) Publish the translations to the owning stores (ie. future
  //      translation checks should find them)
  //   *) Lock released
  //
  // This is slightly different for an optimized translation which might get
  // emitted into local buffers and then relocated.  Optimized translation also
  // make use of some of the internal publishing methods so they can hold the
  // code and metadata locks across multiple operations.

  static constexpr size_t kTranslationAlign = 16;
  // The following members are the inputs to the translation pipeline.
  SrcKey sk;
  TransKind kind;
  // Thread ID whose code view to relocate into. Defaults to the current
  // thread; async JIT workers override this to the requesting thread's ID
  // so translations land in the requester's TC blocks.
  pthread_t viewTid{pthread_self()};
  // The TransID for this translation.  This is always set for Profile and
  // ProfPrologue translations, and holds their ID in ProfData.  For other
  // translations, this is only valid if the TransDB is enabled, and it's their
  // ID in the TransDB.
  TransID transId{kInvalidTransID};
  // For OptPrologues, this holds the corresponding ProfPrologue's ID.  This is
  // used to smash the callers of the ProfPrologue.
  TransID proflogueTransId{kInvalidTransID};
  Annotations annotations{};

  explicit Translator(SrcKey sk, TransKind kind = TransKind::Invalid);
  virtual ~Translator();

  virtual Optional<TranslationResult> getCached() = 0;
  virtual void resetCached() = 0;
  virtual void setCachedForProcessFail() = 0;
  virtual void smashBackup() = 0;

  // Returns a TCA for already translated code if found, this can be nullptr if
  // the desired behavior is to trigger use of non jited code.  If none is
  // returned the locks for translation were successfully acquired.
  Optional<TranslationResult> acquireLeaseAndRequisitePaperwork();
  // Check on tc sizes and make sure we are looking to translate more
  // translations of the specified type.
  TranslationResult::Scope shouldTranslate(bool noThreshold, bool noSizeLimit);
  // Returns false if the kind is Live and there are retranslateAll or profile
  // translations are in flight.
  // Returns true otherwise, including any other TransKind.
  bool shouldEmitLiveTranslation();
  // Generate and emit machine code into the provided view (if given) otherwise
  // the default view.
  Optional<TranslationResult>
  translate(Optional<CodeCache::View> view = std::nullopt);

  bool translateSuccess() const;

  // Relocate the generated machine code to its final location.  This may be a
  // no-op if it was initially emitted into the correct location.
  Optional<TranslationResult> relocate(bool alignMain);

  // Bind the outgoing edges either directly to already existing translations,
  // or to a service request stub requesting a translation.
  Optional<TranslationResult> bindOutgoingEdges();

  // Publish the translation starts, ends etc. into the required metadata
  // structures.  This includes publishing them as debug info, but also caching
  // the translation start in a manner that would be detected in
  // acquireLeaseAndRequisitePaperwork.  When publishing finishes the locks may
  // be released, and if the translation isn't properly recorded in the SrcKey
  // database (or other equivalent structure) we may end up with duplicate
  // translations.
  TranslationResult publish();
  void publishMetaInternal();
  void publishCodeInternal();
  TCA entry() const {
    if (!transMeta) return nullptr;
    assertx(!transMeta->view.isLocal());
    return transMeta->range.loc().entry();
  }
  TransRange range() const {
    if (!transMeta) return TransRange{};
    return transMeta->range;
  }
  CGMeta& meta() {
    assertx(transMeta.has_value());
    return transMeta->fixups;
  }
  void reset() {
    transMeta.reset();
  }

  Annotations* getAnnotations() {
    return &annotations;
  }

  // The gen method is responsible for building the vunit.  It may
  // optionally set the IR unit, which was used for generation.
  // This will enable the printir functionality during vasm emit,
  // and relocation phases.
  std::unique_ptr<IRUnit> unit;
  std::unique_ptr<Vunit> vunit;

protected:
  Optional<LeaseHolder> m_lease{};

  struct TransMeta {
    explicit TransMeta(CodeCache::View view)
      : view(view)
    {}
    // Relocation and publication metadata.
    // This info is generated from stage 1 (generation of machine code), and
    // contains the info relevant to stages 2 and 3 (relocation and
    // publication).  It holds TC address ranges, CGMeta fixup information, and
    // anything else needed.

    // The translation range holds the present location of the translation.  It
    // is initially set during translation, and is updated during relocation.
    // The same applies to the view.  It is set during translation, and updated
    // during relocation.
    TransRange range;
    CodeCache::View view;
    // The fixups are generated during translation, and used during relocation.
    CGMeta fixups;
  };

  // The translation metadata is written during translation.  It will be none
  // until successful translation.  This metadata is the output of the
  // translation pipeline.  Publishing uses this info to write start addresses
  // and ranges to make the code executable.
  Optional<TransMeta> transMeta{};

  virtual void computeKind() = 0;
  // This function is charged with producing the code to generate.
  virtual void gen() = 0;
  virtual void publishMetaImpl() = 0;
  virtual void publishCodeImpl() = 0;

private:
  // This local buffer is only used in ReusableTC mode where the buffer is
  // owned by the translator rather than the FuncMetaInfo.
  std::unique_ptr<LocalTCBuffer> m_localTCBuffer;
  std::unique_ptr<uint8_t[]> m_localBuffer;
};

////////////////////////////////////////////////////////////////////////////////

using CodeViewPtr = std::unique_ptr<CodeCache::View>;

struct FuncMetaInfo {
  enum class Kind : uint8_t {
    Prologue,
    Translation,
  };

  FuncMetaInfo() = default;
  FuncMetaInfo(Func* f, LocalTCBuffer&& buf)
    : funcToken(f->getFuncToken())
    , tcBuf(std::move(buf))
  {}

  FuncMetaInfo(FuncMetaInfo&&) = default;
  FuncMetaInfo& operator=(FuncMetaInfo&&) = default;

  std::shared_ptr<FuncToken> funcToken;
  LocalTCBuffer tcBuf;

  // Set once a retranslateAll worker is done with this job, whether or not it
  // optimized the function. Written under the mcgen worker condvar mutex and
  // accessed through std::atomic_ref so waitForTranslate() can check it
  // without taking the mutex.
  bool jobDone{false};

  void add(std::unique_ptr<Translator>&& p) {
    translators.emplace_back(std::move(p));
  }

  void clear() {
    translators.clear();
  }

  // For now these are prolgoue translators.
  std::vector<std::unique_ptr<Translator>> translators;
};

////////////////////////////////////////////////////////////////////////////////

/*
 * Publish a set of optimized translations associated with a particular
 * function. It is assumed that these translations have been emitted into per-
 * thread buffers and will need to be relocated.
 */
void publishOptFunc(const Func* func, FuncMetaInfo info);

/*
 * Acquires the code and metadata locks once, and then processes all the
 * functions in `infos' by:
 *  1) relocating their translations into the TC in the order given by `infos';
 *  2) smashing all the calls and jumps between these translations;
 *  3) optimizing the calls and jumps smashed in step 2);
 *  4) publishing these translations.
 */
void relocatePublishSortedOptFuncs(std::vector<FuncMetaInfo> infos);

////////////////////////////////////////////////////////////////////////////////

/*
 * True iff the global translation limit has not yet been reached.
 */
bool canTranslate();

/*
 * Whether we should emit a translation of kind for sk, ignoring the cap on
 * overall TC size.
 */
TranslationResult::Scope shouldTranslateNoSizeLimit(SrcKey sk, TransKind kind,
                                                    bool noThreshold = false);

/*
 * Whether we should emit a translation of kind for sk.
 */
TranslationResult::Scope shouldTranslate(SrcKey sk, TransKind kind,
                                         bool noThreshold = false);

/*
 * Whether we are still profiling new functions.
 */
bool shouldProfileNewFuncs();

/*
 * Whether we should try profile-guided optimization when translating `func'.
 */
bool profileFunc(const Func* func);

/*
 * Attempt to discard profData via the treadmill if it is no longer needed.
 */
void checkFreeProfData();

////////////////////////////////////////////////////////////////////////////////

/*
 * SrcRec for sk or nullptr if no SrcRec has been created.
 */
SrcRec* findSrcRec(SrcKey sk);

/*
 * Create a SrcRec for sk with an sp offset of spOff if it doesn't exist and
 * return it. If there's not enough TC space for any stubs, return nullptr.
 */
SrcRec* createSrcRec(SrcKey sk, SBInvOffset spOff);

////////////////////////////////////////////////////////////////////////////////

/*
 * Assert ownership of the CodeCache by this thread.
 *
 * Must be held even if the current thread owns the global write lease.
 */
void assertOwnsCodeLock(OptView v = std::nullopt);

/*
 * Assert ownership of the tc metadata by this thread.
 *
 * Must be held even if the current thread owns the global write lease.
 */
void assertOwnsMetadataLock();

////////////////////////////////////////////////////////////////////////////////

/*
 * Get the table of unique stubs
 *
 * Pre: processInit()
 */
ALWAYS_INLINE const UniqueStubs& ustubs() {
  extern UniqueStubs g_ustubs;
  return g_ustubs;
}

/*
 * Perform one time process initialization for the structures associated with
 * this module.
 */
void processInit();

/*
 * Perform process shutdown functions for the TC including joining any
 * outstanding background threads.
 */
void processExit();

////////////////////////////////////////////////////////////////////////////////

/*
 * Perform TC related request initialization and teardown.
 */
void requestInit();
void requestExit();

/*
 * Returns the total size of the TC now and at the beginning of this request,
 * in bytes. Note that the code may have been emitted by other threads.
 *
 * Pre: requestInit()
 */
void codeEmittedThisRequest(size_t& requestEntry, size_t& now);

////////////////////////////////////////////////////////////////////////////////

/*
 * Reclaim all TC space associated with func
 *
 * Before any space is reclaimed the following actions will be performed:
 *  (1) Smash all prologues
 *  (2) Smash all callers to bind-call unique stubs
 *  (3) Erase all call meta-data for calls into function
 *
 * After all calls and prologues have been smashed any on-going requests will be
 * allowed to complete before TC Space will be reclaimed for:
 *  (1) All prologues
 *  (2) All translations
 *  (3) All anchor translations
 *
 * This function should only be called from Func::destroy() and may access the
 * fullname and ID of the function.
 */
void reclaimFunction(const StringData* name, RecycleInfo recycleInfo);

void cleanupSrcDBKeys(const std::vector<SrcKey>& srcKeys);

/*
 * Allows TC space for translations in trans to be reused in future
 * translations.
 *
 * Reclaiming a translation will:
 *  (1) Mark bytes available for reuse in the code-blocks associated with
 *      the translation
 *  (2) Erase any IBs from translation into other SrcRecs
 *  (3) Erase any jump annotations in MCGenerator used to generate optimized
 *      traces
 *  (4) Erase an metadata about smashed calls in the translation from both the
 *      reuse-tc module and the prof-data module
 *
 * The translation _must_ be unreachable before reclaimTranslation() is called,
 * this is generally done by calling reclaimFunction() or performing
 * replaceOldTranslations() on a SrcRec
 */
void reclaimTranslations(GrowableVector<TransLoc>&& trans);

////////////////////////////////////////////////////////////////////////////////

struct UsageInfo {
  std::string name;
  size_t used;
  size_t capacity;
  bool global;
};

/*
 * Get UsageInfo data for all the TC code sections, including global data, and
 * also for RDS.
 */
std::vector<UsageInfo> getUsageInfo();

/*
 * Like getUsageInfo(), but formatted as a pleasant string.
 */
std::string getTCSpace();

/*
 * Return RDS usage breakdown as a JSON string, categorized by symbol type.
 */
std::string getRDSUsageJSON();

/*
 * Return a string containing the names and start addresses of all TC code
 * sections.
 */
std::string getTCAddrs();

/*
 * Return whether or not TC dumping is enabled.
 */
bool dumpEnabled();

/*
 * Dump the translation cache to files in Cfg::Eval::DumpTCPath
 * (default: /tmp), returning success.
 */
bool dump(bool ignoreLease = false);

struct TCMemInfo {
  std::string name;
  size_t used;
  size_t allocs;
  size_t frees;
  size_t free_size;
  size_t free_blocks;
};

/*
 * Get per section memory usage data for the TC.
 */
std::vector<TCMemInfo> getTCMemoryUsage();

////////////////////////////////////////////////////////////////////////////////

/*
 * Convert between TC addresses and offsets.
 */
extern CodeCache* g_code;
ALWAYS_INLINE TCA offsetToAddr(uint32_t off) {
  return g_code->toAddr(off);
}
ALWAYS_INLINE uint32_t addrToOffset(CTCA addr) {
  return g_code->toOffset(addr);
}

/*
 * Check if `addr' is an address within the TC.
 */
bool isValidCodeAddress(TCA addr);

////////////////////////////////////////////////////////////////////////////////

/*
 * Relocate a new translation to the current frontiers of main and cold. Code
 * in frozen is not moved.
 *
 * TODO(t10543562): This can probably be merged with relocateNewTranslation.
 */
void relocateTranslation(
  const IRUnit* unit,
  CodeBlock& main, CodeBlock& main_in, CodeAddress main_start,
  CodeBlock& cold, CodeBlock& cold_in, CodeAddress cold_start,
  CodeBlock& frozen, CodeAddress frozen_start,
  AsmInfo* ai, CGMeta& meta
);

////////////////////////////////////////////////////////////////////////////////

/*
 * Information about the number of bound calls, branches, and tracked functions
 * for use in logging.
 */
int smashedCalls();
int smashedBranches();

/*
 * Record a jmp at address toSmash to SrcRec sr.
 *
 * When a translation is reclaimed we remove all annotations from all SrcRecs
 * containing IBs from the translation so that they cannot be inadvertently
 * smashed in the process of replaceOldTranslations()
 */
void recordJump(TCA toSmash, SrcRec* sr);

/*
 * Bind a call to start at toSmash, where start is the prologue for callee, when
 * invoked with nArgs.
 */
void bindCall(TCA toSmash, TCA start, Func* callee, int nArgs);

}}
