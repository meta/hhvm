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

#include "hphp/runtime/base/request-id.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/satellite-server.h"
#include "hphp/runtime/server/server-task-event.h"
#include "hphp/runtime/server/transport.h"
#include "hphp/util/configs/xbox.h"
#include "hphp/util/synchronizable.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Array;
struct XboxServerInfo;
struct XboxTransport;

struct XboxServer {
  /**
   * Start or restart xbox server.
   */
  static void Restart();
  static void Stop();

  static bool Enabled();

public:
  /**
   * Local tasklet for parallel processing.
   */
  static OptResource TaskStart(const OptString& msg, const OptString& reqInitDoc = "",
      ServerTaskEvent<XboxServer, XboxTransport> *event = nullptr, RequestId m_root_req_id = RequestId());
  static bool TaskStatus(const OptResource& task);
  static int TaskResult(const OptResource& task, int timeout_ms, Variant *ret);
  static int TaskResult(XboxTransport* const job, int timeout_ms, Variant *ret);

  static int GetActiveWorkers();
  static size_t GetLiveThreadCount();
  static int GetQueuedJobs();
};

///////////////////////////////////////////////////////////////////////////////

struct XboxServerInfo : SatelliteServerInfo {
  XboxServerInfo() : SatelliteServerInfo(IniSetting::Map::object, Hdf()) {
    m_type = SatelliteServer::Type::KindOfXboxServer;
    m_name = "xbox";
    reload();
  }

  void reload() {
    m_threadCount = Cfg::Xbox::ServerInfoThreadCount;
    m_reqInitFunc = Cfg::Xbox::ServerInfoRequestInitFunction;
    m_reqInitDoc  = Cfg::Xbox::ServerInfoRequestInitDocument;
  }
};

///////////////////////////////////////////////////////////////////////////////

extern const StaticString s_xbox;

struct XboxTransport final : Transport, Synchronizable {
  explicit XboxTransport(
    const folly::StringPiece message,
    const RequestId root_req_id,
    const folly::StringPiece reqInitDoc = "");

  timespec getStartTimer() const { return m_queueTime; }

  /**
   * Request URI.
   */
  const char *getUrl() override;
  const char *getRemoteHost() override { return "127.0.0.1"; }
  uint16_t getRemotePort() override { return 0; }
  const std::string& getServerAddr() override {
    auto const& ipv4 = RuntimeOption::GetServerPrimaryIPv4();
    return ipv4.empty() ? RuntimeOption::GetServerPrimaryIPv6() : ipv4;
  }

  /**
   * Request data.
   */
  Method getMethod() override { return Transport::Method::POST; }
  const void *getPostData(size_t &size) override {
    size = m_message.size();
    return m_message.data();
  }

  /**
   * Manage headers.
   */
  std::string getHeader(const char *name) override;
  const HeaderMap& getHeaders() override {
    static const HeaderMap emptyMap{};
    return emptyMap;
  }
  void addHeaderImpl(const char* /*name*/, const char* /*value*/) override {}
  void removeHeaderImpl(const char* /*name*/) override {}

  void sendImpl(const void *data, int size, int code, bool chunked, bool eom)
       override;
  void onSendEndImpl() override;

  /**
   * Get a description of the type of transport.
   */
  OptString describe() const override {
    return s_xbox;
  }

  /**
   * Task interface.
   */
  bool isDone() { return m_done; }
  OptString getResults(int &code, int timeout_ms = 0);

  void setHost(const std::string &host) { m_host = host;}
  void setAsioEvent(ServerTaskEvent<XboxServer, XboxTransport> *event) {
    m_event = event;
  }

  // Refcounting.
  void incRefCount() {
    ++m_refCount;
  }
  void decRefCount() {
    assertx(m_refCount.load());
    if (--m_refCount == 0) {
      delete this;
    }
  }

  void setCliContext(CLIContext&& ctx) {
    m_cli.emplace(std::move(ctx));
  }
  Optional<CLIContext> detachCliContext() { return std::move(m_cli); }

private:
  std::atomic<int> m_refCount;

  std::string m_message;

  bool m_done;
  std::string m_response;
  int m_code;
  std::string m_host;
  std::string m_reqInitDoc;

  Optional<CLIContext> m_cli;

  // points to an event with an attached waithandle from a different request
  ServerTaskEvent<XboxServer, XboxTransport> *m_event;
};

///////////////////////////////////////////////////////////////////////////////
}
