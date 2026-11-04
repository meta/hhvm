/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/ChildProcess.h"
#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <memory>
#include <system_error>
#include <thread>
#include "watchman/Logging.h"

namespace watchman {

ChildProcess::Environment::Environment() {
  // Construct the map from the current process environment
  uint32_t nenv, i;
  const char* eq;
  const char* ent;

  for (i = 0, nenv = 0; environ[i]; i++) {
    nenv++;
  }

  map_.reserve(nenv);

  for (i = 0; environ[i]; i++) {
    ent = environ[i];
    eq = strchr(ent, '=');
    if (!eq) {
      continue;
    }

    // slice name=value into a key and a value string
    auto key = w_string_piece(ent, eq - ent);
    auto val = w_string_piece(eq + 1);

    // Replace rather than set, just in case we somehow have duplicate
    // keys in our environment array.
    map_[key.asWString()] = val.asWString();
  }
}

ChildProcess::Environment::Environment(
    const std::unordered_map<w_string, w_string>& map)
    : map_(map) {}

/* Constructs an envp array from a hash table.
 * The returned array occupies a single contiguous block of memory
 * such that it can be released by a single call to free(3).
 * The last element of the returned array is set to NULL for compatibility
 * with posix_spawn() */
std::unique_ptr<char*, ChildProcess::Deleter>
ChildProcess::Environment::asEnviron(size_t* env_size) const {
  size_t len = (1 + map_.size()) * sizeof(char*);

  // Make a pass through to compute the required memory size
  for (const auto& it : map_) {
    const auto& key = it.first;
    const auto& val = it.second;

    // key=value\0
    len += key.size() + 1 + val.size() + 1;
  }

  auto envp = (char**)malloc(len);
  if (!envp) {
    throw std::bad_alloc();
  }
  auto result = std::unique_ptr<char*, Deleter>(envp, Deleter());

  // Now populate
  auto buf = (char*)(envp + map_.size() + 1);
  size_t i = 0;
  for (const auto& it : map_) {
    const auto& key = it.first;
    const auto& val = it.second;

    envp[i++] = buf;

    // key=value\0
    memcpy(buf, key.data(), key.size());
    buf += key.size();

    memcpy(buf, "=", 1);
    buf++;

    memcpy(buf, val.data(), val.size());
    buf += val.size();

    *buf = 0;
    buf++;
  }

  envp[map_.size()] = nullptr;

  if (env_size) {
    *env_size = len;
  }
  return result;
}

void ChildProcess::Environment::set(const w_string& key, const w_string& val) {
  map_[key] = val;
}

void ChildProcess::Environment::setBool(const w_string& key, bool bval) {
  if (bval) {
    map_[key] = "true";
  } else {
    map_.erase(key);
  }
}

void ChildProcess::Environment::set(
    std::initializer_list<std::pair<w_string_piece, w_string_piece>> pairs) {
  for (auto& pair : pairs) {
    set(pair.first.asWString(), pair.second.asWString());
  }
}

void ChildProcess::Environment::unset(const w_string& key) {
  map_.erase(key);
}

ChildProcess::Options::Options() : inner_(std::make_unique<Inner>()) {
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  setFlags(POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
}

ChildProcess::Options::Inner::Inner() {
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_init(&actions);
}

ChildProcess::Options::Inner::~Inner() {
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
}

void ChildProcess::Options::setFlags(short flags) {
  short currentFlags;
  auto err = posix_spawnattr_getflags(&inner_->attr, &currentFlags);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawnattr_getflags");
  }
  err = posix_spawnattr_setflags(&inner_->attr, currentFlags | flags);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawnattr_setflags");
  }
}

#ifdef POSIX_SPAWN_SETSIGMASK
void ChildProcess::Options::setSigMask(const sigset_t& mask) {
  posix_spawnattr_setsigmask(&inner_->attr, &mask);
  setFlags(POSIX_SPAWN_SETSIGMASK);
}
#endif

ChildProcess::Environment& ChildProcess::Options::environment() {
  return env_;
}

void ChildProcess::Options::dup2(int fd, int targetFd) {
  auto err = posix_spawn_file_actions_adddup2(&inner_->actions, fd, targetFd);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawn_file_actions_adddup2");
  }
}

void ChildProcess::Options::dup2(const FileDescriptor& fd, int targetFd) {
#ifdef _WIN32
  auto err = posix_spawn_file_actions_adddup2_handle_np(
      &inner_->actions, fd.handle(), targetFd);
  if (err) {
    throw std::system_error(
        err,
        std::generic_category(),
        "posix_spawn_file_actions_adddup2_handle_np");
  }
#else
  auto err =
      posix_spawn_file_actions_adddup2(&inner_->actions, fd.fd(), targetFd);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawn_file_actions_adddup2");
  }
