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

/*

Welcome to the CLI Server!

This file implements a unix socket based server for spawning "requests" which
behave (by-in-large) as though they were scripts executing in the context of
the client's UID. In order to achieve this the file implements the following
classes and abstractions:

================================ [ The Server ] ================================

== CLIServer

This class is a singleton representing an active CLIServer bound to a unix
socket. It carries with it a state: UNDEFINED, READY, RUNNING, or STOPPED.

 - The server can be constructed via init_cli_server() which will call
   CLIServer::CLIServer. This will bind() the unix socket and construct
   the AsyncServerSocket.

 - In order to begin accepting connections, start_cli_server() must be called,
   it will spawn an event loop thread and begin accept()-ing connections. This
   function will also construct a JobQueueDispatcher to handle connections.

 - Once teardown_cli_server() is called the server will stop accepting new
   connections and shutdown the dispatcher. This function will join the event
   loop thread and then destruct the singleton instance of the CLIServer.

After constructing a CLIServer it is "READY" and once start() is called it will
either be "RUNNING" or if listen() or accept() fails "STOPPED".

A running CLIServer has an AsyncServerSocket bound to the unix socket on which
requests are served, as well as a std::thread executing the event loop for its
socket.

CLIServer offers separate init and start entry points in order to allow callers
to bind() the unix socket prior to dropping privileges, but defer new
connections until after privileges have been reset. This enables callers to
bind their sockets in privileged locations.

The teardown function must be called prior to process exit as JobQueue threads
may access global server state in their thread exit handlers.

== CLIWorker

These workers execute requests sent to the CLIServer and are managed by the
JobQueueDispatcher. The CLIWorker::doJob() function contains all of the server
side logic to extract the startup parameters from the client, manipulate the
ExecutionContext, and ultimately start the request via hphp_invoke.

Before beginning execution the doJob routine reads information from the client
over its unix socket. Most importantly it receives the client's stdin, stdout,
and stderr file-descriptors, a file-descriptor to communicate with the client's
light process pool, the command line arguments, and a JSON blob of INI settings.

The setup performed in doJob resembles normal command line execution. Notably,
however, the IniSetting::Mode::Request options are read from the client as a
JSON string and used to override the server defaults, the STD[IN,OUT,ERR]
constants are defined the ExecutionContext is instructed to write to the
client's stdout, and the LightProcess abstraction is given an override socket to
allow the client to spawn child processes via its own light process pool (so
that commands executed by proc_open and friends have the correct uid/gid).
Additionally the cwd is set to the client's cwd, the is_cli_server_mode()
function returns true, and safe directory access is disabled.

Lastly, a special file wrapper, CLIWrapper, is set to handle all local file
system operations.

== Monitor thread

The CLIWorker will spawn a special monitor thread just prior to starting
the request. This thread will poll() the unix socket and wait for flags that
indicate the client has hung-up, the socket is in an error state, or the
file-descriptor is no longer valid. At this point it will set the TimedOutFlag
on the request and terminate. This ensure that if the client hangs up (by ^C
for instance), the server does not continue spinning, and promptly releases the
clients I/O file descriptors (which may remain open if the underlying terminal
is still active).

== CLIWrapper

This special Stream::Wrapper implements all file:// IO for requests handled by
CLIServer. It proxies all syscalls to the client process over the unix socket
connection. The client responds with the results of these calls, and in the
case of open() and opendir(), with the actual file-descriptors it opened (via
SCM_RIGHTS)

================================ [ The Client ] ================================

== cli_process_command_loop

After establishing a connection and transferring INI-settings and file-
descriptors to the server, the client sits in the cli_process_command_loop. In
this loop the client handles requests from the CLIWrapper, commands are sent
as strings followed by arguments. The special "exit" command is sent when
the request completes along with an argument indicating the code with which
the client should terminate.

== run_command_on_cli_server

This is the main entry point for the client. It attempts to establish a
connection with the CLIServer and transfers the initialization information and
file descriptors required by the server. The file descriptor for a specially
constructed light process pool is also sent to the server. If the client is
unable to establish a connection the function will return allowing the caller
to execute locally or fail. If an error occurs after the connection is
established or the server completes the request the function will not return.
Should an error occur executing locally is unsafe as the client will have no
way to determine how much progress the server made.

*/

#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/cli-server-ext.h"

#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/file-stream-wrapper.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/surprise-flags.h"
#include "hphp/runtime/base/stream-wrapper-registry.h"
#include "hphp/runtime/base/stream-wrapper.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/tracing.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/util/alloc.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/ext/hash/hash_murmur.h"
#include "hphp/runtime/server/job-queue-vm-stack.h"
#include "hphp/util/afdt-util.h"
#include "hphp/util/configs/errorhandling.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/configs/xbox.h"
#include "hphp/util/job-queue.h"
#include "hphp/util/light-process.h"
#include "hphp/util/logger.h"
#include "hphp/util/trace.h"
#include "hphp/util/user-info.h"
#include "hphp/zend/zend-strtod.h"

#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/Sockets.h>

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <utime.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <systemd/sd-daemon.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>


TRACE_SET_MOD(clisrv)

#include "hphp/runtime/server/cli-server-impl.h"

namespace HPHP {

namespace {
/*
 *               READ BEFORE MODIFYING THE CLI SERVER PROTOCOL
 * ===== WARNING ===== WARNING ===== WARNING ===== WARNING ===== WARNING =====
 *
 * The CLI client and server use cli_server_api_version() to negotiate a
 * conncection, any changes to their API must include a bump of this version.
 * Version is not tied to HHVM compiler-id because changes rarely affect
 * communication between the server and client.
 *
 * Adding, removing, or changing the signatures of functions using the
 * CLI_REGISTER_HANDLER macro does not require a change here.
 *
 * ===== WARNING ===== WARNING ===== WARNING ===== WARNING ===== WARNING =====
 */
const uint32_t CLI_SERVER_API_BASE_VERSION = 6;
std::atomic<uint64_t> s_cliServerComputedVersion(0);

constexpr std::array<char, 8> kMagicStr{'H','H','V','M',' ','C','L','I'};
constexpr uint64_t kMagic = std::bit_cast<uint64_t>(kMagicStr);

// When running with Eval.UnixServerRunPSPInBackground this pipe allows us to
// send an exit code to the foreground process prior to PSP completing.
int s_foreground_pipe = -1;

}

struct CLIClientGuardedFile : PlainFile {
  explicit CLIClientGuardedFile(int fd, const char* mode) :
    PlainFile(fdopen(fd, mode))
  {
    setFd(fd);
  }

  int64_t readImpl(char *buffer, int64_t length) override {
    assertClientAlive();
    return PlainFile::readImpl(buffer, length);
  }
  int getc() override {
    assertClientAlive();
    return PlainFile::getc();
  }
  OptString read() override {
    assertClientAlive();
    return PlainFile::read();
  }
  OptString read(int64_t length) override {
    assertClientAlive();
    return PlainFile::read(length);
  }
  int64_t writeImpl(const char *buffer, int64_t length) override {
    assertClientAlive();
    return PlainFile::writeImpl(buffer, length);
  }
  bool seek(int64_t offset, int whence = SEEK_SET) override {
    assertClientAlive();
    return PlainFile::seek(offset, whence);
  }
  int64_t tell() override {
    assertClientAlive();
    return PlainFile::tell();
  }
  bool eof() override {
    assertClientAlive();
    return PlainFile::eof();
  }
  bool rewind() override {
    assertClientAlive();
    return PlainFile::rewind();
  }
  bool flush() override {
    assertClientAlive();
    return PlainFile::flush();
  }
  bool truncate(int64_t size) override {
    assertClientAlive();
    return PlainFile::truncate(size);
  }

 private:
  void assertClientAlive() {
    if (stackLimitAndSurprise().getFlag(CLIClientTerminated)) {
      raise_fatal_error("File I/O blocked as CLI client terminated");
    }
  }
};
static_assert(sizeof(CLIClientGuardedFile) == sizeof(PlainFile),
              "CLIClientGuardedFile inherits PlainFile::heapSize()");


int cli_read_fd(int afdt_fd) {
  int fd = afdt::recv_fd(afdt_fd);
  FTRACE(4, "{} = cli_read_fd({})\n", fd, afdt_fd);
  if (fd < 0) {
    throw Exception("Failed in afdt::recv_fd: %s",
                    folly::errnoStr(errno).c_str());
  }
  return fd;
}

void cli_write_fd(int afdt_fd, int fd) {
  FTRACE(4, "cli_write_fd({}, {})\n", afdt_fd, fd);
  if (!afdt::send_fd(afdt_fd, fd)) {
    throw Exception("Failed in afdt::send_fd: %s",
                    folly::errnoStr(errno).c_str());
  }
}

namespace {

#ifdef SCM_CREDENTIALS

template<class F, class R = typename std::invoke_result<F, msghdr*>::type>
R with_ucred_message(int fd, F func) {
  iovec iov;
  int data = 1;
  msghdr msgh;
  union {
    struct cmsghdr cmh;
    char space[CMSG_SPACE(sizeof(ucred))];
  } control_un;

  bzero(&control_un, sizeof(control_un));
  bzero(&msgh, sizeof(msgh));

  control_un.cmh.cmsg_len = CMSG_LEN(sizeof(ucred));
  control_un.cmh.cmsg_level = SOL_SOCKET;
  control_un.cmh.cmsg_type = SCM_CREDENTIALS;

  iov.iov_base = &data;
  iov.iov_len = sizeof(data);

  msgh.msg_iov = &iov;
  msgh.msg_iovlen = 1;
  msgh.msg_control = &control_un.cmh;
  msgh.msg_controllen = sizeof(control_un);

  auto set_passcred = [&] (int val) {
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) == -1) {
      throw Exception("Could not set socket to SO_PASSCRED: %s",
                      folly::errnoStr(errno).c_str());
    }
  };

