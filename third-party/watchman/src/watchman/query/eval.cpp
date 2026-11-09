/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/eval.h"

#include <fmt/chrono.h>
#include <folly/ScopeGuard.h>

#include "eden/common/utils/ProcessInfoCache.h"
#include "watchman/ClientContext.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/PerfSample.h"
#include "watchman/QueryableView.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/LocalFileResult.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/root/Root.h"
#include "watchman/saved_state/SavedStateInterface.h"
#include "watchman/scm/SCM.h"
#include "watchman/telemetry/LogEvent.h"
#include "watchman/telemetry/WatchmanStructuredLogger.h"

using namespace watchman;

namespace {
std::vector<w_string> computeUnconditionalLogFilePrefixes() {
  Configuration globalConfig;
  auto names =
      globalConfig.get("unconditional_log_if_results_contain_file_prefixes");

  std::vector<w_string> result;
  if (names) {
    for (auto& name : names->array()) {
      result.push_back(json_to_w_string(name));
    }
  }

  return result;
}

const std::vector<w_string>& getUnconditionalLogFilePrefixes() {
  // Meyer's singleton to hold this for the life of the process
  static const std::vector<w_string> names =
      computeUnconditionalLogFilePrefixes();
  return names;
}

ClockOrigin sinceClockOrigin(const QuerySince& since) {
  auto* sinceClock = std::get_if<QuerySince::Clock>(&since.since);
  return sinceClock ? sinceClock->origin : ClockOrigin::None;
}

const char* clockOriginName(ClockOrigin origin) {
  switch (origin) {
    case ClockOrigin::None:
      return "";
    case ClockOrigin::SameProcess:
      return "same_process";
    case ClockOrigin::OtherProcess:
      return "other_process";
    case ClockOrigin::OtherRoot:
      return "other_root";
  }
  return "";
}
} // namespace

/* Query evaluator */
void w_query_process_file(
    const Query* query,
    QueryContext* ctx,
    std::unique_ptr<FileResult> file) {
  // TODO: Should this be implicit by assigning a file to the QueryContext? It
  // could be cleared when resetting the file.
  ctx->resetWholeName();
  ctx->file = std::move(file);
  SCOPE_EXIT {
    ctx->file.reset();
  };

  // For fresh instances, only return files that currently exist
  // TODO: shift this clause to execute_common and generate
  // a wrapped query: ["allof", "exists", EXPR] and execute that
  // instead of query->expr so that the lazy evaluation logic can
  // be automatically applied and avoid fetching the exists flag
  // for every file.  See also related TODO in batchFetchNow.
  if (!ctx->disableFreshInstance &&
      std::holds_alternative<QuerySince::Clock>(ctx->since.since) &&
      std::get<QuerySince::Clock>(ctx->since.since).is_fresh_instance) {
    auto exists = ctx->file->exists();
    if (!exists.has_value()) {
      // Reconsider this one later
      ctx->addToEvalBatch(std::move(ctx->file));
      return;
    }
    if (!exists.value()) {
      return;
    }
  }

  // We produce an output for this file if there is no expression,
  // or if the expression matched.
  if (query->expr) {
    auto match = query->expr->evaluate(ctx, ctx->file.get());

    if (!match.has_value()) {
      // Reconsider this one later
      ctx->addToEvalBatch(std::move(ctx->file));
      return;
    } else if (!*match) {
      return;
    }
  }

  if (ctx->query->dedup_results) {
    auto name = ctx->getWholeName();

    auto inserted = ctx->dedup.insert(name);
    if (!inserted.second) {
      // Already present in the results, no need to emit it again
      ctx->num_deduped++;
      return;
    }
  }

  auto logPrefixes = getUnconditionalLogFilePrefixes();
  if (!logPrefixes.empty()) {
    auto name = ctx->getWholeName();
    for (auto& prefix : logPrefixes) {
      if (name.piece().startsWith(prefix)) {
        ctx->namesToLog.push_back(name);
      }
    }
  }

  ctx->maybeRender(std::move(ctx->file));
}