#endif
}

void ChildProcess::Options::open(
    int targetFd,
    const char* path,
    int flags,
    int mode) {
  auto err = posix_spawn_file_actions_addopen(
      &inner_->actions, targetFd, path, flags, mode);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawn_file_actions_addopen");
  }
}

void ChildProcess::Options::pipe(int targetFd, bool childRead) {
  if (pipes_.find(targetFd) != pipes_.end()) {
    throw std::runtime_error("targetFd is already present in pipes map");
  }

  auto result =
      pipes_.emplace(std::make_pair(targetFd, std::make_unique<Pipe>()));
  auto pipe = result.first->second.get();

#ifndef _WIN32
  pipe->read.clearNonBlock();
  pipe->write.clearNonBlock();
#endif

  dup2(childRead ? pipe->read : pipe->write, targetFd);
}

void ChildProcess::Options::pipeStdin() {
  pipe(STDIN_FILENO, true);
}

void ChildProcess::Options::pipeStdout() {
  pipe(STDOUT_FILENO, false);
}

void ChildProcess::Options::pipeStderr() {
  pipe(STDERR_FILENO, false);
}

void ChildProcess::Options::nullFd(int fd, int flags) {
#ifdef _WIN32
  open(fd, "NUL", flags, 0);
#else
  open(fd, "/dev/null", flags, 0666);
#endif
}

void ChildProcess::Options::nullStdin() {
  nullFd(STDIN_FILENO, O_RDONLY);
}

void ChildProcess::Options::nullStdout() {
  nullFd(STDOUT_FILENO, O_WRONLY);
}

void ChildProcess::Options::nullStderr() {
  nullFd(STDERR_FILENO, O_WRONLY);
}

void ChildProcess::Options::chdir(w_string_piece path) {
  cwd_ = std::string(path.data(), path.size());
#ifdef _WIN32
  posix_spawnattr_setcwd_np(&inner_->attr, cwd_.c_str());
#endif
}

static std::vector<std::string_view> json_args_to_string_vec(
    const json_ref& args) {
  std::vector<std::string_view> vec;

  for (auto& arg : args.array()) {
    vec.emplace_back(json_to_w_string(arg).view());
  }

  return vec;
}

ChildProcess::ChildProcess(const json_ref& args, Options&& options)
    : ChildProcess(json_args_to_string_vec(args), std::move(options)) {}

ChildProcess::ChildProcess(
    std::vector<std::string_view> args,
    Options&& options)
    : pipes_(std::move(options.pipes_)) {
  std::vector<char*> argv;
  std::vector<std::string> argStrings;

  argStrings.reserve(args.size());
  argv.reserve(args.size() + 1);

  for (auto& str : args) {
    argStrings.emplace_back(str.data(), str.size());
    argv.emplace_back(&argStrings.back()[0]);
  }
  argv.emplace_back(nullptr);

#ifndef _WIN32
  auto lock = lockCwdMutex();
  char savedCwd[WATCHMAN_NAME_MAX];
  if (!getcwd(savedCwd, sizeof(savedCwd))) {
    throw std::system_error(errno, std::generic_category(), "failed to getcwd");
  }
  SCOPE_EXIT {
    if (!options.cwd_.empty()) {
      if (chdir(savedCwd) != 0) {
        // log(FATAL) rather than throw because SCOPE_EXIT is
        // a noexcept destructor and will call std::terminate
        // in this case anyway.
        log(FATAL, "failed to restore cwd of ", savedCwd);
      }
    }
  };

  if (!options.cwd_.empty()) {
    if (chdir(options.cwd_.c_str()) != 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          fmt::format("failed to chdir to {}", options.cwd_));
    }
  }
#endif

  auto envp = options.env_.asEnviron();
  auto ret = posix_spawnp(
      &pid_,
      argv[0],
      &options.inner_->actions,
      &options.inner_->attr,
      &argv[0],
      envp.get());

  if (ret) {
    // Failed, so the creator cannot call wait() on us.
    // mark us as already done.
    waited_ = true;
  }

  // Log some info
  auto level = ret == 0 ? watchman::DBG : watchman::ERR;
  watchman::log(level, "ChildProcess: pid=", pid_, "\n");
  for (size_t i = 0; i < args.size(); ++i) {
    watchman::log(level, "argv[", i, "] ", args[i], "\n");
  }
  for (size_t i = 0; envp.get()[i]; ++i) {
    watchman::log(level, "envp[", i, "] ", envp.get()[i], "\n");
  }

  // Close the other ends of the pipes
  for (auto& it : pipes_) {
    if (it.first == STDIN_FILENO) {
      it.second->read.close();
    } else {
      it.second->write.close();
    }
  }

  if (ret) {
    throw std::system_error(ret, std::generic_category(), "posix_spawnp");
  }
}

