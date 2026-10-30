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

#include <memory>
#include <tuple>
#include <vector>
#include <map>
#include <exception>

#include <variant>
#include <tbb/concurrent_hash_map.h>

#include <folly/Hash.h>

#include "hphp/util/compact-vector.h"
#include "hphp/util/either.h"
#include "hphp/util/tiny-vector.h"
#include "hphp/util/tribool.h"

#include "hphp/runtime/base/repo-auth-type.h"

#include "hphp/runtime/vm/coeffects.h"
#include "hphp/runtime/vm/type-constraint.h"

#include "hphp/hhbbc/hhbbc.h"
#include "hphp/hhbbc/misc.h"
#include "hphp/hhbbc/type-structure.h"
#include "hphp/hhbbc/type-system.h"

namespace HPHP::HHBBC {

//////////////////////////////////////////////////////////////////////

struct Index;
struct AnalysisIndex;
struct PublicSPropMutations;
struct FuncAnalysisResult;
struct ClassAnalysis;
struct UnitAnalysis;
struct Context;
struct ContextHash;
struct CallContext;
struct PropertiesInfo;
struct MethodsInfo;

namespace php {
struct Class;
struct ClassBytecode;
struct Prop;
struct Const;
struct Func;
struct FuncBytecode;
struct Unit;
struct Program;
struct TypeAlias;
}

//////////////////////////////////////////////////////////////////////

/*
 * This module contains functions for building and querying an Index
 * of data relating to "resolved" versions of the names in of a
 * php::Program.  It also records dependencies so it is possible to
 * tell which parts of the program may be interested in new inferred
 * information about other parts of the program.
 *
 * The main entry point here is the Index class.  The Index is built
 * after parse time, and then analysis can query it for information.
 */

//////////////////////////////////////////////////////////////////////

enum class Dep : uintptr_t {
  /* This dependency should trigger when the return type changes */
  ReturnTy = (1u << 0),
  /* This dependency should trigger when a DefCns is resolved */
  ConstVal = (1u << 1),
  /* This dependency should trigger when a class constant is resolved */
  ClsConst = (1u << 2),
  /* This dependency should trigger when the bad initial prop value bit for a
   * class changes */
  PropBadInitialValues = (1u << 3),
  /* This dependency should trigger when a public static property changes */
  PublicSProp = (1u << 4),
  /* This dependency means that we refused to do inline analysis on
   * this function due to inline analysis depth. The dependency will
   * trigger if the target function becomes effect-free, or gets a
   * literal return value.
   */
  InlineDepthLimit = (1u << 5),
};

/*
 * A DependencyContext encodes enough of the context to record a dependency - a
 * php::Func, if we're doing private property analysis and its a suitable class,
 * a php::Class, or a public static property.
 */

enum class DependencyContextType : uint16_t {
  Func,
  Class,
  Prop,
  FuncFamily
};

using DependencyContext = CompactTaggedPtr<const void, DependencyContextType>;

struct DependencyContextEquals {
  bool operator()(const DependencyContext& a,
                  const DependencyContext& b) const {
    return a.getOpaque() == b.getOpaque();
  }
};

struct DependencyContextHash {
  size_t operator()(const DependencyContext& d) const {
    return pointer_hash<void>{}(reinterpret_cast<void*>(d.getOpaque()));
  }
};

struct DependencyContextHashCompare : DependencyContextHash {
  bool equal(const DependencyContext& a, const DependencyContext& b) const {
    return a.getOpaque() == b.getOpaque();
  }
  size_t hash(const DependencyContext& d) const { return (*this)(d); }
};

using DependencyContextSet = hphp_fast_set<DependencyContext,
                                           DependencyContextHash,
                                           DependencyContextEquals>;

std::string show(Context);

/*
 * State of properties on a class.  Map from property name to its
 * Type.
 */
struct PropStateElem {
  Type ty;
  const TypeIntersectionConstraint* tc = nullptr;
  Attr attrs;
  bool everModified;

  bool operator==(const PropStateElem& o) const {
    return
      equal(ty, o.ty) &&
      tc == o.tc &&
      attrs == o.attrs &&
      everModified == o.everModified;
  }
  template <typename SerDe> void serde(SerDe& sd) {
    sd(ty)
      (attrs)
      (everModified)
      ;
  }
};
using PropState = std::map<LSString, PropStateElem, string_data_lt>;

/*
 * The result of Index::lookup_static
 */
struct PropLookupResult {
  Type ty; // The best known type of the property (TBottom if not found)
  SString name; // The statically known name of the string, if any
  TriBool found; // If the property was found
  TriBool isConst; // If the property is AttrConst
  TriBool readOnly; // If the property is AttrIsReadonly
  TriBool lateInit; // If the property is AttrLateInit
  TriBool internal; // If the property is Internal
  bool classInitMightRaise; // If class initialization during the
                            // property access can raise (unlike the
                            // others, this is only no or maybe).
};

inline PropLookupResult& operator|=(PropLookupResult& a,
                                    const PropLookupResult& b) {
  assertx(a.name == b.name);
  a.ty |= b.ty;
  a.found |= b.found;
  a.isConst |= b.isConst;
  a.readOnly |= b.readOnly;
  a.lateInit |= b.lateInit;
  a.internal |= b.internal;
  a.classInitMightRaise |= b.classInitMightRaise;
  return a;
}

inline PropLookupResult& operator&=(PropLookupResult& a,
                                    const PropLookupResult& b) {
  assertx(a.name == b.name);
  a.ty &= b.ty;
  a.found &= b.found;
  a.isConst &= b.isConst;
  a.readOnly &= b.readOnly;
  a.lateInit &= b.lateInit;
  a.internal &= b.internal;
  a.classInitMightRaise &= b.classInitMightRaise;
  return a;
}

std::string show(const PropLookupResult&);

/*
 * The result of Index::merge_static_type
 */
struct PropMergeResult {
  Type adjusted; // The merged type, potentially adjusted according to
                 // the prop's type-constraint (it's the subtype of
                 // the merged type that would succeed).
  TriBool throws; // Whether the mutation this merge represents
                  // can throw.
};

inline PropMergeResult& operator|=(PropMergeResult& a,
                                   const PropMergeResult& b) {
  a.adjusted |= b.adjusted;
  a.throws |= b.throws;
  return a;
}

inline PropMergeResult& operator&=(PropMergeResult& a,
                                   const PropMergeResult& b) {
  a.adjusted &= b.adjusted;
  a.throws &= b.throws;
  return a;
}

std::string show(const PropMergeResult&);

/*
 * The result of Index::lookup_class_constant
 */
struct ClsConstLookupResult {
  Type ty;            // The best known type of the constant (might not be a
                      // scalar).
  TriBool found;      // If the constant was found
  bool mightThrow;    // If accessing the constant can throw

  template <typename SerDe> void serde(SerDe& sd) {
    sd(ty)(found)(mightThrow);
  }
};

inline ClsConstLookupResult& operator|=(ClsConstLookupResult& a,
                                        const ClsConstLookupResult& b) {
  a.ty |= b.ty;
  a.found |= b.found;
  a.mightThrow |= b.mightThrow;
  return a;
}

inline ClsConstLookupResult& operator&=(ClsConstLookupResult& a,
                                        const ClsConstLookupResult& b) {
  a.ty &= b.ty;
  a.found &= b.found;
  a.mightThrow &= b.mightThrow;
  return a;
}

std::string show(const ClsConstLookupResult&);

/*
 * The result of Index::lookup_class_type_constant
 */
struct ClsTypeConstLookupResult {
  TypeStructureResolution resolution; // The result from resolving
                                      // the type-structure
  TriBool found;    // If the constant was found
  TriBool abstract; // If the constant was abstract (this only applies
                    // to the subset which wasn't found).
};

inline ClsTypeConstLookupResult& operator|=(
    ClsTypeConstLookupResult& a,
    const ClsTypeConstLookupResult& b) {
  a.resolution |= b.resolution;
  if (a.found == TriBool::Yes) {
    a.abstract = b.abstract;
  } else if (b.found != TriBool::Yes) {
    a.abstract |= b.abstract;
  }
  a.found |= b.found;
  return a;
}

inline ClsTypeConstLookupResult& operator&=(
    ClsTypeConstLookupResult& a,
    const ClsTypeConstLookupResult& b) {
  a.resolution &= b.resolution;
  if (b.found == TriBool::Yes) {
    a.abstract = b.abstract;
  } else if (a.found != TriBool::Yes) {
    a.abstract &= b.abstract;
  }
  a.found &= b.found;
  return a;
}

std::string show(const ClsTypeConstLookupResult&);

//////////////////////////////////////////////////////////////////////

/*
 * Represents a class constant, pointing to where the constant was
 * originally declared (the class name and it's position in the
 * class' constant table).
 */
struct ConstIndex {
  using Idx = uint32_t;

  ConstIndex() = default;
  ConstIndex(SString cls, Idx idx)
    : cls{cls}, idx{idx} {}

  SString cls;
  Idx idx;

  bool operator==(const ConstIndex& o) const {
    return idx == o.idx && cls->tsame(o.cls);
  }
  bool operator<(const ConstIndex& o) const {
    if (idx != o.idx) return idx < o.idx;
    return string_data_lt_type{}(cls, o.cls);
  }

  struct Hasher {
    size_t operator()(const ConstIndex& idx) const {
      return folly::hash::hash_combine(idx.cls->hash(), idx.idx);
    }
  };

  template <typename SerDe> void serde(SerDe& sd) {
    sd(cls)(idx);
  }
};

std::string show(const ConstIndex&);
std::string show(const ConstIndex&, const IIndex&);

//////////////////////////////////////////////////////////////////////

// Inferred class constant type from a 86cinit.
struct ClsConstInfo {
  Type type;
  size_t refinements = 0;
  template <typename SerDe> void serde(SerDe& sd) {
    sd(type)(refinements);
  }
};

using ResolvedConstants =
  CompactVector<std::pair<ConstIndex::Idx, ClsConstInfo>>;

//////////////////////////////////////////////////////////////////////

struct ResolvedClsTypeConst {
  SArray resolved;
  bool contextInsensitive;
  ConstIndex from;
  php::Const::Invariance invariance;
};

using ResolvedClsTypeConsts = CompactVector<ResolvedClsTypeConst>;

//////////////////////////////////////////////////////////////////////

struct ResolvedTypeAlias {
  size_t idx;
  SArray resolved;
};

using ResolvedTypeAliases = CompactVector<ResolvedTypeAlias>;

//////////////////////////////////////////////////////////////////////

/*
 * Represents a method, without requiring an explicit pointer to a
 * php::Func (so can be used across remote workers).
 */
struct MethRef {
  MethRef() = default;
  explicit MethRef(const php::Func& f)
    : cls{f.cls->name}, idx{f.clsIdx} {}
  MethRef(SString cls, uint32_t idx)
    : cls{cls}, idx{idx} {}

  SString cls{nullptr};
  // Index in the class' methods table.
  uint32_t idx{std::numeric_limits<uint32_t>::max()};

  bool operator==(const MethRef& o) const {
    return cls->tsame(o.cls) && idx == o.idx;
  }
  bool operator!=(const MethRef& o) const {
    return !(*this == o);
  }
  bool operator<(const MethRef& o) const {
    // The ordering for MethRef is arbitrary. Compare by idx and then
    // by the class name's hash to avoid having to do the more
    // expensive string comparison.
    if (idx != o.idx) return idx < o.idx;
    auto const hash1 = cls->hash();
    auto const hash2 = o.cls->hash();
    if (hash1 != hash2) return hash1 < hash2;
    return string_data_lt_type{}(cls, o.cls);
  }

  struct Hash {
    size_t operator()(const MethRef& m) const {
      return folly::hash::hash_combine(m.cls->hash(), m.idx);
    }
  };

