/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <folly/Exception.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/net/NetworkSocket.h>
#include <folly/portability/Fcntl.h>
#include <folly/system/Shell.h>

#include <stdio.h>
#include <optional>

#include "watchman/ChildProcess.h"
#include "watchman/Client.h"
#include "watchman/Clock.h"
#include "watchman/CloseRandomFds.h"
#include "watchman/Command.h"
#include "watchman/Connect.h"
#include "watchman/GroupLookup.h"
#include "watchman/LogConfig.h"
#include "watchman/Logging.h"
#include "watchman/Options.h"
#include "watchman/PDU.h"
#include "watchman/PathUtils.h"
#include "watchman/PerfSample.h"
#include "watchman/ProcessLock.h"
#include "watchman/ProcessUtil.h"
#include "watchman/ThreadPool.h"
#include "watchman/UserDir.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/XattrUtils.h"
#include "watchman/fs/DirHandle.h"
#include "watchman/fs/FileSystem.h"
#include "watchman/listener.h"
#include "watchman/root/watchlist.h"
#include "watchman/sockname.h"
#include "watchman/state.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_stream.h"

#ifdef _WIN32
#include <Lmcons.h> // @manual
#include <Shlobj.h> // @manual
#include "watchman/thirdparty/deelevate_binding/include/deelevate.h" // @manual
#endif

using namespace watchman;

#ifdef __APPLE__
#include <mach-o/dyld.h> // @manual
#endif

namespace {
/// How should server requests be encoded?
PduFormat server_format{is_bser, 0};
/// How should output to stdout be encoded?
PduFormat output_format{is_json_pretty, 0};
} // namespace

namespace {
const std::string& get_pid_file() {
  // We defer computing this path until we're in the server context because
  // eager evaluation can trigger integration test failures unless all clients
  // are aware of both the pidfile and the sockpath being used in the tests.
  watchman::compute_file_name(
      flags.pid_file, computeUserName(), "pid", "pidfile");
  return flags.pid_file;
}
} // namespace

W_CAP_REG("bser-v2")

/**
 * Log and fatal if Watchman was started with a low priority, which can cause a
 * poor experience, as Watchman is unable to keep up with the filesystem's
 * change notifications, triggering recrawls.
 */
void detect_low_process_priority() {
#ifndef _WIN32
  // Since `-1` is a valid nice level, in order to detect an
  // error we clear errno first and then test whether it is
  // non-zero after we have retrieved the nice value.
  errno = 0;
  auto nice_value = nice(0);
  folly::checkPosixError(errno, "failed to get `nice` value");

  auto min_acceptable_nice_value = cfg_get_int("min_acceptable_nice_value", 0);
  if (nice_value > min_acceptable_nice_value) {
    logf(
        watchman::FATAL,
        "Watchman is running at a lower than normal priority. (nice_value={}, "
        "min_acceptable_nice_value={}). Since that results in poor performance "
        "that is otherwise very difficult to trace, diagnose and debug, "
        "Watchman is refusing to start.\n",
        nice_value,
        min_acceptable_nice_value);
  }
#endif
}

/*
 * Detect the command that starts watchman
 */
std::optional<std::string> detect_starting_command(pid_t ppid) {
#ifndef _WIN32
  try {
    auto processInfo = lookupProcessInfo(ppid).get();
    return processInfo.name;
  } catch (const std::exception& e) {
    logf(
        ERR,
        "Failed to lookup process info for pid {} exception {} \n",
        ppid,
        e.what());
  }

#endif
  return std::nullopt;
}

