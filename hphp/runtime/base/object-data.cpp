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

#include "hphp/runtime/base/object-data.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/object-iterator.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"

#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/ext/simplexml/ext_simplexml.h"
#include "hphp/runtime/ext/datetime/ext_datetime.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/member-operations.h"
#include "hphp/runtime/vm/memo-cache.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/native-prop-handler.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/runtime.h"

#include "hphp/system/systemlib.h"

#include "hphp/util/configs/eval.h"

#include <fmt/format.h>
#include <folly/Hash.h>
#include <folly/ScopeGuard.h>

#include <vector>

namespace HPHP {
//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(runtime)

//////////////////////////////////////////////////////////////////////

namespace {

const StaticString s_clone("__clone");

ALWAYS_INLINE
void verifyTypeHint(const Class* thisCls,
                    const Class::Prop* prop,
                    tv_lval val) {
  assertx(tvIsPlausible(*val));
  assertx(type(val) != KindOfUninit);
  if (!prop || Cfg::Eval::CheckPropTypeHints <= 0) return;
  for (auto const& tc : prop->typeConstraints.range()) {
    if (tc.isCheckable()) {
      tc.verifyProperty(val, thisCls, prop->cls, prop->name);
    }
  }
}

ALWAYS_INLINE
void unsetTypeHint(const Class::Prop* prop) {
  if (Cfg::Eval::CheckPropTypeHints <= 0) return;
  if (!prop) return;
  for (auto const& tc : prop->typeConstraints.range()) {
    if (!tc.isMixedResolved()) {
      raise_property_typehint_unset_error(
        prop->cls,
        prop->name,
        tc.isSoft(),
        tc.isUpperBound()
      );
    }
  }
}

}

//////////////////////////////////////////////////////////////////////

// Check that the given property's type matches its type-hint.
namespace {
bool assertATypeHint(const TypeConstraint& tc, tv_rval val) {
  if (!tc.isCheckable() || tc.isSoft()) return true;
  if (val.type() == KindOfUninit) return tc.maybeMixed();
  return tc.assertCheck(val);
}
}

bool ObjectData::assertTypeHint(tv_rval prop, Slot slot) const {
  assertx(tvIsPlausible(*prop));
  assertx(slot < m_cls->numDeclProperties());
  auto const& propDecl = m_cls->declProperties()[slot];

  if (prop.type() == KindOfResource && g_context->doingInlineInterp()) {
    return true;
  }

  if (debug && Cfg::Repo::Authoritative) {
    // The fact that uninitialized LateInit props are uninit isn't
    // reflected in the repo-auth-type.
    if (prop.type() != KindOfUninit || !(propDecl.attrs & AttrLateInit)) {
      always_assert(tvMatchesRepoAuthType(*prop, propDecl.repoAuthType));
    }
  }

  // If we're not hard enforcing, then the prop might contain anything.
  if (Cfg::Eval::CheckPropTypeHints <= 2) return true;
  if (prop.type() == KindOfNull && !(propDecl.attrs & AttrNoImplicitNullable)) {
    return true;
  }
  if (prop.type() == KindOfUninit && (propDecl.attrs & AttrLateInit)) {
    return true;
  }
  for (auto const& tc : propDecl.typeConstraints.range()) {
    if (!assertATypeHint(tc, prop)) return false;
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

NEVER_INLINE
static void freeDynPropArray(ObjectData* inst) {
  auto& table = g_context->dynPropTable;
  auto it = table.find(inst);
  assertx(it != end(table));
  assertx(it->second.arr().isDict());
  it->second.destroy();
  table.erase(it);
}

NEVER_INLINE
void ObjectData::slowDestroyCases() {
  assertx(slowDestroyCheck());

  if (getAttribute(UsedMemoCache)) {
    assertx(m_cls->hasMemoSlots());
    auto const nSlots = m_cls->numMemoSlots();
    for (Slot i = 0; i < nSlots; ++i) {
      auto slot = memoSlot(i);
      if (slot->isCache()) {
        if (auto cache = slot->getCache()) req::destroy_raw(cache);
      } else {
        tvDecRefGen(*slot->getValue());
      }
    }
  }

  if (UNLIKELY(getAttribute(HasDynPropArr))) freeDynPropArray(this);
  if (UNLIKELY(getAttribute(IsWeakRefed))) {
    WeakRefData::invalidateWeakRef((uintptr_t)this);
  }

  auto const memoSize = m_cls->memoSize();
  auto const ptr = reinterpret_cast<char*>(this) - memoSize;
  tl_heap->objFreeIndex(ptr, m_cls->sizeIdx());
}

// Single check for a couple different unlikely actions during destruction.
inline bool ObjectData::slowDestroyCheck() const {
  return m_aux16 & (HasDynPropArr | IsWeakRefed | UsedMemoCache | BigAllocSize);
}

void ObjectData::release(ObjectData* obj, const Class* cls) noexcept {
  assertx(obj->kindIsValid());
  assertx(!obj->hasInstanceDtor());
  assertx(!obj->hasNativeData());
  assertx(obj->getVMClass() == cls);
  assertx(cls->releaseFunc() == &ObjectData::release);
  assertx(obj->props()->checkInvariants(cls->numDeclProperties()));

  // Note: cleanups done in this function are only run for classes without an
  // instanceDtor. Some of these cleanups are duplicated in ~ObjectData, and
  // your instanceDtor may call that to have them run; if you choose not to run
  // ~ObjectData from your instanceDtor you MUST do some of them manually
  // (e.g. invalidate WeakRefs). Some cleanups (e.g. clearing memo caches) are
  // not done from ~ObjectData because it is assumed they're not needed for
  // builtin classes (and in the case of memo caches, since the clearing needs
  // to be done differently when there is native data).
  // Finally, cleanups such as invalidating WeakRefs that have to be done for
  // correctness MUST also be done in Collector::sweep, since none of the code
  // in this function or the instanceDtor will be run when the object is
  // collected by GC.

  // `obj' is being torn down now---be careful about where/how you dereference
  // it from here on.

  obj->props()->release(cls->countablePropsEnd());

  if (UNLIKELY(obj->slowDestroyCheck())) {
    obj->slowDestroyCases();
  } else {
    assertx((obj->m_aux16 & BigAllocSize) == 0);
    auto const memoSize = cls->memoSize();
    auto const ptr = reinterpret_cast<char*>(obj) - memoSize;
    assertx(memoSize == 0 ||
            reinterpret_cast<const MemoNode*>(ptr)->objOff() == memoSize);

    tl_heap->freeSmallIndex(ptr, cls->sizeIdx());
  }

  AARCH64_WALKABLE_FRAME();
}

///////////////////////////////////////////////////////////////////////////////
// class info

StrNR ObjectData::getClassName() const {
  return m_cls->preClass()->nameStr();
}

bool ObjectData::instanceof(const OptString& s) const {
  assertx(kindIsValid());
  auto const cls = Class::lookup(s.get());
  return cls && instanceof(cls);
}

bool ObjectData::toBooleanImpl() const noexcept {
  // Note: if you add more cases here, hhbbc/class-util.cpp also needs
  // to be changed.
  if (isCollection()) {
    if (Cfg::Eval::NoticeOnCollectionToBool) {
      raise_notice(
        "%s to boolean cast",
        collections::typeToString((CollectionType)m_kind)->data()
      );
    }
    return collections::toBool(this);
  }

  if (instanceof(SimpleXMLElementLoader::classof())) {
    // SimpleXMLElement is the only non-collection class that has custom bool
    // casting.
    if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
      raise_notice("SimpleXMLElement to boolean cast");
    }
    return SimpleXMLElement_objectCast(this, KindOfBoolean).toBoolean();
  }

  always_assert(false);
}

int64_t ObjectData::toInt64() const {
  /* SimpleXMLElement is the only class that has proper custom num casting. */
  if (LIKELY(!instanceof(SimpleXMLElementLoader::classof()))) {
    throwObjToIntException(classname_cstr());
  }
  if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
    raise_notice("SimpleXMLElement to integer cast");
  }
  return SimpleXMLElement_objectCast(this, KindOfInt64).toInt64();
}

double ObjectData::toDouble() const {
  /* SimpleXMLElement is the only class that has proper custom num casting. */
  if (LIKELY(!instanceof(SimpleXMLElementLoader::classof()))) {
    throwObjToDoubleException(classname_cstr());
  }
  if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
    raise_notice("SimpleXMLElement to double cast");
  }
  return SimpleXMLElement_objectCast(this, KindOfDouble).toDouble();
}

