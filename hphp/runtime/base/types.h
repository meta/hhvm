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

#include "hphp/runtime/vm/func-id.h"

#include "hphp/util/blob-encoder.h"
#include "hphp/util/ptr.h"

#include <cstdint>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct OptString;
struct StaticString;
struct Array;
struct Variant;

#define uninit_variant    tvAsCVarRef(&immutable_uninit_base)
#define init_null_variant tvAsCVarRef(&immutable_null_base)

extern const OptString null_string;
extern const Array null_array;

// Use String::Empty() if you're returning String
// Use empty_string_tv() if you're returning TypedValue
// Or use these if you need to pass by const reference:
extern const StaticString empty_string_ref; // const StaticString&

///////////////////////////////////////////////////////////////////////////////

struct StringData;
using PackedStringPtr = PackedPtr<const StringData>;

template<>
struct BlobEncoderHelper<PackedStringPtr> {
  // Implemented in string-data.cpp
  static void serde(BlobEncoder&, PackedStringPtr);
  static void serde(BlobDecoder&, PackedStringPtr&);
};

///////////////////////////////////////////////////////////////////////////////

/*
 * Program counters in the bytecode interpreter.
 *
 * Normally points to an Opcode, but has type const uchar* because
 * during a given instruction it is incremented while decoding
 * immediates and may point to arbitrary bytes.
 */
using PC = const uint8_t*;

/*
 * Id type for various components of a unit that have to have unique
 * identifiers.  For example, function ids, class ids, string literal
 * ids.
 */
using Id = int;
constexpr Id kInvalidId = -1;


/*
 * Id type for local names.  Indexes into the local name table of the func.
 */
using LocalName = int;
constexpr LocalName kInvalidLocalName = -1;

/*
 * A local index alongside an local name id.
 */
struct NamedLocal {
  LocalName name;
  int32_t id;
};

/*
 * Translation IDs.
 *
 * These represent compilation units for the JIT, and are used to key into
 * several runtime structures for finding profiling data or tracking
 * translation information.
 *
 * Because we often convert between JIT block IDs and TransIDs, these are
 * signed integers (blocks can have negative IDs).  However, negative block IDs
 * logically correspond to blocks without associated translations---hence,
 * negative TransID's are simply invalid.
 *
 * kInvalidTransID should be used when initializing or checking against a
 * sentinel value.  To ask if a TransID is meaningful in a translation context,
 * use isValidTransID().
 */
using TransID = int32_t;
constexpr TransID kInvalidTransID = -1;

constexpr bool isValidTransID(TransID transID) {
  return transID >= 0;
}

/*
 * Bytecode offsets.  Used for both absolute offsets and relative offsets.
 */
using Offset = int32_t;
constexpr Offset kInvalidOffset = std::numeric_limits<Offset>::max();

/*
 * Various fields in the VM's runtime have indexes that are addressed
 * using this "slot" type.  For example: methods, properties, class
 * constants.
 *
 * No slot value greater than or equal to kInvalidSlot will actually
 * be used for one of these.
 */
using Slot = uint32_t;
constexpr Slot kInvalidSlot = -1;

///////////////////////////////////////////////////////////////////////////////
}