  set_passcred(1);
  SCOPE_EXIT { set_passcred(0); };
  return func(&msgh);
}

ucred cli_read_ucred(int fd) {
  return with_ucred_message(fd, [&] (msghdr* msgh) -> ucred {
    ssize_t ret = recvmsg(fd, msgh, MSG_WAITALL);

    if (ret < 0) {
      FTRACE(4, "cli_read_ucred({}): %s\n", fd, folly::errnoStr(errno));
      throw Exception("cli_read_ucred: recvmsg failed: %s",
                      folly::errnoStr(errno).c_str());
    }

    auto cmhp = CMSG_FIRSTHDR(msgh);
    if (!cmhp ||
        cmhp->cmsg_len != CMSG_LEN(sizeof(ucred)) ||
        cmhp->cmsg_level != SOL_SOCKET ||
        cmhp->cmsg_type != SCM_CREDENTIALS) {
      FTRACE(4, "cli_read_ucred({}): bad payload\n", fd);
      throw Exception("cli_read_ucred: malformed response");
    }


    auto uc = (ucred*)CMSG_DATA(cmhp);

    FTRACE(4, "cli_read_ucred({}): ret = {} UID = {}, GID = {}, PID = {}\n",
           fd, ret, uc->uid, uc->gid, uc->pid);

    return ucred {uc->pid, uc->uid, uc->gid};
  });
}

void cli_write_ucred(int fd) {
  return with_ucred_message(fd, [&] (msghdr* msgh) {
    auto cred = (ucred*)CMSG_DATA(CMSG_FIRSTHDR(msgh));

    cred->uid = getuid();
    cred->gid = getgid();
    cred->pid = getpid();

    ssize_t ret = sendmsg(fd, msgh, MSG_WAITALL);

    FTRACE(4, "cli_write_ucred({}): ret = {} UID = {}, GID = {}, PID = {}\n",
           fd, ret, cred->uid, cred->gid, cred->pid);

    if (ret < 0) {
      FTRACE(4, "cli_write_ucred({}): %s\n", fd, folly::errnoStr(errno));
      throw Exception("cli_write_ucred: sendmsg failed: %s",
                      folly::errnoStr(errno).c_str());
    }
  });
}

#else /* defined(SCM_CREDENTIALS) */

ucred cli_read_ucred(int fd) {
  ucred u;
  if (getpeereid(fd, &u.uid, &u.gid) == -1) {
    throw Exception("getpeerid: %s", folly::errnoStr(errno).c_str());
  }
  u.pid = getpid();

  return u;
}
void cli_write_ucred(int fd) {}

#endif /* defined(SCM_CREDENTIALS) */

////////////////////////////////////////////////////////////////////////////////

struct CLIWorker
  : JobQueueWorker<int,void*,true,false,JobQueueDropVMStack>
{
  void doJob(int fd) override;
  void abortJob(int fd) override {
    Logger::Warning("CLI request (%i) dropped because of timeout.", fd);
    close(fd);
  }
};

struct CLIWrapper final : Stream::ExtendedWrapper {
  explicit CLIWrapper(int fd) : m_cli_fd(fd) {}

  req::ptr<File> open(const OptString& filename,
                      const OptString& mode,
                      int options,
                      const req::ptr<StreamContext>& context) override;
  int access(const OptString& path, int mode) override;
  int lstat(const OptString& path, struct stat* buf) override;
  int stat(const OptString& path, struct stat* buf) override;
  int unlink(const OptString& path) override;
  int rename(const OptString& oldname, const OptString& newname) override;
  int mkdir(const OptString& path, int mode, int options) override;
  int rmdir(const OptString& path, int options) override;
  req::ptr<Directory> opendir(const OptString& path) override;
  OptString realpath(const OptString& path) override;

  // extended api
  bool touch(const OptString& path, int64_t mtime, int64_t atime) override;
  bool chmod(const OptString& path, int64_t mode) override;
  bool chown(const OptString& path, int64_t uid) override;
  bool chown(const OptString& path, const OptString& uid) override;
  bool chgrp(const OptString& path, int64_t gid) override;
  bool chgrp(const OptString& path, const OptString& gid) override;

  Optional<std::string> getxattr(const char* path, const char* xattr) override;

  bool isNormalFileStream() const override { return true; }

private:
  int m_cli_fd;
};

struct CLIServer final : folly::AsyncServerSocket::AcceptCallback {
  explicit CLIServer(const char* path);
  ~CLIServer() override = default;

  void start();
  void stop();

  void connectionAccepted(folly::NetworkSocket fdNetworkSocket,
                          const folly::SocketAddress&,
                          AcceptInfo /* info */) noexcept override {

    int fd = fdNetworkSocket.toFd();

    if (Cfg::Eval::UnixServerFailWhenBusy) {
      if (m_dispatcher->getActiveWorker() >=
          m_dispatcher->getTargetNumWorkers()) {
        Logger::Info("Queue is full, refusing CLI connection");
        close(fd);
        return;
      }
    }

    Logger::Verbose("Accepting CLI connection");
    FTRACE(1, "CLIServer::connectionAccepted({}): enqueue job\n", fd);
    int opts = fcntl(fd, F_GETFL);
    int res = fcntl(fd, F_SETFL, opts & ~O_NONBLOCK);
    if (res) {
      Logger::Warning("Could not set socket to blocking: %s",
                      folly::errnoStr(errno).c_str());
      close(fd);
      return;
    }

    m_dispatcher->enqueue(fd);
  }

  using folly::AsyncServerSocket::AcceptCallback::acceptError;
  void acceptError(const std::exception& ex) noexcept override {
    Logger::Warning("Error accepting connection: %s", ex.what());
  }

  int getActiveWorkers() const {
    return m_dispatcher ? m_dispatcher->getActiveWorker() : 0;
  }

private:
  enum class State { UNINIT, READY, RUNNING, STOPPED };
  using JobQueue = JobQueueDispatcher<CLIWorker>;

  std::unique_lock<std::mutex> lockState() {
    return std::unique_lock<std::mutex>(m_stateLock);
  }
  void waitFor(State s) {
    auto lock = lockState();
    m_stateCV.wait(lock, [=] {
      return m_state.load(std::memory_order_acquire) == s;
    });
  }
  void waitWhile(State s) {
    auto lock = lockState();
    m_stateCV.wait(lock, [=] {
      return m_state.load(std::memory_order_acquire) != s;
    });
  }
  void setState(State s) {
    auto lock = lockState();
    m_state.store(s, std::memory_order_release);
    lock.unlock();
    m_stateCV.notify_all();
  }

  State getState() const { return m_state.load(std::memory_order_acquire); }

  folly::EventBase* m_ev{nullptr};
  std::atomic<State> m_state{State::UNINIT};
  std::mutex m_stateLock;
  std::condition_variable m_stateCV;
  std::thread m_mainThread;

  folly::AsyncServerSocket::UniquePtr m_server;
  std::unique_ptr<JobQueue> m_dispatcher;
};

CLIServer* s_cliServer{nullptr};
std::map<std::string, void(*)(detail::CLIServerInterface&)> s_extensionHandlers;

__thread Array* tl_env{nullptr};
__thread CLIContext* tl_context{nullptr};

int cli_sock() { assertx(tl_context); return tl_context->client(); }

////////////////////////////////////////////////////////////////////////////////

CLIServer::CLIServer(const char* path) {
  int err = unlink(path);
  if (err != 0 && errno != ENOENT) {
    Logger::Warning("Could not delete defunct unix socket: %s (%s)",
                    path, folly::errnoStr(errno).c_str());

    setState(State::STOPPED);
    return;
  }

  folly::SocketAddress addr;
  addr.setFromPath(path);
  m_server.reset(new folly::AsyncServerSocket());

  try {
    m_server->bind(addr);
  } catch (const std::exception& ex) {
    Logger::Warning("Unable to bind unix socket: %s", ex.what());
    m_server.reset();
    setState(State::STOPPED);
    return;
  }

  if (chmod(path, 0666) == -1) {
    Logger::Warning("Unable to chmod unix socket: %s",
                    folly::errnoStr(errno).c_str());
  }

  setState(State::READY);
}

void CLIServer::start() {
  if (getState() != State::READY) return;

  m_dispatcher = std::make_unique<JobQueue>(
    Cfg::Eval::UnixServerWorkers,
    Cfg::Eval::UnixServerWorkers,
    Cfg::Server::ThreadDropCacheTimeoutSeconds,
    Cfg::Server::ThreadDropStack,
    nullptr
  );

  m_dispatcher->start();

  m_mainThread = std::thread([&] {
    m_ev = folly::EventBaseManager::get()->getEventBase();

    Logger::Info("CLI server thread starting");
    FTRACE(1, "CLIServer::start(): starting...\n");

    try {
      m_server->listen(Cfg::Server::Backlog);
      m_server->startAccepting();
    } catch (const std::exception& ex) {
      Logger::Warning("Unable to begin accepting CLI connections: %s",
                      ex.what());
      m_dispatcher->stop();
      m_server.reset();
      m_dispatcher.reset();
      setState(State::STOPPED);
      return;
    }

    m_server->attachEventBase(m_ev);
    m_server->addAcceptCallback(this, nullptr);
    setState(State::RUNNING);

    if (Cfg::Eval::UnixServerNotifySystemd) sd_notify(0, "READY=1");

    m_ev->loop();
    Logger::Info("CLI server thread terminating");
  });

  waitWhile(State::READY);
}