///////////////////////////////////////////////////////////////////////////////
// instance methods and properties

const StaticString s_getIterator("getIterator");

Object ObjectData::iterableObject(bool& isIterable,
                                  bool mayImplementIterator /* = true */) {
  assertx(mayImplementIterator || !isIterator());
  if (mayImplementIterator && isIterator()) {
    isIterable = true;
    return Object(this);
  }
  Object obj(this);
  CoeffectsAutoGuard _;
  while (obj->instanceof(SystemLib::getIteratorAggregateClass())) {
    auto iterator =
      obj->o_invoke_few_args(s_getIterator, RuntimeCoeffects::automatic(), 0);
    if (!iterator.isObject()) break;
    auto o = iterator.getObjectData();
    if (o->isIterator()) {
      isIterable = true;
      return Object{o};
    }
    obj.reset(o);
  }
  if (!isIterator() && obj->instanceof(SimpleXMLElementLoader::classof())) {
    if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
      raise_notice("SimpleXMLElement used as iterator");
    }
    isIterable = true;
    return create_object(
      SimpleXMLElementIteratorLoader::className(),
      make_vec_array(obj)
    );
  }
  isIterable = false;
  return obj;
}

Array& ObjectData::dynPropArray() const {
  assertx(getAttribute(HasDynPropArr));
  assertx(g_context->dynPropTable.contains(this));
  assertx(g_context->dynPropTable[this].arr().isDict());
  return g_context->dynPropTable[this].arr();
}

void ObjectData::setDynProps(const Array& newArr) {
  // don't expose the ref returned by setDynPropArr
  (void)setDynPropArray(newArr.toDict());
}

void ObjectData::reserveDynProps(int numDynamic) {
  // don't expose the ref returned by reserveProperties()
  (void)reserveProperties(numDynamic);
}

Array& ObjectData::reserveProperties(int numDynamic /* = 2 */) {
  if (getAttribute(HasDynPropArr)) {
    return dynPropArray();
  }

  auto const allocsz = VanillaDict::computeAllocBytesFromMaxElms(numDynamic);
  if (UNLIKELY(allocsz > kMaxSmallSize && tl_heap->preAllocOOM(allocsz))) {
    check_non_safepoint_surprise();
  }

  return setDynPropArray(Array::attach(
      VanillaDict::MakeReserveDict(numDynamic)));
}

Array& ObjectData::setDynPropArray(const Array& newArr) {
  assertx(!g_context->dynPropTable.contains(this));
  assertx(!getAttribute(HasDynPropArr));
  assertx(newArr.isDict());

  if (m_cls->forbidsDynamicProps()) {
    throw_object_forbids_dynamic_props(getClassName().data());
  }
  if (Cfg::Eval::NoticeOnCreateDynamicProp) {
    IterateKV(newArr.get(), [&] (TypedValue k, TypedValue v) {
      auto const key = tvCastToString(k);
      raiseCreateDynamicProp(key.get());
    });
  }

  // newArr can have refcount 2 or higher
  auto& arr = g_context->dynPropTable[this].arr();
  assertx(arr.isNull());
  arr = newArr;
  setAttribute(HasDynPropArr);
  return arr;
}

tv_lval ObjectData::makeDynProp(const StringData* key) {
  if (Cfg::Eval::NoticeOnCreateDynamicProp) {
    raiseCreateDynamicProp(key);
  }
  if (!reserveProperties().exists(StrNR(key))) {
    reserveProperties().set(StrNR(key), make_tv<KindOfNull>());
  }
  return reserveProperties().lval(StrNR(key), AccessFlags::Key);
}

void ObjectData::setDynProp(const StringData* key, TypedValue val) {
  if (Cfg::Eval::NoticeOnCreateDynamicProp) {
    raiseCreateDynamicProp(key);
  }
  reserveProperties().set(StrNR(key), val, true);
}

Variant ObjectData::o_get(const OptString& propName, bool error /* = true */,
                          const OptString& context /*= null_string*/) {
  assertx(kindIsValid());

  // This is not (just) a check for empty string; property names that start
  // with null are intentionally being rejected here.
  if (UNLIKELY(!*propName.data())) {
    throw_invalid_property_name(propName);
  }

  Class* ctx = nullptr;
  if (!context.empty()) {
    ctx = Class::lookup(context.get());
  }
  auto const propCtx =
    ctx ? MemberLookupContext(ctx, ctx->moduleName()) : nullctx;

  // Can't use propImpl here because if the property is not accessible and
  // there is no native get, propImpl will raise_error("Cannot access ...",
  // but o_get will only (maybe) raise_notice("Undefined property ..." :-(

  auto const lookup = getPropImpl<false, true, true>(propCtx, propName.get());
  if (lookup.val && lookup.accessible) {
    if (lookup.val.type() != KindOfUninit) {
      return Variant::wrap(lookup.val.tv());
    } else if (lookup.prop && (lookup.prop->attrs & AttrLateInit)) {
      if (error) throw_late_init_prop(lookup.prop->cls, propName.get(), false);
      return uninit_null();
    }
  }

  if (error) {
    SystemLib::throwUndefinedPropertyExceptionObject(
       fmt::format("Undefined property: {}::${}",
                      getClassName().data(),
                      propName.data()));
  }

  return uninit_null();
}

void ObjectData::o_set(const OptString& propName, const Variant& v,
                       const OptString& context /* = null_string */) {
  assertx(kindIsValid());

  // This is not (just) a check for empty string; property names that start
  // with null are intentionally being rejected here.
  if (UNLIKELY(!*propName.data())) {
    throw_invalid_property_name(propName);
  }

  Class* ctx = nullptr;
  if (!context.empty()) {
    ctx = Class::lookup(context.get());
  }
  auto const propCtx =
    ctx ? MemberLookupContext(ctx, ctx->moduleName()) : nullctx;

  // Can't use setProp here because if the property is not accessible and
  // there is no native set, setProp will raise_error("Cannot access ...",
  // but o_set will skip writing and return normally.

  auto const lookup = getPropImpl<true, false, true>(propCtx, propName.get());
  auto prop = lookup.val;
  if (prop && lookup.accessible) {
    if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
      throwMutateConstProp(lookup.slot);
    }
    auto val = tvToInit(*v.asTypedValue());
    verifyTypeHint(m_cls, lookup.prop, &val);
    tvSet(val, prop);
    return;
  }

  if (!prop) {
    setDynProp(propName.get(), tvToInit(*v.asTypedValue()));
  }
}

