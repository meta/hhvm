/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>

#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryExpr.h"
#include "watchman/query/TermRegistry.h"
#include "watchman/query/parse.h"
#include "watchman/root/Root.h"

namespace watchman {

namespace {

bool parse_since(Query* res, const json_ref& query) {
  auto since = query.get_optional("since");
  if (!since) {
    return true;
  }

  auto spec = ClockSpec::parseOptionalClockSpec(*since);
  if (spec) {
    // res owns the ref to spec
    res->since_spec = std::move(spec);
    return true;
  }

  throw QueryParseError("invalid value for 'since'");
}

bool parse_paths(Query* res, const json_ref& query) {
  size_t i;

  auto paths = query.get_optional("path");
  if (!paths) {
    return true;
  }

  if (!paths->isArray()) {
    throw QueryParseError("'path' must be an array");
  }

  auto size = json_array_size(*paths);

  res->paths.emplace();
  std::vector<QueryPath>& res_paths = *res->paths;
  res_paths.resize(size);

  for (i = 0; i < size; i++) {
    const auto& ele = paths->at(i);
    w_string name;

    res_paths[i].depth = -1;

    if (ele.isString()) {
      name = json_to_w_string(ele);
    } else if (ele.isObject()) {
      name = json_to_w_string(ele.get("path"));

      auto depth = ele.get("depth");
      if (!depth.isInt()) {
        throw QueryParseError("path.depth must be an integer");
      }

      res_paths[i].depth = depth.asInt();
    } else {
      throw QueryParseError(
          "expected object with 'path' and 'depth' properties");
    }

    res_paths[i].name = name.normalizeSeparators();
  }

  return true;
}

W_CAP_REG("relative_root")

void parse_relative_root(
    const std::shared_ptr<Root>& root,
    Query* res,
    const json_ref& query) {
  auto relative_root = query.get_optional("relative_root");
  if (!relative_root) {
    return;
  }

  if (!relative_root->isString()) {
    throw QueryParseError("'relative_root' must be a string");
  }

  auto path = json_to_w_string(*relative_root).normalizeSeparators();
  if (path.empty()) {
    // An empty relative_root is equivalent to not specifying
    // a relative root.  Importantly, we want to avoid setting
    // relative_root to "" because that introduces some complexities
    // in handling that case for eg: eden.
    return;
  }

  auto canon_path = w_string_canon_path(path);
  res->relative_root = w_string::pathCat({root->root_path, canon_path});
  res->relative_root_slash = w_string::build(res->relative_root.value(), "/");
}

void parse_query_expression(Query* res, const json_ref& query) {
  auto exp = query.get_optional("expression");
  if (!exp) {
    // Empty expression means that we emit all generated files
    return;
  }

  res->expr = parseQueryExpr(res, *exp);
}

void parse_request_id(Query* res, const json_ref& query) {
  auto request_id = query.get_optional("request_id");
  if (!request_id) {
    return;
  }

  if (!request_id->isString()) {
    throw QueryParseError("'request_id' must be a string");
  }

  res->request_id = json_to_w_string(*request_id);
}

namespace {
json_int_t parse_nonnegative_integer(std::string_view name, json_ref v) {
  if (!v.isInt()) {
    throw QueryParseError(std::string{name} + " must be an integer value >= 0");
  }
  json_int_t value = v.asInt();
  if (value < 0) {
    throw QueryParseError(std::string{name} + " must be an integer value >= 0");
  }
  return value;
}
} // namespace

void parse_sync(Query* res, const json_ref& query) {
  auto settle_period = query.get_optional("settle_period");
  auto settle_timeout = query.get_optional("settle_timeout");
  if (settle_period && settle_timeout) {
    auto settle_period_value =
        parse_nonnegative_integer("settle_period", *settle_period);
    auto settle_timeout_value =
        parse_nonnegative_integer("settle_timeout", *settle_timeout);
    Query::SettleTimeouts settle_timeouts;
    settle_timeouts.settle_period =
        std::chrono::milliseconds{settle_period_value};
    settle_timeouts.settle_timeout =
        std::chrono::milliseconds{settle_timeout_value};
    res->settle_timeouts = settle_timeouts;
  } else if (settle_period) {
    throw QueryParseError("settle_period specified without settle_timeout");
  } else if (settle_timeout) {
    throw QueryParseError("settle_timeout specified without settle_period");
  }

  auto sync_timeout = query.get_default(
      "sync_timeout",
      json_integer(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kDefaultQuerySyncTimeout)
              .count()));
  res->sync_timeout = std::chrono::milliseconds{
      parse_nonnegative_integer("sync_timeout", sync_timeout)};
}

void parse_lock_timeout(Query* res, const json_ref& query) {
  auto lock_timeout = query.get_default(
      "lock_timeout",
      json_integer(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kDefaultQuerySyncTimeout)
              .count()));

