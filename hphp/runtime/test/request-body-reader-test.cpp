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
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/server/request-body-reader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "folly/io/async/EventBase.h"
#include "folly/portability/GTest.h"

namespace HPHP {
namespace {
///////////////////////////////////////////////////////////////////////////////

using Status = RequestBodyReadStatus;
using Clock = RequestBodyReader::Clock;

// Native completion/delivery receipts, not Asio events or request-heap state.
struct ReadReceipt {
  size_t calls{0};
  Status status{Status::Data};
  std::unique_ptr<folly::IOBuf> data;
  bool disposed{false};
  folly::Function<void() noexcept> onComplete;

  std::string payload() const {
    if (!data) return {};
    return std::string(reinterpret_cast<const char*>(data->data()),
                       data->length());
  }
};

std::unique_ptr<RequestBodyReadCompletion> completion(ReadReceipt& receipt) {
  return std::make_unique<RequestBodyReadCompletion>(
    [&receipt](Status status, std::unique_ptr<folly::IOBuf> data) noexcept {
      ++receipt.calls;
      receipt.status = status;
      receipt.data = std::move(data);
      if (receipt.onComplete) receipt.onComplete();
    }
  );
}

std::shared_ptr<RequestBodyReader> makeQuietReader(
  std::shared_ptr<RequestBodyProfile> profile,
  folly::EventBase& eventBase,
  decltype(RequestBodyReader::Callbacks::dispatchRead) dispatchRead = {}
) {
  RequestBodyReader::Callbacks callbacks;
  callbacks.pauseIngress = [](bool) noexcept {};
  callbacks.requestAbandon = []() noexcept {};
  callbacks.dispatchRead = std::move(dispatchRead);
  return std::make_shared<RequestBodyReader>(
    std::move(profile), &eventBase, std::move(callbacks)
  );
}

struct RequestBodyReaderTest : testing::Test {
  using DispatchRead = decltype(RequestBodyReader::Callbacks::dispatchRead);

  RequestBodyReaderTest()
    : profile(std::make_shared<RequestBodyProfile>("test", 8, 1))
    , reader(makeReader(profile))
  {}

  std::shared_ptr<RequestBodyReader> makeReader(
    std::shared_ptr<RequestBodyProfile> selectedProfile,
    RequestBodyReader::Metadata metadata = {},
    DispatchRead dispatchRead = {}
  ) {
    RequestBodyReader::Callbacks callbacks;
    callbacks.dispatchRead = std::move(dispatchRead);
    callbacks.pauseIngress = [this](bool paused) noexcept {
      pauses.push_back(paused);
      if (!paused && onResume) onResume();
    };
    // Delivery is explicit, just like a FIFO control message, not an inline
    // transport action in a timer or request-thread disposal callback.
    callbacks.requestAbandon = [this]() noexcept {
      ++abandonRequests;
      if (onAbandonRequest) onAbandonRequest();
    };
    return std::make_shared<RequestBodyReader>(
      std::move(selectedProfile), &eventBase, std::move(callbacks),
      std::move(metadata)
    );
  }

  void SetUp() override {
    ASSERT_TRUE(reader->tryAdmit());
  }

  void TearDown() override {
    if (reader) {
      reader->detached();
      reader->finish();
      for (auto& receipt : receipts) {
        if (receipt.calls && !receipt.disposed) dispose(receipt);
      }
      EXPECT_FALSE(reader->hasReadCredit());
    }
    EXPECT_EQ(0, profile->inUse());
  }

  void append(const std::string& bytes) {
    auto input = folly::IOBuf::copyBuffer(bytes.data(), bytes.size());
    reader->append(*input);
  }

  ReadReceipt& newReceipt() {
    receipts.emplace_back();
    return receipts.back();
  }

  ReadReceipt& startRead(
    size_t maxBytes = 8,
    Clock::time_point deadline = Clock::now() + std::chrono::hours(1)
  ) {
    auto& receipt = newReceipt();
    if (!reader->tryReserveRead()) {
      ADD_FAILURE() << "read credit was unexpectedly unavailable";
      return receipt;
    }
    reader->read(maxBytes, deadline, completion(receipt));
    return receipt;
  }

  void dispose(ReadReceipt& receipt, bool delivered = true) {
    ASSERT_EQ(1, receipt.calls);
    ASSERT_FALSE(receipt.disposed);
    receipt.data.reset();
    receipt.disposed = true;
    reader->disposeRead(delivered, receipt.status);
  }