void ObjectData::o_setArray(const Array& properties) {
  for (ArrayIter iter(properties); iter; ++iter) {
    OptString k = iter.first().toString();
    Class* ctx = nullptr;
    // If the key begins with a NUL, it's a private or protected property. Read
    // the class name from between the two NUL bytes.
    //
    // Note: if you change this, you need to change similar logic in
    // apc-object.
    if (!k.empty() && k[0] == '\0') {
      int subLen = k.find('\0', 1) + 1;
      OptString cls = k.substr(1, subLen - 2);
      if (cls.size() == 1 && cls[0] == '*') {
        // Protected.
        ctx = m_cls;
      } else {
        // Private.
        ctx = Class::lookup(cls.get());
        if (!ctx) continue;
      }
      k = k.substr(subLen);
    }
    // TODO(T126821336): property can be internal
    // This function is only used in ext_mysql.cpp,
    // but has its own encoding mechanism
    auto const propCtx =
      ctx ? MemberLookupContext(ctx, ctx->moduleName()) : nullctx;
    setProp(propCtx, k.get(), tvAssertPlausible(iter.secondVal()));
  }
}

void ObjectData::o_getArray(Array& props,
                            bool pubOnly /* = false */,
                            bool ignoreLateInit /* = false */) const {
  assertx(kindIsValid());

  // Fast path for classes with no declared properties
  if (!m_cls->numDeclProperties() && getAttribute(HasDynPropArr)) {
    props = dynPropArray();
    if (Cfg::Eval::NoticeOnReadDynamicProp) {
      IterateKV(props.get(), [&](TypedValue k, TypedValue) {
        auto const key = tvCastToString(k);
        raiseReadDynamicProp(key.get());
      });
    }
    return;
  }

  auto cls = m_cls;
  if (cls->hasReifiedGenerics()) {
    auto const slot = cls->lookupReifiedInitProp();
    assertx(slot != kInvalidSlot);
    auto const declProps = cls->declProperties();
    auto const prop = declProps[slot];
    auto val = this->propRvalAtOffset(slot);
    props.set(StrNR(prop.name).asString(), val.tv());
  }
  IteratePropToArrayOrder(
    this,
    [&](Slot slot, const Class::Prop& prop, tv_rval val) {
      assertx(assertTypeHint(val, slot));
      if (UNLIKELY(val.type() == KindOfUninit)) {
        if (!ignoreLateInit && (prop.attrs & AttrLateInit)) {
          throw_late_init_prop(prop.cls, prop.name, false);
        }
      } else if (!pubOnly || (prop.attrs & AttrPublic)) {
        // Skip all the reified properties since we already prepended the
        // current class' reified property to the list
        if (prop.name != s_86reified_prop.get()) {
          props.set(StrNR(prop.mangledName()).asString(), val.tv());
        }
      }
    },
    [&](TypedValue key_tv, TypedValue val) {
      props.set(key_tv, val, true);
      if (Cfg::Eval::NoticeOnReadDynamicProp) {
        auto const key = tvCastToString(key_tv);
        raiseReadDynamicProp(key.get());
      }
    }
  );

  if (m_cls->needsInitThrowable()) {
    throwable_mark_array(this, props);
  }
}

template <IntishCast IC /* = IntishCast::None */>
Array ObjectData::toArray(bool pubOnly /* = false */,
                          bool ignoreLateInit /* = false */) const {
  assertx(kindIsValid());

  // We can quickly tell if this object is a collection, which lets us avoid
  // checking for each class in turn if it's not one.
  if (isCollection()) {
    return collections::toArray<IC>(this);
  } else if (UNLIKELY(m_cls->rtAttribute(Class::CallToImpl))) {
    // If we end up with other classes that need special behavior, turn the
    // assert into an if and add cases.
    assertx(instanceof(SimpleXMLElementLoader::classof()));
    if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
      raise_notice("SimpleXMLElement to array cast");
    }
    return SimpleXMLElement_darrayCast(this);
  } else if (UNLIKELY(instanceof(SystemLib::getArrayIteratorClass()))) {
    SystemLib::throwInvalidOperationExceptionObject(
      "ArrayIterator to array cast"
    );
    not_reached();
  } else if (UNLIKELY(instanceof(c_Closure::classof()))) {
    return make_vec_array(Object(const_cast<ObjectData*>(this)));
  } else if (UNLIKELY(instanceof(DateTimeData::classof()))) {
    return Native::data<DateTimeData>(this)->getDebugInfo();
  } else {
    auto ret = Array::CreateDict();
    o_getArray(ret, pubOnly, ignoreLateInit);
    return ret;
  }
}

template
Array ObjectData::toArray<IntishCast::None>(bool, bool) const;
template
Array ObjectData::toArray<IntishCast::Cast>(bool, bool) const;


namespace {

size_t getPropertyIfAccessible(ObjectData* obj,
                               const MemberLookupContext& ctx,
                               const StringData* key,
                               Array& properties,
                               size_t propLeft) {
  auto const prop = obj->getProp(ctx, key);
  if (prop && prop.type() != KindOfUninit) {
    --propLeft;
    properties.set(StrNR(key), prop.tv(), true);
  }
  return propLeft;
}

}

Array ObjectData::o_toIterArray(const Class* ctx) {
  if (!m_cls->numDeclProperties()) {
    if (getAttribute(HasDynPropArr)) {
      auto const props = dynPropArray();
      if (Cfg::Eval::NoticeOnReadDynamicProp) {
        IterateKV(props.get(), [&](TypedValue k, TypedValue) {
          auto const key = tvCastToString(k);
          raiseReadDynamicProp(key.get());
        });
      }
      // not returning Array&; makes a copy
      return props;
    }
    return Array::CreateDict();
  }

  size_t accessibleProps = m_cls->declPropNumAccessible();
  size_t size = accessibleProps;
  if (getAttribute(HasDynPropArr)) {
    size += dynPropArray().size();
  }
  Array retArray { Array::attach(VanillaDict::MakeReserveDict(size)) };

  auto const propCtx =
    ctx ? MemberLookupContext(ctx, ctx->moduleName()) : nullctx;

  // Get all declared properties first, bottom-to-top in the inheritance
  // hierarchy, in declaration order.
  const Class* klass = m_cls;
  while (klass) {
    const PreClass::Prop* props = klass->preClass()->properties();
    const size_t numProps = klass->preClass()->numProperties();

    for (size_t i = 0; i < numProps; ++i) {
      auto key = const_cast<StringData*>(props[i].name());
      accessibleProps = getPropertyIfAccessible(
          this, propCtx, key, retArray, accessibleProps);
    }
    klass = klass->parent();
  }
  if (!(m_cls->attrs() & AttrNoExpandTrait) && accessibleProps > 0) {
    // we may have properties from traits
    for (auto const& prop : m_cls->declProperties()) {
      auto const key = prop.name.get();
      if (!retArray.get()->exists(key)) {
        accessibleProps = getPropertyIfAccessible(
          this, propCtx, key, retArray, accessibleProps);
        if (accessibleProps == 0) break;
      }
    }
  }

  // Now get dynamic properties.
  if (getAttribute(HasDynPropArr)) {
    auto& dynProps = dynPropArray();
    auto ad = dynProps.get();
    ssize_t iter = ad->iter_begin();
    auto pos_limit = ad->iter_end();
    while (iter != pos_limit) {
      ad = dynProps.get();
      auto const key = ad->nvGetKey(iter);
      iter = ad->iter_advance(iter);

      if (Cfg::Eval::NoticeOnReadDynamicProp) {
        auto const k = tvCastToString(key);
        raiseReadDynamicProp(k.get());
      }

      // You can get this if you cast an array to object. These
      // properties must be dynamic because you can't declare a
      // property with a non-string name.
      if (UNLIKELY(!isStringType(key.m_type))) {
        assertx(key.m_type == KindOfInt64);
        auto const val = dynProps.get()->at(key.m_data.num);
        retArray.set(key.m_data.num, val);
        continue;
      }

      auto const strKey = key.m_data.pstr;
      auto const val = dynProps.get()->at(strKey);
      retArray.set(StrNR(strKey), val, true /* isKey */);
    }
  }

  return retArray;
}

