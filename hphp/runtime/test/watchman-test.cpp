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

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <folly/FileUtil.h>
#include <folly/Try.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/json/bser/Bser.h>
#include <folly/json/dynamic.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/testing/TestUtil.h>

#include "hphp/runtime/base/watchman.h"
#include "hphp/util/optional.h"

using ::testing::ElementsAre;

namespace HPHP {
namespace {

/**
 * Just enough of a Watchman server for WatchmanImpl: answers each command on
 * its own connection and records the commands each connection sent.
 */
struct FakeWatchman {
  FakeWatchman() {
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    checkUnixError(m_listenFd, "socket");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socketPath().copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    checkUnixError(
        ::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
        "bind");
    checkUnixError(::listen(m_listenFd, 8), "listen");
    m_acceptThread = std::thread{[this] { acceptConnections(); }};
  }

  ~FakeWatchman() {
    ::shutdown(m_listenFd, SHUT_RDWR);
    m_acceptThread.join();
    ::close(m_listenFd);
    std::vector<std::shared_ptr<Connection>> connections;
    {
      std::lock_guard g{m_mutex};
      connections = m_connections;
    }
    for (auto& connection : connections) {
      ::shutdown(connection->m_fd, SHUT_RDWR);
      connection->m_thread.join();
      ::close(connection->m_fd);
    }
  }

  FakeWatchman(const FakeWatchman&) = delete;
  FakeWatchman& operator=(const FakeWatchman&) = delete;

  std::filesystem::path root() const {
    return m_dir.path();
  }

  std::string socketPath() const {
    return (m_dir.path() / "sock").native();
  }

  void failNextQuery() {
    m_failNextQuery = true;
  }

  size_t connectionCount() const {
    std::lock_guard g{m_mutex};
    return m_connections.size();
  }

  /**
   * The queries the given connection subscribed to, in the order it sent them.
   */
  std::vector<folly::dynamic> subscriptions(size_t connection) const {
    std::vector<folly::dynamic> queries;
    std::lock_guard g{m_mutex};
    for (auto const& command : m_connections.at(connection)->m_commands) {
      if (command[0] == "subscribe") {
        queries.push_back(command[3]);
      }
    }
    return queries;
  }

  /**
   * Send a subscription notification for every subscription the given
   * connection has made.
   */
  void notifySubscribers(size_t connection) {
    std::shared_ptr<Connection> conn;
    std::vector<std::string> names;
    {
      std::lock_guard g{m_mutex};
      conn = m_connections.at(connection);
      for (auto const& command : conn->m_commands) {
        if (command[0] == "subscribe") {
          names.push_back(command[2].asString());
        }
      }
    }
    for (auto const& name : names) {
      conn->send(folly::dynamic::object("subscription", name)("clock", "c:2")(
          "files", folly::dynamic::array())("is_fresh_instance", false)(
          "unilateral", true));
    }
  }

 private:
  struct Connection {
    explicit Connection(int fd) : m_fd{fd} {}

    void send(const folly::dynamic& pdu) {
      auto buf =
          folly::bser::toBserIOBuf(pdu, folly::bser::serialization_opts{});
      buf->coalesce();
      std::lock_guard g{m_writeMutex};
      folly::writeFull(m_fd, buf->data(), buf->length());
    }

    const int m_fd;
    std::thread m_thread;
    std::mutex m_writeMutex;
    // Guarded by FakeWatchman::m_mutex
    std::vector<folly::dynamic> m_commands;
  };

  static void checkUnixError(int ret, const char* what) {
    if (ret < 0) {
      throw std::system_error(errno, std::system_category(), what);
    }
  }

  void acceptConnections() {
    while (true) {
      int fd = ::accept(m_listenFd, nullptr, nullptr);
      if (fd < 0) {
        return;
      }
      auto connection = std::make_shared<Connection>(fd);
      std::lock_guard g{m_mutex};
      connection->m_thread =
          std::thread{[this, connection] { serve(*connection); }};
      m_connections.push_back(std::move(connection));
    }
  }

  void serve(Connection& connection) {
    folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
    while (true) {
      auto [buf, size] = queue.preallocate(4096, 65536);
      auto n = folly::readNoInt(connection.m_fd, buf, size);
      if (n <= 0) {
        return;
      }
      queue.postallocate(n);
      while (auto pdu = splitPdu(queue)) {
        auto command = folly::bser::parseBser(pdu.get());
        {
          std::lock_guard g{m_mutex};
          connection.m_commands.push_back(command);
        }
        connection.send(respond(command));
      }
    }
  }

