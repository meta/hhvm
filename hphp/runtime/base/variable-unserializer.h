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

#include "hphp/runtime/base/contiguous-source.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/util/compact-tagged-ptrs.h"

namespace HPHP {
struct StringBuffer;
struct ZStdDecompressSource;

///////////////////////////////////////////////////////////////////////////////

enum class UnserializeMode {
  Value = 0,
  Key = 1,
  ColKey = 2,
};

struct InvalidAllowedClassesException : Exception {
};

/*
 * Source-independent state and types shared by every VariableUnserializerImpl
 * instantiation. Keeping Type here (rather than in the template) makes it a
 * single enum type across all byte sources, so e.g. VariableUnserializer::Type
 * and ZStdVariableUnserializer::Type are the same type and interchangeable.
 */
struct VariableUnserializerBase {
  /*
   * Supported unserialization formats.
   */
  enum class Type {
    Serialize,
    Internal,
    APCSerialize,
    DebuggerSerialize,
    Last
  };
};

/*
 * Unserializes a serialized string into a Variant. Templated on the byte Source
 * so the common in-memory path (ContiguousSource) is a fully monomorphic
 * instantiation with zero per-byte dispatch overhead, while the streaming
 * zstd-decompression path (ZStdDecompressSource) is a separate instantiation
 * selected at the call site. Neither instantiation pays for the other.
 */
template <class Source>
struct VariableUnserializerImpl : VariableUnserializerBase {

  /*
   * Construct an unserializer, with an optional whitelist of classes to
   * allow. In the whitelist, empty_array means "allow no classes", while
   * null_array means allow any classes.  We default to null_array since
   * serialization is not limited inside the VM.
   */
  VariableUnserializerImpl(
    const char* str,
    size_t len,
    Type type,
    bool allowUnknownSerializableClass = false,
    const Array& options = null_array);

  /*
   * Optimize for output that is expected to be immortal and immutable.
   */
  void setReadOnly() { m_readOnly = true; }

  /*
   * Main API; unserialize the buffer and return as a Variant.
   */
  Variant unserialize();

  void setUnitFilename(const StringData* name) {
    assertx(name->isStatic());
    assertx(m_type == Type::Internal);
    m_unitFilename = name;
  }

  // Should we be calling the pure callbacks
  void setPure() { m_pure = true; }

 private:
  /*
   * Used to object property values that are overwritten by later entries.
   * For array element values, later values for the key override earlier ones.
   */
  Array m_overwrittenList = Array::CreateVec();

  bool readOnly() const { return m_readOnly; }

  /*
   * Read the appropriate data type from buffer.
   */
  int64_t readInt();
  double readDouble();
  char readChar();

  // Return a StringPiece of up to n characters pointing into the source window
  folly::StringPiece readStr(unsigned n);

  /*
   * Read a character and throw if it differs from expected.
   */
  void expectChar(char expected);

  /*
   * Attempt to consume a serialized string with content matching str.
   * Return false and rewind stream on non-standard format or content mismatch.
   * Only available for a contiguous source; a no-op returning false otherwise.
   */
  bool matchString(folly::StringPiece str);

  /*
   * Accessors.
   */
  Type type() const;
  bool allowUnknownSerializableClass() const;
  char peek();
  char peekBack();
  bool endOfBuffer();

  /*
   * True if clsName is allowed to be unserialized.
   */
  bool whitelistCheck(const OptString& clsName) const;

  /*
   * Push v onto the vector of refs for future reference.
   */
  void add(tv_lval v, UnserializeMode mode);

  /*
   * Preallocate memory for an expected number of values to be added
   * (excluding those with mode UnserializeMode::Key).
   */
  void reserveForAdd(size_t count);

  /*
   * Used by the 'r' encoding to re-use previously deserialized values.
   */
  TypedValue getByVal(int64_t id);

  /*
   * Register an object that needs its __wakeup() method called after
   * unserialization of the top-level value is complete.
   */
  void addSleepingObject(const Object&);

private:
  void checkElemTermination();

  Type m_type;
  bool m_readOnly;
  // Owns the byte cursor. ContiguousSource is a zero-overhead view over an
  // in-memory buffer; ZStdDecompressSource streams from a compressed frame.
  Source m_source;
  req::vector<tv_rval> m_refs;
  bool m_unknownSerializable;
  const Array& m_options; // e.g. classes allowed to be unserialized
  req::vector<Object> m_sleepingObjects;
  bool m_forceDArrays;
  bool m_markLegacyArrays;
  bool m_pure{false};            // should we call the pure callbacks?
  /* unitFilename should be set when we are deserializing
   * an adata from the repo--it is needed to *re*construct the
   * correct provenance tag */
  const StringData* m_unitFilename{nullptr};

  void unserializeVariant(tv_lval self,
                          UnserializeMode mode = UnserializeMode::Value);
  Array unserializeArray();
  void unserializeProvenanceTag();
  Array unserializeDict();
  Array unserializeVec();
  Array unserializeKeyset();
  Array unserializeVArray();
  Array unserializeDArray();
  Array unserializeBespokeTypeStructure();
  folly::StringPiece unserializeStringPiece(char delimiter0 = '"',
                                            char delimiter1 = '"');
  OptString unserializeString(char delimiter0 = '"', char delimiter1 = '"');
  void unserializeCollection(ObjectData* obj, int64_t sz, char type);
  void unserializeVector(ObjectData*, int64_t sz,
                         char type);
  void unserializeMap(ObjectData*, int64_t sz, char type);
  void unserializeSet(ObjectData*, int64_t sz, char type);
  void unserializePair(ObjectData*, int64_t sz, char type);
  void unserializePropertyValue(tv_lval v, int remainingProps);
  bool tryUnserializeStrIntMap(struct BaseMap* map, int64_t sz);
  void unserializeProp(ObjectData* obj, const OptString& key, Class* ctx,
                       const OptString& realKey, int nProp);
  void unserializeRemainingProps(Object& obj, int remainingProps,
                                 Variant& serializedNativeData,
                                 bool& hasSerializedNativeData);
};

// Default path: a contiguous in-memory buffer, no decompression overhead.
using VariableUnserializer = VariableUnserializerImpl<ContiguousSource>;
// Streaming-zstd variant, selected by unserialize()'s zstd option.
using ZStdVariableUnserializer = VariableUnserializerImpl<ZStdDecompressSource>;

///////////////////////////////////////////////////////////////////////////////
}
