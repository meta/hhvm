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

#include "hphp/util/blob.h"
#include "hphp/util/blob-encoder.h"
#include "hphp/util/build-info.h"
#include "hphp/util/debug-format.h"
#include "hphp/util/htonll.h"

#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <fmt/core.h>
#include <folly/system/MemoryMapping.h>
#include <magic_enum/magic_enum.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HPHP {

namespace Blob {

using Magic = std::array<char, 4>;
using Version = uint16_t;

enum class ReadMode { PReadOnly, MMap };
enum class ErrorMode { Assert, Throw };

template <typename... Args>
[[noreturn]] inline void fail(
  ErrorMode mode,
  DebugFormatString<Args...> format,
  Args&&... args
) {
  if (mode == ErrorMode::Throw) {
    throw std::runtime_error(debugFormat(format, std::forward<Args>(args)...));
  }
  always_assert_flog(false, format, std::forward<Args>(args)...);
}

////////////////////////////////////////////////////////////////////////////////
// Wrapper around file descriptor for automatic closing and wrapping
// operations in a nicer interface.
struct FD {
  FD() : m_fd{-1} {}
  FD(
    const std::string& path,
    int flags,
    ErrorMode errorMode = ErrorMode::Assert
  ) : m_path{path}
    , m_errorMode{errorMode}
  {
    auto fd = folly::openNoInt(m_path.c_str(), flags, 0644);
    if (fd < 0) {
      auto const error = folly::errnoStr(errno);
      fail(m_errorMode, "Unable to open {}: {}", m_path, error);
    }
    m_fd = fd;
  }
  ~FD() { if (isOpen()) ::close(m_fd); }

  bool isOpen() const { return m_fd >= 0; }

  void write(const void* data, size_t size) const {
    auto const written = folly::writeFull(m_fd, data, size);
    if (written == size) return;
    if (written < 0) {
      auto const error = folly::errnoStr(errno);
      fail(
        m_errorMode,
        "Failed writing {} bytes to {}: {}",
        size,
        m_path,
        error
      );
    }
    fail(
      m_errorMode,
      "Partial write to {} (expected {}, actual {})",
      m_path,
      size,
      written
    );
  }

  // Write an integer in network byte-order
  template <typename T> void writeInt(T v) const {
    v = hostToFile(v);
    this->write(&v, sizeof(v));
  }

  void read(void* data, size_t size) const {
    auto const read = folly::readFull(m_fd, data, size);
    if (read == size) return;
    if (read < 0) {
      auto const error = folly::errnoStr(errno);
      fail(
        m_errorMode,
        "Failed reading {} bytes from {}: {}",
        size,
        m_path,
        error
      );
    }
    fail(
      m_errorMode,
      "Partial read from {} (expected {}, actual {})",
      m_path,
      size,
      read
    );
  }

  // Read a network byte-order integer
  template <typename T> std::remove_cv_t<T> readInt() const {
    std::remove_cv_t<T> v;
    this->read(&v, sizeof(v));
    return fileToHost(v);
  }

  void pread(void* data, size_t size, size_t offset) const {
    auto const read = folly::preadFull(m_fd, data, size, offset);
    if (read == size) return;
    if (read < 0) {
      auto const error = folly::errnoStr(errno);
      fail(
        m_errorMode,
        "Failed reading {} bytes from {} at {}: {}",
        size,
        m_path,
        offset,
        error
      );
    }
    fail(
      m_errorMode,
      "Partial read from {} at {} (expected {}, actual {})",
      m_path,
      offset,
      size,
      read
    );
  }

  void seek(size_t offset) const {
    auto const actualOffset = ::lseek(m_fd, offset, SEEK_SET);
    if (actualOffset == offset) return;
    if (actualOffset < 0) {
      auto const error = folly::errnoStr(errno);
      fail(
        m_errorMode,
        "Failed to seek to {} in {}: {}",
        offset,
        m_path,
        error
      );
    }
    fail(
      m_errorMode,
      "Partial seek in {} (expected {}, actual {})",
      m_path,
      offset,
      actualOffset
    );
  }

  uint64_t fileSize() const {
    auto const size = ::lseek(m_fd, 0, SEEK_END);
    if (size >= 0) return size;
    auto const error = folly::errnoStr(errno);
    fail(
      m_errorMode,
      "Failed to seek to end of {}: {}",
      m_path,
      error
    );
  }

  struct Blob {
    std::unique_ptr<char[]> buffer;
    BlobDecoder decoder;
  };