[[noreturn]] static void run_service(ProcessLock::Handle&&, pid_t ppid) {
#ifndef _WIN32
  // Before we redirect stdin/stdout to the log files, move any inetd-provided
  // socket to a different descriptor number.
  if (flags.inetd_style) {
    w_listener_prep_inetd();
  }
#endif

  // redirect std{in,out,err}
  int fd = folly::fileops::open("/dev/null", O_RDONLY);
  if (fd != -1) {
    ignore_result(::dup2(fd, STDIN_FILENO));
    folly::fileops::close(fd);
  }

  if (logging::log_name != "-") {
    fd = folly::fileops::open(
        logging::log_name.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd != -1) {
      ignore_result(::dup2(fd, STDOUT_FILENO));
      ignore_result(::dup2(fd, STDERR_FILENO));
      folly::fileops::close(fd);
    }
  }

  // If we weren't attached to a tty, check this now that we've opened
  // the log files so that we can log the problem there.
  //
  // This is unlikely to trip, as both foreground and daemonized execution
  // check process priority prior.
  detect_low_process_priority();

#ifndef _WIN32
  /* we are the child, let's set things up */
  ignore_result(chdir("/"));
#endif

  w_set_thread_name("listener");
  {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';
    auto startingCommandName = detect_starting_command(ppid);
    logf(
        ERR,
        "Watchman {} {} starting up on {} by command {}\n",
        PACKAGE_VERSION,
#ifdef WATCHMAN_BUILD_INFO
        WATCHMAN_BUILD_INFO,
#else
        "<no build info set>",
#endif
        hostname,
        startingCommandName.value_or("<unknown_command>"));
  }

#ifndef _WIN32
  // Block SIGCHLD by default; we only want it to be delivered
  // to the reaper thread and only when it is ready to reap.
  // This MUST happen before we spawn any threads so that they
  // can pick up our default blocked signal mask.
  {
    sigset_t sigset;

    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigset, nullptr);
  }
#endif

  bool res = false;
  {
    watchman::getThreadPool().start(
        cfg_get_int("thread_pool_worker_threads", 16),
        cfg_get_int("thread_pool_max_items", 1024 * 1024));

    ClockSpec::init();
    w_state_load();
    SCOPE_EXIT {
      w_state_shutdown();
    };
    res = w_start_listener();
    w_root_free_watched_roots();
    perf_shutdown();
    cfg_shutdown();
  }

  log(ERR, "Exiting from service with res=", res, "\n");

  if (res) {
    exit(0);
  }
  exit(1);
}

[[noreturn]] static void run_service_in_foreground() {
  detect_low_process_priority();
  close_random_fds();

  auto& pid_file = get_pid_file();
  auto processLock = ProcessLock::acquire(pid_file);
  run_service(processLock.writePid(pid_file), getppid());
}

namespace {
struct [[nodiscard]] SpawnResult {
  enum Status {
    Spawned,
    FailedToLock,
  };

  SpawnResult() = delete;

  /* implicit */ SpawnResult(Status s, std::string r = {})
      : status{s}, reason{std::move(r)} {}

  void exitIfFailed() {
    if (status == FailedToLock) {
      fprintf(stderr, "%s\n", reason.c_str());
      exit(1);
    }
  }

  Status status;

  /**
   * If status is not Spawned, then this contains the error message.
   */
  std::string reason;
};
} // namespace

#ifndef _WIN32
/**
 * Forks and daemonizes, starting the Watchman service in the child.
 */
static SpawnResult run_service_as_daemon() {
  detect_low_process_priority();
  close_random_fds();

  // Lock the pidfile before we daemonize so that errors can be detected
  // and returned (for logging) before we drop stderr. This prevents failure to
  // lock from causing the daemonize process to start and immediately exit with
  // an error, making it hard to track down why a command isn't succeeding.
  auto acquireResult = ProcessLock::tryAcquire(get_pid_file());
  if (auto* reason = std::get_if<std::string>(&acquireResult)) {
    return SpawnResult{SpawnResult::FailedToLock, *reason};
  }

  auto& processLock = std::get<ProcessLock>(acquireResult);
  auto parentPid = getppid();

  // the double-fork-and-setsid trick establishes a
  // child process that runs in its own process group
  // with its own session and that won't get killed
  // off when your shell exits (for example).
  if (fork()) {
    // The parent of the first fork is the client
    // process that is being run by the user, and
    // we want to allow that to continue.
    return SpawnResult::Spawned;
  }
  setsid();
  if (fork()) {
    // The parent of the second fork has served its
    // purpose, so we simply exit here, otherwise
    // we'll duplicate the effort of either the
    // client or the server depending on if we
    // return or not.
    _exit(0);
  }

  // We are the child. Let's populate the pid file and start listening on the
  // socket.
  run_service(processLock.writePid(get_pid_file()), parentPid);
}
#endif