static bool decode_invoke(const OptString& s, ObjectData* obj, bool fatal,
                          CallCtx& ctx) {
  ctx.this_ = obj;
  ctx.cls = obj->getVMClass();
  ctx.dynamic = true;

  ctx.func = ctx.cls->lookupMethod(s.get());
  if (!ctx.func) {
    // Bail if this_ is non-null AND we could not find a method.
    o_invoke_failed(ctx.cls->name()->data(), s.data(), fatal);
    return false;
  }

  // Null out this_ for statically called methods
  if (ctx.func->isStaticInPrologue()) {
    ctx.this_ = nullptr;
  }
  return true;
}

Variant ObjectData::o_invoke(const OptString& s, const Variant& params,
                             bool fatal /* = true */) {
  CallCtx ctx;
  if (!decode_invoke(s, this, fatal, ctx) ||
      (!isContainer(params) && !params.isNull())) {
    return Variant(Variant::NullInit());
  }
  CoeffectsAutoGuard _;
  return Variant::attach(
    g_context->invokeFunc(ctx, params, nullptr, RuntimeCoeffects::automatic())
  );
}

Variant ObjectData::o_invoke_few_args(const OptString& s,
                                      RuntimeCoeffects providedCoeffects,
                                      int count,
                                      const Variant& a0 /* = uninit_variant*/,
                                      const Variant& a1 /* = uninit_variant*/,
                                      const Variant& a2 /* = uninit_variant*/,
                                      const Variant& a3 /* = uninit_variant*/,
                                      const Variant& a4 /* = uninit_variant*/) {

  CallCtx ctx;
  if (!decode_invoke(s, this, true, ctx)) {
    return Variant(Variant::NullInit());
  }

  TypedValue args[5];
  switch(count) {
    default: not_implemented();
    case  5: tvCopy(*a4.asTypedValue(), args[4]); [[fallthrough]];
    case  4: tvCopy(*a3.asTypedValue(), args[3]); [[fallthrough]];
    case  3: tvCopy(*a2.asTypedValue(), args[2]); [[fallthrough]];
    case  2: tvCopy(*a1.asTypedValue(), args[1]); [[fallthrough]];
    case  1: tvCopy(*a0.asTypedValue(), args[0]); [[fallthrough]];
    case  0: break;
  }

  return Variant::attach(
    g_context->invokeFuncFew(ctx, count, nullptr, args, providedCoeffects)
  );
}

ObjectData* ObjectData::clone() {
  if (isCppBuiltin()) {
    assertx(!m_cls->hasMemoSlots());
    if (isCollection()) return collections::clone(this);
    if (instanceof(c_Closure::classof())) {
      return c_Closure::fromObject(this)->clone();
    }
    assertx(instanceof(c_Awaitable::classof()));
    // cloning WaitHandles is not allowed
    // invoke the instanceCtor to get the right sort of exception
    auto const ctor = m_cls->instanceCtor();
    ctor(m_cls);
    always_assert(false);
  }

  // clone prevents a leak if something throws before clone() returns
  Object clone;
  auto const nProps = m_cls->numDeclProperties();
  if (hasNativeData()) {
    assertx(m_cls->instanceDtor() == Native::nativeDataInstanceDtor);
    clone = Object::attach(
      Native::nativeDataInstanceCopyCtor(this, m_cls, nProps)
    );
    assertx(clone->hasExactlyOneRef());
    assertx(clone->hasInstanceDtor());
  } else {
    auto const alloc = allocMemoInit(m_cls);

    auto const obj = new (NotNull{}, alloc.mem)
                     ObjectData(m_cls, InitRaw{}, alloc.flags);
    clone = Object::attach(obj);
    assertx(clone->hasExactlyOneRef());
    assertx(!clone->hasInstanceDtor());
  }

  auto const cloneProps = clone->props();
  for (auto slot = Slot{0}; slot < nProps; slot++) {
    auto index = m_cls->propSlotToIndex(slot);
    auto const prop = props()->at(index);
    if (!isLazyProp(prop)) {
      tvDup(*prop, cloneProps->at(index));
      assertx(assertTypeHint(cloneProps->at(index), slot));
    } else {
      auto const cloneProp = cloneProps->at(index);
      type(cloneProp) = type(prop);
      val(cloneProp).num = val(prop).num;
    }
  }

  if (UNLIKELY(getAttribute(HasDynPropArr))) {
    clone->setAttribute(HasDynPropArr);
    g_context->dynPropTable.emplace(clone.get(), dynPropArray().get());
  }
  if (m_cls->rtAttribute(Class::HasClone)) {
    assertx(!isCppBuiltin());
    auto const method = clone->m_cls->lookupMethod(s_clone.get());
    assertx(method);
    clone->unlockObject();
    SCOPE_EXIT { clone->lockObject(); };
    CoeffectsAutoGuard _;
    g_context->invokeMethodV(clone.get(), method, InvokeArgs{}, nullptr,
                             RuntimeCoeffects::automatic());
  }
  return clone.detach();
}

bool ObjectData::equal(const ObjectData& other) const {
  if (this == &other) return true;
  if (isCollection()) {
    return collections::equals(this, &other);
  }
  if (UNLIKELY(instanceof(SystemLib::getDateTimeInterfaceClass()) &&
               other.instanceof(SystemLib::getDateTimeInterfaceClass()))) {
    return DateTimeData::compare(this, &other) == 0;
  }
  if (getVMClass() != other.getVMClass()) return false;
  if (UNLIKELY(instanceof(SimpleXMLElementLoader::classof()))) {
    if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
      raise_notice("SimpleXMLElement equality comparison");
    }
    // Compare the whole object (including native data), not just props
    auto ar1 = SimpleXMLElement_darrayCast(this);
    auto ar2 = SimpleXMLElement_darrayCast(&other);
    return ArrayData::Equal(ar1.get(), ar2.get());
  }
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // First comparison already proves they are different
    return false;
  }

  // check for dynamic props first because we need to short-circuit if there's
  // a different number of them
  auto thisSize = UNLIKELY(getAttribute(HasDynPropArr)) ?
    dynPropArray().size() : 0;
  size_t otherSize = 0;
  ArrayData* otherDynProps = nullptr;
  if (UNLIKELY(other.getAttribute(HasDynPropArr))) {
    otherDynProps = other.dynPropArray().get();
    otherSize = otherDynProps->size();
  }
  if (thisSize != otherSize) return false;

  // Prevent circular referenced objects/arrays or deep ones.
  check_recursion_error();

  bool result = true;
  IteratePropMemOrder(
    this,
    [&](Slot slot, const Class::Prop& prop, tv_rval thisVal) {
      auto otherVal = other.propRvalAtOffset(slot);
      if ((UNLIKELY(thisVal.type() == KindOfUninit) ||
           UNLIKELY(otherVal.type() == KindOfUninit)) &&
          (prop.attrs & AttrLateInit)) {
        throw_late_init_prop(prop.cls, prop.name, false);
      }
      if (!tvEqual(thisVal.tv(), otherVal.tv())) {
        result = false;
        return true;
      }
      return false;
    },
    [&](TypedValue key, TypedValue thisVal) {
      auto const otherVal = otherDynProps->get(key);
      if (!otherVal.is_init() || !tvEqual(thisVal, otherVal)) {
        result = false;
        return true;
      }
      return false;
    }
  );
  return result;
}

