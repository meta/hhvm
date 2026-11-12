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

#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/lazy-string-data.h"
#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/tracing.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/user-attributes.h"

#include "hphp/runtime/vm/coeffects.h"
#include "hphp/runtime/vm/indexed-string-map.h"
#include "hphp/runtime/vm/iter.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/generics-info.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/util/atomic.h"
#include "hphp/util/atomic-countable.h"
#include "hphp/util/check-size.h"
#include "hphp/util/fixed-vector.h"
#include "hphp/util/ptr.h"

#include <atomic>
#include <utility>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct ActRec;
struct Class;
struct FuncToken;
struct NamedFunc;
struct PreClass;
struct StringData;
struct StructuredLogEntry;
template <typename T> struct AtomicPackedPtrVector;

/*
 * Signature for native functions called by the hhvm using the hhvm
 * calling convention that provides raw access to the ActRec.
 */
using ArFunction = TypedValue* (*)(ActRec* ar);

/*
 * Signature for native functions expecting the platform ABI calling
 * convention. This must always be casted to a proper signature before
 * calling, so make something up to prevent accidental mixing with other
 * function pointer types.
 */
struct NativeArgs; // never defined
using NativeFunction = void(*)(NativeArgs*);

using StaticCoeffectNamesMap = CompactVector<PackedStringPtr>;

using InheritedRetTypesMap = folly::ConcurrentHashMap<FuncId::Int, TypeIntersectionConstraint>;

///////////////////////////////////////////////////////////////////////////////
// EH table.

/*
 * Exception handler table entry.
 */
struct EHEnt {
  Offset m_base;
  Offset m_past;
  int m_iterId;
  int m_parentIndex;
  Offset m_handler;
  Offset m_end;

  EHEnt()
    : m_base()
    , m_past()
    , m_iterId()
    , m_parentIndex()
    , m_handler()
    , m_end()
  {}

  template<class SerDe> void serde(SerDe& sd);
};

///////////////////////////////////////////////////////////////////////////////
/*
 * Metadata about a PHP function or method.
 *
 * The Func class cannot be safely extended, because variable amounts of memory
 * associated with the Func are allocated before and after the actual object.
 *
 * All Funcs are also followed by a variable number of function prologue
 * pointers.  Six are statically allocated as part of the Func object, but more
 * may follow, depending on the value of getMaxNumPrologues().
 *
 *              +--------------------------------+ Func* address
 *              |  Func object                   |
 *              |                                |
 *              |  prologues at end of Func      |
 *              +--------------------------------+ Func* address
 *              |  [additional prologues]        |
 *              +--------------------------------+ high address
 *
 */
struct Func final {
  friend struct FuncEmitter;
  friend struct UnitEmitter;

  /////////////////////////////////////////////////////////////////////////////
  // Types.

  /*
   * Parameter default value info.
   */
  struct ParamInfo {
    enum class Flags {
      InOut,      // Is this an `inout' parameter?
      OutOnly,    // Is this an <<__OutOnly>> parameter? Implies InOut.
      Readonly,   // Is this a `readonly` parameter?
      Variadic,   // Is this a `t...' parameter or a `...t` parameter?
      Optional,   // Marked as `optional` in an abstract method
      Splat,      // Is this a `...t` parameter? (Not set for variadic)
      Named,      // Is this a `named` parameter?
    };

    enum class BuiltinAbi : uint8_t {
      Value,            // HPHP::Value, non-floating point
      FPValue,          // HPHP::Value, floating point (might need a SIMD reg)
      ValueByRef,       // reference to HPHP::Value
      TypedValue,       // HPHP::TypedValue
      TypedValueByRef,  // reference to HPHP::TypedValue (equivalent to Variant)
      InOutByRef,       // reference to HPHP::Value or HPHP::TypedValue (inout)
    };

    ParamInfo();

    bool hasDefaultValue() const;
    bool hasScalarDefaultValue() const;
    bool hasTrivialDefaultValue() const;
    bool isInOut() const;
    bool isOutOnly() const;
    bool isReadonly() const;
    bool isVariadic() const;
    bool isSplat() const;
    bool isOptional() const;
    bool isNamed() const;
    void setFlag(Flags flag);

    template<class SerDe> void serde(SerDe& sd);

    // Flags as defined by the Flags enum.
    uint8_t flags{0};
    // If this is a builtin, how to pass the value to the C++ impl.
    BuiltinAbi builtinAbi;
    // DV initializer funclet offset.
    Offset funcletOff{kInvalidOffset};
    // Set to Uninit if there is no DV, or if there's a nonscalar DV.
    TypedValue defaultValue;
    // Eval-able PHP code.
    LazyStringData phpCode;
    // User-annotated type.
    LazyStringData userType;
    TypeIntersectionConstraint typeConstraints;
    UserAttributeMap userAttributes;
  };

  using ParamInfoVec = VMFixedVector<ParamInfo>;
  using EHEntVec = VMFixedVector<EHEnt>;
  using CoeffectRules = VMFixedVector<CoeffectRule>;

  /////////////////////////////////////////////////////////////////////////////
  // Creation and destruction.

  Func(Unit& unit, const StringData* name, Attr attrs);
  Func(Unit& unit, const StringData* name, Attr attrs,
    const StringData *methCallerCls, const StringData *methCallerMeth);
  ~Func();

  /*
   * Allocate memory for a function, including the variable number of prologues
   * that follow.
   */
  static void* allocFuncMem(int numParams);

  /*
   * Destruct and free a Func*.
   */
  void startDestroy();
  void finishDestroy();

  /*
   * Address of the end of the Func's variable-length memory allocation.
   */
  const void* mallocEnd() const;

  /*
   * Duplicate this function.
   *
   * Funcs are cloned for a number of reasons---most notably, methods on
   * Classes are cloned from the methods defined on their respective
   * PreClasses.
   *
   * We also clone methods from traits when we transclude the trait in its user
   * Classes in repo mode.
   */
  Func* clone(Class* cls, const StringData* name = nullptr) const;

  /*
   * Reset this function's cls and attrs.
   *
   * Used to change the Class scope of a closure method.
   */
  void rescope(Class* ctx);

  /*
   * Free up a PreFunc for re-use as a cloned Func.
   *
   * @requires: isPreFunc()
   */
  void freeClone();

  /*
   * Verify that a Func's data is coherent.
   *
   * FIXME: Currently this method does almost nothing.
   */
  bool validate() const;

  /////////////////////////////////////////////////////////////////////////////
  // FuncId manipulation.

  /*
   * Get this function's ID.
   *
   * We allocate a unique 32-bit ID to almost all Funcs.  The Func* can be
   * retrieved by using this ID as an index into a global vector.  This lets
   * the JIT store references to Funcs more compactly.
   *
   * Funcs which do not represent actual runtime functions (namely, Funcs on
   * PreClasses) are not assigned an ID.
   */
  FuncId getFuncId() const;

  std::shared_ptr<FuncToken> getFuncToken() const;

