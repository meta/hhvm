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
#include "hphp/runtime/ext/soap/encoding.h"

#include <cstdlib>
#include <map>
#include <memory>

#include <folly/Conv.h>

#include "hphp/runtime/ext/soap/ext_soap.h"
#include "hphp/runtime/ext/soap/soap.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/zend-functions.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/vm/native-data.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

using std::string;
using std::abs;

bool php_libxml_xmlCheckUTF8(const unsigned char *s) {
  int i;
  unsigned char c;

  for (i = 0; (c = s[i++]);) {
    if ((c & 0x80) == 0) {
    } else if ((c & 0xe0) == 0xc0) {
      if ((s[i++] & 0xc0) != 0x80) {
        return false;
      }
    } else if ((c & 0xf0) == 0xe0) {
      if ((s[i++] & 0xc0) != 0x80 || (s[i++] & 0xc0) != 0x80) {
        return false;
      }
    } else if ((c & 0xf8) == 0xf0) {
      if ((s[i++] & 0xc0) != 0x80 || (s[i++] & 0xc0) != 0x80 ||
          (s[i++] & 0xc0) != 0x80) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

static int zend_strnlen(const char* s, int maxlen) {
  int len = 0;
  while (*s++ && maxlen--) len++;
  return len;
}

static bool zend_unmangle_property_name(const char *mangled_property, int len,
                                        const char **class_name,
                                        const char **prop_name) {
  int class_name_len;
  *class_name = nullptr;

  if (mangled_property[0]!=0) {
    *prop_name = mangled_property;
    return true;
  }
  if (len < 3 || mangled_property[1]==0) {
    raise_warning("Illegal member variable name");
    *prop_name = mangled_property;
    return false;
  }

  class_name_len = zend_strnlen(mangled_property+1, --len - 1) + 1;
  if (class_name_len >= len || mangled_property[class_name_len]!=0) {
    raise_warning("Corrupt member variable name");
    *prop_name = mangled_property;
    return false;
  }
  *class_name = mangled_property+1;
  *prop_name = (*class_name)+class_name_len;
  return true;
}

///////////////////////////////////////////////////////////////////////////////

static Variant to_zval_double  (encodeType* type, xmlNodePtr data);
static Variant to_zval_long    (encodeType* type, xmlNodePtr data);
static Variant to_zval_bool    (encodeType* type, xmlNodePtr data);
static Variant to_zval_string  (encodeType* type, xmlNodePtr data);
static Variant to_zval_stringr (encodeType* type, xmlNodePtr data);
static Variant to_zval_stringc (encodeType* type, xmlNodePtr data);
static Variant to_zval_map     (encodeType* type, xmlNodePtr data);
static Variant to_zval_null    (encodeType* type, xmlNodePtr data);
static Variant to_zval_base64  (encodeType* type, xmlNodePtr data);
static Variant to_zval_hexbin  (encodeType* type, xmlNodePtr data);

static xmlNodePtr to_xml_long
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_double
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_bool
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

/* String encode */
static xmlNodePtr to_xml_string
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_base64
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_hexbin
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

/* Null encode */
static xmlNodePtr to_xml_null
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

/* Array encode */
static xmlNodePtr guess_array_map
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_map
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

static xmlNodePtr to_xml_list
(encodeType* enc, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_list1
(encodeType* enc, const Variant& data, int style, xmlNodePtr parent);

/* Datetime encode/decode */
static xmlNodePtr to_xml_datetime_ex
(encodeType* type, const Variant& data, const char *format, int style,
 xmlNodePtr parent);
static xmlNodePtr to_xml_datetime
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_time
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_date
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_gyearmonth
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_gyear
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_gmonthday
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_gday
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_gmonth
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_duration
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

static Variant to_zval_object(encodeType* type, xmlNodePtr data);
static Variant to_zval_array(encodeType* type, xmlNodePtr data);

static xmlNodePtr to_xml_object
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);
static xmlNodePtr to_xml_array
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

static Variant to_zval_any
(encodeType* type, xmlNodePtr data);
static xmlNodePtr to_xml_any
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

/* Try and guess for non-wsdl clients and servers */
static Variant guess_zval_convert
(encodeType* type, xmlNodePtr data);
static xmlNodePtr guess_xml_convert
(encodeType* type, const Variant& data, int style, xmlNodePtr parent);

static encodePtr get_array_type
(xmlNodePtr node, const Variant& array, string &out_type);

static xmlNodePtr check_and_resolve_href(xmlNodePtr data);

static void set_ns_prop(xmlNodePtr node, const char *ns, const char *name,
                        const char *val);
static void set_xsi_nil(xmlNodePtr node);
static void set_xsi_type(xmlNodePtr node, const char *type);

static void get_type_str
(xmlNodePtr node, const char* ns, const char* type, string &ret);
static void set_ns_and_type_ex(xmlNodePtr node, const char *ns,
                               const char *type);

static void set_ns_and_type(xmlNodePtr node, encodeType* type);

#define FIND_XML_NULL(xml, v)                                   \
  {                                                             \
    if (!xml) {                                                 \
      v = init_null();                                          \
      return v;                                                 \
    }                                                           \
    if (xml->properties) {                                      \
      xmlAttrPtr n = get_attribute(xml->properties, "nil");     \
      if (n) {                                                  \
        v = init_null();                                        \
        return v;                                               \
      }                                                         \
    }                                                           \
  }

#define CHECK_XML_NULL(xml)                                     \
  {                                                             \
    if (!xml) {                                                 \
      return uninit_null();                                     \
    }                                                           \
    if (xml->properties) {                                      \
      xmlAttrPtr n = get_attribute(xml->properties, "nil");     \
      if (n) {                                                  \
        return uninit_null();                                   \
      }                                                         \
    }                                                           \
  }

#define FIND_ZVAL_NULL(v, xml, style)                           \
  {                                                             \
    if (v.isNull()) {                                           \
      if (style == SOAP_ENCODED) {                              \
        set_xsi_nil(xml);                                       \
      }                                                         \
      return xml;                                               \
    }                                                           \
  }

///////////////////////////////////////////////////////////////////////////////

encodeStatic s_defaultEncoding[] = {
  {UNKNOWN_TYPE, "", "", guess_zval_convert, guess_xml_convert},

  {SOAP_ENC_NULL_DT, SOAP_ENC_NULL_DT_STRING, XSI_NAMESPACE,
   to_zval_null, to_xml_null},

  {XSD_STRING, XSD_STRING_STRING, XSD_NAMESPACE,
   to_zval_string, to_xml_string},
  {XSD_BOOLEAN, XSD_BOOLEAN_STRING, XSD_NAMESPACE,
   to_zval_bool, to_xml_bool},
  {XSD_DECIMAL, XSD_DECIMAL_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_FLOAT, XSD_FLOAT_STRING, XSD_NAMESPACE,
   to_zval_double, to_xml_double},
  {XSD_DOUBLE, XSD_DOUBLE_STRING, XSD_NAMESPACE,
   to_zval_double, to_xml_double},

  {XSD_DATETIME, XSD_DATETIME_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_datetime},
  {XSD_TIME, XSD_TIME_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_time},
  {XSD_DATE, XSD_DATE_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_date},
  {XSD_GYEARMONTH, XSD_GYEARMONTH_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_gyearmonth},
  {XSD_GYEAR, XSD_GYEAR_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_gyear},
  {XSD_GMONTHDAY, XSD_GMONTHDAY_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_gmonthday},
  {XSD_GDAY, XSD_GDAY_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_gday},
  {XSD_GMONTH, XSD_GMONTH_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_gmonth},
  {XSD_DURATION, XSD_DURATION_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_duration},

  {XSD_HEXBINARY, XSD_HEXBINARY_STRING, XSD_NAMESPACE,
   to_zval_hexbin, to_xml_hexbin},
  {XSD_BASE64BINARY, XSD_BASE64BINARY_STRING, XSD_NAMESPACE,
   to_zval_base64, to_xml_base64},

  {XSD_LONG, XSD_LONG_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_INT, XSD_INT_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_SHORT, XSD_SHORT_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_BYTE, XSD_BYTE_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_NONPOSITIVEINTEGER,XSD_NONPOSITIVEINTEGER_STRING,XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_POSITIVEINTEGER, XSD_POSITIVEINTEGER_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_NONNEGATIVEINTEGER,XSD_NONNEGATIVEINTEGER_STRING,XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_NEGATIVEINTEGER, XSD_NEGATIVEINTEGER_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_UNSIGNEDBYTE, XSD_UNSIGNEDBYTE_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_UNSIGNEDSHORT, XSD_UNSIGNEDSHORT_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_UNSIGNEDINT, XSD_UNSIGNEDINT_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_UNSIGNEDLONG, XSD_UNSIGNEDLONG_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_INTEGER, XSD_INTEGER_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},

  {XSD_ANYTYPE, XSD_ANYTYPE_STRING, XSD_NAMESPACE,
   guess_zval_convert, guess_xml_convert},
  {XSD_UR_TYPE, XSD_UR_TYPE_STRING, XSD_NAMESPACE,
   guess_zval_convert, guess_xml_convert},
  {XSD_ANYURI, XSD_ANYURI_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_QNAME, XSD_QNAME_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_NOTATION, XSD_NOTATION_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_NORMALIZEDSTRING, XSD_NORMALIZEDSTRING_STRING, XSD_NAMESPACE,
   to_zval_stringr, to_xml_string},
  {XSD_TOKEN, XSD_TOKEN_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_LANGUAGE, XSD_LANGUAGE_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_NMTOKEN, XSD_NMTOKEN_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_NMTOKENS, XSD_NMTOKENS_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_list1},
  {XSD_NAME, XSD_NAME_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_NCNAME, XSD_NCNAME_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_ID, XSD_ID_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_IDREF, XSD_IDREF_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_IDREFS, XSD_IDREFS_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_list1},
  {XSD_ENTITY, XSD_ENTITY_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_ENTITIES, XSD_ENTITIES_STRING, XSD_NAMESPACE,
   to_zval_stringc, to_xml_list1},

  {APACHE_MAP, APACHE_MAP_STRING, APACHE_NAMESPACE,
   to_zval_map, to_xml_map},

  {SOAP_ENC_OBJECT, SOAP_ENC_OBJECT_STRING, SOAP_1_1_ENC_NAMESPACE,
   to_zval_object, to_xml_object},
  {SOAP_ENC_ARRAY, SOAP_ENC_ARRAY_STRING, SOAP_1_1_ENC_NAMESPACE,
   to_zval_array, to_xml_array},
  {SOAP_ENC_ARRAY_DT, SOAP_ENC_ARRAY_DT_STRING, SOAP_1_1_ENC_NAMESPACE,
   to_zval_array, guess_array_map},
  {SOAP_ENC_OBJECT, SOAP_ENC_OBJECT_STRING, SOAP_1_2_ENC_NAMESPACE,
   to_zval_object, to_xml_object},
  {SOAP_ENC_ARRAY, SOAP_ENC_ARRAY_STRING, SOAP_1_2_ENC_NAMESPACE,
   to_zval_array, to_xml_array},
  {SOAP_ENC_ARRAY_DT, SOAP_ENC_ARRAY_DT_STRING, SOAP_1_2_ENC_NAMESPACE,
   to_zval_array, guess_array_map},
  {SOAP_ENC_INT_DT, SOAP_ENC_INT_DT_STRING, XSD_NAMESPACE,
   to_zval_long, to_xml_long},
  {SOAP_ENC_DOUBLE_DT, SOAP_ENC_DOUBLE_DT_STRING, XSD_NAMESPACE,
   to_zval_double, to_xml_double},

  /* support some of the 1999 data types */
  {XSD_STRING, XSD_STRING_STRING, XSD_1999_NAMESPACE,
   to_zval_string, to_xml_string},
  {XSD_BOOLEAN, XSD_BOOLEAN_STRING, XSD_1999_NAMESPACE,
   to_zval_bool, to_xml_bool},
  {XSD_DECIMAL, XSD_DECIMAL_STRING, XSD_1999_NAMESPACE,
   to_zval_stringc, to_xml_string},
  {XSD_FLOAT, XSD_FLOAT_STRING, XSD_1999_NAMESPACE,
   to_zval_double, to_xml_double},
  {XSD_DOUBLE, XSD_DOUBLE_STRING, XSD_1999_NAMESPACE,
   to_zval_double, to_xml_double},

  {XSD_LONG, XSD_LONG_STRING, XSD_1999_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_INT, XSD_INT_STRING, XSD_1999_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_SHORT, XSD_SHORT_STRING, XSD_1999_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_BYTE, XSD_BYTE_STRING, XSD_1999_NAMESPACE,
   to_zval_long, to_xml_long},
  {XSD_1999_TIMEINSTANT,XSD_1999_TIMEINSTANT_STRING,XSD_1999_NAMESPACE,
   to_zval_stringc, to_xml_string},

  {XSD_ANYXML, "<anyXML>", "<anyXML>", to_zval_any, to_xml_any},

  {END_KNOWN_TYPES, nullptr, nullptr, guess_zval_convert, guess_xml_convert},
};

///////////////////////////////////////////////////////////////////////////////

static string get_full_typename(const char *ns, const char *type) {
  string nscat;
  if (ns && *ns) {
    nscat = ns;
    nscat += ':';
  }
  if (type) {
    nscat += type;
  }
  return nscat;
}

static encodePtr get_typemap_type(const char *ns, const char *type) {
  USE_SOAP_GLOBAL;
  std::string nscat = get_full_typename(ns, type);
  if (SOAP_GLOBAL(typemap)) {
    encodeMap::const_iterator iter = SOAP_GLOBAL(typemap)->find(nscat);
    if (iter != SOAP_GLOBAL(typemap)->end()) {
      return iter->second;
    }
  }
  return encodePtr();
}

void whiteSpace_replace(xmlChar* str) {
  while (*str != '\0') {
    if (*str == '\x9' || *str == '\xA' || *str == '\xD') {
      *str = ' ';
    }
    str++;
  }
}

void whiteSpace_collapse(xmlChar* str) {
  xmlChar *pos;
  xmlChar old;

  pos = str;
  whiteSpace_replace(str);
  while (*str == ' ') {
    str++;
  }
  old = '\0';
  while (*str != '\0') {
    if (*str != ' ' || old != ' ') {
      *pos = *str;
      pos++;
    }
    old = *str;
    str++;
  }
  if (old == ' ') {
     --pos;
  }
  *pos = '\0';
}

static encodePtr find_encoder_by_type_name(sdl *sdl, const char *type) {
  if (sdl) {
    for (const auto& e : sdl->encoders) {
      if (strcmp(e.second->details.type_str.c_str(), type) == 0) {
        return e.second;
      }
    }
  }
  return encodePtr();
}

static bool soap_check_zval_ref(const Variant& data, xmlNodePtr node) {
  USE_SOAP_GLOBAL;
  HeapObject* hash = nullptr;
  if (data.isObject()) {
    hash = data.getObjectData();
  }
  if (hash) {
    auto& node_map = SOAP_GLOBAL(node_map);
    auto node_ptr = folly::get_default(node_map, hash);
    if (node_ptr) {
      if (node_ptr == node) {
        return false;
      }
      xmlNodeSetName(node, node_ptr->name);
      xmlSetNs(node, node_ptr->ns);
      xmlAttrPtr attr = node_ptr->properties;
      const char *id;
      std::string prefix;
      if (SOAP_GLOBAL(soap_version) == SOAP_1_1) {
        while (1) {
          attr = get_attribute(attr, "id");
          if (attr == nullptr || attr->ns == nullptr) {
            break;
          }
          attr = attr->next;
        }
        if (attr) {
          prefix = '#';
          prefix += (char*)attr->children->content;
          id = prefix.c_str();
        } else {
          SOAP_GLOBAL(cur_uniq_ref)++;
          prefix = "#ref";
          prefix += folly::to<string>(SOAP_GLOBAL(cur_uniq_ref));
          id = prefix.c_str();
          xmlSetProp(node_ptr, BAD_CAST("id"), BAD_CAST(id+1));
        }
        xmlSetProp(node, BAD_CAST("href"), BAD_CAST(id));
      } else {
        attr = get_attribute_ex(attr, "id", SOAP_1_2_ENC_NAMESPACE);
        if (attr) {
          prefix = '#';
          prefix += (char*)attr->children->content;
          id = prefix.c_str();
        } else {
          SOAP_GLOBAL(cur_uniq_ref)++;
          prefix = "#ref";
          prefix += folly::to<string>(SOAP_GLOBAL(cur_uniq_ref));
          id = prefix.c_str();
          set_ns_prop(node_ptr, SOAP_1_2_ENC_NAMESPACE, "id", id+1);
        }
        set_ns_prop(node, SOAP_1_2_ENC_NAMESPACE, "ref", id);
      }
      return true;
    }
    node_map.emplace(hash, node);
  }
  return false;
}

static bool soap_check_xml_ref(Variant& data, xmlNodePtr node) {
  USE_SOAP_GLOBAL;
  auto& ref_map = SOAP_GLOBAL(ref_map);
  auto const it = ref_map.find(node);
  if (it != ref_map.end()) {
    auto const& data2 = it->second;
    if (!data.isObject() ||
        !data2.isObject() ||
        data.getObjectData() != data2.getObjectData()) {
      data = data2;
      return true;
    }
  }
  return false;
}

static void soap_add_xml_ref(Variant& data, xmlNodePtr node) {
  USE_SOAP_GLOBAL;
  auto& ref_map = SOAP_GLOBAL(ref_map);
  ref_map.emplace(node, data);
}

static xmlNodePtr master_to_xml_int(encodePtr encode, const Variant& data, int style,
                                    xmlNodePtr parent, bool check_class_map) {
  USE_SOAP_GLOBAL;
  xmlNodePtr node = nullptr;
  bool add_type = false;

  /* Special handling of class SoapVar */
  if (data.isObject() && data.toObject().instanceof(SoapVar::classof())) {
    auto dobj = data.toObject().get();
    auto enc_stype = SoapVar::getEncSType(dobj);
    auto enc_ns = SoapVar::getEncNS(dobj);
    encodePtr enc = nullptr;
    if (!enc_stype.isNull()) {
      enc = enc_ns.empty()
        ? get_encoder_ex(SOAP_GLOBAL(sdl), enc_stype.c_str())
        : get_encoder(SOAP_GLOBAL(sdl), enc_ns.c_str(), enc_stype.c_str());


      if (!enc && SOAP_GLOBAL(typemap)) {
        enc = get_typemap_type(enc_ns.c_str(), enc_stype.c_str());
      }
    }
    if (!enc) {
      auto enc_type = SoapVar::getEncType(dobj);
      enc = get_conversion(enc_type);
    }
    if (!enc) {
      enc = encode;
    }

    auto enc_value = SoapVar::getEncValue(dobj);
    node = master_to_xml(enc, enc_value, style, parent);

    if (style == SOAP_ENCODED || (SOAP_GLOBAL(sdl) && encode != enc)) {
      if (!enc_stype.empty()) {
        set_ns_and_type_ex(node, enc_ns.c_str(), enc_stype.c_str());
      }
    }

    auto enc_name = SoapVar::getEncName(dobj);
    if (!enc_name.empty()) {
      xmlNodeSetName(node, BAD_CAST(enc_name.c_str()));
    }
    auto enc_namens = SoapVar::getEncNameNS(dobj);
    if (!enc_namens.empty()) {
      xmlNsPtr nsp = encode_add_ns(node, enc_namens.data());
      xmlSetNs(node, nsp);
    }
  } else {
    if (check_class_map && !SOAP_GLOBAL(soap_classmap).empty() &&
        data.isObject()) {
      auto clsname = data.toObject()->getClassName();
      for (ArrayIter iter(SOAP_GLOBAL(soap_classmap)); iter; ++iter) {
        if (same(iter.second(), clsname.get())) {
          /* TODO: namespace isn't stored */
          encodePtr enc = nullptr;
          if (SOAP_GLOBAL(sdl)) {
            enc = get_encoder(SOAP_GLOBAL(sdl),
                              SOAP_GLOBAL(sdl)->target_ns.c_str(),
                              iter.first().toString().data());
            if (!enc) {
              enc = find_encoder_by_type_name(SOAP_GLOBAL(sdl),
                                              iter.first().toString().data());
            }
          }
          if (enc) {
            if (encode != enc && style == SOAP_LITERAL) {
              add_type = true;
            }
            encode = enc;
          }
          break;
        }
      }
    }

    if (encode == nullptr) {
      encode = get_conversion(UNKNOWN_TYPE);
    }
    if (SOAP_GLOBAL(typemap) && !encode->details.type_str.empty()) {
      encodePtr enc = get_typemap_type(encode->details.ns.c_str(),
                                       encode->details.type_str.c_str());
      if (enc) encode = enc;
    }
    if (encode->to_xml) {
      node = encode->to_xml(&encode->details, data, style, parent);
      if (add_type) {
        set_ns_and_type(node, &encode->details);
      }
    }
  }
  return node;
}

xmlNodePtr master_to_xml(encodePtr encode, const Variant& data, int style,
                         xmlNodePtr parent) {
  return master_to_xml_int(encode, data, style, parent, true);
}

static Variant master_to_zval_int(encodePtr encode, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  Variant ret;
  if (SOAP_GLOBAL(typemap)) {
    if (!encode->details.type_str.empty()) {
      encodePtr enc = get_typemap_type(encode->details.ns.c_str(),
                                       encode->details.type_str.c_str());
      if (enc) encode = enc;
    } else {
      xmlAttrPtr type_attr = get_attribute_ex(data->properties, "type",
                                              XSI_NAMESPACE);
      if (type_attr) {
        std::string ns, cptype;
        parse_namespace(type_attr->children->content, cptype, ns);
        xmlNsPtr nsptr = xmlSearchNs(data->doc, data, NS_STRING(ns));
        string nscat;
        if (nsptr) {
          nscat = (char*)nsptr->href;
          nscat += ':';
        }
        nscat += cptype.data();
        encodeMap::const_iterator iter = SOAP_GLOBAL(typemap)->find(nscat);
        if (iter != SOAP_GLOBAL(typemap)->end()) {
          encode = iter->second;
        }
      }
    }
  }
  if (encode->to_zval) {
    ret = encode->to_zval(&encode->details, data);
  }
  return ret;
}

Variant master_to_zval(encodePtr encode, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  data = check_and_resolve_href(data);

  if (encode == nullptr) {
    encode = get_conversion(UNKNOWN_TYPE);
  } else {
    /* Use xsi:type if it is defined */
    xmlAttrPtr type_attr = get_attribute_ex(data->properties, "type",
                                            XSI_NAMESPACE);
    if (type_attr) {
      encodePtr enc = get_encoder_from_prefix(SOAP_GLOBAL(sdl), data,
                                              type_attr->children->content);
      if (enc && enc != encode) {
        encodePtr tmp = enc;
        while (tmp && tmp->details.sdl_type &&
               tmp->details.sdl_type->kind != XSD_TYPEKIND_COMPLEX) {
          if (enc == tmp->details.sdl_type->encode ||
              tmp == tmp->details.sdl_type->encode) {
            enc.reset();
            break;
          }
          tmp = tmp->details.sdl_type->encode;
        }
        if (enc) {
          encode = enc;
        }
      }
    }
  }
  return master_to_zval_int(encode, data);
}

xmlNodePtr to_xml_user(encodeType* type, const Variant& data, int style,
                       xmlNodePtr parent) {
  xmlNodePtr ret = nullptr;
  if (type && type->map && !type->map->to_xml.isNull()) {
    Variant return_value = vm_call_user_func(type->map->to_xml,
                                             make_vec_array(data));
    if (return_value.isString()) {
      OptString sdoc = return_value.toString();
      xmlDocPtr doc = soap_xmlParseMemory(sdoc.data(), sdoc.size());
      if (doc && doc->children) {
        ret = xmlDocCopyNode(doc->children, parent->doc, 1);
      }
      xmlFreeDoc(doc);
    }
  }
  if (!ret) {
    ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  }
  xmlAddChild(parent, ret);
  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

Variant to_zval_user(encodeType* type, xmlNodePtr node) {
  Variant return_value;
  if (type && type->map && !type->map->to_zval.isNull()) {
    xmlNodePtr copy = xmlCopyNode(node, 1);
    xmlBufferPtr buf = xmlBufferCreate();
    xmlNodeDump(buf, nullptr, copy, 0, 0);
    OptString data((char*)xmlBufferContent(buf), CopyString);
    xmlBufferFree(buf);
    xmlFreeNode(copy);

    return_value = vm_call_user_func(type->map->to_zval,
                                     make_vec_array(data));
  }
  return return_value;
}

/* TODO: get rid of "bogus".. ither by passing in the already created xmlnode
   or passing in the node name */
/* String encode/decode */
static Variant to_zval_string(encodeType* /*type*/, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      if (SOAP_GLOBAL(encoding) != nullptr) {
        xmlBufferPtr in =
          xmlBufferCreateStatic(data->children->content,
                                xmlStrlen(data->children->content));
        xmlBufferPtr out = xmlBufferCreate();
        int n = xmlCharEncOutFunc(SOAP_GLOBAL(encoding), out, in);
        if (n >= 0) {
          ret = OptString((char*)xmlBufferContent(out), CopyString);
        } else {
          ret = OptString((char*)data->children->content, CopyString);
        }
        xmlBufferFree(out);
        xmlBufferFree(in);
      } else {
        ret = OptString((char*)data->children->content, CopyString);
      }
    } else if (data->children->type == XML_CDATA_SECTION_NODE &&
               data->children->next == nullptr) {
      ret = OptString((char*)data->children->content, CopyString);
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  } else {
    ret = empty_string_variant();
  }
  return ret;
}

static Variant to_zval_stringr(encodeType* /*type*/, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      whiteSpace_replace(data->children->content);
      if (SOAP_GLOBAL(encoding) != nullptr) {
        xmlBufferPtr in =
          xmlBufferCreateStatic(data->children->content,
                                xmlStrlen(data->children->content));
        xmlBufferPtr out = xmlBufferCreate();
        int n = xmlCharEncOutFunc(SOAP_GLOBAL(encoding), out, in);
        if (n >= 0) {
          ret = OptString((char*)xmlBufferContent(out), CopyString);
        } else {
          ret = OptString((char*)data->children->content, CopyString);
        }
        xmlBufferFree(out);
        xmlBufferFree(in);
      } else {
        ret = OptString((char*)data->children->content, CopyString);
      }
    } else if (data->children->type == XML_CDATA_SECTION_NODE &&
               data->children->next == nullptr) {
      ret = OptString((char*)data->children->content, CopyString);
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  } else {
    ret = empty_string_variant();
  }
  return ret;
}

static Variant to_zval_stringc(encodeType* /*type*/, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      whiteSpace_collapse(data->children->content);
      if (SOAP_GLOBAL(encoding) != nullptr) {
        xmlBufferPtr in =
          xmlBufferCreateStatic(data->children->content,
                                xmlStrlen(data->children->content));
        xmlBufferPtr out = xmlBufferCreate();
        int n = xmlCharEncOutFunc(SOAP_GLOBAL(encoding), out, in);
        if (n >= 0) {
          ret = OptString((char*)xmlBufferContent(out), CopyString);
        } else {
          ret = OptString((char*)data->children->content, CopyString);
        }
        xmlBufferFree(out);
        xmlBufferFree(in);
      } else {
        ret = OptString((char*)data->children->content, CopyString);
      }
    } else if (data->children->type == XML_CDATA_SECTION_NODE &&
               data->children->next == nullptr) {
      ret = OptString((char*)data->children->content, CopyString);
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  } else {
    ret = empty_string_variant();
  }
  return ret;
}

static Variant to_zval_base64(encodeType* /*type*/, xmlNodePtr data) {
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      whiteSpace_collapse(data->children->content);
      OptString str =
        StringUtil::Base64Decode(OptString((const char*)data->children->content));
      if (str.isNull()) {
        throw SoapException("Encoding: Violation of encoding rules");
      }
      ret = str;
    } else if (data->children->type == XML_CDATA_SECTION_NODE &&
               data->children->next == nullptr) {
      OptString str =
        StringUtil::Base64Decode(OptString((const char*)data->children->content));
      if (str.isNull()) {
        throw SoapException("Encoding: Violation of encoding rules");
      }
      ret = str;
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  } else {
    ret = empty_string_variant();
  }
  return ret;
}