  template <typename SerDe> void serde(SerDe& sd) {
    sd(cls)(idx);
  }
};

using MethRefSet = hphp_fast_set<MethRef, MethRef::Hash>;

std::string show(const MethRef&);

//////////////////////////////////////////////////////////////////////

struct PropInitInfo {
  TypedValue val;
  bool satisfies;
  bool deepInit;
};
using ResolvedPropInits = CompactVector<std::pair<size_t, PropInitInfo>>;

//////////////////////////////////////////////////////////////////////

// private types
struct ClassBundle;
struct ClassGraph;
struct ClassInfo;
struct ClassInfo2;
struct FuncInfo2;
struct FuncFamily2;
struct MethodsWithoutCInfo;

struct ClassBundle;

//////////////////////////////////////////////////////////////////////

// Opaque input to an AnalysisIndex. Avoids exposing AnalysisIndex
// internals to those who need to convey the data into a remote
// worker.
template <typename T> struct AnalysisIndexParam {
  AnalysisIndexParam() = default;

  void serde(BlobEncoder&) const;
  void serde(BlobDecoder&);
private:
  struct Deleter { void operator()(T*) const; };
  std::unique_ptr<T, Deleter> ptr;

  friend struct ::HPHP::HHBBC::AnalysisIndex;
};

using AnalysisIndexBundle = AnalysisIndexParam<ClassBundle>;

//////////////////////////////////////////////////////////////////////

/*
 * References to "resolved" entities with information in the index are
 * in the res:: namespace.
 *
 * These represent handles to program entities that may have variable
 * amounts of information.  For example, we may know the name of a
 * class in a res::Class, but do not know for sure which php::Class
 * struct is actually associated with it.
 */
namespace res {

/*
 * A resolved runtime Class, for a particular php::Class.
 *
 * Provides various lookup tables that allow querying the Class'
 * information.
 */
struct Class {
  /*
   * Returns whether two (exact) classes are definitely same at
   * runtime. This ignores all complications due to non-regular
   * classes and is usually not what you want to use.
   */
  bool same(const Class& o) const;

  /*
   * Returns true if this class is a subtype of 'o'. That is, every
   * subclass of 'this' (including 'this' itself) is a subclass of
   * 'o'. For the exact variant, 'this' is considered to be exact
   * (that is, exactly that class). For the sub variant, 'this' could
   * also be a subclass. This distinction only matters for non-regular
   * classes. 'o' is considered to be a subclass except for
   * exactSubtypeOfExact. This is needed since two exact classes, even
   * if the same, may not be a sub-type of one another if
   * nonRegularL/nonRegularR differ.
   *
   * Classes can have implementations/subclasses which aren't
   * "regular" classes (interfaces/traits/abstract/etc). Whether these
   * will be considered as part of the check (on either side) is
   * context dependent and specified by nonRegularL and nonRegularR.
   */
  bool exactSubtypeOf(const Class& o, bool nonRegularL, bool nonRegularR) const;
  bool exactSubtypeOfExact(const Class& o,
                           bool nonRegularL,
                           bool nonRegularR) const;
  bool subSubtypeOf(const Class& o, bool nonRegularL, bool nonRegularR) const;

  /*
   * Returns false if this class has no subclasses in common with
   * 'o'. This is equivalent to saying that their intersection is
   * empty. For the exact variant, 'this' is considered to be exactly
   * that class (no sub-classes). For the sub variant, 'this' might be
   * that class, or any subclass of that class.
   *
   * Since couldBe is symmetric, this covers all of the cases. 'o' is
   * considered to be a subclass, except for exactCouldBeExact. This
   * is needed since two exact classes, even if the same, may not have
   * anything in common if nonRegularL/nonRegularR differ.
   *
   * Classes can have implementations/subclasses which aren't
   * "regular" classes (interfaces/traits/abstract/etc). Whether these
   * will be considered as part of the check (on either side) is
   * context dependent and specified by nonRegularL and nonRegularR.
   */
  bool exactCouldBe(const Class& o, bool nonRegularL, bool nonRegularR) const;
  bool exactCouldBeExact(const Class& o,
                         bool nonRegularL,
                         bool nonRegularR) const;
  bool subCouldBe(const Class& o, bool nonRegularL, bool nonRegularR) const;

  /*
   * Returns the name of this class.  Non-null guarantee.
   */
  SString name() const;

  /*
   * Returns whether this class is a final class as determined by
   * static analysis. The first variant considers all classes, while
   * the second variant only considers potentially overriding regular
   * classes (this distinction is important if you already have an
   * instance of the class, as abstract classes cannot be
   * instantiated).
   *
   * NB: Traits never can be overridden and will always return false
   * (their subclass list reflects users which the trait will be
   * flattened into at runtime).
   *
   * When returning false the class is guaranteed to be final. When
   * returning true the system cannot tell though the class may still
   * be final.
   */
  bool couldBeOverridden() const;
  bool couldBeOverriddenByRegular() const;

  /*
   * Returns whether this class might be a regular/non-regular class
   * at runtime. For resolved classes this check is precise, but for
   * unresolved classes this will always conservatively return
   * true. This only checks the class itself and says nothing about
   * any potential subclasses of this.
   */
  bool mightBeRegular() const;
  bool mightBeNonRegular() const;

  /*
   * Whether this class (or its subtypes) might be a non-regular
   * class. For resolved classes this check is precise, but for
   * unresolved classes this will always conservatively return true.
   */
  bool mightContainNonRegular() const;

  /*
   * Return the best class equivalent to this, but with any
   * non-regular classes removed. By equivalent, we mean the most
   * specific representable class which contains all of the same
   * regular subclasses that the original did. If std::nullopt is
   * returned, this class contains no regular classes (and therefore
   * the result is a Bottom). The returned classes may be the original
   * class, and for interfaces and abstract classes, the result may
   * not be an interface or abstract class.
   */
  Optional<res::Class> withoutNonRegular() const;

  /*
   * Whether this class (or its subtypes) could possibly have have
   * a magic toBoolean() method.
   */
  bool couldHaveMagicBool() const;

  /*
   * Whether this class could possibly have a sub-class that is
   * mocked, including itself.
   */
  bool couldHaveMockedSubClass() const;

  /*
   * Whether this class could possibly be mocked.
   */
  bool couldBeMocked() const;

  /*
   * Whether this class could have reified generics
   */
  bool couldHaveReifiedGenerics() const;

  /*
   * Whether this class must have reified generics
   */
  bool mustHaveReifiedGenerics() const;

  /*
   * Whether this class could have a reified parent
   */
  bool couldHaveReifiedParent() const;

  /*
   * Whether this class must have a reified parent
   */
  bool mustHaveReifiedParent() const;

  /*
   * Returns whether this resolved class might distinguish being constructed
   * dynamically versus being constructed normally (IE, might raise a notice).
   */
  bool mightCareAboutDynConstructs() const;

  /*
   * Returns whether this class will raise a notice if it is loaded with
   * HH\class_to_classname;
   */
  bool mightCareAboutDynamicallyReferenced() const;

  /*
   * Returns whether this resolved class might belong to a strict package.
   */
  bool mightBeInStrictPackage() const;

  /*
   * Whether this class (or clases derived from it) could have const props.
   */
  bool couldHaveConstProp() const;
  bool subCouldHaveConstProp() const;

  /*
   * Returns the res::Class for this Class's parent if there is one,
   * or std::nullopt.
   */
  Optional<Class> parent() const;

  /*
   * Returns the php::Class for this Class if there is one, or
   * nullptr.
   */
  const php::Class* cls() const;

  /*
   * Returns true if this class has it's full child class information.
   */
  bool hasCompleteChildren() const;
  /*
   * A class is complete if it has full child class information, or if
   * it's a "conservative" class (a class which has too many child
   * classes to track completely).
   */
  bool isComplete() const;

  /*
   * Classes that come out of the BlobDecoder start out as
   * "serialized". This means it just wraps a string. Almost nothing
   * can be done on a serialized class, except call unserialize on
   * it. Unserialize will produce the "real" class, recording
   * dependencies as necessary. If unserialize returns std::nullopt
   * then the class definitely doesn't exist.
   *
   * Likewise, when using BlobEncoder, you must manually serialize the
   * class beforehand.
   */
  bool isSerialized() const;
  Optional<Class> unserialize(const IIndex&) const;
  Class serialize() const;

  using ClassVec = TinyVector<Class, 4>;

  /*
   * Invoke the given function on every possible subclass of this
   * class (including itself), providing the name and the Attr bits of
   * the subclass. Only the Attr bits corresponding to the type of
   * class are provided (AttrEnum/AttrTrait/AttrInterface). Returns
   * false and doesn't call the function if this class does not know
   * it's complete children, true otherwise.
   */
  bool forEachSubclass(const std::function<void(SString, Attr)>&) const;

  /*
   * Given two lists of classes, calculate the union between them (in
   * canonical form). A list of size 1 represents a single class, and
   * a larger list represents an intersection of classes. The input
   * lists are assumed to be in canonical form. If the output is an
   * empty list, the union is *all* classes (corresponding to TObj or
   * TCls). This function is really an implementation detail of
   * union_of() and not a general purpose interface.
   */
  static ClassVec combine(folly::Range<const Class*> classes1,
                          folly::Range<const Class*> classes2,
                          bool isSub1,
                          bool isSub2,
                          bool nonRegular1,
                          bool nonRegular2);
  /*
   * Given two lists of classes, calculate the intersection between
   * them (in canonical form). A list of size 1 represents a single
   * class, and a larger list represents an intersection of
   * classes. The input lists are assumed to be in canonical form. If
   * the output is an empty list, the intersection is empty
   * (equivalent to Bottom). This function is really an implementation
   * detail of intersection_of() and not a general purpose interface.
   */
  static ClassVec intersect(folly::Range<const Class*> classes1,
                            folly::Range<const Class*> classes2,
                            bool nonRegular1,
                            bool nonRegular2,
                            bool& nonRegularOut);

  /*
   * Given a list of classes, return a new list of classes
   * representing all of the non-regular classes removed (and
   * canonicalized). If the output list is empty, there are no regular
   * classes.
   */
  static ClassVec removeNonRegular(folly::Range<const Class*> classes);

  /*
   * Given two lists of classes, calculate whether their intersection
   * is non-empty. This is equivalent to calling intersect and
   * checking the result, but more efficient.
   */
  static bool couldBeIsect(folly::Range<const Class*> classes1,
                           folly::Range<const Class*> classes2,
                           bool nonRegular1,
                           bool nonRegular2);

  /*
   * Convert this class to/from an opaque integer. The integer is
   * "pointerish" (has upper bits cleared), so can be used in
   * something like CompactTaggedPtr. It is not, however, guaranteed
   * to be aligned (lower bits may be set).
   */
  uintptr_t toOpaque() const { return opaque.toOpaque(); }
  static Class fromOpaque(uintptr_t o) { return Class{E::fromOpaque(o)}; }

  size_t hash() const { return toOpaque(); }

  /*
   * Obtain with a given name or associated with the given ClassInfo
   * (must already exist).
   */
  static Class get(SString);
  static Class get(const ClassInfo&);
  static Class get(const ClassInfo2&);

  /*
   * Obtain with a given name. If does not already exist, create an
   * unresolved Class and return that.
   */
  static Class getOrCreate(SString);

  /*
   * Make an unresolved Class representing the given name. The name
   * cannot be an existing class.
   */
  static Class getUnresolved(SString);

  void serde(BlobEncoder&) const;
  static Class makeForSerde(BlobDecoder&);

  void makeConservativeForTest();
#ifndef NDEBUG
  bool isMissingDebug() const;
#endif

  ClassGraph graph() const;
private:
  ClassInfo* cinfo() const;
  ClassInfo2* cinfo2() const;

  template <typename F>
  static void visitEverySub(folly::Range<const Class*>, bool, const F&);

  friend std::string show(const Class&);

  friend struct ::HPHP::HHBBC::Index;
  friend struct ::HPHP::HHBBC::AnalysisIndex;

  using E = Either<void*, const StringData*>;
  E opaque;

  explicit Class(ClassGraph);
  explicit Class(E o): opaque{o} {}
};

/*
 * This is an abstraction layer to represent possible runtime function
 * resolutions.
 *
 * Internally, this may only know the name of the function (or method), or we
 * may know exactly which source-code-level function it refers to, or we may
 * only have ruled it down to one of a few functions in a class hierarchy.  The
 * interpreter can treat all these cases the same way using this.
 */
struct Func {
  /*
   * Returns the (fully qualified) name of this function.
   */
  std::string name() const;

  /*
   * If this resolved function represents exactly one php::Func, return it.
   */
  const php::Func* exactFunc() const;

  /*
   * Whether this function definitely exists or definitely does not
   * exist.
   */
  TriBool exists() const;

  /*
   * Returns whether this resolved function is definitely safe to constant fold.
   */
  bool isFoldable() const;

  /*
   * Whether this function could have reified generics
   */
  bool couldHaveReifiedGenerics() const;

  /*
   * Returns whether this resolved function might distinguish being called
   * dynamically versus being called normally (IE, might raise a notice).
   */
  bool mightCareAboutDynCalls() const;

  /*
   * Returns whether this resolved function might be a builtin.
   */
  bool mightBeBuiltin() const;

  /*
   * Minimum/maximum bound on the number of non-variadic parameters of the
   * function.
   */
  uint32_t minNonVariadicParams() const;
  uint32_t maxNonVariadicParams() const;

