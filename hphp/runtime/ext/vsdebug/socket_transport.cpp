/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2017-present Facebook, Inc. (http://www.facebook.com)  |
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

#include "hphp/runtime/ext/vsdebug/socket_transport.h"

#include "hphp/runtime/ext/vsdebug/debugger.h"
#include "hphp/runtime/ext/vsdebug/ext_vsdebug.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/user-info.h"

#include <algorithm>

#include <pwd.h>
#include <grp.h>
#include <sys/socket.h>

namespace HPHP {
namespace VSDEBUG {

SocketTransport::SocketTransport(
  Debugger* debugger,
  const SocketTransportOptions& options
) :
  DebugTransport(debugger),
  m_terminating(false),
  m_clientConnected(false),
  m_listenPort(options.tcpListenPort),
  m_domainSocketPath(options.domainSocketPath),
  m_connectThread(this, &SocketTransport::listenForClientConnection) {

  Lock lock(m_lock);

  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "SocketTransport created with options: domain socket path=%s, tcp port=%d",
    options.domainSocketPath.empty() ? "" : options.domainSocketPath.c_str(),
    options.tcpListenPort
  );

  assertx(m_abortPipeFd[0] == -1 && m_abortPipeFd[1] == -1);

  // Create a set of pipe file descriptors to use to inform the thread
  // polling for socket connections that it's time to exit.
  createAbortPipe();
  m_connectThread.setNoInitFini();
  m_connectThread.start();
}

SocketTransport::~SocketTransport() {
  {
    Lock lock(m_lock);
    m_terminating = true;
  }

  stopConnectionThread();

  if (useDomainSocket() && !m_domainSocketPath.empty()) {
    unlink(m_domainSocketPath.c_str());
  }
}

void SocketTransport::stopConnectionThread() {
  char value = '\0';
  write(m_abortPipeFd[1], &value, 1);
  close(m_abortPipeFd[1]);
  m_abortPipeFd[1] = -1;
  m_connectThread.waitForEnd();
}

void SocketTransport::createAbortPipe() {

  if (m_abortPipeFd[0] != -1) {
    close(m_abortPipeFd[0]);
  }

  if (m_abortPipeFd[1] != -1) {
    close(m_abortPipeFd[1]);
  }

  if (pipe(m_abortPipeFd) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to open pipe for transport termination event."
    );

    // This is unexpected and treated as fatal because we won't be able
    // to stop the polling thread in an orderly fashion at this point.
    assertx(false);
  }
}

void SocketTransport::onClientDisconnected() {
  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "SocketTransport client disconnected."
  );

  DebugTransport::onClientDisconnected();

  // If we're not shutting down, start listening for new connections.
  {
    Lock lock(m_lock);

    if (!m_terminating && m_clientConnected) {
      stopConnectionThread();
      m_clientConnected = false;
      m_clientInfo.clientUser = "";
      m_clientInfo.clientPid = 0;
      createAbortPipe();
      m_connectThread.start();
    }
  }
}

bool SocketTransport::clientConnected() const {
  Lock lock(m_lock);
  return m_clientConnected;
}

bool SocketTransport::useDomainSocket() const {
  return !m_domainSocketPath.empty();
}

void SocketTransport::listenForClientConnection() {
  // If there was a previous connection in the base class, shut it down.
  // This will close the old transport and wait for both the send and receive
  // worker threads to terminate before proceeding.
  if (getTransportFd() >= 0) {
    shutdown();
  }

  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "SocketTransport worker thread listening for connections..."
  );

  int abortFd;
  {
    Lock lock(m_lock);
    abortFd = m_abortPipeFd[0];
    assertx(abortFd >= 0);
  }

  std::vector<int> socketFds;
  struct addrinfo* ai = nullptr;

  SCOPE_EXIT {
    if (ai != nullptr) {
      freeaddrinfo(ai);
    }

    std::for_each(socketFds.begin(), socketFds.end(), close);

    close(abortFd);
    m_abortPipeFd[0] = -1;

    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "SocketTransport connection polling thread exiting."
    );
  };

  // Share existing DebuggerDisableIPv6 configuration with hphpd.
  bool anyInterfaceBound = false;
  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Socket transport using domain socket? %s",
    useDomainSocket() ? "YES" : "NO"
  );

  if (!useDomainSocket()) {
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));

    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;

    if (Cfg::Debugger::DisableIPv6) {
      hint.ai_family = AF_INET;
    }

    const auto name = Cfg::Debugger::ServerIP.empty()
      ? "localhost"
      : Cfg::Debugger::ServerIP.c_str();
    if (getaddrinfo(name, std::to_string(m_listenPort).c_str(), &hint, &ai)) {
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Failed to call getaddrinfo: %d.",
        errno
      );

      return;
    }

    // Attempt to listen on the specified port on each of this host's available
    // addresses.
    struct addrinfo* address;
    for (address = ai; address != nullptr; address = address->ai_next) {
      if (bindAndListenTCP(address, socketFds)) {
        anyInterfaceBound = true;
      }
    }
  } else {
    // Use a UNIX domain socket.
    if (bindAndListenDomain(socketFds)) {
      anyInterfaceBound = true;
    }
  }

  if (!anyInterfaceBound) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelWarning,
      "Debugger failed to bind to any interface!"
    );
    return;
  } else {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "Debugger bound to at least one interface."
    );
  }

  waitForConnection(socketFds, abortFd);
}

