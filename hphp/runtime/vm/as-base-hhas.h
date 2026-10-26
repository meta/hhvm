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
namespace HPHP {
//////////////////////////////////////////////////////////////////////

/*
 * This header is needed to avoid combining source files as-attr-ffi.cpp
 * and as-shared.cpp to minimize the amount of dependencies in the ffi
 * between HHVM and HackC.
 */

//////////////////////////////////////////////////////////////////////
namespace {

using ContextMask = uint32_t;

constexpr auto C = static_cast<ContextMask>(AttrContext::Class);
constexpr auto F = static_cast<ContextMask>(AttrContext::Func);
constexpr auto P = static_cast<ContextMask>(AttrContext::Prop);
constexpr auto T = static_cast<ContextMask>(AttrContext::TraitImport);
constexpr auto A = static_cast<ContextMask>(AttrContext::Alias);
constexpr auto K = static_cast<ContextMask>(AttrContext::Constant);
constexpr auto M = static_cast<ContextMask>(AttrContext::Module);

constexpr bool supported(ContextMask mask, AttrContext a) {
  return mask & static_cast<ContextMask>(a);
}

#define HHAS_ATTRS                                                  \
  X(AttrPublic,                   F|P|T,   "public");               \
  X(AttrProtected,                F|P|T,   "protected");            \
  X(AttrPrivate,                  F|P|T,   "private");              \
  X(AttrInternal,                 F|P|C,   "internal");             \
  X(AttrInternalSoft,             F|P|C,   "internal_soft");        \
  X(AttrStatic,                   F|P,     "static");               \
  X(AttrEnum,                     C,       "enum");                 \
  X(AttrDeepInit,                 P,       "deep_init");            \
  X(AttrHasInheritedReturnTypes,  F,       "has_inherited_return_types"); \
  X(AttrInterface,                C,       "interface");            \
  X(AttrNoExpandTrait,            C,       "no_expand_trait");      \
  X(AttrAbstract,                 C|F|T|K, "abstract");             \
  X(AttrNoOverride,               C|F,     "no_override");          \
  X(AttrNoOverrideRegular,        C,       "no_override_regular");  \
  X(AttrFinal,                    C|F|T,   "final");                \
  X(AttrSealed,                   C,       "sealed");               \
  X(AttrTrait,                    C|F|P,   "trait");                \
  X(AttrBuiltin,                  C|F,     "builtin");              \
  X(AttrPersistent,               C|F|A|K|M, "persistent");         \
  X(AttrIsConst,                  C|P,     "is_const");             \
  X(AttrIsReadonly,               P,       "readonly");             \
  X(AttrReadonlyReturn,           F,       "readonly_return");      \
  X(AttrReadonlyThis,             F,       "readonly_this");        \
  X(AttrForbidDynamicProps,       C,       "no_dynamic_props");     \
  X(AttrDynamicallyConstructible, C,       "dyn_constructible");    \
  X(AttrDynamicallyReferenced,    C,       "dyn_referenced");       \
  X(AttrProvenanceSkipFrame,      F,       "prov_skip_frame");      \
  X(AttrIsFoldable,               F,       "foldable");             \
  X(AttrInStrictPackage,          C,       "in_strict_package");    \
  X(AttrNoInjection,              F,       "no_injection");         \
  X(AttrInterceptable,            F,       "interceptable");        \
  X(AttrDynamicallyCallable,      F,       "dyn_callable");         \
  X(AttrLSB,                      P,       "lsb");                  \
  X(AttrNoBadRedeclare,           P,       "no_bad_redeclare");     \
  X(AttrEagerOnly,                F,       "eager_only");           \
  X(AttrSystemInitialValue,       P,       "sys_initial_val");      \
  X(AttrNoImplicitNullable,       P,       "no_implicit_null");     \
  X(AttrInitialSatisfiesTC,       P,       "initial_satisfies_tc"); \
  X(AttrNoMock,                   C,       "no_mock");              \
  X(AttrLateInit,                 P,       "late_init");            \
  X(AttrNoReifiedInit,            C,       "noreifiedinit");        \
  X(AttrIsMethCaller,             F,       "is_meth_caller");       \
  X(AttrIsClosureClass,           C,       "is_closure_class");     \
  X(AttrEnumClass,                C,       "enum_class");           \
  X(AttrNoRecording,              F,       "no_recording");         \
  X(AttrVariadicParam,            F,       "variadic_param");       \
  X(AttrVariadicNamedParam,       F,       "variadic_named_param"); \
  X(AttrSplatParam,               F,       "splat_param");
  /* */

  #define HHAS_TYPE_FLAGS                                   \
  X(Nullable,        "nullable");                           \
  X(TypeVar,         "type_var");                           \
  X(Soft,            "soft");                               \
  X(TypeConstant,    "type_constant")                       \
  X(Resolved,        "resolved")                            \
  X(DisplayNullable, "display_nullable")                    \
  X(UpperBound,      "upper_bound")

  #define HHAS_FCALL_FLAGS                             \
  X(HasUnpack,            "Unpack");                   \
  X(HasGenerics,          "Generics");                 \
  X(LockWhileUnwinding,   "LockWhileUnwinding");       \
  X(EnforceMutableReturn, "EnforceMutableReturn")      \
  X(EnforceReadonlyThis,  "EnforceReadonlyThis")

  #define HHAS_ITER_ARGS_FLAGS                         \
  X(BaseConst,            "BaseConst");                \
  X(WithKeys,             "WithKeys");
}

}
