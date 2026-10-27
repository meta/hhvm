/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

//
// Unit tests for Squangle components using mock interfaces.
// Tests Query, QueryArgument, EphemeralRowFields, and end-to-end
// query rendering + mock execution without requiring a MySQL server.
//

// This file deliberately exercises the deprecated RowBlock build API
// (startRow()/appendValue()/appendNull()/finishRow()), including negative
// tests, so suppress the deprecation lint for the whole file.
// @lint-ignore-every CLANGTIDY facebook-hte-Deprecated

#include <fmt/core.h>
#include <folly/FixedString.h>
#include <folly/Optional.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <gtest/gtest.h>
#include <array>
#include <forward_list>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "squangle/mysql_client/OperationHelpers.h"

#include "squangle/mysql_client/Query.h"
#include "squangle/mysql_client/Row.h"
#include "squangle/mysql_client/test/MockInternalConnection.h"
#include "squangle/mysql_client/test/MockInternalResult.h"
#include "squangle/mysql_client/test/MockInternalRow.h"
#include "squangle/mysql_client/test/MockInternalRowMetadata.h"

namespace facebook::common::mysql_client::test {

// ============================================================================
// Checked Query Compile-Time Validation Tests
//
// These static_asserts verify that the checked query compile-time parser
// correctly accepts valid format strings and rejects invalid ones at
// compile time, without needing to actually invoke Query::checked() which
// would hard-fail compilation for invalid cases (making negative testing
// cumbersome in same translation unit). By testing the underlying
// detail:: traits directly, we prove the compile-time logic works.
//
// This serves as documentation of expected behavior and as regression
// protection for future changes to Query.h checked API.
// ============================================================================

namespace checked_compile_tests {
using namespace facebook::common::mysql_client;
using namespace facebook::common::mysql_client::detail;

// Valid format strings should parse ok
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM users"}>());
static_assert(
    parse_ok_fixed<fixed_string{"SELECT %s FROM %T WHERE %C = %d"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %C, %C FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %LC FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %Ls FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %Ld FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %Lu FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %Lf FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %Lm FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"UPDATE %T SET %U WHERE %W"}>());
static_assert(parse_ok_fixed<fixed_string{
                  "SELECT %C FROM %T WHERE %W AND %C <= %d LIMIT %d"}>());
static_assert(
    parse_ok_fixed<fixed_string{
        "SELECT COUNT(*) as oncall_cnt, SUM(CASE WHEN %W THEN 1 ELSE 0 END) as tenant_cnt FROM %T WHERE %W"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT %m FROM %T"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM %T WHERE %C %=d"}>());
static_assert(parse_ok_fixed<fixed_string{"INSERT INTO %T VALUES %V"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM %T %K"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM %T WHERE %LO"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM %T WHERE %LA"}>());
static_assert(parse_ok_fixed<fixed_string{"SELECT 50%% done FROM %T"}>());

// Disallowed specifiers should fail parse at compile time
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE name = ''"}>()); // dangerous single quote in
                                                    // literal
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users; DROP TABLE users"}>()); // dangerous semicolon
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE x = %Q"}>()); // %Q explicitly disallowed in
                                                 // checked mode
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE x = %v"}>()); // simplified %v alias not
                                                 // supported
static_assert(!parse_ok_fixed<fixed_string{
                  "SELECT * FROM %i WHERE id = 1"}>()); // simplified %i alias
                                                        // not supported
static_assert(!parse_ok_fixed<fixed_string{
                  "SELECT * FROM users WHERE x = %z"}>()); // unknown specifier
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE x = %LT"}>()); // %LT historically not
                                                  // working, explicitly
                                                  // rejected
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE x = %Lv"}>()); // simplified list alias not
                                                  // supported
static_assert(
    !parse_ok_fixed<fixed_string{
        "SELECT * FROM users WHERE x = %Li"}>()); // simplified list alias not
                                                  // supported
static_assert(!parse_ok_fixed<fixed_string{
                  "SELECT * FROM users WHERE x = %"}>()); // unfinished percent
static_assert(!parse_ok_fixed<fixed_string{
                  "SELECT * FROM \"users\""}>()); // dangerous double quote
static_assert(
    !parse_ok_fixed<fixed_string{"SELECT * FROM `users`"}>()); // dangerous
                                                               // backtick
static_assert(
    !parse_ok_fixed<fixed_string{"SELECT * WHERE x %="}>()); // incomplete %=
static_assert(!parse_ok_fixed<fixed_string{"SELECT x %L"}>()); // incomplete %L
static_assert(!parse_ok_fixed<fixed_string{
                  "SELECT %=x FROM t"}>()); // %= with bad sub-type

// Parameter count validation
static_assert(count_specs_fixed<fixed_string{"SELECT %s FROM %T"}>() == 2);
static_assert(count_specs_fixed<fixed_string{"SELECT * FROM users"}>() == 0);
static_assert(
    count_specs_fixed<fixed_string{"SELECT %% FROM users"}>() ==
    0); // %% does not consume argument
static_assert(
    count_specs_fixed<fixed_string{"UPDATE %T SET %U WHERE %W"}>() == 3);
static_assert(
    count_specs_fixed<fixed_string{"SELECT %=d FROM %T"}>() ==
    2); // %=d consumes one argument
static_assert(
    count_specs_fixed<fixed_string{"SELECT %Ls, %LC FROM %T"}>() == 3);

// ----------------------------------------------------------------------------
// Per-specifier argument type validation. For each specifier we assert every
// accepted argument category and that representative wrong categories are
// rejected. Acceptance mirrors the legacy QueryRenderer (see
// QueryRenderer.cpp).
// ----------------------------------------------------------------------------

// A scoped enum used to verify enums are accepted for integer/value specifiers
// (QueryArgument collapses them to int64, matching legacy).
enum class CheckedTestEnum : int { First = 1, Second = 7 };

// %s — any string-like type, optional<string-like>, or null. Not numbers,
// identifiers, or lists.
static_assert(check_args_fixed<fixed_string{"%s"}, std::string>());
static_assert(check_args_fixed<fixed_string{"%s"}, std::string_view>());
static_assert(check_args_fixed<fixed_string{"%s"}, folly::fbstring>());
static_assert(check_args_fixed<fixed_string{"%s"}, folly::StringPiece>());
static_assert(check_args_fixed<fixed_string{"%s"}, const char*>());
static_assert(
    check_args_fixed<fixed_string{"%s"}, std::optional<std::string>>());
static_assert(check_args_fixed<fixed_string{"%s"}, std::nullptr_t>());
static_assert(check_args_fixed<fixed_string{"%s"}, std::nullopt_t>());
static_assert(check_args_fixed<fixed_string{"%s"}, folly::None>());
// Regression: an engaged optional must accept exactly what a bare value does.
// The QueryArgument optional ctor delegates to the scalar ctors; a raw
// `value_ = opt.value()` used to reject optional<string_view>/optional<
// StringPiece> (fbstring's ctor from them is explicit), mis-store
// optional<const char*> as bool, and reject optional<unsigned>.
static_assert(
    check_args_fixed<fixed_string{"%s"}, std::optional<std::string_view>>());
static_assert(
    check_args_fixed<fixed_string{"%s"}, std::optional<folly::StringPiece>>());
static_assert(
    check_args_fixed<fixed_string{"%s"}, std::optional<const char*>>());
static_assert(
    std::is_constructible_v<QueryArgument, std::optional<std::string_view>>);
static_assert(
    std::is_constructible_v<QueryArgument, std::optional<folly::StringPiece>>);
static_assert(
    std::is_constructible_v<QueryArgument, std::optional<const char*>>);
static_assert(std::is_constructible_v<QueryArgument, std::optional<uint64_t>>);
static_assert(
    std::is_constructible_v<QueryArgument, folly::Optional<std::string_view>>);
// %s (and %m) accept a sub-Query; the other value specifiers do not.
static_assert(check_args_fixed<fixed_string{"%s"}, Query>());
static_assert(check_args_fixed<fixed_string{"%m"}, Query>());
static_assert(!check_args_fixed<fixed_string{"%d"}, Query>());
static_assert(!check_args_fixed<fixed_string{"%f"}, Query>());
// A pre-built (type-erased) QueryArgument is accepted for ANY specifier; its
// value-vs-specifier check is deferred to the runtime renderer. This is how
// existing call sites pass values (columnName(), QueryArgument::fromDynamic()).
static_assert(check_args_fixed<fixed_string{"%s"}, QueryArgument>());
static_assert(check_args_fixed<fixed_string{"%d"}, QueryArgument>());
static_assert(check_args_fixed<fixed_string{"%C"}, QueryArgument>());
static_assert(check_args_fixed<fixed_string{"%Ld"}, QueryArgument>());
static_assert(check_args_fixed<fixed_string{"%W"}, QueryArgument>());
static_assert(check_args_fixed<fixed_string{"%V"}, QueryArgument>());
// A bare folly::dynamic is NOT accepted by checked, even though it is
// type-erased. Runtime-shaped data must be passed as an explicit
// QueryArgument::fromDynamic() (a QueryArgument, accepted above) or a concrete
// typed argument. The legacy Query() constructor still accepts folly::dynamic.
static_assert(!check_args_fixed<fixed_string{"%LC"}, folly::dynamic>());
static_assert(!check_args_fixed<fixed_string{"%Lm"}, folly::dynamic>());
static_assert(!check_args_fixed<fixed_string{"%s"}, folly::dynamic>());
static_assert(!check_args_fixed<fixed_string{"%W"}, folly::dynamic>());
static_assert(!check_args_fixed<fixed_string{"%s"}, int>());
static_assert(!check_args_fixed<fixed_string{"%s"}, double>());
static_assert(!check_args_fixed<fixed_string{"%s"}, QualifiedColumn>());
static_assert(
    !check_args_fixed<fixed_string{"%s"}, std::vector<QueryArgument>>());

// %d and %u — any integral (including bool), optional<integral>, or null;
// matches legacy, which stores every integer as int64. Not strings or floats.
static_assert(check_args_fixed<fixed_string{"%d"}, int>());
static_assert(check_args_fixed<fixed_string{"%d"}, int64_t>());
static_assert(check_args_fixed<fixed_string{"%d"}, unsigned int>());
static_assert(check_args_fixed<fixed_string{"%d"}, std::size_t>());
static_assert(check_args_fixed<fixed_string{"%d"}, bool>());
static_assert(check_args_fixed<fixed_string{"%d"}, std::optional<int>>());
static_assert(check_args_fixed<fixed_string{"%d"}, std::nullptr_t>());
static_assert(check_args_fixed<fixed_string{"%u"}, int>());
static_assert(check_args_fixed<fixed_string{"%u"}, unsigned int>());
static_assert(!check_args_fixed<fixed_string{"%d"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%d"}, std::string>());
static_assert(!check_args_fixed<fixed_string{"%d"}, double>());
static_assert(!check_args_fixed<fixed_string{"%u"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%u"}, double>());

// Enums are accepted wherever an integer is (QueryArgument stores them as
// int64), so callers need not static_cast. Also valid for %m and as list/
// optional element types; still rejected for string/identifier specifiers.
static_assert(check_args_fixed<fixed_string{"%d"}, CheckedTestEnum>());
static_assert(check_args_fixed<fixed_string{"%u"}, CheckedTestEnum>());
static_assert(check_args_fixed<fixed_string{"%m"}, CheckedTestEnum>());
static_assert(check_args_fixed<fixed_string{"%=d"}, CheckedTestEnum>());
static_assert(
    check_args_fixed<fixed_string{"%d"}, std::optional<CheckedTestEnum>>());
static_assert(
    check_args_fixed<fixed_string{"%Ld"}, std::vector<CheckedTestEnum>>());
static_assert(!check_args_fixed<fixed_string{"%s"}, CheckedTestEnum>());
static_assert(!check_args_fixed<fixed_string{"%T"}, CheckedTestEnum>());

// %f — floating point, optional<float>, or null. Not ints or strings.
static_assert(check_args_fixed<fixed_string{"%f"}, double>());
static_assert(check_args_fixed<fixed_string{"%f"}, float>());
static_assert(check_args_fixed<fixed_string{"%f"}, std::optional<double>>());
static_assert(check_args_fixed<fixed_string{"%f"}, std::nullptr_t>());
static_assert(!check_args_fixed<fixed_string{"%f"}, int>());
static_assert(!check_args_fixed<fixed_string{"%f"}, const char*>());

// %m — any value (arithmetic, bool, string-like, Query, null, optional). Not an
// identifier tuple, pair list, or value list.
static_assert(check_args_fixed<fixed_string{"%m"}, int>());
static_assert(check_args_fixed<fixed_string{"%m"}, double>());
static_assert(check_args_fixed<fixed_string{"%m"}, bool>());
static_assert(check_args_fixed<fixed_string{"%m"}, std::string>());
static_assert(check_args_fixed<fixed_string{"%m"}, const char*>());
static_assert(check_args_fixed<fixed_string{"%m"}, Query>());
static_assert(check_args_fixed<fixed_string{"%m"}, std::nullptr_t>());
static_assert(check_args_fixed<fixed_string{"%m"}, std::optional<int>>());
static_assert(!check_args_fixed<fixed_string{"%m"}, QualifiedColumn>());
// An optional is accepted only when what it contains is, so a category error
// inside the optional is still a compile error rather than a render-time throw.
static_assert(
    check_args_fixed<fixed_string{"%m"}, std::optional<std::string>>());
static_assert(check_args_fixed<fixed_string{"%m"}, folly::Optional<double>>());
static_assert(
    !check_args_fixed<fixed_string{"%m"}, std::optional<QualifiedColumn>>());
static_assert(!check_args_fixed<
              fixed_string{"%Lm"},
              std::vector<std::optional<QualifiedColumn>>>());
static_assert(
    !check_args_fixed<fixed_string{"%m"}, std::vector<QueryArgument>>());
static_assert(!check_args_fixed<
              fixed_string{"%m"},
              std::vector<std::pair<folly::fbstring, QueryArgument>>>());

// %T and %C — identifier: string-like or a (possibly aliased / aggregate)
// qualified column tuple. Not numbers, lists, or pair lists.
static_assert(check_args_fixed<fixed_string{"%T"}, const char*>());
static_assert(check_args_fixed<fixed_string{"%T"}, std::string>());
static_assert(check_args_fixed<fixed_string{"%T"}, QualifiedColumn>());
static_assert(check_args_fixed<fixed_string{"%C"}, std::string_view>());
static_assert(check_args_fixed<fixed_string{"%C"}, QualifiedColumn>());
static_assert(check_args_fixed<fixed_string{"%C"}, AliasedQualifiedColumn>());
static_assert(check_args_fixed<fixed_string{"%C"}, AggregateColumn>());
static_assert(check_args_fixed<fixed_string{"%C"}, AliasedAggregateColumn>());
static_assert(!check_args_fixed<fixed_string{"%C"}, int>());
static_assert(!check_args_fixed<fixed_string{"%T"}, int>());
static_assert(
    !check_args_fixed<fixed_string{"%C"}, std::vector<QueryArgument>>());

// %=s / %=d / %=u / %=f / %=m — same categories as the scalar equivalents (and
// null renders as IS NULL).
static_assert(check_args_fixed<fixed_string{"%=s"}, const char*>());
static_assert(check_args_fixed<fixed_string{"%=d"}, int>());
static_assert(check_args_fixed<fixed_string{"%=d"}, std::nullptr_t>());
static_assert(check_args_fixed<fixed_string{"%=f"}, double>());
static_assert(check_args_fixed<fixed_string{"%=m"}, std::string>());
static_assert(!check_args_fixed<fixed_string{"%=d"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%=s"}, int>());

// %Ls / %Ld / %Lu / %Lf / %Lm / %LC — require a list (vector / initializer_list
// of QueryArgument); a scalar of the element type is rejected (matches legacy).
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, std::vector<QueryArgument>>());
static_assert(
    check_args_fixed<fixed_string{"%Ld"}, std::vector<QueryArgument>>());
static_assert(
    check_args_fixed<fixed_string{"%Lu"}, std::vector<QueryArgument>>());
static_assert(
    check_args_fixed<fixed_string{"%Lf"}, std::vector<QueryArgument>>());
static_assert(
    check_args_fixed<fixed_string{"%Lm"}, std::vector<QueryArgument>>());
static_assert(check_args_fixed<
              fixed_string{"%LC"},
              std::initializer_list<QueryArgument>>());
static_assert(!check_args_fixed<fixed_string{"%Ls"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%Ld"}, int>());
static_assert(!check_args_fixed<fixed_string{"%LC"}, const char*>());

// A homogeneous typed vector is also accepted for an element-list specifier,
// with its element type checked at compile time (stronger than a type-erased
// std::vector<QueryArgument>). The element type must match the specifier.
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, std::vector<std::string>>());
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, std::vector<std::string_view>>());
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, std::vector<folly::fbstring>>());
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::vector<int64_t>>());
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::vector<int>>());
static_assert(check_args_fixed<fixed_string{"%Lu"}, std::vector<uint64_t>>());
static_assert(check_args_fixed<fixed_string{"%Lf"}, std::vector<double>>());
static_assert(check_args_fixed<fixed_string{"%Lm"}, std::vector<int>>());
static_assert(
    check_args_fixed<fixed_string{"%LC"}, std::vector<std::string>>());
// Element type must match the specifier's subtype.
static_assert(
    !check_args_fixed<fixed_string{"%Ld"}, std::vector<std::string>>());
static_assert(!check_args_fixed<fixed_string{"%Ls"}, std::vector<int>>());
static_assert(!check_args_fixed<fixed_string{"%Lf"}, std::vector<int>>());

// std::set / std::unordered_set are accepted the same way as std::vector, with
// the same element-type checking.
static_assert(check_args_fixed<fixed_string{"%Ls"}, std::set<std::string>>());
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, std::unordered_set<std::string>>());
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::set<int64_t>>());
static_assert(
    check_args_fixed<fixed_string{"%Ld"}, std::unordered_set<uint64_t>>());
static_assert(check_args_fixed<fixed_string{"%LC"}, std::set<std::string>>());
static_assert(!check_args_fixed<fixed_string{"%Ld"}, std::set<std::string>>());
static_assert(!check_args_fixed<fixed_string{"%Ls"}, std::set<int>>());

// Any iterable collection works via the QueryArgumentCollection concept — e.g.
// folly's F14 sets — without the renderer naming the container type.
static_assert(
    check_args_fixed<fixed_string{"%Ls"}, folly::F14FastSet<std::string>>());
static_assert(
    check_args_fixed<fixed_string{"%Ld"}, folly::F14FastSet<int64_t>>());
static_assert(
    !check_args_fixed<fixed_string{"%Ld"}, folly::F14FastSet<std::string>>());
// A non-sized range (no size()) is also accepted: toQueryArgumentList guards
// its reserve() with sized_range, so e.g. std::forward_list works.
static_assert(std::is_constructible_v<QueryArgument, std::forward_list<int>>);
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::forward_list<int>>());
// A std::string is a range of char but must stay a scalar, not a list.
static_assert(!check_args_fixed<fixed_string{"%Ls"}, std::string>());
// A map is a range, but its pair element isn't a list element.
static_assert(!check_args_fixed<
              fixed_string{"%Ls"},
              std::unordered_map<std::string, int>>());

// %U / %W / %LO / %LA — pair list: a typed vector<pair<string-ish,
// QueryArgument>>. Not a scalar, a plain value list, or an int. A bare
// folly::dynamic (object) is NOT accepted by checked -- use a typed pair list
// or QueryArgument::fromDynamic(); the legacy Query() ctor still accepts it.
static_assert(check_args_fixed<
              fixed_string{"%U"},
              std::vector<std::pair<folly::fbstring, QueryArgument>>>());
static_assert(check_args_fixed<
              fixed_string{"%U"},
              std::vector<std::pair<std::string, QueryArgument>>>());
static_assert(!check_args_fixed<fixed_string{"%W"}, folly::dynamic>());
// folly::dynamic::object(...) returns a (private) ObjectMaker; also rejected.
static_assert(!check_args_fixed<
              fixed_string{"%W"},
              decltype(folly::dynamic::object("k", 1))>());
static_assert(check_args_fixed<
              fixed_string{"%LO"},
              std::vector<std::pair<folly::fbstring, QueryArgument>>>());
static_assert(!check_args_fixed<fixed_string{"%LA"}, folly::dynamic>());
static_assert(!check_args_fixed<fixed_string{"%U"}, const char*>());
static_assert(
    !check_args_fixed<fixed_string{"%U"}, std::vector<QueryArgument>>());
static_assert(!check_args_fixed<fixed_string{"%W"}, int>());
static_assert(!check_args_fixed<fixed_string{"%LO"}, int>());

// Associative pair lists: any range of string-keyed pairs is accepted for
// %U/%W/%LO/%LA (std::map / std::unordered_map / folly F14), enabling
// lookup-and-replace by column before the query is built.
static_assert(check_args_fixed<
              fixed_string{"%U"},
              std::map<std::string, QueryArgument>>());
static_assert(check_args_fixed<
              fixed_string{"%W"},
              std::unordered_map<std::string, QueryArgument>>());
static_assert(check_args_fixed<
              fixed_string{"%LO"},
              folly::F14FastMap<std::string, QueryArgument>>());
static_assert(check_args_fixed<
              fixed_string{"%LA"},
              std::map<folly::fbstring, QueryArgument>>());
// A non-QueryArgument value is fine as long as it is
// QueryArgument-constructible.
static_assert(
    check_args_fixed<fixed_string{"%U"}, std::map<std::string, std::string>>());
// ...and it must actually be constructible, not merely pass the category
// predicate: a map<string, string> is both a PairRange and (because its
// value_type pair converts to QualifiedColumn) a QueryArgumentCollection, so
// the two constrained ctors must not be ambiguous.
static_assert(
    std::is_constructible_v<QueryArgument, std::map<std::string, std::string>>);
// A map whose key is not string-like is not a valid pair list.
static_assert(
    !check_args_fixed<fixed_string{"%U"}, std::map<int, QueryArgument>>());

// %V — values matrix, supplied as a type-erased vector<QueryArgument> whose
// elements are themselves lists, or as a vector<vector<QueryArgument>> (each
// inner vector becomes a row). Not a scalar, and not a generic
// vector<vector<T>> with a uniform cell type (rows are heterogeneous, so cells
// are QueryArguments).
static_assert(
    check_args_fixed<fixed_string{"%V"}, std::vector<QueryArgument>>());
static_assert(check_args_fixed<
              fixed_string{"%V"},
              std::vector<std::vector<QueryArgument>>>());
static_assert(
    !check_args_fixed<fixed_string{"%V"}, std::vector<std::vector<int>>>());
static_assert(!check_args_fixed<fixed_string{"%V"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%V"}, int>());
// A std::vector<ValueRow<Fmt>> is a values matrix: each row's arity/types are
// checked at ValueRow construction, so %V just accepts the whole matrix.
static_assert(
    check_args_fixed<fixed_string{"%V"}, std::vector<ValueRow<"%d %s %f">>>());
// ValueRow's constrained ctor enforces arity and per-column types at compile
// time (so is_constructible reflects the schema).
static_assert(std::is_constructible_v<ValueRow<"%d %s">, int, const char*>);
static_assert(!std::is_constructible_v<ValueRow<"%d %s">, int>); // too few
static_assert(
    !std::is_constructible_v<ValueRow<"%d %s">, int, const char*, int>); // many
static_assert(
    !std::is_constructible_v<ValueRow<"%d %s">, const char*, const char*>);
static_assert(
    !std::is_constructible_v<ValueRow<"%d %s">, folly::dynamic, const char*>);
// The schema accepts specifiers separated by no delimiter, whitespace, commas,
// or vertical bars -- all equivalent (here: a 3-column %d %s %f row).
static_assert(
    std::is_constructible_v<ValueRow<"%d%s%f">, int, const char*, double>);
static_assert(
    std::is_constructible_v<ValueRow<"%d %s %f">, int, const char*, double>);
static_assert(
    std::is_constructible_v<ValueRow<"%d\t%s\t%f">, int, const char*, double>);
static_assert(
    std::is_constructible_v<ValueRow<"%d,%s,%f">, int, const char*, double>);
static_assert(
    std::is_constructible_v<ValueRow<"%s|%d|%f">, const char*, int, double>);
// Arity is unaffected by delimiter style: the comma form still rejects 2 args.
static_assert(!std::is_constructible_v<ValueRow<"%d,%s,%f">, int, const char*>);

// A %q column takes a Query and nothing else, so a cell whose value is an
// expression keeps its own compile-time guarantees.
static_assert(std::is_constructible_v<ValueRow<"%q %d">, Query, int>);
static_assert(!std::is_constructible_v<ValueRow<"%q %d">, const char*, int>);
static_assert(!std::is_constructible_v<ValueRow<"%q %d">, int, int>);
// A matrix of %q-bearing rows is still a values matrix for %V.
static_assert(
    check_args_fixed<fixed_string{"%V"}, std::vector<ValueRow<"%q %d">>>());

// %K — comment: string-like or optional<string-like>. Not numbers or lists.
static_assert(check_args_fixed<fixed_string{"%K"}, const char*>());
static_assert(check_args_fixed<fixed_string{"%K"}, std::string>());
static_assert(
    check_args_fixed<fixed_string{"%K"}, std::optional<std::string>>());
static_assert(!check_args_fixed<fixed_string{"%K"}, int>());
static_assert(
    !check_args_fixed<fixed_string{"%K"}, std::vector<QueryArgument>>());

// Multi-argument format strings and argument-count mismatches.
static_assert(check_args_fixed<
              fixed_string{"SELECT %C FROM %T WHERE %C = %s"},
              const char*,
              const char*,
              const char*,
              const char*>());
static_assert(
    !check_args_fixed<fixed_string{"SELECT %s"}>(),
    "too few args should fail");
static_assert(
    !check_args_fixed<fixed_string{"SELECT a"}, int>(),
    "too many args should fail");

// ----------------------------------------------------------------------------
// The string-like set is shared across specifiers.
//
// is_value_arg_v (%s/%m), is_identifier_arg_v (%T/%C) and CommentArg (%K) are
// all defined in terms of is_string_like_v, so every accepted string type is
// accepted by all of them. These pin that: when the three traits enumerated
// the types separately the lists drifted, and char* had to be added to each
// one to keep %s consistent with %m and %T/%C.
// ----------------------------------------------------------------------------
template <typename T>
constexpr bool stringTypeAcceptedEverywhere() {
  return check_args_fixed<fixed_string{"%s"}, T>() &&
      check_args_fixed<fixed_string{"%m"}, T>() &&
      check_args_fixed<fixed_string{"%=s"}, T>() &&
      check_args_fixed<fixed_string{"%T"}, T>() &&
      check_args_fixed<fixed_string{"%C"}, T>() &&
      check_args_fixed<fixed_string{"%K"}, T>();
}
static_assert(stringTypeAcceptedEverywhere<std::string>());
static_assert(stringTypeAcceptedEverywhere<std::string_view>());
static_assert(stringTypeAcceptedEverywhere<folly::fbstring>());
static_assert(stringTypeAcceptedEverywhere<folly::StringPiece>());
static_assert(stringTypeAcceptedEverywhere<const char*>());
static_assert(stringTypeAcceptedEverywhere<char*>());
// A non-string type must not be swept in by the shared definition.
static_assert(!stringTypeAcceptedEverywhere<int>());
static_assert(!stringTypeAcceptedEverywhere<std::vector<QueryArgument>>());

// ----------------------------------------------------------------------------
// char* parity with const char*.
//
// A mutable buffer (char buf[N]) decays to char*, which the legacy
// QueryArgument(char const*) ctor accepts, so it must work wherever const char*
// does.
// ----------------------------------------------------------------------------
static_assert(std::is_constructible_v<QueryArgument, char*>);
static_assert(check_args_fixed<fixed_string{"%s"}, char*>());
static_assert(check_args_fixed<fixed_string{"%m"}, char*>());
static_assert(check_args_fixed<fixed_string{"%T"}, char*>());
static_assert(check_args_fixed<fixed_string{"%C"}, char*>());
static_assert(check_args_fixed<fixed_string{"%=s"}, char*>());
static_assert(check_args_fixed<fixed_string{"%K"}, char*>());
static_assert(check_args_fixed<fixed_string{"%Ls"}, std::vector<char*>>());

// ----------------------------------------------------------------------------
// Character ranges are not element lists.
//
// is_string_like_v enumerates exact types, so a range of char is neither
// string-like nor (now) a QueryArgumentCollection. Without the char exclusion
// these silently satisfied the collection concept and rendered as a list of
// character codes for %Ld/%Lu; they must be a compile error instead. Use
// .toRange() or a std::string_view for string semantics.
// ----------------------------------------------------------------------------
static_assert(!check_args_fixed<fixed_string{"%Ld"}, folly::FixedString<4>>());
static_assert(!check_args_fixed<fixed_string{"%Lu"}, folly::FixedString<4>>());
static_assert(!check_args_fixed<fixed_string{"%Ls"}, folly::FixedString<4>>());
static_assert(!check_args_fixed<fixed_string{"%s"}, folly::FixedString<4>>());
static_assert(!check_args_fixed<fixed_string{"%Ld"}, std::vector<char>>());
static_assert(!check_args_fixed<fixed_string{"%Lu"}, std::vector<char>>());
static_assert(!check_args_fixed<fixed_string{"%Ls"}, std::vector<char>>());
static_assert(!check_args_fixed<fixed_string{"%m"}, std::vector<char>>());
static_assert(!check_args_fixed<fixed_string{"%Ld"}, std::array<char, 4>>());
// The exclusion tests for exactly `char`. int8_t is signed char and uint8_t is
// unsigned char -- distinct types -- so genuine small-integer lists still work.
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::vector<int8_t>>());
static_assert(check_args_fixed<fixed_string{"%Lu"}, std::vector<uint8_t>>());
static_assert(check_args_fixed<fixed_string{"%Ld"}, std::vector<int>>());

// ----------------------------------------------------------------------------
// ValueRow schema. valid_value_row_schema reads specifier forms off the parse
// result instead of re-recognising specifiers, so it cannot drift from
// consteval_parse_checked. These pin the accepted and rejected sets; testing
// the predicate directly avoids instantiating ValueRow, whose static_assert
// would hard-fail the build rather than yield false.
// ----------------------------------------------------------------------------
static_assert(valid_value_row_schema<fixed_string{"%d %s %f %u"}>());
static_assert(valid_value_row_schema<fixed_string{"%d%s"}>());
static_assert(valid_value_row_schema<fixed_string{"%d,%s|%f"}>());
static_assert(valid_value_row_schema<fixed_string{""}>()); // zero columns
// %q declares a sub-Query cell, and mixes with the literal specifiers. %V
// renders cells through appendValue's 'v' type, which splices a Query whatever
// the declared specifier, so the schema and the renderer agree. (The
// single-column cases live with the other %q rules below.)
static_assert(valid_value_row_schema<fixed_string{"%q %d %s"}>());
// %h is rejected even though it is a plain value specifier: the 'v' path has
// no hex-encoding branch, so the cell would render as a quoted string and the
// schema would promise something the renderer does not do.
static_assert(!valid_value_row_schema<fixed_string{"%h"}>());
static_assert(!valid_value_row_schema<fixed_string{"%d %h"}>());
// %m opts out of per-column type checking, defeating the point of a schema.
static_assert(!valid_value_row_schema<fixed_string{"%m"}>());
static_assert(!valid_value_row_schema<fixed_string{"%d %m"}>());
// Non-plain forms. %=s and %Ls both record spec_char 's', so SpecForm is the
// only thing that distinguishes them from a plain %s here.
static_assert(!valid_value_row_schema<fixed_string{"%=s"}>());
static_assert(!valid_value_row_schema<fixed_string{"%Ls"}>());
static_assert(!valid_value_row_schema<fixed_string{"%LC"}>());
static_assert(!valid_value_row_schema<fixed_string{"%LO"}>());
static_assert(!valid_value_row_schema<fixed_string{"%LA"}>());
// Identifier, pair and structural specifiers are not value cells.
static_assert(!valid_value_row_schema<fixed_string{"%T"}>());
static_assert(!valid_value_row_schema<fixed_string{"%C"}>());
static_assert(!valid_value_row_schema<fixed_string{"%U"}>());
static_assert(!valid_value_row_schema<fixed_string{"%W"}>());
static_assert(!valid_value_row_schema<fixed_string{"%V"}>());
static_assert(!valid_value_row_schema<fixed_string{"%K"}>());
// A literal percent is not a value cell.
static_assert(!valid_value_row_schema<fixed_string{"%%"}>());
static_assert(!valid_value_row_schema<fixed_string{"%d %% %s"}>());
// Stray literal text, so a dropped % is not swallowed as a delimiter.
static_assert(!valid_value_row_schema<fixed_string{"%d x %s"}>());
static_assert(!valid_value_row_schema<fixed_string{"%d s"}>());
// A format the main parser rejects is not a valid schema either.
static_assert(!valid_value_row_schema<fixed_string{"%d %"}>());
static_assert(!valid_value_row_schema<fixed_string{"%z"}>());

// ----------------------------------------------------------------------------
// %q — sub-query, and only a sub-query.
//
// A Query is also accepted by %s and %m (and renders as a spliced sub-query
// there), but those accept plain strings too, which render as a quoted literal
// instead. %q makes the intent explicit and the wrong type a compile error.
// ----------------------------------------------------------------------------
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM (%q) t"}>());
static_assert(count_specs_fixed<fixed_string{"SELECT * FROM (%q) t"}>() == 1);
static_assert(check_args_fixed<fixed_string{"%q"}, Query>());
// Everything that is not a Query is rejected, including the things %s allows.
static_assert(!check_args_fixed<fixed_string{"%q"}, std::string>());
static_assert(!check_args_fixed<fixed_string{"%q"}, std::string_view>());
static_assert(!check_args_fixed<fixed_string{"%q"}, const char*>());
static_assert(!check_args_fixed<fixed_string{"%q"}, char*>());
static_assert(!check_args_fixed<fixed_string{"%q"}, int>());
static_assert(!check_args_fixed<fixed_string{"%q"}, std::nullptr_t>());
static_assert(!check_args_fixed<fixed_string{"%q"}, std::optional<Query>>());
static_assert(
    !check_args_fixed<fixed_string{"%q"}, std::vector<QueryArgument>>());
// A bare QueryArgument is the type-erased escape hatch and is accepted for any
// specifier; %q validates it at render time instead.
static_assert(check_args_fixed<fixed_string{"%q"}, QueryArgument>());
// %Lq takes a list of sub-queries.
static_assert(parse_ok_fixed<fixed_string{"UPDATE t SET %Lq"}>());
static_assert(check_args_fixed<fixed_string{"%Lq"}, std::vector<Query>>());
static_assert(
    !check_args_fixed<fixed_string{"%Lq"}, std::vector<std::string>>());
static_assert(!check_args_fixed<fixed_string{"%Lq"}, Query>());
// A type-erased list is accepted and validated per element at render time.
static_assert(
    check_args_fixed<fixed_string{"%Lq"}, std::vector<QueryArgument>>());
// %q is a valid ValueRow cell type: a column whose value is an expression.
static_assert(valid_value_row_schema<fixed_string{"%q"}>());
static_assert(valid_value_row_schema<fixed_string{"%d %q"}>());
// There is no %=q.
static_assert(!parse_ok_fixed<fixed_string{"SELECT * FROM t WHERE %C%=q"}>());

// ----------------------------------------------------------------------------
// %h — a MySQL binary literal, X'<hex>'.
//
// Takes the same string-like types as %s, but renders their bytes hex-encoded
// instead of quoting them. Numbers are rejected: hex-encoding the decimal text
// of an int is never what a caller means.
// ----------------------------------------------------------------------------
static_assert(parse_ok_fixed<fixed_string{"SELECT %h"}>());
static_assert(count_specs_fixed<fixed_string{"SELECT %h"}>() == 1);
static_assert(check_args_fixed<fixed_string{"%h"}, std::string>());
static_assert(check_args_fixed<fixed_string{"%h"}, std::string_view>());
static_assert(check_args_fixed<fixed_string{"%h"}, folly::fbstring>());
static_assert(check_args_fixed<fixed_string{"%h"}, folly::StringPiece>());
static_assert(check_args_fixed<fixed_string{"%h"}, const char*>());
static_assert(check_args_fixed<fixed_string{"%h"}, char*>());
static_assert(
    check_args_fixed<fixed_string{"%h"}, std::optional<std::string>>());
static_assert(check_args_fixed<fixed_string{"%h"}, std::nullptr_t>());
static_assert(check_args_fixed<fixed_string{"%h"}, std::nullopt_t>());
static_assert(!check_args_fixed<fixed_string{"%h"}, int>());
static_assert(!check_args_fixed<fixed_string{"%h"}, double>());
static_assert(!check_args_fixed<fixed_string{"%h"}, bool>());
static_assert(!check_args_fixed<fixed_string{"%h"}, Query>());
static_assert(
    !check_args_fixed<fixed_string{"%h"}, std::vector<std::string>>());
// Byte-range types are not accepted: every hex-literal call site in fbcode
// today hands the bytes over as a std::string, so there is nothing to serve.
// Supporting one means a new QueryArgument constructor, because
// ByteRange -> StringPiece is an explicit conversion.
static_assert(!check_args_fixed<fixed_string{"%h"}, folly::ByteRange>());
// The type-erased escape hatch, validated at render time like every other
// specifier.
static_assert(check_args_fixed<fixed_string{"%h"}, QueryArgument>());
// %Lh is a comma-separated list of binary literals, taking the same element
// types %h takes as a scalar.
static_assert(parse_ok_fixed<fixed_string{"SELECT %Lh"}>());
static_assert(
    check_args_fixed<fixed_string{"%Lh"}, std::vector<std::string>>());
static_assert(
    check_args_fixed<fixed_string{"%Lh"}, std::vector<folly::StringPiece>>());
static_assert(!check_args_fixed<fixed_string{"%Lh"}, std::vector<int>>());
static_assert(!check_args_fixed<fixed_string{"%Lh"}, std::string>());
static_assert(
    check_args_fixed<fixed_string{"%Lh"}, std::vector<QueryArgument>>());
// %=h compares against a binary literal, and takes what scalar %h takes.
static_assert(parse_ok_fixed<fixed_string{"SELECT * FROM t WHERE %C%=h"}>());
static_assert(check_args_fixed<fixed_string{"%=h"}, std::string>());
static_assert(check_args_fixed<fixed_string{"%=h"}, std::nullptr_t>());
static_assert(!check_args_fixed<fixed_string{"%=h"}, int>());
// %h remains invalid as a ValueRow cell.
static_assert(!valid_value_row_schema<fixed_string{"%h"}>());

} // namespace checked_compile_tests

// ============================================================================
// Query Class Tests
//
// Tests query parsing, rendering, and parameter binding.
// Uses renderInsecure() which requires no database connection.
// Note: renderInsecure() uses double quotes for strings, not single quotes.
// =============================================================================

class QueryTest : public ::testing::Test {};

TEST_F(QueryTest, SimpleQueryRender) {
  Query q("SELECT * FROM users");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users");
}

TEST_F(QueryTest, QueryWithIntParameter) {
  Query q("SELECT * FROM users WHERE id = %d", 42);
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE id = 42");
}

TEST_F(QueryTest, QueryWithStringParameter) {
  Query q("SELECT * FROM users WHERE name = %s", "Alice");
  auto rendered = q.renderInsecure();
  // renderInsecure() uses double quotes for strings
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE name = \"Alice\"");
}

TEST_F(QueryTest, QueryWithTableName) {
  Query q("SELECT * FROM %T WHERE id = 1", "users");
  auto rendered = q.renderInsecure();
  // Table names use backticks
  EXPECT_EQ(rendered, "SELECT * FROM `users` WHERE id = 1");
}

TEST_F(QueryTest, QueryWithColumnName) {
  Query q("SELECT %C FROM users", "email");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT `email` FROM users");
}

TEST_F(QueryTest, QueryWithMultipleParameters) {
  Query q(
      "INSERT INTO users (name, age, active) VALUES (%s, %d, %d)",
      "Bob",
      30,
      1);
  auto rendered = q.renderInsecure();
  // Strings use double quotes
  EXPECT_EQ(
      rendered,
      "INSERT INTO users (name, age, active) VALUES (\"Bob\", 30, 1)");
}

TEST_F(QueryTest, QueryAppend) {
  Query q1("SELECT * FROM users");
  Query q2("WHERE active = 1");
  q1.append(q2);
  auto rendered = q1.renderInsecure();
  // append() adds a space between the two queries
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE active = 1");
}

TEST_F(QueryTest, QueryConcatenation) {
  Query q1("SELECT * FROM users");
  Query q2("ORDER BY id");
  Query combined = q1 + q2;
  auto rendered = combined.renderInsecure();
  // operator+ adds a space between the two queries
  EXPECT_EQ(rendered, "SELECT * FROM users ORDER BY id");
}

TEST_F(QueryTest, UnsafeQuery) {
  auto q = Query::unsafe("SELECT * FROM users; DROP TABLE users;");
  EXPECT_TRUE(q.isUnsafe());
}

TEST_F(QueryTest, SafeQueryIsNotUnsafe) {
  Query q("SELECT * FROM users WHERE id = %d", 1);
  EXPECT_FALSE(q.isUnsafe());
}

TEST_F(QueryTest, QueryGetFormat) {
  Query q("SELECT * FROM %T WHERE id = %d", "users", 42);
  EXPECT_EQ(q.getQueryFormat(), "SELECT * FROM %T WHERE id = %d");
}

TEST_F(QueryTest, QueryWithBoolAsInt) {
  // Booleans are converted to int64_t internally
  Query q("SELECT * FROM users WHERE active = %d", true);
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE active = 1");

  Query q2("SELECT * FROM users WHERE active = %d", false);
  auto rendered2 = q2.renderInsecure();
  EXPECT_EQ(rendered2, "SELECT * FROM users WHERE active = 0");
}

TEST_F(QueryTest, QueryWithNullParameter) {
  Query q("UPDATE users SET deleted_at = %s WHERE id = %d", nullptr, 1);
  auto rendered = q.renderInsecure();
  // nullptr becomes NULL
  EXPECT_TRUE(rendered.find("NULL") != std::string::npos);
}

TEST_F(QueryTest, QueryWithDoubleParameter) {
  Query q("SELECT * FROM products WHERE price < %f", 19.99);
  auto rendered = q.renderInsecure();
  EXPECT_TRUE(rendered.find("19.99") != std::string::npos);
}

TEST_F(QueryTest, CheckedSimpleQueryRender) {
  auto q = Query::checked("SELECT * FROM users");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users");
  EXPECT_TRUE(q.isChecked());
}

TEST_F(QueryTest, CheckedAcceptsConstexprFormatConstants) {
  // The format may be a string literal, a constexpr char array, or a constexpr
  // std::string_view — all are compile-time constants, so validation still
  // runs.
  static constexpr std::string_view kSvQuery = "SELECT * FROM %T WHERE id = %d";
  EXPECT_EQ(
      Query::checked(kSvQuery, "users", 1).renderInsecure(),
      "SELECT * FROM `users` WHERE id = 1");

  static constexpr char kArrQuery[] = "SELECT %C FROM t";
  EXPECT_EQ(
      Query::checked(kArrQuery, "col").renderInsecure(), "SELECT `col` FROM t");
}

TEST_F(QueryTest, CheckedQueryWithValueParameter) {
  // %d for int value parameter in checked mode (original spec set)
  auto q = Query::checked("SELECT * FROM users WHERE id = %d", 42);
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE id = 42");
  EXPECT_TRUE(q.isChecked());
  EXPECT_EQ(q.getQueryFormat(), "SELECT * FROM users WHERE id = %d");
}

TEST_F(QueryTest, CheckedQueryWithStringValue) {
  auto q = Query::checked("SELECT * FROM users WHERE name = %s", "Alice");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE name = \"Alice\"");
}

TEST_F(QueryTest, CheckedQueryWithIdentifierParameter) {
  // %T for table identifier, %C for column identifier in checked mode
  auto q = Query::checked("SELECT * FROM %T WHERE id = 1", "users");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM `users` WHERE id = 1");
  EXPECT_TRUE(q.isChecked());
}

TEST_F(QueryTest, CheckedQueryWithQualifiedIdentifier) {
  // QualifiedColumn tuple should render as backtick-quoted table.column
  auto q =
      Query::checked("SELECT %C FROM %T", QualifiedColumn{"db", "tbl"}, "mydb");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT `db`.`tbl` FROM `mydb`");
}

TEST_F(QueryTest, CheckedQueryWithAliasedIdentifier) {
  // AliasedQualifiedColumn tuple renders with AS
  auto q = Query::checked(
      "SELECT %C FROM users", AliasedQualifiedColumn{"db", "tbl", "alias"});
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT `db`.`tbl` AS `alias` FROM users");
}

TEST_F(QueryTest, CheckedQueryWithListValue) {
  // %Ld with list replaces old list specs, now using original spec set
  auto q = Query::checked(
      "SELECT * FROM users WHERE id IN (%Ld)",
      std::vector<QueryArgument>{1, 2, 3});
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users WHERE id IN (1, 2, 3)");
}

TEST_F(QueryTest, CheckedQueryWithIdentifierList) {
  // %LC for list of identifiers
  auto q = Query::checked(
      "SELECT %LC FROM users",
      std::initializer_list<QueryArgument>{"a", "b", "c"});
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT `a`, `b`, `c` FROM users");
}

TEST_F(QueryTest, CheckedQueryWithEqValueNull) {
  // %=d renders " IS NULL" for null and " = <value>" otherwise.
  auto q1 =
      Query::checked("SELECT * FROM users WHERE %C%=d", "deleted_at", nullptr);
  EXPECT_EQ(
      q1.renderInsecure(), "SELECT * FROM users WHERE `deleted_at` IS NULL");

  auto q2 = Query::checked("SELECT * FROM users WHERE %C%=d", "deleted_at", 42);
  EXPECT_EQ(q2.renderInsecure(), "SELECT * FROM users WHERE `deleted_at` = 42");
}

TEST_F(QueryTest, CheckedQueryWithUpdate) {
  // %U renders comma-separated assignments. folly::dynamic objects iterate in
  // hash (not insertion) order, so assert the checked output exactly matches
  // the legacy renderer on the same object rather than a hardcoded key order.
  folly::dynamic obj = folly::dynamic::object("name", "Bob")("age", 30);
  // checked rejects a bare folly::dynamic; wrap runtime data via fromDynamic.
  auto arg = QueryArgument::fromDynamic(obj);
  auto checked =
      Query::checked("UPDATE %T SET %U WHERE id = %d", "users", arg, 1);
  auto legacy = Query("UPDATE %T SET %U WHERE id = %d", "users", arg, 1);
  EXPECT_EQ(checked.renderInsecure(), legacy.renderInsecure());
  EXPECT_NE(checked.renderInsecure().find(", "), folly::fbstring::npos);
}

TEST_F(QueryTest, CheckedQueryWithWhere) {
  // %W renders AND-separated conditions; assert parity with legacy (see %U note
  // re: folly::dynamic ordering).
  folly::dynamic obj = folly::dynamic::object("active", 1)("name", "Alice");
  auto arg = QueryArgument::fromDynamic(obj);
  auto checked = Query::checked("SELECT * FROM %T WHERE %W", "users", arg);
  auto legacy = Query("SELECT * FROM %T WHERE %W", "users", arg);
  EXPECT_EQ(checked.renderInsecure(), legacy.renderInsecure());
  EXPECT_NE(checked.renderInsecure().find(" AND "), folly::fbstring::npos);
}

TEST_F(QueryTest, CheckedQueryWithValuesMatrix) {
  std::vector<QueryArgument> vals = {
      std::vector<QueryArgument>{1, "a"}, std::vector<QueryArgument>{2, "b"}};
  auto q = Query::checked("INSERT INTO %T VALUES %V", "t", vals);
  EXPECT_EQ(
      q.renderInsecure(), "INSERT INTO `t` VALUES (1, \"a\"), (2, \"b\")");
}

TEST_F(QueryTest, CheckedQueryWithValuesMatrixVectorOfVectors) {
  // A std::vector<std::vector<QueryArgument>> may be passed directly for %V;
  // each inner vector becomes a row and renders identically to the
  // vector<QueryArgument>-of-lists form.
  std::vector<std::vector<QueryArgument>> vals = {{1, "a"}, {2, "b"}};
  auto q = Query::checked("INSERT INTO %T VALUES %V", "t", vals);
  EXPECT_EQ(
      q.renderInsecure(), "INSERT INTO `t` VALUES (1, \"a\"), (2, \"b\")");
}

TEST_F(QueryTest, CheckedQueryWithValueRowMatrix) {
  // A std::vector<ValueRow<Fmt>> supplies a compile-time-schema'd matrix: each
  // row is a bare braced list validated against the schema, and renders like
  // any other %V matrix. Works with both checked and legacy.
  std::vector<ValueRow<"%d %s %f">> rows = {{1, "a", 1.5}, {2, "b", 2.5}};
  auto checked = Query::checked("INSERT INTO %T VALUES %V", "t", rows);
  auto legacy = Query("INSERT INTO %T VALUES %V", "t", rows);
  EXPECT_EQ(
      checked.renderInsecure(),
      "INSERT INTO `t` VALUES (1, \"a\", 1.5), (2, \"b\", 2.5)");
  EXPECT_EQ(checked.renderInsecure(), legacy.renderInsecure());
}

namespace {
// Records which QueryArgument conversion the collection path selects, so the
// tests below can tell the moving path from the copying one. Ref-qualified the
// same way ValueRow is, which is what makes std::vector<CountingCell> a
// QueryArgumentCollection.
struct CountingCell {
  static inline int constConversions = 0;
  static inline int rvalueConversions = 0;

