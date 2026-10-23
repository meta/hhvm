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

#include "hphp/runtime/base/builtin-functions.h"

#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/code-coverage.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/request-injection-data.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/variable-unserializer.h"

#include "hphp/runtime/debugger/debugger.h"

#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/system/systemlib.h"

#include "hphp/util/process.h"
#include "hphp/util/string-vsnprintf.h"
#include "hphp/util/text-util.h"

#include <fmt/format.h>

#include <algorithm>

namespace HPHP {

using std::string;

///////////////////////////////////////////////////////////////////////////////
// static strings

const StaticString
  s_Array("Array"),
  s_offsetExists("offsetExists"),
  s___invoke("__invoke"),
  s_self("self"),
  s_parent("parent"),
  s_static("static");

const StaticString s_cmpWithCollection(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "a collection with an integer, double, string, array, or object"
);
const StaticString s_cmpWithVec(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "a vec with a non-vec"
);
const StaticString s_cmpWithDict(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "dicts"
);
const StaticString s_cmpWithKeyset(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "keysets"
);
const StaticString s_cmpWithClsMeth(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "a clsmeth"
);
const StaticString s_cmpWithRClsMeth(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "a reified class meth"
);
const StaticString s_cmpWithRFunc(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) with "
  "reified functions"
);
const StaticString s_cmpWithOpaqueResource(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) with "
  "opaque values"
);
const StaticString s_cmpWithECL(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) with "
  "enum class labels"
);
const StaticString s_cmpWithNonArr(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "a PHP array with a non-array"
);
const StaticString s_cmpWithFunc(
  "Cannot use relational comparison operators (<, <=, >, >=, <=>) to compare "
  "funcs"
);

///////////////////////////////////////////////////////////////////////////////

bool array_is_valid_callback(const Array& arr) {
  if (!arr.isVec() && !arr.isDict()) return false;
  if (arr.size() != 2 || !arr.exists(int64_t(0)) || !arr.exists(int64_t(1))) {
    return false;
  }
  auto const elem0 = arr.lookup(0);
  if (!isStringType(elem0.type()) && !isObjectType(elem0.type()) &&
      !isClassType(elem0.type()) && !isLazyClassType(elem0.type())) {
    return false;
  }
  auto const elem1 = arr.lookup(1);
  if (!isStringType(elem1.type()) && !isFuncType(elem1.type())) {
    return false;
  }
  return true;
}

const StaticString
  s__invoke("__invoke"),
  s_Closure__invoke("Closure::__invoke"),
  s_colon2("::");

bool is_callable(const Variant& v) {
  CallCtx ctx;
  vm_decode_function(v, ctx, DecodeFlags::LookupOnly);
  return ctx.func != nullptr && !ctx.func->isAbstract();
}

bool is_callable(const Variant& v, bool syntax_only, Variant* name) {
  bool ret = true;
  if (LIKELY(!syntax_only)) {
    ret = is_callable(v);
    if (LIKELY(!name)) return ret;
  }

  auto const tv_func = v.asTypedValue();
  if (isFuncType(tv_func->m_type)) {
    auto func_name = tv_func->m_data.pfunc->fullName();
    if (name) *name = Variant{func_name, Variant::PersistentStrInit{}};
    return true;
  }

  if (isClsMethType(tv_func->m_type)) {
    auto const clsmeth = tv_func->m_data.pclsmeth;
    if (name) {
      *name = concat3(
        clsmeth->getCls()->nameStr(), "::", clsmeth->getFunc()->nameStr());
    }
    return true;
  }

  if (isStringType(tv_func->m_type)) {
    if (name) *name = v;
    return ret;
  }

  if (isVecType(tv_func->m_type) ||
      isDictType(tv_func->m_type)) {
    auto const arr = Array(tv_func->m_data.parr);
    auto const clsname = arr.lookup(int64_t(0));
    auto const mthname = arr.lookup(int64_t(1));

    if (arr.size() != 2 || !clsname.is_init() ||
        (!isStringType(mthname.type()) && !isFuncType(mthname.type()))) {
      if (name) *name = s_Array;
      return false;
    }

    StringData* clsString = nullptr;
    if (isObjectType(clsname.type())) {
      clsString = clsname.val().pobj->getClassName().get();
    } else if (isStringType(clsname.type())) {
      clsString = clsname.val().pstr;
    } else if (isLazyClassType(clsname.type())) {
      clsString = const_cast<StringData*>(clsname.val().plazyclass.name());
    } else if (isClassType(clsname.type())) {
      clsString = const_cast<StringData*>(clsname.val().pclass->name());
    } else {
      if (name) *name = s_Array;
      return false;
    }

    if (isFuncType(mthname.type())) {
      if (name) {
        *name = Variant{mthname.val().pfunc->fullName(),
                               Variant::PersistentStrInit{}};
      }
      return true;
    }

    if (name) {
      *name = concat3(OptString{clsString},
                                s_colon2,
                                OptString{mthname.val().pstr});
    }
    return ret;
  }

  if (tv_func->m_type == KindOfObject) {
    ObjectData *d = tv_func->m_data.pobj;
    const Func* invoke = d->getVMClass()->lookupMethod(s__invoke.get());
    if (name) {
      if (d->instanceof(c_Closure::classof())) {
        // Hack to stop the mangled name from showing up
        *name = s_Closure__invoke;
      } else {
        *name = d->getClassName().asString() + "::__invoke";
      }
    }
    return invoke != nullptr;
  }

  return false;
}

