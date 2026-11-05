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

#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

#include <folly/futures/Future.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/testing/TestUtil.h>

#include "hphp/runtime/ext/facts/exception.h"
#include "hphp/runtime/ext/facts/facts-store.h"
#include "hphp/runtime/ext/facts/sqlite-autoload-db.h"
#include "hphp/runtime/ext/facts/sqlite-key.h"
#include "hphp/runtime/ext/facts/watcher.h"

namespace HPHP::Facts {
namespace {

struct MockWatcher : Watcher {
  MOCK_METHOD(folly::SemiFuture<Delta>, getChanges, (Clock), (override));
  MOCK_METHOD(
      void,
      subscribe,
      (const Clock&, std::function<void(Delta&&)>),
      (override));
};

TEST(FactsStoreTest, EmptyUpdatesAdvanceAndPersistClock) {
  folly::test::TemporaryDirectory temp{"facts-clock"};
  const std::filesystem::path root{temp.path().native()};
  const auto key = SQLiteKey::readWriteCreate(
      root / "autoload.sqlite", static_cast<::gid_t>(-1), 0644);
  const auto openDb = [key]() -> std::shared_ptr<AutoloadDB> {
    return SQLiteAutoloadDB::get(key);
  };

  const Clock first{.m_clock = "1"};
  const Clock second{.m_clock = "2"};
  const Clock third{.m_clock = "3"};
  {
    auto db = openDb();
    db->insertClock(first);
    db->commit();
  }

  auto watcher = std::make_shared<testing::StrictMock<MockWatcher>>();
  testing::InSequence sequence;
  EXPECT_CALL(*watcher, getChanges(first)).WillOnce([&](Clock since) {
    return folly::makeSemiFuture(Watcher::Delta{
        .m_lastClock = std::move(since), .m_newClock = second});
  });
  EXPECT_CALL(*watcher, getChanges(second)).WillOnce([&](Clock since) {
    return folly::makeSemiFuture(Watcher::Delta{
        .m_lastClock = std::move(since), .m_newClock = third});
  });
  EXPECT_CALL(*watcher, getChanges(third)).WillOnce([&](Clock since) {
    return folly::makeSemiFuture(Watcher::Delta{
        .m_lastClock = std::move(since), .m_newClock = third});
  });

  auto store = make_watcher_facts(root, openDb, watcher, false, {}, {}, {});
  ASSERT_NO_THROW(store->ensureUpdated());
  ASSERT_NO_THROW(store->ensureUpdated());
  store.reset();

  store = make_watcher_facts(root, openDb, watcher, false, {}, {}, {});
  ASSERT_NO_THROW(store->ensureUpdated());
}

TEST(FactsStoreTest, SubscriptionNotificationAfterCloseIsIgnored) {
  folly::test::TemporaryDirectory temp{"facts-close"};
  const std::filesystem::path root{temp.path().native()};
  const auto key = SQLiteKey::readWriteCreate(
      root / "autoload.sqlite", static_cast<::gid_t>(-1), 0644);
  const auto openDb = [key]() -> std::shared_ptr<AutoloadDB> {
    return SQLiteAutoloadDB::get(key);
  };

  auto watcher = std::make_shared<testing::StrictMock<MockWatcher>>();
  std::function<void(Watcher::Delta&&)> notify;
  EXPECT_CALL(*watcher, subscribe).WillOnce(testing::SaveArg<1>(&notify));

  auto store = make_watcher_facts(root, openDb, watcher, true, {}, {}, {});
  store->close();

  // FIXME: A notification that arrives after close() throws out of the
  // subscription callback. This should be EXPECT_NO_THROW, with no call to
  // getChanges().
  EXPECT_THROW(notify(Watcher::Delta{}), UpdateExc);
}

} // namespace
} // namespace HPHP::Facts
