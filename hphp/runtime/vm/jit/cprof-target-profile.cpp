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

#include "hphp/runtime/vm/jit/cprof-target-profile.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Range.h>

#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/jit/array-access-profile.h"
#include "hphp/runtime/vm/jit/array-iter-profile.h"
#include "hphp/runtime/vm/jit/cls-cns-profile.h"
#include "hphp/runtime/vm/jit/coeffect-fun-param-profile.h"
#include "hphp/runtime/vm/jit/cow-profile.h"
#include "hphp/runtime/vm/jit/cprof-counter.h"
#include "hphp/runtime/vm/jit/cprof-serde.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/incref-profile.h"
#include "hphp/runtime/vm/jit/is-type-struct-profile.h"
#include "hphp/runtime/vm/jit/prof-data-target-profile.h"
#include "hphp/runtime/vm/jit/switch-profile.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/type-profile.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/util/assertions.h"
#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {

namespace {

// Cumulative target-profile outcomes, not function counts or execution counts.
// Capture counts sweep attempts for retained translations, not all live RDS
// sites; concurrent sweeps can examine the same site. Preparation precedes
// cross-translation method-conflict filtering. Installed does not imply the
// optimizer used the profile or published code. Outcome totals cover normal
// returns; an escaping exception can leave an attempt without an outcome.
struct TargetProfileCounters {
  ContProfCounter captureAttempts;
  ContProfCounter captured;
  ContProfCounter prepareAttempts;
  ContProfCounter prepared;
  ContProfCounter installed;
};

#define CPROF_TARGET_KINDS(X) \
  X(DecRef, DecRefProfile, decref) \
  X(COW, COWProfile, cow) \
  X(CoeffectFunParam, CoeffectFunParamProfile, coeffect_fun_param) \
  X(IncRef, IncRefProfile, incref) \
  X(IsTypeStruct, IsTypeStructProfile, is_type_struct) \
  X(Switch, SwitchProfile, switch) \
  X(ArrayAccess, ArrayAccessProfile, array_access) \
  X(ClsCns, ClsCnsProfile, cls_cns) \
  X(Type, TypeProfile, type) \
  X(ArrayIter, ArrayIterProfile, array_iter) \
  X(CallTarget, CallTargetProfile, call_target) \
  X(Meth, MethProfile, meth)

#define X(kind, type, name) \
  TargetProfileCounters const s_##kind##Counters{ \
    ContProfCounter("jit.cprof.target." #name ".capture_attempts"), \
    ContProfCounter("jit.cprof.target." #name ".captured"), \
    ContProfCounter("jit.cprof.target." #name ".prepare_attempts"), \
    ContProfCounter("jit.cprof.target." #name ".prepared"), \
    ContProfCounter("jit.cprof.target." #name ".installed"), \
  };
CPROF_TARGET_KINDS(X)
#undef X

const TargetProfileCounters* targetCounters(rds::ProfileKind kind) {
  switch (kind) {
#define X(kind, type, name) \
    case rds::ProfileKind::type: return &s_##kind##Counters;
    CPROF_TARGET_KINDS(X)
#undef X
    default: return nullptr;
  }
}

const TargetProfileCounters* targetCounters(ContProfTargetProfileKind kind) {
  switch (kind) {
#define X(kind, type, name) \
    case ContProfTargetProfileKind::kind: return &s_##kind##Counters;
    CPROF_TARGET_KINDS(X)
#undef X
  }
  return nullptr;
}
#undef CPROF_TARGET_KINDS

auto const s_captureAttempts = ContProfCounter(
  "jit.cprof.target.capture.attempts");
auto const s_captured = ContProfCounter(
  "jit.cprof.target.capture.captured");
// One first-match reason per failed snapshot attempt.
auto const s_captureInvalidOffset = ContProfCounter(
  "jit.cprof.target.capture.skipped.invalid_offset");
auto const s_captureUnsupportedKind = ContProfCounter(
  "jit.cprof.target.capture.skipped.unsupported_kind");
auto const s_captureEmpty = ContProfCounter(
  "jit.cprof.target.capture.skipped.empty");
auto const s_captureInvalidFuncKey = ContProfCounter(
  "jit.cprof.target.capture.skipped.invalid_func_key");
// No unique method: includes empty, BaseMeth, InterfaceMeth and invalid states.
auto const s_captureNonUniqueMethod = ContProfCounter(
  "jit.cprof.target.capture.skipped.non_unique_method");
auto const s_captureUnsupportedMethod = ContProfCounter(
  "jit.cprof.target.capture.skipped.unsupported_method");
auto const s_captureAllocationFailure = ContProfCounter(
  "jit.cprof.target.capture.skipped.allocation_failure");

auto const s_prepareAttempts = ContProfCounter(
  "jit.cprof.target.prepare.attempts");
auto const s_prepared = ContProfCounter(
  "jit.cprof.target.prepare.prepared");

// One first-match reason per failed preparation, even for a CallTarget profile
// containing several unresolved callees. Decoding failure takes precedence.
enum class PrepareRejection {
  InvalidPayload,
  UnsupportedMethod,
  MissingUnit,
  UnresolvedOrStale,
  ResolveException,
};
ContProfCounter const s_prepareRejections[] = {
  ContProfCounter("jit.cprof.target.prepare.skipped.invalid_payload"),
  ContProfCounter(
    "jit.cprof.target.prepare.skipped.unsupported_method"),
  ContProfCounter("jit.cprof.target.prepare.skipped.missing_unit"),
  ContProfCounter(
    "jit.cprof.target.prepare.skipped.unresolved_or_stale"),
  ContProfCounter(
    "jit.cprof.target.prepare.skipped.resolve_exception"),
};

// Successful method preparations with a saved receiver, partitioned into
// restored UniqueClass and fallback to UniqueMeth (not discarded profiles).
auto const s_receiverRestored = ContProfCounter(
  "jit.cprof.target.meth.receiver_restored");
auto const s_receiverUnavailable = ContProfCounter(
  "jit.cprof.target.meth.receiver_unavailable");
auto const s_receiverChangedDispatch = ContProfCounter(
  "jit.cprof.target.meth.receiver_changed_dispatch");
// Captures that retain the method but cannot encode its unique receiver hint.
auto const s_receiverNotCaptured = ContProfCounter(
  "jit.cprof.target.meth.receiver_not_captured");

auto const s_installAttempts = ContProfCounter(
  "jit.cprof.target.install.attempts");
auto const s_installed = ContProfCounter(
  "jit.cprof.target.install.installed");
// False returns from installation (allocation or payload decoding failure).
auto const s_installFailures = ContProfCounter(
  "jit.cprof.target.install.failures");

// Buffer-backed adapters for the profile's existing serialize/deserialize.
struct PayloadWriter {
  std::vector<uint8_t> payload;
};

struct PayloadReader {
  const uint8_t* data;
  size_t remaining;
  bool failed{false};
};

template<class T>
void write_raw(PayloadWriter& writer, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);

  auto const bytes = reinterpret_cast<const uint8_t*>(&value);
  writer.payload.insert(writer.payload.end(), bytes, bytes + sizeof(value));
}

template<class T>
void read_raw(PayloadReader& reader, T& value) {
  static_assert(std::is_trivially_copyable_v<T>);

  if (reader.failed || sizeof(value) > reader.remaining) {
    reader.failed = true;
    return;
  }

  std::memcpy(&value, reader.data, sizeof(value));
  reader.data += sizeof(value);
  reader.remaining -= sizeof(value);
}

void writeBytes(PayloadWriter& writer, const void* data, size_t size) {
  write_raw(writer, size);
  auto const bytes = static_cast<const uint8_t*>(data);
  writer.payload.insert(writer.payload.end(), bytes, bytes + size);
}

folly::ByteRange readBytes(PayloadReader& reader) {
  size_t size{};
  read_raw(reader, size);
  if (reader.failed || size > reader.remaining) {
    reader.failed = true;
    return {};
  }
  folly::ByteRange bytes{reader.data, size};
  reader.data += size;
  reader.remaining -= size;
  return bytes;
}

std::string readString(PayloadReader& reader) {
  auto const bytes = readBytes(reader);
  if (reader.failed) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct CallTargetWriter : PayloadWriter {
  bool failed{false};
  bool hasTarget{false};
};

void write_func(CallTargetWriter& writer, const Func* func) {
  if (!func) {
    write_raw(writer, size_t{0});
    return;
  }

  auto const key = makeContProfFuncKey(*func);
  if (!key || key->closureContextName) {
    writer.failed = true;
    return;
  }

  auto const encoded = serializeContProfFuncKey(*key);
  assertx(encoded);
  writeBytes(writer, encoded->data(), encoded->size());
  writer.hasTarget = true;
}

template<class Resolve>
struct CallTargetReader : PayloadReader {
  Resolve resolve;
};

template<class Resolve>
const Func* read_func(CallTargetReader<Resolve>& reader) {
  auto const bytes = readBytes(reader);
  if (reader.failed || bytes.empty()) return nullptr;

  auto const key = deserializeContProfFuncKey(bytes);
  if (!key) {
    reader.failed = true;
    return nullptr;
  }
  return reader.resolve(*key);
}

template<class Resolve>
std::optional<CallTargetProfile> decodeCallTargetPayload(
  const std::vector<uint8_t>& payload,
  Resolve resolve
) {
  CallTargetReader<Resolve> reader{
    {payload.data(), payload.size()}, std::move(resolve)
  };
  CallTargetProfile result{};
  result.deserialize(reader);
  if (reader.failed || reader.remaining != 0) return std::nullopt;
  return result;
}

std::optional<ContProfTargetProfile>
snapshotCallTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  assertx(profile.name);
  assertx(allocationSize == sizeof(CallTargetProfile));

  CallTargetProfile reduced{};
  TargetProfile<CallTargetProfile>::reduce(reduced, handle, allocationSize);
  CallTargetWriter writer;
  reduced.serialize(writer);
  // A concurrent reduction can see initialization before the first target.
  if (writer.failed || !writer.hasTarget) {
    (writer.failed ? s_captureInvalidFuncKey : s_captureEmpty)->increment();
    return std::nullopt;
  }

  return ContProfTargetProfile{
    ContProfTargetProfileKind::CallTarget,
    profile.bcOff,
    profile.name->toCppString(),
    std::move(writer.payload),
  };
}

const Func* resolveTargetFunc(
  const ContProfFuncKey& key,
  const std::map<std::string, Unit*>& units,
  PrepareRejection& rejection
) {
  rejection = PrepareRejection::UnresolvedOrStale;
  if (key.closureContextName) return nullptr;
  auto const unit = units.find(key.resolutionUnitPath);
  if (unit == units.end() || (key.bytecodeUnitPath &&
      units.find(*key.bytecodeUnitPath) == units.end())) {
    rejection = PrepareRejection::MissingUnit;
    return nullptr;
  }
  try {
    return resolveContProfFunc(key, *unit->second);
  } catch (...) {
    rejection = PrepareRejection::ResolveException;
    return nullptr;
  }
}

bool isSupportedMethTarget(const Func* func) {
  return func && func->isMethod() && (func->attrs() & AttrPublic) &&
    !func->isInternal() && !func->hasPrivateAncestor() &&
    !func->isClosureBody();
}

bool isSupportedMethReceiver(const Class* cls) {
  return cls && isNormalClass(cls) && !cls->isBuiltin() &&
    !cls->isClosureClass() &&
    !PreClassEmitter::IsAnonymousClassName(cls->name()->slice());
}

struct MethProfileReceiver {
  std::string unitPath;
  std::string className;
};

struct MethProfilePayload {
  ContProfFuncKey method;
  std::optional<MethProfileReceiver> receiver;
};

std::optional<MethProfilePayload>
decodeMethProfilePayload(const std::vector<uint8_t>& payload) {
  PayloadReader reader{payload.data(), payload.size()};
  auto method = deserializeContProfFuncKey(readBytes(reader));
  if (!method) return std::nullopt;

  std::optional<MethProfileReceiver> receiver;
  if (reader.remaining) {
    receiver = MethProfileReceiver{readString(reader), readString(reader)};
    if (reader.failed || reader.remaining ||
        !isValidContProfUnitPath(receiver->unitPath) ||
        receiver->className.empty() ||
        receiver->className.find('\0') != std::string::npos) {
      return std::nullopt;
    }
  }
  return MethProfilePayload{std::move(*method), std::move(receiver)};
}

const Class* resolveMethReceiver(
  const MethProfileReceiver& receiver,
  const std::map<std::string, Unit*>& units
) {
  if (units.find(receiver.unitPath) == units.end()) return nullptr;
  auto const name = lookupStaticString(folly::StringPiece{receiver.className});
  auto const cls = name ? Class::lookup(name) : nullptr;
  return isSupportedMethReceiver(cls) ? cls : nullptr;
}

std::optional<ContProfTargetProfile> snapshotMethProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  assertx(profile.name);
  assertx(allocationSize == sizeof(MethProfile));

  MethProfile reduced{};
  TargetProfile<MethProfile>::reduce(reduced, handle, allocationSize);
  auto const func = reduced.uniqueMeth();
  if (!isSupportedMethTarget(func)) {
    (func ? s_captureUnsupportedMethod : s_captureNonUniqueMethod)->increment();
    return std::nullopt;
  }

  auto const key = makeContProfFuncKey(*func);
  if (!key) {
    s_captureInvalidFuncKey->increment();
    return std::nullopt;
  }
  auto const method = serializeContProfFuncKey(*key);
  assertx(method);

  // The receiver path is a preload hint, not a bytecode compatibility key.
  // An inherited method can live in a different unit from its receiver.
  PayloadWriter writer;
  writeBytes(writer, method->data(), method->size());
  auto const cls = reduced.uniqueClass();
  bool capturedReceiver = false;
  if (isSupportedMethReceiver(cls)) {
    if (auto const path = contProfUnitPath(*cls->preClass()->unit())) {
      writeBytes(writer, path->data(), path->size());
      writeBytes(writer, cls->name()->data(), cls->name()->size());
      capturedReceiver = true;
    }
  }
  if (cls && !capturedReceiver) s_receiverNotCaptured->increment();

  return ContProfTargetProfile{
    ContProfTargetProfileKind::Meth,
    profile.bcOff,
    profile.name->toCppString(),
    std::move(writer.payload),
  };
}

template<class T>
bool addTargetProfileValue(
  const rds::Profile& key,
  const T& source,
  ProfDataTargetProfile& targetProfiles
) {
  static_assert(std::is_nothrow_copy_constructible_v<T>);
  static_assert(std::is_trivially_destructible_v<T>);

  auto const memory = std::malloc(sizeof(T));
  if (!memory) return false;
  std::unique_ptr<T, decltype(&std::free)> value{
    new (memory) T{source},
    &std::free,
  };

  targetProfiles.add(key, value.get());
  value.release();
  return true;
}

template<class T>
bool isValidRawTargetProfileSize(size_t size) {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::has_unique_object_representations_v<T>);

  if constexpr (std::is_same_v<T, SwitchProfile>) {
    static_assert(sizeof(SwitchProfile) == sizeof(uint32_t));

    return size >= sizeof(SwitchProfile) && size % sizeof(uint32_t) == 0;
  } else {
    return size == sizeof(T);
  }
}

template<class T>
bool isValidRawTargetProfile(const ContProfTargetProfile& profile) {
  // The kind determines the layout; names are opaque RDS lookup keys.
  return profile.bytecodeOffset >= 0 &&
    isValidRawTargetProfileSize<T>(profile.payload.size());
}

template<class T>
std::optional<ContProfTargetProfile>
snapshotRawTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize,
  ContProfTargetProfileKind kind
) {
  assertx(profile.name);
  assertx(isValidRawTargetProfileSize<T>(allocationSize));

  std::vector<uint8_t> payload(allocationSize);
  auto const captured = TargetProfile<T>::withTemporary(
    allocationSize, [&] (T& reduced) {
      TargetProfile<T>::reduce(reduced, handle, allocationSize);
      std::memcpy(payload.data(), &reduced, payload.size());
    }
  );
  if (!captured) {
    s_captureAllocationFailure->increment();
    return std::nullopt;
  }

  return ContProfTargetProfile{
    kind,
    profile.bcOff,
    profile.name->toCppString(),
    std::move(payload),
  };
}

template<class T>
bool installRawTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  assertx(profile.bytecodeOffset >= 0);
  assertx(profile.name);
  assertx(isValidRawTargetProfileSize<T>(profile.payload.size()));