namespace {
Class* vm_decode_class_from_name(
  const OptString& clsName,
  const OptString& funcName,
  bool nameContainsClass,
  ActRec* ar,
  bool& forwarding,
  Class* ctx,
  DecodeFlags flags) {
  Class* cls = nullptr;
  if (clsName.get()->tsame(s_self.get())) {
    if (ctx) {
      cls = ctx;
    }
    if (!nameContainsClass) {
      forwarding = true;
    }
  } else if (clsName.get()->tsame(s_parent.get())) {
    if (ctx && ctx->parent()) {
      cls = ctx->parent();
    }
    if (!nameContainsClass) {
      forwarding = true;
    }
  } else if (clsName.get()->tsame(s_static.get())) {
    if (ar && ar->func()->cls()) {
      if (ar->hasThis()) {
        cls = ar->getThis()->getVMClass();
      } else {
        cls = ar->getClass();
      }
      if (flags != DecodeFlags::NoWarn && cls) {
        if (Cfg::Eval::WarnOnSkipFrameLookup) {
          raise_warning(
            "vm_decode_function() used to decode a LSB class "
            "method on %s",
            cls->name()->data()
          );
        }
      }
    }
  } else {
    if (flags == DecodeFlags::Warn && nameContainsClass) {
      OptString nameClass = funcName.substr(0, funcName.find("::"));
      if (nameClass.get()->tsame(s_self.get())   ||
          nameClass.get()->tsame(s_static.get())) {
        raise_warning("behavior of call_user_func(array('%s', '%s')) "
                      "is undefined", clsName.data(), funcName.data());
      }
    }
    cls = Class::load(clsName.get());
  }
  return cls;
}

const Func* vm_decode_func_from_name(
  const OptString& funcName,
  ActRec* ar,
  bool forwarding,
  ObjectData*& this_,
  Class*& cls,
  Class* ctx,
  Class* cc,
  DecodeFlags flags) {
  CallType lookupType = this_ ? CallType::ObjMethod : CallType::ClsMethod;
  auto const moduleName =
    ar ? ar->func()->unit()->moduleName() : (const StringData*)nullptr;
  auto const callCtx = MemberLookupContext(ctx, moduleName);
  auto f = lookupMethodCtx(cc, funcName.get(), callCtx, lookupType,
                           MethodLookupErrorOptions::NoErrorOnModule);
  if (f && (f->attrs() & AttrStatic)) {
    // If we found a method and its static, null out this_
    this_ = nullptr;
  } else {
    if (!this_ && ar && ar->func()->cls() && ar->hasThis()) {
      // If we did not find a static method AND this_ is null AND there is a
      // frame ar, check if the current instance from ar is compatible
      auto const obj = ar->getThis();
      if (obj->instanceof(cls)) {
        this_ = obj;
        cls = obj->getVMClass();
      }
      if (flags != DecodeFlags::NoWarn && this_) {
        if (Cfg::Eval::WarnOnSkipFrameLookup) {
          raise_warning(
            "vm_decode_function() used to decode a method on $this, an "
            "instance of %s, from the caller, %s",
            cls->name()->data(),
            ar->func()->fullName()->data()
          );
        }
      }
    }
    if (!f) {
      if (lookupType == CallType::ClsMethod) {
        this_ = nullptr;
      }
      // Bail out if we couldn't find the method
      if (flags == DecodeFlags::Warn) {
        raise_invalid_argument_warning("function: method '%s' not found",
                               funcName.data());
      }
      return nullptr;
    }
  }

  if (!this_ && !f->isStaticInPrologue()) {
    if (flags != DecodeFlags::LookupOnly) throw_missing_this(f);
  }

  assertx(f && f->preClass());
  // If this_ is non-NULL, then this_ is the current instance and cls is
  // the class of the current instance.
  assertx(!this_ || this_->getVMClass() == cls);
  // If we are doing a forwarding call and this_ is null, set cls
  // appropriately to propagate the current late bound class.
  if (!this_ && forwarding && ar && ar->func()->cls()) {
    auto const fwdCls = ar->hasThis() ?
      ar->getThis()->getVMClass() : ar->getClass();

    // Only forward the current late bound class if it is the same or
    // a descendent of cls
    if (fwdCls->classof(cls)) {
      cls = fwdCls;
    }

    if (flags != DecodeFlags::NoWarn && fwdCls) {
      if (Cfg::Eval::WarnOnSkipFrameLookup) {
        raise_warning(
          "vm_decode_function() forwarded the calling context, %s",
          fwdCls->name()->data()
        );
      }
    }
  }

  if (flags != DecodeFlags::NoWarn && !f->isPublic()) {
    if (Cfg::Eval::WarnOnSkipFrameLookup) {
      raise_warning(
        "vm_decode_function() used to decode a %s method: %s",
        f->attrs() & AttrPrivate ? "private" : "protected",
        f->fullName()->data()
      );
    }
  }
  return f;
}
}