  /* Whether this function may have named params. */
  bool mightHaveNamedParams() const;

  /*
   * Return if the function supports async eager return.
   */
  TriBool supportsAsyncEagerReturn() const;

  /*
   * Returns the number of inout parameters expected by func (if known).
   */
  Optional<uint32_t> lookupNumInoutParams() const;

  /*
   * Returns the parameter preparation kind (if known) for parameter
   * `paramId' on this function.
   */
  PrepKind lookupParamPrep(uint32_t paramId) const;

  /*
   * Returns whether the function's return value is readonly
   */
  TriBool lookupReturnReadonly() const;

  /*
   * Returns whether the function is marked as readonly
   */
  TriBool lookupReadonlyThis() const;

  /*
   * Returns the wrapped HHVM builtin iff this is a trivial HHVM builtin wrapper
   */
  Optional<SString> triviallyWrappedFunc() const;

  /*
   * Coeffects
   */
  const RuntimeCoeffects* requiredCoeffects() const;
  // Returns nullptr if we cant tell whether there are coeffect rules
  const CompactVector<CoeffectRule>* coeffectRules() const;

  struct FuncInfo;
  struct FuncFamily;

private:
  friend struct ::HPHP::HHBBC::Index;
  friend struct ::HPHP::HHBBC::AnalysisIndex;

  struct FuncName {
    SString name;
  };
  struct MethodName {
    SString cls;
    SString name;
  };
  struct Fun {
    const FuncInfo* finfo;
  };
  struct Fun2 {
    const FuncInfo2* finfo;
  };
  struct Method {
    const FuncInfo* finfo;
  };
  struct Method2 {
    const FuncInfo2* finfo;
  };
  // Like Method, but the method is not guaranteed to actually exist
  // (this only matters for things like exactFunc()).
  struct MethodOrMissing {
    const FuncInfo* finfo;
  };
  struct MethodOrMissing2 {
    const FuncInfo2* finfo;
  };
  // Method/Func is known to not exist
  struct MissingFunc {
    SString name;
  };
  struct MissingMethod {
    SString cls;
    SString name;
  };
  // Group of methods (a wrapper around a FuncFamily).
  struct MethodFamily {
    FuncFamily* family;
    bool regularOnly;
  };
  struct MethodFamily2 {
    const FuncFamily2* family;
    bool regularOnly;
  };
  // Simultaneously a group of func families. Any data must be
  // intersected across all of the func families in the list. Used for
  // method resolution on a DCls where isIsect() is true.
  struct Isect {
    CompactVector<FuncFamily*> families;
    bool regularOnly{false};
  };
  struct Isect2 {
    CompactVector<const FuncFamily2*> families;
    bool regularOnly{false};
  };
  using Rep = std::variant< FuncName
                          , MethodName
                          , Fun
                          , Fun2
                          , Method
                          , Method2
                          , MethodFamily
                          , MethodFamily2
                          , MethodOrMissing
                          , MethodOrMissing2
                          , MissingFunc
                          , MissingMethod
                          , Isect
                          , Isect2
                          >;

private:
  explicit Func(Rep);
  friend std::string show(const Func&);

private:
  Rep val;
};

/*
 * Produce a trace-able string for a res::Func or res::Class.
 */
std::string show(const Func&);
std::string show(const Class&);

}

//////////////////////////////////////////////////////////////////////

/*
 * A type which is an alias to another. This includes standard
 * type-aliases, but also enums (which are aliases of their underlying
 * base type). Type mappings can alias to another type mapping.
 */
struct TypeMapping {
  LSString name;
  TypeConstraint value;
  bool isTypeAlias;
  bool isEnum;
  LSString unit;
  SArray typeStructure;

  bool operator==(const TypeMapping& o) const {
    return name->tsame(o.name);
  }
  bool operator<(const TypeMapping& o) const {
    return string_data_lt_type{}(name, o.name);
  }

  template <typename SerDe> void serde(SerDe& sd) {
    sd(name)(value)(isTypeAlias)(isEnum)(unit)(typeStructure);
  }
};

//////////////////////////////////////////////////////////////////////

struct LocalFlattenOutput {
  TSStringToOneT<SStringSet> missingMethods;

  template <typename SerDe> void serde(SerDe& sd) {
    sd(missingMethods, string_data_lt_type{}, string_data_lt{});
  }
};

//////////////////////////////////////////////////////////////////////

/*
 * This class encapsulates the known facts about the program, with a
 * whole-program view.
 *
 * This structure contains unowned pointers into the php::Program it
 * was created for.  It should not out-live the Program.
 *
 * The const member functions of this class are thread safe for
 * concurrent reads and writes.  The non-const functions should be
 * called in a single threaded context only (they are used during the
 * "update" step in between whole program analysis rounds).
 */
struct Index {
  // The input used to build the Index is largely the extern-worker
  // refs representing the program components. However, some
  // additional metadata is needed locally to know what the refs
  // represent (and schedule some initial jobs).
  struct Input {
    template <typename T> using R = extern_worker::Ref<std::unique_ptr<T>>;

    struct ClassMeta {
      R<php::Class> cls;
      LSString name;
      std::vector<SString> dependencies;
      // If this class is a closure declared in a top-level func, this
      // is the name of that func.
      LSString closureFunc;
      std::vector<SString> closures;
      LSString unit;
      // If this class is an enum, the type-mapping representing it's
      // base type.
      Optional<TypeMapping> typeMapping;
      std::vector<SString> unresolvedTypes;
    };

    struct FuncMeta {
      R<php::Func> func;
      LSString name;
      LSString unit;
      bool methCaller;
      std::vector<SString> unresolvedTypes;
    };

    struct UnitMeta {
      R<php::Unit> unit;
      LSString name;
      std::vector<TypeMapping> typeMappings;
      std::vector<std::pair<SString, bool>> constants;
      SStringSet cinitPredeps;
      SStringSet predeps;
    };

    struct FuncBytecodeMeta {
      R<php::FuncBytecode> bc;
      LSString name;
      LSString unit;
      bool methCaller;
    };

    struct ClassBytecodeMeta {
      R<php::ClassBytecode> bc;
      LSString name;
    };

    static std::vector<SString> makeDeps(const php::Class&);

    std::vector<ClassMeta> classes;
    std::vector<UnitMeta> units;
    std::vector<FuncMeta> funcs;
    std::vector<ClassBytecodeMeta> classBC;
    std::vector<FuncBytecodeMeta> funcBC;
  };

  /*
   * Create an Index from extern-worker refs. Performs some initial analysis
   * of the program.
   */
  Index(Input,
        Config,
        std::unique_ptr<TicketExecutor>,
        std::unique_ptr<extern_worker::Client>,
        DisposeCallback,
        StructuredLogEntry*);

  /* Create an Index which owns an already local php::Program. */
  Index(php::Program, StructuredLogEntry*);
  Index(php::Program, StructuredLogEntry*, LocalFlattenOutput*);
  ~Index();

  Index(const Index&) = delete;
  Index(Index&&);
  Index& operator=(const Index&) = delete;
  Index& operator=(Index&&);

  /*
   * The index operates in two modes: frozen, and unfrozen.
   *
   * Conceptually, the index is mutable and may acquire new
   * information until it has been frozen, and once frozen, it retains
   * the information it had at the point it was frozen.
   *
   * The reason this exists is because certain functions on the index
   * may cause it to need to consult information in the bodies of
   * functions other than the Context passed in.  Specifically, if the
   * interpreter tries to look up the return type for a callee in a
   * given CallContext, the index may choose to recursively invoke
   * type inference on that callee's function body to see if more
   * precise information can be determined, unless it is frozen.
   *
   * This is fine until the final pass, because all bytecode is
   * read-only at that stage.  However, in the final pass, other
   * threads might be optimizing a callee's bytecode and changing it,
   * so we should not be reading from it to perform type inference
   * concurrently.  Freezing the index tells it it can't do that
   * anymore.
   *
   * These are the functions to query and transition to frozen state.
   */
  bool frozen() const;
  void freeze();
  void thaw();

  /*
   * Throw away data structures that won't be needed during or after
   * the final pass. Currently the dependency map, which can take a
   * long time to destroy.
   */
  void cleanup_for_final();

  /*
   * Throw away data structures that won't be needed after the emit
   * stage.
   */
  void cleanup_post_emit();

  /*
   * Prepare the index for local execution. This marks the boundary
   * between running in extern-worker and running in the "classic"
   * way.
   */
  void make_local();

  /*
   * Pre-resolve type-alias and type-constant type-structures using the
   * local Index, then calculate type-constant invariance. The program must not
   * contain locally resolved type-structures, and the Index must have complete
   * class hierarchy information.
   */
  void preresolve_type_structures();

  /*
   * Access the StructuredLogEntry that the Index is using (if any).
   */
  StructuredLogEntry* sample() const;

  /*
   * Obtain the extern-worker related state that the Index used.
   */
  TicketExecutor& executor() const;
  extern_worker::Client& client() const;
  const CoroAsyncValue<extern_worker::Ref<Config>>& configRef() const;

  /*
   * The names of all units which have type-aliases defined within
   * them.
   */
  const SStringSet& units_with_type_aliases() const;

  /*
   * The names of all classes which should be analyzed (all classes
   * except closures declared within another class).
   */
  const TSStringSet& all_classes_to_analyze() const;

  /*
   * The names of all top-level functions.
   */
  const FSStringSet& all_funcs() const;

  /*
   * The names of all units.
   */
  const SStringSet& all_units() const;

  /*
   * Access the php::Program this Index is analyzing.
   */
  const php::Program& program() const;

  /*
   * Obtain a pointer to the unit which defined the given func.
   */
  const php::Unit* lookup_func_unit(const php::Func&) const;
  const php::Unit* lookup_func_original_unit(const php::Func&) const;

  /*
   * Obtain a pointer to the unit which defined the given class.
   */
  const php::Unit* lookup_class_unit(const php::Class&) const;

  /*
   * Obtain a pointer to the class which defines the given class
   * constant.
   */
  const php::Class* lookup_const_class(const php::Const&) const;

  /*
   * Obtain a pointer to the class which serves as the context for the
   * given class. For non-closures, this is just the input, but may be
   * different in closures.
   */
  const php::Class* lookup_closure_context(const php::Class&) const;

  /*
   * Look up the php::Class with the given name, returning nullptr if
   * it does not exist. The presence of a php::Class does not
   * necessarily mean the class is actually instantiable or won't
   * fatal when referenced.
   */
  const php::Class* lookup_class(SString) const;

  /*
   * Call the given callback for each (top-level) func defined in the
   * given Unit.
   */
  void for_each_unit_func(const php::Unit&,
                          std::function<void(const php::Func&)>) const;
  void for_each_unit_func_mutable(php::Unit&,
                                  std::function<void(php::Func&)>);

  /*
   * Call the given callback for each class defined in the given Unit.
   */
  void for_each_unit_class(const php::Unit&,
                           std::function<void(const php::Class&)>) const;
  void for_each_unit_class_mutable(php::Unit&,
                                   std::function<void(php::Class&)>);

  /*
   * Find all the extra methods associated with a class from its
   * traits.
   */
  const hphp_fast_set<const php::Func*>*
    lookup_extra_methods(const php::Class*) const;

  /*
   * Resolve the given class name to a res::Class.
   *
   * Returns std::nullopt if no such class with that name exists, or
   * if the class is not definable.
   */
  Optional<res::Class> resolve_class(SString name) const;
  Optional<res::Class> resolve_class(const php::Class&) const;

  /*
   * Find a type-alias with the given name. If a nullptr is returned,
   * then no type-alias exists with that name.
   */
  const php::TypeAlias* lookup_type_alias(SString name) const;

  /*
   * Find a class or a type-alias with the given name. This can be
   * more efficient than doing two different lookups.
   */
  struct ClassOrTypeAlias {
    // At most one of these will be non-null.
    const php::Class* cls;
    const php::TypeAlias* typeAlias;
    bool maybeExists;
  };
  ClassOrTypeAlias lookup_class_or_type_alias(SString name) const;

  /*
   * Resolve the given php::Func, which can be a function or
   * method. resolved() is guaranteed to be true for the returned
   * res::Func.
   */
  res::Func resolve_func_or_method(const php::Func&) const;

  /*
   * Try to resolve a function named `name'.
   */
  res::Func resolve_func(SString name) const;