  std::string value;

  explicit CountingCell(std::string v) : value(std::move(v)) {}

  explicit operator QueryArgument() const& {
    ++constConversions;
    return QueryArgument(value);
  }
  explicit operator QueryArgument() && {
    ++rvalueConversions;
    return QueryArgument(std::move(value));
  }

  static void resetCounts() {
    constConversions = 0;
    rvalueConversions = 0;
  }
};

std::vector<CountingCell> makeCountingCells() {
  std::vector<CountingCell> cells;
  cells.reserve(2);
  cells.emplace_back("a");
  cells.emplace_back("b");
  CountingCell::resetCounts();
  return cells;
}
} // namespace

// These two use the legacy ctor because checked()'s %Ls rule requires a
// string-like element type, which a copy-counting stand-in cannot be. Both
// ctors forward their arguments the same way, so they exercise the same
// QueryArgument collection constructor.
TEST_F(QueryTest, QueryCollectionArgumentMovesFromRvalue) {
  // An rvalue collection is owned by the query, so each element is consumed
  // rather than duplicated.
  auto cells = makeCountingCells();
  auto query = Query("WHERE x IN (%Ls)", std::move(cells));
  EXPECT_EQ(query.renderInsecure(), "WHERE x IN (\"a\", \"b\")");
  EXPECT_EQ(CountingCell::rvalueConversions, 2);
  EXPECT_EQ(CountingCell::constConversions, 0);
}

TEST_F(QueryTest, QueryCollectionArgumentCopiesFromLvalue) {
  // An lvalue collection still belongs to the caller, so its elements must be
  // copied and it has to survive the call intact.
  auto cells = makeCountingCells();
  auto first = Query("WHERE x IN (%Ls)", cells);
  EXPECT_EQ(CountingCell::constConversions, 2);
  EXPECT_EQ(CountingCell::rvalueConversions, 0);
  EXPECT_EQ(cells[0].value, "a");
  EXPECT_EQ(cells[1].value, "b");

  auto second = Query("WHERE x IN (%Ls)", cells);
  EXPECT_EQ(first.renderInsecure(), second.renderInsecure());
  EXPECT_EQ(first.renderInsecure(), "WHERE x IN (\"a\", \"b\")");
}

TEST_F(QueryTest, CheckedQueryValueRowMatrixFromRvalue) {
  // The matrix renders the same whether it is moved in or copied, and a
  // matrix passed as an lvalue is left intact for a second use.
  std::vector<ValueRow<"%d %s">> rows = {{1, "a"}, {2, "b"}};
  constexpr std::string_view kExpected =
      "INSERT INTO `t` VALUES (1, \"a\"), (2, \"b\")";

  auto fromLvalue = Query::checked("INSERT INTO %T VALUES %V", "t", rows);
  EXPECT_EQ(fromLvalue.renderInsecure(), kExpected);

  auto reused = Query::checked("INSERT INTO %T VALUES %V", "t", rows);
  EXPECT_EQ(reused.renderInsecure(), kExpected);

  auto fromRvalue =
      Query::checked("INSERT INTO %T VALUES %V", "t", std::move(rows));
  EXPECT_EQ(fromRvalue.renderInsecure(), kExpected);
}

TEST_F(QueryTest, CheckedQueryValueRowMatrixWithSubQueryCell) {
  // A %q cell carries an expression rather than a literal -- here a value that
  // is either a quoted string or SQL NULL, chosen per row at run time. The
  // sub-query is spliced in place of the cell, and the enclosing row keeps its
  // compile-time arity and per-column type checks.
  std::vector<ValueRow<"%d %q">> rows;
  rows.push_back({1, Query::checked("%s", "2026-01-01")});
  rows.push_back({2, Query::checked("NULL")});

  auto checked = Query::checked("INSERT INTO %T VALUES %V", "t", rows);
  auto legacy = Query("INSERT INTO %T VALUES %V", "t", rows);
  EXPECT_EQ(
      checked.renderInsecure(),
      "INSERT INTO `t` VALUES (1, \"2026-01-01\"), (2, NULL)");
  EXPECT_EQ(checked.renderInsecure(), legacy.renderInsecure());
}

TEST_F(QueryTest, CheckedQueryRendersIntegers) {
  // Absolute (not parity) checks: pin %d/%u rendering. %u must render as an
  // unsigned value — a high-bit-set value must not come out negative.
  EXPECT_EQ(
      Query::checked("WHERE id = %d", -5).renderInsecure(), "WHERE id = -5");
  EXPECT_EQ(
      Query::checked("WHERE id = %u", 42u).renderInsecure(), "WHERE id = 42");
  EXPECT_EQ(
      Query::checked("WHERE id = %u", std::numeric_limits<uint64_t>::max())
          .renderInsecure(),
      "WHERE id = 18446744073709551615");
}

TEST_F(QueryTest, CheckedQueryAcceptsSubqueryForStringAndM) {
  // %s and %m accept a sub-Query (rendered as a nested query), matching legacy.
  auto sub = Query("SELECT %d", 1);
  EXPECT_EQ(
      Query::checked("WHERE id IN (%s)", sub).renderInsecure(),
      "WHERE id IN (SELECT 1)");
  EXPECT_EQ(
      Query::checked("WHERE id IN (%m)", sub).renderInsecure(),
      "WHERE id IN (SELECT 1)");
}

TEST_F(QueryTest, CheckedQueryAcceptsPrebuiltQueryArgument) {
  // Mirrors the common migration pattern where helpers return a pre-built,
  // type-erased QueryArgument (e.g. columnName(), fromDynamic()). It's accepted
  // for any specifier and validated/rendered at runtime.
  // Use parentheses: brace-init would prefer the initializer_list ctor and wrap
  // the value in a one-element list.
  QueryArgument col("col");
  QueryArgument val(42);
  EXPECT_EQ(
      Query::checked("SELECT %C WHERE %C = %d", col, col, val).renderInsecure(),
      "SELECT `col` WHERE `col` = 42");

  // A QueryArgument holding a list satisfies a list specifier.
  QueryArgument ids(std::vector<QueryArgument>{1, 2, 3});
  EXPECT_EQ(
      Query::checked("id IN (%Ld)", ids).renderInsecure(), "id IN (1, 2, 3)");
}

TEST_F(QueryTest, CheckedQueryAcceptsQueryArgumentFromDynamic) {
  // A bare folly::dynamic is rejected by checked; runtime-shaped data is passed
  // as an explicit QueryArgument::fromDynamic(). A dynamic array wrapped this
  // way renders as a comma-separated identifier list.
  EXPECT_EQ(
      Query::checked(
          "SELECT %LC FROM t",
          QueryArgument::fromDynamic(folly::dynamic::array("a", "b")))
          .renderInsecure(),
      "SELECT `a`, `b` FROM t");
}

TEST_F(QueryTest, CheckedQueryAcceptsTypedVectorForListSpec) {
  // A homogeneous typed vector renders the same as a vector<QueryArgument>, but
  // its element type is checked at compile time.
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE name IN (%Ls)",
          std::vector<std::string>{"a", "b", "c"})
          .renderInsecure(),
      "SELECT * FROM t WHERE name IN (\"a\", \"b\", \"c\")");
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE id IN (%Ld)", std::vector<int64_t>{1, 2, 3})
          .renderInsecure(),
      "SELECT * FROM t WHERE id IN (1, 2, 3)");
  EXPECT_EQ(
      Query::checked("SELECT %LC FROM t", std::vector<std::string>{"a", "b"})
          .renderInsecure(),
      "SELECT `a`, `b` FROM t");
}