bool checkMethCallerTarget(const Func* meth, const Class* ctx, bool error) {
  if (meth->isStaticInPrologue()) {
    if (!error) return false;
    SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
      "meth_caller(): method {} is static",
      meth->fullName()->data()
    ));
  }

  if (meth->isPublic() || ctx == meth->cls()) return true;
  if (ctx && (meth->attrs() & AttrProtected) && ctx->classof(meth->cls())) {
    return true;
  }

  // Executing this meth_caller() can still be an error but we won't know until
  // we know precisely what instance it's being invoked on.
  // XXX: Do we want this behavior?
  if (meth->hasPrivateAncestor() && meth->cls()->classof(ctx)) {
    auto const cmeth = ctx->lookupMethod(meth->name());
    if (cmeth && cmeth->cls() == ctx && (cmeth->attrs() & AttrPrivate)) {
      return true;
    }
  }

  if (canTestsBypassVisibility(meth->userAttributes(), ctx)) return true;

  if (error) {
    SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
      "meth_caller(): method {} cannot be called from this context",
      meth->fullName()->data()
    ));
  }
  return false;
}

void checkMethCaller(const Func* func, const Class* ctx) {
  auto const cls = Class::load(func->methCallerClsName());
  if (!cls) {
    SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
      "meth_caller(): class {} not found", func->methCallerClsName()->data()
    ));
  }

  if (isTrait(cls)) {
    SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
      "meth_caller(): class {} is a trait", func->methCallerClsName()->data()
    ));
  }

  auto const meth = [&] () -> const Func* {
    if (auto const m = cls->lookupMethod(func->methCallerMethName())) return m;
    for (auto const i : cls->allInterfaces().range()) {
      if (auto const m = i->lookupMethod(func->methCallerMethName())) return m;
    }
    return nullptr;
  }();
  if (!meth) {
    SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
      "meth_caller(): method {}::{} not found",
      func->methCallerClsName()->data(),
      func->methCallerMethName()->data()
    ));
  }

  checkMethCallerTarget(meth, ctx, true);
}