bool SocketTransport::setSocketPermissions(const char* path) {
  int mask = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
  const std::string& configGroup = VSDebugExtension::getDomainSocketGroup();

  if (!configGroup.empty()) {
    const char* groupName = configGroup.c_str();
    auto buf = GroupBuffer{};
    struct group* grp;
    if (getgrnam_r(groupName, &buf.ent, buf.data.get(), buf.size, &grp)) {
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Failed to call getgrnam_r for group %s: %s",
        groupName,
        folly::errnoStr(errno).c_str()
      );
      return false;
    }
    if (grp == nullptr) {
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Group %s does not exist",
        groupName
      );
      return false;
    }

    auto gid = grp->gr_gid;
    if (chown(path, static_cast<uid_t>(-1), gid) < 0) {
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Failed to call chown for socket %s to group id %d",
        path,
        gid
      );
      return false;
    }

    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "Successfully called chown for socket %s to group id %d",
      path,
      gid
    );
  } else {
    // If no security group is configured, fall back to the previous
    // behavior of allowing any local user to talk to the debugger.
    mask |= S_IROTH | S_IXOTH | S_IWOTH;
  }

  // Set socket permissions
  if (chmod(path, mask) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call chmod for domain socket: %d.",
      errno
    );
    return false;
  }

  return true;
}

bool SocketTransport::bindAndListenDomain(std::vector<int>& socketFds) {
  struct sockaddr_un addr;
  std::string socketPath = m_domainSocketPath;

  int sockFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sockFd < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call socket for type AF_UNIX: %d.",
      errno
    );

    return false;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path,
          socketPath.c_str(),
          sizeof(addr.sun_path) - 1);

  // If the previous socket was left hanging around, clean it up.
  struct stat tstat;
  if (lstat(addr.sun_path, &tstat) == 0) {
    if (S_ISSOCK(tstat.st_mode)) {
      unlink(addr.sun_path);
    }
  }

  if (bind(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call bind for type AF_UNIX: %d.",
      errno
    );

    return false;
  }

  if (!setSocketPermissions(addr.sun_path)) {
    return false;
  }

  if (listen(sockFd, 0) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call listen for type AF_UNIX: %d.",
      errno
    );

    return false;
  }

  socketFds.push_back(sockFd);
  return true;
}

bool SocketTransport::bindAndListenTCP(
  struct addrinfo* address,
  std::vector<int>& socketFds
) {
  int fd = socket(address->ai_family,
                  address->ai_socktype | SOCK_CLOEXEC,
                  address->ai_protocol);

  if (fd < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelWarning,
      "SocketTransport: socket() call failed: %d",
      errno
    );

    return false;
  }

  constexpr int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (address->ai_family == AF_INET6) {
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
  }

  if (bind(fd, address->ai_addr, address->ai_addrlen) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call bind: %d. (%s)",
      errno,
      strerror(errno)
    );

    return false;
  }

  if (listen(fd, 1) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "Failed to call listen on port %u: %d. (%s)",
      m_listenPort,
      errno,
      strerror(errno)
    );

    return false;
  }

  socketFds.push_back(fd);
  return true;
}

