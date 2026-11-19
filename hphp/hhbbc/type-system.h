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

#include <cstdint>
#include <vector>
#include <utility>

#include "hphp/util/copy-ptr.h"
#include "hphp/util/ptr.h"

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/string-data.h"

#include "hphp/runtime/vm/type-constraint.h"

#include "hphp/hhbbc/type-system-bits.h"

#include "hphp/hhbbc/array-like-map.h"
#include "hphp/hhbbc/misc.h"

//////////////////////////////////////////////////////////////////////

/*
 * Type system.
 *
 *   Types are represented by a "trep", which is a bitset, and an
 *   optional specialization. The trep specifies what combination of
 *   basic types the Type represents. The specialization (if present)
 *   gives more information about some subset of the Type. For any
 *   given Type, there's a B* variant which is just the trep, and a T*
 *   variant which can also contain the specialization.
 *
 *   For each "base" type, there's a single bit in the trep. In
 *   addition to the base types, there's many predefined unions of
 *   bits. See type-system-bits.h for the full list.
 *
 *
 * String type:
 *
 *   The String type is divided along one dimension: counted or
 *   uncounted. CStr and SStr represent the two possibilities, with
 *   Str representing the union of both.
 *
 * Array types:
 *
 *   Array types are divided along two dimensions: counted or
 *   uncounted, and empty or non-empty.  Unions of either are allowed.
 *   The naming convention is {S,C,}Arr{N,E,} (where Arr can be any
 *   array type, for example Vec, Dict, or Keyset), where leaving out
 *   either bit means it's unknown along that dimension. An example
 *   lattice looks like this:
 *
 *                         Arr
 *                          |
 *                  +----+--+--+---+
 *                  |    |     |   |
 *                  |  SArr   CArr |
 *                  |   |      |   |
 *              +-------+---------------+
 *              |   |          |   |    |
 *              |  ArrN  +-----+  ArrE  |
 *              | /   \  |     | /   \  |
 *             SArrN  CArrN   CArrE  SArrE
 *
 *
 * Specializations:
 *
 *   Specializations represent further information about some subset
 *   of the type (usually constant values). Each possible
 *   specialization is "supported" by some trep. That is, the
 *   specialization is only allowed if that trep is present. In
 *   addition (right now), only one specialization is allowed in a
 *   Type, and a specialization is only allowed to be present if
 *   exactly one "supportful" trep is present. For example, both Str
 *   and Int support specializations. So a type is not allowed to have
 *   any specialization if both Str and Int are present in a type (but
 *   its fine if only one of them is). The specializations are:
 *
 *     Int=n
 *
 *       Integer with known constant value. Supported by BInt.
 *
 *     Dbl=n
 *
 *       Double with known constant value. Supported by BDbl.
 *
 *     {S,C}Str=s
 *
 *       String with known constant value. Supported by any of B{C,S}Str.
 *
 *     Obj{<}=c
 *     Cls{<}=c
 *
 *       Object or Class with a known class type. The
 *       class type can either be exact or a
 *       subclass. Supported by BObj or BCls.
 *
 *     Obj=WaitH<T>
 *
 *       Object is a wait handle known to produce T when awaited (or
 *       possibly throw). T must be a subtype of TInitCell. Supported
 *       by BObj.
 *
 *     Arr~(static array)
 *
 *       Array is a known static array.
 *
 *     Arr(T1, T2, .... TN)
 *
 *       Array is known to be a packed array containing precisely N
 *       values of type T1,T2...TN.
 *
 *     Arr([T])
 *
 *       Array is known to be a packed array of indeterminate
 *       length. All values have type T.
 *
 *     Arr([T1:T2])
 *
 *       Array is known to be an array with keys of type T1, and
 *       values of type T2. This potentially includes packed arrays.
 *
 *     Arr(Str=s1:T1, Str=s2:T2, .... Str=sn:TN, [TK:TV])
 *
 *       Array is known to be a (non-packed) array with known keys s1
 *       to sn (the keys can also be integers). Each known key has a
 *       known value type T1...TN. In addition, there's TK is the
 *       union of all possible non-statically known keys, and TV is
 *       the union of all possible values for those keys. TK and TV
 *       can be Bottom if there are none. TK/TV is disjoint from the
 *       known keys.
 *
 *   Array specializations are all supported by BArrLikeN (not
 *   BArrLike). This means the specialization only applies to the
 *   non-empty portion of the union.
 *
 *   NOTE: Having the S* types be a sibling of the C* types is
 *   problematic. There is an assumption that types in the index only
 *   ever get more refined, but eg we might "know" that a type is C*
 *   early on (because eg we stored a value to it, thus modifying it),
 *   but later we might know the value both before and after the
 *   modification, and just replace each with a static array. In fact,
 *   this is almost guaranteed to happen when building arrays from
 *   constants (not literals), because on the first pass we won't know
 *   the constant values, and will produce a CArr, but eventually we
 *   could figure them out, and produce an SArr. This means that in
 *   practice, we can't use CArr anywhere, because it might be
 *   improved to SArr, which is not a subtype. Even if we only
 *   generated CArr after all modifications are done (eg during the
 *   insert-assertions phase) we could still run into problems where
 *   we annotate an array as CArr but jit time analysis/optimization
 *   was able to produce a static array - so it doesn't appear to be
 *   useful, except as a superclass of SArr.
 *
 */

//////////////////////////////////////////////////////////////////////

namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace HHBBC {

struct Context;
struct IIndex;
struct Type;

namespace res { struct Class; }
namespace php { struct Class; }

#define DATATAGS                                                \
  DT(Str, SString, sval)                                        \
  DT(Int, int64_t, ival)                                        \
  DT(Dbl, double, dval)                                         \
  DT(ArrLikeVal, SArray, aval)                                  \
  DT(Obj, DCls, dobj)                                           \
  DT(WaitHandle, copy_ptr<DWaitHandle>, dwh)                    \
  DT(Cls, DCls, dcls)                                           \
  DT(LazyCls, SString, lazyclsval)                              \
  DT(EnumClassLabel, SString, eclval)                           \
  DT(ArrLikePacked, copy_ptr<DArrLikePacked>, packed)           \
  DT(ArrLikePackedN, copy_ptr<DArrLikePackedN>, packedn)        \
  DT(ArrLikeMap, copy_ptr<DArrLikeMap>, map)                    \
  DT(ArrLikeMapN, copy_ptr<DArrLikeMapN>, mapn)

// Tag for what kind of specialized data a Type object has.
enum class DataTag : uint8_t {
  None,
#define DT(name,...) name,
  DATATAGS
#undef DT
};

//////////////////////////////////////////////////////////////////////

/*
 * Information about a class type. The class is either exactly the
 * supplied class, a subtype of the supplied class, or an intersection
 * of classes it is a subtype of.
 *
 * The intersection case is needed to maintain monotonicity when
 * performing unions or intersections involving intersections. We want
 * to maintain the invariant that if a <= A, and b <= B, then
 * union_of(a, b) <= union_of(A, B) (the same applies for
 * intersection_of). Another way to state this is that refining a type
 * anywhere should never result in a worse type anywhere else.
 *
 * When unioning/intersecting an interface with anything else, there's
 * not necessarily a single result class which guarantees
 * monotonicity. In one of those cases, we instead produce a list of
 * classes which the result is a subtype of.
 *
 * In addition, we want the intersection representation to uniquely
 * identify a class, so we impose a certain canonical form. No class
 * in the list can be a subtype of another (if so, the "larger" class
 * is redundant and should be dropped). Every class "could be" every
 * other class. If not, the intersection is empty and the type is
 * actually Bottom. Finally, the list is kept sorted. The ordering is
 * fixed but arbitrary and we try to ensure that "smaller" classes
 * come first.
 *
 * The intersection always have 2 or more elements (any less should be
 * Bottom, Exact or Sub instead). The intersection can only ever
 * contain resolved classes or unresolved classes, never a mix
 * (unioning a resolved class with an unresolved class results in
 * TObj/TCls, and intersecting a resolved class with an unresolved
 * class results in TBottom).
 */
struct DCls {
  using IsectSet = std::vector<res::Class>;

  DCls() : DCls{TagNone, nullptr} {}

  static DCls MakeExact(res::Class cls, bool nonReg);
  static DCls MakeSub(res::Class cls, bool nonReg);
  static DCls MakeIsect(IsectSet isect, bool nonReg);
  static DCls MakeIsectAndExact(res::Class exact, IsectSet isect, bool nonReg);

  // Need to implement these manually so we do proper ref-counting on
  // the IsectWrapper (if available).
  DCls(const DCls&);
  DCls(DCls&& o) noexcept : val{std::move(o.val)}
  { o.val.set(TagNone, nullptr); }

  DCls& operator=(const DCls&);
  DCls& operator=(DCls&& o) { val.swap(o.val); return *this; }

  ~DCls();

  bool isExact() const { return (val.tag() & (TagExact|TagIsect)) == TagExact; }
  bool isSub() const { return !(val.tag() & (TagExact|TagIsect)); }
  bool isIsect() const { return tagIsIsect(val.tag()); }
  bool isIsectAndExact() const { return tagIsIsectAndExact(val.tag()); }

  bool containsNonRegular() const { return !(val.tag() & TagReg); }

  // Obtain the res::Class this DCls represents. Only valid if
  // !isSect(), as that cannot be completely represented by any single
  // res::Class.
  res::Class cls() const;

  // Obtain a res::Class which is a super-type of what this DCls
  // represents. That is, it may be larger than the "actual"
  // class. For non-intersections, this is just cls(). For
  // intersections, it's one of the classes in the intersection (any
  // of them is valid, as we're a subclass of all of them). We use the
  // first class in the list, which in canonical order is the
  // smallest.
  res::Class smallestCls() const;

  const IsectSet& isect() const;
  std::pair<res::Class, const IsectSet*> isectAndExact() const;

  bool isCtx() const { return val.tag() & TagCtx; }

  void setCtx(bool ctx) {
    val.set(ctx ? addCtx(val.tag()) : removeCtx(val.tag()), val.ptr());
  }
  void setNonReg(bool nonReg) {
    val.set(nonReg ? addNonReg(val.tag()) : removeNonReg(val.tag()), val.ptr());
  }

  void setCls(res::Class cls);

  bool same(const DCls& o, bool checkCtx = true) const;

  void serde(BlobEncoder&) const;
  void serde(BlobDecoder&);

private:
  // To keep size down, we encode everything into a single
  // pointer. The tag encodes whether isCtx() is true, and the type of
  // pointer. The pointer will either be a res::Class (in opaque
  // encoding), or to an IsectWrapper.
  enum Tag : uint8_t {
    TagNone  = 0,
    TagExact = (1 << 0),
    TagIsect = (1 << 1),
    TagReg   = (1 << 2),
    TagCtx   = (1 << 3)
  };
  CompactTaggedPtr<void, Tag> val;

  DCls(Tag t, void* p) : val{t, p} {}

  static bool tagIsIsect(Tag t) {
    return (t & Tag(TagExact|TagIsect)) == TagIsect;
  }
  static bool tagIsIsectAndExact(Tag t) {
    return (t & Tag(TagExact|TagIsect)) == Tag(TagExact|TagIsect);
  }

  static Tag addCtx(Tag t)       { return Tag(t | TagCtx); }
  static Tag removeCtx(Tag t)    { return Tag(t & ~TagCtx); }
  static Tag addNonReg(Tag t)    { return Tag(t & ~TagReg); }
  static Tag removeNonReg(Tag t) { return Tag(t | TagReg); }

  struct IsectWrapper;
  struct IsectAndExactWrapper;

  IsectWrapper* rawIsect() const;
  IsectAndExactWrapper* rawIsectAndExact() const;
};

std::string show(const DCls&, bool isObj = false);

//////////////////////////////////////////////////////////////////////

struct DWaitHandle;
struct DArrLikePacked;
struct DArrLikePackedN;
struct DArrLikeMap;
struct DArrLikeMapN;
struct IterTypes;
struct COWer;

//////////////////////////////////////////////////////////////////////

struct MapElem;
using MapElems = ArrayLikeMap<TypedValue, MapElem>;

//////////////////////////////////////////////////////////////////////

enum class LegacyMark : uint8_t {
  Bottom,   // NA (for when type does not include arrays)
  Marked,   // definitely mark
  Unmarked, // definitely unmarked
  Unknown   // either
};

//////////////////////////////////////////////////////////////////////

enum class Emptiness {
  Empty,
  NonEmpty,
  Maybe
};

// Represents whether a promotion can happen, and whether that
// promotion might throw.
enum class Promotion {
  No,
  Yes,
  YesMightThrow
};

//////////////////////////////////////////////////////////////////////

struct Type {
  Type() : Type{BBottom} {}
  explicit Type(trep t) : Type{t, topLegacyMarkForBits(t)} {}
  Type(trep t, LegacyMark m)
    : m_bits{t}
    , m_dataTag{DataTag::None}
    , m_legacyMark{m} {
    assertx(checkInvariants());
  }

  Type(const Type& o) noexcept
    : m_raw{o.m_raw}
  {
    SCOPE_EXIT { assertx(checkInvariants()); };
    if (LIKELY(m_dataTag == DataTag::None)) return;
    copyData(o);
  }

  Type(Type&& o) noexcept
    : m_raw{o.m_raw}
  {
    SCOPE_EXIT { assertx(o.checkInvariants()); };
    if (LIKELY(m_dataTag == DataTag::None)) return;
    moveData(std::move(o));
  }

  Type& operator=(const Type&) noexcept;
  Type& operator=(Type&&) noexcept;

  ~Type() {
    if (LIKELY(m_dataTag == DataTag::None)) return;
    destroyData();
  }

  template <typename SerDe> void serde(SerDe& sd) {
    ScopedStringDataIndexer _;
    if constexpr (SerDe::deserializing) {
      if (UNLIKELY(m_dataTag != DataTag::None)) destroyData();
      sd(m_raw);
      switch (m_dataTag) {
        case DataTag::None: break;
        #define DT(tag_name,type,name)            \
          case DataTag::tag_name: {               \
            type t;                               \
            sd(t);                                \
            new (&m_data.name) type{std::move(t)};\
            break;                                \
          }
        DATATAGS
        #undef DT
      }
      assertx(checkInvariants());
    } else {
      assertx(checkInvariants());
      sd(m_raw);
      switch (m_dataTag) {
        case DataTag::None: break;
        #define DT(tag_name,type,name) \
          case DataTag::tag_name: sd(m_data.name); break;
        DATATAGS
        #undef DT
      }
    }
  }

  /*
   * Exact equality or inequality of types, and hashing.
   */
  bool equal(const Type&) const;
  bool equalNoContext(const Type&) const;

  // operator== deliberately not implemented. You must be explicit
  // about whether you need to care about the context or not.

  size_t hash() const;

  const Type& operator|=(const Type& other);
  const Type& operator|=(Type&& other);
  const Type& operator&=(const Type& other);
  const Type& operator&=(Type&& other);

  /*
   * Returns true if this type is more refined or strictly more
   * refined than `o`.  This is similar to the subtype operations
   * defined below, except they take into account if a type is tagged
   * as a context.
   */
  bool moreRefined(const Type& o) const;
  bool strictlyMoreRefined(const Type& o) const;

  /*
   * Returns true if this type is definitely going to be a subtype or a strict
   * subtype of `o' at runtime.  If this function returns false, this may
   * still be a subtype of `o' at runtime, it just may not be known.
   */
  bool subtypeOf(const Type& o) const;
  bool strictSubtypeOf(const Type& o) const;

  /*
   * Similar, but only check the trep (same as subtypeOf(Type{bits}),
   * but cheaper).
   */
  bool subtypeOf(trep b) const { return HPHP::HHBBC::subtypeOf(bits(), b); }
  bool strictSubtypeOf(trep bits) const {
    // If the bits match, only specialized data can make it more
    // specific.
    return is(bits) ? hasData() : subtypeOf(bits);
  }

  /*
   * Similar to subtypeOf(), but only checks if *this is a subtype of
   * b1 excluding all types except for those in b2. For example, this
   * is useful if one wants to check if *this is a specific array
   * type, without caring if its unioned with non-array types.
   */
  bool subtypeAmong(trep b1, trep b2) const {
    return HPHP::HHBBC::subtypeAmong(bits(), b1, b2);
  }

  /*
   * Returns whether there are any values of this type that are also
   * values of the type `o'.
   * When this function returns false, it is known that this type
   * must not be in any subtype relationship with the argument Type 'o'.
   * When true is returned the two types may still be unrelated but it is
   * not possible to tell.
   * Essentially this function can conservatively return true but must be
   * precise when returning false.
   */
  bool couldBe(const Type& o) const;
  bool couldBe(trep b) const { return HPHP::HHBBC::couldBe(bits(), b); }

  // Equality, but more efficient
  bool is(trep b) const { return bits() == b; }

  bool hasData() const { return m_dataTag != DataTag::None; }

  struct ArrayCat {
    enum { None, Empty, Packed, Struct, Mixed } cat;
    bool hasValue;
  };

  static LegacyMark topLegacyMarkForBits(trep b) {
    return HPHP::HHBBC::couldBe(b, kLegacyMarkBits)
      ? LegacyMark::Unknown : LegacyMark::Bottom;
  }

  // Legacy marks are only tracked for Vec and Dict.
  static constexpr trep kLegacyMarkBits = BVec | BDict;

private:
  static Type specializeWaitHandle(Type, Type, bool);

  friend Optional<int64_t> arr_size(const Type& t);
  friend ArrayCat categorize_array(const Type& t);
  friend CompactVector<LSString> get_string_keys(const Type& t);
  friend Type wait_handle(Type, bool);
  friend bool is_specialized_wait_handle(const Type&);
  friend bool is_specialized_array_like(const Type&);
  friend bool is_specialized_array_like_arrval(const Type&);
  friend bool is_specialized_array_like_packedn(const Type&);
  friend bool is_specialized_array_like_packed(const Type&);
  friend bool is_specialized_array_like_mapn(const Type&);
  friend bool is_specialized_array_like_map(const Type&);

  friend bool is_specialized_obj(const Type&);
  friend bool is_specialized_cls(const Type&);
  friend bool is_specialized_lazycls(const Type&);
  friend bool is_specialized_ecl(const Type&);
  friend bool is_specialized_string(const Type&);
  friend bool is_specialized_int(const Type&);
  friend bool is_specialized_double(const Type&);
  friend Type wait_handle_inner(const Type&);
  friend bool wait_handle_can_suspend(const Type&);
  friend Type set_wait_handle_can_suspend(Type, bool);
  friend Type sval(SString);
  friend Type sval_nonstatic(SString);
  friend Type sval_counted(SString);
  friend Type ival(int64_t);
  friend Type dval(double);
  friend Type lazyclsval(SString);
  friend Type enumclasslabelval(SString);
  friend Type subObj(res::Class);
  friend Type objExact(res::Class);
  friend Type subCls(res::Class, bool);
  friend Type clsExact(res::Class, bool);
  friend Type packed_impl(trep, LegacyMark, std::vector<Type>);
  friend Type packedn_impl(trep, LegacyMark, Type);
  friend Type map_impl(trep, LegacyMark, MapElems, Type, Type);
  friend Type mapn_impl(trep, LegacyMark, Type, Type);
  friend const DCls& dobj_of(const Type&);
  friend Type demote_wait_handle(Type);
  friend const DCls& dcls_of(const Type&);
  friend SString sval_of(const Type&);
  friend SString lazyclsval_of(const Type&);
  friend SString eclval_of(const Type&);
  friend int64_t ival_of(const Type&);
  friend Type union_of(Type, Type);
  friend Type intersection_of(Type, Type);
  friend void widen_type_impl(Type&, uint32_t);
  friend Type widen_type(Type);
  friend Type widening_union(const Type&, const Type&);
  friend std::pair<Emptiness, bool> emptiness(const Type&);
  friend Type opt(Type);
  friend Type unopt(Type);
  template<typename R, bool, bool>
  friend R tvImpl(const Type&);
  friend Type scalarize(Type t);

  friend Type return_with_context(Type, Type);
  friend Type setctx(Type, bool);
  friend Type unctx(Type);

  friend Type remove_data(Type, trep);
  friend Type remove_int(Type);
  friend Type remove_double(Type);
  friend Type remove_string(Type);
  friend Type remove_lazycls(Type);
  friend Type remove_cls(Type);
  friend Type remove_obj(Type);
  friend Type remove_keyset(Type);
  friend Type remove_bits(Type, trep);
  friend std::pair<Type, Type> split_obj(Type);
  friend std::pair<Type, Type> split_cls(Type);
  friend std::pair<Type, Type> split_array_like(Type);
  friend std::pair<Type, Type> split_string(Type);
  friend std::pair<Type, Type> split_lazycls(Type);

  friend Type promote_classish(Type);

  friend std::string show(const Type&);
  friend std::pair<Type,bool> array_like_elem_impl(const Type&, const Type&);
  friend std::pair<Type,bool> array_like_set_impl(Type,
                                                  const Type&,
                                                  const Type&);
  friend std::pair<Type,bool> array_like_newelem_impl(Type, const Type&);
  friend std::pair<Type,bool> arr_val_elem(const Type&, const Type&);
  friend std::pair<Type,bool> arr_packed_elem(const Type&, const Type&);
  friend std::pair<Type,bool> arr_packedn_elem(const Type&, const Type&);
  friend std::pair<Type,bool> arr_map_elem(const Type&, const Type&);
  friend std::pair<Type,bool> arr_mapn_elem(const Type&, const Type&);
  friend bool arr_packed_set(Type&, const Type&, const Type&, bool);
  friend bool arr_packedn_set(Type&, const Type&, const Type&, bool);
  friend bool arr_map_set(Type&, const Type&, const Type&, bool);
  friend bool arr_map_newelem(Type&, const Type&, bool);
  friend IterTypes iter_types(const Type&);
  friend Optional<RepoAuthType> make_repo_type_arr(const Type&);

  friend bool array_like_set_can_be_undone(const Type&, const Type&);
  friend Type undo_array_like_set(Type, const Type&);
  friend Type array_like_set_values(Type, Type, Type);

  friend Type vec_val(SArray);
  friend Type vec_empty();
  friend Type some_vec_empty();
  friend Type dict_val(SArray);
  friend Type dict_empty();
  friend Type some_dict_empty();
  friend Type keyset_val(SArray);
  friend bool could_contain_objects(const Type&);
  friend Type loosen_staticness(Type);
  friend Type loosen_string_staticness(Type);
  friend Type loosen_array_staticness(Type);
  friend Type loosen_values(Type);
  friend Type loosen_string_values(Type);
  friend Type loosen_array_values(Type);
  friend Type loosen_emptiness(Type);
  friend Type loosen_likeness(Type);
  friend Type loosen_likeness_recursively(Type);
  friend Type loosen_to_datatype(Type);
  friend Type add_nonemptiness(Type);
  friend Type assert_emptiness(Type);
  friend Type assert_nonemptiness(Type);
  friend Type remove_uninit(Type t);
  friend Type to_cell(Type t);
  friend bool inner_types_might_raise(const Type& t1, const Type& t2);
  friend std::pair<Type, Promotion> promote_classlike_to_key(Type);
  friend Type toobj(const Type&);
  friend Type objcls(const Type&);

  friend bool might_have_dcls(const Type&);

  friend struct WaitHandleCOWer;
  friend struct ArrLikePackedNCOWer;
  friend struct ArrLikePackedCOWer;
  friend struct ArrLikeMapNCOWer;
  friend struct ArrLikeMapCOWer;

  friend Type unserialize_classes(const IIndex&, Type);
  friend void unserialize_classes_impl(const IIndex&, const Type&, COWer&);

  friend Type serialize_classes(Type);
  friend void serialize_classes_impl(const Type&, COWer&);

  // These have to be defined here but are not meant to be used
  // outside of type-system.cpp
  friend Type isectObjInternal(DCls::IsectSet);
  friend Type isectClsInternal(DCls::IsectSet, bool);
  friend Type isectAndExactObjInternal(res::Class, DCls::IsectSet);
  friend Type isectAndExactClsInternal(res::Class, DCls::IsectSet, bool);

  friend Type set_trep_for_testing(Type, trep);
  friend trep get_trep_for_testing(const Type&);

  friend Type make_obj_for_testing(trep, res::Class, bool, bool, bool);
  friend Type make_cls_for_testing(trep, res::Class, bool, bool, bool, bool);
  friend Type make_arrval_for_testing(trep, SArray);
  friend Type make_arrpacked_for_testing(trep, std::vector<Type>,
                                         Optional<LegacyMark>);
  friend Type make_arrpackedn_for_testing(trep, Type);
  friend Type make_arrmap_for_testing(trep, MapElems, Type, Type,
                                      Optional<LegacyMark>);
  friend Type make_arrmapn_for_testing(trep, Type, Type);

  friend Type set_mark_for_testing(Type, LegacyMark);
  friend Type loosen_mark_for_testing(Type);

private:
  union Data {
    Data() {}
    ~Data() {}

#define DT(tag_name,type,name) type name;
  DATATAGS
#undef DT
  };

  template<class Ret, class T, class Function>
  struct DDHelperFn;

private:
  trep bits() const { return trep(m_bits); }
  static Type unctxHelper(Type, bool&);

  template<class Ret, class T, class Function>
  DDHelperFn<Ret,T,Function> ddbind(const Function& f, const T& t) const;
  template<class Ret, class T, class Function>
  Ret dd2nd(const Type&, DDHelperFn<Ret,T,Function>) const;
  template<class Function> typename Function::result_type
  dualDispatchDataFn(const Type&, Function) const;
  template<typename Function> typename Function::result_type
  dispatchArrLikeNone(const Type&, Function) const;

  template<bool contextSensitive>
  bool equivImpl(const Type& o) const;
  template<bool contextSensitive>
  bool subtypeOfImpl(const Type& o) const;
  bool checkInvariants() const;

  void copyData(const Type& o);
  void moveData(Type&& o);
  void destroyData();

private:
  union {
    struct {
      uint64_t m_bits : kTRepBitsStored;
      DataTag m_dataTag;
      LegacyMark m_legacyMark;
    };
    uint64_t m_raw;
  };
  Data m_data;
};

//////////////////////////////////////////////////////////////////////

struct TypeHasher {
  size_t operator()(const Type& t) const {
    return t.hash();
  }
};

//////////////////////////////////////////////////////////////////////

// The value portion of MapElems. Stores the known type of the value,
// and whether the key is static or not. (Modifying the key is
// problematic, so its easier to store with the value).

struct MapElem {
  Type val;
  TriBool keyStaticness;

  MapElem() = default;
  MapElem(Type val, TriBool keyStaticness)
    : val{std::move(val)}, keyStaticness{keyStaticness} {}

  // Keep the same value, but modify the staticness
  MapElem withStaticness(TriBool b) const {
    return MapElem{val, b};
  }
  // Keep the same staticness, but modify the value
  MapElem withType(Type t) const {
    return MapElem{std::move(t), keyStaticness};
  }

  // Create a MapElem using staticness inferred from the key's type.
  static MapElem KeyFromType(const Type& key, Type val);

  static MapElem IntKey(Type val) {
    return MapElem{std::move(val), TriBool::Yes};
  }
  static MapElem StrKey(Type val) {
    return MapElem{std::move(val), TriBool::Maybe};
  }
  static MapElem SStrKey(Type val) {
    return MapElem{std::move(val), TriBool::Yes};
  }
  static MapElem CStrKey(Type val) {
    return MapElem{std::move(val), TriBool::No};
  }

  template <typename SerDe> void serde(SerDe& sd) {
    sd(val)(keyStaticness);
  }
};

//////////////////////////////////////////////////////////////////////

/*
 * Information about a wait handle (sub-class of HH\\Awaitable) carry a
 * type that awaiting the wait handle will produce.
 */
struct DWaitHandle {
  DWaitHandle() = default;
  // cls is HH\\Awaitable or a subtype. Keeping it here preserves class
  // refinements when intersecting a wait handle with an object type and lets
  // us demote to a DCls without having the Index available.
  DWaitHandle(DCls cls, Type inner, bool canSuspend)
    : cls{std::move(cls)}
    , inner{std::move(inner)}
    , canSuspend{canSuspend} {}
  DCls cls;
  Type inner;
  // False means the wait handle is known to be finished. True conservatively
  // means awaiting it may suspend.
  bool canSuspend{true};

  template <typename SerDe> void serde(SerDe& sd) {
    sd(inner)(cls)(canSuspend);
  }
};

struct DArrLikePacked {
  DArrLikePacked() = default;
  explicit DArrLikePacked(std::vector<Type> elems)
    : elems(std::move(elems)) {}
  std::vector<Type> elems;
  template <typename SerDe> void serde(SerDe& sd) { sd(elems); }
};

struct DArrLikePackedN {
  DArrLikePackedN() = default;
  explicit DArrLikePackedN(Type t) : type(std::move(t)) {}
  Type type;
  template <typename SerDe> void serde(SerDe& sd) { sd(type); }
};

struct DArrLikeMap {
  DArrLikeMap() = default;
  explicit DArrLikeMap(MapElems map, Type optKey, Type optVal)
    : map(std::move(map))
    , optKey(std::move(optKey))
    , optVal(std::move(optVal))
  {}
  bool hasOptElements() const { return !optKey.is(BBottom); }
  // The array always starts with these known keys
  MapElems map;
  // Key/value types for optional elements after known keys. Bottom if
  // none.
  Type optKey;
  Type optVal;
  template <typename SerDe> void serde(SerDe& sd) {
    sd(map)(optKey)(optVal);
  }
};

// DArrLikePackedN and DArrLikeMapN do not need the LegacyMark because they
// cannot be converted to a TypedValue
struct DArrLikeMapN {
  DArrLikeMapN() = default;
  explicit DArrLikeMapN(Type key, Type val)
    : key(std::move(key))
    , val(std::move(val))
  {}
  Type key;
  Type val;
  template <typename SerDe> void serde(SerDe& sd) { sd(key)(val); }
};

//////////////////////////////////////////////////////////////////////

#define X(y, ...) extern const Type T##y;
HHBBC_TYPE_PREDEFINED(X)
#undef X

//////////////////////////////////////////////////////////////////////

/*
 * Return WaitH<T> for a type t. canSuspend may be false to indicate that the
 * wait handle is guaranteed to be finished.
 */
Type wait_handle(Type t, bool canSuspend = true);

/*
 * Return T from a WaitH<T>.
 *
 * Pre: is_specialized_wait_handle(t);
 */
Type wait_handle_inner(const Type& t);

/*
 * Return whether awaiting t may suspend. This returns false only when every
 * object represented by t is a wait handle known to be finished. Non-object
 * components are ignored because awaiting them throws synchronously.
 */
bool wait_handle_can_suspend(const Type& t);

/*
 * Refine the suspension state carried by a specialized wait handle.
 */
Type set_wait_handle_can_suspend(Type t, bool canSuspend);

/*
 * Create Types that represent constant values.
 */
Type ival(int64_t);
Type dval(double);
Type lazyclsval(SString);
Type enumclasslabelval(SString);
Type vec_val(SArray);
Type dict_val(SArray);
Type keyset_val(SArray);

/*
 * Create string types with different counted/static flavor.
 */
Type sval(SString);
Type sval_nonstatic(SString);
Type sval_counted(SString);

/*
 * Create empty string types with different counted/static flavor.
 */
Type sempty();
Type sempty_nonstatic();
Type sempty_counted();

/*
 * Create static empty array types
 */
Type vec_empty();
Type dict_empty();
Type keyset_empty();

/*
 * Create an any-countedness empty array/vec/dict type.
 */
Type some_vec_empty();
Type some_dict_empty();
Type some_keyset_empty();

/*
 * Create types for objects or classes with some known constraint on
 * which res::Class is associated with them.
 */
Type subObj(res::Class);
Type objExact(res::Class);
Type subCls(res::Class, bool nonReg);
Type clsExact(res::Class, bool nonReg);

/*
 * vec types with known size.
 *
 * Pre: !v.empty()
 */
Type vec(std::vector<Type> v);
Type svec(std::vector<Type> v);

/*
 * Vec type of unknown size.
 */
Type vec_n(Type);
Type svec_n(Type);

/*
 * Struct-like dicts.
 *
 * Pre: !m.empty()
 */
Type dict_map(MapElems m, Type optKey = TBottom, Type optVal = TBottom);
Type sdict_map(MapElems m, Type optKey = TBottom, Type optVal = TBottom);

/*
 * Dict with key/value types.
 */
Type dict_n(Type, Type);
Type sdict_n(Type, Type);

/*
 * Dicts with vector-like data.
 */
Type dict_packed(std::vector<Type> v);
Type sdict_packed(std::vector<Type> v);
Type dict_packedn(Type);
Type sdict_packedn(Type);

/*
 * Keyset with key (same as value) type.
 */
Type keyset_n(Type);
Type ckeyset_n(Type);

/*
 * Keyset from MapElems
 */
Type keyset_map(MapElems);

/*
 * Unions the type `t' with TInitNull. This is identical to
 * union_of(t, TInitNull) but more efficient.
 */
Type opt(Type t);

/*
 * Removes TInitNull from `t' (if present). If `t' is TInitNull, this
 * results in TBottom.
 */
Type unopt(Type t);

/*
 * Improves the type `t` given the current context.  This returns the
 * intersection of the type `t` with the `context` if the `context` is a valid
 * class or object, and `t` is tagged as being the context.  If `context` is
 * a class we will first convert it to an object.
 */
Type return_with_context(Type t, Type context);

/*
 * If `to` is false.  This is an identity operation.  If `to` is true, and
 * `t` is a specialized object or class, this will return `t` tagged as a
 * context.
 */
Type setctx(Type t, bool to = true);

/*
 * This removes any context tags in the type `t`, even if nested inside other
 * types.
 */
Type unctx(Type t);

/*
 * Refinedness equivalence checks.
 */
inline bool equal(const Type& a, const Type& b) {
  return a.equal(b);
}

template<
  typename Iterable,
  typename = std::enable_if<
    std::is_same<typename Iterable::value_type, Type>::value
  >
>
bool equal(const Iterable& a, const Iterable& b) {
  if (a.size() != b.size()) return false;
  for (auto ita = a.begin(), itb = b.begin(); ita != a.end(); ++ita, ++itb) {
    if (!equal(*ita, *itb)) return false;
  }
  return true;
}

inline bool equal_no_context(const Type& a, const Type& b) {
  return a.equalNoContext(b);
}

/*
 * Returns true if type 't' represents a "specialized" object, that is an
 * object of a known class, or an optional object of a known class.
 */
bool is_specialized_obj(const Type&);

/*
 * Returns true if type 't' represents a "specialized" class---i.e. a class
 * with a DCls structure.
 */
bool is_specialized_cls(const Type&);

/*
 * Returns true if type 't' represents a "specialized"
 * string/int/double/lazy class--i.e. with a known value.
 */
bool is_specialized_string(const Type&);
bool is_specialized_lazycls(const Type&);
bool is_specialized_ecl(const Type&);
bool is_specialized_int(const Type&);
bool is_specialized_double(const Type&);

/*
 * Returns whether `t' is a WaitH<T> or ?WaitH<T> for some T.
 *
 * Note that this function returns false for Obj<=WaitHandle with no
 * tracked inner type.
 */
bool is_specialized_wait_handle(const Type& t);

/*
 * Returns true if type 't' represents a specialized array. That is,
 * with either a constant value or some (maybe partially) known shape.
 */
bool is_specialized_array_like(const Type& t);
bool is_specialized_array_like_arrval(const Type& t);
bool is_specialized_array_like_packedn(const Type& t);
bool is_specialized_array_like_packed(const Type& t);
bool is_specialized_array_like_mapn(const Type& t);
bool is_specialized_array_like_map(const Type& t);

/*
 * Split the type `t' into two types, one representing the
 * TObj/TCls/TArrLike/TStr portion, and the other containing the
 * rest. If `t' doesn't contain one of those types, the first result
 * will be TBottom. Likewise if it's only one of those types, the
 * second result will be TBottom. `t' must be a subtype of TCell
 * (removing types form TTop produces unrepresentable types).
 */
std::pair<Type, Type> split_obj(Type);
std::pair<Type, Type> split_cls(Type);
std::pair<Type, Type> split_array_like(Type);
std::pair<Type, Type> split_string(Type);
std::pair<Type, Type> split_lazycls(Type);

/*
 * Remove TInt/TDbl/TStr/TCls/TObj from the type `t', including any
 * associated specialized data. `t' must be a subtype of TCell
 * (removing types from TTop produces unrepresentable types).
 */
Type remove_int(Type);
Type remove_double(Type);
Type remove_string(Type);
Type remove_lazycls(Type);
Type remove_cls(Type);
Type remove_obj(Type);

// Remove TKeyset from the type `t'. Unlike the other remove
// functions, this attempts to keep any appropriate array specialized
// data. `t' must be a subtype of TCell.
Type remove_keyset(Type);

/*
 * Remove arbitrary bits from the type `t', including any specialized
 * data associated with those bits. This is a more general version of
 * the above remove_* functions, but not as efficient. `t' must be a
 * subtype of TCell for the same reasons as above.
 *
 * Note: if you remove any of BArrLikeN bits, all array specialized
 * data is removed.
 */
Type remove_bits(Type, trep);

/*
 * If the type might contain TCls or TLazyCls, remove it, and add
 * TSStr instead. Any lazy class/class specialization is preserved and
 * translated into a string specialization.
 */
Type promote_classish(Type);

/*
 * Returns the best known instantiation of a class type.
 *
 * Pre: t.subypeOf(TCls)
 */
Type toobj(const Type& t);

/*
 * Returns the best known TCls subtype for an object type.
 *
 * Pre: t.subtypeOf(TObj)
 */
Type objcls(const Type& t);

/*
 * If the type t has a known constant value, return it as a
 * TypedValue. Otherwise return std::nullopt. tv() will fail to return
 * a constant value if the type is known to be counted. tvCounted()
 * will return the constant value regardless of the type's
 * countedness.
 *
 * The returned TypedValue can only contain non-reference-counted types.
 */
Optional<TypedValue> tv(const Type& t);
Optional<TypedValue> tvCounted(const Type& t);

/*
 * If the type t has a known constant value, return it as a TypedValue.
 * Otherwise return std::nullopt.
 *
 * The returned TypedValue may contain reference-counted types.
 *
 * You are responsible for any required ref-counting.
 */
Optional<TypedValue> tvNonStatic(const Type& t);

/*
 * If the type t has a known constant value, return true. Otherwise
 * return false. is_scalar() will fail to return true if the type is
 * known to be counted. is_scalar_counted() will return true
 * regardless of the type's countedness.
 */
bool is_scalar(const Type& t);
bool is_scalar_counted(const Type& t);

/*
 * Return the canonical scalar type for t - equivalent to
 * from_cell(*tv(t)).
 *
 * This can be used to ensure that the arguments in a CallContext are
 * canonicalized, so that immaterial changes to them (eg TArrN ->
 * TSArrN or DArrLikeMap -> DArrLikeVal) don't affect which entry gets
 * looked up.
 *
 * pre: is_scalar(t).
 */
Type scalarize(Type t);

/*
 * Get the type in our typesystem that corresponds to an hhbc
 * IsTypeOp.
 *
 * Pre: op != IsTypeOp::Scalar
 */
Type type_of_istype(IsTypeOp op);

/*
 * Get the hhbc IsTypeOp that corresponds to the type in our typesystem.
 * Returns std::nullopt if no matching IsTypeOp is found.
 */
Optional<IsTypeOp> type_to_istypeop(const Type& t);

/*
 * Get the type in our typesystem that corresponds to type given by the
 * potentially unresolved type structure.
 * Returns std::nullopt if the type structure is unresolved or
 * no matching Type is found.
 *
 */
Optional<Type> type_of_type_structure(const IIndex&, Context, SArray ts);

/*
 * Return the DCls structure for a strict subtype of TObj or TOptObj.
 *
 * Pre: is_specialized_obj(t)
 */
const DCls& dobj_of(const Type& t);

/*
 * Return the DCls structure for a strict subtype of TCls.
 *
 * Pre: is_specialized_cls(t)
 */
const DCls& dcls_of(const Type& t);

/*
 * Return the SString for a strict subtype of TStr.
 *
 * Pre: is_specialized_string(t)
 */
SString sval_of(const Type& t);

/*
 * Return the SString for a strict subtype of TLazyCls.
 *
 * Pre: is_specialized_lazycls(t)
 */
SString lazyclsval_of(const Type& t);

/*
 * Return the SString for a strict subtype of TEnumClassLabel.
 *
 * Pre: is_specialized_ecl(t)
 */
SString eclval_of(const Type& t);

/*
 * Return the specialized integer value for a type.
 *
 * Pre: is_specialized_int(t)
 */
int64_t ival_of(const Type& t);

/*
 * Create a Type from a TypedValue.
 *
 * Pre: the cell must contain a non-reference-counted type.
 * Post: returned type is a subtype of TUnc
 */
Type from_cell(TypedValue tv);

/*
 * Create a Type from a DataType.
 *
 * Pre: dt is one of the DataTypes that actually represent php values
 * (or KindOfUninit).
 */
Type from_DataType(DataType dt);

/*
 * Create a Type from a builtin type specification string.
 *
 * This is used for HNI class properties.  We assume that these are
 * accurate.  `s' may be nullptr.
 */
Type from_hni_constraint(SString s);

/*
 * Make a type that represents values from the intersection of the
 * supplied types.
 */
Type intersection_of(Type a, Type b);

/*
 * Make a type that represents values from either of the supplied
 * types.
 *
 * Importantly, note that there are infinitely long chains of array
 * types that continue to become less specialized, so chains of
 * union_of operations are not guaranteed to reach a stable point in
 * finite steps.
 */
Type union_of(Type a, Type b);

// Helper function to union together multiple things
template<typename... Types>
Type union_of(Type a, Type b, Types... ts) {
  return union_of(std::move(a), union_of(std::move(b), std::move(ts)...));
}

/*
 * Widening union.
 *
 * This operation returns a type T, such that a is a subtype of T, b
 * is a subtype of T, and union_of(a, b) is a subtype of T.  The
 * widening union also has the property that every possible chain of
 * successive applications of the function eventually reaches a stable
 * point.
 *
 * This is currently implemented by unioning the types, then applying
 * widen_type() to the result.
 *
 * For portions of our analysis that rely on growing types reaching
 * stable points for termination, this function must occasionally be
 * used instead of union_of to guarantee termination.  See details in
 * analyze.cpp.
 */
Type widening_union(const Type& a, const Type& b);

/*
 * Widen a type to one which has a finite chain under the union operator. This
 * generally involves restricting the type's nesting depth to a fixed limit and
 * preventing a specialized array type from growing larger unbounded.
 */
Type widen_type(Type t);

/*
 * Returns what we know about the emptiness of the type and whether
 * checking is effect-free.
 */
std::pair<Emptiness, bool> emptiness(const Type&);

/*
 * Returns whether a Type could hold an object that has a custom
 * boolean conversion function.
 */
bool could_have_magic_bool_conversion(const Type&);

/*
 * Returns the smallest type that `a' is a subtype of, from the
 * following set: TInitCell, TUninit.
 *
 * Pre: `a' is a subtype of TCell.
 */
Type stack_flav(Type a);

/*
 * Discard any countedness information about the type. Force any type
 * (recursively) which contains only static or counted variants to contain
 * both. Doesn't change the type otherwise.
 */
Type loosen_staticness(Type);

/*
 * Like loosen_staticness, but non-recursive and only affects
 * arrays. Used to model the potential COW effects during defining
 * member instructions.
 */
Type loosen_array_staticness(Type);

/*
 * Discard any string countedness information. Force any string type
 * which contains only static or counted variants to contain
 * both. Doesn't change any non-string types.
 */
Type loosen_string_staticness(Type);

/*
 * Force any type which might contain TVec or TDict to contain TVec|TDict.
 * This discards any emptiness, staticness, or value information if the
 * type is modified.
 */
Type loosen_vec_or_dict(Type);

/*
 * Drop any data from the type (except for object class information) and force
 * TTrue or TFalse to TBool. Doesn't change the type otherwise.
 */
Type loosen_values(Type t);

/*
 * Drop any knowledge about the inner structure of array-like types
 * The type is unchanged otherwise. This is like loosen_values except
 * it only modifies TArrLikes.
 */
Type loosen_array_values(Type t);

/*
 * Drop any knowledge about known string values. The type is unchanged
 * otherwise.
 */
Type loosen_string_values(Type t);

/*
 * Discard any emptiness information about the type. Force any type which
 * contains only empty or non-empty variants to contain both. Doesn't change the
 * type otherwise.
 */
Type loosen_emptiness(Type t);

/*
 * Force any type which includes TCls or TLazyCls to also include
 * TSStr.
 */
Type loosen_likeness(Type t);

/*
 * Like loosen_likeness, but operates recursively on any specialized
 * data present. This is usually not what you want.
 */
Type loosen_likeness_recursively(Type t);

/*
 * Loosens staticness, emptiness, and values from the type. This forces a type
 * to its most basic form (except for object class information).
 */
Type loosen_all(Type t);

/*
 * Like loosen_all, but also drops all object/class information. This
 * puts a type into it's most basic form.
 */
Type loosen_to_datatype(Type t);

/*
 * If t contains TUninit, returns the best type we can that contains
 * at least everything t contains, but doesn't contain TUninit.  Note
 * that this function will return TBottom for TUninit.
 */
Type remove_uninit(Type t);

/*
 * If t contains TUninit, return union_of(remove_uninit(t), TInitNull).
 */
Type to_cell(Type t);

/*
 * Add non-empty variants of the type to the type if not already
 * present. Doesn't change the type otherwise.
 */
Type add_nonemptiness(Type t);

/*
 * Produced the most refined type possible, given that
 * t passed/failed an emptiness check.
 */
Type assert_emptiness(Type);
Type assert_nonemptiness(Type);

/*
 * If t is definitely an array with a known size, return
 * it. std::nullopt otherwise.
 */
Optional<int64_t> arr_size(const Type&);

/*
 *
 * Returns the best known type of an array inner element given a type
 * for the key, along with whether the value will always be
 * present. The returned type is always a subtype of TInitCell.
 *
 * The returned type will be TBottom if there's guaranteed to be no
 * value for that key.
 *
 * Pre: first arg could be TArrLike, the second arg is a subtype of
 * TArrKey.
 *
 * Note: the array parameter merely "could be" TArrLike. Any
 * non-TArrLike bits in the type are ignored.
 */
std::pair<Type,bool> array_like_elem(const Type& arr, const Type& key);

/*
 * Perform an array-like set on types. Returns a type that represents
 * the effects of arr[key] = val and whether the set can possibly
 * throw.
 *
 * The returned type will not contain the TArrLike bits if the
 * operation will always throw (if you pass it in subtypes of
 * TArrLike, this means TBottom).
 *
 * Pre: first arg could be TArrLike, the second arg is a subtype of
 * TArrKey.
 *
 * Note: the array parameter merely "could be" TArrLike. Any
 * non-TArrLike bits in the type are ignored for the sake of modeling
 * the set. The returned type includes the non-TArrLike bits copied
 * into them.
 */
std::pair<Type,bool> array_like_set(Type arr,
                                    const Type& key,
                                    const Type& val);

/*
 * Refine a non-empty Vec or Dict after every existing position's value has
 * been overwritten. Preserve the base's keys, shape, and legacy mark.
 */
Type array_like_set_values(Type base, Type key, Type value);

/*
 * Perform a newelem operation on an array-like type.  Returns an
 * array that contains a new pushed-back element with the supplied
 * value, in the sense of arr[] = val, and whether the operation might
 * throw. If the operation always throws, the new base will not
 * contain the TArrLike bits (if you pass in a subtype of TArrLike,
 * this means TBottom).
 *
 * Pre: first arg could be TArrLike, second arg is a subtype of
 * TInitCell.
 *
 * Note: the array parameter merely "could be" TArrLike. Any
 * non-TArrLike bits in the type are ignored for the sake of modeling
 * the newelem. The returned type includes the non-TArrLike bits
 * copied into them.
 */
std::pair<Type,bool> array_like_newelem(Type arr, const Type& val);

/*
 * Check if an array_like_set on the given array with the given key
 * and "undo-able". That is, given just the post-set array and the
 * key, it's possible to reconstruct the array before the
 * mutation. True will be returned if it definitely can be, false
 * otherwise.
 *
 * Right now this is used to track chains of AddElemC without keeping
 * a copy of each intermediate state.
 */
bool array_like_set_can_be_undone(const Type& arr, const Type& key);

/*
 * Given an array which is the product of array_like_set with the
 * given key, produce the array as it was *before* the
 * array_like_set. This is only if array_like_set returned true on the
 * pre-set version of the array (with the same key).
 */
Type undo_array_like_set(Type arr, const Type& key);

/*
 * Return the best known information for iteration of the supplied type. This is
 * only intended for non-mutable iteration, so the returned types are at worst
 * InitCell.
 */
struct IterTypes {
  Type key;
  Type value;
  // The number of elements we're iterating over:
  enum struct Count {
    Empty,     // No elements
    Single,    // Exactly one element
    ZeroOrOne, // Less than 2 elements
    NonEmpty,  // Unknown upper bound, but non-empty
    Any        // Nothing known
  };
  Count count;
  // Can a IterInit op throw on this iterator?
  bool mayThrowOnInit;
  // Can a IterGetKey, IterGetValue or IterNext op throw on this iterator?
  // Can only happen for object types.
  bool mayThrowOnGetOrNext;
};
IterTypes iter_types(const Type&);

/*
 * Create a RepoAuthType for a Type.
 */
RepoAuthType make_repo_type(const Type& t);

/*
 * Returns true iff an IsType testing for testTy/testOp on valTy might raise.
 */
bool is_type_might_raise(const Type& testTy, const Type& valTy);
bool is_type_might_raise(IsTypeOp testOp, const Type& valTy);

/*
 * Returns true iff a compare of two types might raise a HAC notice
 */
bool compare_might_raise(const Type& t1, const Type& t2);

/*
 * Model potential promotion of TClsLike into a key for array
 * access. Class-likes will promote into a static string (of the class
 * name). Returns the best known post-promotion type, and whether the
 * promotion happens with potential throwing.
 */
std::pair<Type, Promotion> promote_classlike_to_key(Type);

/*
 * Returns true if the Type *might* contain a DCls within it. This is
 * conservative. If it returns false, the Type definitely does not. If
 * it returnes true, the Type still might not contain one.
 */
bool might_have_dcls(const Type&);

//////////////////////////////////////////////////////////////////////

/*
 * Calls res::Class::unserialize on any res::Class contained within
 * the given Type. It also puts the Type into a "canonical" form. A
 * Type that comes from BlobDecoder may not be in a canonical form
 * (since the extern-worker job which produced it didn't have complete
 * class information). This makes the Type canonical according to the
 * class information known in this job (if not in a job it should be
 * complete).
 */
Type unserialize_classes(const IIndex&, Type);

/*
 * Calls res::Class::serialize on any res::Class contains within the
 * given type. This is necessary for any Type which will be sent to
 * BlobEncoder, as only serialized classes can be encoded.
 */
Type serialize_classes(Type);

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

}
