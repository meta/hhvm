/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/PerfSample.h"

#include <folly/Synchronized.h>
#include <condition_variable>
#include <thread>

#include "watchman/ChildProcess.h"
#include "watchman/Logging.h"
#include "watchman/Options.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/sockname.h"
#include "watchman/watchman_system.h"
#include "watchman/watchman_time.h"

using namespace watchman;

namespace watchman {
namespace {
class PerfLogThread {
  struct State {
    explicit State(bool start) : running(start) {}

    bool running;
    std::vector<json_ref> samples;
  };

  folly::Synchronized<State, std::mutex> state_;
  std::thread thread_;
  std::condition_variable cond_;

  void loop() noexcept;

 public:
  explicit PerfLogThread(bool start) : state_(std::in_place, start) {
    if (start) {
      thread_ = std::thread([this] { loop(); });
    }
  }

  ~PerfLogThread() {
    stop();
  }

  void stop() {
    {
      auto state = state_.lock();
      if (!state->running) {
        return;
      }
      state->running = false;
    }
    cond_.notify_all();
    thread_.join();
  }

  void addSample(json_ref&& sample) {
    auto wlock = state_.lock();
    wlock->samples.push_back(std::move(sample));
    cond_.notify_one();
  }
};

PerfLogThread& getPerfThread(bool start = true) {
  // Get the perf logging thread, starting it on the first call.
  // Meyer's singleton!
  static PerfLogThread perfThread(start);
  return perfThread;
}
} // namespace

void processSamples(
    size_t argv_limit,
    size_t maximum_batch_size,
    std::vector<json_ref>& samples,
    std::function<void(std::vector<std::string>)> command_line,
    std::function<void(std::string)> single_large_sample) {
  while (samples.size() > 0) {
    std::string encoded_sample = json_dumps(samples.front(), 0);
    samples.erase(samples.begin()); // O(N^2)

    if (encoded_sample.size() + 1 > argv_limit) {
      single_large_sample(std::move(encoded_sample));
    } else {
      size_t arg_size = encoded_sample.size() + 1;
      std::vector<std::string> args;
      args.push_back(std::move(encoded_sample));

      while (args.size() < maximum_batch_size && samples.size() > 0) {
        encoded_sample = json_dumps(samples[0], 0);
        if (arg_size + encoded_sample.size() + 1 > argv_limit) {
          break;
        }
        samples.erase(samples.begin()); // O(N^2)
        arg_size += encoded_sample.size() + 1;
        args.push_back(std::move(encoded_sample));
      }
      command_line(std::move(args));
    }
  }
}

PerfSample::PerfSample(const char* description) : description(description) {
  gettimeofday(&time_begin, nullptr);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_begin);
#endif
}

double PerfSample::get_perf_sampling_thresh() const {
  static double perf_sampling_thresh{0};
  if (perf_sampling_thresh == 0) {
    auto thresh = cfg_get_json("perf_sampling_thresh");
    if (thresh) {
      if (thresh->isNumber()) {
        perf_sampling_thresh = json_number_value(*thresh);
      } else {
        perf_sampling_thresh =
            json_number_value(thresh->get_default(description, json_real(0.0)));
      }
    }
  }
  return perf_sampling_thresh;
}

bool PerfSample::finish() {
  gettimeofday(&time_end, nullptr);
  w_timeval_sub(time_end, time_begin, &duration);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_end);

  // Compute the delta for the usage
  w_timeval_sub(usage_end.ru_utime, usage_begin.ru_utime, &usage.ru_utime);
  w_timeval_sub(usage_end.ru_stime, usage_begin.ru_stime, &usage.ru_stime);

#define DIFFU(n) usage.n = usage_end.n - usage_begin.n
  DIFFU(ru_maxrss);
  DIFFU(ru_ixrss);
  DIFFU(ru_idrss);
  DIFFU(ru_minflt);
  DIFFU(ru_majflt);
  DIFFU(ru_nswap);
  DIFFU(ru_inblock);
  DIFFU(ru_oublock);
  DIFFU(ru_msgsnd);
  DIFFU(ru_msgrcv);
  DIFFU(ru_nsignals);
  DIFFU(ru_nvcsw);
  DIFFU(ru_nivcsw);
#undef DIFFU
#endif

  if (!will_log) {
    if (wall_time_elapsed_thresh == 0) {
      wall_time_elapsed_thresh = get_perf_sampling_thresh();
    }

    if (wall_time_elapsed_thresh > 0 &&
        w_timeval_diff(time_begin, time_end) > wall_time_elapsed_thresh) {
      will_log = true;
    }
  }

  return will_log;
}

void PerfSample::add_root_metadata(const RootMetadata& root_metadata) {
  auto meta = json_object(
      {{"path", w_string_to_json(root_metadata.root_path)},
       {"recrawl_count", json_integer(root_metadata.recrawl_count)},
       {"case_sensitive", json_boolean(root_metadata.case_sensitive)}});
  if (!root_metadata.watcher.empty()) {
    meta.set({{"watcher", w_string_to_json(root_metadata.watcher)}});
  }
  add_meta("root", std::move(meta));
}