bool ObjectData::less(const ObjectData& other) const {
  // compare is not symmetrical; order of operands matters here
  return compare(other) < 0;
}

bool ObjectData::lessEqual(const ObjectData& other) const {
  // compare is not symmetrical; order of operands matters here
  return compare(other) <= 0;
}

bool ObjectData::more(const ObjectData& other) const {
  // compare is not symmetrical; order of operands matters here
  return other.compare(*this) < 0;
}

bool ObjectData::moreEqual(const ObjectData& other) const {
  // compare is not symmetrical; order of operands matters here
  return other.compare(*this) <= 0;
}

int64_t ObjectData::compare(const ObjectData& other) const {
  if (isCollection() || other.isCollection()) {
    throw_collection_compare_exception();
  }
  if (this == &other) return 0;
  if (UNLIKELY(instanceof(SystemLib::getDateTimeInterfaceClass()) &&
               other.instanceof(SystemLib::getDateTimeInterfaceClass()))) {
    return DateTimeData::compare(this, &other);
  }
  if (getVMClass() != other.getVMClass()) {
    const auto lhs = make_tv<DataType::Object>(const_cast<ObjectData*>(this));
    const auto rhs = make_tv<DataType::Object>(const_cast<ObjectData*>(&other));
    throwCmpBadTypesException(&lhs, &rhs);
    not_reached();
  }
  if (UNLIKELY(instanceof(SimpleXMLElementLoader::classof()))) {
    if (Cfg::Eval::NoticeOnSimpleXMLBehavior) {
      raise_notice("SimpleXMLElement comparison");
    }
    // Compare the whole object (including native data), not just props
    auto ar1 = SimpleXMLElement_darrayCast(this);
    auto ar2 = SimpleXMLElement_darrayCast(&other);
    return ArrayData::Compare(ar1.get(), ar2.get());
  }
  if (UNLIKELY(instanceof(c_Closure::classof()))) {
    // comparing different closures with <=> always returns 1
    return 1;
  }

  // check for dynamic props first, because we need to short circuit if there's
  // a different number of them
  auto thisSize = UNLIKELY(getAttribute(HasDynPropArr)) ?
    dynPropArray().size() : 0;
  size_t otherSize = 0;
  ArrayData* otherDynProps = nullptr;
  if (UNLIKELY(other.getAttribute(HasDynPropArr))) {
    otherDynProps = other.dynPropArray().get();
    otherSize = otherDynProps->size();
  }
  if (thisSize > otherSize) {
    return 1;
  } else if (thisSize < otherSize) {
    return -1;
  }

  // Prevent circular referenced objects/arrays or deep ones.
  check_recursion_error();

  int64_t result = 0;
  IteratePropToArrayOrder(
    this,
    [&](Slot slot, const Class::Prop& prop, tv_rval thisVal) {
      auto otherVal = other.propRvalAtOffset(slot);
      if ((UNLIKELY(thisVal.type() == KindOfUninit) ||
           UNLIKELY(otherVal.type() == KindOfUninit)) &&
          (prop.attrs & AttrLateInit)) {
        throw_late_init_prop(prop.cls, prop.name, false);
      }
      auto cmp = tvCompare(thisVal.tv(), otherVal.tv());
      if (cmp != 0) {
        result = cmp;
        return true;
      }
      return false;
    },
    [&](TypedValue key, TypedValue thisVal) {
      auto const otherVal = otherDynProps->get(key);
      if (!otherVal.is_init()) {
        result = 1;
        return true;
      }
      auto cmp = tvCompare(thisVal, otherVal);
      if (cmp != 0) {
        result = cmp;
        return true;
      }
      return false;
    }
  );
  return result;
}

///////////////////////////////////////////////////////////////////////////////

const StaticString
  s___sleep("__sleep"),
  s___toDebugDisplay("__toDebugDisplay"),
  s___wakeup("__wakeup"),
  s___debugInfo("__debugInfo");

void deepInitHelper(ObjectProps* props,
                    const Class::PropInitVec* initVec,
                    size_t nProps) {
  auto initIter = initVec->cbegin();
  props->foreach(nProps, [&](tv_lval lval){
    auto entry = *initIter++;
    tvCopy(entry.val.tv(), lval);
    if (entry.deepInit) {
      tvIncRefGen(*lval);
      collections::deepCopy(lval);
    }
  });
}

void ObjectData::setReifiedGenerics(Class* cls, ArrayData* reifiedTypes) {
  auto const arg = make_array_like_tv(reifiedTypes);
  auto const meth = cls->lookupMethod(s_86reifiedinit.get());
  assertx(meth != nullptr);
  g_context->invokeMethod(this, meth, InvokeArgs(&arg, 1),
                          nullptr, RuntimeCoeffects::fixme());
}

// called from jit code
ObjectData* ObjectData::newInstanceRawSmall(Class* cls, size_t size,
                                            size_t index) {
  assertx(size <= kMaxSmallSize);
  assertx(!cls->hasMemoSlots());
  assertx(cls->sizeIdx() == index);
  auto mem = tl_heap->mallocSmallIndexSize(index, size);
  auto const flags = IsBeingConstructed | SmallAllocSize;
  return new (NotNull{}, mem) ObjectData(cls, InitRaw{}, flags);
}

ObjectData* ObjectData::newInstanceRawBig(Class* cls, size_t size) {
  assertx(!cls->hasMemoSlots());
  auto mem = tl_heap->mallocBigSize(size);
  auto const flags = IsBeingConstructed | BigAllocSize;
  return new (NotNull{}, mem) ObjectData(cls, InitRaw{}, flags);
}

// called from jit code
ObjectData* ObjectData::newInstanceRawMemoSmall(Class* cls,
                                                size_t size,
                                                size_t index,
                                                size_t objoff) {
  assertx(size <= kMaxSmallSize);
  assertx(cls->hasMemoSlots());
  assertx(!cls->getNativeDataInfo());
  assertx(objoff == ObjectData::objOffFromMemoNode(cls));
  assertx(cls->sizeIdx() == index);
  auto mem = tl_heap->mallocSmallIndexSize(index, size);
  new (NotNull{}, mem) MemoNode(objoff);
  mem = reinterpret_cast<char*>(mem) + objoff;
  auto const flags = IsBeingConstructed | SmallAllocSize;
  return new (NotNull{}, mem) ObjectData(cls, InitRaw{}, flags);
}

ObjectData* ObjectData::newInstanceRawMemoBig(Class* cls,
                                              size_t size,
                                              size_t objoff) {
  assertx(cls->hasMemoSlots());
  assertx(!cls->getNativeDataInfo());
  assertx(objoff == ObjectData::objOffFromMemoNode(cls));
  auto mem = tl_heap->mallocBigSize(size);
  new (NotNull{}, mem) MemoNode(objoff);
  mem = reinterpret_cast<char*>(mem) + objoff;
  auto const flags = IsBeingConstructed | BigAllocSize;
  return new (NotNull{}, mem) ObjectData(cls, InitRaw{}, flags);
}

