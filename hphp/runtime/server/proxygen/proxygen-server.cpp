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

#include "hphp/runtime/server/proxygen/proxygen-server.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <thread>
#include "hphp/runtime/server/fake-transport.h"
#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/proxygen/proxygen-transport.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/util/configs/server.h"
#include "hphp/runtime/base/crash-reporter.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/url.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/util/alloc.h"
#include "hphp/util/process.h"
#include "hphp/util/timer.h"

#include <folly/portability/Unistd.h>
#include <proxygen/lib/http/codec/HTTP2Constants.h>

namespace HPHP {

constexpr auto kPollInterval = std::chrono::milliseconds(60000); // 60 sec
constexpr uint32_t kStreamFlowControl = 1 << 20; // 1 MB
constexpr uint32_t kConnFlowControl = kStreamFlowControl * 1.5; // 1.5 MB
using folly::SocketAddress;
using folly::AsyncServerSocket;
using wangle::Acceptor;

HPHPSessionAcceptor::HPHPSessionAcceptor(
    std::shared_ptr<const proxygen::AcceptorConfiguration> config,
    ProxygenServer *server,
    HPHPWorkerThread *worker)
      : HTTPSessionAcceptor(std::move(config)),
        m_server(server),
        m_worker(worker),
        m_controllerPtr(new proxygen::SimpleController(this)) {
}

bool HPHPSessionAcceptor::canAccept(const SocketAddress& /*address*/) {
  // for now, we don't bother with the address whitelist
  return m_server->canAccept();
}

void HPHPSessionAcceptor::onIngressError(
#ifdef PROXYGEN_HTTP_SESSION_USES_BASE
  const proxygen::HTTPSessionBase& /*session*/,
#else
  const proxygen::HTTPSession& /*session*/,
#endif
  proxygen::ProxygenError error) {
  // This method is invoked when the HTTP library encountered some error before
  // it could completely parse the headers.  Most of these are HTTP garbage
  // (400 Bad Request) or client timeouts (408).
  FakeTransport transport((error == proxygen::kErrorTimeout) ? 408 : 400);
  transport.m_url = folly::to<std::string>("/onIngressError?error=",
                                           proxygen::getErrorString(error));
  m_server->onRequestError(&transport);
}

proxygen::HTTPTransaction::Handler*
HPHPSessionAcceptor::newHandler(proxygen::HTTPTransaction& /*txn*/,
                                proxygen::HTTPMessage* /*msg*/) noexcept {
  auto transport = std::make_shared<ProxygenTransport>(m_server, m_worker);
  transport->setTransactionReference(transport);
  return transport.get();
}

void HPHPSessionAcceptor::onConnectionsDrained() {
  auto const evb = m_server->m_workers[0]->getEventBase();
  evb->runInEventBaseThread([server = m_server] {
    server->onConnectionsDrained();
  });
}

ProxygenJob::ProxygenJob(std::shared_ptr<ProxygenTransport> t) :
    transport(t),
    reqStart(t->getRequestStart()) {
}

void ProxygenJob::getRequestStart(struct timespec *outReqStart) {
  *outReqStart = reqStart;
}

///////////////////////////////////////////////////////////////////////////////
// ProxygenTransportTraits

ProxygenTransportTraits::ProxygenTransportTraits(
  std::shared_ptr<ProxygenJob> job, void* opaque, int /*id*/)
    : server_((ProxygenServer*)opaque), transport_(std::move(job->transport)) {
  VLOG(4) << "executing request with path=" << transport_->getUrl();
}

ProxygenTransportTraits::~ProxygenTransportTraits() {
  // ProxygenTransport must be released in worker thread
  ProxygenTransport *transport = transport_.get();
  transport->finish(std::move(transport_));
}

void HPHPWorkerThread::setup() {
  WorkerThread::setup();
  hphp_thread_init();
  startConsuming(getEventBase(), &m_responseQueue);
}

void HPHPWorkerThread::cleanup() {
  hphp_thread_exit();
  WorkerThread::cleanup();
}

void HPHPWorkerThread::addPendingTransport(ProxygenTransport& transport) {
  m_pendingTransportsCount.fetch_add(1, std::memory_order_acq_rel);
  if (m_server->partialPostEchoEnabled()) {
    const auto status = m_server->getStatus();
    transport.setShouldRepost(status == ProxygenServer::RunStatus::STOPPING
                              || status == ProxygenServer::RunStatus::STOPPED);
  }
  m_pendingTransports.push_back(transport);
}

void HPHPWorkerThread::removePendingTransport(ProxygenTransport& transport) {
  if (transport.is_linked()) {
    transport.unlink();
    m_pendingTransportsCount.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void HPHPWorkerThread::returnPartialPosts() {
  for (auto& transport : m_pendingTransports) {
    if (!transport.getClientComplete()) {
      transport.beginPartialPostEcho();
    }
  }
}

void HPHPWorkerThread::abortPendingTransports() {
  if (!m_pendingTransports.empty()) {
    Logger::Warning("aborting %lu incomplete requests",
                    m_pendingTransports.size());
    // Avoid iterating the list, as abort() will unlink(), leaving the
    // list iterator in a corrupt state.
    do {
      auto& transport = m_pendingTransports.front();
      transport.abort();                // will unlink()
    } while (!m_pendingTransports.empty());
  }
}

///////////////////////////////////////////////////////////////////////////////
ProxygenServer::ProxygenEventBaseObserver::ProxygenEventBaseObserver(
    uint32_t loop_sample_rate, int worker
  ) : m_sample_rate_(loop_sample_rate),
      m_busytime_estimator(std::chrono::seconds{60}),
      m_idletime_estimator(std::chrono::seconds{60}),
      m_evbLoopCountTimeSeries(ServiceData::createTimeSeries(
                                 folly::to<std::string>(
                                   "proxygen_evb_loop_count_",
                                   worker),
                                 {ServiceData::StatsType::COUNT},
                                 {std::chrono::seconds(60)}, 60)) {
  m_counterCallback.init([this, worker](ServiceData::CounterMap& values){
    auto now = ClockT::now();
    // export p90 p99 for evb busy time
    const std::array<double, 2> quantiles_busytime {0.9, 0.99};
    auto estimates = m_busytime_estimator.estimateQuantiles(
      quantiles_busytime, now).quantiles;

    auto const p90_busy_name = folly::to<std::string>(
        "proxygen_evb_busy_time_us_", worker, ".p90.60");
    values[p90_busy_name] = estimates[0].second;
    auto const p99_busy_name = folly::to<std::string>(
        "proxygen_evb_busy_time_us_", worker, ".p99.60");
    values[p99_busy_name] = estimates[1].second;

    // export p1 p10 for evb idle time
    const std::array<double, 2> quantiles_idletime {0.01, 0.1};
    estimates = m_idletime_estimator.estimateQuantiles(
      quantiles_idletime, now).quantiles;

    auto const p1_idle_name = folly::to<std::string>(
        "proxygen_evb_idle_time_us_", worker, ".p1.60");
    values[p1_idle_name] = estimates[0].second;
    auto const p10_idle_name = folly::to<std::string>(
        "proxygen_evb_idle_time_us_", worker, ".p10.60");
    values[p10_idle_name] = estimates[1].second;
  }, "proxygen_evb_");
}

void ProxygenServer::ProxygenEventBaseObserver::loopSample(
  int64_t busytime /*usec */, int64_t idletime /* usec */) {
  auto now = ClockT::now();
  m_busytime_estimator.addValue(static_cast<double>(busytime), now);
  m_idletime_estimator.addValue(static_cast<double>(idletime), now);
  m_evbLoopCountTimeSeries->addValue(static_cast<int64_t>(getSampleRate()));
}

ProxygenServer::ProxygenServer(
  const ServerOptions& options
  ) : Server(options.m_address, options.m_port),
      m_accept_sock(options.m_serverFD),
      m_accept_sock_ssl(options.m_sslFD),
      m_dispatcher(options.m_maxThreads,
                   options.m_maxQueue,
                   Cfg::Server::ThreadDropCacheTimeoutSeconds,
                   Cfg::Server::ThreadDropStack,
                   this, Cfg::Server::ThreadJobLIFOSwitchThreshold,
                   Cfg::Server::ThreadJobMaxQueuingMilliSeconds,
                   kNumPriorities,
                   options.m_hugeThreads,
                   options.m_initThreads,
                   options.m_hugeStackKb,
                   options.m_extraKb) {
  always_assert_flog(Cfg::Server::IOThreadCount > 0,
                     "Proxygen must have at least 1 thread to function.");
  for (int i = 0; i < Cfg::Server::IOThreadCount; i++) {
    m_workers.emplace_back(
      std::make_unique<HPHPWorkerThread>(&m_eventBaseManager, this));
  }
  m_httpAcceptors.resize(Cfg::Server::IOThreadCount);
  m_httpsAcceptors.resize(Cfg::Server::IOThreadCount);

  if (options.m_loop_sample_rate > 0) {
    for (int i = 0; i < m_workers.size(); i++) {
      auto observer = std::make_shared<ProxygenEventBaseObserver>(
          options.m_loop_sample_rate, i);
      m_workers[i]->getEventBase()->setObserver(std::move(observer));
    }
  }
  SocketAddress address;
  if (options.m_address.empty()) {
    address.setFromLocalPort(options.m_port);
  } else {
    address.setFromHostPort(options.m_address, options.m_port);
  }
  m_httpConfig->bindAddress = address;
  m_httpConfig->acceptBacklog = Cfg::Server::Backlog;
  m_httpsConfig->acceptBacklog = Cfg::Server::Backlog;
  // TODO: proxygen only supports downstream keep-alive
  std::chrono::seconds timeout;
  if (Cfg::Server::ConnectionTimeoutSeconds > 0) {
    timeout = std::chrono::seconds(Cfg::Server::ConnectionTimeoutSeconds);
  } else {
    // default to 50s (to match libevent)
    timeout = std::chrono::seconds(50);
  }
  m_httpConfig->connectionIdleTimeout = timeout;
  m_httpConfig->transactionIdleTimeout = timeout;
  m_httpsConfig->connectionIdleTimeout = timeout;
  m_httpsConfig->transactionIdleTimeout = timeout;

  // Set flow control (for uploads) to 1MB.  We could also make this
  // configurable if needed
  m_httpsConfig->initialReceiveWindow = kStreamFlowControl;
  m_httpsConfig->receiveSessionWindowSize = kConnFlowControl;
  if (Cfg::Server::EnableH2C) {
    m_httpConfig->initialReceiveWindow = kStreamFlowControl;
    m_httpConfig->receiveSessionWindowSize = kConnFlowControl;
  }

  const std::vector<std::chrono::seconds> levels {
    std::chrono::seconds(10), std::chrono::seconds(120)};
  ProxygenTransport::s_requestErrorCount =
    ServiceData::createTimeSeries("http_response_error",
                                  {ServiceData::StatsType::COUNT},
                                  levels, 10);
  ProxygenTransport::s_requestNonErrorCount =
    ServiceData::createTimeSeries("http_response_nonerror",
                                  {ServiceData::StatsType::COUNT},
                                  levels, 10);
}

ProxygenServer::~ProxygenServer() {
  Logger::Verbose("%p: destroying ProxygenServer", this);
  waitForEnd();
  Logger::Info("%p: ProxygenServer destroyed", this);
}

DispatcherStats ProxygenServer::getDispatcherStats() {
  return m_dispatcher.getDispatcherStats();
}

std::unique_ptr<HPHPSessionAcceptor> ProxygenServer::createAcceptor(
    std::shared_ptr<const proxygen::AcceptorConfiguration> config,
    HPHPWorkerThread *worker) {
  return std::make_unique<HPHPSessionAcceptor>(std::move(config), this, worker);
}

void ProxygenServer::start() {
  start(true);
}

void ProxygenServer::start(bool beginAccepting) {
  m_httpServerSocket.reset(new AsyncServerSocket(m_workers[0]->getEventBase()));
  bool needListen = true;
  auto failedToListen = [](const std::exception& ex,
                           const folly::SocketAddress& addr) {
    Logger::Error("failed to listen: %s", ex.what());
    throw FailedToListenException(addr.getAddressStr(), addr.getPort());
  };

  /*
   * Order of setting up m_httpServerSocket (for the main server only, not
   * including admin server, etc.).
   * (1) Try to use Cfg::Server::PortFd (the inherited socket should
   * have already been bound to a proper port, but isn't listening yet).
   * (2) If (1) fails, and try to take over the socket of an old server.
   * (3) If both (1) and (2) fail, try to bind to Cfg::Server::Port.
   */
  bool socketSetupSucceeded = false;
  if (m_accept_sock >= 0) {
    try {
      m_httpServerSocket->useExistingSocket(
        folly::NetworkSocket::fromFd(m_accept_sock));
      socketSetupSucceeded = true;
      Logger::Info("inheritfd: successfully inherited fd %d for server",
                   m_accept_sock);
    } catch (const std::exception&) {
      Logger::Warning("inheritfd: failed to inherit fd %d for server",
                      m_accept_sock);
    }
  }
  if (!socketSetupSucceeded) {
    // make it possible to quickly reuse the port when needed.
    auto const allowReuse = Cfg::Server::StopOld;
    m_httpServerSocket->setReusePortEnabled(allowReuse);
    try {
      m_httpServerSocket->bind(m_httpConfig->bindAddress);
    } catch (const std::exception& ex) {
      failedToListen(ex, m_httpConfig->bindAddress);
    }
  }

  for (int i = 0; i < m_workers.size(); i++) {
    auto acceptor = createAcceptor(m_httpConfig, m_workers[i].get());
    acceptor->init(m_httpServerSocket.get(), m_workers[i]->getEventBase());
    m_httpAcceptors[i] = std::move(acceptor);
  }

  if (m_httpsConfig->isSSL()) {
    if (!Cfg::Server::SSLCertificateDir.empty()) {
      m_filePoller = std::make_unique<wangle::FilePoller>(
          kPollInterval);

      CertReloader::load(
        Cfg::Server::SSLCertificateDir,
        std::bind(&ProxygenServer::resetSSLContextConfigs,
          this,
          std::placeholders::_1));

      m_filePoller->addFileToTrack(
        Cfg::Server::SSLCertificateDir,
        [this, dir = Cfg::Server::SSLCertificateDir] {
          CertReloader::load(
            dir,
            std::bind(&ProxygenServer::resetSSLContextConfigs,
              this,
              std::placeholders::_1));
        });
    }
  }
  if (m_httpsConfig->isSSL()) {
    m_httpsServerSocket.reset(
      new AsyncServerSocket(m_workers[0]->getEventBase()));
    try {
      if (m_accept_sock_ssl >= 0) {
        Logger::Info("inheritfd: using inherited fd %d for ssl",
                     m_accept_sock_ssl);
        m_httpsServerSocket->useExistingSocket(
          folly::NetworkSocket::fromFd(m_accept_sock_ssl));
      } else {
        m_httpsServerSocket->setReusePortEnabled(Cfg::Server::StopOld);
        m_httpsServerSocket->bind(m_httpsConfig->bindAddress);
      }
    } catch (const std::system_error& ex) {
      failedToListen(ex, m_httpsConfig->bindAddress);
    }

    for (int i = 0; i < m_workers.size(); i++) {
      auto acceptor = createAcceptor(m_httpsConfig, m_workers[i].get());
      try {
        acceptor->init(m_httpsServerSocket.get(),
                       m_workers[i]->getEventBase());
      } catch (const std::exception& ex) {
        // Could be some cert thing
        failedToListen(ex, m_httpsConfig->bindAddress);
      }
      m_httpsAcceptors[i] = std::move(acceptor);
    }
  }

  if (!Cfg::Server::SSLTicketSeedFile.empty() && !m_httpsAcceptors.empty()) {
    // setup ticket seed watcher
    const auto& ticketPath = Cfg::Server::SSLTicketSeedFile;
    auto seeds = wangle::TLSCredProcessor::processTLSTickets(ticketPath);
    if (seeds) {
      updateTLSTicketSeeds(std::move(*seeds));
    }
    m_credProcessor =
        std::make_unique<wangle::TLSCredProcessor>();
    m_credProcessor->setTicketPathToWatch(ticketPath);
    m_credProcessor->addTicketCallback([&](wangle::TLSTicketKeySeeds seeds) {
        updateTLSTicketSeeds(std::move(seeds));
    });
  }

  if (needListen) {
    try {
      m_httpServerSocket->listen(m_httpConfig->acceptBacklog);
    } catch (const std::system_error& ex) {
      failedToListen(ex, m_httpConfig->bindAddress);
    }
  }
  if (m_httpsServerSocket) {
    try {
      m_httpsServerSocket->listen(m_httpsConfig->acceptBacklog);
    } catch (const std::system_error& ex) {
      failedToListen(ex, m_httpsConfig->bindAddress);
    }
  }

  if (beginAccepting) {
    startAccepting();
  }

  setStatus(RunStatus::RUNNING);
  folly::AsyncTimeout::attachEventBase(m_workers[0]->getEventBase());
  for (auto& worker : m_workers) {
    worker->start();
  }
  m_dispatcher.start();
}

void ProxygenServer::startAccepting() {
  m_httpServerSocket->startAccepting();
  if (m_httpsServerSocket) {
    m_httpsServerSocket->startAccepting();
  }
}

void ProxygenServer::waitForEnd() {
  Logger::Info("%p: Waiting for ProxygenServer port=%d", this, m_port);
  // Event base 0 waits for all workers and cleans them up.  We are
  // left taking care of only worker 0.
  m_workers[0]->wait();
  always_assert(m_workers.size() == 1);
}

// Server shutdown - Explained
//
// 0. An alarm may be set in http-server.cpp to kill this process after
//    ServerPreShutdownWait + ServerShutdownListenWait +
//    ServerGracefulShutdownWait seconds
// 1. Set run status to STOPPING.  This should fail downstream healthchecks.
//    If it is the page server, it will continue accepting requests for
//    ServerPreShutdownWait seconds
// 2. Shutdown the listen sockets, this will
//     2.a Close any idle connections
//     2.b Send SPDY GOAWAY frames
//     2.c Insert Connection: close on HTTP/1.1 keep-alive connections as the
//         response headers are sent
// 3. If the server hasn't received the entire request body
//    ServerShutdownEOM seconds after shutdown starts, the request
//    will be aborted.  ServerShutdownEOM isn't required to be smaller
//    than ServerShutdownListenWait, but it makes sense to make it be,
//    in order to make shutdown faster.
// 4. After all requests finish executing OR all connections close OR
//    ServerShutdownListenWait seconds elapse, stop the VM.
//    Incomplete requests in the I/O thread will not be executed.
//    Stopping the VM is synchronous and all requests will run to
//    completion, unless the alarm fires.
// 5. Allow responses to drain for up to ServerGracefulShutdownWait seconds.
//    Note if shutting the VM down took non-zero time it's possible that the
//    alarm will fire first and kill this process.

void ProxygenServer::stop() {
  if (getStatus() != RunStatus::RUNNING ||
      m_shutdownState != ShutdownState::SHUTDOWN_NONE) return;

  Logger::Info("%p: Stopping ProxygenServer port=%d", this, m_port);

  setStatus(RunStatus::STOPPING);

  if (m_credProcessor) {
    m_credProcessor->stop();
  }

  if (m_filePoller) {
    m_filePoller->stop();
  }

  // close listening sockets, this will initiate draining, including closing
  // idle conns
  m_workers[0]->getEventBase()->runInEventBaseThread([this] {
      // Only wait ServerPreShutdownWait seconds for the page server.
      int delayMilliSeconds = Cfg::Server::PreShutdownWait * 1000;
      if (delayMilliSeconds < 0 || getPort() != Cfg::Server::Port) {
        delayMilliSeconds = 0;
      }
      m_workers[0]->getEventBase()->runAfterDelay([this] { stopListening(); },
                                             delayMilliSeconds);
      reportShutdownStatus();
    });
}

void ProxygenServer::stopListening(bool hard) {
  m_shutdownState = ShutdownState::DRAINING_READS;
  HttpServer::MarkShutdownStat(ShutdownEvent::SHUTDOWN_DRAIN_READS);

  // triggers acceptStopped/sets acceptor state to Draining
  if (hard) {
    m_httpServerSocket.reset();
    m_httpsServerSocket.reset();
  } else {
    if (m_httpServerSocket) {
      m_httpServerSocket->stopAccepting();
    }
    if (m_httpsServerSocket) {
      m_httpsServerSocket->stopAccepting();
    }
  }

  if (Cfg::Server::ShutdownListenWait > 0) {
    std::chrono::seconds s(Cfg::Server::ShutdownListenWait);
    VLOG(4) << this << ": scheduling shutdown listen timeout="
            << s.count() << " port=" << m_port;
    scheduleTimeout(s);
    if (Cfg::Server::ShutdownEOMWait > 0) {
      int delayMilliSeconds = Cfg::Server::ShutdownEOMWait * 1000;
      m_workers[0]->getEventBase()->runAfterDelay([this] {

        auto const accelerateShutdown = [this] {
          // Accelerate shutdown if all requests that were enqueued are done,
          // since no more is coming in.
          auto const evb = m_workers[0]->getEventBase();
          evb->runInEventBaseThread([this] {
            if (m_enqueuedCount == 0 &&
                m_shutdownState == ShutdownState::DRAINING_READS) {
                doShutdown();
            }
          });
        };

        for (int i = 1; i < m_workers.size(); i++) {
          auto const& worker = m_workers[i];
          auto evb = worker->getEventBase();
          evb->runInEventBaseThread([w = worker.get(), accelerateShutdown] {
            w->abortPendingTransports();
            accelerateShutdown();
          });
        }
        m_workers[0]->abortPendingTransports();
        accelerateShutdown();
      }, delayMilliSeconds);
    }
  } else {
    doShutdown();
  }
}

void ProxygenServer::onConnectionsDrained() {
  ++m_drainCount;
  Logger::Info("All connections drained from ProxygenServer drainCount=%d",
               m_drainCount);
  if (!drained()) {
    // All acceptors have to finish
    Logger::Verbose("%p: waiting for all acceptors", this);
    return;
  }

  // Stop the graceful shutdown timer
  cancelTimeout();

  // proceed to next shutdown phase
  doShutdown();
}

void ProxygenServer::timeoutExpired() noexcept {
  Logger::Info("%p: shutdown timer expired for ProxygenServer port=%d, "
               "state=%d, a/q/e %d/%d/%d", this, m_port, (int)m_shutdownState,
               getActiveWorker(), getQueuedJobs(),
               getLibEventConnectionCount());
  // proceed to next shutdown phase
  doShutdown();
}

void ProxygenServer::doShutdown() {
  switch(m_shutdownState) {
    case ShutdownState::SHUTDOWN_NONE:
      // Transition from SHUTDOWN_NONE to DRAINING_READS needs to happen
      // explicitly through `stopListening`, not here.
      not_reached();
    case ShutdownState::DRAINING_READS:
      // Even though connections may be open for reading, they will not be
      // executed in the VM
      stopVM();
      break;
    case ShutdownState::STOPPING_VM:
      // this is a no-op, and can happen when we triggered VM shutdown from
      // the timeout code path, but the connections drain while waiting for
      // shutdown.  We let the VM shutdown continue and it will advance us
      // to the next state.
      break;
    case ShutdownState::DRAINING_WRITES:
      forceStop();
      break;
  }
}

void ProxygenServer::stopVM() {
  m_shutdownState = ShutdownState::STOPPING_VM;
  HttpServer::MarkShutdownStat(ShutdownEvent::SHUTDOWN_DRAIN_DISPATCHER);
  // we can't call m_dispatcher.stop() from the event loop, because it blocks
  // all I/O.  Spawn a thread to call it and callback when it's done.
  std::thread vmStopper([this] {
      purge_all();
      Logger::Info("%p: Stopping dispatcher port=%d", this, m_port);
      m_dispatcher.stop();
      Logger::Info("%p: Dispatcher stopped port=%d.  conns=%d", this, m_port,
                   getLibEventConnectionCount());
      m_workers[0]->getEventBase()->runInEventBaseThread([this] {
          vmStopped();
        });
    });
  vmStopper.detach();
}

void ProxygenServer::vmStopped() {
  m_shutdownState = ShutdownState::DRAINING_WRITES;
  HttpServer::MarkShutdownStat(ShutdownEvent::SHUTDOWN_DRAIN_WRITES);
  if (!drained() && Cfg::Server::GracefulShutdownWait > 0) {
    m_workers[0]->getEventBase()->runInEventBaseThread([&] {
        std::chrono::seconds s(Cfg::Server::GracefulShutdownWait);
        VLOG(4) << this << ": scheduling graceful timeout=" << s.count() <<
          " port=" << m_port;
        scheduleTimeout(s);
      });
  } else {
    forceStop();
  }
}

void ProxygenServer::forceStop() {
  auto evb = m_workers[0]->getEventBase();
  evb->checkIsInEventBaseThread();
  if (getStatus() == RunStatus::STOPPED) return;
  Logger::Info("%p: forceStop ProxygenServer port=%d, enqueued=%d, conns=%d",
               this, m_port, m_enqueuedCount.load(std::memory_order_acquire),
               getLibEventConnectionCount());
  m_httpServerSocket.reset();
  m_httpsServerSocket.reset();

  // Drops all open connections
  // forceStop may be called from any thread.
  for (auto const& acceptor : m_httpAcceptors) {
    if (acceptor && acceptor->getState() < Acceptor::State::kDone) {
      acceptor->forceStop();
    }
  }
  for (auto const& acceptor : m_httpsAcceptors) {
    if (acceptor && acceptor->getState() < Acceptor::State::kDone) {
      acceptor->forceStop();
    }
  }

  // No more responses coming from worker threads
  for (auto& worker : m_workers) {
    worker->stopConsuming();
  }
  Logger::Verbose("%p: Stopped response queue consumer port=%d", this, m_port);

  Logger::Verbose("%p: i/o thread notified to stop port=%d", this, m_port);

  // Aaaand we're done - oops not thread safe.  Does it matter?
  setStatus(RunStatus::STOPPED);

  HttpServer::MarkShutdownStat(ShutdownEvent::SHUTDOWN_DONE);

  // Listeners may need to run code in the event bases.
  for (auto listener : m_listeners) {
    listener->serverStopped(this);
  }

  // The worker should exit gracefully
  // Shutdown the main worker last in case its event base needs to handle
  // requests from other workers.
  for (int i = 1; i < m_workers.size(); i++) {
    m_workers[i]->stopWhenIdle();
  }
  while (m_workers.size() > 1) {
    m_workers.back()->wait();
    m_workers.pop_back();
  }
  // We are running on event base 0 we can't wait for it here.  Let it get
  // cleaned up by the ProxygenServer destructor.
  m_workers[0]->stopWhenIdle();
}

void ProxygenServer::reportShutdownStatus() {
  if (m_port != Cfg::Server::Port) return;
  if (getStatus() == RunStatus::STOPPED) return;
  Logger::FInfo("Shutdown state={}, a/q/e/p {}/{}/{}/{}, RSS={}Mb",
                static_cast<int>(m_shutdownState),
                getActiveWorker(),
                getQueuedJobs(),
                getLibEventConnectionCount(),
                getPendingTransportsCount(),
                Process::GetMemUsageMb());
  m_workers[0]->getEventBase()->runAfterDelay([this]{reportShutdownStatus();},
                                             500);
}

bool ProxygenServer::canAccept() {
  return (Cfg::Server::ConnectionLimit == 0 ||
          getLibEventConnectionCount() < Cfg::Server::ConnectionLimit);
}

int ProxygenServer::getLibEventConnectionCount() {
  uint32_t conns = 0;
  for (auto const& acceptor : m_httpAcceptors) {
    if (acceptor) {
      conns += acceptor->getNumConnections();
    }
  }
  for (auto const& acceptor : m_httpsAcceptors) {
    if (acceptor) {
      conns += acceptor->getNumConnections();
    }
  }
  return conns;
}

uint32_t ProxygenServer::getPendingTransportsCount() {
  uint32_t count = 0;
  for (auto& worker : m_workers) {
    count += worker->getPendingTransportsCount();
  }
  return count;
}

void ProxygenServer::resetSSLContextConfigs(
  const std::vector<CertKeyPair>& paths) {
  std::vector<wangle::SSLContextConfig> configs;
  // always include the default cert/key
  auto cfg = createContextConfig({
        Cfg::Server::SSLCertificateFile,
        Cfg::Server::SSLCertificateKeyFile}, true);
  configs.emplace_back(cfg);

  for (const auto& path : paths) {
    configs.emplace_back(createContextConfig(path));
  }

  for (int i = 0; i < m_workers.size(); i++) {
    auto evb = m_workers[i]->getEventBase();
    evb->runInEventBaseThread([this, configs, i] {
        if (m_httpsAcceptors[i] && m_httpsConfig->isSSL()) {
          m_httpsAcceptors[i]->resetSSLContextConfigs(configs);
        }
    });
  }
}

void ProxygenServer::updateTLSTicketSeeds(wangle::TLSTicketKeySeeds seeds) {
  for (int i = 0; i < m_workers.size(); i++) {
    auto evb = m_workers[i]->getEventBase();
    evb->runInEventBaseThread([seeds = seeds, this, i] {
        if (m_httpsAcceptors[i]) {
          m_httpsAcceptors[i]->setTLSTicketSecrets(
              seeds.oldSeeds, seeds.currentSeeds, seeds.newSeeds);
        }
    });
  }
}

bool ProxygenServer::enableSSL(int port) {
  if (port == 0) {
    return false;
  }
  SocketAddress address = m_httpConfig->bindAddress;
  address.setPort(port);

  m_httpsConfig->bindAddress = address;
  m_httpsConfig->strictSSL = false;

  m_httpsConfig->sslContextConfigs.emplace_back(
      createContextConfig({
        Cfg::Server::SSLCertificateFile,
        Cfg::Server::SSLCertificateKeyFile}, true));
  m_https = true;
  return true;
}

wangle::SSLContextConfig ProxygenServer::createContextConfig(
    const CertKeyPair& path,
    bool isDefault) {
  wangle::SSLContextConfig sslCtxConfig;

  if (Cfg::Server::SSLClientAuthLevel == 2) {
    sslCtxConfig.clientCAFiles = std::vector<std::string>{Cfg::Server::SSLClientCAFile};
    sslCtxConfig.clientVerification =
      folly::SSLContext::VerifyClientCertificate::ALWAYS;
  } else if (Cfg::Server::SSLClientAuthLevel == 1) {
    sslCtxConfig.clientCAFiles = std::vector<std::string>{Cfg::Server::SSLClientCAFile};
    sslCtxConfig.clientVerification =
      folly::SSLContext::VerifyClientCertificate::IF_PRESENTED;
  } else {
    sslCtxConfig.clientVerification =
      folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
  }

  try {
    sslCtxConfig.setCertificate(path.certPath, path.keyPath, "");
    sslCtxConfig.sslVersion = folly::SSLContext::TLSv1_2;
    sslCtxConfig.isDefault = isDefault;
    sslCtxConfig.setNextProtocols({
      std::begin(Cfg::Server::SSLNextProtocols),
      std::end(Cfg::Server::SSLNextProtocols)
    });
  } catch (const std::exception& ex) {
    Logger::Error("Invalid certificate file or key file: %s", ex.what());
  }
  return sslCtxConfig;
}

void ProxygenServer::onRequest(std::shared_ptr<ProxygenTransport> transport) {
  if (CrashingThread.load(std::memory_order_acquire) != 0) {
    Logger::Error("Discarding request while crashing");
    if (m_shutdownState == ShutdownState::SHUTDOWN_NONE) {
      m_shutdownState = ShutdownState::DRAINING_READS;
    }
    m_httpServerSocket.reset();
    m_httpsServerSocket.reset();
    if (Cfg::Server::Five0ThreeOnShutdownAbort) {
      if (Cfg::Server::Five0ThreeRetryAfterSeconds >= 0) {
        transport->addHeader("Retry-After", folly::to<std::string>(
              Cfg::Server::Five0ThreeRetryAfterSeconds).c_str());
      }
      transport->sendString("Service Unavailable", 503);
      transport->onSendEnd();
    } else {
      transport->abort();
    }
    return;
  }

  if (getStatus() == RunStatus::RUNNING ||
      (getStatus() == RunStatus::STOPPING &&
       m_shutdownState <= ShutdownState::DRAINING_READS)) {
    RequestPriority priority = getRequestPriority(transport->getUrl());
    VLOG(4) << this << ": enqueing request with path=" << transport->getUrl() <<
      " and priority=" << priority;
    m_enqueuedCount.fetch_add(1, std::memory_order_acq_rel);
    transport->setEnqueued();
    m_dispatcher.enqueue(std::make_shared<ProxygenJob>(transport), priority);
  } else {
    // VM is shutdown
    if (Cfg::Server::Five0ThreeOnShutdownAbort) {
      if (Cfg::Server::Five0ThreeRetryAfterSeconds >= 0) {
        transport->addHeader("Retry-After", folly::to<std::string>(
              Cfg::Server::Five0ThreeRetryAfterSeconds).c_str());
      }
      transport->sendString("Service Unavailable", 503);
      transport->onSendEnd();
    } else {
      transport->abort();
    }
    Logger::Error("%p: throwing away one new request while shutting down",
                  this);
  }
}

void ProxygenServer::decrementEnqueuedCount() {
  auto const cnt = m_enqueuedCount.fetch_sub(1, std::memory_order_acq_rel);
  if (cnt == 1) {
    auto const evb = m_workers[0]->getEventBase();
    evb->runInEventBaseThread([this] {
      if (m_enqueuedCount.load(std::memory_order_acquire) == 0 &&
          isScheduled()) {
        // If all requests that got enqueued are done, and no more request
        // is coming in, accelerate shutdown.
        if ((m_shutdownState == ShutdownState::DRAINING_READS &&
             !getPendingTransportsCount()) ||
            m_shutdownState == ShutdownState::DRAINING_WRITES) {
          cancelTimeout();
          doShutdown();
        }
      }
    });
  }
}

ProxygenServer::RequestPriority ProxygenServer::getRequestPriority(
  const char *uri) {
  auto const command = URL::getCommand(URL::getServerObject(uri));
  if (Cfg::Server::HighPriorityEndPoints.find(command) ==
      Cfg::Server::HighPriorityEndPoints.end()) {
    return PRIORITY_NORMAL;
  }
  return PRIORITY_HIGH;
}

void ProxygenServer::onRequestError(Transport* transport) {
  if (!m_handler) {
    m_handler = createRequestHandler();
    if (!m_handler) {
      return;
    }
  }

  try {
    timespec start;
    Timer::GetMonotonicTime(start);
    transport->onRequestStart(start);
    m_handler->logToAccessLog(transport);
  } catch (...) {}
}

///////////////////////////////////////////////////////////////////////////////
}