  auto const key = rds::Profile{
    static_cast<T*>(nullptr),
    transId,
    profile.bytecodeOffset,
    profile.name,
  };

  auto const memory = std::malloc(profile.payload.size());
  if (!memory) return false;
  std::unique_ptr<T, decltype(&std::free)> value{
    new (memory) T{}, &std::free
  };
  std::memcpy(value.get(), profile.payload.data(), profile.payload.size());
  targetProfiles.add(key, value.get());
  value.release();
  return true;
}

void writeType(PayloadWriter& writer, Type type) {
  // These profiles contain cell types, so no pointer location is needed.
  // Store only the basic bits: constants and specializations can contain
  // process-local pointers.
  write_raw(writer, type.rawBits());
}

std::optional<Type> readType(PayloadReader& reader) {
  Type::bits_t bits{};
  read_raw(reader, bits);
  if (reader.failed || (bits & Type::kCell) != bits) {
    return std::nullopt;
  }

  return Type{bits, PtrLocation::Bottom};
}

std::vector<uint8_t> encodeTypeProfilePayload(const TypeProfile& profile) {
  PayloadWriter writer;
  writer.payload.reserve(sizeof(profile));
  writeType(writer, profile.type);
  write_raw(writer, profile.count);
  return std::move(writer.payload);
}

