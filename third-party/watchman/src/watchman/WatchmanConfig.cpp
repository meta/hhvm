/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/WatchmanConfig.h"

#include <folly/ExceptionString.h>
#include <folly/Synchronized.h>
#include <optional>

#include "watchman/Errors.h"
#include "watchman/Logging.h"

using namespace watchman;

namespace {

struct ConfigState {
  std::optional<json_ref> global_cfg;
  w_string global_config_file_path;
};
folly::Synchronized<ConfigState> configState;

std::optional<std::pair<json_ref, w_string>> loadSystemConfig() {
  const char* cfg_file = getenv("WATCHMAN_CONFIG_FILE");
#ifdef WATCHMAN_CONFIG_FILE
  if (!cfg_file) {
    cfg_file = WATCHMAN_CONFIG_FILE;
  }
#endif
  if (!cfg_file || cfg_file[0] == '\0') {
    return std::nullopt;
  }

  std::string cfg_file_default = std::string{cfg_file} + ".default";
  const char* current_cfg_file;

  json_ref config = json_null();
  try {
    // Try to load system watchman configuration
    try {
      current_cfg_file = cfg_file;
      config = json_load_file(current_cfg_file, 0);
    } catch (const std::system_error& exc) {
      if (exc.code() == watchman::error_code::no_such_file_or_directory) {
        // Fallback to trying to load default watchman configuration if there
        // is no system configuration
        try {
          current_cfg_file = cfg_file_default.c_str();
          config = json_load_file(current_cfg_file, 0);
        } catch (const std::system_error& default_exc) {
          // If there is no default configuration either, just return
          if (default_exc.code() ==
              watchman::error_code::no_such_file_or_directory) {
            return std::nullopt;
          } else {
            throw;
          }
        }
      } else {
        throw;
      }
    }
  } catch (const std::system_error& exc) {
    logf(
        ERR,
        "Failed to load config file {}: {}\n",
        current_cfg_file,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  } catch (const std::exception& exc) {
    logf(
        ERR,
        "Failed to parse config file {}: {}\n",
        current_cfg_file,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  }

  if (!config.isObject()) {
    logf(ERR, "config {} must be a JSON object\n", current_cfg_file);
    return std::nullopt;
  }

  return {{config, current_cfg_file}};
}

std::optional<json_ref> loadUserConfig() {
  // TODO(xavierd): We should follow XDG and Windows AppData folder instead.
  // Helper functions for this are available in UserInfo.h
  const char* home = getenv(folly::kIsWindows ? "USERPROFILE" : "HOME");
  if (!home) {
    return std::nullopt;
  }
  auto path = std::string{home} + "/.watchman.json";
  try {
    json_ref config = json_load_file(path.c_str(), 0);
    if (!config.isObject()) {
      logf(ERR, "config {} must be a JSON object\n", path);
      return std::nullopt;
    }
    return config;
  } catch (const std::system_error& exc) {
    if (exc.code() == watchman::error_code::no_such_file_or_directory) {
      return std::nullopt;
    }
    logf(
        ERR,
        "Failed to load config file {}: {}\n",
        path,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  } catch (const std::exception& exc) {
    logf(
        ERR,
        "Failed to parse config file {}: {}\n",
        path,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  }
}

} // namespace

/* Called during shutdown to free things so that we run cleanly
 * under valgrind */
void cfg_shutdown() {
  auto state = configState.wlock();
  state->global_cfg.reset();
}

w_string cfg_get_global_config_file_path() {
  return configState.rlock()->global_config_file_path;
}

void cfg_load_global_config_file() {
  auto systemConfig = loadSystemConfig();
  auto userConfig = loadUserConfig();

  auto lockedState = configState.wlock();
  if (systemConfig) {
    lockedState->global_cfg = systemConfig->first;
    lockedState->global_config_file_path = systemConfig->second;
  }

  if (userConfig) {
    if (!lockedState->global_cfg) {
      lockedState->global_cfg = json_object();
    }
    for (auto& [key, value] : userConfig->object()) {
      json_object_set(*lockedState->global_cfg, key.c_str(), value);
    }
  }
}

void cfg_set_global(const char* name, const json_ref& val) {
  auto state = configState.wlock();
  if (!state->global_cfg) {
    state->global_cfg = json_object();
  }

  state->global_cfg->set(name, json_ref(val));
}

std::optional<json_ref> cfg_get_json(const char* name) {
  auto state = configState.rlock();
  if (state->global_cfg) {
    return state->global_cfg->get_optional(name);
  } else {
    return std::nullopt;
  }
}

const char* cfg_get_string(const char* name, const char* defval) {
  auto val = cfg_get_json(name);
  if (!val) {
    return defval;
  }

  if (!val->isString()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a string", name));
  }
  return json_string_value(*val);
}

// Return true if the json ref is an array of string values
static bool is_array_of_strings(const json_ref& ref) {
  if (!ref.isArray()) {
    return false;
  }
  for (auto& elt : ref.array()) {
    if (!elt.isString()) {
      return false;
    }
  }
  return true;
}

// Given an array of string values, if that array does not contain
// a ".watchmanconfig" entry, prepend it
static void prepend_watchmanconfig_to_array(std::vector<json_ref>& ref) {
  const char* val;

  if (ref.empty()) {
    ref.push_back(typed_string_to_json(".watchmanconfig", W_STRING_UNICODE));
    return;
  }

  val = json_string_value(ref[0]);
  if (!strcmp(val, ".watchmanconfig")) {
    return;
  }
  ref.insert(
      ref.begin(), typed_string_to_json(".watchmanconfig", W_STRING_UNICODE));
}

// Compute the effective value of the root_files configuration and
// return a json reference.  The caller must decref the ref when done
// (we may synthesize this value).   Sets enforcing to indicate whether
// we will only allow watches on the root_files.
// The array returned by this function (if not NULL) is guaranteed to
// list .watchmanconfig as its zeroth element.
std::optional<json_ref> cfg_compute_root_files(bool* enforcing) {
  *enforcing = false;

  auto ref = cfg_get_json("enforce_root_files");
  if (ref) {
    if (!ref->isBool()) {
      logf(FATAL, "Expected config value enforce_root_files to be boolean\n");
    }
    *enforcing = ref->asBool();
  }

  ref = cfg_get_json("root_files");
  if (ref) {
    if (!is_array_of_strings(*ref)) {
      logf(FATAL, "global config root_files must be an array of strings\n");
      *enforcing = false;
      return std::nullopt;
    }
    std::vector<json_ref> arr = ref->array();
    prepend_watchmanconfig_to_array(arr);
    return json_array(std::move(arr));
  }

  // Try legacy root_restrict_files configuration
  ref = cfg_get_json("root_restrict_files");
  if (ref) {
    if (!is_array_of_strings(*ref)) {
      logf(
          FATAL,
          "deprecated global config root_restrict_files "
          "must be an array of strings\n");
      *enforcing = false;
      return std::nullopt;
    }
    std::vector<json_ref> arr = ref->array();
    prepend_watchmanconfig_to_array(arr);
    *enforcing = true;
    return json_array(std::move(arr));
  }

  // Synthesize our conservative default value.
  // .watchmanconfig MUST be first
  return json_array(
      {typed_string_to_json(".watchmanconfig"),
       typed_string_to_json(".hg"),
       typed_string_to_json(".git"),
       typed_string_to_json(".svn")});
}

// Produces a string like:  "`foo`, `bar`, and `baz`"
std::string cfg_pretty_print_root_files(const json_ref& root_files) {
  std::string result;
  for (unsigned int i = 0; i < root_files.array().size(); ++i) {
    const auto& r = root_files.array()[i];
    if (i > 1 && i == root_files.array().size() - 1) {
      // We are last in a list of multiple items
      result.append(", and ");
    } else if (i > 0) {
      result.append(", ");
    }
    result.append("`");
    result.append(json_string_value(r));
    result.append("`");
  }
  return result;
}

json_int_t cfg_get_int(const char* name, json_int_t defval) {
  auto val = cfg_get_json(name);
  if (!val) {
    return defval;
  }

  if (!val->isInt()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be an integer", name));
  }
  return val->asInt();
}

bool cfg_get_bool(const char* name, bool defval) {
  auto val = cfg_get_json(name);
  if (!val) {
    return defval;
  }

  if (!val->isBool()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a boolean", name));
  }
  return val->asBool();
}

double cfg_get_double(const char* name, double defval) {
  auto val = cfg_get_json(name);
  if (!val) {
    return defval;
  }

  if (!val->isNumber()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a number", name));
  }
  return json_real_value(*val);
}

#ifndef _WIN32
#define MAKE_GET_PERM(PROP, SUFFIX)                                 \
  static mode_t get_##PROP##_perm(                                  \
      const char* name,                                             \
      const json_ref& val,                                          \
      bool write_bits,                                              \
      bool execute_bits) {                                          \
    mode_t ret = 0;                                                 \
    auto perm = val.get_optional(#PROP);                            \
    if (perm) {                                                     \
      if (!perm->isBool()) {                                        \
        logf(                                                       \
            FATAL,                                                  \
            "Expected config value {}." #PROP " to be a boolean\n", \
            name);                                                  \
      }                                                             \
      if (perm->asBool()) {                                         \
        ret |= S_IR##SUFFIX;                                        \
        if (write_bits) {                                           \
          ret |= S_IW##SUFFIX;                                      \
        }                                                           \
        if (execute_bits) {                                         \
          ret |= S_IX##SUFFIX;                                      \
        }                                                           \
      }                                                             \
    }                                                               \
    return ret;                                                     \
  }

MAKE_GET_PERM(group, GRP)
MAKE_GET_PERM(others, OTH)

/**
 * This function expects the config to be an object containing the keys 'group'
 * and 'others', each a bool.
 */
mode_t cfg_get_perms(const char* name, bool write_bits, bool execute_bits) {
  auto val = cfg_get_json(name);
  mode_t ret = S_IRUSR | S_IWUSR;
  if (execute_bits) {
    ret |= S_IXUSR;
  }

  if (val) {
    if (!val->isObject()) {
      logf(FATAL, "Expected config value {} to be an object\n", name);
    }

    ret |= get_group_perm(name, *val, write_bits, execute_bits);
    ret |= get_others_perm(name, *val, write_bits, execute_bits);
  }

  return ret;
}
#endif

const char* cfg_get_trouble_url() {
  return cfg_get_string(
      "troubleshooting_url",
      "https://meta.github.io/watchman/docs/troubleshooting.html");
}

namespace watchman {

Configuration::Configuration() {}

Configuration::Configuration(std::optional<json_ref> local)
    : local_{std::move(local)} {}

std::optional<json_ref> Configuration::get(const char* name) const {
  // Highest precedence: options set locally
  if (local_) {
    std::optional<json_ref> val = local_->get_optional(name);
    if (val) {
      return std::move(*val);
    }
  }
  auto state = configState.rlock();

  // then: global config options
  if (!state->global_cfg) {
    return std::nullopt;
  }
  return state->global_cfg->get_optional(name);
}

const char* Configuration::getString(const char* name, const char* defval)
    const {
  auto val = get(name);
  if (!val) {
    return defval;
  }

  if (!val->isString()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a string", name));
  }
  return json_string_value(*val);
}

json_int_t Configuration::getInt(const char* name, json_int_t defval) const {
  auto val = get(name);
  if (!val) {
    return defval;
  }

  if (!val->isInt()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be an integer", name));
  }
  return val->asInt();
}

bool Configuration::getBool(const char* name, bool defval) const {
  auto val = get(name);
  if (!val) {
    return defval;
  }

  if (!val->isBool()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a boolean", name));
  }
  return val->asBool();
}

double Configuration::getDouble(const char* name, double defval) const {
  auto val = get(name);
  if (!val) {
    return defval;
  }

  if (!val->isNumber()) {
    throw std::runtime_error(
        fmt::format("Expected config value {} to be a number", name));
  }
  return json_real_value(*val);
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
