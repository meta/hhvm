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

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <utility>

#include "hphp/runtime/base/timestamp.h"
#include "hphp/util/build-info.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/process-host.h"

namespace HPHP::jit {

struct ProfileMetadata {
  uint32_t magic{0};
  off_t extraDataOffset{0};
  uint64_t repoSignature{0};
  std::string repoSchema;
  std::string producerHost;
  std::string tag;
  int64_t createdAt{0}; // Unix seconds.
};

inline ProfileMetadata currentProfileMetadata(uint32_t magic,
                                              uint64_t repoSignature = 0) {
  return {
    magic,
    off_t{0},
    repoSignature,
    repoSchemaId().str(),
    Process::GetHostName(),
    Cfg::Eval::ProfDataTag,
    TimeStamp::Current()
  };
}

// Native jit.prof header encoding, with size_t lengths for the string fields.
template<class WriteBytes>
bool writeProfileMetadata(const ProfileMetadata& metadata,
                          WriteBytes&& writeBytes) {
  auto const writeString = [&](const std::string& value) {
    auto const size = value.size();
    return writeBytes(&size, sizeof(size)) &&
           writeBytes(value.data(), size);
  };
  return writeBytes(&metadata.magic, sizeof(metadata.magic)) &&
         writeBytes(&metadata.extraDataOffset, sizeof(metadata.extraDataOffset)) &&
         writeBytes(&metadata.repoSignature, sizeof(metadata.repoSignature)) &&
         writeString(metadata.repoSchema) &&
         writeString(metadata.producerHost) &&
         writeString(metadata.tag) &&
         writeBytes(&metadata.createdAt, sizeof(metadata.createdAt));
}

template<class ReadBytes>
bool readProfileMetadata(ProfileMetadata& metadata,
                         ReadBytes&& readBytes,
                         size_t maxStringBytes) {
  auto const readString = [&](std::string& value) {
    size_t size{0};
    if (!readBytes(&size, sizeof(size)) || size > maxStringBytes) return false;
    value.resize(size);
    return readBytes(value.data(), size);
  };
  ProfileMetadata result;
  if (!readBytes(&result.magic, sizeof(result.magic)) ||
      !readBytes(&result.extraDataOffset, sizeof(result.extraDataOffset)) ||
      !readBytes(&result.repoSignature, sizeof(result.repoSignature)) ||
      !readString(result.repoSchema) ||
      !readString(result.producerHost) ||
      !readString(result.tag) ||
      !readBytes(&result.createdAt, sizeof(result.createdAt))) {
    return false;
  }
  metadata = std::move(result);
  return true;
}

}
