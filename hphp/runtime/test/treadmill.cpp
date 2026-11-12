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

#include "hphp/runtime/vm/treadmill.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace HPHP {

TEST(Treadmill, EnqueueOwnsMoveOnlyCallback) {
  auto result = 0;
  Treadmill::enqueue([value = std::make_unique<int>(42), &result] {
    result = *value;
  });
  Treadmill::finishRequest();
  Treadmill::startRequest(Treadmill::SessionKind::UnitTests);
  EXPECT_EQ(result, 42);
}

TEST(Treadmill, EnqueueOwnsExplicitlyMovedLambda) {
  auto result = 0;
  auto value = std::make_shared<int>(42);
  std::weak_ptr<int> retained = value;
  auto callback = [value = std::move(value), &result]() mutable {
    if (!value) return;
    result = *value;
    value.reset();
  };
  Treadmill::enqueue(std::move(callback));
  callback();
  result = 0;
  EXPECT_FALSE(retained.expired());
  Treadmill::finishRequest();
  Treadmill::startRequest(Treadmill::SessionKind::UnitTests);
  EXPECT_EQ(result, 42);
  EXPECT_TRUE(retained.expired());
}

}
