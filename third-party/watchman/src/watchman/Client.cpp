/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"

#include <folly/MapUtil.h>
#include <folly/stop_watch.h>

#include "eden/common/utils/ProcessInfoCache.h"
#include "watchman/ClientContext.h"
#include "watchman/Command.h"
#include "watchman/Errors.h"
#include "watchman/Logging.h"
#include "watchman/MapUtil.h"
#include "watchman/Poison.h"
#include "watchman/ProcessUtil.h"
#include "watchman/QueryableView.h"
#include "watchman/Shutdown.h"
#include "watchman/root/Root.h"
#include "watchman/telemetry/LogEvent.h"
#include "watchman/telemetry/WatchmanStructuredLogger.h"
#include "watchman/watchman_cmd.h"

namespace watchman {

namespace {

constexpr size_t kResponseLogLimit = 0;

folly::Synchronized<std::unordered_set<UserClient*>> clients;

// TODO: If used in a hot loop, EdenFS has a faster implementation.
// https://github.com/meta/sapling/blob/c745d644d969dae1e4c0d184c19320fac7c27ae5/eden/fs/utils/IDGen.h
std::atomic<uint64_t> id_generator{1};
} // namespace

Client::Client() : Client(nullptr) {}

Client::Client(std::unique_ptr<watchman_stream> stm)
    : unique_id{id_generator++},
      stm(std::move(stm)),
      ping(
#ifdef _WIN32
          (this->stm &&
           this->stm->getFileDescriptor().fdType() ==
               FileDescriptor::FDType::Socket)
              ? w_event_make_sockets()
              : w_event_make_named_pipe()
#else
          w_event_make_sockets()
#endif
              ),
      peerPid_{this->stm ? this->stm->getPeerProcessID() : 0},
      peerInfo_{lookupProcessInfo(peerPid_)} {
  logf(DBG, "accepted client:stm={}\n", fmt::ptr(this->stm.get()));
}

Client::~Client() {
  debugSub.reset();
  errorSub.reset();

  logf(DBG, "client_delete {}\n", unique_id);

  if (stm) {
    stm->shutdown();
  }
}

void Client::enqueueResponse(json_ref resp) {
  responses.push_back(std::move(resp));
}

void Client::enqueueResponse(UntypedResponse resp) {
  enqueueResponse(std::move(resp).toJson());
}

void Client::sendErrorResponse(std::string_view formatted) {
  UntypedResponse resp;
  resp.set("error", typed_string_to_json(formatted));

  if (dispatch_command) {
    dispatch_command->error = formatted;
  }

  if (perf_sample) {
    perf_sample->add_meta("error", typed_string_to_json(formatted));
  }

  if (current_command) {
    auto command = json_dumps(current_command->render(), 0);
    watchman::log(
        watchman::ERR,
        "send_error_response: ",
        command,
        ", failed: ",
        formatted,
        "\n");
  } else {
    watchman::log(watchman::ERR, "send_error_response: ", formatted, "\n");
  }

  enqueueResponse(std::move(resp));
}

bool Client::dispatchCommand(const Command& command, CommandFlags mode) {
  // Stash a reference to the current command to make it easier to log
  // the command context in some of the error paths
  current_command = &command;
  SCOPE_EXIT {
    current_command = nullptr;
  };

  try {
    auto* def = command.getCommandDefinition();
    if (!def) {
      CommandValidationError::throwf("unknown command {}", command.name());
    }
    if (def->flags.containsNoneOf(mode)) {
      CommandValidationError::throwf(
          "command {} not available in this mode", command.name());
    }

    if (!poisoned_reason.rlock()->empty() &&
        !def->flags.contains(CMD_POISON_IMMUNE)) {
      sendErrorResponse(*poisoned_reason.rlock());
      return false;
    }

    if (!client_is_owner && !def->flags.contains(CMD_ALLOW_ANY_USER)) {
      sendErrorResponse(
          "you must be the process owner to execute '{}'", def->name);
      return false;
    }

    // Scope for the perf sample
    {
      logf(DBG, "dispatch_command: {}\n", def->name);
      DispatchCommand dispatchCommand;
      dispatchCommand.command = command.name();
      dispatch_command = &dispatchCommand;

      auto sample_name = "dispatch_command:" + std::string{def->name};
      PerfSample sample(sample_name.c_str());
      perf_sample = &sample;

      SCOPE_EXIT {
        dispatch_command = nullptr;
        perf_sample = nullptr;
      };

      sample.set_wall_time_thresh(
          cfg_get_double("slow_command_log_threshold_seconds", 1.0));

      // TODO: It's silly to convert a Command back into JSON after parsing it.
      // Let's change `func` to take a Command after Command knows what a root
      // path is.
      auto rendered = command.render();
      auto renderedString = rendered.toString();

      try {
        enqueueResponse(def->handler(this, rendered));
      } catch (const ErrorResponse& e) {
        sendErrorResponse(e.what());
        dispatchCommand.error = e.what();
      } catch (const ResponseWasHandledManually&) {
      }

      if (sample.finish()) {
        sample.add_meta("args", std::move(rendered));
        sample.add_meta(
            "client", json_object({{"pid", json_integer(peerPid_)}}));
        sample.log();
      }

      const auto& [samplingRate, eventCount] =
          getLogEventCounters(LogEventType::DispatchCommandType);
      // Log if override set, or if we have hit the sample rate
      if (sample.will_log || eventCount == samplingRate) {
        dispatchCommand.event_count =
            eventCount != samplingRate ? 0 : eventCount;
        dispatchCommand.args = renderedString;
        dispatchCommand.client_pid = peerPid_;
        dispatchCommand.client_name =
            facebook::eden::ProcessInfoCache::cleanProcessCommandline(
                std::move(peerInfo_.get().name));

        getLogger()->logEvent(dispatchCommand);
      }

      logf(DBG, "dispatch_command: {} (completed)\n", def->name);
    }

    return true;
  } catch (const std::exception& e) {
    sendErrorResponse(folly::exceptionStr(e));
    return false;
  }
}

ClientContext Client::getClientInfo() const {
  return ClientContext{peerPid_, peerInfo_};
}

std::string ClientStatus::getName() const {
  switch (state_.load(std::memory_order_acquire)) {
    case THREAD_STARTING:
      return "thread starting";
    case THREAD_STARTED:
      return "thread started";
    case WAITING_FOR_REQUEST:
      return "waiting for request";
    /// The client thread is decoding request data.
    case DECODING_REQUEST:
      return "decoding request";
    /// The client thread is executing a request.
    case DISPATCHING_COMMAND:
      return "dispatching command";
    /// The client thread is reading subscription events and processing them.
    case PROCESSING_SUBSCRIPTION:
      return "processing subscription";
    /// The client thread is sending responses.
    case SENDING_SUBSCRIPTION_RESPONSES:
      return "sending subscription responses";
    /// The client thread is shutting down.
    case THREAD_STOPPING:
      return "stopping";
  }

  return "<unknown>";
}

void UserClient::create(std::unique_ptr<watchman_stream> stm) {
  auto uc = std::make_shared<UserClient>(PrivateBadge{}, std::move(stm));

  // Start a thread for the client.
  //
  // We used to use libevent for this, but we have a low volume of concurrent
  // clients and the json parse/encode APIs are not easily used in a
  // non-blocking server architecture.
  //
  // The thread holds a reference count for its life, so the shared_ptr must be
  // created before the thread is started.
  std::thread{[uc] { uc->clientThread(); }}.detach();
}

UserClient::UserClient(PrivateBadge, std::unique_ptr<watchman_stream> stm)
    : Client{std::move(stm)}, since_{std::chrono::system_clock::now()} {
  clients.wlock()->insert(this);
}

UserClient::~UserClient() {
  clients.wlock()->erase(this);

  /* cancel subscriptions */
  subscriptions.clear();

  vacateStates();
}

std::vector<std::shared_ptr<UserClient>> UserClient::getAllClients() {
  std::vector<std::shared_ptr<UserClient>> v;

  auto lock = clients.rlock();
  v.reserve(lock->size());
  for (auto& c : *lock) {
    v.push_back(std::static_pointer_cast<UserClient>(c->shared_from_this()));
  }
  return v;
}

std::vector<ClientDebugStatus> UserClient::getStatusForAllClients() {
  std::vector<ClientDebugStatus> rv;
  auto lock = clients.rlock();
  rv.reserve(lock->size());
  for (auto& c : *lock) {
    rv.push_back(c->getDebugStatus());
  }
  return rv;
}

ClientDebugStatus UserClient::getDebugStatus() const {
  ClientDebugStatus rv;
  rv.state = status_.getName();
  if (peerPid_) {
    rv.peer.emplace();
    rv.peer->pid = peerPid_;
    // May briefly, once, block on the ProcessInfoCache thread.
    rv.peer->name = std::move(peerInfo_.get().name);
  }
  rv.since = std::chrono::system_clock::to_time_t(since_);
  return rv;
}

void UserClient::vacateStates() {
  while (!states.empty()) {
    auto it = states.begin();
    auto assertion = it->second.lock();

    if (!assertion) {
      states.erase(it->first);
      continue;
    }

    auto root = assertion->root;

    logf(
        watchman::ERR,
        "implicitly vacating state {} on {} due to client disconnect\n",
        assertion->name,
        root->root_path);

    // This will delete the state from client->states and invalidate
    // the iterator.
    w_leave_state(this, assertion, true, std::nullopt);
  }
}

void UserClient::clientThread() noexcept {
  status_.transitionTo(ClientStatus::THREAD_STARTED);

  // Keep a persistent vector around so that we can avoid allocating
  // and releasing heap memory when we collect items from the publisher
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;

  stm->setNonBlock(true);
  w_set_thread_name(
      "client=", unique_id, ":stm=", uintptr_t(stm.get()), ":pid=", peerPid_);

  client_is_owner = stm->peerIsOwner();

  EventPoll pfd[2];
  pfd[0].evt = stm->getEvents();
  pfd[1].evt = ping.get();

  bool client_alive = true;
  while (!w_is_stopping() && client_alive) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    status_.transitionTo(ClientStatus::WAITING_FOR_REQUEST);
    ignore_result(w_poll_events(pfd, 2, 2000));
    if (w_is_stopping()) {
      break;
    }

    if (pfd[0].ready) {
      status_.transitionTo(ClientStatus::DECODING_REQUEST);
      json_error_t jerr;
      auto request = reader.decodeNext(stm.get(), &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        if (reader.wpos == reader.rpos) {
          // If they disconnected in between PDUs, no need to log
          // any error
          goto disconnected;
        }
        sendErrorResponse(
            "invalid json at position {}: {}", jerr.position, jerr.text);
        logf(ERR, "invalid data from client: {}\n", jerr.text);

        goto disconnected;
      } else if (request) {
        format = reader.format;
        status_.transitionTo(ClientStatus::DISPATCHING_COMMAND);
        dispatchCommand(Command::parse(*request), CMD_DAEMON);
      }
    }

    if (pfd[1].ready) {
      while (ping->testAndClear()) {
        status_.transitionTo(ClientStatus::PROCESSING_SUBSCRIPTION);
        // Enqueue refs to pending log payloads
        pending.clear();
        getPending(pending, debugSub, errorSub);
        for (auto& item : pending) {
          enqueueResponse(json_ref(item->payload));
        }

        // Maybe we have subscriptions to dispatch?
        std::vector<w_string> subsToDelete;
        for (auto& [sub, subStream] : unilateralSub) {
          watchman::log(
              watchman::DBG, "consider fan out sub ", sub->name, "\n");

          pending.clear();
          subStream->getPending(pending);
          bool seenSettle = false;
          for (auto& item : pending) {
            auto dumped = json_dumps(item->payload, 0);
            watchman::log(
                watchman::DBG,
                "Unilateral payload for sub ",
                sub->name,
                " ",
                dumped,
                "\n");

            if (item->payload.get_optional("canceled")) {
              watchman::log(
                  watchman::ERR,
                  "Cancel subscription ",
                  sub->name,
                  " due to root cancellation\n");

              UntypedResponse resp;
              resp.set(
                  {{"unilateral", json_true()},
                   {"canceled", json_true()},
                   {"subscription", w_string_to_json(sub->name)}});
              if (auto root = item->payload.get_optional("root")) {
                resp.set("root", *root);
              }
              enqueueResponse(std::move(resp));
              // Remember to cancel this subscription.
              // We can't do it in this loop because that would
              // invalidate the iterators and cause a headache.
              subsToDelete.push_back(sub->name);
              continue;
            }

            if (item->payload.get_optional("state-enter") ||
                item->payload.get_optional("state-leave")) {
              UntypedResponse resp;
              resp.insert(
                  item->payload.object().begin(), item->payload.object().end());
              // We have the opportunity to populate additional response
              // fields here (since we don't want to block the command).
              // We don't populate the fat clock for SCM aware queries
              // because determination of mergeBase could add latency.
              resp.set(
                  {{"unilateral", json_true()},
                   {"subscription", w_string_to_json(sub->name)}});
              enqueueResponse(std::move(resp));

              watchman::log(
                  watchman::DBG,
                  "Fan out subscription state change for ",
                  sub->name,
                  "\n");
              continue;
            }

            if (!sub->debug_paused && item->payload.get_optional("settled")) {
              seenSettle = true;
              continue;
            }
          }

          if (seenSettle) {
            sub->processSubscription();
          }
        }

        for (auto& name : subsToDelete) {
          unsubByName(name);
        }
      }
    }

    /* now send our response(s) */
    while (!responses.empty() && client_alive) {
      status_.transitionTo(ClientStatus::SENDING_SUBSCRIPTION_RESPONSES);
      auto& response_to_send = responses.front();

      stm->setNonBlock(false);
      /* Return the data in the same format that was used to ask for it.
       * Update client liveness based on send success.
       */
      auto encodeResult =
          writer.pduEncodeToStream(this->format, response_to_send, stm.get());
      client_alive = encodeResult.hasValue();
      stm->setNonBlock(true);

      std::optional<json_ref> subscriptionValue =
          response_to_send.get_optional("subscription");
      if (kResponseLogLimit && subscriptionValue &&
          subscriptionValue->isString() &&
          json_string_value(*subscriptionValue)) {
        auto subscriptionName = json_to_w_string(*subscriptionValue);
        if (auto* sub = folly::get_ptr(subscriptions, subscriptionName)) {
          if ((*sub)->lastResponses.size() >= kResponseLogLimit) {
            (*sub)->lastResponses.pop_front();
          }
          (*sub)->lastResponses.push_back(
              ClientSubscription::LoggedResponse{
                  std::chrono::system_clock::now(), response_to_send});
        }
      }

      responses.pop_front();
    }
  }

disconnected:
  status_.transitionTo(ClientStatus::THREAD_STOPPING);
  w_set_thread_name(
      "NOT_CONN:client=",
      unique_id,
      ":stm=",
      uintptr_t(stm.get()),
      ":pid=",
      peerPid_);
}

} // namespace watchman

void w_leave_state(
    watchman::UserClient* client,
    std::shared_ptr<watchman::ClientStateAssertion> assertion,
    bool abandoned,
    const std::optional<json_ref>& metadata) {
  // Broadcast about the state leave
  auto payload = json_object(
      {{"root", w_string_to_json(assertion->root->root_path)},
       {"clock",
        w_string_to_json(assertion->root->view()->getCurrentClockString())},
       {"state-leave", w_string_to_json(assertion->name)}});
  if (metadata) {
    payload.set("metadata", json_ref(*metadata));
  }
  if (abandoned) {
    payload.set("abandoned", json_true());
  }
  assertion->root->unilateralResponses->enqueue(std::move(payload));

  // Now remove the state assertion
  assertion->root->assertedStates.wlock()->removeAssertion(assertion);
  // Increment state transition counter for this root
  assertion->root->stateTransCount++;

  if (client) {
    mapRemove(client->states, assertion->name);
  }
}
