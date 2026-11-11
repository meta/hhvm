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

#include "hphp/runtime/base/isame-log.h"
#include "hphp/runtime/vm/source-location.h"

#include "hphp/util/blob.h"
#include "hphp/util/blob-writer.h"
#include "hphp/util/compact-tagged-ptrs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

struct ArrayData;
struct BlobDecoder;
struct BlobEncoder;
struct Extension;
struct PackageInfo;
struct RepoAutoloadMapBuilder;
struct RepoFileData;
struct RepoFileIndex;
struct RepoGlobalData;
struct SHA1;
struct StringData;
struct string_data_tsame;
struct string_data_fsame;
struct string_data_same;
struct UnitEmitter;

namespace Native {
struct FuncTable;
}

///////////////////////////////////////////////////////////////////////////////

struct RepoUnitInfo {
  int64_t unitSn;
  const StringData* path = nullptr;
  Blob::Bounds emitterLocation;
  Blob::Bounds symbolsLocation;

  void serde(BlobEncoder& sd) const;
  void serde(BlobDecoder& sd);
};

enum class RepoSymbolType {
  TYPE,
  FUNC,
  CONSTANT,
  TYPE_ALIAS,
  MODULE
};

using RepoUnitSymbols =
  std::vector<std::pair<const StringData*, RepoSymbolType>>;

///////////////////////////////////////////////////////////////////////////////

/*
 * RepoFileBuilder is used to create a RepoFile (you can either use a
 * RepoFile, or create one, there's no simultaneous creation/use).
 *
 * Usage is simple. Instantiate one, call add() with as many
 * UnitEmitters as needed, then call finish().
 *
 * System errors (for example, while writing to the file) result in
 * always_assert. This avoids having to write exception handling code,
 * and such errors are necessarily fatal anyways in the context where
 * we're using this.
 *
 * Unless stated otherwise, no member functions on RepoFileBuilder are
 * meant to be called concurrently and are meant to be used in a
 * single-threaded manner.
 */

struct RepoFileBuilder {
  // Create a new repo file at the given path. The file will actually
  // be created with a temporary name, then renamed to the given name
  // when finish() is called.
  explicit RepoFileBuilder(const std::string& path,
                           bool enableUnitEmitterReuse = false);

  ~RepoFileBuilder();

  // Encoding a UnitEmitter is expensive, so one can do it ahead of
  // time (in different threads), then add it directly.
  struct EncodedUE {
    explicit EncodedUE(const UnitEmitter& ue, bool includeUnitMethCallers);
   private:
    friend struct RepoFileBuilder;
    const StringData* path;
    int64_t sn;
    std::vector<char> blob;
    std::vector<const StringData*> methCallers;
  };

  // Add the given UnitEmitter to the repo file. This will encode it
  // and then write it to disk. This can only be called before
  // finish() is called.
  void add(const EncodedUE& ue);
  void add(EncodedUE&& ue);
  void add(const UnitEmitter& ue);

  // Write units to disk in serial-number order, buffering up to
  // `bufferBytes` of units which arrive early. Serial numbers must be
  // dense and start at 0. Must be called before any add().
  void writeInSnOrder(uint64_t bufferBytes);

  // Retain reusable units from `base`, excluding units whose paths match or
  // are descendants of a path in `exclude`. `exclude` must contain unique,
  // non-empty paths relative to the source root, including every added unit
  // and every input change which can affect a retained unit. The caller must
  // use a compatible compiler/configuration.
  void addFrom(RepoFileData& base,
               RepoAutoloadMapBuilder& autoload,
               std::vector<std::string> exclude,
               const PackageInfo& packageInfo);

  // "Finish" the repo file by writing RepoGlobalData, the
  // RepoAutoloadMapBuilder, the literal string table, and indexing
  // information into the file. Once successful, the temporary file is
  // renamed to its final name. Once finish() is called, nothing can
  // be done with the RepoFileBuilder except destroy it.
  void finish(const RepoGlobalData&,
              const RepoAutoloadMapBuilder&,
              const PackageInfo&);

