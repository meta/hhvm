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

#include <atomic>

#include "hphp/util/atomic.h"
#include "hphp/util/hash.h"
#include "hphp/util/lock.h"

#include "hphp/runtime/base/type-variant.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

enum class APCHandleLevel {
  Outer, // directly referenced by the apc store
  Inner, // referenced by some other Inner or Outer handle
  OuterAcyclic // Caller guarantees no cycles, otherwise the same as Outer
};

// handle kind, instead of overloading DataType
enum class APCKind: uint8_t {
  Uninit, Null, Bool,  // see APCHandle::isSingletonKind before updating
  Int, Double,
  PersistentFunc,
  PersistentClass,
  LazyClass,
  PersistentClsMeth,
  SharedArray,
  SharedBespoke,
  SharedString,
  StaticArray,
  StaticBespoke,
  StaticString,
  CopiedVec, CopiedLegacyVec,
  CopiedDict, CopiedLegacyDict,
  CopiedKeyset,
  CopiedObject, CopiedCollection,
  SerializedVec,
  SerializedDict,
  SerializedKeyset,
  SerializedObject,
  FuncEntity,
  ClsMeth,
  RFunc,
  RClsMeth
};

/*
 * An APCHandle is the externally visible handle for in-memory APC values.  The
 * main role of the APCHandle is to hold the type information of the value,
 * manage the lifetime.  When new values are added to APC, they are created
 * using APCHandle::Create.
 *
 * Internally the family of APC entities (all APCXXXX classes, e.g. APCString,
 * APCArray, ...) embed the handle and provide an API to return the instance
 * from the handle pointer. Examples:
 *
 *    APCString                         APCTypedValue
 *   ----------------                  --------------
 *   | APCHandle    |                  | SharedData |
 *   | StringData   |                  | APCHandle  |
 *   ----------------                  --------------
 *
 * So, for an APCString the caller gets back an APCHandle* pointing to the
 * location of the field in the class.  APCString::fromHandle(APCHandle*
 * handle) will simply return the pointer itself as the handle sits at offset 0
 * (reinterpret_cast<APCString*>(handle)).  For an APCTypedValue,
 * APCTypedValue::fromHandle(APCHandle* handle) would return the handle pointer
 * minus the offset (in words) of the handle.  Normally the handle sits at
 * position 0 so the call ends up being a no-op.
 *
 * APCTypedValue is, at the moment, the only entity with the handle at a
 * non-zero offset and that is because the layout of APCTypedValue and
 * TypedValue have to be the same, and we let the DataType field in APCHandle
 * be used through a TypedValue*.  This layout allows for an important
 * optimization when an APCTypedValue is contained inside an APCArray---in such
 * cases the APCTypedValue is returned directly instead of creating another
 * TypedValue.
 *
 * APCHandle::kind() determines the representation (APCString, APCObject,
 * or APCTypedValue), and the DataType, for APCTypedValue.
 *
 *  APCKind           Representation  DataType
 *  -------           --------------  --------
 *  Uninit            APCTypedValue   KindOfUninit
 *  Null              APCTypedValue   KindOfNull
 *  Bool              APCTypedValue   KindOfBool
 *  Int               APCTypedValue   KindOfInt64
 *  Double            APCTypedValue   KindOfDouble
 *  StaticArray       APCTypedValue   (a persistent array type)
 *  StaticBespoke     APCTypedValue   (a persistent array type)
 *  StaticString      APCTypedValue   KindOfString
 *  SharedArray       APCTypedValue   (a persistent array type)
 *  SharedBespoke     APCTypedValue   (a persistent array type)
 *  SharedString      APCTypedValue   KindOfString
 *  CopiedVec         APCArray        kInvalidDataType
 *  CopiedLegacyVec   APCArray        kInvalidDataType
 *  CopiedDict        APCArray        kInvalidDataType
 *  CopiedLegacyDict  APCArray        kInvalidDataType
 *  CopiedKeyset      APCArray        kInvalidDataType
 *  CopiedObject      APCObject       kInvalidDataType
 *  CopiedCollection  APCObject       kInvalidDataType
 *  SerializedArray   APCString       kInvalidDataType
 *  SerializedVec     APCString       kInvalidDataType
 *  SerializedDict    APCString       kInvalidDataType
 *  SerializedKeyset  APCString       kInvalidDataType
 *  SerializedObject  APCString       kInvalidDataType
 *  RClsMeth          APCRClsMeth     kInvalidDataType
 *  RFunc             APCRFunc        kInvalidDataType
 *
 * Thread safety:
 *
 * const-qualified member functions on this class are safe for concurrent
 * use by multiple threads, as long as no other thread may be calling any
 * non-const member functions that are not documented as exceptions to this
 * rule.
 */
