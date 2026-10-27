/*
  +----------------------------------------------------------------------+
  | HipHop for PHP                                                       |
  +----------------------------------------------------------------------+
  | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
*/

#pragma once

#include <fmt/format.h>

#include "hphp/runtime/base/tv-val.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/unaligned-typed-value.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/util/type-traits.h"

#include "folly/Range.h"

#include <type_traits>

namespace HPHP {

void tvDecRefGen(TypedValue);
void tvDecRefCountable(TypedValue);

namespace tv_layout {

/* A TV layout represents a integer-indexed aggregate of TypedValues.
 *
 * The intended way to use one is to reinterpret_cast a region of memory
 * to the appropriate tv_layout type.
 *
 * The type must implement the following interface and extend LayoutBase:
 *
 * Member types:
 * - index_t: some integral type used to index the container
 * - iterator, const_iterator: a (const) forward iterator over the container,
 *                             producing TypedValues with the additional
 *                             constraint that tv_lval{iter} is well-formed as
 *                             long as the iterator is dereferencable
 * - quick_index: some type, implicitly coercible to index_t, designed to allow
 *                faster access to the collection. This may be the same as
 *                index_t
 *
 * Static members:
 * size_t max_index
 *     The maximum index in the container accessible both by index_t and
 *     quick_index
 *
 * Static functions:
 * size_t sizeFor(index_t size);
 *     Produces the size in bytes to be allocated for a container of the given size.
 *     This must be aligned to a multiple of 16 as there are several optimizations
 *     that rely on this fact.
 *
 * tv_val_offset offsetOf(index_t idx) const;
 * tv_val_offset offsetOf(quick_index idx) const; (optional)
 *     Produces a tv_val_offset to the given index's typed value.
 *
 * quick_index quickIndex(index_t idx);
 *     Produces the quick index corresponding to the given index
 *
 * static index_t offset2Idx(size_t offset);
 *     Produces the index of an element corresponding to the given byte offset
 *
 * static void setInvariantsAfterGrow(char *data, size_t size_old_data, size_t size_new_data);
 *     Called after the region of data has been expanded from size_old_data
 *     to size_new_data, so that the particular layout class can format the newly allocated
 *     portion as need to meet its invariants.
 *
 * Member functions:
 * void init(index_t size);
 *    Establishes any invariants the container needs to operate,
 *    for a container of the given size
 *
 * bool checkInvariants(index_t size) const;
 *     Asserts if any invariant of the container is not met
 *
 * void scan(quick_index size, type_scan::Scanner&) const;
 *     Scans the countable values in the container
 *
 * void release(quick_index size);
 *     Decrefs the countable values in the container
 *
 * iterator iteratorAt(index_t pos);
 * const_iterator iteratorAt(index_t pos);
 *     Produce an iterator starting at the given index
 */

template <typename Impl,
          typename Iterator,
          typename ConstIterator,
          typename Index,
          typename QuickIndex>
struct LayoutBase {
  using iterator = Iterator;
  using const_iterator = ConstIterator;

  using index_t = Index;
  using quick_index = QuickIndex;
  static_assert(std::is_integral<index_t>::value, "");

  Impl& impl() { return *static_cast<Impl*>(this); }
  const Impl& impl() const { return *static_cast<const Impl*>(this); }

  /*
   * Produce a range over the container, for use in for-each loops
   */
  folly::Range<iterator>
  range(index_t begin, index_t end) {
    return folly::Range<iterator>{
      impl().iteratorAt(begin), impl().iteratorAt(end)
    };
  }

  folly::Range<const_iterator>
  range(index_t begin, index_t end) const {
    return folly::Range<const_iterator>{
      impl().iteratorAt(begin), impl().iteratorAt(end)
    };
  }

  /*
   * Iterate over the container _by tv_val_ as opposed to by TypedValue
   */
  template <typename Iter>
  std::enable_if_t<is_invocable<Iter, tv_lval>::value>
  foreach(index_t start, index_t len, Iter&& iter) {
    auto it = impl().iteratorAt(start);
    auto end = impl().iteratorAt(start + len);
    while (it != end) {
      iter(tv_lval{it});
      ++it;
    }
  }
  template <typename Iter>
  std::enable_if_t<is_invocable<Iter, tv_lval>::value>
  foreach(index_t len, Iter&& iter) {
    foreach(0, len, iter);
  }