  folly::EventBase eventBase;
  std::vector<bool> pauses;
  size_t abandonRequests{0};
  folly::Function<void() noexcept> onResume;
  folly::Function<void() noexcept> onAbandonRequest;
  std::deque<ReadReceipt> receipts;
  std::shared_ptr<RequestBodyProfile> profile;
  std::shared_ptr<RequestBodyReader> reader;
};

TEST(RequestBodyProfileTest, concurrent_acquisition_never_exceeds_capacity) {
  RequestBodyProfile profile("shared", 1024, 3);
  std::promise<void> start;
  auto gate = start.get_future().share();
  std::array<bool, 8> acquired{};
  std::vector<std::thread> workers;
  for (size_t i = 0; i < acquired.size(); ++i) {
    workers.emplace_back([&, i] {
      gate.wait();
      acquired[i] = profile.tryAcquire();
    });
  }
  start.set_value();
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(3, std::count(acquired.begin(), acquired.end(), true));
  EXPECT_EQ(3, profile.inUse());
  EXPECT_FALSE(profile.tryAcquire());
  for (auto const ownsSlot : acquired) {
    if (ownsSlot) profile.release();
  }
  EXPECT_EQ(0, profile.inUse());
  EXPECT_TRUE(profile.tryAcquire());
  profile.release();
}

TEST(RequestBodyProfileTest, zero_capacity_does_not_admit) {
  RequestBodyProfile profile("disabled", 1024, 0);
  EXPECT_FALSE(profile.tryAcquire());
  EXPECT_EQ(0, profile.inUse());
}

TEST(RequestBodyReadCompletionTest, dropped_command_completes_once) {
  ReadReceipt receipt;
  {
    auto command = completion(receipt);
  }
  EXPECT_EQ(1, receipt.calls);
  EXPECT_EQ(Status::TransportError, receipt.status);
  EXPECT_FALSE(receipt.data);
}

TEST(RequestBodyReadCompletionTest, move_transfers_completion_obligation) {
  ReadReceipt receipt;
  auto original = completion(receipt);
  auto moved = std::make_unique<RequestBodyReadCompletion>(std::move(*original));
  original.reset();
  EXPECT_EQ(0, receipt.calls);
  moved->complete(Status::Eof, nullptr);
  moved->complete(Status::TransportError, nullptr);
  moved.reset();
  EXPECT_EQ(1, receipt.calls);
  EXPECT_EQ(Status::Eof, receipt.status);
}

TEST_F(RequestBodyReaderTest, aliases_share_profile_admission) {
  auto alias = makeReader(profile);
  EXPECT_EQ(&reader->profile(), &alias->profile());
  EXPECT_FALSE(alias->tryAdmit());
  EXPECT_FALSE(alias->tryReserveRead());
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(0, profile->inUse());
  EXPECT_TRUE(alias->tryAdmit());
  EXPECT_EQ(1, profile->inUse());
  reader->detached();
  reader->finish();
  EXPECT_EQ(1, profile->inUse());
  alias->detached();
  EXPECT_EQ(1, profile->inUse());
  alias->finish();
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, aliases_wait_for_finished_read_result_disposal) {
  auto alias = makeReader(profile);
  append("abcdefgh");
  auto& result = startRead(4);
  ASSERT_TRUE(result.data);
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_EQ(1, profile->inUse());
  EXPECT_FALSE(alias->tryAdmit());
  dispose(result);
  EXPECT_EQ(0, profile->inUse());
  EXPECT_TRUE(alias->tryAdmit());
  reader->detached();
  EXPECT_EQ(1, profile->inUse());
  alias->finish();
  EXPECT_EQ(0, profile->inUse());
  alias->detached();
}

TEST_F(RequestBodyReaderTest, metadata_defaults_empty_and_owns_constructor_values) {
  EXPECT_TRUE(reader->metadata().route.empty());
  EXPECT_TRUE(reader->metadata().segments.empty());
  reader->detached();
  reader->finish();
  RequestBodyReader::Metadata metadata{"test-route", {"one", "two"}};
  reader = makeReader(profile, metadata);
  ASSERT_TRUE(reader->tryAdmit());
  metadata.route.clear();
  metadata.segments.clear();
  EXPECT_EQ("test-route", reader->metadata().route);
  EXPECT_EQ(std::vector<std::string>({"one", "two"}),
            reader->metadata().segments);
}

TEST_F(RequestBodyReaderTest, request_read_forwards_without_processing_io) {
  reader->detached();
  reader->finish();
  size_t forwardedBytes = 0;
  Clock::time_point forwardedDeadline;
  std::unique_ptr<RequestBodyReadCompletion> forwarded;
  reader = makeReader(profile, {},
    [&](size_t maxBytes, Clock::time_point deadline,
        std::unique_ptr<RequestBodyReadCompletion> command) {
      forwardedBytes = maxBytes;
      forwardedDeadline = deadline;
      forwarded = std::move(command);
    }
  );
  ASSERT_TRUE(reader->tryAdmit());
  append("abcdefgh");
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  auto const deadline = Clock::now() + std::chrono::hours(1);
  std::thread requestThread(
    [owner = reader, deadline, command = completion(result)]() mutable {
      owner->requestRead(4, deadline, std::move(command));
    }
  );
  requestThread.join();
  EXPECT_EQ(4, forwardedBytes);
  EXPECT_EQ(deadline, forwardedDeadline);
  ASSERT_TRUE(forwarded);
  EXPECT_EQ(0, result.calls);
  EXPECT_EQ(8, reader->queuedBytes());
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  reader->read(forwardedBytes, forwardedDeadline, std::move(forwarded));
  EXPECT_EQ(Status::Data, result.status);
  EXPECT_EQ("abcd", result.payload());
  dispose(result);
}

TEST_F(RequestBodyReaderTest, posted_command_discard_poisons_before_completion) {
  reader->detached();
  reader->finish();
  std::unique_ptr<RequestBodyReadCompletion> forwarded;
  reader = makeReader(profile, {},
    [&](size_t, Clock::time_point,
        std::unique_ptr<RequestBodyReadCompletion> command) {
      forwarded = std::move(command);
    }
  );
  ASSERT_TRUE(reader->tryAdmit());
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  result.onComplete = [&]() noexcept {
    EXPECT_EQ(Status::TransportError, reader->failure());
    EXPECT_EQ(1, abandonRequests);
    reader->requestAbandon();
  };
  reader->requestRead(8, Clock::now() + std::chrono::hours(1),
                      completion(result));
  ASSERT_TRUE(forwarded);
  EXPECT_EQ(0, result.calls);
  std::thread discard([command = std::move(forwarded)]() mutable {
    command.reset();
  });
  discard.join();
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::TransportError, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_EQ(Status::TransportError, reader->failure());
  EXPECT_EQ(1, abandonRequests);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  dispose(result, false);
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, missing_dispatch_completes_categorically) {
  append("abcdefgh");
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  reader->requestRead(8, Clock::now() + std::chrono::hours(1),
                      completion(result));
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::TransportError, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_EQ(Status::TransportError, reader->failure());
  EXPECT_EQ(8, reader->queuedBytes());
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_EQ(1, abandonRequests);
  dispose(result, false);
  reader->abandon();
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
  EXPECT_EQ(1, abandonRequests);
}

TEST_F(RequestBodyReaderTest, throwing_dispatch_preserves_completion_and_credit) {
  reader->detached();
  reader->finish();
  reader = makeReader(profile, {},
    [](size_t, Clock::time_point,
       std::unique_ptr<RequestBodyReadCompletion>) {
      throw std::runtime_error("test post failure");
    }
  );
  ASSERT_TRUE(reader->tryAdmit());
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  result.onComplete = [&]() noexcept {
    EXPECT_EQ(Status::TransportError, reader->failure());
    EXPECT_EQ(1, abandonRequests);
    reader->requestAbandon();
  };
  reader->requestRead(8, Clock::now() + std::chrono::hours(1),
                      completion(result));
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::TransportError, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_EQ(Status::TransportError, reader->failure());
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  EXPECT_EQ(1, abandonRequests);
  dispose(result, false);
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_EQ(1, abandonRequests);
}

TEST_F(RequestBodyReaderTest, request_abandon_only_posts_control_once) {
  append("abcdefgh");
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  auto command = completion(result);
  std::thread requestThread([owner = reader] {
    owner->requestAbandon();
    owner->requestAbandon();
  });
  requestThread.join();
  EXPECT_EQ(Status::Abandoned, reader->failure());
  EXPECT_EQ(1, abandonRequests);
  EXPECT_EQ(8, reader->queuedBytes());
  EXPECT_EQ(0, result.calls);
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  reader->abandon();
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
  reader->read(8, Clock::now() + std::chrono::hours(1), std::move(command));
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Abandoned, result.status);
  EXPECT_FALSE(result.data);
  dispose(result, false);
  EXPECT_EQ(1, abandonRequests);
}

TEST_F(RequestBodyReaderTest, repeated_admission_does_not_take_another_slot) {
  EXPECT_TRUE(reader->tryAdmit());
  EXPECT_TRUE(reader->tryAdmit());
  EXPECT_EQ(1, profile->inUse());
}

TEST_F(RequestBodyReaderTest, credit_precedes_event_allocation_and_io_delivery) {
  size_t allocations = 0;
  auto reserveThenAllocate = [&]() -> std::unique_ptr<RequestBodyReadCompletion> {
    if (!reader->tryReserveRead()) return nullptr;
    ++allocations;
    return completion(newReceipt());
  };
  auto command = reserveThenAllocate();
  ASSERT_TRUE(command);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reserveThenAllocate());
  EXPECT_EQ(1, allocations);
  EXPECT_EQ(0, receipts.back().calls);

  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  reader->read(8, Clock::now() + std::chrono::hours(1), std::move(command));
  auto& result = receipts.back();
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Abandoned, result.status);
  EXPECT_EQ(1, profile->inUse());
  dispose(result, false);
  EXPECT_EQ(0, profile->inUse());
  EXPECT_FALSE(reader->tryReserveRead());
  EXPECT_FALSE(reader->tryAdmit());
  reader->finish();
  reader->detached();
  reader->detached();
  EXPECT_EQ(1, result.calls);
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, completed_native_result_keeps_credit) {
  append("abcdefgh");
  auto& result = startRead(4);
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Data, result.status);
  EXPECT_EQ("abcd", result.payload());
  EXPECT_EQ(4, reader->queuedBytes());
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  EXPECT_EQ(1, profile->inUse());
  dispose(result);
  auto& next = startRead();
  EXPECT_EQ("efgh", next.payload());
  dispose(next);
}

