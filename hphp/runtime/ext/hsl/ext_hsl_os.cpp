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

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/file-await.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/server/cli-server-ext.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/light-process.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/server.h"

#include <folly/functional/Invoke.h>
#include <fmt/core.h>
#include <type_traits>

#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

namespace HPHP {
namespace {

const StaticString
  s_fd("fd"),
  s_resource("resource"),
  s_type("type"),
  s_ErrnoException("HH\\Lib\\_Private\\_OS\\ErrnoException"),
  s_HSL_sockaddr("HH\\Lib\\_Private\\_OS\\sockaddr"),
  s_HSL_sockaddr_in("HH\\Lib\\_Private\\_OS\\sockaddr_in"),
  s_HSL_sockaddr_in6("HH\\Lib\\_Private\\_OS\\sockaddr_in6"),
  s_HSL_sockaddr_un("HH\\Lib\\_Private\\_OS\\sockaddr_un"),
  s_HSL_sockaddr_un_pathname("HH\\Lib\\_Private\\_OS\\sockaddr_un_pathname"),
  s_HSL_sockaddr_un_unnamed("HH\\Lib\\_Private\\_OS\\sockaddr_un_unnamed");

const Slot
  s_sa_family_idx { 0 },
  s_sin_port_idx { 1 },
  s_sin_addr_idx { 2 },
  s_sin6_port_idx { 1 },
  s_sin6_flowinfo_idx { 2 },
  s_sin6_addr_idx { 3 },
  s_sin6_scope_id_idx { 4 },
  s_sun_path_idx { 1 };

IMPLEMENT_REQUEST_LOCAL(std::set<int>, s_fds_to_close);

[[noreturn]]
void throw_errno_exception(int number, const OptString& message = OptString()) {
  throw_object(
    s_ErrnoException,
    make_vec_array(
      message.isNull() ? fmt::format("Errno {}: {}", number, ::strerror(number)) : message,
      number
    )
  );

}

template<class T>
T&& throw_errno_if_minus_one(T&& var) {
  if (var == -1) {
    throw_errno_exception(errno);
  }
  return std::forward<T>(var);
}

template<class TRet, class ...Args, class TFn>
std::enable_if_t<folly::is_invocable_r_v<TRet, TFn, Args...>, TRet>
retry_on_eintr(TRet failureValue, TFn impl, Args... args) {
  TRet ret;
  for (int i = 0; i < 5; ++i) {
    ret = impl(args...);
    if (!(ret == failureValue && errno == EINTR)) {
      break;
    }
  }
  return ret;
}

Object hsl_sockaddr_from_native(const sockaddr_storage& addr, socklen_t len) {
  // Using `ntohs` etc instead of the more explicit `::noths`, as
  // on GNU systems it is sometimes a function, sometimes a macro,
  // depending on optimization level - and if it's a macro, `::ntohs` is
  // invalid.
  switch (addr.ss_family) {
    case AF_UNIX:
      {
        static_assert(
          sizeof(sockaddr_un) <= sizeof(addr),
          "sockaddr_un must fit in allocated space"
        );
        const sockaddr_un* const detail = reinterpret_cast<const sockaddr_un*>(&addr);
        // Documented way to check for "unnamed" sockets: 0-byte-length sun_path
        const bool is_unnamed = len == sizeof(sa_family_t);
        if (is_unnamed) {
          return create_object(s_HSL_sockaddr_un_unnamed, Array::CreateVec());
        }


        // This is *super* fun:
        // - `sun_path` MAY have trailing nulls
        // - `sun_len` MAY include that trailing null on Linux.
        const auto max_path_len = len - offsetof(struct sockaddr_un, sun_path);
        const auto actual_path_len = ::strnlen(detail->sun_path, max_path_len);

        return create_object(
          s_HSL_sockaddr_un_pathname,
          make_vec_array(OptString(detail->sun_path, actual_path_len, CopyString))
        );
      }
    case AF_INET:
      {
        static_assert(
          sizeof(sockaddr_in) <= sizeof(addr),
          "sockaddr_in must fit in allocated space"
        );
        const sockaddr_in* const detail = reinterpret_cast<const sockaddr_in*>(&addr);
        return create_object(
          s_HSL_sockaddr_in,
          make_vec_array(
            ntohs(detail->sin_port),
            ntohl(detail->sin_addr.s_addr)
          )
        );
      }
    case AF_INET6:
      {
        static_assert(
          sizeof(sockaddr_in6) <= sizeof(addr),
          "sockaddr_in6 must fit in allocated space"
        );
        const sockaddr_in6* const detail = reinterpret_cast<const sockaddr_in6*>(&addr);
        return create_object(
          s_HSL_sockaddr_in6,
          make_vec_array(
            ntohs(detail->sin6_port),
            ntohs(detail->sin6_flowinfo),
            OptString(reinterpret_cast<const char*>(detail->sin6_addr.s6_addr), 16, CopyString),
            ntohs(detail->sin6_scope_id)
          )
        );
      }
    default:
      {
        return create_object(
          s_HSL_sockaddr,
          make_vec_array(ntohs(addr.ss_family))
        );
      }
  }
}

void native_sockaddr_from_hsl(const Object& object, sockaddr_storage& native, socklen_t& address_len) {
  if (!object.instanceof(s_HSL_sockaddr)) {
    throw_errno_exception(EINVAL, "Specified address is not a sockaddr");
  }
  bzero(&native, sizeof(native));
  address_len = offsetof(struct sockaddr_storage, ss_family) + sizeof(native.ss_family);
  native.ss_family = object->propRvalAtOffset(s_sa_family_idx).val().num;
#define CHECK_SOCKADDR_TYPE(sa, af) \
  static_assert( \
    sizeof(sa) <= sizeof(native), \
    #sa " must fit in allocated space" \
  );\
  if (!object.instanceof(s_HSL_ ## sa)) { \
    throw_errno_exception( \
      EINVAL, \
      fmt::format( fmt::runtime(\
        "Esapected an instance of type {} for {}, got type {}"), \
        (s_HSL_ ## sa).c_str(), \
        #af, \
        object->getClassName().c_str() \
      ) \
    ); \
  }

  switch (native.ss_family) {
    case AF_UNIX:
      {
        CHECK_SOCKADDR_TYPE(sockaddr_un, AF_UNIX);
        auto detail = reinterpret_cast<sockaddr_un*>(&native);
        const auto offset = offsetof(struct sockaddr_un, sun_path);
        if (object.instanceof(s_HSL_sockaddr_un_unnamed)) {
          address_len = sizeof(sa_family_t);
          return;
        }
        assertx(object.instanceof(s_HSL_sockaddr_un_pathname));

        auto path = object->propRvalAtOffset(s_sun_path_idx).val().pstr;
        if (path->empty()) {
          throw_errno_exception(
            EINVAL,
            "sockaddr_un can not have an empty pathname"
          );
        }
        if (path->size() > sizeof(detail->sun_path)) {
          throw_errno_exception(
            ENAMETOOLONG,
            "Path is too long for sockaddr_un"
          );
        }
        memcpy(detail->sun_path, path->data(), path->size());
        address_len = path->size() + offset;
        return;
      }
    case AF_INET:
      {
        CHECK_SOCKADDR_TYPE(sockaddr_in, AF_INET);
        auto detail = reinterpret_cast<sockaddr_in*>(&native);
        address_len = sizeof(*detail);
        detail->sin_port = htons(object->propRvalAtOffset(s_sin_port_idx).val().num);
        detail->sin_addr.s_addr = htonl(object->propRvalAtOffset(s_sin_addr_idx).val().num);
        return;
      }
    case AF_INET6:
      {
        CHECK_SOCKADDR_TYPE(sockaddr_in6, AF_INET6);
        const auto addr_str = object->propRvalAtOffset(s_sin6_addr_idx).val().pstr;
        const auto in6_addr_len = sizeof(struct in6_addr);
        if (addr_str->size() != in6_addr_len) {
          throw_errno_exception(
            EINVAL,
            fmt::format("sockaddr_in6 address must be {} bytes long", in6_addr_len)
          );
        }
        auto detail = reinterpret_cast<sockaddr_in6*>(&native);
        address_len = sizeof(*detail);
        detail->sin6_port = htons(object->propRvalAtOffset(s_sin6_port_idx).val().num);
        detail->sin6_flowinfo = htonl(object->propRvalAtOffset(s_sin6_flowinfo_idx).val().num);
        detail->sin6_scope_id = htonl(object->propRvalAtOffset(s_sin6_scope_id_idx).val().num);
        memcpy(detail->sin6_addr.s6_addr, addr_str->data(), in6_addr_len);
        return;
      }
    default:
      throw_errno_exception(EAFNOSUPPORT, "Socket address family not supported by HHVM");
  }
#undef CHECK_SOCKADDR_TYPE
}

} // namespace

struct HSLFileDescriptor :
    SystemLib::ClassLoader<"HH\\Lib\\OS\\FileDescriptor"> {
  enum class Type {
    FD,
    RESOURCE
  };

  enum class Awaitability {
    UNKNOWN,
    AWAITABLE,
    NOT_AWAITABLE
  };

  /* Construct an invalid instance.
   *
   * NativeData always invokes this; you probably want to call
   * HSLFileDescriptor::newInstance() instead.
   */
  HSLFileDescriptor() = default;

  HSLFileDescriptor(int fd):
    m_type(Type::FD),
    m_fd(fd) {

    // Callers should check for an invalid FD before trying to construct a
    // FileDescriptor object, but don't trust them.
    if (fd < 0) {
      SystemLib::throwErrorObject(
        "Asked to create a negative FileDescriptor instance; this indicates "
        "a bug in HHVM or a misbehaving CLI client."
      );
    }

    s_fds_to_close->insert(fd);
  }

  /** For migration where mixing old and new code is *required*; strongly
   * prefer adding raw FD support instead.
   *
   * The primary purpose of this is to support FD-based access to
   * STDIN/STDOUT/STDERR, without breaking the PHP resources, logging
   * systems, and exception handlers.
   */
  HSLFileDescriptor(req::ptr<PlainFile> resource):
    m_type(Type::RESOURCE),
    m_resource(resource) {

    if (rawfd() < 0) {
      SystemLib::throwErrorObject(
        "Asked to create a FileDescriptor instance for a FILE* without an fd; "
        "this indicates a bug in HHVM."
      );
    }
  }

  template<class ...Args>
  static Object newInstance(Args&&... args) {
    Object obj { HSLFileDescriptor::classof() };

    auto* data = Native::data<HSLFileDescriptor>(obj);
    new (data) HSLFileDescriptor(args...);
    return obj;
  }

  static HSLFileDescriptor* get(const Object& obj) {
    if (obj.isNull()) {
      raise_typehint_error_without_first_frame(
        "Expected an HSL FileDescriptor, got null");
    }
    if (!obj->instanceof(HSLFileDescriptor::classof())) {
      raise_typehint_error_without_first_frame(
        fmt::format(
          "Expected an HSL FileDescriptor, got instance of class '{}'",
          obj->getClassName().c_str()
        )
      );
    }
    return Native::data<HSLFileDescriptor>(obj);
  }

  static HSLFileDescriptor* get(const Variant& var) {
    if (!var.isObject()) {
      raise_typehint_error_without_first_frame(
        fmt::format(
          "Expected an HSL FileDescriptor, got {}",
          getDataTypeString(var.getType()).c_str()
        )
      );
    }
    return get(var.asCObjRef());
  }

  template<class T>
  static int fd(const T& obj) {
    return get(obj)->fd();
  }

  int fd() const {
    auto fd = rawfd();
    if (fd < 0) throw_errno_exception(EBADF);
    return fd;
  }

  void sweep() {}

  void close() {
    switch (m_type) {
      case Type::FD:
        FileAwait::closeAllForFD(m_fd);
        throw_errno_if_minus_one(::close(fd()));
        s_fds_to_close->erase(m_fd);
        m_fd = -1;
        return;
      case Type::RESOURCE:
        if (m_resource == nullptr) {
          throw_errno_exception(EBADF, "Already closed");
        }
        m_resource->close();
        m_resource = nullptr;
        return;
      default:
        assertx(false);
    }
  }

  Array __debugInfo() const {
    OptString type;
    switch (m_type) {
      case Type::FD:
        type = s_fd;
        break;
      case Type::RESOURCE:
        type = s_resource;
        break;
      default:
        assertx(false);
    }
    return make_dict_array(
      s_type, type,
      s_fd, VarNR{make_tv<KindOfInt64>(rawfd())}
    );
  }

  Awaitability m_awaitability = Awaitability::UNKNOWN;
 private:

  // Which of `m_fd` or `m_resource` is meaningful
  Type m_type = Type::FD;
  // intentionally not closed by destructor: that would introduce observable
  // refcounting behavior. Instead, it's closed at end of request from
  // s_fds_to_close.
  int m_fd = -1;
  // As this is a resource, refcounting and/or GC will deal with it
  // appropriately, nothing special for HSLFileDescriptor - except that we must
  // re-extract the FD every time, in case it was closed elsewhere.
  req::ptr<PlainFile> m_resource = nullptr;

  int rawfd() const {
    if (m_type == Type::FD) {
      return m_fd;
    }
    assertx(m_type == Type::RESOURCE);
    if (!(m_resource && !m_resource->isInvalid())) return -1;
    FILE* stream = m_resource->getStream();
    if (!stream) return -1;
    return ::fileno(stream);
  }
};

Array HHVM_METHOD(HSLFileDescriptor, __debugInfo) {
  return Native::data<HSLFileDescriptor>(this_)->__debugInfo();
}

namespace {

template<class T>
T hsl_cli_unwrap(CLISrvResult<T, int> res) {
  if (res.succeeded()) {
    return res.result();
  }
  throw_errno_exception(res.error());
}

#define HSL_CLI_INVOKE(...) hsl_cli_unwrap(INVOKE_ON_CLI_CLIENT(__VA_ARGS__))

CLISrvResult<ReturnedFdData, int>
CLI_CLIENT_HANDLER(HSL_os_open, std::string path, int64_t flags, int64_t mode) {
  auto const fd = [&] {
    if (flags & O_CREAT) {
      return retry_on_eintr(-1, ::open, path.c_str(), flags, mode);
    }
    return retry_on_eintr(-1, ::open, path.c_str(), flags);
  }();
  if (fd == -1) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, ReturnedFdData { fd } };
}

Object HHVM_FUNCTION(HSL_os_open, const OptString& path, int64_t flags, int64_t mode) {
  int fd = HSL_CLI_INVOKE(
    HSL_os_open,
    path.toCppString(),
    flags,
    mode
  ).fd;
  return HSLFileDescriptor::newInstance(fd);
}

CLISrvResult<std::tuple<ReturnedFdData, std::string>, int>
CLI_CLIENT_HANDLER(HSL_os_mkostemps, std::string path_template, int64_t suffixlen, int64_t flags) {
  // 0 is reasonable for emulating `mkstemp`
  if (suffixlen < 0) {
    return { CLIError {}, EINVAL };
  }

  char* buf = (char*) malloc(path_template.length() + 1);
  if (buf == nullptr) {
    // this will probably fail if malloc fails, but... at least we tried?
    return { CLIError {} , ENOMEM };
  }
  SCOPE_EXIT { free(buf); };

  strncpy(buf, path_template.c_str(), path_template.length());
  buf[path_template.length()] = 0;

  // only checking for memory safety; glibc will also raise EINVAL if
  // strlen(buf) < suffixlen + 6
  if (strlen(buf) < suffixlen) {
    return { CLIError {}, EINVAL };
  }

  auto const fd = retry_on_eintr(-1, ::mkostemps, buf, suffixlen, flags);
  if (fd == -1) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, std::make_tuple(ReturnedFdData { fd }, std::string(buf)) };
}

Array HHVM_FUNCTION(HSL_os_mkostemps, const OptString& path_template, int64_t suffixlen, int64_t flags) {
  ReturnedFdData fd;
  std::string path;
  std::tie(fd, path) = HSL_CLI_INVOKE(
    HSL_os_mkostemps,
    path_template.toCppString(),
    suffixlen,
    flags
  );
  return make_vec_array(
    HSLFileDescriptor::newInstance(fd.fd),
    path
  );
}

CLISrvResult<std::string, int>
CLI_CLIENT_HANDLER(HSL_os_mkdtemp, std::string path_template) {
  const size_t buf_size = path_template.length() + 1;
  char* buf = (char*) malloc(buf_size);
  if (buf == nullptr) {
    return { CLIError {}, ENOMEM };
  }
  SCOPE_EXIT { free(buf); };
  memcpy(buf, path_template.c_str(), buf_size);

  if (::mkdtemp(buf) == nullptr) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, std::string(buf) };
}

OptString HHVM_FUNCTION(HSL_os_mkdtemp, const OptString& path_template) {
  return HSL_CLI_INVOKE(HSL_os_mkdtemp, path_template.toCppString());
}

OptString HHVM_FUNCTION(HSL_os_read, const Object& obj, int64_t max) {
  if (max <= 0) {
    throw_errno_exception(EINVAL, "Max bytes can not be negative");
  }
  if (max > StringData::MaxSize) {
    max = StringData::MaxSize;
  }
  OptString buf(max, ReserveString);
  auto fd = HSLFileDescriptor::fd(obj);
  ssize_t read = retry_on_eintr(-1, ::read, fd, buf.mutableData(), max);
  if (read < 0) {
    buf.clear();
    throw_errno_exception(errno);
  }
  buf.setSize(read);
  return buf;
}

int64_t HHVM_FUNCTION(HSL_os_write, const Object& obj, const OptString& data) {
  auto fd = HSLFileDescriptor::fd(obj);
  ssize_t written = retry_on_eintr(-1, ::write, fd, data.data(), data.length());
  throw_errno_if_minus_one(written);
  return written;
}

void HHVM_FUNCTION(HSL_os_close, const Object& obj) {
  HSLFileDescriptor::get(obj)->close();
}

Object HHVM_FUNCTION(HSL_os_dup, const Object& fd_wrapper) {
  // Not using CLI client/server proxying as `dup()` preserves SO_PEERCRED
  auto in = HSLFileDescriptor::fd(fd_wrapper);
  auto out = throw_errno_if_minus_one(::dup(in));
  return HSLFileDescriptor::newInstance(out);
}

Array HHVM_FUNCTION(HSL_os_pipe) {
  int fds[2];
  throw_errno_if_minus_one(retry_on_eintr(-1, ::pipe, fds));
  return make_vec_array(
    HSLFileDescriptor::newInstance(fds[0]),
    HSLFileDescriptor::newInstance(fds[1])
  );
}

Object HHVM_FUNCTION(HSL_os_getpeername, const Object& fd_wrapper) {
  auto fd = HSLFileDescriptor::fd(fd_wrapper);
  sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  throw_errno_if_minus_one(retry_on_eintr(-1, ::getpeername, fd, reinterpret_cast<sockaddr*>(&addr), &addrlen));
  return hsl_sockaddr_from_native(addr, addrlen);
}

Object HHVM_FUNCTION(HSL_os_getsockname, const Object& fd_wrapper) {
  auto fd = HSLFileDescriptor::fd(fd_wrapper);
  sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  throw_errno_if_minus_one(retry_on_eintr(-1, ::getsockname, fd, reinterpret_cast<sockaddr*>(&addr), &addrlen));
  return hsl_sockaddr_from_native(addr, addrlen);
}

Array HHVM_FUNCTION(HSL_os_socketpair, int64_t domain, int64_t type, int64_t protocol) {
  if (domain != AF_UNIX) {
    // True on Linux; claim true everywhere to avoid worrying about portability,
    // or permissions with different sockets
    throw_errno_exception(EAFNOSUPPORT, "`socketpair` only supports AF_UNIX");
  }
  int fds[2];
  throw_errno_if_minus_one(retry_on_eintr(-1, ::socketpair, domain, type, protocol, fds));
  return make_vec_array(
    HSLFileDescriptor::newInstance(fds[0]),
    HSLFileDescriptor::newInstance(fds[1])
  );
}

CLISrvResult<ReturnedFdData, int>
CLI_CLIENT_HANDLER(HSL_os_socket, int64_t domain, int64_t type, int64_t protocol) {
  auto fd = retry_on_eintr(-1, ::socket, domain, type, protocol);
  if (fd == -1) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, ReturnedFdData { fd } };
}