  template <typename Iter>
  std::enable_if_t<is_invocable<Iter, tv_rval>::value>
  foreach(index_t start, index_t len, Iter&& iter) const {
    auto it = impl().iteratorAt(start);
    auto end = impl().iteratorAt(start + len);
    while (it != end) {
      iter(tv_rval{it});
      ++it;
    }
  }

  template <typename Iter>
  std::enable_if_t<is_invocable<Iter, tv_rval>::value>
  foreach(index_t len, Iter&& iter) const {
    foreach(0, len, iter);
  }

  template <typename T>
  tv_lval at(T idx) {
    return impl().offsetOf(idx).apply(reinterpret_cast<char*>(this));
  }

  template <typename T>
  tv_rval at(T idx) const {
    return impl().offsetOf(idx).apply(reinterpret_cast<const char*>(this));
  }
};

////////////////////////////////////////////////////////////////////////////////

/*
 * Unaligned typed-value layout
 *
 * This implements a flavor of an array layout storing unaligned typed values, which
 * occupy 9 bytes and are stored one after another (no padding for alignment between
 * elements.
 *
 *  ______________9 * N bytes________________
 * /                                         \
 *  9 bytes  9 bytes   ............   9 bytes
 * /       \/       \                /       \
 * +--------+--------+---------------+--------+
 * | v1  |t1| v1  |t2| ............  | vN  |tN|
 * +--------+--------+---------------+--------+
 *
 */

namespace detail_utv {

template <bool is_const>
struct iterator_impl {
  using tv_val_t = tv_val<is_const>;

  // iterator_traits member types
  using value_type = UnalignedTypedValue;
  using reference = UnalignedTypedValue&;
  using pointer = void;
  using difference_type = ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  /* implicit */ iterator_impl(UnalignedTypedValue *utv)
    : utv(utv)
  {}

  UnalignedTypedValue operator*() const {
    return *utv;
  }

  operator tv_val_t() const {
    return utv;
  }

  iterator_impl& operator++() {
    utv += 1;
    return *this;
  }

  iterator_impl operator++(int) {
    auto const ret = *this;
    ++(*this);
    return ret;
  }

  bool operator==(const iterator_impl& other) const {
    return utv == other.utv;
  }

  bool operator!=(const iterator_impl& other) const {
    return !(*this == other);
  }

private:
  UnalignedTypedValue *utv;
};

}

struct UnalignedTVLayout :
  public LayoutBase<UnalignedTVLayout,
                    detail_utv::iterator_impl<false>,
                    detail_utv::iterator_impl<true>,
                    uint16_t,
                    uint16_t> {

  static size_t constexpr max_index = UINT16_MAX - 1;

  static size_t constexpr sizeFor(index_t len) {
    auto const bytes = len * sizeof(UnalignedTypedValue);
    return bytes;
  }

  // Since index_t == quick_index, don't define functions on the second type

  static tv_val_offset offsetOf(index_t idx) {
    auto const base = idx * sizeof(UnalignedTypedValue);
    return {
      ptrdiff_t(base + offsetof(UnalignedTypedValue, m_type)),
      ptrdiff_t(base + offsetof(UnalignedTypedValue, m_data))
    };
  }

  static quick_index quickIndex(size_t idx) {
    return idx;
  }

  static index_t offset2Idx(size_t offset) {
    return offset / sizeof(UnalignedTypedValue);
  }

  static void setInvariantsAfterGrow(char *, size_t, size_t) {
    return;
  }

  bool checkInvariants(index_t) const {
    return true;
  }

  void scan(index_t count, type_scan::Scanner& scanner) const {
    foreach(count, [&](const auto &elm) {
      if (isRefcountedType(elm.type())) {
        scanner.scan(elm.val().pcnt);
      }
    });
  }

  void release(index_t count) {
    foreach(count, [](auto elm) {
      tvDecRefGen(*elm);
    });
  }

  iterator iteratorAt(index_t pos) {
    auto const elm = &as_utv()[pos];
    return iterator{elm};
  }

  const_iterator iteratorAt(index_t pos) const {
    auto const elm = &as_utv()[pos];
    return const_iterator{const_cast<UnalignedTypedValue*>(elm)};
  }

private:

  UnalignedTypedValue* as_utv() {
    return reinterpret_cast<UnalignedTypedValue*>(this);
  }

  const UnalignedTypedValue* as_utv() const {
    return const_cast<UnalignedTVLayout*>(this)->as_utv();
  }

};
} // namespace tv_layout
} // namespace HPHP
