/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>
#include <folly/Likely.h>
#include <concepts>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "squangle/base/ConnectionKey.h"
// DbResult.h only forward-declares Connection, but DbQueryResult holds a
// unique_ptr<Connection> whose destructor needs the complete type.
#include "squangle/mysql_client/Connection.h"
#include "squangle/mysql_client/DbResult.h"
#include "squangle/mysql_client/Operation.h"
#include "squangle/mysql_client/Row.h"

namespace facebook::common::mysql_client::test {

// Builds the query-result objects a test needs to stand in for a real fetch.
//
//   auto result = MockQueryResult::builder()
//       .column("id", MYSQL_TYPE_LONG)
//       .column("name", MYSQL_TYPE_VARCHAR)
//       .row({1, "alice"})
//       .row({2, nullptr})
//       .buildDbQueryResult();
//
// Stop at whichever level the test actually consumes -- buildRowBlock(),
// buildQueryResult() and buildDbQueryResult() all build the same rows.
//
// This exists because the objects a fetch produces are assembled from five
// pieces (RowFields, RowBlock, QueryResult, connection key, DbQueryResult) that
// a test has no interest in, and two of the joins between them are easy to get
// wrong in ways nothing reports:
//
//   - A freshly constructed QueryResult is `partial_ == true` with an
//     OperationResult of Unknown, because it is built to be filled in
//     incrementally by a running fetch. A test that forgets setPartial(false)
//     gets a result that claims more rows are coming.
//   - DbQueryResult's constructor takes seven positional arguments, two of
//     which are adjacent integers with different meanings
//     (num_queries_executed, result_size).
//
// The defaults here describe a query that completed successfully, since that is
// what nearly every fixture wants; the setters below cover the rest.
//
// Nothing in production builds results this way -- a real fetch reads them off
// a row stream -- so this is deliberately test-only and lives outside the
// client's shipped API.
class MockQueryResult {
 public:
  // Only a home for the builder types and their factories.
  MockQueryResult() = delete;

  template <typename FieldsBuilder>
  class BasicBuilder;

  // Columns carry a charsetnr, defaulting to binary. See RowFields::builder().
  using Builder = BasicBuilder<RowFields::Builder>;

  // TRANSITIONAL -- columns carry no charsetnr at all, for tests standing in
  // for a protocol that cannot report them. See
  // RowFields::builderWithoutCharsets().
  using BuilderWithoutCharsets =
      BasicBuilder<RowFields::BuilderWithoutCharsets>;

  static Builder builder();
  static BuilderWithoutCharsets builderWithoutCharsets();
};

template <typename FieldsBuilder>
class MockQueryResult::BasicBuilder {
 public:
  // Adds a column. Arguments forward to the underlying RowFields builder, so
  // the same defaults apply and passing a Charsetnr to the no-charsets flavor
  // is still a compile error.
  //
  // Must precede every row(): the column list is sealed as soon as the first
  // row arrives, because a row's values are copied into storage immediately
  // rather than buffered.
  template <typename... Args>
  BasicBuilder& column(Args&&... args) {
    checkNotBuilt();
    if (fields_ != nullptr) [[unlikely]] {
      throwColumnAfterSeal();
    }
    fieldsBuilder_.column(std::forward<Args>(args)...);
    return *this;
  }

  // Adds one row to the current block. Throws std::out_of_range if it does not
  // have exactly one value per column.
  BasicBuilder& row(std::initializer_list<CellValue> values) {
    checkNotBuilt();
    sealColumns();
    blocks_.back().addRow(values);
    return *this;
  }

  // Ditto, for a row assembled at runtime -- any range of
  // CellValue-convertibles, e.g. std::vector<std::string>.
  //
  // String-like types are excluded: they satisfy the range requirement, and
  // because char is a signed integral their characters would each convert to
  // an integer CellValue, silently producing a row of numbers.
  template <typename Range>
    requires(!std::convertible_to<const Range&, std::string_view>)
  BasicBuilder& row(const Range& values) {
    checkNotBuilt();
    sealColumns();
    blocks_.back().addRow(values);
    return *this;
  }

  // Starts a new RowBlock. A real fetch emits one block per read from the
  // wire, so a test exercising block boundaries needs more than one; tests
  // that do not care can ignore this and get a single block.
  //
  // A no-op while the current block is still empty, so it can be called
  // unconditionally at the top of a row loop.
  BasicBuilder& nextBlock() {
    checkNotBuilt();
    sealColumns();
    if (blocks_.back().numRows() > 0) {
      blocks_.emplace_back(fields_);
    }
    return *this;
  }

  BasicBuilder& rowsMatched(uint64_t value) {
    checkNotBuilt();
    rowsMatched_ = value;
    return *this;
  }
  BasicBuilder& lastInsertId(uint64_t value) {
    checkNotBuilt();
    lastInsertId_ = value;
    return *this;
  }
  BasicBuilder& numRowsAffected(uint64_t value) {
    checkNotBuilt();
    numRowsAffected_ = value;
    return *this;
  }
  BasicBuilder& queryNum(int value) {
    checkNotBuilt();
    queryNum_ = value;
    return *this;
  }
  // Applies to both the QueryResult and, via buildDbQueryResult(), the
  // DbQueryResult -- so a failed result reports ok() == false.
  BasicBuilder& operationResult(OperationResult value) {
    checkNotBuilt();
    operationResult_ = value;
    return *this;
  }
  BasicBuilder& numQueriesExecuted(int value) {
    checkNotBuilt();
    numQueriesExecuted_ = value;
    return *this;
  }
  BasicBuilder& resultSize(uint64_t value) {
    checkNotBuilt();
    resultSize_ = value;
    return *this;
  }
  BasicBuilder& connectionKey(std::shared_ptr<const ConnectionKey> value) {
    checkNotBuilt();
    connectionKey_ = std::move(value);
    return *this;
  }