  if (!lock_timeout.isInt()) {
    throw QueryParseError("lock_timeout must be an integer value >= 0");
  }

  auto value = lock_timeout.asInt();

  if (value < 0) {
    throw QueryParseError("lock_timeout must be an integer value >= 0");
  }

  res->lock_timeout = value;
}

bool parse_bool_param(
    const json_ref& query,
    const char* name,
    bool default_value) {
  auto value = query.get_default(name, json_boolean(default_value));
  if (!value.isBool()) {
    throw QueryParseError(fmt::format("{} must be a boolean", name));
  }

  return value.asBool();
}

W_CAP_REG("dedup_results")

void parse_dedup(Query* res, const json_ref& query) {
  res->dedup_results = parse_bool_param(query, "dedup_results", false);
}

void parse_fail_if_no_saved_state(Query* res, const json_ref& query) {
  res->fail_if_no_saved_state =
      parse_bool_param(query, "fail_if_no_saved_state", false);
}

void parse_omit_changed_files(Query* res, const json_ref& query) {
  res->omit_changed_files =
      parse_bool_param(query, "omit_changed_files", false);
}

void parse_empty_on_fresh_instance(Query* res, const json_ref& query) {
  res->empty_on_fresh_instance =
      parse_bool_param(query, "empty_on_fresh_instance", false);
}

W_CAP_REG("scope_fresh_instance_threshold")

void parse_scope_fresh_instance_threshold(Query* res, const json_ref& query) {
  res->scope_fresh_instance_threshold =
      parse_bool_param(query, "scope_fresh_instance_threshold", false);
}

void parse_always_include_directories(Query* res, const json_ref& query) {
  res->alwaysIncludeDirectories =
      parse_bool_param(query, "always_include_directories", false);
}

void parse_benchmark(Query* res, const json_ref& query) {
  // Preserve behavior by supporting a boolean value. Also support int values.
  auto bench = query.get_optional("bench");
  if (bench) {
    if (bench->isBool()) {
      res->bench_iterations = 100;
    } else {
      res->bench_iterations = bench->asInt();
    }
  }
}

void parse_case_sensitive(
    Query* res,
    const std::shared_ptr<Root>& root,
    const json_ref& query) {
  auto case_sensitive = parse_bool_param(
      query,
      "case_sensitive",
      root->case_sensitive == CaseSensitivity::CaseSensitive);

  res->case_sensitive = case_sensitive ? CaseSensitivity::CaseSensitive
                                       : CaseSensitivity::CaseInSensitive;
}

} // namespace

std::shared_ptr<Query> parseQuery(
    const std::shared_ptr<Root>& root,
    const json_ref& query) {
  auto result = std::make_shared<Query>();
  auto res = result.get();

  parse_benchmark(res, query);
  parse_case_sensitive(res, root, query);
  parse_sync(res, query);
  parse_dedup(res, query);
  parse_lock_timeout(res, query);
  parse_relative_root(root, res, query);
  parse_empty_on_fresh_instance(res, query);
  parse_scope_fresh_instance_threshold(res, query);
  parse_fail_if_no_saved_state(res, query);
  parse_omit_changed_files(res, query);
  parse_always_include_directories(res, query);

  /* Look for path generators */
  parse_paths(res, query);

  /* Look for glob generators */
  parse_globs(res, query);

  /* Look for suffix generators */
  parse_suffixes(res, query);

  /* Look for since generator */
  parse_since(res, query);

  parse_query_expression(res, query);

  parse_request_id(res, query);

  parse_field_list(query.get_optional("fields"), &res->fieldList);

  res->query_spec = query;

  return result;
}

