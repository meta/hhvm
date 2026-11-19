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
#include "hphp/runtime/base/typed-value.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/variable-serializer.h"

#include "hphp/runtime/vm/coeffects.h"

#include "hphp/util/trace.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

// Code should not use the values of these TypedValues, so we set them to
// unusual values that may (and, indeed, have) caught logic errors.
const TypedValue immutable_null_base{{0xdeadbeef}, KindOfNull};
const TypedValue immutable_uninit_base{{0xfacade}, KindOfUninit};

std::string TypedValue::pretty() const {
  char buf[20];
  snprintf(buf, sizeof(buf), "0x%lx", long(m_data.num));
  return Trace::prettyNode(tname(m_type).c_str(), std::string(buf));
}

StaticCoeffects ConstModifiers::getCoeffects(ConstModifierFlags flags) const {
  assertx(flags.kind == ConstModifierFlags::Kind::Context);
  return StaticCoeffects::fromValue(u_coeffectsData);
}

void ConstModifiers::setCoeffects(StaticCoeffects coeffects) {
  u_coeffectsData = coeffects.value();
}

//////////////////////////////////////////////////////////////////////

void TypedValue::serde(BlobEncoder& encoder) const {
  switch (m_type) {
    case KindOfUninit:
      encoder(KindOfUninit);
      break;
    case KindOfNull:
      encoder(KindOfNull);
      break;
    case KindOfBoolean:
      encoder(KindOfBoolean);
      encoder(m_data.num ? true : false);
      break;
    case KindOfInt64:
      encoder(KindOfInt64);
      encoder(m_data.num);
      break;
    case KindOfDouble:
      encoder(KindOfDouble);
      encoder(m_data.dbl);
      break;

    case KindOfEnumClassLabel:
      encoder(KindOfEnumClassLabel);
      encoder(static_cast<const StringData*>(m_data.pstr));
      break;

    case KindOfString: {
      encoder(KindOfString);
      const StringData* s = m_data.pstr->isStatic()
        ? m_data.pstr : makeStaticString(m_data.pstr);
      encoder(s);
      break;
    }

    case KindOfVec:
      assertx(m_data.parr);
      encoder(KindOfVec);
      encoder((const ArrayData*)m_data.parr);
      break;

    case KindOfKeyset:
      assertx(m_data.parr);
      encoder(KindOfKeyset);
      encoder((const ArrayData*)m_data.parr);
      break;

    case KindOfDict:
      assertx(m_data.parr);
      encoder(KindOfDict);
      encoder((const ArrayData*)m_data.parr);
      break;

    case KindOfLazyClass:
      encoder(KindOfLazyClass);
      encoder(m_data.plazyclass.name());
      break;

    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfClsMeth:
    case KindOfRClsMeth: {
      encoder(m_type);
      auto const s = internal_serialize(tvAsCVarRef(this));
      assertx(s.get());
      auto const sz = s.size();
      assertx(sz > 0);
      assertx(sz <= std::numeric_limits<uint32_t>::max());
      encoder(static_cast<uint32_t>(sz));
      encoder.writeRaw(s.data(), sz);
      break;
    }
  }
}

void TypedValue::serde(BlobDecoder& decoder,
                       bool makeStatic) {
  decoder(m_type);

  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
      break;

    case KindOfBoolean: {
      bool b;
      decoder(b);
      m_data.num = b ? 1 : 0;
      break;
    }
    case KindOfInt64:
      decoder(m_data.num);
      break;
    case KindOfDouble:
      decoder(m_data.dbl);
      break;

    case KindOfEnumClassLabel: {
      const StringData* s;
      decoder(s); // Ignore makeStatic since ECL requires static string.
      assertx(s->isStatic());
      m_data.pstr = const_cast<StringData*>(s);
      break;
    }

    case KindOfString: {
      const StringData* s;
      decoder(s, makeStatic);
      assertx(s);
      assertx(IMPLIES(makeStatic, s->isStatic()));
      if (!makeStatic) m_type = KindOfString;
      m_data.pstr = const_cast<StringData*>(s);
      break;
    }

    case KindOfVec: {
      const ArrayData* a;
      decoder(a, makeStatic);
      assertx(a && a->isVecType());
      assertx(IMPLIES(makeStatic, a->isStatic()));
      if (!makeStatic) m_type = KindOfVec;
      m_data.parr = const_cast<ArrayData*>(a);
      break;
    }

    case KindOfKeyset: {
      const ArrayData* a;
      decoder(a, makeStatic);
      assertx(a && a->isKeysetType());
      assertx(IMPLIES(makeStatic, a->isStatic()));
      if (!makeStatic) m_type = KindOfKeyset;
      m_data.parr = const_cast<ArrayData*>(a);
      break;
    }

    case KindOfDict: {
      const ArrayData* a;
      decoder(a, makeStatic);
      assertx(a && a->isDictType());
      assertx(IMPLIES(makeStatic, a->isStatic()));
      if (!makeStatic) m_type = KindOfDict;
      m_data.parr = const_cast<ArrayData*>(a);
      break;
    }

    case KindOfLazyClass: {
      const StringData* s;
      decoder(s); // Don't pass makeStatic here. LazyClassData
                  // requires a static string.
      assertx(s->isStatic());
      m_data.plazyclass = LazyClassData::create(s);
      break;
    }

    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfClsMeth:
    case KindOfRClsMeth: {
      tvWriteUninit(*this);

      uint32_t size;
      decoder(size);
      assertx(size > 0);

      auto const data = decoder.data();
      std::string str{data, data+size};
      decoder.advance(size);

      tvAsVariant(this) = unserialize_from_buffer(
        str.data(),
        str.size(),
        VariableUnserializer::Type::Internal
      );
      if (makeStatic) tvAsVariant(this).setEvalScalar();
      break;
    }
  }
}

//////////////////////////////////////////////////////////////////////

}
