/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Clock.h"
#include <folly/Overload.h>
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <folly/portability/SysTime.h>
#include <memory>

using namespace watchman;

static int proc_pid;
static uint64_t proc_start_time;

void ClockSpec::init() {
  struct timeval tv;

  proc_pid = (int)::getpid();
  if (gettimeofday(&tv, nullptr) == -1) {
    logf(FATAL, "gettimeofday failed: {}\n", folly::errnoStr(errno));
  }
  proc_start_time = (uint64_t)tv.tv_sec;
}

ClockSpec::ClockSpec(const json_ref& value) {
  auto parseClockString = [=, this](const char* str) {
    uint64_t start_time;
    int pid;
    ClockRoot root_number = 0;
    ClockTicks ticks;
    // Parse a >= 2.8.2 version clock string
    if (sscanf(
            str,
            "c:%" PRIu64 ":%d:%" PRIu64 ":%" PRIu64,
            &start_time,
            &pid,
            &root_number,
            &ticks) == 4) {
      spec = Clock{start_time, pid, ClockPosition{root_number, ticks}};
      return true;
    }

    if (sscanf(str, "c:%d:%" PRIu64, &pid, &ticks) == 2) {
      // old-style clock value (<= 2.8.2) -- by setting clock time and root
      // number to 0 we guarantee that this is treated as a fresh instance
      spec = Clock{0, pid, ClockPosition{root_number, ticks}};
      return true;
    }

    return false;
  };

  switch (value.type()) {
    case JSON_INTEGER:
      spec = Timestamp{static_cast<time_t>(value.asInt())};
      return;

    case JSON_OBJECT: {
      auto clockStr = value.get_optional("clock");
      if (clockStr) {
        if (!parseClockString(json_string_value(*clockStr))) {
          throw std::domain_error("invalid clockspec");
        }
      } else {
        spec = Clock{0, 0, ClockPosition{0, 0}};
      }

      auto scm = value.get_optional("scm");
      if (scm) {
        scmMergeBase = json_to_w_string(
            scm->get_default("mergebase", w_string_to_json("")));
        scmMergeBaseWith = json_to_w_string(scm->get("mergebase-with"));
        auto savedState = scm->get_optional("saved-state");
        if (savedState) {
          savedStateConfig = savedState->get("config");
          savedStateStorageType = json_to_w_string(savedState->get("storage"));
          auto commitId = savedState->get_optional("commit-id");
          if (commitId) {
            savedStateCommitId = json_to_w_string(*commitId);
          } else {
            savedStateCommitId = w_string();
          }
        }
      }

      return;
    }

    case JSON_STRING: {
      auto str = json_string_value(value);

      if (str[0] == 'n' && str[1] == ':') {
        spec = NamedCursor{json_to_w_string(value)};
        return;
      }

      if (parseClockString(str)) {
        return;
      }

      /* fall through to default case and throw error.
       * The redundant looking comment below is a hint to
       * gcc that it is ok to fall through. */
      [[fallthrough]];
    }

    default:
      throw std::domain_error("invalid clockspec");
  }
}

std::unique_ptr<ClockSpec> ClockSpec::parseOptionalClockSpec(
    const json_ref& value) {
  if (value.isNull()) {
    return nullptr;
  }
  return std::make_unique<ClockSpec>(value);
}

ClockSpec::ClockSpec() : spec{Timestamp{0}} {}

ClockSpec::ClockSpec(const ClockPosition& position)
    : spec{Clock{proc_start_time, proc_pid, position}} {}

QuerySince ClockSpec::evaluate(
    const ClockPosition& position,
    ClockTicks lastAgeOutTick,
    folly::Synchronized<std::unordered_map<w_string, ClockTicks>>* cursorMap)
    const {
  return folly::variant_match(
      spec,
      [](const Timestamp& ts) -> QuerySince {
        return QuerySince::Timestamp{ts.time};
      },
      [&](const Clock& clock) -> QuerySince {
        QuerySince::Clock since_clock;
        if (clock.start_time == proc_start_time && clock.pid == proc_pid &&
            clock.position.rootNumber == position.rootNumber) {
          since_clock.origin = ClockOrigin::SameProcess;
          since_clock.is_fresh_instance = clock.position.ticks < lastAgeOutTick;
          if (since_clock.is_fresh_instance) {
            since_clock.ticks = 0;
          } else {
            since_clock.ticks = clock.position.ticks;
          }
        } else {
          // If the pid, start time or root number don't match, they asked a
          // different incarnation of the server or a different instance of this
          // root, so we treat them as having never spoken to us before.
          since_clock.origin = clock.position.rootNumber == position.rootNumber
              ? ClockOrigin::OtherProcess
              : ClockOrigin::OtherRoot;
          since_clock.is_fresh_instance = true;
          since_clock.ticks = 0;
        }
        return since_clock;
      },
      [&](const NamedCursor& named_cursor) -> QuerySince {
        if (!cursorMap) {
          // This is checked for and handled at parse time in SinceExpr::parse,
          // so this should be impossible to hit.
          throw std::runtime_error(
              "illegal to use a named cursor in this context");
        }

        QuerySince::Clock since_clock;

        {
          auto wlock = cursorMap->wlock();
          auto& cursors = *wlock;
          auto it = cursors.find(named_cursor.cursor);

          if (it == cursors.end()) {
            since_clock.is_fresh_instance = true;
            since_clock.ticks = 0;
          } else {
            since_clock.ticks = it->second;
            since_clock.is_fresh_instance = since_clock.ticks < lastAgeOutTick;
          }

          // record the current tick value against the cursor so that we use
          // that as the basis for a subsequent query.
          cursors[named_cursor.cursor] = position.ticks;
        }

        log(DBG,
            "resolved cursor ",
            named_cursor.cursor,
            " -> ",
            since_clock.ticks,
            "\n");

        return since_clock;
      });
}

bool clock_id_string(
    ClockRoot root_number,
    ClockTicks ticks,
    char* buf,
    size_t bufsize) {
  int res = snprintf(
      buf,
      bufsize,
      "c:%" PRIu64 ":%d:%" PRIu64 ":%" PRIu64,
      proc_start_time,
      proc_pid,
      root_number,
      ticks);

  if (res == -1) {
    return false;
  }
  return (size_t)res < bufsize;
}

w_string ClockPosition::toClockString() const {
  char clockbuf[128];
  if (!clock_id_string(rootNumber, ticks, clockbuf, sizeof(clockbuf))) {
    throw std::runtime_error("clock is too big for clockbuf");
  }
  return w_string(clockbuf, W_STRING_UNICODE);
}

json_ref ClockSpec::toJson() const {
  if (scmMergeBase) {
    auto scm = json_object(
        {{"mergebase", w_string_to_json(scmMergeBase.value())},
         {"mergebase-with", w_string_to_json(scmMergeBaseWith)}});
    if (savedStateStorageType) {
      auto savedState = json_object(
          {{"storage", w_string_to_json(savedStateStorageType.value())},
           {"config", savedStateConfig.value()}});
      if (savedStateCommitId != w_string()) {
        json_object_set(
            savedState, "commit-id", w_string_to_json(savedStateCommitId));
      }
      json_object_set(scm, "saved-state", savedState);
    }
    return json_object(
        {{"clock", w_string_to_json(position().toClockString())},
         {"scm", scm}});
  }
  return w_string_to_json(position().toClockString());
}

bool ClockSpec::hasScmParams() const {
  return scmMergeBase.has_value();
}

bool ClockSpec::hasSavedStateParams() const {
  return savedStateStorageType.has_value();
}

/* vim:ts=2:sw=2:et:
 */
