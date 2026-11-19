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

#include "hphp/runtime/base/string-data.h"

#include <cmath>
#include <utility>

#include "hphp/runtime/base/tv-conv-throw.h"
#include "hphp/util/alloc.h"

#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/static-literals.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/base/zend-functions.h"

#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/zend/zend-strtod.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

NEVER_INLINE void raiseStringLengthExceededError(size_t len) {
  raise_error("String length exceeded: %zu > %u", len, StringData::MaxSize);
}

// Allocate, initialize `m_data' and HeapObject, but not `m_lenAndHash'.
ALWAYS_INLINE StringData* allocFlat(size_t len) {
  if (UNLIKELY(len > StringData::MaxSize)) {
    raiseStringLengthExceededError(len);
  }
  auto const sizeIndex = MemoryManager::size2Index(len + kStringOverhead);
  auto sd = static_cast<StringData*>(tl_heap->objMallocIndex(sizeIndex));
  // Refcount initialized to 1.
  sd->initHeader_16(HeaderKind::String, OneReference, sizeIndex);
  assertx(sd->capacity() >= len);
  return sd;
}

//////////////////////////////////////////////////////////////////////

namespace {

SymbolPrefix* getSymbolPrefix(StringData* sd) {
  assertx(sd->isSymbol());
  return reinterpret_cast<SymbolPrefix*>(sd) - 1;
}

const SymbolPrefix* getSymbolPrefix(const StringData* sd) {
  assertx(sd->isSymbol());
  return getSymbolPrefix(const_cast<StringData*>(sd));
}
}

bool StringData::isSymbol() const {
  return (m_aux16 >> 8) & kIsSymbolMask;
}

Class* StringData::getCachedClass() const {
  return getSymbolPrefix(this)->cls;
}

NamedType* StringData::getNamedType() const {
  return getSymbolPrefix(this)->nty;
}

void StringData::setCachedClass(Class* cls) {
  auto const prefix = getSymbolPrefix(this);
  assertx(IMPLIES(prefix->cls, prefix->cls == cls));
  prefix->cls = cls;
}

void StringData::setNamedType(NamedType* nty) {
  auto const prefix = getSymbolPrefix(this);
  assertx(IMPLIES(prefix->nty, prefix->nty == nty));
  prefix->nty = nty;
}

ptrdiff_t StringData::isSymbolOffset() {
  return offsetof(StringData, m_aux16) + 1;
}

ptrdiff_t StringData::cachedClassOffset() {
  return offsetof(SymbolPrefix, cls) - sizeof(SymbolPrefix);
}

//////////////////////////////////////////////////////////////////////

ptrdiff_t StringData::colorOffset() {
  return offsetof(StringData, m_aux16);
}

uint16_t StringData::color() const {
  return m_aux16 & kColorMask;
}

void StringData::setColor(uint16_t color) {
  assertx((color & ~kColorMask) == 0);
  m_aux16 |= color;
}

//////////////////////////////////////////////////////////////////////

// Create either a static or an uncounted string.
// Difference between static and uncounted is in the lifetime
// of the string. Static are alive for the lifetime of the process.
// Uncounted are not ref counted but will be deleted at some point.
template <bool trueStatic> ALWAYS_INLINE
MemBlock StringData::AllocatePersistent(folly::StringPiece sl) {
  if (UNLIKELY(sl.size() > StringData::MaxSize)) {
    raiseStringLengthExceededError(sl.size());
  }

  auto const extra = trueStatic ? sizeof(SymbolPrefix) : 0;
  auto const bytes = sl.size() + kStringOverhead + extra;
  auto const ptr = trueStatic ? low_malloc(bytes) : AllocShared(bytes);
  return MemBlock{ptr, bytes};
}

