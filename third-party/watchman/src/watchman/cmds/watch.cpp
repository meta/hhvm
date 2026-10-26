/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"
#include "watchman/ClientContext.h"
#include "watchman/Command.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/LogConfig.h"
#include "watchman/QueryableView.h"
#include "watchman/root/Root.h"
#include "watchman/root/watchlist.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

void w_cmd_realpath_root(Command& command) {
  std::vector<json_ref> args = command.args().array();

  if (args.empty()) {
    throw CommandValidationError("wrong number of arguments");
  }

  const char* path = json_string_value(args[0]);
  if (!path) {
    throw CommandValidationError(
        "second argument must be a string expressing the path to the watch");
  }

  try {
    auto resolved = realPath(path);
    args[0] = w_string_to_json(resolved);
  } catch (const std::exception& exc) {
    CommandValidationError::throwf(
        "Could not resolve {} to the canonical watch path: {}",
        path,
        exc.what());
  }

  command.args() = json_array(std::move(args));
}
W_CAP_REG("clock-sync-timeout")

/* Add the current clock value to the response */
static void annotate_with_clock(
    const std::shared_ptr<Root>& root,
    UntypedResponse& resp) {
  resp.set("clock", w_string_to_json(root->view()->getCurrentClockString()));
}

/* clock /root [options]
 * Returns the current clock value for a watched root
 * If the options contain a sync_timeout, we ensure that the repo
 * is synced up-to-date and the returned clock represents the
 * latest state.
 */
static UntypedResponse cmd_clock(Client* client, const json_ref& args) {
  int sync_timeout = 0;

  // TODO: merge this parse and sync logic with the logic in query evaluation
  if (json_array_size(args) == 3) {
    auto& opts = args.at(2);
    if (!opts.isObject()) {
      throw ErrorResponse(
          "the third argument to 'clock' must be an optional object");
    }

    auto sync = opts.get_optional("sync_timeout");
    if (sync) {
      if (!sync->isInt()) {
        throw ErrorResponse(
            "the sync_timeout option passed to 'clock' must be an integer");
      }
      sync_timeout = sync->asInt();
    }
  } else if (json_array_size(args) != 2) {
    throw ErrorResponse("wrong number of arguments to 'clock'");
  }

  /* resolve the root */
  auto root = resolveRoot(client, args);

  if (sync_timeout) {
    root->syncToNow(
        std::chrono::milliseconds(sync_timeout), client->getClientInfo());
  }

  UntypedResponse resp;
  annotate_with_clock(root, resp);

  return resp;
}
W_CMD_REG(
    "clock",
    cmd_clock,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

/* watch-del /root
 * Stops watching the specified root */
static UntypedResponse cmd_watch_delete(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    throw ErrorResponse("wrong number of arguments to 'watch-del'");
  }

  auto root = resolveRoot(client, args);

  UntypedResponse resp;
  resp.set(
      {{"watch-del", json_boolean(root->stopWatch("watch-del"))},
       {"root", w_string_to_json(root->root_path)}});
  return resp;
}
W_CMD_REG("watch-del", cmd_watch_delete, CMD_DAEMON, w_cmd_realpath_root);

/* watch-del-all
 * Stops watching all roots */
static UntypedResponse cmd_watch_del_all(Client*, const json_ref&) {
  UntypedResponse resp;
  auto roots = w_root_stop_watch_all();
  resp.set("roots", std::move(roots));
  return resp;
}
W_CMD_REG(
    "watch-del-all",
    cmd_watch_del_all,
    CMD_DAEMON | CMD_POISON_IMMUNE,
    nullptr);

/* watch-list
 * Returns a list of watched roots */
static UntypedResponse cmd_watch_list(Client*, const json_ref&) {
  UntypedResponse resp;
  auto root_paths = w_root_watch_list_to_json();
  resp.set("roots", std::move(root_paths));
  return resp;
}
W_CMD_REG(
    "watch-list",
    cmd_watch_list,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    nullptr);

// For each directory component in candidate_dir to the root of the filesystem,
// look for root_file.  If root_file is present, update relpath to reflect the
// relative path to the original value of candidate_dir and return true.  If
// not found, return false. candidate_dir is modified by this function if
// return true.
static bool find_file_in_dir_tree(
    const w_string& root_file,
    w_string_piece& candidate_dir,
    w_string_piece& relpath) {
  w_string_piece current_dir(candidate_dir);
  while (true) {
    auto projPath = w_string::pathCat({current_dir, root_file});

    if (w_path_exists(projPath.c_str())) {
      // Got a match
      relpath = w_string_piece(candidate_dir);
      if (candidate_dir.size() == current_dir.size()) {
        relpath = w_string_piece();
      } else {
        relpath.advance(current_dir.size() + 1);
        candidate_dir = current_dir;
      }
      return true;
    }

    auto parent = current_dir.dirName();
    if (parent.empty() || parent == current_dir) {
      return false;
    }
    current_dir = parent;
  }
}

