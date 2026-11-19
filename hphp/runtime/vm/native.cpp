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

#include "hphp/runtime/vm/native.h"

#include "hphp/runtime/base/annot-type.h"
#include "hphp/runtime/base/req-ptr.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/native-func-table.h"
#include "hphp/runtime/vm/runtime.h"

namespace HPHP::Native {

//////////////////////////////////////////////////////////////////////////////

ConstantMap s_constant_map;
ClassConstantMapMap s_class_constant_map;

/////////////////////////////////////////////////////////////////////////////

namespace {

#ifdef __aarch64__
  constexpr size_t kNumGPRegs = 8;
#else
  // amd64 calling convention (also used by x64): rdi, rsi, rdx, rcx, r8, r9
  constexpr size_t kNumGPRegs = 6;
#endif

// Note: This number should generally not be modified
// as it depends on the CPU's ABI.
// If an update is needed, however, update and run
// make_native-func-caller.php as well
constexpr size_t kNumSIMDRegs = 8;

#include "hphp/runtime/vm/native-func-caller.h"

struct Registers {
  // The spilled arguments come right after the GP regs so that we can treat
  // them as a single array of kMaxBuiltinArgs ints after populating them.
  int64_t GP_regs[kNumGPRegs];
  int64_t spilled_args[kMaxBuiltinArgs - kNumGPRegs];
  double SIMD_regs[kNumSIMDRegs];
  TypedValue spilled_rvals[kMaxBuiltinArgs];