  /*
   * Get a function ID that is guaranteed not to be reused during the lifetime
   * of the process.
   */
  uint32_t getStableId() const;

private:
  /*
   * Reserve the next available FuncId for `this', and add `this' to the
   * function table.
   */
  void setNewFuncId();

public:
  /*
   * The max FuncId num.
   */
  static FuncId::Int maxFuncIdNum();

  /*
   * Lookup a Func* by its ID.
   */
  static const Func* fromFuncId(FuncId id);

  /*
   * Whether `id' actually keys a Func*.
   */
  static bool isFuncIdValid(FuncId id);

  /////////////////////////////////////////////////////////////////////////////
  // Basic info.                                                        [const]

  /*
   * The Unit the function is defined in.
   */
  Unit* unit() const;

  /*
   * The various Class contexts of a method.
   *
   * cls():       The Class context of the method.  This is usually the Class
   *              which implements the method, but for closure methods (i.e.,
   *              the __invoke() method on a closure object), it is instead the
   *              Class that the Closure object is scoped to.
   *
   * preClass():  The PreClass of the method's cls().  For closures, this still
   *              corresponds to the Closure subclass, rather than to the
   *              scoped Class.
   *
   *              When isFromTrait() is true, preClass() refers to different
   *              entities in repo vs. non-repo mode.  In repo mode, traits are
   *              flattened ahead of time, and preClass() refers to the class
   *              which imported the trait.  In non-repo mode, trait methods
   *              are cloned into trait users, but preClass() will still refer
   *              to the trait which defined the method.
   *
   * baseCls():   The first Class in the inheritance hierarchy which declares
   *              this method.
   *
   * implCls():   The Class which implements the method.  Just like cls(), but
   *              ignores closure scope (so it returns baseCls() for closures).
   *
   * It is possible for cls() to be nullptr on a method---this occurs when a
   * closure method is scoped to a null class context (e.g., if the closure is
   * created in a non-method function scope).  In this case, only the `cls' is
   * changed; the `preClass' and `baseCls' will continue to refer to the
   * PreClass and Class of the closure object.
   *
   * The converse also occurs---a function can have a `cls' (and `baseCls')
   * without being a method.  This happens when a pseudomain is included from a
   * class context.
   *
   * Consequently, none of these methods should be used to test whether the
   * function is a method; for that purpose, see isMethod().
   */
  Class* cls() const;
  PreClass* preClass() const;
  bool hasBaseCls() const;
  Class* baseCls() const;
  Class* implCls() const;

  int sn() const;

  /*
   * The function's short name (e.g., foo).
   */
  const StringData* name() const;
  OptString nameWithClosureName() const;
  StrNR nameStr() const;

  /*
   * A hash for this func that will remain constant across process restarts.
   */
  size_t stableHash() const;

  /*
   * The function's fully class-qualified, name (e.g., C::foo).
   */
  const StringData* fullName() const;
  OptString fullNameWithClosureName() const;
  StrNR fullNameStr() const;

  /*
   * The function's named entity.  Only valid for non-methods.
   *
   * @requires: shared()->m_preClass == nullptr
   */
  NamedFunc* getNamedFunc();
  const NamedFunc* getNamedFunc() const;

  /**
   * meth_caller
   */
  const StringData* methCallerClsName() const;
  const StringData* methCallerMethName() const;

  /////////////////////////////////////////////////////////////////////////////
  // File info.                                                         [const]

  /*
   * The filename where the function was originally defined.
   *
   * In repo mode, we flatten traits into the classes they're used in, so we
   * need this to track the original file for backtraces and errors.
   */
  const StringData* originalUnit() const;

  /*
   * The original filename if it is defined, the unit's filename otherwise.
   */
  const StringData* filename() const;

  /*
   * Start and end line of the function.
   *
   * It'd be nice if these were called lineStart and lineEnd or something, but
   * we're not allowed to have nice things.
   */
  int line1() const;
  int line2() const;

  /*
   * The system- or user-defined doc comment accompanying the function.
   */
  const StringData* docComment() const;

  /////////////////////////////////////////////////////////////////////////////
  // Bytecode.                                                          [const]

  /*
   * Get the function's main entrypoint.
   */
  PC entry() const;
  Offset bclen() const;

  /*
  * Get a hash of the bytecode of this function. Note that this performs an
  * order dependent hash of the actual bytes of the bytecode.
  */
  uint64_t bcHash() const {
    return folly::hash::hash_range(entry(), at(bclen()));
  }

  /*
   * Whether a given PC or Offset (from the beginning of the unit) is within
   * the function's bytecode stream.
   */
  bool contains(PC pc) const;
  bool contains(Offset offset) const;

  /*
   * Convert between PC and Offset from entry().
   */
  PC at(Offset off) const;
  Offset offsetOf(PC pc) const;

  /*
   * Get the Op at `instrOffset'.
   */
  Op getOp(Offset instrOffset) const;

  /*
   * Is there a main or default value entrypoint at the given offset?
   */
  bool isEntry(Offset offset) const;
  bool isDVEntry(Offset offset) const;

  /*
   * Has a DV func entry that is non-scalar or requires a runtime type check.
   */
  bool hasNonTrivialDVFuncEntry() const;

  /*
   * Get the correct entrypoint (whether the main entry or a DV funclet) when
   * `numPositionalsPassed' arguments are passed to the prologue.
   *
   * This is the DV funclet offset of the
   * `numNamedParams + numPositionalsPassed`-th parameter, or the
   * next parameter that has a DV funclet.
   */
  Offset getPrologueEntryForNumPositionals(int numPositionalsPassed) const;
  /*
   * Get the correct entrypoint (whether the main entry or a DV funclet) when
   * `numArgsPassed' arguments are passed to the func entry.
   *
   * This is the DV funclet offset of the numArgsPassed-th parameter, or the
   * next parameter that has a DV funclet.
   *
   * allNamedParamsPassed should be true only if we statically know that we
   * passed all named args for the callee.
   */
  Offset getFuncEntryForNumPositionals(int numPositionalsPassed,
                                       bool allNamedParamsPassed) const;
  /*
   * Get the correct entrypoint (either the main entry, or the chain of named
   * params DV funclets) for the func.
   */
  Offset getNamedParamsFuncEntry() const;

  /////////////////////////////////////////////////////////////////////////////
  // Return type.                                                       [const]

  /*
   * Return type inferred by HHBBC's static analysis. TGen if no data is
   * available.
   */
  RepoAuthType repoReturnType() const;

  /*
   * For async functions, the statically inferred inner type of the returned
   * WH based on HHBBC's analysis.
   */
  RepoAuthType repoAwaitedReturnType() const;

  /*
   * For builtins, whether the return value is returned in registers (as
   * opposed to indirect return, via tvBuiltinReturn).
   *
   * Not well-defined if this function is not a builtin.
   */
  bool isReturnByValue() const;

  /*
   * Builtins can potentially return null when the return type is
   * specified as a reference type. This function returns true if
   * the builtin return type is a reference type.
   * (e.g. ref types like String, Array, Object)
  */
  bool hasUntrustedReturnType() const;