void SocketTransport::rejectClientWithMsg(
  int newFd,
  int abortFd,
  RejectReason reason,
  ClientInfo& existingClientInfo
) {
  std::string msg = "An internal error occurred while connecting to HHVM.";
  const bool validClient = !existingClientInfo.clientUser.empty() &&
      existingClientInfo.clientPid > 0;

  if (reason == RejectReason::ClientAlreadyAttached) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "SocketTransport: new client connection rejected because another "
        "client is already connected."
    );

    msg = "Failed to attach to HHVM: another client is already attached";
    if (validClient) {
      msg += ": process ";
      msg += std::to_string(existingClientInfo.clientPid);
      msg += " owned by user ";
      msg += existingClientInfo.clientUser;
    }
  } else if (reason == RejectReason::AuthenticationFailed) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "SocketTransport: new client connection rejected because domain "
        "socket peer could not be authenticated."
    );
    msg = "Failed to attach to HHVM: Access was denied.";
  }

  folly::dynamic rejectMsg = folly::dynamic::object;
  rejectMsg["category"] = OutputLevelError;
  rejectMsg["output"] = msg;

  folly::dynamic response = folly::dynamic::object;
  response["event"] = EventTypeOutput;
  response["body"] = rejectMsg;
  response["type"] = MessageTypeEvent;

  // Send the user an error message.
  const std::string serialized =  folly::toJson(response);
  const char* output = serialized.c_str();
  write(newFd, output, strlen(output) + 1);

  // Send a custom refused event that clients can detect.
  folly::dynamic rejectDetails = folly::dynamic::object;
  if (validClient) {
    rejectDetails["pid"] = existingClientInfo.clientPid;
    rejectDetails["user"] = existingClientInfo.clientUser;
  }

  folly::dynamic refusedEvent = folly::dynamic::object;
  refusedEvent["event"] = EventTypeConnectionRefused;
  refusedEvent["body"] = rejectDetails;
  refusedEvent["type"] = MessageTypeEvent;
  const std::string json = folly::toJson(refusedEvent);
  const char* refusedEventOutput = json.c_str();
  write(newFd, refusedEventOutput, strlen(refusedEventOutput) + 1);
  shutdownSocket(newFd, abortFd);
}

void SocketTransport::cleanupFd(int fd) {
  shutdownSocket(fd, m_abortPipeFd[0]);
}

void SocketTransport::shutdownSocket(int sockFd, int abortFd) {
  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Shutting down sockFd(%d) abortFd(%d)",
    sockFd,
    abortFd
  );

  // The process might be exiting, and this logging is important
  // for debugging shutdown issues, so explicitly flush here.
  VSDebugLogger::LogFlush();

  // Perform an orderly shutdown of the socket so that the message is actually
  // sent and received. This requires us to shutdown the write end of the socket
  // and then drain the receive buffer before closing the socket. If abortFd
  // is signalled before this is complete, we just close the socket and bail.
  if (::shutdown(sockFd, SHUT_WR) < 0) {
    // This is normal if there is no client connection.
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelWarning,
      "Socket ::shutdown returned: %d",
      errno
    );
    return;
  }

  int result;
  size_t size = sizeof(struct pollfd) * 2;
  struct pollfd* fds = (struct pollfd*)malloc(size);
  char* buffer = (char*)malloc(1024);
  SCOPE_EXIT {
    if (fds != nullptr) {
      free(fds);
    }

    close(sockFd);

    if (buffer != nullptr) {
      free(buffer);
    }
  };

  if (buffer == nullptr || fds == nullptr) {
    return;
  }

  int eventMask = POLLIN | POLLERR | POLLHUP | g_platformPollFlags;
  constexpr int abortIdx = 0;
  constexpr int readIdx = 1;

  memset(fds, 0, size);
  fds[abortIdx].fd = abortFd;
  fds[abortIdx].events = eventMask;
  fds[readIdx].fd = sockFd;
  fds[readIdx].events = eventMask;

  while (true) {
    result = poll(fds, 2, -1);
    if (result == -EINTR) {
      continue;
    } else if (result < 0 ||
               fds[abortIdx].revents != 0 ||
               fds[readIdx].revents & POLLERR ||
               fds[readIdx].revents & POLLHUP ||
               fds[readIdx].revents & g_platformPollFlags) {

      break;
    } else if (fds[readIdx].revents & POLLIN) {
      result = read(sockFd, buffer, 1024);
      if (result <= 0) {
        break;
      }
    }
  }
}