const HPHP::Func*
vm_decode_function(const_variant_ref function,
                   ActRec* ar,
                   ObjectData*& this_,
                   HPHP::Class*& cls,
                   bool& dynamic,
                   DecodeFlags flags /* = DecodeFlags::Warn */,
                   bool genericsAlreadyGiven /* = false */) {
  bool forwarding = false;
  dynamic = true;

  if (function.isFunc()) {
    dynamic = false;
    this_ = nullptr;
    cls = nullptr;
    return function.toFuncVal();
  }

  if (function.isClsMeth()) {
    dynamic = false;
    this_ = nullptr;
    auto const clsmeth = function.toClsMethVal();
    cls = clsmeth->getCls();
    return clsmeth->getFunc();
  }

  if (function.isString() || function.isArray()) {
    HPHP::Class* ctx = nullptr;
    if (ar) ctx = arGetContextClass(ar);
    // Decode the 'function' parameter into this_, cls, name, pos, and
    // nameContainsClass.
    this_ = nullptr;
    cls = nullptr;
    OptString name;
    int pos = OptString::npos;
    bool nameContainsClass = false;
    if (function.isString()) {
      // If 'function' is a string we simply assign it to name and
      // leave this_ and cls set to NULL.
      name = function.toString();
      pos = name.find("::");
      nameContainsClass =
        (pos != 0 && pos != OptString::npos && pos + 2 < name.size());
    } else {
      // If 'function' is an array with exactly two indices 0 and 1, we
      // assign the value at index 1 to name and we use the value at index
      // 0 to populate cls (if the value is a string) or this_ and cls (if
      // the value is an object).
      assertx(function.isArray());
      Array arr = function.toArray();
      if (!array_is_valid_callback(arr)) {
        if (flags == DecodeFlags::Warn) {
          raise_invalid_argument_warning("function: not a valid callback array");
        }
        return nullptr;
      }

      Variant elem0 = arr[0];
      Variant elem1 = arr[1];
      if (elem1.isFunc()) {
        if (elem0.isObject()) {
          this_ = elem0.getObjectData();
          cls = this_->getVMClass();
        } else if (elem0.isClass()) {
          cls = elem0.toClassVal();
        } else if (elem0.isLazyClass()) {
          cls = Class::load(elem0.toLazyClassVal().name());
        } else {
          raise_error("calling an ill-formed array without resolved "
                      "object/class pointer");
          return nullptr;
        }
        return elem1.toFuncVal();
      }

      assertx(elem1.isString());
      name = elem1.toString();
      pos = name.find("::");
      nameContainsClass =
        (pos != 0 && pos != OptString::npos && pos + 2 < name.size());
      if (elem0.isString() || elem0.isLazyClass()) {
        auto const clsName = elem0.isString() ?
          elem0.toString() : StrNR{elem0.toLazyClassVal().name()};
        cls = vm_decode_class_from_name(
          clsName, name, nameContainsClass, ar, forwarding, ctx,
          flags);
        if (!cls) {
          if (flags == DecodeFlags::Warn) {
            raise_invalid_argument_warning("function: class not found");
          }
          return nullptr;
        }
      } else if (elem0.isClass()) {
        cls = elem0.toClassVal();
      } else {
        assertx(elem0.isObject());
        this_ = elem0.getObjectData();
        cls = this_->getVMClass();
      }
    }

    HPHP::Class* cc = cls;
    if (nameContainsClass) {
      OptString c = name.substr(0, pos);
      name = name.substr(pos + 2);
      if (c.get()->tsame(s_self.get())) {
        if (cls) {
          cc = cls;
        } else if (ctx) {
          cc = ctx;
        }
        if (!this_) {
          forwarding = true;
        }
      } else if (c.get()->tsame(s_parent.get())) {
        if (cls) {
          cc = cls->parent();
        } else if (ctx && ctx->parent()) {
          cc = ctx->parent();
        }
        if (!this_) {
          forwarding = true;
        }
      } else if (c.get()->tsame(s_static.get())) {
        if (ar && ar->func()->cls()) {
          if (ar->hasThis()) {
            cc = ar->getThis()->getVMClass();
          } else {
            cc = ar->getClass();
          }
        }
        if (flags != DecodeFlags::NoWarn && cc) {
          if (Cfg::Eval::WarnOnSkipFrameLookup) {
            raise_warning(
              "vm_decode_function() used to decode a LSB class "
              "method on %s",
              cc->name()->data()
            );
          }
        }
      } else {
        cc = Class::load(c.get());
      }
      if (!cc) {
        if (flags == DecodeFlags::Warn) {
          raise_invalid_argument_warning("function: class not found");
        }
        return nullptr;
      }
      if (cls) {
        if (!cls->classof(cc)) {
          if (flags == DecodeFlags::Warn) {
            raise_warning("call_user_func expects parameter 1 to be a valid "
                          "callback, class '%s' is not a subclass of '%s'",
                          cls->preClass()->name()->data(),
                          cc->preClass()->name()->data());
          }
          return nullptr;
        }
      }
      // If there is not a current instance, cc trumps cls.
      if (!this_) {
        cls = cc;
      }
    }

    if (!cls) {
      HPHP::Func* f = HPHP::Func::load(name.get());
      if (!f) {
        if (flags == DecodeFlags::Warn) {
          raise_invalid_argument_warning("function: method '%s' not found",
                                 name.data());
        }
        return nullptr;
      }
      assertx(f && f->preClass() == nullptr);
      if (f->hasReifiedGenerics() && !genericsAlreadyGiven &&
          !f->getGenericsInfo().allGenericsSoft()) {
        raise_invalid_argument_warning(
          "You may not call the reified function '%s' "
          "without reified arguments",
          f->fullName()->data());
        return nullptr;
      }
      return f;
    }
    assertx(cls);
    return vm_decode_func_from_name(
      name, ar, forwarding, this_, cls, ctx, cc, flags);
  }
  if (function.isObject()) {
    this_ = function.asCObjRef().get();
    cls = nullptr;
    dynamic = false;
    const HPHP::Func *f = this_->getVMClass()->lookupMethod(s___invoke.get());
    if (f != nullptr && f->isStaticInPrologue()) {
      // If __invoke is static, invoke it as such
      cls = this_->getVMClass();
      this_ = nullptr;
    }
    if (flags == DecodeFlags::Warn && f == nullptr) {
      raise_warning("call_user_func() expects parameter 1 to be a valid "
                    "callback, object of class %s given (no __invoke "
                    "method found)", this_->getVMClass()->name()->data());
    }
    return f;
  }
  if (flags == DecodeFlags::Warn) {
    raise_invalid_argument_warning("function: not string, closure, or array");
  }
  return nullptr;
}

Variant vm_call_user_func(const_variant_ref function, const Variant& params,
                          RuntimeCoeffects providedCoeffects
                            /* = RuntimeCoeffects::fixme() */,
                          bool checkRef /* = false */,
                          bool allowDynCallNoPointer /* = false */) {
  CallCtx ctx;
  vm_decode_function(function, ctx);
  if (ctx.func == nullptr || (!isContainer(params) && !params.isNull())) {
    return init_null();
  }
  // TODO(named_params) support calling user funcs with named args.
  const ArrayData* namedArgNames = nullptr;
  return Variant::attach(
    g_context->invokeFunc(ctx.func, params, namedArgNames, ctx.this_, ctx.cls,
                          providedCoeffects, ctx.dynamic,
                          checkRef, allowDynCallNoPointer)
  );
}

Variant
invoke(const OptString& function, const Variant& params,
       bool allowDynCallNoPointer /* = false */) {
  Func* func = Func::load(function.get());
  if (func && (isContainer(params) || params.isNull())) {
    auto ret = Variant::attach(
      g_context->invokeFunc(func, params, nullptr, nullptr, nullptr,
                            RuntimeCoeffects::fixme(), true, false,
                            allowDynCallNoPointer)

    );
    return ret;
  }
  throw ExtendedException("(1) call the function without enough arguments OR "
                          "(2) Unable to find function \"%s\" OR "
                          "(3) function was not in invoke table OR "
                          "(4) function was renamed to something else.",
                          function.c_str());
}

