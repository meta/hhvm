/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

//
// Classes representing rows and blocks of rows returned by a MySQL
// query.  Note a query can return multiple blocks.
//
// These classes make heavy use of StringPiece.  This means if you
// wish to store the data in the query, you must copy it out.  Once
// you lose your RowBlock, any Rows or StringPieces referencing it
// will be invalid.

#pragma once

#include <boost/iterator/iterator_facade.hpp>
#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/Range.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <folly/json/dynamic.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <chrono>
#include <concepts>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "squangle/mysql_client/InternalConnection.h"
#include "squangle/util/StorageRow.h"
#include "squangle/util/StringStore.h"

namespace facebook::common::mysql_client {

class RowBlock;

namespace detail {

[[noreturn]] void
throwIndexOutOfRange(std::string_view what, size_t value, size_t size);

// Throws std::out_of_range when `value` is not a valid index into a container
// of the given `size` (i.e. value >= size). The message includes both `value`
// and `size` so an out-of-bounds row/field/column index -- e.g. from a
// malformed server response -- fails the query with a diagnosable error
// instead of aborting the process.
inline void
checkIndexInRange(std::string_view what, size_t value, size_t size) {
  if (value >= size) [[unlikely]] {
    throwIndexOutOfRange(what, value, size);
  }
}

} // namespace detail

// A row of returned data.  This makes the columns available either
// positionally or by name, both via operator[].  In addition, the raw
// values are available via iteration.  A Row is only valid for as
// long as the RowBlock it belongs to is valid, so don't save these.
//
// Note that if multiple columns have the same name (as reported by
// the MySQL server when it returns rows), the column name access will
// return only one of them; to get all values, you should use the
// integer indexes in operator[] or iterate over the columns of the
// row directly.
class Row {
 public:
  Row(const RowBlock* row_block, size_t row_number);

  // L should be StringPiece, size_t, or convertible therefrom.  The
  // return value is converted with folly::to<T>.
  template <typename T, typename L>
  T get(const L& l) const;
  // Like above, but a default value is supplied.  If the column is null the
  // default value is returned
  template <typename T, typename L>
  T get(const L& l, T d) const;

  // Similar to above but will return as an optional which will be empty if the
  // column is null
  template <
      typename T,
      template <typename> class Optional = std::optional,
      typename L>
  Optional<T> getOptional(const L& l) const;

  folly::dynamic getDynamic(size_t l) const;
  folly::dynamic getDynamic(folly::StringPiece l) const;

  // Vector-like and map-like access.  Note the above about ambiguity
  // for map access when column names conflict.
  size_t size() const;
  folly::StringPiece operator[](size_t col) const;
  folly::StringPiece operator[](folly::StringPiece field) const;

  // Return the field index if the row contains a field with the given name.
  std::optional<size_t> fieldIndexOpt(folly::StringPiece field) const;

  // Is the field nullable?
  bool isNull(size_t col) const;
  bool isNull(folly::StringPiece field) const;

  // Our very simple iterator.  Just barely enough to support
  // range-based for loops.
  class Iterator : public boost::iterator_facade<
                       Iterator,
                       const folly::StringPiece,
                       boost::single_pass_traversal_tag,
                       const folly::StringPiece> {
   public:
    Iterator(const Row* row, size_t column_number)
        : row_(row), current_column_number_(column_number) {}

    void increment() {
      ++current_column_number_;
    }
    const folly::StringPiece dereference() const {
      detail::checkIndexInRange(
          "column index", current_column_number_, row_->size());
      return row_->get<folly::StringPiece>(current_column_number_);
    }
    bool equal(const Iterator& other) const {
      return (
          row_ == other.row_ &&
          current_column_number_ == other.current_column_number_);
    }

   private:
    const Row* row_;
    size_t current_column_number_;
  };

  Iterator begin() const;
  Iterator end() const;

 private:
  const RowBlock* row_block_; // unowned
  const size_t row_number_;
};

// RowFields encapsulates the data about the fields (name, flags, types).
class RowFields {
 public:
  RowFields(
      folly::F14NodeMap<std::string, int>&& field_name_map,
      std::vector<std::string>&& field_names,
      std::vector<std::string>&& table_names,
      std::vector<uint64_t>&& mysql_field_flags,
      std::vector<enum_field_types>&& mysql_field_types,
      std::vector<unsigned int>&& mysql_field_charsetnrs = {},
      std::vector<unsigned int>&& mysql_field_decimals = {})
      : num_fields_(field_names.size()),
        field_name_map_(std::move(field_name_map)),
        field_names_(std::move(field_names)),
        table_names_(std::move(table_names)),
        mysql_field_flags_(std::move(mysql_field_flags)),
        mysql_field_types_(std::move(mysql_field_types)),
        mysql_field_charsetnrs_(std::move(mysql_field_charsetnrs)),
        mysql_field_decimals_(std::move(mysql_field_decimals)) {
    validateFieldAlignment();
  }