static std::mutex& getCwdMutex() {
  // Meyers singleton
  static std::mutex m;
  return m;
}

std::unique_lock<std::mutex> ChildProcess::lockCwdMutex() {
  return std::unique_lock<std::mutex>(getCwdMutex());
}

ChildProcess::~ChildProcess() {
  if (!waited_) {
    watchman::log(
        watchman::FATAL,
        "you must call ChildProcess.wait() before destroying a ChildProcess\n");
  }
}

void ChildProcess::disown() {
  waited_ = true;
}

bool ChildProcess::terminated() {
  if (waited_) {
    return true;
  }

  auto pid = waitpid(pid_, &status_, WNOHANG);
  if (pid == pid_) {
    waited_ = true;
  }

  return waited_;
}

int ChildProcess::wait() {
  if (waited_) {
    return status_;
  }

  while (true) {
    auto pid = waitpid(pid_, &status_, 0);
    if (pid == pid_) {
      waited_ = true;
      return status_;
    }

    if (errno != EINTR) {
      // Pretend that we've successfully waited the child. Otherwise the
      // destructor will abort the process due to waited_ not being set.
      waited_ = true;
      throw std::system_error(errno, std::generic_category(), "waitpid");
    }
  }
}

void ChildProcess::kill(
#ifndef _WIN32
    int signo
#endif
) {
#ifndef _WIN32
  if (!waited_) {
    ::kill(pid_, signo);
  }
#endif
}

std::unique_ptr<Pipe> ChildProcess::takeStdin() {
  CHECK_EQ(1, pipes_.size());
  CHECK_EQ(0, pipes_.begin()->first);
  auto pipe = std::move(pipes_.begin()->second);
  return pipe;
}

std::pair<std::optional<w_string>, std::optional<w_string>>
ChildProcess::communicate(pipeWriteCallback writeCallback) {
#ifdef _WIN32
  return threadedCommunicate(writeCallback);
#else
  return pollingCommunicate(writeCallback);
#endif
}

#ifndef _WIN32
std::pair<std::optional<w_string>, std::optional<w_string>>
ChildProcess::pollingCommunicate(pipeWriteCallback writeCallback) {
  std::unordered_map<int, std::string> outputs;

  for (auto& it : pipes_) {
    if (it.first == STDIN_FILENO) {
      // We only want output streams here
      continue;
    }
    watchman::log(
        watchman::DBG, "Setting up output buffer for fd ", it.first, "\n");
    outputs.emplace(std::make_pair(it.first, ""));
  }

  std::vector<pollfd> pfds;
  std::unordered_map<int, int> revmap;
  pfds.reserve(pipes_.size());
  revmap.reserve(pipes_.size());

  while (!pipes_.empty()) {
    revmap.clear();
    pfds.clear();

    watchman::log(
        watchman::DBG, "Setting up pollfds for ", pipes_.size(), " fds\n");

    for (auto& it : pipes_) {
      pollfd pfd;
      if (it.first == STDIN_FILENO) {
        pfd.fd = it.second->write.fd();
        pfd.events = POLLOUT;
      } else {
        pfd.fd = it.second->read.fd();
        pfd.events = POLLIN;
      }
      pfds.emplace_back(std::move(pfd));
      revmap[pfd.fd] = it.first;
    }

    int r;
    do {
      watchman::log(watchman::DBG, "waiting for ", pfds.size(), " fds\n");
      r = ::poll(pfds.data(), pfds.size(), -1);
    } while (r == -1 && errno == EINTR);
    if (r == -1) {
      watchman::log(watchman::ERR, "poll error\n");
      throw std::system_error(errno, std::generic_category(), "poll");
    }

    for (auto& pfd : pfds) {
      watchman::log(
          watchman::DBG,
          "fd ",
          pfd.fd,
          " revmap to ",
          revmap[pfd.fd],
          " has events ",
          pfd.revents,
          "\n");
      if ((pfd.revents & (POLLHUP | POLLIN)) &&
          revmap[pfd.fd] != STDIN_FILENO) {
        watchman::log(
            watchman::DBG,
            "fd ",
            pfd.fd,
            " rev=",
            revmap[pfd.fd],
            " is readable\n");
        char buf[BUFSIZ];
        auto l = ::read(pfd.fd, buf, sizeof(buf));
        if (l == -1 && (errno == EAGAIN || errno == EINTR)) {
          watchman::log(
              watchman::DBG,
              "fd ",
              pfd.fd,
              " rev=",
              revmap[pfd.fd],
              " read give EAGAIN\n");
          continue;
        }
        if (l == -1) {
          int err = errno;
          watchman::log(
              watchman::ERR,
              "failed to read from pipe fd ",
              pfd.fd,
              " err ",
              folly::errnoStr(err),
              "\n");
          throw std::system_error(
              err, std::generic_category(), "reading from child process");
        }
        watchman::log(
            watchman::DBG,
            "fd ",
            pfd.fd,
            " rev=",
            revmap[pfd.fd],
            " read ",
            l,
            " bytes\n");
        if (l == 0) {
          // Stream is done; close it out.
          pipes_.erase(revmap[pfd.fd]);
          continue;
        }
        outputs[revmap[pfd.fd]].append(buf, l);
      }

      if ((pfd.revents & POLLHUP) && revmap[pfd.fd] == STDIN_FILENO) {
        watchman::log(
            watchman::DBG,
            "fd ",
            pfd.fd,
            " rev ",
            revmap[pfd.fd],
            " closed by the other side\n");
        pipes_.erase(revmap[pfd.fd]);
        continue;
      }
      if ((pfd.revents & POLLOUT) && revmap[pfd.fd] == STDIN_FILENO &&
          writeCallback(pipes_.at(revmap[pfd.fd])->write)) {
        // We should close it
        watchman::log(
            watchman::DBG,
            "fd ",
            pfd.fd,
            " rev ",
            revmap[pfd.fd],
            " writer says to close\n");
        pipes_.erase(revmap[pfd.fd]);
        continue;
      }

      if (pfd.revents & POLLERR) {
        // Something wrong with it, so close it
        pipes_.erase(revmap[pfd.fd]);
        watchman::log(
            watchman::DBG,
            "fd ",
            pfd.fd,
            " rev ",
            revmap[pfd.fd],
            " error status, so closing\n");
        continue;
      }
    }

    watchman::log(watchman::DBG, "remaining pipes ", pipes_.size(), "\n");
  }

  auto optBuffer = [&](int fd) -> std::optional<w_string> {
    auto it = outputs.find(fd);
    if (it == outputs.end()) {
      watchman::log(watchman::DBG, "communicate fd ", fd, " nullptr\n");
      return std::nullopt;
    }
    watchman::log(
        watchman::DBG, "communicate fd ", fd, " gives ", it->second, "\n");
    return w_string(it->second.data(), it->second.size());
  };

  return std::make_pair(optBuffer(STDOUT_FILENO), optBuffer(STDERR_FILENO));
}
#endif