// Note: the normal object destruction path does not actually call this
// destructor.  See ObjectData::release.
ObjectData::~ObjectData() {
  if (UNLIKELY(slowDestroyCheck())) {
    // The only builtin classes that use ~ObjectData and support memoization
    // are ones with native data, and the memo slot cleanup for them happens
    // in nativeDataInstanceDtor.
    assertx(!getAttribute(UsedMemoCache) || hasNativeData());
    if (getAttribute(HasDynPropArr)) freeDynPropArray(this);
    if (getAttribute(IsWeakRefed)) {
      WeakRefData::invalidateWeakRef((uintptr_t)this);
    }
  }
}

Object ObjectData::FromArray(ArrayData* properties) {
  auto const props = properties->toDict(true);
  Object retval{SystemLib::getstdClassClass()};
  retval->setAttribute(HasDynPropArr);
  g_context->dynPropTable.emplace(retval.get(), props);
  if (props != properties) decRefArr(props);
  return retval;
}

void ObjectData::throwMutateConstProp(Slot prop) const {
  throw_cannot_modify_const_prop(
    getClassName().data(),
    m_cls->declProperties()[prop].name->data()
  );
}

void ObjectData::throwMustBeMutable(Slot prop) const {
  throw_must_be_mutable(
    getClassName().data(),
    m_cls->declProperties()[prop].name->data()
  );
}

void ObjectData::throwMustBeEnclosedInReadonly(Slot prop) const {
  throw_must_be_enclosed_in_readonly(
    getClassName().data(),
    m_cls->declProperties()[prop].name->data()
  );
}

void ObjectData::throwMustBeReadonly(Slot prop) const {
  throw_must_be_readonly(
    getClassName().data(),
    m_cls->declProperties()[prop].name->data()
  );
}

void ObjectData::throwMustBeValueType(Slot prop) const {
  throw_must_be_value_type(
    getClassName().data(),
    m_cls->declProperties()[prop].name->data()
  );
}

void ObjectData::checkReadonly(const PropLookup& lookup, ReadonlyOp op,
                               bool writeMode) const {
  if ((op == ReadonlyOp::CheckMutROCOW && lookup.readonly) ||
    op == ReadonlyOp::CheckROCOW) {
    vmMInstrState().roProp = true;
  }
  if (lookup.readonly) {
    if (op == ReadonlyOp::CheckMutROCOW || op == ReadonlyOp::CheckROCOW) {
      if (type(lookup.val) == KindOfObject) {
        throwMustBeValueType(lookup.slot);
      }
    } else if (op == ReadonlyOp::Mutable) {
      if (writeMode) {
        throwMustBeMutable(lookup.slot);
      } else {
        throwMustBeEnclosedInReadonly(lookup.slot);
      }
    }
  } else if (op == ReadonlyOp::Readonly || op == ReadonlyOp::CheckROCOW) {
    throwMustBeReadonly(lookup.slot);
  }
}

void ObjectData::deserializeAllLazyProps() {
  if (!m_cls->currentlyUsingLazyAPCDeserialization()) return;
  props()->foreach(m_cls->numDeclProperties(), [&](tv_lval lval) {
    if (isLazyProp(lval)) deserializeLazyProp(lval);
  });
}

void ObjectData::deserializeLazyProp(tv_lval prop) {
  assertx(isLazyProp(prop));
  auto const handle = reinterpret_cast<APCHandle*>(prop.val().num);
  assertx(handle->checkInvariants());
  tvCopy(handle->toLocalHelper(false).detach(), prop);
}

bool ObjectData::isLazyProp(tv_rval prop) {
  return prop.type() == kInvalidDataType;
}

template <bool forWrite, bool forRead, bool ignoreLateInit>
ALWAYS_INLINE
ObjectData::PropLookup ObjectData::getPropImpl(
  const MemberLookupContext& propCtx,
  const StringData* key
) {
  auto const lookup = m_cls->getDeclPropSlot(propCtx, key);
  auto const propSlot = lookup.slot;

  if (LIKELY(propSlot != kInvalidSlot)) {
    // We found a visible property in one of the object's slots. Immediately
    // deserialize it if it's a lazy prop. Then, check if it's accessible.
    auto const propIndex = m_cls->propSlotToIndex(propSlot);
    auto prop = props()->at(propIndex);
    if (isLazyProp(prop)) deserializeLazyProp(prop);
    assertx(assertTypeHint(prop, propSlot));

    auto const& declProp = m_cls->declProperties()[propSlot];
    if (!ignoreLateInit && lookup.accessible) {
      if (UNLIKELY(type(prop) == KindOfUninit) &&
          (declProp.attrs & AttrLateInit)) {
        throw_late_init_prop(declProp.cls, key, false);
      }
    }

    // If the prop is internal, check that modules are compatible
    if (lookup.internal &&
        will_symbol_raise_module_boundary_violation(&declProp, &propCtx)) {
      raiseModulePropertyViolation(m_cls, key, propCtx.moduleName(), false);
    }

    return {
     prop,
     &declProp,
     propSlot,
     lookup.accessible,
     // we always return true in the !forWrite case; this way the compiler
     // may optimize away this value, and if a caller intends to write but
     // instantiates with false by mistake it will always see const
     forWrite
       ? bool(declProp.attrs & AttrIsConst)
       : true,
     lookup.readonly
    };
  }

  // We could not find a visible declared property. We need to check for a
  // dynamic property with this name.
  if (UNLIKELY(getAttribute(HasDynPropArr))) {
    auto& arr = dynPropArray();
    if (arr->exists(key)) {
      if (forRead && Cfg::Eval::NoticeOnReadDynamicProp) {
        raiseReadDynamicProp(key);
      }
      // Returning a non-declared property. We know that it is accessible and
      // not const since all dynamic properties are. If we may write to
      // the property we need to allow the array to escalate.
      auto const lval = arr.lval(StrNR(key), AccessFlags::Key);
      return { lval, nullptr, kInvalidSlot, true, !forWrite, false };
    }
  }

  return { nullptr, nullptr, kInvalidSlot, false, !forWrite, false };
}

tv_lval ObjectData::getPropLval(const MemberLookupContext& ctx, const StringData* key) {
  auto const lookup = getPropImpl<true, false, true>(ctx, key);
  if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
    throwMutateConstProp(lookup.slot);
  }
  return lookup.val && lookup.accessible ? lookup.val : nullptr;
}

tv_rval ObjectData::getProp(const MemberLookupContext& ctx, const StringData* key) const {
  auto const lookup = const_cast<ObjectData*>(this)
    ->getPropImpl<false, true, false>(ctx, key);
  return lookup.val && lookup.accessible ? lookup.val : nullptr;
}

tv_rval ObjectData::getPropIgnoreLateInit(const MemberLookupContext& ctx,
                                          const StringData* key) const {
  auto const lookup = const_cast<ObjectData*>(this)
    ->getPropImpl<false, true, true>(ctx, key);
  return lookup.val && lookup.accessible ? lookup.val : nullptr;
}

tv_lval ObjectData::getPropIgnoreAccessibility(const StringData* key) {
  auto const lookup = getPropImpl<false, true, true>(MemberLookupContext(nullptr, (const StringData*) nullptr), key);
  auto prop = lookup.val;
  if (!prop) return nullptr;
  if (lookup.prop && type(prop) == KindOfUninit &&
      (lookup.prop->attrs & AttrLateInit)) {
    throw_late_init_prop(lookup.prop->cls, key, false);
  }
  return prop;
}