  class Builder;
  class BuilderWithoutCharsets;

  // Builds a RowFields one column at a time. Every column contributes exactly
  // one entry to every per-column vector and the name -> index map is derived
  // rather than hand-maintained, so the misalignments the raw constructor
  // permits cannot be expressed. Prefer this over the constructor.
  //
  //   auto fields = RowFields::builder()
  //       .column("id", MYSQL_TYPE_LONG)
  //       .column("name", MYSQL_TYPE_VARCHAR, "users")
  //       .buildShared();
  //
  // Only `name` and `type` are required; `table`, `flags`, `charsetnr` and
  // `decimals` default. Every column still contributes one entry to every
  // vector, so a defaulted column is fully valid -- `tableName()` returns ""
  // rather than reading out of bounds.
  //
  // There are two flavors because whether a charsetnr is reported is fixed by
  // the protocol, not chosen per column: the MySQL protocol carries one for
  // every column (63, `binary`, for non-text), while some protocols carry none
  // at all. A per-column optional would reintroduce exactly the partial-vector/
  // misalignment these builders exist to prevent, so the choice is made once,
  // in the type.
  static Builder builder();

  // TRANSITIONAL -- for protocols that cannot supply charsetnrs at all. Once
  // every supported protocol reports them, migrate the remaining callers to
  // builder() and delete this overload along with hasFieldCharsetnrs() and the
  // throwing branch of getFieldCharsetnr().
  static BuilderWithoutCharsets builderWithoutCharsets();

  // Get the MySQL type of the field.
  enum_field_types getFieldType(size_t field_num) const {
    return mysql_field_types_[field_num];
  }

  // Ditto, but by name.
  enum_field_types getFieldType(folly::StringPiece field_name) const {
    return mysql_field_types_[fieldIndex(field_name)];
  }

  // Get the MySQL flags of the field.
  uint64_t getFieldFlags(size_t field_num) const {
    return mysql_field_flags_[field_num];
  }

  // Ditto, but by name.
  uint64_t getFieldFlags(folly::StringPiece field_name) const {
    return mysql_field_flags_[fieldIndex(field_name)];
  }

  // Whether this result set carries `MYSQL_FIELD::charsetnr` per column. False
  // for protocols that do not report it — see
  // `InternalRowMetadata::getFieldCharsetnr`.
  bool hasFieldCharsetnrs() const noexcept {
    return !mysql_field_charsetnrs_.empty();
  }

  // `MYSQL_FIELD::charsetnr`: the collation of the bytes this field's values
  // arrive in. Throws when `hasFieldCharsetnrs()` is false — 63 (`binary`) is
  // the only in-band fallback and a decoder cannot tell it from a real binary
  // column, so it would hand back raw bytes for text and call it correct.
  unsigned int getFieldCharsetnr(size_t field_num) const {
    if (mysql_field_charsetnrs_.empty()) {
      throw std::runtime_error(
          "column charsets are unavailable on this connection's protocol");
    }
    return mysql_field_charsetnrs_[field_num];
  }

  // Ditto, but by name.
  unsigned int getFieldCharsetnr(folly::StringPiece field_name) const {
    return getFieldCharsetnr(fieldIndex(field_name));
  }

  // `MYSQL_FIELD::decimals`: the digits after the decimal point for DECIMAL,
  // FLOAT(M,D) and DOUBLE(M,D) columns, or the fractional-seconds precision
  // for temporal columns. `kDecimalsNotSpecified` when the column has none,
  // or when the result set was built without decimals.
  unsigned int getFieldDecimals(size_t field_num) const;

  // Ditto, but by name.
  unsigned int getFieldDecimals(folly::StringPiece field_name) const {
    return getFieldDecimals(fieldIndex(field_name));
  }

  // Check if the row contains the field name.
  bool containsFieldName(folly::StringPiece field_name) const {
    return field_name_map_.find(field_name) != field_name_map_.end();
  }

  // What is the name of the i'th column in the result set?
  folly::StringPiece fieldName(size_t i) const {
    return field_names_[i];
  }

  // What is the name of the table (or alias) for the i'th column in the
  // result set?
  folly::StringPiece tableName(size_t i) const {
    return table_names_[i];
  }

