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

#include "hphp/util/data-block.h"

namespace HPHP {

void* DataBlock::allocInner(size_t len) {
  assert(len < std::numeric_limits<Size>::max());

  if (!len) return nullptr;

  auto freeList = m_freeLists.lower_bound(len);
  if (freeList == m_freeLists.end()) {
    return nullptr;
  }
  always_assert(!freeList->second.empty());

  auto size = freeList->first;
  auto off  = *freeList->second.begin();
  freeList->second.erase(off);
  m_freeRanges.erase(off);
  m_freeRanges.erase(off + size);

  assert(size >= len);

  if (size != len) {
    m_freeLists[size - len].emplace(off + len);
    m_freeRanges[off + len] = size - len;
    m_freeRanges[off + size] = -(int64_t)(size - len);
  }

  ++m_nalloc;
  m_bytesFree -= len;

  if (freeList->second.empty()) m_freeLists.erase(freeList);

  return m_destBase + off;
}

void DataBlock::free(void* vaddr, size_t len) {
  if (len == 0) {
    return;
  }
  auto addr = static_cast<void*>(((char*)vaddr - (char*)m_destBase) + m_base);

  assert(len < std::numeric_limits<uint32_t>::max() &&
         (CodeAddress)addr + len <= m_frontier);

  ++m_nfree;

  Offset off = (Offset)((CodeAddress)addr - m_base);

  auto after = m_freeRanges.find(off + len);
  auto before = m_freeRanges.find(off);
  if (before != m_freeRanges.end()) {
    assertx(before->second < 0);
    auto beforeEnd = before;
    before = m_freeRanges.find(off + before->second);
    m_freeRanges.erase(beforeEnd);
    assertx(before != m_freeRanges.end());
  }

  if (before != m_freeRanges.end()) {
    auto list = m_freeLists.find(before->second);
    assertx(list != m_freeLists.end());

    list->second.erase(before->first);
    if (list->second.empty()) m_freeLists.erase(list);
  }

  if ((CodeAddress)addr + len == m_frontier) {
    if (before != m_freeRanges.end()) {
      m_bytesFree -= before->second;
      len += before->second;
      m_freeRanges.erase(before);
    }
    assert(after == m_freeRanges.end());
    m_frontier -= len;
    return;
  }

  m_bytesFree += len;

  if (after != m_freeRanges.end()) {
    len += after->second;
    auto list = m_freeLists.find(after->second);
    assertx(list != m_freeLists.end());

    list->second.erase(after->first);
    if (list->second.empty()) m_freeLists.erase(list);
    m_freeRanges.erase(after);
  }

  if (before != m_freeRanges.end()) {
    before->second = (len += before->second);
    off = before->first;
  } else {
    m_freeRanges[off] = len;
  }
  m_freeLists[len].emplace(off);
  m_freeRanges[off + len] = -(int64_t)len;
}

void DataBlock::reportFull(size_t nBytes) const {
  throw DataBlockFull(m_name, fmt::format(
    "Attempted to emit {} byte(s) into a {} byte DataBlock with {} bytes "
    "available ({} max bytes). This almost certainly means the TC is full.",
    nBytes, m_size, m_size - (m_frontier - m_base), m_maxGrow));
}

void DataBlock::reportMallocError(size_t nBytes) const {
  throw DataBlockFull(m_name, fmt::format(
    "Encountered a malloc/realloc error while attempting to grow a {} byte "
    "DataBlock to {} bytes. The maximum size for this DataBlock is {} byte(s).",
    m_size, nBytes, m_maxGrow));
}

}