/** Spawn a thread to read from the pipe connected to the specified fd.
 * Returns a Future that will hold a string with the entire output from
 * that stream. */
folly::Future<std::optional<w_string>> ChildProcess::readPipe(int fd) {
  auto it = pipes_.find(fd);
  if (it == pipes_.end()) {
    return std::nullopt;
  }

  auto p = std::make_shared<folly::Promise<w_string>>();
  std::thread thr([this, fd, p]() noexcept {
    std::string result;
    p->setWith([&] {
      auto& pipe = pipes_[fd];
      while (true) {
        char buf[4096];
        auto readResult = pipe->read.read(buf, sizeof(buf));
        readResult.throwIfError();
        auto len = readResult.value();
        if (len == 0) {
          // all done
          break;
        }
        result.append(buf, len);
      }
      return w_string(result.data(), result.size());
    });
  });

  thr.detach();
  return p->getFuture();
}

/** threadedCommunicate uses threads to read from the output streams.
 * It is intended to be used on Windows where there is no reasonable
 * way to carry out a non-blocking read on a pipe.  We compile and
 * test it on all platforms to make it easier to avoid regressions. */
std::pair<std::optional<w_string>, std::optional<w_string>>
ChildProcess::threadedCommunicate(pipeWriteCallback writeCallback) {
  auto outFuture = readPipe(STDOUT_FILENO);
  auto errFuture = readPipe(STDERR_FILENO);

  auto it = pipes_.find(STDIN_FILENO);
  if (it != pipes_.end()) {
    auto& inPipe = pipes_[STDIN_FILENO];
    while (!writeCallback(inPipe->write)) {
      ; // keep trying to greedily write to the pipe
    }
    // Close the input stream; this typically signals the child
    // process that we're done and allows us to safely block
    // on the reads below.
    pipes_.erase(STDIN_FILENO);
  }

  return std::make_pair(std::move(outFuture).get(), std::move(errFuture).get());
}

size_t ChildProcess::getArgMax() {
#ifdef _WIN32
  return 32767;
#else
  int result = sysconf(_SC_ARG_MAX);
  if (result == -1) {
    return _POSIX_ARG_MAX;
  }
  // POSIX guarantees _SC_ARG_MAX must be greater than _POSIX_ARG_MAX.
  return result;
#endif
}

} // namespace watchman