TEST_F(QueryTest, LegacyQueryAcceptsAnyContainerForListSpec) {
  // The container->list conversion lives in the QueryArgument constructor,
  // which the legacy (non-checked) Query path shares, so any collection works
  // there too — it just lacks the compile-time element-type check.
  EXPECT_EQ(
      Query("SELECT * FROM t WHERE id IN (%Ld)", std::set<int64_t>{3, 1, 2})
          .renderInsecure(),
      "SELECT * FROM t WHERE id IN (1, 2, 3)");
  EXPECT_EQ(
      Query(
          "SELECT * FROM t WHERE name IN (%Ls)",
          folly::F14FastSet<std::string>{"only"})
          .renderInsecure(),
      "SELECT * FROM t WHERE name IN (\"only\")");
}

TEST_F(QueryTest, CheckedQueryAcceptsSetForListSpec) {
  // std::set renders its elements in iteration (sorted) order; that determinism
  // lets us assert exact output here.
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE name IN (%Ls)",
          std::set<std::string>{"c", "a", "b"})
          .renderInsecure(),
      "SELECT * FROM t WHERE name IN (\"a\", \"b\", \"c\")");
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE id IN (%Ld)", std::set<int64_t>{3, 1, 2})
          .renderInsecure(),
      "SELECT * FROM t WHERE id IN (1, 2, 3)");
  // unordered_set order is unspecified, so just check a single-element set.
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE id IN (%Ld)", std::unordered_set<int64_t>{7})
          .renderInsecure(),
      "SELECT * FROM t WHERE id IN (7)");
  // Any iterable collection works (e.g. an F14 set), via the generic concept.
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE name IN (%Ls)",
          folly::F14FastSet<std::string>{"only"})
          .renderInsecure(),
      "SELECT * FROM t WHERE name IN (\"only\")");
}