static Variant to_zval_hexbin(encodeType* /*type*/, xmlNodePtr data) {
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      whiteSpace_collapse(data->children->content);
    } else if (data->children->type != XML_CDATA_SECTION_NODE ||
               data->children->next != nullptr) {
      throw SoapException("Encoding: Violation of encoding rules");
    }
    auto const str =
      HHVM_FN(hex2bin)(OptString((const char*)data->children->content)).toString();
    if (str.isNull()) {
      throw SoapException("Encoding: Violation of encoding rules");
    }
    ret = str;
  } else {
    ret = empty_string_variant();
  }
  return ret;
}

static
xmlNodePtr to_xml_string(encodeType* type, const Variant& data, int style,
                         xmlNodePtr parent) {
  USE_SOAP_GLOBAL;
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  OptString str = data.toString();
  if (SOAP_GLOBAL(encoding) != nullptr) {
    xmlBufferPtr in  = xmlBufferCreateStatic((void*)str.data(), str.size());
    xmlBufferPtr out = xmlBufferCreate();
    int n = xmlCharEncInFunc(SOAP_GLOBAL(encoding), out, in);
    if (n >= 0) {
      str = OptString((char*)xmlBufferContent(out), n, CopyString);
    }
    xmlBufferFree(out);
    xmlBufferFree(in);
  }

  if (!php_libxml_xmlCheckUTF8(BAD_CAST(str.data()))) {
    char *err = (char*)req::malloc_noptrs(str.size() + 8);
    char c;
    memcpy(err, str.data(), str.size() + 1);
    int i = 0;
    while ((c = err[i++])) {
      if ((c & 0x80) == 0) {
      } else if ((c & 0xe0) == 0xc0) {
        if ((err[i] & 0xc0) != 0x80) {
          break;
        }
        i++;
      } else if ((c & 0xf0) == 0xe0) {
        if ((err[i] & 0xc0) != 0x80 || (err[i+1] & 0xc0) != 0x80) {
          break;
        }
        i += 2;
      } else if ((c & 0xf8) == 0xf0) {
        if ((err[i] & 0xc0) != 0x80 ||
            (err[i+1] & 0xc0) != 0x80 || (err[i+2] & 0xc0) != 0x80) {
          break;
        }
        i += 3;
      } else {
        break;
      }
    }
    if (c) {
      err[i-1] = '\\';
      err[i++] = 'x';
      err[i++] = ((unsigned char)c >> 4) +
        ((((unsigned char)c >> 4) > 9) ? ('a' - 10) : '0');
      err[i++] = (c & 15) + (((c & 15) > 9) ? ('a' - 10) : '0');
      err[i++] = '.';
      err[i++] = '.';
      err[i++] = '.';
      err[i++] = 0;
    }
    std::string serr = err;
    req::free(err);
    throw SoapException("Encoding: string '%s' is not a valid utf-8 string",
                        serr.c_str());
  }

  xmlNodePtr text = xmlNewTextLen(BAD_CAST(str.data()), str.size());
  xmlAddChild(ret, text);

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

static
xmlNodePtr to_xml_base64(encodeType* type, const Variant& data, int style,
                         xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  OptString str = StringUtil::Base64Encode(data.toString());
  xmlAddChild(ret, xmlNewTextLen(BAD_CAST(str.data()), str.size()));

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

static
xmlNodePtr to_xml_hexbin(encodeType* type, const Variant& data, int style,
                         xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  OptString str = HHVM_FN(bin2hex)(data.toString());
  xmlAddChild(ret, xmlNewTextLen(BAD_CAST(str.data()), str.size()));

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

static Variant to_zval_double(encodeType* /*type*/, xmlNodePtr data) {
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      int64_t lval; double dval;
      whiteSpace_collapse(data->children->content);
      OptString content((char*)data->children->content, CopyString);

      auto dt = is_numeric_string((const char *)data->children->content,
                                  data->children->content ?
                                  strlen((char*)data->children->content) : 0,
                                  &lval, &dval, 0);
      if (isIntType(dt)) {
        ret = lval;
      } else if (isDoubleType(dt)) {
        ret = dval;
      } else {
        if (data->children->content) {
          if (strcasecmp((const char *)data->children->content, "NaN") == 0) {
            ret = atof("nan");
          } else if (strcasecmp((const char *)data->children->content, "INF")
                     == 0) {
            ret = atof("inf");
          } else if (strcasecmp((const char *)data->children->content, "-INF")
                     == 0) {
            ret = -atof("inf");
          } else {
            throw SoapException("Encoding: Violation of encoding rules");
          }
        } else {
          throw SoapException("Encoding: Violation of encoding rules");
        }
      }
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  }
  return ret;
}

static Variant to_zval_long(encodeType* /*type*/, xmlNodePtr data) {
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      int64_t lval; double dval;
      whiteSpace_collapse(data->children->content);

      auto dt = is_numeric_string((const char *)data->children->content,
                                  data->children->content ?
                                  strlen((char*)data->children->content) : 0,
                                  &lval, &dval, 0);
      if (isIntType(dt)) {
        ret = (int64_t)lval;
      } else if (isDoubleType(dt)) {
        ret = dval;
      } else {
        throw SoapException("Encoding: Violation of encoding rules");
      }
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  }
  return ret;
}

static xmlNodePtr to_xml_long(encodeType* type, const Variant& data, int style,
                              xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  if (data.isDouble()) {
    char s[256];
    snprintf(s, sizeof(s), "%0.0F", floor(data.toDouble()));
    xmlNodeSetContent(ret, BAD_CAST(s));
  } else {
    Variant d = data.toInt64();
    OptString sd = d.toString();
    xmlNodeSetContentLen(ret, BAD_CAST(sd.data()), sd.size());
  }

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

static
xmlNodePtr to_xml_double(encodeType* type, const Variant& data, int style,
                         xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  Variant d = data.toDouble();
  OptString sd = d.toString();
  xmlNodeSetContentLen(ret, BAD_CAST(sd.data()), sd.size());

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

static Variant to_zval_bool(encodeType* /*type*/, xmlNodePtr data) {
  Variant ret;
  FIND_XML_NULL(data, ret);
  if (data && data->children) {
    if (data->children->type == XML_TEXT_NODE &&
        data->children->next == nullptr) {
      whiteSpace_collapse(data->children->content);
      const char *content = (const char *)data->children->content;
      if (strcasecmp((char*)content, "true") == 0 ||
          strcasecmp((char*)content, "t") == 0 ||
          strcmp((char*)content, "1") == 0) {
        ret = true;
      } else if (strcasecmp((char*)content, "false") == 0 ||
                 strcasecmp((char*)content, "f") == 0 ||
                 strcmp((char*)content, "0") == 0) {
        ret = false;
      } else {
        ret = OptString((char*)content, CopyString).toBoolean();
      }
    } else {
      throw SoapException("Encoding: Violation of encoding rules");
    }
  }
  return ret;
}

static xmlNodePtr to_xml_bool(encodeType* type, const Variant& data, int style,
                              xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);

  if (data.toBoolean()) {
    xmlNodeSetContent(ret, BAD_CAST("true"));
  } else {
    xmlNodeSetContent(ret, BAD_CAST("false"));
  }

  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, type);
  }
  return ret;
}