  /*
   * Try to resolve a method named `name' with a this type of
   * `thisType' within a given Context.
   *
   * The type of `thisType' determines if the method is meant to be
   * static or not. A this type of BCls is a static method and a BObj
   * is for an instance method.
   *
   * Note: a resolved method does not imply the function is actually
   * callable at runtime. You still need to apply visibility checks,
   * etc.
   *
   * Pre: thisType.subtypeOf(BCls) || thisType.subtypeOf(BObj)
   */
  res::Func resolve_method(Context, const Type& thisType, SString name) const;

  /*
   * Resolve a class constructor for the supplied object type.
   *
   * Pre: obj.subtypeOf(BObj)
   */
  res::Func resolve_ctor(const Type& obj) const;

  /*
   * Lookup metadata about the constant access `cls'::`name', in the
   * current context `ctx'. The returned metadata not only includes
   * the best known type of the constant, but whether it is definitely
   * found, and whether accessing the constant might throw.  This
   * function is responsible for walking the class hierarchy to find
   * all possible constants and combining the results.  This is
   * intended to be the source of truth about constants during
   * analysis.
   *
   * This function only looks up non-type, non-context constants.
   */
  ClsConstLookupResult
  lookup_class_constant(Context ctx, const Type& cls, const Type& name) const;

  /*
   * Retrieve the information the Index knows about all of the class
   * constants defined on the given class. This does not register any
   * dependency.
   */
  std::vector<std::pair<SString, ClsConstInfo>>
  lookup_class_constants(const php::Class&) const;

  /*
   * Lookup metadata about the constant access `cls'::`name', where
   * that constant is meant to be a type-constant. The returned
   * metadata includes the best known type of the resolved
   * type-structure, whether it was found, and whether it was
   * abstract. This is intended to be the source of truth about
   * type-constants during analysis. The returned type-structure type
   * will always be static.
   *
   * By default, lookup_class_type_constant calls
   * resolve_type_structure to resolve any found type-structure. This
   * behavior can be overridden by providing a customer resolver.
   */
  using ClsTypeConstLookupResolver =
    std::function<TypeStructureResolution(const php::Const&,const php::Class&)>;

  ClsTypeConstLookupResult
  lookup_class_type_constant(
    const Type& cls,
    const Type& name,
    const ClsTypeConstLookupResolver& resolver = {}) const;

  /*
   * Lookup metadata about the constant given by the ConstIndex (with
   * the given name). The php::Class is used to provide the context.
   */
  ClsTypeConstLookupResult
  lookup_class_type_constant(const php::Class&, SString, ConstIndex) const;

  /*
   * Retrieve all type constants on the given class, whether declared
   * on the class or inherited.
   */
  std::vector<std::pair<SString, HHBBC::ConstIndex>>
  lookup_flattened_class_type_constants(const php::Class&) const;

  /*
   * Lookup what the best known Type for a constant would be, using a
   * given Index and Context, if a constant of that name were defined.
   */
  Type lookup_constant(Context ctx, SString cnsName) const;

  /*
   * Return whether a top-level constant initializer still has a dynamic
   * result and must be analyzed and emitted.
   */
  bool constant_init_needed(const php::Func&) const;

  /*
   * Return true if the return value of the function might depend on arg.
   */
  bool func_depends_on_arg(const php::Func* func, size_t arg) const;

  /*
   * Return type knowledge, whether the function is effect-free, and whether
   * an async invocation can suspend before producing its result.
   */
  struct ReturnType {
    Type t;
    bool effectFree{false};
    bool canSuspend{true};
    template <typename SerDe> void serde(SerDe& sd) {
      sd(t)(effectFree)(canSuspend);
    }
  };

  /*
   * If func is effect-free when called with args, and it returns a constant,
   * return that constant; otherwise return TInitCell.
   */
  ReturnType lookup_foldable_return_type(Context ctx,
                                         const CallContext& calleeCtx) const;

  /*
   * Return the best known return type for a resolved function, in a
   * context insensitive way.  Returns TInitCell at worst.
   */
  ReturnType lookup_return_type(Context, MethodsInfo*, res::Func,
                                Dep dep = Dep::ReturnTy) const;

  /*
   * Return the best known return type for a resolved function and
   * whether it is effect-free, given the supplied calling context.
   * Returns TInitCell at worst.
   *
   * During analyze phases, this function may re-enter analyze in
   * order to interpret the callee with these argument types.
   */
  ReturnType lookup_return_type(Context caller,
                                MethodsInfo*,
                                const CompactVector<Type>& args,
                                const NamedArgNameVec* argNames,
                                const Type& context,
                                res::Func,
                                Dep dep = Dep::ReturnTy) const;

  /*
   * Look up raw return type information for an unresolved
   * function. This is the best known return type, and the number of
   * refinements done to that type.
   *
   * This function does not register a dependency on the return type
   * information, so should not be used during analysis.
   *
   * Nothing may be writing to the index when this function is used,
   * but concurrent readers are allowed.
   */
  std::pair<ReturnType, size_t> lookup_return_type_raw(const php::Func*) const;

  /*
   * Return the best known types of a closure's used variables (on
   * entry to the closure).  The function is the closure body.
   */
  CompactVector<Type>
  lookup_closure_use_vars(const php::Func*) const;

  /*
   * Returns the control-flow insensitive inferred private instance
   * property types for a Class.  The Class doesn't need to be
   * resolved, because private properties don't depend on the
   * inheritance hierarchy.
   *
   * The Index tracks the largest types for private properties that
   * are guaranteed to hold at any program point.
   *
   * If move is true, the value will be moved out of the index. This
   * should only be done at emit time. (note that the only other user
   * of this info is analysis, which only uses it when processing the
   * owning class, so its safe to kill after emitting the owning
   * unit).
   */
  PropState lookup_private_props(const php::Class*,
                                 bool move = false) const;

  /*
   * Returns the control-flow insensitive inferred private static
   * property types for a Class.  The class doesn't need to be
   * resolved for the same reasons as for instance properties.
   *
   * The Index tracks the largest types for private static properties
   * that are guaranteed to hold at any program point.
   *
   * If move is true, the value will be moved out of the index. This
   * should only be done at emit time. (note that the only other user
   * of this info is analysis, which only uses it when processing the
   * owning class, so its safe to kill after emitting the owning
   * unit).
   */
  PropState lookup_private_statics(const php::Class*,
                                   bool move = false) const;
  PropState lookup_public_statics(const php::Class*) const;

  /*
   * Lookup metadata about the static property access `cls'::`name',
   * in the current context `ctx'. The returned metadata not only
   * includes the best known type of the property, but whether it is
   * definitely found, and whether the access might raise for various
   * reasons. This function is responsible for walking the class
   * hierarchy to find the appropriate property while applying
   * accessibility rules. This is intended to be the source of truth
   * about static properties during analysis.
   */
  PropLookupResult lookup_static(Context ctx,
                                 const PropertiesInfo& privateProps,
                                 const Type& cls,
                                 const Type& name) const;

  /*
   * Lookup the best known type for a public (non-static) property. Since we
   * don't do analysis on public properties, this just inferred from the
   * property's type-hint (if enforced).
   */
  Type lookup_public_prop(const Type& obj, const Type& name) const;
  Type lookup_public_prop(const php::Class* cls, SString name) const;

  /*
   * Returns the computed vtable slot for the given class, if it's an interface
   * that was given a vtable slot. No two interfaces implemented by the same
   * class will share the same vtable slot. May return kInvalidSlot, if the
   * given class isn't an interface or if it wasn't assigned a slot.
   */
  Slot lookup_iface_vtable_slot(const php::Class*) const;

  /*
   * Return the DependencyContext for ctx.
   */
  DependencyContext dependency_context(const Context& ctx) const;

  /*
   * Determine whether to use class-at-a-time, or function-at-a-time
   * dependencies.
   *
   * Must be called in single-threaded context.
   */
  void use_class_dependencies(bool f);
  bool using_class_dependencies() const;

  /*
   * Select the conservative function-resolution behavior used while
   * analyzing constant initializers.
   *
   * Must be called in single-threaded context, outside of analysis.
   */
  void set_constant_analysis(bool enabled);

  /*
   * Merge the type `val' into the known type for static property
   * `cls'::`name'. Depending on what we know about `cls' and `name',
   * this might affect multiple properties. This function is
   * responsible for walking the class hierarchy to find the
   * appropriate property while applying accessibility
   * rules. Mutations of AttrConst properties are ignored unless
   * `ignoreConst' is set to true. If `checkUB' is true, upper-bound
   * type constraints are consulted in addition to the normal type
   * constraints.
   *
   * The result tells you the subtype of val that would be
   * successfully set (according to the type constraints), and if the
   * mutation would throw or not.
   */
  PropMergeResult merge_static_type(Context ctx,
                                    PublicSPropMutations& publicMutations,
                                    PropertiesInfo& privateProps,
                                    const Type& cls,
                                    const Type& name,
                                    const Type& val,
                                    bool checkUB = false,
                                    bool ignoreConst = false,
                                    bool mustBeReadOnly = false) const;

  /*
   * Refine the types of the class constants defined by an 86cinit,
   * based on a round of analysis.
   *
   * No other threads should be using ctx.cls->constants or deps when
   * this function is called.
   *
   * Merges the set of Contexts that depended on the constants defined
   * by this 86cinit.
   */
  void refine_class_constants(const Context& ctx,
                              const ResolvedConstants& resolved,
                              DependencyContextSet& deps);

  /*
   * Refine the types of the constants defined by a function, based on
   * a round of analysis.
   *
   * Constants not defined by a pseudomain are considered unknowable
   *
   * No other threads should be calling functions on this Index when
   * this function is called.
   *
   * Merges the set of Contexts that depended on the constants defined
   * by this php::Func into deps.
   */
  void refine_constants(const FuncAnalysisResult& fa,
                        DependencyContextSet& deps);

  /*
   * Refine the return type for a function, based on a round of
   * analysis.
   *
   * No other threads should be calling functions on this Index when
   * this function is called.
   *
   * Merges the set of Contexts that depended on the return type of
   * this php::Func into deps.
   */
  void refine_return_info(const FuncAnalysisResult& fa,
                          DependencyContextSet& deps);

  /*
   * Refine the used var types for a closure, based on a round of
   * analysis.
   *
   * No other threads should be calling functions on this Index when
   * this function is called.
   *
   * Returns: true if the types have changed.
   */
  bool refine_closure_use_vars(const php::Class*,
                               const CompactVector<Type>&);

  /*
   * Refine the private property types for a class, based on a round
   * of analysis.
   *
   * No other threads should be calling functions on this Index when
   * this function is called.
   */
  void refine_private_props(const php::Class* cls,
                            const PropState&);

  /*
   * Refine the static private property types for a class, based on a
   * round of analysis.
   *
   * No other threads should be calling functions on this Index when
   * this function is called.
   */
  void refine_private_statics(const php::Class* cls,
                              const PropState&);

  /*
   * Record in the index that the given set of public static property mutations
   * has been found while analyzing the given function. During a round of
   * analysis, the mutations are gathered from the analysis results for each
   * function, recorded in the index, and then refine_public_statics is called
   * to process the mutations and update the index.
   *
   * No other threads should be calling functions on this Index when this
   * function is called.
   */
  void record_public_static_mutations(const php::Func& func,
                                      PublicSPropMutations mutations);

  /*
   * After a round of analysis with all the public static property mutations
   * being recorded with record_public_static_mutations, the types can be
   * reflected into the index for use during another type inference pass.
   *
   * No other threads should be calling functions on this Index when this
   * function is called.
   *
   * Merges the set of Contexts that depended on a public static property whose
   * type has changed.
   */
  void refine_public_statics(DependencyContextSet& deps);

  /*
   * Update the initial values for properties inferred from
   * 86[p/s]init functions during analysis. The modification of the
   * relevant php::Prop instances must be done here when we know
   * nobody else is reading them.
   */
  void update_prop_initial_values(const Context&,
                                  const ResolvedPropInits&,
                                  DependencyContextSet&);

  struct IndexData;

private:
  friend struct AnalysisScheduler;
  friend struct PublicSPropMutations;

  template <typename F>
  bool visit_every_dcls_cls(const DCls&, const F&) const;

  template <typename P, typename G>
  res::Func rfunc_from_dcls(const DCls&, SString, const P&, const G&) const;

private:
  std::unique_ptr<IndexData> m_data;
};

//////////////////////////////////////////////////////////////////////

/*
 * Download an Index::Input and restore its separately stored bytecode.
 * The executor and client are passed to the disposal callback before return.
 */
folly::coro::Task<php::Program> materialize_index_input(
  Index::Input,
  std::unique_ptr<TicketExecutor>,
  std::unique_ptr<extern_worker::Client>,
  DisposeCallback,
  StructuredLogEntry*
);

//////////////////////////////////////////////////////////////////////

// Abstracts away particular Index implementation from the analysis
// logic.
struct IIndex {
  IIndex() = default;
  IIndex(const IIndex&) = delete;
  IIndex(IIndex&&) = delete;
  IIndex& operator=(const IIndex&) = delete;
  IIndex& operator=(IIndex&&) = delete;

