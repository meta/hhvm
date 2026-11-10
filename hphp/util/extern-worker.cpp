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

#include "hphp/util/extern-worker.h"

#include "hphp/util/assertions.h"
#include "hphp/util/current-executable.h"
#include "hphp/util/debug-format.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/logger.h"
#include "hphp/util/match.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/subprocess-scheduler.h"
#include "hphp/util/trace.h"
#include "hphp/util/zstd.h"

#include <folly/FileUtil.h>
#include <fmt/core.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/memory/UninitializedMemoryHacks.h>

#include <boost/filesystem.hpp>

#include <exception>
#include <filesystem>
#include <mutex>
#include <stack>

namespace fs = std::filesystem;

namespace coro = folly::coro;

namespace HPHP::extern_worker {

//////////////////////////////////////////////////////////////////////

using namespace detail;
using namespace folly::gen;

//////////////////////////////////////////////////////////////////////

const char* const s_option = "--extern-worker";

std::atomic<uint64_t> RequestId::s_next{0};
std::atomic<uint64_t> RequestId::s_active{0};

const std::array<OutputType, 1> Client::s_valOutputType{OutputType::Val};
const std::array<OutputType, 1> Client::s_vecOutputType{OutputType::Vec};
const std::array<OutputType, 1> Client::s_optOutputType{OutputType::Opt};

ImplHook g_impl_hook{nullptr};

thread_local bool g_in_job{false};

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(extern_worker)

//////////////////////////////////////////////////////////////////////

// If passed via the command-line, the worker is running in "local"
// mode and will use a different mechanism to read inputs/write
// outputs.
const char* const g_local_option = "--local";

// If running in local mode, the FD of the pipe used to communicate
// output back to the parent.
constexpr int g_local_pipe_fd = 3;

// For the subprocess impl, if the size of the data is <= this
// constant, we'll store it "inline" in the ref itself, rather than
// writing it to disk. This values probably needs more tuning.
constexpr size_t g_inline_size = 256;

// Blob writers buffer compressed blobs until they have this many bytes
// (or are flushed) rather than writing each one separately. Most blobs
// are small, and some filesystems (like btrfs) do expensive per-write
// work.
constexpr size_t g_blob_write_buffer_size = 1 << 20;

// The compression level is between 0 (no compression) and 22 (lots of
// computation, smaller resulting artifacts). We can probably tune
// this further.
size_t g_zstd_compression_level = 10;

// A well-known name for where ZSTD dictionaries are expected to live for SerializedSource/Sinks.
const char* const g_zstddict_filename = "zstd.dict";

fs::path zstdDictLocation(const fs::path& root) {
  return root / g_zstddict_filename;
}

std::pair<ZSTD_CDict*, ZSTD_DDict*> createZstdDictionaries(const fs::path& dictPath)
{
  std::string dictContent;
  if (!folly::readFile(dictPath.c_str(), dictContent)) {
    // Fail silently if the expected path isn't a ZSTD dict.
    return {nullptr, nullptr};
  }
  auto cdict =
    ZSTD_createCDict(dictContent.data(), dictContent.size(), g_zstd_compression_level);
  assertx(cdict != nullptr);
  auto ddict = ZSTD_createDDict(dictContent.data(), dictContent.size());
  assertx(ddict != nullptr);
  FTRACE(3, "ZSTD compression dictionary initialized from `{}`\n", dictPath.native());
  return {cdict, ddict};
}

RefId inlineRefId(const std::string blobToInline) {
  DEBUG_ONLY auto const sz = blobToInline.size();
  assertx(sz <= g_inline_size);
  return RefId{std::move(blobToInline), blobToInline.size(), 0};
}

//////////////////////////////////////////////////////////////////////

struct Registry {
  hphp_fast_string_map<JobBase*> registry;
  std::mutex lock;
};

Registry& registry() {
  static Registry registry;
  return registry;
}

//////////////////////////////////////////////////////////////////////

// Represents an open file. Used this over readFile/writeFile if you
// want to maintain a persistent FD, or want to just read/write just a
// portion of the file. Only reading or appending to the file is
// supported.
struct FD {
  FD(const fs::path& path, bool read, bool write, bool create)
      : m_path{path}, m_offset{0} {
    assertx(IMPLIES(create, write));

    auto flags = O_CLOEXEC;
    if (read) {
      flags |= (write ? O_RDWR : O_RDONLY);
    } else {
      assertx(write);
      flags |= O_WRONLY;
    }

    if (write) flags |= O_APPEND;
    if (create) {
      flags |= O_CREAT | O_EXCL;
      // We're creating it, so the file is empty
      m_offset = 0;
    }

    auto fd = folly::openNoInt(m_path.c_str(), flags);
    if (fd < 0) {
      throw Error{
        fmt::format(
          "Unable to open {} [{}]",
          m_path.native(), folly::errnoStr(errno)
        )
      };
    }
    m_fd = fd;
    SCOPE_FAIL { ::close(m_fd); };

    // If we're going to write to the file, but not creating it, we
    // don't know what the end of the file is, so find the current
    // offset.
    if (write && !create) syncOffset();
  }
  ~FD() { if (m_fd >= 0) ::close(m_fd); }

  FD(const FD&) = delete;
  FD(FD&& o) noexcept : m_fd{o.m_fd}, m_path{o.m_path}, m_offset{o.m_offset}
  { o.m_fd = -1; }
  FD& operator=(const FD&) = delete;
  FD& operator=(FD&& o) noexcept {
    std::swap(m_fd, o.m_fd);
    std::swap(m_path, o.m_path);
    std::swap(m_offset, o.m_offset);
    return *this;
  }

  const fs::path& path() const { return m_path; }

  std::string read(size_t offset, size_t size) const {
    assertx(m_fd >= 0);

    std::string data;
    folly::resizeWithoutInitialization(data, size);

    auto const read = folly::preadFull(m_fd, data.data(), size, offset);
    if (read == size) return data;
    if (read < 0) {
      throw Error{
        fmt::format(
          "Failed reading {} bytes from {} at {} [{}]",
          size, m_path.native(), offset, folly::errnoStr(errno)
        )
      };
    }
    throw Error{
      fmt::format(
        "Partial read from {} at {} (expected {}, actual {})",
        m_path.native(), offset, size, read
      )
    };
  }

  size_t append(const std::string& data) {
    assertx(m_fd >= 0);

    auto const written = folly::writeFull(m_fd, data.data(), data.size());
    if (written < 0) {
      throw Error{
        fmt::format(
          "Failed writing {} bytes to {} [{}]",
          data.size(), m_path.native(), folly::errnoStr(errno)
        )
      };
    }
    if (written != data.size()) {
      throw Error{
        fmt::format(
          "Partial write to {} (expected {}, actual {})",
          m_path.native(), data.size(), written
        )
      };
    }
    auto const prev = m_offset;
    m_offset += written;
    return prev;
  }

  // Update m_offset to the end of the file. This is only needed if
  // you've opened an already created file, or if you know someone
  // else has written to it.
  void syncOffset() {
    assertx(m_fd >= 0);
    auto const size = ::lseek(m_fd, 0, SEEK_END);
    if (size < 0) {
      throw Error{
        fmt::format(
          "Unable to seek to end of {}",
          m_path.native()
        )
      };
    }
    m_offset = size;
  }

private:
  int m_fd;
  fs::path m_path;
  size_t m_offset;
};

// Encapsulates a blob file for reading. Concurrent reads are OK.
//
// All blobs are assumed to be stored as individual ZSTD-compressed blobs, and
// the read API (readAndDecompress) requires a buffer & decompression context
// to be passed by the caller - this is to allow re-using the same memory
// when we're reading blobs from lots of different files.
struct BlobReader {
  BlobReader(const fs::path& path) {
    auto fd = folly::openNoInt(path.c_str(), O_CLOEXEC | O_RDONLY);
    if (fd < 0) {
      throw Error{
        fmt::format(
          "Unable to open {} [{}]",
          path.native(), folly::errnoStr(errno)
        )
      };
    }
    FTRACE(3, "Readable blob opened for {} as fd {}\n", path.native(), fd);
    m_fd = fd;
  }

  ~BlobReader() { if (m_fd >= 0) ::close(m_fd); }
  BlobReader(const BlobReader&) = delete;
  BlobReader(BlobReader&& o) noexcept : m_fd{o.m_fd}
  { o.m_fd = -1; }
  BlobReader& operator=(const BlobReader&) = delete;
  BlobReader& operator=(BlobReader&& o) noexcept {
    std::swap(m_fd, o.m_fd);
    return *this;
  }

