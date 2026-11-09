/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/portability/SysTypes.h>
#include <folly/stop_watch.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "eden/common/telemetry/DynamicEvent.h"
#include "eden/common/telemetry/LogEvent.h"

namespace watchman {

using DynamicEvent = facebook::eden::DynamicEvent;
using TypedEvent = facebook::eden::TypedEvent;

struct WatchmanBaseEvent : public TypedEvent {
  // TODO: add system and user time for Unix systems
  std::chrono::time_point<std::chrono::system_clock> start_time =
      std::chrono::system_clock::now();
  std::string root;
  std::string error;
  int64_t event_count = 1;

  void populate(DynamicEvent& event) const override {
    std::chrono::duration<int64_t, std::nano> elapsed_time =
        std::chrono::system_clock::now() - start_time;
    event.addInt(
        "start_time", std::chrono::system_clock::to_time_t(start_time));
    event.addInt(
        "elapsed_time",
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time)
            .count());
    if (!root.empty()) {
      event.addString("root", root);
    }
    if (!error.empty()) {
      event.addString("error", error);
    }
    event.addInt("event_count", event_count);
  }

  // Keep getType() pure virtual to force subclasses to implement it
  virtual const char* getType() const override = 0;
};

struct WatchmanEvent : public WatchmanBaseEvent {
  int64_t recrawl = 0;
  bool case_sensitive = false;
  std::string watcher;
  pid_t client_pid = 0;
  std::string client_name;

  void populate(DynamicEvent& event) const override {
    WatchmanBaseEvent::populate(event);
    event.addInt("recrawl", recrawl);
    event.addBool("case_sensitive", case_sensitive);
    if (!watcher.empty()) {
      event.addString("watcher", watcher);
    }

    if (client_pid != 0) {
      event.addInt("client_pid", client_pid);
    }
    if (!client_name.empty()) {
      event.addString("client", client_name);
    }
  }

  // Keep getType() pure virtual to force subclasses to implement it
  virtual const char* getType() const override = 0;
};

enum LogEventType : uint8_t {
  DispatchCommandType,
  ClockTestType,
  AgeOutType,
  SyncToNowType,
  SavedStateType,
  QueryExecuteType,
  FullCrawlType,
  DroppedType,
  // Sentinel used to size counter storage; this is not an event type.
  LogEventTypeCount,
};

// Returns samplingRate and eventCount
std::pair<int64_t, int64_t> getLogEventCounters(const LogEventType& type);

struct DispatchCommand : public WatchmanEvent {
  std::string command;
  std::string args;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    event.addString("command", command);
    if (!args.empty()) {
      event.addString("args", args);
    }
  }

  const char* getType() const override {
    return "dispatch_command";
  }
};

struct ClockTest : public WatchmanBaseEvent {
  void populate(DynamicEvent& event) const override {
    WatchmanBaseEvent::populate(event);
  }

  const char* getType() const override {
    return "clock_test";
  }
};

struct AgeOut : public WatchmanEvent {
  int64_t walked = 0;
  int64_t files = 0;
  int64_t dirs = 0;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    event.addInt("walked", walked);
    event.addInt("files", files);
    event.addInt("dirs", dirs);
  }

  const char* getType() const override {
    return "age_out";
  }
};

struct SyncToNow : public WatchmanEvent {
  bool success = true;
  int64_t timeoutms = 0;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    event.addBool("success", success);
    event.addInt("timeoutms", timeoutms);
  }

  const char* getType() const override {
    return "sync_to_now";
  }
};

struct SavedState : public WatchmanEvent {
  enum Target { Manifold = 1, Xdb = 2 };
  enum Action { GetProperties = 1, Connect = 2, Query = 3 };

  Target target = Manifold;
  Action action = GetProperties;
  std::string project;
  std::string path;
  int64_t commit_date = 0;
  std::optional<std::string> projectMetadata;
  std::string properties;
  bool success = false;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    event.addInt("target", target);
    event.addInt("action", action);
    event.addString("project", project);
    if (!path.empty()) {
      event.addString("path", path);
    }
    event.addInt("commit_date", commit_date);
    if (projectMetadata.has_value()) {
      event.addString("metadata", projectMetadata.value());
    }
    if (!properties.empty()) {
      event.addString("properties", properties);
    }
    event.addBool("success", success);
  }

  const char* getType() const override {
    return "saved_state";
  }
};

struct QueryExecute : public WatchmanEvent {
  std::string request_id;
  int64_t num_special_files = 0;
  std::string special_files;
  bool fresh_instance = false;
  std::string fresh_instance_cause;
  std::string since_clock_eval;
  bool saved_state_missing = false;
  int64_t deduped = 0;
  int64_t results = 0;
  int64_t walked = 0;
  std::string query;
  int64_t eden_glob_files_duration_us = 0;
  int64_t eden_changed_files_duration_us = 0;
  int64_t eden_file_properties_duration_us = 0;
  int64_t scm_files_changed_since_mergebase_with_duration_us = 0;
  std::string generator;
  int64_t generation_duration_ms = 0;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    if (!request_id.empty()) {
      event.addString("request_id", request_id);
    }
    event.addInt("num_special_files", num_special_files);
    if (!special_files.empty()) {
      event.addString("special_files", special_files);
    }
    event.addBool("fresh_instance", fresh_instance);
    if (!query.empty()) {
      event.addString("fresh_instance_cause", fresh_instance_cause);
    }
    if (!since_clock_eval.empty()) {
      event.addString("since_clock_eval", since_clock_eval);
    }
    event.addInt("deduped", deduped);
    event.addInt("results", results);
    event.addInt("walked", walked);
    if (!query.empty()) {
      event.addString("query", query);
    }
    if (eden_glob_files_duration_us != 0) {
      event.addInt("eden_glob_files_duration_us", eden_glob_files_duration_us);
    }
    if (eden_changed_files_duration_us != 0) {
      event.addInt(
          "eden_changed_files_duration_us", eden_changed_files_duration_us);
    }
    if (eden_file_properties_duration_us != 0) {
      event.addInt(
          "eden_file_properties_duration_us", eden_file_properties_duration_us);
    }
    if (scm_files_changed_since_mergebase_with_duration_us != 0) {
      event.addInt(
          "scm_files_changed_since_mergebase_with_duration_us",
          scm_files_changed_since_mergebase_with_duration_us);
    }
    if (!generator.empty()) {
      event.addString("generator", generator);
    }
    if (generation_duration_ms != 0) {
      event.addInt("generation_duration_ms", generation_duration_ms);
    }
    if (saved_state_missing) {
      event.addBool("saved_state_missing", saved_state_missing);
    }
  }

  const char* getType() const override {
    return "query_execute";
  }
};

struct FullCrawl : public WatchmanEvent {
  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
  }

  const char* getType() const override {
    return "full_crawl";
  }
};

struct Dropped : public WatchmanEvent {
  bool isKernel = false;

  void populate(DynamicEvent& event) const override {
    WatchmanEvent::populate(event);
    event.addBool("isKernel", isKernel);
  }

  const char* getType() const override {
    return "dropped";
  }
};

} // namespace watchman