TEST_F(QueryTest, CheckedQueryValidatesTypeErasedArgAtRuntime) {
  // A type-erased argument's value type can't be proven at compile time, so a
  // mismatch against its specifier must still be caught at render time, even
  // though the query is "checked". (The format structure is what compile-time
  // checking proves; the value type is not, for type-erased args.)
  QueryArgument stringArg("not a number");
  auto q = Query::checked("SELECT * FROM t WHERE id = %d", stringArg);
  EXPECT_TRUE(q.isChecked());
  EXPECT_THROW(q.renderInsecure(), std::invalid_argument);

  // Same via QueryArgument::fromDynamic (the explicit type-erased escape
  // hatch).
  auto q2 = Query::checked(
      "SELECT * FROM t WHERE id = %d",
      QueryArgument::fromDynamic(folly::dynamic("str")));
  EXPECT_TRUE(q2.isChecked());
  EXPECT_THROW(q2.renderInsecure(), std::invalid_argument);

  // A correctly-typed type-erased argument still renders fine.
  QueryArgument intArg(42);
  EXPECT_EQ(
      Query::checked("SELECT * FROM t WHERE id = %d", intArg).renderInsecure(),
      "SELECT * FROM t WHERE id = 42");
}

TEST_F(QueryTest, CheckedQueryValidatesEmbeddedNonCheckedSubquery) {
  // A non-checked sub-query embedded in a checked query must be validated by
  // its OWN mode, not the outer query's: checked() proved nothing about the
  // inner query's format. Here the legacy sub-query's format has a dangerous
  // char, which the validating renderer must still reject even though the outer
  // query is checked.
  Query legacy("SELECT `x`");
  EXPECT_FALSE(legacy.isChecked());
  auto q = Query::checked("SELECT %s", legacy);
  EXPECT_THROW(q.renderInsecure(), std::invalid_argument);

  // A checked sub-query embedded in a checked query renders normally.
  auto inner = Query::checked("SELECT %d", 7);
  EXPECT_EQ(
      Query::checked("SELECT %s", inner).renderInsecure(), "SELECT SELECT 7");
}

TEST_F(QueryTest, CheckedQueryRendersEnumAsInteger) {
  // An enum passed to %d (or %Ld) renders as its underlying integer value, with
  // no static_cast required at the call site.
  using checked_compile_tests::CheckedTestEnum;
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE status = %d", CheckedTestEnum::Second)
          .renderInsecure(),
      "SELECT * FROM t WHERE status = 7");
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE status IN (%Ld)",
          std::vector<CheckedTestEnum>{
              CheckedTestEnum::First, CheckedTestEnum::Second})
          .renderInsecure(),
      "SELECT * FROM t WHERE status IN (1, 7)");
}

TEST_F(QueryTest, CheckedQueryAcceptsPairVectorForU) {
  // %U now accepts a prebuilt pair vector (no cast / no folly::dynamic needed),
  // both the fbstring-keyed (ArgumentPair) and std::string-keyed forms.
  std::vector<ArgumentPair> fbPairs;
  fbPairs.emplace_back("a", QueryArgument(1));
  fbPairs.emplace_back("b", QueryArgument("x"));
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", std::move(fbPairs))
          .renderInsecure(),
      "UPDATE `t` SET `a` = 1, `b` = \"x\"");

  std::vector<std::pair<std::string, QueryArgument>> strPairs{
      {"a", 1}, {"b", "x"}};
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", strPairs).renderInsecure(),
      "UPDATE `t` SET `a` = 1, `b` = \"x\"");
}

TEST_F(QueryTest, CheckedQueryAcceptsMapForPairList) {
  // An associative map is accepted for %U/%W. std::map iterates in sorted key
  // order, so it renders identically to the equivalent sorted
  // vector<ArgumentPair> -- proving the map path matches the established
  // pair-vector path. Maps enable lookup-and-replace by column before building.
  std::map<std::string, QueryArgument> m{{"b", "x"}, {"a", 1}};
  m["a"] = QueryArgument(2); // replace an existing column by key
  m["c"] = QueryArgument(3); // add another
  std::vector<ArgumentPair> expected;
  expected.emplace_back("a", QueryArgument(2));
  expected.emplace_back("b", QueryArgument("x"));
  expected.emplace_back("c", QueryArgument(3));
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", m).renderInsecure(),
      Query::checked("UPDATE %T SET %U", "t", expected).renderInsecure());

  std::map<std::string, QueryArgument> where{{"id", 5}, {"name", "y"}};
  std::vector<ArgumentPair> expectedWhere;
  expectedWhere.emplace_back("id", QueryArgument(5));
  expectedWhere.emplace_back("name", QueryArgument("y"));
  EXPECT_EQ(
      Query::checked("SELECT * FROM %T WHERE %W", "t", where).renderInsecure(),
      Query::checked("SELECT * FROM %T WHERE %W", "t", expectedWhere)
          .renderInsecure());

  // An unordered_map renders the same assignments; use a single element so the
  // result is independent of hash order.
  std::unordered_map<std::string, QueryArgument> single{{"only", 9}};
  std::vector<ArgumentPair> expectedSingle;
  expectedSingle.emplace_back("only", QueryArgument(9));
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", single).renderInsecure(),
      Query::checked("UPDATE %T SET %U", "t", expectedSingle).renderInsecure());
}

TEST_F(QueryTest, CheckedQueryPairListAcceptsOptionalValue) {
  // The pair value is a QueryArgument, so an optional works: engaged -> value,
  // empty -> NULL (and for %U, an empty value renders `col = NULL`).
  std::vector<ArgumentPair> pairs;
  pairs.emplace_back("a", std::optional<int>{5});
  pairs.emplace_back("b", std::optional<int>{});
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", std::move(pairs))
          .renderInsecure(),
      "UPDATE `t` SET `a` = 5, `b` = NULL");
}

TEST_F(QueryTest, CheckedQueryOptionalStringLikeRendersAsString) {
  // Regression: the optional ctor must route string-like values through the
  // dedicated scalar ctors. optional<const char*> in particular must render as
  // a string, not bool (1), which is what a raw variant assignment would do.
  EXPECT_EQ(
      Query::checked("SELECT %s", std::optional<const char*>{"x"})
          .renderInsecure(),
      "SELECT \"x\"");
  EXPECT_EQ(
      Query::checked("SELECT %s", std::optional<std::string_view>{"y"})
          .renderInsecure(),
      "SELECT \"y\"");
  EXPECT_EQ(
      Query::checked("SELECT %s", std::optional<folly::StringPiece>{"z"})
          .renderInsecure(),
      "SELECT \"z\"");
  // nullopt still renders as NULL.
  EXPECT_EQ(
      Query::checked("SELECT %s", std::optional<const char*>{})
          .renderInsecure(),
      "SELECT NULL");
}

TEST_F(QueryTest, CheckedQueryUnsignedHighBitRoundTrip) {
  // uint64_t is stored as int64_t (the variant has no uint64_t alternative), so
  // a high-bit value is stored negative. %u casts back to uint64_t on render,
  // so it round-trips correctly...
  constexpr auto kMax = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(
      Query::checked("SELECT %u", kMax).renderInsecure(),
      "SELECT 18446744073709551615");
  EXPECT_EQ(
      Query::checked("SELECT %u", std::optional<uint64_t>{kMax})
          .renderInsecure(),
      "SELECT 18446744073709551615");
  // ...while %d is signed by design: the same bits render negative. Callers
  // must use %u for unsigned values >= 2^63.
  EXPECT_EQ(Query::checked("SELECT %d", kMax).renderInsecure(), "SELECT -1");
}