  /*
   * Returns a TypeIntersectionConstraint consisting of all enforced types
   * for the given method, including any additional return types inherited
   * from overridden methods.
   */
  const TypeIntersectionConstraint& returnTypeConstraints() const;

  /*
   * The user-annotated Hack return type.
   */
  const StringData* returnUserType() const;

  /**
   * Returns whether this Func has additional return type constraints
   * inherited from overridden methods.
   */
  bool hasInheritedReturnTypes() const;

  /**
   * Stores the given return types in a side-table keyed by FuncId and
   * marks this Func as having inherited types.
   */
  void storeInheritedReturnTypes(TypeIntersectionConstraint&& tcs);

  /**
   * Returns true if the return type of the given function is inherently
   * trust worthy such as:
   *  - C++ builtins (return type inferred from type hints)
   *  - Memoize wrappers (return type checks are performed in the impl method)
   */
  bool trustReturnType() const;

  /////////////////////////////////////////////////////////////////////////////
  // Parameters.                                                        [const]

  /*
   * Const reference to the parameter info table.
   *
   * ParamInfo objects pulled from the table will also be const.
   */
  const ParamInfoVec& params() const;

  /*
   * Number of parameters (including `...' and named parameters) accepted by
   * the function.
   */
  uint32_t numParams() const;

  /*
   * Number of named parameters accepted by the function.
   */
  uint32_t numNamedParams() const;

  /**
   * Number of positional parameters, not including `...` accepted by the function.
   */
  uint32_t numPositionalParams() const;

  /**
   * Number of positional parameters, including `...` if present.
   */
  uint32_t numPositionalParamsVariadic() const;

  /*
   * Number of parameters, not including `...', accepted by the function.
   */
  uint32_t numNonVariadicParams() const;

  /*
   * Number of required named parameters.
   */
  uint32_t numRequiredNamedParams() const;

  /*
   * Number of required positional parameters.
   */
  uint32_t numRequiredPositionalParams() const;

  /*
   * Number of required parameters, i.e. all parameters starting from
   * the returned position have default value.
   */
  uint32_t numRequiredParams() const;

  /*
   * Whether the function is declared with a `...' parameter (variadic, or splat).
   */
  bool hasVariadicCaptureParam() const;

  /*
   * Whether the function is declared with a `named T...' parameter.
   *
   * Such a parameter has no name, so it declares nothing the body can read. It
   * only means that named arguments matching no named parameter are accepted
   * and discarded, rather than rejected.
   */
  bool hasVariadicNamedParam() const;

  /*
   * Whether the function is declared with a `...T' parameter (splat).
   */
   bool hasSplatParam() const;

   /*
   * Whether the arg-th parameter was declared inout.
   */
  bool isInOut(int32_t arg) const;

  /*
   * Return the raw m_inoutBits field.
   */
  uint64_t inOutBits() const;

  /*
   * Whether the arg-th parameter was declared readonly.
   */
  bool isReadonly(int32_t arg) const;

  /*
   * Whether any of the parameters to this function are inout parameters.
   */
  bool takesInOutParams() const;

  /*
   * Returns the number of inout parameters taken by func.
   */
  uint32_t numInOutParams() const;

  /*
   * Returns the number of inout parameters for the given number of
   * arguments.
   */
  uint32_t numInOutParamsForArgs(int32_t numArgs) const;

   /*
   * Whether the arg-th parameter is named.
   */
  bool isNamed(int32_t arg) const;
  /////////////////////////////////////////////////////////////////////////////
  // Locals, iterators, and stack.                                      [const]

  /*
   * Number of locals, iterators, closure use locals or named locals.
   */
  int numLocals() const;
  int numIterators() const;
  uint32_t numClosureUseLocals() const;
  Id numNamedLocals() const;

  /*
   * Find the integral ID assigned to a named local.
   */
  Id lookupVarId(const StringData* name) const;

  /*
   * Number of initialized locals at FuncEntry.
   */
  uint32_t numFuncEntryInputs() const;

  /*
   * Returns the ID of coeffects and reified generics locals.
   * Requires hasCoeffectRules() and hasReifiedGenerics() respectively
   */
  uint32_t coeffectsLocalId() const;
  uint32_t reifiedGenericsLocalId() const;

  /*
   * Returns the ID of the first closure use local.
   */
  uint32_t firstClosureUseLocalId() const;

  /*
   * Returns the ID of the first regular local, i.e. a first local that is not
   * a parameter, reified generics, coeffects or closure use local.
   */
  uint32_t firstRegularLocalId() const;


  /*
   * Find the name of the local with the given ID.
   */
  const StringData* localVarName(Id id) const;

  /*
   * Array of named locals.  Includes parameter names.
   * May contain nullptrs for unammed locals that mixed in with named ones.
   *
   * Should not be indexed past numNamedLocals() - 1.
   */
  PackedStringPtr const* localNames() const;

  /*
   * Array containing all named param names, sorted lexicographically.
   *
   * Should not be indexed past numNamedParams() - 1.
   */
  PackedStringPtr const* sortedNamedParamNames() const;

  /*
   * Number of stack slots used by locals and iterator cells.
   */
  int numSlotsInFrame() const;

  /*
   * Access to the maximum stack cells this function can use.  This is
   * used for stack overflow checks.
   *
   * The maximum cells for a function includes all its locals, all cells
   * for its iterators, and all temporary eval stack slots. It does not
   * include its own ActRec, because whoever called it must have(+) included
   * the stack slot space reserved for this ActRec.  The reason it must still
   * count its parameter locals is that the caller may or may not pass any of
   * the parameters, regardless of how many are declared.
   *
   *   + Except in a re-entry situation.  That must be handled
   *     specially in bytecode.cpp.
   */
  int maxStackCells() const;

  /*
   * Checks if $this belong to a class that is not a subclass of cls().
   */
  bool hasForeignThis() const;

  void setHasForeignThis(bool);

  void registerInDataMap();
  void deregisterInDataMap();

  /////////////////////////////////////////////////////////////////////////////
  // Definition context.                                                [const]

  /*
   * Is this function a method defined on a class?
   *
   * Note that trait methods may not satisfy isMethod().
   */
  bool isMethod() const;

  /*
   * Was this function imported from a trait?
   *
   * Note that this returns false for a trait method in the trait it was
   * originally declared.
   */
  bool isFromTrait() const;

  /*
   * Is this function declared with `public', `static', or `abstract'?
   */
  bool isPublic() const;
  bool isStatic() const;
  bool isAbstract() const;

  /*
   * Is this function declared as internal to its module?
   */
  bool isInternal() const;

  /*
   * What module does this function belong to?
   */
  const StringData* moduleName() const;

  /*
   * Whether a function is called non-statically. Generally this means
   * isStatic(), but eg static closures are still called with a valid
   * this pointer.
   */
  bool isStaticInPrologue() const;

  /*
   * Whether a method is guaranteed to have a valid this in the body.
   * A method which is !isStatic() || isClosureBody() is guaranteed to
   * be called with a valid this, but closures swap out the closure
   * object for the closure context in the prologue, so may not have
   * a this in the body.
   */
  bool hasThisInBody() const;

