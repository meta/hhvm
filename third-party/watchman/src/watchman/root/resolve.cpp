/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <folly/String.h>
#include <system_error>
#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/fs/FSDetect.h"
#include "watchman/fs/FileSystem.h"
#include "watchman/root/Root.h"
#include "watchman/root/watchlist.h"
#include "watchman/state.h"
#include "watchman/watcher/WatcherRegistry.h"

using namespace watchman;

namespace {

/* Returns true if the global config root_restrict_files is not defined or if
 * one of the files in root_restrict_files exists, false otherwise. */
bool root_check_restrict(const char* watch_path) {
  bool enforcing;
  auto root_restrict_files = cfg_compute_root_files(&enforcing);
  if (!root_restrict_files) {
    return true;
  }
  if (!enforcing) {
    return true;
  }

  if (!root_restrict_files->isArray()) {
    return false;
  }
  auto& arr = root_restrict_files->array();

  for (size_t i = 0; i < arr.size(); i++) {
    auto& obj = arr[i];
    const char* restrict_file = json_string_value(obj);

    if (!restrict_file) {
      logf(
          ERR,
          "resolve_root: global config root_restrict_files "
          "element {} should be a string\n",
          i);
      continue;
    }

    auto restrict_path = fmt::format("{}/{}", watch_path, restrict_file);
    bool rv = w_path_exists(restrict_path.c_str());
    if (rv) {
      return true;
    }
  }

  return false;
}

static void check_allowed_fs(const char* filename, const w_string& fs_type) {
  const char* advice = nullptr;

  // Report this to the log always, as it is helpful in understanding
  // problem reports
  logf(ERR, "path {} is on filesystem type {}\n", filename, fs_type);

  auto illegal_fstypes = cfg_get_json("illegal_fstypes");
  if (!illegal_fstypes) {
    return;
  }

  auto advice_string = cfg_get_json("illegal_fstypes_advice");
  if (advice_string) {
    advice = json_string_value(*advice_string);
  }
  if (!advice) {
    advice = "relocate the dir to an allowed filesystem type";
  }

  if (!illegal_fstypes->isArray()) {
    logf(ERR, "resolve_root: global config illegal_fstypes is not an array\n");
    return;
  }

  auto& arr = illegal_fstypes->array();

  for (size_t i = 0; i < arr.size(); i++) {
    auto& obj = arr[i];
    const char* name = json_string_value(obj);

    if (!name) {
      logf(
          ERR,
          "resolve_root: global config illegal_fstypes "
          "element {} should be a string\n",
          i);
      continue;
    }

    if (fs_type == name) {
      RootResolveError::throwf(
          "path uses the \"{}\" filesystem "
          "and is disallowed by global config illegal_fstypes: {}",
          fs_type,
          advice);
    }
  }
}

std::optional<json_ref> load_root_config(const char* path) {
  char cfgfilename[WATCHMAN_NAME_MAX];
  snprintf(cfgfilename, sizeof(cfgfilename), "%s/.watchmanconfig", path);

  if (!w_path_exists(cfgfilename)) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    logf(
        ERR, "{} is not accessible: {}\n", cfgfilename, folly::errnoStr(errno));
    return std::nullopt;
  }

  return json_load_file(cfgfilename, 0);
}

} // namespace

