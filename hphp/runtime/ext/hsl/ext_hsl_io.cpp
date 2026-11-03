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
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/asio/asio-external-thread-event.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/request-body-reader.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <folly/ScopeGuard.h>

namespace HPHP {
namespace {
  Array HHVM_FUNCTION(HH_io_pipe) {
    int fds[2];
    if (::pipe(fds) != 0) {
      raise_error("Failed to pipe(2)");
    }
    return make_vec_array(
      req::make<PlainFile>(fds[0]),
      req::make<PlainFile>(fds[1])
    );
  }

  int64_t HHVM_FUNCTION(HH_io_response_write, const OptString& data) {
    if (is_any_cli_mode()) {
      auto stream = dyn_cast_or_null<File>(BuiltinFiles::getSTDOUT());
      if (!stream) {
        return 0;
      }
      // Per `File`'s comments:
      // - write() for PHP stream behavior
      // - writeImpl() for C-like behavior
      return stream->writeImpl(data.c_str(), data.length());
    }
    // Written length is not returned, assume it wrote all, like OutputFile
    g_context->write(data);
    return data.length();
  }

  void HHVM_FUNCTION(HH_io_response_flush) {
    g_context->flush();
  }

  RDS_LOCAL(ssize_t, request_read_offset);

  OptString HHVM_FUNCTION(HH_io_request_read, int64_t max) {
    if (is_any_cli_mode()) {
      auto stream = dyn_cast_or_null<File>(BuiltinFiles::getSTDIN());
      if (!stream) {
        return StrNR(staticEmptyString());
      }
      return stream->read(max);
    }

    const auto offset = *request_read_offset.get();
    const auto raw_post = g_context->getRawPostData();
    const auto remaining = raw_post.size() - offset;
    const auto to_read = max > remaining ? remaining : max;
    const auto result = raw_post.substr(offset, to_read);
    *request_read_offset.get() += to_read;
    return result;
  }

  std::shared_ptr<RequestBodyReader> requestBodyReader() {
    auto const transport = g_context->getTransport();
    return transport ? transport->getRequestBodyReader() : nullptr;
  }

  std::shared_ptr<RequestBodyReader> requireRequestBodyReader() {
    auto reader = requestBodyReader();
    if (!reader) {
      SystemLib::throwInvalidOperationExceptionObject(
        "No request-only body reader is selected");
    }
    return reader;
  }

  struct RequestBodyReadEvent final : AsioExternalThreadEvent {
    explicit RequestBodyReadEvent(std::shared_ptr<RequestBodyReader> reader)
      : m_reader(std::move(reader)) {}

    void complete(RequestBodyReadStatus status,
                  std::unique_ptr<folly::IOBuf> data) noexcept {
      m_status = status;
      m_data = std::move(data);
      markAsFinished();
    }

    void unserialize(TypedValue& result) override {
      auto data = m_data
        ? OptString(reinterpret_cast<const char*>(m_data->data()),
                    m_data->length(), CopyString)
        : OptString("");
      auto value = make_vec_array(static_cast<int64_t>(m_status), data);
      tvCopy(make_array_like_tv(value.detach()), result);
      m_delivered = true;
      dispose();
    }

   private:
    ~RequestBodyReadEvent() override { dispose(); }

    void dispose() noexcept {
      if (m_disposed) return;
      m_disposed = true;
      m_data.reset();
      m_reader->disposeRead(m_delivered, m_status);
    }

    std::shared_ptr<RequestBodyReader> m_reader;
    std::unique_ptr<folly::IOBuf> m_data;
    RequestBodyReadStatus m_status{RequestBodyReadStatus::TransportError};
    bool m_delivered{false};
    bool m_disposed{false};
  };

