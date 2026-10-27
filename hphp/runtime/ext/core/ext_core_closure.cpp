/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

#include "hphp/runtime/ext/core/ext_core.h"
#include "hphp/runtime/vm/native-prop-handler.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

Class* c_Closure::cls_Closure;

const StaticString
  s_Closure("Closure");

struct ClosurePropHandler: Native::BasePropHandler {
  static bool isPropSupported(const OptString&, const OptString&) {
    raise_error("Closure object cannot have properties");
    return false;
  }
};

///////////////////////////////////////////////////////////////////////////////

void c_Closure::init(int numArgs, ActRec* ar, TypedValue* sp) {
  auto const cls = getVMClass();
  auto const invokeFunc = getInvokeFunc();

  if (invokeFunc->cls()) {
    if (invokeFunc->isStatic()) {
      setClass(ar->hasClass() ? ar->getClass() : ar->getThis()->getVMClass());
    } else {
      setThis(ar->getThis());
      getThisUnchecked()->incRefCount();
    }
  } else {
    hdr()->ctx = nullptr;
  }

  /*
   * Copy the use vars to instance variables.
   */
  auto const hasCoeffectsProp = cls->hasClosureCoeffectsProp();
  DEBUG_ONLY auto const numProps = cls->numDeclProperties();
  assertx(numProps == numArgs + (hasCoeffectsProp ? 1 : 0));

  if (debug) {
    // Closure properties shouldn't have type-hints nor should they be LateInit.
    for (auto const& prop : cls->declProperties()) {
      always_assert(!(prop.attrs & AttrLateInit));
      for (auto const& tc : prop.typeConstraints.range()) {
        always_assert(!tc.isCheckable());
      }
    }
  }

  auto beforeCurUseVar = sp + numArgs;

  assertx(props()->checkInvariants(numProps));
  if (hasCoeffectsProp) setCoeffects(ar->requiredCoeffects());
  assertx(IMPLIES(hasCoeffectsProp, coeffectsPropSlot() == 0));
  props()->foreach(hasCoeffectsProp ? 1 : 0, numArgs, [&](tv_lval lval) {
    assertx(beforeCurUseVar != sp);
    tvCopy(*--beforeCurUseVar, lval);
  });
}

void c_Closure::initActRecFromClosure(ActRec* ar) {
  auto const func = ar->func();
  assertx(func->isClosureBody());

  auto const closure = c_Closure::fromObject(ar->getThisInPrologue());

  assertx(func->implCls() == closure->getVMClass());

  // Put in the correct context
  if (func->cls()) {
    // Swap in the $this or late bound class or null if it is from a top level
    // function
    ar->setThisOrClass(closure->getThisOrClass());

    if (ar->hasThis()) {
      ar->getThis()->incRefCount();
    }
  } else {
    ar->trashThis();
  }


  // Copy in all the use vars
  auto const cls = func->implCls();
  auto const hasCoeffectsProp = cls->hasClosureCoeffectsProp();
  assertx(closure->props()->checkInvariants(cls->numDeclProperties()));
  assertx(IMPLIES(hasCoeffectsProp, closure->coeffectsPropSlot() == 0));
  assertx((hasCoeffectsProp ? 1 : 0) + func->numClosureUseLocals() ==
          cls->numDeclProperties());
  auto loc = reinterpret_cast<TypedValue*>(ar) - func->firstClosureUseLocalId();

  auto const iterProps = [&](auto&& assignFunc){
    closure->props()->foreach(
      hasCoeffectsProp ? 1 : 0,
      func->numClosureUseLocals(),
      assignFunc
    );
  };

  // Closure properties can be moved rather than duplicated when closure has
  // exactly 1 reference. After moving props, closure can be freed.
  if (closure->hasExactlyOneRef()) {
    iterProps([&](tv_rval rval) {tvCopy(*rval, *--loc);});
    releaseShallow(closure);
  } else {
    iterProps([&](tv_rval rval) {tvDup(*rval, *--loc);});
    closure->decRefCount();
  }

}

///////////////////////////////////////////////////////////////////////////////

ObjectData* c_Closure::instanceCtor(Class* cls) {
  raise_error("Can't create a Closure directly");
}

