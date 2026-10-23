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

#include <fmt/format.h>
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/type-specialization.h"
#include "hphp/util/bitset.h"

#include <cstdint>
#include <type_traits>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct ArrayData;
struct Class;
struct Func;
struct StringData;
struct TypeConstraint;
struct TypedValue;

namespace jit {
///////////////////////////////////////////////////////////////////////////////

struct GuardConstraint;
struct ProfDataSerializer;
struct ProfDataDeserializer;
struct SBProfType;

/*
 * The PtrLocation enum is a lattice that represents what kind of
 * locations a pointer/lval Type may point to.
 *
 * We have a kind for each of the major segregated locations in which
 * php values can live (eval stack, frame slots, properties, etc...).  These
 * classify MemTo* types into some categories that cannot possibly alias,
 * without any smarter analysis needed to prove it.  There is also a union for
 * the various locations things can point after a fully generic member
 * operation (see Memb below).
 *
 * Memb is a number of different locations that result from the more generic
 * types of member operations: Prop, Elem, MIS, MMisc, and Other. MMisc
 * contains something living in a collection instance or object's dynamic
 * property array. Other contains init_null_variant, uninit_variant, or the
 * lvalBlackHole.
 *
 * ClsInit is a pointer to class property initializer data.
 *
 * ClsCns is a pointer to class constant values in RDS.
 *
 * The hierarchy looks something like this:
 *
 *                            All
 *                             |
 *         +-------------------+----+--------+-------+
 *         |                        |        |       |
 *        Memb                      |     ClsInit  ClsCns
 *         |                        |
 *         |                        +--------+----- ... etc
 *         |                        |        |
 *         |                      Frame     Stk
 *      +--+-+------+----+
 *      |    |      |    |
 *     MIS  Prop   Elem Field
 *
 * Note: if you add a new pointer type, you very likely need to update
 * pointee() in memory-effects.cpp for it to remain correct.
 *
 */

#define PTR_LOCATION_PRIMITIVE(f,...)                       \
  f(ClsInit,  1U << 0, __VA_ARGS__)                         \
  f(Frame,    1U << 1, __VA_ARGS__)                         \
  f(Stk,      1U << 2, __VA_ARGS__)                         \
  f(Gbl,      1U << 3, __VA_ARGS__)                         \
  f(Prop,     1U << 4, __VA_ARGS__)                         \
  f(Elem,     1U << 5, __VA_ARGS__)                         \
  f(SProp,    1U << 6, __VA_ARGS__)                         \
  f(MISTemp,  1U << 7, __VA_ARGS__)                         \
  f(Other,    1U << 8, __VA_ARGS__)                         \
  f(Const,    1U << 9, __VA_ARGS__)                         \
  // Keep the last bit in sync with PtrLocation::All below

#define PTR_LOCATION_TYPES(f, ...)                          \
  PTR_LOCATION_PRIMITIVE(f, __VA_ARGS__)                    \
  f(ElemOrConst, Elem | Const, __VA_ARGS__)

enum class PtrLocation : uint16_t {
  Bottom = 0,           // Nothing. Only valid if type is not a Ptr
  All    = (1U << 10) - 1,
#define PTRT(name, bits, ...) name = (bits),
  PTR_LOCATION_TYPES(PTRT)
#undef PTRT
};

using ptr_location_t = std::underlying_type<PtrLocation>::type;

constexpr PtrLocation operator~(PtrLocation p) {
  return static_cast<PtrLocation>(~static_cast<ptr_location_t>(p));
}
constexpr PtrLocation operator|(PtrLocation a, PtrLocation b) {
  return static_cast<PtrLocation>(
    static_cast<ptr_location_t>(a) | static_cast<ptr_location_t>(b)
  );
}
inline PtrLocation& operator|=(PtrLocation& a, PtrLocation b) {
  return a = a | b;
}
constexpr PtrLocation operator&(PtrLocation a, PtrLocation b) {
  return static_cast<PtrLocation>(
    static_cast<ptr_location_t>(a) & static_cast<ptr_location_t>(b)
  );
}
inline PtrLocation& operator&=(PtrLocation& a, PtrLocation b) {
  return a = a & b;
}
constexpr PtrLocation operator-(PtrLocation a, PtrLocation b) {
  return static_cast<PtrLocation>(
    static_cast<ptr_location_t>(a) & ~static_cast<ptr_location_t>(b)
  );
}

constexpr bool operator<=(PtrLocation a, PtrLocation b) {
  return (a & b) == a;
}
constexpr bool operator>=(PtrLocation a, PtrLocation b) {
  return b <= a;
}
constexpr bool operator<(PtrLocation a, PtrLocation b) {
  return a <= b && a != b;
}
constexpr bool operator>(PtrLocation a, PtrLocation b) {
  return a >= b && a != b;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * This section defines a number of macros used to stamp out code and data for
 * all predefined types (types not represented here can be constructed by
 * combining different types). It can be viewed as one big X macro, with
 * different values of X:
 *
 * IRT(name, bits): A plain type, like Bool or Obj.
 * IRTP(name, ptr): A Ptr type
 * IRTL(name, ptr): An Lval type
 * IRTM(name, ptr): A Mem type
 * IRTX(name, ptr, bits): A type with both bits and ptr location
 */

#define IRTP_FROM_PTR(ptr, ...)                               \
  IRTP(PtrTo##ptr, ptr)

#define IRTL_FROM_PTR(ptr, ...)                               \
  IRTL(LvalTo##ptr, ptr)

#define IRTM_FROM_PTR(ptr, ...)                               \
  IRTM(MemTo##ptr, ptr)

#define IRT_PTRS_LVALS                                        \
  PTR_LOCATION_TYPES(IRTP_FROM_PTR)                           \
  PTR_LOCATION_TYPES(IRTL_FROM_PTR)                           \
  PTR_LOCATION_TYPES(IRTM_FROM_PTR)

/**/

#define IRT_PHP                                                         \
  IRT(Uninit,          bits_t::bit<0>())                                \
  IRT(InitNull,        bits_t::bit<1>())                                \
  IRT(Bool,            bits_t::bit<2>())                                \
  IRT(Int,             bits_t::bit<3>())                                \
  IRT(Dbl,             bits_t::bit<4>())                                \
  IRT(StaticStr,       bits_t::bit<5>())                                \
  IRT(SharedStr,       bits_t::bit<6>())                                \
  IRT(CountedStr,      bits_t::bit<7>())                                \
  IRT(StaticVec,       bits_t::bit<8>())                                \
  IRT(SharedVec,       bits_t::bit<9>())                                \
  IRT(CountedVec,      bits_t::bit<10>())                               \
  IRT(StaticDict,      bits_t::bit<11>())                               \
  IRT(SharedDict,      bits_t::bit<12>())                               \
  IRT(CountedDict,     bits_t::bit<13>())                               \
  IRT(StaticKeyset,    bits_t::bit<14>())                               \
  IRT(SharedKeyset,    bits_t::bit<15>())                               \
  IRT(CountedKeyset,   bits_t::bit<16>())                               \
  IRT(Obj,             bits_t::bit<17>())                               \
  IRT(Res,             bits_t::bit<18>())                               \
  IRT(Func,            bits_t::bit<19>())                               \
  IRT(Cls,             bits_t::bit<20>())                               \
  IRT(ClsMeth,         bits_t::bit<21>())                               \
  IRT(RFunc,           bits_t::bit<22>())                               \
  IRT(RClsMeth,        bits_t::bit<23>())                               \
  IRT(LazyCls,         bits_t::bit<24>())                               \
  IRT(EnumClassLabel,  bits_t::bit<25>())                               \
/**/

#define PRIMITIVE_INIT_UNION \
  (kInitNull|kBool|kInt|kDbl|kFunc|kCls|kLazyCls|kClsMeth|kEnumClassLabel)

#define INIT_CELL_UNION \
  (kUncountedInit|kStr|kArrLike|kObj|kRes|kRFunc|kRClsMeth)

/*
 * This list should be in non-increasing order of specificity.
 */
#define IRT_PHP_UNIONS                                                  \
  IRT(Null,                kUninit|kInitNull)                           \
  IRT(FuncLike,            kFunc|kRFunc)                                \
  IRT(ClsMethLike,         kClsMeth|kRClsMeth)                          \
  IRT(PrimitiveInit,       PRIMITIVE_INIT_UNION)                        \
  IRT(Primitive,           kUninit|kPrimitiveInit)                      \
  IRT(PersistentStr,       kStaticStr|kSharedStr)                       \
  IRT(Str,                 kPersistentStr|kCountedStr)                  \
  IRT(PersistentVec,       kStaticVec|kSharedVec)                       \
  IRT(Vec,                 kPersistentVec|kCountedVec)                  \
  IRT(PersistentDict,      kStaticDict|kSharedDict)                     \
  IRT(Dict,                kPersistentDict|kCountedDict)                \
  IRT(PersistentKeyset,    kStaticKeyset|kSharedKeyset)                 \
  IRT(Keyset,              kPersistentKeyset|kCountedKeyset)            \
  IRT(StaticArrLike,       kStaticVec|kStaticDict|kStaticKeyset)        \
  IRT(PersistentArrLike,   kPersistentVec|kPersistentDict|kPersistentKeyset) \
  IRT(ArrLike,             kVec|kDict|kKeyset)                          \
  IRT(NullableObj,         kObj|kInitNull|kUninit)                      \
  IRT(Static,              kStaticStr|kStaticArrLike)                   \
  IRT(Persistent,          kPersistentStr|kPersistentArrLike)           \
  IRT(UncountedInit,       kPrimitiveInit|kPersistent)                  \
  IRT(Uncounted,           kUninit|kUncountedInit)                      \
  IRT(NonNull,             INIT_CELL_UNION & ~kNull)                    \
  IRT(InitCell,            INIT_CELL_UNION)                             \
  IRT(Cell,                kUninit|kInitCell)

/*
 * Adding a new runtime type needs updating numRuntime variable.
 */
#define IRT_RUNTIME                                                     \
  IRT(NamedType,   bits_t::bit<kRuntime>())                             \
  IRT(NamedFunc,   bits_t::bit<kRuntime+1>())                           \
  IRT(RetAddr,     bits_t::bit<kRuntime+2>()) /* Return address */      \
  IRT(StkPtr,      bits_t::bit<kRuntime+3>()) /* Stack pointer */       \
  IRT(FramePtr,    bits_t::bit<kRuntime+4>()) /* Frame pointer */       \
  IRT(TCA,         bits_t::bit<kRuntime+5>())                           \
  IRT(ABC,         bits_t::bit<kRuntime+6>()) /* AsioBlockableChain */  \
  IRT(RDSHandle,   bits_t::bit<kRuntime+7>()) /* rds::Handle */         \
  IRT(Nullptr,     bits_t::bit<kRuntime+8>())                           \
  IRT(Smashable,   bits_t::bit<kRuntime+9>()) /* Smashable uint64_t */  \
  IRT(VoidPtr,     bits_t::bit<kRuntime+10>()) /* Arbitrary pointer */  \
  /* bits above this are unused */

/*
 * Cell, Counted, Init, PtrToCell, etc...
 */
#define COUNTED_INIT_UNION \
  kCountedStr|kCountedVec|kCountedDict|kCountedKeyset|kObj|kRes| \
  kRFunc|kRClsMeth

#define IRT_SPECIAL                                           \
  /* Bottom and Top use IRTX to specify a custom PtrLocation kind */  \
  IRTX(Bottom,         Bottom, kBottom)                       \
  IRTX(Top,            All,    kTop)                          \
  IRTX(Ptr,            All,    kPtr)                          \
  IRTX(Lval,           All,    kLval)                         \
  IRTX(Mem,            All,    kMem)                          \
  IRT(Counted,                 COUNTED_INIT_UNION)            \
/**/

/*
 * All types that represent a non-union type.
 */
#define IRT_PRIMITIVE IRT_PHP IRT_PTRS_LVALS IRT_RUNTIME

/*
 * All types.
 */
#define IR_TYPES                  \
  IRT_PHP                         \
  IRT_PHP_UNIONS                  \
  IRT_PTRS_LVALS                  \
  IRT_RUNTIME                     \
  IRT_SPECIAL

///////////////////////////////////////////////////////////////////////////////

/*
 * Type is used to represent the types of values in the jit.  Every Type
 * represents a set of types, with TTop being a superset of all Types and
 * TBottom being a subset of all Types.  The elements forming these sets of
 * types come from the types of PHP-visible values (Str, Obj, Int, ...) and
 * runtime-internal types (Func, TCA, ...).
 *
 * Types can be constructed from the predefined constants or by composing
 * existing Types in various ways.  Unions, intersections, and subtractions are
 * all supported, though for implementation-specific reasons certain
 * combinations of specialized types cannot be represented.  A type is
 * considered specialized if it refers to a specific Class or a specific
 * non-Top ArrayLayout. As an example, if A and B are unrelated Classes,
 * Obj<A> | Obj<B> is impossible to represent.  However, if B is a subclass of
 * A, Obj<A> | Obj<B> == Obj<B>, which can be represented as a Type.
 */
struct Type {
private:
  static constexpr size_t kRuntime = 27;
  static constexpr size_t kNumRuntime = 10;
  static constexpr size_t kNumPtr = 2;

public:
  using bits_t = BitSet<kRuntime + kNumRuntime + kNumPtr>;
  static constexpr bits_t kBottom{};
  static constexpr bits_t kTop = ~kBottom;

#define IRT(name, bits)       static constexpr bits_t k##name = (bits);
#define IRTP(name, ...)
#define IRTL(name, ...)
#define IRTM(name, ...)
#define IRTX(name, ...)
    IR_TYPES
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX

  static constexpr bits_t kPtr = bits_t::bit<kRuntime+kNumRuntime>();
  static constexpr bits_t kLval = bits_t::bit<kRuntime+kNumRuntime+1>();
  static constexpr bits_t kMem = kPtr | kLval;

  static constexpr bits_t kArrSpecBits  = kArrLike;
  static constexpr bits_t kClsSpecBits  = kObj | kCls;

  /////////////////////////////////////////////////////////////////////////////
  // Basic methods.

public:
  /*
   * Default bottom constructor.
   */
  Type();

  /*
   * Construct from a predefined set of bits and pointer location
   */
  constexpr Type(bits_t bits, PtrLocation ptr);

  /*
   * Hash the Type as a bitfield.
   * stableHash() will return a consistent value across HHVM restarts.
   */
  size_t hash() const;
  size_t stableHash() const;

  /*
   * Stringify the Type.
   *
   * constValString: @requires: hasConstVal() ||
   *                            subtypeOfAny(Uninit, InitNull, Nullptr)
   */
  std::string toString() const;
  std::string constValString() const;
  static std::string debugString(Type t);

  /*
   * Serialization/deserialization.
   */
  void serialize(ProfDataSerializer&) const;
  static Type deserialize(ProfDataDeserializer&);

  bits_t rawBits() const;

  /////////////////////////////////////////////////////////////////////////////
  // DataType.

  /*
   * Construct from a DataType.
   */
  explicit Type(DataType outer);

  /*
   * Return true iff there exists a DataType that represents a non-strict
   * supertype of this type.
   *
   * @requires: *this <= Cell
   */
  bool isKnownDataType() const;

  /*
   * Return the most specific DataType that is a supertype of this Type.
   *
   * @requires: isKnownDataType()
   */
  DataType toDataType() const;


  /////////////////////////////////////////////////////////////////////////////
  // Comparisons.                                                       [const]

  /*
   * Return true if this is exactly equal to `rhs'.
   *
   * Be careful---you probably mean `<='.
   */
  bool operator==(Type rhs) const;
  bool operator!=(Type rhs) const;

  /*
   * Does this represent a subset (or superset) of `t2'?
   *
   * All operators are implemented in terms of operator==() and operator<=().
   */
  bool operator<=(Type rhs) const;
  bool operator>=(Type rhs) const;
  bool operator<(Type rhs) const;
  bool operator>(Type rhs) const;

  /*
   * Is this a non-strict subtype of any among a variadic list of Types?
   */
  template<typename... Types>
  bool subtypeOfAny(Type t2, Types... ts) const;
  bool subtypeOfAny() const;

  /*
   * Is this a non-strict subtype of `t2' and is not Bottom?
   */
  bool nonTrivialSubtypeOf(Type t2) const;

  /*
   * Return true if this has nontrivial intersection with `t2'.
   */
  bool maybe(Type t2) const;


  /////////////////////////////////////////////////////////////////////////////
  // Combinators.

  /*
   * Set operations: union, intersection, and difference.
   *
   * These operations may all return larger types than the "true" union,
   * intersection, or difference.  (They must be conservative in that direction
   * for types that are too hard for us to represent, or we could generate
   * incorrect code by assuming certain possible values are impossible.)
   *
   * Note: operator| and operator& guarantee commutativity; operator- guarantees
   * (a - b) <= a.
   */
  Type operator|(Type other) const;
  Type& operator|=(Type other) { return *this = *this | other; }

  Type operator&(Type other) const;
  Type& operator&=(Type other) { return *this = *this & other; }

  Type operator-(Type other) const;
  Type& operator-=(Type other) { return *this = *this - other; }

  template<typename... Types>
  static Type unionAll(Type t, Types... ts);
  static Type unionAll();


  /////////////////////////////////////////////////////////////////////////////
  // Is-a methods.                                                      [const]

  /*
   * Is this a union type?
   *
   * Note that this is the plain old set definition of union, so TStr,
   * TVec, and TNull will all return true.
   */
  bool isUnion() const;

  /*
   * Does this require a register to hold a DataType at runtime?
   */
  bool needsReg() const;

  /*
   * Return true if this corresponds to a type that is passed by (value/
   * reference) in C++.
   */
  bool isSimpleType() const;
  bool isSingularReferenceType() const;


  /////////////////////////////////////////////////////////////////////////////
  // Constant type creation.                                     [const/static]

  /*
   * Return a const copy of `ret' with constant value `val'.
   */
  template<typename T>
  static Type cns(T val, Type ret);

  /*
   * Return a const type corresponding to `val'.
   *
   * @returns: cns(val, forConst(val))
   */
  template<typename T>
  static Type cns(T val);

  /*
   * @returns: TNullptr
   */
  static Type cns(std::nullptr_t);

  /*
   * Return a const type for `tv'. `cns' will assert if given a type
   * which isn't allowed to have constants, while `tryCns' will return
   * std::nullopt.
   */
  static Type cns(TypedValue tv);
  static Optional<Type> tryCns(TypedValue tv);

  /*
   * If this represents a constant value, return the most specific strict
   * supertype of this we can represent, else return *this.
   *
   * In most cases this just erases the constant value:
   *    Int<4> -> Int
   *    Dbl<2.5> -> Dbl
   *
   * Arrays are special since they can be both constant and specialized,
   * so dropping an array's constant value preserves layout information.
   */
  Type dropConstVal() const;


  /////////////////////////////////////////////////////////////////////////////
  // Constant introspection.                                            [const]

  /*
   * Does this Type have a constant value?  If true, we can call xxVal().
   *
   * NOTE: Bottom is a type with no value, and Uninit/InitNull/Nullptr are
   * considered types with a single unique value, so this function returns false
   * for those types.  You may want to explicitly check for them as needed.
   */
  bool hasConstVal() const;

  /*
   * @returns: hasConstVal() && *this <= t
   */
  bool hasConstVal(Type t) const;

  /*
   * Does this Type represent the constant val `val'?
   *
   * @returns: hasConstVal(cns(val))
   */
  template<typename T>
  bool hasConstVal(T val) const;

  /*
   * Whether this Type represents a single possible value.
   *
   * @returns: hasConstVal() || subtypeOfAny(TNullptr, TInitNull, TUninit)
   */
  bool admitsSingleVal() const;

  /*
   * Return the const value for a const Type as a uint64_t.
   *
   * @requires: hasConstVal()
   */
  uint64_t rawVal() const;

  /*
   * Returns a TypedValue if admitsSingleVal() is true and if this type is a
   * subtype of TCell. Otherwise, returns std::nullopt.
   */
  Optional<TypedValue> tv() const;

  /*
   * Return the const value for a const Type.
   *
   * @requires: hasConstVal(Type::T)
   */
  bool boolVal() const;
  int64_t intVal() const;
  double dblVal() const;
  const StringData* strVal() const;
  const StringData* eclVal() const;
  const ArrayData* arrVal() const;
  const ArrayData* vecVal() const;
  const ArrayData* dictVal() const;
  const ArrayData* shapeVal() const;
  const ArrayData* keysetVal() const;
  const ArrayData* arrLikeVal() const;
  const HPHP::Func* funcVal() const;
  const Class* clsVal() const;
  LazyClassData lclsVal() const;
  ClsMethDataRef clsmethVal() const;
  rds::Handle rdsHandleVal() const;
  jit::TCA tcaVal() const;
  void* voidPtrVal() const;
  const TypedValue* ptrVal() const;


  /////////////////////////////////////////////////////////////////////////////
  // Specialized type creation.                                  [const/static]

  /*
   * Return a specialized TVec/TDict/TKeyset.
   */
  static Type Vec(const RepoAuthType::Array*);
  static Type Dict(const RepoAuthType::Array*);
  static Type Keyset(const RepoAuthType::Array*);

  /*
   * Return a specialized TStaticVec/TStaticDict/TStaticKeyset.
   */
  static Type StaticVec(const RepoAuthType::Array*);
  static Type StaticDict(const RepoAuthType::Array*);
  static Type StaticKeyset(const RepoAuthType::Array*);

  /*
   * Return a specialized TCountedVec/TCountedDict/TCountedKeyset.
   */
  static Type CountedVec(const RepoAuthType::Array*);
  static Type CountedDict(const RepoAuthType::Array*);
  static Type CountedKeyset(const RepoAuthType::Array*);

  /*
   * Return a specialized TObj.
   */
  static Type SubObj(const Class* cls);
  static Type ExactObj(const Class* cls);

  /*
   * Return a specialized TCls.
   */
  static Type ExactCls(const Class* cls);
  static Type SubCls(const Class* cls);

  /*
   * Return a copy of this type with a vanilla ArraySpec. Examples:
   *   TArrLike.narrowToVanilla()    == TVanillaArrLike
   *   (TVec|TInt).narrowToVanilla() == TVanillaVec|TInt
   *   (TVec|TObj).narrowToVanilla() == TVec|TObj
   */
  Type narrowToVanilla() const;

  /*
   * Return a copy of this type with an ArraySpec for the given layout
   *   TVec.narrowToLayout(<some layout>) == TVec=<some layout>
   *   (TVec|TInt).narrowToLayout(<some layout>) == TVec=<some layout>|TInt
   */
  Type narrowToLayout(ArrayLayout) const;

  /*
   * Return a copy of this Type with the specialization dropped.
   *
   * @returns: Type(m_bits)
   */
  Type unspecialize() const;

  /*
   * Return a copy of this Type with the specialization and staticness
   * dropped.
   *
   * @requires *this <= TInitCell
   */
  Type modified() const;

  /*
   * Intersect with refinement.
   *
   * For intersections that cannot be performed monotonically (e.g. RATArrays),
   * select the refinement version of the specialization.
   */
  Type refine(Type refinement) const;

  /////////////////////////////////////////////////////////////////////////////
  // Specialization introspection.                                      [const]

  /*
   * Does this Type have a specialization?
   */
  bool isSpecialized() const;

  /*
   * Whether this type can meaningfully specialize along `kind'.
   *
   * For example, a Type only supports SpecKind::Class if its bits intersect
   * nontrivially with kObj.
   */
  bool supports(SpecKind kind) const;

  /*
   * Return the corresponding type specialization.
   *
   * If the Type is able to support the specialization (i.e., supports()
   * returns true for the corresponding SpecKind), but no specialization is
   * present, Spec::Top will be returned.
   *
   * If supports() would return false for the corresponding SpecKind,
   * Spec::Bottom is returned.
   *
   * The Spec objects cast (explicitly) to true iff they are neither Spec::Top
   * nor Spec::Bottom, so these functions also answer the question, "Is this
   * Type nontrivially specialized along the respective kind?"
   */
  ArraySpec arrSpec() const;
  ClassSpec clsSpec() const;

  /*
   * Return a discriminated TypeSpec for this Type's specialization.
   */
  TypeSpec spec() const;


  /////////////////////////////////////////////////////////////////////////////
  // Inner types.                                                       [const]

  /*
   * Return the pointer location aspect of a Type.
   */
  PtrLocation ptrLocation() const;

  /*
   * Convert the TPtr branch of the Type (if any) into its equivalent
   * TLval form, or vice-versa.
   */
  Type ptrToLval() const;
  Type lvalToPtr() const;

  /////////////////////////////////////////////////////////////////////////////
  // Internal methods.

private:
  /*
   * Internal constructors.
   */
  Type(bits_t bits, PtrLocation ptr, bool hasConstVal, uintptr_t extra);
  Type(Type t, ArraySpec arraySpec);
  Type(Type t, ClassSpec classSpec);

  /*
   * Bit-pack a DataType
   */
  static bits_t bitsFromDataType(DataType outer);

  /*
   * Check invariants and return false if the type is malformed.
   */
  bool checkValid() const;

  /*
   * Return a version of *this with the given specialization. If TypeSpec
   * contains both kinds of specialization or *this supports both types of
   * specialization, the resulting type will not be specialized at all. Any
   * constant value in *this will be dropped if the specialization is
   * applied. *this must support any specializations present in the given
   * TypeSpec.
   */
  Type specialize(TypeSpec) const;

  /*
   * Do the given bits support a specific kind of specialization?
   */
  static bool supports(bits_t, SpecKind kind);

  /////////////////////////////////////////////////////////////////////////////
  // Data members.

private:
  bits_t m_bits;
  PtrLocation m_ptr;
  bool m_hasConstVal;

  union {
    uintptr_t m_extra;

    // Constant values.  Validity determined by m_hasConstVal and m_bits.
    bool m_boolVal;
    int64_t m_intVal;
    double m_dblVal;
    const StringData* m_strVal;
    const ArrayData* m_arrVal;
    const ArrayData* m_vecVal;
    const ArrayData* m_dictVal;
    const ArrayData* m_keysetVal;
    const HPHP::Func* m_funcVal;
    const Class* m_clsVal;
    ClsMethDataRef m_clsmethVal;
    LazyClassData m_lclsVal;
    jit::TCA m_tcaVal;
    void* m_voidPtrVal;
    rds::Handle m_rdsHandleVal;
    TypedValue* m_ptrVal;

    // Specializations for object classes and arrays.
    ClassSpec m_clsSpec;
    ArraySpec m_arrSpec;
  };

  friend Type typeFromSBProfType(const SBProfType&);
};

using OptType = Optional<Type>;

/////////////////////////////////////////////////////////////////////////////

/*
 * Return the most refined Type that can be used to represent the type of a
 * live TypedValue or a RepoAuthType.
 *
 * For these methods, ctx may be null in general. propCls may be null during
 * Class initialization, but only if the type-hints involved are not "this",
 * "self", or other hints that use propCls. We always check this constraint.
 */
Type typeFromTV(tv_rval tv, const Class* ctx);
Type typeFromRAT(RepoAuthType ty, const Class* ctx);
Type typeFromPropTC(const HPHP::TypeIntersectionConstraint& tc,
                    const Class* propCls,
                    const Class* ctx,
                    bool isSProp,
                    bool systemLibOnly = false);
Type typeFromFuncParam(const Func* func, uint32_t paramId);
Type typeFromFuncReturn(const Func* func, bool pessimizeForBuiltin = false);
Type typeFromFuncOut(const Func* func, uint32_t inOutIdx);
Type typeFromSBProfType(const SBProfType&);

///////////////////////////////////////////////////////////////////////////////

/*
 * Returns the type that a value may have if it had type `srcType' and failed a
 * CheckType with `typeParam'.  Not all typeParams for CheckTypes are precise,
 * so the return value may even be `srcType' itself in some situations.
 */
Type negativeCheckType(Type typeParam, Type srcType);

/*
 * Returns the least specific supertype of `t' that maintains the properties
 * required by `cat' or by `gc'. The latter should be used where possible;
 * we have constraints when forming tracelet regions, but we only keep around
 * DataTypeCategory when we save them to the profile data.
 */
Type relaxType(Type t, DataTypeCategory cat);
Type relaxToConstraint(Type t, const GuardConstraint& gc);

/*
 * Returns the smallest supertype of ty that we can reasonably guard on. Used
 * for checking inner ref cells and locals in pseudomains.
 */
Type relaxToGuardable(Type ty);

///////////////////////////////////////////////////////////////////////////////

}}

///////////////////////////////////////////////////////////////////////////////

namespace std {
  template<> struct hash<HPHP::jit::Type> {
    size_t operator()(HPHP::jit::Type t) const { return t.hash(); }
  };
}

///////////////////////////////////////////////////////////////////////////////

template <>
struct fmt::formatter<HPHP::jit::Type> : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::Type& v, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(v.toString(), ctx);
  }
};

///////////////////////////////////////////////////////////////////////////////

#define incl_HPHP_JIT_TYPE_INL_H_
#include "hphp/runtime/vm/jit/type-inl.h"
#undef incl_HPHP_JIT_TYPE_INL_H_