/* Null encode/decode */
static Variant to_zval_null(encodeType* /*type*/, xmlNodePtr /*data*/) {
  return init_null();
}

static xmlNodePtr to_xml_null(encodeType* /*type*/, const Variant& /*data*/,
                              int style, xmlNodePtr parent) {
  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  if (style == SOAP_ENCODED) {
    set_xsi_nil(ret);
  }
  return ret;
}

namespace {
// Returns Uninit for a missing property. May also return Uninit for properties
// on objects that haven't yet been initialized.
TypedValue get_property(const Variant &object, const char* name) {
  OptString sname(name);
  if (object.isObject()) {
    auto const obj = object.toObject();
    auto const lval = obj->getPropIgnoreAccessibility(sname.get());
    return lval ? lval.tv() : make_tv<KindOfUninit>();
  } else if (object.isArray()) {
    return object.asCArrRef()->get(sname.get());
  }
  return make_tv<KindOfUninit>();
}
}

const StaticString
  s_any("any"),
  s__("_");

static void model_to_zval_any(Variant &ret, xmlNodePtr node) {
  const char* name = nullptr;
  Variant any;
  while (node != nullptr) {
    auto const prop = get_property(ret, (const char *)node->name);
    if (type(prop) == KindOfUninit) {
      Variant val = master_to_zval(get_conversion(XSD_ANYXML), node);

      if (!any.isNull() && !any.isArray()) {
        Array arr = Array::CreateDict();
        if (name) {
          arr.set(OptString(name, CopyString), any);
        } else {
          arr.append(any);
        }
        any = arr;
      }

      if (val.isString() && val.toString().charAt(0) == '<') {
        name = nullptr;
        while (node->next != nullptr) {
          Variant val2 = master_to_zval(get_conversion(XSD_ANYXML),
                                        node->next);
          if (!val2.isString()) {
            break;
          }
          concat_assign(val.asTypedValue(), val2.toString());
          node = node->next;
        }
      } else {
        name = (const char*)node->name;
      }

      if (any.isNull()) {
        if (name) {
          Array arr = Array::CreateDict();
          arr.set(OptString(name, CopyString), val);
          any = arr;
          name = nullptr;
        } else {
          any = val;
        }
      } else {
        /* Add array element */
        if (name) {
          OptString name_str(name);
          if (any.asArrRef().exists(name_str)) {
            auto const el = any.asArrRef().lval(name_str);
            if (!isArrayLikeType(el.type())) {
              /* Convert into array */
              Array arr = Array::CreateVec();
              arr.append(el.tv());
              tvSet(make_array_like_tv(arr.get()), el);
            }
            asArrRef(el).append(val);
          } else {
            any.asArrRef().set(name_str, val);
          }
        } else {
          any.asArrRef().append(val);
        }
        name = nullptr;
      }
    }
    node = node->next;
  }
  if (any.toBoolean()) {
    if (name) {
      ret.toObject()->o_set(OptString(name), any);
    } else {
      ret.toObject()->setProp(nullctx, s_any.get(), *any.asTypedValue());
    }
  }
}

static void model_to_zval_object(Variant &ret, sdlContentModelPtr model,
                                 xmlNodePtr data, sdl *sdl) {
  USE_SOAP_GLOBAL;
  switch (model->kind) {
  case XSD_CONTENT_ELEMENT:
    if (!model->u_element->name.empty()) {
      xmlNodePtr node = get_node(data->children,
                                 (char*)model->u_element->name.c_str());
      if (node) {
        Variant val;
        xmlNodePtr r_node = check_and_resolve_href(node);
        if (r_node && r_node->children && r_node->children->content) {
          if (!model->u_element->fixed.empty() &&
              model->u_element->fixed != (char*)r_node->children->content) {
            throw SoapException("Encoding: Element '%s' has fixed value '%s' "
                                "(value '%s' is not allowed)",
                                model->u_element->name.c_str(),
                                model->u_element->fixed.c_str(),
                                r_node->children->content);
          }
          val = master_to_zval(model->u_element->encode, r_node);
        } else if (!model->u_element->fixed.empty()) {
          xmlNodePtr dummy = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlNodeSetContent(dummy, BAD_CAST(model->u_element->fixed.c_str()));
          val = master_to_zval(model->u_element->encode, dummy);
          xmlFreeNode(dummy);
        } else if (!model->u_element->def.empty() &&
                   !model->u_element->nillable) {
          xmlNodePtr dummy = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlNodeSetContent(dummy,
                            BAD_CAST(model->u_element->def.c_str()));
          val = master_to_zval(model->u_element->encode, dummy);
          xmlFreeNode(dummy);
        } else {
          val = master_to_zval(model->u_element->encode, r_node);
        }
        if ((node = get_node(node->next,
                           (char*)model->u_element->name.c_str())) != nullptr) {
          Array array = Array::CreateVec();
          array.append(val);
          do {
            if (node && node->children && node->children->content) {
              if (!model->u_element->fixed.empty() &&
                  model->u_element->fixed != (char*)node->children->content) {
                throw SoapException("Encoding: Element '%s' has fixed value "
                                    "'%s' (value '%s' is not allowed)",
                                    model->u_element->name.c_str(),
                                    model->u_element->fixed.c_str(),
                                    node->children->content);
              }
              val = master_to_zval(model->u_element->encode, node);
            } else if (!model->u_element->fixed.empty()) {
              xmlNodePtr dummy = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
              xmlNodeSetContent(dummy,
                                BAD_CAST(model->u_element->fixed.c_str()));
              val = master_to_zval(model->u_element->encode, dummy);
              xmlFreeNode(dummy);
            } else if (!model->u_element->def.empty() &&
                       !model->u_element->nillable) {
              xmlNodePtr dummy = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
              xmlNodeSetContent(dummy,
                                BAD_CAST(model->u_element->def.c_str()));
              val = master_to_zval(model->u_element->encode, dummy);
              xmlFreeNode(dummy);
            } else {
              val = master_to_zval(model->u_element->encode, node);
            }
            array.append(val);
          } while ((node = get_node
                    (node->next,
                     (char*)model->u_element->name.c_str())) != nullptr);
          val = array;
        } else if ((!val.isNull() || !model->u_element->nillable) &&
                   (SOAP_GLOBAL(features) & SOAP_SINGLE_ELEMENT_ARRAYS) &&
                   (model->max_occurs == -1 || model->max_occurs > 1)) {
          Array array = Array::CreateVec();
          array.append(val);
          val = array;
        }
        ret.toObject()->o_set(OptString(model->u_element->name), val);
      }
    }
    break;
  case XSD_CONTENT_ALL:
  case XSD_CONTENT_SEQUENCE:
  case XSD_CONTENT_CHOICE: {
    sdlContentModelPtr any;
    for (unsigned int i = 0; i < model->u_content.size(); i++) {
      sdlContentModelPtr content = model->u_content[i];
      if (content->kind == XSD_CONTENT_ANY) {
        any = content;
      } else {
        model_to_zval_object(ret, content, data, sdl);
      }
    }
    if (any) {
      model_to_zval_any(ret, data->children);
    }
    break;
  }
  case XSD_CONTENT_GROUP:
    model_to_zval_object(ret, model->u_group->model, data, sdl);
    break;
  default:
    break;
  }
}