  // How many fields and rows do we have?
  size_t numFields() const {
    return num_fields_;
  }

  // Given a field_name, return the numeric column number, or die trying.
  std::optional<size_t> fieldIndexOpt(folly::StringPiece field_name) const {
    auto it = field_name_map_.find(field_name);
    if (it != field_name_map_.end()) {
      return it->second;
    }

    return std::nullopt;
  }

  size_t fieldIndex(folly::StringPiece field_name) const {
    if (auto opt = fieldIndexOpt(field_name); opt) {
      return *opt;
    }

    throw std::out_of_range(fmt::format("Invalid field: {}", field_name));
  }

 private:
  // Every per-column vector is indexed by the same field number, so a short
  // vector silently misaligns every column past the gap and makes the
  // unchecked accessors above read out of bounds. `numFields()` is defined by
  // `field_names_`; everything else must agree with it. Throws
  // std::invalid_argument rather than aborting, per T283583376.
  void validateFieldAlignment() const;

  size_t num_fields_;
  folly::F14NodeMap<std::string, int> field_name_map_;
  std::vector<std::string> field_names_;
  std::vector<std::string> table_names_;
  std::vector<uint64_t> mysql_field_flags_;
  std::vector<enum_field_types> mysql_field_types_;
  std::vector<unsigned int> mysql_field_charsetnrs_;
  // Empty when the result set was built without decimals.
  std::vector<unsigned int> mysql_field_decimals_;

  friend class RowBlock;
};

// `MYSQL_FIELD::charsetnr` for the `binary` collation, which is what the MySQL
// protocol reports for every non-text column. It is a real collation, not a
// "missing" sentinel -- see RowFields::getFieldCharsetnr.
inline constexpr unsigned int kBinaryCharsetnr = 63;

// A charsetnr is deliberately not a bare integer, because `flags` and
// `charsetnr` sit next to each other in Builder::column() and both would
// otherwise accept any integer. The wrapper turns each confusion into a
// compile error instead of a silently wrong column:
//   - omitting `flags` and passing the charsetnr positionally, where it would
//     become the flags value;
//   - passing a plain integer where a charsetnr belongs;
//   - passing a charsetnr to BuilderWithoutCharsets, which has no such
//     parameter.
struct Charsetnr {
  explicit constexpr Charsetnr(unsigned int v) : value(v) {}
  unsigned int value;
};

// `MYSQL_FIELD::decimals` for a column with no fixed number of decimals, such
// as FLOAT, DOUBLE or a string. The server's `DECIMAL_NOT_SPECIFIED`.
inline constexpr unsigned int kDecimalsNotSpecified = 31;

// Wrapped for the same reason as Charsetnr: it sits next to other integer
// parameters in the builders' column().
struct Decimals {
  explicit constexpr Decimals(unsigned int v) : value(v) {}
  unsigned int value;
};

namespace detail {

// Column accumulation shared by both RowFields builder flavors. Held by value
// rather than inherited from so each builder exposes only its own `column()`.
class RowFieldsColumns {
 public:
  // One column per call. Accumulating a vector of whole columns: if it throws,
  // no column is half-added, so the builder cannot be left holding a misaligned
  // set -- which is the invariant the builders exist to provide.
  //
  // Throws std::logic_error if `charsetnr` is present here but absent on the
  // columns already added, or vice versa. Only presence has to agree -- the
  // values differ per column -- and build() relies on that.
  void add(
      std::string name,
      std::string table,
      enum_field_types type,
      uint64_t flags,
      std::optional<unsigned int> charsetnr,
      unsigned int decimals);

  size_t size() const {
    return columns_.size();
  }

  RowFields build();

 private:
  struct Column {
    std::string name;
    std::string table;
    enum_field_types type;
    uint64_t flags;
    std::optional<unsigned int> charsetnr;
    unsigned int decimals;
  };

  std::vector<Column> columns_;
};

} // namespace detail

// Builds a RowFields whose columns all carry a charsetnr. See
// RowFields::builder().
class RowFields::Builder {
 public:
  // `table` and `flags` default because most callers -- mocks especially --
  // have no meaningful value for them, and a defaulted-but-populated vector is
  // still fully aligned. `charsetnr` defaults to binary, i.e. "these are just
  // bytes", which is both true of mock data and the conservative shape for a
  // decoder. A test exercising text decoding should pass one explicitly.
  Builder& column(
      std::string name,
      enum_field_types type,
      std::string table = "",
      uint64_t flags = 0,
      Charsetnr charsetnr = Charsetnr{kBinaryCharsetnr},
      Decimals decimals = Decimals{kDecimalsNotSpecified}) {
    columns_.add(
        std::move(name),
        std::move(table),
        type,
        flags,
        charsetnr.value,
        decimals.value);
    return *this;
  }