std::optional<TypeProfile>
decodeTypeProfilePayload(const std::vector<uint8_t>& payload) {
  PayloadReader reader{payload.data(), payload.size()};
  auto const type = readType(reader);
  if (!type) return std::nullopt;

  TypeProfile result{};
  result.type = *type;
  read_raw(reader, result.count);
  if (reader.failed || reader.remaining != 0) return std::nullopt;
  return result;
}

std::vector<uint8_t>
encodeArrayIterProfilePayload(const ArrayIterProfile& profile) {
  auto const result = profile.result();

  PayloadWriter writer;
  writer.payload.reserve(sizeof(profile));
  write_raw(writer, result.key_types.toBits());
  writeType(writer, result.value_type);
  return std::move(writer.payload);
}

std::optional<ArrayIterProfile>
decodeArrayIterProfilePayload(const std::vector<uint8_t>& payload) {
  PayloadReader reader{payload.data(), payload.size()};
  uint8_t keyTypes{};
  read_raw(reader, keyTypes);
  auto const allowedKeyTypes = ArrayKeyTypes::Any().toBits();
  if ((keyTypes | allowedKeyTypes) != allowedKeyTypes) {
    return std::nullopt;
  }

  auto const valueType = readType(reader);
  if (!valueType || !(*valueType <= TInitCell)) return std::nullopt;
  if (reader.remaining != 0) return std::nullopt;

  return ArrayIterProfile{
    .m_key_types = ArrayKeyTypes::FromBits(keyTypes),
    .m_value_type = *valueType,
  };
}