  virtual bool frozen() const = 0;

  virtual const php::Unit* lookup_func_unit(const php::Func&) const = 0;
  virtual const php::Unit* lookup_func_original_unit(const php::Func&) const = 0;

  virtual const php::Unit* lookup_class_unit(const php::Class&) const = 0;

  virtual const php::Class* lookup_const_class(const php::Const&) const = 0;

  virtual const php::Class* lookup_closure_context(const php::Class&) const = 0;

  virtual const php::Class* lookup_class(SString) const = 0;

  virtual void for_each_unit_func(const php::Unit&,
                                  std::function<void(const php::Func&)>) const = 0;
  virtual void for_each_unit_func_mutable(php::Unit&,
                                          std::function<void(php::Func&)>) = 0;
  virtual void for_each_unit_class(const php::Unit&,
                                   std::function<void(const php::Class&)>) const = 0;
  virtual void for_each_unit_class_mutable(php::Unit&,
                                           std::function<void(php::Class&)>) = 0;

  virtual CompactVector<const php::Func*>
  lookup_extra_methods(const php::Class&) const = 0;

  virtual Optional<res::Class> resolve_class(SString) const = 0;
  virtual Optional<res::Class> resolve_class(const php::Class&) const = 0;

  virtual std::pair<const php::TypeAlias*, bool>
  lookup_type_alias(SString) const = 0;

  virtual Index::ClassOrTypeAlias lookup_class_or_type_alias(SString) const = 0;

  virtual res::Func resolve_func_or_method(const php::Func&) const = 0;

  virtual res::Func resolve_func(SString) const = 0;

  virtual res::Func resolve_method(Context,
                                   const Type& thisType,
                                   SString name) const = 0;

  virtual res::Func resolve_ctor(const Type& obj) const = 0;

  virtual std::vector<std::pair<SString, ClsConstInfo>>
  lookup_class_constants(const php::Class&) const = 0;

  virtual ClsConstLookupResult
  lookup_class_constant(Context, const Type& cls, const Type& name) const = 0;

  virtual ClsTypeConstLookupResult lookup_class_type_constant(
    const Type& cls,
    const Type& name,
    const Index::ClsTypeConstLookupResolver& resolver = {}
  ) const = 0;

  virtual ClsTypeConstLookupResult lookup_class_type_constant(
    const php::Class&,
    SString,
    ConstIndex
  ) const = 0;

  virtual std::vector<std::pair<SString, ConstIndex>>
  lookup_flattened_class_type_constants(const php::Class&) const = 0;

  virtual Type lookup_constant(Context, SString) const = 0;

  virtual bool func_depends_on_arg(const php::Func* func, size_t arg) const = 0;

  virtual Index::ReturnType
  lookup_foldable_return_type(Context, const CallContext&) const = 0;

  virtual Index::ReturnType
  lookup_return_type(Context, MethodsInfo*, res::Func,
                     Dep dep = Dep::ReturnTy) const = 0;

  virtual Index::ReturnType
  lookup_return_type(Context caller,
                     MethodsInfo*,
                     const CompactVector<Type>& args,
                     const NamedArgNameVec* argNames,
                     const Type& context,
                     res::Func,
                     Dep dep = Dep::ReturnTy) const = 0;

  virtual std::pair<Index::ReturnType, size_t>
  lookup_return_type_raw(const php::Func*) const = 0;

  virtual CompactVector<Type>
  lookup_closure_use_vars(const php::Func&) const = 0;
  virtual CompactVector<Type>
  lookup_closure_use_vars_raw(const php::Func&) const = 0;

  virtual PropState lookup_private_props(const php::Class*,
                                         bool move = false) const = 0;

  virtual PropState lookup_private_statics(const php::Class*,
                                           bool move = false) const = 0;
  virtual PropState lookup_public_statics(const php::Class*) const = 0;

  virtual PropLookupResult lookup_static(Context,
                                         const PropertiesInfo& privateProps,
                                         const Type& cls,
                                         const Type& name) const = 0;

  virtual Type lookup_public_prop(const Type& obj, const Type& name) const = 0;

  virtual Slot lookup_iface_vtable_slot(const php::Class*) const = 0;

  virtual PropMergeResult
  merge_static_type(Context ctx,
                    PublicSPropMutations& publicMutations,
                    PropertiesInfo& privateProps,
                    const Type& cls,
                    const Type& name,
                    const Type& val,
                    bool checkUB = false,
                    bool ignoreConst = false,
                    bool mustBeReadOnly = false) const = 0;

  virtual bool tracking_public_sprops() const = 0;
private:
  virtual void push_context(const Context&) const = 0;
  virtual void pop_context() const = 0;

  virtual bool set_in_type_cns(bool) const = 0;

  friend struct ContextPusher;
  friend struct InTypeCns;
};

//////////////////////////////////////////////////////////////////////

// Push the given Context onto the Index's stack of Contexts, and
// remove it automatically.
struct ContextPusher {
  ContextPusher(const IIndex& index, const Context& ctx) : index{index} {
    index.push_context(ctx);
  }
  ~ContextPusher() { index.pop_context(); }
private:
  const IIndex& index;
};

//////////////////////////////////////////////////////////////////////

// RAII class to mark that we're resolving a class' type-constants.
struct InTypeCns {
  explicit InTypeCns(const IIndex& index, bool b = true)
    : index{index}
    , was{index.set_in_type_cns(b)} {}
  ~InTypeCns() { index.set_in_type_cns(was); }
private:
  const IIndex& index;
  bool was;
};

//////////////////////////////////////////////////////////////////////

struct IndexAdaptor : public IIndex {
  explicit IndexAdaptor(const Index& index)
   : index{const_cast<Index&>(index)} {}

  bool frozen() const override { return index.frozen(); }

  const php::Unit* lookup_func_unit(const php::Func& f) const override {
    return index.lookup_func_unit(f);
  }
  const php::Unit* lookup_func_original_unit(const php::Func& f) const override {
    return index.lookup_func_original_unit(f);
  }
  const php::Unit* lookup_class_unit(const php::Class& c) const override {
    return index.lookup_class_unit(c);
  }
  const php::Class* lookup_const_class(const php::Const& c) const override {
    return index.lookup_const_class(c);
  }
  const php::Class* lookup_closure_context(const php::Class& c) const override {
    return index.lookup_closure_context(c);
  }
  const php::Class* lookup_class(SString c) const override {
    return index.lookup_class(c);
  }
  CompactVector<const php::Func*>
  lookup_extra_methods(const php::Class& c) const override {
    if (auto const extra = index.lookup_extra_methods(&c)) {
      CompactVector<const php::Func*> out;
      out.reserve(extra->size());
      for (auto const e : *extra) out.emplace_back(e);
      std::sort(
        begin(out), end(out),
        [] (const php::Func* f1, const php::Func* f2) {
          assertx(f1->cls);
          assertx(f2->cls);
          if (f1->cls != f2->cls) {
            return string_data_lt_type{}(f1->cls->name, f2->cls->name);
          }
          return string_data_lt_func{}(f1->name, f2->name);
        }
      );
      return out;
    }
    return {};
  }
  void for_each_unit_func(const php::Unit& u,
                          std::function<void(const php::Func&)> f) const override {
    index.for_each_unit_func(u, std::move(f));
  }
  void for_each_unit_func_mutable(php::Unit& u,
                                  std::function<void(php::Func&)> f) override {
    index.for_each_unit_func_mutable(u, std::move(f));
  }
  void for_each_unit_class(const php::Unit& u,
                           std::function<void(const php::Class&)> f) const override {
    index.for_each_unit_class(u, std::move(f));
  }
  void for_each_unit_class_mutable(php::Unit& u,
                                   std::function<void(php::Class&)> f) override {
    index.for_each_unit_class_mutable(u, std::move(f));
  }

  Optional<res::Class> resolve_class(SString c) const override {
    return index.resolve_class(c);
  }
  Optional<res::Class> resolve_class(const php::Class& c) const override {
    return index.resolve_class(c);
  }
  std::pair<const php::TypeAlias*, bool>
  lookup_type_alias(SString a) const override {
    auto const ta = index.lookup_type_alias(a);
    return std::make_pair(ta, (bool)ta);
  }
  Index::ClassOrTypeAlias
  lookup_class_or_type_alias(SString name) const override {
    return index.lookup_class_or_type_alias(name);
  }
  res::Func resolve_func_or_method(const php::Func& f) const override {
    return index.resolve_func_or_method(f);
  }
  res::Func resolve_func(SString s) const override {
    return index.resolve_func(s);
  }
  res::Func resolve_method(Context c, const Type& t, SString n) const override {
    return index.resolve_method(c, t, n);
  }
  res::Func resolve_ctor(const Type& o) const override {
    return index.resolve_ctor(o);
  }
  std::vector<std::pair<SString, ClsConstInfo>>
  lookup_class_constants(const php::Class& c) const override {
    return index.lookup_class_constants(c);
  }
  ClsConstLookupResult lookup_class_constant(Context c,
                                             const Type& t,
                                             const Type& n) const override {
    return index.lookup_class_constant(c, t, n);
  }
  ClsTypeConstLookupResult
  lookup_class_type_constant(
    const Type& c,
    const Type& n,
    const Index::ClsTypeConstLookupResolver& r = {}
  ) const override {
    return index.lookup_class_type_constant(c, n, r);
  }
  ClsTypeConstLookupResult
  lookup_class_type_constant(const php::Class& ctx,
                             SString n,
                             ConstIndex idx) const override {
    return index.lookup_class_type_constant(ctx, n, idx);
  }
  std::vector<std::pair<SString, ConstIndex>>
  lookup_flattened_class_type_constants(const php::Class& cls) const override {
    return index.lookup_flattened_class_type_constants(cls);
  }
  Type lookup_constant(Context c, SString s) const override {
    return index.lookup_constant(c, s);
  }
  bool func_depends_on_arg(const php::Func* f, size_t p) const override {
    return index.func_depends_on_arg(f, p);
  }
  Index::ReturnType
  lookup_foldable_return_type(Context c1,
                              const CallContext& c2) const override {
    return index.lookup_foldable_return_type(c1, c2);
  }
  Index::ReturnType lookup_return_type(Context c, MethodsInfo* m, res::Func f,
                                       Dep d = Dep::ReturnTy) const override {
    return index.lookup_return_type(c, m, f, d);
  }
  Index::ReturnType lookup_return_type(Context c,
                                       MethodsInfo* m,
                                       const CompactVector<Type>& a,
                                       const NamedArgNameVec* argNames,
                                       const Type& t,
                                       res::Func f,
                                       Dep d = Dep::ReturnTy) const override {
    return index.lookup_return_type(c, m, a, argNames, t, f, d);
  }
  std::pair<Index::ReturnType, size_t>
  lookup_return_type_raw(const php::Func* f) const override {
    return index.lookup_return_type_raw(f);
  }
  CompactVector<Type>
  lookup_closure_use_vars(const php::Func& f) const override {
    return index.lookup_closure_use_vars(&f);
  }
  CompactVector<Type>
  lookup_closure_use_vars_raw(const php::Func& f) const override {
    return index.lookup_closure_use_vars(&f);
  }
  PropState lookup_private_props(const php::Class* c,
                                 bool m = false) const override {
    return index.lookup_private_props(c, m);
  }
  PropState lookup_private_statics(const php::Class* c,
                                   bool m = false) const override {
    return index.lookup_private_statics(c, m);
  }
  PropState lookup_public_statics(const php::Class* c) const override {
    return index.lookup_public_statics(c);
  }
  PropLookupResult lookup_static(Context c1,
                                 const PropertiesInfo& p,
                                 const Type& c2,
                                 const Type& n) const override {
    return index.lookup_static(c1, p, c2, n);
  }
  Type lookup_public_prop(const Type& o, const Type& n) const override {
    return index.lookup_public_prop(o, n);
  }
  Slot lookup_iface_vtable_slot(const php::Class* c) const override {
    return index.lookup_iface_vtable_slot(c);
  }

