/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/InMemoryView.h"
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GTest.h>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "watchman/fs/FSDetect.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/root/Root.h"
#include "watchman/test/lib/FakeFileSystem.h"
#include "watchman/test/lib/FakeWatcher.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_dir.h"
#include "watchman/watchman_file.h"

namespace {

using namespace watchman;

Configuration getConfiguration(bool usePwalk) {
  json_ref json = json_object();
  json_object_set(json, "enable_parallel_crawl", json_boolean(usePwalk));
  return Configuration{std::move(json)};
}

class InMemoryViewTest : public testing::TestWithParam<bool /* pwalk */> {
 public:
  using Continue = InMemoryView::Continue;

  const w_string root_path{FAKEFS_ROOT "root"};

  FakeFileSystem fs;
  Configuration config = getConfiguration(GetParam());
  std::shared_ptr<FakeWatcher> watcher = std::make_shared<FakeWatcher>(fs);

  std::shared_ptr<InMemoryView> view =
      std::make_shared<InMemoryView>(fs, root_path, config, watcher);
  PendingCollection& pending = view->unsafeAccessPendingFromWatcher();

  InMemoryViewTest() {
    pending.lock()->ping();
  }

  /**
   * Replaces a file by a directory, a directory by a file and a file by
   * another file between two steps of the iothread, which is told about
   * `reported`. Returns how a since query then describes each changed path.
   */
  std::map<std::string, std::string> changesAfterReplacing(
      const std::vector<std::pair<const char*, PendingFlags>>& reported);
};

TEST_P(InMemoryViewTest, can_construct) {
  fs.defineContents({
      FAKEFS_ROOT "root",
  });

  Root root{
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {}};
}

TEST_P(InMemoryViewTest, drive_initial_crawl) {
  fs.defineContents({FAKEFS_ROOT "root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};

  // This will perform the initial crawl.
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx{&query, root, false};
  view->pathGenerator(&query, &ctx);

  EXPECT_EQ(2, ctx.resultsArray.size());
  EXPECT_STREQ("dir", ctx.resultsArray.at(0).asCString());
  EXPECT_STREQ("dir/file.txt", ctx.resultsArray.at(1).asCString());
}

TEST_P(InMemoryViewTest, respond_to_watcher_events) {
  getLog().setStdErrLoggingLevel(DBG);

  fs.defineContents({FAKEFS_ROOT "root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx1{&query, root, false};
  view->pathGenerator(&query, &ctx1);

  EXPECT_EQ(2, ctx1.resultsArray.size());

  auto one = ctx1.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  auto two = ctx1.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Update filesystem and ensure the query results don't update.

  fs.updateMetadata(FAKEFS_ROOT "root/dir/file.txt", [&](FileInformation& fi) {
    fi.size = 100;
  });
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx2{&query, root, false};
  view->pathGenerator(&query, &ctx2);

  one = ctx2.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx2.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Now notify the iothread of the change, process events, and assert the view
  // updates.
  pending.lock()->add(
      FAKEFS_ROOT "root/dir/file.txt", {}, W_PENDING_VIA_NOTIFY);
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx3{&query, root, false};
  view->pathGenerator(&query, &ctx3);

  one = ctx3.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx3.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(100, two.get("size").asInt());
}

TEST_P(InMemoryViewTest, wait_for_respond_to_watcher_events) {
  getLog().setStdErrLoggingLevel(DBG);

  fs.defineContents({FAKEFS_ROOT "root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx1{&query, root, false};
  view->pathGenerator(&query, &ctx1);

  EXPECT_EQ(2, ctx1.resultsArray.size());

  auto one = ctx1.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  auto two = ctx1.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Update filesystem and ensure the query results don't update.

  fs.updateMetadata(FAKEFS_ROOT "root/dir/file.txt", [&](FileInformation& fi) {
    fi.size = 100;
  });
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx2{&query, root, false};
  view->pathGenerator(&query, &ctx2);

  one = ctx2.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx2.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Now notify the iothread of the change, process events, and assert the view
  // updates.
  pending.lock()->add(
      FAKEFS_ROOT "root/dir/file.txt", {}, W_PENDING_VIA_NOTIFY);
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx3{&query, root, false};
  view->pathGenerator(&query, &ctx3);

  one = ctx3.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx3.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(100, two.get("size").asInt());
}

TEST_P(
    InMemoryViewTest,
    syncToNow_does_not_return_until_cookie_dir_is_crawled) {
  getLog().setStdErrLoggingLevel(DBG);

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"file.txt", 1});

  fs.defineContents({FAKEFS_ROOT "root/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  // Initial crawl

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  // Somebody has updated a file.

  fs.updateMetadata(
      FAKEFS_ROOT "root/file.txt", [&](FileInformation& fi) { fi.size = 100; });

  // We have not seen the new size, so the size should be zero.

  {
    QueryContext ctx{&query, root, false};
    view->pathGenerator(&query, &ctx);

    EXPECT_EQ(1, ctx.resultsArray.size());

    auto one = ctx.resultsArray.at(0);
    EXPECT_STREQ("file.txt", one.get("name").asCString());
    EXPECT_EQ(0, one.get("size").asInt());
  }

  // A query starts, but the watcher has not notified us.

  auto executor = folly::ManualExecutor{};

  // Query, to synchronize, writes a cookie to the filesystem.
  auto syncFuture1 =
      root->cookies.sync().via(folly::Executor::getKeepAliveToken(executor));

  // But we want to know exactly when it unblocks:
  auto syncFuture = std::move(syncFuture1).thenValue([&](auto) {
    // We are running in the iothread, so it is unsafe to access
    // InMemoryView, but this test is trying to simulate another query's thread
    // being unblocked too early. Access the ViewDatabase unsafely because the
    // iothread currently has it locked. That's okay because this test is
    // single-threaded.

    const auto& viewdb = view->unsafeAccessViewDatabase();
    auto* dir = viewdb.resolveDir(FAKEFS_ROOT "root");
    auto* file = dir->getChildFile("file.txt");
    return file->stat.size;
  });

  // Have Watcher publish change to "/root" but this watcher does not have
  // per-file notifications.

  pending.lock()->add(
      FAKEFS_ROOT "root",
      {},
      W_PENDING_VIA_NOTIFY | W_PENDING_NONRECURSIVE_SCAN);

  executor.drain();

  EXPECT_FALSE(syncFuture.isReady());
  // This will notice the cookie and unblock.
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));
  executor.drain();
  EXPECT_TRUE(syncFuture.isReady());

  EXPECT_EQ(100, std::move(syncFuture).get());
}

TEST_P(
    InMemoryViewTest,
    syncToNow_does_not_return_until_all_pending_events_are_processed) {
  getLog().setStdErrLoggingLevel(DBG);

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"dir/file.txt", 1});

  fs.defineContents({FAKEFS_ROOT "root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  // Initial crawl

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  // Somebody has updated a file.

  fs.updateMetadata(FAKEFS_ROOT "root/dir/file.txt", [&](FileInformation& fi) {
    fi.size = 100;
  });

  // We have not seen the new size, so the size should be zero.

  {
    QueryContext ctx{&query, root, false};
    view->pathGenerator(&query, &ctx);

    EXPECT_EQ(1, ctx.resultsArray.size());

    auto one = ctx.resultsArray.at(0);
    EXPECT_STREQ("dir/file.txt", one.get("name").asCString());
    EXPECT_EQ(0, one.get("size").asInt());
  }

  // A query starts, but the watcher has not notified us.

  auto executor = folly::ManualExecutor{};

  // Query, to synchronize, writes a cookie to the filesystem.
  auto syncFuture1 =
      root->cookies.sync().via(folly::Executor::getKeepAliveToken(executor));

  // But we want to know exactly when it unblocks:
  auto syncFuture = std::move(syncFuture1).thenValue([&](auto) {
    // We are running in the iothread, so it is unsafe to access
    // InMemoryView, but this test is trying to simulate another query's thread
    // being unblocked too early. Access the ViewDatabase unsafely because the
    // iothread currently has it locked. That's okay because this test is
    // single-threaded.

    const auto& viewdb = view->unsafeAccessViewDatabase();
    auto* dir = viewdb.resolveDir(FAKEFS_ROOT "root/dir");
    auto* file = dir->getChildFile("file.txt");
    return file->stat.size;
  });

  // Have Watcher publish its change events but this watcher does not have
  // per-file notifications.

  // The Watcher event from the modified file, which was sequenced before the
  // cookie write.
  pending.lock()->add(
      FAKEFS_ROOT "root/dir",
      {},
      W_PENDING_VIA_NOTIFY | W_PENDING_NONRECURSIVE_SCAN);

  // The Watcher event from the cookie.
  pending.lock()->add(
      FAKEFS_ROOT "root",
      {},
      W_PENDING_VIA_NOTIFY | W_PENDING_NONRECURSIVE_SCAN);

  // This will notice the cookie and unblock.
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));
  executor.drain();
  EXPECT_TRUE(syncFuture.isReady());

  EXPECT_EQ(100, std::move(syncFuture).get());
}

TEST_P(
    InMemoryViewTest,
    syncToNow_does_not_return_until_initial_crawl_completes) {
  getLog().setStdErrLoggingLevel(DBG);

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"dir/file.txt", 1});

  fs.defineContents({
      FAKEFS_ROOT "root/dir/file.txt",
  });
  // TODO: add a mode for defining FileInformation with the hierarchy
  fs.updateMetadata(FAKEFS_ROOT "root/dir/file.txt", [&](FileInformation& fi) {
    fi.size = 100;
  });

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  auto executor = folly::ManualExecutor{};

  // A query is immediately issued. To synchronize, a cookie is written.
  auto syncFuture1 =
      root->cookies.sync().via(folly::Executor::getKeepAliveToken(executor));

  // But we want to know exactly when it unblocks:
  auto syncFuture = std::move(syncFuture1).thenValue([&](auto) {
    // We are running in the iothread, so it is unsafe to access
    // InMemoryView, but this test is trying to simulate another query's thread
    // being unblocked too early. Access the ViewDatabase unsafely because the
    // iothread currently has it locked. That's okay because this test is
    // single-threaded.

    const auto& viewdb = view->unsafeAccessViewDatabase();
    auto* dir = viewdb.resolveDir(FAKEFS_ROOT "root/dir");
    auto* file = dir->getChildFile("file.txt");
    EXPECT_EQ(100, file->stat.size);
  });

  executor.drain();

  EXPECT_FALSE(syncFuture.isReady());

  // Initial crawl...

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  executor.drain();

  // ... should unblock the cookie when it's done.
  EXPECT_TRUE(syncFuture.isReady());
  std::move(syncFuture).get();
}

TEST_P(InMemoryViewTest, waitUntilReadyToQuery_waits_for_initial_crawl) {
  getLog().setStdErrLoggingLevel(DBG);

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"dir/file.txt", 1});

  fs.defineContents({
      FAKEFS_ROOT "root/dir/file.txt",
  });
  // TODO: add a mode for defining FileInformation with the hierarchy
  fs.updateMetadata(FAKEFS_ROOT "root/dir/file.txt", [&](FileInformation& fi) {
    fi.size = 100;
  });

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  auto syncFuture = view->waitUntilReadyToQuery();
  EXPECT_FALSE(syncFuture.isReady());

  // Initial crawl...

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  // Unblocks!
  std::move(syncFuture).get();
}