struct APCHandle {
  struct Pair {
    APCHandle* handle;
    size_t size;
    explicit operator bool() const { return handle != nullptr; }
  };

  explicit APCHandle(APCKind kind, DataType type = kInvalidDataType)
    : m_type(type), m_kind(kind) {
    assertx(checkInvariants());
  }

  APCHandle(const APCHandle&) = delete;
  APCHandle& operator=(APCHandle const&) = delete;

  /*
   * Create an instance of an APC object according to the type of source and
   * the various flags. This is the only entry point to create APC entities.
   */
  static Pair Create(const_variant_ref source, APCHandleLevel level,
                     bool unserializeObj, bool pure);
  static Pair Create(const Variant& var, APCHandleLevel level,
                     bool unserializeObj, bool pure) {
    return Create(const_variant_ref{var}, level, unserializeObj, pure);
  }

  /*
   * Memory management API.
   *
   * APC handles can be managed both by atomic reference counting and via the
   * Treadmill.  Memory management operations on APC handles go through this
   * API to hide which scheme is being used from users of APCHandle.  The
   * active scheme can be different for different handles in the same process.
   * In any case, the correct method for removing references must be used:
   *
   * unreferenceRoot
   *  should only be called once to remove the initial reference added by Create
   * unreferenceNonRoot
   *  should be called to remove each reference added by referenceNonRoot
   *
   * This table shows the implementation of the two modes:
   *
   *            function | Uncounted |  Counted
   *  -------------------+-----------+-----------
   *    referenceNonRoot |   no-op   |  incref
   *  unreferenceNonRoot |   no-op   |  decref
   *     unreferenceRoot | treadmill |  decref
   *
   * The `size' argument to unreferenceRoot is only use for stats collection,
   * so zero may be passed in some situations.
   *
   * unreferenceRoot may be called while this APCHandle is still being read by
   * other threads---it is an exception to the thread-safety rule documented
   * above the class.
   */
  void referenceNonRoot() const;
  void unreferenceNonRoot() const;
  void unreferenceRoot(size_t size = 0);

  /*
   * Get an instance of the PHP object represented by this APCHandle. The
   * instance returned will be local to the request/thread that performed
   * the call.
   */
  TypedValue toLazyProp() const;
  Variant toLocalHelper(bool pure) const;
  Variant toLocal(bool pure) const;
  bool toLocalMayRaise() const;

  /*
   * Return the APCKind represented by this APCHandle.
   */
  APCKind kind() const { return m_kind; }

  /*
   * Return the DataType (if any) of this APCHandle.
   */
  DataType type() const { return m_type; }

  /*
   * When we load serialized objects (in an APCString), we may attempt to
   * convert it to an APCObject representation. If the conversion is not
   * possible (for example, if there are internal references), we set this
   * flag so that we don't try over and over.
   *
   * The non-const setObjAttempted function is safe for concurrent use with
   * multiple readers and writers on a live APCHandle---it is an exception to
   * the thread-safety rule documented above the class.
   */
  bool objAttempted() const {
    assertx(m_kind == APCKind::SerializedObject ||
           m_kind == APCKind::CopiedObject ||
           m_kind == APCKind::CopiedCollection);
    return m_obj_attempted.load(std::memory_order_acquire);
  }
  void setObjAttempted() {
    assertx(m_kind == APCKind::SerializedObject ||
           m_kind == APCKind::CopiedObject ||
           m_kind == APCKind::CopiedCollection);
    m_obj_attempted.store(true, std::memory_order_release);
  }

  /*
   * If true, this APCHandle is an APCTypedValue holding a "shared"
   * array-like or string (not a static or refcounted value).
   */
  bool isShared() const {
    return m_kind == APCKind::SharedArray ||
           m_kind == APCKind::SharedBespoke ||
           m_kind == APCKind::SharedString;
  }

  bool isTypedValue() const {
    return m_type != kInvalidDataType;
  }

  /*
   * If true, this handle and value are allocated on startup and never deleted.
   */
  bool isSingletonKind() const {
    static_assert(APCKind::Uninit <= APCKind::Bool &&
                  APCKind::Null <= APCKind::Bool &&
                  static_cast<int>(APCKind::Bool) == 2,
                  "Uninit, Null and Bool must be the first three APCKinds");
    return m_kind <= APCKind::Bool;
  }

  bool checkInvariants() const;
  bool isAtomicCounted() const;

private:
  void atomicIncRef() const;
  void atomicDecRef() const;
  void deleteCopied();

private:
  const DataType m_type;
  const APCKind m_kind;
  std::atomic<uint8_t> m_obj_attempted{false};
  DEBUG_ONLY std::atomic<uint8_t> m_unref_root_count{0};
  mutable std::atomic<uint32_t> m_count{1};
};

//////////////////////////////////////////////////////////////////////

}