  Blob readBlob(size_t offset, size_t size) const {
    if (m_mapping) {
      auto range = m_mapping->range();
      if (size > range.size() || offset > range.size() - size) {
        fail(
          m_errorMode,
          "readBlob out of bounds in {}: offset {} + size {} > mapLen {}",
          m_path,
          offset,
          size,
          range.size()
        );
      }
      BlobDecoder decoder{
        reinterpret_cast<const char*>(range.data() + offset), size};
      return { nullptr, std::move(decoder) };
    }
    auto buffer = std::make_unique<char[]>(size);
    if (size > 0) this->pread(buffer.get(), size, offset);
    BlobDecoder decoder{buffer.get(), size};
    return { std::move(buffer), std::move(decoder) };
  }

  FD(const FD&) = delete;
  FD& operator=(const FD&) = delete;

  FD(FD&& o) noexcept
    : m_fd{o.m_fd}
    , m_path{std::move(o.m_path)}
    , m_mapping{std::move(o.m_mapping)}
    , m_errorMode{o.m_errorMode}
  {
    o.m_fd = -1;
  }

  FD& operator=(FD&& o) {
    std::swap(m_fd, o.m_fd);
    std::swap(m_path, o.m_path);
    std::swap(m_mapping, o.m_mapping);
    std::swap(m_errorMode, o.m_errorMode);
    return *this;
  }

  // mmap the file for zero-copy reads. CHECKs on failure.
  void enableMmap(size_t fileSize) {
    if (fileSize == 0) {
      fail(m_errorMode, "Unable to mmap empty file {}", m_path);
    }
    m_mapping.emplace(m_fd, 0, fileSize);
  }

private:
  static uint8_t  hostToFile(uint8_t v)  { return v; }
  static uint16_t hostToFile(uint16_t v) { return htons(v); }
  static uint32_t hostToFile(uint32_t v) { return htonl(v); }
  static uint64_t hostToFile(uint64_t v) { return htonll(v); }

  static uint8_t  fileToHost(uint8_t v)  { return v; }
  static uint16_t fileToHost(uint16_t v) { return ntohs(v); }
  static uint32_t fileToHost(uint32_t v) { return ntohl(v); }
  static uint64_t fileToHost(uint64_t v) { return ntohll(v); }

  int m_fd;
  std::string m_path;
  std::optional<folly::MemoryMapping> m_mapping;
  ErrorMode m_errorMode{ErrorMode::Assert};
};

////////////////////////////////////////////////////////////////////////////////

// Blob of data containing the sizes of the various sections. This is
// stored near the beginning of the file and can be used to compute
// offsets for the sections. Since we don't know the sizes until after
// we write everything, we have to write these as fixed size integers.
template <typename Chunk, typename Index>
struct SizeHeader {
  static constexpr size_t ChunkSize = magic_enum::enum_count<Chunk>();
  static constexpr size_t IndexSize = magic_enum::enum_count<Index>();

  size_t chunks[ChunkSize] = { 0 };
  size_t indexes[IndexSize * 2] = { 0 };

  constexpr static size_t kFileSize = sizeof(chunks) + sizeof(indexes);

  void write(const Blob::FD& fd) const {
    for (auto i = 0; i < ChunkSize; i++) {
      fd.writeInt(chunks[i]);
    }
    for (auto i = 0; i < IndexSize * 2; i++) {
      fd.writeInt(indexes[i]);
    }
  }

  void read(const Blob::FD& fd) {
    for (auto i = 0; i < ChunkSize; i++) {
      chunks[i] = fd.readInt<size_t>();
    }
    for (auto i = 0; i < IndexSize * 2; i++) {
      indexes[i] = fd.readInt<size_t>();
    }
  }

  size_t get(Chunk chunk) {
    return chunks[uint32_t(chunk)];
  }

  void add(Chunk chunk, size_t size) {
    assertx(canWrite(chunk));
    auto chunk_int = size_t(chunk);
    chunks[chunk_int] += size;
  }

  void set(Index index, size_t indexSize, size_t dataSize) {
    // We must write indexes from the lowest to highest and after chunks
    assertx(canWrite(index));
    auto index_int = size_t(index);
    indexes[index_int * 2] = indexSize;
    indexes[index_int * 2 + 1] = dataSize;
  }

  bool canWrite(Chunk chunk) {
    for (auto i = size_t(chunk) + 1; i < ChunkSize; i++) {
      if (chunks[i] != 0) {
        return false;
      }
    }
    return canWrite(Index(-1));
  }