  // To save memory, the caller provides a reused string to hold the
  // compressed data we read from the file and we re-use a decompression
  // context.
  std::string readAndDecompress(
    const RefId::OffsetAndLength& offsetAndLength,
    std::string& compressedBuffer,
    ZSTD_DCtx* dctx,
    ZSTD_DDict* ddict) const {
    assertx(m_fd >= 0);
    assertx(offsetAndLength.length > 0);

    folly::resizeWithoutInitialization(compressedBuffer, offsetAndLength.length);
    auto const read = folly::preadFull(m_fd, compressedBuffer.data(),
                                       offsetAndLength.length, offsetAndLength.offset);
    if (static_cast<uint32_t>(read) != offsetAndLength.length) {
      throw Error{
        fmt::format(
          "Blob failed reading {} bytes from {} at {}, actual read {} [{}]",
          offsetAndLength.length, m_fd, offsetAndLength.offset, read, folly::errnoStr(errno)
        )
      };
    }
    size_t decompressedSize =
      ZSTD_getFrameContentSize(compressedBuffer.data(), compressedBuffer.size());
    if (decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN
        || decompressedSize == ZSTD_CONTENTSIZE_ERROR) {
      decompressedSize = ZSTD_decompressBound(compressedBuffer.data(), compressedBuffer.size());
    }
    if (ZSTD_isError(decompressedSize)) {
      throw Error{
        fmt::format("ZSTD error {} while reading fd {}", 0 - decompressedSize, m_fd)
      };
    }
    std::string decompressed;
    folly::resizeWithoutInitialization(decompressed, decompressedSize);
    if (ddict != nullptr) {
      decompressedSize =
        ZSTD_decompress_usingDDict(dctx,
                                   decompressed.data(), decompressedSize,
                                   compressedBuffer.data(), compressedBuffer.size(),
                                   ddict);
    } else {
      decompressedSize =
        ZSTD_decompressDCtx(dctx,
                            decompressed.data(), decompressedSize,
                            compressedBuffer.data(), compressedBuffer.size());
    }
    if (ZSTD_isError(decompressedSize)) {
      throw Error{
        fmt::format("ZSTD error while decompressing: {}",
                       0 - decompressedSize)
      };
    }
    folly::resizeWithoutInitialization(decompressed, decompressedSize);
    return decompressed;
  }

private:
  int m_fd;
};

// Represents a blob file for writing by a single thread. Blob RefId's are
// obtained by appending the blob to the file. All writes are compressed
// with zstd (using a compression dict if it is available) as individual
// frames. Writes are buffered, so a blob can only be read from the file
// after flush().
//
// If create is set to true, we create the file and throw if the file exists.
// Otherwise, we open the file in append mode and seek to the end.
struct BlobWriter {
  BlobWriter(const fs::path& path, bool create, ZSTD_CDict* compressionDict)
  : m_path{path}, m_offset{0}, m_compressionDict{compressionDict} {
    auto flags = O_CLOEXEC;
    if (create) {
      flags |= O_CREAT | O_WRONLY | O_EXCL;
    } else {
      flags |= O_WRONLY | O_APPEND;
    }
    auto fd = folly::openNoInt(m_path.c_str(), flags);
    if (fd < 0) {
      throw Error{
        fmt::format(
          "Unable to open {} [{}]",
          m_path.native(), folly::errnoStr(errno)
        )
      };
    }
    m_fd = fd;
    m_cctx = ZSTD_createCCtx();
    SCOPE_FAIL { ::close(m_fd); };
    if (m_cctx == nullptr) {
      throw Error{
        fmt::format(
          "Unable to create ZSTD context for {}",
          m_path.native()
        )
      };
    }
    SCOPE_FAIL { ZSTD_freeCCtx(m_cctx); };
    if (!create) syncOffset();
  }

 ~BlobWriter() {
    // Buffered blobs are only dropped if we're unwinding from an error.
    assertx(m_compressedBuffer.empty() || std::uncaught_exceptions() > 0);
    if (m_fd >= 0) ::close(m_fd);
    if (m_cctx != nullptr) ZSTD_freeCCtx(m_cctx);
  }

  BlobWriter(const BlobWriter&) = delete;
  BlobWriter(BlobWriter&& o) noexcept
  : m_path{std::move(o.m_path)}, m_fd{o.m_fd}, m_offset{o.m_offset}
  , m_compressedBuffer{std::move(o.m_compressedBuffer)}
  , m_cctx{o.m_cctx}, m_compressionDict{o.m_compressionDict}
  {
    o.m_fd = -1;
    o.m_cctx = nullptr;
    o.m_compressionDict = nullptr;
  }
  BlobWriter& operator=(const BlobWriter&) = delete;
  BlobWriter& operator=(BlobWriter&& o) noexcept {
    std::swap(m_fd, o.m_fd);
    std::swap(m_cctx, o.m_cctx);
    std::swap(m_path, o.m_path);
    std::swap(m_compressedBuffer, o.m_compressedBuffer);
    std::swap(m_offset, o.m_offset);
    std::swap(m_compressionDict, o.m_compressionDict);
    return *this;
  }


  const fs::path& path() const { return m_path; }

  void syncOffset() {
    assertx(m_fd >= 0);
    assertx(m_compressedBuffer.empty());
    auto const size = ::lseek(m_fd, 0, SEEK_END);
    if (size < 0) {
      throw Error{
        fmt::format(
          "Unable to seek to end of {}",
          m_path.native()
        )
      };
    }
    m_offset = size;
  }

  // For long-lived blobs, it's useful to occasionally free the buffer
  // that contains the compressed strings we're writing to the filesystem.
  void freeBuffer() {
    assertx(m_compressedBuffer.empty());
    m_compressedBuffer.shrink_to_fit();
  }

  // Write any buffered blobs to the file.
  void flush() {
    if (m_compressedBuffer.empty()) return;
    assertx(m_fd >= 0);
    auto const written = folly::writeFull(
      m_fd, m_compressedBuffer.data(), m_compressedBuffer.size()
    );
    if (written != m_compressedBuffer.size()) {
      throw Error{
        fmt::format(
          "Failed writing to {} (expected {}, actual {})",
          m_path.native(), m_compressedBuffer.size(), written
        )
      };
    }
    m_compressedBuffer.clear();
  }

  RefId compressAndAppend(const std::string& data) {
    assertx(m_cctx != nullptr);

    FTRACE(5, "Compress blob request for size {}\n", data.size());
    auto const size = compressBlobToBuffer(data);
    if (m_compressedBuffer.size() >= g_blob_write_buffer_size) flush();
    auto const offset = m_offset;
    m_offset += size;
    assertx(offset < std::numeric_limits<uint32_t>::max());
    assertx(size < std::numeric_limits<uint32_t>::max());
    return RefId{
      m_path.filename().native(),
      data.size(),
      static_cast<uint32_t>(offset),
      static_cast<uint32_t>(size)
    };
  }

private:
  // Compress the blob onto the end of `m_compressedBuffer`, returning
  // its compressed size.
  size_t compressBlobToBuffer(const std::string& blob) {
    assertx(blob.size() > g_inline_size);
    auto const start = m_compressedBuffer.size();
    const size_t compressedSizeMax = ZSTD_compressBound(blob.size());
    // resizeWithoutInitialization() reserves exactly what it needs, and
    // with some standard libraries reserve() doesn't grow geometrically,
    // so appending would copy the buffer every time.
    auto const needed = start + compressedSizeMax;
    if (needed > m_compressedBuffer.capacity()) {
      m_compressedBuffer.reserve(
        std::max(needed, 2 * m_compressedBuffer.capacity())
      );
    }
    folly::resizeWithoutInitialization(m_compressedBuffer, needed);
    auto const dest = m_compressedBuffer.data() + start;

    size_t compressedSize;
    if (m_compressionDict != nullptr ) {
      compressedSize =
        ZSTD_compress_usingCDict(m_cctx, dest, compressedSizeMax,
                                blob.data(), blob.size(), m_compressionDict);
    } else {
      compressedSize =
        ZSTD_compressCCtx(m_cctx, dest, compressedSizeMax,
                          blob.data(), blob.size(), g_zstd_compression_level);
    }
    if (ZSTD_isError(compressedSize)) {
      m_compressedBuffer.resize(start);
      throw Error{
        fmt::format("ZSTD error while compressing: {}",
                      0 - compressedSize)
      };
    }
    folly::resizeWithoutInitialization(
      m_compressedBuffer, start + compressedSize
    );
    return compressedSize;
  }

