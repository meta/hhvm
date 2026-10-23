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
#include "hphp/runtime/server/http-protocol.h"

#include <string>

#include <folly/Conv.h>
#include <folly/portability/SysTime.h>
#include <fmt/format.h>

#include "hphp/util/arch.h"
#include "hphp/util/configs/debug.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/log.h"
#include "hphp/util/logger.h"
#include "hphp/util/stack-trace.h"
#include "hphp/util/text-util.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/http-client.h"
#include "hphp/runtime/base/php-globals.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/base/zend-url.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/replay-transport.h"
#include "hphp/runtime/server/request-uri.h"
#include "hphp/runtime/server/source-root-info.h"
#include "hphp/runtime/server/transport.h"
#include "hphp/runtime/server/upload.h"
#include "hphp/runtime/server/virtual-host.h"

#define DEFAULT_POST_CONTENT_TYPE "application/x-www-form-urlencoded"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
// helper functions

static bool read_all_post_data(Transport *transport,
                               const void *&data, size_t &size) {
  if (transport->hasMorePostData()) {
    data = buffer_duplicate(data, size);
    do {
      size_t delta = 0;
      const void *extra = transport->getMorePostData(delta);
      if (size + delta < VirtualHost::GetMaxPostSize()) {
        data = buffer_append(data, size, extra, delta);
        size += delta;
      }
    } while (transport->hasMorePostData());
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

const VirtualHost *HttpProtocol::GetVirtualHost(Transport *transport) {
  if (!RuntimeOption::VirtualHosts.empty()) {
    std::string host = transport->getHeader("Host");
    if (auto vh = VirtualHost::Resolve(host)) {
      VirtualHost::SetCurrent(vh);
      return vh;
    }
  }
  VirtualHost::SetCurrent(nullptr);
  return VirtualHost::GetCurrent();
}

const StaticString
  s_HPHP("HPHP"),
  s_HHVM("HHVM"),
  s_HHVM_JIT("HHVM_JIT"),
  s_HHVM_ARCH("HHVM_ARCH"),
  s_HPHP_SERVER("HPHP_SERVER"),
  s_HTTP_HOST("HTTP_HOST"),
  s_CONTENT_TYPE("CONTENT_TYPE"),
  s_CONTENT_LENGTH("CONTENT_LENGTH"),
  s_PHP_AUTH_USER("PHP_AUTH_USER"),
  s_PHP_AUTH_PW("PHP_AUTH_PW"),
  s_PHP_AUTH_DIGEST("PHP_AUTH_DIGEST"),
  s_REQUEST_URI("REQUEST_URI"),
  s_SCRIPT_URL("SCRIPT_URL"),
  s_SCRIPT_URI("SCRIPT_URI"),
  s_SCRIPT_NAME("SCRIPT_NAME"),
  s_PHP_SELF("PHP_SELF"),
  s_SCRIPT_FILENAME("SCRIPT_FILENAME"),
  s_PATH_TRANSLATED("PATH_TRANSLATED"),
  s_PATH_INFO("PATH_INFO"),
  s_argc("argc"),
  s_argv("argv"),
  s__SERVER("_SERVER"),
  s__GET("_GET"),
  s__POST("_POST"),
  s__ENV("_ENV"),
  s_HTTP_RAW_POST_DATA("HTTP_RAW_POST_DATA"),
  s__FILES("_FILES"),
  s_GATEWAY_INTERFACE("GATEWAY_INTERFACE"),
  s_CGI_1_1("CGI/1.1"),
  s_SERVER_ADDR("SERVER_ADDR"),
  s_SERVER_NAME("SERVER_NAME"),
  s_SERVER_PORT("SERVER_PORT"),
  s_SERVER_SOFTWARE("SERVER_SOFTWARE"),
  s_SERVER_PROTOCOL("SERVER_PROTOCOL"),
  s_SERVER_ADMIN("SERVER_ADMIN"),
  s_SERVER_SIGNATURE("SERVER_SIGNATURE"),
  s_REQUEST_METHOD("REQUEST_METHOD"),
  s_GET("GET"),
  s_HEAD("HEAD"),
  s_POST("POST"),
  s_HTTPS("HTTPS"),
  s_on("on"),
  s_QUERY_STRING("QUERY_STRING"),
  s_REMOTE_ADDR("REMOTE_ADDR"),
  s_REMOTE_HOST("REMOTE_HOST"),
  s_REMOTE_PORT("REMOTE_PORT"),
  s_DOCUMENT_ROOT("DOCUMENT_ROOT"),
  s_THREAD_TYPE("THREAD_TYPE"),
  s_dash("-"),
  s_underscore("_"),
  s_HTTP_("HTTP_"),
  s_forwardslash("/");

static void PrepareEnv(Array& env, Transport *transport) {
  // $_ENV
  process_env_variables(env);
  env.set(s_HPHP, 1);
  env.set(s_HHVM, 1);
  if (Cfg::Jit::Enabled) {
    env.set(s_HHVM_JIT, 1);
  }
  ARCH_MATCH(
    [&](arch::X64) {
      env.set(s_HHVM_ARCH, "x64");
    },
    [&](arch::ARM) {
      env.set(s_HHVM_ARCH, "arm");
    }
  );

  if (!is_any_cli_mode()) {
    env.set(s_HPHP_SERVER, 1);
  }

  // Do this last so it can overwrite all the previous settings
  HeaderMap transportParams;
  transport->getTransportParams(transportParams);
  for (auto const& header : transportParams) {
    OptString key(header.first);
    OptString value(header.second.back());
    g_context->setenv(key, value);
    env.set(env.convertKey<IntishCast::Cast>(key),
            make_tv<KindOfString>(value.get()));
  }
}

void HttpProtocol::PrepareSystemVariables(Transport *transport,
                                          const RequestURI &r) {
  auto const vhost = VirtualHost::GetCurrent();
  auto const emptyArr = empty_dict_array();
  php_global_set(s__SERVER, emptyArr);
  php_global_set(s__GET, emptyArr);
  php_global_set(s__POST, emptyArr);
  php_global_set(s__FILES, emptyArr);
  php_global_set(s__ENV, emptyArr);

  // according to doc if content type is multipart/form-data
  // $HTTP_RAW_POST_DATA should always not available
  bool shouldSetHttpRawPostData = false;
  if (Cfg::Server::AlwaysPopulateRawPostData) {
    std::string dummy;
    if (!IsRfc1867(transport->getHeader("Content-Type"), dummy)) {
      shouldSetHttpRawPostData = true;
    }
  }

  if (shouldSetHttpRawPostData) {
    php_global_set(s_HTTP_RAW_POST_DATA, Variant{String::Empty()});
  }

#define X(name)                                       \
  auto name##arr = empty_dict_array();                    \
  SCOPE_EXIT { php_global_set(s__##name, name##arr); };

  X(ENV)
  X(GET)
  X(POST)
  X(FILES)
  X(SERVER)

#undef X

  Variant HTTP_RAW_POST_DATA;
  SCOPE_EXIT {
    if (shouldSetHttpRawPostData) {
      php_global_set(s_HTTP_RAW_POST_DATA, std::move(HTTP_RAW_POST_DATA));
    }
  };

  // Env
  PrepareEnv(ENVarr, transport);

  // Get
  if (!r.queryString().empty()) {
    PrepareGetVariable(GETarr, r);
  }

  // Post
  PreparePostVariables(POSTarr, HTTP_RAW_POST_DATA,
                        FILESarr, transport, r);

  // Server
  init_server_request_time(SERVERarr);
  PrepareServerVariable(SERVERarr, transport, r, vhost);
}

void HttpProtocol::PrepareGetVariable(Array& get,
                                      const RequestURI &r) {
  DecodeParameters(get,
                   r.queryString().data(),
                   r.queryString().size());
}

void HttpProtocol::PreparePostVariables(Array& post,
                                        Variant& raw_post,
                                        Array& files,
                                        Transport *transport,
                                        const RequestURI& r) {
  if (!transport->shouldReadPostData()) {
    return;
  }

  std::string contentType = transport->getHeader("Content-Type");
  std::string contentLength = transport->getHeader("Content-Length");

  bool needDelete = false;
  size_t size = 0;
  const void *data = transport->getPostData(size);
  if (data && size) {
    std::string boundary;
    auto content_length = strtoll(contentLength.c_str(), nullptr, 10);
    bool rfc1867Post = IsRfc1867(contentType, boundary);
    std::string files_str;
    if (rfc1867Post) {
      if (content_length < 0 ||
          content_length > VirtualHost::GetMaxPostSize()) {
        // $_POST and $_FILES are empty
        Logger::Warning("POST Content-Length of %lld bytes exceeds "
                        "the limit of %" PRId64 " bytes",
                        content_length, VirtualHost::GetMaxPostSize());
        while (transport->hasMorePostData()) {
          size_t delta = 0;
          transport->getMorePostData(delta);
        }
        data = nullptr;
        size = 0;
      } else {
        // content_length is a reasonable nonnegative size.
        bool invalidate = false;
        if (transport->hasMorePostData()) {
          // Calls to getMorePostData may invalidate data, so make a copy
          // iff we're trying to coalesce the entire POST body.  Otherwise,
          // data may be invalid when DecodeRfc1867 returns.  See
          // upload.cpp:read_post.
          if (Cfg::Server::AlwaysPopulateRawPostData) {
            needDelete = true;
            data = buffer_duplicate(data, size);
          } else {
            invalidate = true;
          }
        }
        DecodeRfc1867(transport,
                      post,
                      files,
                      content_length,
                      data,
                      size,
                      boundary);
        if (invalidate) {
          data = nullptr;
          size = 0;
        }
      }
      assertx(!transport->getFiles(files_str));
    } else {
      needDelete = read_all_post_data(transport, data, size);

      bool decodeData = strncasecmp(contentType.c_str(),
                                    DEFAULT_POST_CONTENT_TYPE,
                                    sizeof(DEFAULT_POST_CONTENT_TYPE)-1) == 0;

      if (!decodeData) {
        auto vhost = VirtualHost::GetCurrent();
        if (vhost && vhost->alwaysDecodePostData(r.originalURL())) {
          decodeData = true;
        }
      }

      if (decodeData) {
        DecodeParameters(post, (const char*)data, size, true);
      }

      bool ret = transport->getFiles(files_str);
      if (ret) {
        files = unserialize_from_string(
          files_str,
          VariableUnserializer::Type::Serialize
        );
      }
    }

    if (!data) {
      return;
    }
    if (size > StringData::MaxSize) {
      // Can't store it anywhere
      if (needDelete) {
        free((void*) data);
      }
    } else {
      auto string_data = needDelete ?
        OptString((char*)data, size, AttachString) :
        OptString((char*)data, size, CopyString);
      g_context->setRawPostData(string_data);
      if (Cfg::Server::AlwaysPopulateRawPostData || ! needDelete) {
        // For literal we disregard Cfg::Server::AlwaysPopulateRawPostData
        raw_post = string_data;
      }
    }
  }
}

static void CopyHeaderVariables(Array& server,
                                const HeaderMap& headers) {
  static std::atomic<int> badRequests(-1);

  std::vector<std::string> badHeaders;
  for (auto const& header : headers) {
    auto const& key = header.first;
    auto const& values = header.second;
    auto normalizedKey = s_HTTP_ +
                         string_replace(HHVM_FN(strtoupper)(key), s_dash,
                                        s_underscore);

    // Detect suspicious headers.  We are about to modify header names for
    // the SERVER variable.  This means that it is possible to deliberately
    // cause a header collision, which an attacker could use to sneak a
    // header past a proxy that would either overwrite or filter it
    // otherwise.  Client code should use apache_request_headers() to
    // retrieve the original headers if they are security-critical.
    if (Cfg::Log::HeaderMangle != 0 && server.exists(normalizedKey)) {
      badHeaders.push_back(key);
    }

    if (!values.empty()) {
      // When a header has multiple values, we always take the last one.
      server.set(normalizedKey, OptString(values.back()));
    }
  }

  if (!badHeaders.empty()) {
    auto reqId = badRequests.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!(reqId % Cfg::Log::HeaderMangle)) {
      std::string badNames = folly::join(", ", badHeaders);
      std::string allHeaders;

      const char* separator = "";
      for (auto const& header : headers) {
        for (auto const& value : header.second) {
          folly::toAppend(separator, header.first, ": ", value,
                          &allHeaders);
          separator = "\n";
        }
      }

      Logger::Warning(
        "HeaderMangle warning: "
        "The header(s) [%s] overwrote other headers which mapped to the same "
        "key. This happens because PHP normalises - to _, ie AN_EXAMPLE "
        "and AN-EXAMPLE are equivalent. You should treat this as "
        "malicious.",
        badNames.c_str());
    }
  }
}

static void CopyTransportParams(Array& server, Transport* transport) {
  HeaderMap transportParams;
  // Get additional server params from the transport if it has any. In the case
  // of fastcgi this is basically a full header list from apache/nginx.
  transport->getTransportParams(transportParams);
  for (auto const& header : transportParams) {
    // These overwrite anything already set in the $_SERVER
    // When a header has multiple values, we always take the last one.
    server.set(OptString(header.first), OptString(header.second.back()));
  }
}

static void CopyServerInfo(Array& server,
                           Transport *transport,
                           const VirtualHost *vhost) {

  std::string hostHeader = transport->getHeader("Host");
  OptString hostName(vhost->serverName(hostHeader));
  OptString serverNameHeader(transport->getServerName());
  if (hostHeader.empty()) {
    server.set(s_HTTP_HOST, hostName);
    StackTraceNoHeap::AddExtraLogging("Server", hostName.data());
  } else {
    // reset the HTTP_HOST header from apache.
    server.set(s_HTTP_HOST, hostHeader);
    StackTraceNoHeap::AddExtraLogging("Server", hostHeader.c_str());
  }

  // Use the header from the transport if it is available
  if (!serverNameHeader.empty()) {
    hostName = serverNameHeader;
  } else if (hostName.empty() || Cfg::Server::ForceServerNameToHeader) {
    hostName = hostHeader;
  }

  // _SERVER['SERVER_NAME'] shouldn't contain the port number
  int colonPos = hostName.find(':');
  if (colonPos != OptString::npos) {
    hostName = hostName.substr(0, colonPos);
  }

  StackTraceNoHeap::AddExtraLogging("Server_SERVER_NAME", hostName.data());

  server.set(s_GATEWAY_INTERFACE, s_CGI_1_1);
  server.set(s_SERVER_ADDR, transport->getServerAddr());
  server.set(s_SERVER_NAME, hostName);
  server.set(s_SERVER_PORT, transport->getServerPort());
  server.set(s_SERVER_SOFTWARE, transport->getServerSoftware());
  server.set(s_SERVER_PROTOCOL, "HTTP/" + transport->getHTTPVersion());
  server.set(s_SERVER_ADMIN, empty_string_tv());
  server.set(s_SERVER_SIGNATURE, empty_string_tv());
}

static void CopyRemoteInfo(Array& server, Transport *transport) {
  OptString remoteAddr(transport->getRemoteAddr(), CopyString);
  OptString remoteHost(transport->getRemoteHost(), CopyString);
  if (remoteAddr.empty()) {
    remoteAddr = remoteHost;
  }
  // Always set remoteAddr, even if it is empty
  server.set(s_REMOTE_ADDR, remoteAddr);
  if (!remoteHost.empty()) {
    server.set(s_REMOTE_HOST, remoteHost);
  }
  server.set(s_REMOTE_PORT, transport->getRemotePort());
}

static void CopyAuthInfo(Array& server, Transport *transport) {
  // APE processes Authorization: Basic into PHP_AUTH_USER and PHP_AUTH_PW
  std::string authorization = transport->getHeader("Authorization");
  if (!authorization.empty()) {
    if (strncmp(authorization.c_str(), "Basic ", 6) == 0) {
      // it's safe to pass this as a string literal since authorization
      // outlives decodedAuth; this saves us a superfluous copy.
      OptString decodedAuth =
        StringUtil::Base64Decode(OptString(authorization.c_str() + 6));
      int colonPos = decodedAuth.find(':');
      if (colonPos != OptString::npos) {
        server.set(s_PHP_AUTH_USER, decodedAuth.substr(0, colonPos));
        server.set(s_PHP_AUTH_PW, decodedAuth.substr(colonPos + 1));
      }
    } else if (strncmp(authorization.c_str(), "Digest ", 7) == 0) {
      server.set(s_PHP_AUTH_DIGEST, OptString(authorization.c_str() + 7));
    }
  }
}

static void CopyPathInfo(Array& server,
                         Transport *transport,
                         const RequestURI& r,
                         const VirtualHost *vhost) {
  server.set(s_REQUEST_URI, OptString(transport->getUrl(), CopyString));
  server.set(s_SCRIPT_URL, r.originalURL());
  OptString prefix(transport->isSSL() ? "https://" : "http://");

  // Need to append port
  assertx(server.exists(s_SERVER_PORT));
  std::string serverPort = "80";
  if (server.exists(s_SERVER_PORT)) {
    Variant port = server[s_SERVER_PORT];
    always_assert(port.isInteger() || port.isString());
    if (port.isInteger()) {
      serverPort = folly::to<std::string>((int)port.toInt64());
    } else {
      serverPort = port.toString().data();
    }
  }

  OptString port_suffix("");
  if (!transport->isSSL() && serverPort != "80") {
    port_suffix = fmt::format(":{}", serverPort);
  }

  std::string hostHeader;
  if (server.exists(s_HTTP_HOST)) {
    hostHeader = server[s_HTTP_HOST].asCStrRef().data();
  }
  OptString hostName;
  if (server.exists(s_SERVER_NAME)) {
    assertx(server[s_SERVER_NAME].isString());
    hostName = server[s_SERVER_NAME].asCStrRef();
  }
  server.set(s_SCRIPT_URI,
             OptString(prefix + (hostHeader.empty() ? hostName + port_suffix :
                                 OptString(hostHeader)) + r.originalURL()));

  if (r.rewritten()) {
    // when URL is rewritten, PHP decided to put original URL as SCRIPT_NAME
    OptString name = r.originalURL();
    if (!r.pathInfo().empty()) {
      int pos = name.find(r.pathInfo());
      if (pos >= 0) {
        name = name.substr(0, pos);
      }
    }
    if (r.globalDoc()) {
      name = OptString(Cfg::Server::GlobalDocument);
    }
    if (r.defaultDoc()) {
      if (!name.empty() && name[name.length() - 1] != '/') {
        name += "/";
      }
      name += OptString(Cfg::Server::DefaultDocument);
    }
    server.set(s_SCRIPT_NAME, name);
  } else {
    server.set(s_SCRIPT_NAME, r.resolvedURL());
  }

  if (r.rewritten()) {
    server.set(s_PHP_SELF, r.originalURL());
  } else {
    server.set(s_PHP_SELF, r.resolvedURL() + r.origPathInfo());
  }

  OptString documentRoot = transport->getDocumentRoot();
  if (documentRoot.empty()) {
    // Right now this is just Cfg::Server::SourceRoot but mwilliams wants to
    // fix it so it is settable, so I'll leave this for now
    documentRoot = vhost->getDocumentRoot();
  }
  if (documentRoot != s_forwardslash &&
      documentRoot[documentRoot.length() - 1] == '/') {
    documentRoot = documentRoot.substr(0, documentRoot.length() - 1);
  }
  server.set(s_DOCUMENT_ROOT, documentRoot);
  server.set(s_SCRIPT_FILENAME, r.absolutePath());

  if (r.pathInfo().empty()) {
    server.set(s_PATH_TRANSLATED, r.absolutePath());
  } else {
    assertx(server.exists(s_DOCUMENT_ROOT));
    assertx(server[s_DOCUMENT_ROOT].isString());
    // reset path_translated back to the transport if it has it.
    auto const& pathTranslated = transport->getPathTranslated();
    if (!pathTranslated.empty()) {
      if (documentRoot == s_forwardslash) {
        // path outside document root or / is document root
        server.set(s_PATH_TRANSLATED, OptString(pathTranslated));
      } else {
        server.set(s_PATH_TRANSLATED,
                   OptString(server[s_DOCUMENT_ROOT].asCStrRef() +
                             s_forwardslash + pathTranslated));
      }
    } else {
      server.set(s_PATH_TRANSLATED,
                 OptString(server[s_DOCUMENT_ROOT].asCStrRef() +
                           server[s_SCRIPT_NAME].asCStrRef() +
                           r.pathInfo().data()));
    }
    server.set(s_PATH_INFO, r.pathInfo());
  }

  switch (transport->getMethod()) {
  case Transport::Method::GET:  server.set(s_REQUEST_METHOD, s_GET);  break;
  case Transport::Method::HEAD: server.set(s_REQUEST_METHOD, s_HEAD); break;
  case Transport::Method::POST:
    if (transport->getExtendedMethod() == nullptr) {
      server.set(s_REQUEST_METHOD, s_POST);
    } else {
      server.set(s_REQUEST_METHOD, transport->getExtendedMethod());
    }
    break;
  default:
    server.set(s_REQUEST_METHOD, empty_string_tv()); break;
  }
  if (transport->isSSL()) {
    server.set(s_HTTPS, s_on);
  } else {
    server.set(s_HTTPS, empty_string_tv());
  }
  server.set(s_QUERY_STRING, r.queryString());

  server.set(s_argv, make_vec_array(r.queryString()));
  server.set(s_argc, 1);
}

void HttpProtocol::PrepareServerVariable(Array& server,
                                         Transport *transport,
                                         const RequestURI &r,
                                         const VirtualHost *vhost) {
  // $_SERVER

  std::string contentType = transport->getHeader("Content-Type");
  std::string contentLength = transport->getHeader("Content-Length");

  if (Cfg::Eval::SetHeadersInServerSuperGlobal) {
    // HTTP_ headers -- we don't exclude headers we handle elsewhere (e.g.,
    // Content-Type, Authorization), since the CGI "spec" merely says the server
    // "may" exclude them; this is not what APE does, but it's harmless.
    auto const& headers = transport->getHeaders();
    // Do this first so other methods can overwrite them
    CopyHeaderVariables(server, headers);
  }
  CopyServerInfo(server, transport, vhost);
  // Do this last so it can overwrite all the previous settings
  CopyTransportParams(server, transport);
  CopyRemoteInfo(server, transport);
  CopyAuthInfo(server, transport);
  CopyPathInfo(server, transport, r, vhost);

  // APE sets CONTENT_TYPE and CONTENT_LENGTH without HTTP_
  if (!contentType.empty()) {
    server.set(s_CONTENT_TYPE, OptString(contentType));
  }
  if (!contentLength.empty()) {
    server.set(s_CONTENT_LENGTH, OptString(contentLength));
  }

  for (auto& kv : RuntimeOption::ServerVariables) {
    OptString idx(kv.first);
    const auto arrkey =
      server.convertKey<IntishCast::Cast>(idx);
    OptString str(kv.second);
    server.set(arrkey, make_tv<KindOfString>(str.get()), true);
  }
  for (auto& kv : vhost->getServerVars()) {
    OptString idx(kv.first);
    const auto arrkey =
      server.convertKey<IntishCast::Cast>(idx);
    OptString str(kv.second);
    server.set(arrkey, make_tv<KindOfString>(str.get()), true);
  }
  server = SourceRootInfo::SetServerVariables(std::move(server));

  const char *threadType = transport->getThreadTypeName();
  server.set(s_THREAD_TYPE, threadType);
  StackTraceNoHeap::AddExtraLogging("ThreadType", threadType);
}

std::string HttpProtocol::RecordRequest(Transport *transport) {
  char tmpfile[PATH_MAX + 1];
  if (Cfg::Debug::RecordInput) {
    strcpy(tmpfile, "/tmp/hphp_request_XXXXXX");
    close(mkstemp(tmpfile));

    ReplayTransport rt;
    rt.recordInput(transport, tmpfile);
    Logger::Info("request recorded in %s", tmpfile);
    return tmpfile;
  }
  return "";
}

void HttpProtocol::ClearRecord(bool success, const std::string &tmpfile) {
  if (success && Cfg::Debug::ClearInputOnSuccess && !tmpfile.empty()) {
    unlink(tmpfile.c_str());
    Logger::Info("request %s deleted", tmpfile.c_str());
  }
}

///////////////////////////////////////////////////////////////////////////////

void HttpProtocol::DecodeParameters(Array& variables, const char *data,
                                    size_t size, bool post /* = false */) {
  if (data == nullptr || size == 0) {
    return;
  }

  const char *s = data;
  const char *e = s + size;
  const char *p, *val;

  while (s < e && (p = (const char *)memchr(s, '&', (e - s)))) {
  last_value:
    if ((val = (const char *)memchr(s, '=', (p - s)))) {
      OptString sname = url_decode(s, val - s);

      val++;
      OptString value = url_decode(val, p - val);

      register_variable(variables, (char*)sname.data(), value);
    } else if (!post) {
      OptString sname = url_decode(s, p - s);
      register_variable(variables, (char*)sname.data(), Variant{String::Empty()});
    }
    s = p + 1;
  }
  if (s < e) {
    p = e;
    goto last_value;
  }
}

void HttpProtocol::DecodeCookies(Array& variables, char *data) {
  assertx(data && *data);

  char *strtok_buf = nullptr;
  char *var = strtok_r(data, ";", &strtok_buf);
  while (var) {
    char *val = strchr(var, '=');

    // Remove leading spaces from cookie names, needed for multi-cookie
    // header where ; can be followed by a space */
    while (isspace(*var)) {
      var++;
    }

    if (var != val && *var != '\0') {
      if (val) { /* have a value */
        OptString sname = url_decode(var, val - var);

        ++val;
        OptString value = url_decode(val, strlen(val));

        register_variable(variables, (char*)sname.data(), value, false);
      } else {
        OptString sname = url_decode(var, strlen(var));

        register_variable(variables, (char*)sname.data(),
                          Variant{String::Empty()}, false);
      }
    }

    var = strtok_r(nullptr, ";", &strtok_buf);
  }
}

bool HttpProtocol::IsRfc1867(const std::string contentType, std::string &boundary) {
  if (contentType.empty()) return false;
  const char *ctstr = contentType.c_str();
  char *s;
  char *e;
  for (s = (char*)ctstr; *s && !(*s == ';' || *s == ',' || *s == ' '); s++) ;
  if (strncasecmp(ctstr, MULTIPART_CONTENT_TYPE, s - ctstr)) {
    return false;
  }
  s = strstr(s, "boundary");
  if (!s || !(s=strchr(s, '='))) {
    Logger::Warning("Missing boundary in multipart/form-data POST data");
    return false;
  }
  s++;
  if (s[0] == '"') {
    s++;
    e = strchr(s, '"');
    if (!e) {
      Logger::Warning("Invalid boundary in multipart/form-data POST data");
      return false;
    }
  } else {
    /* search for the end of the boundary */
    e = strpbrk(s, ",;");
  }
  if (e) {
    e[0] = '\0';
  }
  boundary = s;
  return true;
}

void HttpProtocol::DecodeRfc1867(Transport *transport,
                                 Array& post,
                                 Array& files,
                                 size_t contentLength,
                                 const void *&data,
                                 size_t &size,
                                 std::string boundary) {
  rfc1867PostHandler(transport,
                     post,
                     files,
                     contentLength,
                     data,
                     size,
                     boundary);
}

const char *HttpProtocol::GetReasonString(int code) {
  // https://tools.ietf.org/html/rfc7231#section-6.1
  switch (code) {
  case 100: return "Continue";
  case 101: return "Switching Protocols";
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 203: return "Non-Authoritative Information";
  case 204: return "No Content";
  case 205: return "Reset Content";
  case 206: return "Partial Content";
  case 300: return "Multiple Choices";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 303: return "See Other";
  case 304: return "Not Modified";
  case 305: return "Use Proxy";
  case 307: return "Temporary Redirect";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 402: return "Payment Required";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 406: return "Not Acceptable";
  case 407: return "Proxy Authentication Required";
  case 408: return "Request Timeout";
  case 409: return "Conflict";
  case 410: return "Gone";
  case 411: return "Length Required";
  case 412: return "Precondition Failed";
  case 413: return "Payload Too Large";
  case 414: return "URI Too Long";
  case 415: return "Unsupported Media Type";
  case 416: return "Range Not Satisfiable";
  case 417: return "Expectation Failed";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 505: return "HTTP Version Not Supported";
  }
  return "";
}

///////////////////////////////////////////////////////////////////////////////

bool HttpProtocol::ProxyRequest(Transport *transport, bool force,
                                const std::string &url,
                                int &code, std::string &error,
                                StringBuffer &response,
                                HeaderMap *extraHeaders /* = NULL */) {
  assertx(transport);
  if (transport->headersSent()) {
    raise_warning("Cannot proxy request - headers already sent");
    return false;
  }

  auto requestHeaders = transport->getHeaders();
  if (extraHeaders) {
    for (HeaderMap::const_iterator iter = extraHeaders->begin();
         iter != extraHeaders->end(); ++iter) {
      auto& values = requestHeaders[iter->first];
      for (auto& h : iter->second) {
        values.push_back(h);
      }
    }
  }

  size_t size = 0;
  const char *data = nullptr;
  if (transport->getMethod() == Transport::Method::POST) {
    data = (const char *)transport->getPostData(size);
  }

  req::vector<OptString> responseHeaders;
  HttpClient http;
  code = http.request(transport->getMethodName(),
                      url.c_str(), data, size, response, &requestHeaders,
                      &responseHeaders);

  if (code == 0) {
    if (!force) return false; // so we can retry
    Logger::Error("Unable to proxy %s: %s", url.c_str(),
                  http.getLastError().c_str());
    error = http.getLastError();
    return true;
  }

  for (unsigned int i = 0; i < responseHeaders.size(); i++) {
    OptString &header = responseHeaders[i];
    if (header.find(":") != OptString::npos &&
        header.find("Content-Length: ") != 0 &&
        header.find("Client-Transfer-Encoding: ") != 0 &&
        header.find("Transfer-Encoding: ") != 0 &&
        header.find("Connection: ") != 0) {
      transport->addHeader(header.data());
    }
  }
  const char* respData = response.data();
  if (!respData) {
    respData = "";
  }
  Logger::Verbose("Response code was %d when proxying %s", code, url.c_str());
  return true;
}

///////////////////////////////////////////////////////////////////////////////
}
