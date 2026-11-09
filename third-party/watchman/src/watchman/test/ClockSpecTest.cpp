/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <folly/portability/GTest.h>
#include "watchman/Clock.h"
#include "watchman/thirdparty/jansson/jansson.h"

using namespace watchman;

namespace {

const ClockPosition kNow{/*rootNumber=*/7, /*ticks=*/100};

class ClockSpecTest : public testing::Test {
 protected:
  void SetUp() override {
    ClockSpec::init();
    auto thisProcess = std::get<ClockSpec::Clock>(ClockSpec{kNow}.spec);
    startTime_ = thisProcess.start_time;
    pid_ = thisProcess.pid;
  }

  static QuerySince::Clock evaluate(const std::string& clock) {
    return std::get<QuerySince::Clock>(
        ClockSpec{typed_string_to_json(clock.c_str())}.evaluate(kNow, 0).since);
  }

  std::string clockFromThisProcess(ClockPosition position) const {
    return fmt::format(
        "c:{}:{}:{}:{}", startTime_, pid_, position.rootNumber, position.ticks);
  }

  std::string clockFromEarlierProcess(ClockPosition position) const {
    return fmt::format(
        "c:{}:{}:{}:{}",
        startTime_ - 60,
        pid_ + 1,
        position.rootNumber,
        position.ticks);
  }

  uint64_t startTime_{0};
  int pid_{0};
};

TEST_F(ClockSpecTest, clockFromThisProcessResumesFromItsTicks) {
  auto since = evaluate(clockFromThisProcess({7, 40}));

  EXPECT_FALSE(since.is_fresh_instance);
  EXPECT_EQ(since.ticks, 40);
  EXPECT_EQ(since.origin, ClockOrigin::SameProcess);
}

TEST_F(ClockSpecTest, clockFromEarlierProcessIsFreshInstance) {
  auto since = evaluate(clockFromEarlierProcess({7, 40}));

  // FIXME: a view whose clocks survive a watchman restart should be able to
  // resume from this clock.
  EXPECT_TRUE(since.is_fresh_instance);
  EXPECT_EQ(since.origin, ClockOrigin::OtherProcess);
}

TEST_F(ClockSpecTest, clockForAnotherRootIsFreshInstance) {
  auto since = evaluate(clockFromThisProcess({8, 40}));

  EXPECT_TRUE(since.is_fresh_instance);
  EXPECT_EQ(since.origin, ClockOrigin::OtherRoot);
}

TEST_F(ClockSpecTest, oldStyleClockIsFreshInstance) {
  auto since = evaluate(fmt::format("c:{}:{}", pid_, 40));

  EXPECT_TRUE(since.is_fresh_instance);
  EXPECT_EQ(since.origin, ClockOrigin::OtherRoot);
}

} // namespace