std::optional<ContProfTargetProfile>
snapshotTypeTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  assertx(profile.name);
  assertx(allocationSize == sizeof(TypeProfile));

  TypeProfile reduced{};
  TargetProfile<TypeProfile>::reduce(reduced, handle, allocationSize);

  return ContProfTargetProfile{
    ContProfTargetProfileKind::Type,
    profile.bcOff,
    profile.name->toCppString(),
    encodeTypeProfilePayload(reduced),
  };
}

std::optional<ContProfTargetProfile>
snapshotArrayIterTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  assertx(profile.name);
  assertx(allocationSize == sizeof(ArrayIterProfile));

  ArrayIterProfile reduced{};
  TargetProfile<ArrayIterProfile>::reduce(reduced, handle, allocationSize);

  return ContProfTargetProfile{
    ContProfTargetProfileKind::ArrayIter,
    profile.bcOff,
    profile.name->toCppString(),
    encodeArrayIterProfilePayload(reduced),
  };
}

bool installTypeTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  assertx(profile.kind == ContProfTargetProfileKind::Type);
  assertx(profile.bytecodeOffset >= 0);
  assertx(profile.name);

  auto const decoded = decodeTypeProfilePayload(profile.payload);
  if (!decoded) return false;

  return addTargetProfileValue(
    rds::Profile{
      static_cast<TypeProfile*>(nullptr),
      transId,
      profile.bytecodeOffset,
      profile.name,
    },
    *decoded,
    targetProfiles
  );
}