void CLIServer::stop() {
  if (getState() != State::RUNNING) return;

  m_ev->runInEventBaseThread([&] {
    FTRACE(1, "CLIServer::stop(): stopping...\n");
    m_server.reset();

    setState(State::STOPPED);
  });

  m_dispatcher->stop();
  m_dispatcher.reset();
  waitFor(State::STOPPED);

  m_mainThread.join();
}

////////////////////////////////////////////////////////////////////////////////

struct CliStdoutHook final : ExecutionContext::StdoutHook {
  int fd;
  explicit CliStdoutHook(int fd) : fd(fd) {}
  void operator()(const char* s, int len) override {
    if (!stackLimitAndSurprise().getFlag(CLIClientTerminated)) {
      if (is_hphp_session_initialized() && !MemoryManager::exiting()) {
        write(fd, s, len);
      }
    }
  }
};

struct CliLoggerHook final : LoggerHook {
  int fd;
  explicit CliLoggerHook(int fd) : fd(fd) {}
  void operator()(const char* /*hdr*/, const char* msg, const char* ending)
       override {
    if (!stackLimitAndSurprise().getFlag(CLIClientTerminated)) {
      if (is_hphp_session_initialized() && !MemoryManager::exiting()) {
        write(fd, msg, strlen(msg));
        if (ending) write(fd, ending, strlen(ending));
      }
    }
  }
};

void load_ini_settings(const folly::dynamic& ini) {
  FTRACE(1, "init_ini_settings: CLI server decoding settings\n");

  if (!ini.isObject()) {
    Logger::Warning("CLI server received malformed INI settings");
    return;
  }

  FTRACE(1, "init_ini_settings: {} settings to process\n", ini.size());

  UNUSED int count = 0;
  for (auto& [name, opt] : ini.items()) {
    if (!name.isString() || !opt.isObject() || !opt.count("local_value") ||
        !opt.count("access") || !opt["access"].isInt()) {
      Logger::Warning("CLI server received a malformed INI setting value");
      continue;
    }

    auto const nameStr = name.asString();
    auto value = opt["local_value"];
    if (!IniSetting::canSet(opt["access"].asInt(), IniSetting::Mode::Request)) {
      FTRACE(5, "init_ini_settings: skipping INI setting {}\n", nameStr);
      continue;
    }
    FTRACE(5, "init_ini_settings: loading INI setting {} = {}\n", nameStr,
           folly::toJson(value));

    bool res = false;
    if (value.isString()) res = IniSetting::SetUser(nameStr, value.asString());
    if (value.isInt())    res = IniSetting::SetUser(nameStr, value.asInt());
    if (value.isBool())   res = IniSetting::SetUser(nameStr, value.asBool());
    if (value.isDouble()) res = IniSetting::SetUser(nameStr, value.asDouble());
    if (!res) {
      FTRACE(5, "init_ini_settings: unable to set Mode::Request setting: {} "
             "(access = {})\n", nameStr, opt["access"].asInt());
      Logger::Warning("CLI server received an invalid INI setting: %s "
                      "(access = %" PRId64 ")",
                      nameStr.c_str(), opt["access"].asInt());
    } else {
      count++;
    }
  }

  FTRACE(1, "init_ini_settings: loaded {} settings\n", count);
}

std::unordered_set<uid_t> s_allowedUsers;
std::unordered_set<gid_t> s_allowedGroups;

void check_cli_server_access(ucred& cred) {
  // Fail closed
  if (s_allowedUsers.empty() && s_allowedGroups.empty()) {
    if (cred.uid == getuid()) return;
    throw Exception("access denied");
  }

  FTRACE(2, "Checking access for uid = {}\n", cred.uid);

  // Fast checks
  if (s_allowedUsers.find(cred.uid) != s_allowedUsers.end()) return;
  if (s_allowedGroups.find(cred.gid) != s_allowedGroups.end()) return;

  try {
    if (!s_allowedGroups.empty()) {
      FTRACE(2, "check_cli_server_access: starting slow check...\n");

      std::vector<gid_t> groups;
      int ngroups = 0;
      UserInfo user(cred.uid);
      if (getgrouplist(user.pw->pw_name, cred.gid, nullptr, &ngroups) != -1) {
        FTRACE(2, "check_cli_server_access: oops 1...\n");
        throw Exception("getgrouplist: could not get ngroups");
      }
      FTRACE(2, "Will check {} groups for user {}\n",
             ngroups, user.pw->pw_name);
      groups.resize(ngroups);
      if (getgrouplist(user.pw->pw_name, cred.gid, &groups[0], &ngroups) < 0) {
        FTRACE(2, "check_cli_server_access: oops 2...\n");
        throw Exception("getgrouplist: invalid return value");
      }
      for (int i = 0; i < ngroups; ++i) {
        FTRACE(2, "Checking access for gid = {}\n", groups[i]);
        if (s_allowedGroups.find((gid_t)groups[i]) != s_allowedGroups.end()) {
          return;
        }
      }
    }
  } catch (const Exception& ex) {
    Logger::Warning("Error checking access to CLI server: %s", ex.what());
  }

  throw Exception("access denied");
}

struct MonitorThread final {
  explicit MonitorThread(int client);
  MonitorThread(const MonitorThread&) = delete;
  MonitorThread& operator=(const MonitorThread&) = delete;
  ~MonitorThread() {
    join();
    ::close(m_rpipe);
    ::close(m_wpipe);
  }
  void join() {
    if (m_monitor.joinable()) {
      ::write(m_wpipe, "stop", 5);
      m_monitor.join();
    }
  }

private:
  int m_rpipe{-1};
  int m_wpipe{-1};
  std::thread m_monitor;
};

MonitorThread::MonitorThread(int client) {
  int monitor_pipe[2];
  if (::pipe(monitor_pipe) == -1) {
    throw Exception("Unable to construct pipe for monitor: %s",
                    folly::errnoStr(errno).c_str());
  }

  m_rpipe = monitor_pipe[0];
  m_wpipe = monitor_pipe[1];
  auto flags = &stackLimitAndSurprise();
  try {
    m_monitor = std::thread([this,client,flags] {
      int ret = 0;
      ::pollfd pfd[] = {{client, 0, 0}, {m_rpipe, POLLIN, 0}};
      while ((ret = poll(pfd, 2, -1)) != -1) {
        if (ret == 0) continue;
        if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
          FTRACE(2, "CLIWorker::doJob({}): observed socket reset in monitor\n",
                 client);
          Logger::Info("CLIWorker::doJob(%i): monitor thread aborting request",
                       client);
          flags->setFlag(CLIClientTerminated);
          return;
        }
        if (pfd[1].revents) {
          FTRACE(2, "CLIWorker::doJob({}): monitor got request completed\n",
                 client);
          return;
        }
      }
      Logger::Warning("CLIWorker::doJob(%i): monitor thread terminated: %s",
                      client, folly::errnoStr(errno).c_str());
      FTRACE(2, "CLIWorker::doJob({}): got error in monitor thread: {}",
             client, folly::errnoStr(errno));
    });
  } catch (const std::system_error& err) {
    throw Exception("Could not start monitor thread: %s", err.what());
  }
}

////////////////////////////////////////////////////////////////////////////////

int read_file_helper(int client, const char* name) {
  auto const fd = cli_read_fd(client);
  FTRACE(2, "CLIContext::initFromClient({}): {} = {}\n", client, name, fd);
  return fd;
}

FILE* read_file_helper(int client, const char* name, const char* mode) {
  return fdopen(read_file_helper(client, name), mode);
}

folly::dynamic get_setting(
  const folly::dynamic& ini,
  const std::string& setting
) {
  if (!ini.count(setting)) return {};
  if (!ini[setting].isObject() || !ini[setting].count("local_value")) return {};
  return ini[setting]["local_value"];
}

int64_t get_setting_int(const folly::dynamic& ini, const std::string& setting) {
  auto const r = get_setting(ini, setting);
  if (r.isString()) {
    if (auto const v = folly::tryTo<int64_t>(r.asString())) return v.value();
  }
  return r.isInt() ? r.asInt() : 0;
}

bool get_setting_bool(const folly::dynamic& ini, const std::string& setting) {
  auto const r = get_setting(ini, setting);
  if (r.isString()) {
    if (auto const v = folly::tryTo<int64_t>(r.asString())) return v.value();
  }
  return r.isBool() && r.asBool();
}

std::string get_setting_string(
  const folly::dynamic& ini,
  const std::string& setting,
  const std::string& def
) {
  auto const r = get_setting(ini, setting);
  return r.isString() ? r.asString() : def;
}

