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

#include "hphp/runtime/base/variable-serializer.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/array-provenance.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/utf8-decode.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-vec-defs.h"
#include "hphp/runtime/base/zend-functions.h"
#include "hphp/runtime/base/zend-printf.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/base/zstd-string-buffer.h"

#include "hphp/runtime/ext/collections/ext_collections.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"
#include "hphp/runtime/ext/json/JSON_parser.h"
#include "hphp/runtime/ext/json/ext_json.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/rds-local.h"

#include <algorithm>
#include <cmath>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_serializedNativeDataKey("\0native"),
  s_JsonSerializable("JsonSerializable"),
  s_jsonSerialize("jsonSerialize"),
  s_serialize("serialize"),
  s_zero("\0"),
  s_protected_prefix("\0*\0"),
  s_PHP_DebugDisplay("__PHP_DebugDisplay"),
  s_PHP_Incomplete_Class("__PHP_Incomplete_Class"),
  s_PHP_Incomplete_Class_Name("__PHP_Incomplete_Class_Name"),
  s_debugInfo("__debugInfo"),
  s_message("message");

[[noreturn]] NEVER_INLINE
static void throwNestingException() {
  throw ExtendedException("Nesting level too deep - recursive dependency?");
}

///////////////////////////////////////////////////////////////////////////////

template <class Buffer>
VariableSerializerImpl<Buffer>::SavedRefMap::~SavedRefMap() {
  for (auto& i : m_mapping) {
    tvDecRefGen(const_cast<TypedValue*>(&i.first));
  }
}

template <class Buffer>
VariableSerializerImpl<Buffer>::~VariableSerializerImpl() {
}

template <class Buffer>
VariableSerializerImpl<Buffer>::VariableSerializerImpl(Type type,
                                                       int option /* = 0 */,
                                                       int maxRecur /* = 3 */)
  : m_type(type)
  , m_option(option)
  , m_keepDVArrays{type != Type::Serialize}
  , m_maxCount(maxRecur)
{
  if (type == Type::DebuggerSerialize && g_context) {
    m_maxLevelDebugger = g_context->debuggerSettings.printLevel;
  }
}

