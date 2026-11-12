/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include <folly/Overload.h>
#include <folly/io/IOBuf.h>
#include <thrift/lib/cpp/transport/TTransportException.h>
#include <thrift/lib/cpp2/async/ClientStreamBridge.h>
#include <thrift/lib/cpp2/async/RequestCallback.h>
#include <thrift/lib/cpp2/async/RequestChannel.h>
#include <thrift/lib/cpp2/async/StreamCallbacks.h>
#include <variant>
#include <exception>
#include <tuple>
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/type-resource.h"
#include "hphp/runtime/base/types.h"
#include "hphp/runtime/ext/asio/asio-external-thread-event.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/vm/native-data.h"

namespace HPHP {
namespace thrift {
///////////////////////////////////////////////////////////////////////////////

extern const int64_t k_THRIFT_MARK_LEGACY_ARRAYS;

void HHVM_FUNCTION(
    thrift_protocol_write_binary,
    const Object& transportobj,
    const OptString& method_name,
    int64_t msgtype,
    const Object& request_struct,
    int64_t seqid,
    bool strict_write,
    bool oneway = false);

void HHVM_FUNCTION(
    thrift_protocol_write_binary_struct,
    const Object& transportobj,
    const Object& request_struct);

OptString HHVM_FUNCTION(
    thrift_protocol_write_binary_struct_to_string,
    const Object& request_struct);

Object HHVM_FUNCTION(
    thrift_protocol_read_binary,
    const Object& transportobj,
    const OptString& obj_typename,
    bool strict_read,
    int64_t options);

Variant HHVM_FUNCTION(
    thrift_protocol_read_binary_struct,
    const Object& transportobj,
    const OptString& obj_typename,
    int64_t options);


Object HHVM_FUNCTION(
    thrift_protocol_read_binary_struct_from_string,
    const OptString& serialized,
    const OptString& obj_typename,
    int64_t options);

int64_t HHVM_FUNCTION(thrift_protocol_set_compact_version, int64_t version);

void HHVM_FUNCTION(
    thrift_protocol_write_compact2,
    const Object& transportobj,
    const OptString& method_name,
    int64_t msgtype,
    const Object& request_struct,
    int64_t seqid,
    bool oneway = false,
    int64_t version = 2);

void HHVM_FUNCTION(
    thrift_protocol_write_compact_struct,
    const Object& transportobj,
    const Object& request_struct,
    int64_t version = 2);

OptString HHVM_FUNCTION(
    thrift_protocol_write_compact_struct_to_string,
    const Object& request_struct,
    int64_t version);

Variant HHVM_FUNCTION(
    thrift_protocol_read_compact,
    const Object& transportobj,
    const OptString& obj_typename,
    int64_t options);

Object HHVM_FUNCTION(
    thrift_protocol_read_compact_struct,
    const Object& transportobj,
    const OptString& obj_typename,
    int64_t options,
    int64_t version = 2);

Object HHVM_FUNCTION(
    thrift_protocol_read_compact_struct_from_string,
    const OptString& serialized,
    const OptString& obj_typename,
    int64_t options,
    int64_t version = 2);

///////////////////////////////////////////////////////////////////////////////
// Helper functions for compact serialization and deserialization

Object compact_deserialize_from_string(
    const OptString& serialized,
    const OptString& thrift_typename,
    int64_t options = 0, int64_t version = 2);

OptString compact_serialize_to_string(
                   const Object& thrift_struct,
                   int64_t version = 2);

///////////////////////////////////////////////////////////////////////////////

struct InteractionId : SystemLib::ClassLoader<"InteractionId"> {
  static Object newInstance() {
    return Object{classof()};
  }

  ~InteractionId() {
    sweep();
  }

  void sweep() {
    if (interactionId_) {
      channel_->terminateInteraction(std::move(interactionId_));
      channel_.reset();
    }
  }

  void attach(
      const std::shared_ptr<apache::thrift::RequestChannel>& channel,
      apache::thrift::InteractionId&& id) {
    channel_ = channel;
    interactionId_ = std::move(id);
  }

  const apache::thrift::InteractionId& getInteractionId() const {
    return interactionId_;
  }

 private:
  apache::thrift::InteractionId interactionId_;
  std::shared_ptr<apache::thrift::RequestChannel> channel_;
};

/////////////////////////////////////////////////////////////////////////////

struct RpcOptions : SystemLib::ClassLoader<"RpcOptions"> {
  RpcOptions() {}
  RpcOptions& operator=(const RpcOptions& that_) {
    return *this;
  }

  void sweep() {
    close(true);
  }

  void close(bool /*sweeping*/ = false) {}

  static Object newInstance() {
    return Object{classof()};
  }