TEST_F(RequestBodyReaderTest, dropped_read_command_preserves_transport_error) {
  ASSERT_TRUE(reader->tryReserveRead());
  auto& result = newReceipt();
  auto command = completion(result);
  command.reset();
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::TransportError, result.status);
  EXPECT_TRUE(reader->hasReadCredit());
  dispose(result, false);
  EXPECT_EQ(Status::TransportError, reader->failure());
  EXPECT_EQ(1, abandonRequests);
  append("unreadable");
  auto& next = startRead();
  EXPECT_EQ(Status::TransportError, next.status);
  EXPECT_FALSE(next.data);
  dispose(next, false);
  EXPECT_EQ(1, abandonRequests);
}

TEST_F(RequestBodyReaderTest, canceled_data_gap_poison_precedes_returned_credit) {
  append("abcdefgh");
  auto& result = startRead(4);
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ("abcd", result.payload());
  onAbandonRequest = [&]() noexcept {
    EXPECT_TRUE(reader->hasReadCredit());
    EXPECT_EQ(Status::Canceled, reader->failure());
  };
  dispose(result, false);
  EXPECT_EQ(1, abandonRequests);
  EXPECT_EQ(Status::Canceled, reader->failure());
  EXPECT_FALSE(reader->hasReadCredit());
  reader->eof();
  EXPECT_EQ(0, reader->queuedBytes());
  append("later");
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(0, reader->queuedCapacity());
  auto& next = startRead();
  EXPECT_EQ(Status::Canceled, next.status);
  EXPECT_FALSE(next.data);
  dispose(next, false);
  reader->abandon();
  reader->abandon();
  EXPECT_EQ(1, abandonRequests);
  EXPECT_EQ(Status::Canceled, reader->failure());
}

