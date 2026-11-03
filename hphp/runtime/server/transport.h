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

#pragma once

#include "hphp/runtime/base/debuggable.h"
#include "hphp/runtime/base/request-id.h"
#include "hphp/runtime/base/request-tracing.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/util/functional.h"
#include "hphp/util/gzip.h"
#include "hphp/util/string-holder.h"
#include "hphp/util/tiny-vector.h"

#include "proxygen/lib/http/HTTPHeaders.h"

#include <chrono>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Array;
struct Variant;
struct ResponseCompressorManager;
namespace stream_transport {
struct StreamTransport;
}
struct StructuredLogEntry;

using HeaderMap = hphp_fast_string_imap<TinyVector<std::string>>;
using CookieMap = hphp_fast_string_map<std::string>;

struct ITransportHeaders {
  enum class Method {
    Unknown,
    GET,
    POST,
    HEAD,
    AUTO, // check GET parameter first, then POST
  };
  /* Request header methods */
  virtual const char *getUrl() = 0;
  virtual std::string getCommand() = 0; // URL with params stripped
  virtual std::string getHeader(const char *name) = 0;
  virtual const HeaderMap& getHeaders() = 0;
  virtual const proxygen::HTTPHeaders* getProxygenHeaders() { return nullptr; }
  virtual Method getMethod() = 0;
  virtual const char *getMethodName() = 0;
  virtual const void *getPostData(size_t &size) = 0;

  /* Response header methods */
  virtual void addHeaderNoLock(const char *name, const char *value) = 0;
  virtual void addHeader(const char *name, const char *value) = 0;
  virtual void addHeader(const OptString& header) = 0;
  virtual void replaceHeader(const char *name, const char *value) = 0;
  virtual void replaceHeader(const OptString& header) = 0;
  virtual void removeHeader(const char *name) = 0;
  virtual void removeAllHeaders() = 0;
  virtual void getResponseHeaders(HeaderMap& headers) = 0;
  virtual void addToCommaSeparatedHeader(
      const char* name, const char* value) = 0;
};

/**
 * A class defining an interface that request handler can use to query
 * transport related information.
 *
 * Note that one transport object is created for each request, and
 * one transport is ONLY accessed from one single thread.
 */
struct Transport : IDebuggable, ITransportHeaders {
  using Method = ITransportHeaders::Method;

  // TODO: add all status codes
  // (http://www.w3.org/Protocols/rfc2616/rfc2616-sec10.html)
  enum class StatusCode {
    Unknown,

    // Success
    OK = 200,

    // Redirection
    MOVED_PERMANENTLY = 301,

    // Client Error
    BAD_REQUEST = 400,
    FORBIDDEN = 403,
    NOT_FOUND = 404,

    // Server Error
    INTERNAL_SERVER_ERROR = 500,
    SERVICE_UNAVAILABLE = 503,
  };

  enum class ThreadType {
    RequestThread,
    PageletThread,
    XboxThread,
    RpcThread,
  };

public:
  Transport();
  ~Transport() override;

  void onRequestStart(const timespec &queueTime);
  const timespec &getQueueTime() const { return m_queueTime; }
  const std::chrono::steady_clock::time_point getQueueTimePoint() const {
    return timespecToSteadyTimePoint(m_queueTime);
  }
  const timespec &getWallTime() const { return m_wallTime; }
  const std::chrono::steady_clock::time_point getWallTimePoint() const {
    return timespecToSteadyTimePoint(m_wallTime);
  }
  const timespec &getCpuTime() const { return m_cpuTime; }
  const std::chrono::steady_clock::time_point getCpuTimePoint() const {
    return timespecToSteadyTimePoint(m_cpuTime);
  }
  const int64_t &getInstructions() const { return m_instructions; }
  const int64_t &getSleepTime() const { return m_sleepTime; }
  void incSleepTime(unsigned int seconds) { m_sleepTime += seconds; }
  const int64_t &getuSleepTime() const { return m_usleepTime; }
  void incuSleepTime(unsigned int useconds) { m_usleepTime += useconds; }
  const int64_t &getnSleepTimeS() const { return m_nsleepTimeS; }
  const int32_t &getnSleepTimeN() const { return m_nsleepTimeN; }
  void incnSleepTime(unsigned long int seconds, unsigned int nseconds) {
    m_nsleepTimeN += nseconds;
    m_nsleepTimeS += seconds + (m_nsleepTimeN / 1000000000);
    m_nsleepTimeN %= 1000000000;
  }

