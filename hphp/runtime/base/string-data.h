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
#include <folly/Range.h>

#include "hphp/util/alloc.h"
#include "hphp/util/bstring.h"
#include "hphp/util/hash.h"
#include "hphp/util/word-mem.h"

#include "hphp/runtime/base/countable.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/static-literals.h"
#include "hphp/runtime/base/string-data-macros.h"
#include "hphp/util/assertions.h"
#include "hphp/util/blob-encoder.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/ptr-impl.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct APCString;
struct Array;
struct OptString;
struct APCHandle;
struct NamedType;
struct UnitEmitter;
struct Unit;

//////////////////////////////////////////////////////////////////////

// Copy the passed-in string and free the buffer immediately.
enum AttachStringMode { AttachString };

// const char* points to client-owned memory, StringData will copy it
// at construct-time using req::malloc.  This is only ok when the StringData
// itself was request-allocated.
enum CopyStringMode { CopyString };

/*
 * Runtime representation of PHP strings.
 *
 * All StringData have the same layout: a 16-byte header containing a
 * HeapObject header, a length, and a hash, followed by a co-allocated,
 * \0-terminated array of characters. (Note that \0 can be a character
 * within the array, as well.)
 *
 * StringDatas can also be allocated in multiple ways.  Normally, they
 * are created through one of the Make overloads, which drops them in
 * the request-local heap.  They can also be low-malloced (for static
 * strings), or uncounted-malloced for APC shared or uncounted strings.
 */