  Variant HHVM_FUNCTION(HH_io_request_body_profile) {
    auto reader = requestBodyReader();
    if (!reader) return init_null();
    auto const& metadata = reader->metadata();
    auto const& profile = reader->profile();
    VecInit segments(metadata.segments.size());
    for (auto const& segment : metadata.segments) {
      segments.append(OptString(segment));
    }
    auto const failure = reader->failure();
    return make_dict_array(
      "name", OptString(profile.name),
      "route", OptString(metadata.route),
      "segments", segments.toArray(),
      "capacity", static_cast<int64_t>(profile.capacity),
      "queue_high_water_mark", static_cast<int64_t>(profile.highWaterMark),
      "max_read_bytes", static_cast<int64_t>(reader->maxReadBytes()),
      "failure", failure == RequestBodyReadStatus::Data
        ? Variant(init_null()) : Variant(static_cast<int64_t>(failure)));
  }

  bool HHVM_FUNCTION(HH_io_request_body_try_admit) {
    return requireRequestBodyReader()->tryAdmit();
  }

  Object HHVM_FUNCTION(HH_io_request_body_read,
                       int64_t maxBytes, int64_t timeoutNs) {
    auto reader = requireRequestBodyReader();
    auto const maxRead = reader->maxReadBytes();
    if (maxBytes <= 0 || static_cast<uint64_t>(maxBytes) > maxRead ||
        timeoutNs <= 0) {
      SystemLib::throwInvalidArgumentExceptionObject(
        "A body read requires a bounded positive size and timeout");
    }
    auto const now = RequestBodyReader::Clock::now();
    auto const timeout = std::chrono::nanoseconds(timeoutNs);
    if (timeout > RequestBodyReader::Clock::time_point::max() - now) {
      SystemLib::throwInvalidArgumentExceptionObject(
        "Request body read deadline overflows the monotonic clock");
    }
    auto const deadline = now + timeout;
    if (!reader->tryReserveRead()) {
      SystemLib::throwInvalidOperationExceptionObject(
        "Request body is not admitted or already has an outstanding read");
    }

    RequestBodyReadEvent* event = nullptr;
    bool transferred = false;
    SCOPE_EXIT {
      if (!transferred) {
        if (event) {
          event->complete(RequestBodyReadStatus::TransportError, nullptr);
        } else {
          reader->disposeRead(false, RequestBodyReadStatus::TransportError);
        }
      }
    };
    event = new RequestBodyReadEvent(reader);
    // Retain the handle before a canceled event can complete and delete itself.
    Object handle(event->getWaitHandle());
    auto completion = std::make_unique<RequestBodyReadCompletion>(
      [event](RequestBodyReadStatus status,
              std::unique_ptr<folly::IOBuf> data) noexcept {
        event->complete(status, std::move(data));
      });
    transferred = true;
    reader->requestRead(maxBytes, deadline, std::move(completion));
    return handle;
  }

  void HHVM_FUNCTION(HH_io_request_body_abandon) {
    if (auto reader = requestBodyReader()) reader->requestAbandon();
  }

  int64_t HHVM_FUNCTION(HH_io_request_body_monotonic_ns) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      RequestBodyReader::Clock::now().time_since_epoch()).count();
  }

  struct IOExtension final : Extension {

    IOExtension() : Extension("hsl_io", "1.0", "hphp_hphpi") {}

    void moduleRegisterNative() override {
      // Clang 15 doesn't like the HHVM_FALIAS macro with \\N
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\Native\\pipe",
        HH_io_pipe
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\response_write,
        HH_io_response_write
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\response_flush,
        HH_io_response_flush
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\request_read,
        HH_io_request_read
      );
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\_IO\\request_body_profile",
        HH_io_request_body_profile
      );
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\_IO\\request_body_try_admit",
        HH_io_request_body_try_admit
      );
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\_IO\\request_body_read",
        HH_io_request_body_read
      );
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\_IO\\request_body_abandon",
        HH_io_request_body_abandon
      );
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\_IO\\request_body_monotonic_ns",
        HH_io_request_body_monotonic_ns
      );
    }

    void requestInit() {
      *request_read_offset.get() = 0;
    }
  } s_io_extension;

} // anonymous namespace
} // namespace HPHP
