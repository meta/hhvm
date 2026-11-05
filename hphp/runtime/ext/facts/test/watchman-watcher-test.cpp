/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include "hphp/runtime/base/watchman.h"
#include "hphp/runtime/ext/facts/exception.h"
#include "hphp/runtime/ext/facts/file-facts.h"
#include "hphp/runtime/ext/facts/watchman-watcher.h"

using ::testing::ByMove;
using ::testing::ElementsAre;
using ::testing::Return;
using ::testing::WithArg;

namespace HPHP {
namespace Facts {
namespace {

struct MockWatchman final : public Watchman {
  MOCK_METHOD(
      folly::SemiFuture<folly::dynamic>,
      query,
      (folly::dynamic),
      (override));

  MOCK_METHOD(folly::SemiFuture<watchman::Clock>, getClock, (), (override));

  MOCK_METHOD(
      void,
      subscribe,
      (const folly::dynamic& queryObj,
       watchman::SubscriptionCallback&& callback),
      (override));
};

TEST(WatchmanWatcherTest, sinceAndClockArePassedThrough) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher =
      make_watchman_watcher(folly::dynamic::object(), mockWatchman, {});

  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(folly::dynamic::object(
              "clock", "this is the new clock")("is_fresh_instance", false)))));

  auto since = Clock{.m_clock = "this is the old clock"};
  auto result = watcher->getChanges(since).get();
  EXPECT_EQ(result.m_lastClock, since);
  EXPECT_EQ(result.m_newClock, Clock{.m_clock = "this is the new clock"});
}

TEST(WatchmanWatcherTest, filesAndExistenceArePassedThrough) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher =
      make_watchman_watcher(folly::dynamic::object(), mockWatchman, {});

  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(
              folly::dynamic::object("clock", "2")("is_fresh_instance", false)(
                  "files",
                  folly::dynamic::array(
                      folly::dynamic::object("name", "a.hck")("exists", true)(
                          "content.sha1hex", "faceb00c"),
                      folly::dynamic::object("name", "b.hck")(
                          "exists", false)))))));

  auto results = watcher->getChanges(Clock{}).get();
  EXPECT_THAT(
      results.m_files,
      ElementsAre(
          Watcher::FileDelta{
              .m_path = "a.hck",
              .m_exists = true,
              .m_watcher_hash = "faceb00c"},
          Watcher::FileDelta{.m_path = "b.hck", .m_exists = false}));
}

TEST(WatchmanWatcherTest, malformedWatchmanOutput) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher = make_watchman_watcher(
      folly::dynamic::object(), mockWatchman, {.m_retries = 0});

  // No "clock" field
  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(folly::dynamic::object))));
  EXPECT_THROW(watcher->getChanges({}).get(), UpdateExc);

  // "clock" field is an empty object instead of a string
  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(
              folly::dynamic::object("clock", folly::dynamic::object)))));
  EXPECT_THROW(watcher->getChanges({}).get(), UpdateExc);
}

TEST(WatchmanWatcherTest, querySinceMergebaseIsNotFresh) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher =
      make_watchman_watcher(folly::dynamic::object(), mockWatchman, {});

  // If you didn't give Watchman a local clock, Watchman will return
  // `is_fresh_instance: true` even if you gave it a mergebase. Results from
  // these queries are not actually fresh as far as we're concerned.
  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(folly::dynamic::object(
              "clock", "1")("is_fresh_instance", true)))))
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(folly::dynamic::object(
              "clock", "2")("is_fresh_instance", true)))));

  // This query is asking for all files in the repo, not since a given point in
  // time or since a given commit. This is actually fresh from our perspective.
  Clock sinceWithoutMergebase;
  auto resultsWithoutMergebase = watcher->getChanges(Clock{}).get();
  EXPECT_TRUE(resultsWithoutMergebase.m_fresh);

  // This query is based off of a mergebase commit hash, so we don't consider it
  // fresh - Watchman is not returning all the files in the repo, it's returning
  // all the files since a given point in time.
  auto resultsWithMergebase =
      watcher->getChanges(Clock{.m_mergebase = "faceb00c"}).get();
  EXPECT_FALSE(resultsWithMergebase.m_fresh);
}

struct WatchmanFailure : public std::runtime_error {
  explicit WatchmanFailure(std::string msg)
      : std::runtime_error{std::move(msg)} {}
};