TEST_F(RequestBodyReaderTest, undelivered_eof_poison_is_canceled) {
  reader->eof();
  auto& result = startRead();
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Eof, result.status);
  dispose(result, false);
  EXPECT_EQ(Status::Canceled, reader->failure());
  auto& next = startRead();
  EXPECT_EQ(Status::Canceled, next.status);
  EXPECT_FALSE(next.data);
  dispose(next);
}

TEST_F(RequestBodyReaderTest, undelivered_data_preserves_an_existing_failure) {
  append("abcdefgh");
  auto& result = startRead(4);
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Data, result.status);
  reader->fail(Status::TransportError);
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_TRUE(reader->hasReadCredit());
  dispose(result, false);
  EXPECT_EQ(Status::TransportError, reader->failure());
  auto& next = startRead();
  EXPECT_EQ(Status::TransportError, next.status);
  EXPECT_FALSE(next.data);
  dispose(next, false);
  EXPECT_EQ(1, abandonRequests);
}

TEST_F(RequestBodyReaderTest, pending_credit_survives_completion_until_disposal) {
  auto& result = startRead();
  EXPECT_EQ(0, result.calls);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  reader->fail(Status::Canceled);
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Canceled, result.status);
  EXPECT_TRUE(reader->hasReadCredit());
  EXPECT_FALSE(reader->tryReserveRead());
  dispose(result, false);
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_EQ(Status::Canceled, reader->failure());
}

TEST_F(RequestBodyReaderTest, first_failure_discards_queue_and_beats_deadline) {
  append("abcdefgh");
  reader->fail(Status::BodyTooLarge);
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(0, reader->queuedCapacity());
  reader->eof();
  reader->fail(Status::TransportError);
  reader->fail(Status::Timeout);
  reader->abandon();
  append("discarded");
  auto& result = startRead(8, Clock::now() - std::chrono::seconds(1));
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::BodyTooLarge, result.status);
  EXPECT_FALSE(result.data);
  dispose(result, false);
  EXPECT_EQ(Status::BodyTooLarge, reader->failure());
  EXPECT_EQ(1, abandonRequests);
  EXPECT_EQ(1, profile->inUse());
}

