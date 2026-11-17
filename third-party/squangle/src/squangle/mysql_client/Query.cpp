/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/String.h>
#include <folly/Traits.h>
#include <algorithm>
#include <type_traits>
#include <vector>

#include "squangle/mysql_client/Query.h"
#include "squangle/mysql_client/QueryRenderer.h"

namespace facebook::common::mysql_client {

using ArgPair = std::pair<folly::fbstring, QueryArgument>;

// fbstring constructors
QueryArgument::QueryArgument(folly::StringPiece val)
    : value_(folly::fbstring(val.data(), val.size())) {}

QueryArgument::QueryArgument(std::string_view val)
    : value_(folly::fbstring(val.data(), val.size())) {}

QueryArgument::QueryArgument(char const* val) : value_(folly::fbstring(val)) {}

QueryArgument::QueryArgument(const std::string& string_value)
    : value_(folly::fbstring(string_value)) {}

QueryArgument::QueryArgument(const folly::fbstring& val) : value_(val) {}

QueryArgument::QueryArgument(folly::fbstring&& val) : value_(std::move(val)) {}

QueryArgument::QueryArgument(double double_val) : value_(double_val) {}

QueryArgument::QueryArgument(const std::initializer_list<QueryArgument>& list)
    : value_(std::vector<QueryArgument>(list.begin(), list.end())) {}

QueryArgument::QueryArgument(std::vector<QueryArgument> arg_list)
    : value_(std::move(arg_list)) {}

QueryArgument::QueryArgument() : value_(std::vector<ArgumentPair>()) {}

QueryArgument::QueryArgument(
    folly::StringPiece param1,
    const QueryArgument& param2)
    : value_(std::vector<ArgumentPair>()) {
  getPairs().emplace_back(param1, param2);
}

QueryArgument::QueryArgument(Query q) : value_(std::move(q)) {}

bool QueryArgument::isString() const {
  return std::holds_alternative<folly::fbstring>(value_);
}

bool QueryArgument::isQuery() const {
  return std::holds_alternative<Query>(value_);
}

bool QueryArgument::isPairList() const {
  return std::holds_alternative<std::vector<ArgPair>>(value_);
}

bool QueryArgument::isNull() const {
  return std::holds_alternative<std::monostate>(value_);
}

bool QueryArgument::isList() const {
  return std::holds_alternative<std::vector<QueryArgument>>(value_);
}

bool QueryArgument::isDouble() const {
  return std::holds_alternative<double>(value_);
}

bool QueryArgument::isInt() const {
  return std::holds_alternative<int64_t>(value_);
}

bool QueryArgument::isUInt() const {
  return std::holds_alternative<uint64_t>(value_);
}

bool QueryArgument::isTwoTuple() const {
  return std::holds_alternative<QualifiedColumn>(value_);
}

bool QueryArgument::isThreeTuple() const {
  return std::holds_alternative<AliasedQualifiedColumn>(value_);
}

bool QueryArgument::isAggregateColumn() const {
  return std::holds_alternative<AggregateColumn>(value_);
}

bool QueryArgument::isAliasedAggregateColumn() const {
  return std::holds_alternative<AliasedAggregateColumn>(value_);
}

QueryArgument&& QueryArgument::operator()(
    folly::StringPiece q1,
    const QueryArgument& q2) {
  getPairs().emplace_back(q1.str(), q2);
  return std::move(*this);
}

QueryArgument&& QueryArgument::operator()(
    folly::fbstring&& q1,
    QueryArgument&& q2) {
  getPairs().emplace_back(std::move(q1), std::move(q2));
  return std::move(*this);
}

folly::fbstring QueryArgument::asString() const {
  return std::visit(
      [](auto&& arg) -> folly::fbstring {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (
            std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
            std::is_same_v<T, uint64_t> || std::is_same_v<T, folly::fbstring>) {
          return folly::to<folly::fbstring>(arg);
        }

        throw std::invalid_argument(
            fmt::format(
                "Only allowed type conversions are Int, Double and String:"
                " type found: {}",
                typeid(arg).name()));
      },
      value_);
}

std::string_view QueryArgument::typeName() const {
  return std::visit(
      [](auto&& arg) -> std::string_view {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, double>) {
          return "double";
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return "int64_t";
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          return "uint64_t";
        } else if constexpr (std::is_same_v<T, folly::fbstring>) {
          return "string";
        } else if constexpr (std::is_same_v<T, std::monostate>) {
          return "monostate";
        } else if constexpr (std::is_same_v<T, Query>) {
          return "Query";
        } else if constexpr (std::is_same_v<T, std::vector<QueryArgument>>) {
          return "std::vector<QueryArgument>";
        } else if constexpr (std::is_same_v<T, std::vector<ArgumentPair>>) {
          return "std::vector<ArgumentPair>";
        } else if constexpr (std::is_same_v<T, QualifiedColumn>) {
          return "QualifiedColumn";
        } else if constexpr (std::is_same_v<T, AliasedQualifiedColumn>) {
          return "AliasedQualifiedColumn";
        } else if constexpr (std::is_same_v<T, AggregateColumn>) {
          return "AggregateColumn";
        } else if constexpr (std::is_same_v<T, AliasedAggregateColumn>) {
          return "AliasedAggregateColumn";
        } else {
          // Unreachable by construction: every alternative of the variant is
          // handled above. A static_assert makes that a build error rather
          // than a runtime abort if an alternative is ever added without a
          // branch here.
          static_assert(
              folly::always_false<T>,
              "Unhandled QueryArgument alternative in typeName()");
        }
      },
      value_);
}

