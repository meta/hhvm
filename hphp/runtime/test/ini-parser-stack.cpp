// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include "hphp/runtime/base/ini-parser/zend-ini.h"

namespace HPHP {

namespace {

struct IniStackCallback : IniSetting::ParserCallback {
  IniStackCallback() : ParserCallback(IniSetting::ParserCallbackMode::DICT) {}

  void onEntry(
      const std::string&,
      const std::string& value,
      void*) override {
    parsedValue = value;
  }

  std::string parsedValue;
};

}

TEST(IniSetting, NestedExpressionWithinInitialStack) {
  IniStackCallback callback;
  auto const input = "value=" + std::string(50, '(') + "42" +
    std::string(50, ')') + "\n";
  EXPECT_TRUE(zend_parse_ini_string(
    input, "stack-test.ini", IniSetting::NormalScanner, callback, nullptr));
  EXPECT_EQ("42", callback.parsedValue);
}

TEST(IniSetting, InitialStackExhaustionAndRecovery) {
  IniStackCallback callback;
  auto const input = "value=" + std::string(500, '(') + "42" +
    std::string(500, ')') + "\n";
  EXPECT_FALSE(zend_parse_ini_string(
    input, "stack-test.ini", IniSetting::NormalScanner, callback, nullptr));
  EXPECT_TRUE(callback.parsedValue.empty());
  EXPECT_TRUE(zend_parse_ini_string(
    "value=42\n", "stack-test.ini", IniSetting::NormalScanner,
    callback, nullptr));
  EXPECT_EQ("42", callback.parsedValue);
}

}