  PropMergeResult
  merge_static_type(Context ctx,
                    PublicSPropMutations& publicMutations,
                    PropertiesInfo& privateProps,
                    const Type& cls,
                    const Type& name,
                    const Type& val,
                    bool checkUB = false,
                    bool ignoreConst = false,
                    bool mustBeReadOnly = false) const override {
    return index.merge_static_type(
      ctx, publicMutations, privateProps,
      cls, name, val, checkUB, ignoreConst,
      mustBeReadOnly
    );
  }
  bool tracking_public_sprops() const override {
    return index.using_class_dependencies();
  }

private:
  void push_context(const Context&) const override {}
  void pop_context() const override {}

  bool set_in_type_cns(bool) const override { return false; }

  Index& index;
};

//////////////////////////////////////////////////////////////////////

// What kind of analysis is being done?
enum class AnalysisMode {
  Full,
  Final
};

//////////////////////////////////////////////////////////////////////

// Represents all of the things a class or func might depend up for
// analysis. If any of these things change, the class or func will be
// scheduled to be analyzed again.
struct AnalysisDeps {
  // Some dependencies (for example, functions) can have multiple
  // "types" (IE, return-type, bytecode, etc). A class or func's
  // dependency on a function will be some union of these types.
  enum Type : uint16_t {
    None          = 0,
    // The existence of this thing and basic metadata and nothing
    // more.
    Meta          = (1 << 0),
    // Return-type of function
    RetType       = (1 << 1),
    // Return-type of function but only if it becomes a scalar
    ScalarRetType = (1 << 2),
    // Whether any parameters are returned unchanged
    RetParam      = (1 << 3),
    // Whether any parameters are unused
    UnusedParams  = (1 << 4),
    // Bytecode of class or function
    Bytecode      = (1 << 5),
    // Use vars of a closure
    UseVars       = (1 << 6),
    // Whether a class might raise when being initialized
    ClassInitMightRaise = (1 << 7),
    // The initial (non-scalar) value of any property on the class
    // (per-property granularity not necessary).
    PropInitVals = (1 << 8)
  };

  // Some types can change as a result of analysis and hence can
  // trigger reschedules. The only type right now that cannot change
  // is "Meta" (since it represents the basic metadata of a class or
  // function which doesn't change).
  static constexpr Type kValidForChanges = static_cast<Type>(
    Type::RetType |
    Type::ScalarRetType |
    Type::RetParam |
    Type::UnusedParams |
    Type::Bytecode |
    Type::UseVars |
    Type::ClassInitMightRaise |
    Type::PropInitVals
  );

  static constexpr bool isValidForChanges(Type t) {
    return
      t != Type::None &&
      (t & kValidForChanges) == t;
  }

  // Add dependencies on various entities

  struct Class { SString name; };
  struct Func { SString name; };
  struct Constant { SString name; };
  // Dependency on an unspecified class constant (name is the class).
  struct AnyClassConstant { SString name; };

  // Dependency on a specific property of a class (whether static or instance).
  struct Property {
    SString cls;   // Class name
    SString prop;  // Property name
    bool operator==(const Property& o) const {
      return cls->tsame(o.cls) && prop == o.prop;
    }
    bool operator<(const Property& o) const {
      if (string_data_lt_type{}(cls, o.cls)) return true;
      if (string_data_lt_type{}(o.cls, cls)) return false;
      return string_data_lt{}(prop, o.prop);
    }
    struct Hasher {
      size_t operator()(const Property& p) const {
        return folly::hash::hash_combine(
          p.cls->hash(),
          p.prop->hash()
        );
      }
    };
    template <typename SerDe> void serde(SerDe& sd) {
      sd(cls)(prop);
    }
  };
  // Dependency on any property changes from a class. Used when property name
  // is unknown or for bulk operations.
  struct AnyProperty { SString cls; };

  Type add(Class, Type);

  bool add(ConstIndex, bool inTypeCns);
  bool add(Constant);
  bool add(AnyClassConstant, bool inTypeCns);

  bool add(Property);
  bool add(AnyProperty);

  Type add(const php::Func&, Type);
  Type add(MethRef, Type);
  Type add(Func, Type);

  bool empty() const;

  // Remove redundant dependencies
  void clean();

  AnalysisDeps& operator|=(const AnalysisDeps&);

  template <typename SerDe> void serde(SerDe& sd) {
    sd(funcs, string_data_lt_func{})
      (methods, std::less<>{})
      (classes, string_data_lt_type{})
      (clsConstants, std::less<>{})
      (anyClsConstants, string_data_lt_type{})
      (constants, string_data_lt{})
      (typeCnsClsConstants, std::less<>{})
      (typeCnsAnyClsConstants, string_data_lt_type{})
      (properties, std::less<>{})
      (anyProperties, string_data_lt_type{})
      ;
  }

private:
  FSStringToOneT<Type> funcs;
  hphp_fast_map<MethRef, Type, MethRef::Hash> methods;

  TSStringToOneT<Type> classes;
  hphp_fast_set<ConstIndex, ConstIndex::Hasher> clsConstants;
  TSStringSet anyClsConstants;
  SStringSet constants;

  hphp_fast_set<ConstIndex, ConstIndex::Hasher> typeCnsClsConstants;
  TSStringSet typeCnsAnyClsConstants;

  hphp_fast_set<Property, Property::Hasher> properties;
  TSStringSet anyProperties;

  static Type merge(Type&, Type);

  friend std::string show(const AnalysisDeps&);

  friend struct AnalysisIndex;
  friend struct AnalysisScheduler;
};

// Make Type enum behave like bitset.
inline AnalysisDeps::Type operator|(AnalysisDeps::Type a,
                                    AnalysisDeps::Type b) {
  using T = std::underlying_type_t<AnalysisDeps::Type>;
  return static_cast<AnalysisDeps::Type>(
    static_cast<T>(a) | static_cast<T>(b)
  );
}
inline AnalysisDeps::Type& operator|=(AnalysisDeps::Type& a,
                                      AnalysisDeps::Type b) {
  a = a | b;
  return a;
}
inline AnalysisDeps::Type operator&(AnalysisDeps::Type a,
                                    AnalysisDeps::Type b) {
  using T = std::underlying_type_t<AnalysisDeps::Type>;
  return static_cast<AnalysisDeps::Type>(
    static_cast<T>(a) & static_cast<T>(b)
  );
}
inline AnalysisDeps::Type& operator&=(AnalysisDeps::Type& a,
                                      AnalysisDeps::Type b) {
  a = a & b;
  return a;
}
inline AnalysisDeps::Type operator-(AnalysisDeps::Type a,
                                    AnalysisDeps::Type b) {
  using T = std::underlying_type_t<AnalysisDeps::Type>;
  return static_cast<AnalysisDeps::Type>(
    static_cast<T>(a) & ~static_cast<T>(b)
  );
}

std::string show(AnalysisDeps::Type);

//////////////////////////////////////////////////////////////////////

// Represents the changes made to a class or function during
// analysis. This is the other side of AnalysisDeps. A function or
// class marking a change in its AnalysisChangeSet will trigger other
// classes or functions which have a dependency on the same type to be
// re-analyzed.
struct AnalysisChangeSet {
  using Type = AnalysisDeps::Type;
  using Class = AnalysisDeps::Class;
  using Property = AnalysisDeps::Property;

  void changed(ConstIndex);
  void changed(const php::Constant&);
  void changed(const php::Class&, Type);
  void changed(const php::Func&, Type);
  void changed(const php::Class&, const php::Prop&);

  void fixed(ConstIndex);
  void fixed(const php::Class&);
  void fixed(const php::Unit&);

  void typeCnsName(const php::Class&, Class);
  void typeCnsName(const php::Unit&, Class);

  void remove(const php::Func& f) { funcs.erase(f.name); }

  void filter(const TSStringSet&,
              const FSStringSet&,
              const SStringSet&,
              const SStringSet&);

  template <typename SerDe> void serde(SerDe& sd) {
    sd(funcs, string_data_lt_func{})
      (classes, string_data_lt_type{})
      (methods, std::less<>{})
      (constants, string_data_lt{})
      (clsConstants, std::less<>{})
      (fixedClsConstants, std::less<>{})
      (allClsConstantsFixed, string_data_lt_type{})
      (unitsFixed, string_data_lt{})
      (properties, std::less<>{})
      (clsTypeCnsNames, string_data_lt_type{}, string_data_lt_type{})
      (unitTypeCnsNames, string_data_lt{}, string_data_lt_type{})
      ;
  }
private:
  FSStringToOneT<Type> funcs;
  TSStringToOneT<Type> classes;
  hphp_fast_map<MethRef, Type, MethRef::Hash> methods;
  SStringSet constants;
  hphp_fast_set<ConstIndex, ConstIndex::Hasher> clsConstants;
  hphp_fast_set<ConstIndex, ConstIndex::Hasher> fixedClsConstants;
  TSStringSet allClsConstantsFixed;
  SStringSet unitsFixed;
  hphp_fast_set<Property, Property::Hasher> properties;

  TSStringToOneT<TSStringSet> clsTypeCnsNames;
  TSStringToOneT<TSStringSet> unitTypeCnsNames;

  friend struct AnalysisScheduler;
};

//////////////////////////////////////////////////////////////////////

/*
 * Input to a single analysis job, produced by AnalysisScheduler::schedule().
 *
 * Each AnalysisInput represents one bucket of work to be processed in parallel.
 * It contains:
 * - reportBundles: Bundles whose analysis results should be reported back
 * - noReportBundles: Bundles to analyze speculatively (on someone's trace)
 * - Meta: Additional information needed to perform the analysis
 *
 * The bundles are categorized as described in AnalysisScheduler's documentation.
 * Pure dependency bundles are fetched on-demand by the analysis job based on
 * the dependency information in Meta.
 */
struct AnalysisInput {
  AnalysisInput() = default;
  AnalysisInput(const AnalysisInput&) = delete;
  AnalysisInput(AnalysisInput&&) = default;
  AnalysisInput& operator=(const AnalysisInput&) = delete;
  AnalysisInput& operator=(AnalysisInput&&) = default;

  bool empty() const {
    return reportBundles.empty() && noReportBundles.empty();
  }
  SString key() const { return m_key; }

  /*
   * Metadata needed by the analysis job to perform analysis and manage
   * dependencies.
   */
  struct Meta {
    Meta() = default;
    Meta(const Meta&) = delete;
    Meta(Meta&&) = default;
    Meta& operator=(const Meta&) = delete;
    Meta& operator=(Meta&&) = default;

    // Names of all bundles included in this job (report + noReport + pureDep)
    std::vector<SString> bundleNames;

    // Missing/undefined entities that are referenced as dependencies
    TSStringSet badClasses;
    FSStringSet badFuncs;
    SStringSet badConstants;

    // Dependency information for each entity being processed.
    // Used to fetch additional bundles on-demand and track changes.
    TSStringToOneT<AnalysisDeps> classDeps;
    FSStringToOneT<AnalysisDeps> funcDeps;
    SStringToOneT<AnalysisDeps> unitDeps;

    // Entities that are starting points for this analysis (have
    // toSchedule=true).  These are the entities with changed
    // information that triggered scheduling.
    TSStringSet startCls;
    FSStringSet startFunc;
    SStringSet startUnit;

    // Interface slot mapping (only populated in final pass)
    TSStringToOneT<Slot> ifaceSlotMap;

    // Index of this bucket in the overall scheduling round
    uint32_t bucketIdx;

    // Whether to dump the index and/or representation for units
    // during final-pass.
    bool dumpIndex{false};
    bool dumpRep{false};

    template <typename SerDe> void serde(SerDe& sd) {
      ScopedStringDataIndexer _;
      sd(bundleNames)
        (badClasses, string_data_lt_type{})
        (badFuncs, string_data_lt_func{})
        (badConstants, string_data_lt{})
        (classDeps, string_data_lt_type{})
        (funcDeps, string_data_lt_func{})
        (unitDeps, string_data_lt{})
        (startCls, string_data_lt_type{})
        (startFunc, string_data_lt_func{})
        (startUnit, string_data_lt{})
        (ifaceSlotMap, string_data_lt_type{})
        (bucketIdx)
        (dumpIndex)
        (dumpRep)
        ;
    }
  };

  std::vector<SString> reportBundleNames() const;

  Meta takeMeta() { return std::move(meta); }

  // Produces input for the analysis job.
  using Tuple = std::tuple<
    RefVec<AnalysisIndexBundle>,
    RefVec<AnalysisIndexBundle>,
    extern_worker::Ref<Meta>
  >;
  Tuple toInput(extern_worker::Ref<Meta>) const;
private:
  // Key for identifying this input (typically the first bundle name)
  SString m_key{nullptr};