  static std::unique_ptr<folly::IOBuf> splitPdu(folly::IOBufQueue& queue) {
    if (!queue.front()) {
      return nullptr;
    }
    size_t length = 0;
    try {
      length = folly::bser::decodePduLength(queue.front());
    } catch (const std::out_of_range&) {
      return nullptr;
    }
    if (length > queue.chainLength()) {
      return nullptr;
    }
    return queue.split(length);
  }

  folly::dynamic respond(const folly::dynamic& command) {
    auto const& name = command[0];
    if (name == "version") {
      return folly::dynamic::object("version", "fake")(
          "capabilities", folly::dynamic::object("relative_root", true));
    }
    if (name == "watch-project") {
      return folly::dynamic::object("version", "fake")("watch", command[1]);
    }
    if (name == "clock") {
      return folly::dynamic::object("version", "fake")("clock", "c:1");
    }
    if (name == "subscribe") {
      return folly::dynamic::object("version", "fake")("subscribe", command[2])(
          "clock", "c:1");
    }
    if (name == "query") {
      if (m_failNextQuery.exchange(false)) {
        return folly::dynamic::object("version", "fake")("error", "fake error");
      }
      return folly::dynamic::object("version", "fake")("clock", "c:2")(
          "is_fresh_instance", false)("files", folly::dynamic::array());
    }
    return folly::dynamic::object("version", "fake")(
        "error", "unexpected command");
  }

  folly::test::TemporaryDirectory m_dir{"watchman-test", "/tmp"};
  int m_listenFd{-1};
  std::thread m_acceptThread;
  std::atomic<bool> m_failNextQuery{false};
  mutable std::mutex m_mutex;
  std::vector<std::shared_ptr<Connection>> m_connections;
};

folly::dynamic queryFor(std::string_view file) {
  return folly::dynamic::object(
      "expression", folly::dynamic::array("name", file));
}

std::shared_ptr<Watchman> connect(const FakeWatchman& fake) {
  auto watchman =
      Watchman::get(fake.root(), Optional<std::string>{fake.socketPath()});
  // Finish connecting before subscribing.
  watchman->getClock().get();
  return watchman;
}

TEST(WatchmanTest, reconnectSkipsReleasedSubscriptions) {
  FakeWatchman fake;
  auto watchman = connect(fake);

  auto live = watchman->subscribe(
      queryFor("live.php"), [](folly::Try<folly::dynamic>&&) {});
  auto released = watchman->subscribe(
      queryFor("released.php"), [](folly::Try<folly::dynamic>&&) {});
  released.reset();

  // A failed query makes WatchmanImpl reconnect and re-send its subscriptions
  // on the new connection, ahead of the retried query.
  fake.failNextQuery();
  watchman->query(queryFor("query.php")).get();

  ASSERT_EQ(fake.connectionCount(), 2);
  EXPECT_THAT(fake.subscriptions(1), ElementsAre(queryFor("live.php")));
}

TEST(WatchmanTest, releasedSubscriptionGetsNoNotifications) {
  FakeWatchman fake;
  auto watchman = connect(fake);

  int liveNotifications = 0;
  int releasedNotifications = 0;
  auto live =
      watchman->subscribe(queryFor("a.php"), [&](folly::Try<folly::dynamic>&&) {
        ++liveNotifications;
      });
  auto released =
      watchman->subscribe(queryFor("a.php"), [&](folly::Try<folly::dynamic>&&) {
        ++releasedNotifications;
      });
  released.reset();
  // Callbacks run on WatchmanImpl's executor in order, so a round trip through
  // it means the subscription went out and its notification was handled.
  watchman->getClock().get();
  fake.notifySubscribers(0);
  watchman->getClock().get();

  EXPECT_EQ(fake.subscriptions(0), std::vector{queryFor("a.php")});
  EXPECT_EQ(liveNotifications, 1);
  EXPECT_EQ(releasedNotifications, 0);
}

TEST(WatchmanTest, resubscribingReusesWatchmanSubscription) {
  FakeWatchman fake;
  auto watchman = connect(fake);

  int notifications = 0;
  watchman->subscribe(queryFor("a.php"), [](folly::Try<folly::dynamic>&&) {})
      .reset();
  auto subscription =
      watchman->subscribe(queryFor("a.php"), [&](folly::Try<folly::dynamic>&&) {
        ++notifications;
      });
  watchman->getClock().get();
  fake.notifySubscribers(0);
  watchman->getClock().get();

  EXPECT_EQ(fake.subscriptions(0), std::vector{queryFor("a.php")});
  EXPECT_EQ(notifications, 1);
}

} // namespace
} // namespace HPHP
