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

#include <folly/portability/GTest.h>

#include <limits>
#include <string_view>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/server/http-protocol.h"

namespace HPHP {

namespace {

Array decode(std::string_view query) {
  Array vars = Array::CreateDict();
  HttpProtocol::DecodeParameters(vars, query.data(), query.size());
  return vars;
}

}

TEST(HttpProtocol, DecodeParametersNestedAppend) {
  auto const expected =
    make_dict_array("a", make_dict_array(0, make_dict_array(0, "x")));
  EXPECT_TRUE(decode("a[][]=x")->same(expected.get()));
}

TEST(HttpProtocol, DecodeParametersNestedAppendAfterMaxIntKey) {
  auto const expected = make_dict_array(
    "test", make_dict_array(std::numeric_limits<int64_t>::max(), "0"));
  auto const vars = decode("test[9223372036854775807]=0&test[][]=coredump");
  EXPECT_TRUE(vars->same(expected.get()));
}

TEST(HttpProtocol, DecodeParametersNestedAppendAfterAppendToMaxIntKey) {
  auto const expected = make_dict_array(
    "test",
    make_dict_array(
      std::numeric_limits<int64_t>::max() - 1, "0",
      std::numeric_limits<int64_t>::max(), "0"));
  auto const vars =
    decode("test[9223372036854775806]=0&test[]=0&test[][]=coredump");
  EXPECT_TRUE(vars->same(expected.get()));
}

}
