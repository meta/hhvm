/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include "watchman/ClientContext.h"
#include "watchman/Clock.h"
#include "watchman/fs/FileSystem.h"
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"

namespace watchman {

class FileResult;
struct GlobTree;
struct QueryContext;
class QueryExpr;

struct QueryFieldRenderer {
  w_string name;
  std::optional<json_ref> (*make)(FileResult* file, const QueryContext* ctx);
};

class QueryFieldList : public std::vector<QueryFieldRenderer*> {
 public:
  /**
   * Adds the specified field to the list of those requested by the query.
   *
   * Throws QueryParseError if the name is invalid.
   */
  void add(const w_string& name);
};

struct QueryPath {
  w_string name;
  int depth;
};

struct Query {
  CaseSensitivity case_sensitive = CaseSensitivity::CaseInSensitive;
  bool fail_if_no_saved_state = false;
  bool empty_on_fresh_instance = false;
  /**
   * If set, the EdenFS watcher counts only changes this query can return
   * (inside relative_root and not ignored) toward
   * `eden_file_count_threshold_for_fresh_instance`. Changes elsewhere in the
   * mount then no longer turn a `since` query into a fresh instance.
   */
  bool scope_fresh_instance_threshold = false;
  bool omit_changed_files = false;
  bool dedup_results = false;
  uint32_t bench_iterations = 0;

  /**
   * Optional full path to relative root, without and with trailing slash.
   */
  std::optional<w_string> relative_root;
  w_string relative_root_slash;

  std::optional<std::vector<QueryPath>> paths;

  std::unique_ptr<GlobTree> glob_tree;
  // Additional flags to pass to wildmatch in the glob_generator
  int glob_flags = 0;

  struct SettleTimeouts {
    std::chrono::milliseconds settle_period;
    std::chrono::milliseconds settle_timeout;
  };

  /**
   * If set, the query will wait for a period of no new watcher events, up until
   * the specified timeout.
   */
  std::optional<SettleTimeouts> settle_timeouts;

  /**
   * How long this query should attempt to wait for a cookie file to be
   * observed.
   *
   * If zero, no syncing is performed.
   */
  std::chrono::milliseconds sync_timeout;

  uint32_t lock_timeout = 0;

  // We can't (and mustn't!) evaluate the clockspec
  // fully until we execute query, because we have
  // to evaluate named cursors and determine fresh
  // instance at the time we execute
  std::unique_ptr<ClockSpec> since_spec;

  std::unique_ptr<QueryExpr> expr;

  // The query that we parsed into this struct
  std::optional<json_ref> query_spec;

  QueryFieldList fieldList;

  std::optional<w_string> request_id;
  std::optional<w_string> subscriptionName;
  ClientContext clientInfo{0, std::nullopt};

  bool alwaysIncludeDirectories{false};

  ~Query();

  /** Returns true if the supplied name is contained in
   * the parsed fieldList in this query */
  bool isFieldRequested(w_string_piece name) const;
};

} // namespace watchman