  static RpcOptions* GetDataOrThrowException(ObjectData* object_) {
    if (object_ == nullptr) {
      throw_null_pointer_exception();
      not_reached();
    }
    if (!object_->getVMClass()->classofNonIFace(classof())) {
      raise_error("RpcOptions expected");
      not_reached();
    }
    return Native::data<RpcOptions>(object_);
  }

  apache::thrift::RpcOptions rpcOptions;

 private:
  TYPE_SCAN_IGNORE_ALL;
};

///////////////////////////////////////////////////////////////////////////////

struct TClientStreamError {
 public:
  TClientStreamError() {}
  TClientStreamError(std::unique_ptr<folly::IOBuf> encodedErrorMsg)
      : errorMsg_(std::move(encodedErrorMsg)), isEncoded_(true) {}
  TClientStreamError(std::string errorStr)
      : errorMsg_(folly::IOBuf::copyBuffer(errorStr, errorStr.size())),
        isEncoded_(false) {}

  static TClientStreamError create(folly::exception_wrapper ew) {
    std::unique_ptr<folly::IOBuf> msgBuffer;
    std::string msgStr = ew.what().toStdString();
    ew.handle(
        [&](apache::thrift::detail::EncodedError& err) {
          msgBuffer = std::move(err.encoded);
        },
        [&](apache::thrift::detail::EncodedStreamError& err) {
          auto& payload = err.encoded;
          DCHECK(payload.metadata.payloadMetadata().has_value());
          DCHECK_EQ(
              payload.metadata.payloadMetadata()->getType(),
              apache::thrift::PayloadMetadata::Type::exceptionMetadata);
          auto& exceptionMetadataBase =
              payload.metadata.payloadMetadata()->get_exceptionMetadata();
          if (auto exceptionMetadataRef =
                  exceptionMetadataBase.metadata()) {
            if (exceptionMetadataRef->getType() ==
                apache::thrift::PayloadExceptionMetadata::Type::declaredException) {
              msgBuffer = std::move(payload.payload);
              if (!msgBuffer) {
                msgStr = "Failed to parse declared exception";
              }
            } else {
              msgStr = exceptionMetadataBase.what_utf8().value_or("");
            }
          } else {
            msgStr = "Missing payload exception metadata";
          }
        },
        [&](apache::thrift::detail::EncodedStreamRpcError& err) {
          apache::thrift::StreamRpcError streamRpcError;
          apache::thrift::CompactProtocolReader reader;
          reader.setInput(err.encoded.get());
          streamRpcError.read(&reader);
          msgStr = streamRpcError.what_utf8().value_or("");
        },
        [](...) {});

    if (msgBuffer) {
      return TClientStreamError(std::move(msgBuffer));
    }
    return TClientStreamError(msgStr);
  }

  std::unique_ptr<folly::IOBuf> errorMsg_;
  bool isEncoded_ = false;
};

Object HHVM_METHOD(TClientBufferedStream, genNext);

struct TClientBufferedStream : SystemLib::ClassLoader<"TClientBufferedStream"> {
  TClientBufferedStream() = default;
  TClientBufferedStream(const TClientBufferedStream&) = delete;
  TClientBufferedStream& operator=(const TClientBufferedStream&) = delete;
  ~TClientBufferedStream() {
    sweep();
  }

  using BufferAndErrorPair =
      std::pair<std::vector<std::unique_ptr<folly::IOBuf>>, TClientStreamError>;

  void sweep() {
    endStream();
  }

  void init(
      apache::thrift::detail::ClientStreamBridge::ClientPtr streamBridge,
      apache::thrift::BufferOptions bufferOptions) {
    streamBridge_ = std::move(streamBridge);
    bufferOptions_ = bufferOptions;
    if (bufferOptions_.chunkSize == 0) {
      streamBridge_->requestN(1);
      ++bufferOptions_.chunkSize;
    }
    outstanding_ = bufferOptions_.chunkSize;
    payloadDataSize_ = 0;
  }

  void endStream() {
    streamBridge_.reset();
  }

  bool shouldRequestMore() {
    if (outstanding_ <= bufferOptions_.chunkSize / 2) {
      return true;
    }
    return use16KBBufferingPolicy_ && (payloadDataSize_ >= kRequestCreditPayloadSize);
  }

