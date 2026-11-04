/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/cprof-replay.h"

#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <folly/FileUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

namespace HPHP::jit::cprof {
namespace {

TEST(ContProfReplay, AbandonedAttemptDiscardsOnlyAggregateAndSkipsReplay) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  auto const path = directory.path().native();
  auto const checkpoint = (directory.path() / "jit.cprof").native();
  auto const nested = (directory.path() / "cont-prof-3-3.cprof").native();
  for (auto const name : {
    "jit.cprof", "cont-prof-1-1.cprof", "cont-prof-2-2.cprof",
    "jit.cprof.tmp", "cont-prof-5-5.cprof.tmp", "unrelated"
  }) {
    ASSERT_TRUE(folly::writeFile(
      std::string{"checkpoint"}, (directory.path() / name).c_str()
    ));
  }
  ASSERT_TRUE(std::filesystem::create_directory(nested));
  ASSERT_TRUE(folly::writeFile(
    std::string{"checkpoint"}, (nested + "/cont-prof-6-6.cprof").c_str()
  ));
  std::filesystem::create_symlink(
    "unrelated", (directory.path() / "cont-prof-4-4.cprof").native()
  );

  {
    ContProfReplayGuard replay{path};
    ASSERT_TRUE(replay.active());
  }

  {
    ContProfReplayGuard replay{path};
    EXPECT_FALSE(replay.active());
    EXPECT_FALSE(replay.complete());
  }
  std::set<std::string> remaining;
  for (auto const& entry : std::filesystem::directory_iterator{path}) {
    remaining.insert(entry.path().filename().native());
  }
  std::set<std::string> const expected{
    "cont-prof-1-1.cprof", "cont-prof-2-2.cprof",
    "cont-prof-3-3.cprof", "cont-prof-4-4.cprof",
    "cont-prof-5-5.cprof.tmp", "cont-prof-replay.lock",
    "jit.cprof.tmp", "unrelated"
  };
  EXPECT_EQ(expected, remaining);
  EXPECT_TRUE(std::filesystem::exists(nested + "/cont-prof-6-6.cprof"));
  EXPECT_EQ(0, std::filesystem::file_size(
    (directory.path() / "cont-prof-replay.lock").native()
  ));

  ASSERT_TRUE(folly::writeFile(
    std::string{"fresh checkpoint"}, checkpoint.c_str()
  ));
  ContProfReplayGuard replay{path};
  ASSERT_TRUE(replay.active());
  EXPECT_TRUE(replay.complete());
  EXPECT_TRUE(std::filesystem::exists(checkpoint));
}

TEST(ContProfReplay, AbandonedAttemptWithoutRegularCheckpointClearsMarker) {
  using FileType = std::filesystem::file_type;
  for (auto const type : {
    FileType::not_found, FileType::directory, FileType::symlink
  }) {
    SCOPED_TRACE(static_cast<int>(type));
    folly::test::TemporaryDirectory directory{"cont-prof-replay"};
    auto const path = directory.path().native();
    auto const checkpoint = (directory.path() / "jit.cprof").native();
    auto const legacy = (directory.path() / "cont-prof-1-1.cprof").native();
    ASSERT_TRUE(folly::writeFile(std::string{"checkpoint"}, legacy.c_str()));
    if (type == FileType::directory) {
      ASSERT_TRUE(std::filesystem::create_directory(checkpoint));
    } else if (type == FileType::symlink) {
      std::filesystem::create_symlink(legacy, checkpoint);
    }

    {
      ContProfReplayGuard replay{path};
      ASSERT_TRUE(replay.active());
    }
    {
      ContProfReplayGuard replay{path};
      EXPECT_FALSE(replay.active());
      EXPECT_FALSE(replay.complete());
    }
    EXPECT_EQ(type, std::filesystem::symlink_status(checkpoint).type());
    EXPECT_TRUE(std::filesystem::exists(legacy));
    EXPECT_EQ(0, std::filesystem::file_size(
      (directory.path() / "cont-prof-replay.lock").native()
    ));

    ContProfReplayGuard replay{path};
    ASSERT_TRUE(replay.active());
    EXPECT_TRUE(replay.complete());
  }
}

TEST(ContProfReplay, SuccessfulCompletionAllowsLaterReplay) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  auto const path = directory.path().native();
  auto const checkpoint = (directory.path() / "jit.cprof").native();
  ASSERT_TRUE(folly::writeFile(std::string{"checkpoint"}, checkpoint.c_str()));