  /*
   * Is this Func owned by a PreClass?
   *
   * A PreFunc may be "adopted" by a Class when clone() is called, but only the
   * owning PreClass is allowed to free it.
   */
  bool isPreFunc() const;

  /*
   * Is this func a memoization wrapper?
   */
  bool isMemoizeWrapper() const;

  /*
   * Is this func a memoization wrapper with LSB parameter set?
   */
  bool isMemoizeWrapperLSB() const;

  /*
   * What kind of memoized function is this?
   * NB: MemoizeICType must be a unsigned char in order to enable
   * struct bit packing.
   */
  enum MemoizeICType : unsigned char {
    NoIC = 0,
    KeyedByIC = 1,
    MakeICInaccessible = 2,
    NotKeyedByICAndLeakIC = 3,
  };

  MemoizeICType memoizeICType() const;

  bool isKeyedByImplicitContextMemoize() const;

  /*
   * Is this string the name of a memoize implementation.
   */
  static bool isMemoizeImplName(const StringData*);

  /*
   * Is this function a memoization implementation.
   */
  bool isMemoizeImpl() const;

  /*
   * Assuming this func is a memoization wrapper, the name of the function it is
   * wrapping.
   *
   * Pre: isMemoizeWrapper()
   */
  const StringData* memoizeImplName() const;

  /*
   * Given the name of a memoization wrapper function, return the generated name
   * of the function it wraps. This is static so it can be used in contexts
   * where the actual Func* is not available.
   */
  static const StringData* genMemoizeImplName(const StringData*);

  /*
   * Returns the number of local slots used for the memoization key calculation.
   */
  size_t numKeysForMemoize() const;

  /*
   * Given a meth_caller, return the class name or method name
   */
   static std::pair<const StringData*, const StringData*> getMethCallerNames(
     const StringData* name);

  /////////////////////////////////////////////////////////////////////////////
  // Builtins.                                                          [const]

  /*
   * Is the function a builtin, whether PHP or C++?
   */
  bool isBuiltin() const;

  /*
   * Is this function a C++ builtin (ie HNI function)?.
   *
   * @implies: isBuiltin()
   */
  bool isCPPBuiltin() const;

  /*
   * The function returned by arFuncPtr() takes an ActRec*, unpacks it,
   * and usually dispatches to a nativeFuncPtr() with a specific signature.
   *
   * All C++ builtins have an ArFunction, with no exceptions.
   *
   * Most HNI functions share a single ArFunction, which performs
   * unpacking and dispatch. The exception is HNI functions declared
   * with NeedsActRec, which do not have NativeFunctions, but have unique
   * ArFunctions which do all their work.
   */
  ArFunction arFuncPtr() const;

  /*
   * The nativeFuncPtr is a type-punned function pointer to the unerlying
   * function which takes the actual argument types, and does the actual work.
   *
   * These are the functions with names prefixed by f_.
   *
   * All C++ builtins have NativeFunctions, with the ironic exception of HNI
   * functions declared with NeedsActRec.
   */
  NativeFunction nativeFuncPtr() const;

  /////////////////////////////////////////////////////////////////////////////
  // Closures.                                                          [const]

  /*
   * Is this function the body (i.e., __invoke() method) of a Closure object?
   *
   * (All PHP anonymous functions are Closure objects.)
   */
  bool isClosureBody() const;

  /////////////////////////////////////////////////////////////////////////////
  // Resumables.                                                        [const]

  /*
   * Is this function asynchronous?  (May also be a generator.)
   */
  bool isAsync() const;

  /*
   * Is this function a generator?  (May also be async.)
   */
  bool isGenerator() const;

  /*
   * Is this function a generator which yields both key and value?
   *
   * @implies: isGenerator()
   */
  bool isPairGenerator() const;

  /*
   * @returns: !isGenerator() && isAsync()
   */
  bool isAsyncFunction() const;

  /*
   * @returns: isGenerator() && !isAsync()
   */
  bool isNonAsyncGenerator() const;

  /*
   * @returns: isGenerator() && isAsync()
   */
  bool isAsyncGenerator() const;

  /*
   * Is this a resumable function?
   *
   * @returns: isGenerator() || isAsync()
   */
  bool isResumable() const;

  /////////////////////////////////////////////////////////////////////////////
  // Coeffects.                                                        [const]

  /*
   * Returns the runtime representation of coeffects
   */
  RuntimeCoeffects requiredCoeffects() const;
  RuntimeCoeffects coeffectEscapes() const;

  /*
   * Sets required coeffects
   */
  void setRequiredCoeffects(RuntimeCoeffects);

  /*
   * Names of the static coeffects on the function
   * Used for reflection
   */
  StaticCoeffectNamesMap staticCoeffectNames() const;

  /*
   * Does this function use coeffects local to store its ambient coeffects?
   */
  bool hasCoeffectsLocal() const;

  /*
   * Does this function have coeffect rules?
   */
  bool hasCoeffectRules() const;

  /*
   * List of rules for enforcing coeffects
   */
  const CoeffectRules& getCoeffectRules() const;

  /////////////////////////////////////////////////////////////////////////////
  // Methods.                                                           [const]

  /*
   * Index of this function in the method table of its Class.
   */
  Slot methodSlot() const;

  /*
   * Whether this function has a private implementation on a parent class.
   */
  bool hasPrivateAncestor() const;

  /////////////////////////////////////////////////////////////////////////////
  // Magic methods.                                                     [const]

  /*
   * Is this a compiler-generated function?
   *
   * This includes special methods like 86pinit and 86sinit as well
   * as all closures.
   */
  bool isGenerated() const;

  /*
   * Is `name' the name of a special initializer function?
   */
  static bool isSpecial(const StringData* name);

  /////////////////////////////////////////////////////////////////////////////
  // Persistence.                                                       [const]

  /*
   * Whether this function is uniquely named across the codebase.
   *
   * It's legal in PHP to define multiple functions in different pseudomains
   * with the same name, so long as both are not required in the same request.
   *
   * Note that if EvalJitEnableRenameFunction is set, no Func is unique.
   */
  bool isUnique() const;

  /*
   * Whether we can load this function once and persist it across requests.
   *
   * Persistence is possible when a Func is defined in a pseudomain that has no
   * side-effects (except other persistent definitions).
   *
   * @implies: isUnique()
   */
  bool isPersistent() const;

  bool isInterceptable() const;

  /*
   * Given that func would be called when func->name() is invoked on cls,
   * determine if it would also be called when invoked on any descendant
   * of cls.
   */
  bool isImmutableFrom(const Class* cls) const;

  /////////////////////////////////////////////////////////////////////////////
  // Other attributes.                                                  [const]

  /*
   * Get the system and coeffect attributes of the function.
   */
  Attr attrs() const;

  /*
   * Get the user-declared attributes of the function.
   */
  const UserAttributeMap& userAttributes() const;

  /*
   * Get a dict containing user attributes of the function.
   */
  Array userAttributesArray() const;