struct StringData final : MaybeCountable,
                          type_scan::MarkCollectable<StringData> {
  friend struct APCString;
  friend StringData* allocFlat(size_t len);

  /*
   * Max length of a string, not counting the terminal 0.
   *
   * This is smaller than MAX_INT, and it plus StringData overhead should
   * exactly equal a size class.
   */
  static constexpr uint32_t MaxSize = 0x80000000U - 16 - 1;

  /*
   * Creates an empty request-local string with an unspecified amount of
   * reserved space. Ref-count is pre-initialized to 1.
   */
  static StringData* Make();

  /*
   * Constructors that copy the string memory into this StringData, for
   * request-local strings. Ref-count is pre-initialized to 1.
   *
   * Most strings are created this way.
   */
  static StringData* Make(folly::StringPiece);

  static StringData* Make(const char* data, CopyStringMode);
  static StringData* Make(const char* data, size_t len, CopyStringMode);
  static StringData* Make(const StringData* s, CopyStringMode);
  static StringData* Make(folly::StringPiece r1, CopyStringMode);

  /*
   * Attach constructors for request-local strings.
   *
   * These do the same thing as the above CopyStringMode constructors, except
   * that it will also free `data'. Ref-count is pre-initialized to 1.
   */
  static StringData* Make(char* data, AttachStringMode);
  static StringData* Make(char* data, size_t len, AttachStringMode);

  /*
   * Create a new request-local string by concatenating two existing
   * strings. Ref-count is pre-initialized to 1.
   */
  static StringData* Make(const StringData* s1, const StringData* s2);
  static StringData* Make(const StringData* s1, folly::StringPiece s2);
  static StringData* Make(const StringData* s1, const char* lit2);
  static StringData* Make(folly::StringPiece s1, const char* lit2);
  static StringData* Make(folly::StringPiece s1, folly::StringPiece s2);
  static StringData* Make(folly::StringPiece s1, folly::StringPiece s2,
                          folly::StringPiece s3);
  static StringData* Make(folly::StringPiece s1, folly::StringPiece s2,
                          folly::StringPiece s3, folly::StringPiece s4);

  /*
   * Create a new request-local empty string big enough to hold strings of
   * length `reserve' (not counting the \0 terminator). Ref-count is
   * pre-initialized to 1.
   */
  static StringData* Make(size_t reserve);

  /*
   * Allocate a string with malloc, using the low-memory allocator if
   * jemalloc is available, and setting it as a static string.
   *
   * This api is only for the static-string-table.cpp.  The returned
   * StringData is not expected to be reference counted, and must be
   * deallocated using destructStatic.
   */
  static StringData* MakeStatic(folly::StringPiece);

  /*
   * Same as MakeStatic but the string allocated will *not* be in the static
   * string table, will not be in low-memory, and should be deleted using
   * ReleaseShared once the root goes out of scope.
   */
  static StringData* MakeShared(folly::StringPiece);

  /*
   * Same as MakeStatic, but initializes the empty string in reserved storage.
   * This should only be called by the static string table initialization code.
   */
  static StringData* InitializeEmpty();

  /*
   * return estimated capacity for a string of the given size, due to
   * size-class rounding.
   */
  static size_t estimateCap(size_t size);

  /*
   * Offset accessors for the JIT compiler.
   */
  static constexpr ptrdiff_t sizeOff() { return offsetof(StringData, m_len); }
  static constexpr ptrdiff_t hashOff() { return offsetof(StringData, m_hash); }

  /*
   * Called to return a StringData to the request allocator.  This is
   * normally called when the reference count goes to zero (e.g. with
   * a helper like decRefStr).
   */
  void release() noexcept;
  size_t heapSize() const;

  /*
   * StringData objects allocated with MakeStatic should be freed
   * using this function.
   */
  void destructStatic();

  /*
   * StringData objects allocated with MakeShared should be freed
   * using this function. It will remove a reference via
   * sharedDecRef, and if necessary destroy the StringData and
   * return true.
   */
  static void ReleaseShared(StringData*);

  /*
   * Reference-counting related.
   */
  ALWAYS_INLINE void decRefAndRelease() {
    assertx(kindIsValid());
    if (decReleaseCheck()) release();
  }

  bool kindIsValid() const { return m_kind == HeaderKind::String; }

  /*
   * Append the supplied range to this string.  If there is not sufficient
   * capacity in this string to contain the range, a new string may be
   * returned. The new string's reference count will be pre-initialized to 1.
   *
   * Pre: !hasMultipleRefs()
   * Pre: the string is request-local
   */
  StringData* append(folly::StringPiece r);
  StringData* append(folly::StringPiece r1, folly::StringPiece r2);
  StringData* append(folly::StringPiece r1,
                     folly::StringPiece r2,
                     folly::StringPiece r3);

  /*
   * Reserve space for a string of length `maxLen' (not counting null
   * terminator).
   *
   * May not be called for strings created with MakeShared or
   * MakeStatic.
   *
   * Returns: possibly a new StringData, if we had to reallocate.  The new
   * string's reference count will be pre-initialized to 1.
   */
  StringData* reserve(size_t maxLen);

  /*
   * Shrink a string down to length `len` (not counting null terminator).
   *
   * May not be called for strings created with MakeShared or
   * MakeStatic.
   *
   * Returns: a new StringData with reference count 1
   */
  StringData* shrinkImpl(size_t len);

  /*
   * Returns a slice with extents sized to the *string* that this
   * StringData wraps.  This range does not include a null terminator.
   *
   * Note: please do not add new code that assumes the range does
   * include a null-terminator if possible.  (We would like to make
   * this unnecessary eventually.)
   */
  folly::StringPiece slice() const;

  /*
   * Returns a mutable slice with extents sized to the *buffer* this
   * StringData wraps, not the string, minus space for an implicit
   * null terminator.
   *
   * Note: please do not introduce new uses of this API that write
   * nulls 1 byte past slice.len---we want to weed those out.
   */
  folly::MutableStringPiece bufferSlice();

  /*
   * If external users of this object want to modify it (e.g. through
   * bufferSlice or mutableData()), they are responsible for either
   * calling setSize() if the mutation changed the size of the string,
   * or invalidateHash() if not.
   *
   * Pre: !hasMultipleRefs()
   */
  void invalidateHash();
  void setSize(int64_t len);

  /*
   * StringData should not generally be allocated on the stack,
   * because references to it could escape.  This function is for
   * debugging: it asserts that the address of this doesn't point into
   * the C++ stack.
   */
  void checkStack() const;

  /*
   * Access to the string's data as a null-terminated character array.
   *
   * Please try to prefer slice() in new code.
   *
   * The following extensions depend on the null terminator for correctness,
   * and the lack of implicit copying (not perfect for Strings, but
   * best-effort):
   *
   * - libsodium
   * - mcrypt
   * - openssl
   */
  const char* data() const;

  /*
   * Mutable version of data().
   */
  char* mutableData() const;

  /*
   * Accessor for the length of a string.
   *
   * Note: size() is guaranteed to be >= 0 and <= MaxSize.
   */
  int64_t size() const;

  /*
   * Returns: size() == 0
   */
  bool empty() const;

  /*
   * Return the capacity of this string's buffer, not including the space
   * for the null terminator. Always 0 for static/uncounted strings.
   */
  uint32_t capacity() const;

  /*
   * Simultaneously query whether this string is numeric, and pull out
   * the numeric value of the string (as either an int or a double).
   *
   * The allow_errors flag is a boolean that does something currently
   * undocumented.
   *
   * If overflow is set its value is initialized to either zero to
   * indicate that no overflow occurred or 1/-1 to indicate the direction
   * of overflow.
   *
   * Returns: KindOfNull, KindOfInt64 or KindOfDouble.  The int64_t or
   * double out reference params are populated in the latter two cases
   * with the numeric value of the string.  The KindOfNull case
   * indicates the string is not numeric.
   */
  DataType isNumericWithVal(int64_t&, double&, int allowErrors,
                            int* overflow = nullptr) const;

  /*
   * Returns true if this string is numeric.
   *
   * In effect: isNumericWithVal(i, d, false) != KindOfNull
   */
  bool isNumeric() const;

  /*
   * Returns true if this string is "strictly" an integer in the sense
   * of is_strictly_integer from util/hash.h, and if so provides the
   * integer value in res.
   */
  bool isStrictlyInteger(int64_t& res) const;

  /*
   * Returns whether this string contains a single character '0'.
   */
  bool isZero() const;

  /*
   * Change the character at offset `offset' to `c'.
   *
   * May return a reallocated StringData* if this string was a shared
   * string. The new string's reference count is pre-initialized to 1.
   *
   * Pre: offset >= 0 && offset < size()
   *      !hasMultipleRefs()
   *      string must be request local
   */
  StringData* modifyChar(int offset, char c);

  /*
   * Return a string containing the character at `offset', if it is in
   * range.  Otherwise raises a warning and returns an empty string.
   *
   * All return values are guaranteed to be static strings.
   */
  StringData* getChar(int offset) const;

  /*
   * Increment this string in the manner of php's ++ operator.  May return a new
   * string if it had to resize. The new string's reference count is
   * pre-initialized to 1.
   *
   * Pre: !isStatic() && !isEmpty()
   *      string must be request local
   */
  StringData* increment();

  /*
   * We identify certain strings as "symbols", which store a small cache.
   * This concept is just an optimization; nothing is required to be a symbol.
   *
   * After loading a majority of symbols, call StringData::markSymbolsLoaded
   * to avoid allocating these extra caches on any more static strings.
   */
  bool isSymbol() const;

  /*
   * A static string may be assigned a "color" which to be used as the hash key
   * in implementations of perfect hashing for bespoke arrays. The color is
   * present only in static arrays, and is stored in  the lower 14 bits of
   * m_aux16.
   */
  uint16_t color() const;
  void setColor(uint16_t color);

  /*
   * Get or set the cached class or named type. Get will return nullptr
   * if the corresponding cached value hasn't been set yet.
   *
   * Pre: isSymbol()
   */
  Class* getCachedClass() const;
  NamedType* getNamedType() const;
  void setCachedClass(Class* cls);
  void setNamedType(NamedType* ne);

  /*
   * Helpers used to JIT access to the symbol cache.
   */
  constexpr static uint8_t kIsSymbolMask = 0x80;
  static ptrdiff_t isSymbolOffset();
  static ptrdiff_t cachedClassOffset();

  /*
   * Helpers used to JIT access to the color field.
   */
  constexpr static uint16_t kColorMask = 0x3fff;
  constexpr static uint16_t kInvalidColor = 0x0000;
  constexpr static uint16_t kDupColor = 0x0001;
  static ptrdiff_t colorOffset();

  /*
   * Type conversion functions.
   */
  bool toBoolean() const;
  int64_t toInt64(int base = 10) const;
  double toDouble() const;
  DataType toNumeric(int64_t& lval, double& dval) const;
  std::string toCppString() const;

  /*
   * Returns: case insensitive hash value for this string.
   * hashStatic() requires isStatic() as a precondition.
   */
  strhash_t hash() const;
  strhash_t hashStatic() const;
  static strhash_t hash(const char* s, size_t len);
  static strhash_t hash_unsafe(const char* s, size_t len);

  /*
   * Equality comparison, in the sense of php's string == operator.
   * (I.e. numeric strings are compared numerically.)
   */
  bool equal(const StringData* s) const;

  /*
   * Exact comparison, in the sense of php's string === operator.
   * (Exact, case-sensitive comparison.)
   */
  bool same(const StringData* s) const;

  /*
   * Case-insensitive exact string comparison.  (Numeric strings are
   * not treated specially.)
   * DEPRECATED: use same_nocase() for case-insensitive strings that
   * are not language symbols.
   */
  bool tsame(const StringData* s) const;
  bool fsame(const StringData* s) const;

  /*
   * Case-insensitive exact string comparison.  (Numeric strings are
   * not treated specially.)
   */
  bool same_nocase(const StringData* s) const;

  /*
   * Implements comparison in the sense of php's operator < on
   * strings.  (I.e. this compares numeric strings numerically, and
   * other strings lexicographically.)
   *
   * Returns: a number less than zero if *this is less than *v2,
   * greater than zero if *this is greater than *v2, or zero if
   * this->equal(v2).
   */
  int compare(const StringData* v2) const;

  /*
   * Create a sub-string from start with specified length.
   *
   * If the start is outside the bounds of the string, or the length is
   * negative, the empty string is returned.  The range [start, start+length]
   * gets clamped to [start, size()].
   */
  StringData* substr(int start, int length = StringData::MaxSize);

  /*
   * Debug dumping of a StringData to stdout.
   */
  void dump() const;

  bool checkSane() const;

private:
  template<bool trueStatic>
  static MemBlock AllocatePersistent(folly::StringPiece sl);
  template<bool trueStatic>
  static StringData* MakePersistent(folly::StringPiece sl);
  template<bool trueStatic>
  static StringData* MakePersistentAt(folly::StringPiece sl, MemBlock range);

  /*
   * Initialize a static string on a pre-allocated range of memory. This is
   * useful when we need to create static strings at designated addresses when
   * optimizing locality.
   */
  static StringData* MakeStaticAt(folly::StringPiece, MemBlock);

  StringData(const StringData&) = delete;
  StringData& operator=(const StringData&) = delete;
  ~StringData() = delete;

private:
  int numericCompare(const StringData *v2) const;
  void incrementHelper();
  void preCompute();

  // We have the next fields blocked into qword-size unions so
  // StringData initialization can do fewer stores to initialize the
  // fields.  (gcc does not combine the stores itself.)
private:
  union {
    struct {
      uint32_t m_len;
      mutable int32_t m_hash;           // precomputed for persistent strings
    };
    uint64_t m_lenAndHash;
  };
};

