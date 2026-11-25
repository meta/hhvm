/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GTest.h>
#include <cstdlib>
#include "watchman/telemetry/LogEvent.h"
#include "watchman/telemetry/WatchmanStructuredLogger.h"

using namespace watchman;

// Detached threads (client threads, the sanity-check thread) can still log
// events while the process exits. Model that deterministically: this exit
// handler is registered before the logger is first used, so it runs after any
// static destructor the logger registers.
TEST(
    StructuredLoggerExit,
    logging_events_still_works_after_static_destructors_ran) {
  std::atexit([] {
    DispatchCommand event;
    event.command = "logged during exit";
    getLogger()->logEvent(event);
  });
  DispatchCommand event;
  event.command = "first use of the logger";
  getLogger()->logEvent(event);
}