TEST_F(QueryTest, DynamicBoolRendersAsInt) {
  // MySQL has no boolean type, and QueryArgument has no bool alternative: a
  // folly::dynamic bool is stored as int64_t, so it renders 0/1 and is accepted
  // wherever an int is (e.g. %d), not just %m. (folly::dynamic goes through the
  // legacy Query() ctor; checked() does not accept a bare dynamic.)
  folly::dynamic dynTrue = true;
  folly::dynamic dynFalse = false;
  EXPECT_EQ(Query("SELECT %d", dynTrue).renderInsecure(), "SELECT 1");
  EXPECT_EQ(Query("SELECT %m", dynFalse).renderInsecure(), "SELECT 0");
}

TEST_F(QueryTest, OptionalBoolRendersAsInt) {
  // There is no dedicated optional<bool> ctor: bool is integral, so the generic
  // optional ctor delegates to the integral scalar ctor and stores int64_t.
  // These pin that an engaged optional<bool> still renders 0/1 (not "true"/
  // "false", and not a string) and that nullopt is still NULL.
  EXPECT_EQ(
      Query::checked("SELECT %d", std::optional<bool>{true}).renderInsecure(),
      "SELECT 1");
  EXPECT_EQ(
      Query::checked("SELECT %d", std::optional<bool>{false}).renderInsecure(),
      "SELECT 0");
  EXPECT_EQ(
      Query::checked("SELECT %m", std::optional<bool>{true}).renderInsecure(),
      "SELECT 1");
  EXPECT_EQ(
      Query::checked("SELECT %d", std::optional<bool>{}).renderInsecure(),
      "SELECT NULL");

  EXPECT_EQ(
      Query::checked("SELECT %d", folly::Optional<bool>{true}).renderInsecure(),
      "SELECT 1");
  EXPECT_EQ(
      Query::checked("SELECT %d", folly::Optional<bool>{false})
          .renderInsecure(),
      "SELECT 0");
  EXPECT_EQ(
      Query::checked("SELECT %m", folly::Optional<bool>{false})
          .renderInsecure(),
      "SELECT 0");
  EXPECT_EQ(
      Query::checked("SELECT %d", folly::Optional<bool>{}).renderInsecure(),
      "SELECT NULL");

  // The legacy ctor takes the same path.
  EXPECT_EQ(
      Query("SELECT %d", std::optional<bool>{true}).renderInsecure(),
      "SELECT 1");
  EXPECT_EQ(
      Query("SELECT %d", folly::Optional<bool>{false}).renderInsecure(),
      "SELECT 0");
}

TEST_F(QueryTest, CheckedQueryAcceptsConstexprStringPiece) {
  // A compile-time-constant folly::StringPiece works as the format string,
  // validated at compile time like a literal / constexpr std::string_view.
  constexpr folly::StringPiece kFmt = "SELECT * FROM %T WHERE id = %d";
  EXPECT_EQ(
      Query::checked(kFmt, "users", 5).renderInsecure(),
      "SELECT * FROM `users` WHERE id = 5");
}

TEST_F(QueryTest, PairRangeOfPlainStringsIsAPairList) {
  // Regression: a std::pair of two string-like members converts to
  // QualifiedColumn (std::tuple<fbstring, fbstring>), so a map<string, string>
  // is QueryArgument-constructible element-wise and used to satisfy
  // QueryArgumentCollection as well as PairRange -- making the two constrained
  // ctors ambiguous and any such call a hard compile error. A range of
  // std::pair is now excluded from the collection concept, so it resolves
  // unambiguously as a pair list. std::map iterates in key order.
  std::map<std::string, std::string> update{{"a", "1"}, {"b", "2"}};
  EXPECT_EQ(
      Query::checked("UPDATE %T SET %U", "t", update).renderInsecure(),
      "UPDATE `t` SET `a` = \"1\", `b` = \"2\"");
  EXPECT_EQ(
      Query::checked("SELECT * FROM %T WHERE %W", "t", update).renderInsecure(),
      "SELECT * FROM `t` WHERE `a` = \"1\" AND `b` = \"2\"");
  // The legacy ctor resolves the same way.
  EXPECT_EQ(
      Query("UPDATE %T SET %U", "t", update).renderInsecure(),
      "UPDATE `t` SET `a` = \"1\", `b` = \"2\"");
}

TEST_F(QueryTest, CheckedQueryRendersSubQueryForPercentQ) {
  auto sub = Query::checked("SELECT id FROM %T WHERE x = %d", "inner", 5);

  // %q splices the sub-query in place, unquoted.
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%q) t", sub).renderInsecure(),
      "SELECT * FROM (SELECT id FROM `inner` WHERE x = 5) t");

  // %s accepts a Query too and renders identically -- but it also accepts a
  // plain string, which renders as a quoted literal. That ambiguity is the
  // reason %q exists.
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%s) t", sub).renderInsecure(),
      "SELECT * FROM (SELECT id FROM `inner` WHERE x = 5) t");
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%s) t", "inner").renderInsecure(),
      "SELECT * FROM (\"inner\") t");

  // Nesting works: the inner query is rendered with its own validation.
  auto outer = Query::checked("SELECT * FROM (%q) t", sub);
  EXPECT_EQ(
      Query::checked("SELECT count(*) FROM (%q) u", outer).renderInsecure(),
      "SELECT count(*) FROM (SELECT * FROM "
      "(SELECT id FROM `inner` WHERE x = 5) t) u");

  // The legacy constructor reaches the same renderer branch.
  EXPECT_EQ(
      Query("SELECT * FROM (%q) t", sub).renderInsecure(),
      "SELECT * FROM (SELECT id FROM `inner` WHERE x = 5) t");
}

TEST_F(QueryTest, PercentQRejectsNonQueryAtRenderTime) {
  // A type-erased QueryArgument passes the compile-time check for any
  // specifier, so %q has to reject a non-Query when it renders. The legacy
  // constructor has no compile-time check at all, so the same guard is what
  // stops an arbitrary argument reaching renderSubQuery there.
  auto expectRejects = [](auto&& query, std::string_view typeName) {
    try {
      query.renderInsecure();
      FAIL() << "expected a parse error for " << typeName;
    } catch (const std::invalid_argument& e) {
      EXPECT_NE(
          std::string(e.what()).find(
              fmt::format(
                  "invalid value type {} for format string %q", typeName)),
          std::string::npos)
          << "actual: " << e.what();
    }
  };

  expectRejects(
      Query::checked("SELECT * FROM (%q) t", QueryArgument("oops")), "string");
  expectRejects(
      Query::checked("SELECT * FROM (%q) t", QueryArgument(7)), "int64_t");
  // No compile-time net on the legacy path -- this is the case that needs it.
  expectRejects(Query("SELECT * FROM (%q) t", 7), "int64_t");

  // A null is rejected too: unlike every other value specifier, %q has no NULL
  // rendering. It gets its own message rather than the type error above,
  // because this is the case %LQ accepted and so the one a migration hits.
  for (auto&& q : {
           Query::checked("SELECT * FROM (%q) t", QueryArgument(nullptr)),
           Query("SELECT * FROM (%q) t", QueryArgument(nullptr)),
       }) {
    try {
      q.renderInsecure();
      FAIL() << "expected a parse error for a NULL sub-query";
    } catch (const std::invalid_argument& e) {
      EXPECT_NE(
          std::string(e.what()).find("a sub-query position cannot be NULL"),
          std::string::npos)
          << "actual: " << e.what();
    }
  }
}

TEST_F(QueryTest, PercentQDelegatesValidationToTheSubQuery) {
  // %q constrains the argument's type, not its contents. What happens to the
  // sub-query's text depends on how that sub-query was built.

  // Legacy but clean: scanned as it renders, and renders.
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%q) t", Query("SELECT 1"))
          .renderInsecure(),
      "SELECT * FROM (SELECT 1) t");

  // Legacy with a dangerous character: the sub-query's own scan rejects it,
  // even though the enclosing query is checked.
  EXPECT_THROW(
      Query::checked("SELECT * FROM (%q) t", Query("SELECT 'x'"))
          .renderInsecure(),
      std::invalid_argument);

  // Built with Query::unsafe: spliced verbatim, in both modes. %q could refuse
  // this -- it has the Query in hand -- but does not, so %Q -> %q conversion
  // stays mechanical. Asserted alongside %Q and %s, which do the same.
  const auto unsafeSub = Query::unsafe("SELECT 'x'; --");
  const std::string spliced = "SELECT * FROM (SELECT 'x'; --) t";
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%q) t", unsafeSub).renderInsecure(),
      spliced);
  EXPECT_EQ(Query("SELECT * FROM (%q) t", unsafeSub).renderInsecure(), spliced);
  EXPECT_EQ(Query("SELECT * FROM (%Q) t", unsafeSub).renderInsecure(), spliced);
  EXPECT_EQ(
      Query::checked("SELECT * FROM (%s) t", unsafeSub).renderInsecure(),
      spliced);

  // Why refusing an unsafe sub-query at %q would not mean anything: the
  // intervening query embeds the unsafe one through %s, which never reaches
  // appendSubQuery, so a check there sees only the (safe) middle query. Note
  // the inner query must use %s rather than %q -- %q at any depth routes
  // through the same handler, so it would not demonstrate the hole.
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM (%q) t", Query("SELECT * FROM (%s) u", unsafeSub))
          .renderInsecure(),
      "SELECT * FROM (SELECT * FROM (SELECT 'x'; --) u) t");
}

TEST_F(QueryTest, PercentLqRendersAListOfSubQueries) {
  // The %LQ shape this replaces: a comma-joined list of SQL fragments, as an
  // ON DUPLICATE KEY UPDATE clause needs.
  std::vector<Query> updates{
      Query::checked("%C=VALUES(%C)", "a", "a"),
      Query::checked("%C=VALUES(%C)", "b", "b")};
  EXPECT_EQ(
      Query::checked(
          "INSERT INTO %T VALUES (1) ON DUPLICATE KEY UPDATE %Lq", "t", updates)
          .renderInsecure(),
      "INSERT INTO `t` VALUES (1) ON DUPLICATE KEY UPDATE "
      "`a`=VALUES(`a`), `b`=VALUES(`b`)");

  // Same rendering as the %LQ it replaces. Pinned to a literal rather than
  // just comparing the two, which would also hold if both were wrong.
  std::vector<QueryArgument> erased{updates[0], updates[1]};
  const std::string expected = "UPDATE t SET `a`=VALUES(`a`), `b`=VALUES(`b`)";
  EXPECT_EQ(
      Query("UPDATE t SET %LQ", QueryArgument(erased)).renderInsecure(),
      expected);
  EXPECT_EQ(
      Query("UPDATE t SET %Lq", QueryArgument(erased)).renderInsecure(),
      expected);

  // Elements are type-checked at render time when the list is type-erased.
  std::vector<QueryArgument> bad{Query::checked("SELECT 1"), QueryArgument(7)};
  EXPECT_THROW(
      Query::checked("UPDATE t SET %Lq", QueryArgument(bad)).renderInsecure(),
      std::invalid_argument);

  // A null element is where %Lq and %LQ part company: %LQ renders NULL, %q
  // has no NULL form. Worth knowing before converting a %LQ site.
  std::vector<QueryArgument> withNull{
      Query("SELECT 1"), QueryArgument(nullptr)};
  EXPECT_EQ(
      Query("SELECT %LQ", QueryArgument(withNull)).renderInsecure(),
      "SELECT SELECT 1, NULL");
  EXPECT_THROW(
      Query("SELECT %Lq", QueryArgument(withNull)).renderInsecure(),
      std::invalid_argument);

  // An empty list renders as nothing at all, leaving a dangling clause for the
  // server to reject. Same as %Ls and %LQ, but the motivating use case builds
  // this list at runtime, so callers need their own guard.
  EXPECT_EQ(
      Query::checked("UPDATE t SET %Lq", std::vector<Query>{}).renderInsecure(),
      "UPDATE t SET ");
}

TEST_F(QueryTest, PercentHRendersBinaryLiteral) {
  EXPECT_EQ(
      Query::checked("SELECT %h", std::string("A")).renderInsecure(),
      "SELECT X'41'");
  EXPECT_EQ(
      Query::checked("SELECT %h", std::string("AB")).renderInsecure(),
      "SELECT X'4142'");

  // Arbitrary bytes, including a high byte and an embedded NUL -- the reason
  // this cannot be done by quoting a string.
  EXPECT_EQ(
      Query::checked("SELECT %h", std::string("\x00\x80\xff", 3))
          .renderInsecure(),
      "SELECT X'0080ff'");

  // An empty value stays in the X'' form, which MySQL accepts as an empty
  // binary string. Rendering a bare '' instead would silently switch the
  // literal from binary to a character string in the connection charset.
  EXPECT_EQ(
      Query::checked("SELECT %h", std::string()).renderInsecure(),
      "SELECT X''");

  // Single-quoted, unlike %s, which double-quotes its value.
  EXPECT_EQ(
      Query::checked("SELECT %s", std::string("A")).renderInsecure(),
      "SELECT \"A\"");

  // Nulls render as NULL, matching %s.
  EXPECT_EQ(
      Query::checked("SELECT %h", nullptr).renderInsecure(), "SELECT NULL");
  EXPECT_EQ(
      Query::checked("SELECT %h", std::optional<std::string>())
          .renderInsecure(),
      "SELECT NULL");

  // The renderer is shared, so the legacy constructor gets %h too.
  EXPECT_EQ(
      Query("SELECT %h", std::string("A")).renderInsecure(), "SELECT X'41'");
}

TEST_F(QueryTest, PercentLhRendersAListOfBinaryLiterals) {
  EXPECT_EQ(
      Query::checked(
          "SELECT * FROM t WHERE k IN (%Lh)",
          std::vector<std::string>{"A", "B"})
          .renderInsecure(),
      "SELECT * FROM t WHERE k IN (X'41', X'42')");

  // Single element, and an empty list, which renders nothing at all -- the
  // same dangling-clause hazard every %L form has.
  EXPECT_EQ(
      Query::checked("SELECT %Lh", std::vector<std::string>{"A"})
          .renderInsecure(),
      "SELECT X'41'");
  EXPECT_EQ(
      Query::checked("SELECT %Lh", std::vector<std::string>{}).renderInsecure(),
      "SELECT ");

  // A null element renders NULL, matching %Ls rather than %Lq (which throws).
  // An empty element stays in the X'' form, like the scalar.
  std::vector<QueryArgument> mixed{
      std::string("A"), QueryArgument(nullptr), std::string()};
  EXPECT_EQ(
      Query::checked("SELECT %Lh", QueryArgument(mixed)).renderInsecure(),
      "SELECT X'41', NULL, X''");
}

TEST_F(QueryTest, PercentEqualsHComparesAgainstBinaryLiteral) {
  EXPECT_EQ(
      Query::checked("SELECT * FROM t WHERE %C%=h", "k", std::string("A"))
          .renderInsecure(),
      "SELECT * FROM t WHERE `k` = X'41'");

  // Null takes the IS NULL form, like the other %= specifiers, rather than
  // the NULL that scalar %h renders.
  EXPECT_EQ(
      Query::checked("SELECT * FROM t WHERE %C%=h", "k", nullptr)
          .renderInsecure(),
      "SELECT * FROM t WHERE `k` IS NULL");

  // An empty value keeps the X'' form here too.
  EXPECT_EQ(
      Query::checked("SELECT * FROM t WHERE %C%=h", "k", std::string())
          .renderInsecure(),
      "SELECT * FROM t WHERE `k` = X''");
}

TEST_F(QueryTest, PercentHRejectsNonStringAtRenderTime) {
  // A type-erased QueryArgument passes the compile-time check for every
  // specifier, so %h has to reject a non-string when it renders.
  try {
    Query::checked("SELECT %h", QueryArgument(7)).renderInsecure();
    FAIL() << "expected a parse error for an int at %h";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(
        std::string(e.what()).find(
            "invalid value type int64_t for format string %h"),
        std::string::npos)
        << "actual: " << e.what();
  }
}