Object HHVM_FUNCTION(HSL_os_socket, int64_t domain, int64_t type, int64_t protocol) {
  // Use CLI server as:
  // - some operations are/used to be privileged (e.g. raw sockets)
  // - linux allows setting some socket options via type, which may be privileged
  // ... so do the syscall on the client
  int fd = HSL_CLI_INVOKE(
    HSL_os_socket,
    domain,
    type,
    protocol
  ).fd;
  return HSLFileDescriptor::newInstance(fd);
}

#define EINVAL_ON_BAD_SOCKADDR_LEN(ss, sslen) { \
  if (sslen > sizeof(sockaddr_storage) || sslen < 0) { \
    return { CLIError {}, EINVAL }; \
  } \
}

#define IMPL(fun) \
CLISrvResult<int, int> CLI_CLIENT_HANDLER(HSL_os_## fun, \
                                          LoanedFdData fd, \
                                          sockaddr_storage ss, \
                                          int64_t ss_len) { \
  EINVAL_ON_BAD_SOCKADDR_LEN(ss, ss_len); \
  if (retry_on_eintr(-1, ::fun, fd.fd, reinterpret_cast<const sockaddr*>(&ss), ss_len) == -1) { \
    return { CLIError {}, errno }; \
  } \
  return { CLISuccess {}, 0 }; \
} \
\
void HHVM_FUNCTION(HSL_os_ ## fun, const Object& fd, const Object& hsl_sockaddr) { \
  sockaddr_storage ss; \
  socklen_t ss_len; \
  native_sockaddr_from_hsl(hsl_sockaddr, ss, ss_len); \
\
  HSL_CLI_INVOKE( \
    HSL_os_ ## fun, \
    LoanedFdData { HSLFileDescriptor::fd(fd) }, \
    ss, \
    static_cast<int64_t>(ss_len) \
  ); \
}

