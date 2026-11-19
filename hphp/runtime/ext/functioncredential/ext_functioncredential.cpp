// Copyright 2004-present Facebook. All Rights Reserved.

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

#include "hphp/runtime/ext/functioncredential/ext_functioncredential.h"

#include <array>
#include <chrono>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/native-data.h"

#include <folly/Random.h>
#include <folly/String.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include <fmt/core.h>

#include <sodium.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

namespace {

// Meyer's singleton for per-process authentication key.
// This ensures FunctionCredential objects can only be unpacked
// in the same process that packed them.
class FunctionCredentialAuthKey {
 public:
  explicit FunctionCredentialAuthKey(bool testMode = false);

  String sign(const std::string& message) const;
  OptString verify(const OptString& signedMessage) const;

 private:
  std::array<unsigned char, crypto_auth_KEYBYTES> key_;
};

// Validity duration for packed credentials (5 minutes)
constexpr int64_t kValidityDurationSeconds = 5 * 60;

// JSON field names
constexpr const char* kClassNameField = "class_name";
constexpr const char* kFunctionNameField = "function_name";
constexpr const char* kValidUntilField = "valid_until";

// Hex-encoded tag length constants
constexpr size_t kHexTagLen = crypto_auth_BYTES * 2;
constexpr size_t kMinSignedMessageLen = kHexTagLen + 1; // At least ":hex_tag"

// Signed message format: "message:hex_auth_tag"
struct ParsedSignedMessage {
  folly::StringPiece message;
  folly::StringPiece hexTag;
};

void validateSignedMessageFormat(const OptString& signedMessage);
ParsedSignedMessage parseSignedMessage(const OptString& signedMessage);
std::string decodeAuthTag(folly::StringPiece hexTag);
void verifyAuthTag(
    const std::string& tag,
    folly::StringPiece message,
    const std::array<unsigned char, crypto_auth_KEYBYTES>& key);

const Func* loadFreeFunctionOrThrow(folly::StringPiece funcName);
const Func* loadClassMethodOrThrow(
    folly::StringPiece className,
    folly::StringPiece funcName);
const Func* loadFuncOrThrow(
    folly::StringPiece className,
    folly::StringPiece funcName);
void validateCredentialNotExpired(std::int64_t validUntilEpoch);

const FunctionCredentialAuthKey& getAuthKey();
} // namespace

///////////////////////////////////////////////////////////////////////////////
// FunctionCredential implementation

// static
ObjectData* FunctionCredential::newInstance(const Func* func) {
  assertx(func);
  auto objData = ObjectData::newInstance(classof());
  auto data = Native::data<FunctionCredential>(objData);
  data->func_ = func;
  return objData;
}

// static
const FunctionCredential* FunctionCredential::fromObject(
    const ObjectData* obj) {
  return Native::data<FunctionCredential>(obj);
}

static OptString HHVM_METHOD(FunctionCredential, getClassName) {
  auto data = FunctionCredential::fromObject(this_);
  auto func = data->func();

  auto cls = func->cls();
  return cls ? OptString{const_cast<StringData*>(cls->name())}
             : OptString{};
}

static String HHVM_METHOD(FunctionCredential, getFunctionName) {
  auto data = FunctionCredential::fromObject(this_);
  return String::assertNonNull(makeStaticString(data->func()->name()));
}

static String HHVM_METHOD(FunctionCredential, getFilename) {
  auto data = FunctionCredential::fromObject(this_);
  return String::assertNonNull(makeStaticString(data->func()->filename()));
}

static String HHVM_METHOD(FunctionCredential, getMethodOrFunctionName) {
  auto data = FunctionCredential::fromObject(this_);
  return String::assertNonNull(data->func()->fullName());
}