void time_generator(
    const Query* query,
    const std::shared_ptr<Root>& root,
    QueryContext* ctx) {
  root->view()->timeGenerator(query, ctx);
}

static void default_generators(
    const Query* query,
    const std::shared_ptr<Root>& root,
    QueryContext* ctx) {
  bool generated = false;

  // Time based query
  if (ctx->since.is_timestamp() || !ctx->since.is_fresh_instance()) {
    time_generator(query, root, ctx);
    generated = true;
  }

  if (query->paths.has_value()) {
    root->view()->pathGenerator(query, ctx);
    generated = true;
  }

  if (query->glob_tree) {
    root->view()->globGenerator(query, ctx);
    generated = true;
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (!generated) {
    root->view()->allFilesGenerator(query, ctx);
  }
}

static void execute_common(
    QueryContext* ctx,
    QueryExecute* queryExecute,
    PerfSample* sample,
    QueryResult* res,
    QueryGenerator generator,
    const ClientContext& clientInfo) {
  ctx->stopWatch.reset();

  if (ctx->query->dedup_results) {
    ctx->dedup.reserve(64);
  }

  // isFreshInstance is also later set by the value in ctx after generator
  {
    auto* since_clock = std::get_if<QuerySince::Clock>(&ctx->since.since);
    res->isFreshInstance = since_clock && since_clock->is_fresh_instance;
  }

  if (!(res->isFreshInstance && ctx->query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }
    generator(ctx->query, ctx->root, ctx);
  }
  ctx->generationDuration = ctx->stopWatch.lap();
  ctx->state = QueryContextState::Rendering;

  // We may have some file results pending re-evaluation,
  // so make sure that we process them before we get to
  // the render phase below.
  ctx->fetchEvalBatchNow();
  while (!ctx->fetchRenderBatchNow()) {
    // Depending on the implementation of the query terms and
    // the field renderers, we may need to do a couple of fetches
    // to get all that we need, so we loop until we get them all.
  }

  ctx->renderDuration = ctx->stopWatch.lap();
  ctx->state = QueryContextState::Completed;

  // For Eden instances it is possible that when running the query it was
  // discovered that it is actually a fresh instance [e.g. mount generation
  // changes or journal truncation]; update res to match
  {
    auto* since_clock = std::get_if<QuerySince::Clock>(&ctx->since.since);
    res->isFreshInstance |= since_clock && since_clock->is_fresh_instance;
  }
  if (sample && !ctx->namesToLog.empty()) {
    std::vector<json_ref> nameList;
    nameList.reserve(ctx->namesToLog.size());
    for (auto& name : ctx->namesToLog) {
      nameList.push_back(w_string_to_json(name));

      // Avoid listing everything!
      if (nameList.size() >= 12) {
        break;
      }
    }

    // NOTE: sample and queryExecute are either both non-null or both null
    queryExecute->num_special_files = ctx->namesToLog.size();
    queryExecute->special_files = json_array(nameList).toString();

    sample->add_meta(
        "num_special_files_in_result_set",
        json_integer(ctx->namesToLog.size()));
    sample->add_meta(
        "special_files_in_result_set", json_array(std::move(nameList)));
    sample->force_log();
  }

  if (sample) {
    // NOTE: sample and queryExecute are either both non-null or both null
    auto root_metadata = ctx->root->getRootMetadata();

    if (sample->finish()) {
      sample->add_root_metadata(root_metadata);
      auto meta = json_object({
          {"fresh_instance", json_boolean(res->isFreshInstance)},
          {"num_deduped", json_integer(ctx->num_deduped)},
          {"num_results", json_integer(ctx->resultsArray.size())},
          {"num_walked", json_integer(ctx->getNumWalked())},
      });
      if (ctx->query->query_spec) {
        meta.set("query", json_ref(*ctx->query->query_spec));
      }
      sample->add_meta("query_execute", std::move(meta));
      sample->log();
    }

    const auto& [samplingRate, eventCount] =
        getLogEventCounters(LogEventType::QueryExecuteType);
    // Log if override set, or if we have hit the sample rate,
    // or if the query used the changes_since_mergebase_with generator
    if (sample->will_log || eventCount == samplingRate ||
        (!ctx->generatorType.empty() &&
         ctx->generatorType == "scm_files_changed_since_mergebase_with") ||
        (!ctx->freshInstanceCause.empty() &&
         ctx->freshInstanceCause != "New instance")) {
      addRootMetadataToEvent(root_metadata, *queryExecute);
      queryExecute->event_count = eventCount != samplingRate ? 0 : eventCount;
      queryExecute->fresh_instance = res->isFreshInstance;
      queryExecute->deduped = ctx->num_deduped;
      queryExecute->results = ctx->resultsArray.size();
      queryExecute->walked = ctx->getNumWalked();
      queryExecute->eden_glob_files_duration_us =
          ctx->edenGlobFilesDurationUs.load(std::memory_order_relaxed);
      queryExecute->eden_changed_files_duration_us =
          ctx->edenChangedFilesDurationUs.load(std::memory_order_relaxed);
      queryExecute->eden_file_properties_duration_us =
          ctx->edenFilePropertiesDurationUs.load(std::memory_order_relaxed);
      queryExecute->scm_files_changed_since_mergebase_with_duration_us =
          ctx->scmFilesChangedSinceMergebaseWithDurationUs.load(
              std::memory_order_relaxed);
      queryExecute->generation_duration_ms =
          ctx->generationDuration.load().count();
      if (!ctx->generatorType.empty()) {
        queryExecute->generator = ctx->generatorType;
      }

      if (ctx->query->query_spec) {
        queryExecute->query = ctx->query->query_spec->toString();
      }
      queryExecute->client_pid = clientInfo.clientPid;
      queryExecute->client_name = clientInfo.clientInfo.has_value()
          ? facebook::eden::ProcessInfoCache::cleanProcessCommandline(
                std::move(clientInfo.clientInfo.value().get().name))
          : "";

      if (!ctx->freshInstanceCause.empty()) {
        queryExecute->fresh_instance_cause = ctx->freshInstanceCause;
      }
      queryExecute->since_clock_eval =
          clockOriginName(sinceClockOrigin(ctx->since));

      getLogger()->logEvent(*queryExecute);
    }
  }

  res->resultsArray = ctx->renderResults();
  res->dedupedFileNames = std::move(ctx->dedup);
}

// Capability indicating support for scm-aware since queries
W_CAP_REG("scm-since")

QueryResult w_query_execute(
    const Query* query,
    const std::shared_ptr<Root>& root,
    QueryGenerator generator,
    SavedStateFactory savedStateFactory) {
  QueryResult res;
  ClockSpec resultClock(ClockPosition{});
  bool disableFreshInstance{false};
  auto requestId = query->request_id;

  QueryExecute queryExecute;
  PerfSample sample("query_execute");
  if (requestId && !requestId->empty()) {
    log(DBG, "request_id = ", *requestId, "\n");
    queryExecute.request_id = requestId->string();
    sample.add_meta("request_id", w_string_to_json(*requestId));
  }

  // We want to check this before we sync, as the SCM may generate changes
  // in the filesystem when running the underlying commands to query it.
  if (query->since_spec && query->since_spec->hasScmParams()) {
    auto scm = root->view()->getSCM();

    if (!scm) {
      throw QueryExecError("This root does not support SCM-aware queries.");
    }

    // Populate transition counter at start of query. This allows us to
    // determine if SCM operations ocurred concurrent with query execution.
    res.stateTransCountAtStartOfQuery = root->stateTransCount.load();
    resultClock.scmMergeBaseWith = query->since_spec->scmMergeBaseWith;
    resultClock.scmMergeBase =
        scm->mergeBaseWith(resultClock.scmMergeBaseWith, requestId);
    // Always update the saved state storage type and key, but conditionally
    // update the saved state commit id below based on whether the mergebase has
    // changed.
    if (query->since_spec->hasSavedStateParams()) {
      resultClock.savedStateStorageType =
          query->since_spec->savedStateStorageType;
      resultClock.savedStateConfig = query->since_spec->savedStateConfig;
    }

    if (resultClock.scmMergeBase != query->since_spec->scmMergeBase) {
      // The merge base is different, so on the assumption that a lot of
      // things have changed between the prior and current state of
      // the world, we're just going to ask the SCM to tell us about
      // the changes, then we're going to feed that change list through
      // a simpler watchman query.
      auto modifiedMergebase = resultClock.scmMergeBase;
      if (query->since_spec->hasSavedStateParams()) {
        // Find the most recent saved state to the new mergebase and return
        // changed files since that saved state, if available.
        auto savedStateInterface = savedStateFactory(
            query->since_spec->savedStateStorageType.value(),
            query->since_spec->savedStateConfig.value(),
            scm,
            root->config,
            [root](RootMetadata& root_metadata) {
              root->collectRootMetadata(root_metadata);
            },
            query->clientInfo);
        auto savedStateResult = savedStateInterface->getMostRecentSavedState(
            resultClock.scmMergeBase ? resultClock.scmMergeBase->piece()
                                     : w_string_piece{});
        res.savedStateInfo = savedStateResult.savedStateInfo;
        if (!savedStateResult.commitId.empty()) {
          resultClock.savedStateCommitId = savedStateResult.commitId;
          // Modify the mergebase to be the saved state mergebase so we can
          // return changed files since the saved state.
          modifiedMergebase = savedStateResult.commitId;
        } else {
          // Setting the saved state commit id to the empty string alerts the
          // client that the mergebase changed, yet no saved state was
          // available. The changed files list will be relative to the prior
          // clock as if scm-aware queries were not being used at all, to ensure
          // clients have all changed files they need.
          queryExecute.saved_state_missing = true;
          resultClock.savedStateCommitId = w_string();
          modifiedMergebase = std::nullopt;
        }
      }
      // If the modified mergebase is null then we had no saved state available
      // so we need to fall back to the normal behavior of returning all changes
      // since the prior clock, so we should not update the generator in that
      // case.
      if (modifiedMergebase) {
        disableFreshInstance = true;
        generator = [root, modifiedMergebase, requestId](
                        const Query* q,
                        const std::shared_ptr<Root>& r,
                        QueryContext* c) {
          c->generationStarted();
          c->generatorType = "scm_files_changed_since_mergebase_with";
          auto position = c->clockAtStartOfQuery.position();
          folly::stop_watch<std::chrono::microseconds> timer;
          auto changedFiles =
              root->view()->getSCM()->getFilesChangedSinceMergeBaseWith(
                  modifiedMergebase ? modifiedMergebase->piece()
                                    : w_string_piece{},
                  position.toClockString(),
                  requestId);
          c->scmFilesChangedSinceMergebaseWithDurationUs.store(
              timer.elapsed().count(), std::memory_order_relaxed);
          ClockStamp clock{position.ticks, ::time(nullptr)};
          for (const auto& path : changedFiles) {
            auto fullPath = w_string::pathCat({r->root_path, path});
            if (!c->fileMatchesRelativeRoot(fullPath)) {
              continue;
            }
            // Note well!  At the time of writing the LocalFileResult class
            // assumes that removed entries must have been regular files.
            // We don't have enough information returned from
            // getFilesChangedSinceMergeBaseWith() to distinguish between
            // deleted files and deleted symlinks.  Also, it is not possible
            // to see a directory returned from that call; we're only going
            // to enumerate !dirs for this case.
            w_query_process_file(
                q,
                c,
                std::make_unique<LocalFileResult>(
                    fullPath, clock, r->case_sensitive));
          }
        };
      } else if (query->fail_if_no_saved_state) {
        throw QueryExecError(
            "The merge base changed but no corresponding saved state was "
            "found for the new merge base. fail_if_no_saved_state was set "
            "in the query so treating this as an error");
      }
    } else {
      if (query->since_spec->hasSavedStateParams()) {
        // If the mergebase has not changed, then preserve the input value for
        // the saved state commit id so it will be accurate in subscriptions.
        resultClock.savedStateCommitId = query->since_spec->savedStateCommitId;
      }
    }
  }
  // We should skip asking SCM for the changed files if the query
  // indicated to omit those. To do so, lets just make an empty
  // generator.
  if (query->omit_changed_files) {
    generator = [](const Query*, const std::shared_ptr<Root>&, QueryContext*) {
    };
  }
  QueryContext ctx{query, root, disableFreshInstance};

  // Track the query against the root.
  // This is to enable the `watchman debug-status` diagnostic command.
  // It promises only to read the read-only fields in ctx and ctx.query.
  root->queries.wlock()->insert(&ctx);
  SCOPE_EXIT {
    root->queries.wlock()->erase(&ctx);
  };
  if (query->settle_timeouts) {
    auto future = root->waitForSettle(query->settle_timeouts->settle_period);
    try {
      std::move(future).get(query->settle_timeouts->settle_timeout);
    } catch (const folly::FutureTimeout&) {
      QueryExecError::throwf(
          "waitForSettle: timed out waiting for settle {} in {}",
          query->settle_timeouts->settle_period,
          query->settle_timeouts->settle_timeout);
    }
  }
  if (query->sync_timeout.count()) {
    ctx.state = QueryContextState::WaitingForCookieSync;
    ctx.stopWatch.reset();
    try {
      auto result = root->syncToNow(query->sync_timeout, query->clientInfo);
      res.debugInfo.cookieFileNames = std::move(result.cookieFileNames);
    } catch (const std::exception& exc) {
      QueryExecError::throwf("synchronization failed: {}", exc.what());
    }
    ctx.cookieSyncDuration = ctx.stopWatch.lap();
  }

  /* The first stage of execution is generation.
   * We generate a series of file inputs to pass to
   * the query executor.
   *
   * We evaluate each of the generators one after the
   * other.  If multiple generators are used, it is
   * possible and expected that the same file name
   * will be evaluated multiple times if those generators
   * both emit the same file.
   */

  ctx.clockAtStartOfQuery =
      ClockSpec(root->view()->getMostRecentRootNumberAndTickValue());
  ctx.lastAgeOutTickValueAtStartOfQuery =
      root->view()->getLastAgeOutTickValue();

  // Copy in any scm parameters
  res.clockAtStartOfQuery = resultClock;
  // then update the clock position portion
  std::get<ClockSpec::Clock>(res.clockAtStartOfQuery.spec) =
      std::get<ClockSpec::Clock>(ctx.clockAtStartOfQuery.spec);

  // Evaluate the cursor for this root
  ctx.since = query->since_spec ? query->since_spec->evaluate(
                                      ctx.clockAtStartOfQuery.position(),
                                      ctx.lastAgeOutTickValueAtStartOfQuery,
                                      &root->inner.cursors)
                                : QuerySince{};

  // If there is a since spec, check if it is fresh instance
  // By default new instances are fresh instance
  if (ctx.since.is_fresh_instance()) {
    if (query->since_spec) {
      ctx.freshInstanceCause = "Since spec eval resulted in fresh instance";
      log(DBG, "Since spec eval resulted in fresh instance\n");
    } else {
      ctx.freshInstanceCause = "New instance";
    }
  }

  if (query->bench_iterations > 0) {
    for (uint32_t i = 0; i < query->bench_iterations; ++i) {
      QueryContext c{query, root, ctx.disableFreshInstance};
      QueryResult r;
      c.clockAtStartOfQuery = ctx.clockAtStartOfQuery;
      c.since = ctx.since;
      execute_common(&c, nullptr, nullptr, &r, generator, query->clientInfo);
    }
  }

  execute_common(
      &ctx, &queryExecute, &sample, &res, generator, query->clientInfo);
  return res;
}

/* vim:ts=2:sw=2:et:
 */