  // Single use: both leave the builder empty.
  RowFields build() {
    return columns_.build();
  }
  std::shared_ptr<RowFields> buildShared() {
    return std::make_shared<RowFields>(build());
  }

 private:
  detail::RowFieldsColumns columns_;
};

// TRANSITIONAL -- builds a RowFields with no charsetnrs at all, for protocols
// that cannot supply them. See RowFields::builderWithoutCharsets().
class RowFields::BuilderWithoutCharsets {
 public:
  BuilderWithoutCharsets& column(
      std::string name,
      enum_field_types type,
      std::string table = "",
      uint64_t flags = 0,
      Decimals decimals = Decimals{kDecimalsNotSpecified}) {
    columns_.add(
        std::move(name),
        std::move(table),
        type,
        flags,
        std::nullopt,
        decimals.value);
    return *this;
  }

  // Single use: both leave the builder empty.
  RowFields build() {
    return columns_.build();
  }
  std::shared_ptr<RowFields> buildShared() {
    return std::make_shared<RowFields>(build());
  }

 private:
  detail::RowFieldsColumns columns_;
};

inline RowFields::Builder RowFields::builder() {
  return {};
}

inline RowFields::BuilderWithoutCharsets RowFields::builderWithoutCharsets() {
  return {};
}

std::chrono::system_clock::time_point parseDateTime(
    folly::StringPiece datetime,
    enum_field_types date_type);

std::chrono::microseconds parseTimeOnly(
    folly::StringPiece mysql_time,
    enum_field_types field_type);

// A RowBlock holds the raw data from part of a MySQL result set.  It
// corresponds roughly to one set of rows (out of potentially many).
// The size of a block can vary based on the whims of the MySQL client
// and server, so don't count on how many you may get or how many rows
// are in each.
//
// Data layout tries to be efficient; values are packed into memory
// tightly and accessed via StringPieces and Rows that point into this
// block.  This prevents frequent allocations.  See data comments for
// details.
//
// Iterator access is provided as well, allowing for use cases like
//
// for (const auto& row : row_block) {
//   ...
// }

// One cell handed to RowBlock::addRow().  Implicitly constructible from
// everything a StorageRow can hold, plus nullptr for SQL NULL, so a row reads
// as a plain braced list: `{1, "alice", nullptr}`.
//
// Strings are copied into the row's own storage by StorageRow::appendValue, so
// passing a temporary (e.g. folly::to<std::string>(x)) is safe -- it outlives
// the addRow() call it appears in.
class CellValue {
 public:
  /* implicit */ CellValue(std::nullptr_t) : value_(Null{}) {}
  /* implicit */ CellValue(folly::StringPiece v) : value_(v) {}
  /* implicit */ CellValue(const char* v)
      : value_(v == nullptr ? Value{Null{}} : Value{folly::StringPiece(v)}) {}
  /* implicit */ CellValue(const std::string& v)
      : value_(folly::StringPiece(v)) {}
  /* implicit */ CellValue(bool v) : value_(v) {}
  /* implicit */ CellValue(double v) : value_(v) {}

  // Integer literals would be ambiguous across the alternatives below, so they
  // are pinned to the signed/unsigned 64-bit forms StorageRow stores.
  template <typename T>
    requires std::signed_integral<T>
  /* implicit */ CellValue(T v) : value_(static_cast<int64_t>(v)) {}

  // bool satisfies std::unsigned_integral, so it has to be excluded explicitly
  // or it would match here instead of the bool overload above.
  template <typename T>
    requires std::unsigned_integral<T> && (!std::is_same_v<T, bool>)
  /* implicit */ CellValue(T v) : value_(static_cast<uint64_t>(v)) {}

  // An empty optional is SQL NULL; a present one takes the contained value,
  // dispatching to the matching constructor above. One template covers
  // std::optional and folly::Optional (both expose has_value()/operator*).
  // As with the other string-holding constructors, a present string value is
  // referenced, not copied, so the source optional must outlive this CellValue.
  template <typename Opt>
    requires requires(const Opt& o) {
      o.has_value();
      *o;
    }
  /* implicit */ CellValue(const Opt& opt)
      : value_(opt.has_value() ? CellValue(*opt).value_ : Value{Null{}}) {}

 private:
  struct Null {};