void c_Closure::instanceDtor(ObjectData* obj, const Class* cls) {
  if (UNLIKELY(obj->getAttribute(ObjectData::IsWeakRefed))) {
    WeakRefData::invalidateWeakRef((uintptr_t)obj);
  }

  auto closure = c_Closure::fromObject(obj);
  if (auto t = closure->getThis()) decRefObj(t);

  closure->props()->release(cls->countablePropsEnd());
  auto hdr = closure->hdr();
  tl_heap->objFree(hdr, hdr->size());
}

void c_Closure::releaseShallow(ObjectData* obj) {
  if (UNLIKELY(obj->getAttribute(ObjectData::IsWeakRefed))) {
    WeakRefData::invalidateWeakRef((uintptr_t)obj);
  }

  auto closure = c_Closure::fromObject(obj);
  if (auto t = closure->getThis()) decRefObj(t);

  auto hdr = closure->hdr();
  tl_heap->objFree(hdr, hdr->size());
}

///////////////////////////////////////////////////////////////////////////////

ObjectData* createClosureRepoAuthRawSmall(Class* cls, size_t size,
                                          size_t index) {
  assertx(!(cls->attrs() & (AttrAbstract|AttrInterface|AttrTrait|AttrEnum|
                            AttrEnumClass)));
  assertx(!cls->needInitialization());
  assertx(cls->parent() == c_Closure::classof() && cls != c_Closure::classof());
  auto mem = tl_heap->mallocSmallIndexSize(index, size);
  auto hdr = new (mem) ClosureHdr(size, ClosureHdr::NoThrow{});
  auto obj = new (hdr + 1) c_Closure(cls, ObjectData::InitRaw{});
  assertx(obj->hasExactlyOneRef());
  return obj;
}

ObjectData* createClosureRepoAuth(Class* cls) {
  assertx(!(cls->attrs() & (AttrAbstract|AttrInterface|AttrTrait|AttrEnum|
                            AttrEnumClass)));
  assertx(!cls->needInitialization());
  assertx(cls->parent() == c_Closure::classof() || cls == c_Closure::classof());
  auto const nProps = cls->numDeclProperties();
  auto const size = sizeof(ClosureHdr) + ObjectData::sizeForNProps(nProps);
  auto hdr = new (tl_heap->objMalloc(size)) ClosureHdr(size);
  auto obj = new (hdr + 1) c_Closure(cls);
  assertx(obj->props()->checkInvariants(nProps));
  assertx(obj->hasExactlyOneRef());
  return obj;
}

ObjectData* createClosure(Class* cls) {
  /*
   * We call Unit::defClosure while jitting, so its not allowed to
   * mark the class as cached unless its persistent. Do it here
   * instead, but only in non-repo-auth mode.
   */
  if (!rds::isHandleInit(cls->classHandle())) {
    cls->preClass()->namedType()->clsList()->setCached();
  }
  return createClosureRepoAuth(cls);
}

// should never be called
ATTRIBUTE_USED ATTRIBUTE_UNUSED EXTERNALLY_VISIBLE
static void closuseInstanceReference(void) {
  // ensure c_Closure and ClosureHdr ptrs are scanned inside other types
  (void)type_scan::getIndexForMalloc<c_Closure>();
  (void)type_scan::getIndexForMalloc<ClosureHdr>();
}

size_t c_Closure::coeffectsPropSlot() const {
  assertx(getVMClass()->hasClosureCoeffectsProp());
  return getVMClass()->getCoeffectsProp();
}

RuntimeCoeffects c_Closure::getCoeffects() const {
  auto const tv = props()->at(coeffectsPropSlot());
  assertx(tvIsInt(tv));
  return RuntimeCoeffects::fromValue(tv.val().num);
}

void c_Closure::setCoeffects(RuntimeCoeffects coeffects) {
  tvCopy(make_tv<KindOfInt64>(coeffects.value()),
         props()->at(coeffectsPropSlot()));
}

ObjectData* c_Closure::clone() {
  auto const cls = getVMClass();
  auto ret = c_Closure::fromObject(createClosureRepoAuth(cls));

  ret->hdr()->ctx = hdr()->ctx;
  if (auto t = getThis()) {
    t->incRefCount();
  }

  auto const nprops = cls->numDeclProperties();
  auto dst = ret->props()->iteratorAt(0);
  for (auto src : props()->range(0, nprops)) {
    tvDup(src, tv_lval{dst});
    ++dst;
  }

  return ret;
}

void CoreExtension::registerNativeClosure() {
  Native::registerNativePropHandler<ClosurePropHandler>(s_Closure);
}

///////////////////////////////////////////////////////////////////////////////
}