#ifdef _WIN32
static SpawnResult spawn_win32(const std::vector<std::string>& daemon_argv) {
  char module_name[WATCHMAN_NAME_MAX];
  GetModuleFileName(nullptr, module_name, sizeof(module_name));

  ChildProcess::Options opts;
  opts.setFlags(POSIX_SPAWN_SETPGROUP);
  opts.nullStdin();
  opts.open(
      STDOUT_FILENO,
      logging::log_name.c_str(),
      O_WRONLY | O_CREAT | O_APPEND,
      0600);
  opts.dup2(STDOUT_FILENO, STDERR_FILENO);
  opts.chdir("/");

  std::vector<std::string_view> args{module_name, "--foreground"};
  for (auto& arg : daemon_argv) {
    args.push_back(arg);
  }

  ChildProcess proc(args, std::move(opts));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (proc.terminated()) {
    logf(
        ERR,
        "Failed to spawn watchman server; it exited with code {}.\n"
        "Check the log file at {} for more information\n",
        proc.wait(),
        logging::log_name);
    exit(1);
  }
  proc.disown();
  return SpawnResult::Spawned;
}
#endif

#ifndef _WIN32
// Spawn watchman via a site-specific spawn helper program.
// We'll pass along any daemon-appropriate arguments that
// we noticed during argument parsing.
static SpawnResult spawn_site_specific(
    const std::vector<std::string>& daemon_argv,
    const char* spawner) {
  std::vector<std::string_view> args;
  args.reserve(1 + daemon_argv.size());
  args.emplace_back(spawner);
  for (auto& arg : daemon_argv) {
    args.push_back(arg);
  }

  close_random_fds();

  // Note that we're not setting up the output to go to the log files
  // here.  This is intentional; we'd like any failures in the spawner
  // to bubble up to the user as having things silently fail and get
  // logged to the server log doesn't provide any obvious cues to the
  // user about what went wrong.  Watchman will open and redirect output
  // to its log files when it ultimately is launched and enters the
  // run_service() function above.
  // However, we do need to make sure that any output from both stdout
  // and stderr goes to stderr of the end user.
  ChildProcess::Options opts;
  opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
  opts.dup2(STDERR_FILENO, STDOUT_FILENO);
  opts.dup2(STDERR_FILENO, STDERR_FILENO);

  try {
    ChildProcess proc(args, std::move(opts));

    auto res = proc.wait();

    if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
      return SpawnResult::Spawned;
    }

    if (WIFEXITED(res)) {
      log(FATAL, spawner, ": exited with status ", WEXITSTATUS(res), "\n");
    } else if (WIFSIGNALED(res)) {
      log(FATAL, spawner, ": signaled with ", WTERMSIG(res), "\n");
    }
    log(FATAL, spawner, ": failed to start, exit status ", res, "\n");

  } catch (const std::exception& exc) {
    log(FATAL,
        "Failed to spawn watchman via `",
        spawner,
        "': ",
        exc.what(),
        "\n");
  }

  return SpawnResult::Spawned;
}
#endif

#ifdef __APPLE__

std::vector<std::string> escape_args_for_sh(
    const std::vector<std::string>& args) {
  std::vector<std::string> transformedArgs{};
  transformedArgs.reserve(args.size());

  for (auto& arg : args) {
    transformedArgs.push_back(folly::shellQuote(arg));
  }
  return transformedArgs;
}

std::string prep_args_for_plist(
    const std::vector<std::string>& args,
    size_t indentation) {
  std::vector<std::string> transformedArgs{};
  transformedArgs.reserve(args.size());
  for (auto& arg : args) {
    transformedArgs.push_back(
        fmt::format("{:>{}}<string>{}</string>\n", "", indentation, arg));
  }
  return folly::join("", transformedArgs);
}

