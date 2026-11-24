/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/MapUtil.h>
#include "watchman/Client.h"
#include "watchman/ClientContext.h"
#include "watchman/Errors.h"
#include "watchman/Logging.h"
#include "watchman/MapUtil.h"
#include "watchman/ProcessUtil.h"
#include "watchman/QueryableView.h"
#include "watchman/query/Query.h"
#include "watchman/query/eval.h"
#include "watchman/query/parse.h"
#include "watchman/root/Root.h"
#include "watchman/root/watchlist.h"
#include "watchman/saved_state/SavedStateFactory.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

ClientSubscription::ClientSubscription(
    const std::shared_ptr<Root>& root,
    std::weak_ptr<Client> client)
    : root(root), weakClient(client) {}

std::shared_ptr<UserClient> ClientSubscription::lockClient() {
  auto client = weakClient.lock();
  if (client) {
    return std::dynamic_pointer_cast<UserClient>(client);
  }
  return nullptr;
}

ClientSubscription::~ClientSubscription() {
  auto client = lockClient();
  if (client) {
    client->unsubByName(name);
  }
}

bool UserClient::unsubByName(const w_string& name) {
  auto subIter = subscriptions.find(name);
  if (subIter == subscriptions.end()) {
    return false;
  }

  // Break the weakClient pointer so that ~ClientSubscription()
  // cannot successfully lockClient and recursively call us.
  subIter->second->weakClient.reset();
  unilateralSub.erase(subIter->second);
  subscriptions.erase(subIter);

  return true;
}

enum class sub_action { no_sync_needed, execute, defer, drop };

static std::tuple<sub_action, w_string> get_subscription_action(
    ClientSubscription* sub,
    const std::shared_ptr<Root>& root,
    ClockPosition position) {
  auto action = sub_action::execute;
  w_string policy_name;

  log(DBG,
      "sub=",
      fmt::ptr(sub),
      " ",
      sub->name,
      ", last=",
      sub->last_sub_tick,
      " pending=",
      position.ticks,
      "\n");

  if (sub->last_sub_tick != position.ticks) {
    if (!sub->drop_or_defer.empty()) {
      auto assertedStates = root->assertedStates.rlock();
      // This subscription has some policy for states.
      // Figure out what we should do.
      for (auto& policy_iter : sub->drop_or_defer) {
        auto name = policy_iter.first;
        bool policy_is_drop = policy_iter.second;

        if (!assertedStates->isStateAsserted(name)) {
          continue;
        }

        if (action != sub_action::defer) {
          // This policy is active
          action = sub_action::defer;
          policy_name = name;
        }

        if (policy_is_drop) {
          action = sub_action::drop;

          // If we're dropping, we don't need to look at any
          // other policies
          policy_name = name;
          break;
        }
        // Otherwise keep looking until we find a drop
      }
    }
  } else {
    log(DBG, "subscription ", sub->name, " is up to date\n");
    action = sub_action::no_sync_needed;
  }

  return std::make_tuple(action, policy_name);
}

void ClientSubscription::processSubscription() {
  try {
    processSubscriptionImpl();
  } catch (const std::system_error& exc) {
    if (exc.code() == error_code::stale_file_handle) {
      // This can happen if, for example, the Eden filesystem got into
      // a weird state without fully unmounting from the VFS.
      // In this situation we're getting a signal that the root is no longer
      // valid so the correct action is to cancel the watch.
      log(ERR,
          "While processing subscriptions for ",
          root->root_path,
          " got: ",
          exc.what(),
          ".  Cancel watch\n");
      root->cancel(
          fmt::format("Error processing subscriptions: {}", exc.what()));
    } else {
      throw;
    }
  }
}