  /*
   * Whether to ignore this function's frame in backtraces.
   */
  bool isNoInjection() const;

  /*
   * Whether this function's frame should be skipped when searching for context
   * (e.g., array_map evaluates its callback in the context of its caller).
   */
  bool isSkipFrame() const;

  /*
   * Whether this function's frame should be skipped with searching for a
   * context for array provenance
   */
  bool isProvenanceSkipFrame() const;

  /*
   * Whether the function can be constant-folded at callsites where it is
   * passed constant arguments.
   */
  bool isFoldable() const;

  /*
   * Supports async eager return optimization?
   */
  bool supportsAsyncEagerReturn() const;

  /*
   * Is this async function guaranteed not to suspend?
   */
  bool isEagerOnly() const;

  /*
   * Is this func allowed to be called dynamically?
   */
  bool isDynamicallyCallable() const;

  /*
   * If this function is called dynamically should we raise sampled warnings?
   *
   * N.B. When errors are enabled for dynamic calls this overrides that behavior
   *      for functions which specify it.
   */
  Optional<int64_t> dynCallSampleRate() const;

  /*
   * Is this a meth_caller func?
   */
  bool isMethCaller() const;

  /*
   * Indicates that a function does not make any explicit calls to other PHP
   * functions.  It may still call other user-level functions via re-entry
   * (e.g., for autoload), and it may make calls to builtins using FCallBuiltin.
   */
  bool isPhpLeafFn() const;

  /*
   * Does this function has reified generics?
   */
  bool hasReifiedGenerics() const;
  /*
   * Does this function have named params?
   */
  bool hasNamedParams() const;

  /*
   * Does this function have any non-required named parameters?
   */
  bool hasOptionalNamedParameters() const;

  /*
   * Returns GenericsInfo which contains information about each generic
   * parameter defined on this class whether it is reified, or contains
   * the soft or warn annotation.
   *
   * NB: This getter should only be called when this func is known to
   * contain extended data which is where the GenericsInfo struct lives.
   * This can be checked by testing the hasReifiedGenericsInfo flag.
   */
  const GenericsInfo& getGenericsInfo() const;

  /*
   * Does this function wrap a builtin?
   */
  Func* unwrap();

  /////////////////////////////////////////////////////////////////////////////
  // Unit table entries.                                                [const]

  const EHEntVec& ehtab() const;

  /*
   * Find the first EHEnt that covers a given offset, or return null.
   */
  const EHEnt* findEH(Offset o) const;

  /*
   * Same as non-static findEH(), but takes as an operand any ehtab-like
   * container.
   */
  template<class Container>
  static const typename Container::value_type*
  findEH(const Container& ehtab, Offset o);

  bool shouldSampleJit() const { return m_allFlags.m_shouldSampleJit; }

  /////////////////////////////////////////////////////////////////////////////
  // JIT data.

  /*
   * Get, set and reset the function body code pointer.
   */
  jit::TCA getFuncEntry() const;
  void setFuncEntry(jit::TCA funcEntry);
  void resetFuncEntry();

  /*
   * Get and set the `index'-th function prologue.
   */
  uint8_t* getPrologue(int index) const;
  void setPrologue(int index, unsigned char* tca);

  /*
   * Number of prologues allocated for the function.
   */
  int numPrologues() const;

  /*
   * Reset a specific prologue, or all prologues.
   */
  void resetPrologue(int numParams);

  /*
   * Bump/reset JIT request count, which counts the number of times the function
   * is being considered for compilation.
   */
  uint8_t incJitReqCount() const;
  uint8_t readJitReqCount() const;
  void resetJitReqCount() const;

  /////////////////////////////////////////////////////////////////////////////
  // Pretty printer.                                                    [const]

  struct PrintOpts {
    PrintOpts()
      : name(true)
      , metadata(true)
      , startOffset(0)
      , stopOffset(kInvalidOffset)
      , showLines(true)
      , indentSize(1)
    {}

    PrintOpts& noName() {
      name = false;
      return *this;
    }

    PrintOpts& noMetadata() {
      metadata = false;
      return *this;
    }

    PrintOpts& noBytecode() {
      startOffset = kInvalidOffset;
      stopOffset = kInvalidOffset;
      return *this;
    }

    PrintOpts& range(Offset start, Offset stop) {
      startOffset = start;
      stopOffset = stop;
      return *this;
    }

    PrintOpts& noLineNumbers() {
      showLines = false;
      return *this;
    }

    PrintOpts& indent(int i) {
      indentSize = i;
      return *this;
    }

    bool name;
    bool metadata;
    Offset startOffset;
    Offset stopOffset;
    bool showLines;
    int indentSize;
  };

  void prettyPrint(std::ostream& out, const PrintOpts& = PrintOpts()) const;

  /*
   * Print function attributes to out.
   */
  static void print_attrs(std::ostream& out, Attr attrs);


  /////////////////////////////////////////////////////////////////////////////
  // Other methods.
  //
  // You should avoid adding methods to this section.  If the logic you're
  // implementing is specific to a particular subsystem, define it as a helper
  // there instead.
  //
  // If you absolutely must add more methods to Func here, just follow these
  // simple guidelines:
  //
  //    (1) Don't add more methods to Func here.

  /*
   * Intercept hook flag.
   */
  bool maybeIntercepted() const;
  void setMaybeIntercepted();

  /*
   * When function call based coverage is enabled for the current request,
   * records a call to `this`. The no check version asserts that function
   * coverage has already been enabled and the function is both eligible to be
   * covered and has not yet been seen.
   */
  void recordCall() const;
  void recordCallNoCheck() const;

  /*
   * EnableCoverage enables recording of called functions for the current
   * request.
   */
  static void EnableCoverage();

  /*
   * GetCoverage returns a keyset of called functions and disables further
   * coverage for the current request until reenabled by EnableCoverage.
   */
  static Array GetCoverage();

  /*
   * RDS based counter (uint32_t) that when zero indicates coverage is disabled
   * and when non-zero indicates an index which can be used to short circuit
   * tests that functions have been covered.
   */
  static rds::Handle GetCoverageIndex();

  /*
   * Get an RDS counter (uint32_t) that can be compared against GetCoverageIndex
   * to determine if the function has been covered in the current request.
   */
  rds::Handle getCoverageHandle() const;

  /////////////////////////////////////////////////////////////////////////////
  // Debugger.
  /*
   * Return an RDS handle that when initialized, indicates that this function
   * needs to be run in the interpreter so that the debugger hooks are run.
   * Once the handle is initialized, it is not reset
   * for remainder of the request.
   */
  rds::Handle debuggerIntrSetHandle() const;
  rds::Link<bool, rds::Mode::Normal> debuggerIntrSetLink() const;

  /*
   * The handle must be bound before inserting debugger interrupt checks
   * in a function and before resolving any breakpoints in the function.
   */
  void ensureDebuggerIntrSetLinkBound() const;

  /////////////////////////////////////////////////////////////////////////////
  // Public setters.
  //
  // TODO(#4504609): These setters are only used by Class at Class creation
  // time.  We should refactor the creation path into a separate friend module
  // to avoid this garbage.
  //
  // Having public setters here should be avoided, so try not to add any.

