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
#include "hphp/runtime/base/string-buffer.h"

#include <algorithm>

#include <folly/Conv.h>
#include <folly/portability/Fcntl.h>

#include "hphp/runtime/base/file.h"

#include "hphp/util/conv-10.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

StringBuffer::StringBuffer(uint32_t initialSize /* = SmallStringReserve */)
  : m_initialCap(initialSize)
  , m_maxBytes(kDefaultOutputLimit)
  , m_len(0)
{
  m_str = StringData::Make(initialSize);
  m_cap = m_str->capacity();
}

StringBuffer::~StringBuffer() {
  if (m_str) {
    assertx(m_str->hasExactlyOneRef());
    assertx((m_str->setSize(0), true)); // appease StringData::checkSane()
    m_str->release();
  }
}

const char* StringBuffer::data() const {
  if (m_str && m_len) {
    auto buffer = m_str->mutableData();
    buffer[m_len] = '\0'; // fixup
    return buffer;
  }
  return nullptr;
}

OptString StringBuffer::detach() {
  if (m_str && m_len) {
    assertx(m_str->hasExactlyOneRef());
    auto str = OptString::attach(m_str);
    str.setSize(m_len);
    m_str = nullptr;
    m_len = 0;
    m_cap = 0;
    return str;
  }
  return String::Empty();
}

OptString StringBuffer::copy() const {
  return OptString(data(), size(), CopyString);
}

void StringBuffer::absorb(StringBuffer& buf) {
  if (empty()) {
    StringData* str = m_str;

    m_str = buf.m_str;
    m_len = buf.m_len;
    m_cap = buf.m_cap;

    buf.m_str = str;
    if (str) {
      buf.m_len = str->size();
      buf.m_cap = str->capacity();
    } else {
      buf.m_len = 0;
      buf.m_cap = 0;
    }
    buf.clear();
    return;
  }

  append(buf.detach());
}

void StringBuffer::clear() {
  m_len = 0;
}

void StringBuffer::release() {
  if (m_str) {
    assertx(m_str->hasExactlyOneRef());
    if (debug) {
      m_str->mutableData()[m_len] = 0; // appease StringData::checkSane()
    }
    m_str->release();
  }
  m_str = nullptr;
  m_len = m_cap = 0;
}

void StringBuffer::resize(uint32_t size) {
  assertx(size <= m_cap);
  if (size <= m_cap) {
    m_len = size;
  }
}

char* StringBuffer::appendCursor(int size) {
  if (!m_str) {
    makeValid(size);
  } else if (m_cap - m_len < size) {
    m_str->setSize(m_len);
    auto const tmp = m_str->reserve(m_len + size);
    if (UNLIKELY(tmp != m_str)) {
      assertx(m_str->hasExactlyOneRef());
      m_str->release();
      m_str = tmp;
    }
    m_cap = m_str->capacity();
  }
  return m_str->mutableData() + m_len;
}

void StringBuffer::append(int n) {
  char buf[12];
  int len;
  auto const sd = OptString::GetIntegerStringData(n);
  char *p;
  if (!sd) {
    auto sl = conv_10(n, buf + 12);
    p = const_cast<char*>(sl.data());
    len = sl.size();
  } else {
    p = (char *)sd->data();
    len = sd->size();
  }
  append(p, len);
}

void StringBuffer::append(int64_t n) {
  char buf[21];
  int len;
  auto const sd = OptString::GetIntegerStringData(n);
  char *p;
  if (!sd) {
    auto sl = conv_10(n, buf + 21);
    p = const_cast<char*>(sl.data());
    len = sl.size();
  } else {
    p = (char *)sd->data();
    len = sd->size();
  }
  append(p, len);
}

void StringBuffer::append(const Variant& v) {
  auto const cell = v.asTypedValue();
  switch (cell->m_type) {
    case KindOfInt64:
      append(cell->m_data.num);
      break;
    case KindOfString:
      append(cell->m_data.pstr);
      break;
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfDouble:
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
      append(v.toString());
  }
}

void StringBuffer::appendHelper(char ch) {
  if (!valid()) makeValid(1);
  if (m_len == m_cap) {
    growBy(1);
  }
  m_str->mutableData()[m_len++] = ch;
}

void StringBuffer::makeValid(uint32_t minCap) {
  assertx(!valid());
  assertx(!m_len);
  m_str = StringData::Make(std::max(m_initialCap, minCap));
  m_cap = m_str->capacity();
}

void StringBuffer::appendHelper(const char *s, int len) {
  if (!valid()) makeValid(len);

  assertx(s);
  assertx(len >= 0);
  if (len <= 0) return;

  if (len > m_cap - m_len) {
    growBy(len);
  }
  memcpy(m_str->mutableData() + m_len, s, len);
  m_len += len;
}

void StringBuffer::printf(const char *format, ...) {
  va_list ap;
  va_start(ap, format);

  bool printed = false;
  for (int len = 1024; !printed; len <<= 1) {
    va_list v;
    va_copy(v, ap);

    char *buf = (char*)req::malloc_noptrs(len);
    SCOPE_EXIT { req::free(buf); };
    if (vsnprintf(buf, len, format, v) < len) {
      append(buf);
      printed = true;
    }

    va_end(v);
  }

  va_end(ap);
}

void StringBuffer::read(FILE* in, int page_size /* = 1024 */) {
  assertx(in);
  assertx(page_size > 0);

  if (!valid()) makeValid(page_size);
  while (true) {
    int buffer_size = m_cap - m_len;
    if (buffer_size < page_size) {
      growBy(page_size);
      buffer_size = m_cap - m_len;
    }
    size_t len = fread(m_str->mutableData() + m_len, 1, buffer_size, in);
    if (len == 0) break;
    m_len += len;
  }
}

void StringBuffer::read(File* in, int page_size /* = 1024 */) {
  assertx(in);
  assertx(page_size > 0);

  if (!valid()) makeValid(page_size);
  while (true) {
    int buffer_size = m_cap - m_len;
    if (buffer_size < page_size) {
      growBy(page_size);
      buffer_size = m_cap - m_len;
    }
    int64_t len = in->readImpl(m_str->mutableData() + m_len, buffer_size);
    assertx(len >= 0);
    if (len == 0) break;
    m_len += len;
  }
}

void StringBuffer::growBy(int spaceRequired) {
  /*
   * The default initial size is a power-of-two minus 1.
   * This doubling scheme keeps the total block size a
   * power of two, which should be good for memory allocators.
   * But note there is no guarantee either that the initial size
   * is power-of-two minus 1, or that it stays that way
   * (new_size < minSize below).
   */
  auto new_size = m_cap * 2 + 1;
  auto const minSize = static_cast<unsigned>(m_cap) + spaceRequired;
  if (new_size < minSize) {
    new_size = minSize;
  }

  if (new_size > m_maxBytes) {
    if (minSize > m_maxBytes) {
      throw StringBufferLimitException(m_maxBytes, detach());
    } else {
      new_size = m_maxBytes;
    }
  }

  m_str->setSize(m_len);
  auto const tmp = m_str->reserve(new_size);
  if (UNLIKELY(tmp != m_str)) {
    assertx(m_str->hasExactlyOneRef());
    m_str->release();
    m_str = tmp;
  }
  m_cap = m_str->capacity();
}

///////////////////////////////////////////////////////////////////////////////
}
