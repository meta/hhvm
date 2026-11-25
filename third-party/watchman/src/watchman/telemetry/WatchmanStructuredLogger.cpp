/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/telemetry/WatchmanStructuredLogger.h"

#include <folly/Indestructible.h>

#include "eden/common/telemetry/StructuredLoggerFactory.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/telemetry/WatchmanStats.h"

#ifdef WATCHMAN_FACEBOOK_INTERNAL
#include <chrono>
#include <cstdint>

#include "eden/common/telemetry/facebook/XplatLoggerCore.h"
#include "eden/fs/telemetry/IXplatLogger.h"
#include "watchman/telemetry/XplatKeys.h"
#include "watchman/telemetry/facebook/WatchmanXplatLogger.h" // @manual
#endif

namespace watchman {

using UserInfo = facebook::eden::UserInfo;

WatchmanStructuredLogger::WatchmanStructuredLogger(
    std::shared_ptr<ScribeLogger> scribeLogger,
    SessionInfo sessionInfo)
    : ScubaStructuredLogger{std::move(scribeLogger), std::move(sessionInfo)} {}

#ifdef WATCHMAN_FACEBOOK_INTERNAL
namespace {

/// Routes the XplatLoggerCore delivery counters onto WatchmanStats, reusing the
/// same TelemetryStats counters as the EdenFS sink so the metric names match.
class WatchmanXplatStatsSink : public facebook::eden::XplatLoggerStatsSink {
 public:
  void messageEnqueued() override {
    getWatchmanStats()->increment(&TelemetryStats::xplatMessagesEnqueued);
  }
  void messagesWritten(uint64_t count) override {
    getWatchmanStats()->increment(
        &TelemetryStats::xplatMessagesWritten, static_cast<int64_t>(count));
  }
  void messageDroppedQueueFull() override {
    getWatchmanStats()->increment(
        &TelemetryStats::xplatMessagesDroppedQueueFull);
  }
  void messageDroppedShutdown() override {
    getWatchmanStats()->increment(
        &TelemetryStats::xplatMessagesDroppedShutdown);
  }
  void writeZeroOkRecords() override {
    getWatchmanStats()->increment(&TelemetryStats::xplatWriteZeroOkRecords);
  }
  void writeFailure() override {
    getWatchmanStats()->increment(&TelemetryStats::xplatWriteFailures);
  }
  void backoffWait() override {
    getWatchmanStats()->increment(&TelemetryStats::xplatBackoffWaits);
  }
  void messagesDroppedWriteFailures(uint64_t count) override {
    getWatchmanStats()->increment(
        &TelemetryStats::xplatMessagesDroppedWriteFailures,
        static_cast<int64_t>(count));
  }
};

/// Builds the delivery-core tunables from watchman config. Defaults match the
/// XplatLoggerOptions defaults (i.e. the historical hard-coded EdenFS values).
facebook::eden::XplatLoggerOptions optionsFromWatchmanConfig() {
  facebook::eden::XplatLoggerOptions options;
  options.queueLimitBytes = static_cast<size_t>(cfg_get_int(
      "xplatlogger-queue-limit-bytes",
      static_cast<json_int_t>(options.queueLimitBytes)));
  options.maxBatchSize = static_cast<size_t>(cfg_get_int(
      "xplatlogger-max-batch-size",
      static_cast<json_int_t>(options.maxBatchSize)));
  options.maxConsecutiveFailures = static_cast<size_t>(cfg_get_int(
      "xplatlogger-max-consecutive-failures",
      static_cast<json_int_t>(options.maxConsecutiveFailures)));
  options.initialBackoff = std::chrono::milliseconds{cfg_get_int(
      "xplatlogger-initial-backoff-ms", options.initialBackoff.count())};
  options.maxBackoff = std::chrono::milliseconds{
      cfg_get_int("xplatlogger-max-backoff-ms", options.maxBackoff.count())};
  options.flushTimeout = std::chrono::milliseconds{cfg_get_int(
      "xplatlogger-flush-timeout-ms", options.flushTimeout.count())};
  options.connectTimeout = std::chrono::milliseconds{cfg_get_int(
      "xplatlogger-connect-timeout-ms", options.connectTimeout.count())};
  options.rpcTimeout = std::chrono::milliseconds{
      cfg_get_int("xplatlogger-rpc-timeout-ms", options.rpcTimeout.count())};
  return options;
}

/// StructuredLogger that routes events to an XplatLogger instead of the
/// scribe_cat pipeline. Inherits
/// WatchmanStructuredLogger::populateDefaultFields (identity population) and
/// StructuredLogger's sampling-aware logEvent entry points, then overrides
/// delivery to forward the populated DynamicEvent to the XplatLogger under the
/// watchman_events category.
class WatchmanXplatStructuredLogger : public WatchmanStructuredLogger {
 public:
  WatchmanXplatStructuredLogger(
      SessionInfo sessionInfo,
      std::shared_ptr<facebook::eden::IXplatLogger> xplatLogger)
      : WatchmanStructuredLogger{nullptr, std::move(sessionInfo)},
        xplatLogger_{std::move(xplatLogger)} {}

