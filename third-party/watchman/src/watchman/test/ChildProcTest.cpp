/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GTest.h>
#ifdef _WIN32
#include <folly/portability/Windows.h>
#endif
#include <folly/test/TestUtils.h>
#include <list>
#include "watchman/ChildProcess.h"

using watchman::ChildProcess;
using Options = ChildProcess::Options;

TEST(ChildProcess, pipe) {
  Options opts;
  opts.pipeStdout();
  ChildProcess echo(
      {
#ifdef _WIN32
          "cmd /c echo hello",
#else
          "echo",
          "hello",
#endif
      },
      std::move(opts));

  auto outputs = echo.communicate();
  EXPECT_EQ(0, echo.wait())
      << "\nstdout:\n"
      << outputs.first.value() << "\nstderr:" << outputs.second.value() << "\n";

  w_string_piece line(outputs.first.value());
  EXPECT_TRUE(line.startsWith("hello"))
      << "\nstdout:\n"
      << outputs.first.value() << "\nstderr:" << outputs.second.value() << "\n";
}

void test_pipe_input(bool threaded) {
#ifndef _WIN32
  Options opts;
  opts.pipeStdout();
  opts.pipeStdin();
  ChildProcess cat({"cat", "-"}, std::move(opts));

  std::vector<std::string> expected{"one", "two", "three"};
  std::list<std::string> lines{"one\n", "two\n", "three\n"};

  auto writable = [&lines](watchman::FileDescriptor& fd) {
    if (lines.empty()) {
      return true;
    }
    auto str = lines.front();
    if (write(fd.fd(), str.data(), str.size()) == -1) {
      throw std::runtime_error("write to child failed");
    }
    lines.pop_front();
    return false;
  };

  auto outputs =
      threaded ? cat.threadedCommunicate(writable) : cat.communicate(writable);
  cat.wait();

  std::vector<std::string> resultLines;
  w_string_piece(outputs.first.value()).split(resultLines, '\n');
  EXPECT_EQ(resultLines.size(), 3);
  EXPECT_EQ(resultLines, expected);
#else
  (void)threaded;
#endif
}

TEST(ChildProcess, stresstest_pipe_output) {
  SKIP_IF(folly::kIsWindows);

  bool okay = true;
  for (int i = 0; i < 3000; ++i) {
    Options opts;
    opts.pipeStdout();
    ChildProcess proc({"head", "-n20", "/dev/urandom"}, std::move(opts));
    auto outputs = proc.communicate();
    w_string_piece out(outputs.first.value());
    proc.wait();
    if (out.empty() || out[out.size() - 1] != '\n') {
      okay = false;
      break;
    }
  }
  EXPECT_TRUE(okay);
}

void test_empty_piped_stdout(bool threaded) {
#ifndef _WIN32
  Options opts;
  opts.pipeStdout();
  ChildProcess proc({"true"}, std::move(opts));

  auto noInput = [](watchman::FileDescriptor&) { return true; };
  auto outputs =
      threaded ? proc.threadedCommunicate(noInput) : proc.communicate(noInput);
  proc.wait();

  // A piped stream that produced nothing is empty; one that was not piped
  // is absent.
  if (threaded) {
    ASSERT_TRUE(outputs.first.has_value());
    EXPECT_TRUE(outputs.first->empty());
  } else {
    // FIXME(T296478245): pollingCommunicate(), which communicate() uses on
    // POSIX, gives std::nullopt for the empty piped stdout, where
    // threadedCommunicate() gives "". It should give "" too; the next diff
    // fixes it.
    EXPECT_FALSE(outputs.first.has_value());
  }
  EXPECT_FALSE(outputs.second.has_value());
#else
  (void)threaded;
#endif
}

TEST(ChildProcess, emptyPipedStdoutThreaded) {
  test_empty_piped_stdout(true);
}

TEST(ChildProcess, emptyPipedStdoutNotThreaded) {
  test_empty_piped_stdout(false);
}

TEST(ChildProcess, inputThreaded) {
  test_pipe_input(true);
}

TEST(ChildProcess, inputNotThreaded) {
  test_pipe_input(false);
}

#ifdef _WIN32
TEST(ChildProcess, releasesHandlesAfterWait) {
  {
    ChildProcess warmup({"cmd /c exit 0"}, Options{});
    ASSERT_EQ(0, warmup.wait());
  }

  DWORD handlesBefore = 0;
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));

  constexpr int kSpawnCount = 64;
  for (int i = 0; i < kSpawnCount; ++i) {
    ChildProcess child({"cmd /c exit 0"}, Options{});
    ASSERT_EQ(0, child.wait());
  }

  DWORD handlesAfter = 0;
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
  EXPECT_LE(handlesAfter, handlesBefore + 2);
}
#endif