std::shared_ptr<Root>
root_resolve(const char* filename_cstr, bool auto_watch, bool* created) {
  std::error_code realpath_err;
  std::shared_ptr<Root> root;

  *created = false;

  w_string_piece filename{filename_cstr};

  // Sanity check that the path is absolute
  if (!w_string_path_is_absolute(filename)) {
    log(ERR, "resolve_root: path \"", filename, "\" must be absolute\n");
    RootResolveError::throwf("path \"{}\" must be absolute", filename);
  }

  // TODO: This does not prevent watching paths like C:\ on Windows.
  if (filename == "/") {
    log(ERR, "resolve_root: cannot watchman \"/\"\n");
    throw RootResolveError("cannot watch \"/\"");
  }

  w_string root_str;

  try {
    root_str = realPath(filename_cstr);
    try {
      getFileInformation(filename_cstr);
    } catch (const std::system_error& exc) {
      if (exc.code() == error_code::no_such_file_or_directory) {
        RootResolveError::throwf(
            "\"{}\" resolved to \"{}\" but we were "
            "unable to examine \"{}\" using strict "
            "case sensitive rules.  Please check "
            "each component of the path and make "
            "sure that that path exactly matches "
            "the correct case of the files on your "
            "filesystem.",
            filename,
            root_str,
            filename);
      }
      RootResolveError::throwf(
          "unable to lstat \"{}\" {}", filename, exc.what());
    }
  } catch (const std::system_error& exc) {
    realpath_err = exc.code();
    root_str = w_string(filename_cstr, W_STRING_BYTE);
  }

  {
    auto map = watched_roots.rlock();
    const auto& it = map->find(root_str);
    if (it != map->end()) {
      root = it->second;
    }
  }

  if (!root && realpath_err.value() != 0) {
    // Path didn't resolve and neither did the name they passed in
    RootResolveError::throwf(
        "realpath({}) -> {}", filename, realpath_err.message());
  }

  if (root || !auto_watch) {
    if (!root) {
      RootResolveError::throwf("directory {} is not watched", root_str);
    }
    return root;
  }

  logf(DBG, "Want to watch {} -> {}\n", filename, root_str);

  auto fs_type = w_fstype(filename_cstr);
  check_allowed_fs(root_str.c_str(), fs_type);

  if (!root_check_restrict(root_str.c_str())) {
    bool enforcing;
    auto root_files = cfg_compute_root_files(&enforcing);
    auto root_files_list = cfg_pretty_print_root_files(root_files.value());
    RootResolveError::throwf(
        "Your watchman administrator has configured watchman "
        "to prevent watching path `{}`.  None of the files "
        "listed in global config root_files are "
        "present and enforce_root_files is set to true.  "
        "root_files is defined by the `{}` config file and "
        "includes {}.  One or more of these files must be "
        "present in order to allow a watch.  Try pulling "
        "and checking out a newer version of the project?",
        root_str,
        cfg_get_global_config_file_path(),
        root_files_list);
  }

  try {
    auto config_file = load_root_config(root_str.c_str());
    Configuration config{config_file};
    root = std::make_shared<Root>(
        realFileSystem,
        root_str,
        fs_type,
        config_file,
        config,
        WatcherRegistry::initWatcher(root_str, fs_type, config),
        &w_state_save);

    {
      auto wlock = watched_roots.wlock();
      auto& map = *wlock;
      auto& existing = map[root->root_path];
      if (existing) {
        // Someone beat us in this race
        root = existing;
        *created = false;
      } else {
        existing = root;
        *created = true;
      }
    }

    return root;
  } catch (const std::system_error& exc) {
    if (exc.code() == std::errc::not_connected) {
      RootNotConnectedError::throwf(
          "\"{}\" was able to be opened, but we were unable to read its "
          "contents. If \"{}\" is located on a FUSE or network mount, "
          "please ensure that you have mounted it correctly, including "
          "validating any required credentials or certificates.\n",
          filename,
          filename);
    }
    throw;
  }
}

std::shared_ptr<Root> w_root_resolve(const char* filename, bool auto_watch) {
  bool created = false;
  auto root = root_resolve(filename, auto_watch, &created);

  if (created) {
    try {
      root->view()->startThreads(root);
    } catch (const std::exception& e) {
      log(ERR, "w_root_resolve, while calling startThreads: ", e.what());
      root->cancel(
          fmt::format("Error starting threads for root: {}", e.what()));
      throw;
    }
    w_state_save();
  }
  return root;
}

std::shared_ptr<Root> w_root_resolve_for_client_mode(const char* filename) {
  bool created = false;
  auto root = root_resolve(filename, true, &created);

  if (created) {
    auto view = std::dynamic_pointer_cast<InMemoryView>(root->view());
    if (!view) {
      throw RootResolveError("client mode not available");
    }

    /* force a walk now */
    view->clientModeCrawl(root);
  }
  return root;
}

/* vim:ts=2:sw=2:et:
 */
