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

#include "hphp/runtime/base/apc-object.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/logger.h"

#include "hphp/runtime/base/apc-collection.h"
#include "hphp/runtime/base/apc-handle.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/data-walker.h"
#include "hphp/runtime/base/type-variant.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace {

APCObject::ClassOrName make_class(const Class* c) {
  if (classHasPersistentRDS(c)) return c;
  return c->preClass()->name();
}

const StaticString s___wakeup("__wakeup");

}

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE
APCObject::APCObject(ClassOrName cls, uint32_t propCount)
  : m_handle(APCKind::CopiedObject)
  , m_cls{cls}
  , m_propCount{propCount}
  , m_persistent{0}
  , m_may_raise{0}
  , m_no_wakeup{0}
  , m_no_verify_prop_types{0}
  , m_lazy_deserialization{0}
{}

APCHandle::Pair APCObject::Construct(ObjectData* objectData, bool pure) {
  // This function assumes the object and object/array down the tree have no
  // internal references and do not implement the serializable interface.
  assertx(!objectData->instanceof(SystemLib::getSerializableClass()));

  auto cls = objectData->getVMClass();
  auto clsOrName = make_class(cls);
  if (clsOrName.right()) return ConstructSlow(objectData, clsOrName, pure);

  // We have a persistent Class. Build an array of APCHandle* to mirror the
  // declared properties in the object.
  auto const propInfo = cls->declProperties();
  auto const hasDynProps = objectData->hasDynProps();
  auto const numRealProps = propInfo.size();
  auto const numApcProps = numRealProps + hasDynProps;
  auto size = sizeof(APCObject) + sizeof(APCHandle*) * numApcProps;
  auto const apcObj = new (apc_malloc(size)) APCObject(clsOrName, numApcProps);
  apcObj->m_persistent = 1;

  // Set a few more flags for faster fetching: whether or not the object has a
  // wakeup method, and whether or not we can use a fast path that avoids
  // default-initializing properties before setting them to their APC values.
  if (!cls->lookupMethod(s___wakeup.get())) apcObj->m_no_wakeup = 1;

  auto const apcPropVec = apcObj->persistentProps();
  auto const objProps = objectData->props();
  const ObjectProps* propInit = nullptr;

  auto allTypedValues = true;
  auto mayRaise = !apcObj->m_no_wakeup;
  auto propsDontNeedCheck = Cfg::Eval::CheckPropTypeHints > 0;
  for (unsigned slot = 0; slot < numRealProps; ++slot) {
    auto index = cls->propSlotToIndex(slot);
    auto const attrs = propInfo[slot].attrs;
    assertx((attrs & AttrStatic) == 0);

    tv_rval objProp;
    if (attrs & AttrBuiltin) {
      // Special properties like the Memoize cache should be set to their
      // default value, not the current value.
      if (propInit == nullptr) {
        propInit = cls->pinitVec().empty()
          ? cls->declPropInit().data()
          : cls->getPropData()->data();
      }

      objProp = propInit->at(index);
    } else {
      objProp = objProps->at(index);
    }

    if (UNLIKELY(type(objProp) == KindOfUninit) && (attrs & AttrLateInit)) {
      auto const origSlot = slot;
      while (slot > 0) {
        --slot;
        auto idx = cls->propSlotToIndex(slot);
        apcPropVec[idx]->unreferenceRoot();
      }
      apc_sized_free(apcObj, size);
      throw_late_init_prop(propInfo[origSlot].cls, propInfo[origSlot].name,
                           false);
    }

    // If the property value satisfies its type-hint in all contexts, we don't
    // need to do any validation when we re-create the object.
    if (propsDontNeedCheck) {
      propsDontNeedCheck
        = propInfo[slot].typeConstraints.alwaysPasses(objProp);
    }

    auto val = APCHandle::Create(const_variant_ref{objProp},
                                 APCHandleLevel::Inner, true, pure);
    size += val.size;
    apcPropVec[index] = val.handle;
    mayRaise |= val.handle->toLocalMayRaise();
    allTypedValues &= val.handle->isTypedValue();
  }

  if (Cfg::Eval::CheckPropTypeHints <= 0 || propsDontNeedCheck) {
    apcObj->m_no_verify_prop_types = 1;
  }

  if (UNLIKELY(hasDynProps)) {
    auto val = APCHandle::Create(VarNR{objectData->dynPropArray()},
                                 APCHandleLevel::Inner, true, pure);
    size += val.size;
    apcPropVec[numRealProps] = val.handle;
    mayRaise |= val.handle->toLocalMayRaise();
    // It can raise a notice when NoticeOnCreateDynamicProp is enabled.
    mayRaise |= Cfg::Eval::NoticeOnCreateDynamicProp;
  }

  mayRaise |= !propsDontNeedCheck;
  apcObj->m_may_raise = mayRaise;
  if (!mayRaise && (allTypedValues || cls->enableLazyAPCDeserialization())) {
    apcObj->m_lazy_deserialization = 1;
  }

  return {apcObj->getHandle(), size};
}