  int GP_count{0};
  int SIMD_count{0};
  int spilled_count{0};
  int spilled_rval_count{0};
};

// Push an int argument, spilling to the stack if necessary.
void pushInt(Registers& regs, const int64_t value) {
  if (regs.GP_count < kNumGPRegs) {
    regs.GP_regs[regs.GP_count++] = value;
  } else {
    assertx(regs.spilled_count < kMaxBuiltinArgs - kNumGPRegs);
    regs.spilled_args[regs.spilled_count++] = value;
  }
}

void pushRval(Registers& regs, tv_rval tv) {
  // We need to materialize the TypedValue and push a pointer to it.
  assertx(regs.spilled_rval_count < kMaxBuiltinArgs);
  regs.spilled_rvals[regs.spilled_rval_count++] = *tv;
  pushInt(regs, (int64_t)&regs.spilled_rvals[regs.spilled_rval_count - 1]);
}

// Push a double argument, spilling to the stack if necessary. We take the
// input as a Value in order to type-pun it as an int when we spill.
void pushDouble(Registers& regs, const Value value) {
  if (regs.SIMD_count < kNumSIMDRegs) {
    regs.SIMD_regs[regs.SIMD_count++] = value.dbl;
  } else {
    assertx(regs.spilled_count < kMaxBuiltinArgs - kNumGPRegs);
    regs.spilled_args[regs.spilled_count++] = value.num;
  }
}

// Push a TypedValue argument, spilling to the stack if necessary. We need
// two free GP registers to avoid spilling here. The details of what happens
// when we spill with one free GP register changes between architectures.
void pushTypedValue(Registers& regs, TypedValue tv) {
  auto const dataType = static_cast<data_type_t>(type(tv));
  if (regs.GP_count + 1 < kNumGPRegs) {
    regs.GP_regs[regs.GP_count++] = val(tv).num;
    regs.GP_regs[regs.GP_count++] = dataType;
  } else {
    assertx(regs.spilled_count + 1 < kMaxBuiltinArgs - kNumGPRegs);
    regs.spilled_args[regs.spilled_count++] = val(tv).num;
    regs.spilled_args[regs.spilled_count++] = dataType;
    // On x86, if we have one free GP register left, we'll use it for the next
    // int argument, but on ARM, we'll just spill all later int arguments.
    #ifdef __aarch64__
      regs.GP_count = kNumGPRegs;
    #endif
  }
}

// Push each argument, spilling ones we don't have registers for to the stack.
void populateArgs(Registers& regs,
                  const ActRec* fp,
                  const Func* const func,
                  const int numArgs) {
  // Regular FCalls will have their out parameter locations below the ActRec on
  // the stack, while FCallBuiltin has no ActRec to skip over.
  auto io = reinterpret_cast<TypedValue*>(const_cast<ActRec*>(fp) + 1);

  for (auto i = 0; i < numArgs; ++i) {
    auto const arg = frame_local(fp, i);
    auto const& pi = func->params()[i];
    switch (pi.builtinAbi) {
      case Func::ParamInfo::BuiltinAbi::Value:
        pushInt(regs, val(arg).num);
        break;
      case Func::ParamInfo::BuiltinAbi::FPValue:
        pushDouble(regs, val(arg));
        break;
      case Func::ParamInfo::BuiltinAbi::ValueByRef:
        pushInt(regs, (int64_t)&val(arg));
        break;
      case Func::ParamInfo::BuiltinAbi::TypedValue:
        pushTypedValue(regs, *arg);
        break;
      case Func::ParamInfo::BuiltinAbi::TypedValueByRef:
        pushRval(regs, arg);
        break;
      case Func::ParamInfo::BuiltinAbi::InOutByRef:
        assertx(func->isInOut(i));
        if (auto const iv = builtinInValue(func, i)) {
          *io = *iv;
          tvDecRefGen(arg);
        } else {
          *io = *arg;
        }

        // Any persistent values may become counted...
        if (isArrayLikeType(io->m_type)) {
          io->m_type = io->m_data.parr->toDataType();
        } else if (isStringType(io->m_type)) {
          io->m_type = KindOfString;
        }

        // Set the input value to null to avoid double freeing it
        tvWriteNull(arg);

        pushInt(regs, (int64_t)io++);
        break;
    }
  }
}

}  // namespace

/////////////////////////////////////////////////////////////////////////////

/**
 * Dispatches a call to the native function bound to <func>
 * If <ctx> is not nullptr, it is prepended to <args> when
 * calling.
 */
void callFuncImpl(const Func* const func,
                  const ActRec* fp,
                  const void* const ctx,
                  TypedValue& ret) {
  auto const f = func->nativeFuncPtr();
  auto const numArgs = func->numParams();
  auto retType = func->returnTypeConstraints().asSystemlibType(true);
  auto regs = Registers{};

  if (ctx) pushInt(regs, (int64_t)ctx);
  populateArgs(regs, fp, func, numArgs);

  // Decide how many int and double arguments we need to call func. Note that
  // spilled arguments come after the GP registers, in line with them. We can
  // spill to the stack without exhausting the GP registers, in two ways:
  //
  //  1. If we exceeed the number of SIMD arguments and spill doubles.
  //
  //  2. If we fill all but 1 GP register and then need to pass a TypedValue
  //     (two registers in size) by value.
  //
  // In these cases, we'll pass garbage in the unused GP registers to force
  // everything in the regs.spilled_args array to go on the stack.
  auto const spilled = regs.spilled_count;
  auto const GP_args = &regs.GP_regs[0];
  auto const GP_count = spilled > 0 ? spilled + kNumGPRegs : regs.GP_count;
  auto const SIMD_args = &regs.SIMD_regs[0];
  auto const SIMD_count = regs.SIMD_count;

  if (!retType) {
    // A std::nullopt return signifies Variant.
    if (func->isReturnByValue()) {
      ret = callFuncTVImpl(f, GP_args, GP_count, SIMD_args, SIMD_count);
    } else {
      new (&ret) Variant(callFuncIndirectImpl<Variant>(f, GP_args, GP_count,
                                                       SIMD_args, SIMD_count));
    }
    assertx(ret.m_type != KindOfUninit);
    return;
  }

  ret.m_type = *retType;

  switch (*retType) {
    case KindOfNull:
    case KindOfBoolean:
      ret.m_data.num =
        callFuncInt64Impl(f, GP_args, GP_count, SIMD_args, SIMD_count) & 1;
      return;

    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfInt64:
      ret.m_data.num =
        callFuncInt64Impl(f, GP_args, GP_count, SIMD_args, SIMD_count);
      return;

    case KindOfDouble:
      ret.m_data.dbl =
        callFuncDoubleImpl(f, GP_args, GP_count, SIMD_args, SIMD_count);
      return;

    case KindOfString:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfClsMeth:
    case KindOfObject:
    case KindOfEnumClassLabel:
    case KindOfResource: {
      if (func->isReturnByValue()) {
        auto val = callFuncInt64Impl(f, GP_args, GP_count, SIMD_args,
                                     SIMD_count);
        ret.m_data.num = val;
      } else {
        using T = req::ptr<StringData>;
        new (&ret.m_data) T(callFuncIndirectImpl<T>(f, GP_args, GP_count,
                                                    SIMD_args, SIMD_count));
      }
      if (ret.m_data.num == 0) {
        ret.m_type = KindOfNull;
      }
      return;
    }

    case KindOfRFunc:
    case KindOfRClsMeth:
    case KindOfUninit:
      break;
  }

  not_reached();
}

TypedValue callFunc(const Func* const func,
                    const ActRec* fp,
                    const void* const ctx) {
  TypedValue rv;
  rv.m_type = KindOfUninit;
  callFuncImpl(func, fp, ctx, rv);
  assertx(tvIsPlausible(rv));
  assertx(rv.m_type != KindOfUninit);
  return rv;
}


//////////////////////////////////////////////////////////////////////////////

#undef CASE
#undef COERCE_OR_CAST

TypedValue* functionWrapper(ActRec* ar) {
  assertx(ar);
  auto func = ar->func();

  TypedValue rv = callFunc(func, ar, nullptr);

  frame_free_locals_no_this_inl(
    ar,
    func->numLocals(),
    &rv,
    EventHook::Source::Native
  );
  tvCopy(rv, *ar->retSlot());
  ar->retSlot()->m_aux.u_asyncEagerReturnFlag = 0;
  return ar->retSlot();
}

TypedValue* methodWrapper(ActRec* ar) {
  assertx(ar);
  auto func = ar->func();
  bool isStatic = func->isStatic();

  // Prepend a context arg for methods
  // Class when it's being called statically Foo::bar()
  // Object when it's being called on an instance $foo->bar()
  void* ctx;  // ObjectData* or Class*
  if (ar->hasThis()) {
    if (isStatic) {
      throw_instance_method_fatal(func->fullName()->data());
    }
    ctx = ar->getThis();
  } else {
    if (!isStatic) {
      throw_instance_method_fatal(func->fullName()->data());
    }
    ctx = ar->getClass();
  }

  TypedValue rv = callFunc(func, ar, ctx);

  if (isStatic) {
    frame_free_locals_no_this_inl(
      ar,
      func->numLocals(),
      &rv,
      EventHook::Source::Native
    );
  } else {
    frame_free_locals_inl(
      ar,
      func->numLocals(),
      &rv,
      EventHook::Source::Native
    );
  }
  tvCopy(rv, *ar->retSlot());
  ar->retSlot()->m_aux.u_asyncEagerReturnFlag = 0;
  return ar->retSlot();
}

void getFunctionPointers(const NativeFunctionInfo& info, int nativeAttrs,
                         ArFunction& bif, NativeFunction& nif) {
  nif = info.ptr;
  if (!nif) return;
  auto const isMethod = info.sig.args.size() &&
      ((info.sig.args[0] == NativeSig::Type::This) ||
       (info.sig.args[0] == NativeSig::Type::Class));
  bif = isMethod ? methodWrapper : functionWrapper;
}

//////////////////////////////////////////////////////////////////////////////

static Optional<TypedValue> builtinInValue(
  const Func::ParamInfo& pinfo
) {
  if (!pinfo.isOutOnly()) return {};

  auto const dt = pinfo.typeConstraints.underlyingDataType();
  if (!dt) return make_tv<KindOfNull>();

  switch (*dt) {
    case KindOfNull:    return make_tv<KindOfNull>();
    case KindOfBoolean: return make_tv<KindOfBoolean>(false);
    case KindOfInt64:   return make_tv<KindOfInt64>(0);
    case KindOfDouble:  return make_tv<KindOfDouble>(0.0);
    case KindOfString:  return make_tv<KindOfString>(staticEmptyString());
    case KindOfVec:     return make_tv<KindOfVec>(staticEmptyVec());
    case KindOfDict:    return make_tv<KindOfDict>(staticEmptyDictArray());
    case KindOfKeyset:  return make_tv<KindOfKeyset>(staticEmptyKeysetArray());
    case KindOfUninit:
    case KindOfObject:
    case KindOfResource:
    case KindOfEnumClassLabel:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth: return make_tv<KindOfNull>();
  }

  not_reached();
}

Optional<TypedValue> builtinInValue(const Func* builtin, uint32_t i) {
  return builtinInValue(builtin->params()[i]);
}

//////////////////////////////////////////////////////////////////////////////

static bool tcCheckNative(const TypeConstraint& tc, const NativeSig::Type ty,
                          bool isReturn) {
  using T = NativeSig::Type;

  if (tc.typeName() && interface_supports_arrlike(tc.typeName())) {
    // TODO(T116301380): If native builtins resolved properly, we could probably
    // do something smarter here. As it stands we match on whether the type here
    // is _exactly_ one of the magic interfaces that supports array like things,
    // including Hack Collections.
    return ty == T::Mixed || ty == T::MixedTV;
  }

  // Beginning of stricter handling of nullable returns. For now, only
  // ?strings require that the return is OptString. Nullable params still
  // decay to Variant.
  if (isReturn && tc.isNullable()) {
    if (tc.isString()) {
      return ty == T::OptString;
    }
  }

  if (!tc.hasConstraint() || tc.isNullable() || tc.isCallable() ||
      tc.isArrayKey() || tc.isNumber() || tc.isVecOrDict() ||
      tc.isArrayLike() || tc.isClassname() || tc.isClass() ||
      tc.isClassOrClassname() || tc.isTypeVar() || tc.isUnion()) {
    return ty == T::Mixed || ty == T::MixedTV;
  }

  if (tc.isNothing() || tc.isNoReturn()) { return ty == T::Void; }

  if (tc.isUnresolved()) {
    // FIXME(T116301380): native builtins don't resolve properly
    return ty == T::Object || ty == T::ObjectNN;
  }

  if (!tc.underlyingDataType()) {
    return false;
  }

  switch (*tc.underlyingDataType()) {
    case KindOfDouble:       return ty == T::Double;
    case KindOfBoolean:      return ty == T::Bool;
    case KindOfObject:       return ty == T::Object   || ty == T::ObjectNN;
    case KindOfString:
      return isReturn ? ty == T::String
                      : ty == T::String || ty == T::OptString;
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:       return ty == T::Array    || ty == T::ArrayNN;
    case KindOfResource:     return ty == T::Resource || ty == T::ResourceArg;
    case KindOfUninit:
    case KindOfNull:         return ty == T::Void;
    case KindOfInt64:        return ty == T::Int64;
    case KindOfRFunc:        return false; // TODO(T66903859)
    case KindOfFunc:         return ty == T::Func;
    case KindOfClass:        return ty == T::Class;
    case KindOfClsMeth:      return ty == T::ClsMeth;
    case KindOfRClsMeth:     // TODO(T67037453)
    case KindOfLazyClass:    return false; // TODO (T68823958)
    case KindOfEnumClassLabel: return false;
  }
  not_reached();
}

static bool tcCheckNativeIO(
  const Func::ParamInfo& pinfo, const NativeSig::Type ty
) {
  using T = NativeSig::Type;

  auto const checkDT = [&] (DataType dt) -> bool {
    switch (dt) {
      case KindOfDouble:       return ty == T::DoubleIO;
      case KindOfBoolean:      return ty == T::BoolIO;
      case KindOfObject:       return ty == T::ObjectIO;
      case KindOfString:       return ty == T::StringIO;
      case KindOfVec:          return ty == T::ArrayIO;
      case KindOfDict:         return ty == T::ArrayIO;
      case KindOfKeyset:       return ty == T::ArrayIO;
      case KindOfResource:     return ty == T::ResourceIO;
      case KindOfUninit:
      case KindOfNull:         return false;
      case KindOfInt64:        return ty == T::IntIO;
      case KindOfRFunc:        return false; // TODO(T66903859)
      case KindOfFunc:         return ty == T::FuncIO;
      case KindOfClass:        return ty == T::ClassIO;
      case KindOfClsMeth:      return ty == T::ClsMethIO;
      case KindOfRClsMeth:     // TODO (T67037453)
      case KindOfLazyClass:    return false; // TODO (T68823958)
      case KindOfEnumClassLabel: return false;
    }
    not_reached();
  };

  auto const tv = builtinInValue(pinfo);
  if (tv) {
    if (isNullType(tv->m_type)) return ty == T::MixedIO;
    return checkDT(tv->m_type);
  }

  auto const checkOne = [&](auto const& tc) {
    if (!tc.hasConstraint() || tc.isNullable() || tc.isCallable() ||
        tc.isArrayKey() || tc.isNumber() || tc.isVecOrDict() ||
        tc.isArrayLike() || tc.isClassname() || tc.isClass() ||
        tc.isClassOrClassname() || tc.isTypeVar()) {
      return ty == T::MixedIO;
    }

    // FIXME(T116301380): native builtins don't resolve properly
    if (tc.isUnresolved()) return ty == T::ObjectIO;

    if (!tc.underlyingDataType()) {
      return false;
    }

    return checkDT(*tc.underlyingDataType());
  };

  return std::any_of(
    pinfo.typeConstraints.range().begin(),
    pinfo.typeConstraints.range().end(),
    [&](auto const& tc) { return checkOne(tc); }
  );
}

const char* kInvalidReturnTypeMessage = "Invalid return type detected";
const char* kInvalidArgTypeMessage = "Invalid argument type detected";
const char* kInvalidArgCountMessage = "Invalid argument count detected";
const char* kInvalidNumArgsMessage =
  "\"NumArgs\" builtins must take an int64_t as their first declared argument";
const char* kNeedStaticContextMessage =
  "Static class functions must take a Class* as their first argument";
const char* kNeedObjectContextMessage =
  "Instance methods must take an ObjectData* as their first argument";

static const StaticString
  s_native("__Native"),
  s_actrec("ActRec");

const char* checkTypeFunc(const NativeSig& sig,
                          const FuncEmitter* func) {
  using T = NativeSig::Type;

  auto const check = [](auto const& tcs, const T type, bool isReturn) {
    return std::any_of(
      tcs.range().begin(),
      tcs.range().end(),
      [&](auto const& tc) { return tcCheckNative(tc, type, isReturn); }
    );
  };

  if (!check(func->retTypeConstraints, sig.ret, true)) {
    return kInvalidReturnTypeMessage;
  }

  auto argIt = sig.args.begin();
  auto endIt = sig.args.end();
  if (func->pce()) { // called from the verifier so m_cls is not set yet
    if (argIt == endIt) return kInvalidArgCountMessage;
    auto const ctxTy = *argIt++;
    if (func->attrs & HPHP::AttrStatic) {
      if (ctxTy != T::Class) return kNeedStaticContextMessage;
    } else {
      if (ctxTy != T::This) return kNeedObjectContextMessage;
    }
  }

  for (auto const& pInfo : func->params) {
    if (argIt == endIt) return kInvalidArgCountMessage;

    auto const argTy = *argIt++;

    if (pInfo.isVariadic()) {
      if (argTy != T::Array) return kInvalidArgTypeMessage;
      continue;
    }

    if (pInfo.isInOut()) {
      if (!tcCheckNativeIO(pInfo, argTy)) {
        return kInvalidArgTypeMessage;
      }
      continue;
    }

    if (!check(pInfo.typeConstraints, argTy, false)) {
      return kInvalidArgTypeMessage;
    }
  }

  return argIt == endIt ? nullptr : kInvalidArgCountMessage;
}

OptString fullName(const StringData* fname, const StringData* cname,
                   bool isStatic) {
  return {
    cname == nullptr ? OptString{const_cast<StringData*>(fname)} :
    (OptString{const_cast<StringData*>(cname)} +
      (isStatic ? "::" : "->") +
      OptString{const_cast<StringData*>(fname)})
  };
}

NativeFunctionInfo getNativeFunction(const FuncTable& nativeFuncs,
                                     const StringData* fname,
                                     const StringData* cname,
                                     bool isStatic) {
  auto const name = fullName(fname, cname, isStatic);
  if (auto info = nativeFuncs.get(name.get())) {
    return info;
  }
  return NativeFunctionInfo();
}

NativeFunctionInfo getNativeFunction(const FuncTable& nativeFuncs,
                                     const char* fname,
                                     const char* cname,
                                     bool isStatic) {
  return getNativeFunction(nativeFuncs,
                           makeStaticString(fname),
                           cname ? makeStaticString(cname) : nullptr,
                           isStatic);
}

void registerNativeFunc(Native::FuncTable& nativeFuncs,
                        const StringData* name,
                        const NativeFunctionInfo& info) {
  nativeFuncs.insert(name, info);
}

void FuncTable::insert(const StringData* name,
                       const NativeFunctionInfo& info) {
  assertx(name->isStatic());
  DEBUG_ONLY auto it = m_infos.insert(std::make_pair(name, info));
  assertx(it.second || it.first->second == info);
}

NativeFunctionInfo FuncTable::get(const StringData* name) const {
  auto const it = m_infos.find(name);
  if (it != m_infos.end()) return it->second;
  return NativeFunctionInfo();
}

void FuncTable::dump() const {
  for (auto e : m_infos) {
    fprintf(stderr, "%s\n", e.first->data());
  }
}

static std::string nativeTypeString(NativeSig::Type ty) {
  using T = NativeSig::Type;
  switch (ty) {
  case T::Int64:      return "int";
  case T::Double:     return "double";
  case T::Bool:       return "bool";
  case T::Object:     return "object";
  case T::OptString:  return "?string";
  case T::Array:      return "array";
  case T::Resource:   return "resource";
  case T::ObjectNN:   return "object";
  case T::String:     return "string";
  case T::ArrayNN:    return "array";
  case T::ResourceArg:return "resource";
  case T::Mixed:      return "mixed";
  case T::MixedTV:    return "mixed";
  case T::This:       return "this";
  case T::Class:      return "class";
  case T::Void:       return "void";
  case T::Func:       return "func";
  case T::ClsMeth:    return "clsmeth";
  case T::IntIO:      return "inout int";
  case T::DoubleIO:   return "inout double";
  case T::BoolIO:     return "inout bool";
  case T::ObjectIO:   return "inout object";
  case T::StringIO:   return "inout string";
  case T::ArrayIO:    return "inout array";
  case T::ResourceIO: return "inout resource";
  case T::FuncIO:     return "inout func";
  case T::ClassIO:    return "inout class";
  case T::ClsMethIO:  return "inout clsmeth";
  case T::MixedIO:    return "inout mixed";
  }
  not_reached();
}

std::string NativeSig::toString(const char* classname,
                                const char* fname) const {
  using T = NativeSig::Type;

  auto str   = folly::to<std::string>(nativeTypeString(ret), " ");
  auto argIt = args.begin();
  auto endIt = args.end();

  if (argIt != endIt) {
    if (classname) str += classname;
    if (*argIt == T::This) {
      str += "->";
      ++argIt;
    } else if (*argIt == T::Class) {
      str += "::";
      ++argIt;
    }
  }
  str += folly::to<std::string>(fname,
                                "(",
                                argIt != endIt ? nativeTypeString(*argIt++)
                                               : "void");

  for (;argIt != endIt; ++argIt) {
    str += folly::to<std::string>(", ", nativeTypeString(*argIt));
  }
  str += ")";

  return str;
}

//////////////////////////////////////////////////////////////////////////////

using ClassExtraDataHandlerMap = std::unordered_map
  <const StringData*, FinishFunc>;

static ClassExtraDataHandlerMap s_classExtraDataHandlerMap;

void registerClassExtraDataHandler(const OptString& clsName, FinishFunc fn) {
  assertx(s_classExtraDataHandlerMap.find(clsName.get()) ==
    s_classExtraDataHandlerMap.end());
  s_classExtraDataHandlerMap[clsName.get()] = fn;
}

FinishFunc getClassExtraDataHandler(const StringData* clsName) {
  auto it = s_classExtraDataHandlerMap.find(clsName);
  if (it == s_classExtraDataHandlerMap.end()) {
    return nullptr;
  }
  return it->second;
}

//////////////////////////////////////////////////////////////////////////////
} // namespace HPHP::Native