bool installArrayIterTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  assertx(profile.kind == ContProfTargetProfileKind::ArrayIter);
  assertx(profile.bytecodeOffset >= 0);
  assertx(profile.name);

  auto const decoded = decodeArrayIterProfilePayload(profile.payload);
  if (!decoded) return false;

  return addTargetProfileValue(
    rds::Profile{
      static_cast<ArrayIterProfile*>(nullptr),
      transId,
      profile.bytecodeOffset,
      profile.name,
    },
    *decoded,
    targetProfiles
  );
}

std::optional<int32_t> decodeDecRefProfileId(std::string_view name) {
  constexpr std::string_view kPrefix{"DecRefProfile-"};

  if (!name.starts_with(kPrefix)) return std::nullopt;

  auto const suffix = name.substr(kPrefix.size());

  int32_t profileId{};
  auto const [end, error] = std::from_chars(
    suffix.data(),
    suffix.data() + suffix.size(),
    profileId
  );

  if (error != std::errc{} || end != suffix.data() + suffix.size() ||
      profileId < -1) {
    return std::nullopt;
  }

  return profileId;
}

std::vector<uint8_t> encodeDecRefPayload(const DecRefProfile& profile) {
  PayloadWriter writer;
  writer.payload.reserve(sizeof(profile));
  profile.serialize(writer);
  return std::move(writer.payload);
}