  // Bundles whose results should be reported back
  RefVec<AnalysisIndexBundle> reportBundles;
  // Bundles to analyze but not report (on someone's trace)
  RefVec<AnalysisIndexBundle> noReportBundles;

  Meta meta;

  friend struct AnalysisScheduler;
};

//////////////////////////////////////////////////////////////////////

/*
 * Output from a single analysis job, consumed by AnalysisScheduler::record().
 *
 * Contains the results of analyzing the entities in an AnalysisInput, including:
 * - Updated dependency information discovered during analysis
 * - What changed (so the scheduler can determine what needs re-analysis)
 * - Updated bundles with refined analysis results
 */
struct AnalysisOutput {
  /*
   * Metadata about changes and dependencies discovered during analysis.
   */
  struct Meta {
    // Updated dependency information for analyzed entities
    FSStringToOneT<AnalysisDeps> funcDeps;
    TSStringToOneT<AnalysisDeps> classDeps;
    SStringToOneT<AnalysisDeps> unitDeps;

    // What changed during analysis (used to determine eligibility for next round)
    AnalysisChangeSet changed;

    // Functions that were removed (e.g., optimized-away constant initializers)
    FSStringSet removedFuncs;

    // Classes from which each class inherited constants (for tracking dependencies)
    TSStringToOneT<TSStringSet> cnsBases;
    template <typename SerDe> void serde(SerDe& sd) {
      ScopedStringDataIndexer _;
      sd(funcDeps, string_data_lt_func{})
        (classDeps, string_data_lt_type{})
        (unitDeps, string_data_lt{})
        (changed)
        (removedFuncs, string_data_lt_func{})
        (cnsBases, string_data_lt_type{}, string_data_lt_type{})
        ;
    }
  };

  // Names of bundles that were analyzed
  std::vector<SString> bundleNames;
  // Updated bundles with refined analysis results (for reportBundles only)
  RefVec<AnalysisIndexBundle> bundles;

  Meta meta;
};

//////////////////////////////////////////////////////////////////////

/*
 * Scheduler to coordinate incremental whole-program analysis rounds.
 *
 * OVERVIEW:
 * =========
 * The scheduler's primary goal is to minimize the total number of analysis
 * rounds required to reach a fixed point by intelligently grouping work and
 * processing dependency chains together.
 *
 * KEY CONCEPTS:
 * =============
 *
 * Eligibility:
 * -----------
 * An entity (func/class/unit) is "eligible" if it might produce different
 * analysis results this round. This happens when:
 * - The entity's dependencies changed in the previous round, OR
 * - One of its (transitive) dependencies is eligible
 *
 * Eligibility typically decreases over rounds as the program reaches a fixed
 * point. When no entities are eligible, analysis is complete.
 *
 * Traces:
 * -------
 * A "trace" is the set of transitive dependencies that should be analyzed
 * together to short-circuit dependency chains. If A depends on B depends on C
 * depends on D, and all are eligible, then A's trace includes [A,B,C,D].
 *
 * By processing the entire trace in one job, information propagates through
 * the whole chain in a single round instead of requiring N rounds for a
 * chain of length N.
 *
 * Traces are limited by maxTraceWeight (total bytes) and maxTraceDepth
 * (number of hops) to prevent them from spanning the entire codebase.
 *
 * TraceState vs DepState:
 * -----------------------
 * - DepState: Tracks dependencies for a single entity (func/class/unit)
 * - TraceState: Aggregates multiple DepStates that must be scheduled together
 *               (typically represents a bundle of entities)
 *
 * Since entities are bundled together for storage/distribution, all entities
 * in a bundle must be scheduled to the same work bucket.
 *
 * Bundle Types:
 * -------------
 * Each bucket contains three mutually exclusive types of bundles:
 *
 * 1. reportBundles: Entities to analyze and report results for (the "primary"
 *    work). These are entities that changed or whose dependencies changed.
 *
 * 2. noReportBundles: Entities to analyze but NOT report. These are on some
 *    other entity's trace - we analyze them speculatively to shorten dependency
 *    chains, but we don't return their results (they'll be properly analyzed
 *    and reported when scheduled as reportBundles in a future round).
 *
 * 3. pureDepBundles: Entities to NOT analyze. These provide information only.
 *    They're dependencies of other entities but are ineligible (won't change
 *    this round), so we skip analysis and just use their existing info.
 *
 * ALGORITHM PHASES:
 * =================
 * The schedule() method executes these phases:
 *
 * 1. initTraceStates()  - Reset state for this round
 * 2. findSuccs()        - Build successor graph, mark eligible entities
 * 3. findAllDeps()      - Compute full dependency sets (trace + stable deps)
 * 4. makeTraces()       - Build traces by following eligible dependency chains
 * 5. makeBuckets()      - Pack traces into size-bounded buckets
 * 6. makeInputs()       - Transform buckets into AnalysisInput jobs
 *
 * See individual method comments for details on each phase.
 */
struct AnalysisScheduler {
  explicit AnalysisScheduler(Index&);
  AnalysisScheduler(const AnalysisScheduler&) = delete;
  AnalysisScheduler(AnalysisScheduler&&);
  AnalysisScheduler& operator=(const AnalysisScheduler&) = delete;
  AnalysisScheduler& operator=(AnalysisScheduler&&) = delete;
  ~AnalysisScheduler();

  using Mode = AnalysisMode;

  void reserveForRegistration(size_t classes, size_t funcs, size_t units);
  void registerAllBulk(const TSStringSet& classes,
                       const FSStringSet& funcs,
                       const SStringSet& units);

  void enableAll();

  // Record the output of an analysis job. This can be called in a
  // multi-threaded context.
  void record(AnalysisOutput);

  // Called when all analysis jobs for this round have finished. This
  // calculates what has changed and what needs to be re-analyzed.
  void recordingDone();

  // Schedule the work that needs to run into buckets. Each bucket
  // won't exceed the specified maximum weight (by very much). The
  // maximum trace weight determines how much (potentially redundant)
  // extra work to put into each bucket in the hopes of minimizing
  // total rounds.
  std::vector<AnalysisInput> schedule(Mode,
                                      size_t maxTraceWeight,
                                      size_t maxTraceLength,
                                      size_t maxBucketWeight);

  size_t workItems() const { return totalWorkItems; }

private:
  using Type = AnalysisDeps::Type;

  using BucketSet = hphp_fast_set<uint32_t>;

  /*
   * Represents a single func, class, or unit - the finest granularity
   * of dependency tracking.
   *
   * Each entity tracks its own dependencies and whether it needs to be
   * scheduled for analysis this round.
   */
  struct DepState {
    enum Kind {
      Func,
      Class,
      Unit
    };

    using Set = hphp_fast_set<DepState*, pointer_hash<DepState>>;
    using CSet = hphp_fast_set<const DepState*, pointer_hash<DepState>>;

    DepState(SString name, Kind kind) : name{name}, kind{kind} {}
    SString name;
    Kind kind;
    AnalysisDeps deps;  // Dependencies recorded from previous analysis
    Optional<uint32_t> authBucket;  // Authoritative bucket if already scheduled
    // toSchedule indicates this entity has changed information and needs
    // to be re-analyzed (vs being pulled in only as a dependency)
    bool toSchedule{true};
    // eligible indicates this entity might produce different results this
    // round (either it or its dependencies changed last round)
    bool eligible{true};
  };

  // These wrap a DepState, but also include information about what
  // changed (which varies according to the type).
  struct FuncState {
    explicit FuncState(SString name) : depState{name, DepState::Func} {}
    DepState depState;
    Type changed{};
  };
  struct ClassState {
    explicit ClassState(SString name) : depState{name, DepState::Class} {}
    DepState depState;
    Type changed{};
    CompactVector<Type> methodChanges;
    boost::dynamic_bitset<> cnsChanges;
    boost::dynamic_bitset<> cnsFixed;
    TSStringSet typeCnsNames;
    SStringSet propertyChanges;
    bool allCnsFixed{false};
  };
  struct UnitState {
    explicit UnitState(SString name) : depState{name, DepState::Unit} {}
    DepState depState;
    TSStringSet typeCnsNames;
    bool fixed{false};
  };

  /*
   * TraceState aggregates multiple DepStates that must be scheduled together.
   *
   * Typically represents a bundle of entities. Since entities are grouped into
   * bundles for storage and distribution, all entities in a bundle must be
   * assigned to the same work bucket.
   *
   * The TraceState builds up the "trace" - the set of transitive dependencies
   * that should be processed together to short-circuit dependency chains and
   * minimize total analysis rounds.
   */
  struct TraceState {
    explicit TraceState(SString n) : name{n} {}

    using Set = hphp_fast_set<TraceState*, pointer_hash<TraceState>>;
    using CSet = hphp_fast_set<const TraceState*, pointer_hash<TraceState>>;

    SString name;  // Usually the bundle name
    size_t idx{0};  // Index for deterministic ordering

    // True if any DepState in this TraceState is eligible
    bool eligible{false};

    // Successor TraceStates (other bundles this one depends on).
    // Built in findSuccs(), used to construct traces in makeTraces().
    std::vector<TraceState*> succs;

    // Full dependency set: all bundles this TraceState depends on (both
    // eligible and ineligible). Built in findAllDeps().
    std::vector<SString> deps;

    // Subset of deps: bundles to include in trace (up to size/depth limits).
    // These are the bundles that will be provided to the analysis job.
    // Built in makeTraces().
    std::vector<SString> trace;

    // TraceStates that should be actively processed (have eligible work).
    // Built in makeTraces().
    std::vector<const TraceState*> toProcess;

    // "Bad" entities (missing/undefined) that this TraceState depends on.
    // These need to be passed to the analysis job even though they don't exist.
    std::vector<SString> badClasses;
    std::vector<SString> badFuncs;
    std::vector<SString> badConstants;

    // The individual DepStates that make up this TraceState
    std::vector<DepState*> depStates;

    // Set of bucket indices where this TraceState is present
    BucketSet present;
  };

  struct Bucket;

  Trace::Bump bumpFor(const DepState&) const;
  Trace::Bump bumpFor(const TraceState&) const;

  void sortNames();

  void addPredeps(SString, const SStringSet&, AnalysisDeps&) const;

  void removeFuncs();
  void findToSchedule();
  void resetChanges();

  void recordChanges(const AnalysisOutput&);
  void updateDepState(AnalysisOutput&);

  template <typename V> void visitClassAsDep(SString, bool, bool,
                                             const V&) const;
  template <typename V> void visitFuncAsDep(SString, bool, const V&) const;
  template <typename V> void visitUnitAsDep(SString, const V&) const;
  template <typename V> void visitDeps(const DepState&, bool, const V&) const;

  struct BucketPresence {
    const BucketSet* present{nullptr};
    Optional<uint32_t> auth;
  };
  BucketPresence bucketsForClass(SString) const;
  BucketPresence bucketsForFunc(SString) const;
  BucketPresence bucketsForUnit(SString) const;
  BucketPresence bucketsForConstant(SString) const;
  BucketPresence bucketsForTypeAlias(SString) const;
  BucketPresence bucketsForClassOrTypeAlias(SString) const;

  Either<const ClassState*, const UnitState*>
  stateForClassOrTypeAlias(SString) const;

  enum class Presence {
    None,
    Dep,
    Full
  };
  Presence presenceOf(const DepState&, const BucketPresence&) const;
  Presence presenceOfClass(const DepState&, SString) const;
  Presence presenceOfClassOrTypeAlias(const DepState&, SString) const;
  Presence presenceOfFunc(const DepState&, SString) const;
  Presence presenceOfConstant(const DepState&, SString) const;

  DepState::CSet findToProcess(const Bucket&) const;
  SStringSet findRelevantBundles(const DepState::CSet&, const Bucket&) const;

  void initTraceStates();
  void findSuccs(size_t);
  void findAllDeps();
  void makeTraces(size_t, size_t);
  std::vector<Bucket> makeBuckets(size_t);
  std::vector<AnalysisInput> makeInputs(const std::vector<Bucket>&, Mode);
  void calcBucketSets(std::vector<AnalysisInput>&);
  void checkInputInvariants(const std::vector<AnalysisInput>&);

  Index& index;

  size_t round{0};

  std::vector<SString> classNames;
  std::vector<SString> funcNames;
  std::vector<SString> unitNames;
  std::vector<SString> traceNames;

  bool namesSorted{true};

  // Keep the address of the states (and therefore their contained
  // DepStates) the same, so we can keep pointers to them.
  FSStringToOneNodeT<FuncState> funcState;
  TSStringToOneNodeT<ClassState> classState;
  SStringToOneNodeT<UnitState> unitState;
  SStringToOneNodeT<std::atomic<bool>> cnsChanged;