  bool canWrite(Index index) {
    for (auto i = size_t(index) + 1; i < IndexSize; i++) {
      if (indexes[i * 2] != 0 || indexes[i * 2 + 1] != 0) {
        return false;
      }
    }
    return true;
  }
};

////////////////////////////////////////////////////////////////////////////////

template <typename Chunk, typename Index>
struct Writer {

  Blob::FD fd;
  std::string sourceFilename;
  std::string destFilename;
  uint64_t sizeHeaderOffset = 0;
  SizeHeader<Chunk, Index> sizes;
  ErrorMode errorMode{ErrorMode::Assert};

  void header(
    const std::string& path,
    const Magic& magic,
    Version version,
    ErrorMode mode = ErrorMode::Assert
  ) {
    headerImpl(
      path,
      magic,
      version,
      O_CLOEXEC | O_CREAT | O_TRUNC | O_WRONLY,
      mode
    );
  }

  void exclusiveHeader(
    const std::string& path,
    const Magic& magic,
    Version version,
    ErrorMode mode = ErrorMode::Assert
  ) {
    headerImpl(
      path,
      magic,
      version,
      O_CLOEXEC | O_CREAT | O_EXCL | O_WRONLY,
      mode
    );
  }

private:
  void headerImpl(
    const std::string& path,
    const Magic& magic,
    Version version,
    int openFlags,
    ErrorMode mode
  ) {
    errorMode = mode;
    sourceFilename = fmt::format("{}.part", path);
    destFilename = path;
    fd = Blob::FD{sourceFilename, openFlags, errorMode};

    fd.write(magic.data(), sizeof(Magic));
    fd.writeInt(version);

    auto const repoSchema = repoSchemaId();
    always_assert(repoSchema.size() < 256);
    fd.writeInt((uint8_t)repoSchema.size());
    fd.write(repoSchema.data(), repoSchema.size());

    sizeHeaderOffset =
      sizeof(Magic) + sizeof(Version) +
      sizeof(uint8_t) + repoSchema.size();
    // Sizes go here: We don't know the sizes yet, so we'll just write
    // zeros. Afterwards we'll go back and overwrite it with the actual
    // sizes.
    sizes.write(fd);
  }

public:
  void write(Chunk chunk, const void* data, size_t size) {
    fd.write(data, size);
    sizes.add(chunk, size);
  }

  /**
  * The Hash Map Index.
  * Contains two part the index and the data.
  * The index is just the buckets and contains the offset where to find the data
  * for that bucket. Each offset is stored using fixed with uint32_t. So we can
  * seek into it.
  * The data for each bucket is 0 or more items. Starts with the key and then
  * the item data.
  *
  * Layout:
  * Index (N + 1 offsets)
  * ----------------------------------------------------------------------------
  * | offset 0  | offset 1 | offset 2 | offset 3 | ...                         |
  * ----------------------------------------------------------------------------
  * Data (N buckets)
  * ----------------------------------------------------------------------------
  * | key0, item0 data, key1, item1 data | | key2, item2 data | ...            |
  * ----------------------------------------------------------------------------
  *
  * How to read:
  * h = hash(key)
  * bucket = h % N
  * // We read the current bucket and next bucket to know when our bucket ends
  * start_offset, end_offset = read(bucket, 2)
  * blob = read(start_offset, end_offset - start_offset)
  * // iterate through blob and check if the key is there. If it is return data
  * while (blob.remaining() > 0)
  *   candidate_key = blob.read()
  *   data = blob.read()
  *   if (key == candidate_key)
  *     return data
  * return null
  */
  template <typename T, typename KeyCompare, typename L, typename KF, typename VF>
  void hashMapIndex(Index index, L& list, KF key_lambda, VF value_lambda) {
    hashMapIndex<T, std::string, KeyCompare>(
      index, list,
      [&](auto const& it) { return std::string{key_lambda(it)}; },
      value_lambda);
  }