/* Struct encode/decode */
static Variant to_zval_object_ex(encodeType* etype, xmlNodePtr data,
                                 const char *pce) {
  USE_SOAP_GLOBAL;
  const char *ce = "stdClass";
  OptString clsname;
  if (pce) {
    ce = pce;
  } else if (!SOAP_GLOBAL(soap_classmap).empty() && !etype->type_str.empty()) {
    OptString type_str(etype->type_str);
    if (SOAP_GLOBAL(soap_classmap).exists(type_str)) {
      clsname = SOAP_GLOBAL(soap_classmap)[type_str].toString();
      ce = clsname.data();
    }
  }

  Variant ret;
  bool redo_any = false;
  sdlType *sdlType = etype->sdl_type;
  sdl *sdl = SOAP_GLOBAL(sdl);
  if (sdlType) {
    if (sdlType->kind == XSD_TYPEKIND_RESTRICTION &&
        sdlType->encode && etype != &sdlType->encode->details) {
      encodePtr enc = sdlType->encode;
      while (enc && enc->details.sdl_type &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_SIMPLE &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_LIST &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_UNION) {
        enc = enc->details.sdl_type->encode;
      }
      if (enc) {
        if (soap_check_xml_ref(ret, data)) {
          return ret;
        }
        ret = create_object(ce, Array());
        ret.toObject()->setProp(nullctx, s__.get(),
                                *master_to_zval_int(enc, data).asTypedValue());
      } else {
        FIND_XML_NULL(data, ret);
        if (soap_check_xml_ref(ret, data)) {
          return ret;
        }
        ret = create_object(ce, Array());
        soap_add_xml_ref(ret, data);
      }
    } else if (sdlType->kind == XSD_TYPEKIND_EXTENSION &&
               sdlType->encode &&
               etype != &sdlType->encode->details) {
      encodeType &details = sdlType->encode->details;
      if (details.sdl_type &&
          details.sdl_type->kind != XSD_TYPEKIND_SIMPLE &&
          details.sdl_type->kind != XSD_TYPEKIND_LIST &&
          details.sdl_type->kind != XSD_TYPEKIND_UNION) {

        CHECK_XML_NULL(data)
        if (soap_check_xml_ref(ret, data)) {
          return ret;
        }

        if (strcasecmp(ce, "stdClass") != 0 &&
            sdlType->encode->to_zval == sdl_guess_convert_zval &&
            details.sdl_type != nullptr &&
            (details.sdl_type->kind == XSD_TYPEKIND_COMPLEX ||
             details.sdl_type->kind == XSD_TYPEKIND_RESTRICTION ||
             details.sdl_type->kind == XSD_TYPEKIND_EXTENSION) &&
            (details.sdl_type->encode == nullptr ||
             !isSoapArrayType(details.sdl_type->encode->details.type))) {
          ret = to_zval_object_ex(&sdlType->encode->details, data, ce);
        } else {
          ret = master_to_zval_int(sdlType->encode, data);
        }

        soap_add_xml_ref(ret, data);
        redo_any = type(get_property(ret, "any")) != KindOfUninit;
      } else {
        if (soap_check_xml_ref(ret, data)) {
          return ret;
        }
        ret = create_object(ce, Array());
        soap_add_xml_ref(ret, data);
        ret.toObject()->setProp(
          nullctx,
          s__.get(),
          *master_to_zval_int(sdlType->encode, data).asTypedValue()
        );
      }
    } else {
      FIND_XML_NULL(data, ret);
      if (soap_check_xml_ref(ret, data)) {
        return ret;
      }
      ret = create_object(ce, Array());
      soap_add_xml_ref(ret, data);
    }
    if (sdlType->model) {
      if (redo_any) {
        ret.toObject()->unsetProp(nullctx, s_any.get());
        soap_add_xml_ref(ret, data);
      }
      model_to_zval_object(ret, sdlType->model, data, sdl);
      if (redo_any) {
        model_to_zval_any(ret, data->children);
      }
    }
    if (!sdlType->attributes.empty()) {
      for (sdlAttributeMap::const_iterator iter = sdlType->attributes.begin();
           iter != sdlType->attributes.end(); ++iter) {
        sdlAttributePtr attr = iter->second;
        if (!attr->name.empty()) {
          xmlAttrPtr val = get_attribute(data->properties,
                                         (char*)attr->name.c_str());
          const char *str_val = nullptr;
          if (val && val->children && val->children->content) {
            str_val = (char*)val->children->content;
            if (!attr->fixed.empty() && attr->fixed != str_val) {
              throw SoapException("Encoding: Attribute '%s' has fixed value"
                                  " '%s' (value '%s' is not allowed)",
                                  attr->name.c_str(), attr->fixed.c_str(),
                                  str_val);
            }
          } else if (!attr->fixed.empty()) {
            str_val = attr->fixed.c_str();
          } else if (!attr->def.empty()) {
            str_val = attr->def.c_str();
          }
          if (str_val) {
            xmlNodePtr dummy = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
            xmlNodePtr text = xmlNewText(BAD_CAST(str_val));
            xmlAddChild(dummy, text);
            Variant data = master_to_zval(attr->encode, dummy);
            xmlFreeNode(dummy);
            ret.toObject()->o_set(OptString(attr->name), data);
          }
        }
      }
    }
  } else {
    FIND_XML_NULL(data, ret);
    if (soap_check_xml_ref(ret, data)) {
      return ret;
    }

    ret = create_object(ce, Array());
    soap_add_xml_ref(ret, data);
    xmlNodePtr trav = data->children;
    auto obj = ret.asObjRef();

    while (trav != nullptr) {
      if (trav->type == XML_ELEMENT_NODE) {
        Variant tmpVal = master_to_zval(encodePtr(), trav);
        auto const propName = (char*)trav->name;
        // $prop = $ret->{$propName}
        auto const name = OptString(propName, CopyString);
        auto const prop = obj->getPropIgnoreAccessibility(name.get());
        if (!prop.is_set() || prop.type() == KindOfUninit) {
          if (!trav->next || !get_node(trav->next, propName)) {
            obj->o_set(name, tmpVal);
          } else {
            VecInit arr{1};
            arr.append(tmpVal);
            obj->o_set(name, arr.toArray());
          }
        } else {
          if (!tvIsVec(prop)) {
            /* Convert into array */
            VecInit arr{2};
            arr.append(prop.tv());
            arr.append(tmpVal);
            obj->o_set(name, arr.toArray());
          } else {
            asArrRef(prop).append(tmpVal);
          }
        }
      }
      trav = trav->next;
    }
  }
  return ret;
}

static Variant to_zval_object(encodeType* type, xmlNodePtr data) {
  return to_zval_object_ex(type, data, nullptr);
}

static int model_to_xml_object(xmlNodePtr node, sdlContentModelPtr model,
                               Variant &object, int style, int strict) {
  switch (model->kind) {
  case XSD_CONTENT_ELEMENT: {
    xmlNodePtr property;
    encodePtr enc;

    auto const data = get_property(object, model->u_element->name.c_str());
    if (type(data) != KindOfUninit) {
      if (type(data) == KindOfNull &&
          !model->u_element->nillable && model->min_occurs > 0 && !strict) {
        return 0;
      }
      enc = model->u_element->encode;
      if ((model->max_occurs == -1 || model->max_occurs > 1) &&
          isArrayLikeType(type(data)) &&
          val(data).parr->isVectorData()) {
        for (ArrayIter iter(val(data).parr); iter; ++iter) {
          Variant val = iter.second();
          if (val.isNull() && model->u_element->nillable) {
            property = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
            xmlAddChild(node, property);
            set_xsi_nil(property);
          } else {
            property = master_to_xml(enc, val, style, node);
            if (property->children && property->children->content &&
                !model->u_element->fixed.empty() &&
                model->u_element->fixed != (char*)property->children->content) {
              throw SoapException("Encoding: Element '%s' has fixed value "
                                  "'%s' (value '%s' is not allowed)",
                                  model->u_element->name.c_str(),
                                  model->u_element->fixed.c_str(),
                                  property->children->content);
            }
          }
          xmlNodeSetName(property, BAD_CAST(model->u_element->name.c_str()));
          if (style == SOAP_LITERAL &&
              !model->u_element->namens.empty() &&
              model->u_element->form == XSD_FORM_QUALIFIED) {
            xmlNsPtr nsp = encode_add_ns(property,
                                         model->u_element->namens.c_str());
            xmlSetNs(property, nsp);
          }
        }
      } else {
        if (type(data) == KindOfNull && model->u_element->nillable) {
          property = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlAddChild(node, property);
          set_xsi_nil(property);
        } else if (type(data) == KindOfNull && model->min_occurs == 0) {
          return 1;
        } else {
          property = master_to_xml(enc, tvAsCVarRef(data), style, node);
          if (property->children && property->children->content &&
              !model->u_element->fixed.empty() &&
              model->u_element->fixed != (char*)property->children->content) {
            throw SoapException("Encoding: Element '%s' has fixed value '%s' "
                                "(value '%s' is not allowed)",
                                model->u_element->name.c_str(),
                                model->u_element->fixed.c_str(),
                                property->children->content);
          }
        }
        xmlNodeSetName(property, BAD_CAST(model->u_element->name.c_str()));
        if (style == SOAP_LITERAL &&
            !model->u_element->namens.empty() &&
            model->u_element->form == XSD_FORM_QUALIFIED) {
          xmlNsPtr nsp = encode_add_ns(property,
                                       model->u_element->namens.c_str());
          xmlSetNs(property, nsp);
        }
      }
      return 1;
    } else if (strict && model->u_element->nillable && model->min_occurs > 0) {
      property = xmlNewNode(nullptr, BAD_CAST(model->u_element->name.c_str()));
      xmlAddChild(node, property);
      set_xsi_nil(property);
      if (style == SOAP_LITERAL &&
          model->u_element->form == XSD_FORM_QUALIFIED) {
        xmlNsPtr nsp = encode_add_ns(property,
                                     model->u_element->namens.c_str());
        xmlSetNs(property, nsp);
      }
      return 1;
    } else if (model->min_occurs == 0) {
      return 2;
    } else {
      if (strict) {
        throw SoapException("Encoding: object hasn't '%s' property",
                            model->u_element->name.c_str());
      }
      return 0;
    }
  }
  case XSD_CONTENT_ANY: {
    encodePtr enc;

    auto const data = get_property(object, "any");
    if (type(data) != KindOfUninit) {
      enc = get_conversion(XSD_ANYXML);
      if ((model->max_occurs == -1 || model->max_occurs > 1) &&
          isArrayLikeType(type(data)) &&
          val(data).parr->isVectorData()) {
        for (ArrayIter iter(val(data).parr); iter; ++iter) {
          master_to_xml(enc, iter.second(), style, node);
        }
      } else {
        master_to_xml(enc, tvAsCVarRef(data), style, node);
      }
      return 1;
    } else if (model->min_occurs == 0) {
      return 2;
    } else {
      if (strict) {
        throw SoapException( "Encoding: object hasn't 'any' property");
      }
      return 0;
    }
  }
  case XSD_CONTENT_SEQUENCE:
  case XSD_CONTENT_ALL:
    for (unsigned int i = 0; i < model->u_content.size(); i++) {
      sdlContentModelPtr tmp = model->u_content[i];
      if (!model_to_xml_object(node,tmp,object,style,tmp->min_occurs > 0) &&
          tmp->min_occurs > 0) {
        return 0;
      }
    }
    return 1;
  case XSD_CONTENT_CHOICE: {
    int ret = 0;
    for (unsigned int i = 0; i < model->u_content.size(); i++) {
      sdlContentModelPtr tmp = model->u_content[i];
      int tmp_ret = model_to_xml_object(node, tmp, object, style, 0);
      if (tmp_ret == 1) {
        return 1;
      } else if (tmp_ret != 0) {
        ret = 1;
      }
    }
    return ret;
  }
  case XSD_CONTENT_GROUP:
    return model_to_xml_object(node, model->u_group->model, object, style,
                               model->min_occurs > 0);
  default:
    break;
  }
  return 1;
}

static sdlTypePtr model_array_element(sdlContentModelPtr model) {
  switch (model->kind) {
  case XSD_CONTENT_ELEMENT: {
    if (model->max_occurs == -1 || model->max_occurs > 1) {
      return sdlTypePtr(model->u_element, [] (const void*) {});
    } else {
      return sdlTypePtr();
    }
  }
  case XSD_CONTENT_SEQUENCE:
  case XSD_CONTENT_ALL:
  case XSD_CONTENT_CHOICE: {
    if (model->u_content.size() != 1) {
      return sdlTypePtr();
    }
    return model_array_element(model->u_content[0]);
  }
  case XSD_CONTENT_GROUP:
    return model_array_element(model->u_group->model);
  default:
    break;
  }
  return sdlTypePtr();
}