  SStringToOneNodeT<TraceState> traceState;

  struct Untracked {
    SStringToOneT<BucketSet> untrackedBundles;
    FSStringToOneT<BucketSet> badFuncs;
    TSStringToOneT<BucketSet> badClasses;
    SStringToOneT<BucketSet> badConstants;
  };
  Untracked untracked;

  CopyableAtomic<size_t> totalWorkItems;

  FSStringSet funcsToRemove;

  // Keep AnalysisScheduler moveable
  std::unique_ptr<std::mutex> lock;
};

//////////////////////////////////////////////////////////////////////

// Worklist for handling dependencies within the same analysis job. If
// a class or function updates something, if another class or function
// within the same job has a dependency, that will be re-analyzed
// within the same job. This speeds up convergence by avoiding another
// whole roundtrip through the scheduler.
struct AnalysisWorklist {
  FuncClsUnit next();
  FuncClsUnit peek() const;

  size_t size() const { return list.size(); }

  void schedule(FuncClsUnit);
  void sort();
private:
  hphp_fast_set<FuncClsUnit, FuncClsUnitHasher> in;
  std::deque<FuncClsUnit> list;
};

//////////////////////////////////////////////////////////////////////

// Equivalent of Index, but for an analysis job.
struct AnalysisIndex {
  template<typename T> using V = std::vector<T>;
  template<typename T> using VU = V<std::unique_ptr<T>>;

  using Mode = AnalysisMode;

  AnalysisIndex(AnalysisWorklist&,
                V<AnalysisIndexBundle>,
                V<AnalysisIndexBundle>,
                AnalysisInput::Meta,
                Mode);
  ~AnalysisIndex();

  // Must be called in the worker's init() and fini() functions.
  static void start();
  static void stop();

  void freeze();
  bool frozen() const;

  bool tracking_public_sprops() const;

  std::vector<php::Unit*> units_to_emit() const;

  const php::Unit& lookup_func_unit(const php::Func&) const;
  const php::Unit& lookup_func_original_unit(const php::Func&) const;

  const php::Unit& lookup_class_unit(const php::Class&) const;

  const php::Unit& lookup_unit(SString) const;

  const php::Class* lookup_const_class(const php::Const&) const;

  const php::Class& lookup_closure_context(const php::Class&) const;

  const php::Class* lookup_class(SString) const;

  void for_each_unit_func(const php::Unit&,
                          std::function<void(const php::Func&)>) const;
  void for_each_unit_func_mutable(php::Unit&,
                                  std::function<void(php::Func&)>);

  void for_each_unit_class(const php::Unit&,
                           std::function<void(const php::Class&)>) const;
  void for_each_unit_class_mutable(php::Unit&,
                                   std::function<void(php::Class&)>);

  CompactVector<const php::Func*> lookup_extra_methods(const php::Class&) const;
  CompactVector<php::Func*> lookup_func_closure_invokes(const php::Func&) const;

  Optional<res::Class> resolve_class(SString) const;
  Optional<res::Class> resolve_class(const php::Class&) const;

  Type lookup_constant(SString) const;

  std::vector<std::pair<SString, ClsConstInfo>>
  lookup_class_constants(const php::Class&) const;

  ClsConstLookupResult
  lookup_class_constant(const Type&, const Type&) const;

  ClsTypeConstLookupResult
  lookup_class_type_constant(
    const Type& cls,
    const Type& name,
    const Index::ClsTypeConstLookupResolver& resolver = {}) const;

  ClsTypeConstLookupResult
  lookup_class_type_constant(const php::Class&, SString, ConstIndex) const;

  std::vector<std::pair<SString, ConstIndex>>
  lookup_flattened_class_type_constants(const php::Class&) const;

  PropState lookup_private_props(const php::Class&) const;
  PropState lookup_private_statics(const php::Class&) const;
  PropState lookup_public_statics(const php::Class&) const;

  PropLookupResult lookup_static(Context,
                                 const PropertiesInfo&,
                                 const Type&,
                                 const Type&) const;

  Type lookup_public_prop(const Type&, const Type&) const;

  Index::ReturnType lookup_return_type(MethodsInfo*, res::Func) const;
  Index::ReturnType lookup_return_type(MethodsInfo*,
                                       const CompactVector<Type>&,
                                       const NamedArgNameVec*,
                                       const Type&,
                                       res::Func) const;
  Index::ReturnType lookup_foldable_return_type(const CallContext&) const;

  std::pair<Index::ReturnType, size_t>
  lookup_return_type_raw(const php::Func& f) const;

  CompactVector<Type> lookup_closure_use_vars(const php::Func&) const;
  CompactVector<Type> lookup_closure_use_vars_raw(const php::Func&) const;

  bool func_depends_on_arg(const php::Func&, size_t) const;

  res::Func resolve_func(SString) const;

  res::Func resolve_method(const Type&, SString) const;
  res::Func resolve_ctor(const Type&) const;
  res::Func resolve_func_or_method(const php::Func&) const;

  std::pair<const php::TypeAlias*, bool> lookup_type_alias(SString) const;

  Index::ClassOrTypeAlias lookup_class_or_type_alias(SString) const;

  Slot lookup_iface_vtable_slot(const php::Class&) const;

  PropMergeResult
  merge_static_type(Context,
                    PublicSPropMutations&,
                    PropertiesInfo&,
                    const Type&,
                    const Type&,
                    const Type&,
                    bool checkUB = false,
                    bool ignoreConst = false,
                    bool mustBeReadOnly = false) const;

  void refine_constants(const FuncAnalysisResult&);
  void refine_class_constants(const FuncAnalysisResult&);
  void refine_return_info(const FuncAnalysisResult&);
  void refine_closure_use_vars(const FuncAnalysisResult&);
  void refine_private_props(const ClassAnalysis&);
  void refine_private_statics(const ClassAnalysis&);
  void update_prop_initial_values(const FuncAnalysisResult&);
  void update_type_consts(const ClassAnalysis&);
  void update_bytecode(FuncAnalysisResult&);
  void update_type_aliases(const UnitAnalysis&);

  using Output = extern_worker::Multi<
    extern_worker::Variadic<AnalysisIndexBundle>,
    AnalysisOutput::Meta
  >;
  Output finish();

  struct IndexData;
private:
  std::unique_ptr<IndexData> const m_data;

  void initialize_worklist(const AnalysisInput::Meta&);
  void remember_to_report(const AnalysisInput::Meta&);

  void push_context(const Context&);
  void pop_context();

  bool set_in_type_cns(bool);

  template <typename P, typename G>
  res::Func rfunc_from_dcls(const DCls&, SString, const P&, const G&) const;

  template <typename F, typename I, typename M>
  bool visit_prop_decls(const DCls&,
                        SString,
                        const F&,
                        const I&,
                        const M&) const;

  Type unserialize_type(Type) const;
  Type serialize_type(Type) const;

  Index::ReturnType return_type_for_func(const php::Func&) const;
  ClsConstInfo info_for_class_constant(const php::Class&,
                                       const php::Const&) const;

  void refine_private_propstate(const php::Class&,
                                const PropState&,
                                PropState&) const;

  friend struct AnalysisIndexAdaptor;
};

//////////////////////////////////////////////////////////////////////

struct AnalysisIndexAdaptor : public IIndex {
  explicit AnalysisIndexAdaptor(const AnalysisIndex& index)
    : index{const_cast<AnalysisIndex&>(index)} {}

  bool frozen() const override;

  const php::Unit* lookup_func_unit(const php::Func&) const override;
  const php::Unit* lookup_func_original_unit(const php::Func&) const override;
  const php::Unit* lookup_class_unit(const php::Class&) const override;
  const php::Class* lookup_const_class(const php::Const&) const override;
  const php::Class* lookup_closure_context(const php::Class&) const override;
  const php::Class* lookup_class(SString) const override;

  void for_each_unit_func(const php::Unit&,
                          std::function<void(const php::Func&)>) const override;
  void for_each_unit_func_mutable(php::Unit&,
                                  std::function<void(php::Func&)>) override;
  void for_each_unit_class(const php::Unit&,
                           std::function<void(const php::Class&)>) const override;
  void for_each_unit_class_mutable(php::Unit&,
                                   std::function<void(php::Class&)>) override;

  CompactVector<const php::Func*>
  lookup_extra_methods(const php::Class&) const override;

  Optional<res::Class> resolve_class(SString) const override;
  Optional<res::Class> resolve_class(const php::Class&) const override;

  std::pair<const php::TypeAlias*, bool>
  lookup_type_alias(SString) const override;

  Index::ClassOrTypeAlias lookup_class_or_type_alias(SString) const override;

  res::Func resolve_func_or_method(const php::Func&) const override;
  res::Func resolve_func(SString) const override;
  res::Func resolve_method(Context, const Type&, SString) const override;
  res::Func resolve_ctor(const Type&) const override;

  std::vector<std::pair<SString, ClsConstInfo>>
  lookup_class_constants(const php::Class&) const override;

  ClsConstLookupResult lookup_class_constant(Context,
                                             const Type&,
                                             const Type&) const override;

  ClsTypeConstLookupResult
  lookup_class_type_constant(
    const Type&,
    const Type&,
    const Index::ClsTypeConstLookupResolver& r = {}) const override;

  ClsTypeConstLookupResult
  lookup_class_type_constant(const php::Class&,
                             SString,
                             ConstIndex) const override;

  std::vector<std::pair<SString, ConstIndex>>
  lookup_flattened_class_type_constants(const php::Class&) const override;

  Type lookup_constant(Context, SString) const override;
  bool func_depends_on_arg(const php::Func*, size_t) const override;
  Index::ReturnType
  lookup_foldable_return_type(Context, const CallContext&) const override;
  Index::ReturnType lookup_return_type(Context, MethodsInfo*, res::Func,
                                       Dep d = Dep::ReturnTy) const override;
  Index::ReturnType lookup_return_type(Context,
                                       MethodsInfo*,
                                       const CompactVector<Type>&,
                                       const NamedArgNameVec*,
                                       const Type&,
                                       res::Func,
                                       Dep d = Dep::ReturnTy) const override;

  std::pair<Index::ReturnType, size_t>
  lookup_return_type_raw(const php::Func*) const override;

  CompactVector<Type>
  lookup_closure_use_vars(const php::Func&) const override;
  CompactVector<Type>
  lookup_closure_use_vars_raw(const php::Func&) const override;

  PropState lookup_private_props(const php::Class*, bool m = false) const override;
  PropState lookup_private_statics(const php::Class*, bool m = false) const override;
  PropState lookup_public_statics(const php::Class*) const override;

  PropLookupResult lookup_static(Context,
                                 const PropertiesInfo&,
                                 const Type&,
                                 const Type&) const override;

  Type lookup_public_prop(const Type&, const Type&) const override;

  Slot lookup_iface_vtable_slot(const php::Class*) const override;

  PropMergeResult
  merge_static_type(Context,
                    PublicSPropMutations&,
                    PropertiesInfo&,
                    const Type&,
                    const Type&,
                    const Type&,
                    bool checkUB = false,
                    bool ignoreConst = false,
                    bool mustBeReadOnly = false) const override;

  bool tracking_public_sprops() const override;

private:
  void push_context(const Context&) const override;
  void pop_context() const override;

  bool set_in_type_cns(bool) const override;

  AnalysisIndex& index;
};

//////////////////////////////////////////////////////////////////////

/*
 * Used for collecting all mutations of public static property types.
 */
struct PublicSPropMutations {
  explicit PublicSPropMutations(bool enabled = true);
private:
  friend struct Index;

  struct KnownKey {
    ClassInfo* cinfo;
    SString prop;
    bool operator==(const KnownKey& o) const {
      return cinfo == o.cinfo && prop == o.prop;
    }
    struct Hasher {
      size_t operator()(const KnownKey& k) const {
        return folly::hash::hash_combine(k.cinfo, k.prop);
      }
    };
  };

  using UnknownMap = SStringToOneT<Type>;
  using KnownMap = hphp_fast_map<KnownKey, Type, KnownKey::Hasher>;

  // Public static property mutations are actually rare, so defer allocating the
  // maps until we actually see one.
  struct Data {
    bool m_nothing_known{false};
    UnknownMap m_unknown;
    KnownMap m_known;
  };
  std::unique_ptr<Data> m_data;
  bool m_enabled;

  Data& get();

  void mergeKnown(const ClassInfo* ci, const php::Prop& prop, const Type& val);
  void mergeUnknownClass(SString prop, const Type& val);
  void mergeUnknown(Context);
};

//////////////////////////////////////////////////////////////////////

}