Array init_cli_globals(int argc, char** argv,
                       const folly::dynamic& ini, char** envp) {
  auto make_map = [] (const folly::dynamic& v) {
    std::map<std::string,std::string> ret;
    if (!v.isObject()) return ret;
    for (auto& [k, v] : v.items()) {
      if (v.isString()) {
        ret.emplace(k.asString(), v.asString());
      }
    }
    return ret;
  };

  auto retEnv = empty_dict_array();
  auto envVariables = make_map(get_setting(ini, "hhvm.env_variables"));
  auto serverVariables = make_map(get_setting(ini, "hhvm.server_variables"));

  for (const auto& envvar : envVariables) {
    retEnv.set(OptString(envvar.first), OptString(envvar.second));
  }

  for (auto env = envp; *env; ++env) {
    char *p = strchr(*env, '=');
    if (p) {
      OptString name(*env, p - *env, CopyString);
      OptString val(p + 1, CopyString);
      retEnv.set(name, val);
    }
  }

  init_command_line_globals(argc, argv, envp, serverVariables, envVariables);

  return retEnv;
}

template<class Init, class Run, class Finish, class Teardown>
void runInContext(CLIContext&& ctx,
                  bool xbox,
                  bool enableBackgroundPSP,
                  Init&& init,
                  Run&& run,
                  Finish&& finish,
                  Teardown&& teardown) {
  auto data = ctx.getData();
  auto shared = ctx.getShared();
  std::vector<char*> argv;
  std::vector<char*> envp;
  int ret = EXIT_FAILURE;

  if (Cfg::ErrorHandling::VerboseCLIServerLogs && !xbox) {
    std::string uname;
    try {
      UserInfo info{shared->user.uid};
      uname = info.pw->pw_name;
    } catch (const Exception&) {
      uname = "<unknown>";
    }
    Logger::FInfo(
      "Starting command `{}' for user {}",
      folly::join(" ", shared->argv),
      uname
    );
  }

  auto const pspInBackground = get_setting_bool(
    shared->ini,
    "hhvm.unix_server_run_psp_in_background"
  );

  auto const prelude = get_setting_string(
    shared->ini,
    "hhvm.prelude_path",
    Cfg::Eval::PreludePath
  );

  for (auto& a : shared->argv) argv.emplace_back(a.data());
  for (auto& e : shared->envp) envp.emplace_back(e.data());
  argv.emplace_back(nullptr);
  envp.emplace_back(nullptr);

  tracing::Request _{
    xbox ? "cli-server-request-xbox" : "cli-server-request",
    argv[0],
    shared->uuid,
    [&] { return tracing::Props{}.add("file", argv[0]); }
  };

  init(shared->cwd, argv);
  SCOPE_EXIT { teardown(); };

  load_ini_settings(shared->ini);
  auto env = init_cli_globals(argv.size() - 1, &argv[0],
                              shared->ini, &envp[0]);

  MonitorThread monitor(data->client);
  CliStdoutHook stdout_hook(fileno(data->out));
  CliLoggerHook logging_hook(fileno(data->err));
  CLIWrapper wrapper(data->client);

  g_context->addStdoutHook(&stdout_hook);
  setThreadLocalIO(data->in, data->out, data->err);
  LightProcess::setThreadLocalAfdtOverride(data->lwp_afdt);
  Logger::SetThreadHook(&logging_hook);
  Stream::setThreadLocalFileHandler(&wrapper);
  RID().setSafeFileAccess(false);
  tl_env = &env;
	tl_context = &ctx;

  SCOPE_EXIT {
    finish(argv[0]);

    LightProcess::shutdownDelegate();
    LightProcess::setThreadLocalAfdtOverride(nullptr);
    Stream::setThreadLocalFileHandler(nullptr);
    Logger::SetThreadHook(nullptr);
    Logger::ClearThreadLog();
    clearThreadLocalIO();
    tl_env = nullptr;
    tl_context = nullptr;
    env.detach();

    try {
      cli_write(data->client, "exit", ret);
      if (!xbox) {
        if (ret != 0) {
          Logger::FWarning(
            "Command `{}' completed with non-zero return code: {}",
            folly::join(" ", shared->argv),
            ret
          );
        } else if (Cfg::ErrorHandling::VerboseCLIServerLogs) {
          Logger::FInfo("Command completed successfully");
        }
      }
    } catch (const Exception& ex) {
      Logger::Warning("Could not send exit code %i to CLI socket: %s\n",
                      ret, ex.what());
    }
  };

  ret = run(argv[0], prelude);

  if (enableBackgroundPSP && pspInBackground) {
    g_context->obFlushAll();
    Logger::SetThreadHook(nullptr);
    clearThreadLocalIO();
    cli_write(data->client, "background", ret);
  }
}


////////////////////////////////////////////////////////////////////////////////

void CLIWorker::doJob(int client) {
  FTRACE(1, "CLIWorker::doJob({}): starting job...\n", client);
  try {
    runInContext(
      CLIContext::initFromClient(client),
      false, // xbox
      true,  // enableBackgroundPSP
      [&] (const std::string& cwd, std::vector<char*>& argv) {
        init_command_line_session(argv.size() - 1, &argv[0]);
        g_context->setCwd(cwd);
      },
      [&] (const char* argv0, const std::string& prelude) {
        if (tl_context->getShared()->flags & CLIContext::AssumeRepoReadable) {
          auto const& ro = RepoOptions::forFile(argv0);
          if (!ro.path().empty()) {
            g_context->onLoadWithOptions(argv0, ro);
            tl_context->getShared()->repo = ro.dir();
          }
        }

        bool error;
        std::string errorMsg;
        auto const invoke_result = hphp_invoke(
          g_context.getNoCheck(),
          argv0,
          false,
          null_array,
          nullptr,
          "",
          "",
          error,
          errorMsg,
          true /* once */,
          false /* warmup only */,
          false /* richErrorMsg */,
          prelude,
          true /* allowDynCallNoPointer */
        );
        return invoke_result ? *rl_exit_code : EXIT_FAILURE;
      },
      [&] (const char* argv0) {
        execute_command_line_end(true, argv0, false);
      },
      [&] {
        hphp_context_exit();
        hphp_session_exit();
      }
    );
  } catch (const Exception& ex) {
    Logger::Warning("CLI Job failed: %s", ex.what());
  } catch (const std::exception& ex) {
    Logger::FError("CLI Job failed with C++ exception: {}", ex.what());
  } catch (...) {
    Logger::Error("CLI Job failed with unknown exception");
  }

  FTRACE(1, "CLIWorker::doJob({}): done.\n", client);
}

////////////////////////////////////////////////////////////////////////////////

namespace {
bool path_in_repo(const OptString& path) {
  auto const& p = tl_context->getShared()->repo;
  if (p.empty()) return false;
  return boost::starts_with(std::filesystem::path{path.toCppString()}, p);
}

bool use_local_fs(const OptString& path) {
  if (!(tl_context->getShared()->flags & CLIContext::AssumeRepoReadable)) {
    return false;
  }
  return path_in_repo(path);
}
}

req::ptr<File>
CLIWrapper::open(const OptString& filename, const OptString& mode, int options,
                 const req::ptr<StreamContext>& context) {
  mode_t md = static_cast<mode_t>(-1);
  const char* mstr = mode.data();
  int fl = 0;
  switch (mode[0]) {
   case 'x':
     md = 0666;
     fl = O_CREAT|O_EXCL;
     fl |= (mode.find('+') == -1) ? O_WRONLY : O_RDWR;
     mstr = (mode.find('+') == -1) ? "xw" : "xw+";
     break;
   case 'c':
     md = 0666;
     fl = O_CREAT;
     fl |= (mode.find('+') == -1) ? O_WRONLY : O_RDWR;
     mstr = (mode.find('+') == -1) ? "w" : "w+";
     break;
   case 'r':
     fl = (mode.find('+') == -1) ? O_RDONLY : O_RDWR;
     break;
   case 'w':
     md = 0666;
     fl = O_CREAT | O_TRUNC;
     fl |= (mode.find('+') == -1) ? O_WRONLY : O_RDWR;
     break;
   case 'a':
     md = 0666;
     fl = O_CREAT | O_APPEND;
     fl |= (mode.find('+') == -1) ? O_WRONLY : O_RDWR;
     break;
   default:
    raise_warning("Invalid mode string");
    return nullptr;
  }
  if (fl == O_RDONLY && use_local_fs(filename)) {
    return FileStreamWrapper().open(filename, mode, options, context);
  }
  auto fd = cli_openfd_unsafe(
    filename,
    fl,
    md,
    options & File::USE_INCLUDE_PATH,
    /* quiet */ false);
  if (fd == -1) {
    return nullptr;
  }
  return req::make<CLIClientGuardedFile>(fd, mstr);
}

req::ptr<Directory> CLIWrapper::opendir(const OptString& path) {
  if (use_local_fs(path)) {
    return FileStreamWrapper().opendir(path);
  }

  auto tpath = File::TranslatePath(path);
  bool res;
  std::string error;

  FTRACE(3, "CLIWrapper({})::opendir({}) calling remote...\n",
         m_cli_fd, tpath.data());

  cli_write(m_cli_fd, "opendir", tpath.data());
  cli_read(m_cli_fd, res, error);

  FTRACE(3, "{} = CLIWrapper({})::opendir(...) [err = {}]\n",
         res, m_cli_fd, error);

  if (!res) {
    raise_warning("%s", error.c_str());
    return nullptr;
  }

  return req::make<PlainDirectory>(cli_read_fd(m_cli_fd));
}