template <bool trueStatic> ALWAYS_INLINE
StringData* StringData::MakePersistentAt(folly::StringPiece sl, MemBlock range) {
  auto const extra = trueStatic ? sizeof(SymbolPrefix) : 0;
  assertx(range.size >= sl.size() + kStringOverhead + extra);

  StringData* sd = reinterpret_cast<StringData*>(
    reinterpret_cast<uintptr_t>(range.ptr) + extra
  );
  // is_static_string() answers purely from the address, so the allocator
  // AllocatePersistent() picked has to agree with it: low_malloc() puts true
  // statics below kMidArenaMaxAddr, AllocShared() puts uncounted strings in
  // the high arena above it.
  always_assert(is_static_string(sd) == trueStatic);
  auto const data = reinterpret_cast<char*>(sd + 1);

  auto const count = trueStatic ? StaticValue : SharedValue;
  if (trueStatic) {
    auto constexpr aux = kIsSymbolMask << 8 | kInvalidColor;
    sd->initHeader_16(HeaderKind::String, count, aux);
    getSymbolPrefix(sd)->nty = nullptr;
    getSymbolPrefix(sd)->cls = nullptr;
  } else {
    sd->initHeader_16(HeaderKind::String, count, kInvalidColor);
  }
  sd->m_len = sl.size(); // m_hash is computed soon.

  // set null byte
  data[sl.size()] = 0;
  auto const mcret = memcpy(data, sl.data(), sl.size());
  auto const ret = reinterpret_cast<StringData*>(mcret) - 1;
  // Recalculating ret from mcret avoids a spill.
  ret->preCompute();                    // get m_hash right

  assertx(ret == sd);
  assertx(trueStatic ? ret->isStatic() : ret->isShared());
  assertx(ret->isSymbol() == trueStatic);
  assertx(ret->checkSane());
  return ret;
}

template <bool trueStatic> ALWAYS_INLINE
StringData* StringData::MakePersistent(folly::StringPiece sl) {
  assertx(IMPLIES(trueStatic, StaticString::s_globalInit));
  auto range = AllocatePersistent<trueStatic>(sl);
  return MakePersistentAt<trueStatic>(sl, range);
}

StringData* StringData::MakeStatic(folly::StringPiece sl) {
  return MakePersistent<true>(sl);
}

StringData* StringData::MakeShared(folly::StringPiece sl) {
  return MakePersistent<false>(sl);
}

StringData* StringData::InitializeEmpty() {
  auto constexpr size = kStringOverhead + sizeof(SymbolPrefix);
  static_assert(size == StaticLiterals::kEmptyStringSize);

  return StringData::MakePersistentAt<true>(
    folly::StringPiece{""},
    MemBlock{reinterpret_cast<void*>(StaticLiterals::EmptyString()), size}
  );
}

void StringData::destructStatic() {
  assertx(checkSane() && isStatic());
  if (isSymbol()) {
    low_sized_free(reinterpret_cast<SymbolPrefix*>(this) - 1,
                    size() + kStringOverhead + sizeof(SymbolPrefix));
  } else {
    low_sized_free(this, size() + kStringOverhead);
  }
}

void StringData::ReleaseShared(StringData* str) {
  assertx(str->checkSane());
  assertx(!str->sharedCowCheck());
  FreeShared(str, str->size() + kStringOverhead);
}

//////////////////////////////////////////////////////////////////////

StringData* StringData::Make(const StringData* s, CopyStringMode) {
  auto const sd = allocFlat(s->m_len);
  sd->m_lenAndHash = s->m_lenAndHash;
  auto const data = static_cast<void*>(sd + 1);
  *memcpy8(data, s->data(), s->m_len) = 0;

  assertx(sd->same(s));
  return sd;
}