  fs::path m_path;
  int m_fd;
  size_t m_offset;
  // Compressed blobs not yet written to the file.
  std::string m_compressedBuffer;
  ZSTD_CCtx* m_cctx;
  ZSTD_CDict* m_compressionDict;
};
//////////////////////////////////////////////////////////////////////

// Read from the FD (which is assumed to be a pipe) and append the
// contents to the given string. Return true if the pipe is now
// closed, or false otherwise. The false case can only happen if the
// FD is non-blocking.
bool readFromPipe(int fd, std::string& s) {
  auto realEnd = s.size();
  SCOPE_EXIT { s.resize(realEnd); };
  while (true) {
    assertx(realEnd <= s.size());
    auto spaceLeft = s.size() - realEnd;
    while (spaceLeft < 1024) {
      folly::resizeWithoutInitialization(
        s, std::max<size_t>(4096, s.size() * 2)
      );
      spaceLeft = s.size() - realEnd;
    }
    auto const read =
      folly::readNoInt(fd, s.data() + realEnd, spaceLeft);
    if (read < 0) {
      if (errno == EAGAIN) return false;
      throw Error{
        fmt::format(
          "Failed reading from pipe {} [{}]",
          fd,
          folly::errnoStr(errno)
        )
      };
    } else if (read == 0) {
      return true;
    }
    realEnd += read;
  }
}

// Read from the FD (which is assumed to be a pipe) until it is closed
// (returns EOF). Returns all data read. Only use this with a blocking
// FD, as it will spin otherwise.
std::string readFromPipe(int fd) {
  std::string out;
  while (!readFromPipe(fd, out)) {}
  return out;
}

// Write all of the given data to the FD (which is assumed to be a
// pipe).
void writeToPipe(int fd, const char* data, size_t size) {
  auto const written = folly::writeFull(fd, data, size);
  if (written < 0) {
    throw Error{
      fmt::format(
        "Failed writing {} bytes to pipe {} [{}]",
        size, fd, folly::errnoStr(errno)
      )
    };
  }
  if (written != size) {
    throw Error{
      fmt::format(
        "Partial write to pipe {} (expected {}, actual {})",
        fd, size, written
      )
    };
  }
}

//////////////////////////////////////////////////////////////////////

/*
 * "Serialized" API:
 *
 * For this mode a worker expects to be invoked with --local, followed
 * by the name of the command, the root of the blob files, and the
 * name of the blob file to write results to.
 *
 * This mode uses blob files written locally, and communicates its
 * input/output via serialized RefIds via stdin and pipes. It's meant
 * for SubprocessImpl and is more efficient than the "File" mode
 * below.
 *
 * See SubprocessImpl for more description.
 */

struct SerializedSource : public detail::ISource {
  SerializedSource(fs::path root, std::string s, ZSTD_CDict* cdict,
                   ZSTD_DDict* ddict)
    : m_source{std::move(s)}
    , m_decoder{m_source.data(), m_source.size()}
    , m_currentInput{0}
    , m_numInputs{0}
    , m_cdict{cdict}
    , m_ddict{ddict}
    , m_root{std::move(root)}
  {
    // Look for the well-known zstd dictionary location which should be
    // placed under the input root by the scheduler.
    auto zstd_dict_path = m_root / g_zstddict_filename;
    m_dctx = ZSTD_createDCtx();
    if (m_dctx == nullptr) {
      throw Error{"Unable to create ZSTD decompression context"};
    }
  }

  ~SerializedSource() {
    ZSTD_freeDCtx(m_dctx);
  }

  std::string blob() override {
    return refToBlob(decodeRefId(m_decoder));
  }
  Optional<std::string> optBlob() override {
    auto r = decodeOptRefId(m_decoder);
    if (!r) return std::nullopt;
    return refToBlob(*r);
  }

  BlobVec variadic() override {
    return from(decodeRefIdVec(m_decoder))
      | map([&] (const RefId& r) { return refToBlob(r); })
      | as<std::vector>();
  }

  void initDone() override {
    assertx(m_numInputs == 0);
    assertx(m_currentInput == 0);
    m_decoder(m_numInputs);
  }
  bool inputEnd() const override {
    return m_currentInput >= m_numInputs;
  }
  void nextInput() override {
    assertx(!inputEnd());
    ++m_currentInput;
  }
  void finish() override { m_decoder.assertDone(); }

  static RefId decodeRefId(BlobDecoder& d) {
    // Optimize RefId representation for space. If size is <= the
    // inline size, we know that m_offsetAndLength is zero, and
    // that m_id has the same length as m_size, so we do not need to
    // encode it twice.
    decltype(RefId::m_size) size;
    d(size);
    if (size <= g_inline_size) {
      assertx(d.remaining() >= size);
      std::string id{(const char*)d.data(), size};
      d.advance(size);
      return inlineRefId(std::move(id));
    } else {
      decltype(RefId::m_id) id;
      decltype(RefId::OffsetAndLength::offset) offset;
      decltype(RefId::OffsetAndLength::length) length;
      d(id);
      d(offset);
      d(length);
      return RefId{std::move(id), static_cast<uint64_t>(size), offset, length};
    }
  }

  static Optional<RefId> decodeOptRefId(BlobDecoder& d) {
    bool present;
    d(present);
    if (!present) return std::nullopt;
    return decodeRefId(d);
  }

  static IdVec decodeRefIdVec(BlobDecoder& d) {
    std::vector<RefId> out;
    size_t size;
    d(size);
    out.reserve(size);
    std::generate_n(
      std::back_inserter(out),
      size,
      [&] { return decodeRefId(d); }
    );
    return out;
  }

private:
  std::string refToBlob(const RefId& r) {
    if (r.m_size <= g_inline_size) {
      assertx(!r.m_extra);
      return r.m_id;
    }

    fs::path path{r.m_id};
    if (path.is_absolute()) {
      return FD{path, true, false, false}.read(r.m_extra, r.m_size);
    }

    auto it = m_blobReaderCache.find(path.native());
    if (it == m_blobReaderCache.end()) {
      auto [elem, emplaced] = m_blobReaderCache.emplace(
        path.native(),
        BlobReader{m_root / path}
      );
      assertx(emplaced);
      it = elem;
    }
    return it->second.readAndDecompress(r.m_offsetAndLength, m_buffer, m_dctx, m_ddict);
  }

  std::string m_source;
  BlobDecoder m_decoder;
  size_t m_currentInput;
  size_t m_numInputs;
  std::string m_buffer;
  ZSTD_DCtx* m_dctx;

  ZSTD_CDict* m_cdict;
  ZSTD_DDict* m_ddict;

  fs::path m_root;
  hphp_fast_map<std::string, BlobReader> m_blobReaderCache;
};

struct SerializedSink : public detail::ISink {
  explicit SerializedSink(fs::path outputFile, ZSTD_CDict* cdict, ZSTD_DDict* ddict)
    : m_writer{outputFile, /* create */ false, cdict}
    , m_cdict(cdict)
    , m_ddict(ddict) {}


  ~SerializedSink() {
    ZSTD_freeCDict(m_cdict);
    ZSTD_freeDDict(m_ddict);
  }

  void blob(const std::string& b) override {
    encodeRefId(makeRefId(b), m_encoder);
  }
  void optBlob(const Optional<std::string>& b) override {
    encodeOptRefId(b ? makeRefId(*b) : Optional<RefId>{}, m_encoder);
  }
  void variadic(const BlobVec& v) override {
    auto const refs = from(v)
      | map([&] (const std::string& b) { return makeRefId(b); })
      | as<std::vector>();
    encodeRefIdVec(refs, m_encoder);
  }
  void nextOutput() override {}
  void startFini() override {}

  void finish() override {
    m_writer.flush();
    writeToPipe(
      g_local_pipe_fd,
      (const char*)m_encoder.data(),
      m_encoder.size()
    );
    ::close(g_local_pipe_fd);
  }

  static void encodeRefId(const RefId& r, BlobEncoder& e) {
    // Optimized RefId encoding. If it's an inline ref, we can encode
    // it more optimally. See above in SerializedSource::decodeRefId.
    e(r.m_size);
    if (r.m_size <= g_inline_size) {
      assertx(r.m_id.size() == r.m_size);
      assertx(!r.m_extra);
      e.writeRaw(r.m_id.data(), r.m_id.size());
    } else {
      e(r.m_id);
      e(r.m_offsetAndLength.offset);
      e(r.m_offsetAndLength.length);
    }
  }

  static void encodeOptRefId(const Optional<RefId>& r, BlobEncoder& e) {
    if (r) {
      e(true);
      encodeRefId(*r, e);
    } else {
      e(false);
    }
  }

