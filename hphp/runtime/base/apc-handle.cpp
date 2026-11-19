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
#include "hphp/runtime/base/apc-handle.h"

#include "hphp/runtime/base/apc-array.h"
#include "hphp/runtime/base/apc-bespoke.h"
#include "hphp/runtime/base/apc-clsmeth.h"
#include "hphp/runtime/base/apc-collection.h"
#include "hphp/runtime/base/apc-named-entity.h"
#include "hphp/runtime/base/apc-object.h"
#include "hphp/runtime/base/apc-rclass-meth.h"
#include "hphp/runtime/base/apc-rfunc.h"
#include "hphp/runtime/base/apc-string.h"
#include "hphp/runtime/base/apc-typed-value.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

const StaticString s_invalidMethCaller("Cannot store meth_caller in APC");

APCHandle::Pair APCHandle::Create(const_variant_ref source,
                                  APCHandleLevel level,
                                  bool unserializeObj,
                                  bool pure) {
  auto const cell = source.asTypedValue();
  switch (cell.type()) {
    case KindOfUninit: {
      auto const value = APCTypedValue::tvUninit();
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfNull: {
      auto const value = APCTypedValue::tvNull();
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfBoolean: {
      auto const value = val(cell).num ? APCTypedValue::tvTrue()
                                       : APCTypedValue::tvFalse();
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfInt64: {
      auto const value = new APCTypedValue(val(cell).num);
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfDouble: {
      auto const value = new APCTypedValue(val(cell).dbl);
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfRFunc:
      return APCRFunc::Construct(val(cell).prfunc);
    case KindOfFunc: {
      auto const func = val(cell).pfunc;
      if (func->isMethCaller()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCaller.get()}
        );
      }
      if (!func->isMethod()) {
        if (func->isPersistent()) {
          auto const value = new APCTypedValue(func);
          return {value->getHandle(), sizeof(APCTypedValue)};
        }
        auto const value = new APCNamedFunc(func);
        return {value->getHandle(), sizeof(APCNamedFunc)};
      }
      invalidFuncConversion("string");
    }
    case KindOfClass: {
      auto const cls = val(cell).pclass;
      if (cls->isPersistent()) {
        auto const value = new APCTypedValue(cls);
        return {value->getHandle(), sizeof(APCTypedValue)};
      }
      auto const value = new APCTypedValue(LazyClassData::create(cls->name()));
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfLazyClass: {
      auto const value = new APCTypedValue(val(cell).plazyclass);
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    case KindOfString: {
      auto const s = val(cell).pstr;
      if (auto const value = APCTypedValue::HandlePersistent(s)) {
        return value;
      }
      if (auto const st = lookupStaticString(s)) {
        auto const value = new APCTypedValue(APCTypedValue::StaticStr{}, st);
        return {value->getHandle(), sizeof(APCTypedValue)};
      }
      auto const st = StringData::MakeShared(s->slice());
      auto const value = new APCTypedValue(APCTypedValue::SharedStr{}, st);
      return {value->getHandle(), st->size() + sizeof(APCTypedValue)};
    }

    case KindOfVec: {
      auto const ad = val(cell).parr;
      assertx(ad->isVecType());
      return APCArray::MakeCopiedVec(ad, level, unserializeObj, pure);
    }

    case KindOfDict: {
      auto const ad = val(cell).parr;
      assertx(ad->isDictType());
      return APCArray::MakeCopiedDict(ad, level, unserializeObj, pure);
    }

    case KindOfKeyset: {
      auto const ad = val(cell).parr;
      assertx(ad->isKeysetType());
      return APCArray::MakeCopiedKeyset(ad, level, unserializeObj);
    }

    case KindOfObject:
      if (val(cell).pobj->isCollection()) {
        return APCCollection::Make(val(cell).pobj,
                                   level,
                                   unserializeObj,
                                   pure);
      }
      return unserializeObj ? APCObject::Construct(val(cell).pobj, pure) :
             APCString::MakeSerializedObject(apc_serialize(source, pure));

    case KindOfResource:
    case KindOfEnumClassLabel:
      return APCArray::MakeCopiedEmptyVec();

    case KindOfClsMeth: {
      auto const meth = val(cell).pclsmeth;
      if (meth->getCls()->isPersistent()) {
        auto const value = new APCTypedValue(meth);
        return {value->getHandle(), sizeof(APCTypedValue)};
      }
      auto const value = new APCClsMeth(meth->getCls(), meth->getFunc());
      return {value->getHandle(), sizeof(APCClsMeth)};
    }

    case KindOfRClsMeth:
      return APCRClsMeth::Construct(val(cell).prclsmeth);
  }
  not_reached();
}

Variant APCHandle::toLocalHelper(bool pure) const {
  assertx(!isTypedValue());
  switch (m_kind) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::PersistentFunc:
    case APCKind::PersistentClass:
    case APCKind::LazyClass:
    case APCKind::PersistentClsMeth:
    case APCKind::StaticArray:
    case APCKind::StaticBespoke:
    case APCKind::StaticString:
    case APCKind::SharedArray:
    case APCKind::SharedBespoke:
    case APCKind::SharedString:
      not_reached();

    case APCKind::FuncEntity:
      return APCNamedFunc::fromHandle(this)->getEntityOrNull();

    case APCKind::ClsMeth:
      return APCClsMeth::fromHandle(this)->getEntityOrNull();

    case APCKind::SerializedVec: {
      auto const serVec = APCString::fromHandle(this)->getStringData();
      auto const v = apc_unserialize(serVec->data(), serVec->size(), pure);
      assertx(v.isVec());
      return v;
    }
    case APCKind::SerializedDict: {
      auto const serDict = APCString::fromHandle(this)->getStringData();
      auto const v = apc_unserialize(serDict->data(), serDict->size(), pure);
      assertx(v.isDict());
      return v;
    }
    case APCKind::SerializedKeyset: {
      auto const serKeyset = APCString::fromHandle(this)->getStringData();
      auto const v = apc_unserialize(serKeyset->data(), serKeyset->size(), true /* irrelevant for arraykeys */);
      assertx(v.isKeyset());
      return v;
    }
    case APCKind::CopiedVec:
      return Variant::attach(
        APCArray::fromHandle(this)->toLocalVec(pure)
      );
    case APCKind::CopiedLegacyVec:
      return Variant::attach(
        APCArray::fromHandle(this)->toLocalLegacyVec(pure)
      );
    case APCKind::CopiedDict:
      return Variant::attach(
        APCArray::fromHandle(this)->toLocalDict(pure)
      );
    case APCKind::CopiedLegacyDict:
      return Variant::attach(
        APCArray::fromHandle(this)->toLocalLegacyDict(pure)
      );
    case APCKind::CopiedKeyset:
      return Variant::attach(
        APCArray::fromHandle(this)->toLocalKeyset()
      );
    case APCKind::SerializedObject: {
      auto const serObj = APCString::fromHandle(this)->getStringData();
      return apc_unserialize(serObj->data(), serObj->size(), pure);
    }
    case APCKind::CopiedCollection:
      return APCCollection::fromHandle(this)->createObject(pure);
    case APCKind::CopiedObject:
      return APCObject::MakeLocalObject(this, pure);
    case APCKind::RFunc:
      return APCRFunc::Make(this);
    case APCKind::RClsMeth:
      return APCRClsMeth::Make(this);
  }
  not_reached();
}

bool APCHandle::toLocalMayRaise() const {
  switch (m_kind) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::PersistentFunc:
    case APCKind::PersistentClass:
    case APCKind::LazyClass:
    case APCKind::PersistentClsMeth:
    case APCKind::StaticArray:
    case APCKind::StaticBespoke:
    case APCKind::StaticString:
    case APCKind::SharedArray:
    case APCKind::SharedBespoke:
    case APCKind::SharedString:
      return false;

    case APCKind::FuncEntity:
    case APCKind::ClsMeth:
    case APCKind::SerializedVec:
    case APCKind::SerializedDict:
    case APCKind::SerializedKeyset:
    case APCKind::SerializedObject:
    case APCKind::RFunc:
    case APCKind::RClsMeth:
      return true;

    case APCKind::CopiedVec:
    case APCKind::CopiedLegacyVec:
    case APCKind::CopiedDict:
    case APCKind::CopiedLegacyDict:
    case APCKind::CopiedKeyset:
      return APCArray::fromHandle(this)->toLocalMayRaise();

    case APCKind::CopiedCollection:
      return APCCollection::fromHandle(this)->toLocalMayRaise();

    case APCKind::CopiedObject:
      return APCObject::fromHandle(this)->toLocalMayRaise();
  }
  not_reached();
}

void APCHandle::deleteCopied() {
  assertx(checkInvariants());
  switch (m_kind) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
      return;
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::StaticArray:
    case APCKind::StaticString:
    case APCKind::PersistentFunc:
    case APCKind::PersistentClass:
    case APCKind::LazyClass:
    case APCKind::PersistentClsMeth:
      delete APCTypedValue::fromHandle(this);
      return;

    case APCKind::ClsMeth:
      delete APCClsMeth::fromHandle(this);
      return;

    case APCKind::FuncEntity:
      delete APCNamedFunc::fromHandle(this);
      return;

    case APCKind::SerializedVec:
    case APCKind::SerializedDict:
    case APCKind::SerializedKeyset:
    case APCKind::SerializedObject:
      APCString::Delete(APCString::fromHandle(this));
      return;

    case APCKind::CopiedVec:
    case APCKind::CopiedLegacyVec:
    case APCKind::CopiedDict:
    case APCKind::CopiedLegacyDict:
    case APCKind::CopiedKeyset:
      APCArray::Delete(this);
      return;

    case APCKind::CopiedObject:
      APCObject::Delete(this);
      return;

    case APCKind::CopiedCollection:
      APCCollection::Delete(this);
      return;

    case APCKind::RFunc:
      APCRFunc::Delete(this);
      return;

    case APCKind::RClsMeth:
      APCRClsMeth::Delete(this);
      return;

    case APCKind::StaticBespoke:
      freeAPCBespoke(APCTypedValue::fromHandle(this));
      return;

    case APCKind::SharedArray:
    case APCKind::SharedBespoke:
    case APCKind::SharedString:
      assertx(false);
      return;
  }
  not_reached();
}

bool APCHandle::checkInvariants() const {
  switch (m_kind) {
    case APCKind::Uninit:
      assertx(m_type == KindOfUninit);
      return true;
    case APCKind::Null:
      assertx(m_type == KindOfNull);
      return true;
    case APCKind::Bool:
      assertx(m_type == KindOfBoolean);
      return true;
    case APCKind::Int:
      assertx(m_type == KindOfInt64);
      return true;
    case APCKind::Double:
      assertx(m_type == KindOfDouble);
      return true;
    case APCKind::PersistentFunc:
      assertx(m_type == KindOfFunc);
      return true;
    case APCKind::PersistentClass:
      assertx(m_type == KindOfClass);
      return true;
    case APCKind::LazyClass:
      assertx(m_type == KindOfLazyClass);
      return true;
    case APCKind::PersistentClsMeth:
      assertx(m_type == KindOfClsMeth);
      return true;
    case APCKind::StaticString:
    case APCKind::SharedString:
      assertx(m_type == KindOfString);
      return true;
    case APCKind::StaticArray:
    case APCKind::StaticBespoke:
    case APCKind::SharedArray:
    case APCKind::SharedBespoke:
      assertx(m_type == KindOfVec ||
              m_type == KindOfDict ||
              m_type == KindOfKeyset);
      return true;
    case APCKind::FuncEntity:
    case APCKind::ClsMeth:
    case APCKind::RFunc:
    case APCKind::RClsMeth:
    case APCKind::CopiedVec:
    case APCKind::CopiedLegacyVec:
    case APCKind::CopiedDict:
    case APCKind::CopiedLegacyDict:
    case APCKind::CopiedKeyset:
    case APCKind::CopiedObject:
    case APCKind::CopiedCollection:
    case APCKind::SerializedVec:
    case APCKind::SerializedDict:
    case APCKind::SerializedKeyset:
    case APCKind::SerializedObject:
      assertx(m_type == kInvalidDataType);
      return true;
  }
  not_reached();
  return false;
}

void APCHandle::unreferenceRoot(size_t size) {
  assertx(isSingletonKind() || m_unref_root_count++ == 0);
  if (!isShared()) {
    atomicDecRef();
  } else if (APCTypedValue::UseStringHazardPointers()) {
    APCTypedValue::fromHandle(this)->deleteShared();
  } else {
    g_context->enqueueAPCHandle(this, size);
  }
}

//////////////////////////////////////////////////////////////////////

}