  void setAttrs(Attr attrs);
  void setBaseCls(Class* baseCls);
  void setHasPrivateAncestor(bool b);
  void setMethodSlot(Slot s);
  void setGenerated(bool b);

  /////////////////////////////////////////////////////////////////////////////
  // Offset accessors.                                                 [static]

#define OFF(f)                          \
  static constexpr ptrdiff_t f##Off() { \
    return offsetof(Func, m_##f);       \
  }
  OFF(attrs)
  OFF(requiredCoeffects)
  OFF(name)
  OFF(maxStackCells)
  OFF(paramCounts)
  OFF(prologueTable)
  OFF(inoutBits)
  OFF(shared)
  OFF(unit)
  OFF(funcEntry)
  OFF(allFlags)
#undef OFF

  static constexpr ptrdiff_t clsOff() {
    return offsetof(Func, m_u.m_cls.m_impl);
  }

  static constexpr ptrdiff_t methCallerMethNameOff() {
    return offsetof(Func, m_u.m_methCaller.m_methName);
  }

  static constexpr ptrdiff_t methCallerClsNameOff() {
    return offsetof(Func, m_u.m_methCaller.m_clsName);
  }

  static constexpr ptrdiff_t sharedAllFlags() {
    return offsetof(SharedData, m_allFlags);
  }

  static constexpr ptrdiff_t extendedSharedOriginalModuleName() {
    return offsetof(ExtendedSharedData, m_originalModuleName);
  }

  static uint32_t reifiedGenericsMask() {
    ExtendedSharedData::Flags mask;
    mask.m_allFlags = 0;
    mask.m_hasReifiedGenerics = true;
    return mask.m_allFlags;
  }

  static uint32_t hasExtendedSharedDataMask() {
    ExtendedSharedData::Flags mask;
    mask.m_allFlags = 0;
    mask.m_hasExtendedSharedData = true;
    return mask.m_allFlags;
  }

  bool hasExtendedSharedData() const {
    return extShared();
  }

  /////////////////////////////////////////////////////////////////////////////
  // Lookup                                                            [static]

  /*
   * Define `func' for this request by initializing its RDS handle.
   */
  static void def(Func* func);

  /*
   * Look up the defined Func in this request with name `name'
   * Return nullptr if the function is not yet defined in this request.
   */
  static Func* lookup(const StringData* name);

  /*
   * Look up, or autoload and define, the Func in this request with name `name',
   * or with the name mapped to the NamedFunc `ne'.
   *
   * @requires: NamedFunc::get(name) == ne
   */
  static Func* load(const NamedFunc* ne, const StringData* name);
  static Func* load(const StringData* name);

  /*
   * Same as Func::load but also checks for module boundary violations
   */
  static Func* resolve(const NamedFunc* ne, const StringData* name,
                       const Func* callerFunc);
  static Func* resolve(const StringData* name, const Func* callerFunc);

  /*
   * Lookup the builtin in this request with name `name', or nullptr if none
   * exists. This does not access RDS so it is safe to use from within the
   * compiler. Note that does not mean imply that the name binding for the
   * builtin is immutable. The builtin could be renamed or intercepted.
   */
  static Func* lookupBuiltin(const StringData* name);

  /////////////////////////////////////////////////////////////////////////////
  // SharedData.

private:
  using NamedLocalsMap = IndexedStringMap<PackedStringPtr, Id>;

  using BCPtr = TokenOrPtr<unsigned char>;
  using LineTablePtr = TokenOrPtr<LineTable>;

  // Some 16-bit values in SharedData are stored as small deltas if they fit
  // under this limit.  If not, they're set to the limit value and an
  // ExtendedSharedData will be allocated for the full-width field.
  static constexpr auto kSmallDeltaLimit = uint16_t(-1);

  /*
   * Properties shared by all clones of a Func.
   */
  struct SharedData : AtomicCountable {
    SharedData(BCPtr bc, Offset bclen, PreClass* preClass,
               int sn, int line1, int line2, bool isPhpLeafFn);
    ~SharedData();

    /*
     * Interface for AtomicCountable.
     */
    void atomicRelease();

    Offset bclen() const;

    /*
     * Data fields are packed to minimize size.  Try not to add anything new
     * here or reorder anything.
     */
    // (There's a 32-bit integer in the AtomicCountable base class here.)
    LockFreePtrWrapper<BCPtr> m_bc;
    PreClass* m_preClass;
    int m_line1;
    ParamInfoVec m_params;
    NamedLocalsMap m_localNames;
    EHEntVec m_ehtab;
    StaticCoeffectNamesMap m_staticCoeffectNames;

    /*
     * Up to 16 bits.
     */
    union Flags {
      struct {
        bool m_isClosureBody : 1;
        bool m_isAsync : 1;
        bool m_isGenerator : 1;
        bool m_isPairGenerator : 1;
        bool m_isGenerated : 1;
        bool m_hasExtendedSharedData : 1;
        bool m_returnByValue : 1; // only for builtins
        bool m_isUntrustedReturnType : 1;  // applicable for builtins
        bool m_isMemoizeWrapper : 1;
        bool m_isMemoizeWrapperLSB : 1;
        MemoizeICType m_memoizeICType : 2;
        bool m_isPhpLeafFn : 1;
        bool m_hasReifiedGenerics : 1;
      };
      uint16_t m_allFlags;
    };
    static_assert(sizeof(Flags) == sizeof(uint16_t));

    Flags m_allFlags;

    uint16_t m_sn;

    LazyStringData m_retUserType;
    UserAttributeMap m_userAttributes;
    // The link can be bound for const Func.
    mutable rds::Link<bool, rds::Mode::Normal> m_funcHasDebuggerIntr;
    PackedStringPtr m_originalUnit;
    TypeIntersectionConstraint m_retTypeConstraints;
    RepoAuthType m_repoReturnType;
    RepoAuthType m_repoAwaitedReturnType;

    /*
     * The `line2' are likely to be small, particularly relative to m_line1,
     * so we encode each as a 16-bit difference.
     *
     * If the delta doesn't fit, we need to have an ExtendedSharedData to hold
     * the real values---in that case, the field here that overflowed is set to
     * kSmallDeltaLimit and the corresponding field in ExtendedSharedData will
     * be valid.
     */
    uint16_t m_line2Delta;

    /**
     * bclen is likely to be small. So we encode each as a 16-bit value
     *
     * If the value doesn't fit, we need to have an ExtendedSharedData to hold
     * the real values---in that case, the field here that overflowed is set to
     * kSmallDeltaLimit and the corresponding field in ExtendedSharedData will
     * be valid.
     */
    uint16_t m_bclenSmall;

    uint16_t m_numLocals;
    uint16_t m_numIterators;

    mutable LockFreePtrWrapper<VMCompactVector<LineInfo>> m_lineMap;
    mutable LockFreePtrWrapper<LineTablePtr> m_lineTable;
  };
  static_assert(CheckSize<SharedData, 152>(), "");