TEST_P(InMemoryViewTest, directory_removal_does_not_report_parent) {
  getLog().setStdErrLoggingLevel(DBG);

  fs.defineContents({
      FAKEFS_ROOT "root/dir/foo/file.txt",
  });

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  auto beforeFirstStep = view->getMostRecentRootNumberAndTickValue();

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx1{&query, root, false};
  ctx1.since = QuerySince::Clock{false, beforeFirstStep.ticks};

  view->timeGenerator(&query, &ctx1);

  ASSERT_EQ(3, ctx1.resultsArray.size());

  auto one = ctx1.resultsArray.at(0);
  EXPECT_EQ("dir/foo/file.txt", ctx1.resultsArray.at(0).asString());
  EXPECT_EQ("dir/foo", ctx1.resultsArray.at(1).asString());
  EXPECT_EQ("dir", ctx1.resultsArray.at(2).asString());

  auto beforeChanges = view->getMostRecentRootNumberAndTickValue();

  // Now remove all of foo/ and notify the iothread of the change as if we are
  // the FSEvents watcher.
  fs.removeRecursively(FAKEFS_ROOT "root/dir/foo");
  pending.lock()->add(
      FAKEFS_ROOT "root/dir/foo",
      {},
      W_PENDING_VIA_NOTIFY | W_PENDING_NONRECURSIVE_SCAN);
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx2{&query, root, false};
  ctx2.since = QuerySince::Clock{false, beforeChanges.ticks};

  view->timeGenerator(&query, &ctx2);

  ASSERT_EQ(2, ctx2.resultsArray.size());
  EXPECT_EQ("dir/foo", ctx2.resultsArray.at(0).asString());
  EXPECT_EQ("dir/foo/file.txt", ctx2.resultsArray.at(1).asString());

  // iothread will not update the view for dir/ until it sees an actual
  // notification from the watcher for that directory.
}