/*
 * Some static StringData has a SymbolPrefix allocated right in front.
 */
struct SymbolPrefix {
  AtomicPackedPtr<NamedType> nty;
  AtomicPackedPtr<Class> cls;
};

static_assert(sizeof(SymbolPrefix) % alignof(StringData) == 0, "");

//////////////////////////////////////////////////////////////////////

/*
 * The allocation overhead of a StringData: the struct plus the null byte
 */
auto constexpr kStringOverhead = sizeof(StringData) + 1;
static_assert(StringData::MaxSize + kStringOverhead == kSizeIndex2Size[103],
              "max allocation size is a valid size class");

/*
 * A reasonable length to reserve for small strings.  This is also the
 * default reserve size for StringData::Make().
 */
constexpr uint32_t SmallStringReserve = 64 - kStringOverhead;

/* this only exists so that clang won't warn on the subtraction */
inline constexpr uint32_t sizeClassParams2StringCapacity(
  size_t lg_grp,
  size_t lg_delta,
  size_t ndelta
) {
  return ((size_t{1} << lg_grp) + (ndelta << lg_delta)) > kStringOverhead
      && ((size_t{1} << lg_grp) + (ndelta << lg_delta))
        <= StringData::MaxSize + kStringOverhead
    ? ((size_t{1} << lg_grp) + (ndelta << lg_delta)) - kStringOverhead
    : 0;
}