  /*
   * If this Func represents a native function or is exceptionally large
   * (line count or bytecode size), it requires extra information that most
   * Funcs don't need, so it's SharedData is actually one of these extended
   * SharedDatas.
   */
  struct ExtendedSharedData : SharedData {
    template<class... Args>
    explicit ExtendedSharedData(Args&&... args)
    : SharedData(std::forward<Args>(args)...)
    {
      m_allFlags.m_hasExtendedSharedData = true;
    }
    ExtendedSharedData(const ExtendedSharedData&) = delete;
    ExtendedSharedData(ExtendedSharedData&&) = delete;
    ~ExtendedSharedData();

    ArFunction m_arFuncPtr;
    NativeFunction m_nativeFuncPtr;
    GenericsInfo m_genericsInfo;
    CoeffectRules m_coeffectRules;
    Offset m_bclen;  // Only read if SharedData::m_bclen is kSmallDeltaLimit
    int m_line2;    // Only read if SharedData::m_line2 is kSmallDeltaLimit
    int m_sn;       // Only read if SharedData::m_sn is kSmallDeltaLimit
    RuntimeCoeffects m_coeffectEscapes{RuntimeCoeffects::none()};
    // (2 bytes unused)
    int64_t m_dynCallSampleRate;
    PackedStringPtr m_docComment;
    PackedStringPtr m_originalModuleName;
    // Initialized by Func::finishedEmittingParams. Named parameter names live
    // in the internal storage of
    // `m_localNames[0 : m_namedParamCount - 1]`.
    uint32_t m_namedParamCount;
  };
  static_assert(CheckSize<ExtendedSharedData, 224>(), "");

  /*
   * SharedData accessors for internal use.
   */
  const SharedData* shared() const { return m_shared.get(); }
        SharedData* shared()       { return m_shared.get(); }

  /*
   * Returns ExtendedSharedData if we have one, or else a nullptr.
   */
  const ExtendedSharedData* extShared() const;
        ExtendedSharedData* extShared();

  /*
  * We store 'detailed' line number information on a table on the side, because
  * in production modes for HHVM it's generally not useful (which keeps Func
  * smaller in that case)---this stuff is only used for the debugger, where we
  * can afford the lookup here.  The normal Func m_lineMap is capable of
  * producing enough line number information for things needed in production
  * modes (backtraces, warnings, etc).
  */

  struct ExtendedLineInfo {
    SourceLocTable sourceLocTable;

    /*
    * Map from source lines to a collection of all the bytecode ranges the line
    * encompasses.
    *
    * The value type of the map is a list of offset ranges, so a single line
    * with several sub-statements may correspond to the bytecodes of all of the
    * sub-statements.
    *
    * May not be initialized.  Lookups need to check if it's empty() and if so
    * compute it from sourceLocTable.
    */
    LineToOffsetRangeVecMap lineToOffsetRange;
  };

  using ExtendedLineInfoCache = tbb::concurrent_hash_map<
    const SharedData*,
    ExtendedLineInfo,
    pointer_hash<SharedData>
  >;

  static ExtendedLineInfoCache s_extendedLineInfo;

  /////////////////////////////////////////////////////////////////////////////
  // Internal methods.
  //
  // These are all used at emit-time, and should be outsourced to FuncEmitter.

private:
  Func(const Func&) = default;  // used for clone()
  Func& operator=(const Func&) = delete;
  void init(int numPositionalParams);
  void initProloguesAndFuncEntry(int numPositionalParams);
  void setFullName();
  void finishedEmittingParams(std::vector<ParamInfo>& pBuilder);
  void setNamedFunc(const NamedFunc*);
  const TypeIntersectionConstraint& lookupInheritedReturnTypes() const;

  PC loadBytecode();

  /////////////////////////////////////////////////////////////////////////////
  // Internal types.

  struct ClonedFlag {
    ClonedFlag() {}
    ClonedFlag(const ClonedFlag&) {}
    ClonedFlag& operator=(const ClonedFlag&) = delete;

    std::atomic_flag flag = ATOMIC_FLAG_INIT;
  };

  /*
   * Wrapper around std::atomic<Attr> that pretends like it's not atomic.
   *
   * Func::m_attrs is only accessed by multiple threads in the closure scoping
   * process for Closure classes, which is synchronized in Class::rescope().
   * This wrapper is just to make m_attrs copy-constructible, and there should
   * never be a race when copying.
   */
  struct AtomicAttr {
    AtomicAttr() {}
    explicit AtomicAttr(Attr attrs) : m_attrs{attrs} {}

    AtomicAttr(const AtomicAttr& o)
      : m_attrs{o.m_attrs.load(std::memory_order_acquire)}
    {}

    AtomicAttr& operator=(Attr attrs) {
      m_attrs.store(attrs, std::memory_order_release);
      return *this;
    }

    /* implicit */ operator Attr() const {
      return m_attrs.load(std::memory_order_acquire);
    }

  private:
    std::atomic<Attr> m_attrs;
  };

public:

  /////////////////////////////////////////////////////////////////////////////
  // Atomic Flags.

  enum Flags : uint8_t {
    None             = 0,
    Optimized        = 1 << 0,
    Locked           = 1 << 1,
    MaybeIntercepted = 1 << 2,
    LockedForPrologueGen = 1 << 3,
    // A function is marked zombie before the unit or the class of the function
    // is treadmilled for destruction.
    Zombie = 1 << 4,
    LockedForAsyncJit = 1 << 5,
    WasCalled = 1 << 6,
  };

 /*
  * Wrapper around std::atomic<uint8_t> that enables it to be
  * copy constructable,
  */
  struct AtomicFlags {
    AtomicFlags() {}

    AtomicFlags(const AtomicFlags&) {}
    AtomicFlags& operator=(const AtomicFlags&) = delete;

    bool set(Flags flags) {
      auto const prev = m_flags.fetch_or(flags, std::memory_order_acq_rel);
      return prev & flags;
    }

    bool unset(Flags flags) {
      auto const prev =
        m_flags.fetch_and(~uint8_t(flags), std::memory_order_acq_rel);
      return prev & flags;
    }

    bool check(Flags flags) const {
      return m_flags.load(std::memory_order_acquire) & flags;
    }

    std::atomic<uint8_t> m_flags{Flags::None};
  };

  inline AtomicFlags& atomicFlags() const {
    return m_atomicFlags;
  }

  inline AtomicFlags& atomicFlags() {
    return m_atomicFlags;
  }

  union AllFlags {
   struct {
     bool m_isPreFunc : 1;
     bool m_hasPrivateAncestor : 1;
     bool m_shouldSampleJit : 1;
     bool m_hasForeignThis : 1;
     bool m_registeredInDataMap : 1;
     bool m_hasNamedParams : 1;
     // 2 free bits, and there are some more in AtomicFlags.
   };
   uint8_t m_allFlags;
  };
  static_assert(sizeof(AllFlags) == sizeof(uint8_t));

  static uint8_t hasNamedParamsMask() {
    AllFlags mask;
    mask.m_allFlags = 0;
    mask.m_hasNamedParams = true;
    return mask.m_allFlags;
  }

