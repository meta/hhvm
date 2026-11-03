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

#include "hphp/util/async-func.h"

#include <folly/Exception.h>
#include <folly/portability/SysTime.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/SysResource.h>
#include <folly/portability/Unistd.h>

#ifdef HAVE_NUMA
#include <sys/prctl.h>
#endif

#include "hphp/util/alloc.h"
#include "hphp/util/hugetlb.h"
#include "hphp/util/maphuge.h"
#include "hphp/util/numa.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

using PFN_THREAD_FUNC = void(void *);

PFN_THREAD_FUNC* AsyncFuncImpl::s_initFunc = nullptr;
void* AsyncFuncImpl::s_initFuncArg = nullptr;

PFN_THREAD_FUNC* AsyncFuncImpl::s_finiFunc = nullptr;
void* AsyncFuncImpl::s_finiFuncArg = nullptr;

std::atomic<uint32_t> AsyncFuncImpl::s_count { 0 };

AsyncFuncImpl::AsyncFuncImpl(void *obj, PFN_THREAD_FUNC *func,
                             int numaNode, unsigned hugeStackKb,
                             unsigned tlExtraKb,
                             const std::string& threadGroupSuffix)
  : m_obj(obj)
  , m_func(func)
  , m_node(numaNode)
  , m_hugeStackKb(hugeStackKb / 4 * 4)  // align to 4K page boundary
  , m_tlExtraKb((tlExtraKb + 3) / 4 * 4)
  , m_threadGroupSuffix(threadGroupSuffix) {
  if (m_tlExtraKb > (128 * 1024)) {
    // Don't include a big additional per-thread storage to avoid running out of
    // virtual memory.
    throw std::runtime_error{"extra per-thread storage is too big"};
  }
}

AsyncFuncImpl::~AsyncFuncImpl() {
  assert(m_stopped || m_threadId == 0);
  delete m_exception;
}

void *AsyncFuncImpl::ThreadFunc(void *obj) {
  auto self = static_cast<AsyncFuncImpl*>(obj);
  init_stack_limits(self->getThreadAttr());
  s_tlSpace = MemBlock{self->m_tlExtraBase, self->m_tlExtraKb * 1024};
  assertx(!s_tlSpace.ptr || s_tlSpace.size);
  s_hugeRange = self->m_hugePages;
  assertx(!s_hugeRange.ptr || s_hugeRange.size);

  set_numa_binding(self->m_node);
  self->setThreadName();
  self->threadFuncImpl();
  return nullptr;
}

#ifdef __linux__
// Allocate a piece of memory using mmap(), with address range [start, end), so
// that
// (1) start + size == end,
// (2) (start + alignOffset) % alignment == 0, when alignment is nonzero
// (3) the memory can be used for stack, thread-local storage, and heap.
//
// All input should be multiples of 16.
static char* mmap_offset_aligned(size_t size, size_t alignOffset,
                                 size_t alignment) {
  assertx(size % 16 == 0 && alignOffset % 16 == 0 && alignment % 16 == 0);
  assertx(alignOffset <= size);
  assertx(folly::isPowTwo(alignment));
  auto const alignMask = alignment - 1;
  auto const allocSize = size + (alignment > 16) * alignment;
  char* start = (char*)mmap(nullptr, allocSize,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1, 0);
  // Check if `mmap()` returned -1, and throw an exception in that case.
  folly::checkUnixError(reinterpret_cast<intptr_t>(start),
                        "mmap() failed with length = ", allocSize);
  if (alignment <= 16) return start;
  auto const oldAlignPoint = reinterpret_cast<uintptr_t>(start) + alignOffset;
  // Find out how many bytes we need to shift alignPoint to meet alignment
  // requirement.
  auto const offset =
    ((oldAlignPoint + alignMask) & ~alignMask) - oldAlignPoint;
  assertx((oldAlignPoint + offset) % alignment == 0);
  auto const newStart = start + offset;
  auto const newEnd = newStart + size;
  // unmap extra space at both ends, if any.
  if (offset) {
    munmap(start, offset);
  }
  if (auto const extraAfterEnd = start + allocSize - newEnd) {
    munmap(newEnd, extraAfterEnd);
  }
  return newStart;
}
#endif