bool find_project_root(
    const json_ref& root_files,
    w_string_piece& resolved,
    w_string_piece& relpath) {
  if (!root_files.isArray()) {
    return false;
  }
  for (auto& item : root_files.array()) {
    auto name = json_to_w_string(item);

    if (find_file_in_dir_tree(name, resolved, relpath)) {
      return true;
    }
  }
  return false;
}

void check_no_watchman(w_string resolved_root) {
  // check if .nowatchman exists under the resolved path
  auto no_watchman_path = w_string::pathCat({resolved_root, ".nowatchman"});
  if (w_path_exists(no_watchman_path.c_str())) {
    CommandValidationError::throwf(
        "resolve_projpath: the repository is configured to not enable watchman. "
        "Found .nowatchman at {}",
        no_watchman_path);
  }
}

// For watch-project, take a root path string and resolve the
// containing project directory, then update the args to reflect
// that path.
// relpath will hold the path to the project dir, relative to the
// watched dir.  If it is NULL it means that the project dir is
// equivalent to the watched dir.
static w_string resolve_projpath(
    std::vector<json_ref>& args,
    w_string& relpath) {
  const char* path;
  bool enforcing;
  if (args.size() < 2) {
    throw CommandValidationError("wrong number of arguments");
  }

  path = json_string_value(args[1]);
  if (!path) {
    throw CommandValidationError("second argument must be a string");
  }

  auto resolved = realPath(path);

  auto root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    CommandValidationError::throwf(
        "resolve_projpath: error computing root_files configuration value, "
        "consult your log file at {} for more details",
        logging::log_name);
  }

  // See if we're requesting something in a pre-existing watch

  w_string_piece prefix;
  w_string_piece relpiece;
  if (findEnclosingRoot(resolved, prefix, relpiece)) {
    relpath = relpiece.asWString();
    resolved = prefix.asWString();
    args[1] = w_string_to_json(resolved);
    return resolved;
  }
  auto resolvedpiece = resolved.piece();
  if (find_project_root(*root_files, resolvedpiece, relpiece)) {
    relpath = relpiece.asWString();
    resolved = resolvedpiece.asWString();
    check_no_watchman(resolved);
    args[1] = w_string_to_json(resolved);
    return resolved;
  }

  if (!enforcing) {
    // We'll use the path they originally requested
    return resolved;
  }

  // Convert root files to comma delimited string for error message
  auto root_files_list = cfg_pretty_print_root_files(*root_files);

  CommandValidationError::throwf(
      "resolve_projpath:  None of the files listed in global config "
      "root_files are present in path `{}` or any of its "
      "parent directories.  root_files is defined by the "
      "`{}` config file and includes {}.  "
      "One or more of these files must be present in order to allow "
      "a watch. Try pulling and checking out a newer version of the project?",
      path,
      cfg_get_global_config_file_path(),
      root_files_list);
}

/* watch /root */
static UntypedResponse cmd_watch(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    throw ErrorResponse("wrong number of arguments to 'watch'");
  }

  auto root = resolveOrCreateRoot(client, args);
  root->view()->waitUntilReadyToQuery().get();

  UntypedResponse resp;

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(*root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set(
        {{"watch", w_string_to_json(root->root_path)},
         {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  return resp;
}
W_CMD_REG(
    "watch",
    cmd_watch,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

static UntypedResponse cmd_watch_project(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    throw ErrorResponse("wrong number of arguments to 'watch-project'");
  }

  w_string rel_path_from_watch;
  std::vector<json_ref> args_array = args.array();
  auto dir_to_watch = resolve_projpath(args_array, rel_path_from_watch);
  auto root = resolveOrCreateRoot(client, json_array(std::move(args_array)));

  root->view()->waitUntilReadyToQuery().get();

  UntypedResponse resp;

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(*root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set(
        {{"watch", w_string_to_json(root->root_path)},
         {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  if (!rel_path_from_watch.empty()) {
    resp.set("relative_path", w_string_to_json(rel_path_from_watch));
  }
  return resp;
}
W_CMD_REG(
    "watch-project",
    cmd_watch_project,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

/* vim:ts=2:sw=2:et:
 */