static SpawnResult spawn_via_launchd() {
  char watchman_path[WATCHMAN_NAME_MAX];
  uint32_t size = sizeof(watchman_path);
  char plist_path[WATCHMAN_NAME_MAX];
  FILE* fp;

  close_random_fds();

  if (_NSGetExecutablePath(watchman_path, &size) == -1) {
    log(FATAL, "_NSGetExecutablePath: path too long; size ", size, "\n");
  }

  snprintf(
      plist_path,
      sizeof(plist_path),
      "%s/Library/LaunchAgents",
      getHomeDirectory().c_str());
  // Best effort attempt to ensure that the agents dir exists.  We'll detect
  // and report the failure in the fopen call below.
  mkdir(plist_path, 0755);
  snprintf(
      plist_path,
      sizeof(plist_path),
      "%s/Library/LaunchAgents/com.github.facebook.watchman.plist",
      getHomeDirectory().c_str());

  if (access(plist_path, R_OK) == 0) {
    // Unload any that may already exist, as it is likely wrong

    ChildProcess unload_proc(
        {"/bin/launchctl", "unload", "-F", plist_path},
        ChildProcess::Options());
    unload_proc.wait();

    // Forcibly remove the plist.  In some cases it may have some attributes
    // set that prevent launchd from loading it.  This can happen where
    // the system was re-imaged or restored from a backup
    unlink(plist_path);
  }

  fp = fopen(plist_path, "w");
  if (!fp) {
    log(FATAL,
        "Failed to open ",
        plist_path,
        " for write: ",
        folly::errnoStr(errno),
        "\n");
  }

  watchman::compute_file_name(
      flags.pid_file, computeUserName(), "pid", "pidfile");

  const char* path_env =
      Configuration().getString("subprocess_path_env", getenv("PATH"));
  // If subprocess_path_env is not set and PATH is not in the environment,
  // set the path to the empty string.
  if (path_env == nullptr) {
    path_env = "";
  }

  std::vector<std::string> watchman_args{
      watchman_path,
      "--foreground",
      fmt::format("--logfile={}", logging::log_name),
      fmt::format("--log-level={}", logging::log_level),
      fmt::format("--sockname={}", get_unix_sock_name()),
      fmt::format("--statefile={}", flags.watchman_state_file),
      fmt::format("--pidfile={}", flags.pid_file)};
  std::string watchman_spawning_command;

  auto spawn_with_sh = Configuration().getBool("macos_spawn_with_sh", false);
  if (spawn_with_sh) {
    watchman_args = escape_args_for_sh(std::move(watchman_args));
    watchman_args = {"/bin/sh", "-c", folly::join(" ", watchman_args)};
  }

  auto plist_content = fmt::format(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "    <key>Label</key>\n"
      "    <string>com.github.facebook.watchman</string>\n"
      "    <key>Disabled</key>\n"
      "    <false/>\n"
      "    <key>ProgramArguments</key>\n"
      "    <array>\n"
      "{}"
      "    </array>\n"
      "    <key>KeepAlive</key>\n"
      "    <dict>\n"
      "        <key>Crashed</key>\n"
      "        <true/>\n"
      "    </dict>\n"
      "    <key>RunAtLoad</key>\n"
      "    <true/>\n"
      "    <key>EnvironmentVariables</key>\n"
      "    <dict>\n"
      "        <key>PATH</key>\n"
      "        <string>"
      "{}"
      "</string>\n"
      "    </dict>\n"
      "    <key>ProcessType</key>\n"
      "    <string>Interactive</string>\n"
      "    <key>Nice</key>\n"
      "    <integer>-5</integer>\n"
      "</dict>\n"
      "</plist>\n",
      prep_args_for_plist(watchman_args, 8),
      path_env);
  fwrite(plist_content.data(), 1, plist_content.size(), fp);
  fclose(fp);
  // Don't rely on umask, ensure we have the correct perms
  chmod(plist_path, 0644);

  ChildProcess load_proc(
      {"/bin/launchctl", "load", "-F", plist_path}, ChildProcess::Options());
  auto res = load_proc.wait();

  if (WIFEXITED(res) && WEXITSTATUS(res) == 0) {
    return SpawnResult::Spawned;
  }

  // Most likely cause is "headless" operation with no GUI context
  if (WIFEXITED(res)) {
    logf(ERR, "launchctl: exited with status {}\n", WEXITSTATUS(res));
  } else if (WIFSIGNALED(res)) {
    logf(ERR, "launchctl: signaled with {}\n", WTERMSIG(res));
  }
  logf(ERR, "Falling back to daemonize\n");
  return run_service_as_daemon();
}
#endif

static void parse_encoding(std::string_view enc, PduType* pdu) {
  if (enc.empty()) {
    return;
  }
  if (enc == "json") {
    *pdu = is_json_compact;
    return;
  }
  if (enc == "bser") {
    *pdu = is_bser;
    return;
  }
  if (enc == "bser-v2") {
    *pdu = is_bser_v2;
    return;
  }
  log(ERR, "Invalid encoding '", enc, "', use one of json, bser or bser-v2\n");
  exit(EX_USAGE);
}