  RepoFileBuilder(const RepoFileBuilder&) = delete;
  RepoFileBuilder(RepoFileBuilder&&) = delete;
  RepoFileBuilder& operator=(const RepoFileBuilder&) = delete;
  RepoFileBuilder& operator=(RepoFileBuilder&&) = delete;
private:
  template<class T> void addImpl(T&&);
  void writeUnit(const EncodedUE&);
  void writeLowestPending();

  struct Data;
  std::unique_ptr<Data> m_data;
};

///////////////////////////////////////////////////////////////////////////////

struct RepoFileData {
  struct Data;

  explicit RepoFileData(const std::string& path);
  ~RepoFileData();

  RepoFileData(RepoFileData&&) = delete;
  RepoFileData& operator=(RepoFileData&&) = delete;
private:
  friend struct RepoFileBuilder;
  std::unique_ptr<Data> m_data;
};

///////////////////////////////////////////////////////////////////////////////

/*
 * Reader equivalent of RepoFileBuilder. Reads an existing repo file
 * and makes it available for querying.
 *
 * RepoFile is a strong singleton. It is not actually possible to
 * instantiate it, and all interaction is done with static class
 * methods.
 *
 * Like RepoFileBuilder, system errors result in always_assert as we
 * don't have a good way to recover from such errors.
 */
struct RepoFile {

  // To support lazy-loading, RepoFile needs to know where to load
  // certain pieces of data. It is the responsibility of the caller to
  // provide such offsets. The offset is abstracted away as a "token"
  // which the callers can store.
  using Token = uint64_t;

  // Initialize the repo file at the given path. This opens the file
  // and loads some indexing information. No other functions on
  // RepoFile can be called until the repo is initialized. This
  // function cannot be called concurrently.
  static void init(const std::string& path);
  // "Destroys" the repo file. This releases any open file descriptors
  // on the file and destroys any indexing information. If the repo
  // file hasn't been initialized, this does nothing. This function
  // cannot be called concurrently.
  static void destroy();

  // Retrieve the RepoGlobalData which was stored in this repo
  // file. The data is actually loaded during init(), so this does no
  // file I/O. Unlike other querying functions, this does not require
  // loadGlobalTables() be called.
  static const RepoGlobalData& globalData();

  /*
   * Load the various global tables stored in the repo file. This
   * includes the global array type table, the repo autoload map, and
   * various unit emitter indexing information.
   *
   * This must be called after init(), but before any of the below
   * querying functions are called. It can only be called once, and
   * cannot be called concurrently.
   */
  static void loadGlobalTables(bool loadAutoloadMap = true);

  /*
   * Retrieves the package info stored in the repo file.
   */
  static const PackageInfo& packageInfo();

  /*
   * Query functions:
   *
   * Unlike otherwise stated, these all can be called concurrently,
   * and must be called only after loadGlobalTables() is called.
   */

  // Return the paths corresponding to all UnitEmitters in the repo
  // file.
  static std::vector<const StringData*> enumerateUnits();

  // Return the number of units in the repo file.
  static std::size_t numUnits();

  // Load the UnitEmitter with the given path `searchPath`, returning
  // nullptr if none exists. The UnitEmitter will be assigned the path
  // `path' (it might be different from `searchPath` due to SourceRoot
  // issues). If `lazy` is true, the bytecode and line tables are not
  // loaded with the UnitEmitter and will instead be loaded on demand.
  static std::unique_ptr<UnitEmitter>
  loadUnitEmitter(const StringData* path,
                  const RepoUnitInfo* info,
                  bool lazy);

  // Given an unit's SN, and a token for that unit, return the amount
  // of data after that token for the unit. This is used for lazy
  // loading various data out of the unit, where the amount of
  // remaining space needs to be known.
  static size_t remainingSizeOfUnit(int64_t unitSn, Token token);

  // Read "len" bytes of data (into "data") from an unit given by a
  // SN, and a token for that unit. The pointer must be allocated with
  // at least len free space. This is used for lazy loading various
  // bits of UnitEmitters.
  static void readRawFromUnit(int64_t unitSn,
                              Token token,
                              unsigned char* data,
                              size_t len);