static String HHVM_METHOD(FunctionCredential, pack) {
  auto data = FunctionCredential::fromObject(this_);
  auto func = data->func();

  // Calculate expiration time (current time + 5 minutes)
  auto now = std::chrono::system_clock::now();
  auto validUntil = now + std::chrono::seconds(kValidityDurationSeconds);
  auto validUntilEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                             validUntil.time_since_epoch())
                             .count();

  // Build JSON object with class_name, function_name, and valid_until
  folly::dynamic jsonObj = folly::dynamic::object;
  auto cls = func->cls();
  if (cls) {
    jsonObj[kClassNameField] =
        std::string(cls->name()->data(), cls->name()->size());
  } else {
    jsonObj[kClassNameField] = nullptr;
  }
  jsonObj[kFunctionNameField] =
      std::string(func->name()->data(), func->name()->size());
  jsonObj[kValidUntilField] = validUntilEpoch;

  std::string jsonStr = folly::toJson(jsonObj);

  return getAuthKey().sign(jsonStr);
}

static ObjectRet
HHVM_STATIC_METHOD(FunctionCredential, unpack, const OptString& packed) {
  OptString jsonStr = getAuthKey().verify(packed);

  // Parse the JSON
  folly::dynamic jsonObj;
  try {
    jsonObj =
        folly::parseJson(folly::StringPiece(jsonStr.data(), jsonStr.size()));
  } catch (const std::exception&) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: malformed JSON");
  }

  if (!jsonObj.isObject()) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: expected JSON object");
  }

  // Validate required fields using find API to avoid double lookups
  auto* funcNamePtr = jsonObj.get_ptr(kFunctionNameField);
  auto* validUntilPtr = jsonObj.get_ptr(kValidUntilField);

  if (!funcNamePtr || !validUntilPtr || !funcNamePtr->isString() ||
      !validUntilPtr->isInt()) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: missing required fields");
  }

  // Check expiration time
  validateCredentialNotExpired(validUntilPtr->asInt());

  folly::StringPiece funcNamePiece = funcNamePtr->getString();

  // Extract class name and function name
  folly::StringPiece classNamePiece = [&jsonObj]() -> folly::StringPiece {
    if (auto* classNamePtr = jsonObj.get_ptr(kClassNameField);
        classNamePtr && !classNamePtr->isNull()) {
      if (!classNamePtr->isString()) {
        SystemLib::throwInvalidArgumentExceptionObject(
            "Invalid packed FunctionCredential: class_name is not a string");
      }
      return classNamePtr->getString();
    }
    return "";
  }();

  auto func = loadFuncOrThrow(classNamePiece, funcNamePiece);
  return Object{FunctionCredential::newInstance(func)};
}

namespace {

FunctionCredentialAuthKey::FunctionCredentialAuthKey(bool testMode)
    : key_{} // zero initialization for test mode
{
  if (!testMode) {
    folly::Random::secureRandom(key_.data(), key_.size());
  }
}

String FunctionCredentialAuthKey::sign(const std::string& message) const {
  std::array<unsigned char, crypto_auth_BYTES> tag;
  crypto_auth(
      tag.data(),
      reinterpret_cast<const unsigned char*>(message.data()),
      message.size(),
      key_.data());

  // Hex-encode the tag and format as "message:hex_tag"
  auto hexTag = folly::hexlify(folly::ByteRange(tag.data(), tag.size()));
  return String{fmt::format("{}:{}", message, hexTag)};
}

OptString FunctionCredentialAuthKey::verify(const OptString& signedMessage) const {
  validateSignedMessageFormat(signedMessage);
  auto parsed = parseSignedMessage(signedMessage);
  auto tag = decodeAuthTag(parsed.hexTag);
  verifyAuthTag(tag, parsed.message, key_);
  return OptString(parsed.message.data(), parsed.message.size(), CopyString);
}

void validateSignedMessageFormat(const OptString& packedMessage) {
  if (packedMessage.size() < kMinSignedMessageLen) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: data too short");
  }

  size_t messageLen = packedMessage.size() - kHexTagLen - 1;
  if (packedMessage[messageLen] != ':') {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: malformed format");
  }
}