//////////////////////////////////////////////////////////////////////

template<ObjectData::PropMode mode>
ALWAYS_INLINE
tv_lval ObjectData::propImpl(TypedValue* tvRef, const MemberLookupContext& ctx,
                             const StringData* key, const ReadonlyOp op) {
  auto constexpr write = (mode == PropMode::DimForWrite);
  auto constexpr read = (mode == PropMode::ReadNoWarn) ||
                        (mode == PropMode::ReadWarn);
  auto const lookup = getPropImpl<write, read, false>(ctx, key);
  auto const prop = lookup.val;
  if (prop) {
    if (lookup.accessible) {
      auto const checkPropAttrs = [&]() {
        if (mode == PropMode::DimForWrite) {
          if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
            throwMutateConstProp(lookup.slot);
          }
        }
        checkReadonly(lookup, op, mode == PropMode::DimForWrite);
        return prop;
      };

      // Property exists, is accessible, and is not unset.
      if (type(prop) != KindOfUninit) return checkPropAttrs();

      if (mode == PropMode::ReadWarn) throwUndefPropException(key);
      if (write) return checkPropAttrs();
      return const_cast<TypedValue*>(&immutable_null_base);
    }

    // Property exists, but it is either protected or private since accessible
    // is false.
    auto const propSlot = m_cls->lookupDeclProp(key);
    auto const attrs = m_cls->declProperties()[propSlot].attrs;
    auto const priv = (attrs & AttrPrivate) ? "private" : "protected";

    raise_error(
      "Cannot access %s property %s::$%s",
      priv,
      m_cls->preClass()->name()->data(),
      key->data()
    );
  }

  // First see if native getter is implemented.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::getProp(Object{this}, StrNR(key));
    if (r.isInitialized()) {
      tvCopy(r.detach(), *tvRef);
      return tvRef;
    }
  }

  if (UNLIKELY(!*key->data())) {
    throw_invalid_property_name(StrNR(key));
  }

  if (mode == PropMode::ReadWarn) throwUndefPropException(key);
  if (write) return makeDynProp(key);
  return const_cast<TypedValue*>(&immutable_null_base);
}

tv_lval ObjectData::prop(
  TypedValue* tvRef,
  const MemberLookupContext& ctx,
  const StringData* key,
  const ReadonlyOp op
) {
  return propImpl<PropMode::ReadNoWarn>(tvRef, ctx, key, op);
}

tv_lval ObjectData::propW(
  TypedValue* tvRef,
  const MemberLookupContext& ctx,
  const StringData* key,
  const ReadonlyOp op
) {
  return propImpl<PropMode::ReadWarn>(tvRef, ctx, key, op);
}

tv_lval ObjectData::propU(
  TypedValue* tvRef,
  const MemberLookupContext& ctx,
  const StringData* key,
  const ReadonlyOp op
) {
  return propImpl<PropMode::DimForWrite>(tvRef, ctx, key, op);
}

tv_lval ObjectData::propD(
  TypedValue* tvRef,
  const MemberLookupContext& ctx,
  const StringData* key,
  const ReadonlyOp op
) {
  return propImpl<PropMode::DimForWrite>(tvRef, ctx, key, op);
}

bool ObjectData::propIsset(const MemberLookupContext& ctx, const StringData* key) {
  auto const lookup = getPropImpl<false, true, true>(ctx, key);
  if (lookup.val && lookup.accessible) {
    if (lookup.val.type() != KindOfUninit) {
      return lookup.val.type() != KindOfNull;
    }
    if (lookup.prop && (lookup.prop->attrs & AttrLateInit)) {
      return false;
    }
  }

  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::issetProp(Object{this}, StrNR(key));
    if (r.isInitialized()) return r.toBoolean();
  }

  return false;
}

void ObjectData::setProp(const MemberLookupContext& ctx, const StringData* key, TypedValue val, ReadonlyOp op) {
  assertx(tvIsPlausible(val));
  assertx(val.m_type != KindOfUninit);

  auto const lookup = getPropImpl<true, false, true>(ctx, key);
  auto const prop = lookup.val;

  if (prop && lookup.accessible) {
    if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
      throwMutateConstProp(lookup.slot);
    }
    checkReadonly(lookup, op, true);
    // TODO(T61738946): We can remove the temporary here once we no longer
    // coerce class_meth types.
    Variant tmp = tvAsVariant(&val);
    verifyTypeHint(m_cls, lookup.prop, tmp.asTypedValue());
    tvMove(tmp.detach(), prop);
    return;
  }

  // First see if native setter is implemented.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::setProp(Object{this}, StrNR(key), tvAsCVarRef(&val));
    if (r.isInitialized()) return;
  }

  if (prop) raise_error("Cannot access protected property");

  if (UNLIKELY(!*key->data())) {
    throw_invalid_property_name(StrNR(key));
  }
  setDynProp(key, val);
}

tv_lval ObjectData::setOpProp(TypedValue& tvRef,
                              const MemberLookupContext& ctx,
                              SetOpOp op,
                              const StringData* key,
                              TypedValue* val) {
  auto const lookup = getPropImpl<true, true, false>(ctx, key);
  auto prop = lookup.val;

  if (prop && lookup.accessible) {
    if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
      throwMutateConstProp(lookup.slot);
    }

    auto const needsCheck = lookup.prop &&
      setOpNeedsTypeCheck(lookup.prop->typeConstraints, op, prop);

    if (needsCheck) {
      /*
       * If this property has a type-hint, we can't do the setop truly in
       * place. We need to verify that the new value satisfies the type-hint
       * before assigning back to the property (if we raise a warning and throw,
       * we don't want to have already put the value into the prop).
       */
      TypedValue temp;
      tvDup(*prop, temp);
      SCOPE_FAIL { tvDecRefGen(&temp); };
      setopBody(&temp, op, val);
      verifyTypeHint(m_cls, lookup.prop, &temp);
      tvMove(temp, prop);
    } else {
      setopBody(prop, op, val);
    }
    return prop;
  }

  if (UNLIKELY(!*key->data())) throw_invalid_property_name(StrNR(key));

  // Native accessors.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::getProp(Object{this}, StrNR(key));
    if (r.isInitialized()) {
      setopBody(r.asTypedValue(), op, val);
      auto r2 = Native::setProp(Object{this}, StrNR(key), r);
      if (r2.isInitialized()) {
        tvCopy(r.detach(), tvRef);
        return &tvRef;
      }
    }
  }

  if (prop) raise_error("Cannot access protected property");

  // No visible/accessible property, and no applicable native method:
  // create a new dynamic property.  (We know this is a new property,
  // or it would've hit the visible && accessible case above.)
  prop = makeDynProp(key);
  assertx(type(prop) == KindOfNull); // cannot exist yet
  setopBody(prop, op, val);
  return prop;
}