std::map<std::string, std::string> InMemoryViewTest::changesAfterReplacing(
    const std::vector<std::pair<const char*, PendingFlags>>& reported) {
  fs.defineContents({
      FAKEFS_ROOT "root/dir/was_file",
      FAKEFS_ROOT "root/dir/was_dir/inside",
      FAKEFS_ROOT "root/dir/saved",
  });

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  const auto beforeChanges = view->getMostRecentRootNumberAndTickValue();

  // Each path holds something else by the time the iothread looks at it
  // again, so the iothread never sees the path missing.
  fs.removeRecursively(FAKEFS_ROOT "root/dir/was_file");
  fs.defineContents({FAKEFS_ROOT "root/dir/was_file/inside"});
  fs.removeRecursively(FAKEFS_ROOT "root/dir/was_dir");
  fs.defineContents({FAKEFS_ROOT "root/dir/was_dir"});
  // Another file, as a save that renames a temporary file into place leaves.
  fs.removeRecursively(FAKEFS_ROOT "root/dir/saved");
  fs.defineContents({FAKEFS_ROOT "root/dir/saved"});
  for (const auto& [path, flags] : reported) {
    pending.lock()->add(path, {}, flags);
  }
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("exists");
  query.fieldList.add("new");

  QueryContext ctx{&query, root, false};
  ctx.since = QuerySince::Clock{false, beforeChanges.ticks};
  view->timeGenerator(&query, &ctx);

  std::map<std::string, std::string> changes;
  for (const auto& file : ctx.resultsArray) {
    const char* change = !file.get("exists").asBool() ? "deleted"
        : file.get("new").asBool()                    ? "new"
                                                      : "changed";
    changes.emplace(file.get("name").asCString(), change);
  }
  return changes;
}