Variant invoke_static_method(const OptString& s, const OptString& method,
                             const Variant& params, const ArrayData* namedArgNames,
                             bool fatal /* = true */) {
  HPHP::Class* class_ = Class::lookup(s.get());
  if (class_ == nullptr) {
    o_invoke_failed(s.data(), method.data(), fatal);
    return init_null();
  }
  const HPHP::Func* f = class_->lookupMethod(method.get());
  if (f == nullptr || !f->isStaticInPrologue() ||
    (!isContainer(params) && !params.isNull())) {
    o_invoke_failed(s.data(), method.data(), fatal);
    return init_null();
  }
  auto ret = Variant::attach(
    g_context->invokeFunc(f, params, namedArgNames, nullptr, class_, RuntimeCoeffects::fixme())
  );
  return ret;
}

void o_invoke_failed(const char *cls, const char *meth,
                     bool fatal /* = true */) {
   if (fatal) {
    string msg = "Unknown method ";
    msg += cls;
    msg += "::";
    msg += meth;
    raise_fatal_error(msg.c_str());
  } else {
    raise_warning("call_user_func to non-existent method %s::%s", cls, meth);
  }
}

void throw_has_this_need_static(const Func* f) {
  auto const msg = fmt::format(
    "Static method {}() cannot be called on instance",
    f->fullName()->data()
  );
  SystemLib::throwBadMethodCallExceptionObject(msg);
}

void throw_missing_this(const Func* f) {
  auto const msg = fmt::format(
    "Non-static method {}() cannot be called statically",
    f->fullName()->data()
  );
  SystemLib::throwBadMethodCallExceptionObject(msg);
}

void NEVER_INLINE throw_invalid_property_name(const OptString& name) {
  if (!name.size()) {
    raise_error("Cannot access empty property");
  }
  raise_error("Cannot access property started with '\\0'");
}

void throw_instance_method_fatal(const char *name) {
  raise_error("Non-static method %s() cannot be called statically", name);
}

void throw_call_reified_func_without_generics(const Func* f) {
  auto const msg = fmt::format(
    "Cannot call the reified function '{}' without the reified generics",
    f->fullName()->data()
  );
  SystemLib::throwBadMethodCallExceptionObject(msg);
}

void raise_implicit_context_warning(std::string s) {
  raise_warning(s);
}

void throw_iterator_not_valid() {
  SystemLib::throwInvalidOperationExceptionObject(
    "Iterator is not valid");
}

void throw_collection_property_exception() {
  SystemLib::throwInvalidOperationExceptionObject(
    "Cannot access a property on a collection");
}

void throw_invalid_collection_parameter() {
  SystemLib::throwInvalidArgumentExceptionObject(
    "Parameter must be an array or an instance of Traversable");
}

void throw_invalid_operation_exception(StringData* str) {
  SystemLib::throwInvalidOperationExceptionObject(Variant{str});
}

void throw_division_by_zero_exception() {
  SystemLib::throwDivisionByZeroExceptionObject();
}

void throw_collection_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithCollection);
}

void throw_vec_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithVec);
}

void throw_dict_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithDict);
}

void throw_rfunc_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithRFunc);
}

void throw_opaque_resource_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithOpaqueResource);
}

void throw_ecl_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithECL);
}

void throw_keyset_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithKeyset);
}

void throw_clsmeth_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithClsMeth);
}

void throw_rclsmeth_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithRClsMeth);
}

void throw_func_compare_exception() {
  SystemLib::throwInvalidOperationExceptionObject(s_cmpWithFunc);
}

void throw_param_is_not_container() {
  static const string msg("Parameter must be an array or collection");
  SystemLib::throwInvalidArgumentExceptionObject(msg);
}

void throw_invalid_inout_base() {
  SystemLib::throwInvalidArgumentExceptionObject(
    "Parameters marked inout must be contained in locals, vecs, dicts, "
    "keysets, and arrays"
  );
}

void throw_cannot_modify_immutable_object(const char* className) {
  auto msg = fmt::format(
    "Cannot modify immutable object of type {}",
    className
  );
  SystemLib::throwInvalidOperationExceptionObject(msg);
}

void throw_cannot_modify_const_object(const char* className) {
  auto msg = fmt::format(
    "Cannot modify const object of type {}",
    className
  );
  SystemLib::throwInvalidOperationExceptionObject(msg);
}

void throw_object_forbids_dynamic_props(const char* className) {
  auto msg = fmt::format(
    "Class {} does not allow use of dynamic (non-declared) properties",
    className
  );
  SystemLib::throwInvalidOperationExceptionObject(msg);
}

void throw_cannot_modify_const_prop(const char* className,
                                    const char* propName)
{
  auto msg = fmt::format(
    "Cannot modify const property {} of class {} after construction",
    propName, className
  );
  SystemLib::throwInvalidOperationExceptionObject(msg);
}

void throw_cannot_modify_static_const_prop(const char* className,
                                           const char* propName)
{
  auto msg = fmt::format(
    "Cannot modify static const property {} of class {}.",
    propName, className
  );
  SystemLib::throwInvalidOperationExceptionObject(msg);
}

void throw_local_must_be_value_type(const char* locName)
{
  auto const msg = fmt::format("Local {} must be a value type.", locName);
  SystemLib::throwReadonlyViolationExceptionObject(msg);
}