void PerfSample::add_meta(const char* key, json_ref&& val) {
  meta_data.set(key, std::move(val));
}

void PerfSample::set_wall_time_thresh(double thresh) {
  wall_time_elapsed_thresh = thresh;
}

void PerfSample::force_log() {
  will_log = true;
}

void PerfLogThread::loop() noexcept {
  std::vector<json_ref> samples;
  int64_t sample_batch;

  w_set_thread_name("perflog");

  auto stateDir =
      w_string_piece(flags.watchman_state_file).dirName().asWString();

  json_ref perf_cmd = cfg_get_json("perf_logger_command").value_or(json_null());
  if (perf_cmd.isString()) {
    perf_cmd = json_array({perf_cmd});
  }
  if (!perf_cmd.isArray()) {
    logf(
        FATAL,
        "perf_logger_command must be either a string or an array of strings\n");
  }

  sample_batch = cfg_get_int("perf_logger_command_max_samples_per_call", 4);

  while (true) {
    {
      auto state = state_.lock();
      while (true) {
        if (!state->samples.empty()) {
          // We found samples to process
          break;
        }
        if (!state->running) {
          // No samples remaining, and we have been asked to quit.
          return;
        }
        cond_.wait(state.as_lock());
      }

      samples.clear();
      std::swap(samples, state->samples);
    }

    if (!samples.empty()) {
      // Hack: Divide by two because this limit includes environment variables
      // and perf_cmd.
      // It's possible to compute this correctly on every platform given the
      // current environment and any specified environment variables, but it's
      // fine to be conservative here.
      const size_t argv_limit = ChildProcess::getArgMax() / 2;

      processSamples(
          argv_limit,
          sample_batch,
          samples,
          [&](std::vector<std::string> sample_args) {
            std::vector<std::string_view> cmd;
            cmd.reserve(perf_cmd.array().size() + sample_args.size());

            for (auto& c : perf_cmd.array()) {
              cmd.push_back(json_to_w_string(c).view());
            }
            for (auto& sample : sample_args) {
              cmd.push_back(sample);
            }

            ChildProcess::Options opts;
            opts.environment().set(
                {{"WATCHMAN_STATE_DIR", stateDir},
                 {"WATCHMAN_SOCK", get_sock_name_legacy()}});
            opts.nullStdin();
            opts.nullStdout();
            opts.nullStderr();

            try {
              ChildProcess proc(cmd, std::move(opts));
              proc.wait();
            } catch (const std::exception& exc) {
              log(ERR, "failed to spawn perf logger: ", exc.what(), "\n");
            }
          },
          [&](std::string sample_stdin) {
            ChildProcess::Options opts;
            opts.environment().set(
                {{"WATCHMAN_STATE_DIR", stateDir},
                 {"WATCHMAN_SOCK", get_sock_name_legacy()}});
            opts.pipeStdin();
            opts.nullStdout();
            opts.nullStderr();

            try {
              ChildProcess proc({perf_cmd}, std::move(opts));
              auto stdinPipe = proc.takeStdin();

              try {
                const char* data = sample_stdin.data();
                size_t size = sample_stdin.size();

                size_t total_written = 0;
                while (total_written < sample_stdin.size()) {
                  auto result = stdinPipe->write.write(data, size);
                  result.throwIfError();
                  auto written = result.value();
                  data += written;
                  size -= written;
                  total_written += written;
                }
              } catch (const std::exception& exc) {
                log(ERR,
                    "failed to send data to perf logger: ",
                    exc.what(),
                    "\n");
              }

              // close stdin to allow the process to terminate
              stdinPipe.reset();
              proc.wait();
            } catch (const std::exception& exc) {
              log(ERR, "failed to spawn perf logger: ", exc.what(), "\n");
            }
          });
    }
  }
}

void PerfSample::log() {
  if (!will_log) {
    return;
  }

  // Assemble a perf blob
  auto info = json_object(
      {{"description", typed_string_to_json(description)},
       {"meta", meta_data},
       {"pid", json_integer(::getpid())},
       {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)}});

#ifdef WATCHMAN_BUILD_INFO
  info.set(
      "buildinfo", typed_string_to_json(WATCHMAN_BUILD_INFO, W_STRING_UNICODE));
#endif

#define ADDTV(name, tv) info.set(name, json_real(w_timeval_abs_seconds(tv)))
  ADDTV("elapsed_time", duration);
  ADDTV("start_time", time_begin);
#ifdef HAVE_SYS_RESOURCE_H
  ADDTV("user_time", usage.ru_utime);
  ADDTV("system_time", usage.ru_stime);
#endif // HAVE_SYS_RESOURCE_H
#undef ADDTV

  // Log to the log file
  auto dumped = json_dumps(info, 0);
  watchman::log(ERR, "PERF: ", dumped, "\n");

  if (!cfg_get_json("perf_logger_command")) {
    return;
  }

  // Send this to our logging thread for async processing
  auto& perfThread = getPerfThread();
  perfThread.addSample(std::move(info));
}

void perf_shutdown() {
  getPerfThread(false).stop();
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