  BufferAndErrorPair clientQueueToVec() {
    std::vector<std::unique_ptr<folly::IOBuf>> bufferVec;
    TClientStreamError error;
    while (!queue_.empty()) {
      auto& message = queue_.front();
      bool streamEnded = folly::variant_match(
          message,
          [&](apache::thrift::StreamMessage::PayloadOrError& payloadOrError) {
            auto& payload = payloadOrError.streamPayloadTry;
            if (payload.hasException()) {
              error = TClientStreamError::create(payload.exception());
              return true;
            }
            const size_t payloadSize = payload->payload
                ? payload->payload->computeChainDataLength()
                : 0;
            if (payloadSize) {
              payloadDataSize_ += payloadSize;
              bufferVec.push_back(std::move(payload->payload));
            }
            if (payloadSize || payload->isOrderedHeader) {
              --outstanding_;
            }
            return false;
          },
          [&](apache::thrift::StreamMessage::Complete) { return true; });
      queue_.pop();
      if (streamEnded) {
        endStream();
        break;
      }
      if (shouldRequestMore()) {
        break;
      }
    }
    return std::make_pair(std::move(bufferVec), std::move(error));
  }

  static Object newInstance() {
    return Object{classof()};
  }

  static TClientBufferedStream* GetDataOrThrowException(ObjectData* object_) {
    if (object_ == nullptr) {
      throw_null_pointer_exception();
      not_reached();
    }
    if (!object_->getVMClass()->classofNonIFace(classof())) {
      raise_error("TClientBufferedStream expected");
      not_reached();
    }
    return Native::data<TClientBufferedStream>(object_);
  }

 private:
  Object genNext();
  void disable16KBBufferingPolicy();

 public:
  apache::thrift::detail::ClientStreamBridge::ClientPtr streamBridge_;
  apache::thrift::BufferOptions bufferOptions_;
  apache::thrift::detail::ClientStreamBridge::ClientQueue queue_;
  int32_t outstanding_ = 0;
  size_t payloadDataSize_ = 0;
  bool use16KBBufferingPolicy_ = true;
  static constexpr size_t kRequestCreditPayloadSize = 16384;
};

///////////////////////////////////////////////////////////////////////////////

using TClientSinkCreditsOrFinalResponse =
    std::variant<uint64_t, std::unique_ptr<folly::IOBuf>, TClientStreamError>;

struct TClientSink : SystemLib::ClassLoader<"TClientSink"> {
  TClientSink() = default;
  TClientSink(const TClientSink&) = delete;
  TClientSink& operator=(const TClientSink&) = delete;
  ~TClientSink() {
    sweep();
  }

  void sweep() {
    cancel();
  }

  void init(apache::thrift::detail::ClientSinkBridge::ClientPtr sinkBridge) {
    sinkBridge_.reset(sinkBridge.release());
    readyBatchReturned_ = false;
  }

  void endSink() {
    sinkBridge_.reset();
  }

  void cancel() {
    if (sinkBridge_) {
      sinkBridge_->cancel(folly::Try<apache::thrift::StreamPayload>(
          apache::thrift::rocket::RocketException(
              apache::thrift::rocket::ErrorCode::CANCELED)));
      endSink();
    }
  }

  static Object newInstance() {
    return Object{classof()};
  }

  static TClientSink* GetDataOrThrowException(ObjectData* object_) {
    if (object_ == nullptr) {
      throw_null_pointer_exception();
      not_reached();
    }
    if (!object_->getVMClass()->classofNonIFace(classof())) {
      raise_error("TClientSink expected");
      not_reached();
    }
    return Native::data<TClientSink>(object_);
  }

  TClientSinkCreditsOrFinalResponse getCreditsOrFinalResponse() {
    return getCreditsOrFinalResponse(sinkBridge_->getMessages());
  }

  TClientSinkCreditsOrFinalResponse getCreditsOrFinalResponse(
      apache::thrift::detail::ClientSinkBridge::ClientQueue creditsQueue) {
    uint64_t credits = 0;
    folly::Try<apache::thrift::StreamPayload> response;
    bool responseAvailable = false;

    while (!creditsQueue.empty() && !responseAvailable) {
      auto& message = creditsQueue.front();
      folly::variant_match(
          message,
          [&](apache::thrift::StreamMessage::PayloadOrError& payloadOrError) {
            response = std::move(payloadOrError.streamPayloadTry);
            responseAvailable = true;
          },
          [&](apache::thrift::StreamMessage::RequestN& requestN) {
            credits += requestN.n;
          });
      creditsQueue.pop();
    }
    if (responseAvailable) {
      endSink();
      if (response.hasException()) {
        return TClientSinkCreditsOrFinalResponse(
            TClientStreamError::create(response.exception()));
      }
      if (response->payload) {
        return TClientSinkCreditsOrFinalResponse(std::move(response->payload));
      }
      return TClientSinkCreditsOrFinalResponse();
    }
    return TClientSinkCreditsOrFinalResponse(credits);
  }