  void forceInitRequestTrace();
  rqtrace::Trace* getRequestTrace() { return m_requestTrace.get_pointer(); }
  StructuredLogEntry* createStructuredLogEntry();
  StructuredLogEntry* getStructuredLogEntry();
  void resetStructuredLogEntry();

  /*
   * Manage root request id.
   */
  RequestId getRootRequestId() const { return m_root_req_id; }
  void setRootRequestId(RequestId id) { m_root_req_id = id; }

  ///////////////////////////////////////////////////////////////////////////
  // Functions sub-classes have to implement.

  /**
   * Request URI.
   */
  virtual const char *getUrl() override = 0;
  virtual const char *getRemoteHost() = 0;
  virtual uint16_t getRemotePort() = 0;
  // The transport can override REMOTE_ADDR if it has one
  virtual const char *getRemoteAddr() { return ""; }
  // The transport can override the virtualhosts' docroot
  virtual const std::string getDocumentRoot() { return ""; }
  // The transport can say exactly what script to use
  virtual const std::string getScriptFilename() { return ""; }
  virtual const std::string getPathTranslated() { return ""; }
  virtual const std::string getPathInfo() { return ""; }
  virtual bool isPathInfoSet() {return false; }

  /**
   * Server Headers
   */
  virtual const char *getServerName() {
    return "";
  }
  virtual const std::string& getServerAddr() {
    auto const& ipv4 = RuntimeOption::GetServerPrimaryIPv4();
    return ipv4.empty() ? RuntimeOption::GetServerPrimaryIPv6() : ipv4;
  }
  virtual uint16_t getServerPort() {
    return Cfg::Server::Port;
  }
  virtual const char *getServerSoftware() {
    return "HPHP";
  }

  /**
   * Get stream transport if it has one, otherwise returns nullptr
   */
  virtual std::shared_ptr<stream_transport::StreamTransport>
  getStreamTransport() const {
    return nullptr;
  }
  /**
   * Is this a streaming transport?
   */
  virtual bool isStreamTransport() const {
    return false;
  }

  /**
   * Returns true if post data should be read.
   */
  virtual bool shouldReadPostData() {
    return getMethod() == Transport::Method::POST &&
      !isStreamTransport();
  }

  /**
   * POST request's data.
   */
  virtual const void *getPostData(size_t &size) override = 0;
  virtual bool hasMorePostData() { return false; }
  virtual const void *getMorePostData(size_t &size) { size = 0;return nullptr; }
  virtual bool getFiles(std::string& /*files*/) { return false; }

  /**
   * This callback should be called when the StreamTransport is ready
   * to receive data.
   * This is relevant only for transports that have a StreamTransport.
   */
  virtual void onStreamReady() {}

  /**
   * Is this a GET, POST or anything?
   */
  virtual Method getMethod() override = 0;
  virtual const char *getExtendedMethod() { return nullptr;}
  const char *getMethodName() override;

  /**
   * What version of HTTP was the request?
   */
  virtual std::string getHTTPVersion() const;

  /**
   * Get http request size.
   */
  virtual size_t getRequestSize() const;

  /**
   * Get transport params.
   */
  virtual void getTransportParams(HeaderMap& /*serverParams*/){}

  /**
   * Get a description of the type of transport.
   */
  virtual OptString describe() const = 0;

  /**
   * Get/set response headers.
   */
  void addHeaderNoLock(const char *name, const char *value) override;
  void addHeader(const char *name, const char *value) override;
  void addHeader(const OptString& header) override;
  void replaceHeader(const char *name, const char *value) override;
  void replaceHeader(const OptString& header) override;
  void removeHeader(const char *name) override;
  void removeAllHeaders() override;
  void getResponseHeaders(HeaderMap &headers) override;
  std::string getFirstHeaderFile() const { return m_firstHeaderFile;}
  int getFirstHeaderLine() const { return m_firstHeaderLine;}
  // Appends value to the response header field, separated with a ", "
  // If the value doesn't exist in m_responseHeaders[name] then, we add it
  void addToCommaSeparatedHeader(const char* name, const char* value) override;