int CLIWrapper::lstat(const OptString& path, struct stat* buf) {
  if (use_local_fs(path)) {
    return FileStreamWrapper().lstat(path, buf);
  }

  FTRACE(3, "CLIWrapper({})::lstat({}) calling remote...\n",
         m_cli_fd, path.data());
  cli_write(m_cli_fd, "lstat", File::TranslatePath(path).data());
  int res;
  cli_read(m_cli_fd, res, *buf);
  return res;
}

int CLIWrapper::stat(const OptString& path, struct stat* buf) {
  if (use_local_fs(path)) {
    return FileStreamWrapper().stat(path, buf);
  }

  FTRACE(3, "CLIWrapper({})::stat({}) calling remote...\n",
         m_cli_fd, path.data());
  cli_write(m_cli_fd, "stat", File::TranslatePath(path).data());
  int res;
  cli_read(m_cli_fd, res, *buf);
  return res;
}

OptString CLIWrapper::realpath(const OptString& path) {
  if (path_in_repo(path)) {
    auto const flags = tl_context->getShared()->flags;
    if (flags & CLIContext::AssumeRepoRealpath) {
      auto con = FileUtil::canonicalize(path);
      if (con.size() > 1 && con[con.size() - 1] == '/') {
        con.shrink(con.size() - 1);
        assertx(con[con.size()] != '/');
      }
      return con;
    } else if (flags & CLIContext::AssumeRepoReadable) {
      char resolved_path[PATH_MAX];
      if (!::realpath(path.data(), resolved_path)) return null_string;
      return OptString(resolved_path, CopyString);
    }
  }

  bool status;
  std::string ret;
  cli_write(m_cli_fd, "realpath", File::TranslatePath(path).data());
  cli_read(m_cli_fd, status, ret);
  if (!status) return null_string;
  return OptString(ret.c_str(), CopyString);
}

template<class T>
typename std::enable_if<
  !std::is_same<OptString,typename std::remove_cv<T>::type>::value,
  T
>::type cli_wire_type(T&& t) { return t; }

template<class T>
typename std::enable_if<
  std::is_same<OptString,typename std::remove_cv<T>::type>::value,
  std::string
>::type cli_wire_type(T&& t) {
  return File::TranslatePath(t).data();
}

template<class... Args>
int cli_send_wire(int fd, const char* name, Args... args) {
  FTRACE(3, "CLIWrapper({})::{}(...) calling remote...\n", fd, name);
  cli_write(fd, name, cli_wire_type(std::forward<Args>(args))...);
  int res;
  cli_read(fd, res);
  return res;
}

int CLIWrapper::access(const OptString& path, int mode) {
  return cli_send_wire(m_cli_fd, "access", path, mode);
}
int CLIWrapper::unlink(const OptString& path) {
  auto ret = cli_send_wire(m_cli_fd, "unlink", path);
  if (ret != 0) {
    cli_read(m_cli_fd, errno);
    raise_warning(
      "%s(%s): %s",
      __FUNCTION__,
      path.c_str(),
      folly::errnoStr(errno).c_str()
    );
  }
  return ret;
}
int CLIWrapper::rename(const OptString& oldname, const OptString& newname) {
  return cli_send_wire(m_cli_fd, "rename", oldname, newname);
}
int CLIWrapper::mkdir(const OptString& path, int mode, int options) {
  return cli_send_wire(m_cli_fd, "mkdir", path, mode, options);
}
int CLIWrapper::rmdir(const OptString& path, int options) {
  return cli_send_wire(m_cli_fd, "rmdir", path, options);
}
bool CLIWrapper::touch(const OptString& path, int64_t mtime, int64_t atime) {
  return cli_send_wire(m_cli_fd, "touch", path, mtime, atime) != -1;
}
bool CLIWrapper::chmod(const OptString& path, int64_t mode) {
  return cli_send_wire(m_cli_fd, "chmod", path, mode) != -1;
}
bool CLIWrapper::chown(const OptString& path, int64_t uid) {
  return cli_send_wire(m_cli_fd, "chown", path, uid, (int64_t)-1) != -1;
}
bool CLIWrapper::chown(const OptString& path, const OptString& user) {
  uid_t id;
  try {
    UserInfo info(user.data());
    id = info.pw->pw_uid;
  } catch (const Exception& ) {
    return false;
  }
  return cli_send_wire(m_cli_fd, "chown", path, id, (int64_t)-1) != -1;
}
bool CLIWrapper::chgrp(const OptString& path, int64_t gid) {
  return cli_send_wire(m_cli_fd, "chown", path, (int64_t)-1, gid) != -1;
}
bool CLIWrapper::chgrp(const OptString& path, const OptString& group) {
  gid_t id;
  try {
    GroupInfo info(group.data());
    id = info.gr->gr_gid;
  } catch (const Exception& ) {
    return false;
  }
  return cli_send_wire(m_cli_fd, "chown", path, (int64_t)-1, id) != -1;
}

Optional<std::string> CLIWrapper::getxattr(const char* path,
                                           const char* xattr) {
  bool status;
  std::string ret;
  cli_write(m_cli_fd, "getxattr", path, xattr);
  cli_read(m_cli_fd, status, ret);
  if (!status) return std::nullopt;
  return ret;
}

////////////////////////////////////////////////////////////////////////////////

std::mutex s_xboxLock;
std::atomic<size_t> s_numXboxThreads{0};
std::condition_variable s_xboxThreadsCv;

void enqueueXboxJob() {
  std::unique_lock l{s_xboxLock};
  s_numXboxThreads++;
}

void finishXboxJob() {
  std::unique_lock l{s_xboxLock};
  if (--s_numXboxThreads == 0) s_xboxThreadsCv.notify_one();
}

struct CLIXboxWorker
  : JobQueueWorker<int,void*,true,false,JobQueueDropVMStack>
{
  void doJob(int fd) override;
  void abortJob(int fd) override {
    finishXboxJob();
    Logger::Warning("CLI xbox-request (%i) dropped because of timeout.", fd);
    close(fd);
  }
};

JobQueueDispatcher<CLIXboxWorker>* s_xbox_dispatcher = nullptr;

int mkdir_recursive(const char* path, int mode) {
  auto path_len = strlen(path);
  if (path_len > PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }

  // Check first if the whole path exists
  if (access(path, F_OK) >= 0) {
    errno = EEXIST;
    return -1;
  }

  char dir[PATH_MAX+1];
  char *p;
  memcpy(dir, path, path_len + 1); // copy null terminator

  for (p = dir + 1; *p; p++) {
    if (FileUtil::isDirSeparator(*p)) {
      *p = '\0';
      if (access(dir, F_OK) < 0 && mkdir(dir, mode) < 0) {
        return -1;
      }
      *p = FileUtil::getDirSeparator();
    }
  }

  if (access(dir, F_OK) < 0) {
    if (mkdir(dir, mode) < 0) {
      return -1;
    }
  }

  return 0;
}