NEVER_INLINE
APCHandle::Pair APCObject::ConstructSlow(ObjectData* objectData,
                                         ClassOrName name, bool pure) {
  Array odProps;
  objectData->o_getArray(odProps);
  auto const propCount = odProps.size();

  auto size = sizeof(APCObject) + sizeof(Prop) * propCount;
  auto const apcObj = new (apc_malloc(size)) APCObject(name, propCount);
  apcObj->m_may_raise = 1;
  if (!propCount) return {apcObj->getHandle(), size};

  auto prop = apcObj->props();
  for (ArrayIter it(odProps); !it.end(); it.next(), ++prop) {
    Variant key(it.first());
    assertx(key.isString());
    auto const tv = it.secondVal();
    if (!isNullType(type(tv))) {
      auto val = APCHandle::Create(tvAsCVarRef(&tv), APCHandleLevel::Inner,
                                   true, pure);
      prop->val = val.handle;
      size += val.size;
    } else {
      prop->val = nullptr;
    }

    const OptString& keySD = key.asCStrRef();

    if (!keySD.empty() && *keySD.data() == '\0') {
      int32_t subLen = keySD.find('\0', 1) + 1;
      OptString cls = keySD.substr(1, subLen - 2);
      if (cls.size() == 1 && cls[0] == '*') {
        // Protected.
        prop->ctx = nullptr;
      } else {
        // Private.
        auto* ctx = Class::lookup(cls.get());
        if (ctx && ctx->attrs() & AttrPersistent) {
          prop->ctx = ctx;
        } else {
          prop->ctx = makeStaticString(cls.get());
        }
      }
      prop->name = makeStaticString(keySD.substr(subLen));
    } else {
      prop->ctx = nullptr;
      prop->name = makeStaticString(keySD.get());
    }
  }
  assertx(prop == apcObj->props() + propCount);

  return {apcObj->getHandle(), size};
}

ALWAYS_INLINE
APCObject::~APCObject() {
  auto const numProps = m_propCount;

  if (m_persistent) {
    auto props = persistentProps();
    for (unsigned i = 0; i < numProps; ++i) {
      if (props[i]) props[i]->unreferenceRoot();
    }
    return;
  }

  for (auto i = uint32_t{0}; i < numProps; ++i) {
    if (props()[i].val) props()[i].val->unreferenceRoot();
    assertx(props()[i].name->isStatic());
  }
}

void APCObject::Delete(APCHandle* handle) {
  auto const obj = fromHandle(handle);
  auto const allocSize = sizeof(APCObject) + obj->m_propCount *
    (obj->m_persistent ? sizeof(APCHandle*) : sizeof(Prop));
  obj->~APCObject();
  // No need to run Prop destructors.
  apc_sized_free(obj, allocSize);
}

//////////////////////////////////////////////////////////////////////

APCHandle::Pair APCObject::MakeAPCObject(APCHandle* obj, const Variant& value, bool pure) {
  if (!value.is(KindOfObject) || obj->objAttempted()) {
    return {nullptr, 0};
  }
  obj->setObjAttempted();
  ObjectData* o = value.getObjectData();
  if (o->getVMClass()->hasReifiedGenerics()) return {nullptr, 0};
  DataWalker walker(DataWalker::LookupFeature::DetectSerializable);
  DataWalker::DataFeature features = walker.traverseData(o);
  if (features.isCircular || features.hasSerializable) {
    return {nullptr, 0};
  }
  auto tmp = APCHandle::Create(value, APCHandleLevel::Inner, true, pure);
  tmp.handle->setObjAttempted();
  return tmp;
}

Variant APCObject::MakeLocalObject(const APCHandle* handle, bool pure) {
  auto apcObj = APCObject::fromHandle(handle);
  if (apcObj->m_lazy_deserialization) return apcObj->createObjectLazy(pure);
  return apcObj->m_persistent ? apcObj->createObject(pure)
                              : apcObj->createObjectSlow(pure);
}