  static void encodeRefIdVec(const IdVec& v, BlobEncoder& e) {
    e((size_t)v.size());
    for (auto const& r : v) encodeRefId(r, e);
  }

private:
  RefId makeRefId(const std::string& b) {
    if (b.size() <= g_inline_size) {
      return inlineRefId(b);
    }
    return m_writer.compressAndAppend(b);
  }

  BlobWriter m_writer;
  BlobEncoder m_encoder;

  ZSTD_CDict* m_cdict;
  ZSTD_DDict* m_ddict;
};

//////////////////////////////////////////////////////////////////////

/*
 * "File" API:
 *
 * For this mode a worker expects to be invoked with the name of the
 * command, followed by 3 paths to directories. The directories
 * represent the config inputs (for init()), the inputs (for multiple
 * runs()), and the last is the output directory, which will be
 * written to.
 *
 * All three directories use the same format for representing
 * data:
 *
 * - The first level contains numbered directories (from 0 to N). Each
 * numbered directory represents a set of inputs/outputs. For outputs,
 * the number corresponds to the matching number of the inputs. The
 * config directory does not have this level, as there's only ever one
 * of them.
 *
 * - The next level contains numbered files or directories (from 0 to
 * N). Each file/directory specifies a particular input. The exact
 * number depends on the job being executed (both sides need to
 * agree). The format of the file or directory depends on that
 * input/output's type.
 *
 * - If the input/output is a "normal" type, it will be a file
 * containing the serialized data (using BlobEncoder) for that
 * input/output. Strings are a special case. If the type is a
 * std::string, it will not be serialized and the string will be
 * stored directly (this makes it easier to represent files as their
 * contents).
 *
 * - If the input/output is an Opt<T>, it will be represented like a
 * "normal" type of type T, except it is not required to be
 * present. If the file is not present (so a gap in the numbering), it
 * is assumed to be std::nullopt.
 *
 * - If the input/output is a Variadic<T>, it will be a
 * directory. Inside of that directory will be numbered files from
 * 0-N, one for each element of the vector. The vector elements will
 * be encoded as "normal" types.
 *
 * NB: Marker types cannot nest, so there aren't any possible deeper
 * levels than this.
 */

struct FileSource : public detail::ISource {
  FileSource(fs::path config, fs::path input)
    : m_configPath{std::move(config)}
    , m_inputPath{std::move(input)}
    , m_itemIdx{0}
    , m_inputIdx{0}
    , m_itemBase{m_configPath} {}
  ~FileSource() = default;

  std::string blob() override {
    auto const filename = m_itemBase / folly::to<std::string>(m_itemIdx++);
    return detail::readFile(filename);
  }
  Optional<std::string> optBlob() override {
    auto const filename = m_itemBase / folly::to<std::string>(m_itemIdx++);
    if (!fs::exists(filename)) return std::nullopt;
    return detail::readFile(filename);
  }

  BlobVec variadic() override {
    BlobVec out;
    auto const vecBase = m_itemBase / folly::to<std::string>(m_itemIdx++);
    for (size_t i = 0;; ++i) {
      auto const valPath = vecBase / folly::to<std::string>(i);
      // A break in the numbering means the end of the vector.
      if (!fs::exists(valPath)) break;
      out.emplace_back(detail::readFile(valPath));
    }
    return out;
  }

  void initDone() override {
    assertx(m_itemBase == m_configPath);
    assertx(m_inputIdx == 0);
    m_itemIdx = 0;
    m_itemBase = m_inputPath / "0";
  }

  bool inputEnd() const override {
    assertx(m_itemBase == m_inputPath / folly::to<std::string>(m_inputIdx));
    return !fs::exists(m_itemBase);
  }

  void nextInput() override {
    assertx(m_itemBase == m_inputPath / folly::to<std::string>(m_inputIdx));
    m_itemIdx = 0;
    m_itemBase = m_inputPath / folly::to<std::string>(++m_inputIdx);
  }

  void finish() override {}
private:
  fs::path m_configPath;
  fs::path m_inputPath;

  size_t m_itemIdx;
  size_t m_inputIdx;
  fs::path m_itemBase;
};

struct FileSink : public detail::ISink {
  explicit FileSink(fs::path base)
    : m_base{std::move(base)}
    , m_itemIdx{0}
    , m_outputIdx{0}
  {
    // We insist on a clean output directory.
    if (!fs::create_directory(m_base)) {
      throw Error{
        fmt::format("Output directory {} already exists", m_base.native())
      };
    }
  }

  void blob(const std::string& b) override { write(b); }
  void optBlob(const Optional<std::string>& b) override {
    if (!b) {
      makeDir();
      ++m_itemIdx;
    } else {
      write(*b);
    }
  }
  void variadic(const BlobVec& v) override {
    auto const dir = makeDir();
    auto const vecDir = dir / folly::to<std::string>(m_itemIdx);
    fs::create_directory(vecDir, dir);
    for (size_t i = 0; i < v.size(); ++i) {
      auto const& b = v[i];
      writeFile(vecDir / folly::to<std::string>(i), b.data(), b.size());
    }
    ++m_itemIdx;
  }

  void nextOutput() override {
    assertx(m_outputIdx.has_value());
    ++*m_outputIdx;
    m_itemIdx = 0;
  }
  void startFini() override {
    assertx(m_outputIdx.has_value());
    m_outputIdx.reset();
    m_itemIdx = 0;
  }
  void finish() override {}
private:
  fs::path currentDir() {
    if (!m_outputIdx) return m_base / "fini";
    return m_base / folly::to<std::string>(*m_outputIdx);
  }

  fs::path makeDir() {
    auto const outputDir = currentDir();
    if (!m_itemIdx) fs::create_directory(outputDir, m_base);
    return outputDir;
  }

  void write(const std::string& b) {
    auto const dir = makeDir();
    writeFile(dir / folly::to<std::string>(m_itemIdx), b.data(), b.size());
    ++m_itemIdx;
  }

