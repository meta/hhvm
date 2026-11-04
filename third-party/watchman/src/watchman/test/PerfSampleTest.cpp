/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/PerfSample.h"

#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>
#include "watchman/thirdparty/jansson/jansson.h"

using namespace watchman;

namespace {
json_ref make_sample(int i) {
  return json_object({std::make_pair("value", json_integer(i))});
}

[[noreturn]] void unexpected_stdin_callback(const std::string&) {
  throw std::runtime_error("unexpected stdin callback");
}
} // namespace

TEST(Perf, sample_batches_are_limited_to_batch_size) {
  std::vector<json_ref> samples = {
      make_sample(1),
      make_sample(2),
      make_sample(3),
      make_sample(4),
      make_sample(5),
  };

  std::vector<std::vector<std::string>> calls;

  processSamples(
      1000,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        calls.push_back(std::move(samples));
      },
      unexpected_stdin_callback);

  ASSERT_EQ(2, calls.size());
  ASSERT_EQ(4, calls[0].size());
  EXPECT_EQ("{\"value\": 1}", calls[0][0]);
  EXPECT_EQ("{\"value\": 2}", calls[0][1]);
  ASSERT_EQ(1, calls[1].size());
  EXPECT_EQ("{\"value\": 5}", calls[1][0]);
}

TEST(Perf, sample_batches_are_limited_if_total_size_exceeds_argv_limit) {
  std::vector<json_ref> samples = {
      make_sample(1),
      make_sample(2),
      make_sample(3),
      make_sample(4),
      make_sample(5),
  };

  std::vector<std::vector<std::string>> calls;

  // Each sample is 12 bytes, 13 with its NUL: two fill 26 bytes exactly.
  processSamples(
      26,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        calls.push_back(std::move(samples));
      },
      unexpected_stdin_callback);

  ASSERT_EQ(3, calls.size());
  ASSERT_EQ(2, calls[0].size());
  EXPECT_EQ("{\"value\": 1}", calls[0][0]);
  EXPECT_EQ("{\"value\": 2}", calls[0][1]);
  ASSERT_EQ(2, calls[1].size());
  EXPECT_EQ("{\"value\": 3}", calls[1][0]);
  EXPECT_EQ("{\"value\": 4}", calls[1][1]);
  ASSERT_EQ(1, calls[2].size());
  EXPECT_EQ("{\"value\": 5}", calls[2][0]);
}

TEST(Perf, first_sample_counts_toward_argv_limit) {
  std::vector<json_ref> samples = {
      make_sample(1),
      make_sample(2),
      make_sample(3),
  };

  std::vector<std::vector<std::string>> calls;

  // Two 13-byte samples need 26 bytes, one more than the limit.
  processSamples(
      25,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        calls.push_back(std::move(samples));
      },
      unexpected_stdin_callback);

  std::vector<std::vector<std::string>> expected = {
      {"{\"value\": 1}"}, {"{\"value\": 2}"}, {"{\"value\": 3}"}};
  EXPECT_EQ(expected, calls);
}

TEST(Perf, sample_filling_argv_limit_without_its_nul_is_passed_in_stdin) {
  std::vector<json_ref> samples = {make_sample(1)};

  std::vector<std::vector<std::string>> arg_calls;
  std::vector<std::string> stdin_calls;

  // The sample is 12 bytes, so it needs 13 bytes of argv.
  processSamples(
      12,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        arg_calls.push_back(std::move(samples));
      },
      [&](std::string stdin_str) {
        stdin_calls.push_back(std::move(stdin_str));
      });

  EXPECT_EQ(0, arg_calls.size());
  EXPECT_EQ(std::vector<std::string>{"{\"value\": 1}"}, stdin_calls);
}

TEST(Perf, sample_fitting_argv_limit_with_its_nul_is_passed_in_argv) {
  std::vector<json_ref> samples = {make_sample(1)};

  std::vector<std::vector<std::string>> arg_calls;

  processSamples(
      13,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        arg_calls.push_back(std::move(samples));
      },
      unexpected_stdin_callback);

  std::vector<std::vector<std::string>> expected = {{"{\"value\": 1}"}};
  EXPECT_EQ(expected, arg_calls);
}

TEST(Perf, large_samples_are_passed_in_stdin) {
  std::vector<json_ref> samples = {
      make_sample(1),
      make_sample(2),
  };

  std::vector<std::vector<std::string>> arg_calls;
  std::vector<std::string> stdin_calls;

  processSamples(
      5,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        arg_calls.push_back(std::move(samples));
      },
      [&](std::string stdin_str) {
        stdin_calls.push_back(std::move(stdin_str));
      });

  ASSERT_EQ(0, arg_calls.size());
  ASSERT_EQ(2, stdin_calls.size());
  EXPECT_EQ("{\"value\": 1}", stdin_calls[0]);
  EXPECT_EQ("{\"value\": 2}", stdin_calls[1]);
}