void SocketTransport::waitForConnection(
  std::vector<int>& socketFds,
  int abortFd
) {
  // Wait for a connection on any of the fds we are listening to. We allow only
  // one debugger client to connect a a time, so once any bound fd accepts a
  // connection, we stop listening on all the others.
  int count = socketFds.size() + 1;
  size_t size = sizeof(struct pollfd) * count;
  struct pollfd* fds = (struct pollfd*)malloc(size);
  if (fds == nullptr) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelError,
      "SocketTransport out of memory while trying to create pollfd."
    );
    return;
  }

  memset(fds, 0, size);

  SCOPE_EXIT {
    if (fds != nullptr) {
      free(fds);
    }
  };

  // fds[0] will contain the read end of our "abort" pipe. Another thread will
  // write data to tihs pipe to signal it's time for this worker to stop
  // blocking in poll() and terminate.
  int eventMask = POLLIN | POLLERR | POLLHUP | g_platformPollFlags;
  fds[0].fd = abortFd;
  fds[0].events = eventMask;

  for (unsigned int i = 1; i < count; i++) {
    fds[i].fd = socketFds[i - 1];
    fds[i].events = eventMask;
  }

  // Poll socket fds until a connection is established or we're terminated.
  while (true) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "SocketTransport polling for connections..."
    );

    int ret = poll(fds, count, -1);
    if (ret < 0) {
      if (ret == -EINTR) {
        continue;
      }

      VSDebugLogger::Log(
        VSDebugLogger::LogLevelError,
        "Polling inputs failed: %d. (%s)",
        errno,
        strerror(errno)
      );
      return;
    }

    if (fds[0].revents != 0) {
      VSDebugLogger::Log(
        VSDebugLogger::LogLevelInfo,
        "Socket polling thread terminating due to shutdown request."
      );
      return;
    }

    struct sockaddr sa;
    socklen_t len = sizeof(sa);
    for (unsigned int i = 1; i < count; i++) {
      if (fds[i].revents & POLLIN) {
        int newFd = ::accept(fds[i].fd, &sa, &len);
        if (newFd < 0) {
          VSDebugLogger::Log(
            VSDebugLogger::LogLevelWarning,
            "Accept returned an error: %d. (%s)",
            errno,
            strerror(errno)
          );
        } else {
          m_lock.lock();
          bool locked = true;
          SCOPE_EXIT {
            if (locked) {
              m_lock.unlock();
            }
          };

          const bool domainSocket = useDomainSocket();
          RejectReason rejectReason = RejectReason::None;
          if (m_clientConnected) {
            rejectReason = RejectReason::ClientAlreadyAttached;
          } else if (domainSocket &&
                     !validatePeerCreds(newFd, m_clientInfo)) {
            rejectReason = RejectReason::AuthenticationFailed;
          }

          if (rejectReason != RejectReason::None) {
            locked = false;
            m_lock.unlock();
            rejectClientWithMsg(
              newFd,
              abortFd,
              rejectReason,
              m_clientInfo
            );
            // NB this code used to have a manual m_lock.lock() here; however,
            // this causes  a deadlock if the  connection  thread  is  exiting
            // (see D15701465 for details).
          } else {
            VSDebugLogger::Log(
              VSDebugLogger::LogLevelInfo,
              "SocketTransport: new client connection accepted."
            );

            // We have established a connection with a client.
            m_clientConnected = true;
            setTransportFd(newFd);
            m_debugger->setClientConnected(
              true,
              false,
              domainSocket ? &m_clientInfo : nullptr
            );
          }
        }
      }

      // Reset the event flags.
      fds[i].revents = 0;
    }
  }
}

bool SocketTransport::validatePeerCreds(int newFd, ClientInfo& info) {
#ifdef SO_PEERCRED
  struct ucred ucred = {0};
  socklen_t len = sizeof(ucred);

  if (getsockopt(newFd,
                 SOL_SOCKET,
                 SO_PEERCRED,
                 &ucred,
                 &len) < 0) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "Failed to get creds from peer socket: %d",
      errno
    );
    return false;
  }

  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Obtained credentials from peer socket: pid=%ld, uid=%ld",
    (long)ucred.pid,
    (long)ucred.uid
  );

  auto buf = PasswdBuffer{};
  passwd* pw;
  if (getpwuid_r(ucred.uid, &buf.ent, buf.data.get(), buf.size, &pw)) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "Failed to call getpwuid_r: %d",
      errno
    );
    return false;
  }

  if (pw == nullptr) {
    VSDebugLogger::Log(
      VSDebugLogger::LogLevelInfo,
      "No such uid: %d",
      ucred.uid
    );
    return false;
  }

  VSDebugLogger::Log(
    VSDebugLogger::LogLevelInfo,
    "Client username is: %s",
    pw->pw_name
  );

  info.clientUser = std::string(pw->pw_name);
  info.clientPid = ucred.pid;
  info.clientUid = ucred.uid;
  return pw->pw_name != nullptr && strlen(pw->pw_name) > 0;
#else
  return true;
#endif
}

}
}