 public:
  apache::thrift::detail::ClientSinkBridge::ClientReleasePtr sinkBridge_;
  bool readyBatchReturned_ = false;
};
} // namespace thrift

inline OptString ioBufToString(const folly::IOBuf& ioBuf) {
  auto resultStringData = StringData::Make(ioBuf.computeChainDataLength());
  for (const auto& buf : ioBuf) {
    auto const data = reinterpret_cast<const char*>(buf.data());
    auto const piece = folly::StringPiece{data, buf.size()};
    resultStringData->append(piece);
  }
  return OptString::attach(resultStringData);
}

struct Thrift2StreamEvent final : AsioExternalThreadEvent {
  Thrift2StreamEvent() {}

  void finish(
      thrift::TClientBufferedStream::BufferAndErrorPair bufferAndError,
      bool streamEnded) {
    buffer_ = std::move(bufferAndError.first);
    error_ = std::move(bufferAndError.second);
    streamEnded_ = streamEnded;
    markAsFinished();
  }

  void error(std::unique_ptr<folly::IOBuf> err) {
    serverError_ = std::move(err);
    markAsFinished();
  }

 protected:
  // Called by PHP thread
  void unserialize(TypedValue& result) final {
    if (serverError_) {
      throw_object(
          "TTransportException",
          make_vec_array(
              ioBufToString(*serverError_),
              0 // error code UNKNOWN
              ));
    }

    OptString errorStr = null_string;
    if (error_.errorMsg_) {
      if (error_.isEncoded_) {
        // Encoded stream exceptions need to be deserialized by generated code.
        // Adding it at the end of the buffer vec so that it will be decoded
        // along with other payloads and will throw exception after returning
        // all payloads to client.
        buffer_.push_back(std::move(error_.errorMsg_));
      } else {
        errorStr = ioBufToString(*error_.errorMsg_);
      }
    }

    Array bufferVec;
    // Header-only batches can be empty while the stream is still open.
    if (buffer_.empty() && streamEnded_) {
      bufferVec = null_array;
    } else {
      VecInit resArr(buffer_.size());
      for (auto& ioBuf : buffer_) {
        resArr.append(ioBufToString(*ioBuf));
      }
      bufferVec = resArr.toArray();
    }
    tvCopy(
        make_array_like_tv(make_vec_array(bufferVec, errorStr).detach()),
        result);
  }

 private:
  std::vector<std::unique_ptr<folly::IOBuf>> buffer_;
  bool streamEnded_ = false;

  // any error while processing queue, this should be returned as error message
  // along with the buffer and should be sequenced after any received payloads
  thrift::TClientStreamError error_;

  // Unexpected server error which should be thrown immediately
  std::unique_ptr<folly::IOBuf> serverError_;
};

inline Array thriftSinkResponseToArray(
    const thrift::TClientSinkCreditsOrFinalResponse& response) {
  auto responseStr = null_string;
  auto errorStr = null_string;
  uint64_t credits = 0;
  folly::variant_match(
      response,
      [&](const std::unique_ptr<folly::IOBuf>& finalResponse) {
        responseStr = ioBufToString(*finalResponse);
      },
      [&](const thrift::TClientStreamError& finalResponseError) {
        if (finalResponseError.errorMsg_) {
          // Encoded stream exceptions are decoded by generated code.
          if (finalResponseError.isEncoded_) {
            responseStr = ioBufToString(*finalResponseError.errorMsg_);
          } else {
            errorStr = ioBufToString(*finalResponseError.errorMsg_);
          }
        }
      },
      [&](const uint64_t& n) { credits = n; });
  return make_vec_array(credits, responseStr, errorStr);
}

struct ThriftSinkEvent final : AsioExternalThreadEvent {
  ThriftSinkEvent() = default;

  void finish(thrift::TClientSinkCreditsOrFinalResponse credits) {
    creditsOrFinalResponse_ = std::move(credits);
    markAsFinished();
  }

  void error(std::unique_ptr<folly::IOBuf> err) {
    serverError_ = std::move(err);
    markAsFinished();
  }

 protected:
  // Called by PHP thread
  void unserialize(TypedValue& result) final {
    if (serverError_) {
      throw_object(
          "TTransportException",
          make_vec_array(
              ioBufToString(*serverError_),
              0 // error code UNKNOWN
              ));
    }
    tvCopy(
        make_array_like_tv(
            thriftSinkResponseToArray(creditsOrFinalResponse_).detach()),
        result);
  }

 private:
  thrift::TClientSinkCreditsOrFinalResponse creditsOrFinalResponse_;

  // Unexpected server error which should be thrown immediately
  std::unique_ptr<folly::IOBuf> serverError_;
};
} // namespace HPHP
