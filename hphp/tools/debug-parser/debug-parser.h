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

#ifndef incl_HPHP_TOOLS_DEBUG_PARSER_H_
#define incl_HPHP_TOOLS_DEBUG_PARSER_H_

#include <functional>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <folly/DiscriminatedPtr.h>
#include <folly/Overload.h>

#include "hphp/util/optional.h"

#include <tbb/concurrent_hash_map.h>

/*
 * Generic interfaces to query debug information in a platform-agnostic
 * manner. Note that even though debug information (in general) can encode
 * information for a variety of languages, these interfaces assume the source
 * language is C or C++.
 *
 * Right now most of the interfaces concentrate on type information, but more
 * can be added as needed.
 */

namespace debug_parser {

////////////////////////////////////////////////////////////////////////////////

/*
 * Thrown if there's an issue while parsing debug information. Also available
 * for use by applications utilizing the debug information to indicate a higher
 * level issue with the information.
 */
struct Exception: std::runtime_error {
  using std::runtime_error::runtime_error;
};

/*
 * Specific type categories. These are forward declared so they can be used by
 * Type below.
 */
struct VoidType;
struct FuncType;
struct PtrType;
struct RefType;
struct RValueRefType;
struct ArrType;
struct MemberType;
struct ObjectType;
struct ConstType;
struct VolatileType;
struct RestrictType;

/*
 * Type is a union of all the possible specific type categories. Each Type
 * instance stores one (and exactly one) specific type at any given point.
 */
struct Type {
  // Implicit conversion constructors for all the specific types.
  /* implicit */ Type(VoidType);
  /* implicit */ Type(FuncType);
  /* implicit */ Type(PtrType);
  /* implicit */ Type(RefType);
  /* implicit */ Type(RValueRefType);
  /* implicit */ Type(ArrType);
  /* implicit */ Type(MemberType);
  /* implicit */ Type(ObjectType);
  /* implicit */ Type(ConstType);
  /* implicit */ Type(VolatileType);
  /* implicit */ Type(RestrictType);

  // Copy-constructor is deleted to avoid having to do a deep-copy (because
  // types can refer to sub-types), but moving is fine (and efficient).
  Type(const Type&) = delete;
  Type(Type&& t) noexcept { std::swap(m_type, t.m_type); }
  ~Type();

  Type& operator=(const Type&) = delete;
  Type& operator=(Type&& t) {
    std::swap(m_type, t.m_type);
    return *this;
  }

  // Queries to determine which specific type this represents.
  bool isVoid() const { return m_type.hasType<VoidType>(); }
  bool isFunc() const { return m_type.hasType<FuncType>(); }
  bool isPtr() const { return m_type.hasType<PtrType>(); }
  bool isRef() const { return m_type.hasType<RefType>(); }
  bool isRValueRef() const { return m_type.hasType<RValueRefType>(); }
  bool isArr() const { return m_type.hasType<ArrType>(); }
  bool isObject() const { return m_type.hasType<ObjectType>(); }
  bool isMember() const { return m_type.hasType<MemberType>(); }
  bool isConst() const { return m_type.hasType<ConstType>(); }
  bool isVolatile() const { return m_type.hasType<VolatileType>(); }
  bool isRestrict() const { return m_type.hasType<RestrictType>(); }

  // Retrieve the specific type. These all return null if that specific
  // type isn't actually represented.
  VoidType* asVoid() { return m_type.get_nothrow<VoidType>(); }
  FuncType* asFunc() { return m_type.get_nothrow<FuncType>(); }
  PtrType* asPtr() { return m_type.get_nothrow<PtrType>(); }
  RefType* asRef() { return m_type.get_nothrow<RefType>(); }
  RValueRefType* asRValueRef() { return m_type.get_nothrow<RValueRefType>(); }
  ArrType* asArr() { return m_type.get_nothrow<ArrType>(); }
  ObjectType* asObject() { return m_type.get_nothrow<ObjectType>(); }
  MemberType* asMember() { return m_type.get_nothrow<MemberType>(); }
  ConstType* asConst() { return m_type.get_nothrow<ConstType>(); }
  VolatileType* asVolatile() { return m_type.get_nothrow<VolatileType>(); }
  RestrictType* asRestrict() { return m_type.get_nothrow<RestrictType>(); }