  void appendTo(StorageRow& row) const {
    std::visit(
        [&row](const auto& v) {
          if constexpr (std::is_same_v<std::decay_t<decltype(v)>, Null>) {
            row.appendNull();
          } else {
            row.appendValue(v);
          }
        },
        value_);
  }

  using Value =
      std::variant<Null, folly::StringPiece, bool, int64_t, uint64_t, double>;

  Value value_;

  friend class RowBlock;
};

class RowBlock {
 public:
  class Iterator;

  explicit RowBlock(std::shared_ptr<RowFields> row_fields)
      : row_fields_info_(std::move(row_fields)) {}

  ~RowBlock() {}

  // Given a row N and column M, return a T corresponding to the Nth
  // row's Mth column.
  template <typename T>
  T getField(size_t row, size_t field_num) const;

  // Like above, but converting to the specified type T (using
  // folly::to<T>(StringPiece)).
  template <typename T>
  T getField(size_t row, folly::StringPiece field_name) const;

  // Is this field NULL?
  bool isNull(size_t row, size_t field_num) const {
    detail::checkIndexInRange("row index", row, rows_.size());
    detail::checkIndexInRange(
        "field index", field_num, row_fields_info_->numFields());
    return rows_[row].isNull(field_num);
  }

  // Ditto, but by name.
  bool isNull(size_t row, folly::StringPiece field_name) const {
    return isNull(row, row_fields_info_->fieldIndex(field_name));
  }

  // Get the MySQL type of the field.
  enum_field_types getFieldType(size_t field_num) const {
    return row_fields_info_->getFieldType(field_num);
  }

  // Ditto, but by name.
  enum_field_types getFieldType(folly::StringPiece field_name) const {
    return row_fields_info_->getFieldType(field_name);
  }

  // Get the MySQL flags of the field.
  uint64_t getFieldFlags(size_t field_num) const {
    return row_fields_info_->getFieldFlags(field_num);
  }

  // Ditto, but by name.
  uint64_t getFieldFlags(folly::StringPiece field_name) const {
    return row_fields_info_->getFieldFlags(field_name);
  }

  // Get the MySQL decimals of the field. See RowFields::getFieldDecimals.
  unsigned int getFieldDecimals(size_t field_num) const {
    return row_fields_info_->getFieldDecimals(field_num);
  }

  // Ditto, but by name.
  unsigned int getFieldDecimals(folly::StringPiece field_name) const {
    return row_fields_info_->getFieldDecimals(field_name);
  }

  // Access the Nth row of this row block as a Row object.
  Row getRow(size_t n) const {
    return Row(this, n);
  }

  RowFields* getRowFields() {
    return row_fields_info_.get();
  }
  // What is the name of the i'th column in the result set?
  folly::StringPiece fieldName(size_t i) const {
    return row_fields_info_->fieldName(i);
  }

  // What is the index of the column labeled n
  std::optional<size_t> fieldIndexOpt(folly::StringPiece n) const {
    return row_fields_info_->fieldIndexOpt(n);
  }
  size_t fieldIndex(folly::StringPiece n) const {
    return row_fields_info_->fieldIndex(n);
  }

  // Is our rowblock empty?
  bool empty() const {
    return rows_.empty();
  }

  // How many fields and rows do we have?
  size_t numFields() const {
    return row_fields_info_->numFields();
  }

  // How many rows are in this RowBlock?
  size_t numRows() const {
    return rows_.size();
  }

  // Iterator support.  Allows iteration over the rows in this block.
  // Like Row::Iterator, this is mainly for simple range-based for
  // iteration.
  class Iterator : public boost::iterator_facade<
                       Iterator,
                       const Row,
                       boost::single_pass_traversal_tag,
                       const Row> {
   public:
    Iterator(const RowBlock* row_block, size_t row_number)
        : row_block_(row_block), current_row_number_(row_number) {}

    explicit Iterator() = default;

    void increment() {
      ++current_row_number_;
    }
    const Row dereference() const {
      return row_block_->getRow(current_row_number_);
    }
    bool equal(const Iterator& other) const {
      return (
          row_block_ == other.row_block_ &&
          current_row_number_ == other.current_row_number_);
    }

   private:
    const RowBlock* row_block_;
    size_t current_row_number_;
  };

  Iterator begin() const {
    return Iterator(this, 0);
  }

  Iterator end() const {
    return Iterator(this, numRows());
  }