#ifdef _WIN32
bool initialize_winsock() {
  WSADATA wsaData;
  if ((WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) ||
      (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)) {
    return false;
  }
  return true;
}

bool initialize_uds() {
  if (!initialize_winsock()) {
    log(DBG, "unable to initialize winsock, disabling UDS support\n");
  }

  // Test if UDS support is present
  FileDescriptor fd(
      ::socket(PF_LOCAL, SOCK_STREAM, 0), FileDescriptor::FDType::Socket);

  bool fd_initialized = (bool)fd;

  if (!fd_initialized) {
    log(DBG, "unable to create UNIX domain socket, disabling UDS support\n");
    return false;
  }

  return true;
}
#endif

static void setup_sock_name() {
#ifdef _WIN32
  if (!initialize_uds()) {
    // if we can't create UNIX domain socket, disable it.
    disable_unix_socket = true;
  }
#endif

  auto user = computeUserName();

  // Precompute the temporary directory path in case this process's environment
  // changes.
  (void)getTemporaryDirectory();

#ifdef _WIN32
  // On Windows, if an application uses --sockname to override the named
  // pipe path so that it can isolate its watchman integration tests,
  // but doesn't also specify --unix-listener-path then we need to
  // take care to prevent using the default unix domain path which would
  // otherwise break their isolation.
  // If either option is specified without the other, then we disable
  // the use of the other.
  if (!flags.named_pipe_path.empty() || !flags.unix_sock_name.empty()) {
    disable_named_pipe = flags.named_pipe_path.empty();
    disable_unix_socket = flags.unix_sock_name.empty();
  }

  if (flags.named_pipe_path.empty()) {
    flags.named_pipe_path = fmt::format("\\\\.\\pipe\\watchman-{}", user);
  }
#endif
  watchman::compute_file_name(flags.unix_sock_name, user, "sock", "sockname");

  watchman::compute_file_name(
      flags.watchman_state_file, user, "state", "statefile");

  // Log file precedence (highest to lowest):
  // 1. --logfile CLI flag (log_name already set before this function)
  // 2. "log_dir" config key -> <log_dir>/<user>/log
  // 3. Default: <state_dir>/log (handled by compute_file_name below)
  if (logging::log_name.empty()) {
    const char* log_dir = cfg_get_string("log_dir", nullptr);
    if (log_dir) {
      if (log_dir[0] != '/'
#ifdef _WIN32
          && !(log_dir[0] && log_dir[1] == ':')
#endif
      ) {
        log(ERR,
            "log_dir must be an absolute path but '",
            log_dir,
            "' was provided.\n");
        exit(1);
      }
      auto log_directory = fmt::format("{}/{}", log_dir, user);
      std::error_code ec;
      watchman::create_log_dir(log_directory.c_str(), ec);
      if (ec) {
        logf(
            ERR,
            "failed to create log directory {}: {}\n",
            log_directory,
            ec.message());
        exit(1);
      }
      logging::log_name = fmt::format("{}/log", log_directory);
    }
  }

  // If log_name is still empty, compute_file_name populates it with
  // <state_dir>/log. If already set (by --logfile or log_dir above),
  // it just validates the path is absolute.
  watchman::compute_file_name(
      logging::log_name,
      user,
      "log",
      "logfile",
      /*require_absolute=*/logging::log_name != "-");
}

static ResultErrno<folly::Unit> try_command(
    const Command& command,
    int timeout) {
  auto stmResult = w_stm_connect(timeout * 1000);
  if (stmResult.hasError()) {
    return stmResult.error();
  }

  if (command.isNullCommand()) {
    // We've confirmed we can connect -- there's nothing else to do with the
    // null command.
    return folly::unit;
  }

  auto stream = std::move(stmResult).value();

  return command.run(
      *stream,
      flags.persistent,
      server_format,
      output_format,
      flags.yes_pretty ? Pretty::Yes
                       : (flags.no_pretty ? Pretty::No : Pretty::IfTty));
}

