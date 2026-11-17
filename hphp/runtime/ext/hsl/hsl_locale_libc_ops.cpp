/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/ext/hsl/hsl_locale_libc_ops.h"
#include "hphp/runtime/ext/hsl/replace_every_nonrecursive.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/util/bstring.h"
#include "hphp/zend/zend-string.h"

#include <strings.h>

namespace HPHP {

HSLLocaleLibcOps::HSLLocaleLibcOps(
  const Locale& locale
) : m_loc(locale.get()) {
}

HSLLocaleLibcOps::~HSLLocaleLibcOps() {
}

int64_t HSLLocaleLibcOps::strlen(const OptString& str) const {
  return str.length();
}

OptString HSLLocaleLibcOps::uppercase(const OptString& str) const {
  return str.forEachByteFast([this](char c) { return toupper_l(c, this->m_loc); });
}

OptString HSLLocaleLibcOps::lowercase(const OptString& str) const {
  return str.forEachByteFast([this](char c) { return tolower_l(c, this->m_loc); });
}

OptString HSLLocaleLibcOps::foldcase(const OptString& str) const {
  return lowercase(str);
}

Array HSLLocaleLibcOps::chunk(const OptString& str, int64_t chunk_size) const {
  assertx(chunk_size > 0);
  const auto len = str.size();
  VecInit ret { (size_t) (len / chunk_size + 1) };
  if (len <= chunk_size) {
    ret.append(str);
    return ret.toArray();
  }

  for (int i = 0; i < len; i += chunk_size) {
    ret.append(str.substr(i, chunk_size));
  }
  return ret.toArray();
}

Array HSLLocaleLibcOps::split(const OptString& str, const OptString& delimiter, int64_t limit) const {
  assertx(limit > 0);

  // StringUtil::Explode does not respect limit if delimiter is empty string;
  // this is fine in other cases as the empty string is banned for `explode()`
  if (delimiter.empty()) {
    VecInit ret { (size_t) MIN(str.length(), limit) };
    for (int i = 0; i < str.length(); ++i) {
      if (i == limit - 1) {
        ret.append(str.substr(i));
        break;
      }
      ret.append(str.substr(i, 1));
    }
    return ret.toArray();
  }

  auto ret = StringUtil::Explode(str, delimiter, limit);
  assertx(ret.isVec());
  return ret.asCArrRef();
}

int64_t HSLLocaleLibcOps::strcoll(const OptString& a, const OptString& b) const {
  // Overridden  in HSLLocaleByteOps for "C" locale
  assertx(!a.isNull() && !b.isNull());

  return strcoll_l(a.c_str(), b.c_str(), this->m_loc);
}

int64_t HSLLocaleLibcOps::strcasecmp(const OptString& a, const OptString& b) const {
  // Overridden  in HSLLocaleByteOps for "C" locale
  assertx(!a.isNull() && !b.isNull());
  const auto min_len = MIN(a.size(), b.size());
  // Defined on FreeBSD, but also an undocumented glibc extension
  const auto res = strncasecmp_l(a.data(), b.data(), min_len, this->m_loc);
  if (res != 0) {
    return res;
  }

  if (a.size() < b.size()) {
    return -1;
  }
  if (a.size() > b.size()) {
    return 1;
  }
  return 0;
}

bool HSLLocaleLibcOps::starts_with(const OptString& str, const OptString& prefix) const {
  assertx(!str.isNull() && !prefix.isNull());
  if (str.size() < prefix.size()) {
    return false;
  }
  return string_ncmp(str.data(), prefix.data(), prefix.size()) == 0;
}

bool HSLLocaleLibcOps::starts_with_ci(const OptString& str, const OptString& prefix) const {
  assertx(!str.isNull() && !prefix.isNull());
  if (str.size() < prefix.size()) {
    return false;
  }
  return bstrcaseeq(str.data(), prefix.data(), prefix.size());
}

bool HSLLocaleLibcOps::ends_with(const OptString& str, const OptString& suffix) const {
  assertx(!str.isNull() && !suffix.isNull());
  if (str.size() < suffix.size()) {
    return false;
  }
  const auto offset = str.size() - suffix.size();
  return string_ncmp(str.data() + offset, suffix.data(), suffix.size()) == 0;
}

bool HSLLocaleLibcOps::ends_with_ci(const OptString& str, const OptString& suffix) const {
  assertx(!str.isNull() && !suffix.isNull());
  if (str.size() < suffix.size()) {
    return false;
  }
  const auto offset = str.size() - suffix.size();
  return bstrcaseeq(str.data() + offset, suffix.data(), suffix.size());
}

OptString HSLLocaleLibcOps::strip_prefix(const OptString& str, const OptString& prefix) const {
  if (!starts_with(str, prefix)) {
    return str;
  }
  return str.substr(prefix.length());
}

OptString HSLLocaleLibcOps::strip_suffix(const OptString& str, const OptString& suffix) const {
  if (!ends_with(str, suffix)) {
    return str;
  }
  return str.substr(0, str.length() - suffix.length());
}

int64_t HSLLocaleLibcOps::strpos(const OptString& haystack, const OptString& needle, int64_t offset) const {
  offset = normalize_offset(offset, haystack.length());
  if (needle.empty() || haystack.empty()) {
    return -1;
  }
  auto pos = HHVM_FN(strpos)(haystack, needle, offset);
  if (pos.m_type == KindOfBoolean) {
    return -1;
  }
  return pos.m_data.num;
}

int64_t HSLLocaleLibcOps::strrpos(const OptString& haystack, const OptString& needle, int64_t offset) const {
  // Don't store the normalized version, just validate the bounds: strrpos
  // has special behavior:
  // https://github.com/facebook/hhvm/pull/8847
  normalize_offset(offset, haystack.length());
  if (needle.empty() || haystack.empty()) {
    return -1;
  }
  auto pos = HHVM_FN(strrpos)(haystack, needle, offset);
  if (pos.m_type == KindOfBoolean) {
    return -1;
  }
  return pos.m_data.num;
}

int64_t HSLLocaleLibcOps::stripos(const OptString& haystack, const OptString& needle, int64_t offset) const {
  offset = normalize_offset(offset, haystack.length());
  if (needle.empty() || haystack.empty()) {
    return -1;
  }
  auto pos = HHVM_FN(stripos)(haystack, needle, offset);
  if (pos.m_type == KindOfBoolean) {
    return -1;
  }
  return pos.m_data.num;
}

int64_t HSLLocaleLibcOps::strripos(const OptString& haystack, const OptString& needle, int64_t offset) const {
  // As for strrpos, just validate bounds, don't store/use normalized value
  normalize_offset(offset, haystack.length());
  if (needle.empty() || haystack.empty()) {
    return -1;
  }
  auto pos = HHVM_FN(strripos)(haystack, needle, offset);
  if (pos.m_type == KindOfBoolean) {
    return -1;
  }
  return pos.m_data.num;
}

OptString HSLLocaleLibcOps::splice(const OptString& str,
                                   const OptString& replacement,
                                   int64_t offset,
                                   int64_t length) const {
  assertx(length >= 0);
  offset = normalize_offset(offset, str.length());

  const auto prefix = slice(str, 0, offset);
  const auto suffix = str.substr(offset + length, StringData::MaxSize);
  return prefix + replacement + suffix;
}

OptString HSLLocaleLibcOps::slice(const OptString& str, int64_t offset, int64_t length) const {
  offset = normalize_offset(offset, str.length());
  return str.substr(offset, MIN(length, StringData::MaxSize));
}

OptString HSLLocaleLibcOps::reverse(const OptString& str) const {
  return HHVM_FN(strrev)(str);
}

OptString HSLLocaleLibcOps::pad_left(const OptString& str, int64_t len, const OptString& pad) const {
  return string_pad(str.data(), str.length(), len, pad.data(), pad.length(), k_STR_PAD_LEFT);
}

OptString HSLLocaleLibcOps::pad_right(const OptString& str, int64_t len, const OptString& pad) const {
  return string_pad(str.data(), str.length(), len, pad.data(), pad.length(), k_STR_PAD_RIGHT);
}

OptString HSLLocaleLibcOps::trim(const OptString& str, TrimSides sides) const {
  switch (sides) {
    case TrimSides::BOTH:
      return HHVM_FN(trim)(str);
    case TrimSides::LEFT:
      return HHVM_FN(ltrim)(str);
    case TrimSides::RIGHT:
      return HHVM_FN(rtrim)(str);
  }
  not_reached();
}

OptString HSLLocaleLibcOps::trim(const OptString& str, const OptString& what, TrimSides sides) const {
  switch (sides) {
    case TrimSides::BOTH:
      return HHVM_FN(trim)(str, what);
    case TrimSides::LEFT:
      return HHVM_FN(ltrim)(str, what);
    case TrimSides::RIGHT:
      return HHVM_FN(rtrim)(str, what);
  }
  not_reached();
}

OptString HSLLocaleLibcOps::replace(const OptString& haystack,
                                    const OptString& needle,
                                    const OptString& replacement) const {
  const auto ret = HHVM_FN(str_replace)(needle, replacement, haystack);
  assertx(isStringType(ret.type()));
  return OptString::attach(ret.val().pstr);
}

OptString HSLLocaleLibcOps::replace_ci(const OptString& haystack,
                                       const OptString& needle,
                                       const OptString& replacement) const {
  const auto ret = HHVM_FN(str_ireplace)(needle, replacement, haystack);
  assertx(isStringType(ret.type()));
  return OptString::attach(ret.val().pstr);
}

namespace {

inline OptString replace_every_impl(decltype(HHVM_FN(str_replace)) impl,
                            const OptString& haystack,
                            const Array& replacements) {
  VecInit keys { (size_t) replacements.size() };
  VecInit values { (size_t) replacements.size() };

  IterateKV(replacements.get(), [&](TypedValue k, TypedValue v) {
    assertx(isStringType(k.type()));
    assertx(isStringType(v.type()));
    keys.append(OptString(k.val().pstr));
    values.append(OptString(v.val().pstr));
  });

  const auto ret = impl(keys.toArray(), values.toArray(), haystack);
  assertx(isStringType(ret.type()));
  return OptString::attach(ret.val().pstr);
}
} // namespace

OptString HSLLocaleLibcOps::replace_every(const OptString& haystack,
                                          const Array& replacements) const {
  return replace_every_impl(HHVM_FN(str_replace), haystack, replacements);
}

OptString HSLLocaleLibcOps::replace_every_ci(const OptString& haystack,
                                             const Array& replacements) const {
  return replace_every_impl(HHVM_FN(str_ireplace), haystack, replacements);
}

OptString HSLLocaleLibcOps::replace_every_nonrecursive(const OptString& haystack,
                                                       const Array& replacements) const {
  return HHVM_FN(strtr)(haystack, replacements).asCStrRef();
}

OptString HSLLocaleLibcOps::replace_every_nonrecursive_ci(const OptString& haystack,
                                                          const Array& replacements) const {
  auto id = [](const OptString& s) -> OptString { return s; };
  return HPHP::replace_every_nonrecursive<OptString>(
    haystack,
    replacements,
    /* to_t = */ id,
    /* from_t = */ id,
    /* normalize = */ [](OptString* s) {},
    [](OptString* s) {
      *s = HHVM_FN(strtolower)(*s);
    },
    [](const OptString& hs, const OptString& n, int32_t offset) {
      auto pos = HHVM_FN(strpos)(hs, n, offset);
      if (pos.type() == KindOfBoolean) {
        return -1;
      }
      return static_cast<int32_t>(pos.val().num);
    },
    [](OptString* hs, int32_t offset, int32_t len, const OptString& n) {
      *hs = string_replace(*hs, offset, len, n);
    }
  );
}

} // namespace HPHP