static
xmlNodePtr to_xml_object(encodeType* type, const Variant& data_, int style,
                         xmlNodePtr parent) {
  xmlNodePtr xmlParam;
  sdlType *sdlType = type->sdl_type;
  Variant data = data_;

  if (data.isNull()) {
    xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
    xmlAddChild(parent, xmlParam);
    if (style == SOAP_ENCODED) {
      set_xsi_nil(xmlParam);
      set_ns_and_type(xmlParam, type);
    }
    return xmlParam;
  }

  Array prop;
  if (data.isObject() || data.isArray()) {
    prop = data.toArray();
  }

  if (sdlType) {
    if (sdlType->kind == XSD_TYPEKIND_RESTRICTION &&
        sdlType->encode && type != &sdlType->encode->details) {
      encodePtr enc = sdlType->encode;
      while (enc && enc->details.sdl_type &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_SIMPLE &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_LIST &&
             enc->details.sdl_type->kind != XSD_TYPEKIND_UNION) {
        enc = enc->details.sdl_type->encode;
      }
      if (enc) {
        auto const tmp = get_property(data, "_");
        if (tmp.m_type != KindOfUninit) {
          xmlParam = master_to_xml(enc, tvAsCVarRef(tmp), style, parent);
        } else if (prop.isNull()) {
          xmlParam = master_to_xml(enc, data, style, parent);
        } else {
          xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlAddChild(parent, xmlParam);
        }
      } else {
        xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
        xmlAddChild(parent, xmlParam);
      }
    } else if (sdlType->kind == XSD_TYPEKIND_EXTENSION &&
               sdlType->encode && type != &sdlType->encode->details) {
      if (sdlType->encode->details.sdl_type &&
          sdlType->encode->details.sdl_type->kind != XSD_TYPEKIND_SIMPLE &&
          sdlType->encode->details.sdl_type->kind != XSD_TYPEKIND_LIST &&
          sdlType->encode->details.sdl_type->kind != XSD_TYPEKIND_UNION) {
        xmlParam = master_to_xml_int(sdlType->encode, data, style,
                                     parent, false);
      } else {
        auto const enc = sdlType->encode;
        auto const tmp = get_property(data, "_");
        if (tmp.m_type != KindOfUninit) {
          xmlParam = master_to_xml(enc, tvAsCVarRef(tmp), style, parent);
        } else if (prop.isNull()) {
          xmlParam = master_to_xml(enc, data, style, parent);
        } else {
          xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlAddChild(parent, xmlParam);
        }
      }
    } else {
      xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
      xmlAddChild(parent, xmlParam);
    }

    if (soap_check_zval_ref(data, xmlParam)) {
      return xmlParam;
    }
    if (!prop.isNull()) {
      sdlTypePtr array_el;

      if (data.isArray() && data.toArray()->isVectorData() &&
          sdlType->attributes.empty() && sdlType->model != nullptr &&
          (array_el = model_array_element(sdlType->model)) != nullptr) {
        for (ArrayIter iter(prop); iter; ++iter) {
          Variant val = iter.second();
          xmlNodePtr property;
          if (val.isNull() && array_el->nillable) {
            property = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
            xmlAddChild(xmlParam, property);
            set_xsi_nil(property);
          } else {
            property = master_to_xml(array_el->encode, val, style, xmlParam);
          }
          xmlNodeSetName(property, BAD_CAST(array_el->name.c_str()));
          if (style == SOAP_LITERAL && !array_el->namens.empty() &&
              array_el->form == XSD_FORM_QUALIFIED) {
            xmlNsPtr nsp = encode_add_ns(property, array_el->namens.c_str());
            xmlSetNs(property, nsp);
          }
        }
      } else if (sdlType->model) {
        model_to_xml_object(xmlParam, sdlType->model, data, style, 1);
      }
      if (!sdlType->attributes.empty()) {
        for (auto const& iter : sdlType->attributes) {
          sdlAttributePtr attr = iter.second;
          if (!attr->name.empty()) {
            auto const rattr = get_property(data, attr->name.c_str());
            if (rattr.m_type != KindOfUninit) {
              xmlNodePtr dummy = master_to_xml(attr->encode, tvAsCVarRef(rattr),
                                               SOAP_LITERAL, xmlParam);
              if (dummy->children && dummy->children->content) {
                if (!attr->fixed.empty() &&
                    attr->fixed != (char*)dummy->children->content) {
                  throw SoapException("Encoding: Attribute '%s' has fixed "
                                      "value '%s' (value '%s' is not allowed)",
                                      attr->name.c_str(), attr->fixed.c_str(),
                                      dummy->children->content);
                }
                /* we need to handle xml: namespace specially, since it is
                   an implicit schema. Otherwise, use form. */
                if (!strncmp(attr->namens.c_str(), XML_NAMESPACE,
                             sizeof(XML_NAMESPACE)) ||
                    attr->form == XSD_FORM_QUALIFIED) {
                  xmlNsPtr nsp = encode_add_ns(xmlParam, attr->namens.c_str());
                  xmlSetNsProp(xmlParam, nsp, BAD_CAST(attr->name.c_str()),
                               dummy->children->content);
                } else {
                  xmlSetProp(xmlParam, BAD_CAST(attr->name.c_str()),
                             dummy->children->content);
                }
              }
              xmlUnlinkNode(dummy);
              xmlFreeNode(dummy);
            }
          }
        }
      }
    }
    if (style == SOAP_ENCODED) {
      set_ns_and_type(xmlParam, type);
    }
  } else {
    xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
    xmlAddChild(parent, xmlParam);

    if (soap_check_zval_ref(data, xmlParam)) {
      return xmlParam;
    }
    if (!prop.isNull()) {
      for (ArrayIter iter(prop); iter; ++iter) {
        Variant key = iter.first();
        Variant zprop = iter.second();
        xmlNodePtr property = master_to_xml(
          get_conversion(dataTypeToSoap(zprop.getType())),
          zprop, style, xmlParam);
        if (key.isString()) {
          OptString skey = key.toString();
          const char *prop_name;
          if (data.isObject()) {
            const char *class_name;
            zend_unmangle_property_name(skey.data(), skey.size(),
                                        &class_name, &prop_name);
          } else {
            prop_name = skey.data();
          }
          if (prop_name) {
            xmlNodeSetName(property, BAD_CAST(prop_name));
          }
        }
      }
    }
    if (style == SOAP_ENCODED) {
      set_ns_and_type(xmlParam, type);
    }
  }
  return xmlParam;
}

/* Array encode/decode */
static xmlNodePtr guess_array_map(encodeType* /*type*/, const Variant& data,
                                  int style, xmlNodePtr parent) {
  encodePtr enc;

  if (data.isArray()) {
    if (!data.toArray()->isVectorData()) {
      enc = get_conversion(APACHE_MAP);
    } else {
      enc = get_conversion(SOAP_ENC_ARRAY);
    }
  }
  if (!enc) {
    enc = get_conversion(dataTypeToSoap(KindOfNull));
  }

  return master_to_xml(enc, data, style, parent);
}

static int calc_dimension_12(const char* str) {
  int i = 0, flag = 0;
  while (*str != '\0' && (*str < '0' || *str > '9') && (*str != '*')) {
    str++;
  }
  if (*str == '*') {
    i++;
    str++;
  }
  while (*str != '\0') {
    if (*str >= '0' && *str <= '9') {
      if (flag == 0) {
         i++;
         flag = 1;
       }
    } else if (*str == '*') {
      throw SoapException("Encoding: '*' may only be first arraySize value"
                          " in list");
    } else {
      flag = 0;
    }
    str++;
  }
  return i < 1 ? 1 : i;
}

static int* get_position_12(int dimension, const char* str) {
  int *pos;
  int i = -1, flag = 0;

  pos = (int*)req::calloc_noptrs(dimension, sizeof(int));
  while (*str != '\0' && (*str < '0' || *str > '9') && (*str != '*')) {
    str++;
  }
  if (*str == '*') {
    str++;
    i++;
  }
  while (*str != '\0') {
    if (*str >= '0' && *str <= '9') {
      if (flag == 0) {
        i++;
        flag = 1;
      }
      pos[i] = (pos[i]*10)+(*str-'0');
    } else if (*str == '*') {
      throw SoapException("Encoding: '*' may only be first arraySize value in list");
    } else {
      flag = 0;
    }
    str++;
  }
  return pos;
}

static int calc_dimension(const char* str) {
  int i = 1;
  while (*str != ']' && *str != '\0') {
    if (*str == ',') {
        i++;
    }
    str++;
  }
  return i;
}

static void get_position_ex(int dimension, const char* str, int** pos) {
  int i = 0;

  memset(*pos,0,sizeof(int)*dimension);
  while (*str != ']' && *str != '\0' && i < dimension) {
    if (*str >= '0' && *str <= '9') {
      (*pos)[i] = ((*pos)[i]*10)+(*str-'0');
    } else if (*str == ',') {
      i++;
    }
    str++;
  }
}

static int* get_position(int dimension, const char* str) {
  int *pos = (int*)req::malloc_noptrs(sizeof(int) * dimension);
  get_position_ex(dimension, str, &pos);
  return pos;
}

static void add_xml_array_elements(xmlNodePtr xmlParam,
                                   sdlTypePtr type,
                                   encodePtr enc,
                                   xmlNsPtr ns,
                                   int dimension ,
                                   int* dims,
                                   const Variant& data,
                                   int style) {
  if (data.isArray()) {
    Array arr = data.toArray();
    ArrayIter iter(arr);
    for (int j=0; j<dims[0]; j++) {
      Variant zdata = iter.second(); ++iter;
      if (dimension == 1) {
        xmlNodePtr xparam;
        if (!zdata.isNull()) {
          if (enc == nullptr) {
            xparam = master_to_xml(
              get_conversion(dataTypeToSoap(zdata.getType())), zdata,
              style, xmlParam
            );
          } else {
            xparam = master_to_xml(enc, zdata, style, xmlParam);
          }
        } else {
          xparam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
          xmlAddChild(xmlParam, xparam);
        }

        if (type) {
          xmlNodeSetName(xparam, BAD_CAST(type->name.c_str()));
        } else if (style == SOAP_LITERAL && enc &&
                   !enc->details.type_str.empty()) {
          xmlNodeSetName(xparam, BAD_CAST(enc->details.type_str.c_str()));
          xmlSetNs(xparam, ns);
        } else {
          xmlNodeSetName(xparam, BAD_CAST("item"));
        }
      } else {
        if (!zdata.isNull()) {
          add_xml_array_elements(xmlParam, type, enc, ns, dimension-1, dims+1,
                                 zdata, style);
        } else {
          add_xml_array_elements(xmlParam, type, enc, ns, dimension-1, dims+1,
                                 uninit_null(), style);
        }
      }
    }
  } else {
    for (int j=0; j<dims[0]; j++) {
      if (dimension == 1) {
        xmlNodePtr xparam;

        xparam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
        xmlAddChild(xmlParam, xparam);
        if (type) {
          xmlNodeSetName(xparam, BAD_CAST(type->name.c_str()));
        } else if (style == SOAP_LITERAL && enc &&
                   !enc->details.type_str.empty()) {
          xmlNodeSetName(xparam, BAD_CAST(enc->details.type_str.c_str()));
          xmlSetNs(xparam, ns);
        } else {
          xmlNodeSetName(xparam, BAD_CAST("item"));
        }
      } else {
        add_xml_array_elements(xmlParam, type, enc, ns, dimension-1, dims+1,
                               uninit_null(), style);
      }
    }
  }
}

static std::shared_ptr<sdlExtraAttribute>
get_extra_attributes(sdlType *sdl_type,
                     const char *type,
                     const char *wsdl_type) {
  if (sdl_type) {
    sdlAttributeMap::const_iterator iterAttributes =
      sdl_type->attributes.find(type);
    if (iterAttributes != sdl_type->attributes.end()) {
      sdlExtraAttributeMap::const_iterator iterExtraAttributes =
        iterAttributes->second->extraAttributes.find(wsdl_type);
      if (iterExtraAttributes !=
          iterAttributes->second->extraAttributes.end()) {
        return iterExtraAttributes->second;
      }
    }
  }
  return std::shared_ptr<sdlExtraAttribute>();
}