static bool try_client_mode_command(const Command& command, bool pretty) {
  auto client = std::make_shared<watchman::Client>();
  client->client_mode = true;

  bool res = client->dispatchCommand(command, CMD_CLIENT);

  if (!client->responses.empty()) {
    json_dumpf(
        client->responses.front(),
        stdout,
        pretty ? JSON_INDENT(4) : JSON_COMPACT);
    fmt::print("\n");
  }

  return res;
}

static std::vector<std::string> parse_cmdline(int* argcp, char*** argvp) {
  cfg_load_global_config_file();

  auto daemon_argv = watchman::parseOptions(argcp, argvp);
  watchman::getLog().setStdErrLoggingLevel(
      static_cast<watchman::LogLevel>(logging::log_level));
  setup_sock_name();
  parse_encoding(flags.server_encoding, &server_format.type);
  parse_encoding(flags.output_encoding, &output_format.type);
  if (flags.output_encoding.empty()) {
    output_format.type = flags.no_pretty ? is_json_compact : is_json_pretty;
  }

  // Prevent integration tests that call the watchman cli from
  // accidentally spawning a server.
  if (getenv("WATCHMAN_NO_SPAWN")) {
    flags.no_spawn = true;
  }

  return daemon_argv;
}

static Command build_command_from_stdin() {
  auto err = json_error_t();
  PduBuffer buf;

  auto cmd = buf.decodeNext(w_stm_stdin(), &err);

  if (buf.format.type == is_bser) {
    // If they used bser for the input, select bser for output
    // unless they explicitly requested something else
    if (flags.server_encoding.empty()) {
      server_format.type = is_bser;
    }
    if (flags.output_encoding.empty()) {
      output_format.type = is_bser;
    }
  } else if (buf.format.type == is_bser_v2) {
    // If they used bser v2 for the input, select bser v2 for output
    // unless they explicitly requested something else
    if (flags.server_encoding.empty()) {
      server_format.type = is_bser_v2;
    }
    if (flags.output_encoding.empty()) {
      output_format.type = is_bser_v2;
    }
  }

  if (!cmd) {
    fprintf(
        stderr,
        "failed to parse command from stdin: "
        "line %d, column %d, position %d: %s\n",
        err.line,
        err.column,
        err.position,
        err.text);
    exit(1);
  }
  return Command::parse(std::move(*cmd));
}

static Command build_command(int argc, char** argv) {
  if (flags.json_input_arg) {
    return build_command_from_stdin();
  }

  // Special case: no arguments means that we just want
  // to verify that the service is up, starting it if
  // needed.
  // TODO: Add telemetry. Does anyone actually do this?
  if (argc == 0) {
    return Command{nullptr};
  }

  w_string name = argv[0];
  std::vector<json_ref> args;
  for (int i = 1; i < argc; i++) {
    args.push_back(typed_string_to_json(argv[i], W_STRING_UNICODE));
  }

  return Command{std::move(name), json_array(std::move(args))};
}

static SpawnResult try_spawn_watchman(
    const std::vector<std::string>& daemon_argv) {
  // Every spawner that doesn't fork() this client process is susceptible to a
  // race condition if `watchman shutdown-server` and `watchman <command>` are
  // run in short order. The latter tries to spawn a daemon while the former is
  // still shutting down, holding the pid lock, and this causes it to time out
  // and fail. The solution would be to implement some kind of startup pipe that
  // allows the server to indicate to the client when it's done starting up,
  // communicating errors that are worthy of a retry.

#ifndef _WIN32
  if (flags.no_site_spawner) {
    // The astute reader will notice this we're calling run_service_as_daemon()
    // here and not the various other platform spawning functions in the block
    // further below in this function.  This is deliberate: we want
    // to do the most simple background running possible when the
    // no_site_spawner flag is used.   In the future we plan to
    // migrate the platform spawning functions to use the site_spawn
    // functionality.
    return run_service_as_daemon();
  }
  // If we have a site-specific spawning requirement, then we'll
  // invoke that spawner rather than using any of the built-in
  // spawning functionality.
  const char* site_spawn = cfg_get_string("spawn_watchman_service", nullptr);
  if (site_spawn) {
    return spawn_site_specific(daemon_argv, site_spawn);
  }
#endif

#if defined(__APPLE__)
  return spawn_via_launchd();
#elif defined(_WIN32)
  return spawn_win32(daemon_argv);
#else
  return run_service_as_daemon();
#endif
}