void w_query_legacy_field_list(QueryFieldList* flist) {
  // TODO: Avoid the round-trip through json_ref and insert the requested fields
  // into QueryFieldList directly.

  static const char* names[] = {
      "name",
      "exists",
      "size",
      "mode",
      "uid",
      "gid",
      "mtime",
      "ctime",
      "ino",
      "dev",
      "nlink",
      "new",
      "cclock",
      "oclock"};
  uint8_t i;
  std::vector<json_ref> list;

  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    list.push_back(typed_string_to_json(names[i], W_STRING_UNICODE));
  }

  parse_field_list(json_array(std::move(list)), flist);
}

// Translate from the legacy array into the new style, then
// delegate to the main parser.
// We build a big anyof expression
std::shared_ptr<Query> parseQueryLegacy(
    const std::shared_ptr<Root>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p) {
  bool include = true;
  bool negated = false;
  uint32_t i;
  const char* term_name = "match";
  std::vector<json_ref> included_array;
  std::vector<json_ref> excluded_array;
  auto query_obj = json_object();

  if (!args.isArray()) {
    throw QueryParseError("Expected an array");
  }

  auto& args_array = args.array();

  for (i = start; i < args_array.size(); i++) {
    const char* arg = json_string_value(args_array[i]);
    if (!arg) {
      /* not a string value! */
      throw QueryParseError(
          fmt::format("rule @ position {} is not a string value", i));
    }
  }

  for (i = start; i < json_array_size(args); i++) {
    const char* arg = json_string_value(args_array[i]);
    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }
    if (!strcmp(arg, "-P")) {
      term_name = "ipcre";
      continue;
    }
    if (!strcmp(arg, "-p")) {
      term_name = "pcre";
      continue;
    }

    // Which group are we going to file it into
    std::vector<json_ref>* container;
    if (include) {
      if (included_array.empty()) {
        included_array.push_back(
            typed_string_to_json("anyof", W_STRING_UNICODE));
      }
      container = &included_array;
    } else {
      if (excluded_array.empty()) {
        excluded_array.push_back(
            typed_string_to_json("anyof", W_STRING_UNICODE));
      }
      container = &excluded_array;
    }

    auto term = json_array(
        {typed_string_to_json(term_name, W_STRING_UNICODE),
         typed_string_to_json(arg),
         typed_string_to_json("wholename", W_STRING_UNICODE)});
    if (negated) {
      term = json_array({typed_string_to_json("not", W_STRING_UNICODE), term});
    }
    container->push_back(std::move(term));

    // Reset negated flag
    negated = false;
    term_name = "match";
  }

  std::optional<json_ref> included = included_array.empty()
      ? std::nullopt
      : std::make_optional(json_array(std::move(included_array)));

  std::optional<json_ref> excluded;
  if (!excluded_array.empty()) {
    excluded = json_array(
        {typed_string_to_json("not", W_STRING_UNICODE),
         json_array(std::move(excluded_array))});
  }

  std::optional<json_ref> query_array;
  if (included && excluded) {
    query_array = json_array(
        {typed_string_to_json("allof", W_STRING_UNICODE),
         *excluded,
         *included});
  } else if (included) {
    query_array = included;
  } else {
    query_array = excluded;
  }

  // query_array may be NULL, which means find me all files.
  // Otherwise, it is the expression we want to use.
  if (query_array) {
    json_object_set_new_nocheck(
        query_obj, "expression", std::move(*query_array));
  }

  // For trigger
  if (next_arg) {
    *next_arg = i;
  }

  if (clockspec) {
    json_object_set_new_nocheck(
        query_obj, "since", typed_string_to_json(clockspec, W_STRING_UNICODE));
  }

  /* compose the query with the field list */
  auto query = parseQuery(root, query_obj);

  if (expr_p) {
    *expr_p = query_obj;
  }

  if (query) {
    w_query_legacy_field_list(&query->fieldList);
  }

  return query;
}

} // namespace watchman