Optional<int> cli_process_command_loop(int fd, bool ignore_bg, bool isclone) {
  FTRACE(1, "{}({}): starting...\n", __func__, fd);
  std::string cmd;
  cli_read(fd, cmd);

  FTRACE(2, "{}({}): initial command: {}\n", __func__, fd, cmd);

  if (!isclone) {
    if (cmd == "version_bad") {
      // Returning will cause us to re-run the script locally when not in force
      // server mode.
      return std::nullopt;
    }

    if (cmd != "version_ok") {
      // Server is too old / didn't send a version. Only version 0 is compatible
      // with an unversioned server.
      return std::nullopt;
    } else {
      cli_read(fd, cmd);
    }

    cli_client_init();
  } else {
    cli_client_thread_init();
  }

  SCOPE_EXIT {
    if (!isclone) {
      if (s_xbox_dispatcher) {
        std::unique_lock l{s_xboxLock};
        s_xboxThreadsCv.wait(l, [] { return s_numXboxThreads == 0; });

        s_xbox_dispatcher->stop();
        delete s_xbox_dispatcher;
        s_xbox_dispatcher = nullptr;
      }
    } else {
      assertx(s_xbox_dispatcher);
      cli_client_thread_exit();
    }
  };

  for (;; cli_read(fd, cmd)) {
    FTRACE(2, "{}({}): got command: {}\n", __func__, fd, cmd);

    if (cmd == "exit") {
      int ret;
      cli_read(fd, ret);
      FTRACE(1, "{}({}): exiting with code {}\n", __func__, fd, ret);
      return ret;
    }

    if (cmd == "open") {
      std::string name;
      int flags;
      mode_t mode;
      cli_read(fd, name, flags, mode);

      int new_fd = mode != static_cast<unsigned int>(-1)
        ? open(name.c_str(), flags, mode)
        : open(name.c_str(), flags);

      FTRACE(2, "{}({}): {} = open({}, {})\n", __func__, fd, new_fd, name,mode);

      if (new_fd < 0) {
        cli_write(fd, false, folly::errnoStr(errno).c_str());
        continue;
      }
      cli_write(fd, true, "OK");
      cli_write_fd(fd, new_fd);
      close(new_fd);
      continue;
    }

    if (cmd == "opendir") {
      std::string name;
      cli_read(fd, name);
      int dir_fd = open(name.c_str(), O_RDONLY|O_DIRECTORY);

      FTRACE(2, "{}({}): {} = diropen({})\n", __func__, fd, dir_fd, name);

      if (dir_fd < 0) {
        cli_write(fd, false, folly::errnoStr(errno).c_str());
        continue;
      }
      cli_write(fd, true, "OK");
      cli_write_fd(fd, dir_fd);
      close(dir_fd);
      continue;
    }

    if (cmd == "lstat") {
      std::string path;
      struct stat st;
      cli_read(fd, path);
      FTRACE(2, "{}({}): lstat({})\n", __func__, fd, path);
      int res = lstat(path.c_str(), &st);
      cli_write(fd, res, st);
      continue;
    }

    if (cmd == "stat") {
      std::string path;
      struct stat st;
      cli_read(fd, path);
      FTRACE(2, "{}({}): stat({})\n", __func__, fd, path);
      int res = stat(path.c_str(), &st);
      cli_write(fd, res, st);
      continue;
    }

    if (cmd == "access") {
      std::string path;
      int mode;
      cli_read(fd, path, mode);
      FTRACE(2, "{}({}): access({}, {})\n", __func__, fd, path, mode);
      cli_write(fd, access(path.c_str(), mode));
      continue;
    }

    if (cmd == "unlink") {
      std::string path;
      cli_read(fd, path);
      FTRACE(2, "{}({}): unlink({})\n", __func__, fd, path);
      auto ret = unlink(path.c_str());
      cli_write(fd, ret);
      if (ret != 0) cli_write(fd, errno);
      continue;
    }

    if (cmd == "rename") {
      std::string oldp;
      std::string newp;
      cli_read(fd, oldp, newp);

      FTRACE(2, "{}({}): rename({}, {})\n", __func__, fd, oldp, newp);

      int ret = Cfg::Server::UseDirectCopy
        ? FileUtil::directRename(oldp.c_str(), newp.c_str())
        : FileUtil::rename(oldp.c_str(), newp.c_str());

      cli_write(fd, ret);
      continue;
    }

    if (cmd == "mkdir") {
      int options;
      int mode;
      std::string path;
      cli_read(fd, path, mode, options);

      FTRACE(2, "{}({}): mkdir({}, {}, {})\n", __func__,fd,path, mode, options);

      if (options & k_STREAM_MKDIR_RECURSIVE) {
        cli_write(fd, mkdir_recursive(path.c_str(), mode));
        continue;
      }

      cli_write(fd, mkdir(path.c_str(), mode));
      continue;
    }

    if (cmd == "rmdir") {
      std::string path;
      int options;
      cli_read(fd, path, options);

      FTRACE(2, "{}({}): rmdir({})\n", __func__, fd, path);

      cli_write(fd, rmdir(path.c_str()));
      continue;
    }

    if (cmd == "touch") {
      std::string path;
      int64_t mtime;
      int64_t atime;
      cli_read(fd, path, mtime, atime);

      if (access(path.c_str(), F_OK)) {
        FILE *f = fopen(path.c_str(), "w");
        if (!f) {
          cli_write(fd, -1);
          continue;
        }
        fclose(f);
      }

      if (mtime == 0 && atime == 0) {
        // It is important to pass nullptr so that the OS sets mtime and atime
        // to the current time with maximum precision (more precise then
        // seconds)
        cli_write(fd, utime(path.c_str(), nullptr));
      } else {
        struct utimbuf newtime;
        newtime.actime = atime ? atime : mtime;
        newtime.modtime = mtime;
        cli_write(fd, utime(path.c_str(), &newtime));
      }
      FTRACE(2, "{}({}): touch({}, {}, {})\n", __func__, fd, path, mtime,atime);
      continue;
    }

    if (cmd == "chmod") {
      std::string path;
      int64_t mode;
      cli_read(fd, path, mode);

      FTRACE(2, "{}({}): chmod({}, {})\n", __func__, fd, path, mode);

      cli_write(fd, chmod(path.c_str(), mode));
      continue;
    }

    if (cmd == "chown") {
      std::string path;
      int64_t uid;
      int64_t gid;
      cli_read(fd, path, uid, gid);

      FTRACE(2, "{}({}): chown({}, {}, {})\n", __func__, fd, path, uid, gid);

      cli_write(fd, chown(path.c_str(), (uid_t)uid, (gid_t)gid));
      continue;
    }

    if (cmd == "mkstemp") {
      std::string path;
      cli_read(fd, path);
      FTRACE(2, "{}({}): mkstemp({})\n", __func__, fd, path);
      char* str = strdup(path.data());
      SCOPE_EXIT { free(str); };
      auto tempfd = mkstemp(str);
      if (tempfd < 0) {
        cli_write(fd, false, "ERROR");
        continue;
      }
      close(tempfd);
      std::string out = str;
      cli_write(fd, true, out);
      continue;
    }

    if (cmd == "realpath") {
      std::string path;
      cli_read(fd, path);
      FTRACE(2, "{}({}): realpath({})\n", __func__, fd, path);
      char resolved_path[PATH_MAX];
      if (!realpath(path.c_str(), resolved_path)) {
        cli_write(fd, false, "ERROR");
        continue;
      }
      std::string out(resolved_path);
      cli_write(fd, true, out);
      continue;
    }

    if (cmd == "ext") {
      std::string name;
      cli_read(fd, name);
      FTRACE(2, "{}({}): {}\n", __func__, fd, name);
      auto handler = s_extensionHandlers.find(name);
      if (handler == s_extensionHandlers.end()) {
        cli_write(fd, /* handler_recognized = */ false);
        continue;
      }
      cli_write(fd, /* handler recognized = */ true);
      detail::CLIServerInterface server(fd);
      handler->second(server);
      continue;
    }

    if (cmd == "getxattr") {
      std::string path;
      std::string xattr;
      cli_read(fd, path, xattr);

      std::string buf;
      buf.resize(64);

      [&] {
        while (true) {
          auto const ret =
            ::getxattr(path.c_str(), xattr.c_str(), buf.data(), buf.size());
          if (ret >= 0) {
            assertx(ret <= buf.size());
            buf.resize(ret);
            cli_write(fd, true, buf);
            return;
          }
          if (errno != ERANGE) break;
          auto const actualSize =
            ::getxattr(path.c_str(), xattr.c_str(), nullptr, 0);
          if (actualSize < 0) break;
          buf.resize(std::max<size_t>(actualSize, buf.size()));
        }
        cli_write(fd, false, std::string{});
      }();
      continue;
    }

    if (cmd == "background") {
      int ret;
      cli_read(fd, ret);

      if (ignore_bg) continue;
      if (setsid() == -1) {
        Logger::FError("Failed to set session ID on background process: {}",
                       folly::errnoStr(errno));
        continue;
      }

      if (folly::writeFull(s_foreground_pipe, &ret, sizeof(ret)) == -1) {
        Logger::FError("Failed to communicate with foreground process: {}",
                       folly::errnoStr(errno));
      }
      close(s_foreground_pipe);
      s_foreground_pipe = -1;

      auto fd = open("/dev/null", O_RDWR);
      if (fd != -1) {
        auto const logerr = [&] (const char* name) {
          Logger::FError("Failed close {}: {}", name, folly::errnoStr(errno));
        };
        if (dup2(fd, STDIN_FILENO)  == -1) logerr("stdin");
        if (dup2(fd, STDOUT_FILENO) == -1) logerr("stdout");
        if (dup2(fd, STDERR_FILENO) == -1) logerr("stderr");
        close(fd);
      } else {
        Logger::FError("Failed to open /dev/null: {}", folly::errnoStr(errno));
      }
      continue;
    }

    if (cmd == "xbox-init") {
      int XboxServerThreadCount;
      int ServerThreadDropCacheTimeoutSeconds;
      bool ServerThreadDropStack;

      cli_read(
        fd,
        XboxServerThreadCount,
        ServerThreadDropCacheTimeoutSeconds,
        ServerThreadDropStack
      );

      FTRACE(2, "{}({}): xbox-init: ThreadCount = {}\n",
             __func__, fd, XboxServerThreadCount);

      assertx(!s_xbox_dispatcher);
      s_xbox_dispatcher = new JobQueueDispatcher<CLIXboxWorker>(
        XboxServerThreadCount,
        XboxServerThreadCount,
        ServerThreadDropCacheTimeoutSeconds,
        ServerThreadDropStack,
        nullptr
      );
      s_xbox_dispatcher->start();
      continue;
    }

    if (cmd == "clone") {
      assertx(s_xbox_dispatcher);

      int socks[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) == -1) {
        Logger::FError("socketpair() failed: {}", folly::errnoStr(errno));
        exit(-1);
      }

      FTRACE(2, "{}({}): clone(): {} (local) <-> {} (remote)\n", __func__,
             fd, socks[0], socks[1]);

      cli_write_fd(fd, socks[1]);
      enqueueXboxJob();

      close(socks[1]);
      s_xbox_dispatcher->enqueue(socks[0]);
      continue;
    }

    // - if the unrecognized command takes no arguments, everything's fine
    // - if the unrecognized command takes a string first arg, we'll treat that
    //   string argument as a command on the next loop; recurse. Then we get
    //   unpredicatble behavior depending on the value of the arg
    // - once we hit a non-string argument, we'll get an error from cli_read(cmd)
    FTRACE(2, "{}({}): bad command: {}\n", __func__, fd, cmd);
    if (Cfg::Eval::CheckCLIClientCommands == 1) {
      Logger::Warning("Unrecognized CLI client command: %s\n", cmd.c_str());
    } else if (Cfg::Eval::CheckCLIClientCommands == 2) {
      Logger::Error("Unrecognized CLI client command: %s\n", cmd.c_str());
      return 1;
    }
  }
}