  // Variant for non-string key types (e.g. SHA1). Keys are serialized via
  // serde instead of as length-prefixed strings. Pair with the StoredKey
  // parameter on getFromIndex() on the reader side.
  template <typename T, typename Key, typename KeyCompare,
            typename L, typename KF, typename VF>
  void hashMapIndex(Index index, L& list, KF key_lambda, VF value_lambda) {
    BlobEncoder indexBlob;
    BlobEncoder dataBlob;
    auto num_buckets = list.size();

    struct Item {
      Key key;
      const T* value;
    };
    using Bucket = std::vector<Item>;
    auto buckets = std::vector<Bucket>(num_buckets);

    auto compare = KeyCompare{};

    for (auto const& it : list) {
      auto key = key_lambda(it);
      auto hash = compare.hash(key);
      auto bucket = hash % num_buckets;
      buckets[bucket].push_back(Item { key, value_lambda(it) });
    }
    for (auto const& bucket : buckets) {
      auto offset = dataBlob.size();
      indexBlob.fixedWidth(uint32_t(offset));
      for (auto& item : bucket) {
        dataBlob(item.key)(*(item.value));
      }
    }
    if (errorMode == ErrorMode::Throw &&
        dataBlob.size() > std::numeric_limits<uint32_t>::max()) {
      fail(
        errorMode,
        "Hash-map index data for {} is {} bytes, exceeding the {}-byte "
        "addressable limit",
        destFilename,
        dataBlob.size(),
        std::numeric_limits<uint32_t>::max()
      );
    }
    indexBlob.fixedWidth(uint32_t(dataBlob.size()));

    fd.write(indexBlob.data(), indexBlob.size());
    fd.write(dataBlob.data(), dataBlob.size());
    sizes.set(index, indexBlob.size(), dataBlob.size());
  }

  /**
  * The Hash List Index.
  * Contains two part the index and the data.
  * The index is just the offsets so we can find the data. Each offset is stored
  * using fixed with uint32_t. So we can seek into it.
  * The data for each bucket contains the data for that item
  *
  * Layout:
  * Index (N + 1 offsets)
  * ----------------------------------------------------------------------------
  * | offset 0  | offset 1 | offset 2 | offset 3 | ...                         |
  * ----------------------------------------------------------------------------
  * Data (N buckets)
  * ----------------------------------------------------------------------------
  * | item0 data | item1 data | item2 data | ...                               |
  * ----------------------------------------------------------------------------
  *
  * How to read:
  * // We read the current offset and next offset to know when our data ends
  * start_offset, end_offset = read(id, 2)
  * blob = read(start_offset, end_offset - start_offset)
  * return blob.read()
  */
  template <typename T>
  std::vector<Blob::Bounds> listIndex(Index index, std::vector<T> list) {
    BlobEncoder indexBlob;
    BlobEncoder dataBlob;

    std::vector<Blob::Bounds> bounds;
    bounds.reserve(list.size());
    size_t offset = 0;
    for (auto& item : list) {
      indexBlob.fixedWidth(uint32_t(dataBlob.size()));
      dataBlob(item);
      auto size = dataBlob.size() - offset;
      bounds.push_back(Blob::Bounds { offset, size });
      offset += size;
    }
    indexBlob.fixedWidth(uint32_t(dataBlob.size()));

    fd.write(indexBlob.data(), indexBlob.size());
    fd.write(dataBlob.data(), dataBlob.size());
    sizes.set(index, indexBlob.size(), dataBlob.size());
    return bounds;
  }

  void finish() {
    // All the sizes are updated, so now go patch the size table.
    fd.seek(sizeHeaderOffset);
    sizes.write(fd);

    // Signify completion of the file by renaming it to its final name.
    if (::rename(sourceFilename.c_str(), destFilename.c_str()) < 0) {
      auto const error = folly::errnoStr(errno);
      fail(
        errorMode,
        "Unable to rename {} to {}: {}",
        sourceFilename,
        destFilename,
        error
      );
    }
  }
};

////////////////////////////////////////////////////////////////////////////////

template <typename Chunk, typename Index>
struct Offsets {
  static constexpr size_t ChunkSize = magic_enum::enum_count<Chunk>();
  static constexpr size_t IndexSize = magic_enum::enum_count<Index>();

  uint64_t chunks[ChunkSize] = { 0 };
  uint64_t indexes[IndexSize * 2] = { 0 };

  size_t init(size_t offset, SizeHeader<Chunk, Index> sizes) {
    for (auto i = 0; i < ChunkSize; i++) {
      chunks[i] = offset;
      offset += sizes.chunks[i];
    }
    for (auto i = 0; i < IndexSize * 2; i++) {
      indexes[i] = offset;
      offset += sizes.indexes[i];
    }
    return offset;
  }

  size_t get(Chunk chunk) const {
    return chunks[uint32_t(chunk)];
  }
};

////////////////////////////////////////////////////////////////////////////////

template <typename Chunk, typename Index>
struct Reader {