  const VoidType* asVoid() const {
    return m_type.get_nothrow<VoidType>();
  }
  const FuncType* asFunc() const {
    return m_type.get_nothrow<FuncType>();
  }
  const PtrType* asPtr() const {
    return m_type.get_nothrow<PtrType>();
  }
  const RefType* asRef() const {
    return m_type.get_nothrow<RefType>();
  }
  const RValueRefType* asRValueRef() const {
    return m_type.get_nothrow<RValueRefType>();
  }
  const ArrType* asArr() const {
    return m_type.get_nothrow<ArrType>();
  }
  const ObjectType* asObject() const {
    return m_type.get_nothrow<ObjectType>();
  }
  const MemberType* asMember() const {
    return m_type.get_nothrow<MemberType>();
  }
  const ConstType* asConst() const {
    return m_type.get_nothrow<ConstType>();
  }
  const VolatileType* asVolatile() const {
    return m_type.get_nothrow<VolatileType>();
  }
  const RestrictType* asRestrict() const {
    return m_type.get_nothrow<RestrictType>();
  }

  /*
   * Callback interface, allowing one to do:
   *
   * type.match<bool>(
   *   [&](const VoidType*) { return false; },
   *   [&](const ObjectType*) { return true; },
   *   ..... etc
   * );
   */
  template<class Ret, class... Funcs>
  Ret match(Funcs&&... funcs) {
    return matchImpl<Ret>(m_type, std::forward<Funcs>(funcs)...);
  }

  template<class Ret, class... Funcs>
  Ret match(Funcs&&... funcs) const {
    return matchImpl<Ret>(m_type, std::forward<Funcs>(funcs)...);
  }

  // Obtain a string representation of the type. This should be used for logging
  // and debugging only. The string representation is not guaranteed to be
  // unique.
  std::string toString() const;

  friend std::ostream& operator<<(std::ostream& os, const Type& t) {
    return os << t.toString();
  }
 private:
  template<class Ret, class Ptr, class... Funcs>
  static Ret matchImpl(Ptr& ptr, Funcs&&... funcs) {
    return ptr.apply([&](auto* p) -> Ret {
      if constexpr (std::is_void_v<Ret>) {
        folly::overload(std::forward<Funcs>(funcs)...)(p);
      } else {
        return folly::overload(std::forward<Funcs>(funcs)...)(p);
      }
    });
  }

  // Since void doesn't actually store anything, we can save some memory by
  // using a single static instance of the void type.
  static VoidType s_void_type;

  folly::DiscriminatedPtr<
    VoidType,
    FuncType,
    PtrType,
    RefType,
    RValueRefType,
    ArrType,
    MemberType,
    ObjectType,
    ConstType,
    VolatileType,
    RestrictType
  > m_type;

public:
  // Similar to match, but takes a visitor object with an operator() overload
  // for each specific type. These functions need to be defined here because the
  // return type refers to m_type (so it needs to be after).
  template <typename V>
  auto apply(V&& v) -> decltype(m_type.apply(std::forward<V>(v))) {
    return m_type.apply(std::forward<V>(v));
  }
  template <typename V>
  auto apply(V&& v) const -> decltype(m_type.apply(std::forward<V>(v))) {
    return m_type.apply(std::forward<V>(v));
  }
};

/*
 * Objects are the set of types in the type system which have names (classes,
 * unions, enums, primitives). All other types are nameless and are identified
 * solely by their structure.
 *
 * However, the names aren't sufficient to determine equality of two different
 * object types. This is because its quite legal in C++ to have two different
 * distinct types with the same name, as long as they have the proper
 * linkage. Therefore for our purposes, an object type's "name" includes its
 * linkage, as that's needed to infer object type equality.
 *
 * Note that the debug parser does not make any attempt to determine which
 * object types are the same. Instead it merely provides the information and
 * lets the consumer make that decision.
 */
struct ObjectTypeName {
  std::string name;