void CLIXboxWorker::doJob(int fd) {
  SCOPE_EXIT { finishXboxJob(); };
  try {
    cli_process_command_loop(fd, true, true);
  } catch (const Exception& ex) {
    Logger::Warning("CLI XBox Job failed: %s", ex.what());
  } catch (const std::exception& ex) {
    Logger::FError("CLI XBox Job failed with C++ exception: {}", ex.what());
  } catch (...) {
    Logger::Error("CLI XBox Job failed with unknown exception");
  }
}

Optional<int> run_client(const char* sock_path,
                         const std::vector<std::string>& args,
                         bool ignore_bg) {
  if (Cfg::Repo::Authoritative) {
    Logger::Warning("Unable to use CLI server to run script in "
                    "repo-auth mode.");
    return std::nullopt;
  }
  FTRACE(1, "run_command_on_cli_server({}, ...): sending command...\n",
         sock_path);

  std::vector<std::string> env_vec;

  for (char** env = environ; env && *env; env++) {
    env_vec.emplace_back(*env);
  }

  int delegate = LightProcess::createDelegate();
  if (delegate < 0) {
    Logger::Warning("Could not create delegate for CLI server: %s",
                    folly::errnoStr(errno).c_str());
    return std::nullopt;
  }
  FTRACE(2, "run_command_on_cli_server(): delegate = {}\n", delegate);

  afdt_error_t err = AFDT_ERROR_T_INIT;
  int fd = afdt_connect(sock_path, &err);
  if (fd < 0) {
    Logger::Info("Could not attach to CLI server: %s",
                 folly::errnoStr(errno).c_str());
    return std::nullopt;
  }

  FTRACE(2, "run_command_on_cli_server(): fd = {}\n", fd);

  try {
    cli_write(fd, kMagic);
    cli_write_ucred(fd);

    char cwd[PATH_MAX];
    getcwd(cwd, PATH_MAX);
    cli_write(fd, cwd);

    zend_get_bigint_data();
    tl_heap.getCheck()->init();

    // We need to initialize the CLI server extension handlers before we
    // serialize the runtime-options so that they are included in the API
    // version we send to the server.
    ExtensionRegistry::cliClientInit();

    auto settings = IniSetting::GetAllAsJSON();
    cli_write(fd, settings);

    FTRACE(2, "run_command_on_cli_server(): sending fds...\n", fd);

    cli_write_fd(fd, fileno(stdin));
    cli_write_fd(fd, fileno(stdout));
    cli_write_fd(fd, fileno(stderr));
    cli_write_fd(fd, delegate);

    FTRACE(2, "run_command_on_cli_server(): file/args...\n", fd);
    cli_write(fd, 0 /* unused */, args, env_vec);
    return cli_process_command_loop(fd, ignore_bg, false);
  } catch (const Exception& ex) {
    Logger::Error(
      "Problem communicating with CLI server: %s\n"
      "It likely crashed. Check the HHVM error log and look for coredumps",
      ex.what()
    );
    exit(HPHP_EXIT_FAILURE);
  }
}

[[noreturn]] void waitAndExit(int pid, int options, int err) {
  int status = 0;
  if (waitpid(pid, &status, options) != pid) {
    Logger::FError("Lost communication with child: {}",
                   folly::errnoStr(err ? err : errno));
    _Exit(EXIT_FAILURE);
  }
  if (WIFEXITED(status))   _Exit(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) kill(getpid(), WTERMSIG(status));
  _Exit(EXIT_FAILURE);
}

void moveToBackground(int count) {
  int fg_pipe[2];
  if (pipe(fg_pipe) == -1) {
    throw Exception("Could not create foreground pipe: %s",
                    folly::errnoStr(errno).c_str());
  }

  int background_pipe = fg_pipe[0];
  int foreground_pipe = fg_pipe[1];

  auto const pid = fork();
  if (pid == -1) {
    throw Exception("Could not create background process: %s",
                    folly::errnoStr(errno).c_str());
  }

  if (pid != 0) {
    // Ensure that the foreground thread is shutdown in the event that the
    // background thread terminates without sending an exit message.
    std::thread([pid] { waitAndExit(pid, 0, 0); }).detach();

    int ret = -1;
    close(foreground_pipe);
    while (count--) {
      switch (folly::readFull(background_pipe, &ret, sizeof(ret))) {
      case -1: waitAndExit(pid, WNOHANG, errno);
      case 0:  waitAndExit(pid, 0, 0);
      default: break;
      }
    }
    _Exit(ret);
  }

  close(background_pipe);
  s_foreground_pipe = foreground_pipe;
}

////////////////////////////////////////////////////////////////////////////////
}

CLIContext::CLIContext(CLIContext&& other)
  : m_data(other.m_data)
  , m_shared(std::move(other.m_shared))
{
  other.m_data.client = -1;
}

CLIContext& CLIContext::operator=(CLIContext&& other) {
  std::swap(m_data, other.m_data);
  std::swap(m_shared, other.m_shared);
  return *this;
}

CLIContext::~CLIContext() {
  if (m_data.client == -1) return;

  assertx(m_data.client != -1 && m_data.lwp_afdt != -1);
  assertx(m_data.in && m_data.out && m_data.err);

  close(m_data.client);
  close(m_data.lwp_afdt);

  auto const sync_and_close = [] (FILE* f) {
    fflush(f);
    fsync(fileno(f));
    fclose(f);
  };
  sync_and_close(m_data.in);
  sync_and_close(m_data.out);
  sync_and_close(m_data.err);
}

CLIContext CLIContext::initFromParent(const CLIContext& other) {
  CLIContext::Data data;
  auto const old_client = other.m_data.client;

  auto const fail = [&] {
    if (data.client != -1) close(data.client);
    if (data.lwp_afdt != -1) close(data.lwp_afdt);
    if (data.in) fclose(data.in);
    if (data.out) fclose(data.out);
    if (data.err) fclose(data.err);
  };
  auto guard = folly::makeGuard(fail);

  cli_write(old_client, "clone");
  data.client = cli_read_fd(old_client);
  data.lwp_afdt = LightProcess::cloneDelegate();

  if (data.lwp_afdt < 0) throw Exception("Error creating delegate");

  data.in = fdopen(dup(fileno(other.m_data.in)), "r");
  data.out = fdopen(dup(fileno(other.m_data.out)), "w");
  data.err = fdopen(dup(fileno(other.m_data.err)), "w");

  FTRACE(2, "{}({}): clone() = {} (afdt = {})\n", __func__,
         old_client, data.client, data.lwp_afdt);

  guard.dismiss();
  return CLIContext{std::move(data), other.m_shared};
}


CLIContext CLIContext::initFromClient(int client) {
  CLIContext::Data data;
  CLIContext::SharedData shared;

  auto const fail = [&] {
    close(client);
    if (data.lwp_afdt != -1) close(data.lwp_afdt);
    if (data.in) fclose(data.in);
    if (data.out) fclose(data.out);
    if (data.err) fclose(data.err);
  };
  auto guard = folly::makeGuard(fail);

  data.client = client;

  uint64_t magic;
  cli_read(client, magic);
  FTRACE(2, "{}({}): magic = {}\n", __func__, client, magic);
  if (magic != kMagic) {
    throw Exception("Got bad magic from client: %lu", magic);
  }

  shared.user = cli_read_ucred(client);
  shared.uuid = boost::uuids::to_string(boost::uuids::random_generator()());

  // Throw if the client is not authorized to access the CLI server
  check_cli_server_access(shared.user);

  std::string iniSettings;
  cli_read(client, shared.cwd, iniSettings);
  shared.ini = folly::parseJson(iniSettings);

  FTRACE(1, "{}({}): cwd = {}\n", __func__, client, shared.cwd);
  FTRACE(4, "{}({}): iniSettings = {}\n", __func__, client,
         folly::toPrettyJson(shared.ini));

  data.in = read_file_helper(client, "stdin", "r");
  data.out = read_file_helper(client, "stdout", "w");
  data.err = read_file_helper(client, "stderr", "w");
  data.lwp_afdt = read_file_helper(client, "afdt");

  int unused_flag;
  cli_read(client, unused_flag, shared.argv, shared.envp);

  FTRACE(2, "{}({}): args = \n\t{}\n", __func__, client,
         folly::join("\n\t", shared.argv));
  FTRACE(3, "{}({}): env = \n\t{}\n", __func__, client,
         folly::join("\n\t", shared.envp));

  auto const api_version = get_setting_int(shared.ini,
                                           "hphp.cli_server_api_version");

  if (api_version != cli_server_api_version()) {
    FTRACE(2, "{}({}): detected bad version got: {}, wanted: {}\n",
           __func__, client, api_version, cli_server_api_version());
    cli_write(client, "version_bad");
    throw Exception(
      "cli_server_api_version() (%" PRIu64") "
      "does not match client (%" PRIu64 ")",
      cli_server_api_version(), api_version
    );
  } else {
    // Even if the client is too old to understand this command, unknown
    // commands are handled silently.
    cli_write(client, "version_ok");
  }

  // UnixServerProxyXbox is used to communicate between the CLI client and
  // server and should not be deleted.
  static_assert(std::is_same_v<decltype(Cfg::Eval::UnixServerProxyXbox), bool>);
  auto const proxy_xbox = get_setting_bool(shared.ini,
                                           "hhvm.unix_server_proxy_xbox");

  if (proxy_xbox) {
    FTRACE(2, "{}({}): Sending xbox-init: ThreadCount = {}\n",
           __func__, client, Cfg::Xbox::ServerInfoThreadCount);
    cli_write(
      client,
      "xbox-init",
      Cfg::Xbox::ServerInfoThreadCount,
      Cfg::Server::ThreadDropCacheTimeoutSeconds,
      Cfg::Server::ThreadDropStack
    );
    shared.flags = static_cast<Flags>(shared.flags | Flags::ProxyXbox);
  }

  auto const assume_readable = get_setting_bool(
    shared.ini,
    "hhvm.unix_server_assume_repo_readable"
  );

  if (assume_readable && Cfg::Eval::UnixServerAssumeRepoReadable) {
    shared.flags = static_cast<Flags>(
      shared.flags | Flags::AssumeRepoReadable
    );
  }

  auto const assume_realpath = get_setting_bool(
    shared.ini,
    "hhvm.unix_server_assume_repo_realpath"
  );

  if (assume_realpath && Cfg::Eval::UnixServerAssumeRepoRealpath) {
    shared.flags = static_cast<Flags>(
      shared.flags | Flags::AssumeRepoRealpath
    );
  }

  auto const active_deployment = get_setting_string(
    shared.ini,
    "hhvm.active_deployment",
    Cfg::Eval::ActiveDeployment
  );
  shared.activeDeployment = active_deployment;

  guard.dismiss();
  return CLIContext{std::move(data), std::move(shared)};
}

