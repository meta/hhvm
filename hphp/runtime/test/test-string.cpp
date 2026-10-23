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

#include <gtest/gtest.h>

#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/util/debug-format.h"
#include "hphp/util/ptr-impl.h"

namespace HPHP {

#define VS(str1, str2) EXPECT_TRUE(!strcmp((str1), (str2)))

TEST(OptString, Constructors) {
  // constructors
  VS(OptString::FromInt64(15).c_str(), "15");
  VS(OptString::FromInt64(-15).c_str(), "-15");
  VS(OptString::FromInt64(int64_t(12345678912345678LL)).c_str(), "12345678912345678");
  VS(OptString::FromInt64(int64_t(-12345678912345678LL)).c_str(), "-12345678912345678");
  VS(OptString::FromDouble(5.603).c_str(), "5.603");
  VS(OptString("test").c_str(), "test");
  VS(OptString(OptString("test")).c_str(), "test");
}

TEST(OptString, Informational) {
  EXPECT_TRUE(OptString().isNull());
  EXPECT_TRUE(!OptString("").isNull());
  EXPECT_TRUE(OptString().empty());
  EXPECT_TRUE(OptString("").empty());
  EXPECT_TRUE(!OptString("test").empty());
  EXPECT_TRUE(OptString().size() == 0);
  EXPECT_TRUE(OptString().length() == 0);
  EXPECT_TRUE(OptString("").size() == 0);
  EXPECT_TRUE(OptString("").length() == 0);
  EXPECT_TRUE(OptString("test").size() == 4);
  EXPECT_TRUE(OptString("test").length() == 4);
  EXPECT_TRUE(!OptString("2test").isNumeric());
  EXPECT_TRUE(!OptString("test").isNumeric());
  EXPECT_TRUE(OptString("23").isNumeric());
  EXPECT_TRUE(OptString("23.3").isNumeric());
}

TEST(OptString, Operators) {
  OptString s;
  s = "test1";                   VS(s.c_str(), "test1");
  s = OptString("test2");           VS(s.c_str(), "test2");
  s = OptString("a") + "b";         VS(s.c_str(), "ab");
  s = OptString("c") + OptString("d"); VS(s.c_str(), "cd");
  s += "efg";                    VS(s.c_str(), "cdefg");
  s += OptString("hij");            VS(s.c_str(), "cdefghij");
}

TEST(OptString, Manipulations) {
  OptString s = HHVM_FN(strtolower)("Test");
  VS(s.c_str(), "test");
}

TEST(OptString, Conversions) {
  EXPECT_TRUE(!OptString().toBoolean());
  EXPECT_TRUE(OptString("123").toBoolean());
  EXPECT_TRUE((int)OptString("1234567890").toInt64() == 1234567890);
  EXPECT_TRUE(OptString("123456789012345678").toInt64() == 123456789012345678LL);
  EXPECT_TRUE(OptString("123.45").toDouble() == 123.45);
}

TEST(OptString, Compare) {
  EXPECT_TRUE(OptString("abc").compare("abc") == 0);
  EXPECT_TRUE(OptString("abc").compare("bbc") < 0);
  EXPECT_TRUE(OptString("bbc").compare("abc") > 0);
  EXPECT_TRUE(OptString("abc").compare(OptString("abc")) == 0);
  EXPECT_TRUE(OptString("abc").compare(OptString("bbc")) < 0);
  EXPECT_TRUE(OptString("bbc").compare(OptString("abc")) > 0);

  EXPECT_TRUE(OptString("abc").compare("abcd") < 0);
  EXPECT_TRUE(OptString("abcd").compare("abc") > 0);
  EXPECT_TRUE(OptString("abc").compare(OptString("abcd")) < 0);
  EXPECT_TRUE(OptString("abcd").compare(OptString("abc")) > 0);

  // check Strings with embedded nulls
  std::string str1("a\0bc", 4);
  std::string str2("a\0bb", 4);
  EXPECT_TRUE(OptString(str1).compare(OptString(str1)) == 0);
  EXPECT_TRUE(OptString(str1).compare(OptString(str2)) > 0);
  EXPECT_TRUE(OptString(str2).compare(OptString(str1)) < 0);
}

TEST(StringData, DebugFormatPrintsContents) {
  const StringData* sd = makeStaticString("abc");
  StringData* mutableSd = const_cast<StringData*>(sd);
  const StringData* nullSd = nullptr;
  ptrimpl::PtrImpl<const StringData, ptrimpl::Normal, ptrimpl::UInt64> ptr{sd};
  EXPECT_EQ("abc abc [  abc] abc (null)",
            debugFormat("{} {} [{:>5}] {} {}", sd, mutableSd, sd, ptr, nullSd));
  EXPECT_EQ("test test", debugFormat("{} {}", OptString("test"),
                                     StaticString("test")));
}

}