IMPL(connect)
IMPL(bind)

#undef IMPL
#undef EINVAL_ON_BAD_SOCKADDR_LEN

CLISrvResult<int, int> CLI_CLIENT_HANDLER(HSL_os_listen,
                                          LoanedFdData fd,
                                          int64_t backlog) {
  if (retry_on_eintr(-1, ::listen, fd.fd, backlog) == -1 ) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, 0 };
}

void HHVM_FUNCTION(HSL_os_listen, const Object& fd, int64_t backlog) {
  HSL_CLI_INVOKE(
    HSL_os_listen,
    LoanedFdData { HSLFileDescriptor::fd(fd) },
    backlog
  );
}


Array HHVM_FUNCTION(HSL_os_accept, const Object& hsl_server_fd) {
  auto server_fd  = HSLFileDescriptor::fd(hsl_server_fd);
  sockaddr_storage ss;
  socklen_t ss_len = sizeof(ss);
  const auto fd = retry_on_eintr(
    -1,
    ::accept,
    server_fd,
    reinterpret_cast<sockaddr*>(&ss),
    &ss_len
  );
  throw_errno_if_minus_one(fd);
  return make_vec_array(
    HSLFileDescriptor::newInstance(fd),
    hsl_sockaddr_from_native(ss, ss_len)
  );
}