  /**
   * Content/MIME type related functions.
   */
  void setMimeType(const OptString& mimeType);
  OptString getMimeType();
  bool getUseDefaultContentType() { return m_sendContentType;}
  void setUseDefaultContentType(bool send) { m_sendContentType = send;}

  /**
   * Can we compress response?
   */
  void enableCompression();
  void disableCompression();
  bool isCompressionEnabled();

  /**
   * Set cookie response header.
   */
  bool setCookie(const OptString& name, const OptString& value, int64_t expire = 0,
                 const OptString& path = "", const OptString& domain = "",
                 bool secure = false, bool httponly = false,
                 bool encode_url = true);

  /**
   * Add/remove a response header.
   */
  virtual void addHeaderImpl(const char *name, const char *value) = 0;
  virtual void removeHeaderImpl(const char *name) = 0;

  /**
   * Add/remove a request header. Default is no-op, because not all transports
   * need to support incoming request header manipulations.
   */
  virtual void
  addRequestHeaderImpl(const char* /*name*/, const char* /*value*/) {}
  virtual void removeRequestHeaderImpl(const char* /*name*/) {}

  /**
   * Called when all sending should be done by this time point. Designed for
   * sending last chunk of response for chunked encoding.
   */
  void onSendEnd();

  /**
   * Send back a response with specified code.
   * Caller deletes data, callee must copy
   */
  virtual void sendImpl(const void *data, int size, int code,
                        bool chunked, bool eom) = 0;

  /**
   * Override to implement more send end logic.
   */
  virtual void onSendEndImpl() {}

  /**
   * Need this implementation to break keep-alive connections.
   */
  virtual bool isServerStopping() { return false;}

  ///////////////////////////////////////////////////////////////////////////
  // Pre-implemented utitlity functions.

  /**
   * We define a "server object" as the part of URL without domain name:
   *
   *   http://facebook.com/foo?x=1       server object is "/foo?x=1"
   *   http://facebook.com/foo/bar?x=1   server object is "/foo/bar?x=1"
   */
  virtual const char *getServerObject();

  /**
   * We define a "command" as the part of URL without parameters:
   *
   *   /foo?x=1      command is "foo"
   *   foo?x=1       command is "foo"
   *   foo/bar?x=1   command is "foo/bar"
   *   /foo/bar?x=1  command is "foo/bar"
   */
  std::string getCommand() override;

  /**
   * Get value of a parameter. Returns empty string is not present.
   */
  std::string getParam(const char *name, Method method = Method::GET);

  /**
   * Turn a string parameter into an integer.
   */
  int getIntParam(const char *name, Method method = Method::GET);

  /**
   * Turn a string parameter into a 64-bit number.
   */
  long long getInt64Param(const char *name, Method method = Method::GET);

  /**
   * Collect multiple string parameters with the same name into "values".
   *
   *   /foo?x=1&x=2&x=3
   */
  void getArrayParam(const char *name, std::vector<std::string> &values,
                     Method method = Method::GET);

  /**
   * Split a string parameter into multiple sub-strings.
   *
   *  /foo?x=1:2:3
   */
  void getSplitParam(const char *name, std::vector<std::string> &values,
                     char delimiter, Method method = Method::GET);

  /**
   * Test whether client accepts a certain encoding.
   */
  bool acceptEncoding(const char *encoding);

  /**
   * Test whether cookie header has the "name=".
   */
  bool cookieExists(const char *name);

  /**
   * Get value of cookie "name"
   */
  std::string getCookie(const std::string &name);

  /**
   * Sending back a response.
   * These APIs should not be used for streaming treansports.
   */
  void setResponse(int code, const char *info = nullptr);
  const std::string &getResponseInfo() const { return m_responseCodeInfo; }
  bool headersSent() { return m_headerSent;}
  void sendRaw(const char *data, int size, int code = 200,
               bool precompressed = false, bool chunked = false,
               const char *codeInfo = nullptr);

  /**
   * Sending response for streaming transports.
   */
  virtual void sendStreamResponse(const void* /*data*/, int /*size*/) {}
  virtual void sendStreamEOM() {}
private:
  const std::chrono::steady_clock::time_point timespecToSteadyTimePoint(const  timespec &ts) const {
    auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}
    );
    return std::chrono::steady_clock::time_point(duration);
  }

  void sendRawInternal(const char *data, int size, int code = 200,
                       bool precompressed = false,
                       const char *codeInfo = nullptr);
