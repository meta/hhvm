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

#include "hphp/runtime/server/warmup-request-handler.h"

#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/ext/server/ext_server.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/replay-transport.h"

#include "hphp/util/boot-stats.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/hash-set.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/timer.h"

#include <fmt/format.h>
#include <folly/Range.h>
#include <folly/Memory.h>

#include <filesystem>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

void WarmupRequestHandler::setupRequest(Transport* transport) {
  m_reqHandler.setupRequest(transport);
}

void WarmupRequestHandler::teardownRequest(Transport* transport) noexcept {
  m_reqHandler.teardownRequest(transport);
}

void WarmupRequestHandler::handleRequest(Transport *transport) {
  // There is one WarmupRequestHandler per-thread, but we want to track request
  // count across all threads.  Therefore we let WarmupRequestHandlerFactory
  // track the global request count.
  m_factory->bumpReqCount();
  m_reqHandler.handleRequest(transport);
}

void WarmupRequestHandler::abortRequest(Transport *transport) {
  m_reqHandler.abortRequest(transport);
}

void WarmupRequestHandler::logToAccessLog(Transport *transport) {
  m_reqHandler.logToAccessLog(transport);
}

std::unique_ptr<RequestHandler> WarmupRequestHandlerFactory::createHandler() {
  return
    std::make_unique<WarmupRequestHandler>(m_timeout, shared_from_this());
}

void WarmupRequestHandlerFactory::bumpReqCount() {
  // Bump the request count.  When we hit m_warmupReqThreshold,
  // add additional threads to the server.
  auto const oldReqNum = m_reqNumber.fetch_add(1, std::memory_order_acq_rel);
  if (oldReqNum != m_warmupReqThreshold) {
    return;
  }

  Logger::Info("Finished warmup; saturating worker threads");
  m_server->saturateWorkers();
}

void InternalWarmupWorker::doJob(WarmupJob job) {
  if (HHVM_FN(server_is_stopping)()) return;
  if (HHVM_FN(server_uptime)() > 0 &&
      jit::mcgen::retranslateAllScheduled()) return;
  HttpServer::CheckMemAndWait();
  folly::StringPiece f(job.hdfFile);
  auto const pos = f.rfind('/');
  auto const str = (pos == f.npos) ? f : f.subpiece(pos + 1);
  BootStats::Block timer(fmt::format("warmup:{}:{}", str, job.index),
                         Cfg::Server::Mode);
  try {
    HttpRequestHandler handler(0);
    ReplayTransport rt;
    Logger::FInfo("Replaying warmup request {}:{}", job.hdfFile, job.index);
    timespec start;
    Timer::GetMonotonicTime(start);
    rt.onRequestStart(start);
    rt.replayInput(Hdf(job.hdfFile));
    handler.run(&rt);
  } catch (std::exception& e) {
    Logger::FWarning("Got exception during warmup request {}:{}, {}",
                     job.hdfFile, job.index, e.what());
  }
}

InternalWarmupRequestPlayer::InternalWarmupRequestPlayer(int threadCount,
                                                         bool dedup)
  : JobQueueDispatcher<InternalWarmupWorker>(threadCount, threadCount,
                                             0, false, nullptr)
  , m_noDuplicate(dedup) {
  start();
}

InternalWarmupRequestPlayer::~InternalWarmupRequestPlayer() {
  waitEmpty();
}

void InternalWarmupRequestPlayer::
runAfterDelay(const std::vector<std::string>& files,
              unsigned nTimes, unsigned delaySeconds) {
  if (nTimes == 0) return;
  if (delaySeconds) {
    /* sleep override */
    sleep(delaySeconds);
  }
  hphp_fast_string_map<unsigned> seen;
  hphp_fast_string_set deduped;
  do {
    deduped.clear();
    for (auto const& file : files) {
      if (m_noDuplicate && !deduped.insert(file).second) continue;
      try {
        std::filesystem::path p(file);
        if (std::filesystem::is_regular_file(p)) {
          enqueue(WarmupJob{file, ++seen[file]});
        } else if (std::filesystem::is_directory(p)) {
          for (auto const& f : std::filesystem::directory_iterator(p)) {
            if (std::filesystem::is_regular_file(f.path())) {
              std::string subFile = f.path().native();
              // Only do it for .hdf files.
              if (subFile.size() < 5 ||
                  subFile.substr(subFile.size() - 4) != ".hdf") {
                Logger::FWarning("Skipping {} for warmup because it doesn't "
                                 "look like a .hdf file", subFile);
                continue;
              }
              enqueue(WarmupJob{subFile, ++seen[subFile]});
            }
          }
        }
      } catch (std::exception& e) {
        Logger::FError("Exception preparing warmup requests: {}", e.what());
      }
    }
  } while (--nTimes);
  // Log what was replayed.
  if (StructuredLog::enabled()) {
    for (const auto& row : seen) {
      StructuredLogEntry cols;
      cols.setStr("file", row.first);
      cols.setInt("times", row.second);
      StructuredLog::log("hhvm_replay", cols);
    }
  }
}

namespace {
InternalWarmupRequestPlayer* GetReplayer() {
  if (!Cfg::Server::ExtendedWarmupThreadCount) return nullptr;
  static InternalWarmupRequestPlayer* instance =
    new InternalWarmupRequestPlayer(Cfg::Server::ExtendedWarmupThreadCount);
  return instance;
}

static InitFiniNode _([] { delete GetReplayer(); },
                      InitFiniNode::When::ServerExit);
}

void replayExtendedWarmupRequests() {
  if (!Cfg::Server::Mode) return;
  auto const threadCount = Cfg::Server::ExtendedWarmupThreadCount;
  if (threadCount <= 0) return;
  if (Cfg::Server::ExtendedWarmupRequests.empty()) return;
  auto const delay = Cfg::Server::ExtendedWarmupDelaySeconds;
  auto const nTimes = Cfg::Server::ExtendedWarmupRepeat;
  GetReplayer()->runAfterDelay(Cfg::Server::ExtendedWarmupRequests,
                            nTimes, delay);
}

///////////////////////////////////////////////////////////////////////////////
}
