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

#include "hphp/runtime/base/request-fanout-limit.h"

#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/server/server-note.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/logger.h"
#include "hphp/util/struct-log.h"

#include <fmt/format.h>

namespace HPHP {

static std::atomic<int64_t> s_maxConcurrentFanoutCount = 0;

bool RequestFanoutLimit::RequestFanoutData::increment() {
  /**
  * This operation is safe from operating on abandoned
  * counter erased by other racing xbox threads, because:
  * - The counter is always initialized to 2 during insertion to account for root-req, AND
  * - All increment for children request will happen before enqueue()
  *   and inside the parent request's lifetime, ensures the count will always > 0
  *   as long as any offspring xbox request is running
  */
  auto countBeforeIncrement = currentCount.fetch_add(1, std::memory_order_acq_rel);
  auto newCount = countBeforeIncrement + 1;

  /**
   * Check the new count is below the configured fanout limit
   */
  if (Cfg::Server::EnforceRequestFanout && newCount > Cfg::Server::RequestFanoutLimit) {
    // In enforcement mode, we will throw an exception if the limit is exceeded
    // and the request will not be queued for execution, so we need to rollback
    // the incremented counter.
    currentCount.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }

  /**
   * Update the max concurrent count for the given root req
   */
  auto prevMax = maxCount.load(std::memory_order_acquire);
  while (newCount > prevMax
    && !maxCount.compare_exchange_weak(
      prevMax,
      newCount,
      std::memory_order_acq_rel,
      std::memory_order_acquire)
    ) {
      // prevMax updated by compare_exchange_weak
  }

  return true;
}

int64_t RequestFanoutLimit::RequestFanoutData::decrement() {
  return currentCount.fetch_sub(1, std::memory_order_acq_rel);
}

RequestFanoutLimit::RequestFanoutLimit(int limit, int size)
  : m_limit(limit), m_map(size) {
  assertx(limit > 0);
  assertx(size > 0);
  if (Cfg::Server::EnableRequestFanoutLogging) {
    m_fanoutLogger = ServiceData::createCounter("request_fanout.max_concurrent_count");
  }
}

void RequestFanoutLimit::increment(const RequestId& id) {
  if (id.unallocated()) {
    return;
  }

  auto it = m_map.find(id.id());
  if (it == m_map.end()) {
    // Inserting 2 here (instead of 1) to include parent request as part of tracking.
    // This, and together with all increment for children request will happen before
    // enqueue() and inside the parent request's lifetime, ensures the count will
    // always > 0 as long as any offspring children (i.e. xbox) request is running,
    // which avoids operating on abandoned counter erased by other racing children
    // (i.e. xbox) threads.
    auto insertResult = m_map.insert(
      id.id(),
      std::make_shared<RequestFanoutLimit::RequestFanoutData>(2, 2)
    );
  } else {
    auto counter = it->second.get();
    assertx(counter);
    if (!counter->increment()) {
      if (Cfg::Server::EnableRequestFanoutLogging
          && StructuredLog::enabled()
          && Cfg::Server::RequestFanoutEnforcementLoggingSampleRate != 0
          && id.id() % Cfg::Server::RequestFanoutEnforcementLoggingSampleRate == 0 ) {
        StructuredLogEntry entry;
        entry.setStr("event", "FANOUT_LIMIT_EXCEEDED");
        entry.setInt("request_max_concurrent_fanout_count", counter->currentCount);
        if (!counter->rootReqScriptFilename.empty()) {
          entry.setStr("script_filename", counter->rootReqScriptFilename);
        }
        StructuredLog::log("hhvm_request_fanout", entry);
      }
      auto msg = fmt::format("Request fanout limit exceeded.");
      SystemLib::throwRequestFanoutLimitExceededExceptionObject(msg);
    }
  }
}

void RequestFanoutLimit::decrement(const RequestId& id) {
  if (id.unallocated()) {
    return;
  }

  auto it = m_map.find(id.id());
  if (it == m_map.end()) {
    // This check is safe and necessary to handle the cases of cli-server also
    // calling RI.onSessionExit() during SCOPE_EXIT.
    return;
  }

  auto counter = it->second.get();
  assertx(counter);

  auto countBeforeDecrement = counter->decrement();

  if (countBeforeDecrement == 1) {
    // Update the global server max counter if current root-req has spawn
    // more children req than any other previous root-req
    auto prevGlobalMax = s_maxConcurrentFanoutCount.load(std::memory_order_acquire);
    auto currReqMax = counter->maxCount.load(std::memory_order_acquire);
    while (
      currReqMax > prevGlobalMax
      && !s_maxConcurrentFanoutCount.compare_exchange_weak(
        prevGlobalMax,
        currReqMax,
        std::memory_order_acq_rel,
        std::memory_order_acquire
      )
    ) {
      // global server max updated by compare_exchange_weak
    }

    if (Cfg::Server::RequestFanoutLoggingSampleRate != 0
      && id.id() % Cfg::Server::RequestFanoutLoggingSampleRate == 0) {
      // Log the up-to-date server maxConcurrentFanoutCount to ODS
      if (m_fanoutLogger) {
        m_fanoutLogger->setValue(s_maxConcurrentFanoutCount.load(std::memory_order_acquire));
      }
      // Log maxConcurrentCount of current root request to Scuba
      if (Cfg::Server::EnableRequestFanoutLogging && StructuredLog::enabled()) {
        StructuredLogEntry entry;
        entry.setStr("event", "REQUEST_GROUP_FINISHED");
        entry.setInt("request_max_concurrent_fanout_count", currReqMax);
        if (!counter->rootReqScriptFilename.empty()) {
          entry.setStr("script_filename", counter->rootReqScriptFilename);
        }
        StructuredLog::log("hhvm_request_fanout", entry);
      }
    }

    // Erase the entry
    m_map.erase(id.id());
    Logger::Verbose("All children requests of %ld completed", id.id());
  }
}

void RequestFanoutLimit::setScriptFilename(const RequestId id, std::string scriptFilename) {
  if (id.unallocated()) {
    return;
  }

  auto it = m_map.find(id.id());
  if (it == m_map.end()) {
    // This check is safe and necessary to handle the cases of cli-server also
    // calling RI.onSessionExit() during SCOPE_EXIT.
    return;
  }

  auto counter = it->second.get();
  assertx(counter);

  if (!scriptFilename.empty()) {
    counter->rootReqScriptFilename = scriptFilename;
  }
}

int RequestFanoutLimit::getCurrentCount(const RequestId& id) const {
  auto it = m_map.find(id.id());
  if (it == m_map.end()) {
    return -1;
  }

  auto counter = it->second.get();
  if (counter == nullptr) {
    return -1;
  }

  return counter->currentCount.load(std::memory_order_acquire);
}

int RequestFanoutLimit::getMaxCount(const RequestId& id) const {
  auto it = m_map.find(id.id());
  if (it == m_map.end()) {
    return -1;
  }

  auto counter = it->second.get();
  if (counter == nullptr) {
    return -1;
  }

  return counter->maxCount.load(std::memory_order_acquire);
}

} // namespace HPHP