////////////////////////////////////////////////////////////////////////////////

void init_cli_server(const char* socket_path) {
  if (Cfg::Repo::Authoritative) return;

  for (auto user : Cfg::Eval::UnixServerAllowedUsers) {
    try {
      UserInfo info(user.c_str());
      s_allowedUsers.emplace(info.pw->pw_uid);
    } catch (const Exception& ex) {
      Logger::Warning(
        "Could not get uid for user %s in Eval.UnixServerAllowedUsers: %s",
        user.c_str(), ex.what()
      );
    }
  }

  for (auto group : Cfg::Eval::UnixServerAllowedGroups) {
    try {
      GroupInfo info(group.c_str());
      s_allowedGroups.emplace(info.gr->gr_gid);
    } catch (const Exception& ex) {
      Logger::Warning(
        "Could not get gid for group %s in Eval.UnixServerAllowedGroups: %s",
        group.c_str(), ex.what()
      );
    }
  }

  assertx(!s_cliServer);
  FTRACE(1, "init_cli_server({}): init...\n", socket_path);
  s_cliServer = new CLIServer(socket_path);
}

void start_cli_server() {
  if (!s_cliServer) return;

  FTRACE(1, "start_cli_server(): starting...\n");
  s_cliServer->start();
}

void teardown_cli_server() {
  if (!s_cliServer) return;

  FTRACE(1, "teardown_cli_server(): stopping...\n");

  s_cliServer->stop();
  delete s_cliServer;
  s_cliServer = nullptr;
}

ucred* get_cli_ucred() {
  return tl_context ? tl_context->user() : nullptr;
}

bool cli_mkstemp(char* buf) {
  FTRACE(2, "cli_mkstemp({}): fd = {}\n", buf, cli_sock());
  std::string out = buf;
  cli_write(cli_sock(), "mkstemp", out);
  bool status;
  std::string path;
  cli_read(cli_sock(), status, path);
  if (!status) return false;
  memcpy(buf, path.c_str(), std::min(strlen(buf), path.size()));
  return true;
}

int cli_openfd_unsafe(const OptString& filename, int flags, mode_t mode,
                      bool use_include_path, std::string& error) {
  OptString fname;
  if (StringUtil::IsFileUrl(filename)) {
    fname = StringUtil::DecodeFileUrl(filename);
    if (fname.empty()) {
      error = "invalid file:// URL";
      return -1;
    }
  } else {
    fname = filename;
  }

  if (use_include_path) {
    struct stat s;
    OptString resolved_fname = resolveVmInclude(fname.get(), "", &s);
    if (!resolved_fname.isNull()) {
      fname = resolved_fname;
    }
  }

  bool res;
  FTRACE(3, "cli_openfd[{}]({}, {}, {}): calling remote...\n",
         cli_sock(), fname.data(), flags, mode);
  cli_write(cli_sock(), "open", fname.data(), flags, mode);
  cli_read(cli_sock(), res, error);
  FTRACE(3, "{} = cli_openfd[{}](...) [err = {}]\n",
         res, cli_sock(), error);

  if (!res) {
    return -1;
  }

  return cli_read_fd(cli_sock());
}

int cli_openfd_unsafe(const OptString& filename, int flags, mode_t mode,
                      bool use_include_path, bool quiet) {
  std::string err;
  auto const ret = cli_openfd_unsafe(filename, flags, mode, use_include_path,
                                     err);
  if (ret == -1 && !quiet) {
    raise_warning("%s", err.c_str());
  }

  return ret;
}

Array cli_env() {
  return tl_env ? *tl_env : empty_dict_array();
}

bool is_cli_server_mode() {
  return tl_context != nullptr;
}

bool is_any_cli_mode() {
  return is_cli_server_mode() || !Cfg::Server::Mode;
}

int cli_server_active_workers() {
  return s_cliServer ? s_cliServer->getActiveWorkers() : 0;
}

uint64_t cli_server_api_version() {
  if (s_cliServerComputedVersion != 0) {
    return s_cliServerComputedVersion;
  }
  std::string key;
  for (const auto& it : s_extensionHandlers) {
    key += it.first.c_str();
  }
  s_cliServerComputedVersion = murmur_hash_64A(
    key.c_str(),
    key.length(),
    CLI_SERVER_API_BASE_VERSION
  );
  return s_cliServerComputedVersion;
}

void cli_invoke(
  CLIContext&& ctx,
  std::function<void(const std::string&)>&& invoke
) {
  Logger::Verbose("Starting CLI Proxied XBox request...");
  UNUSED auto const client = ctx.client();
  FTRACE(2, "{}({}): starting...\n", __func__, client);
  try {
    runInContext(
      std::move(ctx),
      true, // xbox
      true, // enableBackgroundPSP
      [&] (const std::string& cwd, std::vector<char*>&) {
        g_context->setCwd(cwd);
      },
      [&] (const char*, const std::string& prelude) {
        invoke(prelude);
        return 0;
      },
      [&] (const char*) {},
      [&] {}
    );
  } catch (const Exception& ex) {
    Logger::Warning("CLI Xbox Job failed: %s", ex.what());
    throw;
  } catch (const std::exception& ex) {
    Logger::FError("CLI Xbox Job failed with C++ exception: {}", ex.what());
    throw;
  } catch (...) {
    Logger::Error("CLI Xbox Job failed with unknown exception");
    throw;
  }

  FTRACE(1, "{}({}): done.\n", __func__, client);
}

CLIContext cli_clone_context() {
  assertx(is_cli_server_mode());
  return CLIContext::initFromParent(*tl_context);
}

bool cli_supports_clone() {
  return tl_context &&
    (tl_context->getShared()->flags & CLIContext::ProxyXbox);
}


std::string cli_get_active_deployment() {
  assertx(is_cli_server_mode());
  return tl_context->getShared()->activeDeployment;
}
////////////////////////////////////////////////////////////////////////////////

void run_command_on_cli_server(const char* sock_path,
                               const std::vector<std::string>& args,
                               int& count) {
  if (Cfg::Eval::UnixServerRunPSPInBackground) moveToBackground(count);

  int ret = 0;
  auto const finish = [&] {
    if (s_foreground_pipe != -1) {
      folly::writeFull(s_foreground_pipe, &ret, sizeof(ret));
      close(s_foreground_pipe);
      s_foreground_pipe = -1;
    }
  };
  auto guard = folly::makeGuard(finish);

  while (count > 0) {
    auto r = run_client(sock_path, args, count > 1);
    if (!r) {
      guard.dismiss();
      return;
    }
    ret = *r;
    count--;
    if (count > 0) {
      // If we're running an unit test with multiple runs, provide
      // a separator between the runs.
      if (auto const sep = getenv("HHVM_MULTI_COUNT_SEP")) {
        fflush(stderr);
        printf("%s", sep);
        fflush(stdout);
      }
    }
    if (Cfg::Eval::FreeRandomUnits) {
      auto units = loadedUnitsNonRepoAuth();
      for (auto const unit : units) {
        if (unit->isSystemLib()) continue;
        invalidateUnit(const_cast<StringData*>(unit->origFilepath()));
      }
      low_malloc(sizeof(Func) + sizeof(jit::AtomicLowTCA) * (count + 1) );
    }
  }

  // Send the return value (calling exit() will not run the guard)
  guard.dismiss();
  finish();

  hphp_process_exit();
  exit(ret);
}

namespace detail {

void cli_register_handler(
  const std::string& id,
  void(*impl)(CLIServerInterface&)
) {
  s_extensionHandlers.emplace(id, impl);
}
// This is defined here so that cli_sock does not need to be exposed
CLIClientInterface::CLIClientInterface(const std::string& id)
: CLIServerExtensionInterface(cli_sock()), id(id) {
}

} // namespace HPHP::detail

} // namespace HPHP
