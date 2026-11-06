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
#include "hphp/runtime/server/replay-transport.h"
#include <folly/Conv.h>

#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/base/config.h"
#include "hphp/runtime/server/http-protocol.h"
#include "hphp/util/process.h"
#include "hphp/util/safe-cast.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

void ReplayTransport::recordInput(Transport* transport, const char *filename) {
  assertx(transport);

  Hdf hdf;

  char buf[32];
  snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)getpid());
  hdf["pid"] = std::string(buf);
  snprintf(buf, sizeof(buf), "%" PRIx64, (int64_t)Process::GetThreadId());
  hdf["tid"] = std::string(buf);
  snprintf(buf, sizeof(buf), "%u", Process::GetThreadPid());
  hdf["tpid"] = std::string(buf);

  hdf["cmd"] = static_cast<int>(transport->getMethod());
  hdf["url"] = transport->getUrl();
  hdf["https"] = transport->isSSL();
  hdf["remote_host"] = transport->getRemoteHost();
  hdf["remote_port"] = transport->getRemotePort();

  auto const& headers = transport->getHeaders();
  for (auto const& pair : headers) {
    m_requestHeaders[pair.first] = pair.second;
  }
  int index = 0;
  for (HeaderMap::const_iterator iter = m_requestHeaders.begin();
       iter != m_requestHeaders.end(); ++iter) {
    for (unsigned int i = 0; i < iter->second.size(); i++) {
      Hdf header = hdf["headers"][index++];
      header["name"] = iter->first;
      header["value"] = iter->second[i];
    }
  }

  size_t size;
  const void *data = transport->getPostData(size);
  if (size) {
    OptString encoded = string_uuencode((const char *)data, safe_cast<int>(size));
    hdf["post"] = encoded.get()->data();
  } else {
    hdf["post"] = "";
  }

  hdf.write(filename);
}

void ReplayTransport::replayInput(const char *filename) {
  std::string fname = filename;
  Config::ParseConfigFile(fname, m_ini, m_hdf);
  replayInputImpl();
}

void ReplayTransport::replayInput(Hdf hdf) {
  m_hdf.assign(hdf);
  replayInputImpl();
}

void ReplayTransport::replayInputImpl() {
  m_isSSL = Config::GetBool(m_ini, m_hdf, "https", false, false);
  OptString postData = StringUtil::UUDecode(Config::GetString(m_ini, m_hdf, "post",
                                                              "", false));
  m_postData = std::string(postData.data(), postData.size());
  m_requestHeaders.clear();
  auto headers_callback = [&](const IniSetting::Map& ini_h, const Hdf& hdf_h,
                              const std::string& /*ini_h_key*/) {
    m_requestHeaders[Config::GetString(ini_h, hdf_h, "name",
                                       "", false)].push_back(
      Config::GetString(ini_h, hdf_h, "value", "", false)
    );
  };
  Config::Iterate(headers_callback, m_ini, m_hdf, "headers", false);
}

const char *ReplayTransport::getUrl() {
  return Config::Get(m_ini, m_hdf, "url", "", false);
}

const char *ReplayTransport::getRemoteHost() {
  return Config::Get(m_ini, m_hdf, "remote_host", "", false);
}
uint16_t ReplayTransport::getRemotePort() {
  return Config::GetUInt16(m_ini, m_hdf, "remote_port", 0, false);
}

const void *ReplayTransport::getPostData(size_t &size) {
  size = m_postData.size();
  return m_postData.data();
}

Transport::Method ReplayTransport::getMethod() {
  return (Transport::Method)Config::GetInt32(m_ini, m_hdf, "cmd", false);
}

std::string ReplayTransport::getHeader(const char *name) {
  assertx(name);
  if (m_requestHeaders.find(name) != m_requestHeaders.end()) {
    assertx(!m_requestHeaders[name].empty());
    return m_requestHeaders[name][0];
  }
  return "";
}

const HeaderMap& ReplayTransport::getHeaders() {
  return m_requestHeaders;
}

void ReplayTransport::addHeaderImpl(const char *name, const char *value) {
  assertx(name && value);
  m_responseHeaders[name].push_back(value);
}

void ReplayTransport::removeHeaderImpl(const char *name) {
  assertx(name);
  m_responseHeaders.erase(name);
}

void ReplayTransport::sendImpl(const void* data, int size, int code,
                               bool /*chunked*/, bool eom) {
  m_code = code;

  m_response = "HTTP/1.1 ";
  m_response += folly::to<std::string>(code);
  m_response += " ";
  m_response += HttpProtocol::GetReasonString(m_code);
  m_response += "\r\n";

  for (HeaderMap::const_iterator iter = m_responseHeaders.begin();
       iter != m_responseHeaders.end(); ++iter) {
    for (unsigned int i = 0; i < iter->second.size(); i++) {
      m_response += iter->first;
      m_response += ": ";
      m_response += iter->second[i];
      m_response += "\r\n";
    }
  }

  m_response += "\r\n";
  m_response.append((const char *)data, size);
  m_response += "\r\n";
  if (eom) {
    onSendEndImpl();
  }
}

///////////////////////////////////////////////////////////////////////////////
}
