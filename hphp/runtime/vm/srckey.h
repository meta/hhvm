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

#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/resumable.h"

#include <boost/operators.hpp>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

namespace arrprov { struct Tag; }

struct Func;
struct Unit;

///////////////////////////////////////////////////////////////////////////////

/*
 * A SrcKey is a logical source instruction---it's enough to identify
 * these using a (Func id, hhbc instruction) pair.
 */
struct SrcKey {
  static_assert(sizeof(PackedPtr<const Func>) == sizeof(uint32_t), "");
  static_assert(sizeof(Offset) == sizeof(uint32_t), "");

  /////////////////////////////////////////////////////////////////////////////
  // Types.

  using AtomicInt = uint64_t;

  struct Hasher;
  struct StableHasher;
  struct TbbHashCompare;

  using Set = hphp_hash_set<SrcKey, SrcKey::Hasher>;

  /*
   * Used for SrcKeys corresponding to the prologue which precedes a function
   * entry source location.
   *
   * Used disjointly from the `resumeMode'.
   */
  enum class PrologueTag {};

  /*
   * Used for SrcKeys corresponding to the function entry location. Either
   * points to a default value funclet for positional params, or to the main
   * func entry which precedes the function body.
   *
   * Used disjointly from the `resumeMode'.
   */
  enum class FuncEntryTag {};

  /*
   * Used for SrcKeys corresponding to the (single) function entry location
   * which populates all optional named params.
   *
   * Used disjointly from the `resumeMode'.
   */
  enum class NamedParamsFuncEntryTag {};
  /////////////////////////////////////////////////////////////////////////////

  /*
   * Invalid SrcKey constructor.
   */
  SrcKey();

  SrcKey(const Func* f, Offset off, ResumeMode resumeMode);
  SrcKey(const Func* f, PC pc, ResumeMode resumeMode);

  SrcKey(const Func* f, uint32_t numArgs, PrologueTag);
  SrcKey(const Func* f, uint32_t numArgs, bool mayHaveUninitNamed, FuncEntryTag);

  SrcKey(SrcKey other, Offset off);

  explicit SrcKey(AtomicInt in);
  static SrcKey fromAtomicInt(AtomicInt in);

  /////////////////////////////////////////////////////////////////////////////

  /*
   * Whether this is the sentinel value of invalid SrcKey.
   */
  bool valid() const;

  /*
   * Packed representation of SrcKeys for use in contexts (e.g. the SrcDB)
   * where we want atomicity.
   */
  AtomicInt toAtomicInt() const;

  /*
   * Stable hash.
   */
  size_t stableHash() const;

  /*
   * Direct accessors.
   */
  // Offset of the bytecode represented by this SrcKey.
  // Valid only when !prologue() && !funcEntry() && !namedParamsFuncEntry().
  Offset offset() const;

  // Offset of the bytecode that will be used to enter the function.
  // Valid only when prologue() || funcEntry() || namedParamsFuncEntry().
  Offset entryOffset() const;

  // Number of positional arguments passed to the prologue or func entry.
  // Valid only when prologue() || funcEntry().
  uint32_t numEntryArgs() const;

  ResumeMode resumeMode() const;
  bool prologue() const;
  bool funcEntry() const;
  bool namedParamsFuncEntry() const;
  // Whether this SK is a func entry or a namedParamsFuncEntry.
  bool anyFuncEntry() const;
  bool hasThis() const;

  // Whether this is a trivial default value initializer func entry, i.e.
  // the default value is scalar and does not require a runtime type check.
  bool trivialDVFuncEntry() const;

  // Whether this is a non-trivial func entry, i.e. either it is the main func
  // entry, or a non-trivial DV func entry.
  bool nonTrivialFuncEntry() const;

  /*
   * Derived accessors.
   */
  const Func* func() const;
  const Unit* unit() const;
  Op op() const;
  PC pc() const;
  int lineNumber() const;
  const PackageInfo* packageInfo() const;

  // Human readable offset of one of the above. Gives a std::string instead of
  // an Offset, as this should be used only for debugging and tracing.
  std::string printableOffset() const;

  /////////////////////////////////////////////////////////////////////////////

  /*
   * Set this SrcKey's offset.
   *
   * Does not perform any bounds or consistency checks.
   */
  void setOffset(Offset o);

  /*
   * Get all possible Offsets for the next bytecode.
   */
  Set succSrcKeys() const;

  /*
   * Advance the SrcKey to the next instruction.
   *
   * If the SrcKey points to the last instruction in a function, this will
   * advance past the end of the function, and potentially contain an invalid
   * bytecode offset.
   */
  void advance(const Func* f = nullptr);

  /*
   * Return a SrcKey representing the next instruction, without mutating this
   * SrcKey.
   */
  SrcKey advanced(const Func* f = nullptr) const;

  SrcKey withFunc(const Func* func) const;

  /////////////////////////////////////////////////////////////////////////////

  /*
   * Comparisons.
   *
   * SrcKeys are ordered by their AtomicInt values.
   */
  bool operator==(const SrcKey& r) const;
  bool operator!=(const SrcKey& r) const;

  /*
   * Stringification.
   */
  std::string showInst() const;
  std::string getSymbol() const;

  /////////////////////////////////////////////////////////////////////////////

private:
  uint32_t encodeResumeMode(ResumeMode resumeMode);
  uint32_t encodePrologue();
  uint32_t encodeFuncEntry();
  uint32_t encodeNamedParamsFuncEntry();

  /////////////////////////////////////////////////////////////////////////////

  static constexpr size_t kNumModeBits = 3;
  static constexpr size_t kNumOffsetBits = 32 - kNumModeBits;

  union {
    AtomicInt m_atomicInt;
    struct {
      PackedPtr<const Func> m_func;
      uint32_t m_offsetOrNumArgs : kNumOffsetBits;
      uint32_t m_resumeModeAndTags : kNumModeBits;
    } m_s;
  };
};

///////////////////////////////////////////////////////////////////////////////

struct SrcKey::Hasher {
  size_t operator()(SrcKey sk) const {
    return hash_int64(sk.toAtomicInt());
  }
};

struct SrcKey::StableHasher {
  size_t operator()(SrcKey sk) const {
    return sk.stableHash();
  }
};

///////////////////////////////////////////////////////////////////////////////

struct SrcKey::TbbHashCompare {
  static size_t hash(const SrcKey& sk) { return hash_int64(sk.toAtomicInt()); }
  static bool equal(const SrcKey& a, const SrcKey& b) { return a == b; }
};

///////////////////////////////////////////////////////////////////////////////

std::string show(SrcKey sk);
std::string showShort(SrcKey sk);

void sktrace(SrcKey sk, ATTRIBUTE_PRINTF_STRING const char *fmt, ...)
  ATTRIBUTE_PRINTF(2,3);
#define SKTRACE(level, sk, ...) \
  ONTRACE(level, sktrace(sk, __VA_ARGS__))

///////////////////////////////////////////////////////////////////////////////

}

#include "hphp/runtime/vm/srckey-inl.h"
