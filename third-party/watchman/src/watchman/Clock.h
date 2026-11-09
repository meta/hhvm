/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Synchronized.h>
#include <unordered_map>
#include <variant>
#include "watchman/Logging.h"

namespace watchman {

using ClockTicks = uint64_t;
using ClockRoot = uint64_t;

struct ClockStamp {
  ClockTicks ticks;
  time_t timestamp;
};

/**
 * Where the clock in a since clockspec came from, relative to this watchman
 * process and the root's current instance.
 */
enum class ClockOrigin {
  // No since, or a timestamp or named cursor since.
  None,
  SameProcess,
  // Not from this process, but it has the root's current root number. Only
  // EdenFS root numbers (mount generations) name the same root instance in
  // every watchman process.
  OtherProcess,
  // Its root number is not the current one. Old-style clocks and scm since
  // specs without a clock have root number 0.
  OtherRoot,
};

struct QuerySince {
  struct Timestamp {
    time_t time;
  };
  struct Clock {
    bool is_fresh_instance{false};
    ClockTicks ticks{0};
    ClockOrigin origin{ClockOrigin::None};
  };
  std::variant<Timestamp, Clock> since;

  QuerySince() : since{Clock{true, 0}} {}
  /* implicit */ QuerySince(Timestamp ts) : since{ts} {}
  /* implicit */ QuerySince(Clock clock) : since{clock} {}

  bool is_timestamp() const {
    return std::holds_alternative<Timestamp>(since);
  }

  /**
   * Throws if this holds a Timestamp.
   */
  bool is_fresh_instance() const {
    return std::get<Clock>(since).is_fresh_instance;
  }

  /**
   * Set the clock to a fresh instance.
   *
   * Throws if this holds a Timestamp.
   */
  void set_fresh_instance() {
    std::get<Clock>(since).is_fresh_instance = true;
  }
};

struct ClockPosition {
  ClockRoot rootNumber{0};
  ClockTicks ticks{0};

  ClockPosition() = default;
  ClockPosition(ClockRoot rootNumber, ClockTicks ticks)
      : rootNumber(rootNumber), ticks(ticks) {}

  w_string toClockString() const;
};

struct ClockSpec {
  struct Timestamp {
    time_t time;
  };

  struct Clock {
    uint64_t start_time;
    int pid;
    ClockPosition position;
  };

  struct NamedCursor {
    w_string cursor;
  };

  std::variant<Timestamp, Clock, NamedCursor> spec;

  // Optional SCM merge base parameters
  std::optional<w_string> scmMergeBase;
  w_string scmMergeBaseWith;
  // Optional saved state parameters
  std::optional<json_ref> savedStateConfig;
  std::optional<w_string> savedStateStorageType;
  w_string savedStateCommitId;

  ClockSpec();
  explicit ClockSpec(const ClockPosition& position);
  explicit ClockSpec(const json_ref& value);

  /** Given a json value, parse out a clockspec.
   * Will return nullptr if the input was json null, indicating
   * an absence of a specified clock value.
   * Throws std::domain_error for badly formed clockspec value.
   */
  static std::unique_ptr<ClockSpec> parseOptionalClockSpec(
      const json_ref& value);

  /** Evaluate the clockspec against the inputs, returning
   * the effective since parameter.
   * If cursorMap is passed in, it MUST be unlocked, as this method
   * will acquire a lock to evaluate a named cursor. */
  QuerySince evaluate(
      const ClockPosition& position,
      const ClockTicks lastAgeOutTick,
      folly::Synchronized<std::unordered_map<w_string, ClockTicks>>* cursorMap =
          nullptr) const;

  /** Initializes some global state needed for clockspec evaluation */
  static void init();

  inline const ClockPosition& position() const {
    auto* c = std::get_if<Clock>(&spec);
    w_check(c, "position() called for non-clock clockspec");
    return c->position;
  }

  bool hasScmParams() const;
  bool hasSavedStateParams() const;

  /** Returns a json value representing the current state of this ClockSpec
   * that can be parsed by the ClockSpec(const json_ref&)
   * constructor of this class */
  json_ref toJson() const;
};

} // namespace watchman

bool clock_id_string(
    watchman::ClockRoot root_number,
    watchman::ClockTicks ticks,
    char* buf,
    size_t bufsize);