double QueryArgument::getDouble() const {
  return std::get<double>(value_);
}

int64_t QueryArgument::getInt() const {
  return std::get<int64_t>(value_);
}

uint64_t QueryArgument::getUInt() const {
  return std::get<uint64_t>(value_);
}

const Query& QueryArgument::getQuery() const {
  return std::get<Query>(value_);
}

const folly::fbstring& QueryArgument::getString() const {
  return std::get<folly::fbstring>(value_);
}

const std::vector<QueryArgument>& QueryArgument::getList() const {
  return std::get<std::vector<QueryArgument>>(value_);
}

const std::vector<ArgPair>& QueryArgument::getPairs() const {
  return std::get<std::vector<ArgPair>>(value_);
}

const QualifiedColumn& QueryArgument::getTwoTuple() const {
  return std::get<QualifiedColumn>(value_);
}

const AliasedQualifiedColumn& QueryArgument::getThreeTuple() const {
  return std::get<AliasedQualifiedColumn>(value_);
}

const AggregateColumn& QueryArgument::getAggregateColumn() const {
  return std::get<AggregateColumn>(value_);
}

const AliasedAggregateColumn& QueryArgument::getAliasedAggregateColumn() const {
  return std::get<AliasedAggregateColumn>(value_);
}

void QueryArgument::initFromDynamic(const folly::dynamic& param) {
  // Convert to basic values and get type
  if (param.isObject()) {
    // List of pairs
    std::vector<folly::dynamic> keys(param.keys().begin(), param.keys().end());
    std::sort(keys.begin(), keys.end());
    value_ = std::vector<ArgPair>();
    auto& vec = std::get<std::vector<ArgPair>>(value_);
    vec.reserve(keys.size());

    for (const auto& key : keys) {
      QueryArgument q2(fromDynamic(param[key]));

      vec.emplace_back(key.asString(), q2);
    }
  } else if (param.isNull()) {
    value_ = {};
  } else if (param.isArray()) {
    value_ = std::vector<QueryArgument>();
    std::get<std::vector<QueryArgument>>(value_).reserve(param.size());
    auto& v = std::get<std::vector<QueryArgument>>(value_);
    for (const auto& val : param) {
      v.emplace_back(fromDynamic(val));
    }
  } else if (param.isString()) {
    value_ = folly::fbstring(param.getString());
  } else if (param.isBool()) {
    // MySQL has no boolean type (BOOL is an alias for tinyint(1)), so store as
    // an int that renders 0/1. QueryArgument has no bool alternative.
    value_ = static_cast<int64_t>(param.asBool());
  } else if (param.isDouble()) {
    value_ = param.asDouble();
  } else if (param.isInt()) {
    value_ = param.asInt();
  } else {
    throw std::invalid_argument("Dynamic type doesn't match to accepted ones");
  }
}