  fs::path m_base;
  size_t m_itemIdx;
  Optional<size_t> m_outputIdx;
};

//////////////////////////////////////////////////////////////////////

// folly::writeFile expects a container-like input, so this makes a
// ptr/length pair behave like one (without copying).
struct Adaptor {
  size_t size() const { return m_size; }
  bool empty() const { return !size(); }
  const char& operator[](size_t idx) const { return m_ptr[idx]; }
  const char* m_ptr;
  size_t m_size;
};

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

namespace detail {

//////////////////////////////////////////////////////////////////////

// Wrappers around the folly functions with error handling

std::string readFile(const fs::path& path) {
  std::string s;
  if (!folly::readFile(path.c_str(), s)) {
    throw Error{
      fmt::format(
        "Unable to read input from {} [{}]",
        path.c_str(), folly::errnoStr(errno)
      )
    };
  }
  return s;
}

void writeFile(const fs::path& path,
               const char* ptr, size_t size) {
  if (!folly::writeFile(Adaptor{ptr, size}, path.c_str())) {
    throw Error{
      fmt::format(
        "Unable to write output to {} [{}]",
        path.c_str(), folly::errnoStr(errno)
      )
    };
  }
}

//////////////////////////////////////////////////////////////////////

JobBase::JobBase(const std::string& name)
  : m_name{name}
{
  FTRACE(4, "registering remote worker command \"{}\"\n", m_name);
  auto& r = registry();
  std::lock_guard<std::mutex> _{r.lock};
  auto const insert = r.registry.emplace(m_name, this);
  always_assert(insert.second);
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

namespace {

// Parse the command-line and create the appropriate Source and Sink
// for input and output.
std::tuple<
  std::unique_ptr<ISource>,
  std::unique_ptr<ISink>,
  std::string
>
parseOptions(int argc, char** argv) {
  always_assert(argc > 1);
  always_assert(!strcmp(argv[1], s_option));

  if (argc >= 2 && !strcmp(argv[2], g_local_option)) {
    if (argc != 6) {
      std::cerr << "Usage: "
                << argv[0]
                << " " << s_option
                << " " << g_local_option
                << " <command name>"
                << " <root>"
                << " <output file>"
                << std::endl;
      return std::make_tuple(nullptr, nullptr, "");
    }

    std::string name{argv[3]};
    fs::path root{argv[4]};
    fs::path outputFile{argv[5]};

    FTRACE(2, "extern worker run (local) (\"{}\", {}, {})\n",
           name, root.native(), outputFile.native());

    // Input comes from STDIN
    auto zstdDictPath = root/ g_zstddict_filename;
    auto zstdDicts = createZstdDictionaries(zstdDictPath);
    auto source =
      time("read-pipe", [] { return readFromPipe(STDIN_FILENO); });
    return std::make_tuple(
      std::make_unique<SerializedSource>(
        std::move(root),
        std::move(source),
        zstdDicts.first,
        zstdDicts.second
      ),
      // Even though we pass the zstd dicts to both the source and the
      // sink, we make the SerializedSink the owner of the data as it
      // is referenced after the source.
      std::make_unique<SerializedSink>(
        std::move(outputFile),
        zstdDicts.first,
        zstdDicts.second
      ),
      std::move(name)
    );
  } else if (argc != 6) {
    std::cerr << "Usage: "
              << argv[0]
              << " " << s_option
              << " <command name>"
              << " <config dir>"
              << " <output dir>"
              << " <input dir>"
              << std::endl;
    return std::make_tuple(nullptr, nullptr, "");
  } else {
    std::string name{argv[2]};
    fs::path configPath{argv[3]};
    fs::path outputPath{argv[4]};
    fs::path inputPath{argv[5]};

    FTRACE(2, "extern worker run(\"{}\", {}, {}, {})\n",
           name, configPath.native(), outputPath.native(),
           inputPath.native());

    return std::make_tuple(
      std::make_unique<FileSource>(
        std::move(configPath),
        std::move(inputPath)
      ),
      std::make_unique<FileSink>(std::move(outputPath)),
      std::move(name)
    );
  }
}

}

//////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
  Timer _{"main"};

  try {
    auto const [source, sink, name] = parseOptions(argc, argv);
    if (!source) return EXIT_FAILURE;

    // Lookup the registered job for the requested name.
    auto const worker = [&, name = name] {
      auto& r = registry();
      std::lock_guard<std::mutex> _{r.lock};
      auto const it = r.registry.find(name);
      if (it == r.registry.end()) {
        throw Error{fmt::format("No command named `{}` registered", name)};
      }
      return it->second;
    }();

    g_in_job = true;
    SCOPE_EXIT { g_in_job = false; };

    // First do any global initialization.
    time("init", [&, &source = source] {
      worker->init(*source);
    });
    time("run-all", [&, &source = source, &sink = sink] {
      // Then execute run() until we're out of inputs.
      size_t run = 0;
      while (!source->inputEnd()) {
        time(
          [&] { return fmt::format("run {}", run); },
          [&, &source = source, &sink = sink] { worker->run(*source, *sink); }
        );
        ++run;
      }
      source->finish();
    });
    // Do any cleanup and flush output to its final destination
    time("fini", [&, &sink = sink] { worker->fini(*sink); });
    time("flush", [&sink = sink] { sink->finish(); });
    return 0;
  } catch (const std::exception& exn) {
    std::cerr << "Error: " << exn.what() << std::endl;
    return EXIT_FAILURE;
  }
}

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

/*
 * The builtin implementation which uses fork+exec (and stores data on
 * disk). It is always available, reasonably efficient, and assumed to
 * be "reliable" (IE, never needs a fallback).
 *
 * All data is stored under the "root" (which is the working-dir
 * specified in Options).
 *
 * Data is stored in blob files under the root, whose names are
 * assigned numerically. The blob files are pooled. When a thread
 * needs to write, it checks out a free file, append the data to it,
 * then "returns" it back to the pool. The particular blob file data
 * is stored to is therefore arbitrary. Files are not stored at all
 * (since they're already on disk).
 *
 * The RefIds are just the path to the file, either a blob file, or
 * some other file. If the data was stored in a blob file, the m_extra
 * of the RefId will be interpreted as a pair of (uint32_t, uint32_t)
 * consisting of the offset and length in the blob file.
 * For stored files, m_extra will always be zero.
 *
 * As an optimization, if the data is less or equal to g_inline_size,
 * then its stored "inline" in the RefId. That is, m_id is the blob
 * itself. Inline/non-inline blobs can be distinguished by the m_size
 * field. This also applies to stored files (if sufficiently small,
 * we'll read it and store it inline).
 *
 * Stored blobs are compressed: For blobs stored in files, `m_size`
 * refers to the uncompressed size of the data, whereas
 * `m_offsetAndLength`'s * length field contains the compressed size
 * and the offset in the blobfile to start reading from.
 *
 * Workers are forked, given their inputs, calculate outputs, then
 * write their outputs and exit. Input is given to the worker via
 * stdin (as a stream of serialized RefIds). As part of the
 * command-line, the worker is given an output file to write its
 * output to. It does (creating RefIds in the process), and reports
 * the output RefIds via a pipe (g_local_pipe_fd).
 */
struct SubprocessImpl : public Client::Impl {
  SubprocessImpl(const Options&, Client&);
  ~SubprocessImpl() override;

  std::string session() const override { return m_fdManager->root().native(); }

  bool isSubprocess() const override { return true; }
  bool supportsOptimistic() const override { return false; }

  coro::Task<BlobVec> load(const RequestId&, IdVec) override;
  coro::Task<IdVec> store(const RequestId&, PathVec, BlobVec,
                          bool) override;
  coro::Task<std::vector<RefValVec>>
  exec(const RequestId&,
       const std::string&,
       RefValVec,
       std::vector<RefValVec>,
       const folly::Range<const OutputType*>&,
       const folly::Range<const OutputType*>*,
       Client::ExecMetadata
       ) override;

private:
  // Manage the pool of blob files
  struct FDManager {
    explicit FDManager(fs::path);

    const fs::path& root() const { return m_root; }

    // Acquire a FD to the given file to read from. If the path does
    // not correspond to a blob file, nullptr is returned.
    const BlobReader* acquireForRead(const fs::path&);
    // Acquire a FD to a blob file to append to. You don't have a say
    // in which file you get. The FD must be returned via release()
    // when done. You have exclusive access to this FD.
    // Takes an optional ZSTD compression dict.
    BlobWriter* acquireForAppend(ZSTD_CDict*);
    // Writes the ZSTD compression dict to a location known to workers.
    // Returns the written path.
    fs::path provideZSTDCompressionDict(const fs::path&) const;
    void release(BlobWriter& writer);

  private:
    fs::path m_root;
    folly_concurrent_hash_map_simd<std::string, std::unique_ptr<BlobReader>> m_blobReaders;
    folly_concurrent_hash_map_simd<std::string, std::unique_ptr<BlobWriter>> m_blobWriters;

    std::mutex m_lock;
    std::stack<BlobWriter*> m_forAppend;
    size_t m_nextBlob;
  };

  // Similar to FDManager, but manages trace files. We have workers
  // re-use trace files to avoid generating a huge number of time.
  struct TraceFileManager {
    fs::path get();
    void put(fs::path);
  private:
    std::mutex m_lock;
    std::stack<fs::path> m_paths;
    size_t m_nextId{0};
  };

  static fs::path newRoot(const Options&);

  coro::Task<std::string> doSubprocess(const RequestId&,
                                       const std::string&,
                                       std::string,
                                       const fs::path&,
                                       Optional<uint64_t>);

