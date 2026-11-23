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

#include "hphp/util/roar-pid-map.h"

#include <folly/FileUtil.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

namespace HPHP {

TEST(ROARPidMapFile, RecordBoundaries) {
  for (auto const count : std::array<size_t, 6>{0, 1, 15, 16, 17, 4097}) {
    SCOPED_TRACE(count);
    ROARPidMapFile file;
    ASSERT_GE(file.fd(), 0);
    for (size_t i = 0; i < count; ++i) {
      auto const name = "PHP::function_" + std::to_string(i);
      ASSERT_EQ(file.append(0x1234567800000000 + i, 32 + i, name.c_str()), 0);
    }
    ASSERT_EQ(file.flush(), 0);
    struct stat st;
    ASSERT_EQ(fstat(file.fd(), &st), 0);
    EXPECT_EQ(st.st_size, count * sizeof(ROARIPCPidMapEntry));
    EXPECT_EQ(lseek(file.fd(), 0, SEEK_CUR), st.st_size);
    std::vector<ROARIPCPidMapEntry> entries(count);
    ASSERT_EQ(folly::preadFull(file.fd(), entries.data(), st.st_size, 0),
              st.st_size);
    EXPECT_EQ(lseek(file.fd(), 0, SEEK_CUR), st.st_size);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(entries[i].Address, 0x1234567800000000 + i);
      EXPECT_EQ(entries[i].Size, 32 + i);
      EXPECT_EQ(std::string{entries[i].Name},
                "PHP::function_" + std::to_string(i));
    }
  }
}

TEST(ROARPidMapFile, NameTruncationAndBufferReuse) {
  ROARPidMapFile file;
  auto const longName = std::string(512, 'a');
  for (size_t i = 0; i < 16; ++i) {
    ASSERT_EQ(file.append(i, 7, longName.c_str()), 0);
  }
  ASSERT_EQ(file.append(16, 8, "short"), 0);
  char const embeddedNul[] = "before\0after";
  ASSERT_EQ(file.append(17, 9, embeddedNul), 0);
  ASSERT_EQ(file.flush(), 0);
  std::array<ROARIPCPidMapEntry, 18> entries;
  ASSERT_EQ(folly::preadFull(file.fd(), entries.data(), sizeof(entries), 0),
            sizeof(entries));
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(std::string{entries[i].Name}, std::string(255, 'a'));
    EXPECT_EQ(entries[i].Name[255], '\0');
  }
  EXPECT_STREQ(entries[16].Name, "short");
  EXPECT_STREQ(entries[17].Name, "before");
  for (size_t i = 6; i < 256; ++i) EXPECT_EQ(entries[16].Name[i], '\0');
  for (size_t i = 7; i < 256; ++i) EXPECT_EQ(entries[17].Name[i], '\0');
}

TEST(ROARPidMapFile, CloseOnExec) {
  ROARPidMapFile file;
  ASSERT_EQ(file.append(1, 2, "name"), 0);
  ASSERT_EQ(file.flush(), 0);
  EXPECT_NE(fcntl(file.fd(), F_GETFD) & FD_CLOEXEC, 0);
}

TEST(ROARPidMapFile, OwnedDescriptorAndIndependentReceiverLifetime) {
  int fd;
  int receiverFd;
  {
    ROARPidMapFile file;
    ASSERT_EQ(file.append(42, 64, "owned name"), 0);
    ASSERT_EQ(file.flush(), 0);
    fd = file.fd();
    receiverFd = dup(fd);
    ASSERT_GE(receiverFd, 0);
  }
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  ROARIPCPidMapEntry entry;
  EXPECT_EQ(folly::preadFull(receiverFd, &entry, sizeof(entry), 0),
            sizeof(entry));
  EXPECT_EQ(entry.Address, 42);
  EXPECT_STREQ(entry.Name, "owned name");
  EXPECT_EQ(close(receiverFd), 0);
}

TEST(ROARPidMapFile, WriteFailureIsRetained) {
  ROARPidMapFile file;
  ASSERT_GE(file.fd(), 0);
  for (size_t i = 0; i < 16; ++i) {
    ASSERT_EQ(file.append(i, 2, "written"), 0);
  }
  struct stat before;
  ASSERT_EQ(fstat(file.fd(), &before), 0);
  ASSERT_GT(before.st_size, 0);

  auto const path = "/proc/self/fd/" + std::to_string(file.fd());
  auto const readOnlyFd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(readOnlyFd, 0);
  ASSERT_EQ(dup2(readOnlyFd, file.fd()), file.fd());
  ASSERT_EQ(close(readOnlyFd), 0);
  int error = 0;
  for (size_t i = 16; i < 64; ++i) {
    error = file.append(i, 2, "unwritten");
    if (error) break;
  }
  ASSERT_EQ(error, EBADF);
  EXPECT_EQ(file.append(64, 2, "later"), EBADF);
  EXPECT_EQ(file.flush(), EBADF);
  struct stat after;
  ASSERT_EQ(fstat(file.fd(), &after), 0);
  EXPECT_EQ(after.st_size, before.st_size);
}

}