void ClientSubscription::processSubscriptionImpl() {
  auto client = lockClient();
  if (!client) {
    log(ERR, "encountered a vacated client while running subscription rules\n");
    return;
  }

  sub_action action;
  w_string policy_name;
  auto position = root->view()->getMostRecentRootNumberAndTickValue();
  std::tie(action, policy_name) = get_subscription_action(this, root, position);

  if (action != sub_action::no_sync_needed) {
    bool executeQuery = true;

    if (action == sub_action::drop) {
      // fast-forward over any notifications while in the drop state
      last_sub_tick = position.ticks;
      query->since_spec = std::make_unique<ClockSpec>(position);
      log(DBG,
          "dropping subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated. Advanced ticks to ",
          last_sub_tick,
          "\n");
      executeQuery = false;
    } else if (action == sub_action::defer) {
      log(DBG,
          "deferring subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated\n");
      executeQuery = false;
    } else if (vcs_defer && root->view()->isVCSOperationInProgress()) {
      log(DBG,
          "deferring subscription notifications for ",
          name,
          " until VCS operations complete\n");
      executeQuery = false;
    }

    if (executeQuery) {
      try {
        last_sub_tick =
            runSubscriptionRules(client.get(), root).position().ticks;
      } catch (const std::exception& exc) {
        // This may happen if an SCM aware query fails to run hg for
        // whatever reason.  Since last_sub_tick is not advanced,
        // we haven't missed any results and will re-evaluate with
        // the same basis the next time a file is changed.  Due to
        // the way that hg works, it is quite likely that it has
        // touched some files already and that we'll get called
        // again almost immediately.
        log(ERR,
            "Error while performing query for subscription ",
            name,
            ": ",
            exc.what(),
            ". Deferring until next change.\n");
      }
    }
  } else {
    log(DBG, "subscription ", name, " is up to date\n");
  }
}

void ClientSubscription::updateSubscriptionTicks(QueryResult* res) {
  // create a new spec that will be used the next time
  query->since_spec = std::make_unique<ClockSpec>(res->clockAtStartOfQuery);
}

std::optional<UntypedResponse> ClientSubscription::buildSubscriptionResults(
    const std::shared_ptr<Root>& root,
    ClockSpec& position,
    OnStateTransition onStateTransition) {
  auto since_spec = query->since_spec.get();

  if (const auto* clock = since_spec
          ? std::get_if<ClockSpec::Clock>(&since_spec->spec)
          : nullptr) {
    log(DBG,
        "running subscription ",
        name,
        " rules since ",
        clock->position.ticks,
        "\n");
  } else {
    log(DBG, "running subscription ", name, " rules (no since)\n");
  }

  // Subscriptions never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  query->sync_timeout = std::chrono::milliseconds(0);
  // We're called by the io thread, so there's little chance that the root
  // could be legitimately blocked by something else.  That means that we
  // can use a short lock_timeout
  query->lock_timeout =
      uint32_t(root->config.getInt("subscription_lock_timeout_ms", 100));
  logf(DBG, "running subscription {} {}\n", name, fmt::ptr(this));

  try {
    auto res = w_query_execute(query.get(), root, time_generator, getInterface);

    logf(
        DBG,
        "subscription {} generated {} results\n",
        name,
        res.resultsArray.results.size());

    position = res.clockAtStartOfQuery;

    // An SCM operation was interleaved with the query execution. This could
    // result in over-reporing query results. Discard our results but, do not
    // update the clock in order to allow changes to be reported the next time
    // the query is run.
    bool scmAwareQuery = since_spec && since_spec->hasScmParams();
    if (onStateTransition == OnStateTransition::DontAdvance && scmAwareQuery) {
      if (root->stateTransCount.load() != res.stateTransCountAtStartOfQuery) {
        log(DBG,
            "discarding SCM aware query results, SCM activity interleaved\n");
        return std::nullopt;
      }
    }

    // We can suppress empty results, unless this is a source code aware query
    // and the mergeBase has changed or this is a fresh instance.
    bool mergeBaseChanged = scmAwareQuery &&
        res.clockAtStartOfQuery.scmMergeBase != query->since_spec->scmMergeBase;
    if (res.resultsArray.results.empty() && !mergeBaseChanged &&
        !res.isFreshInstance) {
      updateSubscriptionTicks(&res);
      return std::nullopt;
    }

    UntypedResponse response;

    // It is way too much of a hassle to try to recreate the clock value if it's
    // not a relative clock spec, and it's only going to happen on the first run
    // anyway, so just skip doing that entirely.
    if (since_spec &&
        std::holds_alternative<ClockSpec::Clock>(since_spec->spec)) {
      response.set("since", since_spec->toJson());
    }
    updateSubscriptionTicks(&res);

    response.set(
        {{"is_fresh_instance", json_boolean(res.isFreshInstance)},
         {"clock", res.clockAtStartOfQuery.toJson()},
         {"files", std::move(res.resultsArray).toJson()},
         {"root", w_string_to_json(root->root_path)},
         {"subscription", w_string_to_json(name)},
         {"unilateral", json_true()}});
    if (res.savedStateInfo) {
      response.set({{"saved-state-info", std::move(*res.savedStateInfo)}});
    }

    return response;
  } catch (const QueryExecError& e) {
    log(ERR, "error running subscription ", name, " query: ", e.what());
    return std::nullopt;
  }
}