TEST_F(QueryTest, PercentHRejectsNonStringInEveryForm) {
  // The scalar case is covered above; the list and comparison forms route
  // through the same check and must reject too.
  EXPECT_THROW(
      Query::checked("SELECT %Lh", QueryArgument(std::vector<QueryArgument>{7}))
          .renderInsecure(),
      std::invalid_argument);
  EXPECT_THROW(
      Query::checked("SELECT * FROM t WHERE %C%=h", "k", QueryArgument(7))
          .renderInsecure(),
      std::invalid_argument);

  // A default-constructed QueryArgument holds an empty pair list, not a null
  // and not an empty string, so it must take the type-error path rather than
  // rendering X''.
  try {
    Query::checked("SELECT %h", QueryArgument()).renderInsecure();
    FAIL() << "expected a parse error for a default-constructed QueryArgument";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(
        std::string(e.what()).find("for format string %h"), std::string::npos)
        << "actual: " << e.what();
  }

  // %=h through the legacy constructor, which shares the renderer.
  EXPECT_EQ(
      Query("SELECT * FROM t WHERE %C%=h", "k", std::string("A"))
          .renderInsecure(),
      "SELECT * FROM t WHERE `k` = X'41'");
}

TEST_F(QueryTest, PercentHIsIndependentOfEscapeMode) {
  // Hex output is [0-9a-f], so there is nothing to escape and no connection to
  // consult. renderInsecure() therefore matches what is sent to the server.
  const auto query = Query::checked("SELECT %h", std::string("\x00\xff", 2));
  EXPECT_EQ(query.renderInsecure(), "SELECT X'00ff'");
  EXPECT_EQ(query.render(nullptr), "SELECT X'00ff'");
}

TEST_F(QueryTest, UnknownListSubTypeIsRejectedOnTheLegacyPath) {
  // checked() has always rejected these at compile time; the legacy path let
  // them through to the value renderer, which ignores the sub-type char for
  // sub-query and NULL elements. So %Lj rendered a list of sub-queries.
  std::vector<QueryArgument> queries{Query("SELECT 1"), Query("SELECT 2")};
  EXPECT_THROW(
      Query("SELECT %Lj", QueryArgument(queries)).renderInsecure(),
      std::invalid_argument);

  std::vector<QueryArgument> nulls{QueryArgument(nullptr)};
  EXPECT_THROW(
      Query("SELECT %Lj", QueryArgument(nulls)).renderInsecure(),
      std::invalid_argument);

  // The sub-type is rejected before the argument is inspected, so the shape of
  // the argument does not matter.
  EXPECT_THROW(
      Query("SELECT %Lj", std::string("not a list")).renderInsecure(),
      std::invalid_argument);

  try {
    Query("SELECT %Lz", QueryArgument(queries)).renderInsecure();
    FAIL() << "expected a parse error for an unknown %L sub-type";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(
        std::string(e.what()).find("unknown %L sub-type"), std::string::npos)
        << "actual: " << e.what();
  }
}

TEST_F(QueryTest, KnownListSubTypesStillRender) {
  // Every sub-type the legacy renderer supports must survive the new
  // allowlist. %LQ in particular has real call sites and is only reachable
  // through the value renderer's sub-query branch.
  std::vector<QueryArgument> queries{Query("SELECT 1"), Query("SELECT 2")};
  EXPECT_EQ(
      Query("SELECT %LQ", QueryArgument(queries)).renderInsecure(),
      "SELECT SELECT 1, SELECT 2");
  // %LQ with a null element keeps rendering NULL via the same branch.
  std::vector<QueryArgument> withNull{
      Query("SELECT 1"), QueryArgument(nullptr)};
  EXPECT_EQ(
      Query("SELECT %LQ", QueryArgument(withNull)).renderInsecure(),
      "SELECT SELECT 1, NULL");

  EXPECT_EQ(
      Query("SELECT %Ls", std::vector<std::string>{"a", "b"}).renderInsecure(),
      "SELECT \"a\", \"b\"");
  EXPECT_EQ(
      Query("SELECT %Ld", std::vector<int>{1, 2}).renderInsecure(),
      "SELECT 1, 2");
  EXPECT_EQ(
      Query("SELECT %Lu", std::vector<int>{1, 2}).renderInsecure(),
      "SELECT 1, 2");
  EXPECT_EQ(
      Query("SELECT %Lm", std::vector<std::string>{"a"}).renderInsecure(),
      "SELECT \"a\"");
  EXPECT_EQ(
      Query("SELECT %LC", std::vector<std::string>{"a", "b"}).renderInsecure(),
      "SELECT `a`, `b`");
  EXPECT_EQ(
      Query("SELECT %Lq", QueryArgument(queries)).renderInsecure(),
      "SELECT SELECT 1, SELECT 2");
  EXPECT_EQ(
      Query("SELECT %Lh", std::vector<std::string>{"A"}).renderInsecure(),
      "SELECT X'41'");
  EXPECT_EQ(
      Query("SELECT %Lf", std::vector<double>{1.5}).renderInsecure(),
      "SELECT 1.5");
}

TEST_F(QueryTest, QueryAcceptsMutableCharArray) {
  // Regression: a non-const char[] argument must be treated as a string, not a
  // QueryArgumentCollection. decay_t<char[N]> is char* (not const char*), so
  // before is_string_like_v covered char* it slipped into the collection
  // constructor and called .size() on a C array -- a hard compile error for any
  // legacy caller passing a mutable char buffer.
  char buf[20] = "hello";
  EXPECT_EQ(
      Query("SELECT %s", buf).renderInsecure(),
      Query("SELECT %s", std::string("hello")).renderInsecure());
  EXPECT_EQ(
      Query::checked("SELECT %s", buf).renderInsecure(),
      Query::checked("SELECT %s", std::string("hello")).renderInsecure());
}

TEST_F(QueryTest, CheckedQueryRendersAggregateColumn) {
  // Exercises resolveAggregateFunctionName via an aggregate column identifier.
  auto q = Query::checked(
      "SELECT %C FROM t",
      AggregateColumn{AggregateFunction::COUNT, {"db", "tbl"}});
  EXPECT_EQ(q.renderInsecure(), "SELECT COUNT(`db`.`tbl`) FROM t");
}

TEST_F(QueryTest, CheckedQueryValuesMatrixRejectsNonListRow) {
  // A %V whose rows are not lists must produce a clean parse error, not an
  // opaque std::bad_variant_access from getList().
  auto q = Query::checked(
      "INSERT INTO %T VALUES %V", "t", std::vector<QueryArgument>{1, 2});
  EXPECT_THROW(q.renderInsecure(), std::invalid_argument);
}

TEST_F(QueryTest, CheckedQueryWithComment) {
  auto q = Query::checked("SELECT * FROM users %K", "test comment");
  auto rendered = q.renderInsecure();
  EXPECT_EQ(rendered, "SELECT * FROM users /*test comment*/");
}

TEST_F(QueryTest, CheckedQueryGetFormatPreservesSimplifiedSpec) {
  auto q = Query::checked("SELECT %C FROM %T WHERE %C = %d", "a", "b", "c", 1);
  // getQueryFormat should return original spec set as provided
  EXPECT_EQ(q.getQueryFormat(), "SELECT %C FROM %T WHERE %C = %d");
  EXPECT_TRUE(q.isChecked());
}

TEST_F(QueryTest, CheckedQueryIsNotUnsafe) {
  auto q = Query::checked("SELECT * FROM %T WHERE id = %d", "users", 1);
  EXPECT_FALSE(q.isUnsafe());
  EXPECT_TRUE(q.isChecked());
}

// =============================================================================
// Checked-vs-legacy render parity
//
// Checked and legacy queries run through the same renderer; the only
// difference is that a checked query passes validate=false, skipping the
// dangerous-character scan its format string was already proven to pass. These
// tests pin that the two produce byte-identical output for the same format
// string and arguments across every specifier.
// =============================================================================

// Renders the same literal + args through legacy Query(...) and checked
// Query::checked(...) and asserts identical output. The format must be a
// literal (Query::checked requires it), so the literal is expanded into both
// calls.
#define EXPECT_RENDER_PARITY(fmt, ...)                        \
  EXPECT_EQ(                                                  \
      Query(fmt __VA_OPT__(, ) __VA_ARGS__).renderInsecure(), \
      Query::checked(fmt __VA_OPT__(, ) __VA_ARGS__).renderInsecure())

#define EXPECT_PARTIAL_PARITY(fmt, ...)                               \
  EXPECT_EQ(                                                          \
      Query(fmt __VA_OPT__(, ) __VA_ARGS__).renderPartiallyEscaped(), \
      Query::checked(fmt __VA_OPT__(, ) __VA_ARGS__).renderPartiallyEscaped())

TEST_F(QueryTest, CheckedRenderMatchesLegacy) {
  // Values.
  EXPECT_RENDER_PARITY("SELECT * FROM users");
  EXPECT_RENDER_PARITY("WHERE id = %d", 42);
  EXPECT_RENDER_PARITY("WHERE id = %d", -7);
  EXPECT_RENDER_PARITY("WHERE b = %d", true);
  EXPECT_RENDER_PARITY("WHERE x = %f", 19.99);
  EXPECT_RENDER_PARITY("WHERE x = %m", 5);
  EXPECT_RENDER_PARITY("WHERE x = %m", "str");
  EXPECT_RENDER_PARITY("WHERE x = %s", nullptr);
  // Note: checked and legacy share one renderer template (they differ only in
  // elided runtime checks), so these parity assertions guard *dispatch*, not
  // rendering — a shared rendering bug would cancel out on both sides.
  // Rendering correctness is pinned by the absolute-value tests below (e.g.
  // CheckedQueryRendersIntegers, CheckedQueryWith*).
  EXPECT_RENDER_PARITY("WHERE id = %u", std::numeric_limits<uint64_t>::max());

  // Identifiers, including qualified/aliased column tuples.
  EXPECT_RENDER_PARITY("SELECT * FROM %T", "users");
  EXPECT_RENDER_PARITY("SELECT %C FROM t", "col");
  EXPECT_RENDER_PARITY("SELECT %C FROM t", QualifiedColumn{"db", "tbl"});
  EXPECT_RENDER_PARITY(
      "SELECT %C FROM t", AliasedQualifiedColumn{"db", "tbl", "a"});

  // (List and values-matrix specifiers are covered separately in
  // CheckedRenderMatchesLegacyForLists — a bare std::vector<QueryArgument>
  // passed to the legacy Query(...) binds to its all-params constructor, so the
  // two sides can't share one expression.)

  // Pair lists (single key to keep folly::dynamic iteration order moot) plus
  // %=/IS NULL via %W. checked rejects a bare folly::dynamic, so the runtime
  // data is wrapped in QueryArgument::fromDynamic() (accepted by both checked
  // and legacy, so parity still holds).
  EXPECT_RENDER_PARITY(
      "UPDATE %T SET %U WHERE id = %d",
      "users",
      QueryArgument::fromDynamic(folly::dynamic::object("name", "Bob")),
      1);
  EXPECT_RENDER_PARITY(
      "WHERE %W", QueryArgument::fromDynamic(folly::dynamic::object("a", 1)));
  EXPECT_RENDER_PARITY(
      "WHERE %W",
      QueryArgument::fromDynamic(folly::dynamic::object("a", nullptr)));
  EXPECT_RENDER_PARITY(
      "WHERE %LO", QueryArgument::fromDynamic(folly::dynamic::object("a", 1)));
  EXPECT_RENDER_PARITY(
      "WHERE %LA", QueryArgument::fromDynamic(folly::dynamic::object("a", 1)));

  // Comment (terminator neutralization), literal percent, and %= both branches.
  EXPECT_RENDER_PARITY("SELECT * FROM t %K", "hint */ DROP");
  EXPECT_RENDER_PARITY("SELECT 50%% done");
  EXPECT_RENDER_PARITY("WHERE %C %=d", "x", nullptr);
  EXPECT_RENDER_PARITY("WHERE %C %=d", "x", 42);

  // Subquery rendered as a value (exercises the recursive render path).
  EXPECT_RENDER_PARITY("SELECT * FROM (%m) sub", Query("SELECT %d", 1));

  // Escaping parity (renderInsecure does no escaping; use partial-escape mode).
  EXPECT_PARTIAL_PARITY("WHERE name = %s", "O'Brien\n\"x\"");
}

TEST_F(QueryTest, CheckedRenderMatchesLegacyForLists) {
  // For list/matrix specifiers the legacy side must wrap the single list
  // argument inside the all-params vector (a bare std::vector<QueryArgument>
  // would otherwise be taken as the entire parameter list); Query::checked
  // takes it directly. Both feed the identical QueryArgument to the shared
  // renderer.
  auto legacyOneArg = [](const char* fmt, QueryArgument arg) {
    return Query(fmt, std::vector<QueryArgument>{std::move(arg)})
        .renderInsecure();
  };

  std::vector<QueryArgument> ids{1, 2, 3};
  EXPECT_EQ(
      legacyOneArg("WHERE id IN (%Ld)", ids),
      Query::checked("WHERE id IN (%Ld)", ids).renderInsecure());

  std::vector<QueryArgument> empty{};
  EXPECT_EQ(
      legacyOneArg("WHERE id IN (%Ld)", empty),
      Query::checked("WHERE id IN (%Ld)", empty).renderInsecure());

  std::vector<QueryArgument> cols{"a", "b"};
  EXPECT_EQ(
      legacyOneArg("SELECT %LC FROM t", cols),
      Query::checked("SELECT %LC FROM t", cols).renderInsecure());

  std::vector<QueryArgument> matrix{
      std::vector<QueryArgument>{1, "a"}, std::vector<QueryArgument>{2, "b"}};
  EXPECT_EQ(
      Query(
          "INSERT INTO %T VALUES %V",
          std::vector<QueryArgument>{QueryArgument("t"), QueryArgument(matrix)})
          .renderInsecure(),
      Query::checked("INSERT INTO %T VALUES %V", "t", matrix).renderInsecure());
}

TEST_F(QueryTest, CheckedTruncationOverloadsDispatchToCheckedRenderer) {
  // Exercises the truncating renderInsecureFb(maxSize) /
  // renderPartiallyEscapedFb(maxSize) overloads on a checked query — previously
  // these silently used the legacy renderer. Equality with the legacy query
  // confirms correct dispatch (and the same for the std::string variants).
  auto legacy = Query("SELECT * FROM %T WHERE id = %d", "users", 12345);
  auto checked =
      Query::checked("SELECT * FROM %T WHERE id = %d", "users", 12345);
  EXPECT_EQ(legacy.renderInsecureFb(), checked.renderInsecureFb());
  EXPECT_EQ(legacy.renderInsecureFb(10), checked.renderInsecureFb(10));
  EXPECT_EQ(
      legacy.renderPartiallyEscapedFb(10),
      checked.renderPartiallyEscapedFb(10));
  EXPECT_EQ(
      legacy.renderPartiallyEscapedStr(10),
      checked.renderPartiallyEscapedStr(10));
}

TEST_F(QueryTest, CheckedQueryCopyMovePreservesRendering) {
  auto q = Query::checked("SELECT * FROM %T WHERE id = %d", "users", 1);
  const auto expected = q.renderInsecure();

  Query copy = q;
  EXPECT_TRUE(copy.isChecked());
  EXPECT_EQ(copy.renderInsecure(), expected);

  Query moved = std::move(q);
  EXPECT_TRUE(moved.isChecked());
  EXPECT_EQ(moved.renderInsecure(), expected);
}

TEST_F(QueryTest, CheckedQueryAppendDowngradesToLegacyButRendersCorrectly) {
  // Appending a legacy (unchecked) query downgrades the result: the legacy half
  // was never validated at compile time, so it must be validated at render
  // time.
  auto q = Query::checked("SELECT * FROM %T", "users");
  q.append(Query("WHERE active = %d", 1));
  EXPECT_FALSE(q.isChecked());
  EXPECT_EQ(q.renderInsecure(), "SELECT * FROM `users` WHERE active = 1");
}

TEST_F(QueryTest, CheckedQueryAppendStaysCheckedWhenBothChecked) {
  // Concatenating two checked queries preserves checkedness: both sides were
  // validated at compile time, so the merged query needs no render-time
  // validation.
  auto q = Query::checked("SELECT * FROM %T", "users");
  q.append(Query::checked("WHERE active = %d", 1));
  EXPECT_TRUE(q.isChecked());
  EXPECT_EQ(q.renderInsecure(), "SELECT * FROM `users` WHERE active = 1");
}

TEST_F(QueryTest, CheckedQueryConcatOperatorStaysChecked) {
  // operator+ copies the (checked) left side then appends the (checked) right
  // side, so the result remains checked.
  auto combined = Query::checked("SELECT %C FROM %T", "id", "users") +
      Query::checked("WHERE id = %d", 5);
  EXPECT_TRUE(combined.isChecked());
  EXPECT_EQ(combined.renderInsecure(), "SELECT `id` FROM `users` WHERE id = 5");
}

