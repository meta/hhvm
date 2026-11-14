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

#include "hphp/runtime/base/repo-auth-type.h"

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/prologue-id.h"

#include "hphp/util/optional.h"

#include <string>
#include <vector>

namespace HPHP {
//////////////////////////////////////////////////////////////////////

struct Unit;
struct Class;
struct Func;
struct StringData;
struct ArrayData;
struct SrcKey;
struct TypeAlias;

namespace jit {
//////////////////////////////////////////////////////////////////////

// Flags controlling payload selection during sandbox Jumpstart serialization
// and side effects during deserialization. Deserialization always consumes each
// section from the stream so files produced with any flag combination retain
// the same framing.
enum class SBWarmupFlags : uint32_t {
  None    = 0,
  Preload = 1 << 0,  // Unit preload paths
  Jit     = 1 << 1,  // Async JIT profile records
  Apc     = 1 << 2,  // APC extension warmup data
  All     = Preload | Jit | Apc,
};

inline SBWarmupFlags operator|(SBWarmupFlags a, SBWarmupFlags b) {
  return static_cast<SBWarmupFlags>(
    static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline SBWarmupFlags operator&(SBWarmupFlags a, SBWarmupFlags b) {
  return static_cast<SBWarmupFlags>(
    static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool operator!(SBWarmupFlags f) {
  return static_cast<uint32_t>(f) == 0;
}

struct ProfDataSerializer {
  enum class FileMode {
    Create,
    Append,
    // Don't open or write any file; only populate the mappers.
    Discard
  };

  explicit ProfDataSerializer(const std::string& name, FileMode mode);
  ~ProfDataSerializer();

  friend void write_raw(ProfDataSerializer& ser, const void* data, size_t sz);

  using Id = size_t;

  std::pair<Id, bool> serialize(const Unit*);
  std::pair<Id, bool> serialize(const Func*);
  std::pair<Id, bool> serialize(const Class*);
  std::pair<Id, bool> serialize(const TypeAlias*);
  std::pair<Id, bool> serialize(const ArrayData*);
  std::pair<Id, bool> serialize(const StringData*);
  std::pair<Id, bool> serialize(const RepoAuthType::Array*);
  std::pair<Id, bool> serialize(FuncId);

  bool present(const Class*) const;
  bool present(const TypeAlias*) const;

  // Units in the order they were first serialized, which is the order in which
  // the deserializer will first look them up.
  std::vector<const Unit*> unitsInSerializationOrder() const;

  // Atomically create the output file, or throw runtime error upon failure.
  void finalize();

  const std::string& filename() const { return baseFileName; }

  struct Mappers {
  private:
    template <typename T>
    struct Mapper {
      hphp_fast_map<T, Id> map;
      Id next = 1;

      std::pair<Id, bool> get(T t) {
        if (!t) return std::make_pair(0, true);
        auto const it = map.find(t);
        if (it != map.end()) return std::make_pair(it->second, true);
        map.emplace(t, next);
        return std::make_pair(next++, false);
      }

      void assign(Id id, T t) {
        assertx(id > 0);
        assertx(t);
        auto const DEBUG_ONLY insert = map.emplace(t, id);
        assertx(insert.second);
        next = std::max(next, id + 1);
      }

      bool present(T t) const { return !t || (map.count(t) > 0); }
    };

    Mapper<const Unit*> unitMap;
    Mapper<const Func*> funcMap;
    Mapper<const Class*> classMap;
    Mapper<const TypeAlias*> typeAliasMap;
    Mapper<const StringData*> stringMap;
    Mapper<const ArrayData*> arrayMap;
    Mapper<const RepoAuthType::Array*> ratMap;
    Mapper<FuncId::Int> funcIdMap;

    friend struct ProfDataSerializer;
    friend struct ProfDataDeserializer;
  };

  Mappers&& getMappers() { return std::move(mappers); }
  void setMappers(Mappers m) { mappers = std::move(m); }
private:
  int fd;
  static constexpr uint32_t buffer_size = 8192;
  uint32_t offset{0};
  char buffer[buffer_size];
  std::string baseFileName;
  std::string fileName;
  const FileMode fileMode;
  Mappers mappers;
};

struct ProfDataDeserializer {
  explicit ProfDataDeserializer(const std::string& name);
  ~ProfDataDeserializer();

  friend void read_raw(ProfDataDeserializer& ser, void* data, size_t sz);

  Unit* getUnit(ProfDataSerializer::Id) const;
  Func* getFunc(ProfDataSerializer::Id) const;
  Class* getClass(ProfDataSerializer::Id) const;
  const TypeAlias* getTypeAlias(ProfDataSerializer::Id) const;
  ArrayData* getArray(ProfDataSerializer::Id) const;
  StringData* getString(ProfDataSerializer::Id) const;
  const RepoAuthType::Array* getArrayRAT(ProfDataSerializer::Id) const;
  FuncId getFuncId(ProfDataSerializer::Id) const;

  void record(ProfDataSerializer::Id, Unit*);
  void record(ProfDataSerializer::Id, Func*);
  void record(ProfDataSerializer::Id, Class*);
  void record(ProfDataSerializer::Id, const TypeAlias*);
  void record(ProfDataSerializer::Id, ArrayData*);
  void record(ProfDataSerializer::Id, StringData*);
  void record(ProfDataSerializer::Id, const RepoAuthType::Array*);
  void record(ProfDataSerializer::Id, FuncId);

  void setExtraDataOffset(off_t off) { extraDataOffset = off; }

  bool done();

  ProfDataSerializer::Mappers getMappers() const;

 private:
  int fd;
  static constexpr uint32_t buffer_size = 8192;
  uint32_t offset{buffer_size};

  // Logical offset past the end of the profile data in the serialized file,
  // which may be the start of extra data that is not part of the proper JIT
  // profile data.
  off_t extraDataOffset{0};

  uint32_t totalBytesRead{0};

  char buffer[buffer_size];

  template <typename T>
  struct RevMapper {
    std::vector<T> map;

    T get(ProfDataSerializer::Id id) const {
      if (!id) return T{};
      always_assert(id < map.size() && map[id]);
      return map[id];
    }

    void record(ProfDataSerializer::Id id, T t) {
      assertx(id > 0);
      assertx(t);
      if (id >= map.size()) map.resize(id+1);
      always_assert(!map[id]);
      map[id] = t;
    }
  };

  RevMapper<Unit*> unitMap;
  RevMapper<Func*> funcMap;
  RevMapper<Class*> classMap;
  RevMapper<const TypeAlias*> typeAliasMap;
  RevMapper<StringData*> stringMap;
  RevMapper<ArrayData*> arrayMap;
  RevMapper<const RepoAuthType::Array*> ratMap;
  RevMapper<FuncId::Int> funcIdMap;

  friend std::string deserializeProfData(const std::string&, int, bool);
};

void write_raw(ProfDataSerializer& ser, const void* data, size_t sz);
void read_raw(ProfDataDeserializer& ser, void* data, size_t sz);

template<typename T>
void write_raw(ProfDataSerializer& ser, const T& t) {
  static_assert(!std::is_pointer_v<T> &&
                !std::is_member_object_pointer_v<T> &&
                !std::is_member_function_pointer_v<T>,
                "Pointers need to be mapped using Ids");
  static_assert(!std::is_same_v<T, FuncId>,
                "FuncIds need to be mapped using Ids");
  write_raw(ser, &t, sizeof(t));
}

template<typename T>
void read_raw(ProfDataDeserializer& ser, T& t) {
  static_assert(!std::is_pointer_v<T> &&
                !std::is_member_object_pointer_v<T> &&
                !std::is_member_function_pointer_v<T>,
                "Pointers need to be mapped using Ids");
  static_assert(!std::is_same_v<T, FuncId>,
                "FuncIds need to be mapped using Ids");
  read_raw(ser, &t, sizeof(t));
}

template<typename T>
T read_raw(ProfDataDeserializer& ser) {
  std::aligned_storage_t<sizeof(T), alignof(T)> t;
  read_raw(ser, t);
  return reinterpret_cast<T&>(t);
}

void write_raw_string(ProfDataSerializer& ser, const StringData* str);
StringData* read_raw_string(ProfDataDeserializer& ser, bool skip = false);
void write_string(ProfDataSerializer& ser, const StringData* str);
void write_string(ProfDataSerializer& ser, const std::string_view& str);
StringData* read_string(ProfDataDeserializer& ser);
std::string read_cpp_string(ProfDataDeserializer& ser);
void write_array(ProfDataSerializer& ser, const ArrayData* arr);
ArrayData* read_array(ProfDataDeserializer& ser);
void write_array_rat(ProfDataSerializer& ser, const RepoAuthType::Array*);
const RepoAuthType::Array* read_array_rat(ProfDataDeserializer& ser);
void write_unit(ProfDataSerializer& ser, const Unit* unit);
Unit* read_unit(ProfDataDeserializer& ser);
void write_class(ProfDataSerializer& ser, const Class* cls);
Class* read_class(ProfDataDeserializer& ser);
void write_lclass(ProfDataSerializer& ser, const LazyClassData cls);
LazyClassData read_lclass(ProfDataDeserializer& ser);
void write_typealias(ProfDataSerializer& ser, const TypeAlias* td);
const TypeAlias* read_typealias(ProfDataDeserializer& ser);
void write_func(ProfDataSerializer& ser, const Func* func);
Func* read_func(ProfDataDeserializer& ser);
void write_clsmeth(ProfDataSerializer& ser, ClsMethDataRef clsMeth);
ClsMethDataRef read_clsmeth(ProfDataDeserializer& ser);
void write_regionkey(ProfDataSerializer& ser, const RegionEntryKey& regionkey);
RegionEntryKey read_regionkey(ProfDataDeserializer& des);
void write_prologueid(ProfDataSerializer& ser, const PrologueID& pid);
PrologueID read_prologueid(ProfDataDeserializer& des);
void write_srckey(ProfDataSerializer& ser, SrcKey sk);
SrcKey read_srckey(ProfDataDeserializer& des);
void write_layout(ProfDataSerializer& ser, ArrayLayout layout);
ArrayLayout read_layout(ProfDataDeserializer& des);
void write_func_id(ProfDataSerializer& ser, FuncId fid);
FuncId read_func_id(ProfDataDeserializer& des);

// Return an empty string upon success, and a string that describes the reason
// of failure otherwise.
std::string serializeProfData(const std::string& filename);
std::string serializeOptProfData(const std::string& filename);

std::string serializeSBProfData(
  const std::string& root,
  const std::string& filename,
  SBWarmupFlags flags = SBWarmupFlags::All
);

// If loadRDS is true, only the RDS ordering information will be
// loaded.
std::string deserializeProfData(const std::string& filename,
                                int numWorkers,
                                bool loadRDS);

bool tryDeserializePartialProfData(const std::string& filename,
                                   int numWorkers,
                                   bool loadRDS);

std::string deserializeSBProfData(const std::string& root,
                                  const std::string& filename,
                                  SBWarmupFlags flags = SBWarmupFlags::All);

// Return whether or not serialization of profile data for optimized code is
// enabled.
bool serializeOptProfEnabled();

// Get the name of the file deserialized if any.
Optional<std::string> getFilenameDeserialized();

bool didDeserializeSBProfData();

//////////////////////////////////////////////////////////////////////
} }