  // Adds one complete row.  Preferred over the startRow()/appendValue()/
  // finishRow() sequence below: a row is complete or it is not, so there is no
  // partially built state to abandon and the column count is checked at a
  // single, well-defined point.
  //
  //   block.addRow({1, "alice"});      // arity checked here
  //   block.addRow({2, nullptr});      // nullptr is SQL NULL
  //   block.addRow(values);            // any range of CellValue-convertibles
  //
  // Throws std::out_of_range if the row does not have exactly numFields()
  // values, and std::logic_error if a startRow() is still open. out_of_range
  // derives from logic_error, so catching logic_error covers both.
  void addRow(StorageRow&& row) {
    if (current_row_.has_value()) [[unlikely]] {
      throwAddRowWhileRowOpen();
    }
    if (row.count() != row_fields_info_->numFields()) [[unlikely]] {
      throwWrongColumnCount(row.count());
    }
    rows_.push_back(std::move(row));
  }

  void addRow(std::initializer_list<CellValue> values) {
    addRowFromRange(values.begin(), values.end(), values.size());
  }

  template <typename Range>
    requires(
        !std::is_same_v<std::decay_t<Range>, StorageRow> &&
        !std::is_same_v<std::decay_t<Range>, RowBlock>)
  void addRow(const Range& values) {
    addRowFromRange(
        std::begin(values),
        std::end(values),
        static_cast<size_t>(
            std::distance(std::begin(values), std::end(values))));
  }

  // Functions called when building a RowBlock.  Not for general use.
  //
  // Prefer addRow() above.  These are deprecated in favor of it and remain
  // only for callers not yet migrated.
  //
  // Contract: each startRow() is paired with exactly one finishRow(), with
  // exactly numFields() values appended in between.  Violations throw rather
  // than abort:
  //   - startRow() twice without an intervening finishRow()
  //   - appendValue()/appendNull()/finishRow() before startRow()
  //   - appending more than numFields() values (std::out_of_range)
  //   - finishRow() with fewer than numFields() values appended
  //
  // A row becomes visible to readers only once finishRow() succeeds, so a
  // partially built row is never observable.  This is a behavior change: these
  // misuses previously either aborted the process or (for a row abandoned
  // without finishRow()) left the partial row in place.
  //
  // KNOWN GAP: omitting finishRow() on the final row is not detected -- that
  // row is silently dropped.  Omitting it on any earlier row is caught by the
  // next startRow().  An RAII-scoped builder that makes the pairing automatic
  // is planned to replace this API; prefer it once available.
  [[deprecated(
      "ADVICE: prefer RowBlock::addRow(); this build API is "
      "deprecated")]]
  void startRow() {
    if (current_row_) {
      throw std::logic_error(
          "Attempting to start a row without finishing the previous one.  "
          "Call finishRow() for each startRow()");
    }

    current_row_ = StorageRow(row_fields_info_->numFields());
  }

  [[deprecated(
      "ADVICE: prefer RowBlock::addRow(); this build API is "
      "deprecated")]]
  void finishRow() {
    if (!current_row_) {
      throw std::logic_error(
          "Attempting to finish a row that hasn't been started.  "
          "Call startRow() before finishRow()");
    }

    auto count = current_row_->count();
    auto expected = row_fields_info_->numFields();
    if (count != expected) {
      throw std::logic_error(
          fmt::format("row has {} fields, expected {}", count, expected));
    }

    rows_.push_back(std::move(*current_row_));
    current_row_ = std::nullopt;
  }

  template <typename T>
  [[deprecated(
      "ADVICE: prefer RowBlock::addRow(); this build API is "
      "deprecated")]]
  void appendValue(T value) {
    checkRowStartedAndCapacity();
    current_row_->appendValue(std::forward<T>(value));
  }

  // Special override for folly::StringPiece to match existing code
  [[deprecated(
      "ADVICE: prefer RowBlock::addRow(); this build API is "
      "deprecated")]]
  void appendValue(folly::StringPiece value) {
    checkRowStartedAndCapacity();
    current_row_->appendValue(value);
  }

  [[deprecated(
      "ADVICE: prefer RowBlock::addRow(); this build API is "
      "deprecated")]]
  void appendNull() {
    checkRowStartedAndCapacity();
    current_row_->appendNull();
  }

  // Let the compiler make our move operations.  We disallow copies below.
  RowBlock(RowBlock&&) = default;
  RowBlock& operator=(RowBlock&&) = default;

 private:
  time_t getDateField(size_t row, size_t field_num) const;

  bool isDate(size_t /*row*/, size_t field_num) const {
    const auto fieldType = getFieldType(field_num);
    return (fieldType == MYSQL_TYPE_TIMESTAMP) ||
        (fieldType == MYSQL_TYPE_DATETIME) || (fieldType == MYSQL_TYPE_DATE);
  }