 protected:
  void logDynamicEvent(DynamicEvent event) override {
    xplatLogger_->logEvent(xplat_keys::kWatchmanEventsCategory, event);
  }

 private:
  std::shared_ptr<facebook::eden::IXplatLogger> xplatLogger_;
};

} // namespace

std::shared_ptr<StructuredLogger> makeWatchmanXplatStructuredLogger(
    SessionInfo sessionInfo,
    std::shared_ptr<facebook::eden::IXplatLogger> xplatLogger) {
  return std::make_shared<WatchmanXplatStructuredLogger>(
      std::move(sessionInfo), std::move(xplatLogger));
}
#endif // WATCHMAN_FACEBOOK_INTERNAL

std::shared_ptr<StructuredLogger> getLogger() {
  auto makeLogger = []() -> std::shared_ptr<StructuredLogger> {
    auto sessionInfo = facebook::eden::makeSessionInfo(
        UserInfo::lookup(), facebook::eden::getHostname(), PACKAGE_VERSION);
#ifdef WATCHMAN_FACEBOOK_INTERNAL
    if (cfg_get_bool("enable-xplatlogger-watchman-events", false)) {
      return makeWatchmanXplatStructuredLogger(
          std::move(sessionInfo),
          makeWatchmanXplatLogger(
              optionsFromWatchmanConfig(),
              std::make_shared<WatchmanXplatStatsSink>()));
    }
#endif
    return facebook::eden::
        makeDefaultStructuredLogger<WatchmanStructuredLogger, WatchmanStatsPtr>(
            cfg_get_string("scribe-cat", ""),
            cfg_get_string("scribe-category", ""),
            std::move(sessionInfo),
            getWatchmanStats());
  };
  // Never destroyed: detached threads can still log events during exit.
  static folly::Indestructible<std::shared_ptr<StructuredLogger>> logger{
      makeLogger()};
  return *logger;
}

DynamicEvent WatchmanStructuredLogger::populateDefaultFields(
    std::optional<const char*> type) {
  DynamicEvent event = StructuredLogger::populateDefaultFields(type);
  event.addString("version", sessionInfo_.appVersion);
#ifdef WATCHMAN_BUILD_INFO
  event.addString("buildinfo", WATCHMAN_BUILD_INFO);
#endif
  event.addString("logged_by", "watchman");

  const auto& fbInfo = sessionInfo_.fbInfo;
  for (const auto& info : fbInfo) {
    const auto& key = info.first;
    const auto& value = info.second;
    std::visit(
        [&](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>) {
            event.addString(key, v);
          } else if constexpr (std::is_same_v<T, uint64_t>) {
            event.addInt(key, v);
          }
        },
        value);
  }

  return event;
}

} // namespace watchman