namespace {
[[noreturn]]
void throw_readonly_violation(const char* className, const char* propName,
                              const char* msg) {
  std::string fmtMsg;
  string_printf(fmtMsg, msg, propName, className);
  SystemLib::throwReadonlyViolationExceptionObject(fmtMsg);
}
}

void throw_must_be_readonly(const char* className, const char* propName) {
  throw_readonly_violation(className, propName, Strings::MUST_BE_READONLY);
}

void throw_must_be_mutable(const char* className, const char* propName) {
  throw_readonly_violation(className, propName, Strings::MUST_BE_MUTABLE);
}

void throw_must_be_enclosed_in_readonly(const char* className, const char* propName) {
  throw_readonly_violation(className, propName, Strings::MUST_BE_ENCLOSED_IN_READONLY);
}

void throw_must_be_value_type(const char* className, const char* propName) {
  throw_readonly_violation(className, propName, Strings::MUST_BE_VALUE_TYPE);
}

void throw_cannot_modify_readonly_collection() {
  SystemLib::throwReadonlyViolationExceptionObject(
    Strings::READONLY_COLLECTIONS_CANNOT_BE_MODIFIED
  );
}

bool readonlyLocalShouldThrow(TypedValue tv, ReadonlyOp op) {
  if (op == ReadonlyOp::CheckROCOW || op == ReadonlyOp::CheckMutROCOW) {
    vmMInstrState().roProp = true;
    if (type(tv) == KindOfObject) return true;
  }
  return false;
}

void checkReadonly(const TypedValue* tv,
                   const Class* cls,
                   const StringData* name,
                   bool readonly,
                   ReadonlyOp op,
                   bool writeMode) {
  if ((op == ReadonlyOp::CheckMutROCOW && readonly) || op == ReadonlyOp::CheckROCOW) {
    vmMInstrState().roProp = true;
  }
  if (readonly) {
    if (op == ReadonlyOp::CheckMutROCOW || op == ReadonlyOp::CheckROCOW) {
      if (type(tv) == KindOfObject) {
        throw_must_be_value_type(cls->name()->data(), name->data());
      }
    } else if (op == ReadonlyOp::Mutable) {
      if (writeMode) {
        throw_must_be_mutable(cls->name()->data(), name->data());
      } else {
        throw_must_be_enclosed_in_readonly(cls->name()->data(), name->data());
      }
    }
  } else if (op == ReadonlyOp::Readonly || op == ReadonlyOp::CheckROCOW) {
    throw_must_be_readonly(cls->name()->data(), name->data());
  }
}

NEVER_INLINE
void throw_late_init_prop(const Class* cls,
                          const StringData* propName,
                          bool isSProp) {
  SystemLib::throwInvalidOperationExceptionObject(
    fmt::format(
      "Accessing <<__LateInit>> {} '{}::{}' before initialization",
      isSProp ? "static property" : "property",
      cls->name()->slice(),
      propName->slice()
    )
  );
}

void check_collection_cast_to_array() {
  if (Cfg::Server::WarnOnCollectionToArray) {
    raise_warning("Casting a collection to an array is an expensive operation "
                  "and should be avoided where possible. To convert a "
                  "collection to an array without raising a warning, use the "
                  "toArray() method.");
  }
}

Object create_object_only(const OptString& s) {
  return Object::attach(g_context->createObjectOnly(s.get()));
}

Object init_object(const OptString& s, const Array& params,
                   const ArrayData* namedArgNames, ObjectData* o) {
  return Object{g_context->initObject(s.get(), params, namedArgNames, o)};
}

Object
create_object(const OptString& s, const Array& params,
              const ArrayData* namedArgNames /* = nullptr */, bool init /* = true */) {
  return Object::attach(g_context->createObject(s.get(), params, namedArgNames, init));
}

Object
create_object(const Class* cls, const Array& params,
              const ArrayData* namedArgNames /* = nullptr */, bool init /* = true */) {
  return Object::attach(g_context->createObject(cls, params, namedArgNames, init));
}

void throw_object(const Object& e) {
  throw req::root<Object>(e);
}

#if ((__GNUC__ != 4) || (__GNUC_MINOR__ != 8))
void throw_object(Object&& e) {
  throw req::root<Object>(std::move(e));
}
#endif

/*
 * This function is used when another thread is segfaulting---we just
 * want to wait forever to give it a chance to write a stacktrace file
 * (and maybe a core file).
 */
void pause_forever() {
  for (;;) sleep(300);
}

bool is_constructor_name(const char* fn) {
  auto const len = strlen(fn);
  const char construct[] = "__construct";
  auto const clen = sizeof(construct) - 1;

  if (len >= clen && !strcmp(fn + len - clen, construct)) {
    if (len == clen || (len > clen + 2 &&
                        fn[len - clen - 1] == ':' &&
                        fn[len - clen - 2] == ':')) {
      return true;
    }
  }
  return false;
}

void throw_missing_arguments_nr(const char *fn, int expected, int got) {
  SystemLib::throwRuntimeExceptionObject(fmt::format(
    "{}() expects exactly {} positional parameter{}, {} given",
    fn,
    expected,
    expected == 1 ? "" : "s",
    got
  ));
}

