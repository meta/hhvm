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

#include "hphp/util/assertions.h"

#include <folly/FileUtil.h>
#include <roar-c/ROARIPCMessaging.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <sys/mman.h>
#include <unistd.h>

namespace HPHP {

struct ROARPidMapFile {
  ROARPidMapFile() {
    m_fd = memfd_create("hhvm-roar-pid-map", MFD_CLOEXEC);
    if (m_fd < 0) m_error = errno;
  }

  ~ROARPidMapFile() {
    assertx(m_used == 0 || m_error);
    if (m_fd >= 0) close(m_fd);
  }
  ROARPidMapFile(const ROARPidMapFile&) = delete;
  ROARPidMapFile& operator=(const ROARPidMapFile&) = delete;

  int fd() const { return m_fd; }

  int append(uint64_t address, uint64_t size, const char* name) {
    if (m_error) return m_error;
    m_buffer[m_used++] = CreateIPCPidMapEntry(
      reinterpret_cast<void*>(address), size, name
    );
    return m_used == m_buffer.size() ? flush() : 0;
  }

  int flush() {
    if (m_error) return m_error;
    auto const bytes = m_used * sizeof(m_buffer[0]);
    if (!bytes) return 0;
    auto const written = folly::writeFull(m_fd, m_buffer.data(), bytes);
    if (written < 0) return m_error = errno;
    if (size_t(written) != bytes) return m_error = EIO;
    m_used = 0;
    return 0;
  }

private:
  std::array<ROARIPCPidMapEntry, 16> m_buffer;
  size_t m_used{0};
  int m_fd{-1};
  int m_error{0};
};

}