TEST_F(RequestBodyReaderTest, post_eof_egress_error_overrides_queued_data) {
  append("abcdefgh");
  reader->eof();
  reader->fail(Status::TransportError);
  auto& result = startRead();
  EXPECT_EQ(Status::TransportError, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_EQ(0, reader->queuedBytes());
  dispose(result, false);
  EXPECT_EQ(Status::TransportError, reader->failure());
}

TEST_F(RequestBodyReaderTest, expired_queued_deadline_beats_bytes_while_paused) {
  append("abcdefgh");
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  auto& result = startRead(8, Clock::now() - std::chrono::nanoseconds(1));
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Timeout, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  EXPECT_EQ(1, abandonRequests);
  dispose(result, false);
  EXPECT_EQ(Status::Timeout, reader->failure());
  reader->abandon();
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
  append("late");
  EXPECT_EQ(0, reader->queuedBytes());
}

TEST_F(RequestBodyReaderTest, event_base_timer_runs_with_transport_pause_held) {
  // Model an independent transport/session pause that remains held after the
  // reader's resume request. A pending read still has an EventBase deadline.
  bool transportPaused = false;
  onResume = [&]() noexcept { EXPECT_TRUE(transportPaused); };
  append("abcdefgh");
  transportPaused = true;
  auto& data = startRead();
  ASSERT_EQ(1, data.calls);
  dispose(data);
  EXPECT_TRUE(transportPaused);
  auto const pausesBeforeTimer = pauses;

  bool watchdogFired = false;
  auto watchdog = folly::AsyncTimeout::make(eventBase, [&]() noexcept {
    watchdogFired = true;
    eventBase.terminateLoopSoon();
  });
  ASSERT_TRUE(watchdog->scheduleTimeout(std::chrono::seconds(2), {}));
  auto& result = newReceipt();
  result.onComplete = [&]() noexcept { eventBase.terminateLoopSoon(); };
  ASSERT_TRUE(reader->tryReserveRead());
  auto const deadline = Clock::now() + std::chrono::milliseconds(50);
  reader->read(8, deadline, completion(result));
  ASSERT_EQ(0, result.calls);
  eventBase.loop();
  watchdog->cancelTimeout();
  EXPECT_FALSE(watchdogFired);
  ASSERT_EQ(1, result.calls);
  EXPECT_GE(Clock::now(), deadline);
  EXPECT_EQ(Status::Timeout, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_TRUE(transportPaused);
  EXPECT_EQ(pausesBeforeTimer, pauses);
  EXPECT_EQ(1, abandonRequests);
  dispose(result, false);
  EXPECT_EQ(1, profile->inUse());
}

TEST_F(RequestBodyReaderTest, partial_data_completes_without_filling_maximum) {
  append("abc");
  auto& result = startRead(8);
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Data, result.status);
  EXPECT_EQ("abc", result.payload());
  EXPECT_EQ(0, reader->queuedBytes());
  dispose(result);
  reader->eof();
  auto& next = startRead();
  EXPECT_EQ(Status::Eof, next.status);
  EXPECT_FALSE(next.data);
  dispose(next);
}

TEST_F(RequestBodyReaderTest, eof_drains_queued_bytes_before_empty_eof_result) {
  append("abcdefgh");
  reader->eof();
  auto& first = startRead(3);
  EXPECT_EQ("abc", first.payload());
  EXPECT_EQ(Status::Data, first.status);
  dispose(first);
  auto& second = startRead(3);
  EXPECT_EQ("def", second.payload());
  EXPECT_EQ(Status::Data, second.status);
  dispose(second);
  auto& third = startRead(3);
  EXPECT_EQ("gh", third.payload());
  EXPECT_EQ(Status::Data, third.status);
  EXPECT_EQ(0, reader->queuedCapacity());
  dispose(third);
  auto& end = startRead();
  EXPECT_EQ(Status::Eof, end.status);
  EXPECT_FALSE(end.data);
  EXPECT_FALSE(reader->isQuiescent());
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  dispose(end);
  EXPECT_EQ(0, profile->inUse());
  reader->detached();
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, queue_owns_copies_and_counts_partial_head_slack) {
  std::string source = "abcdefgh";
  auto input = folly::IOBuf::create(1 << 20); // Empty frame retains no queue node.
  input->appendToChain(folly::IOBuf::wrapBuffer(source.data(), 4));
  input->appendToChain(folly::IOBuf::wrapBuffer(source.data() + 4, 4));
  reader->append(*input);
  input.reset();
  source.assign(source.size(), 'x');

  folly::IOBufQueue expected(folly::IOBufQueue::cacheChainLength());
  expected.append("abcdefgh", 8);
  auto const packedCapacity = expected.front()->capacity() + sizeof(folly::IOBuf);
  EXPECT_EQ(8, reader->queuedBytes());
  EXPECT_EQ(packedCapacity, reader->queuedCapacity());
  auto& first = startRead(1);
  ASSERT_TRUE(first.data);
  EXPECT_EQ("a", first.payload());
  EXPECT_FALSE(first.data->isChained());
  EXPECT_FALSE(first.data->isShared());
  EXPECT_EQ(7, reader->queuedBytes());
  EXPECT_EQ(packedCapacity, reader->queuedCapacity());
  first.data->writableData()[0] = 'z';
  dispose(first);
  auto& rest = startRead(7);
  EXPECT_EQ("bcdefgh", rest.payload());
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(0, reader->queuedCapacity());
  ASSERT_TRUE(rest.data);
  EXPECT_FALSE(rest.data->isShared());
  dispose(rest);
}

TEST_F(RequestBodyReaderTest, one_byte_frames_are_packed_and_empty_frames_dropped) {
  auto empty = folly::IOBuf::create(1 << 20);
  reader->append(*empty);
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_TRUE(pauses.empty());

  std::string source(128, 'a');
  auto chain = folly::IOBuf::create(0);
  for (auto& byte : source) {
    chain->appendToChain(folly::IOBuf::wrapBuffer(&byte, 1));
  }
  reader->append(*chain);
  chain.reset();
  source.assign(source.size(), 'z');
  folly::IOBufQueue expected(folly::IOBufQueue::cacheChainLength());
  expected.append(std::string(128, 'a'));
  EXPECT_EQ(128, reader->queuedBytes());
  EXPECT_EQ(expected.front()->capacity() + sizeof(folly::IOBuf),
            reader->queuedCapacity());
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  auto& result = startRead();
  EXPECT_EQ(std::string(8, 'a'), result.payload());
  dispose(result);
  reader->abandon();
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
}

TEST_F(RequestBodyReaderTest, large_read_copies_across_packed_queue_nodes) {
  reader->detached();
  reader->finish();
  auto const maxBytes = RequestBodyReader::kMaxReadBytes;
  profile = std::make_shared<RequestBodyProfile>("large", maxBytes, 1);
  reader = makeReader(profile);
  ASSERT_TRUE(reader->tryAdmit());

  std::string source(maxBytes + 17, 'a');
  source.replace(maxBytes / 2, 4, "bcde");
  append(source);
  auto& first = startRead(maxBytes);
  ASSERT_TRUE(first.data);
  EXPECT_EQ(source.substr(0, maxBytes), first.payload());
  EXPECT_FALSE(first.data->isChained());
  EXPECT_FALSE(first.data->isShared());
  EXPECT_EQ(17, reader->queuedBytes());
  EXPECT_GE(reader->queuedCapacity(), 17 + sizeof(folly::IOBuf));
  dispose(first);
  auto& last = startRead(maxBytes);
  EXPECT_EQ(source.substr(maxBytes), last.payload());
  EXPECT_EQ(0, reader->queuedCapacity());
  dispose(last);
}

TEST_F(RequestBodyReaderTest,
       finish_waits_for_native_data_disposal_without_detach) {
  append("abcdefgh");
  auto& result = startRead();
  ASSERT_TRUE(result.data);
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  EXPECT_TRUE(reader->hasReadCredit());
  dispose(result);
  EXPECT_EQ(0, profile->inUse());
  reader->finish();
  reader->detached();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, result.calls);
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, disposal_before_finish_releases_without_detach) {
  append("abcdefgh");
  auto& result = startRead();
  dispose(result);
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_FALSE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  reader->eof();
  EXPECT_FALSE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  reader->finish();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(0, profile->inUse());
  reader->detached();
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, finish_discards_unread_input_without_detach) {
  append("abcdefgh");
  onResume = [&]() noexcept {
    EXPECT_TRUE(reader->isQuiescent());
    EXPECT_EQ(0, reader->queuedCapacity());
    EXPECT_EQ(0, profile->inUse());
  };
  reader->finish();
  EXPECT_EQ(Status::Abandoned, reader->failure());
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(0, profile->inUse());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
  append("discarded");
  EXPECT_EQ(0, reader->queuedCapacity());
  EXPECT_FALSE(reader->tryReserveRead());
}