ParsedSignedMessage parseSignedMessage(const OptString& signedMessage) {
  size_t messageLen = signedMessage.size() - kHexTagLen - 1;
  return ParsedSignedMessage{
      folly::StringPiece(signedMessage.data(), messageLen),
      folly::StringPiece(signedMessage.data() + messageLen + 1, kHexTagLen)};
}

std::string decodeAuthTag(folly::StringPiece hexTag) {
  std::string tagStr;
  if (!folly::unhexlify(hexTag, tagStr) || tagStr.size() != crypto_auth_BYTES) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: invalid authentication tag");
  }
  return tagStr;
}

void verifyAuthTag(
    const std::string& tag,
    folly::StringPiece message,
    const std::array<unsigned char, crypto_auth_KEYBYTES>& key) {
  // reinterpret_cast required for libsodium API
  int result = crypto_auth_verify(
      reinterpret_cast<const unsigned char*>(tag.data()),
      reinterpret_cast<const unsigned char*>(message.data()),
      message.size(),
      key.data());

  if (result != 0) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: "
        "verification failed (packed in a different process?)");
  }
}

const Func* loadFreeFunctionOrThrow(folly::StringPiece funcNamePiece) {
  OptString funcName{funcNamePiece};

  const auto* func = Func::load(funcName.get());

  if (!func) {
    SystemLib::throwInvalidArgumentExceptionObject(
        fmt::format("Unable to find function {}", funcName.c_str()));
  }
  return func;
}

const Func* loadClassMethodOrThrow(
    folly::StringPiece classNamePiece,
    folly::StringPiece funcNamePiece) {
  OptString className{classNamePiece};

  const auto* cls = Class::load(className.get());

  if (!cls) {
    SystemLib::throwInvalidArgumentExceptionObject(
        fmt::format("Unable to find class {}", className.c_str()));
  }

  OptString funcName{funcNamePiece};

  const auto* func = cls->lookupMethod(funcName.get());

  if (!func) {
    SystemLib::throwInvalidArgumentExceptionObject(
        fmt::format("Unable to find method {}::{}", className.c_str(), funcName.c_str()));
  }
  return func;
}

const Func* loadFuncOrThrow(
    folly::StringPiece className,
    folly::StringPiece funcName) {
  return className.empty() ? loadFreeFunctionOrThrow(funcName)
                           : loadClassMethodOrThrow(className, funcName);
}

void validateCredentialNotExpired(std::int64_t validUntilEpoch) {
  auto now = std::chrono::system_clock::now();
  auto nowEpoch =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  if (nowEpoch > validUntilEpoch) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Invalid packed FunctionCredential: credential has expired");
  }
}

///////////////////////////////////////////////////////////////////////////////
// Extension registration

struct FunctionCredentialExtension final : Extension {
  FunctionCredentialExtension()
      : Extension("functioncredential", "1.0", "authz_scaled_solutions") {}

  void moduleLoad(const IniSetting::Map& ini, Hdf hdf) override {
    bool testMode =
        Config::GetBool(ini, hdf, "FunctionCredential.EnableTestMode", false);
    key_.emplace(testMode);
  }

  void moduleRegisterNative() override {
    HHVM_ME(FunctionCredential, getClassName);
    HHVM_ME(FunctionCredential, getFunctionName);
    HHVM_ME(FunctionCredential, getFilename);
    HHVM_ME(FunctionCredential, getMethodOrFunctionName);
    HHVM_ME(FunctionCredential, pack);
    HHVM_STATIC_ME(FunctionCredential, unpack);

    Native::registerNativeDataInfo<FunctionCredential>();
  }

  const FunctionCredentialAuthKey& getAuthKey() const {
    return key_.value();
  }

 private:
  std::optional<FunctionCredentialAuthKey> key_;
  // NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
} s_functioncredential_extension;

const FunctionCredentialAuthKey& getAuthKey() {
  return s_functioncredential_extension.getAuthKey();
}
} // namespace
} // namespace HPHP