std::vector<ArgPair>& QueryArgument::getPairs() {
  return std::get<std::vector<ArgPair>>(value_);
}

// Query can be constructed with or without params.
// By default we deep copy the query text
Query::Query(const folly::StringPiece query_text) : query_text_(query_text) {}

Query::Query(QueryText&& query_text) : query_text_(std::move(query_text)) {}

Query::Query(
    const folly::StringPiece query_text,
    std::vector<QueryArgument> params)
    : query_text_(query_text),
      unsafe_query_(false),
      params_(std::move(params)) {}

Query::~Query() {}

Query::Query(Query&&) noexcept = default;
Query::Query(const Query&) = default;

Query& Query::operator=(Query&&) noexcept = default;
Query& Query::operator=(const Query&) = default;

namespace {

// EscapeMode is nested in QueryRenderer, so each StringType instantiation has
// its own type. These aliases name the two in use.
using FbEscapeMode = QueryRenderer<folly::fbstring>::EscapeMode;
using StdEscapeMode = QueryRenderer<std::string>::EscapeMode;

// Render via QueryRenderer<StringType>, turning the caller's checked/legacy
// distinction into the renderer's validate flag.
template <typename StringType>
StringType renderWith(
    bool checked,
    std::string_view queryText,
    bool unsafeQuery,
    const std::vector<QueryArgument>& params,
    typename QueryRenderer<StringType>::EscapeMode escapeMode,
    const InternalConnection* conn = nullptr,
    size_t maxSize = SIZE_MAX,
    std::string_view truncationIndicator = "...") {
  // A checked query's format string was validated at compile time, so the
  // renderer can skip the dangerous-character scan. That is the only
  // difference between the two modes; every other check runs either way. This
  // is the single place the checked/validate polarity is inverted.
  return QueryRenderer<StringType>::render(
      queryText,
      unsafeQuery,
      /*validate=*/!checked,
      params,
      escapeMode,
      conn,
      maxSize,
      truncationIndicator);
}

} // namespace

void Query::resolveModeOnAppend(Mode otherMode) {
  // The concatenation of two compile-time-checked queries is itself fully
  // validated: each side's specifiers, argument counts, and argument types were
  // checked at construction, and joining them (QueryText::operator+= inserts a
  // separating space) introduces no new specifiers. So the result stays Checked
  // and skips redundant render-time validation. If either side is a legacy
  // (unchecked) query, the merged query still needs render-time validation, so
  // it downgrades to Legacy.
  if (otherMode != Mode::Checked) {
    mode_ = Mode::Legacy;
  }
}

void Query::append(const Query& query2) {
  resolveModeOnAppend(query2.mode_);
  query_text_ += query2.query_text_;
  params_.reserve(params_.size() + query2.params_.size());
  for (const auto& param2 : query2.params_) {
    params_.push_back(param2);
  }
}

void Query::append(Query&& query2) {
  resolveModeOnAppend(query2.mode_);
  query_text_ += query2.query_text_;
  params_.reserve(params_.size() + query2.params_.size());
  for (auto& param2 : query2.params_) {
    params_.push_back(std::move(param2));
  }
}

namespace {

template <typename StringType, typename RenderFunc>
StringType renderMultiQueryImpl(
    const std::vector<Query>& queries,
    std::string_view query_prefix,
    RenderFunc&& renderOne) {
  size_t reserve_size = 0;
  for (const Query& query : queries) {
    reserve_size += query.getQueryFormat().size() +
        8 * query.getParams().size() + query_prefix.size();
  }
  if (queries.size() > 0) {
    reserve_size += queries.size() - 1;
  }

  StringType ret;
  ret.reserve(reserve_size);

  for (const Query& query : queries) {
    if (!ret.empty()) {
      ret.append(";");
    }
    if (!query_prefix.empty()) {
      ret.append(query_prefix.data(), query_prefix.size());
    }
    auto rendered = renderOne(query);
    ret.append(rendered.data(), rendered.size());
  }

  return ret;
}

} // namespace

folly::fbstring Query::renderMultiQueryFb(
    const InternalConnection* conn,
    const std::vector<Query>& queries,
    std::string_view query_prefix) {
  return renderMultiQueryImpl<folly::fbstring>(
      queries, query_prefix, [conn](const Query& q) {
        return q.renderFb(conn);
      });
}