std::optional<DecRefProfile> decodeDecRefPayload(
  const std::vector<uint8_t>& payload) {
  PayloadReader reader{payload.data(), payload.size()};
  DecRefProfile result{};
  result.deserialize(reader);

  if (reader.failed || reader.remaining != 0) return std::nullopt;
  return result;
}

std::optional<int32_t>
validateDecRefTargetProfile(const ContProfTargetProfile& profile) {
  if (profile.bytecodeOffset < 0) return std::nullopt;

  auto const profileId = decodeDecRefProfileId(profile.name);
  auto const value = decodeDecRefPayload(profile.payload);

  if (!profileId || !value) return std::nullopt;

  return profileId;
}

std::optional<ContProfTargetProfile>
snapshotDecRefTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  assertx(profile.name);
  assertx(allocationSize == sizeof(DecRefProfile));
  assertx(decodeDecRefProfileId(profile.name->slice()));

  DecRefProfile reduced{};
  TargetProfile<DecRefProfile>::reduce(reduced, handle, allocationSize);
  if (reduced.total == 0) {
    s_captureEmpty->increment();
    return std::nullopt;
  }

  assertx(
    reduced.datatype == kNoDataTypesSeen ||
    reduced.datatype == kMultipleDataTypesSeen ||
    (isRealType(reduced.datatype) &&
     reduced.datatype == dt_modulo_persistence(reduced.datatype))
  );

  return ContProfTargetProfile{
    ContProfTargetProfileKind::DecRef,
    static_cast<int32_t>(profile.bcOff),
    profile.name->toCppString(),
    encodeDecRefPayload(reduced),
  };
}

bool installDecRefTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  assertx(profile.kind == ContProfTargetProfileKind::DecRef);
  assertx(profile.bytecodeOffset >= 0);
  assertx(profile.name);

  auto const decoded = decodeDecRefPayload(profile.payload);
  if (!decoded) return false;

  return addTargetProfileValue(
    rds::Profile{
      static_cast<DecRefProfile*>(nullptr),
      transId,
      profile.bytecodeOffset,
      profile.name,
    },
    *decoded,
    targetProfiles
  );
}

}

bool isValidContProfTargetProfile(const ContProfTargetProfile& profile) {
  switch (profile.kind) {
    case ContProfTargetProfileKind::DecRef:
      return validateDecRefTargetProfile(profile).has_value();

    case ContProfTargetProfileKind::COW:
      return isValidRawTargetProfile<COWProfile>(profile);

    case ContProfTargetProfileKind::CoeffectFunParam:
      return isValidRawTargetProfile<CoeffectFunParamProfile>(profile);

    case ContProfTargetProfileKind::IncRef:
      return isValidRawTargetProfile<IncRefProfile>(profile);

    case ContProfTargetProfileKind::IsTypeStruct:
      return isValidRawTargetProfile<IsTypeStructProfile>(profile);

    case ContProfTargetProfileKind::Switch:
      return isValidRawTargetProfile<SwitchProfile>(profile);

    case ContProfTargetProfileKind::ArrayAccess:
      return isValidRawTargetProfile<ArrayAccessProfile>(profile);

    case ContProfTargetProfileKind::ClsCns:
      return isValidRawTargetProfile<ClsCnsProfile>(profile);

    case ContProfTargetProfileKind::Type:
      return profile.bytecodeOffset >= 0 &&
        decodeTypeProfilePayload(profile.payload).has_value();

    case ContProfTargetProfileKind::ArrayIter:
      return profile.bytecodeOffset >= 0 &&
        decodeArrayIterProfilePayload(profile.payload).has_value();

    case ContProfTargetProfileKind::CallTarget:
      return profile.bytecodeOffset >= 0 &&
        decodeCallTargetPayload(
          profile.payload, [](const ContProfFuncKey&) -> const Func* {
            return nullptr;
          }
        ).has_value();

    case ContProfTargetProfileKind::Meth:
      return profile.bytecodeOffset >= 0 &&
        decodeMethProfilePayload(profile.payload).has_value();
  }

  return false;
}