CLISrvResult<int, int> CLI_CLIENT_HANDLER(HSL_os_fcntl_intarg,
                                          LoanedFdData fd,
                                          int64_t cmd,
                                          int64_t arg) {
  const int result = retry_on_eintr(-1, ::fcntl, fd.fd, cmd, arg);
  if (result == -1) {
    return { CLIError {}, errno };
  }
  return { CLISuccess {}, result };
}

// For now, always returns int, but F_DUPFD or F_GETPATH (MacOS)
// may be implemented later.
Variant HHVM_FUNCTION(HSL_os_fcntl,
                      const Object& fd_wrapper,
                      int64_t cmd,
                      const Variant& arg) {
  const auto fd = HSLFileDescriptor::fd(fd_wrapper);
  switch (cmd) {
    // no arg, getters (no CLI server)
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
      {
        auto result = retry_on_eintr(-1, ::fcntl, fd, cmd);
        throw_errno_if_minus_one(result);
        return result;
      }
    // int arg: may have security implications
    case F_SETFD:
    case F_SETFL:
    case F_SETOWN:
      if (!arg.isInteger()) {
        throw_errno_exception(
          EINVAL,
          "Argument for specific fcntl operation must be an int"
        );
      }
      return HSL_CLI_INVOKE(
        HSL_os_fcntl_intarg,
        LoanedFdData { fd },
        cmd,
        arg.toInt64()
      );
    default:
      throw_errno_exception(
        ENOTSUP,
        "HHVM does not currently support the specified fcntl command"
      );
  }
}

