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

#include <cstdint>
#include <limits>
#include <vector>

#include "hphp/runtime/base/string-data.h"

#include "hphp/util/functional.h"
#include "hphp/util/hash-map.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

template<class V, class ExtraType = int32_t>
struct FixedStringMap {
  explicit FixedStringMap(int num) : m_table(nullptr) { init(num); }
  FixedStringMap() : m_maskAndConst(0), m_table(nullptr) {}
  ~FixedStringMap() { clear(); }

  FixedStringMap(const FixedStringMap&) = delete;
  const FixedStringMap& operator=(const FixedStringMap&) = delete;

  void clear();
  void init(int num, uint32_t numExtraBytes = 0);
  V* find(const StringData* s) const;

  template<class Iter>
  void addFrom(Iter begin, Iter end);

  void* extraData() { return m_table; }
  const void* extraData() const { return m_table; }
  ExtraType& extra() { return m_extra; }
  const ExtraType& extra() const { return m_extra; }

  ExtraType size() const { return m_extra; }

  static constexpr ptrdiff_t tableOff() {
    return offsetof(FixedStringMap, m_table);
  }
  static constexpr ptrdiff_t sizeOff() {
    return offsetof(FixedStringMap, m_extra);
  }
  static constexpr size_t sizeSize() {
    return sizeof(m_extra);
  }

private:
  void add(const StringData* s, const V& v);

private:
  static constexpr uint32_t kHashConstsSize = 10;
  static constexpr uint32_t kMaskSize = 31;
  static constexpr uint32_t kMaskSizePerfect = kMaskSize - kHashConstsSize;

  bool isPerfectHash() const { return m_maskAndConst >> kMaskSize; }
  uint32_t mask() const {
    auto const size = isPerfectHash() ? kMaskSizePerfect : kMaskSize;
    return m_maskAndConst & ((1u << size) - 1);
  }
  uint32_t hashConsts() const {
    assertx(isPerfectHash());
    return (m_maskAndConst >> kMaskSizePerfect) & ((1 << kHashConstsSize) - 1);
  }
  void setMaskAndConsts(uint32_t mask, uint32_t consts = 0);
  uint32_t capacity() const { return mask() + 1; }

private:
  struct Elm {
    PackedStringPtr sd;
    V data;
  };

  /*
   * There are two different hashing strategy this table employs. Based on the
   * hashing strategy the way m_maskAndConst is organized shown below.
   * 1) Perfect hashing
   * bit 31: 0
   * bits 30-21: consts
   * bits 20:0: mask
   *
   * 2) Linear probing
   * bit 31: 1
   * bits 30-0: mask
   *
   * This implies that the capacity of the maps is limited by which hashing
   * solution we determine.
   */

  /*
   * The fields we need here are just m_mask and m_table.  This would leave 4
   * byte padding hole, though, which some users (e.g. IndexedStringMap) might
   * have a use for, so we expose it as a slot for our users.
   */
  uint32_t  m_maskAndConst;
  ExtraType m_extra;  // not ours
  Elm*      m_table;
};

//////////////////////////////////////////////////////////////////////

template<class T, class ExtraType = int32_t>
struct FixedStringToSlotMapBuilder {
  using V = Slot;
  using Map =
    hphp_hash_map<const StringData*, V, string_data_hash, string_data_same>;
  using FSMap = FixedStringMap<V, ExtraType>;

  using const_iterator = typename Map::const_iterator;
  using iterator = typename Map::iterator;

  iterator find(const StringData* key) { return m_map.find(key); }
  iterator begin()                     { return m_map.begin(); }
  iterator end()                       { return m_map.end(); }
  V        size() const                { return m_list.size(); }

  const_iterator find(const StringData* key) const {
    return m_map.find(key);
  }
  const_iterator begin() const { return m_map.begin(); }
  const_iterator end()   const { return m_map.end(); }

  T& operator[](V idx);
  const T& operator[](V idx) const;

  /*
   * Add an object to the position on the end, and allow lookup by `name'.
   */
  void add(const StringData* name, const T& t);

  /*
   * Add an object that occupies an index but can't be located by name.
   */
  void addUnnamed(const T& t);

  /*
   * Create a FixedStringMap from the builder.
   */
  void create(FSMap& map);

private:
  std::vector<T> m_list;
  Map m_map;
};

//////////////////////////////////////////////////////////////////////

}

#include "hphp/runtime/vm/fixed-string-map-inl.h"