ClockSpec ClientSubscription::runSubscriptionRules(
    UserClient* client,
    const std::shared_ptr<Root>& root) {
  ClockSpec position;

  auto response =
      buildSubscriptionResults(root, position, OnStateTransition::DontAdvance);

  if (response) {
    add_root_warnings_to_response(*response, root);
    client->enqueueResponse(std::move(*response));
  }
  return position;
}

static UntypedResponse cmd_flush_subscriptions(
    Client* clientbase,
    const json_ref& args) {
  auto client = (UserClient*)clientbase;

  int sync_timeout;
  std::optional<json_ref> subs;

  // TODO: merge this parse and sync logic with the logic in query evaluation
  if (json_array_size(args) == 3) {
    auto& sync_timeout_obj = args.at(2).get("sync_timeout");
    subs = args.at(2).get_optional("subscriptions");
    if (!sync_timeout_obj.isInt()) {
      throw ErrorResponse("'sync_timeout' must be an integer");
    }
    sync_timeout = sync_timeout_obj.asInt();
  } else {
    throw ErrorResponse("wrong number of arguments to 'flush-subscriptions'");
  }

  auto root = resolveRoot(client, args);

  std::vector<w_string> subs_to_sync;
  if (subs) {
    if (!subs->isArray()) {
      throw ErrorResponse(
          "expected 'subscriptions' to be an array of subscription names");
    }

    for (auto& sub_name : subs->array()) {
      if (!sub_name.isString()) {
        throw ErrorResponse(
            "expected 'subscriptions' to be an array of subscription names");
      }

      auto& sub_name_str = json_to_w_string(sub_name);
      auto sub_iter = client->subscriptions.find(sub_name_str);
      if (sub_iter == client->subscriptions.end()) {
        throw ErrorResponse(
            "this client does not have a subscription named '{}'",
            sub_name_str);
      }
      auto& sub = sub_iter->second;
      if (sub->root != root) {
        throw ErrorResponse(
            "subscription '{}' is on root '{}' different from command root "
            "'{}'",
            sub_name_str,
            sub->root->root_path,
            root->root_path);
      }

      subs_to_sync.push_back(sub_name_str);
    }
  } else {
    // Look for all subscriptions matching this root.
    for (auto& sub_iter : client->subscriptions) {
      if (sub_iter.second->root == root) {
        subs_to_sync.push_back(sub_iter.first);
      }
    }
  }

  root->syncToNow(
      std::chrono::milliseconds(sync_timeout), client->getClientInfo());

  UntypedResponse resp;
  std::vector<json_ref> synced;
  std::vector<json_ref> no_sync_needed;
  std::vector<json_ref> dropped;

  for (auto& sub_name_str : subs_to_sync) {
    auto sub_iter = client->subscriptions.find(sub_name_str);
    auto& sub = sub_iter->second;

    sub_action action;
    w_string policy_name;
    auto position = root->view()->getMostRecentRootNumberAndTickValue();
    std::tie(action, policy_name) =
        get_subscription_action(sub.get(), root, position);

    if (action == sub_action::drop) {
      sub->last_sub_tick = position.ticks;
      sub->query->since_spec = std::make_unique<ClockSpec>(position);
      log(DBG,
          "(flush-subscriptions) dropping subscription notifications for ",
          sub->name,
          " until state ",
          policy_name,
          " is vacated. Advanced ticks to ",
          sub->last_sub_tick,
          "\n");
      dropped.push_back(w_string_to_json(sub_name_str));
    } else {
      // flush-subscriptions means that we _should NOT defer_ notifications. So
      // ignore defer and defer_vcs.
      ClockSpec out_position;
      log(DBG,
          "(flush-subscriptions) executing subscription ",
          sub->name,
          "\n");
      auto sub_result = sub->buildSubscriptionResults(
          root, out_position, OnStateTransition::QueryAnyway);
      if (sub_result) {
        client->enqueueResponse(std::move(*sub_result));
        synced.push_back(w_string_to_json(sub_name_str));
      } else {
        no_sync_needed.push_back(w_string_to_json(sub_name_str));
      }
    }
  }

  resp.set(
      {{"synced", json_array(std::move(synced))},
       {"no_sync_needed", json_array(std::move(no_sync_needed))},
       {"dropped", json_array(std::move(dropped))}});
  add_root_warnings_to_response(resp, root);
  return resp;
}
W_CMD_REG(
    "flush-subscriptions",
    cmd_flush_subscriptions,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

/* unsubscribe /root subname
 * Cancels a subscription */
static UntypedResponse cmd_unsubscribe(
    Client* clientbase,
    const json_ref& args) {
  UserClient* client = (UserClient*)clientbase;

  auto root = resolveRoot(client, args);

  auto jstr = args.at(2);
  const char* name = json_string_value(jstr);
  if (!name) {
    throw ErrorResponse("expected 2nd parameter to be subscription name");
  }

  auto sname = json_to_w_string(jstr);
  bool deleted = client->unsubByName(sname);

  UntypedResponse resp;
  resp.set(
      {{"unsubscribe", typed_string_to_json(name)},
       {"deleted", json_boolean(deleted)}});
  return resp;
}
W_CMD_REG(
    "unsubscribe",
    cmd_unsubscribe,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
static UntypedResponse cmd_subscribe(Client* clientbase, const json_ref& args) {
  UserClient* client = (UserClient*)clientbase;

  if (json_array_size(args) != 4) {
    throw ErrorResponse("wrong number of arguments for subscribe");
  }

  auto root = resolveRoot(client, args);

  json_ref jname = args.at(2);
  if (!jname.isString()) {
    throw ErrorResponse("expected 2nd parameter to be subscription name");
  }

  json_ref query_spec = args.at(3);

  auto query = parseQuery(root, query_spec);
  auto clientPid = client->stm ? client->stm->getPeerProcessID() : 0;
  query->clientInfo.clientPid = clientPid;
  query->clientInfo.clientInfo = clientPid
      ? std::make_optional(lookupProcessInfo(clientPid))
      : std::nullopt;
  query->subscriptionName = json_to_w_string(jname);

  auto defer_list = query_spec.get_optional("defer");
  if (defer_list && !defer_list->isArray()) {
    throw ErrorResponse("defer field must be an array of strings");
  }

  auto drop_list = query_spec.get_optional("drop");
  if (drop_list && !drop_list->isArray()) {
    throw ErrorResponse("drop field must be an array of strings");
  }

  const std::vector<json_ref>* defer_array =
      defer_list ? &defer_list->array() : nullptr;
  const std::vector<json_ref>* drop_array =
      drop_list ? &drop_list->array() : nullptr;

  UntypedResponse resp;

  auto sub_name = json_to_w_string(jname);

  // Check for duplicate subscription names. We do this early because
  // constructing a ClientSubscription with a duplicate name isn't safe.
  if (mapContainsAny(client->subscriptions, sub_name)) {
    if (root->config.getBool("enforce_unique_subscription_names", false)) {
      throw ErrorResponse("subscription name '{}' is not unique", sub_name);
    }
    log(ERR, "clobbering existing subscription '", sub_name, "'\n");
    resp.set(
        "warning",
        w_string_to_json(
            w_string::format(
                "subscription name '{}' is not unique", sub_name)));
  }

  auto sub =
      std::make_shared<ClientSubscription>(root, client->shared_from_this());

  sub->name = std::move(sub_name);
  sub->query = query;

  auto defer = query_spec.get_default("defer_vcs", json_true());
  if (!defer.isBool()) {
    throw ErrorResponse("defer_vcs must be boolean");
  }
  sub->vcs_defer = defer.asBool();

  if (defer_array) {
    for (auto& elt : *defer_array) {
      sub->drop_or_defer[json_to_w_string(elt)] = false;
    }
  }
  if (drop_array) {
    for (auto& elt : *drop_array) {
      sub->drop_or_defer[json_to_w_string(elt)] = true;
    }
  }

  // If they want SCM aware results we should wait for SCM events to finish
  // before dispatching subscriptions
  if (query->since_spec && query->since_spec->hasScmParams()) {
    sub->vcs_defer = true;

    // If they didn't specify any drop/defer behavior, default to a reasonable
    // setting that works together with the fsmonitor extension for hg.
    if (mapContainsAny(sub->drop_or_defer, "hg.update", "hg.transaction")) {
      sub->drop_or_defer["hg.update"] = false; // defer
      sub->drop_or_defer["hg.transaction"] = false; // defer
    }
  }

  // If we are replacing a subscription with the same name, drop the old one
  // first.  Assigning over client->subscriptions[name] below would otherwise
  // leave the old subscription in unilateralSub, where it keeps running its
  // query and is never freed.  unsubByName resets the old subscription's
  // weakClient, so its destructor cannot remove the new entry by name.
  client->unsubByName(sub->name);

  // Connect the root to our subscription
  {
    auto client_id = w_string::build(client->unique_id);
    auto client_stream = w_string::build(fmt::ptr(client->stm.get()));
    auto info_json = json_object(
        {{"name", w_string_to_json(sub->name)},
         {"client", w_string_to_json(client_id)},
         {"stm", w_string_to_json(client_stream)},
         {"is_owner", json_boolean(client->stm->peerIsOwner())},
         {"pid", json_integer(client->stm->getPeerProcessID())}});
    if (sub->query->query_spec) {
      info_json.set("query", json_ref(*sub->query->query_spec));
    }

    std::weak_ptr<Client> clientRef(client->shared_from_this());
    client->unilateralSub.insert(
        std::make_pair(
            sub,
            root->unilateralResponses->subscribe(
                [clientRef, sub]() {
                  auto client = clientRef.lock();
                  if (client) {
                    client->ping->notify();
                  }
                },
                info_json)));
  }

  client->subscriptions[sub->name] = sub;

  resp.set("subscribe", json_ref(jname));

  add_root_warnings_to_response(resp, root);
  ClockSpec position;
  auto initial_subscription_results = sub->buildSubscriptionResults(
      root, position, OnStateTransition::DontAdvance);
  resp.set("clock", position.toJson());
  if (initial_subscription_results) {
    if (auto* saved_state_info =
            folly::get_ptr(*initial_subscription_results, "saved-state-info")) {
      resp.set("saved-state-info", *saved_state_info);
    }
  }

  std::vector<json_ref> asserted_states;
  {
    auto rootAssertedStates = root->assertedStates.rlock();
    for (const auto& key : sub->drop_or_defer) {
      if (rootAssertedStates->isStateAsserted(key.first)) {
        // Not sure what to do in case of failure here. -jupi
        asserted_states.push_back(w_string_to_json(key.first));
      }
    }
  }
  resp.set("asserted-states", json_array(std::move(asserted_states)));

  // TODO: It would be nice to return something that indicates the start of a
  // potential stream of responses, rather than manually enqueuing here and
  // return null.
  client->enqueueResponse(std::move(resp));
  if (initial_subscription_results) {
    client->enqueueResponse(std::move(*initial_subscription_results));
  }
  throw ResponseWasHandledManually{};
}
W_CMD_REG(
    "subscribe",
    cmd_subscribe,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root);

/* vim:ts=2:sw=2:et:
 */