static
xmlNodePtr to_xml_array(encodeType* type, const Variant& data_, int style,
                        xmlNodePtr parent) {
  USE_SOAP_GLOBAL;
  sdlType *sdl_type = type->sdl_type;
  sdlTypePtr element_type;
  std::string array_type, array_size;
  int i;
  xmlNodePtr xmlParam;
  encodePtr enc;
  int dimension = 1;
  int* dims = nullptr;
  int soap_version;
  Variant array_copy;
  Variant data = data_;

  soap_version = SOAP_GLOBAL(soap_version);

  xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, xmlParam);

  if (data.isNull()) {
    if (style == SOAP_ENCODED) {
      set_xsi_nil(xmlParam);
      if (SOAP_GLOBAL(features) & SOAP_USE_XSI_ARRAY_TYPE) {
        set_ns_and_type_ex(xmlParam, (soap_version == SOAP_1_1) ?
                           SOAP_1_1_ENC_NAMESPACE : SOAP_1_2_ENC_NAMESPACE,
                           "Array");
      } else {
        set_ns_and_type(xmlParam, type);
      }
    }
    return xmlParam;
  }

  if (data.isObject() &&
      data.toObject().instanceof(SystemLib::getHH_IteratorClass())) {
    array_copy = Array::CreateDict();
    for (ArrayIter iter(data.toObject().get()); iter; ++iter) {
      if (!iter.first().isNull() && iter.first().isString()) {
        array_copy.asArrRef().set(iter.first(), iter.second());
      } else {
        array_copy.asArrRef().append(iter.second());
      }
    }
    data = array_copy;
  }

  if (data.isArray()) {
    std::shared_ptr<sdlExtraAttribute> ext;
    sdlTypeMap::const_iterator iterElements;
    sdlAttributeMap::const_iterator iterAttributes;
    sdlExtraAttributeMap::const_iterator iterExtraAttributes;
    sdlTypePtr elementType;

    i = data.toArray().size();

    if ((ext = get_extra_attributes(sdl_type,
                                    SOAP_1_1_ENC_NAMESPACE":arrayType",
                                    WSDL_NAMESPACE":arrayType"))) {
      OptString value(ext->val);
      char *end = const_cast<char*>(strrchr(value.data(), '['));
      if (end) {
        *end = '\0';
        end++;
        dimension = calc_dimension(end);
      }
      if (!ext->ns.empty()) {
        enc = get_encoder(SOAP_GLOBAL(sdl), ext->ns.c_str(), value.data());
        get_type_str(xmlParam, ext->ns.c_str(), value.data(), array_type);
      } else {
        array_type += value.data();
      }

      dims = (int*)req::malloc_noptrs(sizeof(int) * dimension);
      dims[0] = i;
      Array el = data.toArray();
      for (i = 1; i < dimension; i++) {
        if (!el.empty()) {
          ArrayIter iter(el);
          Variant tmp = iter.second();
          if (tmp.isArray()) {
            dims[i] = tmp.toArray().size();
          } else {
            dims[i] = 0;
          }
        }
      }

      array_size += folly::to<string>(dims[0]);
      for (i=1; i<dimension; i++) {
        array_size += ',';
        array_size += folly::to<string>(dims[i]);
      }

    } else if ((ext = get_extra_attributes
                (sdl_type,
                 SOAP_1_2_ENC_NAMESPACE":itemType",
                 WSDL_NAMESPACE":itemType"))) {
      if (!ext->ns.empty()) {
        enc = get_encoder(SOAP_GLOBAL(sdl), ext->ns.c_str(), ext->val.c_str());
        get_type_str(xmlParam, ext->ns.c_str(), ext->val.c_str(), array_type);
      } else {
        array_type += ext->val;
      }
      if ((iterAttributes =
           sdl_type->attributes.find(SOAP_1_2_ENC_NAMESPACE":arraySize")) !=
          sdl_type->attributes.end()) {
        if ((iterExtraAttributes =
             iterAttributes->second->extraAttributes.find
             (WSDL_NAMESPACE":arraySize")) !=
            iterAttributes->second->extraAttributes.end()) {
          auto ext = iterExtraAttributes->second;
          dimension = calc_dimension_12(ext->val.c_str());
          dims = get_position_12(dimension, ext->val.c_str());
          if (dims[0] == 0) {dims[0] = i;}

          array_size += folly::to<string>(dims[0]);
          for (i=1; i<dimension; i++) {
            array_size += ',';
            array_size += folly::to<string>(dims[i]);
          }
        }
      } else {
        dims = (int*)req::malloc_noptrs(sizeof(int));
        *dims = 0;
        array_size += folly::to<string>(i);
      }
    } else if ((ext = get_extra_attributes
                (sdl_type,
                 SOAP_1_2_ENC_NAMESPACE":arraySize",
                 WSDL_NAMESPACE":arraySize"))) {
      dimension = calc_dimension_12(ext->val.c_str());
      dims = get_position_12(dimension, ext->val.c_str());
      if (dims[0] == 0) {dims[0] = i;}

      array_size += folly::to<string>(dims[0]);
      for (i=1; i<dimension; i++) {
        array_size += ',';
        array_size += folly::to<string>(dims[i]);
      }

      if (sdl_type && sdl_type->elements.size() == 1 &&
          (elementType = sdl_type->elements[0]) != nullptr &&
          elementType->encode &&
          !elementType->encode->details.type_str.empty()) {
        elementType = sdl_type->elements[0];
        element_type = elementType;
        enc = elementType->encode;
        get_type_str(xmlParam, elementType->encode->details.ns.c_str(),
                     elementType->encode->details.type_str.c_str(),
                     array_type);
      } else {
        enc = get_array_type(xmlParam, data, array_type);
      }
    } else if
        (sdl_type && sdl_type->elements.size() == 1 &&
         (elementType = sdl_type->elements[0]) != nullptr &&
         elementType->encode &&
         !elementType->encode->details.type_str.empty()) {
      element_type = elementType;
      enc = elementType->encode;
      get_type_str(xmlParam, elementType->encode->details.ns.c_str(),
                   elementType->encode->details.type_str.c_str(), array_type);

      array_size += folly::to<string>(i);
      dims = (int*)req::malloc_noptrs(sizeof(int) * dimension);
      dims[0] = i;
    } else {

      enc = get_array_type(xmlParam, data, array_type);
      array_size += folly::to<string>(i);
      dims = (int*)req::malloc_noptrs(sizeof(int) * dimension);
      dims[0] = i;
    }

    if (style == SOAP_ENCODED) {
      if (soap_version == SOAP_1_1) {
        if (array_type == "xsd:anyType") {
          array_type = "xsd:ur-type";
        }
        array_type += '[';
        array_type += array_size;
        array_type += ']';
        set_ns_prop(xmlParam, SOAP_1_1_ENC_NAMESPACE, "arrayType",
                    array_type.c_str());
      } else {
        string replaced;
        replaced.reserve(array_size.size());
        for (int i2 = 0; i2 < (int)array_size.size(); i2++) {
          char ch = array_size[i2];
          if (ch == ',') {
            replaced += ' ';
          } else {
            replaced += ch;
          }
        }
        set_ns_prop(xmlParam, SOAP_1_2_ENC_NAMESPACE, "itemType",
                    array_type.c_str());
        set_ns_prop(xmlParam, SOAP_1_2_ENC_NAMESPACE, "arraySize",
                    replaced.c_str());
      }
    }
    add_xml_array_elements(xmlParam, element_type, enc,
                           enc ? encode_add_ns(xmlParam,
                                             enc->details.ns.c_str()) : nullptr,
                           dimension, dims, data, style);
    req::free(dims);
  }
  if (style == SOAP_ENCODED) {
    if (SOAP_GLOBAL(features) & SOAP_USE_XSI_ARRAY_TYPE) {
      set_ns_and_type_ex
        (xmlParam, (soap_version == SOAP_1_1) ?
         SOAP_1_1_ENC_NAMESPACE : SOAP_1_2_ENC_NAMESPACE, "Array");
    } else {
      set_ns_and_type(xmlParam, type);
    }
  }

  return xmlParam;
}

static Variant to_zval_array(encodeType* type, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  Variant ret;
  xmlNodePtr trav;
  encodePtr enc;
  int dimension = 1;
  int* dims = nullptr;
  int* pos = nullptr;
  xmlAttrPtr attr;
  sdl *sdl;
  sdlAttributePtr arrayType;
  std::shared_ptr<sdlExtraAttribute> ext;
  sdlTypePtr elementType;

  FIND_XML_NULL(data, ret);
  sdl = SOAP_GLOBAL(sdl);

  if (data &&
      (attr = get_attribute(data->properties,"arrayType")) &&
      attr->children && attr->children->content) {
    xmlNsPtr nsptr;
    string type, ns;
    parse_namespace(attr->children->content, type, ns);
    nsptr = xmlSearchNs(attr->doc, attr->parent, NS_STRING(ns));

    OptString stype(type);
    char *end = const_cast<char*>(strrchr(stype.data(), '['));
    if (end) {
      *end = '\0';
      dimension = calc_dimension(end+1);
      dims = get_position(dimension, end+1);
    }
    if (nsptr != nullptr) {
      enc = get_encoder(sdl, (char*)nsptr->href, stype.data());
    }

  } else if ((attr = get_attribute(data->properties,"itemType")) &&
      attr->children &&
      attr->children->content) {
    string type, ns;
    parse_namespace(attr->children->content, type, ns);
    xmlNsPtr nsptr;
    nsptr = xmlSearchNs(attr->doc, attr->parent, NS_STRING(ns));
    if (nsptr != nullptr) {
      enc = get_encoder(sdl, (char*)nsptr->href, type.data());
    }

    if ((attr = get_attribute(data->properties,"arraySize")) &&
        attr->children && attr->children->content) {
      dimension = calc_dimension_12((char*)attr->children->content);
      dims = get_position_12(dimension, (char*)attr->children->content);
    } else {
      dims = (int*)req::malloc_noptrs(sizeof(int));
      *dims = 0;
    }

  } else if ((attr = get_attribute(data->properties,"arraySize")) &&
             attr->children && attr->children->content) {

    dimension = calc_dimension_12((char*)attr->children->content);
    dims = get_position_12(dimension, (char*)attr->children->content);

  } else if ((ext = get_extra_attributes
              (type->sdl_type,
               SOAP_1_1_ENC_NAMESPACE":arrayType",
               WSDL_NAMESPACE":arrayType"))) {
    OptString type(ext->val);
    char *end = const_cast<char*>(strrchr(type.data(), '['));
    if (end) {
      *end = '\0';
    }
    if (!ext->ns.empty()) {
      enc = get_encoder(sdl, ext->ns.c_str(), type.data());
    }

    dims = (int*)req::malloc_noptrs(sizeof(int));
    *dims = 0;
  } else if ((ext = get_extra_attributes
              (type->sdl_type,
               SOAP_1_2_ENC_NAMESPACE":itemType",
               WSDL_NAMESPACE":itemType"))) {
    if (!ext->ns.empty()) {
      enc = get_encoder(sdl, ext->ns.c_str(), ext->val.c_str());
    }

    if ((ext = get_extra_attributes
         (type->sdl_type,
          SOAP_1_2_ENC_NAMESPACE":arraySize",
          WSDL_NAMESPACE":arraySize"))) {
      dimension = calc_dimension_12(ext->val.c_str());
      dims = get_position_12(dimension, ext->val.c_str());
    } else {
      dims = (int*)req::malloc_noptrs(sizeof(int));
      *dims = 0;
    }
  } else if ((ext = get_extra_attributes
              (type->sdl_type,
               SOAP_1_2_ENC_NAMESPACE":arraySize",
               WSDL_NAMESPACE":arraySize"))) {
    dimension = calc_dimension_12(ext->val.c_str());
    dims = get_position_12(dimension, ext->val.c_str());
    if (type->sdl_type && type->sdl_type->elements.size() == 1 &&
        (elementType = type->sdl_type->elements[0]) != nullptr &&
        elementType->encode) {
      enc = elementType->encode;
    }
  } else if (type->sdl_type && type->sdl_type->elements.size() == 1 &&
             (elementType = type->sdl_type->elements[0]) != nullptr &&
             elementType->encode) {
    enc = elementType->encode;
  }
  if (dims == nullptr) {
    dimension = 1;
    dims = (int*)req::malloc_noptrs(sizeof(int));
    *dims = 0;
  }
  pos = (int*)req::calloc_noptrs(sizeof(int), dimension);
  if (data && (attr = get_attribute(data->properties,"offset")) &&
      attr->children && attr->children->content) {
    char* tmp = strrchr((char*)attr->children->content,'[');
    if (tmp == nullptr) {
      tmp = (char*)attr->children->content;
    }
    get_position_ex(dimension, tmp, &pos);
  }

  ret = Array::CreateDict();
  trav = data->children;
  while (trav) {
    if (trav->type == XML_ELEMENT_NODE) {
      int i;
      xmlAttrPtr position = get_attribute(trav->properties,"position");

      Variant tmpVal = master_to_zval(enc, trav);
      if (position != nullptr && position->children &&
          position->children->content) {
        char* tmp = strrchr((char*)position->children->content, '[');
        if (tmp == nullptr) {
          tmp = (char*)position->children->content;
        }
        get_position_ex(dimension, tmp, &pos);
      }

      /* Get/Create intermediate arrays for multidimensional arrays */
      i = 0;
      tv_lval ar = ret.asTypedValue();
      while (i < dimension-1) {
        auto& arr = asArrRef(ar);
        if (!arr.exists(pos[i])) {
          arr.set(pos[i], Array::CreateDict());
        }
        ar = arr.lval(pos[i]);
        i++;
      }
      asArrRef(ar).set(pos[i], tmpVal);

      /* Increment position */
      i = dimension;
      while (i > 0) {
        i--;
        pos[i]++;
        if (pos[i] >= dims[i]) {
          if (i > 0) {
            pos[i] = 0;
          } else {
            /* TODO: Array index overflow */
          }
        } else {
          break;
        }
      }
    }
    trav = trav->next;
  }
  req::free(dims);
  req::free(pos);
  return ret;
}

/* Map encode/decode */
static xmlNodePtr to_xml_map(encodeType* type, const Variant& data, int style,
                             xmlNodePtr parent) {
  xmlNodePtr xmlParam;
  int i;

  xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, xmlParam);
  FIND_ZVAL_NULL(data, xmlParam, style);

  if (data.isArray()) {
    Array arr = data.toArray();
    i = arr.size();
    ArrayIter iter(arr);
    for (;i > 0;i--) {
      xmlNodePtr xparam, item;
      xmlNodePtr key;
      Variant temp_key = iter.first();
      Variant temp_data = iter.second();
      ++iter;

      item = xmlNewNode(nullptr, BAD_CAST("item"));
      xmlAddChild(xmlParam, item);
      key = xmlNewNode(nullptr, BAD_CAST("key"));
      xmlAddChild(item,key);
      if (temp_key.isString()) {
        if (style == SOAP_ENCODED) {
          set_xsi_type(key, "xsd:string");
        }
        xmlNodeSetContent(key, BAD_CAST(temp_key.toString().data()));
      } else {
        if (style == SOAP_ENCODED) {
          set_xsi_type(key, "xsd:int");
        }
        OptString skey = temp_key.toString();
        xmlNodeSetContentLen(key, BAD_CAST(skey.data()), skey.size());
      }

      xparam = master_to_xml(
        get_conversion(dataTypeToSoap(temp_data.getType())),
        temp_data, style, item
      );
      xmlNodeSetName(xparam, BAD_CAST("value"));
    }
  }
  if (style == SOAP_ENCODED) {
    set_ns_and_type(xmlParam, type);
  }

  return xmlParam;
}