std::string Query::renderMultiQueryStr(
    const InternalConnection* conn,
    const std::vector<Query>& queries,
    std::string_view query_prefix) {
  return renderMultiQueryImpl<std::string>(
      queries, query_prefix, [conn](const Query& q) {
        return q.renderStr(conn);
      });
}

// -- Fb variants --

folly::fbstring Query::renderFb(const InternalConnection* conn) const {
  return renderWith<folly::fbstring>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      FbEscapeMode::Full,
      conn);
}

// Caller-supplied params: Query::checked only proves the specifier count
// against params_, so this renders through the validating path, which reports
// a count mismatch as a parse error.
folly::fbstring Query::renderFb(
    const InternalConnection* conn,
    const std::vector<QueryArgument>& params) const {
  return renderWith<folly::fbstring>(
      /*checked=*/false,
      getQueryFormat(),
      unsafe_query_,
      params,
      FbEscapeMode::Full,
      conn);
}

folly::fbstring Query::renderInsecureFb() const {
  return renderWith<folly::fbstring>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      FbEscapeMode::None);
}

// Caller-supplied params: see renderFb(conn, params).
folly::fbstring Query::renderInsecureFb(
    const std::vector<QueryArgument>& params) const {
  return renderWith<folly::fbstring>(
      /*checked=*/false,
      getQueryFormat(),
      unsafe_query_,
      params,
      FbEscapeMode::None);
}

folly::fbstring Query::renderInsecureFb(
    size_t maxSize,
    std::string_view truncationIndicator) const {
  return renderWith<folly::fbstring>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      FbEscapeMode::None,
      nullptr,
      maxSize,
      truncationIndicator);
}

folly::fbstring Query::renderPartiallyEscapedFb() const {
  return renderWith<folly::fbstring>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      FbEscapeMode::Simple);
}

folly::fbstring Query::renderPartiallyEscapedFb(
    size_t maxSize,
    std::string_view truncationIndicator) const {
  return renderWith<folly::fbstring>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      FbEscapeMode::Simple,
      nullptr,
      maxSize,
      truncationIndicator);
}

// -- Str variants --

std::string Query::renderStr(const InternalConnection* conn) const {
  return renderWith<std::string>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      StdEscapeMode::Full,
      conn);
}

// Caller-supplied params: see renderFb(conn, params).
std::string Query::renderStr(
    const InternalConnection* conn,
    const std::vector<QueryArgument>& params) const {
  return renderWith<std::string>(
      /*checked=*/false,
      getQueryFormat(),
      unsafe_query_,
      params,
      StdEscapeMode::Full,
      conn);
}

std::string Query::renderInsecureStr() const {
  return renderWith<std::string>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      StdEscapeMode::None);
}

// Caller-supplied params: see renderFb(conn, params).
std::string Query::renderInsecureStr(
    const std::vector<QueryArgument>& params) const {
  return renderWith<std::string>(
      /*checked=*/false,
      getQueryFormat(),
      unsafe_query_,
      params,
      StdEscapeMode::None);
}

std::string Query::renderInsecureStr(
    size_t maxSize,
    std::string_view truncationIndicator) const {
  return renderWith<std::string>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      StdEscapeMode::None,
      nullptr,
      maxSize,
      truncationIndicator);
}

std::string Query::renderPartiallyEscapedStr() const {
  return renderWith<std::string>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      StdEscapeMode::Simple);
}

std::string Query::renderPartiallyEscapedStr(
    size_t maxSize,
    std::string_view truncationIndicator) const {
  return renderWith<std::string>(
      mode_ == Mode::Checked,
      getQueryFormat(),
      unsafe_query_,
      params_,
      StdEscapeMode::Simple,
      nullptr,
      maxSize,
      truncationIndicator);
}

std::shared_ptr<folly::fbstring> MultiQuery::renderQuery(
    const InternalConnection* conn,
    std::string_view prefix) const {
  if (!rendered_multi_query_) {
    rendered_multi_query_ = std::make_shared<folly::fbstring>(
        Query::renderMultiQuery(conn, queries_, prefix));
  }

  return rendered_multi_query_;
}

} // namespace facebook::common::mysql_client