  // Linkage determines within what scope an object type's name is unique.
  enum class Linkage {
    // External linkage is a globally unique name.
    external,
    // Internal linkage is a name unique within a single compile-unit.
    internal,
    // No linkage means the name is never unique and never matches any other
    // object type, even if they have the same name.
    none,
    // Pseudo linkage applies to the so-called "pseudo" object type (see below
    // for further explanation).
    pseudo
  };
  Linkage linkage;
};

/*
 * An object type's id (as opposed to its name) is an unique identifier for a
 * particular appearance of an object type in the debug information.  Multiple
 * object type ids can refer to the same object type, since object types can
 * appear multiple times (usually in different compile-units). It is up to a
 * particular debug parser implementation to decide how to assign these ids, but
 * simply using the offset of the information in the file is easiest.
 */
using ObjectTypeId = std::size_t;

/*
 * A compile unit id is an unique identifier for a particular compilation unit
 * in the debug information. This is needed as an object type name may refer to
 * the same object type depending on whether they're in the same compilation
 * unit or not. Like the object type id, it is up to the debug parser
 * implementation to decide how to assign these ids, but using the offset in the
 * debug information file is easiest.
 */
using CompileUnitId = std::size_t;

/*
 * The combination of an object type id and the compile unit that the type is
 * defined in forms a unique "key", which can be used to unambiguously look up a
 * particular appearance of an object type in the debug information.
 */
struct ObjectTypeKey {
  ObjectTypeId object_id;
  CompileUnitId compile_unit_id;
};

// Specific type categories:

// Empty type.
struct VoidType {};

// Function with a return type and 0 or more arguments.
struct FuncType {
  Type ret;
  std::vector<Type> args;
};

// Pointer (*) to some other type.
struct PtrType {
  Type pointee;
};

// Reference (&) to some other type.
struct RefType {
  Type referenced;
};

// R-value (&&) reference to some other type.
struct RValueRefType {
  Type referenced;
};

// Array of some type with optional bounds. If there's no bound, it
// represents an array of indeterminate size.
struct ArrType {
  Type element;
  HPHP::Optional<std::size_t> count;
};

// An object (a class, union, enum, or primitive), which has a name (see above)
// and an unique key. For efficiency reasons, we don't represent the details of
// the object here. Instead, that's done with the Object class (see below),
// which can be retrieved using the object type key.
struct ObjectType {
  ObjectTypeName name;
  ObjectTypeKey key;
  // Is the object type incomplete in this context? An incomplete object type
  // has no layout information about it.
  bool incomplete;
};

// Member of an object. This type can't appear on its own, but there can't be a
// pointer to one.
struct MemberType {
  ObjectType obj;
  Type member;
};

// Const qualifier on a type.
struct ConstType {
  Type modified;
};

// Volatile qualifier on a type.
struct VolatileType {
  Type modified;
};

// Restrict (from C) qualifier on a type.
struct RestrictType {
  Type modified;
};

inline Type::Type(VoidType): m_type(&s_void_type) {}
inline Type::Type(FuncType t): m_type(new FuncType(std::move(t))) {}
inline Type::Type(PtrType t): m_type(new PtrType(std::move(t))) {}
inline Type::Type(RefType t): m_type(new RefType(std::move(t))) {}
inline Type::Type(RValueRefType t): m_type(new RValueRefType(std::move(t))) {}
inline Type::Type(ArrType t): m_type(new ArrType(std::move(t))) {}
inline Type::Type(MemberType t): m_type(new MemberType(std::move(t))) {}
inline Type::Type(ObjectType t): m_type(new ObjectType(std::move(t))) {}
inline Type::Type(ConstType t): m_type(new ConstType(std::move(t))) {}
inline Type::Type(VolatileType t): m_type(new VolatileType(std::move(t))) {}
inline Type::Type(RestrictType t): m_type(new RestrictType(std::move(t))) {}

/*
 * Detailed object representation. As stated above, for efficiency we do not
 * store everything known about an object inside its ObjectType. Instead the key
 * is provided, which can be used to look up the object type details.
 *
 * A note on the so-called "pseudo" type:
 *
 * For various reasons, including efficiency, the type parser may not find every
 * single type defined in a file's debug information. For example, type
 * definitions inside functions are relatively rare, and also expensive to find
 * (because there are so many functions), so the parser may not make the effort
 * to find these. However, other types may still refer to them (though this is
 * very rare), so they need to be represented somehow without actually parsing
 * them. To fix this, they all get swept into a single type, called the "pseudo"
 * type. It is essentially an empty type with no linkage at all (and a synthetic
 * name). If the type parser can't find an object with the given key, it will
 * assume it refers to the pseudo type and return that.
 */
struct Object {
  // Object's name, including linkage. All the rules about equality of object
  // types involving names and linkage applies.
  ObjectTypeName name;
  // The size of the object in bytes. The size may be zero if the object is
  // incomplete, or in certain other contexts (like flexible array members).
  std::size_t size;

  // The object type's key. This is the same information passed in when the
  // query is made for the object type.
  ObjectTypeKey key;

  // Whether this object type represents a class, union, primitive, enum, or
  // some other catch-all category. This field is mainly to distinguish unions
  // from other categories, as they have different semantics.
  enum class Kind {
    k_class,
    k_union,
    k_primitive,
    k_enum,
    k_other
  };
  Kind kind;

  // Whether this object type represents just a declaration, not a
  // definition. An incomplete type will generally have 0 size and no other
  // useful information.
  bool incomplete;