static int inner_main(int argc, char** argv) {
  // TODO: We used to avoid folly::init so it didn't interfere with our own
  // signal handling. We want to swap to folly signal handling, so we'll do a
  // full init on Linux to test it. We should remove this if in the future.
  if (kUseFollySignalHandler) {
    folly::unsafe_unscoped_init(
        &argc, &argv, folly::InitOptions().useGFlags(false));
  } else {
    folly::SingletonVault::singleton()->registrationComplete();
  }

  SCOPE_EXIT {
    if (!kUseFollySignalHandler) {
      folly::SingletonVault::singleton()->destroyInstancesFinal();
    }
  };

  auto daemon_argv = parse_cmdline(&argc, &argv);

#ifdef _WIN32
  // On Windows its not possible to connect to elevated Watchman daemon from
  // non-elevated processes. To ensure that Watchman daemon will always be
  // accessible, deelevate by default if needed.
  // Note watchman runs in some environments which require elevated
  // permissions, so we can not always de-elevate.
  if (Configuration().getBool("should_deelevate_on_startup", false)) {
    deelevate_requires_normal_privileges();
  }
#endif

  if (flags.foreground) {
    run_service_in_foreground();
  }

  w_set_thread_name("cli");
  auto cmd = build_command(argc, argv);
  cmd.validateOrExit(output_format);

  auto should_start = [](int err) -> bool {
    return err == ECONNREFUSED || err == ENOENT;
  };

  auto ran = try_command(cmd, 0);
  if (ran.hasError() && should_start(ran.error())) {
    if (flags.no_spawn) {
      if (!flags.no_local) {
        if (try_client_mode_command(cmd, !flags.no_pretty)) {
          ran = folly::unit;
        }
      }
    } else {
      // Failed to run command. Try to spawn a daemon.

      // Some site spawner scripts will asynchronously launch the service.
      // When that happens we may encounter ECONNREFUSED.  We need to
      // tolerate this, so we add some retries.
      int attempts = 10;
      std::chrono::milliseconds interval{10};

      bool spawned = false;
      while (true) {
        if (!spawned) {
          auto spawn_result = try_spawn_watchman(daemon_argv);
          switch (spawn_result.status) {
            case SpawnResult::Spawned:
              spawned = true;
              break;
            case SpawnResult::FailedToLock:
              // Otherwise, it's possible another daemon is still shutting down,
              // and we should try to start again next time. Alternatively,
              // another daemon is starting up, and when it's ready, the command
              // should succeed.
              break;
          }
        }

        ran = try_command(cmd, 10);
        if (ran.hasError() && should_start(ran.error()) && attempts-- > 0) {
          /* sleep override */ std::this_thread::sleep_for(interval);
          // 10 doublings of 10 ms is about 10 seconds total.
          interval *= 2;
          continue;
        }
        // Success or terminal failure
        break;
      }
    }
  }

  if (ran.hasValue()) {
    return 0;
  }

  if (!flags.no_spawn) {
    log(ERR,
        "unable to talk to your watchman on ",
        get_sock_name_legacy(),
        "! (",
        folly::errnoStr(ran.error()),
        ")\n");
#ifdef __APPLE__
    if (getenv("TMUX")) {
      logf(
          ERR,
          "\n"
          "You may be hitting a tmux related session issue.\n"
          "An immediate workaround is to run:\n"
          "\n"
          "    watchman version\n"
          "\n"
          "just once, from *outside* your tmux session, to allow the launchd\n"
          "registration to be setup.  Once done, you can continue to access\n"
          "watchman from inside your tmux sessions as usual.\n"
          "\n"
          "Longer term, you may wish to install this tool:\n"
          "\n"
          "    https://github.com/ChrisJohnsen/tmux-MacOSX-pasteboard\n"
          "\n"
          "and configure tmux to use `reattach-to-user-namespace`\n"
          "when it launches your shell.\n");
    }
#endif
  }
  return 1;
}

int main(int argc, char** argv) {
  try {
    return inner_main(argc, argv);
  } catch (const std::exception& e) {
    logf_stderr(
        "Uncaught C++ exception: {}\n", folly::exceptionStr(e).toStdString());
    return 1;
  } catch (...) {
    logf_stderr("Uncaught C++ exception: ...\n");
    return 1;
  }
}

/* vim:ts=2:sw=2:et:
 */