  void checkRowStartedAndCapacity() {
    if (!current_row_) [[unlikely]] {
      throwRowNotStarted();
    }
    if (current_row_->count() >= row_fields_info_->numFields()) [[unlikely]] {
      throwNoMoreCapacity();
    }
  }

  // Builds the row up front so the arity check happens before anything is
  // published, and so a bad row leaves the block completely untouched.
  template <typename Iter>
  void addRowFromRange(Iter first, Iter last, size_t count) {
    if (current_row_.has_value()) [[unlikely]] {
      throwAddRowWhileRowOpen();
    }
    if (count != row_fields_info_->numFields()) [[unlikely]] {
      throwWrongColumnCount(count);
    }
    StorageRow row(count);
    for (; first != last; ++first) {
      CellValue(*first).appendTo(row);
    }
    rows_.push_back(std::move(row));
  }

  [[noreturn]] static void throwRowNotStarted();
  [[noreturn]] void throwNoMoreCapacity();
  [[noreturn]] static void throwAddRowWhileRowOpen();
  [[noreturn]] void throwWrongColumnCount(size_t actual) const;

  std::vector<StorageRow> rows_;
  std::optional<StorageRow> current_row_;

  // Storage for strings when we convert a column to std::string_view or
  // folly::StringPiece.  Must be `mutable` because this can occur on a
  // `getField()` call which is a `const` method.
  using RowColumnKey = std::pair<size_t, size_t>;
  mutable StringStore<RowColumnKey> string_store_;

  // field_name_map_ and field_names_ are owned by the RowFields shared between
  // RowBlocks of same query
  std::shared_ptr<RowFields> row_fields_info_;

  RowBlock(const RowBlock&) = delete;
  RowBlock& operator=(const RowBlock&) = delete;
};

class EphemeralRowFields {
 public:
  explicit EphemeralRowFields(std::unique_ptr<InternalRowMetadata> metadata)
      : metadata_(std::move(metadata)) {}

  ~EphemeralRowFields() = default;

  EphemeralRowFields(EphemeralRowFields const&) = delete;
  EphemeralRowFields& operator=(EphemeralRowFields const&) = delete;

  EphemeralRowFields(EphemeralRowFields&&) = default;
  EphemeralRowFields& operator=(EphemeralRowFields&&) = default;

  size_t numFields() const {
    return metadata_->numFields();
  }

  std::optional<size_t> fieldIndexOpt(const char* field_name) const {
    return fieldIndexOpt(std::string_view(field_name));
  }

  std::optional<size_t> fieldIndexOpt(folly::StringPiece field_name) const {
    return fieldIndexOpt(
        std::string_view(field_name.data(), field_name.size()));
  }

  std::optional<size_t> fieldIndexOpt(std::string_view field_name) const {
    for (size_t ii = 0; ii < numFields(); ii++) {
      if (field_name == metadata_->getFieldName(ii)) {
        return ii;
      }
    }

    return std::nullopt;
  }

  size_t fieldIndex(const char* field_name) const {
    return fieldIndex(std::string_view(field_name));
  }

  size_t fieldIndex(folly::StringPiece field_name) const {
    return fieldIndex(std::string_view(field_name.data(), field_name.size()));
  }

  size_t fieldIndex(std::string_view field_name) const {
    if (auto opt = fieldIndexOpt(field_name); opt) {
      return *opt;
    }

    throw std::out_of_range(fmt::format("Invalid field: {}", field_name));
  }

  enum_field_types fieldType(size_t index) const {
    detail::checkIndexInRange("field index", index, metadata_->numFields());
    return metadata_->getFieldType(index);
  }

  // See RowFields::getFieldDecimals.
  unsigned int fieldDecimals(size_t index) const {
    detail::checkIndexInRange("field index", index, metadata_->numFields());
    return metadata_->getFieldDecimals(index);
  }

  template <typename StringLike = folly::StringPiece>
  StringLike fieldName(size_t index) const {
    auto res = metadata_->getFieldName(index);
    return StringLike(res.data(), res.size());
  }

  std::shared_ptr<RowFields> makeBufferedFields() const;

 private:
  std::unique_ptr<InternalRowMetadata> metadata_;
};

class EphemeralRow {
 public:
  EphemeralRow() = default;

  EphemeralRow(
      std::unique_ptr<InternalRow> row,
      std::shared_ptr<EphemeralRowFields> row_fields)
      : row_(std::move(row)), row_fields_(std::move(row_fields)) {}