  // List of object type's base classes.
  struct Base {
    ObjectType type;
    // The offset within this object where this base class is placed. If the
    // offset isn't present, the base class is virtual.
    HPHP::Optional<std::size_t> offset;
  };
  std::vector<Base> bases;

  // Non-function members (both static and non-static).
  struct Member {
    // Member name as it appears in the source.
    std::string name;
    // Offset of the member from the beginning of the object. If no offset is
    // present, the member is static.
    HPHP::Optional<std::size_t> offset;
    // Optional string for static members giving the symbol for this member.
    std::string linkage_name;
    // Optional address for static members giving the address for this
    // member. Whether this address is absolute or relocatable depends on the
    // file the information is being extracted from.
    HPHP::Optional<std::uintptr_t> address;
    Type type;
  };
  std::vector<Member> members;

  // Function members (both static and non-static).
  struct Function {
    enum class Kind {
      // Normal member function
      k_member,
      // Virtual member function
      k_virtual,
      // Static member function
      k_static
    };

    // Name as it appears in the source.
    std::string name;
    Type ret_type;
    std::vector<Type> arg_types;
    Kind kind;
    // Optional string for static functions giving the symbol for this function.
    std::string linkage_name;
    // Optional address for static functions giving the address for this
    // function. Whether this address is absolute or relocatable depends on the
    // file the information is being extracted from.
    HPHP::Optional<std::uintptr_t> address;
  };
  std::vector<Function> functions;

  // Template parameters, if this object type is a template
  // instantiation. Template parameters are represented positionally (since the
  // debug information does not record their names reliably).
  struct TemplateParam {
    Type type;
  };
  std::vector<TemplateParam> template_params;
};

/*
 * TypeParser is the class responsible for parsing debug information out of a
 * file and extracting type information from it.
 */
struct TypeParser {
  // Virtual destructor since this is an abstract base class.
  virtual ~TypeParser() = default;

  // Factory function to obtain a pointer to the actual platform implementation
  // of the parser. Run the parser on the given filename, which may be an
  // executable or some form of object file. If the platform doesn't have a
  // supported debug info parser, this function will return null.
  // The number of threads controls parallelism when building up state if the
  // implementation supports it. More isn't necessarily better, and the dwarf
  // implementation will allocate memory proportional to the size of the input
  // binary and the number of threads. If the binary is very large, it may
  // exhaust memory resources in some systems.
  static std::unique_ptr<TypeParser> make(const std::string& filename,
                                          int num_threads);

  // Iterate over the list of all object types defined in the file. This is safe
  // to call from multiple threads concurrently.
  template <typename F> void forEachObject(F f) const {
    auto const count = getObjectBlockCount();
    for (auto i = size_t{0}; i < count; ++i) {
      for (auto const& obj : getObjectBlock(i)) f(obj);
    }
  }

  // A parser implementation is allowed to store its list of object types in
  // some number of disjoint "blocks" (the number may be 1). This returns the
  // number of such blocks. This is safe to call from multiple thread
  // concurrently.
  virtual size_t getObjectBlockCount() const = 0;

  // Iterate over the list of object types for one particular block. This is
  // useful if you wish to consume the object list concurrently. This is safe to
  // call from multiple threads concurrently.
  template <typename F> void forEachObjectInBlock(size_t index, F f) const {
    for (auto const& obj : getObjectBlock(index)) f(obj);
  }

  // Given a particular object key, return the object description corresponding
  // to it. This is not safe to call concurrently and is not thread safe.
  virtual Object getObject(ObjectTypeKey key) = 0;
 protected:
  virtual const std::vector<ObjectType>& getObjectBlock(size_t index) const = 0;
};

/*
 * Print raw debug information in some implementation defined format. Mainly for
 * debugging.
 */
struct Printer {
  // Virtual destructor since this is an abstract base class.
  virtual ~Printer() = default;

  // Factory function to obtain a pointer to the actual platform implementation
  // of the printer. Run the printer on the given filename, which may be an
  // executable or some form of object file. If the platform doesn't have a
  // supported debug info parser, this function will return null.
  static std::unique_ptr<Printer> make(const std::string& filename);

  // Perform the actual printing, outputting to the specified ostream. Only
  // records between begin and end will be output.
  virtual void operator()(
    std::ostream& os,
    std::size_t begin = 0,
    std::size_t end = std::numeric_limits<std::size_t>::max(),
    bool dwp = false
  ) const = 0;
};

const char* show(ObjectTypeName::Linkage);

////////////////////////////////////////////////////////////////////////////////

}

#endif