alignas(64) constexpr uint32_t kSizeIndex2StringCapacity[] = {
#define SIZE_CLASS(index, lg_grp, lg_delta, ndelta, lg_delta_lookup, ncontig) \
  sizeClassParams2StringCapacity(lg_grp, lg_delta, ndelta),
  SIZE_CLASSES
#undef SIZE_CLASS
};

/*
 * Call this if we tried to make a string longer than StringData::MaxSize
 */
void raiseStringLengthExceededError(size_t len);

/*
 * DecRef a string s, calling release if its reference count goes to
 * zero.
 */
void decRefStr(StringData* s);

//////////////////////////////////////////////////////////////////////

/*
 * Function objects that forward to the StringData member functions of
 * the same name.
 */
struct string_data_hash;
struct string_data_same;
struct string_data_tsame; // for type names
struct string_data_fsame; // for func names
struct string_data_lt;
struct string_data_lt_type; // for type names
struct string_data_hash_tsame; // for type names

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE StringData* staticEmptyString() {
  auto constexpr ptr = StaticLiterals::EmptyString() + sizeof(SymbolPrefix);
  return reinterpret_cast<StringData*>(ptr);
}

//////////////////////////////////////////////////////////////////////

template<>
struct BlobEncoderHelper<const StringData*> {
  static void serde(BlobEncoder&, const StringData*);
  static void serde(BlobDecoder&, const StringData*&,
                    bool makeStatic = true);

