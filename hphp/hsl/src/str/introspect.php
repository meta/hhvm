<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

namespace HH\Lib\Str;

use namespace HH\Lib\{_Private, _Private\_Str};

/**
 * Returns `< 0` if `$string1` is less than `$string2`, `> 0` if `$string1` is
 * greater than `$string2`, and `0` if they are equal.
 *
 * For a case-insensitive comparison, see `Str\compare_ci()`.
 *
 * @guide /hack/built-in-types/string
 */
function compare(
  string $string1,
  string $string2,
)[]: int {
  return _Str\strcoll_l($string1, $string2);
}

/**
 * Returns `< 0` if `$string1` is less than `$string2`, `> 0` if `$string1` is
 * greater than `$string2`, and `0` if they are equal (case-insensitive).
 *
 * For a case-sensitive comparison, see `Str\compare()`.
 *
 * @guide /hack/built-in-types/string
 */
function compare_ci(
  string $string1,
  string $string2,
)[]: int {
  return _Str\strcasecmp_l($string1, $string2);
}

/**
 * Returns whether the "haystack" string contains the "needle" string.
 *
 * An optional offset determines where in the haystack the search begins. If the
 * offset is negative, the search will begin that many characters from the end
 * of the string. If the offset is out-of-bounds, a ViolationException will be
 * thrown.
 *
 * - To get the position of the needle, see `Str\search()`.
 * - To search for the needle case-insensitively, see `Str\contains_ci()`.
 *
 * @guide /hack/built-in-types/string
 */
function contains(
  string $haystack,
  string $needle,
  int $offset = 0,
)[]: bool {
  if ($needle === '') {
    if ($offset === 0) {
      return true;
    }
    $length = length($haystack);
    if ($offset > $length || $offset < -$length) {
      throw new \InvalidArgumentException(
        format('Offset %d out of bounds for length %d', $offset, $length)
      );
    }
    return true;
  }
  return search($haystack, $needle, $offset) !== null;
}

/**
 * Returns whether the "haystack" string contains the "needle" string
 * (case-insensitive).
 *
 * An optional offset determines where in the haystack the search begins. If the
 * offset is negative, the search will begin that many characters from the end
 * of the string. If the offset is out-of-bounds, a ViolationException will be
 * thrown.
 *
 * - To search for the needle case-sensitively, see `Str\contains()`.
 * - To get the position of the needle case-insensitively, see `Str\search_ci()`.
 *
 * @guide /hack/built-in-types/string
 */
function contains_ci(
  string $haystack,
  string $needle,
  int $offset = 0,
)[]: bool {
  if ($needle === '') {
    if ($offset === 0) {
      return true;
    }
    $length = length($haystack);
    if ($offset > $length || $offset < -$length) {
      throw new \InvalidArgumentException(
        format('Offset %d out of bounds for length %d', $offset, $length)
      );
    }
    return true;
  }
  return search_ci($haystack, $needle, $offset) !== null;
}

/**
 * Returns whether the string ends with the given suffix.
 *
 * For a case-insensitive check, see `Str\ends_with_ci()`.
 *
 * @guide /hack/built-in-types/string
 */
function ends_with(
  string $string,
  string $suffix,
)[]: bool {
  return _Str\ends_with_l($string, $suffix);
}

/**
 * Returns whether the string ends with the given suffix (case-insensitive).
 *
 * For a case-sensitive check, see `Str\ends_with()`.
 *
 * @guide /hack/built-in-types/string
 */
function ends_with_ci(
  string $string,
  string $suffix,
)[]: bool {
  return _Str\ends_with_ci_l($string, $suffix);
}

/**
 * Returns the length of the given string represented in number of bytes.
 *
 * This function is `O(1)`: it always returns the number of bytes in the string,
 * even if a byte is null. For example, `Str\length("foo\0bar")` is 7, not 3.
 *
 * See `Str\length_l()` for the length in characters.
 *
 * @guide /hack/built-in-types/string
 */
function length(
  string $string,
)[]: int {
  // This is functionally equivalent to `Str\length_l()` with the bytes locale,
  // but HHVM's JIT has specific optimizations for `\strlen()`, and it avoids
  // the virtual dispatch between the {Byte/Libc/ICU}Ops C++ classes.
  //
  // Let's do things the fast way :)
  return \strlen($string);
}

/**
 * Returns the first position of the "needle" string in the "haystack" string,
 * or null if it isn't found.
 *
 * An optional offset determines where in the haystack the search begins. If the
 * offset is negative, the search will begin that many characters from the end
 * of the string. If the offset is out-of-bounds, a ViolationException will be
 * thrown.
 *
 * - To simply check if the haystack contains the needle, see `Str\contains()`.
 * - To get the case-insensitive position, see `Str\search_ci()`.
 * - To get the last position of the needle, see `Str\search_last()`.
 *
 * Previously known in PHP as `strpos`.
 *
 * @guide /hack/built-in-types/string
 */
function search(
  string $haystack,
  string $needle,
  int $offset = 0,
)[]: ?int {
  $position = _Str\strpos_l($haystack, $needle, $offset);
  if ($position < 0) {
    return null;
  }
  return $position;
}

/**
 * Returns the first position of the "needle" string in the "haystack" string,
 * or null if it isn't found (case-insensitive).
 *
 * An optional offset determines where in the haystack the search begins. If the
 * offset is negative, the search will begin that many characters from the end
 * of the string. If the offset is out-of-bounds, a ViolationException will be
 * thrown.
 *
 * - To simply check if the haystack contains the needle, see `Str\contains()`.
 * - To get the case-sensitive position, see `Str\search()`.
 * - To get the last position of the needle, see `Str\search_last()`.
 *
 * Previously known in PHP as `stripos`.
 *
 * @guide /hack/built-in-types/string
 */
function search_ci(
  string $haystack,
  string $needle,
  int $offset = 0,
)[]: ?int {
  $position = _Str\stripos_l($haystack, $needle, $offset);
  if ($position < 0) {
    return null;
  }
  return $position;
}

/**
 * Returns the last position of the "needle" string in the "haystack" string,
 * or null if it isn't found.
 *
 * An optional offset determines where in the haystack (from the beginning) the
 * search begins. If the offset is negative, the search will begin that many
 * characters from the end of the string and go backwards. If the offset is
 * out-of-bounds, a ViolationException will be thrown.
 *
 * - To simply check if the haystack contains the needle, see `Str\contains()`.
 * - To get the first position of the needle, see `Str\search()`.
 *
 * Previously known in PHP as `strrpos`.
 *
 * @guide /hack/built-in-types/string
 */
function search_last(
  string $haystack,
  string $needle,
  int $offset = 0,
)[]: ?int {
$haystack_length = length($haystack);
  $position = _Str\strrpos_l($haystack, $needle, $offset);
  if ($position < 0) {
    return null;
  }
  return $position;
}

/**
 * Returns whether the string starts with the given prefix.
 *
 * For a case-insensitive check, see `Str\starts_with_ci()`.
 *
 * @guide /hack/built-in-types/string
 */
function starts_with(
  string $string,
  string $prefix,
)[]: bool {
  return _Str\starts_with_l($string, $prefix);
}

/**
 * Returns whether the string starts with the given prefix (case-insensitive).
 *
 * For a case-sensitive check, see `Str\starts_with()`.
 *
 * @guide /hack/built-in-types/string
 */
function starts_with_ci(
  string $string,
  string $prefix,
)[]: bool {
  return _Str\starts_with_ci_l($string, $prefix);
}