bool HHVM_FUNCTION(HSL_os_isatty, const Object& obj) {
  const auto fd = HSLFileDescriptor::fd(obj);
  errno = 0;
  const auto result = isatty(fd);
  if (result == 1) {
    return true;
  }
  // Not-a-TTY is definitely an expected case for this function,
  // so don't throw. Both Linux and MaOS set errno no ENOTTY for
  // this case.
  //
  // Will still throw for EBADF etc
  if (errno == 0 || errno == ENOTTY) {
    return false;
  }
  throw_errno_exception(errno, "isatty() call failed");
}

OptString HHVM_FUNCTION(HSL_os_ttyname, const Object& obj) {
  const auto fd = HSLFileDescriptor::fd(obj);
  char buf[1024];
  auto error = ttyname_r(fd, buf, sizeof(buf));
  if (error != 0) {
    throw_errno_exception(error, "ttyname_r() call failed");
  }
  const auto len = strnlen(buf, sizeof(buf));
  if (len < 1) {
    throw_errno_exception(ERANGE, "Did not get a valid ttyname");
  }
  return OptString(buf, len, CopyString);
}

int64_t HHVM_FUNCTION(HSL_os_getsockopt_int,
                      const Object& obj,
                      int64_t level,
                      int64_t option) {
  const auto fd = HSLFileDescriptor::fd(obj);

  int value = 0;

  socklen_t value_len = sizeof(value);
  throw_errno_if_minus_one(retry_on_eintr(-1,
    ::getsockopt, fd, level, option, &value, &value_len));
  assertx(value_len <= sizeof(int));
  return value;
}

// in the past, SO_BROADCAST was a privileged operation, but not any more - no
// need for CLI server proxying as long as there are no privileged ops.
void HHVM_FUNCTION(HSL_os_setsockopt_int,
                      const Object& obj,
                      int64_t level,
                      int64_t option,
                      int64_t option_value) {
  static_assert(
    sizeof(int) < sizeof(void*),
    "Security and safety of this function depends on it being unusable for "
    "pointers. (level, option) pairs must be whitelisted if ints and pointers "
    "interchangeable."
  );
  const auto fd = HSLFileDescriptor::fd(obj);
  const int int_value = option_value;
  throw_errno_if_minus_one(retry_on_eintr(-1,
    ::setsockopt, fd, level, option, &int_value, sizeof(int_value)));
}

int64_t HHVM_FUNCTION(HSL_os_lseek, const Object& obj, int64_t offset, int64_t whence) {
  auto fd = HSLFileDescriptor::fd(obj);
  off_t ret = retry_on_eintr(-1, ::lseek, fd, offset, whence);
  throw_errno_if_minus_one(ret);
  return ret;
}

