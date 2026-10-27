#include "hphp/runtime/ext/icu/ext_icu_collator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/zend-collator.h"
#include "hphp/runtime/base/zend-qsort.h"

namespace HPHP::Intl {
/////////////////////////////////////////////////////////////////////////////
// class Collator

enum CollatorSort {
  SORT_REGULAR = 0,
  SORT_STRING = 1,
  SORT_NUMERIC = 2,
};

const StaticString s_Collator("Collator");

#define FETCH_COL(dest, src, ret) \
  auto dest = Collator::Get(src); \
  if (!dest) { \
    raise_recoverable_error("Collator not initialized"); \
    return ret; \
  }

static void HHVM_METHOD(Collator, __construct, const OptString& locale) {
  auto data = Native::data<Collator>(this_);
  data->clearError();
  if (!locale.empty()) {
    UErrorCode error = U_ZERO_ERROR;
    data->setCollator(ucol_open(locale.c_str(), &error));
    if (U_SUCCESS(error)) {
      return;
    }
    /* Fallthrough and use default collator */
  }
  data->setError(U_USING_FALLBACK_WARNING);
  UErrorCode error = U_ZERO_ERROR;
  data->setCollator(ucol_open(uloc_getDefault(), &error));
  if (U_FAILURE(error)) {
    data->setError(error, "collator_create: unable to open ICU collator");
    data->setCollator(nullptr);
    return;
  }
}

static bool HHVM_METHOD(Collator, asort, Variant& arr, int64_t flag) {
  FETCH_COL(data, this_, false);
  if (!arr.isArray()) {
    raise_expected_array_warning("Collator::asort");
    return false;
  }
  data->clearError();
  bool ret = collator_asort(arr, flag, true, data->collator(), data);
  if (U_FAILURE(data->getErrorCode())) {
    return false;
  }
  return ret;
}

static Variant HHVM_METHOD(Collator, compare,
                           const Variant& str1, const Variant& str2) {
  FETCH_COL(data, this_, false);
  data->clearError();
  UErrorCode error = U_ZERO_ERROR;
  icu::UnicodeString ustr1(u16(str1.toString(), error));
  if (U_FAILURE(error)) {
    data->setError(error);
    return false;
  }
  error = U_ZERO_ERROR;
  icu::UnicodeString ustr2(u16(str2.toString(), error));
  if (U_FAILURE(error)) {
    data->setError(error);
    return false;
  }
  return (int64_t)ucol_strcoll(data->collator(),
                               ustr1.getBuffer(), ustr1.length(),
                               ustr2.getBuffer(), ustr2.length());
}

static int64_t HHVM_METHOD(Collator, getAttribute, int64_t attr) {
  FETCH_COL(data, this_, 0);
  data->clearError();
  UErrorCode error = U_ZERO_ERROR;
  int64_t ret = (int64_t)ucol_getAttribute(data->collator(),
                                           (UColAttribute)attr,
                                           &error);
  if (U_FAILURE(error)) {
    data->setError(error, "Error getting attribute value");
    return 0;
  }
  return ret;
}

static int64_t HHVM_METHOD(Collator, getErrorCode) {
  FETCH_COL(data, this_, 0);
  return data->getErrorCode();
}

static OptString HHVM_METHOD(Collator, getErrorMessage) {
  FETCH_COL(data, this_, "");
  return data->getErrorMessage();
}

static OptString HHVM_METHOD(Collator, getLocale, int64_t type) {
  FETCH_COL(data, this_, "");
  data->clearError();
  UErrorCode error = U_ZERO_ERROR;
  auto loc = ucol_getLocaleByType(data->collator(), (ULocDataLocaleType)type,
                                  &error);
  if (U_FAILURE(error)) {
    data->setError(error, "Error getting locale by type");
  }
  return OptString(loc, CopyString);
}

static int64_t HHVM_METHOD(Collator, getStrength) {
  FETCH_COL(data, this_, false);
  return ucol_getStrength(data->collator());
}

static bool HHVM_METHOD(Collator, setAttribute, int64_t attr, int64_t val) {
  FETCH_COL(data, this_, false);
  data->clearError();
  UErrorCode error = U_ZERO_ERROR;
  ucol_setAttribute(data->collator(), (UColAttribute)attr,
                                      (UColAttributeValue)val, &error);
  if (U_FAILURE(error)) {
    data->setError(error, "Error setting attribute value");
    return false;
  }
  return true;
}

static Variant HHVM_METHOD(Collator, getSortKey, const OptString& val) {
  FETCH_COL(data, this_, false);
  UErrorCode error = U_ZERO_ERROR;
  icu::UnicodeString strval(u16(val, error));
  if (U_FAILURE(error)) {
    return false;
  }

  int sortkey_len = ucol_getSortKey(data->collator(),
                                    strval.getBuffer(), strval.length(),
                                    nullptr,
                                    0);
  if (sortkey_len <= 0) {
    return false;
  }

  OptString ret(sortkey_len, ReserveString);
  sortkey_len = ucol_getSortKey(data->collator(),
                                strval.getBuffer(), strval.length(),
                                (uint8_t*) ret.get()->mutableData(),
                                ret.capacity());
  if (sortkey_len <= 0) {
    return false;
  }

  ret.setSize(sortkey_len -1);
  return ret;
}

static bool HHVM_METHOD(Collator, setStrength, int64_t strength) {
  FETCH_COL(data, this_, false);
  ucol_setStrength(data->collator(), (UCollationStrength)strength);
  return true;
}

struct collator_sort_key_index_t {
  char* key;       /* pointer to sort key */
  ssize_t valPos;  /* position of the original array element */
};

/* Bytes to reserve for sort keys */
static const int32_t DEF_SORT_KEYS_BUF_SIZE = 1048576;
static const int32_t DEF_SORT_KEYS_BUF_INCREMENT = 1048576;

/* Number of keys position to allocate */
static const int32_t DEF_SORT_KEYS_INDX_BUF_SIZE = 512;
static const int32_t DEF_SORT_KEYS_INDX_BUF_INCREMENT = 64;

static int collator_cmp_sort_keys(const void* p1, const void* p2, const void*) {
  char* key1 = ((collator_sort_key_index_t*)p1)->key;
  char* key2 = ((collator_sort_key_index_t*)p2)->key;
  return strcmp( key1, key2 );
}

static bool HHVM_METHOD(Collator, sortWithSortKeys, Variant& arr) {
  FETCH_COL(data, this_, false);
  data->clearError();

  if (!arr.isArray()) {
    return true;
  }

  Array hash = arr.toArray();
  if (hash.size() == 0) {
    return true;
  }

  // Preallocate sort keys buffer
  size_t sortKeysOffset = 0;
  size_t sortKeysLength = DEF_SORT_KEYS_BUF_SIZE;
  char*  sortKeys = (char*)req::malloc_noptrs(sortKeysLength);

  if (!sortKeys) {
    throw Exception("Out of memory");
  }
  SCOPE_EXIT{ req::free(sortKeys); };

  // Preallocate index buffer
  size_t sortIndexPos = 0;
  size_t sortIndexLength = DEF_SORT_KEYS_INDX_BUF_SIZE;
  auto   sortIndex =
    req::make_raw_array<collator_sort_key_index_t>(sortIndexLength);
  if (!sortIndex) {
    throw Exception("Out of memory");
  }
  SCOPE_EXIT{ req::destroy_raw_array(sortIndex, sortIndexLength); };

  // Translate input hash to sortable index
  auto pos_limit = hash->iter_end();
  for (ssize_t pos = hash->iter_begin(); pos != pos_limit;
       pos = hash->iter_advance(pos)) {
    Variant val(hash->getValue(pos));

    // Convert to UTF16
    icu::UnicodeString strval;
    if (val.isString()) {
      UErrorCode error = U_ZERO_ERROR;
      strval = u16(val.toString(), error);
      if (U_FAILURE(error)) {
        return false;
      }
     }

    // Generate sort key
    int sortkey_len =
      ucol_getSortKey(data->collator(),
                      strval.getBuffer(), strval.length(),
                      (uint8_t*)(sortKeys + sortKeysOffset),
                      sortKeysLength - sortKeysOffset);

    // Check for key buffer overflow
    if (sortkey_len > (sortKeysLength - sortKeysOffset)) {
      int32_t inc = (sortkey_len > DEF_SORT_KEYS_BUF_INCREMENT)
                  ?  sortkey_len : DEF_SORT_KEYS_BUF_INCREMENT;
      sortKeysLength += inc;
      sortKeys = (char*)req::realloc_noptrs(sortKeys, sortKeysLength);
      if (!sortKeys) {
        throw Exception("Out of memory");
      }
      sortkey_len =
        ucol_getSortKey(data->collator(),
                        strval.getBuffer(), strval.length(),
                        (uint8_t*)(sortKeys + sortKeysOffset),
                        sortKeysLength - sortKeysOffset);
      assertx(sortkey_len <= (sortKeysLength - sortKeysOffset));
    }

    // Check for index buffer overflow
    if ((sortIndexPos + 1) > sortIndexLength) {
      sortIndexLength += DEF_SORT_KEYS_INDX_BUF_INCREMENT;
      sortIndex = static_cast<collator_sort_key_index_t*>(req::realloc(
        sortIndex,
        sortIndexLength * sizeof(collator_sort_key_index_t),
        type_scan::getIndexForMalloc<collator_sort_key_index_t>()
      ));
      if (!sortIndex) {
        throw Exception("Out of memory");
      }
    }

    // Initially store offset into buffer, update later to deal with reallocs
    sortIndex[sortIndexPos].key = (char*)sortKeysOffset;
    sortKeysOffset += sortkey_len;

    sortIndex[sortIndexPos].valPos = pos;
    ++sortIndexPos;
  }

  // Update keys to location in realloc'd buffer
  for (int i = 0; i < sortIndexPos; ++i) {
    sortIndex[i].key = sortKeys + (ptrdiff_t)sortIndex[i].key;
  }

  zend_qsort(sortIndex, sortIndexPos,
             sizeof(collator_sort_key_index_t),
             collator_cmp_sort_keys, nullptr);

  Array ret = Array::CreateVec();
  for (int i = 0; i < sortIndexPos; ++i) {
    ret.append(hash->getValue(sortIndex[i].valPos));
  }
  arr = ret;
  return true;
}

static bool HHVM_METHOD(Collator, sort, Variant& arr,
                        int64_t sort_flag /* = Collator::SORT_REGULAR */) {
  FETCH_COL(data, this_, false);
  if (!arr.isArray()) {
    raise_expected_array_warning("Collator::sort");
    return false;
  }
  data->clearError();
  bool ret = collator_sort(arr, sort_flag, true, data->collator(), data);
  if (U_FAILURE(data->getErrorCode())) {
    return false;
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////

void IntlExtension::registerNativeCollator() {
  HHVM_ME(Collator, __construct);
  HHVM_ME(Collator, asort);
  HHVM_ME(Collator, compare);
  HHVM_ME(Collator, getAttribute);
  HHVM_ME(Collator, getErrorCode);
  HHVM_ME(Collator, getErrorMessage);
  HHVM_ME(Collator, getLocale);
  HHVM_ME(Collator, getSortKey);
  HHVM_ME(Collator, getStrength);
  HHVM_ME(Collator, setAttribute);
  HHVM_ME(Collator, setStrength);
  HHVM_ME(Collator, sortWithSortKeys);
  HHVM_ME(Collator, sort);

  HHVM_RCC_INT(Collator, SORT_REGULAR, SORT_REGULAR);
  HHVM_RCC_INT(Collator, SORT_STRING, SORT_STRING);
  HHVM_RCC_INT(Collator, SORT_NUMERIC, SORT_NUMERIC);

  HHVM_RCC_INT(Collator, FRENCH_COLLATION, UCOL_FRENCH_COLLATION);
  HHVM_RCC_INT(Collator, ALTERNATE_HANDLING, UCOL_ALTERNATE_HANDLING);
  HHVM_RCC_INT(Collator, CASE_FIRST, UCOL_CASE_FIRST);
  HHVM_RCC_INT(Collator, CASE_LEVEL, UCOL_CASE_LEVEL);
  HHVM_RCC_INT(Collator, NORMALIZATION_MODE, UCOL_NORMALIZATION_MODE);
  HHVM_RCC_INT(Collator, STRENGTH, UCOL_STRENGTH);
  HHVM_RCC_INT(Collator, HIRAGANA_QUATERNARY_MODE,
               UCOL_HIRAGANA_QUATERNARY_MODE);
  HHVM_RCC_INT(Collator, NUMERIC_COLLATION, UCOL_NUMERIC_COLLATION);
  HHVM_RCC_INT(Collator, PRIMARY, UCOL_PRIMARY);
  HHVM_RCC_INT(Collator, SECONDARY, UCOL_SECONDARY);
  HHVM_RCC_INT(Collator, TERTIARY, UCOL_TERTIARY);
  HHVM_RCC_INT(Collator, DEFAULT_STRENGTH, UCOL_DEFAULT_STRENGTH);
  HHVM_RCC_INT(Collator, QUATERNARY, UCOL_QUATERNARY);
  HHVM_RCC_INT(Collator, IDENTICAL, UCOL_IDENTICAL);
  HHVM_RCC_INT(Collator, OFF, UCOL_OFF);
  HHVM_RCC_INT(Collator, ON, UCOL_ON);
  HHVM_RCC_INT(Collator, SHIFTED, UCOL_SHIFTED);
  HHVM_RCC_INT(Collator, NON_IGNORABLE, UCOL_NON_IGNORABLE);
  HHVM_RCC_INT(Collator, LOWER_FIRST, UCOL_LOWER_FIRST);
  HHVM_RCC_INT(Collator, UPPER_FIRST, UCOL_UPPER_FIRST);

  HHVM_RCC_INT(Collator, DEFAULT_VALUE, UCOL_DEFAULT);

  Native::registerNativeDataInfo<Collator>(s_Collator.get());
}

//////////////////////////////////////////////////////////////////////////////
} // namespace HPHP::Intl