  /////////////////////////////////////////////////////////////////////////////
  // Code locations.                                                    [const]

  /*
   * Get the line number corresponding to `offset'.
   *
   * Return -1 if not found.
   */
  int getLineNumber(Offset offset) const;

  /*
   * Get the SourceLoc corresponding to `offset'.
   *
   * Return false if not found, else true.
   */
  bool getSourceLoc(Offset offset, SourceLoc& sLoc) const;

  /*
   * Get the Offset range(s) corresponding to `offset'.
   *
   * Return false if not found, else true.
   */
  bool getOffsetRange(Offset offset, OffsetRange& range) const;

  void setLineTable(LineTable);
  void setLineTable(LineTablePtr::Token);

  void stashExtendedLineTable(SourceLocTable table) const;

  const SourceLocTable& getLocTable() const;

  LineToOffsetRangeVecMap getLineToOffsetRangeVecMap() const;

  const LineTable* getLineTable() const;
  LineTable getOrLoadLineTableCopy() const;

private:
  const LineTable& getOrLoadLineTable() const;

  /////////////////////////////////////////////////////////////////////////////
  // Constants.

private:
  static constexpr int kMagic = 0xba5eba11;
  static constexpr PackedStringPtr::storage_type kNeedsFullName =
    std::numeric_limits<PackedStringPtr::storage_type>::max();

public:
  enum class FuncLookupResult {
    None,
    Exact,
    Maybe,
  };

  struct FuncLookup {
    FuncLookupResult tag;
    const Func* func;
  };
  static const FuncLookup lookupKnownMaybe(const StringData* name, const Unit*  unit);
  static const Func* lookupKnown(const StringData* name, const Unit* unit);
  static const FuncLookup maybe(const Func* func) {
    return FuncLookup { FuncLookupResult::Maybe, func };
  }
  static const FuncLookup exact(const Func* func) {
    return FuncLookup { FuncLookupResult::Exact, func };
  }
  static const FuncLookup& none() {
      static FuncLookup lookup { FuncLookupResult::None, nullptr };
      return lookup;
  }

  // Use by m_inoutBits
  static constexpr uint32_t kInoutFastCheckBits = 31;

  static std::atomic<bool>     s_treadmill;
  static std::atomic<uint32_t> s_totalClonedClosures;

  /////////////////////////////////////////////////////////////////////////////
  // Data members.
  //
  // The fields of Func are organized in reverse order of frequency of use.
  // Do not re-order without checking perf!

private:
#ifndef NDEBUG
  // For asserts only.
  int m_magic;
#endif
  jit::AtomicLowTCA m_funcEntry{nullptr};
  mutable AtomicPackedPtr<const StringData> m_fullName{nullptr};
  PackedStringPtr m_name{nullptr};

  struct ClsOrMethCaller {
    struct Cls {
      // The first Class in the inheritance hierarchy that declared this method.
      // Note that this may be an abstract class that did not provide an
      // implementation.
      PackedPtr<Class> m_base{nullptr};
      // Class that provided this method implementation
      AtomicPackedPtr<Class> m_impl{nullptr};

      Cls() {}

      Cls(const Cls& o) {
        m_base = o.m_base;
        m_impl = o.m_impl;
      }

      Cls& operator=(const Cls& o) {
        m_base = o.m_base;
        m_impl = o.m_impl;
        return *this;
      }
    };
    struct MethCaller {
      PackedStringPtr m_methName;
      // Class name provided by meth_caller()
      PackedStringPtr m_clsName;
    };

    // The first part of the union is valid if !isMethCaller
    // and the second part is valid if isMethCaller
    union {
      Cls m_cls;
      MethCaller m_methCaller;
    };

    ClsOrMethCaller() {
      m_cls.m_base = nullptr;
      m_cls.m_impl = nullptr;
    }

    ClsOrMethCaller(const ClsOrMethCaller& o) {
      m_cls = o.m_cls;
    }
  };

  ClsOrMethCaller::Cls& clsData() {
    assertx(!isMethCaller());
    return m_u.m_cls;
  }

  const ClsOrMethCaller::Cls& clsData() const {
    assertx(!isMethCaller());
    return m_u.m_cls;
  }

  ClsOrMethCaller::MethCaller& methCallerData() {
    assertx(isMethCaller());
    return m_u.m_methCaller;
  }

  const ClsOrMethCaller::MethCaller& methCallerData() const {
    assertx(isMethCaller());
    return m_u.m_methCaller;
  }

  ClsOrMethCaller m_u;

  union {
    Slot m_methodSlot{0};
    PackedPtr<const NamedFunc> m_namedFunc;
  };

  mutable ClonedFlag m_cloned;
  mutable AtomicFlags m_atomicFlags;

  AllFlags m_allFlags;

  // Number of times the function has been considered in `shouldTranslate()`
  // when Eval.JitLiveThreshold or Eval.JitProfileThreshold is set. The counter
  // is reset after PGO, so count for live translation only increases when
  // optimized code doesn't cover the function sufficiently.
  mutable CopyableAtomic<uint8_t> m_jitReqCount{0};

  RuntimeCoeffects m_requiredCoeffects{RuntimeCoeffects::none()};
  int16_t m_maxStackCells{0};
  Unit* const m_unit;
  AtomicSharedPtr<SharedData> m_shared;
  // The lower 31 bits represent inout-ness of the corresponding parameter. The
  // highest bit is set if there is an inout parameter beyond the 0..31 range.
  // Initialized by Func::finishedEmittingParams.
  uint32_t m_inoutBits{0};
  // Initialized by Func::finishedEmittingParams.  The least significant bit is
  // 1 if the last param is not variadic; the 31 most significant bits are the
  // total number of params (including the variadic param).
  uint32_t m_paramCounts{0};
  AtomicAttr m_attrs;
  // This must be the last field declared in this structure, and the Func class
  // should not be inherited from.
  jit::AtomicLowTCA m_prologueTable[1];
};
static_assert(CheckSize<Func, debug ? 72 : 64>(), "");

///////////////////////////////////////////////////////////////////////////////

/*
 * Log meta-information about func. Records attributes, number of locals,
 * parameters, static locals, class ref slots, frame cells, high watermark,
 * and iterators. Does not record function name or class.
 */
void logFunc(const Func* func, StructuredLogEntry& ent);

inline tracing::Props traceProps(const Func* f) {
  return tracing::Props{}.add("func_name", f->fullName());
}

/*
 * Throw an exception that func cannot be converted to type.
 */
[[noreturn]] void invalidFuncConversion(const char* type);

///////////////////////////////////////////////////////////////////////////////
// Bytecode

unsigned char* allocateBCRegion(const unsigned char* bc, size_t bclen);
void freeBCRegion(const unsigned char* bc, size_t bclen);

///////////////////////////////////////////////////////////////////////////////

}

#define incl_HPHP_VM_FUNC_INL_H_
#include "hphp/runtime/vm/func-inl.h"
#undef incl_HPHP_VM_FUNC_INL_H_