  Blob::FD fd;
  std::string path;
  uint64_t fileSize = 0;
  SizeHeader<Chunk, Index> sizes;
  Offsets<Chunk, Index> offsets;
  ErrorMode errorMode{ErrorMode::Assert};

  void init(const std::string& p, const Magic& expectedMagic,
            Version expectedVersion,
            ReadMode mode = ReadMode::PReadOnly,
            ErrorMode errors = ErrorMode::Assert) {
    path = p;
    errorMode = errors;
    fd = Blob::FD{path, O_CLOEXEC | O_RDONLY, errorMode};

    {
      Magic magic;
      fd.read(magic.data(), sizeof(magic));
      if (magic != expectedMagic) {
        fail(errorMode, "Incorrect magic bytes in {}", path);
      }
    }

    {
      auto const version = fd.readInt<Version>();
      if (version != expectedVersion) {
        fail(
          errorMode,
          "Unsupported version in {} (expected {}, got {})",
          path,
          expectedVersion,
          version
        );
      }
    }

    auto const repoSchemaSize = fd.readInt<uint8_t>();
    {
      std::string repoSchema;
      repoSchema.resize(repoSchemaSize);
      fd.read(repoSchema.data(), repoSchema.size());
      if (repoSchema != repoSchemaId()) {
        fail(
          errorMode,
          "Mismatched repo-schema in {} (expected {}, got {})",
          path,
          repoSchemaId(),
          repoSchema
        );
      }
    }

    {
      sizes.read(fd);
      fileSize = fd.fileSize();

      auto offset =
        sizeof(Magic) + sizeof(Version) +
        sizeof(uint8_t) + repoSchemaSize +
        SizeHeader<Chunk, Index>::kFileSize;

      offset = offsets.init(offset, sizes);

      if (offset != fileSize) {
        fail(
          errorMode,
          "Corrupted size table for {}: "
          "calculated end of data offset {} does not match file size {}",
          path,
          offset,
          fileSize
        );
      }
    }

    if (mode == ReadMode::MMap) {
      fd.enableMmap(fileSize);
    }
  }

  template <typename KeyCompare>
  Blob::HashMapIndex<KeyCompare> hashMapIndex(Index index) {
    auto i = uint32_t(index);
    auto indexOffset = offsets.indexes[i * 2];
    auto indexSize = sizes.indexes[i * 2];
    auto dataOffset = offsets.indexes[i * 2 + 1];
    auto dataSize = sizes.indexes[i * 2 + 1];
    return Blob::HashMapIndex<KeyCompare>(
      Blob::Bounds { indexOffset, indexSize },
      Blob::Bounds { dataOffset, dataSize });
  }

  Blob::ListIndex listIndex(Index index) {
    auto i = uint32_t(index);
    auto indexOffset = offsets.indexes[i * 2];
    auto indexSize = sizes.indexes[i * 2];
    auto dataOffset = offsets.indexes[i * 2 + 1];
    auto dataSize = sizes.indexes[i * 2 + 1];
    return Blob::ListIndex(Blob::Bounds { indexOffset, indexSize },
                           Blob::Bounds { dataOffset, dataSize });
  }

  template <typename T, typename StoredKey = std::string,
            typename KeyCompare, typename LookupKey>
  Optional<T> getFromIndex(const Blob::HashMapIndex<KeyCompare>& map,
                           const LookupKey& key) const {
    if (map.size == 0) {
      return {};
    }

    auto compare = KeyCompare{};

    auto hash = compare.hash(key);
    auto bucket = hash % map.size;

    typename Blob::HashMapIndex<KeyCompare>::Bucket currentOffset;
    typename Blob::HashMapIndex<KeyCompare>::Bucket nextOffset;
    assertx(sizeof(currentOffset) * bucket < map.indexBounds.size);
    assertx(sizeof(currentOffset) * bucket + sizeof(currentOffset) * 2
            <= map.indexBounds.size);
    auto indexBlob = fd.readBlob(
      map.indexBounds.offset + sizeof(currentOffset) * bucket,
      sizeof(currentOffset) * 2);
    indexBlob.decoder.fixedWidth(currentOffset);
    indexBlob.decoder.fixedWidth(nextOffset);

    if (errorMode == ErrorMode::Throw) {
      if (nextOffset < currentOffset ||
          currentOffset > map.dataBounds.size ||
          nextOffset > map.dataBounds.size) {
        fail(
          errorMode,
          "Corrupt hash-map index in {}: bucket data range [{}, {}] is out of "
          "bounds (index data size {})",
          path,
          currentOffset,
          nextOffset,
          map.dataBounds.size
        );
      }
    } else {
      assertx(currentOffset <= map.dataBounds.size);
      assertx(nextOffset <= map.dataBounds.size);
    }

    if (currentOffset == nextOffset) {
      return {};
    }

    auto dataBlob = fd.readBlob(map.dataBounds.offset + currentOffset,
                                nextOffset - currentOffset);

    while (dataBlob.decoder.remaining() > 0) {
      StoredKey candidate_key;
      dataBlob.decoder(candidate_key);

      T res;
      dataBlob.decoder(res);
      if (compare.equal(candidate_key, key)) {
        return { std::move(res) };
      }
    }
    return {};
  }