TEST_F(RequestBodyReaderTest, detach_and_disposal_before_finish_hold_profile) {
  append("abcdefgh");
  auto& result = startRead();
  reader->detached();
  EXPECT_TRUE(reader->isQuiescent());
  dispose(result);
  EXPECT_EQ(1, profile->inUse());
  reader->finish();
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, finish_cancels_timer_before_result_disposal) {
  auto& result = startRead();
  EXPECT_EQ(0, result.calls);
  bool watchdogFired = false;
  auto watchdog = folly::AsyncTimeout::make(eventBase, [&]() noexcept {
    watchdogFired = true;
    eventBase.terminateLoopSoon();
  });
  watchdog->detachEventBase();
  watchdog->attachEventBase(&eventBase,
                           folly::AsyncTimeout::InternalEnum::INTERNAL);
  ASSERT_TRUE(watchdog->scheduleTimeout(std::chrono::seconds(2), {}));
  reader->finish();
  ASSERT_EQ(1, result.calls);
  EXPECT_EQ(Status::Abandoned, result.status);
  EXPECT_FALSE(result.data);
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_EQ(1, profile->inUse());
  // Only the internal watchdog remains; a live body timer would hold the loop.
  eventBase.loop();
  watchdog->cancelTimeout();
  EXPECT_FALSE(watchdogFired);
  dispose(result, false);
  EXPECT_EQ(Status::Abandoned, reader->failure());
  EXPECT_EQ(0, profile->inUse());
  reader->finish();
  reader->detached();
  reader->detached();
  EXPECT_EQ(1, result.calls);
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, finish_and_request_thread_disposal_can_race) {
  append("abcdefgh");
  auto& result = startRead();
  ASSERT_EQ(1, result.calls);
  std::promise<void> start;
  auto gate = start.get_future();
  auto owner = reader;
  std::thread requestThread([&, owner] {
    gate.wait();
    result.data.reset();
    result.disposed = true;
    owner->disposeRead(true, result.status);
  });
  start.set_value();
  reader->finish();
  requestThread.join();
  EXPECT_TRUE(reader->isQuiescent());
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_EQ(0, profile->inUse());
  reader->detached();
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, resume_can_synchronously_append_and_pause_again) {
  append("abcdefgh");
  bool appended = false;
  onResume = [&]() noexcept {
    if (!appended) {
      appended = true;
      append("ijklmnop");
    }
  };
  auto& first = startRead();
  EXPECT_EQ("abcdefgh", first.payload());
  EXPECT_EQ(std::vector<bool>({true, false, true}), pauses);
  EXPECT_EQ(8, reader->queuedBytes());
  dispose(first);
  auto& second = startRead();
  EXPECT_EQ("ijklmnop", second.payload());
  EXPECT_EQ(std::vector<bool>({true, false, true, false}), pauses);
  dispose(second);
}