public:
  void sendString(const char *data, int code = 200, bool precompressed = false,
                  bool chunked = false,
                  const char * codeInfo = nullptr) {
    sendRaw(data, strlen(data), code, precompressed, chunked, codeInfo);
  }
  void sendString(const std::string &data, int code = 200,
                  bool precompressed = false, bool chunked = false,
                  const char *codeInfo = nullptr) {
    sendRaw(data.c_str(), data.length(), code, precompressed, chunked,
            codeInfo);
  }
  void redirect(const char *location, int code, const char *info = nullptr);

  // TODO: support rfc1867
  virtual bool isUploadedFile(const OptString& filename);

  // For streaming transports, only getResponseSentSize is valid.
  int getResponseSize() const { return m_responseSize; }
  int getResponseTotalSize() const { return m_responseTotalSize; }
  int getResponseSentSize() const { return m_responseSentSize; }

  int getResponseCode() const { return m_responseCode; }
  int64_t getFlushTime() const { return m_flushTimeUs; }
  int getLastChunkSentSize();
  void getChunkSentSizes(Array &ret);
  void onFlushBegin(int totalSize) { m_responseTotalSize = totalSize; }
  void onFlushProgress(int writtenSize, int64_t delayUs);
  void onChunkedProgress(int writtenSize);

  void setThreadType(ThreadType type) { m_threadType = type;}
  ThreadType getThreadType() const { return m_threadType;}
  const char *getThreadTypeName() const;

  // implementing IDebuggable
  void debuggerInfo(InfoVec& info) override;

  void setSSL() {m_isSSL = true;}
  bool isSSL() const {return m_isSSL;}

  /*
   * Request to adjust the maximum number of threads on the server.
   */
  virtual void trySetMaxThreadCount(int max) {}

protected:
  /**
   * Parameter parsing in this class is done by making just one copy of the
   * entire query (either URL or post data), then insert termintaing NULLs
   * at end of tokens (name and value), url decode in-place and then store
   * token's start char * addresses in ParamMaps. Therefore, this entire
   * process is very efficient without excessive string copying.
   */
  using ParamMap = hphp_fast_map<const char*, TinyVector<const char*>,
                                 cstr_hash, eqstr>;

  // timers and other perf data
  timespec m_queueTime{};
  timespec m_wallTime{};
  timespec m_cpuTime{};

  int64_t m_instructions{};

  int64_t m_sleepTime{};
  int64_t m_usleepTime{};
  int64_t m_nsleepTimeS{};
  int32_t m_nsleepTimeN{};

  std::unique_ptr<StructuredLogEntry> m_structLogEntry;

  // input
  char *m_url{};
  char *m_postData{};
  ParamMap m_getParams;
  ParamMap m_postParams;
  bool m_postDataParsed{};

  // output
  bool m_chunkedEncoding{};
  bool m_headerSent{};
  bool m_firstHeaderSet{};
  bool m_sendEnded{};
  bool m_sendStarted{false};
  bool m_sendContentType{true};
  bool m_isSSL{};
  int m_responseCode{-1};
  std::string m_responseCodeInfo;
  HeaderMap m_responseHeaders;
  std::string m_firstHeaderFile;
  int m_firstHeaderLine{};
  int m_responseSize{};
  CookieMap m_responseCookies;
  int m_responseTotalSize{}; // including added headers
  int m_responseSentSize{};
  int64_t m_flushTimeUs{};

  std::vector<int> m_chunksSentSizes;

  std::string m_mimeType;

  std::unique_ptr<ResponseCompressorManager> m_compressor;

  ThreadType m_threadType{ThreadType::RequestThread};

  Optional<rqtrace::Trace> m_requestTrace;

  RequestId m_root_req_id;

  // helpers
  void parseGetParams();
  void parsePostParams();
  static void parseQuery(char *query, ParamMap &params);
  static void urlUnescape(char *value);
  bool splitHeader(const OptString& header, OptString &name, const char *&value);
  std::list<std::string> getCookieLines();

  ResponseCompressorManager& getCompressor();

private:
  StringHolder compressResponse(const char *data, int size, bool last);
  void prepareHeaders(bool precompressed, bool chunked,
    const StringHolder &response, const StringHolder& orig_response);
};

///////////////////////////////////////////////////////////////////////////////
}