  // Map an UnitEmitter's SN to its associated path (returning nullptr
  // if no such UnitEmitter exists).
  static const StringData* findUnitPath(int64_t unitSn);
  // Map an UnitEmitter's SHA1 to its associated path (returning
  // nullptr if no such UnitEmitter exists). NB: SHA1 isn't really "a
  // thing" for RepoFile. We assume the SHA1 matches the SN. This is
  // only provided for compatibility for tc-print.
  static const StringData* findUnitPath(const SHA1&);

  // Get the RepoUnitInfo for a specific key in a specific map.
  // The map must point to RepoBounds containing the bounds for RepoUnitInfo
  template <typename KeyCompare>
  static const RepoUnitInfo* findUnitInfo(
    const Blob::HashMapIndex<KeyCompare>& map, const StringData* key);

  // Get all the symbols for a specific path
  static const RepoUnitSymbols* findUnitSymbols(const StringData* path);

  RepoFile() = delete;
  RepoFile(const RepoFile&) = delete;
  RepoFile(RepoFile&&) = delete;
  RepoFile& operator=(const RepoFile&) = delete;
  RepoFile& operator=(RepoFile&&) = delete;
  ~RepoFile() = delete;
};

///////////////////////////////////////////////////////////////////////////////

// Represents either a pointer to T, or a RepoFile::Token. Does not do
// any resource management of its own (it won't clone or delete the
// memory pointed to by the T*). This always keeps the lower 2-bits
// free so it can be used in a LockFreePtrWrapper.
template <typename T>
struct TokenOrPtr {
  TokenOrPtr() : m_compact{toOpaquePtr(nullptr)} {}

  using Token = RepoFile::Token;

  bool isPtr() const { return getCompact().tag() == 0; }
  bool isToken() const { return getCompact().tag() == 1; }

  T* ptr() const {
    assertx(isPtr());
    return getCompact().ptr();
  }
  Token token() const {
    assertx(isToken());
    return (Token)(uintptr_t)getCompact().ptr();
  }

  static TokenOrPtr FromPtr(T* ptr) {
    return TokenOrPtr{toOpaquePtr(ptr)};
  }
  static TokenOrPtr FromToken(Token token) {
    return TokenOrPtr{toOpaqueToken(token)};
  }
private:
  using Compact = CompactTaggedPtr<T, uint8_t>;
  typename Compact::Opaque m_compact;

  explicit TokenOrPtr(typename Compact::Opaque o) : m_compact{o} {}

  Compact getCompact() const { return Compact{m_compact >> 2}; }

  static typename Compact::Opaque toOpaquePtr(T* p) {
    auto const c = Compact{0, p}.getOpaque();
    assertx(((c << 2) >> 2) == c);
    return c << 2;
  }
  static typename Compact::Opaque toOpaqueToken(Token token) {
    auto const c = Compact{1, (T*)(uintptr_t)token}.getOpaque();
    assertx(((c << 2) >> 2) == c);
    return c << 2;
  }
};

///////////////////////////////////////////////////////////////////////////////

using StringOrToken = TokenOrPtr<const StringData>;
using ArrayOrToken = TokenOrPtr<const ArrayData>;

///////////////////////////////////////////////////////////////////////////////

// We can't use the hardware accelerated hash functions because different
// hardware has different hash functions. We need a hash function that is stable
// between different hardware implementations.
struct TypeNameCompare {
  bool equal(const std::string_view& s1, const std::string_view& s2) const {
    return tstrcmp_slice(s1, s2) == 0;
  }
  size_t hash(const std::string_view& s) const {
    return hash_string_i_software(s.data(), s.size());
  }
};

using FuncNameCompare = Blob::CaseSensitiveCompare;
using CaseSensitiveHashMapIndex = Blob::HashMapIndex<Blob::CaseSensitiveCompare>;
using HashMapTypeIndex = Blob::HashMapIndex<TypeNameCompare>;
using HashMapFuncIndex = Blob::HashMapIndex<FuncNameCompare>;

}