void raise_bad_type_warning(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string msg;
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);

  raise_warning("Invalid operand type was used: %s", msg.c_str());
}

void raise_expected_array_warning(const char* fn /*=nullptr*/) {
  if (!fn) {
    fn = fromLeaf([&](const BTFrame& frm) {
      return frm.func()->name()->data();
    }, backtrace_detail::true_pred, "(unknown)");
  }
  raise_bad_type_warning("%s expects array(s)", fn);
}

void raise_expected_array_or_collection_warning(const char* fn /*=nullptr*/) {
  if (!fn) {
    fn = fromLeaf([&](const BTFrame& frm) {
      return frm.func()->name()->data();
    }, backtrace_detail::true_pred, "(unknown)");
  }
  raise_bad_type_warning("%s expects array(s) or collection(s)", fn);
}

void raise_invalid_argument_warning(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  string msg;
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_warning("Invalid argument: %s", msg.c_str());
}

Variant unserialize_ex(const char* str, int len,
                       VariableUnserializer::Type type,
                       const Array& options /* = null_array */,
                       bool pure /* = false */) {
  if (str == nullptr || len <= 0) {
    return false;
  }

  VariableUnserializer vu(str, len, type,
                          /* allowUnknownSerializableClass = */ true,
                          options);
  if (pure) vu.setPure();
  Variant v;
  try {
    v = vu.unserialize();
  } catch (FatalErrorException& ) {
    throw;
  } catch (InvalidAllowedClassesException& ) {
    raise_warning(
      "unserialize(): allowed_classes option should be array or boolean"
    );
    return false;
  } catch (Exception& e) {
    raise_notice("Unable to unserialize: [%.*s]. %s.",
                 std::min(len, 1000), str, e.getMessage().c_str());
    return false;
  }
  return v;
}

Variant unserialize_ex(const OptString& str,
                       VariableUnserializer::Type type,
                       const Array& options /* = null_array */,
                       bool pure /* = false */) {
  return unserialize_ex(str.data(), str.size(), type, options, pure);
}

OptString concat3(const OptString& s1, const OptString& s2, const OptString& s3) {
  auto r1 = s1.slice();
  auto r2 = s2.slice();
  auto r3 = s3.slice();
  auto len = r1.size() + r2.size() + r3.size();
  auto str = OptString::attach(StringData::Make(len));
  auto const r = str.mutableData();
  memcpy(r,                         r1.data(), r1.size());
  memcpy(r + r1.size(),             r2.data(), r2.size());
  memcpy(r + r1.size() + r2.size(), r3.data(), r3.size());
  str.setSize(len);
  return str;
}

OptString concat4(const OptString& s1, const OptString& s2, const OptString& s3,
                  const OptString& s4) {
  auto r1 = s1.slice();
  auto r2 = s2.slice();
  auto r3 = s3.slice();
  auto r4 = s4.slice();
  auto len = r1.size() + r2.size() + r3.size() + r4.size();
  auto str = OptString::attach(StringData::Make(len));
  auto const r = str.mutableData();
  memcpy(r,                                     r1.data(), r1.size());
  memcpy(r + r1.size(),                         r2.data(), r2.size());
  memcpy(r + r1.size() + r2.size(),             r3.data(), r3.size());
  memcpy(r + r1.size() + r2.size() + r3.size(), r4.data(), r4.size());
  str.setSize(len);
  return str;
}

static bool invoke_file_impl(Variant& res, const OptString& path, bool once,
                             const char *currentDir,
                             bool callByHPHPInvoke) {
  bool initial;
  auto const u = lookupUnit(path.get(), currentDir, &initial, nullptr, false);
  if (u == nullptr) return false;
  if (!once || initial) {
    *res.asTypedValue() = g_context->invokeUnit(u, callByHPHPInvoke);
  }
  return true;
}

static NEVER_INLINE Variant throw_missing_file(const char* file) {
  throw PhpFileDoesNotExistException(file);
}

static Variant invoke_file(const OptString& s,
                           bool once,
                           const char *currentDir,
                           bool callByHPHPInvoke) {
  Variant r;
  if (invoke_file_impl(r, s, once, currentDir, callByHPHPInvoke)) {
    return r;
  }
  return throw_missing_file(s.c_str());
}

Variant include_impl_invoke(const OptString& file, bool once,
                            const char *currentDir, bool callByHPHPInvoke) {
  if (FileUtil::isAbsolutePath(file.toCppString())) {
    if (Cfg::Sandbox::Mode || !Cfg::Server::AlwaysUseRelativePath) {
      try {
        return invoke_file(file, once, currentDir, callByHPHPInvoke);
      } catch(PhpFileDoesNotExistException& ) {}
    }

    try {
      OptString rel_path(FileUtil::relativePath(Cfg::Server::SourceRoot,
                                           string(file.data())));

      // Don't try/catch - We want the exception to be passed along
      return invoke_file(rel_path, once, currentDir, callByHPHPInvoke);
    } catch(PhpFileDoesNotExistException& ) {
      throw PhpFileDoesNotExistException(file.c_str());
    }
  } else {
    // Don't try/catch - We want the exception to be passed along
    return invoke_file(file, once, currentDir, callByHPHPInvoke);
  }
}