const std::map<std::string, std::string> kChangesAfterReplacing{
    {"dir/was_file", "new"},
    {"dir/was_file/inside", "new"},
    {"dir/was_dir", "new"},
    {"dir/was_dir/inside", "deleted"},
    {"dir/saved", "changed"},
};

TEST_P(InMemoryViewTest, path_that_changed_type_is_new) {
  const auto changes = changesAfterReplacing({
      {FAKEFS_ROOT "root/dir/was_file", W_PENDING_VIA_NOTIFY},
      {FAKEFS_ROOT "root/dir/was_dir", W_PENDING_VIA_NOTIFY},
      {FAKEFS_ROOT "root/dir/saved", W_PENDING_VIA_NOTIFY},
  });

  EXPECT_EQ(kChangesAfterReplacing, changes);
}

TEST_P(InMemoryViewTest, path_that_changed_type_is_new_when_found_by_crawl) {
  // The watcher reports the directory only; the crawl finds what its entries
  // hold now.
  const auto changes = changesAfterReplacing({
      {FAKEFS_ROOT "root/dir", W_PENDING_VIA_NOTIFY | W_PENDING_RECURSIVE},
  });

  auto expected = kChangesAfterReplacing;
  expected.emplace("dir", "changed");
  EXPECT_EQ(expected, changes);
}

TEST_P(InMemoryViewTest, dir_seen_after_its_entry_is_crawled) {
  // As on Windows: the watcher reports each file, and no inode number tells
  // that a directory is not the one the view has.
  watcher->flags = WATCHER_HAS_PER_FILE_NOTIFICATIONS;
  fs.defineContents({FAKEFS_ROOT "root/dir/keep"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  fs.defineContents({
      FAKEFS_ROOT "root/dir/sub/reported",
      FAKEFS_ROOT "root/dir/sub/unreported",
  });
  fs.updateMetadata(
      FAKEFS_ROOT "root/dir/sub", [](FileInformation& fi) { fi.ino = 0; });
  // The iothread takes the oldest item first, so it creates `sub` in the view
  // for its entry before it looks at `sub` itself.
  pending.lock()->add(
      FAKEFS_ROOT "root/dir/sub/reported", {}, W_PENDING_VIA_NOTIFY);
  pending.lock()->add(FAKEFS_ROOT "root/dir/sub", {}, W_PENDING_VIA_NOTIFY);
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 2});

  QueryContext ctx{&query, root, false};
  view->pathGenerator(&query, &ctx);

  std::set<std::string> names;
  for (const auto& file : ctx.resultsArray) {
    names.emplace(file.asCString());
  }
  const std::set<std::string> expected{
      "dir",
      "dir/keep",
      "dir/sub",
      "dir/sub/reported",
      "dir/sub/unreported",
  };
  EXPECT_EQ(expected, names);
}

INSTANTIATE_TEST_CASE_P(
    InMemoryViewTests,
    InMemoryViewTest,
    testing::Values(false, true));

} // namespace
