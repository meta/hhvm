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

#include <string>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/*
 * Special properties on PHP classes, functions, and properties.
 *
 * Attr unions are stored as integers in .hhbc repositories, so incompatible
 * changes here require a schema version bump.
 *
 * NOTE: Make sure to keep this in sync with HHAS_ATTRS in as-base-hhas.h so it
 * prints properly.
 *
 * TODO(#4513748): We're almost out of space in Attr---in fact, we already have
 * too many Attrs to fit in Class, which packs them into only 28 bits.  There's
 * no reason to share Attrs among unrelated objects, so we should really have
 * three different Attr types.
 */
#define ATTR_BITS                                                              \
                                         /* class | property | method  */      \
                                         /*       |          |         */      \
  /* Class forbids dynamic properties? */                                      \
  ATTR(ForbidDynamicProps, 0)            /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Indicates that this property cannot be initialized on an ObjectData by    \
     simply memcpy-ing from the initializer vector. */                         \
  ATTR(DeepInit, 0)                      /*       |    X     |         */      \
                                         /*       |          |         */      \
  /* Indicates if a given method has any return types inherited from its      \
     parents. */                                                               \
  ATTR(HasInheritedReturnTypes, 0)       /*       |          |    X    */      \
  /* Method visibility.  The relative ordering of these is important.          \
     N.B. the values are overlayed with some of the no-override bits for magic \
     class methods (next), since they don't apply to classes. */               \
  ATTR(Public, 1)                        /*       |    X     |    X    */      \
  ATTR(Protected, 2)                     /*       |    X     |    X    */      \
  ATTR(Private, 3)                       /*       |    X     |    X    */      \
                                         /*       |          |         */      \
  /* N.B.: AttrEnum and AttrStatic overlap! But they can't be set on the       \
     same things.                                                              \
     Is this class an enum? */                                                 \
  ATTR(Enum, 4)                          /*    X  |          |         */      \
  /* Was this property's initial value supplied by the emitter (rather than a  \
     user). System provided initial values can be modified to match the        \
     property's type-hint. */                                                  \
  ATTR(SystemInitialValue, 5)            /*       |    X     |         */      \
  /* Normally properties might contain KindOfNull values, even if their        \
     type-hint doesn't allow this (because of initial values). This indicates  \
     the property won't contain KindOfNull if its type-hint doesn't allow it.  \
     */                                                                        \
  ATTR(NoImplicitNullable, 6)            /*       |    X     |         */      \
  /* Was this declared static, abstract, or final? */                          \
  ATTR(Static, 4)                        /*       |    X     |    X    */      \
  ATTR(Abstract, 5)                      /*    X  |          |    X    */      \
  ATTR(Final, 6)                         /*    X  |          |    X    */      \
                                         /*       |          |         */      \
  /* Is this class an interface? */                                            \
  ATTR(Interface, 7)                     /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Indicates that a static property has the <<__LSB>> attribute.             \
     Such a property is implicitly redeclared in all derived classes. */       \
  ATTR(LSB, 7)                           /*       |    X     |         */      \
  /* Does this function support the async eager return optimization? If so,    \
     instead of returning a finished Awaitable, this function may return the   \
     unpacked result of the Awaitable, assuming the AsyncEagerRet ActRec flag  \
     was set by the caller. */                                                 \
  ATTR(SupportsAsyncEagerReturn, 7)      /*       |          |    X    */      \
  /* Is this class a trait?  On methods, or properties, this indicates that    \
     the method was imported from a trait. */                                  \
  ATTR(Trait, 8)                         /*    X  |    X     |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that this function should be ignored in backtraces.     */      \
  ATTR(NoInjection, 9)                   /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates this property's initial value satisfies its type-constraint and \
     no runtime check needs to be done. */                                     \
  ATTR(InitialSatisfiesTC, 9)            /*       |    X     |         */      \
  /* Indicates this class or any of its subclasses is not mocked. */           \
  ATTR(NoMock, 9)                        /*    X  |          |         */      \
  /* Indicates that this property is definitely not redeclaring a property in  \
     a parent, or if it is, the type-hints of the two properties are           \
     equivalent (and therefore requires no runtime check). */                  \
  ATTR(NoBadRedeclare, 10)               /*       |    X     |         */      \
  /* Indicates that this async function cannot suspend. */                     \
  ATTR(EagerOnly, 10)                    /*       |          |    X    */      \
  /* Indicates that a function can be used with fb_intercept2. */              \
  ATTR(Interceptable, 11)                /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* This class is sealed */                                                   \
  ATTR(Sealed, 11)                       /*    X  |          |         */      \
  /* Property starts as uninit, will throw if accessed before being explicitly \
     set. */                                                                   \
  ATTR(LateInit, 11)                     /*       |    X     |         */      \
  /* Traits have been flattened on this class. */                              \
  ATTR(NoExpandTrait, 12)                /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Is this a type-splat parameter? */                                        \
  ATTR(SplatParam, 12)                   /*       |          |    X    */      \
  /* Indicates that the class is not extended, or on a method that no          \
     extending class defines the method. Implies                               \
     AttrNoOverrideOverride for classes. */                                    \
  ATTR(NoOverride, 13)                   /*    X  |          |    X    */      \
                                         /*       |          |         */      \
  /* Set on classes to indicate it is not extended by a "regular"              \
     class (but might be extended by non-regular classes). A regular           \
     class is a class which isn't a trait, enum, or abstract                   \
     class. Such classes cannot be instantiated (but can be                    \
     manipulated in static contexts).  This is a weaker condition than         \
     AttrNoOverride but is useful if you have a known instance of the          \
     class (and therefore must be regular). */                                 \
  ATTR(NoOverrideRegular, 14)            /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Indicates that this property was declared as readonly */                  \
  ATTR(IsReadonly, 14)                   /*       |    X     |         */      \
                                         /*       |          |         */      \
  /* Indicates that the function does not modify its instance */               \
  ATTR(ReadonlyThis, 14)                 /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that the function returns readonly value */                     \
  ATTR(ReadonlyReturn, 15)               /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that this symbol is internal to the module it is declared in */ \
  ATTR(Internal, 16)                     /*    X  |    X     |    X    */      \
                                         /*       |          |         */      \
  /* Same as AttrInternal except when producing an error, instead of           \
     throwing, it always raises a warning */                                   \
  ATTR(InternalSoft, 17)                 /*    X  |    X     |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that the function, class or static property can be loaded       \
     once and then persisted across all requests. */                           \
  ATTR(Persistent, 18)                   /*    X  |    X     |    X    */      \
                                         /*       |          |         */      \
  /* Set on functions to mark them as being able to be dynamically called */   \
  ATTR(DynamicallyCallable, 19)          /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Set on classes to mark them as being able to be dynamically constructed   \
     */                                                                        \
  ATTR(DynamicallyConstructible, 19)     /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Set on all builtin functions, whether PHP or C++. */                      \
  ATTR(Builtin, 20)                      /*    X  |          |    X    */      \
                                         /*       |          |         */      \
  /* Set on functions that have 86productAttributionData variable */           \
  ATTR(HasAttributionData, 21)           /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Set on properties to indicate they can't be changed after construction    \
     and on classes to indicate that all that class' properties are const. */  \
  ATTR(IsConst, 21)                      /*    X  |    X     |         */      \
                                         /*       |          |         */      \
  /* Set on classes where a user is allowed get a pointer to the class from a  \
     string. */                                                                \
  ATTR(DynamicallyReferenced, 22)        /*    X  |          |         */      \
  /* Set on base classes that do not have any reified classes that extend it.  \
     */                                                                        \
  ATTR(NoReifiedInit, 23)                /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Does this function have a `named t...' parameter? Such a parameter has no \
     name, so it declares nothing the body can read; it only signals that the  \
     function accepts named arguments that match no named parameter. */        \
  ATTR(VariadicNamedParam, 23)           /*       |          |    X    */      \
                                         /*       |          |         */      \
  ATTR(IsMethCaller, 24)                 /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Set on closure classes */                                                 \
  ATTR(IsClosureClass, 24)               /*    X  |          |         */      \
  /* Set on closure classes that use a property to store required coeffects */ \
  ATTR(HasClosureCoeffectsProp, 25)      /*    X  |          |         */      \
  /* Set on functions with coeffect rules */                                   \
  ATTR(HasCoeffectRules, 25)             /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that this function can be constant-folded if it is called with  \
     all constant arguments. */                                                \
  ATTR(IsFoldable, 26)                   /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that this function cannot be called with FCallBuiltin because   \
     it requires an ActRec argument. */                                        \
  ATTR(NoFCallBuiltin, 27)               /*       |          |    X    */      \
  /* Set on classes, other than closures, defined in a strict package. */      \
  ATTR(InStrictPackage, 27)              /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Does this function have a `...' parameter? */                             \
  ATTR(VariadicParam, 28)                /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Indicates that the frame should be ignored when searching for a context   \
     to store in the provenance tag.  (For HNI builtins, indicates that we     \
     should skip tagging the return value with the builtin's callsite.) */     \
  ATTR(ProvenanceSkipFrame, 29)          /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* Is this an "enum class" (in the sense of the "enum dependent types"       \
     feature)? */                                                              \
  ATTR(EnumClass, 30)                    /*    X  |          |         */      \
                                         /*       |          |         */      \
  /* Is this a native function whose calls should not be included in           \
     recordings? */                                                            \
  ATTR(NoRecording, 30)                  /*       |          |    X    */      \
                                         /*       |          |         */      \
  /* XXX: The enum is used as a bitmask and without a value in the highest bit \
          we get assertions in dev builds. */                                  \
  ATTR(UnusedMaxAttr, 31)                /*    X  |    X     |    X    */

enum Attr {
  AttrNone                 = 0,
  #define ATTR(name, shift) Attr##name = 1 << shift,
    ATTR_BITS
  #undef ATTR
};

std::string show(const Attr attrs);

constexpr Attr operator|(Attr a, Attr b) { return Attr((int)a | (int)b); }

inline Attr& operator|=(Attr& a, const Attr& b) {
  return (a = Attr((int)a | (int)b));
}

inline void attrSetter(Attr& attrs, bool set, Attr what) {
  if (set) {
    attrs |= what;
  } else {
    attrs = Attr(attrs & ~what);
  }
}

constexpr Attr VisibilityAttrs = AttrPublic|AttrProtected|AttrPrivate;

inline const char* attrToVisibilityStr(Attr attr) {
  return (attr & AttrPrivate) ? "private" :
         (attr & AttrProtected) && (attr & AttrInternal) ? "protected internal" :
         (attr & AttrProtected) ? "protected" :
         (attr & AttrInternal) ? "internal" : "public";
}

///////////////////////////////////////////////////////////////////////////////
}