void HHVM_FUNCTION(HSL_os_ftruncate, const Object& obj, int64_t length) {
  auto fd = HSLFileDescriptor::fd(obj);
  // ftruncate() accepts a signed value, and fails with EINVAL if negative. No
  // need to duplicate check here.
  throw_errno_if_minus_one(retry_on_eintr(-1, ::ftruncate, fd, length));
}

Object HHVM_FUNCTION(HSL_os_request_stdio_fd, int64_t client_fd) {
  if (!is_any_cli_mode()) {
    throw_errno_exception(
      EBADF,
      "Request STDIO file descriptors are only available in CLI mode"
    );
  }
  Variant stream;
  switch (client_fd) {
    case STDIN_FILENO:
      stream = BuiltinFiles::getSTDIN();
      break;
    case STDOUT_FILENO:
      stream = BuiltinFiles::getSTDOUT();
      break;
    case STDERR_FILENO:
      stream = BuiltinFiles::getSTDERR();
      break;
    default:
      throw_errno_exception(EINVAL, "Only STDIN, STDOUT, and STDERR fds are permitted");
  }
  if (!stream.isResource()) {
    throw_errno_exception(EBADF, "Unable to retrieve request-local resource");
  }
  if (stream.toResource()->isInvalid()) {
    throw_errno_exception(EBADF, "Resource is invalid (maybe already closed?)");
  }
  const auto builtin = dyn_cast_or_null<BuiltinFile>(stream.toResource());
  if (!builtin) {
    throw_errno_exception(EBADF, "Resource is not a BuiltinFile");
  }

  return HSLFileDescriptor::newInstance(builtin);
}

void HHVM_FUNCTION(HSL_os_flock, const Object& obj, int64_t operation) {
  auto fd = HSLFileDescriptor::fd(obj);
  throw_errno_if_minus_one(retry_on_eintr(-1, ::flock, fd, operation));
}

Object HHVM_FUNCTION(HSL_os_poll_async,
                     const Object& fd_wrapper,
                     int64_t events,
                     int64_t timeout_ns) {
  if (!(events & FileEventHandler::READ_WRITE)) {
    throw_errno_exception(
      EINVAL,
      "Must poll for read, write, or both"
    );
  }
  if (timeout_ns< 0) {
    throw_errno_exception(
      EINVAL,
      "Poll timeout must be >= 0"
    );
  }
  auto hslfd = HSLFileDescriptor::get(fd_wrapper);
  auto fd = hslfd->fd();
  if (hslfd->m_awaitability == HSLFileDescriptor::Awaitability::NOT_AWAITABLE) {
    throw_errno_exception(
      ENOTSUP,
      "Attempted to await a known-non-awaitable File Descriptor"
    );
  } else if (
    hslfd->m_awaitability == HSLFileDescriptor::Awaitability::UNKNOWN
  ) {
    const auto originalFlags = ::fcntl(fd, F_GETFL);
    // This always succeeds...
    ::fcntl(fd, F_SETFL, originalFlags | O_ASYNC);
    // ... but sometimes doesn't actually do anything
    const bool isAsyncableFd = ::fcntl(fd, F_GETFL) & O_ASYNC;
    ::fcntl(fd, F_SETFL, originalFlags);
    if (!isAsyncableFd) {
      hslfd->m_awaitability = HSLFileDescriptor::Awaitability::NOT_AWAITABLE;
      throw_errno_exception(ENOTSUP, "File descriptor is not awaitable");
    }
    hslfd->m_awaitability = HSLFileDescriptor::Awaitability::AWAITABLE;
  }
  // now known to be awaitable

  auto ev = new FileAwait(
    fd,
    events,
    std::chrono::nanoseconds(timeout_ns)
  );
  try {
    return Object{ev->getWaitHandle()};
  } catch (...) {
    assertx(false);
    ev->abandon();
    throw;
  }
}

// options
const StaticString
  s_cwd("cwd"),
  s_setsid("setsid"),
  s_setpgid("setpgid"),
  s_execvpe("execvpe");

// Keep in sync with checkExecAllowed in ext_std_process.cpp.
void checkExecAllowed() {
  if (!Cfg::Server::AllowExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Process execution is disabled by the Server.AllowExec configuration"
    );
  }
  if (!Cfg::Eval::AllowUngatedExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Ungated process execution is disabled by the "
      "Eval.AllowUngatedExec configuration"
    );
  }
}