TEST_F(RequestBodyReaderTest, resume_can_detach_and_drop_last_external_owner) {
  append("abcdefgh");
  auto weak = std::weak_ptr<RequestBodyReader>(reader);
  auto& result = newReceipt();
  result.onComplete = [&]() noexcept { dispose(result); };
  onResume = [&]() noexcept {
    reader->finish();
    reader->detached();
    reader.reset();
  };
  ASSERT_TRUE(reader->tryReserveRead());
  auto raw = reader.get();
  raw->read(8, Clock::now() + std::chrono::hours(1), completion(result));
  EXPECT_EQ(1, result.calls);
  EXPECT_TRUE(result.disposed);
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(0, profile->inUse());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
}

TEST_F(RequestBodyReaderTest, completion_may_delete_native_result_immediately) {
  struct NativeResult {
    NativeResult(std::shared_ptr<RequestBodyReader> reader, bool& destroyed)
      : reader(std::move(reader))
      , destroyed(destroyed)
    {}

    ~NativeResult() {
      data.reset();
      reader->disposeRead(false, status);
      destroyed = true;
    }

    std::shared_ptr<RequestBodyReader> reader;
    bool& destroyed;
    Status status{Status::TransportError};
    std::unique_ptr<folly::IOBuf> data;
  };
  append("abcdefgh");
  ASSERT_TRUE(reader->tryReserveRead());
  bool destroyed = false;
  auto native = std::make_unique<NativeResult>(reader, destroyed);
  auto event = native.get();
  size_t calls = 0;
  auto command = std::make_unique<RequestBodyReadCompletion>(
    [event, &calls](Status status, std::unique_ptr<folly::IOBuf> data) noexcept {
      ++calls;
      event->status = status;
      event->data = std::move(data);
      delete event;
    }
  );
  native.release();
  reader->read(4, Clock::now() + std::chrono::hours(1), std::move(command));
  EXPECT_EQ(1, calls);
  EXPECT_TRUE(destroyed);
  EXPECT_FALSE(reader->hasReadCredit());
  EXPECT_EQ(Status::Canceled, reader->failure());
  EXPECT_EQ(1, abandonRequests);
  EXPECT_EQ(0, reader->queuedCapacity());
  // The completion path leaves ingress paused until FIFO abandonment.
  EXPECT_EQ(std::vector<bool>({true}), pauses);
  reader->abandon();
  EXPECT_EQ(0, reader->queuedBytes());
  EXPECT_EQ(std::vector<bool>({true, false}), pauses);
}

TEST_F(RequestBodyReaderTest, missing_dispatch_completes_with_reader_still_alive) {
  auto weak = std::weak_ptr<RequestBodyReader>(reader);
  auto raw = reader.get();
  size_t calls = 0;
  onAbandonRequest = [&]() noexcept { reader.reset(); };
  ASSERT_TRUE(reader->tryReserveRead());
  auto command = std::make_unique<RequestBodyReadCompletion>(
    [&](Status status, std::unique_ptr<folly::IOBuf> data) noexcept {
      ++calls;
      EXPECT_EQ(Status::TransportError, status);
      auto retained = weak.lock();
      EXPECT_TRUE(retained);
      if (!retained) return;
      data.reset();
      retained->disposeRead(false, status);
      retained->finish();
    }
  );
  raw->requestRead(8, Clock::now() + std::chrono::hours(1), std::move(command));
  EXPECT_EQ(1, calls);
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(0, profile->inUse());
}