/**
 * Used by include_impl.  resolve_include() needs some way of checking the
 * existence of a file path, which for hphpc means attempting to invoke it.
 * This struct carries some context information needed for the invocation, as
 * well as a place for the return value of invoking the file.
 */
struct IncludeImplInvokeContext {
  bool once;
  const char* currentDir;

  Variant returnValue;
};

static bool include_impl_invoke_context(const OptString& file, void* ctx) {
  struct IncludeImplInvokeContext* context = (IncludeImplInvokeContext*)ctx;
  bool invoked_file = false;
  try {
    context->returnValue = include_impl_invoke(file, context->once,
                                               context->currentDir);
    invoked_file = true;
  } catch (PhpFileDoesNotExistException& ) {
    context->returnValue = false;
  }
  return invoked_file;
}

/**
 * tryFile is a pointer to a function that resolve_include() will use to
 * determine if a path references a real file.  ctx is a pointer to some context
 * information that will be passed through to tryFile.  (It's a hacky closure)
 */
OptString resolve_include(const OptString& file, const char* currentDir,
                          bool (*tryFile)(const OptString& file, void*), void* ctx) {
  const char* c_file = file.data();

  auto const getCwd = [] () -> OptString {
    if (LIKELY(!g_context.isNull())) return g_context->getCwd();
    return OptString(Process::CurrentWorkingDirectory, CopyString);
  };

  if (!File::IsPlainFilePath(file)) {
    // URIs don't have an include path
    if (tryFile(file, ctx)) {
      return file;
    }

  } else if (FileUtil::isAbsolutePath(file.toCppString())) {
    OptString can_path = FileUtil::canonicalize(file);

    if (tryFile(can_path, ctx)) {
      return can_path;
    }

  } else if ((c_file[0] == '.' && (c_file[1] == '/' || (
    c_file[1] == '.' && c_file[2] == '/')))) {

    OptString path(OptString(getCwd() + "/" + file));
    OptString can_path = FileUtil::canonicalize(path);

    if (tryFile(can_path, ctx)) {
      return can_path;
    }

  } else {
    if (!RequestInfo::s_requestInfo.isNull()) {
      auto const& includePaths = RID().getIncludePaths();

      for (auto const& includePath : includePaths) {
        OptString path("");
        auto const is_stream_wrapper =
          includePath.find("://") != std::string::npos;

        if (!is_stream_wrapper && !FileUtil::isAbsolutePath(includePath)) {
          path += (getCwd() + "/");
        }

        path += includePath;

        if (path[path.size() - 1] != '/') {
          path += "/";
        }

        path += file;

        OptString can_path;
        if (!is_stream_wrapper) {
          can_path = FileUtil::canonicalize(path);
        } else {
          can_path = OptString(path.c_str());
        }

        if (tryFile(can_path, ctx)) {
          return can_path;
        }
      }
    }

    if (FileUtil::isAbsolutePath(currentDir)) {
      OptString path(currentDir);
      path += "/";
      path += file;
      OptString can_path = FileUtil::canonicalize(path);

      if (tryFile(can_path, ctx)) {
        return can_path;
      }
    } else {
      OptString path(getCwd() + "/" + currentDir + file);
      OptString can_path = FileUtil::canonicalize(path);

      if (tryFile(can_path, ctx)) {
        return can_path;
      }
    }
  }

  return OptString();
}

static Variant include_impl(const OptString& file, bool once,
                            const char *currentDir, bool required,
                            bool raiseNotice) {
  struct IncludeImplInvokeContext ctx = {once, currentDir};
  OptString can_path = resolve_include(file, currentDir,
                                       include_impl_invoke_context, (void*)&ctx);

  if (can_path.isNull()) {
    // Failure
    if (raiseNotice) {
      raise_notice("Tried to invoke %s but file not found.", file.data());
    }
    if (required) {
      OptString ms = "Required file that does not exist: ";
      ms += file;
      raise_fatal_error(ms.data());
    }
    return false;
  }

  return ctx.returnValue;
}

Variant require(const OptString& file,
                bool once,
                const char* currentDir,
                bool raiseNotice) {
  return include_impl(file, once, currentDir, true, raiseNotice);
}

bool function_exists(const OptString& function_name) {
  auto f = Func::lookup(function_name.get());
  return f != nullptr;
}

bool is_generated(const StringData* name) {
  auto slice = name->slice();
  auto ns_pos = slice.rfind('\\');
  return isdigit(slice.data()[ns_pos+1]);
}

///////////////////////////////////////////////////////////////////////////////
// debugger and code coverage instrumentation

void throw_exception(const Object& e) {
  if (!e.instanceof(SystemLib::getThrowableClass())) {
    auto const tv = make_tv<KindOfObject>(e.get());
    raise_error(
      "Exceptions must implement the Throwable interface, got %s.",
      describe_actual_type(&tv).c_str()
    );
  }
  DEBUGGER_ATTACHED_ONLY(phpDebuggerExceptionThrownHook(e.get()));
  throw req::root<Object>(e);
}

///////////////////////////////////////////////////////////////////////////////
}