TEST(WatchmanWatcherTest, RetryOnFailure) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher = make_watchman_watcher(
      folly::dynamic::object(), mockWatchman, {.m_retries = 1});

  // Exercise retries by failing the first query.
  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(
              WatchmanFailure{"Watchman error"}))))
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(folly::dynamic::object(
              "clock", "1")("is_fresh_instance", true)))));

  Clock since;
  auto results = watcher->getChanges(Clock{}).get();
  EXPECT_TRUE(results.m_fresh);
  EXPECT_EQ(results.m_newClock, Clock{.m_clock = "1"});
}

TEST(WatchmanWatcherTest, ThrowAfterRetrying) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher = make_watchman_watcher(
      folly::dynamic::object(), mockWatchman, {.m_retries = 1});

  // Fail twice.
  EXPECT_CALL(*mockWatchman, query)
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(
              std::runtime_error{"Ignored error"}))))
      .WillOnce(Return(ByMove(
          folly::makeSemiFuture<folly::dynamic>(
              WatchmanFailure{"Watchman error"}))));

  Clock since;
  EXPECT_THROW(watcher->getChanges(Clock{}).get(), WatchmanFailure);
}

TEST(WatchmanWatcherTest, HgTransactionUpdate) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  auto watcher = make_watchman_watcher(
      folly::dynamic::object(), mockWatchman, {.m_retries = 0});

  // Invoke the subscribe callback with a {"state-enter": "hg.transaction"}
  // message
  EXPECT_CALL(*mockWatchman, subscribe)
      .WillOnce(WithArg<1>([&](watchman::SubscriptionCallback&& cb) {
        // clang-format off
        folly::dynamic msg = folly::dynamic::object
	  ("clock", "2")
	  ("version", "2022-05-06T03:03:19Z")
	  ("metadata", folly::dynamic::object
	     ("rev", "91f14be170109d514e9115b909d62114cabe5412")
	     ("distance", 0)
	     ("partial", false)
	     ("status", "ok"))
	  ("root", "/var/www")
	  ("state-enter", "hg.transaction")
	  ("unilateral", true)
	  ("subscription", "sub1");
        // clang-format on
        cb(folly::Try{std::move(msg)});
      }));

  Clock oldClock{.m_clock = "1"};
  watcher->subscribe(oldClock, [&](Watcher::Delta&& delta) {
    EXPECT_FALSE(delta.m_lastClock);
    EXPECT_EQ(delta.m_newClock, Clock{.m_clock = "2"});
    EXPECT_FALSE(delta.m_fresh);
    EXPECT_TRUE(delta.m_files.empty());
  });
}

TEST(WatchmanWatcherTest, subscriptionsFromEachStoreShareOneQuery) {
  auto mockWatchman = std::make_shared<MockWatchman>();
  std::vector<folly::dynamic> subscribedQueries;
  EXPECT_CALL(*mockWatchman, subscribe)
      .Times(2)
      .WillRepeatedly(WithArg<0>([&](const folly::dynamic& queryObj) {
        subscribedQueries.push_back(queryObj);
      }));

  // Every FactsStore for a root makes its own watcher and subscribes with its
  // own clock.
  folly::dynamic queryExpr =
      folly::dynamic::object("expression", folly::dynamic::array("type", "f"))(
          "suffix", folly::dynamic::array("php"));
  make_watchman_watcher(queryExpr, mockWatchman, {})
      ->subscribe(Clock{.m_clock = "c:1"}, [](Watcher::Delta&&) {});
  make_watchman_watcher(queryExpr, mockWatchman, {})
      ->subscribe(Clock{.m_clock = "c:2"}, [](Watcher::Delta&&) {});

  auto expectedQuery = [](std::string since) -> folly::dynamic {
    return folly::dynamic::object(
        "expression", folly::dynamic::array("type", "f"))(
        "fields", folly::dynamic::array("name", "exists", "content.sha1hex"))(
        "since", std::move(since));
  };
  // FIXME: Each store's clock is part of its subscription query, so every
  // store adds a watchman subscription that HHVM re-sends with its old clock on
  // every reconnect, and after a watchman restart, watchman answers each of
  // those clocks by walking the whole repo. Both stores should send
  // `queryExpr` with the fields and `"empty_on_fresh_instance": true` instead,
  // and no `since`.
  EXPECT_THAT(
      subscribedQueries,
      ElementsAre(expectedQuery("c:1"), expectedQuery("c:2")));
}

} // namespace
} // namespace Facts
} // namespace HPHP