static std::optional<ContProfTargetProfile> snapshotTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  if (profile.bcOff < 0) {
    s_captureInvalidOffset->increment();
    return std::nullopt;
  }

  switch (profile.kind) {
    case rds::ProfileKind::DecRefProfile:
      return snapshotDecRefTargetProfile(profile, handle, allocationSize);

    case rds::ProfileKind::COWProfile:
      return snapshotRawTargetProfile<COWProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::COW
      );

    case rds::ProfileKind::CoeffectFunParamProfile:
      return snapshotRawTargetProfile<CoeffectFunParamProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::CoeffectFunParam
      );

    case rds::ProfileKind::IncRefProfile:
      return snapshotRawTargetProfile<IncRefProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::IncRef
      );

    case rds::ProfileKind::IsTypeStructProfile:
      return snapshotRawTargetProfile<IsTypeStructProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::IsTypeStruct
      );

    case rds::ProfileKind::SwitchProfile:
      return snapshotRawTargetProfile<SwitchProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::Switch
      );

    case rds::ProfileKind::ArrayAccessProfile:
      return snapshotRawTargetProfile<ArrayAccessProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::ArrayAccess
      );

    case rds::ProfileKind::ClsCnsProfile:
      return snapshotRawTargetProfile<ClsCnsProfile>(
        profile,
        handle,
        allocationSize,
        ContProfTargetProfileKind::ClsCns
      );

    case rds::ProfileKind::TypeProfile:
      return snapshotTypeTargetProfile(profile, handle, allocationSize);

    case rds::ProfileKind::ArrayIterProfile:
      return snapshotArrayIterTargetProfile(profile, handle, allocationSize);

    case rds::ProfileKind::CallTargetProfile:
      return snapshotCallTargetProfile(profile, handle, allocationSize);

    case rds::ProfileKind::MethProfile:
      return snapshotMethProfile(profile, handle, allocationSize);

    default:
      s_captureUnsupportedKind->increment();
      return std::nullopt;
  }
}

std::optional<ContProfTargetProfile> snapshotContProfTargetProfile(
  const rds::Profile& profile,
  rds::Handle handle,
  uint32_t allocationSize
) {
  s_captureAttempts->increment();
  auto const counters = targetCounters(profile.kind);
  if (counters) counters->captureAttempts->increment();
  auto result = snapshotTargetProfile(profile, handle, allocationSize);
  if (result) {
    s_captured->increment();
    assertx(counters);
    counters->captured->increment();
  }
  return result;
}

std::vector<std::string>
contProfTargetProfileUnitPaths(const ContProfTargetProfile& profile) {
  std::vector<std::string> result;
  auto const addPaths = [&] (const ContProfFuncKey& key) {
    if (key.closureContextName) return;
    result.push_back(key.resolutionUnitPath);
    if (key.bytecodeUnitPath) result.push_back(*key.bytecodeUnitPath);
  };
  if (profile.kind == ContProfTargetProfileKind::Meth) {
    auto const decoded = decodeMethProfilePayload(profile.payload);
    if (decoded) {
      addPaths(decoded->method);
      if (decoded->receiver) result.push_back(decoded->receiver->unitPath);
    }
    return result;
  }
  if (profile.kind != ContProfTargetProfileKind::CallTarget) return result;

  auto const decoded = decodeCallTargetPayload(
    profile.payload, [&] (const ContProfFuncKey& key) -> const Func* {
      addPaths(key);
      return nullptr;
    }
  );
  if (!decoded) result.clear();
  return result;
}