TEST_F(QueryTest, CheckedRenderMatchesLegacyWithConnection) {
  // Connection-based (Full) escaping is otherwise only exercised by legacy
  // queries; confirm the checked path dispatches to the checked renderer there
  // too and renders identically. (The mock's escapeString is identity, so the
  // two sides match regardless of the escaping details.)
  MockInternalConnection conn;
  auto legacy = Query("SELECT * FROM %T WHERE name = %s", "users", "O'Brien");
  auto checked =
      Query::checked("SELECT * FROM %T WHERE name = %s", "users", "O'Brien");
  EXPECT_EQ(legacy.renderFb(&conn), checked.renderFb(&conn));
  EXPECT_EQ(legacy.renderStr(&conn), checked.renderStr(&conn));
}

TEST_F(QueryTest, MultiQueryRendersCheckedMember) {
  // A checked query inside a multi-query must still render via the checked
  // renderer (the per-query dispatch happens inside renderFb).
  std::vector<Query> queries;
  queries.emplace_back("SELECT * FROM %T", "a");
  queries.push_back(Query::checked("SELECT * FROM %T WHERE id = %d", "b", 1));
  EXPECT_EQ(
      Query::renderMultiQuery(nullptr, queries),
      "SELECT * FROM `a`;SELECT * FROM `b` WHERE id = 1");
}

namespace {
// A mock connection whose escapeString actually escapes single quotes, so the
// Full (connection-based) escape path can be pinned to an absolute value (the
// base MockInternalConnection's escapeString is an identity copy).
class EscapingMockConnection : public MockInternalConnection {
 public:
  using MockInternalConnection::MockInternalConnection;
  size_t escapeString(char* out, const char* src, size_t length, char quote)
      const override {
    EXPECT_EQ(quote, '"');
    size_t n = 0;
    for (size_t i = 0; i < length; ++i) {
      if (src[i] == '\'') {
        out[n++] = '\\';
      }
      out[n++] = src[i];
    }
    return n;
  }
};
} // namespace

TEST_F(QueryTest, CheckedQueryFullEscapesViaConnection) {
  // Pin the checked path's Full (connection) escaping to an absolute value
  // using a connection that really escapes, then confirm it matches legacy.
  EscapingMockConnection conn;
  auto checked =
      Query::checked("SELECT * FROM %T WHERE name = %s", "users", "O'Brien");
  EXPECT_EQ(
      checked.renderFb(&conn),
      "SELECT * FROM `users` WHERE name = \"O\\'Brien\"");
  auto legacy = Query("SELECT * FROM %T WHERE name = %s", "users", "O'Brien");
  EXPECT_EQ(checked.renderFb(&conn), legacy.renderFb(&conn));
}

TEST_F(QueryTest, MultiQueryStrAndPrefixRenderCheckedMember) {
  std::vector<Query> queries;
  queries.emplace_back("SELECT * FROM %T", "a");
  queries.push_back(Query::checked("SELECT * FROM %T WHERE id = %d", "b", 1));
  // std::string multi-query variant.
  EXPECT_EQ(
      Query::renderMultiQueryStr(nullptr, queries),
      "SELECT * FROM `a`;SELECT * FROM `b` WHERE id = 1");
  // With a per-query prefix.
  EXPECT_EQ(
      Query::renderMultiQueryFb(nullptr, queries, "/* p */ "),
      "/* p */ SELECT * FROM `a`;/* p */ SELECT * FROM `b` WHERE id = 1");
}

TEST_F(QueryTest, MultiQueryWrapperRendersCheckedMember) {
  std::vector<Query> queries;
  queries.emplace_back("SELECT * FROM %T", "a");
  queries.push_back(Query::checked("SELECT * FROM %T WHERE id = %d", "b", 1));
  MultiQuery mq(std::move(queries));
  MockInternalConnection conn;
  auto rendered = mq.renderQuery(&conn);
  ASSERT_NE(rendered, nullptr);
  EXPECT_EQ(*rendered, "SELECT * FROM `a`;SELECT * FROM `b` WHERE id = 1");
}

// =============================================================================
// QueryArgument Tests
//
// Tests query parameter type system and conversions.
// =============================================================================

class QueryArgumentTest : public ::testing::Test {};

TEST_F(QueryArgumentTest, AsStringConversion) {
  QueryArgument intArg(42);
  EXPECT_EQ(intArg.asString(), "42");

  QueryArgument strArg("hello");
  EXPECT_EQ(strArg.asString(), "hello");

  QueryArgument dblArg(3.14);
  // Double conversion may have precision variations
  EXPECT_TRUE(dblArg.asString().find("3.14") != std::string::npos);
}

TEST_F(QueryArgumentTest, TypeNameCoversAggregateColumns) {
  // typeName() must have an arm for every variant alternative; aggregate-column
  // tuples previously hit an unreachable CHECK(false) and crashed.
  EXPECT_EQ(
      QueryArgument(AggregateColumn{AggregateFunction::COUNT, {"db", "tbl"}})
          .typeName(),
      "AggregateColumn");
  EXPECT_EQ(
      QueryArgument(
          AliasedAggregateColumn{AggregateFunction::SUM, {"db", "tbl", "a"}})
          .typeName(),
      "AliasedAggregateColumn");
}

// =============================================================================
// EphemeralRowFields Tests
//
// Tests the EphemeralRowFields class using MockInternalRowMetadata.
// =============================================================================

class EphemeralRowFieldsTest : public ::testing::Test {};

TEST_F(EphemeralRowFieldsTest, FieldLookupByNameIndexAndType) {
  auto metadata =
      std::make_unique<MockInternalRowMetadata>(std::vector<MockFieldInfo>{
          {"id", "users", MYSQL_TYPE_LONG, 0},
          {"name", "users", MYSQL_TYPE_STRING, 0},
          {"price", "products", MYSQL_TYPE_DOUBLE, 0},
      });

  EphemeralRowFields fields(std::move(metadata));

  EXPECT_EQ(fields.numFields(), 3);

  // Index lookup by name
  EXPECT_EQ(fields.fieldIndex("id"), 0);
  EXPECT_EQ(fields.fieldIndex("name"), 1);

  // Optional index lookup
  EXPECT_EQ(*fields.fieldIndexOpt("id"), 0);
  EXPECT_FALSE(fields.fieldIndexOpt("nonexistent").has_value());

  // Field names and types
  EXPECT_EQ(fields.fieldName(0), "id");
  EXPECT_EQ(fields.fieldName(1), "name");
  EXPECT_EQ(fields.fieldType(0), MYSQL_TYPE_LONG);
  EXPECT_EQ(fields.fieldType(2), MYSQL_TYPE_DOUBLE);
}

TEST_F(EphemeralRowFieldsTest, MakeBufferedFields) {
  auto metadata =
      std::make_unique<MockInternalRowMetadata>(std::vector<MockFieldInfo>{
          {"id", "users", MYSQL_TYPE_LONG, 0},
          {"name", "users", MYSQL_TYPE_STRING, 0},
      });

  EphemeralRowFields ephemeralFields(std::move(metadata));
  auto bufferedFields = ephemeralFields.makeBufferedFields();

  EXPECT_NE(bufferedFields, nullptr);
  EXPECT_EQ(bufferedFields->numFields(), 2);
  EXPECT_EQ(bufferedFields->fieldName(0), "id");
  EXPECT_EQ(bufferedFields->fieldName(1), "name");
}

// =============================================================================
// MockInternalConnection Tests
//
// Tests using mock infrastructure to validate query execution flow.
// Basic mock behavior (error injection, query attributes, transaction state,
// reset, default results) is covered in MockTest.cpp (D94587045).
// =============================================================================

class MockIntegrationTest : public ::testing::Test {};

TEST_F(MockIntegrationTest, SimulateSelectQuery) {
  MockConnectionConfig config;
  config.queryResults["SELECT id, name FROM users"] = MockQueryResult{
      .fields =
          {
              {"id", "users", MYSQL_TYPE_LONG, 0},
              {"name", "users", MYSQL_TYPE_STRING, 0},
          },
      .rows =
          {
              {int64_t{1}, std::string("Alice")},
              {int64_t{2}, std::string("Bob")},
          },
  };

  MockInternalConnection conn(std::move(config));

  auto* queryResult = conn.runQuery("SELECT id, name FROM users");
  ASSERT_NE(queryResult, nullptr);

  auto result = conn.getResult();
  ASSERT_NE(result, nullptr);

  auto [status1, row1] = result->fetchRow();
  EXPECT_EQ(status1, InternalStatus::DONE);
  ASSERT_NE(row1, nullptr);
  EXPECT_EQ(row1->columnInt64(0), 1);
  EXPECT_EQ(row1->columnString(1), "Alice");

  auto [status2, row2] = result->fetchRow();
  ASSERT_NE(row2, nullptr);
  EXPECT_EQ(row2->columnString(1), "Bob");

  auto [status3, row3] = result->fetchRow();
  EXPECT_EQ(row3, nullptr);
}

TEST_F(MockIntegrationTest, SimulateQueryError) {
  MockConnectionConfig config;
  config.errorNumber = 1146;
  config.errorMessage = "Table 'test.nonexistent' doesn't exist";

  MockInternalConnection conn(std::move(config));

  auto* result = conn.runQuery("SELECT * FROM nonexistent");
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(conn.getErrno(), 1146);
}

// =============================================================================
// End-to-End Tests: Query Rendering + Mock Execution
// =============================================================================

TEST(EndToEndTest, QueryBuildAndMockExecute) {
  // Build a parameterized query
  Query query("SELECT id, name FROM %T WHERE active = %d", "users", 1);

  auto rendered = query.renderInsecure();
  EXPECT_EQ(rendered, "SELECT id, name FROM `users` WHERE active = 1");

  // Configure mock with this query
  MockConnectionConfig config;
  config.queryResults[rendered.toStdString()] = MockQueryResult{
      .fields =
          {
              {"id", "users", MYSQL_TYPE_LONG, 0},
              {"name", "users", MYSQL_TYPE_STRING, 0},
          },
      .rows =
          {
              {int64_t{1}, std::string("Alice")},
              {int64_t{2}, std::string("Bob")},
          },
  };

  MockInternalConnection conn(std::move(config));
  conn.runQuery(rendered.toStdString());

  auto result = conn.getResult();
  ASSERT_NE(result, nullptr);

  std::vector<std::string> names;
  while (true) {
    auto [status, row] = result->fetchRow();
    if (!row) {
      break;
    }
    names.emplace_back(row->columnString(1));
  }

  const std::vector<std::string> expected{"Alice", "Bob"};
  EXPECT_EQ(names, expected);
}

TEST(EndToEndTest, InsertQueryWithLastInsertId) {
  Query query("INSERT INTO %T (name) VALUES (%s)", "users", "NewUser");

  auto rendered = query.renderInsecure();
  // String values use double quotes in renderInsecure()
  EXPECT_EQ(rendered, "INSERT INTO `users` (name) VALUES (\"NewUser\")");

  MockConnectionConfig config;
  config.queryResults[rendered.toStdString()] = MockQueryResult{
      .lastInsertId = 999,
      .affectedRows = 1,
  };

  MockInternalConnection conn(std::move(config));
  conn.runQuery(rendered.toStdString());

  EXPECT_EQ(conn.getLastInsertId(), 999);
  EXPECT_EQ(conn.getAffectedRows(), 1);
}

TEST(EndToEndTest, ComplexQueryWithMultipleTypes) {
  // Test a more complex query with different parameter types
  Query query(
      "UPDATE %T SET name = %s, age = %d, score = %f WHERE id = %d",
      "users",
      "UpdatedName",
      25,
      99.5,
      42);

  auto rendered = query.renderInsecure();

  // Verify all parts are present
  EXPECT_TRUE(rendered.find("`users`") != std::string::npos);
  EXPECT_TRUE(rendered.find("\"UpdatedName\"") != std::string::npos);
  EXPECT_TRUE(rendered.find("25") != std::string::npos);
  EXPECT_TRUE(rendered.find("99.5") != std::string::npos);
  EXPECT_TRUE(rendered.find("42") != std::string::npos);
}

// =============================================================================
// RowFields Builder Tests
//
// The builder exists so that per-column data cannot be misaligned: every
// column contributes one entry to every vector, and the name -> index map is
// derived rather than hand-written.
// =============================================================================

// build() reads charset availability off the first column and then
// dereferences every column's charsetnr, so a mix has to be rejected as the
// columns go in rather than surfacing as bad_optional_access later.
TEST(RowFieldsBuilderTest, MixedCharsetAvailabilityIsRejected) {
  detail::RowFieldsColumns withCharset;
  withCharset.add("a", "t", MYSQL_TYPE_LONG, 0, 63u);
  EXPECT_THROW(
      withCharset.add("b", "t", MYSQL_TYPE_LONG, 0, std::nullopt),
      std::logic_error);
  // The rejected column left nothing behind.
  EXPECT_EQ(withCharset.size(), 1);

  detail::RowFieldsColumns withoutCharset;
  withoutCharset.add("a", "t", MYSQL_TYPE_LONG, 0, std::nullopt);
  EXPECT_THROW(
      withoutCharset.add("b", "t", MYSQL_TYPE_LONG, 0, 63u), std::logic_error);
  EXPECT_EQ(withoutCharset.size(), 1);
}

TEST(RowFieldsBuilderTest, Builder_ColumnsWithCharsets_PopulatesEveryVector) {
  auto fields =
      RowFields::builder()
          .column("id", MYSQL_TYPE_LONG, "tbl")
          .column(
              "name", MYSQL_TYPE_VARCHAR, "tbl2", /*flags=*/7, Charsetnr{45})
          .build();

  EXPECT_EQ(fields.numFields(), size_t{2});
  EXPECT_EQ(fields.fieldName(0), "id");
  EXPECT_EQ(fields.fieldName(1), "name");
  EXPECT_EQ(fields.tableName(1), "tbl2");
  EXPECT_EQ(fields.getFieldType(0), MYSQL_TYPE_LONG);
  EXPECT_EQ(fields.getFieldType(1), MYSQL_TYPE_VARCHAR);
  EXPECT_EQ(fields.getFieldFlags(0), uint64_t{0});
  EXPECT_EQ(fields.getFieldFlags(1), uint64_t{7});
  EXPECT_TRUE(fields.hasFieldCharsetnrs());
  EXPECT_EQ(fields.getFieldCharsetnr(0), kBinaryCharsetnr);
  EXPECT_EQ(fields.getFieldCharsetnr(1), 45u);
}

TEST(RowFieldsBuilderTest, Builder_DefaultedColumn_IsStillFullyPopulated) {
  // The mock case: only name and type. Every per-column vector must still get
  // an entry, so no accessor reads out of bounds.
  auto fields = RowFields::builder()
                    .column("id", MYSQL_TYPE_LONG)
                    .column("name", MYSQL_TYPE_VARCHAR)
                    .build();

  EXPECT_EQ(fields.numFields(), size_t{2});
  EXPECT_EQ(fields.tableName(0), "");
  EXPECT_EQ(fields.tableName(1), "");
  EXPECT_EQ(fields.getFieldFlags(0), uint64_t{0});
  EXPECT_TRUE(fields.hasFieldCharsetnrs());
  EXPECT_EQ(fields.getFieldCharsetnr(0), kBinaryCharsetnr);
  EXPECT_EQ(fields.getFieldCharsetnr(1), kBinaryCharsetnr);
}

TEST(RowFieldsBuilderTest, Builder_DerivesNameToIndexMap) {
  auto fields = RowFields::builder()
                    .column("id", MYSQL_TYPE_LONG, "tbl")
                    .column("name", MYSQL_TYPE_VARCHAR, "tbl", 0, Charsetnr{45})
                    .build();

  EXPECT_EQ(fields.fieldIndex("id"), size_t{0});
  EXPECT_EQ(fields.fieldIndex("name"), size_t{1});
  EXPECT_TRUE(fields.containsFieldName("name"));
  EXPECT_FALSE(fields.containsFieldName("nope"));
}

TEST(RowFieldsBuilderTest, BuilderWithoutCharsets_LeavesCharsetsUnavailable) {
  auto fields = RowFields::builderWithoutCharsets()
                    .column("id", MYSQL_TYPE_LONG, "tbl")
                    .column("name", MYSQL_TYPE_VARCHAR, "tbl")
                    .build();

  EXPECT_EQ(fields.numFields(), size_t{2});
  EXPECT_FALSE(fields.hasFieldCharsetnrs());
  // Absent charsets must stay an error rather than silently reporting binary.
  EXPECT_THROW(fields.getFieldCharsetnr(0), std::runtime_error);
}