int64_t HHVM_FUNCTION(HSL_os_fork_and_execve,
                      const OptString& path,
                      const Array& argv,
                      const Array& envp,
                      const Array& fds,
                      const Array& options) {
  checkExecAllowed();
  std::string cwd;
  int flags = Process::FORK_AND_EXECVE_FLAG_NONE;
  int pgid = 0;

  if (options.exists(s_cwd)) {
    const auto& val = options[s_cwd];
    if (!val.isString()) {
      throw_errno_exception(EINVAL, "'cwd' option must be a string");
    }
    cwd = val.asCStrRef().toCppString();
  }

  if (options.exists(s_setsid)) {
    const auto& val = options[s_setsid];
    if (!val.isBoolean()) {
      throw_errno_exception(EINVAL, "'setsid' option must be a bool");
    }
    if (val.asBooleanVal()) {
      flags |= Process::FORK_AND_EXECVE_FLAG_SETSID;
    }
  }

  if(options.exists(s_setpgid)) {
    const auto& val = options[s_setpgid];
    if (!val.isInteger()) {
      throw_errno_exception(EINVAL, "'setpgid' option must be an integer");
    }
    pgid = val.asInt64Val();
    if (pgid <= 0) {
      throw_errno_exception(
        ERANGE,
        "'setpgid' option is <= 0, which is not a valid pid"
      );
    }
    flags |= Process::FORK_AND_EXECVE_FLAG_SETPGID;
  }

  if (options.exists(s_execvpe)) {
    const auto& val = options[s_execvpe];
    if (!val.isBoolean()) {
      throw_errno_exception(EINVAL, "'execvpe' option must be a bool");
    }
    if (val.asBooleanVal()) {
      flags |= Process::FORK_AND_EXECVE_FLAG_EXECVPE;
    }
  }

  auto vec_str_to_cpp_arr = ([] (const Array& vec) {
    std::vector<std::string> arr;
    for (ArrayIter iter(vec); iter; ++iter) {
      arr.push_back(iter.second().toString().toCppString());
    }
    return arr;
  });
  std::map<int, int> fds_map;
  for (ArrayIter iter(fds); iter; ++iter) {
    auto target = iter.first();
    if (!target.isInteger()) {
      throw_errno_exception(EINVAL, "Target FD must be an int");
    }
    fds_map[target.asInt64Val()] = HSLFileDescriptor::fd(iter.second());
  }
  pid_t pid = -1;
  if (LightProcess::Available()) {
    pid = LightProcess::ForkAndExecve(
      path.toCppString(),
      vec_str_to_cpp_arr(argv),
      vec_str_to_cpp_arr(envp),
      cwd,
      fds_map,
      flags,
      pgid
    );
  } else {
    if (Cfg::Server::Mode) {
      throw_errno_exception(
        ENOSYS,
        "Fork and execve requires lightprocesses in server mode"
      );
    }
    pid = Process::ForkAndExecve(
      path.toCppString(),
      vec_str_to_cpp_arr(argv),
      vec_str_to_cpp_arr(envp),
      cwd,
      fds_map,
      flags,
      pgid
    );
  }

  if (pid > 0) {
    return pid;
  }
  switch (pid) {
    case -1: throw_errno_exception(errno, "fork() failed");
    case -2: throw_errno_exception(errno, "chdir() failed");
    case -3: throw_errno_exception(errno, "setsid() failed");
    case -4: throw_errno_exception(errno, "setpgid() failed");
    case -5: throw_errno_exception(errno, "execve() failed");
    default: throw_errno_exception(errno);
  }
}

#undef HSL_CLI_INVOKE

struct OSExtension final : Extension {

  OSExtension() : Extension("hsl_os", "0.1", "hphp_hphpi") {}

  void cliClientInit() override {
    CLI_REGISTER_HANDLER(HSL_os_open);
    CLI_REGISTER_HANDLER(HSL_os_mkostemps);
    CLI_REGISTER_HANDLER(HSL_os_mkdtemp);
    CLI_REGISTER_HANDLER(HSL_os_socket);
    CLI_REGISTER_HANDLER(HSL_os_connect);
    CLI_REGISTER_HANDLER(HSL_os_bind);
    CLI_REGISTER_HANDLER(HSL_os_listen);
    CLI_REGISTER_HANDLER(HSL_os_fcntl_intarg);
  }

  void moduleRegisterNative() override {
    // Remember to update the HHI :)

    Native::registerNativeDataInfo<HSLFileDescriptor>();
    HHVM_NAMED_ME(HH\\Lib\\OS\\FileDescriptor, __debugInfo, HHVM_MN(HSLFileDescriptor, __debugInfo));

    // The preprocessor doesn't like "\" immediately before a ##
#define E(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\E##name, E##name)
    E(2BIG);
    E(ACCES);
    E(ADDRINUSE);
    E(ADDRNOTAVAIL);
    E(AFNOSUPPORT);
    E(AGAIN);
    E(ALREADY);
    E(BADF);
    E(BADMSG);
    E(BUSY);
    E(CANCELED);
    E(CHILD);
    E(CONNABORTED);
    E(CONNREFUSED);
    E(CONNRESET);
    E(DEADLK);
    E(DESTADDRREQ);
    E(DOM);
    E(DQUOT);
    E(EXIST);
    E(FAULT);
    E(FBIG);
    E(HOSTDOWN);
    E(HOSTUNREACH);
    E(IDRM);
    E(ILSEQ);
    E(INPROGRESS);
    E(INTR);
    E(INVAL);
    E(IO);
    E(ISCONN);
    E(ISDIR);
    E(LOOP);
    E(MFILE);
    E(MLINK);
    E(MSGSIZE);
    E(MULTIHOP);
    E(NAMETOOLONG);
    E(NETDOWN);
    E(NETRESET);
    E(NETUNREACH);
    E(NFILE);
    E(NOBUFS);
    E(NODATA);
    E(NODEV);
    E(NOENT);
    E(NOEXEC);
    E(NOLCK);
    E(NOLINK);
    E(NOMEM);
    E(NOMSG);
    E(NOPROTOOPT);
    E(NOSPC);
    E(NOSR);
    E(NOSTR);
    E(NOSYS);
    E(NOTBLK);
    E(NOTCONN);
    E(NOTDIR);
    E(NOTEMPTY);
    E(NOTSOCK);
    E(NOTSUP);
    E(NOTTY);
    E(NXIO);
    E(OPNOTSUPP);
    E(OVERFLOW);
    E(PERM);
    E(PFNOSUPPORT);
    E(PIPE);
    E(PROTO);
    E(PROTONOSUPPORT);
    E(PROTOTYPE);
    E(RANGE);
    E(ROFS);
    E(SHUTDOWN);
    E(SOCKTNOSUPPORT);
    E(SPIPE);
    E(SRCH);
    E(STALE);
    E(TIME);
    E(TIMEDOUT);
    E(TXTBSY);
    E(USERS);
    E(XDEV);
#undef E
#define O_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\O_##name, O_##name)
    O_(RDONLY);
    O_(WRONLY);
    O_(RDWR);
    O_(NONBLOCK);
    O_(APPEND);
    O_(CREAT);
    O_(TRUNC);
    O_(EXCL);
    O_(NOFOLLOW);
    O_(CLOEXEC);
#undef O_
    // MacOS: O_EVTONLY, O_SHLOCK, O_EXLOCK, O_SYMLINK
    // Linux: ... lots ...

    CLI_REGISTER_HANDLER(HSL_os_open);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\open, HSL_os_open);