  // The column metadata on its own, for a test that only needs the schema.
  std::shared_ptr<RowFields> buildRowFields() {
    sealColumns();
    return fields_;
  }

  // The rows as a single block. Throws std::logic_error if more than one block
  // holds rows, since there is then no single block to return.
  RowBlock buildRowBlock() {
    checkNotBuilt();
    sealColumns();
    // Empty blocks are dropped here for the same reason buildQueryResult()
    // drops them, so that a trailing nextBlock() does not turn a single-block
    // result into an error.
    std::erase_if(
        blocks_, [](const RowBlock& block) { return block.numRows() == 0; });
    if (blocks_.size() > 1) [[unlikely]] {
      throwNotSingleBlock(blocks_.size());
    }
    // built_ is set only once the call is known to succeed, so a rejected
    // build leaves the builder usable.
    built_ = true;
    if (blocks_.empty()) {
      return RowBlock(fields_);
    }
    return std::move(blocks_.front());
  }

  QueryResult buildQueryResult() {
    checkNotBuilt();
    sealColumns();
    // Nothing below can fail, so the builder is marked consumed up front.
    built_ = true;
    // A real fetch never appends an empty block -- notifyRowsReady() returns
    // early when a read produced no rows -- so a zero-row result has no blocks
    // at all, and begin() == end(). Drop the empties rather than hand out a
    // shape the production path cannot produce.
    auto blocks = std::move(blocks_);
    std::erase_if(
        blocks, [](const RowBlock& block) { return block.numRows() == 0; });

    QueryResult result(queryNum_);
    result.setRowFields(fields_);
    result.setRowBlocks(std::move(blocks));
    // A fetch that has ended, which is the only state a mock can be in.
    result.setPartial(false);
    result.setOperationResult(operationResult_);
    result.setRowsMatched(rowsMatched_);
    result.setLastInsertId(lastInsertId_);
    result.setNumRowsAffected(numRowsAffected_);
    return result;
  }

  DbQueryResult buildDbQueryResult() {
    auto result = buildQueryResult();
    std::shared_ptr<const ConnectionKey> key = std::move(connectionKey_);
    if (key == nullptr) {
      key = std::make_shared<const MysqlConnectionKey>();
    }
    return DbQueryResult{
        std::move(result),
        numQueriesExecuted_,
        resultSize_,
        /*conn=*/nullptr,
        operationResult_,
        std::move(key),
        Duration{}};
  }

 private:
  // Builds the RowFields and opens the first block. Idempotent: every entry
  // point that needs fields calls it, so column ordering is the only rule a
  // caller has to observe.
  //
  // Establishes the invariant that row() and nextBlock() rely on: once fields_
  // is set, blocks_ holds at least one block, so blocks_.back() is valid.
  // Nothing may empty blocks_ without also setting built_, which turns every
  // path back in here into a throw.
  void sealColumns() {
    if (fields_ == nullptr) {
      fields_ = fieldsBuilder_.buildShared();
      blocks_.emplace_back(fields_);
    }
  }

  [[noreturn]] static void throwColumnAfterSeal() {
    throw std::logic_error(
        "MockQueryResult: column() after the column list was sealed; declare "
        "every column before the first row(), nextBlock() or build call");
  }

  [[noreturn]] static void throwAlreadyBuilt() {
    throw std::logic_error(
        "MockQueryResult: builder already used; construct a new one per "
        "result");
  }

  void checkNotBuilt() const {
    if (built_) [[unlikely]] {
      throwAlreadyBuilt();
    }
  }

  [[noreturn]] static void throwNotSingleBlock(size_t count) {
    throw std::logic_error(
        fmt::format(
            "MockQueryResult: buildRowBlock() needs a single block but {} "
            "hold rows; use buildQueryResult() instead",
            count));
  }

  FieldsBuilder fieldsBuilder_;
  // The shared schema. Null until sealColumns() builds it, which also closes
  // the column list.
  std::shared_ptr<RowFields> fields_;
  std::vector<RowBlock> blocks_;
  // A consuming build call moves the rows out; the builder is single use.
  bool built_ = false;

  int queryNum_ = 0;
  OperationResult operationResult_ = OperationResult::Succeeded;
  std::optional<uint64_t> rowsMatched_;
  uint64_t lastInsertId_ = 0;
  uint64_t numRowsAffected_ = 0;
  int numQueriesExecuted_ = 1;
  uint64_t resultSize_ = 0;
  std::shared_ptr<const ConnectionKey> connectionKey_;
};

inline MockQueryResult::Builder MockQueryResult::builder() {
  return {};
}

inline MockQueryResult::BuilderWithoutCharsets
MockQueryResult::builderWithoutCharsets() {
  return {};
}

} // namespace facebook::common::mysql_client::test