TEST(RowFieldsBuilderTest, Builder_DuplicateColumnNames_LastIndexWins) {
  // Duplicate names are a real MySQL result shape; the map collapses them but
  // numFields() and the positional accessors must still see both columns.
  auto fields = RowFields::builderWithoutCharsets()
                    .column("dup", MYSQL_TYPE_LONG, "tbl")
                    .column("dup", MYSQL_TYPE_VARCHAR, "tbl")
                    .build();

  EXPECT_EQ(fields.numFields(), size_t{2});
  EXPECT_EQ(fields.fieldIndex("dup"), size_t{1});
  EXPECT_EQ(fields.getFieldType(0), MYSQL_TYPE_LONG);
  EXPECT_EQ(fields.getFieldType(1), MYSQL_TYPE_VARCHAR);
}

TEST(RowFieldsBuilderTest, Builder_NoColumns_ProducesEmptyFields) {
  auto fields = RowFields::builderWithoutCharsets().build();

  EXPECT_EQ(fields.numFields(), size_t{0});
  EXPECT_FALSE(fields.hasFieldCharsetnrs());
}

TEST(RowFieldsBuilderTest, BuildShared_ProducesUsableRowBlock) {
  auto fields = RowFields::builderWithoutCharsets()
                    .column("id", MYSQL_TYPE_VARCHAR, "tbl")
                    .column("name", MYSQL_TYPE_VARCHAR, "tbl")
                    .buildShared();

  RowBlock block(fields);
  block.startRow();
  block.appendValue(folly::StringPiece("1"));
  block.appendValue(folly::StringPiece("alice"));
  block.finishRow();

  EXPECT_EQ(block.numRows(), size_t{1});
  EXPECT_EQ(block.getField<std::string>(0, "name"), "alice");
}

namespace {

std::shared_ptr<RowFields> makeRowFields(std::vector<std::string> fieldNames) {
  const auto numFields = fieldNames.size();
  folly::F14NodeMap<std::string, int> fieldNameMap;
  for (size_t i = 0; i < numFields; ++i) {
    fieldNameMap[fieldNames[i]] = static_cast<int>(i);
  }
  return std::make_shared<RowFields>(
      std::move(fieldNameMap),
      std::move(fieldNames),
      std::vector<std::string>(numFields, "test_table"),
      std::vector<uint64_t>(numFields, 0),
      std::vector<enum_field_types>(numFields, MYSQL_TYPE_VAR_STRING));
}

} // namespace

// =============================================================================
// RowBlock::addRow Tests
//
// addRow() takes a complete row, so there is no partially built state to
// abandon and the column count is checked at one point.
// =============================================================================

TEST(RowBlockAddRowTest, AddRow_BracedList_StoresRow) {
  RowBlock block(makeRowFields({"id", "name"}));

  block.addRow({1, "alice"});
  block.addRow({2, nullptr});

  EXPECT_EQ(block.numRows(), size_t{2});
  EXPECT_EQ(block.getField<int64_t>(0, 0), 1);
  EXPECT_EQ(block.getField<std::string>(0, 1), "alice");
  EXPECT_EQ(block.getField<int64_t>(1, 0), 2);
  EXPECT_TRUE(block.isNull(1, 1));
}

TEST(RowBlockAddRowTest, AddRow_MixedCellTypes_RoundTrip) {
  RowBlock block(makeRowFields({"i", "u", "d", "b", "s"}));

  block.addRow({int64_t{-7}, uint64_t{7}, 1.5, true, std::string("hello")});

  EXPECT_EQ(block.getField<int64_t>(0, 0), -7);
  EXPECT_EQ(block.getField<uint64_t>(0, 1), uint64_t{7});
  EXPECT_EQ(block.getField<double>(0, 2), 1.5);
  EXPECT_EQ(block.getField<bool>(0, 3), true);
  EXPECT_EQ(block.getField<std::string>(0, 4), "hello");
}

TEST(RowBlockAddRowTest, AddRow_StdOptional_PresentAndEmpty) {
  RowBlock block(makeRowFields({"name", "id"}));

  // A present optional takes the contained value; an empty one is SQL NULL.
  block.addRow({std::optional<std::string>("alice"), std::optional<int64_t>{}});

  ASSERT_EQ(block.numRows(), size_t{1});
  EXPECT_FALSE(block.isNull(0, 0));
  EXPECT_EQ(block.getField<std::string>(0, 0), "alice");
  EXPECT_TRUE(block.isNull(0, 1));
}

TEST(RowBlockAddRowTest, AddRow_FollyOptional_PresentAndEmpty) {
  RowBlock block(makeRowFields({"id", "name"}));

  block.addRow({folly::Optional<int64_t>(7), folly::Optional<std::string>{}});

  ASSERT_EQ(block.numRows(), size_t{1});
  EXPECT_FALSE(block.isNull(0, 0));
  EXPECT_EQ(block.getField<int64_t>(0, 0), 7);
  EXPECT_TRUE(block.isNull(0, 1));
}

TEST(RowBlockAddRowTest, AddRow_RuntimeNullCharPointer_IsSqlNull) {
  RowBlock block(makeRowFields({"id", "name"}));

  // A literal nullptr picks the std::nullptr_t overload; a const char* that is
  // null only at run time reaches CellValue(const char*), where constructing a
  // StringPiece from it would call strlen(nullptr).
  const char* missing = nullptr;
  const char* present = "alice";
  block.addRow({missing, present});

  EXPECT_EQ(block.numRows(), size_t{1});
  EXPECT_TRUE(block.isNull(0, 0));
  EXPECT_FALSE(block.isNull(0, 1));
  EXPECT_EQ(block.getField<std::string>(0, 1), "alice");
}

TEST(RowBlockAddRowTest, AddRow_TemporaryString_IsCopied) {
  RowBlock block(makeRowFields({"s"}));

  // The StringPiece in the CellValue refers to a temporary that dies at the
  // end of this full-expression; StorageRow must have copied the bytes.
  block.addRow({folly::to<std::string>(12345)});

  EXPECT_EQ(block.getField<std::string>(0, 0), "12345");
}

TEST(RowBlockAddRowTest, AddRow_TooFewValues_ThrowsOutOfRange) {
  RowBlock block(makeRowFields({"id", "name"}));

  EXPECT_THROW(block.addRow({1}), std::out_of_range);
  EXPECT_EQ(block.numRows(), size_t{0});
}

TEST(RowBlockAddRowTest, AddRow_TooManyValues_ThrowsAndAddsNothing) {
  RowBlock block(makeRowFields({"id"}));

  EXPECT_THROW(block.addRow({1, 2}), std::out_of_range);
  EXPECT_EQ(block.numRows(), size_t{0});
  EXPECT_TRUE(block.empty());
}

TEST(RowBlockAddRowTest, AddRow_Range_StoresRow) {
  RowBlock block(makeRowFields({"a", "b", "c"}));
  std::vector<std::string> values{"x", "y", "z"};

  block.addRow(values);

  EXPECT_EQ(block.numRows(), size_t{1});
  EXPECT_EQ(block.getField<std::string>(0, 2), "z");
}

TEST(RowBlockAddRowTest, AddRow_StorageRow_StoresRow) {
  RowBlock block(makeRowFields({"id", "name"}));

  StorageRow row(2);
  row.appendValue(folly::StringPiece("1"));
  row.appendValue(folly::StringPiece("alice"));
  block.addRow(std::move(row));

  EXPECT_EQ(block.numRows(), size_t{1});
  EXPECT_EQ(block.getField<std::string>(0, "name"), "alice");
}

TEST(RowBlockAddRowTest, AddRow_StorageRowWrongArity_ThrowsOutOfRange) {
  RowBlock block(makeRowFields({"id", "name"}));

  StorageRow row(1);
  row.appendValue(folly::StringPiece("1"));

  EXPECT_THROW(block.addRow(std::move(row)), std::out_of_range);
  EXPECT_EQ(block.numRows(), size_t{0});
}

TEST(RowBlockAddRowTest, AddRow_WhileStartRowOpen_ThrowsLogicError) {
  // Mixing the two APIs mid-row would interleave; catch it rather than
  // silently reordering the caller's rows.
  RowBlock block(makeRowFields({"id"}));
  block.startRow();

  EXPECT_THROW(block.addRow({1}), std::logic_error);
}

TEST(RowBlockAddRowTest, AddRow_InterleavedWithStartRow_BothVisible) {
  // Sequential use of both APIs is fine; only an *open* row is rejected.
  RowBlock block(makeRowFields({"id"}));

  block.addRow({1});
  block.startRow();
  block.appendValue(folly::StringPiece("2"));
  block.finishRow();
  block.addRow({3});

  EXPECT_EQ(block.numRows(), size_t{3});
  EXPECT_EQ(block.getField<int64_t>(2, 0), 3);
}

// =============================================================================
// RowBlock Build-Path Tests
//
// Covers the RowBlock building API (startRow/appendValue/appendNull/finishRow)
// and the bounds checks on the read path. These run inside a noexcept libevent
// callback, so they must throw a diagnosable exception rather than abort, and
// must never leave a partially built row visible to readers.
// =============================================================================

TEST(RowBlockTest, BuildRows_TwoCompletedRows_StoresExactlyTwoRows) {
  RowBlock block(makeRowFields({"id", "name"}));

  block.startRow();
  block.appendValue(folly::StringPiece("1"));
  block.appendValue(folly::StringPiece("alice"));
  block.finishRow();

  block.startRow();
  block.appendValue(folly::StringPiece("2"));
  block.appendNull();
  block.finishRow();

  EXPECT_EQ(block.numRows(), size_t{2});
  EXPECT_EQ(block.getField<std::string>(0, 0), "1");
  EXPECT_EQ(block.getField<std::string>(0, 1), "alice");
  EXPECT_EQ(block.getField<std::string>(1, 0), "2");
  EXPECT_TRUE(block.isNull(1, 1));
}

TEST(RowBlockTest, AppendValue_PastFieldCount_ThrowsOutOfRange) {
  RowBlock block(makeRowFields({"only"}));

  block.startRow();
  block.appendValue(folly::StringPiece("a"));

  // Overrunning the declared column count is an out_of_range, which derives
  // from logic_error like the rest of the build-API misuse errors.
  EXPECT_THROW(block.appendValue(folly::StringPiece("b")), std::out_of_range);
}

TEST(RowBlockTest, AppendValue_WithoutStartRow_ThrowsLogicError) {
  RowBlock block(makeRowFields({"id"}));

  EXPECT_THROW(block.appendValue(folly::StringPiece("a")), std::logic_error);
  EXPECT_THROW(block.appendNull(), std::logic_error);
}

TEST(RowBlockTest, StartRow_CalledTwiceWithoutFinish_ThrowsLogicError) {
  RowBlock block(makeRowFields({"id"}));

  block.startRow();

  EXPECT_THROW(block.startRow(), std::logic_error);
}

TEST(RowBlockTest, FinishRow_WithMissingFields_ThrowsAndStoresNoRow) {
  RowBlock block(makeRowFields({"id", "name"}));

  block.startRow();
  block.appendValue(folly::StringPiece("1")); // only 1 of the 2 fields

  EXPECT_THROW(block.finishRow(), std::logic_error);
  // The incomplete row must not become visible to readers.
  EXPECT_EQ(block.numRows(), size_t{0});
  EXPECT_TRUE(block.empty());
}

TEST(RowBlockTest, FinishRow_UnstartedRow_ThrowsLogicError) {
  RowBlock block(makeRowFields({"id"}));

  EXPECT_THROW(block.finishRow(), std::logic_error);
}

TEST(RowBlockTest, IsNull_FieldPastEnd_ThrowsOutOfRange) {
  RowBlock block(makeRowFields({"id"}));
  block.startRow();
  block.appendValue(folly::StringPiece("1"));
  block.finishRow();

  // StorageRow::isNull only DCHECKs its column index, so without an explicit
  // check here an out-of-range field would be an unchecked read in opt builds.
  EXPECT_THROW(block.isNull(0, 5), std::out_of_range);
  EXPECT_THROW(block.isNull(5, 0), std::out_of_range);
}

TEST(RowBlockTest, GetField_RowPastEnd_ThrowsOutOfRangeNamingIndexAndSize) {
  RowBlock block(makeRowFields({"id"}));
  block.startRow();
  block.appendValue(folly::StringPiece("1"));
  block.finishRow();

  try {
    block.getField<std::string>(7, 0);
    FAIL() << "expected std::out_of_range for a row index past the end";
  } catch (const std::out_of_range& ex) {
    // The overrun magnitude is the primary diagnostic for the malformed-result
    // cases this check exists for, so index and size must both survive.
    EXPECT_EQ(std::string(ex.what()), "row index 7 out of range (size 1)");
  }
}

// A result set whose rows carry more columns than the RowFields describe is
// rejected as MalformedResultError rather than escaping as the raw
// std::out_of_range that RowBlock throws. The fetch loop runs inside a
// noexcept libevent callback and relies on this type to tell a bad result set
// apart from an exception thrown by a consumer callback.
TEST(MalformedResultTest, RowWiderThanRowFieldsThrowsMalformedResult) {
  // Stream metadata declares two columns...
  auto metadata =
      std::make_unique<MockInternalRowMetadata>(std::vector<MockFieldInfo>{
          {.name = "id", .tableName = "t", .type = MYSQL_TYPE_LONGLONG},
          {.name = "name", .tableName = "t", .type = MYSQL_TYPE_VARCHAR}});
  auto result = std::make_unique<MockInternalResult>(
      std::vector<std::vector<MockColumnValue>>{
          {int64_t{1}, std::string("alice")}});
  RowStream stream(std::move(result), std::move(metadata));

  // ...but the RowFields the rows are copied into describe only one.
  auto narrowFields = std::make_shared<RowFields>(
      folly::F14NodeMap<std::string, int>{{"id", 0}},
      std::vector<std::string>{"id"},
      std::vector<std::string>{"t"},
      std::vector<uint64_t>{0},
      std::vector<enum_field_types>{MYSQL_TYPE_LONGLONG});

  EXPECT_THROW(
      makeRowBlockFromStream(narrowFields, &stream), MalformedResultError);
}

// The originating diagnostic survives the retag -- index and size are what
// make a malformed result set actionable.
TEST(MalformedResultTest, MalformedResultPreservesUnderlyingMessage) {
  auto metadata =
      std::make_unique<MockInternalRowMetadata>(std::vector<MockFieldInfo>{
          {.name = "id", .tableName = "t", .type = MYSQL_TYPE_LONGLONG},
          {.name = "name", .tableName = "t", .type = MYSQL_TYPE_VARCHAR}});
  auto result = std::make_unique<MockInternalResult>(
      std::vector<std::vector<MockColumnValue>>{
          {int64_t{1}, std::string("alice")}});
  RowStream stream(std::move(result), std::move(metadata));

  auto narrowFields = std::make_shared<RowFields>(
      folly::F14NodeMap<std::string, int>{{"id", 0}},
      std::vector<std::string>{"id"},
      std::vector<std::string>{"t"},
      std::vector<uint64_t>{0},
      std::vector<enum_field_types>{MYSQL_TYPE_LONGLONG});

  try {
    makeRowBlockFromStream(narrowFields, &stream);
    FAIL() << "expected MalformedResultError";
  } catch (const MalformedResultError& ex) {
    // Which layer rejects the row -- and so the exact wording -- depends on
    // how copyRowToRowBlock() appends values, so match on the substance: the
    // message must still describe the column mismatch rather than a generic
    // "malformed result".
    EXPECT_NE(std::string(ex.what()).find("column"), std::string::npos)
        << ex.what();
  }
}

// A well-formed result set is unaffected by the retag.
TEST(MalformedResultTest, WellFormedResultBuildsNormally) {
  auto metadata =
      std::make_unique<MockInternalRowMetadata>(std::vector<MockFieldInfo>{
          {.name = "id", .tableName = "t", .type = MYSQL_TYPE_LONGLONG},
          {.name = "name", .tableName = "t", .type = MYSQL_TYPE_VARCHAR}});
  auto result = std::make_unique<MockInternalResult>(
      std::vector<std::vector<MockColumnValue>>{
          {int64_t{1}, std::string("alice")},
          {int64_t{2}, std::string("bob")}});
  RowStream stream(std::move(result), std::move(metadata));

  auto fields = stream.getEphemeralRowFields()->makeBufferedFields();
  auto block = makeRowBlockFromStream(fields, &stream);

  ASSERT_EQ(block.numRows(), 2);
  EXPECT_EQ(block.getField<int64_t>(0, "id"), 1);
  EXPECT_EQ(block.getField<std::string>(1, "name"), "bob");
}

} // namespace facebook::common::mysql_client::test