  for (int attempt = 0; attempt < 2; ++attempt) {
    [&] {
      ContProfReplayGuard replay{path};
      ASSERT_TRUE(replay.active());
      SCOPE_SUCCESS { EXPECT_TRUE(replay.complete()); };
      return;
    }();
  }
  EXPECT_TRUE(std::filesystem::exists(checkpoint));
  EXPECT_EQ(0, std::filesystem::file_size(
    (directory.path() / "cont-prof-replay.lock").native()
  ));
}

TEST(ContProfReplay, ExceptionLeavesMarkerSet) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  auto const path = directory.path().native();

  EXPECT_THROW(([&] {
    ContProfReplayGuard replay{path};
    ASSERT_TRUE(replay.active());
    SCOPE_SUCCESS { replay.complete(); };
    throw std::runtime_error{"interrupted replay"};
  }()), std::runtime_error);

  EXPECT_NE(0, std::filesystem::file_size(
    (directory.path() / "cont-prof-replay.lock").native()
  ));
}

TEST(ContProfReplay, FailedCleanupLeavesMarkerForNextStartup) {
  if (::geteuid() == 0) GTEST_SKIP() << "Requires directory permission checks";

  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  auto const path = directory.path().native();
  auto const checkpoint = (directory.path() / "jit.cprof").native();
  ASSERT_TRUE(folly::writeFile(std::string{"checkpoint"}, checkpoint.c_str()));

  {
    ContProfReplayGuard replay{path};
    ASSERT_TRUE(replay.active());
  }

  {
    auto const permissions = std::filesystem::status(path).permissions();
    SCOPE_EXIT { std::filesystem::permissions(path, permissions); };
    std::filesystem::permissions(
      path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec
    );

    ContProfReplayGuard replay{path};
    EXPECT_FALSE(replay.active());
    EXPECT_TRUE(std::filesystem::exists(checkpoint));
    EXPECT_NE(0, std::filesystem::file_size(
      (directory.path() / "cont-prof-replay.lock").native()
    ));
  }

  {
    ContProfReplayGuard replay{path};
    EXPECT_FALSE(replay.active());
    EXPECT_FALSE(std::filesystem::exists(checkpoint));
  }

  ContProfReplayGuard replay{path};
  ASSERT_TRUE(replay.active());
  EXPECT_TRUE(replay.complete());
}

TEST(ContProfReplay, ContendedAttemptDoesNotChangeOwner) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  auto const path = directory.path().native();
  auto const checkpoint = (directory.path() / "jit.cprof").native();
  ASSERT_TRUE(folly::writeFile(std::string{"checkpoint"}, checkpoint.c_str()));

  {
    ContProfReplayGuard owner{path};
    ASSERT_TRUE(owner.active());

    ContProfReplayGuard contender{path};
    EXPECT_FALSE(contender.active());
    EXPECT_TRUE(std::filesystem::exists(checkpoint));
    EXPECT_TRUE(owner.complete());

    // Even a clean marker cannot be reused while its owner holds the lease.
    ContProfReplayGuard afterCompletion{path};
    EXPECT_FALSE(afterCompletion.active());
  }

  ContProfReplayGuard replay{path};
  ASSERT_TRUE(replay.active());
  EXPECT_TRUE(replay.complete());
}

TEST(ContProfReplay, MissingDirectorySkipsReplay) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  ContProfReplayGuard replay{(directory.path() / "missing").native()};
  EXPECT_FALSE(replay.active());
  EXPECT_FALSE(replay.complete());
}

TEST(ContProfReplay, UnwritableMarkerPathSkipsReplay) {
  folly::test::TemporaryDirectory directory{"cont-prof-replay"};
  ASSERT_TRUE(std::filesystem::create_directory(
    (directory.path() / "cont-prof-replay.lock").native()
  ));

  ContProfReplayGuard replay{directory.path().native()};
  EXPECT_FALSE(replay.active());
  EXPECT_FALSE(replay.complete());
}

}
}