static Variant to_zval_map(encodeType* /*type*/, xmlNodePtr data) {
  Variant key, value;
  Array ret;
  xmlNodePtr trav, item, xmlKey, xmlValue;

  FIND_XML_NULL(data, ret);

  if (data && data->children) {
    trav = data->children;

    FOREACHNODE(trav, "item", item) {
      xmlKey = get_node(item->children, "key");
      if (!xmlKey) {
        throw SoapException( "Encoding: Can't decode map, missing key");
      }

      xmlValue = get_node(item->children, "value");
      if (!xmlKey) {
        throw SoapException( "Encoding: Can't decode map, missing value");
      }

      key = master_to_zval(encodePtr(), xmlKey);
      value = master_to_zval(encodePtr(), xmlValue);

      if (key.isString() || key.isInteger()) {
        ret.set(key, value);
      } else {
        throw SoapException( "Encoding: Can't decode map, only Strings or "
                       "Longs are allowd as keys");
      }
    }
    ENDFOREACH(trav);
  }
  return ret;
}

/* Unknown encode/decode */
static xmlNodePtr guess_xml_convert(encodeType* /*type*/, const Variant& data,
                                    int style, xmlNodePtr parent) {
  encodePtr  enc = get_conversion(dataTypeToSoap(data.getType()));
  xmlNodePtr ret = master_to_xml_int(enc, data, style, parent, false);
/*
  if (style == SOAP_LITERAL && SOAP_GLOBAL(sdl)) {
    set_ns_and_type(ret, &enc->details);
  }
*/
  return ret;
}

static Variant guess_zval_convert(encodeType* type, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  encodePtr enc;
  xmlAttrPtr tmpattr;
  xmlChar *type_name = nullptr;
  Variant ret;

  data = check_and_resolve_href(data);

  if (data == nullptr) {
    enc = get_conversion(dataTypeToSoap(KindOfNull));
  } else if (data->properties &&
             get_attribute_ex(data->properties, "nil", XSI_NAMESPACE)) {
    enc = get_conversion(dataTypeToSoap(KindOfNull));
  } else {
    tmpattr = get_attribute_ex(data->properties,"type", XSI_NAMESPACE);
    if (tmpattr != nullptr) {
      type_name = tmpattr->children->content;
      enc = get_encoder_from_prefix(SOAP_GLOBAL(sdl), data,
                                    tmpattr->children->content);
      if (enc && type == &enc->details) {
        enc.reset();
      }
      if (enc) {
        encodePtr tmp = enc;
        while (tmp && tmp->details.sdl_type != nullptr &&
               tmp->details.sdl_type->kind != XSD_TYPEKIND_COMPLEX) {
          if (enc == tmp->details.sdl_type->encode ||
              tmp == tmp->details.sdl_type->encode) {
            enc.reset();
            break;
          }
          tmp = tmp->details.sdl_type->encode;
        }
      }
    }

    if (!enc) {
      /* Didn't have a type, totally guess here */
      /* Logic: has children = IS_OBJECT else IS_STRING */
      xmlNodePtr trav;

      if (get_attribute(data->properties, "arrayType") ||
          get_attribute(data->properties, "itemType") ||
          get_attribute(data->properties, "arraySize")) {
        enc = get_conversion(SOAP_ENC_ARRAY);
      } else {
        enc = get_conversion(XSD_STRING);
        trav = data->children;
        while (trav != nullptr) {
          if (trav->type == XML_ELEMENT_NODE) {
            enc = get_conversion(SOAP_ENC_OBJECT);
            break;
          }
          trav = trav->next;
        }
      }
    }
  }
  ret = master_to_zval_int(enc, data);
  if (SOAP_GLOBAL(sdl) && type_name && enc->details.sdl_type) {
    Object obj{SoapVar::classof()};
    SoapVar::setEncType(obj.get(), enc->details.type);
    SoapVar::setEncValue(obj.get(), ret);

    string ns, cptype;
    parse_namespace(type_name, cptype, ns);
    xmlNsPtr nsptr = xmlSearchNs(data->doc, data, NS_STRING(ns));

    SoapVar::setEncSType(obj.get(), cptype);
    if (nsptr) {
      SoapVar::setEncNS(obj.get(), OptString((char*)nsptr->href, CopyString));
    }
    ret = std::move(obj);
  }
  return ret;
}

/* Time encode/decode */
static xmlNodePtr to_xml_datetime_ex(encodeType* type, const Variant& data,
                                     const char *format, int style,
                                     xmlNodePtr parent) {
  /* logic hacked from ext/standard/datetime.c */
  struct tm *ta, tmbuf;
  time_t timestamp;
  int max_reallocs = 5;
  size_t buf_len=64, real_len;
  char *buf;
  char tzbuf[8];

  xmlNodePtr xmlParam = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, xmlParam);
  FIND_ZVAL_NULL(data, xmlParam, style);

  if (data.isInteger()) {
    timestamp = data.toInt64();
    ta = localtime_r(&timestamp, &tmbuf);
    /*ta = php_gmtime_r(&timestamp, &tmbuf);*/

    buf = (char *)req::malloc_noptrs(buf_len);
    while ((real_len = strftime(buf, buf_len, format, ta)) == buf_len ||
           real_len == 0) {
      buf_len *= 2;
      buf = (char *)req::realloc_noptrs(buf, buf_len);
      if (!--max_reallocs) break;
    }

#ifndef _MSC_VER
    /* Time zone support */
    snprintf(tzbuf, sizeof(tzbuf), "%c%02d:%02d",
             (ta->tm_gmtoff < 0) ? '-' : '+', (int)abs(ta->tm_gmtoff / 3600),
             (int)abs( (ta->tm_gmtoff % 3600) / 60 ));
    if (strcmp(tzbuf,"+00:00") == 0) {
      strcpy(tzbuf,"Z");
      real_len++;
    } else {
      real_len += 6;
    }
#endif

    if (real_len >= buf_len) {
      buf = (char *)req::realloc_noptrs(buf, real_len+1);
    }
    strncat(buf, tzbuf, real_len);

    xmlNodeSetContent(xmlParam, BAD_CAST(buf));
    req::free(buf);
  } else if (data.isString()) {
    OptString sdata = data.toString();
    xmlNodeSetContentLen(xmlParam, BAD_CAST(sdata.data()), sdata.size());
  }

  if (style == SOAP_ENCODED) {
    set_ns_and_type(xmlParam, type);
  }
  return xmlParam;
}

static
xmlNodePtr to_xml_duration(encodeType* type, const Variant& data, int style,
                           xmlNodePtr parent) {
  // TODO: '-'?P([0-9]+Y)?([0-9]+M)?([0-9]+D)?T([0-9]+H)?([0-9]+M)?([0-9]+S)?
  return to_xml_string(type, data, style, parent);
}

static
xmlNodePtr to_xml_datetime(encodeType* type, const Variant& data, int style,
                           xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "%Y-%m-%dT%H:%M:%S", style, parent);
}

static xmlNodePtr to_xml_time(encodeType* type, const Variant& data, int style,
                              xmlNodePtr parent) {
  /* TODO: microsecconds */
  return to_xml_datetime_ex(type, data, "%H:%M:%S", style, parent);
}

static xmlNodePtr to_xml_date(encodeType* type, const Variant& data, int style,
                              xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "%Y-%m-%d", style, parent);
}

static xmlNodePtr to_xml_gyearmonth(encodeType* type, const Variant& data,
                                    int style, xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "%Y-%m", style, parent);
}

static xmlNodePtr to_xml_gyear(encodeType* type, const Variant& data, int style,
                               xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "%Y", style, parent);
}

static
xmlNodePtr to_xml_gmonthday(encodeType* type, const Variant& data, int style,
                            xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "--%m-%d", style, parent);
}

static xmlNodePtr to_xml_gday(encodeType* type, const Variant& data, int style,
                              xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "---%d", style, parent);
}

static
xmlNodePtr to_xml_gmonth(encodeType* type, const Variant& data, int style,
                         xmlNodePtr parent) {
  return to_xml_datetime_ex(type, data, "--%m--", style, parent);
}

static Variant to_zval_list(encodeType* enc, xmlNodePtr data) {
  /*FIXME*/
  return to_zval_stringc(enc, data);
}

static xmlNodePtr to_xml_list(encodeType* enc, const Variant& data, int style,
                              xmlNodePtr parent) {
  encodePtr list_enc;
  if (enc->sdl_type && enc->sdl_type->kind == XSD_TYPEKIND_LIST &&
      !enc->sdl_type->elements.empty()) {
    list_enc = enc->sdl_type->elements[0]->encode;
  }

  xmlNodePtr ret = xmlNewNode(nullptr, BAD_CAST("BOGUS"));
  xmlAddChild(parent, ret);
  FIND_ZVAL_NULL(data, ret, style);
  if (data.isArray()) {
    Variant tmp;
    string list;
    Array ht = data.toArray();
    for (ArrayIter iter(ht); iter; ++iter) {
      xmlNodePtr dummy = master_to_xml(list_enc, tmp, SOAP_LITERAL, ret);
      if (dummy && dummy->children && dummy->children->content) {
        if (!list.empty()) {
          list += ' ';
        }
        list += (char*)dummy->children->content;
      } else {
        throw SoapException("Encoding: Violation of encoding rules");
      }
      xmlUnlinkNode(dummy);
      xmlFreeNode(dummy);
    }
    xmlNodeSetContentLen(ret, BAD_CAST(list.c_str()), list.size());
  } else {
    OptString sdata = data.toString();
    char *str = req::strndup(sdata.data(), sdata.size());
    whiteSpace_collapse(BAD_CAST(str));
    char *start = str;
    char *next;
    string list;
    while (start != nullptr && *start != '\0') {
      xmlNodePtr dummy;
      Variant dummy_zval;

      next = strchr(start,' ');
      if (next != nullptr) {
        *next = '\0';
        next++;
      }
      dummy_zval = OptString(start);
      dummy = master_to_xml(list_enc, dummy_zval, SOAP_LITERAL, ret);
      if (dummy && dummy->children && dummy->children->content) {
        if (!list.empty()) {
          list += ' ';
        }
        list += (char*)dummy->children->content;
      } else {
        throw SoapException("Encoding: Violation of encoding rules");
      }
      xmlUnlinkNode(dummy);
      xmlFreeNode(dummy);

      start = next;
    }
    xmlNodeSetContentLen(ret, BAD_CAST(list.c_str()), list.size());
    req::free(str);
  }
  return ret;
}

static xmlNodePtr to_xml_list1(encodeType* enc, const Variant& data, int style,
                               xmlNodePtr parent) {
  /*FIXME: minLength=1 */
  return to_xml_list(enc,data,style, parent);
}

static Variant to_zval_union(encodeType* enc, xmlNodePtr data) {
  /*FIXME*/
  return to_zval_list(enc, data);
}

static xmlNodePtr to_xml_union(encodeType* enc, const Variant& data, int style,
                               xmlNodePtr parent) {
  /*FIXME*/
  return to_xml_list(enc,data,style, parent);
}

static Variant to_zval_any(encodeType* /*type*/, xmlNodePtr data) {
  USE_SOAP_GLOBAL;
  xmlBufferPtr buf;
  Variant ret;

  if (SOAP_GLOBAL(sdl) && !SOAP_GLOBAL(sdl)->elements.empty() && data->name) {
    string nscat;
    if (data->ns && data->ns->href) {
      nscat += (char*)data->ns->href;
      nscat += ':';
    }
    nscat += (char*)data->name;

    sdlTypeMap::const_iterator iter = SOAP_GLOBAL(sdl)->elements.find(nscat);
    if (iter != SOAP_GLOBAL(sdl)->elements.end() && iter->second->encode) {
      return master_to_zval_int(iter->second->encode, data);
    }
  }

  buf = xmlBufferCreate();
  xmlNodeDump(buf, nullptr, data, 0, 0);
  ret = OptString((char*)xmlBufferContent(buf), CopyString);
  xmlBufferFree(buf);
  return ret;
}

static xmlNodePtr to_xml_any(encodeType* /*type*/, const Variant& data,
                             int style, xmlNodePtr parent) {
  xmlNodePtr ret = nullptr;

  if (data.isArray()) {
    encodePtr enc = get_conversion(XSD_ANYXML);
    Array arr = data.toArray();
    for (ArrayIter iter(arr); iter; ++iter) {
      ret = master_to_xml(enc, iter.second(), style, parent);
      if (ret && ret->name != xmlStringTextNoenc &&
          iter.first().isString()) {
        xmlNodeSetName(ret, BAD_CAST(iter.first().toString().data()));
      }
    }
    return ret;
  }
  OptString sdata = data.toString();
  ret = xmlNewTextLen(BAD_CAST(sdata.data()), sdata.size());
  ret->name = xmlStringTextNoenc;
  ret->parent = parent;
  ret->doc = parent->doc;
  ret->prev = parent->last;
  ret->next = nullptr;
  if (parent->last) {
    parent->last->next = ret;
  } else {
    parent->children = ret;
  }
  parent->last = ret;

  return ret;
}