  static folly::StringPiece asStringPiece(BlobDecoder&);

  static void skip(BlobDecoder&);
  static size_t peekSize(BlobDecoder&);

  // Encode each string once. If a string occurs more than once, use a
  // small integer to refer to it. Faster and smaller if you have a
  // lot of duplicate strings.
  struct Indexer {
    hphp_fast_map<const StringData*, uint32_t> m_indices;
    std::vector<const StringData*> m_strs;
  };

  // If set, will utilize the UnitEmitter's string table.
  static __thread UnitEmitter* tl_unitEmitter;
  // Likewise, but only for lazy loading (so only deserializing
  // supported).
  static __thread Unit* tl_unit;
  // If set, use that Indexer to encode strings.
  static __thread Indexer* tl_indexer;
};

// Use an Indexer for string serialization (if one isn't already being
// used) while this is in scope.
struct ScopedStringDataIndexer {
  ScopedStringDataIndexer() {
    if (!BlobEncoderHelper<const StringData*>::tl_indexer) {
      BlobEncoderHelper<const StringData*>::tl_indexer = &m_indexer;
      m_used = true;
    }
  }
  ~ScopedStringDataIndexer() {
    if (m_used) {
      assertx(BlobEncoderHelper<const StringData*>::tl_indexer == &m_indexer);
      BlobEncoderHelper<const StringData*>::tl_indexer = nullptr;
    }
  }
  ScopedStringDataIndexer(const ScopedStringDataIndexer&) = delete;
  ScopedStringDataIndexer(ScopedStringDataIndexer&&) = delete;
  ScopedStringDataIndexer& operator=(const ScopedStringDataIndexer&) = delete;
  ScopedStringDataIndexer& operator=(ScopedStringDataIndexer&&) = delete;
private:
  BlobEncoderHelper<const StringData*>::Indexer m_indexer;
  bool m_used{false};
};

//////////////////////////////////////////////////////////////////////

}

namespace folly {
template<> class FormatValue<const HPHP::StringData*> {
 public:
  explicit FormatValue(const HPHP::StringData* str) : m_val(str) {}

  template<typename Callback>
  void format(FormatArg& arg, Callback& cb) const {
    auto piece = folly::StringPiece(m_val->data(), m_val->size());
    format_value::formatString(piece, arg, cb);
  }

 private:
  const HPHP::StringData* m_val;
};

template<> class FormatValue<HPHP::StringData*> {
 public:
  explicit FormatValue(const HPHP::StringData* str) : m_val(str) {}

  template<typename Callback>
  void format(FormatArg& arg, Callback& cb) const {
    FormatValue<const HPHP::StringData*>(m_val).format(arg, cb);
  }

 private:
  const HPHP::StringData* m_val;
};
}

// Formats the string contents, like folly::FormatValue<StringData*> above.
template <typename S, typename P>
struct fmt::formatter<HPHP::ptrimpl::PtrImpl<const HPHP::StringData, S, P>>
    : fmt::formatter<std::string_view> {
  auto format(
      const HPHP::ptrimpl::PtrImpl<const HPHP::StringData, S, P>& v,
      fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        std::string_view(v->data(), v->size()), ctx);
  }
};

template <typename S, typename P>
struct fmt::formatter<HPHP::ptrimpl::PtrImpl<HPHP::StringData, S, P>>
    : fmt::formatter<std::string_view> {
  auto format(
      const HPHP::ptrimpl::PtrImpl<HPHP::StringData, S, P>& v,
      fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        std::string_view(v->data(), v->size()), ctx);
  }
};

#include "hphp/runtime/base/string-data-inl.h"