  ~EphemeralRow() = default;

  EphemeralRow(EphemeralRow const&) = delete;
  EphemeralRow& operator=(EphemeralRow const&) = delete;

  EphemeralRow(EphemeralRow&&) = default;
  EphemeralRow& operator=(EphemeralRow&&) = default;

  // Beginning simple, just give the basic indexing.
  InternalRow::Type getType(size_t col) const;

  bool getBool(size_t col) const;

  int64_t getInt64(size_t col) const;

  uint64_t getUInt64(size_t col) const;

  double getDouble(size_t col) const;

  folly::StringPiece getString(size_t col) const;

  // Helper function to convert the data to string format - note this can be
  // expensive as it always generates a new string.  This is useful for logging
  // and other non-performance critical code.
  std::string convertToString(size_t col) const;

  bool isNull(size_t col) const;

  int numFields() const;

  // Calculates the number of bytes in the row data
  uint64_t calculateRowLength() const;

  const EphemeralRowFields& getRowFields() const {
    return *row_fields_;
  }

  // Attempt to convert the column specified by colName to the type specified in
  // the template.  Note: calling this on a null column will throw an exception
  template <typename DataType>
  DataType convertTo(folly::StringPiece colName) const {
    return convertTo<DataType>(getRowFields().fieldIndex(colName));
  }

  // Attempt to convert the column specified by colIndex to the type specified
  // in the template.  Note: calling this on a null column will throw an
  // exception, so check for null first
  template <typename DataType>
  DataType convertTo(size_t colIndex) const {
    switch (getType(colIndex)) {
      case common::mysql_client::InternalRow::Type::Null:
        throw std::runtime_error(
            fmt::format(
                "Column {} ({}) has a null value",
                colIndex,
                getRowFields().fieldName(colIndex)));
      case common::mysql_client::InternalRow::Type::Bool:
        return folly::to<DataType>(getBool(colIndex));
      case common::mysql_client::InternalRow::Type::Int64:
        return folly::to<DataType>(getInt64(colIndex));
      case common::mysql_client::InternalRow::Type::UInt64:
        return folly::to<DataType>(getUInt64(colIndex));
      case common::mysql_client::InternalRow::Type::Double:
        return folly::to<DataType>(getDouble(colIndex));
      case common::mysql_client::InternalRow::Type::String:
        return folly::to<DataType>(getString(colIndex));
      default:
        throw std::logic_error{
            fmt::format("Received unknown type {}", colIndex)};
    }
  }

 private:
  std::unique_ptr<InternalRow> row_;
  std::shared_ptr<EphemeralRowFields> row_fields_;
};

// Declarations of specializations and trivial implementations.
template <>
bool RowBlock::getField(size_t row, size_t field_num) const;

template <>
int64_t RowBlock::getField(size_t row, size_t field_num) const;

template <>
uint64_t RowBlock::getField(size_t row, size_t field_num) const;

template <>
double RowBlock::getField(size_t row, size_t field_num) const;

template <>
std::string RowBlock::getField(size_t row, size_t field_num) const;

template <>
std::string_view RowBlock::getField(size_t row, size_t field_num) const;

template <>
folly::fbstring RowBlock::getField(size_t row, size_t field_num) const;

template <>
folly::StringPiece RowBlock::getField(size_t row, size_t field_num) const;

template <>
time_t RowBlock::getField(size_t row, size_t field_num) const;

template <>
std::chrono::system_clock::time_point RowBlock::getField(
    size_t row,
    size_t field_num) const;

template <>
std::chrono::microseconds RowBlock::getField(size_t row, size_t field_num)
    const;

template <typename T>
T RowBlock::getField(size_t row, size_t field_num) const {
  return folly::to<T>(getField<folly::StringPiece>(row, field_num));
}

template <typename T>
T RowBlock::getField(size_t row, folly::StringPiece field_name) const {
  return getField<T>(row, row_fields_info_->fieldIndex(field_name));
}

template <typename T, typename L>
T Row::get(const L& l) const {
  return row_block_->getField<T>(row_number_, l);
}

template <typename T, typename L>
T Row::get(const L& l, T d) const {
  if (isNull(l)) {
    return d;
  }
  return get<T>(l);
}

// Similar to above but will return as an optional which will be empty if the
// column is null
template <typename T, template <typename> class Optional, typename L>
Optional<T> Row::getOptional(const L& l) const {
  if (isNull(l)) {
    return {};
  }
  return Optional(get<T>(l));
}

} // namespace facebook::common::mysql_client