  Options m_options;
  std::unique_ptr<FDManager> m_fdManager;
  TraceFileManager m_traceManager;
  Optional<SubprocessScheduler> m_subprocessScheduler;
  Optional<std::thread> m_measureRSSThread{};
  std::atomic_bool m_stopMeasuringRSSThread{false};
  ZSTD_CDict* m_compressionDict{nullptr};
  ZSTD_DDict* m_decompressionDict{nullptr};
};

SubprocessImpl::SubprocessImpl(const Options& options, Client& parent)
  : Impl{"subprocess", parent}
  , m_options{options}
  , m_fdManager{std::make_unique<FDManager>(newRoot(m_options))}
  , m_subprocessScheduler{std::nullopt}
{
  FTRACE(2, "Using subprocess extern-worker impl with root at {}\n",
         m_fdManager->root().native());
  Logger::FInfo(
    "Using subprocess extern-worker at {}",
    m_fdManager->root().native()
  );
  if (!options.m_zstdDictionaryPath.empty()) {
    auto zstdDictPath =
      m_fdManager->provideZSTDCompressionDict(fs::path(options.m_zstdDictionaryPath));
    auto dicts = createZstdDictionaries(zstdDictPath);
    m_compressionDict = dicts.first;
    m_decompressionDict = dicts.second;
  }
  if (m_options.m_useSubprocessScheduler) {
    m_subprocessScheduler.emplace(options.m_maxSubprocessMemory);
    m_measureRSSThread = std::thread([&] {
      auto lastMeasurement = std::chrono::steady_clock::now();
      while (!m_stopMeasuringRSSThread) {
        // We wake more often than we measure to promptly end the thread when destructing.
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastMeasurement);
        if (elapsed.count() >= 5) {
          m_subprocessScheduler->sampleRSSData();
          lastMeasurement = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }});
  }
}

SubprocessImpl::~SubprocessImpl() {
  auto const root = m_fdManager->root();
  // Destroy FD manager first, to ensure no open FDs to any files
  // while cleaning up.
  m_fdManager.reset();
  if (m_measureRSSThread.has_value()) {
    m_stopMeasuringRSSThread.store(true, std::memory_order_release);
    m_measureRSSThread->join();
  }

  ZSTD_freeCDict(m_compressionDict);
  ZSTD_freeDDict(m_decompressionDict);

  if (m_options.m_cleanup) {
    Logger::FInfo(
      "Cleaning up subprocess extern-worker at {}...",
      root.native()
    );
    auto const before = std::chrono::steady_clock::now();
    auto const removed = time(
      "subprocess cleanup",
      [&] {
        std::error_code ec; // Suppress exceptions
        return fs::remove_all(root, ec);
      }
    );
    auto const elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
      std::chrono::steady_clock::now() - before
    ).count();
    FTRACE(2, "removed {} files\n", removed);
    Logger::FInfo(
      "Done cleaning up subprocess extern-worker. "
      "({} files removed) (took {})",
      removed, folly::prettyPrint(elapsed, folly::PRETTY_TIME_HMS, false)
    );
  }
}

// Ensure we always have an unique root under the working directory.
fs::path SubprocessImpl::newRoot(const Options& opts) {
  auto const base = opts.m_workingDir / "hphp-extern-worker";
  fs::create_directories(base);
  auto const full = base / boost::filesystem::unique_path(
    "%%%%-%%%%-%%%%-%%%%-%%%%-%%%%"
  ).native();
  fs::create_directory(full, base);
  return fs::canonical(full);
}

coro::Task<BlobVec> SubprocessImpl::load(const RequestId& requestId,
                                         IdVec ids) {
  // Read every file corresponding to the id and return their
  // contents.
  auto out = from(ids)
    | mapped([&] (const RefId& id) {
        FTRACE(4, "{} reading blob from {}\n",
               requestId.tracePrefix(), id.toString());
        if (id.m_size <= g_inline_size) {
          // Inline data requires no file read
          assertx(id.m_id.size() == id.m_size);
          assertx(!id.m_extra);
          return id.m_id;
        }
        auto reader = m_fdManager->acquireForRead(id.m_id);
        if (reader != nullptr) {
          // The data is in a blob file, so use a cached FD
          std::string compressedBuffer;
          ZSTD_DCtx* dctx = ZSTD_createDCtx();
          SCOPE_EXIT { ZSTD_freeDCtx(dctx); };
          if (dctx == nullptr) {
            throw Error{"Failed to create ZSTD decompression context"};
          }
          return reader->readAndDecompress(
            id.m_offsetAndLength, compressedBuffer, dctx, m_decompressionDict);
        }
        // It's some other (non-blob) file. Create an ephemeral FD to
        // read it.
        return FD{id.m_id, true, false, false}.read(id.m_extra, id.m_size);
      })
    | as<std::vector>();
  co_return out;
}

coro::Task<IdVec> SubprocessImpl::store(const RequestId& requestId,
                                        PathVec paths,
                                        BlobVec blobs,
                                        bool) {
  // SubprocessImpl never "uploads" files, but it must write to disk
  // (which we classify as an upload).
  BlobWriter* writer = nullptr;
  SCOPE_EXIT { if (writer) m_fdManager->release(*writer); };


  // Update stats. Skip blobs which we'll store inline.
  for (auto const& b : blobs) {
    if (b.size() <= g_inline_size) continue;
    ++stats().blobsUploaded;
    stats().blobBytesUploaded += b.size();
    if (!writer) writer = m_fdManager->acquireForAppend(m_compressionDict);
    assertx(writer);
  }

  auto out =
    ((from(paths)
      | mapped([&] (const fs::path& p) {
          auto fileSize = fs::file_size(p);
          if (fileSize <= g_inline_size) {
            FTRACE(4, "{} storing file {} inline\n",
                   requestId.tracePrefix(),
                   p.native());
            auto const contents = readFile(p);
            // Size of file could theoretically change between
            // stat-ing and reading it.
            if (contents.size() <= g_inline_size) {
              return inlineRefId(std::move(contents));
            }
            fileSize = contents.size();
          }
          // We distinguish blob files from non-blob files by making
          // sure non-blob files are always absolute paths (blob files
          // are always relative).
          return RefId{fs::canonical(p).native(), fileSize, 0};
        })
    ) +
    (from(blobs)
     | mapped([&] (const std::string& b) {
         if (b.size() <= g_inline_size) {
           FTRACE(4, "{} storing blob inline\n",
                  requestId.tracePrefix());
           return inlineRefId(b);
         }
         FTRACE(4, "{} writing size {} blob to {}\n",
                requestId.tracePrefix(), b.size(), writer->path().native());
         RefId r = writer->compressAndAppend(b);
         FTRACE(4, "{} written as {}\n", requestId.tracePrefix(), r.toString());
         return r;
       })
    ))
    | as<std::vector>();
  if (writer) writer->flush();
  co_return out;
}

coro::Task<std::vector<RefValVec>>
SubprocessImpl::exec(const RequestId& requestId,
                     const std::string& command,
                     RefValVec config,
                     std::vector<RefValVec> inputs,
                     const folly::Range<const OutputType*>& output,
                     const folly::Range<const OutputType*>* finiOutput,
                     Client::ExecMetadata) {
  FTRACE(4, "{} executing \"{}\" ({} runs)\n",
         requestId.tracePrefix(), command, inputs.size());

  co_await coro::co_safe_point;

  // Each set of inputs should always have the same size.
  if (debug && !inputs.empty()) {
    auto const size = inputs[0].size();
    for (size_t i = 1; i < inputs.size(); ++i) {
      always_assert(inputs[i].size() == size);
    }
  }

  // Encode all the inputs
  BlobEncoder encoder;
  auto const encodeParams = [&] (const RefValVec& params) {
    for (auto const& param : params) {
      match(
        param,
        [&] (const RefId& id) {
          SerializedSink::encodeRefId(id, encoder);
        },
        [&] (const Optional<RefId>& id) {
          SerializedSink::encodeOptRefId(id, encoder);
        },
        [&] (const IdVec& ids) {
          SerializedSink::encodeRefIdVec(ids, encoder);
        }
      );
    }
  };
  encodeParams(config);
  encoder((size_t)inputs.size());
  for (auto const& input : inputs) encodeParams(input);

  // Acquire a FD. We're not actually going to write to it, but the
  // worker will. This ensures the worker has exclusive access to the
  // file while it's running.
  auto writer = m_fdManager->acquireForAppend(m_compressionDict);
  SCOPE_EXIT { m_fdManager->release(*writer); };
  writer->flush();

  Optional<uint64_t> usageEstimate{};
  if (m_subprocessScheduler.has_value()) {
    // Wait until we have enough memory to run the subprocess.
    usageEstimate = m_subprocessScheduler->acquire(command);
    assertx(usageEstimate.value() > 0);
  }
  // Do the actual fork+exec.
  auto const outputBlob = co_await
    doSubprocess(
      requestId,
      command,
      std::string{(const char*)encoder.data(), encoder.size()},
      writer->path(),
      usageEstimate
    );

  // The worker (maybe) wrote to the file, so we need to re-sync our
  // tracked offset.
  writer->syncOffset();
  writer->freeBuffer();

  // Decode the output
  BlobDecoder decoder{outputBlob.data(), outputBlob.size()};
  auto const makeOutput = [&] (OutputType type) -> RefVal {
    switch (type) {
      case OutputType::Val:
        return SerializedSource::decodeRefId(decoder);
      case OutputType::Opt:
        return SerializedSource::decodeOptRefId(decoder);
      case OutputType::Vec:
        return SerializedSource::decodeRefIdVec(decoder);
    }
    always_assert(false);
  };

  auto const makeOutputs = [&] (const folly::Range<const OutputType*>& types) {
    RefValVec vec;
    vec.reserve(types.size());
    for (auto const type : types) {
      vec.emplace_back(makeOutput(type));
    }
    return vec;
  };

  std::vector<RefValVec> out;
  out.reserve(inputs.size() + (finiOutput ? 1 : 0));
  std::generate_n(
    std::back_inserter(out),
    inputs.size(),
    [&] { return makeOutputs(output); }
  );
  if (finiOutput) out.emplace_back(makeOutputs(*finiOutput));

  decoder.assertDone();
  co_return out;
}

coro::Task<std::string>
SubprocessImpl::doSubprocess(const RequestId& requestId,
                             const std::string& command,
                             std::string inputBlob,
                             const fs::path& outputPath,
                             Optional<uint64_t> usageEstimate) {
  auto workerPath = m_options.m_workerPath.empty()
    ? current_executable_path()
    : m_options.m_workerPath;
  std::vector<std::string> args{
    workerPath,
    s_option,
    g_local_option,
    command,
    m_fdManager->root().native(),
    outputPath.native()
  };

  // Propagate the TRACE option in the environment. We'll copy the
  // trace output into this process' trace output.
  std::vector<std::string> env;
  Optional<fs::path> traceFile;
  SCOPE_EXIT { if (traceFile) m_traceManager.put(std::move(*traceFile)); };
  if (auto const trace = getenv("TRACE")) {
    traceFile = m_traceManager.get();
    auto const fullPath = m_fdManager->root() / *traceFile;
    env.emplace_back(fmt::format("TRACE={}", trace));
    env.emplace_back(fmt::format("HPHP_TRACE_FILE={}", fullPath.c_str()));
  }
  if (auto const asan_options = getenv("ASAN_OPTIONS")) {
    env.emplace_back(fmt::format("ASAN_OPTIONS={}", asan_options));
  }

  FTRACE(
    4, "{} executing subprocess for '{}'{}(input size: {})\n",
    requestId.tracePrefix(),
    command,
    traceFile
      ? fmt::format(" (trace-file: {}) ", traceFile->native())
      : "",
    inputBlob.size()
  );

  co_await coro::co_safe_point;

  auto const before = std::chrono::steady_clock::now();

  // Do the actual fork+exec.
  folly::Subprocess subprocess{
    args,
    folly::Subprocess::Options{}
      .parentDeathSignal(SIGKILL)
      .pipeStdin()
      .pipeStdout()
      .pipeStderr()
      .fd(g_local_pipe_fd, folly::Subprocess::PIPE_OUT)
      .closeOtherFds(),
    nullptr,
    &env
  };
  Optional<uint64_t> pid;
  if (m_subprocessScheduler.has_value()) {
    pid = subprocess.pid();
    m_subprocessScheduler->registerSubprocess(*pid, command);
  }

  std::string output;
  std::string stderr;
  size_t inputWritten = 0;

  // Communicate with the worker. When available, read from worker's
  // stderr and output pipe and store the data. Throw everything else
  // away. Attempt to write inputs to the worker's stdin whenever it
  // has free space.
  subprocess.communicate(
    [&] (int parentFd, int childFd) { // Read
      if (childFd == g_local_pipe_fd) {
        return readFromPipe(parentFd, output);
      } else if (childFd == STDERR_FILENO) {
        return readFromPipe(parentFd, stderr);
      } else {
        // Worker is writing to some other random FD (including
        // stdout). Ignore it.
        char dummy[512];
        folly::readNoInt(parentFd, dummy, sizeof dummy);
      }
      return false;
    },
    [&] (int parentFd, int childFd) { // Write
      // Close any writable FD except stdin
      if (childFd != STDIN_FILENO) return true;
      // We've written all of the input, so close stdin. This will
      // signal to the worker that input is all sent.
      if (inputWritten >= inputBlob.size()) return true;

      // Otherwise write what we can
      auto const toWrite = inputBlob.size() - inputWritten;
      auto const written =
        folly::writeNoInt(parentFd, inputBlob.data() + inputWritten, toWrite);
      if (written < 0) {
        if (errno == EAGAIN) return false;
        throw Error{
          fmt::format(
            "Failed writing {} bytes to subprocess input for '{}' [{}]",
            toWrite, command, folly::errnoStr(errno)
          )
        };
      } else if (written == 0) {
        return true;
      }
      inputWritten += written;
      return false;
    }
  );

  auto const returnCode = subprocess.wait();
  auto const elapsed = std::chrono::steady_clock::now() - before;
  if (m_subprocessScheduler.has_value()) {
    assertx(pid.has_value());
    m_subprocessScheduler->release(*pid, command, *usageEstimate);
  }

  stats().execCpuUsec +=
    std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  ++stats().execAllocatedCores;

  FTRACE(
    4,
    "{} subprocess finished (took {}). "
    "Output size: {}, Return code: {}, Stderr: {}\n",
    requestId.tracePrefix(),
    prettyPrint(
      std::chrono::duration_cast<std::chrono::duration<double>>(
        elapsed
      ).count(),
      folly::PRETTY_TIME,
      false
    ),
    output.size(),
    returnCode.str(),
    stderr
  );

  co_await coro::co_safe_point;

  // Do this before checking the return code. If the process failed,
  // we want to capture anything it logged before throwing.
  if (traceFile && fs::exists(m_fdManager->root() / *traceFile)) {
    auto const contents = readFile(m_fdManager->root() / *traceFile);
    if (!contents.empty()) {
      Trace::ftraceRelease(
        "vvvvvvvvvvvvvvvvvv remote-exec ({} \"{}\") vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\n"
        "{}"
        "^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n",
        requestId.toString(),
        command,
        contents
      );
    }
  }

  if (!returnCode.exited() || returnCode.exitStatus() != 0) {
    throw WorkerError{
      fmt::format(
        "Execution of `{}` failed: {}\nstderr:\n{}",
        command,
        returnCode.str(),
        stderr
      )
    };
  }
  if (inputWritten != inputBlob.size()) {
    throw WorkerError{
      fmt::format(
        "Execution of `{}` failed: Process failed to consume input\n",
        command
      )
    };
  }

  co_return output;
}

//////////////////////////////////////////////////////////////////////

SubprocessImpl::FDManager::FDManager(fs::path root)
  : m_root{std::move(root)}
  , m_nextBlob{0}
{}

const BlobReader* SubprocessImpl::FDManager::acquireForRead(const fs::path& path) {
  // Blob files are always relative
  if (path.is_absolute()) return nullptr;

  assertx(m_blobWriters.find(path.native()) != m_blobWriters.end());
  auto const it = m_blobReaders.find(path.native());
  if (it != m_blobReaders.end()) return it->second.get();

  auto const filename = m_root / path.filename().native();
  auto [elem, emplaced] =
    m_blobReaders.emplace(path.filename().native(), std::make_unique<BlobReader>(filename));
  assertx(emplaced);
  return elem->second.get();
}

void SubprocessImpl::FDManager::release(BlobWriter& fd) {
  if (debug) {
    // Should be returning something cached
    auto const it = m_blobWriters.find(fd.path().filename().native());
    always_assert(it != m_blobWriters.end());
    always_assert(it->second.get() == &fd);
  }
  std::scoped_lock<std::mutex> _{m_lock};
  m_forAppend.push(&fd);
}

BlobWriter* SubprocessImpl::FDManager::acquireForAppend(ZSTD_CDict* cdict) {
  // Use the blob file at the top of m_forAppend.
  std::scoped_lock<std::mutex> _{m_lock};
  if (m_forAppend.empty()) {
    // If empty, there's no free blob file. We need to create a new
    // one.
    auto const id = m_nextBlob++;
    auto const filename = m_root / folly::to<std::string>(id);
    auto const [elem, emplaced] = m_blobWriters.emplace(
      filename.filename().native(),
      std::make_unique<BlobWriter>(filename, /* create */ true, cdict)
    );
    assertx(emplaced);
    m_forAppend.push(elem->second.get());
  }
  // We deliberately keep the blob filename as small as possible
  // because they get serialized a lot and it keeps the input/output
  // sizes small.
  auto writer = m_forAppend.top();
  m_forAppend.pop();
  return writer;
}

fs::path SubprocessImpl::FDManager::provideZSTDCompressionDict(
  const fs::path& path) const {
  auto content = detail::readFile(path);
  auto outPath = zstdDictLocation(m_root);
  detail::writeFile(outPath, content.data(), content.length());
  return outPath;
}

//////////////////////////////////////////////////////////////////////

fs::path SubprocessImpl::TraceFileManager::get() {
  std::scoped_lock<std::mutex> _{m_lock};
  if (m_paths.empty()) {
    auto const id = m_nextId++;
    m_paths.push(fmt::format("trace-{:04}.log", id));
  }
  auto path = std::move(m_paths.top());
  m_paths.pop();
  return path;
}

void SubprocessImpl::TraceFileManager::put(fs::path path) {
  std::scoped_lock<std::mutex> _{m_lock};
  m_paths.push(std::move(path));
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

Client::Client(folly::Executor::KeepAlive<> executor,
               const Options& options)
  : m_options{options}
  , m_stats{std::make_shared<Stats>()}
{
  Timer _{"create impl"};
  // Look up which implementation to use. If a hook has been
  // registered, and we're allowed to use it according to the Options,
  // try to use it.
  if (g_impl_hook &&
      m_options.m_useSubprocess != Options::UseSubprocess::Always) {
    m_impl = g_impl_hook(m_options, executor, *this);
    if (!m_impl) throw Error{"No non-subprocess impl available"};
  } else {
    m_impl = std::make_unique<SubprocessImpl>(m_options, *this);
  }
  FTRACE(2, "created \"{}\" impl\n", m_impl->name());
}

Client::~Client() {
  Timer _{[&] { return fmt::format("destroy impl {}", m_impl->name()); }};
  m_impl.reset();
}

coro::Task<Ref<std::string>> Client::storeFile(fs::path path,
                                               bool optimistic) {
  RequestId requestId{"store file"};

  FTRACE(
    2, "{} storing {}{}\n",
    requestId.tracePrefix(),
    path.native(),
    optimistic ? " (optimistically)" : ""
  );

  ++m_stats->files;
  ++m_stats->storeCalls;
  SCOPE_EXIT {
    m_stats->storeLatencyUsec += std::chrono::duration_cast<
      std::chrono::microseconds
    >(requestId.elapsed()).count();
  };

  auto ids = co_await tryWithThrottling<IdVec>([&] {
    return m_impl->store(
      requestId,
      PathVec{path},
      {},
      optimistic
    );
  });
  assertx(ids.size() == 1);

  Ref<std::string> ref{std::move(ids[0])};
  co_return ref;
}

coro::Task<std::vector<Ref<std::string>>>
Client::storeFile(std::vector<fs::path> paths,
                  bool optimistic) {
  RequestId requestId{"store files"};

  FTRACE(
    2, "{} storing {} files{}\n",
    requestId.tracePrefix(),
    paths.size(),
    optimistic ? " (optimistically)" : ""
  );
  ONTRACE(4, [&] {
    for (auto const& p : paths) {
      FTRACE(4, "{} storing {}\n", requestId.tracePrefix(), p.native());
    }
  }());

  m_stats->files += paths.size();
  ++m_stats->storeCalls;
  SCOPE_EXIT {
    m_stats->storeLatencyUsec += std::chrono::duration_cast<
      std::chrono::microseconds
    >(requestId.elapsed()).count();
  };

  auto const DEBUG_ONLY size = paths.size();
  auto ids = co_await tryWithThrottling<IdVec>([&] {
    return m_impl->store(requestId, paths, {}, optimistic);
  });
  assertx(ids.size() == size);

  auto out = from(ids)
    | move
    | mapped([&] (auto&& id) {
        return Ref<std::string>{std::move(id)};
      })
    | as<std::vector>();
  co_return out;
}

//////////////////////////////////////////////////////////////////////

// Sleep some random amount corresponding to the number of retries
// we've tried.
void Client::Impl::throttleSleep(size_t retry,
                                 std::chrono::milliseconds base) {
  // Each retry doubles the size of the window. We select a random
  // amount from within that.
  auto const scale = uint64_t{1} << std::min(retry, size_t{16});
  auto const window = std::chrono::microseconds{base} * scale;

  static std::mt19937_64 engine = [&] {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937_64{seed};
  }();

  std::uniform_int_distribution<> distrib(0, window.count());
  auto const wait = [&] {
    static std::mutex lock;
    std::lock_guard<std::mutex> _{lock};
    return std::chrono::microseconds{distrib(engine)};
  }();
  FTRACE(2, "Sleeping for {} milliseconds...\n", groupDigits(wait.count()));

  // Use normal sleep here, not coro friendly sleep. The whole purpose
  // is to slow down execution and using a coro sleep will just allow
  // a lot of other actions to run.
  std::this_thread::sleep_for(wait);
}

//////////////////////////////////////////////////////////////////////

std::string Client::Stats::toString(const std::string& phase,
                                    const std::string& extra) const {
  auto const bytes = [] (size_t b) {
    auto s = folly::prettyPrint(
      b,
      folly::PRETTY_BYTES
    );
    if (!s.empty() && s[s.size()-1] == ' ') s.resize(s.size()-1);
    return s;
  };

  auto const usecs = [] (size_t t) {
    auto s = prettyPrint(
      double(t) / 1000000.0,
      folly::PRETTY_TIME_HMS,
      false
    );
    if (!s.empty() && s[s.size()-1] == ' ') s.resize(s.size()-1);
    return s;
  };

  auto const pct = [] (size_t a, size_t b) -> std::string {
    if (!b) return "--";
    return fmt::format("{:.2f}%", double(a) / b * 100.0);
  };

  auto const execWorkItems_ = execWorkItems.load();
  auto const allocatedCores = execAllocatedCores.load();
  auto const cpuUsecs = execCpuUsec.load();
  auto const execCalls_ = execCalls.load();
  auto const storeCalls_ = storeCalls.load();
  auto const loadCalls_ = loadCalls.load();

  return fmt::format(
    "  {}:{}\n"
    "  Execs: {} ({}) total, {} cache-hits ({}), {} optimistically\n"
    "  Files: {} total, {} read, {} queried, {} uploaded ({})\n"
    "  Blobs: {} total, {} queried, {} uploaded ({})\n"
    "  Cpu: {} usage, {} allocated cores ({}/core)\n"
    "  Mem: {} max used, {} reserved\n"
    "  {} downloads ({}), {} throttles\n"
    "  Avg Latency: {} exec, {} store, {} load",
    phase,
    extra.empty() ? "" : fmt::format(" {}", extra),
    execCalls_,
    execWorkItems_,
    execCacheHits.load(),
    pct(execCacheHits.load(), execCalls_),
    optimisticExecs.load(),
    files.load(),
    filesRead.load(),
    filesQueried.load(),
    filesUploaded.load(),
    bytes(fileBytesUploaded.load()),
    blobs.load(),
    blobsQueried.load(),
    blobsUploaded.load(),
    bytes(blobBytesUploaded.load()),
    usecs(cpuUsecs),
    allocatedCores,
    usecs(allocatedCores ? (cpuUsecs / allocatedCores) : 0),
    bytes(execMaxUsedMem.load()),
    bytes(execReservedMem.load()),
    downloads.load(),
    bytes(bytesDownloaded.load()),
    throttles.load(),
    usecs(execCalls_ ? (execLatencyUsec.load() / execCalls_) : 0),
    usecs(storeCalls_ ? (storeLatencyUsec.load() / storeCalls_) : 0),
    usecs(loadCalls_ ? (loadLatencyUsec.load() / loadCalls_) : 0)
  );
}

void Client::Stats::logSample(const std::string& phase,
                              StructuredLogEntry& sample) const {
  sample.setInt(phase + "_total_execs", execCalls.load());
  sample.setInt(phase + "_total_exec_work_items", execWorkItems.load());
  sample.setInt(phase + "_cache_hits", execCacheHits.load());
  sample.setInt(phase + "_optimistically", optimisticExecs.load());

  sample.setInt(phase + "_total_files", files.load());
  sample.setInt(phase + "_file_reads", filesRead.load());
  sample.setInt(phase + "_file_queries", filesQueried.load());
  sample.setInt(phase + "_file_stores", filesUploaded.load());
  sample.setInt(phase + "_file_stores_bytes", fileBytesUploaded.load());

  sample.setInt(phase + "_total_blobs", blobs.load());
  sample.setInt(phase + "_blob_queries", blobsQueried.load());
  sample.setInt(phase + "_blob_stores", blobsUploaded.load());
  sample.setInt(phase + "_blob_stores_bytes", blobBytesUploaded.load());

  sample.setInt(phase + "_total_loads", downloads.load());
  sample.setInt(phase + "_total_loads_bytes", bytesDownloaded.load());
  sample.setInt(phase + "_throttles", throttles.load());

  sample.setInt(phase + "_exec_cpu_usec", execCpuUsec.load());
  sample.setInt(phase + "_exec_allocated_cores", execAllocatedCores.load());
  sample.setInt(phase + "_exec_max_used_mem", execMaxUsedMem.load());
  sample.setInt(phase + "_exec_reserved_mem", execReservedMem.load());

  auto const execCalls_ = execCalls.load();
  auto const storeCalls_ = storeCalls.load();
  auto const loadCalls_ = loadCalls.load();

  sample.setInt(
    phase + "_avg_exec_latency_usec",
    execCalls_ ? (execLatencyUsec.load() / execCalls_) : 0
  );
  sample.setInt(
    phase + "_avg_store_latency_usec",
    storeCalls_ ? (storeLatencyUsec.load() / storeCalls_) : 0
  );
  sample.setInt(
    phase + "_avg_load_latency_usec",
    loadCalls_ ? (loadLatencyUsec.load() / loadCalls_) : 0
  );
}

//////////////////////////////////////////////////////////////////////

}