    CLI_REGISTER_HANDLER(HSL_os_mkostemps);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\mkostemps, HSL_os_mkostemps);
    CLI_REGISTER_HANDLER(HSL_os_mkdtemp);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\mkdtemp, HSL_os_mkdtemp);

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\pipe, HSL_os_pipe);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\poll_async, HSL_os_poll_async);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\read, HSL_os_read);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\write, HSL_os_write);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\close, HSL_os_close);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\dup, HSL_os_dup);

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\request_stdio_fd, HSL_os_request_stdio_fd);
    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\STDIN_FILENO, STDIN_FILENO);
    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\STDOUT_FILENO, STDOUT_FILENO);
    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\STDERR_FILENO, STDERR_FILENO);

#define SEEK_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\SEEK_##name, SEEK_##name)
    SEEK_(SET);
    SEEK_(CUR);
    SEEK_(END);
    SEEK_(HOLE);
    SEEK_(DATA);
#undef SEEK_

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\lseek, HSL_os_lseek);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\ftruncate, HSL_os_ftruncate);

#define LOCK_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\LOCK_##name, LOCK_##name)
    LOCK_(SH);
    LOCK_(EX);
    LOCK_(NB);
    LOCK_(UN);
#undef LOCK_
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\flock, HSL_os_flock);

#define AF_(name) \
  HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\AF_##name, AF_##name); \
  HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\PF_##name, PF_##name);
    AF_(UNSPEC);
    AF_(UNIX);
    AF_(INET);
    AF_(INET6);
    AF_(MAX);
#undef AF_

#define SOCK_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\SOCK_##name, SOCK_##name)
    SOCK_(STREAM);
    SOCK_(DGRAM);
    SOCK_(RAW);
#undef SOCK_

#define INADDR_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\INADDR_##name, INADDR_##name)
    INADDR_(ANY);
    INADDR_(LOOPBACK);
    INADDR_(BROADCAST);
    INADDR_(NONE);
#undef INADDR_

    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\SUN_PATH_LEN, sizeof(sockaddr_un::sun_path));

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\getpeername, HSL_os_getpeername);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\getsockname, HSL_os_getsockname);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\socketpair, HSL_os_socketpair);

    CLI_REGISTER_HANDLER(HSL_os_socket);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\socket, HSL_os_socket);
    CLI_REGISTER_HANDLER(HSL_os_connect);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\connect, HSL_os_connect);
    CLI_REGISTER_HANDLER(HSL_os_bind);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\bind, HSL_os_bind);
    CLI_REGISTER_HANDLER(HSL_os_listen);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\listen, HSL_os_listen);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\accept, HSL_os_accept);

#define F_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\F_##name, F_##name)
    F_(GETFD);
    F_(GETFL);
    F_(GETOWN);
    F_(SETFD);
    F_(SETFL);
    F_(SETOWN);
#undef F_
    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\FD_CLOEXEC, FD_CLOEXEC);

    CLI_REGISTER_HANDLER(HSL_os_fcntl_intarg);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\fcntl, HSL_os_fcntl);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\isatty, HSL_os_isatty);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\ttyname, HSL_os_ttyname);

    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\SOL_SOCKET, SOL_SOCKET);
    // The constant formerly known as SOL_TCP
    HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\IPPROTO_TCP, IPPROTO_TCP);
#define SO_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\SO_##name, SO_##name)
    SO_(BROADCAST);
    SO_(DEBUG);
    SO_(DONTROUTE);
    SO_(ERROR);
    SO_(KEEPALIVE);
    SO_(LINGER);
    SO_(OOBINLINE);
    SO_(RCVBUF);
    SO_(RCVLOWAT);
    SO_(REUSEADDR);
    SO_(REUSEPORT);
    SO_(SNDBUF);
    SO_(SNDLOWAT);
    SO_(TYPE);
#undef SO_
#define TCP_(name) HHVM_RC_INT(HH\\Lib\\_Private\\_OS\\TCP_##name, TCP_##name)
    TCP_(FASTOPEN);
    TCP_(KEEPCNT);
    TCP_(KEEPINTVL);
    TCP_(MAXSEG);
    TCP_(NODELAY);
    TCP_(NOTSENT_LOWAT);
#undef TCP_

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\getsockopt_int, HSL_os_getsockopt_int);
    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\setsockopt_int, HSL_os_setsockopt_int);

    HHVM_FALIAS(HH\\Lib\\_Private\\_OS\\fork_and_execve, HSL_os_fork_and_execve);
  }

  void requestShutdown() override {
    if (s_fds_to_close.isNull()) {
      return;
    }
    for (int fd : *s_fds_to_close) {
      // retrying this on EINTR would be unsafe: the call can be interrupted
      // after the FD has been freed but the kernel is doing other work - and
      // the FD may have been reused by the time we retry, so retrying on
      // EINTR may close some unrelated fd
      ::close(fd);
    }
    s_fds_to_close.destroy();
  }
} s_os_extension;

} // anonymous namespace
} // namespace HPHP