TypedValue ObjectData::incDecProp(const MemberLookupContext& ctx, IncDecOp op, const StringData* key) {
  auto const lookup = getPropImpl<true, true, false>(ctx, key);
  auto prop = lookup.val;

  if (prop && lookup.accessible) {
    if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
      throwMutateConstProp(lookup.slot);
    }
    if (type(prop) == KindOfUninit) {
      tvWriteNull(prop);
    }

    /*
     * If this property has a type-hint, we can't do the inc-dec truely in
     * place. We need to verify that the new value satisfies the type-hint
     * before assigning back to the property (if we raise a warning and throw,
     * we don't want to have already put the value into the prop).
     *
     * If the prop is an integer and we're doing the common pre/post inc/dec
     * ops, we know the type won't change, so we can skip the type-hint check in
     * that case.
     */
    auto const fast = [&]{
      if (Cfg::Eval::CheckPropTypeHints <= 0) return true;
      auto const isAnyCheckable = lookup.prop && [&] {
        for (auto const& tc : lookup.prop->typeConstraints.range()) {
          if (tc.isCheckable()) return true;
        }
        return false;
      }();
      if (!isAnyCheckable) return true;

      if (!isIntType(type(prop))) return false;
      return
        op == IncDecOp::PreInc || op == IncDecOp::PostInc ||
        op == IncDecOp::PreDec || op == IncDecOp::PostDec;
    }();
    if (fast) return IncDecBody(op, tvAssertPlausible(prop));

    TypedValue temp;
    tvDup(tvAssertPlausible(*prop), temp);
    SCOPE_FAIL { tvDecRefGen(&temp); };
    auto result = IncDecBody(op, &temp);
    SCOPE_FAIL { tvDecRefGen(&result); };
    verifyTypeHint(m_cls, lookup.prop, &temp);
    tvMove(temp, tvAssertPlausible(prop));
    return result;
  }

  if (UNLIKELY(!*key->data())) throw_invalid_property_name(StrNR(key));

  // Native accessors.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::getProp(Object{this}, StrNR(key));
    if (r.isInitialized()) {
      auto const dest = IncDecBody(op, r.asTypedValue());
      auto r2 = Native::setProp(Object{this}, StrNR(key), r);
      if (r2.isInitialized()) return dest;
    }
  }

  if (prop) raise_error("Cannot access protected property");

  // No visible/accessible property, and no applicable native method:
  // create a new dynamic property.  (We know this is a new property,
  // or it would've hit the visible && accessible case above.)
  prop = makeDynProp(key);
  assertx(type(prop) == KindOfNull); // cannot exist yet
  return IncDecBody(op, prop);
}

void ObjectData::unsetProp(const MemberLookupContext& ctx, const StringData* key) {
  auto const lookup = getPropImpl<true, false, true>(ctx, key);
  auto const prop = lookup.val;

  if (prop && lookup.accessible &&
      (type(prop) != KindOfUninit ||
       (lookup.prop && (lookup.prop->attrs & AttrLateInit)))) {
    if (lookup.slot != kInvalidSlot) {
      // Declared property.
      if (UNLIKELY(lookup.isConst) && !isBeingConstructed()) {
        throwMutateConstProp(lookup.slot);
      }
      unsetTypeHint(lookup.prop);
      tvSet(*uninit_variant.asTypedValue(), prop);
    } else {
      // Dynamic property.
      dynPropArray().remove(StrNR(key).asString(), true /* isString */);
    }
    return;
  }

  // Native unset first.
  if (m_cls->rtAttribute(Class::HasNativePropHandler)) {
    auto r = Native::unsetProp(Object{this}, StrNR(key));
    if (r.isInitialized()) return;
  }

  if (prop && !lookup.accessible) {
    // Defined property that is not accessible.
    raise_error("Cannot unset inaccessible property");
  }

  if (UNLIKELY(!*key->data())) {
    throw_invalid_property_name(StrNR(key));
  }
}

void ObjectData::throwObjToIntException(const char* clsName) {
  SystemLib::throwTypecastExceptionObject(fmt::format(
    "Object of class {} could not be converted to int", clsName));
}

void ObjectData::throwObjToDoubleException(const char* clsName) {
  SystemLib::throwTypecastExceptionObject(fmt::format(
    "Object of class {} could not be converted to float", clsName));
}

void ObjectData::raiseAbstractClassError(Class* cls) {
  Attr attrs = cls->attrs();
  raise_error("Cannot instantiate %s %s",
              (attrs & AttrInterface) ? "interface" :
              (attrs & AttrTrait)     ? "trait" :
              (attrs & (AttrEnum|AttrEnumClass)) ? "enum" : "abstract class",
              cls->preClass()->name()->data());
}

void ObjectData::throwUndefPropException(const StringData* key) const {
  SystemLib::throwUndefinedPropertyExceptionObject(
    fmt::format("Undefined property: {}::${}",
                   m_cls->name()->data(),
                   key->data()));
}

void ObjectData::raiseCreateDynamicProp(const StringData* key) const {
  if (m_cls == SystemLib::getstdClassClass() ||
      m_cls == SystemLib::get__PHP_Incomplete_ClassClass()) {
    // these classes (but not classes derived from them) don't get notices
    return;
  }
  if (key->isStatic()) {
    raise_notice("Created dynamic property with static name %s::%s",
                 m_cls->name()->data(), key->data());
  } else {
    raise_notice("Created dynamic property with dynamic name %s::%s",
                 m_cls->name()->data(), key->data());
  }
}

void ObjectData::raiseReadDynamicProp(const StringData* key) const {
  if (m_cls == SystemLib::getstdClassClass() ||
      m_cls == SystemLib::get__PHP_Incomplete_ClassClass()) {
    // these classes (but not classes derived from them) don't get notices
    return;
  }
  if (key->isStatic()) {
    raise_notice("Read dynamic property with static name %s::%s",
                 m_cls->name()->data(), key->data());
  } else {
    raise_notice("Read dynamic property with dynamic name %s::%s",
                 m_cls->name()->data(), key->data());
  }
}

Variant ObjectData::InvokeSimple(ObjectData* obj, const StaticString& name,
                                 RuntimeCoeffects providedCoeffects) {
  auto const meth = obj->methodNamed(name.get());
  return meth
    ? g_context->invokeMethodV(obj, meth, InvokeArgs{}, nullptr, providedCoeffects)
    : uninit_null();
}

Variant ObjectData::invokeSleep(RuntimeCoeffects provided) {
  return InvokeSimple(this, s___sleep, provided);
}

Variant ObjectData::invokeToDebugDisplay(RuntimeCoeffects provided) {
  return InvokeSimple(this, s___toDebugDisplay, provided);
}

Variant ObjectData::invokeWakeup(RuntimeCoeffects provided) {
  unlockObject();
  SCOPE_EXIT { lockObject(); };
  return InvokeSimple(this, s___wakeup, provided);
}

Variant ObjectData::invokeDebugInfo(RuntimeCoeffects provided) {
  return InvokeSimple(this, s___debugInfo, provided);
}

OptString ObjectData::invokeToString() {
  if (Cfg::Eval::FatalOnConvertObjectToString) {
    raise_convert_object_to_string(classname_cstr());
  }

  const Func* method = m_cls->getToString();
  if (!method) {
    // If the object does not define a __toString() method, raise a
    // recoverable error
    raise_recoverable_error(
      "Object of class %s could not be converted to string",
      classname_cstr()
    );
    // If the user error handler decides to allow execution to continue,
    // we return the empty string.
    return String::Empty();
  }
  // Implicit object-to-string coercion is disallowed: throw rather than invoke __toString().
  SystemLib::throwTypecastExceptionObject(fmt::format(
    "Object of class {} could not be converted to string", classname_cstr()));
}

bool ObjectData::hasToString() {
  return (m_cls->getToString() != nullptr);
}

const char* ObjectData::classname_cstr() const {
  return getClassName().data();
}

} // HPHP