Object APCObject::createObject(bool pure) const {
  auto cls = m_cls.left();
  assertx(cls != nullptr);

  auto obj = Object::attach(
    ObjectData::newInstanceNoPropInit(const_cast<Class*>(cls))
  );

  auto const numProps = cls->numDeclProperties();
  auto const objProp = obj->props();
  auto const apcProp = persistentProps();

  // Re-entry is possible while we're executing toLocal() on each
  // property (e.g. a nested object's __wakeup), so heap inspectors such as
  // objprof may see this object before it is fully initialized. Mark every
  // slot Uninit up front so they never read the raw allocation.
  auto range = objProp->range(0, numProps);
  for (auto slot = range.begin(); slot != range.end(); ++slot) {
    type(tv_lval{slot}) = KindOfUninit;
  }

  auto i = 0;
  for (auto it = range.begin(); it != range.end(); ++it, ++i) {
    tvCopy(apcProp[i]->toLocal(pure).detach(), tv_lval{it});
  }

  // Make sure the unserialized values don't violate any type-hints if they
  // require validation.
  if (!m_no_verify_prop_types) obj->verifyPropTypeHints();
  assertx(obj->assertPropTypeHints());

  if (UNLIKELY(numProps < m_propCount)) {
    auto dynProps = apcProp[numProps];
    assertx(dynProps->type() == KindOfPersistentDict ||
            dynProps->kind() == APCKind::CopiedDict);
    obj->setDynProps(dynProps->toLocal(pure).asCArrRef());
  }

  auto const providedCoeffects =
    pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
  if (!m_no_wakeup) obj->invokeWakeup(providedCoeffects);
  return obj;
}

Object APCObject::createObjectLazy(bool pure) const {
  assertx(!m_may_raise);
  assertx(m_lazy_deserialization);
  assertx(m_no_verify_prop_types);
  assertx(m_no_wakeup);

  auto const cls = m_cls.left();
  assertx(cls != nullptr);

  auto obj = Object::attach(
    ObjectData::newInstanceNoPropInit(const_cast<Class*>(cls))
  );

  auto const numProps = cls->numDeclProperties();
  auto const apcProp = persistentProps();
  auto const objProp = obj->props();

  auto const range = objProp->range(0, numProps);
  auto it = range.begin();

  auto i = 0;
  for (; it != range.end(); ++it, ++i) {
    auto const tv = apcProp[i]->toLazyProp();
    auto const lval = tv_lval{it};
    lval.type() = tv.m_type;
    lval.val() = tv.m_data;
  }
  assertx(obj->assertPropTypeHints());

  if (UNLIKELY(numProps < m_propCount)) {
    auto dynProps = apcProp[numProps];
    assertx(dynProps->type() == KindOfPersistentDict ||
            dynProps->kind() == APCKind::CopiedDict);
    obj->setDynProps(dynProps->toLocal(pure).asCArrRef());
  }

  auto const handle = getHandle();
  APCTypedValue::PushHazardPointer(handle);
  handle->referenceNonRoot();

   auto const providedCoeffects =
    pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
  if (!m_no_wakeup) obj->invokeWakeup(providedCoeffects);
  return obj;
}

Object APCObject::createObjectSlow(bool pure) const {
  const Class* klass;
  if (auto const c = m_cls.left()) {
    klass = c;
  } else {
    klass = Class::load(m_cls.right());
    if (!klass) {
      Logger::Error("APCObject::getObject(): Cannot find class %s",
                    m_cls.right()->data());
      return Object{};
    }
  }
  Object obj{const_cast<Class*>(klass)};

  {
    obj->unlockObject();
    SCOPE_EXIT { obj->lockObject(); };

    auto prop = props();
    auto const propEnd = prop + m_propCount;
    for (; prop != propEnd; ++prop) {
      auto const key = prop->name;

      const Class* ctx = nullptr;
      if (prop->ctx.isNull()) {
        ctx = klass;
      } else {
        if (auto const cls = prop->ctx.left()) {
          ctx = cls;
        } else {
          ctx = Class::lookup(prop->ctx.right());
          if (!ctx) continue;
        }
      }

      assertx(ctx);
      auto val = prop->val ? prop->val->toLocal(pure) : init_null();
      obj->setProp(
        MemberLookupContext(const_cast<Class*>(ctx), ctx->moduleName()),
        key, *val.asTypedValue());
    }
  }

  auto const providedCoeffects =
    pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
  obj->invokeWakeup(providedCoeffects);
  return obj;
}

//////////////////////////////////////////////////////////////////////

}