template <class Buffer>
VariableSerializerBase::ArrayKind
VariableSerializerImpl<Buffer>::getKind(const ArrayData* arr) const {
  if (UNLIKELY(m_forcePHPArrays)) {
    return VariableSerializer::ArrayKind::PHP;
  } else if (UNLIKELY(m_forceHackArrays)) {
    if (arr->isDictType()) {
      if (bespoke::TypeStructure::isBespokeTypeStructure(arr)) {
        return VariableSerializer::ArrayKind::BespokeTypeStructure;
      }
      return VariableSerializer::ArrayKind::Dict;
    } else if (arr->isVecType()) {
      return VariableSerializer::ArrayKind::Vec;
    }
    assertx(arr->isKeysetType());
    return VariableSerializer::ArrayKind::Keyset;
  }

  auto const respectsLegacyBit = [&] {
    switch (getType()) {
    case Type::PrintR:
    case Type::VarDump:
    case Type::VarExport:
    case Type::Serialize:
    case Type::JSON:
    case Type::DebuggerDump:
    case Type::DebuggerSerialize:
      return true;
    case Type::Internal:
    case Type::DebugDump:
    case Type::PHPOutput:
    case Type::APCSerialize:
      return false;
    }
    always_assert(false);
  }();

  auto const serializesLegacyBit =
    getType() == Type::Internal || getType() == Type::APCSerialize ||
    (getType() == Type::Serialize && m_serializeProvenanceAndLegacy);

  if (serializesLegacyBit && arr->isLegacyArray()) {
    assertx(!arr->isKeysetType());
    if (m_keepDVArrays && arr->isVecType()) {
      return VariableSerializer::ArrayKind::MarkedVArray;
    }
    return VariableSerializer::ArrayKind::MarkedDArray;
  }

  if (respectsLegacyBit && arr->isLegacyArray()) {
    assertx(!arr->isKeysetType());
    if (m_keepDVArrays) {
      return arr->isVecType() ? VariableSerializer::ArrayKind::VArray
                              : VariableSerializer::ArrayKind::DArray;
    }
    return VariableSerializer::ArrayKind::PHP;
  }

  if (arr->isDictType()) {
    if (bespoke::TypeStructure::isBespokeTypeStructure(arr)) {
      return VariableSerializer::ArrayKind::BespokeTypeStructure;
    }
    return VariableSerializer::ArrayKind::Dict;
  }
  if (arr->isVecType())  return VariableSerializer::ArrayKind::Vec;
  assertx(arr->isKeysetType());
  return VariableSerializer::ArrayKind::Keyset;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::pushObjectInfo(const OptString& objClass, char objCode) {
  assertx(objCode == 'O' || objCode == 'V' || objCode == 'K');
  m_objectInfos.emplace_back(
    ObjectInfo { m_objClass, m_objCode, m_rsrcName, m_rsrcId }
  );
  m_objClass = objClass;
  m_objCode = objCode;
  m_rsrcName.reset();
  m_rsrcId = 0;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::pushResourceInfo(const OptString& rsrcName, int rsrcId) {
  m_objectInfos.emplace_back(
    ObjectInfo { m_objClass, m_objCode, m_rsrcName, m_rsrcId }
  );
  m_objClass.reset();
  m_objCode = 0;
  m_rsrcName = rsrcName;
  m_rsrcId = rsrcId;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::popObjectInfo() {
  ObjectInfo &info = m_objectInfos.back();
  m_objClass = info.objClass;
  m_objCode = info.objCode;
  m_rsrcName = info.rsrcName;
  m_rsrcId = info.rsrcId;
  m_objectInfos.pop_back();
}

RDS_LOCAL(VariableSerializerBase::SerializationLimitWrapper,
    VariableSerializerBase::serializationSizeLimit);

template <class Buffer>
void VariableSerializerImpl<Buffer>::popResourceInfo() {
  popObjectInfo();
}

template <class Buffer>
Buffer VariableSerializerImpl<Buffer>::makeOutputBuffer() {
  return Buffer{};
}

template <class Buffer>
OptString VariableSerializerImpl<Buffer>::serialize(const_variant_ref v, bool ret,
                                                    bool keepCount /* = false */) {
  Buffer buf = makeOutputBuffer();
  m_buf = &buf;
  if (ret) {
    if (m_ignoreStringSizeLimit) {
      auto const currentUsage = std::max<int64_t>(
        0, tl_heap->getStats().usage());
      auto const remainingMemory = std::max<int64_t>(
        0, tl_heap->getMemoryLimit() - currentUsage);
      auto const limit =
        std::min<int64_t>(StringData::MaxSize, remainingMemory);
      buf.setOutputLimit(safe_cast<uint32_t>(limit));
    } else {
      buf.setOutputLimit(serializationSizeLimit->value);
    }
  } else {
    buf.setOutputLimit(StringData::MaxSize);
  }
  m_valueCount = keepCount ? m_valueCount + 1 : 1;
  write(v);
  if (ret) {
    return m_buf->detach();
  } else {
    OptString str = m_buf->detach();
    g_context->write(str);
  }
  return OptString();
}

template <class Buffer>
OptString VariableSerializerImpl<Buffer>::serializeValue(const Variant& v, bool limit) {
  Buffer buf = makeOutputBuffer();
  m_buf = &buf;
  if (limit) {
    buf.setOutputLimit(serializationSizeLimit->value);
  }
  m_valueCount = 1;
  write(v);
  return m_buf->detach();
}

template <class Buffer>
OptString VariableSerializerImpl<Buffer>::serializeWithLimit(const Variant& v, int limit) {
  if (m_type == Type::Serialize || m_type == Type::Internal ||
      m_type == Type::JSON || m_type == Type::APCSerialize ||
      m_type == Type::DebuggerSerialize) {
    assertx(false);
    return OptString();
  }
  Buffer buf = makeOutputBuffer();
  m_buf = &buf;
  if (serializationSizeLimit->value > 0 &&
      (limit <= 0 || limit > serializationSizeLimit->value)) {
    limit = serializationSizeLimit->value;
  }
  buf.setOutputLimit(limit);
  //Does not need m_valueCount, which is only useful with the unsupported types
  try {
    write(v);
  } catch (StringBufferLimitException& e) {
    return e.m_result;
  }
  return m_buf->detach();
}

///////////////////////////////////////////////////////////////////////////////

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(bool v) {
  switch (m_type) {
  case Type::PrintR:
    if (v) m_buf->append(1);
    break;
  case Type::VarExport:
  case Type::PHPOutput:
  case Type::JSON:
  case Type::DebuggerDump:
    m_buf->append(v ? "true" : "false");
    break;
  case Type::VarDump:
  case Type::DebugDump:
    indent();
    m_buf->append(v ? "bool(true)" : "bool(false)");
    writeRefCount();
    m_buf->append('\n');
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append(v ? "b:1;" : "b:0;");
    break;
  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(int64_t v) {
  switch (m_type) {
  case Type::PrintR:
  case Type::VarExport:
  case Type::PHPOutput:
  case Type::JSON:
  case Type::DebuggerDump:
    m_buf->append(v);
    break;
  case Type::VarDump:
    indent();
    m_buf->append("int(");
    m_buf->append(v);
    m_buf->append(")\n");
    break;
  case Type::DebugDump:
    indent();
    m_buf->append("long(");
    m_buf->append(v);
    m_buf->append(')');
    writeRefCount();
    m_buf->append('\n');
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append("i:");
    m_buf->append(v);
    m_buf->append(';');
    break;
  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(double v) {
  auto const serde_precision = 17;
  auto const precision = m_fullFloatPrecision ? serde_precision : 14;

  switch (m_type) {
  case Type::JSON:
    if (!std::isinf(v) && !std::isnan(v)) {
      char *buf;
      vspprintf(&buf, 0, "%.*k", precision, v);
      m_buf->append(buf);
      if (m_option & k_JSON_PRESERVE_ZERO_FRACTION
          && strchr(buf, '.') == nullptr) {
        m_buf->append(".0");
      }
      free(buf);
    } else {
      json_set_last_error_code(json_error_codes::JSON_ERROR_INF_OR_NAN);

      m_buf->append('0');
    }
    break;
  case Type::VarExport:
  case Type::PHPOutput:
  case Type::PrintR:
  case Type::DebuggerDump:
    {
      char *buf;
      bool isExport = m_type == Type::VarExport || m_type == Type::PHPOutput;
      vspprintf(&buf, 0, isExport ? "%.*H" : "%.*G", precision, v);
      m_buf->append(buf);
      // In PHPOutput mode, we always want doubles to parse as
      // doubles, so make sure there's a decimal point.
      if (m_type == Type::PHPOutput && strpbrk(buf, ".E") == nullptr) {
        m_buf->append(".0");
      }
      free(buf);
    }
    break;
  case Type::VarDump:
  case Type::DebugDump:
    {
      char *buf;
      vspprintf(&buf, 0, "float(%.*G)", precision, v);
      indent();
      m_buf->append(buf);
      free(buf);
      writeRefCount();
      m_buf->append('\n');
    }
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append("d:");
    if (std::isnan(v)) {
      m_buf->append("NAN");
    } else if (std::isinf(v)) {
      if (v < 0) m_buf->append('-');
      m_buf->append("INF");
    } else {
      char *buf;
      vspprintf(&buf, 0, "%.*H", serde_precision, v);
      m_buf->append(buf);
      free(buf);
    }
    m_buf->append(';');
    break;
  default:
    assertx(false);
    break;
  }
}

uint16_t reverse16(uint16_t us) {
  return
    ((us & 0xf) << 12)       | (((us >> 4) & 0xf) << 8) |
    (((us >> 8) & 0xf) << 4) | ((us >> 12) & 0xf);
}

// Potentially need to escape all control characters (< 32) and also "\/<>&'@%
static const bool jsonNoEscape[128] = {
  false, false, false, false, false, false, false, false,
  false, false, false, false, false, false, false, false,
  false, false, false, false, false, false, false, false,
  false, false, false, false, false, false, false, false,
  true,  true,  false, true,  true,  false, false, false,
  true,  true,  true,  true,  true,  true,  true,  false,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  false, true,  false, true,
  false, true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  false, true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
  true,  true,  true,  true,  true,  true,  true,  true,
};

template <class Buffer>
static void appendJsonEscape(Buffer& sb,
                             const char *s,
                             int len,
                             int options) {
  if (len == 0) {
    sb.append("\"\"", 2);
    return;
  }

  static const char digits[] = "0123456789abcdef";

  auto const start = sb.size();
  sb.append('"');

  // Do a fast path for ASCII characters that don't need escaping
  int pos = 0;
  do {
    int c = s[pos];
    if (UNLIKELY((unsigned char)c >= 128 || !jsonNoEscape[c])) {
      goto utf8_decode;
    }
    sb.append((char)c);
    pos++;
  } while (pos < len);
  sb.append('"');
  return;

utf8_decode:
  UTF8To16Decoder decoder(s + pos, len - pos, options & k_JSON_FB_LOOSE);
  for (;;) {
    int c = options & k_JSON_UNESCAPED_UNICODE ? decoder.decodeAsUTF8()
                                               : decoder.decode();
    if (c == UTF8_END) {
      sb.append('"');
      break;
    }
    if (c == UTF8_ERROR) {
      json_set_last_error_code(json_error_codes::JSON_ERROR_UTF8);
      // discard the part that has been already decoded.
      sb.resize(start);
      sb.append("null", 4);
      break;
    }
    assertx(c >= 0);
    unsigned short us = (unsigned short)c;
    switch (us) {
    case '"':
      if (options & k_JSON_HEX_QUOT) {
        sb.append("\\u0022", 6);
      } else {
        sb.append("\\\"", 2);
      }
      break;
    case '\\': sb.append("\\\\", 2); break;
    case '/':
      if (options & k_JSON_UNESCAPED_SLASHES) {
        sb.append('/');
      } else {
        sb.append("\\/", 2);
      }
      break;
    case '\b': sb.append("\\b", 2);  break;
    case '\f': sb.append("\\f", 2);  break;
    case '\n': sb.append("\\n", 2);  break;
    case '\r': sb.append("\\r", 2);  break;
    case '\t': sb.append("\\t", 2);  break;
    case '<':
      if (options & k_JSON_HEX_TAG || options & k_JSON_FB_EXTRA_ESCAPES) {
        sb.append("\\u003C", 6);
      } else {
        sb.append('<');
      }
      break;
    case '>':
      if (options & k_JSON_HEX_TAG) {
        sb.append("\\u003E", 6);
      } else {
        sb.append('>');
      }
      break;
    case '&':
      if (options & k_JSON_HEX_AMP) {
        sb.append("\\u0026", 6);
      } else {
        sb.append('&');
      }
      break;
    case '\'':
      if (options & k_JSON_HEX_APOS) {
        sb.append("\\u0027", 6);
      } else {
        sb.append('\'');
      }
      break;
    case '@':
      if (options & k_JSON_FB_EXTRA_ESCAPES) {
        sb.append("\\u0040", 6);
      } else {
        sb.append('@');
      }
      break;
    case '%':
      if (options & k_JSON_FB_EXTRA_ESCAPES) {
        sb.append("\\u0025", 6);
      } else {
        sb.append('%');
      }
      break;
    default:
      if (us >= ' ' &&
          ((options & k_JSON_UNESCAPED_UNICODE) || (us & 127) == us)) {
        sb.append((char)us);
      } else {
        sb.append("\\u", 2);
        us = reverse16(us);
        sb.append(digits[us & ((1 << 4) - 1)]); us >>= 4;
        sb.append(digits[us & ((1 << 4) - 1)]); us >>= 4;
        sb.append(digits[us & ((1 << 4) - 1)]); us >>= 4;
        sb.append(digits[us & ((1 << 4) - 1)]);
      }
      break;
    }
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(const char *v, int len /* = -1 */,
                                           bool isArrayKey /* = false */,
                                           bool noQuotes /* = false */) {
  if (v == nullptr) v = "";
  if (len < 0) len = strlen(v);

  switch (m_type) {
  case Type::PrintR: {
    m_buf->append(v, len);
    break;
  }
  case Type::VarExport: {
    m_buf->append('\'');
    const char *p = v;
    for (int i = 0; i < len; i++, p++) {
      const char c = *p;
      // adapted from Zend php_var_export and php_addcslashes
      if (c == '\0') {
        m_buf->append("' . \"\\0\" . '");
        continue;
      } else if (c == '\'' || c == '\\') {
        m_buf->append('\\');
      }
      m_buf->append(c);
    }
    m_buf->append('\'');
    break;
  }
  case Type::VarDump:
  case Type::DebugDump: {
    indent();
    m_buf->append("string(");
    m_buf->append(len);
    m_buf->append(") \"");
    m_buf->append(v, len);
    m_buf->append('"');
    writeRefCount();
    m_buf->append('\n');
    break;
  }
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append("s:");
    m_buf->append(len);
    m_buf->append(":\"");
    m_buf->append(v, len);
    m_buf->append("\";");
    break;
  case Type::JSON: {
    if ((m_option & k_JSON_NUMERIC_CHECK) && !isArrayKey) {
      int64_t lval; double dval;
      auto dt = is_numeric_string(v, len, &lval, &dval, 0);
      if (isIntType(dt)) {
        write(lval);
        return;
      } else if (isDoubleType(dt)) {
        write(dval);
        return;
      }
    }
    appendJsonEscape(*m_buf, v, len, m_option);
    break;
  }
  case Type::DebuggerDump:
  case Type::PHPOutput: {
    if (!noQuotes)
      m_buf->append('"');
    for (int i = 0; i < len; ++i) {
      const unsigned char c = v[i];
      switch (c) {
        case '\n': m_buf->append("\\n"); break;
        case '\r': m_buf->append("\\r"); break;
        case '\t': m_buf->append("\\t"); break;
        case '\\': m_buf->append("\\\\"); break;
        case '$':  m_buf->append("\\$"); break;
        case '"':  m_buf->append("\\\""); break;
        default: {
          if (c >= ' ' && c <= '~') {
            // The range [' ', '~'] contains only printable characters
            // and we've already handled special cases above
            m_buf->append(c);
          } else {
            char buf[5];
            snprintf(buf, sizeof(buf), "\\%03o", c);
            m_buf->append(buf);
          }
        }
      }
    }
    if (!noQuotes)
      m_buf->append('"');
    break;
  }
  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(const OptString& v) {
  if (m_type == Type::APCSerialize && !v.isNull() && v.get()->isStatic()) {
    union {
      char buf[8];
      StringData *sd;
    } u;
    u.sd = v.get();
    m_buf->append("S:");
    m_buf->append(u.buf, 8);
    m_buf->append(';');
  } else {
    serializeString(v);
  }
}

const StaticString
  s_invalidMethCallerAPC("Cannot store meth_caller in APC"),
  s_invalidMethCallerSerde("Cannot serialize meth_caller"),
  s_disallowedCollectionSerde("Cannot serialize collection due to options"),
  s_disallowedObjectSerde("Cannot serialize object due to options");

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(const Object& v) {
  if (!v.isNull() && m_type == Type::JSON) {
    if (Cfg::Eval::ForbidMethCallerHelperSerialize &&
        v.get()->getVMClass() == SystemLib::getMethCallerHelperClass()) {
      if (Cfg::Eval::ForbidMethCallerHelperSerialize == 1) {
        raise_warning("Serializing MethCallerHelper");
      } else {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerSerde.get()}
        );
      }
    }

    // m_disallowObjects not relevant in JSON path bc it's only
    //setable via serialize_with_options

    if (!m_skipJsonSerializable && v.instanceof(s_JsonSerializable)) {
      assertx(!v->isCollection());
      auto const providedCoeffects =
        m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
      Variant ret = v->o_invoke_few_args(s_jsonSerialize, providedCoeffects, 0);
      // for non objects or when $this is not returned
      if (!ret.isObject() || ret.getObjectData() != v.get()) {
        if (ret.isArray() || ret.isObject()) {
          preventOverflow(v, [&ret, this]() {
            write(ret);
          });
        } else {
          // Don't need to check for overflows if ret is of primitive type
          // because the depth does not change.
          write(ret);
        }
        return;
      }
    }
    preventOverflow(v, [&v, this]() {
      if (v->isCollection()) {
        serializeCollection(v.get());
      } else if (v->instanceof(c_Closure::classof())) {
        m_buf->append("null");
        json_set_last_error_code(
          json_error_codes::JSON_ERROR_UNSUPPORTED_TYPE);
        return;
      } else {
        auto props = v->toArray(true, m_ignoreLateInit);
        pushObjectInfo(v->getClassName(), 'O');
        serializeObjProps(props);
        popObjectInfo();
      }
    });
  } else {
    serializeObject(v);
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::preventOverflow(const Object& v,
                                                     const std::function<void()>& func) {
  TypedValue tv = make_tv<KindOfObject>(const_cast<ObjectData*>(v.get()));
  if (incNestedLevel(&tv)) {
    writeOverflow(&tv);
  } else {
    func();
  }
  decNestedLevel(&tv);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::write(const_variant_ref v, bool isArrayKey) {
  if (m_type == Type::DebugDump) {
    setRefCount(v.getRefCount());
  }
  if (!isArrayKey && v.isObject()) {
    write(v.toObject());
    return;
  }
  serializeVariant(v.rval(), isArrayKey);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeNull() {
  switch (m_type) {
  case Type::PrintR:
    // do nothing
    break;
  case Type::VarExport:
  case Type::PHPOutput:
    m_buf->append("NULL");
    break;
  case Type::VarDump:
  case Type::DebugDump:
    indent();
    m_buf->append("NULL");
    writeRefCount();
    m_buf->append('\n');
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append("N;");
    break;
  case Type::JSON:
  case Type::DebuggerDump:
    m_buf->append("null");
    break;
  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeOverflow(tv_rval tv) {
  switch (m_type) {
  case Type::PrintR:
    if (!m_objClass.empty()) {
      m_buf->append(m_objClass);
      m_buf->append(" Object\n");
    } else {
      m_buf->append("Array\n");
    }
    m_buf->append(" *RECURSION*");
    break;
  case Type::VarExport:
  case Type::PHPOutput:
    throwNestingException();
  case Type::VarDump:
  case Type::DebugDump:
  case Type::DebuggerDump:
    indent();
    m_buf->append("*RECURSION*\n");
    break;
  case Type::DebuggerSerialize:
    if (m_maxLevelDebugger > 0 && m_levelDebugger > m_maxLevelDebugger) {
      // Not recursion, just cut short of print
      m_buf->append("s:12:\"...(omitted)\";", 20);
      break;
    }
    [[fallthrough]];
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
    {
      int optId = m_refs[tv].m_id;
      assertx(optId != NO_ID);
      bool isObject = tvIsResource(tv) || tvIsObject(tv);
      if (isObject) {
        m_buf->append("r:");
        m_buf->append(optId);
        m_buf->append(';');
      } else {
        m_buf->append("N;");
      }
    }
    break;
  case Type::JSON:
    json_set_last_error_code(json_error_codes::JSON_ERROR_RECURSION);
    m_buf->append("null");
    break;
  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeRefCount() {
  if (m_type != Type::DebugDump) return;

  if (m_refCount >= 0) {
    m_buf->append(" refcount(");
    m_buf->append(m_refCount);
    m_buf->append(')');
  } else if (m_refCount == StaticValue) {
    m_buf->append(" static");
  } else {
    m_buf->append(" uncounted");
  }

  m_refCount = OneReference;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeArrayHeader(int size, bool isVectorData,
                                                      VariableSerializer::ArrayKind kind) {
  m_arrayInfos.push_back(ArrayInfo());
  ArrayInfo &info = m_arrayInfos.back();
  info.first_element = true;
  info.indent_delta = 0;
  info.size = size;

  switch (m_type) {
  case Type::DebuggerDump:
  case Type::PrintR:
    if (!m_rsrcName.empty()) {
      m_buf->append("Resource id #");
      m_buf->append(m_rsrcId);
      if (m_type == Type::DebuggerDump) {
        m_buf->append(" of type ");
        m_buf->append(m_rsrcName);
      }
      break;
    } else if (!m_objClass.empty()) {
      m_buf->append(m_objClass);
      m_buf->append(" Object\n");
    } else {
      switch (kind) {
      case ArrayKind::Dict:
      case ArrayKind::BespokeTypeStructure:
        m_buf->append("Dict\n");
        break;
      case ArrayKind::Vec:
        m_buf->append("Vec\n");
        break;
      case ArrayKind::Keyset:
        m_buf->append("Keyset\n");
        break;
      case ArrayKind::PHP:
      case ArrayKind::VArray:
      case ArrayKind::DArray:
      case ArrayKind::MarkedVArray:
      case ArrayKind::MarkedDArray:
        m_buf->append("Array\n");
        break;
      }
    }
    if (m_indent > 0) {
      m_indent += 4;
      indent();
    }
    m_buf->append("(\n");
    m_indent += (info.indent_delta = 4);
    break;
  case Type::VarExport:
  case Type::PHPOutput:
    if (m_indent > 0 && m_rsrcName.empty() && m_keyPrinted) {
      m_buf->append('\n');
      indent();
    }
    if (!m_objClass.empty()) {
      m_buf->append(m_objClass);
      if (m_objCode == 'O') {
        m_buf->append("::__set_state(darray[\n");
      } else {
        assertx(m_objCode == 'V' || m_objCode == 'K');
        m_buf->append(" {\n");
      }
    } else if (!m_rsrcName.empty()) {
      m_buf->append("NULL");
    } else {
      switch (kind) {
      case ArrayKind::Dict:
      case ArrayKind::BespokeTypeStructure:
        m_buf->append("dict [\n");
        break;
      case ArrayKind::Vec:
        m_buf->append("vec [\n");
        break;
      case ArrayKind::Keyset:
        m_buf->append("keyset [\n");
        break;
      case ArrayKind::PHP:
        m_buf->append("array (\n");
        break;
      case ArrayKind::VArray: {
        auto const dvarray = Cfg::Eval::HackArrDVArrVarExport ||
                             m_type == Type::PHPOutput;
        m_buf->append(dvarray ? "varray [\n" : "array (\n");
        break;
      }
      case ArrayKind::DArray: {
        auto const dvarray = Cfg::Eval::HackArrDVArrVarExport ||
                             m_type == Type::PHPOutput;
        m_buf->append(dvarray ? "darray [\n" : "array (\n");
        break;
      }
      case ArrayKind::MarkedVArray:
      case ArrayKind::MarkedDArray:
        always_assert(0);
      }
    }
    m_indent += (info.indent_delta = 2);
    break;
  case Type::VarDump:
  case Type::DebugDump:
    indent();
    if (!m_rsrcName.empty()) {
      m_buf->append("resource(");
      m_buf->append(m_rsrcId);
      m_buf->append(") of type (");
      m_buf->append(m_rsrcName);
      m_buf->append(")\n");
      break;
    } else if (!m_objClass.empty()) {
      m_buf->append("object(");
      m_buf->append(m_objClass);
      m_buf->append(") ");
    } else {
      auto const header = [&]() {
        switch (kind) {
          case ArrayKind::Dict:
          case ArrayKind::BespokeTypeStructure:
            return "dict";
          case ArrayKind::Vec:
            return "vec";
          case ArrayKind::Keyset:
            return "keyset";
          case ArrayKind::PHP:
            return "array";
          case ArrayKind::VArray:
          case ArrayKind::MarkedVArray:
            return "varray";
          case ArrayKind::DArray:
          case ArrayKind::MarkedDArray:
            return "darray";
        }
        not_reached();
      }();
      m_buf->append(header);
    }
    m_buf->append('(');
    m_buf->append(size);
    m_buf->append(')');

    // ...so to strictly follow PHP's output
    if (m_type == Type::VarDump) {
      m_buf->append(' ');
    } else {
      writeRefCount();
    }

    m_buf->append("{\n");
    m_indent += (info.indent_delta = 2);
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    if (!m_rsrcName.empty() && m_type == Type::DebuggerSerialize) {
      m_buf->append("L:");
      m_buf->append(m_rsrcId);
      m_buf->append(":");
      m_buf->append((int)m_rsrcName.size());
      m_buf->append(":\"");
      m_buf->append(m_rsrcName);
      m_buf->append("\"{");
    } else if (!m_objClass.empty()) {
      m_buf->append(m_objCode);
      m_buf->append(":");
      m_buf->append((int)m_objClass.size());
      m_buf->append(":\"");
      m_buf->append(m_objClass);
      m_buf->append("\":");
      m_buf->append(size);
      m_buf->append(":{");
    } else {
      switch (kind) {
      case ArrayKind::Dict:
        m_buf->append("D:");
        break;
      case ArrayKind::Vec:
        m_buf->append("v:");
        break;
      case ArrayKind::MarkedVArray:
        m_buf->append("x:");
        break;
      case ArrayKind::MarkedDArray:
        m_buf->append("X:");
        break;
      case ArrayKind::Keyset:
        m_buf->append("k:");
        break;
      case ArrayKind::PHP:
        m_buf->append("a:");
        break;
      case ArrayKind::VArray:
        m_buf->append("y:");
        break;
      case ArrayKind::DArray:
        m_buf->append("Y:");
        break;
      case ArrayKind::BespokeTypeStructure:
        m_buf->append(m_type == Type::Internal ? "T:" : "D:");
        break;
      }
      m_buf->append(size);
      m_buf->append(":{");
    }
    break;
  case Type::JSON:
    info.is_vector =
      (m_objClass.empty() || m_objCode == 'V' || m_objCode == 'K') &&
      isVectorData &&
      kind != ArrayKind::Dict;

    if (info.is_vector && m_type == Type::JSON) {
      info.is_vector = (m_option & k_JSON_FORCE_OBJECT)
                       ? false : info.is_vector;
    }

    if (info.is_vector || kind == ArrayKind::Keyset) {
      if (UNLIKELY(Cfg::Eval::HackArrCompatSerializeNotices) &&
          kind == ArrayKind::DArray) {
        if (size == 0 && m_edWarn && !m_hasEDWarned) {
          raise_hackarr_compat_notice("JSON encoding empty darray");
          m_hasEDWarned = true;
        } else if (size != 0 && m_vdWarn && !m_hasVDWarned) {
          raise_hackarr_compat_notice("JSON encoding vec-like darray");
          m_hasVDWarned = true;
        }
      }
      m_buf->append('[');
    } else {
      if (UNLIKELY(Cfg::Eval::HackArrCompatSerializeNotices) &&
          kind == ArrayKind::DArray && m_ddWarn && !m_hasDDWarned) {
        raise_hackarr_compat_notice("JSON encoding dict-like darray");
        m_hasDDWarned = true;
      }
      m_buf->append('{');
    }

    if (m_type == Type::JSON && (m_option & k_JSON_PRETTY_PRINT) &&
        info.size > 0) {
      m_buf->append("\n");
      m_indent += (info.indent_delta = 4);
    }

    break;
  default:
    assertx(false);
    break;
  }

  // ...so we don't mess up next array output
  if (!m_objClass.empty() || !m_rsrcName.empty()) {
    m_objClass.clear();
    info.is_object = true;
  } else {
    info.is_object = false;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writePropertyKey(const OptString& prop) {
  const char *key = prop.data();
  int kl = prop.size();
  if (!*key && kl) {
    const char *cls = key + 1;
    if (*cls == '*') {
      assertx(key[2] == 0);
      m_buf->append(key + 3, kl - 3);
      const char prot[] = "\":protected";
      int o = m_type == Type::PrintR ? 1 : 0;
      m_buf->append(prot + o, sizeof(prot) - 1 - o);
    } else {
      int l = strlen(cls);
      m_buf->append(cls + l + 1, kl - l - 2);
      int o = m_type == Type::PrintR ? 1 : 0;
      m_buf->append(&"\":\""[o], 3 - 2*o);
      m_buf->append(cls, l);
      const char priv[] = "\":private";
      m_buf->append(priv + o, sizeof(priv) - 1 - o);
    }
  } else {
    m_buf->append(prop);
    if (m_type != Type::PrintR && m_type != Type::DebuggerDump) {
      m_buf->append('"');
    }
  }
}

/* key MUST be a non-reference string or int */
template <class Buffer>
void VariableSerializerImpl<Buffer>::writeArrayKey(
  const Variant& key,
  VariableSerializer::ArrayKind kind
) {
  using AK = VariableSerializer::ArrayKind;
  auto const keyCell = tvAssertPlausible(key.asTypedValue());
  bool const skey = isStringType(keyCell->m_type);

  ArrayInfo &info = m_arrayInfos.back();

  switch (m_type) {
  case Type::DebuggerDump:
  case Type::PrintR: {
    indent();
    if (kind == AK::Keyset) return;
    m_buf->append('[');
    if (info.is_object && skey) {
      writePropertyKey(OptString{keyCell->m_data.pstr});
    } else {
      m_buf->append(key);
    }
    m_buf->append("] => ");
    break;
  }

  case Type::VarExport:
  case Type::PHPOutput:
    indent();
    if (kind == AK::Vec || kind == AK::Keyset) return;
    if ((kind == AK::VArray || kind == AK::MarkedVArray) &&
        (Cfg::Eval::HackArrDVArrVarExport || m_type == Type::PHPOutput)) {
      return;
    }
    write(key, true);
    m_buf->append(" => ");
    break;

  case Type::VarDump:
  case Type::DebugDump:
    if (kind == AK::Vec || kind == AK::Keyset ||
        kind == AK::VArray || kind == AK::MarkedVArray) {
      return;
    }
    indent();
    m_buf->append('[');
    if (!skey) {
      m_buf->append(keyCell->m_data.num);
    } else {
      m_buf->append('"');
      if (info.is_object) {
        writePropertyKey(OptString{keyCell->m_data.pstr});
      } else {
        m_buf->append(keyCell->m_data.pstr);
        m_buf->append('"');
      }
    }
    m_buf->append("]=>\n");
    break;

  case Type::APCSerialize:
    if (kind == AK::Vec || kind == AK::Keyset || kind == AK::VArray) return;
    if (skey) {
      write(StrNR(keyCell->m_data.pstr).asString());
      return;
    }
    [[fallthrough]];

  case Type::Serialize:
  case Type::Internal:
  case Type::DebuggerSerialize:
    if (kind == AK::Vec || kind == AK::MarkedVArray ||
        kind == AK::Keyset || kind == AK::VArray) return;
    write(key);
    break;

  case Type::JSON:
    if (!info.is_vector && kind != ArrayKind::Keyset) {
      if (!info.first_element) {
        m_buf->append(',');
      }
      if (UNLIKELY(m_option & k_JSON_PRETTY_PRINT)) {
        if (!info.first_element) {
          m_buf->append("\n");
        }
        indent();
      }
      if (skey) {
        auto const sdata = keyCell->m_data.pstr;
        const char *k = sdata->data();
        int len = sdata->size();
        if (info.is_object && !*k && len) {
          while (*++k) len--;
          k++;
          len -= 2;
        }
        write(k, len, true);
      } else {
        m_buf->append('"');
        m_buf->append(keyCell->m_data.num);
        m_buf->append('"');
      }
      m_buf->append(':');
      if (UNLIKELY(m_option & k_JSON_PRETTY_PRINT)) {
        m_buf->append(' ');
      }
    }
    break;

  default:
    assertx(false);
    break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeCollectionKey(
  const Variant& key,
  VariableSerializer::ArrayKind kind
) {
  if (m_type == Type::Serialize ||
      m_type == Type::Internal ||
      m_type == Type::APCSerialize ||
      m_type == Type::DebuggerSerialize) {
    m_valueCount++;
  }
  writeArrayKey(key, kind);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeArrayValue(
  const Variant& value,
  VariableSerializer::ArrayKind kind
) {
  switch (m_type) {
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    // Do not count values in keysets because they're also keys, and it's not
    // possible to have back references to keys.
    if (kind != VariableSerializer::ArrayKind::Keyset) {
      m_valueCount++;
    }
    write(value);
    break;

  case Type::DebuggerDump:
  case Type::PrintR:
    write(value);
    m_buf->append('\n');
    break;

  case Type::VarExport:
  case Type::PHPOutput: {
    auto const oldKeyPrinted = m_keyPrinted;
    m_keyPrinted = [&]{
      if (kind == ArrayKind::Vec || kind == ArrayKind::Keyset) return false;
      if ((kind == ArrayKind::VArray || kind == ArrayKind::MarkedVArray) &&
          (Cfg::Eval::HackArrDVArrVarExport || m_type == Type::PHPOutput)) {
        return false;
      }
      return true;
    }();
    SCOPE_EXIT { m_keyPrinted = oldKeyPrinted; };
    write(value);
    m_buf->append(",\n");
    break;
  }

  case Type::JSON: {
    ArrayInfo &info = m_arrayInfos.back();
    if (info.is_vector || kind == ArrayKind::Keyset) {
      if (!info.first_element) {
        m_buf->append(',');
      }
      if (UNLIKELY(m_option & k_JSON_PRETTY_PRINT)) {
        if (!info.first_element) {
          m_buf->append("\n");
        }
        indent();
      }
    }
    write(value);
    break;
  }

  default:
    write(value);
    break;
  }

  ArrayInfo &last_info = m_arrayInfos.back();
  last_info.first_element = false;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeArrayFooter(
  VariableSerializer::ArrayKind kind
) {
  ArrayInfo &info = m_arrayInfos.back();

  m_indent -= info.indent_delta;
  switch (m_type) {
  case Type::DebuggerDump:
  case Type::PrintR:
    if (m_rsrcName.empty()) {
      indent();
      m_buf->append(")\n");
      if (m_indent > 0) {
        m_indent -= 4;
      }
    }
    break;
  case Type::VarExport:
  case Type::PHPOutput:
    if (m_rsrcName.empty()) {
      indent();
    }
    if (info.is_object && m_objCode) {
      if (m_objCode == 'O') {
        m_buf->append("])");
      } else {
        assertx(m_objCode == 'V' || m_objCode == 'K');
        m_buf->append("}");
      }
    } else if (m_rsrcName.empty()) { // for rsrc, only write NULL in arrayHeader
      switch (kind) {
      case ArrayKind::Dict:
      case ArrayKind::Vec:
      case ArrayKind::Keyset:
      case ArrayKind::BespokeTypeStructure:
        m_buf->append(']');
        break;
      case ArrayKind::PHP:
        m_buf->append(')');
        break;
      case ArrayKind::VArray:
      case ArrayKind::DArray: {
        auto const dvarrays = Cfg::Eval::HackArrDVArrVarExport ||
                              m_type == Type::PHPOutput;
        m_buf->append(dvarrays ? ']' : ')');
        break;
      }
      case ArrayKind::MarkedVArray:
      case ArrayKind::MarkedDArray:
        always_assert(0);
      }
    }
    break;
  case Type::VarDump:
  case Type::DebugDump:
    if (m_rsrcName.empty()) {
      indent();
      m_buf->append("}\n");
    }
    break;
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
  case Type::DebuggerSerialize:
    m_buf->append('}');
    break;
  case Type::JSON:
    if (m_type == Type::JSON && (m_option & k_JSON_PRETTY_PRINT) &&
        info.size > 0) {
      m_buf->append("\n");
      indent();
    }
    if (info.is_vector || kind == ArrayKind::Keyset) {
      m_buf->append(']');
    } else {
      m_buf->append('}');
    }
    break;
  default:
    assertx(false);
    break;
  }

  m_arrayInfos.pop_back();
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeSerializableObject(const OptString& clsname,
                                                             const OptString& serialized) {
  m_buf->append("C:");
  m_buf->append(clsname.size());
  m_buf->append(":\"");
  m_buf->append(clsname.data(), clsname.size());
  m_buf->append("\":");
  m_buf->append(serialized.size());
  m_buf->append(":{");
  m_buf->append(serialized.data(), serialized.size());
  m_buf->append('}');
}

///////////////////////////////////////////////////////////////////////////////

template <class Buffer>
void VariableSerializerImpl<Buffer>::indent() {
  for (int i = 0; i < m_indent; i++) {
    m_buf->append(' ');
  }
}

template <class Buffer>
bool VariableSerializerImpl<Buffer>::incNestedLevel(tv_rval tv) {
  ++m_currentDepth;

  switch (m_type) {
  case Type::VarExport:
  case Type::PHPOutput:
  case Type::PrintR:
  case Type::VarDump:
  case Type::DebugDump:
  case Type::DebuggerDump:
    return ++m_refs[tv].m_count >= m_maxCount;
  case Type::JSON:
    if (m_currentDepth > m_maxDepth) {
      json_set_last_error_code(json_error_codes::JSON_ERROR_DEPTH);
    }
    return ++m_refs[tv].m_count >= m_maxCount;
  case Type::DebuggerSerialize:
    if (m_maxLevelDebugger > 0 && ++m_levelDebugger > m_maxLevelDebugger) {
      return true;
    }
    [[fallthrough]];
  case Type::Serialize:
  case Type::Internal:
  case Type::APCSerialize:
    {
      auto& ref = m_refs[tv];
      int ct = ++ref.m_count;
      bool isObject = tvIsResource(tv) || tvIsObject(tv);
      if (ref.m_id != NO_ID && isObject) {
        return true;
      }
      ref.m_id = m_valueCount;
      return ct >= (m_maxCount - 1);
    }
    break;
  default:
    assertx(false);
    break;
  }
  return false;
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::decNestedLevel(tv_rval tv) {
  --m_currentDepth;
  --m_refs[tv].m_count;
  if (m_type == Type::DebuggerSerialize && m_maxLevelDebugger > 0) {
    --m_levelDebugger;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeRFunc(const RFuncData* rfunc) {
  switch (getType()) {
    case Type::PrintR:
    case Type::DebuggerDump:
      m_buf->append("reifiedFunction{\n");
      m_indent += 4;
      indent();
      m_buf->append("function(");
      m_buf->append(rfunc->m_func->fullName()->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("[\"reified_generics\"] => ");
      serializeArray(rfunc->m_arr);
      m_indent -= 4;
      indent();
      m_buf->append("}\n");
      break;

    case Type::VarDump:
    case Type::DebugDump:
      indent();
      m_buf->append("reifiedFunction{\n");
      m_indent += 2;
      indent();
      m_buf->append("function(");
      m_buf->append(rfunc->m_func->fullName()->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("[\"reified_generics\"]=>\n");
      serializeArray(rfunc->m_arr);
      m_indent -= 2;
      indent();
      m_buf->append("}\n");
      break;
    case Type::VarExport:
    case Type::Serialize:
    case Type::Internal:
    case Type::JSON:
    case Type::APCSerialize:
    case Type::DebuggerSerialize:
    case Type::PHPOutput:
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize reified function pointer"
      );
      break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeFunc(const Func* func) {
  auto const name = func->fullName();
  switch (getType()) {
    case Type::VarExport:
    case Type::PHPOutput:
      m_buf->append("fun(");
      write(name->data(), name->size());
      m_buf->append(')');
      break;
    case Type::VarDump:
    case Type::DebugDump:
      // TODO (T29639296)
      // For now we use function(foo) to dump function pointers in most cases,
      // and this can be changed in the future.
      indent();
      m_buf->append("function(");
      m_buf->append(name->data());
      m_buf->append(")\n");
      break;
    case Type::PrintR:
    case Type::DebuggerDump:
      m_buf->append("function(");
      m_buf->append(name->data());
      m_buf->append(')');
      break;
    case Type::JSON:
      if (func->isMethCaller()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerSerde.get()}
        );
      }
      write(func->nameStr());
      break;
    case Type::APCSerialize:
      if (func->isMethCaller()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerAPC.get()}
        );
      }
      [[fallthrough]];
    case Type::Serialize:
      if (func->isMethCaller()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerSerde.get()}
        );
      }
      invalidFuncConversion("string");
      break;
    case Type::Internal:
    case Type::DebuggerSerialize:
      m_buf->append("f:");
      m_buf->append(name->size());
      m_buf->append(":\"");
      m_buf->append(name->data(), name->size());
      m_buf->append("\";");
      break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeClass(const Class* cls) {
  switch (getType()) {
    case Type::VarExport:
    case Type::PHPOutput:
      if (Cfg::Eval::ClassAsStringVarExport) {
        write(StrNR(cls->name()));
      } else {
        m_buf->append(cls->name());
        m_buf->append("::class");
      }
      break;
    case Type::VarDump:
      if (Cfg::Eval::ClassAsStringVarDump) {
        write(StrNR(cls->name()));
        break;
      }
      [[fallthrough]];
    case Type::DebugDump:
      indent();
      m_buf->append("class(");
      m_buf->append(cls->name());
      m_buf->append(")\n");
      break;
    case Type::PrintR:
      if (Cfg::Eval::ClassAsStringPrintR) {
        write(StrNR(cls->name()));
        break;
      }
      [[fallthrough]];
    case Type::DebuggerDump:
      m_buf->append("class(");
      m_buf->append(cls->name());
      m_buf->append(')');
      break;
    case Type::JSON:
      write(StrNR(classToStringHelper(cls, "serialization")));
      break;
    case Type::Serialize:
      if (!m_keepClasses) {
        write(StrNR(classToStringHelper(cls, "serialization")));
        break;
      }
      [[fallthrough]];
    case Type::Internal:
    case Type::APCSerialize: {
      auto cname = cls->name();
      m_buf->append("l:");
      m_buf->append(cname->size());
      m_buf->append(":\"");
      m_buf->append(cname->data(), cname->size());
      m_buf->append("\";");
      break;
    }
    case Type::DebuggerSerialize:
      m_buf->append("c:");
      m_buf->append(cls->name()->size());
      m_buf->append(":\"");
      m_buf->append(cls->name()->data(), cls->name()->size());
      m_buf->append("\";");
      break;
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeLazyClass(LazyClassData lcls) {
  switch (getType()) {
    case Type::VarExport:
    case Type::PHPOutput:
      if (Cfg::Eval::ClassAsStringVarExport) {
        write(StrNR(lcls.name()));
      } else {
        m_buf->append(lcls.name());
        m_buf->append("::class");
      }
      break;
    case Type::VarDump:
      if (Cfg::Eval::ClassAsStringVarDump) {
        write(StrNR(lcls.name()));
        break;
      }
      [[fallthrough]];
    case Type::DebugDump:
      indent();
      m_buf->append("class(");
      m_buf->append(lcls.name());
      m_buf->append(")\n");
      break;
    case Type::PrintR:
      if (Cfg::Eval::ClassAsStringPrintR) {
        write(StrNR(lcls.name()));
        break;
      }
      [[fallthrough]];
    case Type::DebuggerDump:
      m_buf->append("class(");
      m_buf->append(lcls.name());
      m_buf->append(')');
      break;
    case Type::JSON:
    case Type::DebuggerSerialize:
      write(StrNR(lazyClassToStringHelper(lcls, "serialization")));
      break;
    case Type::Serialize:
      if (!m_keepClasses) {
        write(StrNR(lazyClassToStringHelper(lcls, "serialization")));
        break;
      }
      [[fallthrough]];
    case Type::Internal:
    case Type::APCSerialize: {
      auto cname = lcls.name();
      m_buf->append("l:");
      m_buf->append(cname->size());
      m_buf->append(":\"");
      m_buf->append(cname->data(), cname->size());
      m_buf->append("\";");
      break;
    }
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeEnumClassLabel(const StringData* label) {
  switch (getType()) {
    case Type::VarExport:
    case Type::PHPOutput:
    case Type::DebuggerDump:
    case Type::PrintR:
        m_buf->append("enum class label (#");
        m_buf->append(label->data());
        m_buf->append(")");
      break;
    case Type::VarDump:
    case Type::DebugDump:
        indent();
        m_buf->append("enum class label (#");
        m_buf->append(label->data());
        m_buf->append(")\n");
      break;
    case Type::JSON:
      write(StrNR(label));
      break;
    case Type::Serialize:
    case Type::DebuggerSerialize:
    case Type::Internal:
    case Type::APCSerialize:
      m_buf->append("e:");
      m_buf->append(label->size());
      m_buf->append(":\"");
      m_buf->append(label->data(), label->size());
      m_buf->append("\";");
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeClsMeth(
  ClsMethDataRef clsMeth, bool skipNestCheck /* = false */) {
  auto const clsName = clsMeth->getCls()->name();
  auto const funcName = clsMeth->getFunc()->name();
  switch (getType()) {
    case Type::DebuggerDump:
    case Type::PrintR:
      m_buf->append("classMeth{\n");
      m_indent += 4;
      indent();
      m_buf->append("class(");
      m_buf->append(clsName->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("function(");
      m_buf->append(funcName->data());
      m_buf->append(")\n");
      m_indent -= 4;
      indent();
      m_buf->append("}");
      break;

    case Type::VarExport:
    case Type::PHPOutput:
      m_buf->append("class_meth(");
      write(clsName->data(), clsName->size());
      m_buf->append(", ");
      write(funcName->data(), funcName->size());
      m_buf->append(')');
      break;

    case Type::VarDump:
    case Type::DebugDump:
      indent();
      m_buf->append("classMeth{\n");
      m_indent += 2;
      indent();
      m_buf->append("class(");
      m_buf->append(clsName->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("function(");
      m_buf->append(funcName->data());
      m_buf->append(")\n");
      m_indent -= 2;
      indent();
      m_buf->append("}\n");
      break;

    case Type::JSON: {
      auto const kind = getKind(empty_vec_array().get());
      writeArrayHeader(2 /* size */, true /* isVectorData */, kind);
      writeArrayKey(VarNR(0), kind);
      writeArrayValue(VarNR(clsName), kind);
      writeArrayKey(VarNR(1), kind);
      writeArrayValue(VarNR(funcName), kind);
      writeArrayFooter(kind);
      break;
    }

    case Type::Serialize:
    case Type::APCSerialize: {
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize class meth pointer"
      );
    }
    case Type::Internal:
    case Type::DebuggerSerialize: {
      m_buf->append("m:");
      m_buf->append(clsName->size());
      m_buf->append(":\"");
      m_buf->append(clsName->data(), clsName->size());
      m_buf->append("\":");
      m_buf->append(funcName->size());
      m_buf->append(":\"");
      m_buf->append(funcName->data(), funcName->size());
      m_buf->append("\";");
      break;
    }
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeRClsMeth(RClsMethData* rclsMeth) {
  switch (getType()) {
    case Type::PrintR:
    case Type::DebuggerDump:
      m_buf->append("reifiedClassMeth{\n");
      m_indent += 4;
      indent();
      m_buf->append("class(");
      m_buf->append(rclsMeth->m_cls->name()->data(), rclsMeth->m_cls->name()->size());
      m_buf->append(")\n");
      indent();
      m_buf->append("function(");
      m_buf->append(rclsMeth->m_func->name()->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("[\"reified_generics\"] => ");
      serializeArray(rclsMeth->m_arr);
      m_indent -= 4;
      indent();
      m_buf->append("}\n");
      break;
    case Type::VarDump:
    case Type::DebugDump:
      indent();
      m_buf->append("reifiedClassMeth{\n");
      m_indent += 2;
      indent();
      m_buf->append("class(");
      m_buf->append(rclsMeth->m_cls->name()->data(), rclsMeth->m_cls->name()->size());
      m_buf->append(")\n");
      indent();
      m_buf->append("function(");
      m_buf->append(rclsMeth->m_func->name()->data());
      m_buf->append(")\n");
      indent();
      m_buf->append("[\"reified_generics\"]=>\n");
      serializeArray(rclsMeth->m_arr);
      m_indent -= 2;
      indent();
      m_buf->append("}\n");
      break;
    case Type::VarExport:
    case Type::Serialize:
    case Type::Internal:
    case Type::JSON:
    case Type::APCSerialize:
    case Type::DebuggerSerialize:
    case Type::PHPOutput:
      SystemLib::throwInvalidOperationExceptionObject(
        "Unable to serialize reified class meth pointer"
      );
      break;
  }
}

template <class Buffer>
NEVER_INLINE
void VariableSerializerImpl<Buffer>::serializeVariant(tv_rval tv,
                                                      bool isArrayKey /* = false */,
                                                      bool skipNestCheck /* = false */,
                                                      bool noQuotes /* = false */) {
  switch (type(tv)) {
    case KindOfUninit:
    case KindOfNull:
      assertx(!isArrayKey);
      writeNull();
      return;

    case KindOfBoolean:
      assertx(!isArrayKey);
      write(val(tv).num != 0);
      return;

    case KindOfInt64:
      write(val(tv).num);
      return;

    case KindOfDouble:
      write(val(tv).dbl);
      return;

    case KindOfString:
      write(val(tv).pstr->data(),
            val(tv).pstr->size(), isArrayKey, noQuotes);
      return;

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
      assertx(!isArrayKey);
      serializeArray(val(tv).parr, skipNestCheck);
      return;

    case KindOfObject:
      assertx(!isArrayKey);
      serializeObject(val(tv).pobj);
      return;

    case KindOfResource:
      assertx(!isArrayKey);
      serializeResource(val(tv).pres->data());
      return;

    case KindOfRFunc:
      assertx(!isArrayKey);
      serializeRFunc(val(tv).prfunc);
      return;

    case KindOfFunc:
      assertx(!isArrayKey);
      serializeFunc(val(tv).pfunc);
      return;

    case KindOfClass:
      assertx(!isArrayKey);
      serializeClass(val(tv).pclass);
      return;

    case KindOfClsMeth:
      assertx(!isArrayKey);
      serializeClsMeth(val(tv).pclsmeth, skipNestCheck);
      return;

    case KindOfRClsMeth:
      assertx(!isArrayKey);
      serializeRClsMeth(val(tv).prclsmeth);
      return;

    case KindOfLazyClass:
      assertx(!isArrayKey);
      serializeLazyClass(val(tv).plazyclass);
      return;

    case KindOfEnumClassLabel:
      assertx(!isArrayKey);
      serializeEnumClassLabel(val(tv).pstr);
      return;
  }
  not_reached();
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeResourceImpl(const ResourceData* res) {
  pushResourceInfo(res->o_getResourceName(), res->getId());
  serializeArray(staticEmptyDictArray());
  popResourceInfo();
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeResource(const ResourceData* res) {
  TypedValue tv = make_tv<KindOfResource>(const_cast<ResourceHdr*>(res->hdr()));
  if (UNLIKELY(incNestedLevel(&tv))) {
    writeOverflow(&tv);
  } else if (auto trace = dynamic_cast<const CompactTrace*>(res)) {
    auto const trace_array = Variant(trace->extract());
    auto const raw = *trace_array.asTypedValue();
    // We use a depth of 2 because backtrace arrays are varrays-of-darrays.
    auto const marked = Variant::attach(arrprov::markTvToDepth(raw, true, 2));
    serializeArray(marked.toArray().get());
  } else {
    serializeResourceImpl(res);
  }
  decNestedLevel(&tv);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeString(const OptString& str) {
  if (str) {
    write(str.data(), str.size());
  } else {
    writeNull();
  }
}

void sortTvPairs(req::vector<std::pair<TypedValue, TypedValue>>& elems) {
  using KVPair = std::pair<TypedValue, TypedValue>;
  sort(elems.begin(), elems.end(), [](const KVPair& a, const KVPair& b) {
    if (tvIsString(a.first)) {
      // Integers come before strings
      if (!tvIsString(b.first)) {
        assertx(tvIsInt(b.first));
        return false;
      }
      return strcmp(val(a.first).pstr->data(), val(b.first).pstr->data()) < 0;
    } else {
      assertx(tvIsInt(a.first));
      if (!tvIsInt(b.first)) {
        assertx(tvIsString(b.first));
        return true;
      }
      return val(a.first).num < val(b.first).num;
    }
  });
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeArrayImpl(const ArrayData* arr,
                                                        bool isVectorData) {
  using AK = VariableSerializer::ArrayKind;
  AK kind = getKind(arr);
  writeArrayHeader(arr->size(), isVectorData, kind);

  if (UNLIKELY(m_sortArrayKeys)) {
    using KVPair = std::pair<TypedValue, TypedValue>;
    req::vector<KVPair> elems;
    IterateKV(
      arr,
      [&elems](TypedValue k, TypedValue v) {
        elems.push_back(std::make_pair(k, v));
      }
    );
    sortTvPairs(elems);
    for (const auto& [k, v]: elems) {
      writeArrayKey(VarNR(k), kind);
      writeArrayValue(VarNR(v), kind);
    }
  } else {
    IterateKV(
      arr,
      [&](TypedValue k, TypedValue v) {
        writeArrayKey(VarNR(k), kind);
        writeArrayValue(VarNR(v), kind);
      }
    );
  }

  writeArrayFooter(kind);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeArray(const ArrayData* arr,
                                                    bool skipNestCheck /* = false */) {
  if (UNLIKELY(Cfg::Eval::HackArrCompatSerializeNotices)) {
    if (UNLIKELY(m_hackWarn && !m_hasHackWarned)) {
      raise_hack_arr_compat_serialize_notice(arr);
      m_hasHackWarned = true;
    }
    if (UNLIKELY(m_dictWarn && !m_hasDictWarned && arr->isDictType())) {
      raise_hack_arr_compat_serialize_notice(arr);
      m_hasDictWarned = true;
    }
    if (UNLIKELY(m_keysetWarn && !m_hasKeysetWarned && arr->isKeysetType())) {
      raise_hack_arr_compat_serialize_notice(arr);
      m_hasKeysetWarned = true;
    }
  }

  const bool isVectorData = arr->isVectorData();
  if (arr->empty()) {
    auto const kind = getKind(arr);
    writeArrayHeader(0, isVectorData, kind);
    writeArrayFooter(kind);
    return;
  }

  if (!skipNestCheck) {
    TypedValue tv = make_array_like_tv(const_cast<ArrayData*>(arr));
    if (incNestedLevel(&tv)) {
      writeOverflow(&tv);
    } else {
      serializeArrayImpl(arr, isVectorData);
    }
    decNestedLevel(&tv);
  } else {
    // If skipNestCheck, the array is temporary and we should not check or
    // save its pointer. We'll serialize it without its header.
    serializeArrayImpl(arr, isVectorData);
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeObjProps(Array& arr) {
  if (arr.isNull()) {
    writeNull();
    return;
  }

  auto const ad = arr.detach();
  auto const dict = ad->toDict(ad->cowCheck());
  if (dict != ad) decRefArr(ad);
  serializeArray(dict, /*skipNestCheck=*/true);
  decRefArr(dict);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::writeCollectionKVSorted(ObjectData* obj) {
  using AK = VariableSerializer::ArrayKind;
  using KVPair = std::pair<TypedValue, TypedValue>;
  req::vector<KVPair> elems;
  for (ArrayIter iter(obj); iter; ++iter) {
    elems.push_back(std::make_pair(iter.nvFirst(), iter.secondVal()));
  }
  sortTvPairs(elems);
  for (const auto& [k, v]: elems) {
    writeCollectionKey(VarNR(k), AK::PHP);
    writeArrayValue(VarNR(v), AK::PHP);
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeCollection(ObjectData* obj) {
  using AK = VariableSerializer::ArrayKind;
  int64_t sz = collections::getSize(obj);
  auto type = obj->collectionType();

  if (m_disallowCollections) {
    SystemLib::throwInvalidOperationExceptionObject(
      VarNR{s_disallowedCollectionSerde.get()}
    );
  }

  if (isMapCollection(type)) {
    pushObjectInfo(obj->getClassName(),'K');
    writeArrayHeader(sz, false, AK::PHP);
    if (UNLIKELY(m_sortArrayKeys)) {
      writeCollectionKVSorted(obj);
    } else {
      for (ArrayIter iter(obj); iter; ++iter) {
        writeCollectionKey(iter.first(), AK::PHP);
        writeArrayValue(iter.second(), AK::PHP);
      }
    }
    writeArrayFooter(AK::PHP);

  } else {
    assertx(isVectorCollection(type) ||
            isSetCollection(type) ||
            (type == CollectionType::Pair));
    pushObjectInfo(obj->getClassName(), 'V');
    writeArrayHeader(sz, true, AK::PHP);
    auto ser_type = getType();
    if (ser_type == VariableSerializer::Type::Serialize ||
        ser_type == VariableSerializer::Type::Internal ||
        ser_type == VariableSerializer::Type::APCSerialize ||
        ser_type == VariableSerializer::Type::DebuggerSerialize ||
        ser_type == VariableSerializer::Type::VarExport ||
        ser_type == VariableSerializer::Type::PHPOutput) {
      // For the 'V' serialization format, we don't print out keys
      // for Serialize, APCSerialize, DebuggerSerialize
      bool const should_indent =
        ser_type == VariableSerializer::Type::VarExport ||
        ser_type == VariableSerializer::Type::PHPOutput;
      for (ArrayIter iter(obj); iter; ++iter) {
        if (should_indent) {
          indent();
        }
        writeArrayValue(iter.second(), AK::PHP);
      }
    } else {
      if (isSetCollection(type)) {
        bool const should_indent =
          ser_type == VariableSerializer::Type::PrintR ||
          ser_type == VariableSerializer::Type::DebuggerDump;
        for (ArrayIter iter(obj); iter; ++iter) {
          if (should_indent) {
            indent();
          }
          writeArrayValue(iter.second(), AK::PHP);
        }
      } else {
        if (UNLIKELY(m_sortArrayKeys)) {
          writeCollectionKVSorted(obj);
        } else {
          for (ArrayIter iter(obj); iter; ++iter) {
              writeCollectionKey(iter.first(), AK::PHP);
              writeArrayValue(iter.second(), AK::PHP);
          }
        }
      }
    }
    writeArrayFooter(AK::PHP);
  }
  popObjectInfo();
}

/* Get properties from the actual object unless we're
 * serializing for var_dump()/print_r() and the object
 * exports a __debugInfo() magic method.
 * In which case, call that and use the array it returns.
 */
template <class Buffer>
Array VariableSerializerImpl<Buffer>::getSerializeProps(const ObjectData* obj) const {
  if (getType() == VariableSerializer::Type::VarExport) {
    Array props = Array::CreateDict();
    for (ArrayIter iter(obj->toArray(false, true)); iter; ++iter) {
      auto key = iter.first().toString();
      // Jump over any class attribute mangling
      if (key[0] == '\0' && key.size() > 0) {
        int sizeToCut = 0;
        do {
          sizeToCut++;
        } while (key[sizeToCut] != '\0');
        key = key.substr(sizeToCut+1);
      }
      props.set(key, iter.secondVal());
    }
    return props;
  }
  if ((getType() != VariableSerializer::Type::PrintR) &&
      (getType() != VariableSerializer::Type::VarDump)) {
    auto const ignoreLateInit =
      (m_ignoreLateInit ||
       getType() == VariableSerializer::Type::DebugDump ||
       getType() == VariableSerializer::Type::DebuggerDump ||
       getType() == VariableSerializer::Type::DebuggerSerialize);
    return obj->toArray(false, ignoreLateInit);
  }
  auto cls = obj->getVMClass();
  auto debuginfo = cls->lookupMethod(s_debugInfo.get());
  if (!debuginfo) {
    // When ArrayIterator is cast to an array, it returns its array object,
    // however when it's being var_dump'd or print_r'd, it shows its properties
    if (UNLIKELY(obj->instanceof(SystemLib::getArrayIteratorClass()))) {
      auto ret = Array::CreateDict();
      obj->o_getArray(ret, false, true);
      return ret;
    }

    // Same with Closure, since it's a dynamic object but still has its own
    // different behavior for var_dump and cast to array
    if (UNLIKELY(obj->instanceof(c_Closure::classof()))) {
      auto ret = Array::CreateDict();
      obj->o_getArray(ret, false, true);
      return ret;
    }

    return obj->toArray(false, true);
  }
  if (debuginfo->attrs() & (AttrPrivate|AttrProtected|
                            AttrAbstract|AttrStatic)) {
    raise_warning("%s::__debugInfo() must be public and non-static",
                  cls->name()->data());
    return obj->toArray(false, true);
  }
  auto const providedCoeffects =
    m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
  auto ret = const_cast<ObjectData*>(obj)->invokeDebugInfo(providedCoeffects);
  if (ret.isArray()) {
    return ret.toArray();
  }
  if (ret.isNull()) {
    return empty_dict_array();
  }
  raise_error("__debugInfo() must return an array");
  not_reached();
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeObjectImpl(const ObjectData* obj) {
  bool handleSleep = false;
  Variant serializableNativeData = init_null();
  Variant ret;
  auto const type = getType();

  if (obj->isCollection()) {
    serializeCollection(const_cast<ObjectData*>(obj));
    return;
  }

  if (Cfg::Eval::ForbidMethCallerHelperSerialize &&
      (type == Type::Serialize || type == Type::Internal ||
       type == Type::DebuggerSerialize || type == Type::JSON) &&
      obj->getVMClass() == SystemLib::getMethCallerHelperClass()) {
    if (Cfg::Eval::ForbidMethCallerHelperSerialize == 1) {
      raise_warning("Serializing MethCallerHelper");
    } else {
      SystemLib::throwInvalidOperationExceptionObject(
        VarNR{s_invalidMethCallerSerde.get()}
      );
    }
  }

  if (m_disallowObjects) {
    SystemLib::throwInvalidOperationExceptionObject(
      VarNR{s_disallowedObjectSerde.get()}
    );
  }

  if (LIKELY(type == VariableSerializer::Type::Serialize ||
             type == VariableSerializer::Type::Internal ||
             type == VariableSerializer::Type::APCSerialize)) {
    if (obj->instanceof(SystemLib::getSerializableClass())) {
      assertx(!obj->isCollection());
      ret =
        const_cast<ObjectData*>(obj)->o_invoke_few_args(s_serialize, RuntimeCoeffects::fixme(), 0);
      if (ret.isString()) {
        writeSerializableObject(obj->getClassName(), ret.toString());
      } else if (ret.isNull()) {
        writeNull();
      } else {
        raise_error("%s::serialize() must return a string or NULL",
                    obj->getClassName().data());
      }
      return;
    }
    // Only serialize CPP extension type instances which can actually
    // be deserialized.  Otherwise, raise a warning and serialize
    // null.
    // Similarly, do not try to serialize WaitHandles
    // as they contain internal state via non-NativeData means.
    auto cls = obj->getVMClass();
    if ((cls->instanceCtor() && !cls->isCppSerializable()) ||
        obj->isWaitHandle()) {
      raise_warning("Attempted to serialize unserializable builtin class %s",
                    obj->getVMClass()->preClass()->name()->data());
      serializeVariant(init_null().asTypedValue());
      return;
    }
    if (type == VariableSerializer::Type::APCSerialize) {
      if (cls == SystemLib::getMethCallerHelperClass() ||
          cls == SystemLib::getDynMethCallerHelperClass()) {
        SystemLib::throwInvalidOperationExceptionObject(
          VarNR{s_invalidMethCallerAPC.get()}
        );
      }
    }
    if (obj->getVMClass()->rtAttribute(Class::HasSleep)) {
      handleSleep = true;
      auto const providedCoeffects =
        m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults();
      ret = const_cast<ObjectData*>(obj)->invokeSleep(providedCoeffects);
    }
    if (obj->hasNativeData()) {
      auto* ndi = cls->getNativeDataInfo();
      if (ndi->isSerializable()) {
        serializableNativeData = Native::nativeDataSleep(obj);
      }
    }
  } else if (UNLIKELY(type == VariableSerializer::Type::DebuggerSerialize)) {
    // Don't try to serialize a CPP extension class which doesn't
    // support serialization. Just send the class name instead.
    if (obj->isCppBuiltin() && !obj->getVMClass()->isCppSerializable()) {
      write(obj->getClassName());
      return;
    }
    if (obj->hasNativeData() &&
        obj->getVMClass()->getNativeDataInfo()->isSerializable()) {
      serializableNativeData = Native::nativeDataSleep(obj);
    }
  }

  if (UNLIKELY(handleSleep)) {
    assertx(!obj->isCollection());
    if (ret.isArray()) {
      Array wanted = Array::CreateDict();
      assertx(isArrayLikeType(ret.getType()));
      const Array &props = ret.asCArrRef();
      for (ArrayIter iter(props); iter; ++iter) {
        OptString memberName = iter.second().toString();
        OptString propName = memberName;
        auto obj_cls = obj->getVMClass();
        Class* ctx = obj_cls;
        auto attrMask = AttrNone;
        if (memberName.data()[0] == 0) {
          int subLen = memberName.find('\0', 1) + 1;
          if (subLen > 2) {
            if (subLen == 3 && memberName.data()[1] == '*') {
              attrMask = AttrProtected;
              memberName = memberName.substr(subLen);
            } else {
              attrMask = AttrPrivate;
              OptString cls = memberName.substr(1, subLen - 2);
              ctx = Class::lookup(cls.get());
              if (ctx) {
                memberName = memberName.substr(subLen);
              } else {
                ctx = obj_cls;
              }
            }
          }
        }

        // TODO(T126821336): Populate with module name
        auto const propCtx = MemberLookupContext(ctx, (StringData*)nullptr);
        auto const lookup = obj_cls->getDeclPropSlot(propCtx, memberName.get());
        auto const slot = lookup.slot;

        if (slot != kInvalidSlot && lookup.accessible) {
          auto propVal = const_cast<ObjectData*>(obj)->propLvalAtOffset(slot);
          auto const& prop = obj_cls->declProperties()[slot];

          if (propVal.type() != KindOfUninit) {
            if (prop.attrs & AttrPrivate) {
              memberName = concat4(s_zero, ctx->nameStr(),
                                   s_zero, memberName);
            } else if (prop.attrs & AttrProtected) {
              memberName = concat(s_protected_prefix, memberName);
            }
            if (!attrMask || (attrMask & prop.attrs) == attrMask) {
              wanted.set(memberName, propVal.tv());
              continue;
            }
          } else if (prop.attrs & AttrLateInit) {
            if (m_ignoreLateInit) {
              continue;
            } else {
              throw_late_init_prop(prop.cls, memberName.get(), false);
            }
          }
        }
        if (!attrMask &&
            UNLIKELY(obj->getAttribute(ObjectData::HasDynPropArr))) {
          auto const prop = obj->dynPropArray()->get(propName.get());
          if (prop.is_init()) {
            wanted.set(propName, prop);
            continue;
          }
        }
        raise_notice("serialize(): \"%s\" returned as member variable from "
                     "__sleep() but does not exist", propName.data());
        wanted.set(propName, init_null());
      }
      pushObjectInfo(obj->getClassName(), 'O');
      if (!serializableNativeData.isNull()) {
        wanted.set(s_serializedNativeDataKey, serializableNativeData);
      }
      serializeObjProps(wanted);
      popObjectInfo();
    } else {
      raise_notice("serialize(): __sleep should return an array only "
                   "containing the names of instance-variables to "
                   "serialize");
      serializeVariant(uninit_null().asTypedValue());
    }
  } else {
    if (type == VariableSerializer::Type::VarExport &&
        obj->instanceof(c_Closure::classof())) {
      write(obj->getClassName());
    } else {
      auto className = obj->getClassName();
      Array properties = getSerializeProps(obj);
      if (type == VariableSerializer::Type::DebuggerSerialize) {
        try {
          auto val = const_cast<ObjectData*>(obj)->invokeToDebugDisplay(
            m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults());
          if (val.isInitialized()) {
            properties.set(s_PHP_DebugDisplay, *val.asTypedValue());
          }
        } catch (const Object &e) {
          assertx(e->instanceof(SystemLib::getErrorClass()) ||
                  e->instanceof(SystemLib::getExceptionClass()));
          assertx(
            SystemLib::getErrorClass()->lookupDeclProp(s_message.get()) == 0 &&
            SystemLib::getExceptionClass()->lookupDeclProp(s_message.get()) == 0
          );
          auto const message_rval = e->propRvalAtOffset(Slot{0});
          if (isStringType(message_rval.type())) {
            raise_warning("%s::__toDebugDisplay() threw PHP exception "
                          "of class %s with message '%s'",
                          obj->getClassName().data(), e->getClassName().data(),
                          message_rval.val().pstr->data());
          } else {
            raise_warning("%s::__toDebugDisplay() threw PHP exception "
                          "of class %s with non-string message",
                          obj->getClassName().data(), e->getClassName().data());
          }
        } catch (const std::exception &e) {
          raise_warning("%s::__toDebugDisplay() threw C++ exception: %s",
                        obj->getClassName().data(), e.what());
        } catch (...) {
          raise_warning("%s::__toDebugDisplay() threw unknown exception",
                        obj->getClassName().data());
        }
      }
      if (type == VariableSerializer::Type::DebuggerDump) {
        // Expect to display as their stringified classname.
        if (obj->instanceof(c_Closure::classof())) {
          write(obj->getVMClass()->nameStr());
          return;
        }

        // If we have a DebugDisplay prop saved, use it.
        auto const debugDisp = obj->getProp(nullctx, s_PHP_DebugDisplay.get());
        if (debugDisp) {
          serializeVariant(debugDisp, false, false, true);
          return;
        }
        // Otherwise compute it if we have a __toDebugDisplay method.
        auto val = const_cast<ObjectData*>(obj)->invokeToDebugDisplay(
          m_pure ? RuntimeCoeffects::pure() : RuntimeCoeffects::defaults());
        if (val.isInitialized()) {
          serializeVariant(val.asTypedValue(), false, false, true);
          return;
        }
      }
      if (className.get() == s_PHP_Incomplete_Class.get() &&
          (type == VariableSerializer::Type::Serialize ||
           type == VariableSerializer::Type::Internal ||
           type == VariableSerializer::Type::APCSerialize ||
           type == VariableSerializer::Type::DebuggerDump)) {
        auto const cname = obj->getProp(
          nullctx,
          s_PHP_Incomplete_Class_Name.get()
        );
        if (cname && isStringType(cname.type())) {
          pushObjectInfo(StrNR(cname.val().pstr), 'O');
          properties.remove(s_PHP_Incomplete_Class_Name, true);
          serializeObjProps(properties);
          popObjectInfo();
          return;
        }
      }
      pushObjectInfo(className, 'O');
      if (!serializableNativeData.isNull()) {
        properties.set(s_serializedNativeDataKey, serializableNativeData);
      }
      serializeObjProps(properties);
      popObjectInfo();
    }
  }
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeObject(const ObjectData* obj) {
  TypedValue tv = make_tv<KindOfObject>(const_cast<ObjectData*>(obj));
  if (UNLIKELY(incNestedLevel(&tv))) {
    writeOverflow(&tv);
  } else {
    serializeObjectImpl(obj);
  }
  decNestedLevel(&tv);
}

template <class Buffer>
void VariableSerializerImpl<Buffer>::serializeObject(const Object& obj) {
  if (obj) {
    serializeObject(obj.get());
  } else {
    writeNull();
  }
}

template struct VariableSerializerImpl<StringBuffer>;
template struct VariableSerializerImpl<ZStdStringBuffer>;

}
