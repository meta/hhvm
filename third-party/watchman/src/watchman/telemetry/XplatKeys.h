/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string_view>

/*
 * Key constants for XplatLogger's DynamicEvent key-value bags for the
 * watchman_events Scuba/Hive table.
 *
 * XplatLogger logs to arbitrary Scuba tables via a generic
 * logEvent(category, DynamicEvent) method. A DynamicEvent is a bag of
 * key-value pairs. For watchman, both the event-specific fields (populated
 * by watchman/telemetry/LogEvent.h) AND the shared identity fields (user,
 * host, os, osver, version, logged_by, ... populated by
 * WatchmanStructuredLogger::populateDefaultFields) live directly in the
 * DynamicEvent maps. The watchman transform therefore reads every field --
 * identity included -- straight from those maps.
 *
 * These constants are shared between call sites (which populate the
 * DynamicEvent) and the transform function (which extracts values to build
 * the typed WatchmanEventsEntry Thrift struct). Key names match the Scuba
 * column names exactly for debuggability, except where the DynamicEvent key
 * historically differs from the wire column (see kArgs / kIsKernel below).
 *
 * Modeled on eden/fs/telemetry/XplatKeys.h.
 */
namespace watchman::xplat_keys {

// --- watchman_events category ---
inline constexpr std::string_view kWatchmanEventsCategory{
    "perfpipe_watchman_events"};

// ===================================================================
// Identity / common fields
//
// Populated by WatchmanStructuredLogger::populateDefaultFields and the
// shared session-info plumbing (StructuredLogger + FacebookSessionInfo).
// ===================================================================
inline constexpr std::string_view kUser = "user";
inline constexpr std::string_view kHost = "host";
inline constexpr std::string_view kOs = "os";
inline constexpr std::string_view kOsVer = "osver";
inline constexpr std::string_view kSystemArchitecture = "system_architecture";
inline constexpr std::string_view kVersion = "version";
inline constexpr std::string_view kBuildInfo = "buildinfo";
inline constexpr std::string_view kLoggedBy = "logged_by";
inline constexpr std::string_view kType = "type";
inline constexpr std::string_view kSessionId = "session_id";
inline constexpr std::string_view kSandcastleInstanceId =
    "sandcastle_instance_id";
inline constexpr std::string_view kSandcastleAlias = "sandcastle_alias";
inline constexpr std::string_view kAtlasEnvId = "atlas_env_id";
inline constexpr std::string_view kCesId = "ces_id";
inline constexpr std::string_view kSystemFingerprint = "system_fingerprint";
inline constexpr std::string_view kOndemandType = "ondemand_type";
inline constexpr std::string_view kOndemandFlavor = "ondemand_flavor";
inline constexpr std::string_view kDevserverType = "devserver_type";
inline constexpr std::string_view kDevserverIsShortTermLease =
    "devserver_is_short_term_lease";
inline constexpr std::string_view kDevserverState = "devserver_state";
inline constexpr std::string_view kAgenticFingerprintId =
    "agentic_fingerprint_id";
inline constexpr std::string_view kAgenticFingerprintInvocationId =
    "agentic_fingerprint_invocation_id";

// ===================================================================
// Event-specific fields (from watchman/telemetry/LogEvent.h)
// ===================================================================

// --- String fields ---
inline constexpr std::string_view kRoot = "root";
inline constexpr std::string_view kError = "error";
inline constexpr std::string_view kWatcher = "watcher";
inline constexpr std::string_view kClient = "client";
inline constexpr std::string_view kCommand = "command";
// DynamicEvent key is "args"; the wire column is "args" but the TulipV2 config
// mangles the reserved column to the thrift field name "field_args".
inline constexpr std::string_view kArgs = "args";
inline constexpr std::string_view kProject = "project";
inline constexpr std::string_view kPath = "path";
inline constexpr std::string_view kMetadata = "metadata";
inline constexpr std::string_view kProperties = "properties";
inline constexpr std::string_view kSpecialFiles = "special_files";
inline constexpr std::string_view kFreshInstanceCause = "fresh_instance_cause";
inline constexpr std::string_view kSinceClockEval = "since_clock_eval";
inline constexpr std::string_view kQuery = "query";
inline constexpr std::string_view kGenerator = "generator";

// --- Int fields (includes bool fields, stored as 0/1 in the int map) ---
inline constexpr std::string_view kStartTime = "start_time";
inline constexpr std::string_view kElapsedTime = "elapsed_time";
inline constexpr std::string_view kEventCount = "event_count";
inline constexpr std::string_view kRecrawl = "recrawl";
inline constexpr std::string_view kCaseSensitive = "case_sensitive";
inline constexpr std::string_view kClientPid = "client_pid";
inline constexpr std::string_view kWalked = "walked";
inline constexpr std::string_view kFiles = "files";
inline constexpr std::string_view kDirs = "dirs";
inline constexpr std::string_view kSuccess = "success";
inline constexpr std::string_view kTimeoutMs = "timeoutms";
inline constexpr std::string_view kTarget = "target";
inline constexpr std::string_view kAction = "action";
inline constexpr std::string_view kCommitDate = "commit_date";
inline constexpr std::string_view kNumSpecialFiles = "num_special_files";
inline constexpr std::string_view kFreshInstance = "fresh_instance";
inline constexpr std::string_view kDeduped = "deduped";
inline constexpr std::string_view kResults = "results";
inline constexpr std::string_view kEdenGlobFilesDurationUs =
    "eden_glob_files_duration_us";
inline constexpr std::string_view kEdenChangedFilesDurationUs =
    "eden_changed_files_duration_us";
inline constexpr std::string_view kEdenFilePropertiesDurationUs =
    "eden_file_properties_duration_us";
inline constexpr std::string_view kScmFilesChangedSinceMergebaseWithDurationUs =
    "scm_files_changed_since_mergebase_with_duration_us";
inline constexpr std::string_view kGenerationDurationMs =
    "generation_duration_ms";
inline constexpr std::string_view kSavedStateMissing = "saved_state_missing";
// DynamicEvent key is "isKernel"; the wire column is "is_kernel".
inline constexpr std::string_view kIsKernel = "isKernel";

// --- Double fields ---
inline constexpr std::string_view kDuration = "duration";

} // namespace watchman::xplat_keys