Variant sdl_guess_convert_zval(encodeType* enc, xmlNodePtr data) {
  sdlType *type;

  type = enc->sdl_type;
  if (type == nullptr) {
    return guess_zval_convert(enc, data);
  }
/*FIXME: restriction support
  if (type && type->restrictions &&
      data &&  data->children && data->children->content) {
    if (type->restrictions->whiteSpace && type->restrictions->whiteSpace->value) {
      if (strcmp(type->restrictions->whiteSpace->value,"replace") == 0) {
        whiteSpace_replace(data->children->content);
      } else if (strcmp(type->restrictions->whiteSpace->value,"collapse") == 0) {
        whiteSpace_collapse(data->children->content);
      }
    }
    if (type->restrictions->enumeration) {
      if (!zend_hash_exists(type->restrictions->enumeration,data->children->content,strlen(data->children->content)+1)) {
        soap_error1(E_WARNING, "Encoding: Restriction: invalid enumeration value \"%s\"", data->children->content);
      }
    }
    if (type->restrictions->minLength &&
        strlen(data->children->content) < type->restrictions->minLength->value) {
      soap_error0(E_WARNING, "Encoding: Restriction: length less than 'minLength'");
    }
    if (type->restrictions->maxLength &&
        strlen(data->children->content) > type->restrictions->maxLength->value) {
      soap_error0(E_WARNING, "Encoding: Restriction: length greater than 'maxLength'");
    }
    if (type->restrictions->length &&
        strlen(data->children->content) != type->restrictions->length->value) {
      soap_error0(E_WARNING, "Encoding: Restriction: length is not equal to 'length'");
    }
  }
*/
  switch (type->kind) {
  case XSD_TYPEKIND_SIMPLE:
    if (type->encode && enc != &type->encode->details) {
      return master_to_zval_int(type->encode, data);
    } else {
      return guess_zval_convert(enc, data);
    }
  case XSD_TYPEKIND_LIST:
    return to_zval_list(enc, data);
  case XSD_TYPEKIND_UNION:
    return to_zval_union(enc, data);
  case XSD_TYPEKIND_COMPLEX:
  case XSD_TYPEKIND_RESTRICTION:
  case XSD_TYPEKIND_EXTENSION:
    if (type->encode && isSoapArrayType(type->encode->details.type)) {
      return to_zval_array(enc, data);
    }
    return to_zval_object(enc, data);
  default:
    throw SoapException("Encoding: Internal Error");
  }
  return guess_zval_convert(enc, data);
}

xmlNodePtr sdl_guess_convert_xml(encodeType* enc, const Variant& data,
                                 int style, xmlNodePtr parent) {
  sdlType *type;
  xmlNodePtr ret = nullptr;

  type = enc->sdl_type;

  if (type == nullptr) {
    ret = guess_xml_convert(enc, data, style, parent);
    if (style == SOAP_ENCODED) {
      set_ns_and_type(ret, enc);
    }
    return ret;
  }
/*FIXME: restriction support
  if (type) {
    if (type->restrictions && Z_TYPE_P(data) == IS_STRING) {
      if (type->restrictions->enumeration) {
        if (!zend_hash_exists(type->restrictions->enumeration,Z_STRVAL_P(data),Z_STRLEN_P(data)+1)) {
          soap_error1(E_WARNING, "Encoding: Restriction: invalid enumeration value \"%s\".", Z_STRVAL_P(data));
        }
      }
      if (type->restrictions->minLength &&
          Z_STRLEN_P(data) < type->restrictions->minLength->value) {
        soap_error0(E_WARNING, "Encoding: Restriction: length less than 'minLength'");
      }
      if (type->restrictions->maxLength &&
          Z_STRLEN_P(data) > type->restrictions->maxLength->value) {
        soap_error0(E_WARNING, "Encoding: Restriction: length greater than 'maxLength'");
      }
      if (type->restrictions->length &&
          Z_STRLEN_P(data) != type->restrictions->length->value) {
        soap_error0(E_WARNING, "Encoding: Restriction: length is not equal to 'length'");
      }
    }
  }
*/
  switch(type->kind) {
  case XSD_TYPEKIND_SIMPLE:
    if (type->encode && enc != &type->encode->details) {
      ret = master_to_xml(type->encode, data, style, parent);
    } else {
      ret = guess_xml_convert(enc, data, style, parent);
    }
    break;
  case XSD_TYPEKIND_LIST:
    ret = to_xml_list(enc, data, style, parent);
    break;
  case XSD_TYPEKIND_UNION:
    ret = to_xml_union(enc, data, style, parent);
    break;
  case XSD_TYPEKIND_COMPLEX:
  case XSD_TYPEKIND_RESTRICTION:
  case XSD_TYPEKIND_EXTENSION:
    if (type->encode && isSoapArrayType(type->encode->details.type)) {
      return to_xml_array(enc, data, style, parent);
    } else {
      return to_xml_object(enc, data, style, parent);
    }
  default:
    throw SoapException("Encoding: Internal Error");
  }
  if (style == SOAP_ENCODED) {
    set_ns_and_type(ret, enc);
  }
  return ret;
}

static xmlNodePtr check_and_resolve_href(xmlNodePtr data) {
  if (data && data->properties) {
    xmlAttrPtr href;

    href = data->properties;
    while (1) {
      href = get_attribute(href, "href");
      if (href == nullptr || href->ns == nullptr) {break;}
      href = href->next;
    }
    if (href) {
      /*  Internal href try and find node */
      if (href->children->content[0] == '#') {
        xmlNodePtr ret = get_node_with_attribute_recursive
          (data->doc->children, nullptr, "id",
           (char*)&href->children->content[1]);
        if (!ret) {
          throw SoapException("Encoding: Unresolved reference '%s'",
                        href->children->content);
        }
        return ret;
      } else {
        /*  TODO: External href....? */
        throw SoapException("Encoding: External reference '%s'",
                      href->children->content);
      }
    }
    /* SOAP 1.2 enc:id enc:ref */
    href = get_attribute_ex(data->properties, "ref", SOAP_1_2_ENC_NAMESPACE);
    if (href) {
      xmlChar* id;
      xmlNodePtr ret;

      if (href->children->content[0] == '#') {
        id = href->children->content+1;
      } else {
        id = href->children->content;
      }
      ret = get_node_with_attribute_recursive_ex
        (data->doc->children, nullptr, nullptr, "id", (char*)id,
         SOAP_1_2_ENC_NAMESPACE);
      if (!ret) {
        throw SoapException("Encoding: Unresolved reference '%s'",
                      href->children->content);
      } else if (ret == data) {
        throw SoapException("Encoding: Violation of id and ref information "
                      "items '%s'", href->children->content);
      }
      return ret;
    }
  }
  return data;
}

static void set_ns_and_type(xmlNodePtr node, encodeType* type) {
  set_ns_and_type_ex(node, type->ns.c_str(), type->type_str.c_str());
}

static void set_ns_and_type_ex(xmlNodePtr node, const char *ns,
                               const char *type) {
  string nstype;
  get_type_str(node, ns, type, nstype);
  set_xsi_type(node, nstype.c_str());
}

static xmlNsPtr xmlSearchNsPrefixByHref(xmlDocPtr doc, xmlNodePtr node,
                                        const xmlChar * href) {
  xmlNsPtr cur;
  xmlNodePtr orig = node;

  while (node) {
    if (node->type == XML_ENTITY_REF_NODE ||
        node->type == XML_ENTITY_NODE ||
        node->type == XML_ENTITY_DECL) {
      return nullptr;
    }
    if (node->type == XML_ELEMENT_NODE) {
      cur = node->nsDef;
      while (cur != nullptr) {
        if (cur->prefix && cur->href && xmlStrEqual(cur->href, href)) {
          if (xmlSearchNs(doc, node, cur->prefix) == cur) {
            return cur;
          }
        }
        cur = cur->next;
      }
      if (orig != node) {
        cur = node->ns;
        if (cur != nullptr) {
          if (cur->prefix && cur->href && xmlStrEqual(cur->href, href)) {
            if (xmlSearchNs(doc, node, cur->prefix) == cur) {
              return cur;
            }
          }
        }
      }
    }
    node = node->parent;
  }
  return nullptr;
}

xmlNsPtr encode_add_ns(xmlNodePtr node, const char* ns) {
  USE_SOAP_GLOBAL;
  xmlNsPtr xmlns;

  if (ns == nullptr || ns[0] == '\0') {
    return nullptr;
  }

  xmlns = xmlSearchNsByHref(node->doc, node, BAD_CAST(ns));
  if (xmlns != nullptr && xmlns->prefix == nullptr) {
    xmlns = xmlSearchNsPrefixByHref(node->doc, node, BAD_CAST(ns));
  }
  if (xmlns == nullptr) {
    std::map<string, string>::const_iterator iter =
      SOAP_GLOBAL(defEncNs).find(ns);
    if (iter != SOAP_GLOBAL(defEncNs).end()) {
      xmlns = xmlNewNs(node->doc->children, BAD_CAST(ns),
                       (xmlChar*)iter->second.c_str());
    } else {
      int num = ++SOAP_GLOBAL(cur_uniq_ns);

      string prefix;
      while (1) {
        prefix = "ns";
        prefix += folly::to<string>(num);
        if (xmlSearchNs(node->doc, node, BAD_CAST(prefix.c_str())) == nullptr) {
          break;
        }
        num = ++SOAP_GLOBAL(cur_uniq_ns);
      }

      xmlns = xmlNewNs(node->doc->children, BAD_CAST(ns),
                       BAD_CAST(prefix.c_str()));
    }
  }
  return xmlns;
}

static void set_ns_prop(xmlNodePtr node, const char *ns, const char *name,
                        const char *val) {
  xmlSetNsProp(node, encode_add_ns(node, ns), BAD_CAST(name), BAD_CAST(val));
}

static void set_xsi_nil(xmlNodePtr node) {
  set_ns_prop(node, XSI_NAMESPACE, "nil", "true");
}

static void set_xsi_type(xmlNodePtr node, const char *type) {
  set_ns_prop(node, XSI_NAMESPACE, "type", type);
}

void encode_reset_ns() {
  USE_SOAP_GLOBAL;
  SOAP_GLOBAL(cur_uniq_ns) = 0;
  SOAP_GLOBAL(cur_uniq_ref) = 0;
  SOAP_GLOBAL(ref_map).clear();
  SOAP_GLOBAL(node_map).clear();
}

void encode_finish() {
  USE_SOAP_GLOBAL;
  SOAP_GLOBAL(cur_uniq_ns) = 0;
  SOAP_GLOBAL(cur_uniq_ref) = 0;
  SOAP_GLOBAL(ref_map).clear();
  SOAP_GLOBAL(node_map).clear();
}

encodePtr get_conversion(int encode) {
  USE_SOAP_GLOBAL;
  std::map<int, encodePtr>::const_iterator iter =
    SOAP_GLOBAL(defEncIndex).find(encode);
  if (iter != SOAP_GLOBAL(defEncIndex).end()) {
    return iter->second;
  }
  throw SoapException( "Encoding: Cannot find encoding");
}

static encodePtr get_array_type(xmlNodePtr node, const Variant& array,
                                string &type) {
  USE_SOAP_GLOBAL;
  int i, count, cur_type, prev_type;
  bool different;
  const char *prev_stype = nullptr, *cur_stype = nullptr, *prev_ns = nullptr,
    *cur_ns = nullptr;

  if (!array.isArray()) {
    type += "xsd:anyType";
    return get_conversion(XSD_ANYTYPE);
  }
  Array ht = array.toArray();

  different = false;
  cur_type = prev_type = 0;
  count = ht.size();

  ArrayIter iter(ht);
  for (i = 0;i < count;i++) {
    Variant tmp = iter.second();
    if (tmp.isObject() && tmp.toObject().instanceof(SoapVar::classof())) {
      auto svobj = tmp.toObject().get();
      cur_type = SoapVar::getEncType(svobj);
      auto enc_stype = SoapVar::getEncSType(svobj);
      if (!enc_stype.empty()) {
        cur_stype = enc_stype.c_str();
      } else {
        cur_stype = nullptr;
      }
      auto enc_ns = SoapVar::getEncNS(svobj);
      if (!enc_ns.empty()) {
        cur_ns = enc_ns.c_str();
      } else {
        cur_ns = nullptr;
      }
    } else if (tmp.isArray() && !tmp.toArray()->isVectorData()) {
      cur_type = APACHE_MAP;
      cur_stype = nullptr;
      cur_ns = nullptr;
    } else {
      cur_type = dataTypeToSoap(tmp.getType());
      cur_stype = nullptr;
      cur_ns = nullptr;
    }

    if (i > 0) {
      if ((cur_type != prev_type) ||
          (cur_stype && prev_stype &&
           strcmp(cur_stype,prev_stype) != 0) ||
          (!cur_stype && cur_stype != prev_stype) ||
          (cur_ns && prev_ns && strcmp(cur_ns,prev_ns)) ||
          (!cur_ns && cur_ns != prev_ns)) {
        different = true;
        break;
      }
    }

    prev_type = cur_type;
    prev_stype = cur_stype;
    prev_ns = cur_ns;
    ++iter;
  }

  if (different || count == 0) {
    type += "xsd:anyType";
    return get_conversion(XSD_ANYTYPE);
  }

  encodePtr enc;
  if (cur_stype) {
     string array_type;

     if (cur_ns) {
       xmlNsPtr ns = encode_add_ns(node,cur_ns);

       type += (char*)ns->prefix;
       type += ':';
       array_type += cur_ns;
       array_type += ':';
     }
     type += cur_stype;
     array_type += cur_stype;

     enc = get_encoder_ex(SOAP_GLOBAL(sdl), array_type);
   } else {
     enc = get_conversion(cur_type);
     get_type_str(node, enc->details.ns.c_str(),
                  enc->details.type_str.c_str(), type);
  }
  return enc;
}

static void get_type_str(xmlNodePtr node, const char* ns, const char* type,
                         string &ret) {
  USE_SOAP_GLOBAL;
  if (ns) {
    xmlNsPtr xmlns;
    if (SOAP_GLOBAL(soap_version) == SOAP_1_2 &&
        strcmp(ns,SOAP_1_1_ENC_NAMESPACE) == 0) {
      ns = SOAP_1_2_ENC_NAMESPACE;
    } else if (SOAP_GLOBAL(soap_version) == SOAP_1_1 &&
               strcmp(ns,SOAP_1_2_ENC_NAMESPACE) == 0) {
      ns = SOAP_1_1_ENC_NAMESPACE;
    }
    xmlns = encode_add_ns(node,ns);
    ret += (char*)xmlns->prefix;
    ret += ':';
  }
  ret += type;
}

///////////////////////////////////////////////////////////////////////////////
}