TEST_F(RequestBodyReaderTest, invalid_size_preserves_existing_failure) {
  EXPECT_EQ(Status::Data, reader->failure());
  append("abcd");
  reader->fail(Status::BodyTooLarge);
  for (auto const forwarding : {false, true}) {
    auto& result = newReceipt();
    ASSERT_TRUE(reader->tryReserveRead());
    auto const deadline = Clock::now() + std::chrono::hours(1);
    if (forwarding) {
      reader->requestRead(0, deadline, completion(result));
    } else {
      reader->read(0, deadline, completion(result));
    }
    EXPECT_EQ(1, result.calls);
    EXPECT_EQ(Status::BodyTooLarge, result.status);
    EXPECT_FALSE(result.data);
    EXPECT_TRUE(reader->hasReadCredit());
    dispose(result, false);
    EXPECT_EQ(Status::BodyTooLarge, reader->failure());
    EXPECT_EQ(1, abandonRequests);
  }
}

TEST(RequestBodyReaderValidationTest, invalid_sizes_fail_without_dispatch_or_data) {
  for (auto const forwarding : {false, true}) {
    for (auto const water : {size_t{8}, 2 * RequestBodyReader::kMaxReadBytes}) {
      auto const limit = std::min(water, RequestBodyReader::kMaxReadBytes);
      for (auto const size : {size_t{0}, limit + 1}) {
        SCOPED_TRACE(testing::Message() << forwarding << ":" << water << ":" << size);
        folly::EventBase eventBase;
        auto profile = std::make_shared<RequestBodyProfile>("test", water, 1);
        size_t dispatches = 0;
        auto reader = makeQuietReader(profile, eventBase,
          [&dispatches](size_t, Clock::time_point,
                        std::unique_ptr<RequestBodyReadCompletion>) {
            ++dispatches;
          }
        );
        EXPECT_EQ(limit, reader->maxReadBytes());
        ASSERT_TRUE(reader->tryAdmit());
        auto input = folly::IOBuf::copyBuffer(std::string(water, 'x'));
        reader->append(*input);
        input.reset();
        ASSERT_TRUE(reader->tryReserveRead());
        ReadReceipt result;
        auto const deadline = Clock::now() + std::chrono::hours(1);
        if (forwarding) {
          reader->requestRead(size, deadline, completion(result));
        } else {
          reader->read(size, deadline, completion(result));
        }
        EXPECT_EQ(0, dispatches);
        EXPECT_EQ(1, result.calls);
        EXPECT_EQ(Status::TransportError, result.status);
        EXPECT_EQ(Status::TransportError, reader->failure());
        EXPECT_FALSE(result.data);
        EXPECT_TRUE(reader->hasReadCredit());
        reader->abandon();
        EXPECT_EQ(0, reader->queuedCapacity());
        reader->disposeRead(false, result.status);
        reader->finish();
        EXPECT_FALSE(reader->hasReadCredit());
        EXPECT_EQ(0, profile->inUse());
      }
    }
  }
}

TEST(RequestBodyReaderLifecycleTest, admission_and_reservation_race_with_finish) {
  for (auto const initiallyAdmitted : {false, true}) {
    for (size_t iteration = 0; iteration < 128; ++iteration) {
      SCOPED_TRACE(testing::Message() << initiallyAdmitted << ":" << iteration);
      folly::EventBase eventBase;
      auto profile = std::make_shared<RequestBodyProfile>("test", 8, 1);
      auto reader = makeQuietReader(profile, eventBase);
      if (initiallyAdmitted) ASSERT_TRUE(reader->tryAdmit());
      std::promise<void> start;
      auto gate = start.get_future();
      std::thread requestThread([&] {
        gate.wait();
        if (!initiallyAdmitted && !reader->tryAdmit()) return;
        if (iteration % 2) std::this_thread::yield();
        if (reader->tryReserveRead()) {
          // A binding construction failure owns no native result storage.
          reader->disposeRead(false, Status::TransportError);
        }
      });
      start.set_value();
      if (!(iteration % 2)) std::this_thread::yield();
      reader->finish();
      requestThread.join();
      EXPECT_TRUE(reader->isQuiescent());
      EXPECT_FALSE(reader->hasReadCredit());
      EXPECT_EQ(0, profile->inUse());
    }
  }
}

TEST(RequestBodyReaderLifecycleTest, destructor_does_not_invent_finish) {
  folly::EventBase eventBase;
  auto profile = std::make_shared<RequestBodyProfile>("test", 8, 1);
  auto reader = makeQuietReader(profile, eventBase);
  ASSERT_TRUE(reader->tryAdmit());
  reader->detached();
  reader.reset();
  EXPECT_EQ(1, profile->inUse());
  // This intentionally malformed owner lifecycle never supplied FINISH.
  profile->release();
}

///////////////////////////////////////////////////////////////////////////////
}
}