StringData* StringData::Make(folly::StringPiece sl, CopyStringMode) {
  auto const sd = allocFlat(sl.size());
  sd->m_lenAndHash = sl.size(); // hash=0
  auto const data = reinterpret_cast<char*>(sd + 1);

  data[sl.size()] = 0;
  auto const mcret = memcpy(data, sl.data(), sl.size());
  auto const ret = reinterpret_cast<StringData*>(mcret) - 1;
  // Recalculating ret from mcret avoids a spill.

  assertx(ret == sd);
  assertx(ret->m_len == sl.size());
  assertx(ret->hasExactlyOneRef());
  assertx(ret->m_hash == 0);
  assertx(ret->checkSane());
  return ret;
}

StringData* StringData::Make(const char* data, size_t len, CopyStringMode) {
  if (UNLIKELY(len > StringData::MaxSize)) {
    raiseStringLengthExceededError(len);
  }

  return Make(folly::StringPiece(data, len), CopyString);
}

StringData* StringData::Make(size_t reserveLen) {
  auto const sd = allocFlat(reserveLen);
  sd->setSize(0);

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::Make() {
  return Make(SmallStringReserve);
}

//////////////////////////////////////////////////////////////////////

StringData* StringData::Make(char* data, size_t len, AttachStringMode) {
  if (UNLIKELY(len > StringData::MaxSize)) {
    raiseStringLengthExceededError(len);
  }
  auto const sd = Make(folly::StringPiece(data, len), CopyString);
  free(data);
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::Make(folly::StringPiece r1, folly::StringPiece r2) {
  // Undefined behavior if we pass nullptr strings into StringData::Make
  assertx(r1.data() && r2.data());
  auto const len = r1.size() + r2.size();
  auto const sd = allocFlat(len);
  sd->m_lenAndHash = len; // hash=0

  auto const data = reinterpret_cast<char*>(sd + 1);
  memcpy(data,             r1.data(), r1.size());
  memcpy(data + r1.size(), r2.data(), r2.size());
  data[len] = 0;

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::Make(const StringData* s1, const StringData* s2) {
  auto const len = s1->m_len + s2->m_len;
  // `memcpy8()' could overrun the buffer by at most 7 bytes, so we allocate 6
  // more bytes here, which (together with the trailing 0) makes it safe.
  auto const sd = allocFlat(len + 6);
  sd->m_lenAndHash = len; // hash=0

  auto const data = reinterpret_cast<char*>(sd + 1);
  auto const next = memcpy8(data, s1->data(), s1->m_len);
  *memcpy8(next, s2->data(), s2->m_len) = 0;

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::Make(folly::StringPiece s1, const char* lit2) {
  return Make(s1, folly::StringPiece(lit2, strlen(lit2)));
}

StringData* StringData::Make(folly::StringPiece r1, folly::StringPiece r2,
                             folly::StringPiece r3) {
  auto const len = r1.size() + r2.size() + r3.size();
  auto const sd = allocFlat(len);
  sd->m_lenAndHash  = len; // hash=0

  auto p = reinterpret_cast<char*>(sd + 1);
  p = static_cast<char*>(memcpy(p,             r1.data(), r1.size()));
  p = static_cast<char*>(memcpy(p + r1.size(), r2.data(), r2.size()));
  p = static_cast<char*>(memcpy(p + r2.size(), r3.data(), r3.size()));
  p[r3.size()] = 0;

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::Make(folly::StringPiece r1, folly::StringPiece r2,
                             folly::StringPiece r3, folly::StringPiece r4) {
  auto const len = r1.size() + r2.size() + r3.size() + r4.size();
  auto const sd = allocFlat(len);
  sd->m_lenAndHash = len; // hash=0

  auto p = reinterpret_cast<char*>(sd + 1);
  p = static_cast<char*>(memcpy(p,             r1.data(), r1.size()));
  p = static_cast<char*>(memcpy(p + r1.size(), r2.data(), r2.size()));
  p = static_cast<char*>(memcpy(p + r2.size(), r3.data(), r3.size()));
  p = static_cast<char*>(memcpy(p + r3.size(), r4.data(), r4.size()));
  p[r4.size()] = 0;

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

//////////////////////////////////////////////////////////////////////

void StringData::release() noexcept {
  fixCountForRelease();
  assertx(isRefCounted());
  assertx(checkSane());
  tl_heap->objFreeIndex(this, m_aux16);
  AARCH64_WALKABLE_FRAME();
}

//////////////////////////////////////////////////////////////////////

#define ALIASING_APPEND_ASSERT(ptr, len)                        \
  assertx(uintptr_t(ptr) <= uintptr_t(data()) ||                 \
         uintptr_t(ptr) >= uintptr_t(data() + capacity() + 1)); \
  assertx(ptr != data() || len <= m_len);

StringData* StringData::append(folly::StringPiece range) {
  assertx(!hasMultipleRefs());

  auto s = range.data();
  auto const len = range.size();
  if (len == 0) return this;
  auto const newLen = size_t(m_len) + size_t(len);

  if (UNLIKELY(newLen > MaxSize)) {
    raiseStringLengthExceededError(newLen);
  }

  /*
   * We may have an aliasing append.  We don't allow appending with an
   * interior pointer, although we may be asked to append less than
   * the whole string in an aliasing situation.
   */
  ALIASING_APPEND_ASSERT(s, len);

  auto const requestLen = static_cast<uint32_t>(newLen);
  auto const target = reserve(requestLen);
  memcpy(target->mutableData() + m_len, s, len);
  target->setSize(newLen);
  assertx(target->checkSane());

  return target;
}

StringData* StringData::append(folly::StringPiece r1, folly::StringPiece r2) {
  assertx(!hasMultipleRefs());

  auto const len = r1.size() + r2.size();

  if (len == 0) return this;
  if (UNLIKELY(size_t(m_len) + size_t(len) > MaxSize)) {
    raiseStringLengthExceededError(size_t(len) + size_t(m_len));
  }

  auto const newLen = m_len + len;

  /*
   * We may have an aliasing append.  We don't allow appending with an
   * interior pointer, although we may be asked to append less than
   * the whole string in an aliasing situation.
   */
  ALIASING_APPEND_ASSERT(r1.data(), r1.size());
  ALIASING_APPEND_ASSERT(r2.data(), r2.size());
  auto const target = reserve(newLen);

  /*
   * memcpy is safe even if it's a self append---the regions will be
   * disjoint, since rN.data() can't point past the start of our source
   * pointer, and rN.size() is smaller than the old length.
   */
  void* p = target->mutableData();
  p = memcpy((char*)p + m_len,     r1.data(), r1.size());
      memcpy((char*)p + r1.size(), r2.data(), r2.size());

  target->setSize(newLen);
  assertx(target->checkSane());

  return target;
}

StringData* StringData::append(folly::StringPiece r1,
                               folly::StringPiece r2,
                               folly::StringPiece r3) {
  assertx(!hasMultipleRefs());

  auto const len = r1.size() + r2.size() + r3.size();

  if (len == 0) return this;
  if (UNLIKELY(size_t(m_len) + size_t(len) > MaxSize)) {
    raiseStringLengthExceededError(size_t(len) + size_t(m_len));
  }

  auto const newLen = m_len + len;

  /*
   * We may have an aliasing append.  We don't allow appending with an
   * interior pointer, although we may be asked to append less than
   * the whole string in an aliasing situation.
   */
  ALIASING_APPEND_ASSERT(r1.data(), r1.size());
  ALIASING_APPEND_ASSERT(r2.data(), r2.size());
  ALIASING_APPEND_ASSERT(r3.data(), r3.size());
  auto const target = reserve(newLen);

  /*
   * memcpy is safe even if it's a self append---the regions will be
   * disjoint, since rN.data() can't point past the start of our source
   * pointer, and rN.size() is smaller than the old length.
   */
  void* p = target->mutableData();
  p = memcpy((char*)p + m_len,     r1.data(), r1.size());
  p = memcpy((char*)p + r1.size(), r2.data(), r2.size());
      memcpy((char*)p + r2.size(), r3.data(), r3.size());

  target->setSize(newLen);
  assertx(target->checkSane());

  return target;
}

#undef ALIASING_APPEND_ASSERT

//////////////////////////////////////////////////////////////////////

StringData* StringData::reserve(size_t cap) {
  assertx(!hasMultipleRefs());

  if (cap <= capacity()) return this;

  cap = std::min(cap + cap / 4, size_t(MaxSize));
  auto const sd = allocFlat(cap);

  // Request-allocated StringData are always aligned at 16 bytes, thus it is
  // safe to copy in 16-byte groups.
  // layout: [header][m_lenAndHash][...data]
  sd->m_lenAndHash = m_lenAndHash;
  // This copies the characters (m_len bytes), and the trailing zero (1 byte)
  memcpy16_inline(sd+1, this+1, (m_len + 1 + 15) & ~0xF);
  assertx(reinterpret_cast<uintptr_t>(this+1) % 16 == 0);

  assertx(sd->hasExactlyOneRef());
  assertx(sd->checkSane());
  return sd;
}

StringData* StringData::shrinkImpl(size_t len) {
  assertx(!hasMultipleRefs());
  assertx(len <= capacity());

  auto const sd = allocFlat(len);
  sd->m_lenAndHash = len;
  auto const src = static_cast<void*>(this + 1);
  auto const dst = static_cast<void*>(sd + 1);
  *memcpy8(dst, src, len) = 0;

  assertx(sd->checkSane());
  return sd;
}

void StringData::dump() const {
  auto s = slice();

  printf("StringData(%d) (%s%s%d): [", m_count,
         isStatic() ? "static " : "",
         isShared() ? "shared " : "",
         static_cast<int>(s.size()));
  for (uint32_t i = 0; i < s.size(); i++) {
    char ch = s.data()[i];
    if (isprint(ch)) {
      printf("%c", ch);
    } else {
      printf("\\x%02x", ch);
    }
  }
  printf("]\n");
}

StringData* StringData::getChar(int offset) const {
  if (offset >= 0 && offset < size()) {
    return makeStaticString(data()[offset]);
  }
  raise_notice("Uninitialized string offset: %d", offset);
  return staticEmptyString();
}

StringData* StringData::increment() {
  assertx(!hasMultipleRefs());
  assertx(!empty());
  auto const sd = reserve(m_len + 1);
  sd->incrementHelper();
  return sd;
}

void StringData::incrementHelper() {
  raise_notice("Increment on string '%s'", data());
  m_hash = 0;

  enum class CharKind {
    UNKNOWN,
    LOWER_CASE,
    UPPER_CASE,
    NUMERIC
  };

  auto const len = m_len;
  auto const s = mutableData();
  int carry = 0;
  int pos = len - 1;
  auto last = CharKind::UNKNOWN; // Shut up the compiler warning
  int ch;

  while (pos >= 0) {
    ch = s[pos];
    if (ch >= 'a' && ch <= 'z') {
      if (ch == 'z') {
        s[pos] = 'a';
        carry=1;
      } else {
        s[pos]++;
        carry=0;
      }
      last = CharKind::LOWER_CASE;
    } else if (ch >= 'A' && ch <= 'Z') {
      if (ch == 'Z') {
        s[pos] = 'A';
        carry=1;
      } else {
        s[pos]++;
        carry=0;
      }
      last = CharKind::UPPER_CASE;
    } else if (ch >= '0' && ch <= '9') {
      if (ch == '9') {
        s[pos] = '0';
        carry=1;
      } else {
        s[pos]++;
        carry=0;
      }
      last = CharKind::NUMERIC;
    } else {
      carry=0;
      break;
    }
    if (carry == 0) {
      break;
    }
    pos--;
  }

  if (carry) {
    if (UNLIKELY(len + 1 > MaxSize)) {
      raiseStringLengthExceededError(len + 1);
    }

    assertx(len + 1 <= capacity());
    memmove(s + 1, s, len);
    s[len + 1] = '\0';
    m_len = len + 1;

    switch (last) {
    case CharKind::NUMERIC:
      s[0] = '1';
      break;
    case CharKind::UPPER_CASE:
      s[0] = 'A';
      break;
    case CharKind::LOWER_CASE:
      s[0] = 'a';
      break;
    default:
      break;
    }
  }
}

void StringData::preCompute() {
  auto s = slice();
  m_hash = hash_string_i_unsafe(s.data(), s.size());
  assertx(m_hash >= 0);
  if (s.size() > 0 &&
      (is_numeric_string(s.data(), s.size(), nullptr, nullptr,
                         1, nullptr) == KindOfNull)) {
    m_hash |= STRHASH_MSB;
  }
}

///////////////////////////////////////////////////////////////////////////////
// type conversions

DataType StringData::isNumericWithVal(int64_t &lval, double &dval,
                                      int allow_errors, int* overflow) const {
  if (m_hash < 0) return KindOfNull;
  DataType ret = KindOfNull;
  auto s = slice();
  if (s.size()) {
    ret = is_numeric_string(
      s.data(),
      s.size(),
      &lval,
      &dval,
      allow_errors,
      overflow
    );
    if (ret == KindOfNull && allow_errors) {
      m_hash |= STRHASH_MSB;
    }
  }
  return ret;
}

bool StringData::isNumeric() const {
  if (m_hash < 0) return false;
  int64_t lval; double dval;
  DataType ret = isNumericWithVal(lval, dval, 0);
  switch (ret) {
    case KindOfNull:
      return false;
    case KindOfInt64:
    case KindOfDouble:
      return true;
    case KindOfUninit:
    case KindOfBoolean:
    case KindOfString:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      break;
  }
  not_reached();
}

bool StringData::toBoolean() const {
  return !empty() && !isZero();
}

int64_t StringData::toInt64(int base /* = 10 */) const {
  return strtoll(data(), nullptr, base);
}

double StringData::toDouble() const {
  auto s = slice();
  if (s.size()) return zend_strtod(s.data(), nullptr);
  return 0;
}

DataType StringData::toNumeric(int64_t &lval, double &dval) const {
  if (m_hash < 0) return KindOfString;
  DataType ret = isNumericWithVal(lval, dval, 0);
  if (ret == KindOfInt64 || ret == KindOfDouble) return ret;
  return KindOfString;
}

///////////////////////////////////////////////////////////////////////////////
// comparisons

bool StringData::equal(const StringData *s) const {
  assertx(s);
  if (s == this) return true;
  return same(s);
}

int StringData::numericCompare(const StringData *v2) const {
  assertx(v2);

  int oflow1, oflow2;
  int64_t lval1, lval2;
  double dval1, dval2;
  DataType ret1, ret2;
  if ((ret1 = isNumericWithVal(lval1, dval1, 0, &oflow1)) == KindOfNull ||
      (ret1 == KindOfDouble && !std::isfinite(dval1)) ||
      (ret2 = v2->isNumericWithVal(lval2, dval2, 0, &oflow2)) == KindOfNull ||
      (ret2 == KindOfDouble && !std::isfinite(dval2))) {
    return -2;
  }
  if (oflow1 && oflow1 == oflow2 && dval1 == dval2) {
    return -2; // overflow in same direction, comparison will be inaccurate
  }
  if (ret1 == KindOfInt64 && ret2 == KindOfInt64) {
    if (lval1 > lval2) return 1;
    if (lval1 == lval2) return 0;
    return -1;
  }
  if (ret1 == KindOfDouble && ret2 == KindOfDouble) {
    if (dval1 > dval2) return 1;
    if (dval1 == dval2) return 0;
    return -1;
  }

  if (ret1 == KindOfDouble) {
    assertx(ret2 == KindOfInt64);
    if (oflow1) {
      return oflow1;
    }
    dval2 = (double)lval2;
  } else {
    assertx(ret1 == KindOfInt64);
    assertx(ret2 == KindOfDouble);
    if (oflow2) {
      return -oflow2;
    }
    dval1 = (double)lval1;
  }

  if (dval1 > dval2) return 1;
  if (dval1 == dval2) return 0;
  return -1;
}

int StringData::compare(const StringData *v2) const {
  assertx(v2);

  if (v2 == this) return 0;

  int ret = numericCompare(v2);
  if (ret < -1) {
    int len1 = size();
    int len2 = v2->size();
    int len = len1 < len2 ? len1 : len2;
    ret = memcmp(data(), v2->data(), len);
    if (ret) return ret;
    if (len1 == len2) return 0;
    return len < len1 ? 1 : -1;
  }
  return ret;
}

StringData*
StringData::substr(int start, int length /* = StringData::MaxSize */) {
  if (start < 0 || start >= size() || length <= 0) {
    return staticEmptyString();
  }

  auto const max_len = size() - start;
  if (length > max_len) {
    length = max_len;
  }

  assertx(length > 0);
  if (UNLIKELY(length == size())) {
    incRefCount();
    return this;
  }
  if (UNLIKELY(length == 1)) {
    return makeStaticString(data()[start]);
  }
  return StringData::Make(data() + start, length, CopyString);
}

///////////////////////////////////////////////////////////////////////////////
// Serialization

__thread UnitEmitter* BlobEncoderHelper<const StringData*>::tl_unitEmitter{nullptr};
__thread Unit* BlobEncoderHelper<const StringData*>::tl_unit{nullptr};
__thread BlobEncoderHelper<const StringData*>::Indexer*
  BlobEncoderHelper<const StringData*>::tl_indexer{nullptr};

void BlobEncoderHelper<const StringData*>::serde(BlobEncoder& encoder,
                                                 const StringData* sd) {
  assertx(!tl_unit);
  if (auto const ue = tl_unitEmitter) {
    Id id = ue->mergeLitstr(sd);
    encoder(id);
    return;
  }
  if (tl_indexer) {
    auto const it = tl_indexer->m_indices.find(sd);
    if (it == end(tl_indexer->m_indices)) {
      auto const id = tl_indexer->m_indices.size();
      assertx(id < std::numeric_limits<std::uint32_t>::max());
      tl_indexer->m_indices.emplace(sd, id);
      encoder(uint32_t(0));
      // Fallthrough and encode the string below
    } else {
      encoder(uint32_t(it->second+1));
      return;
    }
  }

  if (!sd) {
    encoder(uint32_t(0));
    return;
  }
  auto const sz = sd->size();
  encoder(static_cast<uint32_t>(sz + 1));
  if (!sz) return;
  encoder.writeRaw(sd->data(), sz);
}

void BlobEncoderHelper<const StringData*>::serde(BlobDecoder& decoder,
                                                 const StringData*& sd,
                                                 bool makeStatic) {
  if (auto const ue = tl_unitEmitter) {
    Id id;
    decoder(id);
    if (makeStatic) {
      sd = ue->lookupLitstrId(id);
      assertx(!sd || sd->isStatic());
    } else {
      sd = ue->lookupLitstrIdCopy(id).detach();
    }
    return;
  }
  if (auto const u = tl_unit) {
    assertx(makeStatic);
    Id id;
    decoder(id);
    sd = u->lookupLitstrId(id);
    return;
  }

  auto const read = [&] () -> const StringData* {
    uint32_t size;
    decoder(size);
    if (size == 0) return nullptr;
    --size;
    if (size == 0) return staticEmptyString();
    assertx(decoder.remaining() >= size);
    auto const data = decoder.data();
    auto const s = makeStatic
      ? makeStaticString((const char*)data, size)
      : StringData::Make((const char*)data, size, CopyString);
    decoder.advance(size);
    return s;
  };

  if (auto const indexer = tl_indexer) {
    assertx(makeStatic);
    uint32_t id;
    decoder(id);
    if (!id) {
      sd = read();
      indexer->m_strs.emplace_back(sd);
    } else {
      --id;
      assertx(id < indexer->m_strs.size());
      sd = indexer->m_strs[id];
    }
    return;
  }

  sd = read();
}

folly::StringPiece
BlobEncoderHelper<const StringData*>::asStringPiece(BlobDecoder& decoder) {
  if (auto const ue = tl_unitEmitter) {
    Id id;
    decoder(id);
    auto const sd = ue->lookupLitstrId(id);
    if (!sd) return { nullptr, size_t{0} };
    return sd->slice();
  }
  if (auto const u = tl_unit) {
    Id id;
    decoder(id);
    auto const sd = u->lookupLitstrId(id);
    if (!sd) return { nullptr, size_t{0} };
    return sd->slice();
  }
  assertx(!tl_indexer);

  uint32_t size;
  decoder(size);
  if (size == 0) return { (const char*)nullptr, size_t{0} };
  auto const data = reinterpret_cast<const char*>(decoder.data());
  if (size == 1) return { data, size_t{0} };
  --size;
  assertx(decoder.remaining() >= size);
  decoder.advance(size);
  return { data, size };
}

void BlobEncoderHelper<const StringData*>::skip(BlobDecoder& decoder) {
  if (tl_unitEmitter || tl_unit) {
    Id id;
    decoder(id);
    return;
  }
  assertx(!tl_indexer);

  uint32_t size;
  decoder(size);
  // Any size less than 2 has no data (0 is a nullptr, and 1 is an
  // empty string).
  if (size <= 1) return;
  --size;
  assertx(decoder.remaining() >= size);
  decoder.advance(size);
}

size_t BlobEncoderHelper<const StringData*>::peekSize(BlobDecoder& decoder) {
  if (tl_unitEmitter || tl_unit) {
    auto const before = decoder.advanced();
    Id id;
    decoder(id);
    auto const size = decoder.advanced() - before;
    decoder.retreat(size);
    return size;
  }
  assertx(!tl_indexer);

  auto const before = decoder.advanced();
  uint32_t size;
  decoder(size);
  auto const sizeBytes = decoder.advanced() - before;
  decoder.retreat(sizeBytes);
  if (size <= 1) return sizeBytes;
  --size;
  return sizeBytes + size;
}

void BlobEncoderHelper<PackedStringPtr>::serde(BlobEncoder& encoder,
  PackedStringPtr s) {
  auto const sd = s.get();
  encoder(sd);
}

void BlobEncoderHelper<PackedStringPtr>::serde(BlobDecoder& decoder,
  PackedStringPtr& s) {
  const StringData* sd;
  decoder(sd);
  s = sd;
}

///////////////////////////////////////////////////////////////////////////////
// Debug

std::string StringData::toCppString() const {
  auto s = slice();
  return std::string(s.data(), s.size());
}

bool StringData::checkSane() const {
  static_assert(sizeof(StringData) == 16);
  static_assert(size_t(MaxSize) <= size_t(INT_MAX), "Beware int wraparound");
  static_assert(sizeof(StringData) == SD_DATA, "");
  static_assert(offsetof(StringData, m_len) == SD_LEN, "");
  static_assert(offsetof(StringData, m_hash) == SD_HASH, "");
  assertx(kindIsValid());
  assertx(uint32_t(size()) <= MaxSize);
  assertx(size() >= 0);
  assertx(IMPLIES(isSymbol(), isStatic()));
  if (isRefCounted()) {
    assertx(size() <= capacity());
    assertx(capacity() <= MaxSize);
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

}