  template <typename T>
  Optional<T> getFromIndex(const Blob::ListIndex& list, int64_t index) const {
    if (index < 0 || index >= list.size) {
      return {};
    }

    Blob::ListIndex::Bucket currentOffset;
    Blob::ListIndex::Bucket nextOffset;
    assertx(sizeof(currentOffset) * index < list.indexBounds.size);
    assertx(sizeof(currentOffset) * index + sizeof(currentOffset) * 2
            <= list.indexBounds.size);
    auto indexBlob = fd.readBlob(
      list.indexBounds.offset + sizeof(currentOffset) * index,
      sizeof(currentOffset) * 2);
    indexBlob.decoder.fixedWidth(currentOffset);
    indexBlob.decoder.fixedWidth(nextOffset);

    assertx(currentOffset < list.dataBounds.size);
    assertx(nextOffset <= list.dataBounds.size);
    auto dataBlob = fd.readBlob(list.dataBounds.offset + currentOffset,
                                    nextOffset - currentOffset);
    T res;
    dataBlob.decoder(res);
    return { res };
  }

  void check(Chunk chunk, size_t limit) {
    auto size = sizes.get(chunk);
    auto offset = offsets.get(chunk);
    if (limit != 0 && size > limit) {
      fail(
        errorMode,
        "Invalid section size for {}: Chunk {} is {} (larger than limit of {})",
        path,
        uint32_t(chunk),
        size,
        limit
      );
    }
    if (offset > fileSize) {
      fail(
        errorMode,
        "Corrupted size table for {}: "
        "Chunk {} starts at {} (is greater than file size {})",
        path,
        uint32_t(chunk),
        offset,
        fileSize
      );
    }
  }

  void check(Index index, size_t indexLimit, size_t dataLimit) {
    auto index_int = uint32_t(index);
    auto indexSize = sizes.indexes[index_int * 2];
    auto indexOffset = offsets.indexes[index_int * 2];
    if (indexSize > indexLimit) {
      fail(
        errorMode,
        "Invalid section size for {}: Index {} is {} (larger than limit of {})",
        path,
        index_int,
        indexSize,
        indexLimit
      );
    }
    if (indexOffset > fileSize) {
      fail(
        errorMode,
        "Corrupted size table for {}: "
        "Index {} starts at {} (is greater than file size {})",
        path,
        index_int,
        indexOffset,
        fileSize
      );
    }

    auto dataSize = sizes.indexes[index_int * 2 + 1];
    auto dataOffset = offsets.indexes[index_int * 2 + 1];
    if (dataSize > dataLimit) {
      fail(
        errorMode,
        "Invalid section size for {}: "
        "Index Data {} is {} (larger than limit of {})",
        path,
        index_int,
        dataSize,
        dataLimit
      );
    }
    if (dataOffset > fileSize) {
      fail(
        errorMode,
        "Corrupted size table for {}: "
        "Index Data {} starts at {} (is greater than file size {})",
        path,
        index_int,
        dataOffset,
        fileSize
      );
    }
  }

};

// We can't use the hardware accelerated hash functions because different
// hardware has different hash functions. We need a hash function that is stable
// between different hardware implementations.
struct CaseSensitiveCompare {
  bool equal(const std::string_view& s1, const std::string_view& s2) const {
    return s1 == s2;
  }
  size_t hash(const std::string_view& s) const {
    return hash_string_cs_software(s.data(), s.size());
  }
};

struct CaseInsensitiveCompare {
  bool equal(const std::string_view& s1, const std::string_view& s2) const {
    return bstrcasecmp(s1, s2) == 0;
  }
  size_t hash(const std::string_view& s) const {
    return hash_string_i_software(s.data(), s.size());
  }
};

}

////////////////////////////////////////////////////////////////////////////////

}