void AsyncFuncImpl::start() {
  struct rlimit rlim;
  if (getrlimit(RLIMIT_STACK, &rlim) != 0 || rlim.rlim_cur == RLIM_INFINITY ||
      rlim.rlim_cur < kStackSizeMinimum) {
    rlim.rlim_cur = kStackSizeMinimum;
  }
  // Limit the size of the stack to something reasonable, to avoid running out
  // of virtual memory.
  if (rlim.rlim_cur > kStackSizeMinimum * 16) {
    rlim.rlim_cur = kStackSizeMinimum * 16;
  }

  if (m_hugeStackKb * 1024 > rlim.rlim_cur) {
#ifndef NDEBUG
    throw std::invalid_argument{"huge stack size exceeds rlimit"};
#else
    m_hugeStackKb = 0;
#endif
  }
  pthread_attr_init(&m_attr);

#if defined(__linux__)
  if (m_hugeStackKb || m_tlExtraKb) {
    // If m_hugeStackKb is nonzero but not multiple of the huge page size
    // (size2m), the rest of the huge page is shared with part of the extra
    // storage colocated with the stack, like the following.
    //
    // m_threadStack + m_stackAllocSize ---> +------------+
    //                                       . extra      .
    //                                       . storage    .
    //                                       | for the    | ---------------
    //                                       | thread     |             ^
    //                                       | (RDS/slab) |             |
    //                        pthreads  ---> +------------+  huge page  |
    //                                       | TCB        |   ^         |
    //                                       | TLS        | hugeStack   |
    //                                       | Stack      |   v         v
    //                                       .            . ---------------
    //                                       .            .
    //                    m_threadStack ---> +------------+
    //
    assertx(m_hugeStackKb % 4 == 0);
    auto const hugeStartOffset = rlim.rlim_cur - m_hugeStackKb * 1024;

    constexpr unsigned hugePageSizeKb = 2048u;
    auto const stackPartialHugeKb = m_hugeStackKb % hugePageSizeKb;
    auto const nHugePages = m_hugeStackKb / hugePageSizeKb +
      (stackPartialHugeKb != 0) /* partly stack */;
    m_stackAllocSize = std::max(
      rlim.rlim_cur + m_tlExtraKb * 1024,
      hugeStartOffset + size2m * nHugePages
    );
    m_threadStack = mmap_offset_aligned(m_stackAllocSize,
                                        hugeStartOffset,
                                        nHugePages ? size2m : size4k);
    madvise(m_threadStack, m_stackAllocSize, MADV_DONTNEED);
    numa_bind_to(m_threadStack, m_stackAllocSize, m_node);
    if (nHugePages) {
      auto const hugeStart = m_threadStack + hugeStartOffset;
      assertx(reinterpret_cast<uintptr_t>(hugeStart) % size2m == 0);
      for (size_t i = 0; i < nHugePages; i++) {
        remap_2m(hugeStart + i * size2m, m_node);
      }
      m_hugePages = MemBlock { hugeStart, nHugePages * size2m };
    }
    if (m_tlExtraKb) {
      m_tlExtraBase = m_threadStack + rlim.rlim_cur;
    }
  }
#endif

  if (!m_threadStack) {
    m_threadStack =
      (char*)mmap(nullptr, rlim.rlim_cur, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m_threadStack == MAP_FAILED) {
      m_threadStack = nullptr;
    } else {
      m_stackAllocSize = rlim.rlim_cur;
      madvise(m_threadStack, m_stackAllocSize, MADV_DONTNEED);
      numa_bind_to(m_threadStack, m_stackAllocSize, m_node);
    }
  }

  if (m_threadStack) {
    size_t guardsize;
    if (pthread_attr_getguardsize(&m_attr, &guardsize) == 0 && guardsize) {
      mprotect(m_threadStack, guardsize, PROT_NONE);
    }
    pthread_attr_setstack(&m_attr, m_threadStack, rlim.rlim_cur);
  }

  pthread_create(&m_threadId, &m_attr, ThreadFunc, (void*)this);
  assert(m_threadId);
  s_count++;
}

void AsyncFuncImpl::cancel() {
  pthread_cancel(m_threadId);
}

bool AsyncFuncImpl::waitForEnd(int seconds /* = 0 */) {
  if (m_threadId == 0) return true;

  {
    Lock lock(m_stopMonitor.getMutex());
    while (!m_stopped) {
      if (seconds > 0) {
        if (!m_stopMonitor.wait(seconds)) {
          // wait timed out
          return false;
        }
      } else if (seconds < 0) {
        // Don't wait.
        return false;
      } else {
        // Wait with no timeout.
        m_stopMonitor.wait();
      }
    }
  }

  void *ret = nullptr;
  pthread_join(m_threadId, &ret);
  s_count--;
  m_threadId = 0;

  if (m_threadStack != nullptr) {
    size_t guardsize;
    if (pthread_attr_getguardsize(&m_attr, &guardsize) == 0 && guardsize) {
      mprotect(m_threadStack, guardsize, PROT_READ | PROT_WRITE);
    }
    munmap(m_threadStack, m_stackAllocSize);
    m_threadStack = nullptr;
  }

  if (Exception* e = m_exception) {
    m_exception = 0;
    e->throwException();
  }

  return true;
}

void AsyncFuncImpl::setThreadName() {
#ifdef HAVE_NUMA
  static constexpr size_t kMaxCommNameLen = 16;
  char name[kMaxCommNameLen];

  if (!m_threadGroupSuffix.empty()) {
    if (use_numa) {
      snprintf(name, sizeof(name), "hhvmworker%s.ND%d",
               m_threadGroupSuffix.c_str(), m_node);
    } else {
      snprintf(name, sizeof(name), "hhvmworker%s", m_threadGroupSuffix.c_str());
    }
  } else {
    if (use_numa) {
      snprintf(name, sizeof(name), "hhvmworker.ND%d", m_node);
    } else {
      snprintf(name, sizeof(name), "hhvmworker");
    }
  }
  prctl(PR_SET_NAME, name);
#endif
}

void AsyncFuncImpl::threadFuncImpl() {
  if (s_initFunc && !m_noInitFini) {
    s_initFunc(s_initFuncArg);
  }
  try {
    m_func(m_obj);
  } catch (Exception& e) {
    m_exception = e.clone();
  } catch (std::exception& e) {
    m_exception = new Exception(std::string{e.what()});
  } catch (...) {
    m_exception = new Exception("(unknown exception)");
  }
  {
    Lock lock(m_stopMonitor.getMutex());
    m_stopped = true;
    m_stopMonitor.notify();
  }
  if (s_finiFunc && !m_noInitFini) {
    s_finiFunc(s_finiFuncArg);
  }
}

///////////////////////////////////////////////////////////////////////////////
}