static std::optional<ContProfPreparedTargetProfile> prepareTargetProfile(
  const ContProfTargetProfile& profile,
  const std::map<std::string, Unit*>& units,
  PrepareRejection& rejection
) {
  if (profile.bytecodeOffset < 0) return std::nullopt;

  if (profile.kind == ContProfTargetProfileKind::Meth) {
    auto const decoded = decodeMethProfilePayload(profile.payload);
    if (!decoded) return std::nullopt;
    auto const func = resolveTargetFunc(decoded->method, units, rejection);
    if (!func) return std::nullopt;
    if (!isSupportedMethTarget(func)) {
      rejection = PrepareRejection::UnsupportedMethod;
      return std::nullopt;
    }

    auto const cls = decoded->receiver
      ? resolveMethReceiver(*decoded->receiver, units)
      : nullptr;
    std::unique_ptr<MethProfile> meth;
    // Guard the current class only if it still dispatches to this method.
    // Otherwise retain the method and let UniqueMeth guard current dispatch.
    if (cls && cls->lookupMethod(func->name()) == func) {
      meth = std::make_unique<MethProfile>();
      meth->reportMeth(cls, func, nullptr);
      s_receiverRestored->increment();
    } else {
      meth = std::make_unique<MethProfile>(MethProfile::fromUniqueMeth(func));
      if (decoded->receiver) {
        (cls ? s_receiverChangedDispatch : s_receiverUnavailable)->increment();
      }
    }

    return ContProfPreparedTargetProfile{
      profile.kind,
      profile.bytecodeOffset,
      makeStaticString(profile.name),
      {},
      nullptr,
      std::move(meth),
    };
  }

  if (profile.kind == ContProfTargetProfileKind::CallTarget) {
    bool resolved = true;
    auto const value = decodeCallTargetPayload(
      profile.payload, [&] (const ContProfFuncKey& key) -> const Func* {
        auto reason = PrepareRejection::UnresolvedOrStale;
        auto const func = resolveTargetFunc(key, units, reason);
        if (resolved && !func) rejection = reason;
        resolved &= func != nullptr;
        return func;
      }
    );
    // A missing target is not an empty slot: keep all counts or skip the site.
    if (!value) {
      rejection = PrepareRejection::InvalidPayload;
      return std::nullopt;
    }
    if (!resolved) return std::nullopt;

    return ContProfPreparedTargetProfile{
      profile.kind,
      profile.bytecodeOffset,
      makeStaticString(profile.name),
      {},
      std::make_unique<CallTargetProfile>(*value),
    };
  }

  if (!isValidContProfTargetProfile(profile)) return std::nullopt;

  return ContProfPreparedTargetProfile{
    profile.kind,
    profile.bytecodeOffset,
    makeStaticString(profile.name),
    profile.payload,
  };
}

std::optional<ContProfPreparedTargetProfile> prepareContProfTargetProfile(
  const ContProfTargetProfile& profile,
  const std::map<std::string, Unit*>& units
) {
  s_prepareAttempts->increment();
  auto const counters = targetCounters(profile.kind);
  if (counters) counters->prepareAttempts->increment();
  auto rejection = PrepareRejection::InvalidPayload;
  auto result = prepareTargetProfile(profile, units, rejection);
  if (result) {
    s_prepared->increment();
    assertx(counters);
    counters->prepared->increment();
  } else {
    s_prepareRejections[static_cast<size_t>(rejection)]->increment();
  }
  return result;
}

static bool installTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  switch (profile.kind) {
    case ContProfTargetProfileKind::DecRef:
      return installDecRefTargetProfile(profile, transId, targetProfiles);

    case ContProfTargetProfileKind::COW:
      return installRawTargetProfile<COWProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::CoeffectFunParam:
      return installRawTargetProfile<CoeffectFunParamProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::IncRef:
      return installRawTargetProfile<IncRefProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::IsTypeStruct:
      return installRawTargetProfile<IsTypeStructProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::Switch:
      return installRawTargetProfile<SwitchProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::ArrayAccess:
      return installRawTargetProfile<ArrayAccessProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::ClsCns:
      return installRawTargetProfile<ClsCnsProfile>(
        profile,
        transId,
        targetProfiles
      );

    case ContProfTargetProfileKind::Type:
      return installTypeTargetProfile(profile, transId, targetProfiles);

    case ContProfTargetProfileKind::ArrayIter:
      return installArrayIterTargetProfile(profile, transId, targetProfiles);

    case ContProfTargetProfileKind::CallTarget:
      assertx(profile.callTarget);
      return addTargetProfileValue(
        rds::Profile{
          static_cast<CallTargetProfile*>(nullptr),
          transId,
          profile.bytecodeOffset,
          profile.name,
        },
        *profile.callTarget,
        targetProfiles
      );

    case ContProfTargetProfileKind::Meth:
      assertx(profile.meth);
      return addTargetProfileValue(
        rds::Profile{
          static_cast<MethProfile*>(nullptr),
          transId,
          profile.bytecodeOffset,
          profile.name,
        },
        *profile.meth,
        targetProfiles
      );
  }

  return false;
}

bool installContProfTargetProfile(
  const ContProfPreparedTargetProfile& profile,
  TransID transId,
  ProfDataTargetProfile& targetProfiles
) {
  s_installAttempts->increment();
  auto const installed = installTargetProfile(profile, transId, targetProfiles);
  if (installed) {
    s_installed->increment();
    auto const counters = targetCounters(profile.kind);
    assertx(counters);
    counters->installed->increment();
  } else {
    s_installFailures->increment();
  }
  return installed;
}

}
