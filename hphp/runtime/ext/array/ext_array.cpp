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

#include "hphp/runtime/ext/array/ext_array.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-provenance.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/double-to-int64.h"
#include "hphp/runtime/base/request-event-handler.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/sort-flags.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/ext/core/ext_core_closure.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/coeffects.h"
#include "hphp/util/configs/php7.h"
#include "hphp/util/logger.h"
#include "hphp/util/rds-local.h"

#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

#define SORT_DESC               3
#define SORT_ASC                4

const StaticString s_count("count");

enum class CaseMode {
  LOWER = 0,
  UPPER = 1,
};

namespace {

// Create a new array-like with the given type and with enough capacity to
// store `size` elements.
Array makeReserveLike(DataType type, size_t size) {
  auto const ad = [&]{
    switch (type) {
      case KindOfVec:    return VanillaVec::MakeReserveVec(size);
      case KindOfDict:   return VanillaDict::MakeReserveDict(size);
      case KindOfKeyset: return VanillaKeyset::MakeReserveSet(size);
      default:           always_assert(false);
    }
    not_reached();
  }();
  return Array::attach(ad);
}

// Appends all keys and values in the ArrayIter `it` to the array `array`.
// If `array` is a keyset, we preserve int keys; otherwise, we renumber them.
//
// `array` must be the same type of array that `it` iterates over. If `array`
// is a dict or darray, it must have vector-like keys. We use both constraints
// for optimizations in the loops below.
void appendKeysAndVals(Array& array, ArrayIter& it) {
  assertx(array->toDataType() == it.getArrayData()->toDataType());
  if (array.isVec() || array.isKeyset()) {
    for (; it; ++it) {
      array.append(it.secondVal());
    }
  } else {
    assertx(array->isVectorData());
    auto nextKI = safe_cast<int64_t>(array->size());
    for (; it; ++it) {
      auto const key = it.nvFirst();
      if (tvIsInt(key)) {
        array.set(nextKI++, it.secondVal());
      } else {
        auto const str = StrNR(val(key).pstr);
        array.set(str.asString(), it.secondVal(), true);
      }
    }
  }
}

// Pushes the given value into a (non-empty) array and pops the last element,
// allowing us to "cycle" optional arguments for certain variadic functions.
Variant cycleVariadics(Array& array, const Variant& val) {
  assertx(!array.empty());
  auto result = makeReserveLike(array->toDataType(), array.size() + 1);
  result.append(val);
  ArrayIter it(array.detach(), ArrayIter::noInc);
  appendKeysAndVals(result, it);
  array = std::move(result);
  return array.pop();
}

}

TypedValue HHVM_FUNCTION(array_change_key_case,
                         const Variant& input,
                         int64_t case_ /* = 0 */) {
  getCheckedContainer(input);
  return tvReturn(ArrayUtil::ChangeKeyCase(arr_input,
                                           (CaseMode)case_ == CaseMode::LOWER));
}

TypedValue HHVM_FUNCTION(array_chunk,
                         const Variant& input,
                         int64_t chunkSize,
                         bool preserve_keys /* = false */) {
  const auto& cellInput = *input.asTypedValue();
  if (UNLIKELY(!isContainer(cellInput))) {
    raise_warning("Invalid operand type was used: %s expects "
                  "an array or collection as argument 1", __FUNCTION__+2);
    return make_tv<KindOfNull>();
  }

  if (chunkSize < 1) {
    raise_invalid_argument_warning("size: %ld", chunkSize);
    return make_tv<KindOfNull>();
  }

  const size_t inputSize = getContainerSize(cellInput);
  VecInit ret((inputSize + chunkSize - 1) / chunkSize);
  Array chunk;
  int current = 0;
  for (ArrayIter iter(cellInput); iter; ++iter) {
    if (preserve_keys) {
      chunk.set(iter.first(), iter.secondValPlus(), true);
    } else {
      chunk.set(safe_cast<int64_t>(chunk.size()), iter.secondValPlus());
    }
    if ((++current % chunkSize) == 0) {
      ret.append(chunk);
      chunk.clear();
    }
  }
  if (!chunk.empty()) {
    ret.append(chunk);
  }

  return tvReturn(ret.toVariant());
}

static inline bool array_column_coerce_key(Variant &key, const char *name) {
  /* NULL has a special meaning for each field */
  if (key.isNull()) {
    return true;
  }

  /* Custom coercion rules for key types */
  if (key.isInteger() || key.isDouble()) {
    key = key.toInt64();
    return true;
  } else if (key.isString() || key.isObject() || key.isLazyClass() ||
    key.isClass()) {
    key = key.toString();
    return true;
  } else {
    raise_warning("array_column(): The %s key should be either a string "
                  "or an integer", name);
    return false;
  }
}

TypedValue HHVM_FUNCTION(array_column,
                         const Variant& input,
                         const Variant& val_in,
                         const Variant& idx_in /* = uninit_variant */) {
  getCheckedContainer(input);
  Variant val = val_in, idx = idx_in;
  if (!array_column_coerce_key(val, "column") ||
      !array_column_coerce_key(idx, "index")) {
    return make_tv<KindOfBoolean>(false);
  }
  DictInit ret(arr_input.size());
  int64_t nextKI = 0; // for appends
  for (ArrayIter it(arr_input); it; ++it) {
    Array sub;
    if (UNLIKELY(Cfg::PHP7::Builtins && it.second().isObject())) {
      sub = it.second().toObject().get()->toArray<IntishCast::Cast>();
    } else if (it.second().isArray()) {
      sub = it.second().toArray();
    } else {
      continue;
    }

    Variant elem;
    if (val.isNull()) {
      elem = sub;
    } else {
      auto const val_key = sub.convertKey<IntishCast::Cast>(val);
      if (sub.exists(val_key)) {
        elem = sub[val_key];
      } else {
        // skip subarray without named element
        continue;
      }
    }
    if (idx.isNull()) {
      ret.set(nextKI++, elem);
    } else {
      auto const idx_key = sub.convertKey<IntishCast::Cast>(idx);
      if (!sub.exists(idx_key)) {
        // nextKI may wrap around due to overflow if we intish-cast a string
        // key to std::numeric_limits<int64_t>::max() in the case below...
        if (nextKI >= 0) ret.set(nextKI++, elem);
      } else {
        auto const sub_idx = [&]{
          auto const result = sub[idx_key];
          return result.isObject() ? result.toString() : result;
        }();
        auto const converted = sub.convertKey<IntishCast::Cast>(sub_idx);
        if (tvIsInt(converted) && converted.val().num >= nextKI) {
          nextKI = int64_t(size_t(converted.val().num) + 1);
        }
        ret.setUnknownKey<IntishCast::None>(converted, elem);
      }
    }
  }
  return tvReturn(ret.toVariant());
}

TypedValue HHVM_FUNCTION(array_combine,
                         const Variant& keys,
                         const Variant& values) {
  const auto& cell_keys = *keys.asTypedValue();
  const auto& cell_values = *values.asTypedValue();
  if (UNLIKELY(!isContainer(cell_keys) || !isContainer(cell_values))) {
    raise_warning("Invalid operand type was used: array_combine expects "
                  "arrays or collections");
    return make_tv<KindOfNull>();
  }
  auto keys_size = getContainerSize(cell_keys);
  if (UNLIKELY(keys_size != getContainerSize(cell_values))) {
    raise_warning("array_combine(): Both parameters should have an equal "
                  "number of elements");
    return make_tv<KindOfBoolean>(false);
  }
  Array ret = Array::attach(VanillaDict::MakeReserveDict(keys_size));
  for (ArrayIter iter1(cell_keys), iter2(cell_values);
       iter1; ++iter1, ++iter2) {
    auto const key = iter1.secondValPlus();
    if (isIntType(type(key)) || isStringType(type(key))) {
      ret.set(ret.convertKey<IntishCast::Cast>(key), iter2.secondValPlus());
    } else {
      ret.set(ret.convertKey<IntishCast::Cast>(tvCastToString(key)),
                     iter2.secondValPlus());
    }
  }
  return tvReturn(std::move(ret));
}

TypedValue HHVM_FUNCTION(array_count_values,
                         const Variant& input) {
  if (!isContainer(input)) {
    raise_warning("array_count_values() expects parameter 1 to be array, "
                  "%s given",
                  getDataTypeString(input.getType()).c_str());
    return make_tv<KindOfNull>();
  }
  return tvReturn(
    ArrayUtil::CountValues(
      input.isClsMeth() ? input.toArray() : input.isArray() ?
        input.asCArrRef()
        // If this isn't exactly an Array, then it must be a hack collection,
        // so it is safe to get the object data
        : collections::toArray<IntishCast::Cast>(
            input.getObjectData()
          )));
}

Array HHVM_FUNCTION(array_fill_keys,
                    const Variant& keys,
                    const Variant& value) {
  Optional<DictInit> ai;
  auto ok = IterateV(
    *keys.asTypedValue(),
    [&](ArrayData* adata) {
      ai.emplace(adata->size());
    },
    [&](TypedValue v) {
      if (isIntType(v.m_type) || isStringType(v.m_type)) {
        ai->setUnknownKey<IntishCast::Cast>(v, value);
      } else {
        raise_error("keys must be ints or strings");
      }
    },
    [&](ObjectData* coll) {
      if (coll->collectionType() == CollectionType::Pair) {
        ai.emplace(2);
      }
    },
    false
  );

  if (!ok) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Invalid operand type was used: array_fill_keys expects an array or "
      "collection");
  }
  assertx(ai.has_value());
  return ai->toArray();
}

TypedValue HHVM_FUNCTION(array_fill,
                         int64_t start_index,
                         int64_t num,
                         const Variant& value) {
  if (num < 0) {
    raise_invalid_argument_warning("Number of elements can't be negative");
    return make_tv<KindOfBoolean>(false);
  } else if (num == 0) {
    return tvReturn(start_index == 0 ? empty_vec_array() : empty_dict_array());
  }

  if (start_index == 0) {
    VecInit vai(num);
    for (size_t k = 0; k < num; k++) {
      vai.append(value);
    }
    return make_array_like_tv(vai.create());
  } else {
    DictInit ret(num, CheckAllocation{});
    ret.set(start_index, value);
    auto const base = std::max<int64_t>(start_index + 1, 0);
    for (auto i = 1; i < num; i++) {
      ret.set(base + i - 1, value);
    }
    return make_array_like_tv(ret.create());
  }
}

TypedValue HHVM_FUNCTION(array_flip,
                         const Variant& trans) {
  auto const& transCell = *trans.asTypedValue();
  if (UNLIKELY(!isContainer(transCell))) {
    raise_warning("Invalid operand type was used: %s expects "
                  "an array or collection", __FUNCTION__+2);
    return make_tv<KindOfNull>();
  }

  DictInit ret(getContainerSize(transCell));
  for (ArrayIter iter(transCell); iter; ++iter) {
    auto const inner = iter.secondValPlus();
    if (isIntType(type(inner)) || isStringType(type(inner))) {
      ret.setUnknownKey<IntishCast::Cast>(inner, iter.first());
    } else if (isLazyClassType(type(inner)) || isClassType(type(inner))) {
      ret.setValidKey(tvAsCVarRef(tvClassToString(inner)), iter.first());
    } else {
      raise_warning("Can only flip STRING and INTEGER values!");
    }
  }
  return tvReturn(ret.toVariant());
}

bool HHVM_FUNCTION(array_key_exists,
                   const Variant& key,
                   const Variant& search) {
  const ArrayData *ad;

  auto const searchCell = search.asTypedValue();
  if (LIKELY(isArrayLikeType(searchCell->m_type))) {
    ad = searchCell->m_data.parr;
  } else if (searchCell->m_type == KindOfObject) {
    ObjectData* obj = searchCell->m_data.pobj;
    if (obj->isCollection()) {
      return collections::contains(obj, key);
    } else if (obj->instanceof(c_Closure::classof())) {
      return false;
    }
    return HHVM_FN(array_key_exists)(key, obj->toArray(false, true));
  } else {
    raise_bad_type_warning("array_key_exists expects an array or an object; "
                             "false returned.");
    return false;
  }

  auto const cell = key.asTypedValue();

  auto const op = "array_key_exists";
  switch (cell->m_type) {
    case KindOfUninit:
    case KindOfNull:
      throwInvalidArrayKeyException(cell, ad);

    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfBoolean:
    case KindOfDouble:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfEnumClassLabel:
      throwInvalidArrayKeyException(cell, ad);

    case KindOfClass:
      return ad->exists(StrNR(classToStringHelper(cell->m_data.pclass, op)));
    case KindOfLazyClass:
      return ad->exists(StrNR(lazyClassToStringHelper(cell->m_data.plazyclass,
                                                      op)));

    case KindOfString: {
      return ad->exists(StrNR(cell->m_data.pstr));
    }
    case KindOfInt64:
      return ad->exists(cell->m_data.num);
  }
  not_reached();
}

bool HHVM_FUNCTION(key_exists,
                   const Variant& key,
                   const Variant& search) {
  return HHVM_FN(array_key_exists)(key, search);
}

TypedValue HHVM_FUNCTION(array_keys,
                         TypedValue input) {
  if (UNLIKELY(!isContainer(input))) {
    raise_warning("array_keys() expects parameter 1 to be an array "
                  "or collection");
    return make_tv<KindOfNull>();
  }

  VecInit ai(getContainerSize(input));
  IterateKV(input, [&](TypedValue k, TypedValue) {
    ai.append(k);
  });
  return make_array_like_tv(ai.create());
}

namespace {

void php_array_merge_recursive(Array& arr1, const Array& arr2) {
  for (ArrayIter iter(arr2); iter; ++iter) {
    Variant key(iter.first());
    if (key.isNumeric()) {
      arr1.append(iter.secondVal());
    } else if (arr1.exists(key, true)) {
      // There is no need to do toKey() conversion, for a key that is already
      // in the array.
      auto const arrkey =
        arr1.convertKey<IntishCast::Cast>(*key.asTypedValue());
      auto const lval = arr1.lval(arrkey, AccessFlags::Key);
      auto subarr1 = tvCastToArrayLike<IntishCast::Cast>(lval.tv());
      subarr1 = subarr1.toDict();
      auto subarr2 = tvCastToArrayLike<IntishCast::Cast>(iter.secondVal());
      php_array_merge_recursive(subarr1, subarr2);
      tvUnset(lval); // avoid contamination of the value that was strongly bound
      tvSet(make_array_like_tv(subarr1.get()), lval);
    } else {
      arr1.set(key, iter.secondVal(), true);
    }
  }
}

}

TypedValue HHVM_FUNCTION(array_map,
                         const Variant& callback,
                         const Variant& arr1,
                         const Array& _argv) {
  VMRegGuard _;
  CallCtx ctx;
  ctx.func = nullptr;
  if (!callback.isNull()) {
    vm_decode_function(callback, ctx);
  }
  const auto& cell_arr1 = *arr1.asTypedValue();
  if (UNLIKELY(!isContainer(cell_arr1))) {
    raise_warning("array_map(): Argument #2 should be an array or collection");
    return make_tv<KindOfNull>();
  }
  if (LIKELY(_argv.empty())) {
    // Handle the common case where the caller passed two
    // params (a callback and a container)
    if (!ctx.func) {
      if (isArrayLikeType(cell_arr1.m_type)) {
        return tvReturn(arr1);
      } else {
        return tvReturn(arr1.toArray());
      }
    }
    DictInit ret(getContainerSize(cell_arr1));
    bool keyConverted = isArrayLikeType(cell_arr1.m_type);
    if (!keyConverted) {
      auto col_type = cell_arr1.m_data.pobj->collectionType();
      keyConverted = !collectionAllowsIntStringKeys(col_type);
    }
    for (ArrayIter iter(arr1); iter; ++iter) {
      auto const arg = iter.secondValPlus();
      auto result =
        g_context->invokeFuncFew(ctx, 1, nullptr, &arg, RuntimeCoeffects::fixme());
      // if keyConverted is false, it's possible that ret will have fewer
      // elements than cell_arr1; keys int(1) and string('1') may both be
      // present
      ret.setUnknownKey<IntishCast::None>(
        *iter.first().asTypedValue(), Variant::attach(result));
    }
    return tvReturn(ret.toVariant());
  }

  // Handle the uncommon case where the caller passed a callback
  // and two or more containers
  req::vector<ArrayIter> iters;
  iters.reserve(_argv.size() + 1);
  size_t maxLen = getContainerSize(cell_arr1);
  iters.emplace_back(cell_arr1);
  for (ArrayIter it(_argv); it; ++it) {
    auto const c = it.secondValPlus();
    if (UNLIKELY(!isContainer(c))) {
      raise_warning("array_map(): Argument #%d should be an array or "
                    "collection", (int)(iters.size() + 2));
      iters.emplace_back(tvCastToArrayLike(c));
    } else {
      iters.emplace_back(c);
      size_t len = getContainerSize(c);
      if (len > maxLen) maxLen = len;
    }
  }
  VecInit ret_ai(maxLen);
  for (size_t k = 0; k < maxLen; k++) {
    VecInit params_ai(iters.size());
    for (auto& iter : iters) {
      if (iter) {
        params_ai.append(iter.secondValPlus());
        ++iter;
      } else {
        params_ai.append(init_null_variant);
      }
    }
    Array params = params_ai.toArray();
    if (ctx.func) {
      auto result = Variant::attach(
        g_context->invokeFunc(ctx, params, nullptr, RuntimeCoeffects::fixme())
      );
      ret_ai.append(result);
    } else {
      ret_ai.append(params);
    }
  }
  return tvReturn(ret_ai.toVariant());
}

TypedValue HHVM_FUNCTION(array_merge_recursive,
                         const Variant& array1,
                         const Array& arrays /* = null array */) {
  getCheckedArray(array1);
  auto ret = Array::CreateDict();
  php_array_merge_recursive(ret, arr_array1);

  bool success = true;
  IterateV(
    arrays.get(),
    [&](TypedValue v) -> bool {
      if (!tvIsArrayLike(v)) {
        raise_expected_array_warning("array_merge_recursive");
        success = false;
        return true;
      }

      php_array_merge_recursive(ret, asCArrRef(&v));
      return false;
    }
  );

  if (UNLIKELY(!success)) {
    return make_tv<KindOfNull>();
  }
  return tvReturn(std::move(ret));
}

static void php_array_replace(Array &arr1, const Array& arr2) {
  for (ArrayIter iter(arr2); iter; ++iter) {
    Variant key = iter.first();
    arr1.set(key, iter.secondVal(), true);
  }
}

static void php_array_replace_recursive(Array &arr1, const Array& arr2) {
  if (arr1.get() == arr2.get()) {
    // This is an optimization; if the arrays are self recursive, this does
    // change the behavior slightly - it skips the "recursion detected" warning.
    return;
  }

  for (ArrayIter iter(arr2); iter; ++iter) {
    const auto key =
      Variant::wrap(arr1.convertKey<IntishCast::Cast>(iter.first()));
    auto const tv = iter.secondVal();
    if (arr1.exists(key, true) && isArrayLikeType(type(tv))) {
      auto const lval = arr1.lval(key, AccessFlags::Key);
      if (isArrayLikeType(lval.type())) {
        Array subarr1 = tvCastToArrayLike<IntishCast::Cast>(
          lval.tv()
        ).toDictIntishCast();
        php_array_replace_recursive(subarr1, ArrNR(val(tv).parr));
        tvSet(make_array_like_tv(subarr1.get()), lval);
      } else {
        arr1.set(key, iter.secondVal(), true);
      }
    } else {
      arr1.set(key, iter.secondVal(), true);
    }
  }
}

TypedValue HHVM_FUNCTION(array_replace,
                         const Variant& array1,
                         const Variant& array2 /* = uninit_variant */,
                         const Array& args /* = null array */) {
  getCheckedArray(array1);
  Array ret = Array::CreateDict();
  php_array_replace(ret, arr_array1);

  if (UNLIKELY(array2.isNull() && args.empty())) {
    return tvReturn(std::move(ret));
  }

  getCheckedArray(array2);
  php_array_replace(ret, arr_array2);

  for (ArrayIter iter(args); iter; ++iter) {
    auto const v = VarNR(iter.secondVal());
    getCheckedArray(v);
    php_array_replace(ret, arr_v);
  }
  return tvReturn(std::move(ret));
}

TypedValue HHVM_FUNCTION(array_replace_recursive,
                         const Variant& array1,
                         const Variant& array2 /* = uninit_variant */,
                         const Array& args /* = null array */) {
  getCheckedArray(array1);
  Array ret = Array::CreateDict();
  php_array_replace_recursive(ret, arr_array1);

  if (UNLIKELY(array2.isNull() && args.empty())) {
    return tvReturn(std::move(ret));
  }

  getCheckedArray(array2);
  php_array_replace_recursive(ret, arr_array2);

  for (ArrayIter iter(args); iter; ++iter) {
    auto const v = VarNR(iter.secondVal());
    getCheckedArray(v);
    php_array_replace_recursive(ret, arr_v);
  }
  return tvReturn(std::move(ret));
}

TypedValue HHVM_FUNCTION(array_pad,
                         const Variant& input,
                         int64_t pad_size,
                         const Variant& pad_value) {
  getCheckedArray(input);
  auto arr = arr_input.isKeyset() ? arr_input.toDict() : arr_input;
  assertx(arr->isVecType() || arr->isDictType());
  if (pad_size > 0) {
    return tvReturn(ArrayUtil::PadRight(arr, pad_value, pad_size));
  } else {
    return tvReturn(ArrayUtil::PadLeft(arr, pad_value, -pad_size));
  }
}

TypedValue HHVM_FUNCTION(array_pop,
                         Variant& containerRef) {
  auto* container = containerRef.asTypedValue();
  if (UNLIKELY(!isMutableContainer(*container))) {
    raise_warning("array_pop() expects parameter 1 to be an "
                  "array or mutable collection");
    return make_tv<KindOfNull>();
  }
  if (!getContainerSize(*container)) {
    return make_tv<KindOfNull>();
  }
  if (isArrayLikeType(container->m_type)) {
    return tvReturn(containerRef.asArrRef().pop());
  }
  assertx(container->m_type == KindOfObject);
  return tvReturn(collections::pop(container->m_data.pobj));
}

TypedValue HHVM_FUNCTION(array_product,
                         const Variant& input) {
  if (UNLIKELY(!isContainer(input))) {
    raise_warning("Invalid operand type was used: %s expects "
                  "an array or collection as argument 1",
                  __FUNCTION__+2);
    return make_tv<KindOfNull>();
  }

  int64_t i = 1;
  ArrayIter iter(input);
  for (; iter; ++iter) {
    auto const tv = iter.secondValPlus();

    switch (type(tv)) {
      case KindOfUninit:
      case KindOfNull:
      case KindOfBoolean:
      case KindOfInt64:
        i *= tvToInt(tv);
        continue;

      case KindOfDouble:
        goto DOUBLE;

      case KindOfString: {
        int64_t ti;
        double td;
        if (val(tv).pstr->isNumericWithVal(ti, td, 1) == KindOfInt64) {
          i *= ti;
          continue;
        } else {
          goto DOUBLE;
        }
      }

      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
      case KindOfFunc:
      case KindOfClass:
      case KindOfLazyClass:
      case KindOfClsMeth:
      case KindOfRClsMeth:
      case KindOfEnumClassLabel:
        continue;
    }
    not_reached();
  }
  return make_tv<KindOfInt64>(i);

DOUBLE:
  double d = i;
  for (; iter; ++iter) {
    auto const tv = iter.secondValPlus();
    if (tvIsClsMeth(tv)) continue;
    switch (type(tv)) {
      DT_UNCOUNTED_CASE:
      case KindOfString:
        d *= tvCastToDouble(tv);
        continue;

      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfRClsMeth:
      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
        continue;
    }
    not_reached();
  }
  return make_tv<KindOfDouble>(d);
}

TypedValue HHVM_FUNCTION(array_push,
                         Variant& container,
                         const Variant& var,
                         const Array& args /* = null array */) {
  if (LIKELY(container.isArray())) {
    /*
     * Important note: this *must* cast the parr in the inner cell to
     * the Array&---we can't copy it to the stack or anything because we
     * might escalate.
     */
    Array& arr_array = container.asArrRef();
    arr_array.append(var);
    for (ArrayIter iter(args); iter; ++iter) {
      arr_array.append(iter.second());
    }
    return make_tv<KindOfInt64>(arr_array.size());
  }

  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      switch (obj->collectionType()) {
        case CollectionType::Vector: {
          c_Vector* vec = static_cast<c_Vector*>(obj);
          vec->reserve(vec->size() + args.size() + 1);
          vec->add(var);
          for (ArrayIter iter(args); iter; ++iter) {
            vec->add(iter.second());
          }
          return make_tv<KindOfInt64>(vec->size());
        }
        case CollectionType::Set: {
          c_Set* set = static_cast<c_Set*>(obj);
          set->reserve(set->size() + args.size() + 1);
          set->add(var);
          for (ArrayIter iter(args); iter; ++iter) {
            set->add(iter.second());
          }
          return make_tv<KindOfInt64>(set->size());
        }
        case CollectionType::Map:
        case CollectionType::Pair:
        case CollectionType::ImmVector:
        case CollectionType::ImmMap:
        case CollectionType::ImmSet:
          // other collection types are unsupported:
          //  - mapping collections require a key
          //  - immutable collections don't allow insertion
          break;
      }
    }
  }
  raise_expected_array_or_collection_warning("array_push");
  return make_tv<KindOfNull>();
}

TypedValue HHVM_FUNCTION(array_rand,
                         const Variant& input,
                         int64_t num_req /* = 1 */) {
  getCheckedArray(input);
  return tvReturn(ArrayUtil::RandomKeys(arr_input, num_req));
}

TypedValue HHVM_FUNCTION(array_reverse,
                         const Variant& input,
                         bool preserve_keys /* = false */) {

  getCheckedContainer(input);
  return tvReturn(ArrayUtil::Reverse(arr_input, preserve_keys));
}

TypedValue HHVM_FUNCTION(array_shift,
                         Variant& array) {
  auto* cell_array = array.asTypedValue();
  if (UNLIKELY(!isMutableContainer(*cell_array))) {
    raise_warning("array_shift() expects parameter 1 to be an "
                  "array or mutable collection");
    return make_tv<KindOfNull>();
  }
  if (!getContainerSize(*cell_array)) {
    return make_tv<KindOfNull>();
  }
  if (isArrayLikeType(cell_array->m_type)) {
    auto& arr_array = array.asArrRef();
    assertx(!arr_array.empty());
    auto newArray  = makeReserveLike(cell_array->m_type, arr_array.size() - 1);
    if (arr_array->isLegacyArray()) newArray->setLegacyArrayInPlace(true);
    ArrayIter it(arr_array.detach(), ArrayIter::noInc);
    auto const result = it.second();
    ++it;
    appendKeysAndVals(newArray, it);
    arr_array = std::move(newArray);
    return tvReturn(result);
  }
  assertx(cell_array->m_type == KindOfObject);
  return tvReturn(collections::shift(cell_array->m_data.pobj));
}

TypedValue HHVM_FUNCTION(array_slice,
                         TypedValue cell_input,
                         int64_t offset,
                         const Variant& length /* = uninit_variant */,
                         bool preserve_keys /* = false */) {
  if (UNLIKELY(!isContainer(cell_input))) {
    raise_warning("Invalid operand type was used: %s expects "
                  "an array or collection as argument 1",
                  __FUNCTION__+2);
    return make_tv<KindOfNull>();
  }
  int64_t len = length.isNull() ? 0x7FFFFFFF : length.toInt64();

  const int64_t num_in = getContainerSize(cell_input);
  if (offset > num_in) {
    offset = num_in;
  } else if (offset < 0 && (offset = (num_in + offset)) < 0) {
    offset = 0;
  }

  auto const maxLen = num_in - offset;
  if (len < 0) {
    len = maxLen + len;
  } else if (len > maxLen) {
    len = maxLen;
  }

  if (len <= 0) {
    return make_array_like_tv(staticEmptyDictArray());
  }

  bool input_is_packed = [&] {
    if (isClsMethType(cell_input.m_type)) {
      return true;
    } else if (isArrayLikeType(cell_input.m_type)) {
      return tvIsVec(cell_input);
    }

    assertx(cell_input.m_type == KindOfObject);
    assertx(cell_input.m_data.pobj->isCollection());

    return isVectorCollection(cell_input.m_data.pobj->collectionType());
  }();


  // If the slice covers the entirety of a packed input container, we can cast
  // the whole thing into a varray and return it immediately.
  if (offset == 0 && len == num_in && input_is_packed) {
    if (tvIsArrayLike(cell_input)) {
      return tvReturn(ArrNR{val(cell_input).parr}.asArray().toVec());
    }
    return tvReturn(val(cell_input).pobj->toArray());
  }

  int pos = 0;
  ArrayIter iter(cell_input);
  for (; pos < offset && iter; ++pos, ++iter) {}

  if (input_is_packed && (offset == 0 || !preserve_keys)) {
    VecInit ret(len);
    for (; pos < (offset + len) && iter; ++pos, ++iter) {
      ret.append(iter.secondValPlus());
    }
    return tvReturn(ret.toVariant());
  }

  // Otherwise VecInit can't be used because non-numeric keys are
  // preserved even when preserve_keys is false
  Array ret = Array::attach(VanillaDict::MakeReserveDict(len));
  auto nextKI = 0; // for appends
  for (; pos < (offset + len) && iter; ++pos, ++iter) {
    auto const key = iter.first();
    if (!preserve_keys && key.isInteger()) {
      ret.set(nextKI++, iter.secondValPlus());
    } else {
      ret.set(key, iter.secondValPlus(), true);
    }
  }
  return tvReturn(std::move(ret));
}

Variant array_splice(Variant& input, int64_t offset,
                     const Variant& length, const Variant& replacement) {
  getCheckedArrayVariant(input);
  Array ret = Array::CreateDict();
  int64_t len = length.isNull() ? 0x7FFFFFFF : length.toInt64();
  input = ArrayUtil::Splice(arr_input, offset, len, replacement, ret);
  return ret;
}

TypedValue HHVM_FUNCTION(array_splice,
                         Variant& input,
                         int64_t offset,
                         const Variant& length,
                         const Variant& replacement) {
  return tvReturn(array_splice(input, offset, length, replacement));
}

TypedValue HHVM_FUNCTION(array_sum,
                         const Variant& input) {
  if (UNLIKELY(!isContainer(input))) {
    raise_warning("Invalid operand type was used: %s expects "
                  "an array or collection as argument 1",
                  __FUNCTION__+2);
    return make_tv<KindOfNull>();
  }

  int64_t i = 0;
  ArrayIter iter(input);
  for (; iter; ++iter) {
    auto const tv = iter.secondValPlus();

    switch (type(tv)) {
      case KindOfUninit:
      case KindOfNull:
      case KindOfBoolean:
      case KindOfInt64:
        i += tvToInt(tv);
        continue;

      case KindOfDouble:
        goto DOUBLE;

      case KindOfString: {
        int64_t ti;
        double td;
        if (val(tv).pstr->isNumericWithVal(ti, td, 1) == KindOfInt64) {
          i += ti;
          continue;
        } else {
          goto DOUBLE;
        }
      }

      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
      case KindOfFunc:
      case KindOfClass:
      case KindOfLazyClass:
      case KindOfClsMeth:
      case KindOfRClsMeth:
      case KindOfEnumClassLabel:
        continue;
    }
    not_reached();
  }
  return make_tv<KindOfInt64>(i);

DOUBLE:
  double d = i;
  for (; iter; ++iter) {
    auto const tv = iter.secondValPlus();
    if (tvIsClsMeth(tv)) continue;
    switch (type(tv)) {
      DT_UNCOUNTED_CASE:
      case KindOfString:
        d += tvCastToDouble(tv);
        continue;

      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfRClsMeth:
      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
        continue;
    }
    not_reached();
  }
  return make_tv<KindOfDouble>(d);
}

TypedValue HHVM_FUNCTION(array_unshift,
                         Variant& array,
                         const Variant& var,
                         const Array& args /* = null array */) {
  auto* cell_array = array.asTypedValue();
  if (UNLIKELY(!isContainer(*cell_array))) {
    raise_warning("%s() expects parameter 1 to be an array, Vector, or Set",
                  __FUNCTION__+2 /* remove the "f_" prefix */);
    return make_tv<KindOfNull>();
  }
  auto const dt = type(cell_array);
  if (isArrayLikeType(dt)) {
    auto const ad = val(cell_array).parr;
    auto const size = ad->size() + args.size() + 1;
    auto newArray = makeReserveLike(dt, size);
    if (ad->isLegacyArray()) newArray->setLegacyArrayInPlace(true);
    if (isDictType(dt)) {
      int64_t i = 0;
      newArray.set(i++, var);
      if (!args.empty()) {
        IterateV(args.get(), [&](auto val) { newArray.set(i++, val); });
      }
    } else {
      newArray.append(var);
      if (!args.empty()) {
        IterateV(args.get(), [&](auto val) { newArray.append(val); });
      }
    }
    Array& arr_array = array.asArrRef();
    ArrayIter it(arr_array.detach(), ArrayIter::noInc);
    appendKeysAndVals(newArray, it);
    arr_array = std::move(newArray);
    return make_tv<KindOfInt64>(arr_array.size());
  }
  // Handle collections
  assertx(cell_array->m_type == KindOfObject);
  auto* obj = cell_array->m_data.pobj;
  assertx(obj->isCollection());
  switch (obj->collectionType()) {
    case CollectionType::Vector: {
      auto* vec = static_cast<c_Vector*>(obj);
      if (!args.empty()) {
        auto pos_limit = args->iter_end();
        for (ssize_t pos = args->iter_last(); pos != pos_limit;
             pos = args->iter_rewind(pos)) {
          vec->addFront(args->nvGetVal(pos));
        }
      }
      vec->addFront(*var.asTypedValue());
      return make_tv<KindOfInt64>(vec->size());
    }
    case CollectionType::Set: {
      auto* st = static_cast<c_Set*>(obj);
      if (!args.empty()) {
        auto pos_limit = args->iter_end();
        for (ssize_t pos = args->iter_last(); pos != pos_limit;
             pos = args->iter_rewind(pos)) {
          st->addFront(args->nvGetVal(pos));
        }
      }
      st->addFront(*var.asTypedValue());
      return make_tv<KindOfInt64>(st->size());
    }
    case CollectionType::Map:
    case CollectionType::Pair:
    case CollectionType::ImmVector:
    case CollectionType::ImmMap:
    case CollectionType::ImmSet:
      break;
  }
  raise_warning("%s() expects parameter 1 to be an array, Vector, or Set",
                __FUNCTION__+2 /* remove the "f_" prefix */);
  return make_tv<KindOfNull>();
}

TypedValue HHVM_FUNCTION(array_values,
                         const Variant& input) {
  if (input.isArray() || input.isClsMeth()) {
    return tvReturn(input.toVec());
  }

  Optional<VecInit> ai;
  auto ok = IterateV(*input.asTypedValue(),
                     [&](ArrayData* adata) {
                       ai.emplace(adata->size());
                     },
                     [&](TypedValue v) {
                       ai->append(v);
                     },
                     [&](ObjectData* coll) {
                       if (coll->collectionType() == CollectionType::Pair) {
                         ai.emplace(2);
                       }
                     },
                     false);

  if (!ok) {
    raise_warning("array_values() expects parameter 1 to be an array "
                  "or collection");
    return make_tv<KindOfNull>();
  }

  assertx(ai.has_value());
  return tvReturn(ai->toArray());
}

static int php_count_recursive(const Array& array) {
  long cnt = array.size();
  for (ArrayIter iter(array); iter; ++iter) {
    Variant value = iter.second();
    if (value.isArray()) {
      const Array& arr_value = value.asCArrRef();
      check_recursion_throw();
      cnt += php_count_recursive(arr_value);
    }
  }
  return cnt;
}

bool HHVM_FUNCTION(shuffle, Variant& array) {
  if (!array.isArray()) {
    raise_expected_array_warning("shuffle");
    return false;
  }

  auto const ad = array.asCArrRef().get();
  if (ad->empty()) return true;

  auto const legacy = ad->isLegacyArray();
  array = ArrayUtil::Shuffle(array.asCArrRef());
  if (legacy) array.asArrRef()->setLegacyArrayInPlace(true);
  return true;
}

enum class CountMode {
  NORMAL = 0,
  RECURSIVE = 1,
};

int64_t HHVM_FUNCTION(count,
                      const Variant& var,
                      int64_t mode /* = 0 */) {
  switch (var.getType()) {
    case KindOfUninit:
    case KindOfNull:
      return 0;

    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return 1;

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
      if ((CountMode)mode == CountMode::RECURSIVE) {
        const Array& arr_var = var.asCArrRef();
        return php_count_recursive(arr_var);
      }
      return var.getArrayData()->size();

    case KindOfClsMeth:
      return 1;
    case KindOfObject:
      {
        Object obj = var.toObject();
        if (obj->isCollection()) {
          return collections::getSize(obj.get());
        }
        if (obj.instanceof(SystemLib::getCountableClass())) {
          return obj->o_invoke_few_args(s_count, RuntimeCoeffects::fixme(), 0).toInt64();
        }
      }
      return 1;
  }
  not_reached();
}

int64_t HHVM_FUNCTION(sizeof,
                      const Variant& var) {
  return HHVM_FN(count)(var, 0);
}

bool HHVM_FUNCTION(in_array,
                   const Variant& needle,
                   const Variant& haystack,
                   bool strict /* = false */) {
  bool ret = false;
  auto ok = strict ?
    IterateV(*haystack.asTypedValue(),
             [&](TypedValue v) -> bool {
               if (tvSame(v, *needle.asTypedValue())) {
                 ret = true;
                 return true;
               }
               return false;
             }) :
    IterateV(*haystack.asTypedValue(),
             [&](TypedValue v) -> bool {
               if (tvEqual(v, *needle.asTypedValue())) {
                 ret = true;
                 return true;
               }
               return false;
             });

  if (UNLIKELY(!ok)) {
    raise_warning("in_array() expects parameter 2 to be an array "
                  "or collection");
  }
  return ret;
}

Variant array_search(const Variant& needle,
                     const Variant& haystack,
                     bool strict /* = false */) {
  Variant ret = false;
  auto ok = strict ?
    IterateKV(*haystack.asTypedValue(),
              [&](TypedValue k, TypedValue v) -> bool {
                if (tvSame(v, *needle.asTypedValue())) {
                  ret = VarNR(k);
                  return true;
                }
                return false;
              }) :
    IterateKV(*haystack.asTypedValue(),
              [&](TypedValue k, TypedValue v) -> bool {
                if (tvEqual(v, *needle.asTypedValue())) {
                  ret = VarNR(k);
                  return true;
                }
                return false;
              });

  if (UNLIKELY(!ok)) {
    raise_warning("array_search() expects parameter 2 to be an array "
                  "or collection");
    return init_null();
  }

  return ret;
}

TypedValue HHVM_FUNCTION(array_search,
                         const Variant& needle,
                         const Variant& haystack,
                         bool strict /* = false */) {
  return tvReturn(array_search(needle, haystack, strict));
}

TypedValue HHVM_FUNCTION(range,
                         const Variant& low,
                         const Variant& high,
                         const Variant& step /* = 1 */) {
  bool is_step_double = false;
  double dstep = 1.0;
  if (step.isDouble()) {
    dstep = step.toDouble();
    is_step_double = true;
  } else if (step.isString()) {
    int64_t sn;
    double sd;
    DataType stype = step.toString().get()->isNumericWithVal(sn, sd, 0);
    if (stype == KindOfDouble) {
      is_step_double = true;
      dstep = sd;
    } else if (stype == KindOfInt64) {
      dstep = (double)sn;
    } else {
      dstep = step.toDouble();
    }
  } else {
    dstep = step.toDouble();
  }
  /* We only want positive step values. */
  if (dstep < 0.0) dstep *= -1;
  if (low.isString() && high.isString()) {
    OptString slow = low.toString();
    OptString shigh = high.toString();
    if (slow.size() >= 1 && shigh.size() >=1) {
      int64_t n1, n2;
      double d1, d2;
      DataType type1 = slow.get()->isNumericWithVal(n1, d1, 0);
      DataType type2 = shigh.get()->isNumericWithVal(n2, d2, 0);
      if (type1 == KindOfDouble || type2 == KindOfDouble || is_step_double) {
        if (type1 != KindOfDouble) d1 = slow.toDouble();
        if (type2 != KindOfDouble) d2 = shigh.toDouble();
        return tvReturn(ArrayUtil::Range(d1, d2, dstep));
      }

      int64_t lstep = double_to_int64(dstep);
      if (type1 == KindOfInt64 || type2 == KindOfInt64) {
        if (type1 != KindOfInt64) n1 = slow.toInt64();
        if (type2 != KindOfInt64) n2 = shigh.toInt64();
        return tvReturn(ArrayUtil::Range(n1, n2, lstep));
      }

      return tvReturn(ArrayUtil::Range((unsigned char)slow.charAt(0),
                                       (unsigned char)shigh.charAt(0), lstep));
    }
  }

  if (low.is(KindOfDouble) || high.is(KindOfDouble) || is_step_double) {
    return tvReturn(ArrayUtil::Range(low.toDouble(), high.toDouble(), dstep));
  }

  int64_t lstep = double_to_int64(dstep);
  return tvReturn(ArrayUtil::Range(low.toInt64(), high.toInt64(), lstep));
}
///////////////////////////////////////////////////////////////////////////////
// diff/intersect helpers

static int cmp_func(const Variant& v1, const Variant& v2, const void *data) {
  auto callback = static_cast<const Variant*>(data);
  return (int)vm_call_user_func(*callback, make_vec_array(v1, v2)).toInt64();
}

#define COMMA ,
#define diff_intersect_body(type, vararg, intersect_params)     \
  getCheckedArray(array1);                                      \
  if (!arr_array1.size()) {                                     \
    return tvReturn(empty_dict_array());                             \
  }                                                             \
  CoeffectsAutoGuard _;                                         \
  auto ret = arr_array1.type(array2, intersect_params);         \
  if (ret.size()) {                                             \
    for (ArrayIter iter(vararg); iter; ++iter) {                \
      ret = ret.type(iter.second(), intersect_params);          \
      if (!ret.size()) break;                                   \
    }                                                           \
  }                                                             \
  return tvReturn(std::move(ret));

///////////////////////////////////////////////////////////////////////////////
// diff functions

static inline void addToSetHelper(const req::ptr<c_Set>& st,
                                  const TypedValue c,
                                  TypedValue* strTv,
                                  bool convertIntLikeStrs) {
  if (c.m_type == KindOfInt64) {
    st->add(c.m_data.num);
  } else {
    StringData* s;
    if (LIKELY(isStringType(c.m_type))) {
      s = c.m_data.pstr;
    } else {
      s = tvCastToStringData(c);
      decRefStr(strTv->m_data.pstr);
      strTv->m_data.pstr = s;
    }
    int64_t n;
    if (convertIntLikeStrs && s->isStrictlyInteger(n)) {
      st->add(n);
    } else {
      st->add(s);
    }
  }
}

static inline bool checkSetHelper(const req::ptr<c_Set>& st,
                                  const TypedValue c,
                                  TypedValue* strTv,
                                  bool convertIntLikeStrs) {
  if (c.m_type == KindOfInt64) {
    return st->contains(c.m_data.num);
  }
  StringData* s;
  if (LIKELY(isStringType(c.m_type))) {
    s = c.m_data.pstr;
  } else {
    s = tvCastToStringData(c);
    decRefStr(strTv->m_data.pstr);
    strTv->m_data.pstr = s;
  }
  int64_t n;
  if (convertIntLikeStrs && s->isStrictlyInteger(n)) {
    return st->contains(n);
  }
  return st->contains(s);
}

static void containerValuesToSetHelper(const req::ptr<c_Set>& st,
                                       const Variant& container) {
  Variant strHolder(empty_string_variant());
  TypedValue* strTv = strHolder.asTypedValue();
  for (ArrayIter iter(container); iter; ++iter) {
    auto const c = iter.secondValPlus();
    addToSetHelper(st, c, strTv, true);
  }
}

static inline void raiseIsClsMethWarning(const char* fn, int pos) {
  raise_warning(
   "%s() expects parameter %d to be an array or collection, clsmeth given",
   fn, pos);
}

static inline bool checkIsClsMethAndRaise(
  const char* fn, const Variant& arr1, int idx = 1) {
  if (arr1.isClsMeth()) {
    raiseIsClsMethWarning(fn, idx);
    return true;
  }
  return false;
}

static inline bool checkIsClsMethAndRaise(
  const char* fn, const Variant& arr1, const Variant& arr2) {
  if (checkIsClsMethAndRaise(fn, arr1, 1)) return true;
  if (checkIsClsMethAndRaise(fn, arr2, 2)) return true;
  return false;
}

TypedValue HHVM_FUNCTION(array_diff,
                         const Variant& container1,
                         const Variant& container2,
                         const Array& args /* = null array */) {

  /* Check to make sure all inputs are containers */
  const auto& c1 = *container1.asTypedValue();
  const auto& c2 = *container2.asTypedValue();
  if (isClsMethType(c1.m_type) || isClsMethType(c2.m_type)) {
    raiseIsClsMethWarning(__FUNCTION__+2, isClsMethType(c1.m_type) ? 1 : 2);
    return make_tv<KindOfNull>();
  }
  if (UNLIKELY(!isContainer(c1) || !isContainer(c2))) {
    raise_warning("%s() expects parameter %d to be an array or collection",
                  __FUNCTION__+2, /* remove the "f_" prefix */
                  isContainer(c1) ? 2 : 1);
    return make_tv<KindOfNull>();
  }
  bool moreThanTwo = !args.empty();
  size_t largestSize = getContainerSize(c2);
  if (UNLIKELY(moreThanTwo)) {
    int pos = 3;
    for (ArrayIter argvIter(args); argvIter; ++argvIter, ++pos) {
      auto const c = argvIter.secondVal();
      if (!isContainer(c)) {
        raise_warning("%s() expects parameter %d to be an array or collection",
                      __FUNCTION__+2, /* remove the "f_" prefix */
                      pos);
        return make_tv<KindOfNull>();
      }
      size_t sz = getContainerSize(c);
      if (sz > largestSize) {
        largestSize = sz;
      }
    }
  }
  /* If container1 is empty, we can stop here and return the empty array */
  if (!getContainerSize(c1)) {
    return make_array_like_tv(staticEmptyDictArray());
  }
  /* If all of the containers (except container1) are empty, we can just
     return container1 (converting it to an array if needed) */
  if (!largestSize) {
    if (isArrayLikeType(c1.m_type)) {
      return tvReturn(container1);
    } else {
      return tvReturn(container1.toArray<IntishCast::Cast>());
    }
  }
  Array ret = Array::CreateDict();

  // Put all of the values from all the containers (except container1 into a
  // Set. All types aside from integer and string will be cast to string, and
  // we also convert int-like strings to integers.
  auto st = req::make<c_Set>();
  st->reserve(largestSize);
  containerValuesToSetHelper(st, container2);
  if (UNLIKELY(moreThanTwo)) {
    for (ArrayIter argvIter(args); argvIter; ++argvIter) {
      containerValuesToSetHelper(st, VarNR(argvIter.secondVal()));
    }
  }
  // Loop over container1, only copying over key/value pairs where the value
  // is not present in the Set. When checking if a value is present in the
  // Set, any value that is not an integer or string is cast to a string, and
  // we convert int-like strings to integers.
  Variant strHolder(empty_string_variant());
  TypedValue* strTv = strHolder.asTypedValue();
  bool convertIntLikeStrs = !isArrayLikeType(c1.m_type);
  for (ArrayIter iter(container1); iter; ++iter) {
    auto const c = iter.secondValPlus();
    if (checkSetHelper(st, c, strTv, true)) continue;
    auto const key = convertIntLikeStrs
      ? ret.convertKey<IntishCast::Cast>(iter.first())
      : *iter.first().asTypedValue();
    ret.set(key, iter.secondValPlus(), true);
  }
  return tvReturn(std::move(ret));
}

namespace {

template <typename T>
ALWAYS_INLINE
bool array_diff_intersect_key_inputs_ok(const TypedValue& c1, const TypedValue& c2,
                                        const ArrayData* args,
                                        const char* fname,
                                        T callback) {
  if (isClsMethType(c1.m_type) || isClsMethType(c2.m_type)) {
    raiseIsClsMethWarning(fname, isClsMethType(c1.m_type) ? 1 : 2);
    return false;
  }
  if (!isContainer(c1) || !isContainer(c2)) {
    raise_warning("%s() expects parameter %d to be an array or collection",
                  fname, isContainer(c1) ? 2 : 1);
    return false;
  }
  callback(getContainerSize(c2));

  bool ok = true;
  IterateKV(args, [&](TypedValue k, TypedValue v) {
    assertx(k.m_type == KindOfInt64);
    if (isClsMethType(v.m_type)) {
      raiseIsClsMethWarning(fname, k.m_data.num + 3);
      ok = false;
      return true;
    }
    if (!isContainer(v)) {
      raise_warning("%s() expects parameter %ld to be an array or collection",
                    fname, k.m_data.num + 3);
      ok = false;
      return true;
    }
    callback(getContainerSize(v));
    return false;
  });

  return ok;
}

template <bool diff, bool coerceThis, bool coerceAd, typename SI, typename SS>
ALWAYS_INLINE
void array_diff_intersect_key_check_arr(ArrayData* ad, TypedValue k, TypedValue v,
                                        SI setInt, SS setStr) {
  if (k.m_type == KindOfInt64) {
    if (ad->exists(k.m_data.num)) {
      if (!diff) setInt(k.m_data.num, v);
      return;
    }
    if (coerceAd) {
      // Also need to check if ad has a key that will coerce to this int value.
      auto s = OptString::attach(buildStringData(k.m_data.num));
      if (ad->exists(s.get())) {
        if (!diff) setInt(k.m_data.num, v);
        return;
      }
    }
    if (diff) setInt(k.m_data.num, v);
    return;
  }

  if (coerceThis) {
    int64_t n;
    if (k.m_data.pstr->isStrictlyInteger(n)) {
      if (ad->exists(n)) {
        if (!diff) setInt(n, v);
        return;
      }
      if (coerceAd) {
        // Also need to check if ad has a key that will coerce to this int
        // value (as did this key).
        if (ad->exists(k.m_data.pstr)) {
          if (!diff) setInt(n, v);
          return;
        }
      }
      if (diff) setInt(n, v);
      return;
    }
  } else if (coerceAd) {
    // We're coercing keys from ad, but not this. If this string key
    // isStrictlyInteger it can never match a key from ad after that key
    // is coerced.
    int64_t n;
    if (k.m_data.pstr->isStrictlyInteger(n)) {
      if (diff) setStr(k.m_data.pstr, v);
      return;
    }
  }

  if (ad->exists(k.m_data.pstr)) {
    if (!diff) setStr(k.m_data.pstr, v);
  } else {
    if (diff) setStr(k.m_data.pstr, v);
  }
}

template <bool diff, bool coerceThis, typename SI, typename SS>
ALWAYS_INLINE
void array_diff_intersect_key_check_pair(TypedValue k, TypedValue v, SI setInt,
                                         SS setStr) {
  if (k.m_type == KindOfInt64) {
    if (k.m_data.num == 0 || k.m_data.num == 1) {
      if (!diff) setInt(k.m_data.num, v);
    } else {
      if (diff) setInt(k.m_data.num, v);
    }
    return;
  }

  if (coerceThis) {
    int64_t n;
    if (k.m_data.pstr->isStrictlyInteger(n)) {
      if (n == 0 || n == 1) {
        if (!diff) setInt(n, v);
      } else {
        if (diff) setInt(n, v);
      }
      return;
    }
  }

  if (diff) setStr(k.m_data.pstr, v);
}

}

TypedValue HHVM_FUNCTION(array_diff_key,
                         const Variant& container1,
                         const Variant& container2,
                         const Array& args /* = null array */) {
  const auto& c1 = *container1.asTypedValue();
  const auto& c2 = *container2.asTypedValue();

  size_t largestSize = 0;
  auto check_cb = [&] (size_t s) {
    if (s > largestSize) largestSize = s;
  };
  if (!array_diff_intersect_key_inputs_ok(c1, c2, args.get(), "array_diff_key",
                                          check_cb)) {
    return make_tv<KindOfNull>();
  }
  if (getContainerSize(c1) == 0) {
    return make_array_like_tv(staticEmptyDictArray());
  }
  if (largestSize == 0) {
    if (isArrayLikeType(c1.m_type)) {
      return tvReturn(container1);
    } else {
      return tvReturn(container1.toArray<IntishCast::Cast>());
    }
  }

  auto diff_step = [](TypedValue left, TypedValue right) {
    auto leftSize = getContainerSize(left);
    // If we have more than 2 args, the left input could end up empty
    if (leftSize == 0) return empty_dict_array();

    DictInit ret(leftSize);
    auto setInt = [&](int64_t k, TypedValue v) { ret.set(k, v); };
    auto setStr = [&](StringData* k, TypedValue v) { ret.set(k, v); };

    auto iterate_left_with = [&](auto test_key) {
      IterateKV(left, test_key);
    };

    // rightAd will be the backing ArrayData for right, or nullptr if right
    // is a Pair
    auto rightAd = [&](){
      if (isArrayLikeType(type(right))) return right.m_data.parr;
      return collections::asArray(right.m_data.pobj);
    }();

    // For historical reasons, we coerce intish string keys only when they
    // came from a hack collection. We also need to do the lookup in the right
    // array differently if right was a Pair (and so rightAd is nullptr)
    if (!rightAd) {
      if (isArrayLikeType(type(left))) {
        iterate_left_with([&](TypedValue k, TypedValue v) {
          array_diff_intersect_key_check_pair<true, false>(
            k, v, setInt, setStr);
        });
      } else {
        iterate_left_with([&](TypedValue k, TypedValue v) {
          array_diff_intersect_key_check_pair<true, true>(
            k, v, setInt, setStr);
        });
      }
    } else {
      if (isArrayLikeType(type(left))) {
        if (isArrayLikeType(type(right))) {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<true, false, false>(
              rightAd, k, v, setInt, setStr);
          });
        } else {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<true, false, true>(
              rightAd, k, v, setInt, setStr);
          });
        }
      } else {
        if (isArrayLikeType(type(right))) {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<true, true, false>(
              rightAd, k, v, setInt, setStr);
          });
        } else {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<true, true, true>(
              rightAd, k, v, setInt, setStr);
          });
        }
      }
    }

    return ret.toArray();
  };

  auto ret = diff_step(c1, c2);
  IterateV(args.get(), [&](TypedValue v) {
    ret = diff_step(make_array_like_tv(ret.get()), v);
  });
  return make_array_like_tv(ret.detach());
}

TypedValue HHVM_FUNCTION(array_udiff,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = data_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(diff, extra, false COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_diff_assoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  diff_intersect_body(diff, args, true COMMA true);
}

TypedValue HHVM_FUNCTION(array_diff_uassoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(diff, extra, true COMMA true COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_udiff_assoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = data_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(diff, extra, true COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_udiff_uassoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant data_func = data_compare_func;
  Variant key_func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    key_func = cycleVariadics(extra, key_func);
    data_func = cycleVariadics(extra, data_func);
  }
  diff_intersect_body(diff, extra, true
                      COMMA true COMMA cmp_func COMMA &key_func
                      COMMA cmp_func COMMA &data_func);
}

TypedValue HHVM_FUNCTION(array_diff_ukey,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(diff, extra, true COMMA false COMMA cmp_func COMMA &func);
}

///////////////////////////////////////////////////////////////////////////////
// intersect functions

static inline TypedValue* makeContainerListHelper(const Variant& a,
                                                  const Array& argv,
                                                  int count,
                                                  int smallestPos) {
  assertx(count == argv.size() + 1);
  assertx(0 <= smallestPos);
  assertx(smallestPos < count);
  // Allocate a TypedValue array and copy 'a' and the contents of 'argv'
  TypedValue* containers = req::make_raw_array<TypedValue>(count);
  tvCopy(*a.asTypedValue(), containers[0]);
  int pos = 1;
  for (ArrayIter argvIter(argv); argvIter; ++argvIter, ++pos) {
    tvCopy(argvIter.secondVal(), containers[pos]);
  }
  // Perform a swap so that the smallest container occurs at the first
  // position in the TypedValue array; this helps improve the performance
  // of containerValuesIntersectHelper()
  if (smallestPos != 0) {
    TypedValue tmp;
    tvCopy(containers[0], tmp);
    tvCopy(containers[smallestPos], containers[0]);
    tvCopy(tmp, containers[smallestPos]);
  }
  return containers;
}

static inline void addToIntersectMapHelper(const req::ptr<c_Map>& mp,
                                           const TypedValue c,
                                           TypedValue* intOneTv,
                                           TypedValue* strTv,
                                           bool convertIntLikeStrs) {
  if (c.m_type == KindOfInt64) {
    mp->set(c.m_data.num, *intOneTv);
  } else {
    StringData* s;
    if (LIKELY(isStringType(c.m_type))) {
      s = c.m_data.pstr;
    } else {
      s = tvCastToStringData(c);
      decRefStr(strTv->m_data.pstr);
      strTv->m_data.pstr = s;
    }
    int64_t n;
    if (convertIntLikeStrs && s->isStrictlyInteger(n)) {
      mp->set(n, *intOneTv);
    } else {
      mp->set(s, *intOneTv);
    }
  }
}

static inline void updateIntersectMapHelper(const req::ptr<c_Map>& mp,
                                            const TypedValue c,
                                            int pos,
                                            TypedValue* strTv,
                                            bool convertIntLikeStrs) {
  if (c.m_type == KindOfInt64) {
    auto val = mp->get(c.m_data.num);
    if (val && val->m_data.num == pos) {
      assertx(val->m_type == KindOfInt64);
      ++val->m_data.num;
    }
  } else {
    StringData* s;
    if (LIKELY(isStringType(c.m_type))) {
      s = c.m_data.pstr;
    } else {
      s = tvCastToStringData(c);
      decRefStr(strTv->m_data.pstr);
      strTv->m_data.pstr = s;
    }
    int64_t n;
    if (convertIntLikeStrs && s->isStrictlyInteger(n)) {
      auto val = mp->get(n);
      if (val && val->m_data.num == pos) {
        assertx(val->m_type == KindOfInt64);
        ++val->m_data.num;
      }
    } else {
      auto val = mp->get(s);
      if (val && val->m_data.num == pos) {
        assertx(val->m_type == KindOfInt64);
        ++val->m_data.num;
      }
    }
  }
}

static void containerValuesIntersectHelper(const req::ptr<c_Set>& st,
                                           TypedValue* containers,
                                           int count) {
  assertx(count >= 2);
  auto mp = req::make<c_Map>();
  Variant strHolder(empty_string_variant());
  TypedValue* strTv = strHolder.asTypedValue();
  TypedValue intOneTv = make_tv<KindOfInt64>(1);
  for (ArrayIter iter(tvAsCVarRef(&containers[0])); iter; ++iter) {
    auto const c = iter.secondValPlus();
    // For each value v in containers[0], we add the key/value pair (v, 1)
    // to the map. If a value (after various conversions) occurs more than
    // once in the container, we'll simply overwrite the old entry and that's
    // fine.
    addToIntersectMapHelper(mp, c, &intOneTv, strTv, true);
  }
  for (int pos = 1; pos < count; ++pos) {
    for (ArrayIter iter(tvAsCVarRef(&containers[pos])); iter; ++iter) {
      auto const c = iter.secondValPlus();
      // We check if the value is present as a key in the map. If an entry
      // exists and its value equals pos, we increment it, otherwise we do
      // nothing. This is essential so that we don't accidentally double-count
      // a key (after various conversions) that occurs in the container more
      // than once.
      updateIntersectMapHelper(mp, c, pos, strTv, true);
    }
  }
  for (ArrayIter iter(mp.get()); iter; ++iter) {
    // For each key in the map, we copy the key to the set if the
    // corresponding value is equal to pos exactly (which means it
    // was present in all of the containers).
    auto const tv = iter.secondValPlus();
    assertx(type(tv) == KindOfInt64);
    if (val(tv).num == count) {
      st->add(*iter.first().asTypedValue());
    }
  }
}

TypedValue HHVM_FUNCTION(array_intersect,
                         const Variant& container1,
                         const Variant& container2,
                         const Array& args /* = null array */) {

  /* Check to make sure all inputs are containers */
  const auto& c1 = *container1.asTypedValue();
  const auto& c2 = *container2.asTypedValue();
  if (isClsMethType(c1.m_type) || isClsMethType(c2.m_type)) {
    raiseIsClsMethWarning(__FUNCTION__+2, isClsMethType(c1.m_type) ? 1 : 2);
    return make_tv<KindOfNull>();
  }
  if (!isContainer(c1) || !isContainer(c2)) {
    raise_warning("%s() expects parameter %d to be an array or collection",
                  __FUNCTION__+2, /* remove the "f_" prefix */
                  isContainer(c1) ? 2 : 1);
    return make_tv<KindOfNull>();
  }
  bool moreThanTwo = !args.empty();
  /* Keep track of which input container was the smallest (excluding
     container1) */
  int smallestPos = 0;
  size_t smallestSize = getContainerSize(c2);
  if (UNLIKELY(moreThanTwo)) {
    int pos = 1;
    for (ArrayIter argvIter(args); argvIter; ++argvIter, ++pos) {
      auto const c = argvIter.secondVal();
      if (!isContainer(c)) {
        raise_warning("%s() expects parameter %d to be an array or collection",
                      __FUNCTION__+2, /* remove the "f_" prefix */
                      pos+2);
        return make_tv<KindOfNull>();
      }
      size_t sz = getContainerSize(c);
      if (sz < smallestSize) {
        smallestSize = sz;
        smallestPos = pos;
      }
    }
  }
  /* If any of the containers were empty, we can stop here and return the
     empty array */
  if (!getContainerSize(c1) || !smallestSize) {
    return make_array_like_tv(staticEmptyDictArray());
  }

  Array ret = Array::CreateDict();
  // Build up a Set containing the values that are present in all the
  // containers (except container1)
  auto st = req::make<c_Set>();
  if (LIKELY(!moreThanTwo)) {
    // There is only one container (not counting container1) so we can
    // just call containerValuesToSetHelper() to build the Set.
    containerValuesToSetHelper(st, container2);
  } else {
    // We're dealing with three or more containers. Copy all of the containers
    // (except the first) into a TypedValue array.
    int count = args.size() + 1;
    TypedValue* containers =
      makeContainerListHelper(container2, args, count, smallestPos);
    SCOPE_EXIT { req::free(containers); };
    // Build a Set of the values that were present in all of the containers
    containerValuesIntersectHelper(st, containers, count);
  }
  // Loop over container1, only copying over key/value pairs where the value
  // is present in the Set. When checking if a value is present in the Set,
  // any value that is not an integer or string is cast to a string, and we
  // convert int-like strings to integers.
  Variant strHolder(empty_string_variant());
  TypedValue* strTv = strHolder.asTypedValue();
  bool convertIntLikeStrs = !isArrayLikeType(c1.m_type);
  for (ArrayIter iter(container1); iter; ++iter) {
    auto const c = iter.secondValPlus();
    if (!checkSetHelper(st, c, strTv, true)) continue;
    const auto key = convertIntLikeStrs
      ? Variant::wrap(ret.convertKey<IntishCast::Cast>(iter.first()))
      : iter.first();
    ret.set(key, iter.secondValPlus(), true);
  }
  return tvReturn(std::move(ret));
}

TypedValue HHVM_FUNCTION(array_intersect_key,
                         const Variant& container1,
                         const Variant& container2,
                         const Array& args /* = null array */) {
  const auto& c1 = *container1.asTypedValue();
  const auto& c2 = *container2.asTypedValue();

  bool empty_arg = false;
  auto check_cb = [&] (size_t s) {
    if (s == 0) empty_arg = true;
  };
  if (!array_diff_intersect_key_inputs_ok(c1, c2, args.get(),
                                          "array_intersect_key", check_cb)) {
    return make_tv<KindOfNull>();
  }
  if ((getContainerSize(c1) == 0) || empty_arg) {
    return make_array_like_tv(staticEmptyDictArray());
  }

  auto intersect_step = [](TypedValue left, TypedValue right) {
    auto const leftSize = getContainerSize(left);
    if (leftSize == 0) return empty_dict_array();

    DictInit ret(leftSize);
    auto setInt = [&](int64_t k, TypedValue v) { ret.set(k, v); };
    auto setStr = [&](StringData* k, TypedValue v) { ret.set(k, v); };

    auto iterate_left_with = [&](auto test_key) {
      IterateKV(left, test_key);
    };

    // rightAd will be the backing ArrayData for right, or nullptr if right
    // is a Pair
    auto rightAd = [&](){
      if (isArrayLikeType(type(right))) return right.m_data.parr;
      return collections::asArray(right.m_data.pobj);
    }();

    // For historical reasons, we coerce intish string keys only when they
    // came from a hack collection. We also need to do the lookup in the right
    // array differently if right was a Pair (and so rightAd is nullptr)
    if (!rightAd) {
      if (isArrayLikeType(type(left))) {
        iterate_left_with([&](TypedValue k, TypedValue v) {
          array_diff_intersect_key_check_pair<false, false>(
            k, v, setInt, setStr);
        });
      } else {
        iterate_left_with([&](TypedValue k, TypedValue v) {
          array_diff_intersect_key_check_pair<false, true>(
            k, v, setInt, setStr);
        });
      }
    } else {
      if (isArrayLikeType(type(left))) {
        if (isArrayLikeType(type(right))) {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<false, false, false>(
              rightAd, k, v, setInt, setStr);
          });
        } else {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<false, false, true>(
              rightAd, k, v, setInt, setStr);
          });
        }
      } else {
        if (isArrayLikeType(type(right))) {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<false, true, false>(
              rightAd, k, v, setInt, setStr);
          });
        } else {
          iterate_left_with([&](TypedValue k, TypedValue v) {
            array_diff_intersect_key_check_arr<false, true, true>(
              rightAd, k, v, setInt, setStr);
          });
        }
      }
    }

    return ret.toArray();
  };

  auto ret = intersect_step(c1, c2);
  IterateV(args.get(), [&](TypedValue v) {
    ret = intersect_step(make_array_like_tv(ret.get()), v);
  });
  return make_array_like_tv(ret.detach());
}

TypedValue HHVM_FUNCTION(array_uintersect,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = data_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(intersect, extra, false COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_intersect_assoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  diff_intersect_body(intersect, args, true COMMA true);
}

TypedValue HHVM_FUNCTION(array_intersect_uassoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(intersect, extra, true COMMA true
                      COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_uintersect_assoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = data_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(intersect, extra, true COMMA true COMMA NULL COMMA NULL
                      COMMA cmp_func COMMA &func);
}

TypedValue HHVM_FUNCTION(array_uintersect_uassoc,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& data_compare_func,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant data_func = data_compare_func;
  Variant key_func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    key_func = cycleVariadics(extra, key_func);
    data_func = cycleVariadics(extra, data_func);
  }
  diff_intersect_body(intersect, extra, true COMMA true COMMA cmp_func
                      COMMA &key_func COMMA cmp_func COMMA &data_func);
}

TypedValue HHVM_FUNCTION(array_intersect_ukey,
                         const Variant& array1,
                         const Variant& array2,
                         const Variant& key_compare_func,
                         const Array& args /* = null array */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array1, array2)) {
    return make_tv<KindOfNull>();
  }
  Variant func = key_compare_func;
  Array extra = args;
  if (!extra.empty()) {
    func = cycleVariadics(extra, func);
  }
  diff_intersect_body(intersect, extra, true COMMA false
                      COMMA cmp_func COMMA &func);
}

///////////////////////////////////////////////////////////////////////////////
// sorting functions

struct Collator final : RequestEventHandler {
  OptString getLocale() {
    return m_locale;
  }
  Intl::IntlError &getErrorRef() {
    return m_errcode;
  }
  bool setLocale(const OptString& locale) {
    if (m_locale.same(locale)) {
      return true;
    }
    if (m_ucoll) {
      ucol_close(m_ucoll);
      m_ucoll = NULL;
    }
    m_errcode.clearError();
    UErrorCode error = U_ZERO_ERROR;
    m_ucoll = ucol_open(locale.data(), &error);
    if (m_ucoll == NULL) {
      raise_warning("failed to load %s locale from icu data", locale.data());
      return false;
    }
    if (U_FAILURE(error)) {
      m_errcode.setError(error);
      ucol_close(m_ucoll);
      m_ucoll = NULL;
      return false;
    }
    m_locale = locale;
    return true;
  }

  UCollator *getCollator() {
    return m_ucoll;
  }

  bool setAttribute(int64_t attr, int64_t val) {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    m_errcode.clearError();
    UErrorCode error = U_ZERO_ERROR;
    ucol_setAttribute(m_ucoll, (UColAttribute)attr,
                      (UColAttributeValue)val, &error);
    if (U_FAILURE(error)) {
      m_errcode.setError(error);
      Logger::Verbose("Error setting attribute value");
      return false;
    }
    return true;
  }

  bool setStrength(int64_t strength) {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    ucol_setStrength(m_ucoll, (UCollationStrength)strength);
    return true;
  }

  Variant getErrorCode() {
    if (!m_ucoll) {
      Logger::Verbose("m_ucoll is NULL");
      return false;
    }
    return m_errcode.getErrorCode();
  }

  void requestInit() override {
    m_locale = OptString(uloc_getDefault(), CopyString);
    m_errcode.clearError();
    UErrorCode error = U_ZERO_ERROR;
    m_ucoll = ucol_open(m_locale.data(), &error);
    if (U_FAILURE(error)) {
      m_errcode.setError(error);
    }
    assertx(m_ucoll);
  }

  void requestShutdown() override {
    m_locale.reset();
    m_errcode.clearError(false);
    if (m_ucoll) {
      ucol_close(m_ucoll);
      m_ucoll = NULL;
    }
  }

private:
  OptString     m_locale;
  UCollator *m_ucoll;
  Intl::IntlError m_errcode;
};
IMPLEMENT_STATIC_REQUEST_LOCAL(Collator, s_collator);

namespace {
struct ArraySortTmp {
  ArraySortTmp(TypedValue* tv, SortFunction sf) : m_tv(tv) {
    m_ad = val(tv).parr->escalateForSort(sf);
    assertx(m_ad->empty() || m_ad->hasExactlyOneRef());
  }
  ~ArraySortTmp() {
    auto const old = val(m_tv).parr;
    auto const legacy = old->isLegacyArray();

    // We must call BespokeArray::PostSort before cleaning up the old array,
    // because some bespoke arrays may move values from old -> m_ad at PreSort
    // and move them back at PostSort. (LoggingArray moves the entire array.)
    if (!old->isVanilla()) {
      m_ad = BespokeArray::PostSort(old, m_ad);
    }

    // Do not update the inout value if we are throwing an exception. Builtins
    // receive inout values by a TV pointer. The JIT expects that the inout
    // value will not be updated if the builtin throws. For example, it assumes
    // that the type remains the same and if an exception is thrown, it uses the
    // corresponding destructor.
    if (folly::uncaught_exceptions() > m_excCount) {
      if (m_ad != old) decRefArr(m_ad);
      return;
    }

    if (m_ad != old) {
      tvMove(make_array_like_tv(m_ad), m_tv);
    }

    // All sort functions preserve the legacy bit.
    assertx(m_ad == val(m_tv).parr);
    if (legacy != m_ad->isLegacyArray()) {
      auto const marked = arrprov::markTvShallow(*m_tv, legacy);
      tvMove(marked, m_tv);
    }
  }
  ArrayData* operator->() { return m_ad; }
 private:
  TypedValue* m_tv;
  ArrayData* m_ad;
  int m_excCount{folly::uncaught_exceptions()};
};
}

static bool
php_sort(Variant& container, int sort_flags, bool ascending) {
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    if (ad->size() == 1 && ad->isVecType()) return true;
    if (ad->empty()) return true;
    SortFunction sf = getSortFunction(SORTFUNC_SORT, ascending);
    ArraySortTmp ast(container.asTypedValue(), sf);
    ast->sort(sort_flags, ascending);
    return true;
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection() &&
        obj->collectionType() == CollectionType::Vector) {
      SortFunction sf = getSortFunction(SORTFUNC_SORT, ascending);
      c_Vector::SortTmp vst(static_cast<c_Vector*>(obj), sf);
      vst->sort(sort_flags, ascending);
      return true;
    }
    // other collections are not supported:
    //  - Maps and Sets require associative sort
    //  - Immutable collections are not to be modified
  }
  raise_expected_array_or_collection_warning(ascending ? "sort" : "rsort");
  return false;
}

static bool
php_asort(Variant& container, int sort_flags, bool ascending) {
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    if (ad->size() <= 1 && !ad->isVecType()) return true;
    SortFunction sf = getSortFunction(SORTFUNC_ASORT, ascending);
    ArraySortTmp ast(container.asTypedValue(), sf);
    ast->asort(sort_flags, ascending);
    return true;
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      auto type = obj->collectionType();
      if (type == CollectionType::Map || type == CollectionType::Set) {
        SortFunction sf = getSortFunction(SORTFUNC_ASORT, ascending);
        HashCollection::SortTmp hst(static_cast<HashCollection*>(obj), sf);
        hst->asort(sort_flags, ascending);
        return true;
      }
    }
  }
  raise_expected_array_or_collection_warning(ascending ? "asort" : "arsort");
  return false;
}

static bool
php_ksort(Variant& container, int sort_flags, bool ascending) {
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    auto const vec = ad->isVecType();
    if ((vec && ascending) || (!vec && ad->size() <= 1)) return true;
    SortFunction sf = getSortFunction(SORTFUNC_KSORT, ascending);
    ArraySortTmp ast(container.asTypedValue(), sf);
    ast->ksort(sort_flags, ascending);
    return true;
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      auto type = obj->collectionType();
      if (type == CollectionType::Map || type == CollectionType::Set) {
        SortFunction sf = getSortFunction(SORTFUNC_KSORT, ascending);
        HashCollection::SortTmp hst(static_cast<HashCollection*>(obj), sf);
        hst->ksort(sort_flags, ascending);
        return true;
      }
    }
  }
  raise_expected_array_or_collection_warning(ascending ? "ksort" : "krsort");
  return false;
}

bool HHVM_FUNCTION(sort,
                  Variant& array,
                  int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_sort(array, sort_flags, true);
}

bool HHVM_FUNCTION(rsort,
                   Variant& array,
                   int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_sort(array, sort_flags, false);
}

bool HHVM_FUNCTION(asort,
                   Variant& array,
                   int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_asort(array, sort_flags, true);
}

bool HHVM_FUNCTION(arsort,
                   Variant& array,
                   int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_asort(array, sort_flags, false);
}

bool HHVM_FUNCTION(ksort,
                   Variant& array,
                   int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_ksort(array, sort_flags, true);
}

bool HHVM_FUNCTION(krsort,
                   Variant& array,
                   int64_t sort_flags /* = 0 */) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_ksort(array, sort_flags, false);
}

bool HHVM_FUNCTION(natsort, Variant& array) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_asort(array, SORT_NATURAL, true);
}

bool HHVM_FUNCTION(natcasesort, Variant& array) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, array)) return false;
  return php_asort(array, SORT_NATURAL_CASE, true);
}

bool HHVM_FUNCTION(usort,
                   Variant& container,
                   const Variant& cmp_function) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, container)) return false;
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    if (ad->size() == 1 && ad->isVecType()) return true;
    if (ad->empty()) return true;
    ArraySortTmp ast(container.asTypedValue(), SORTFUNC_USORT);
    return ast->usort(cmp_function);
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      if (obj->collectionType() == CollectionType::Vector) {
        c_Vector::SortTmp vst(static_cast<c_Vector*>(obj), SORTFUNC_USORT);
        return vst->usort(cmp_function);
      }
    }
    // other collections are not supported:
    //  - Maps and Sets require associative sort
    //  - Immutable collections are not to be modified
  }
  raise_expected_array_or_collection_warning("usort");
  return false;
}

bool HHVM_FUNCTION(uasort,
                   Variant& container,
                   const Variant& cmp_function) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, container)) return false;
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    if (ad->size() <= 1 && !ad->isVecType()) return true;
    ArraySortTmp ast(container.asTypedValue(), SORTFUNC_UASORT);
    return ast->uasort(cmp_function);
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      auto type = obj->collectionType();
      if (type == CollectionType::Map || type == CollectionType::Set) {
        HashCollection::SortTmp hst(static_cast<HashCollection*>(obj),
                                    SORTFUNC_UASORT);
        return hst->uasort(cmp_function);
      }
    }
    // other collections are not supported:
    //  - Vectors require a non-associative sort
    //  - Immutable collections are not to be modified
  }
  raise_expected_array_or_collection_warning("uasort");
  return false;
}

bool HHVM_FUNCTION(uksort,
                   Variant& container,
                   const Variant& cmp_function) {
  if (checkIsClsMethAndRaise( __FUNCTION__+2, container)) return false;
  if (container.isArray()) {
    auto const ad = container.asTypedValue()->val().parr;
    if (ad->size() <= 1 && !ad->isVecType()) return true;
    ArraySortTmp ast(container.asTypedValue(), SORTFUNC_UKSORT);
    return ast->uksort(cmp_function);
  }
  if (container.isObject()) {
    ObjectData* obj = container.getObjectData();
    if (obj->isCollection()) {
      auto type = obj->collectionType();
      if (type == CollectionType::Map || type == CollectionType::Set) {
        HashCollection::SortTmp hst(static_cast<HashCollection*>(obj),
                                    SORTFUNC_UKSORT);
        return hst->uksort(cmp_function);
      }
    }
    // other collections are not supported:
    //  - Vectors require a non-associative sort
    //  - Immutable collections are not to be modified
  }
  raise_expected_array_or_collection_warning("uksort");
  return false;
}

TypedValue HHVM_FUNCTION(array_unique,
                         const Variant& array,
                         int64_t sort_flags /* = 2 */) {
  getCheckedArray(array);
  switch (sort_flags) {
  case SORT_STRING:
  case SORT_LOCALE_STRING:
    return tvReturn(ArrayUtil::StringUnique(arr_array));
  case SORT_NUMERIC:
    return tvReturn(ArrayUtil::NumericUnique(arr_array));
  case SORT_REGULAR:
  default:
    return tvReturn(ArrayUtil::RegularSortUnique(arr_array));
  }
}

OptString HHVM_FUNCTION(i18n_loc_get_default) {
  return s_collator->getLocale();
}

bool HHVM_FUNCTION(i18n_loc_set_default,
                   const OptString& locale) {
  return s_collator->setLocale(locale);
}

bool HHVM_FUNCTION(i18n_loc_set_attribute,
                   int64_t attr,
                   int64_t val) {
  return s_collator->setAttribute(attr, val);
}

bool HHVM_FUNCTION(i18n_loc_set_strength,
                   int64_t strength) {
  return s_collator->setStrength(strength);
}

Variant HHVM_FUNCTION(i18n_loc_get_error_code) {
  return s_collator->getErrorCode();
}

TypedValue HHVM_FUNCTION(hphp_array_idx,
                         const Variant& search,
                         const Variant& key,
                         const Variant& def) {
  if (!key.isNull()) {
    if (LIKELY(search.isArray())) {
      ArrayData *arr = search.getArrayData();
      auto const index = key.toKey(arr).tv();
      if (!isNullType(index.m_type)) {
        auto const ret = arr->get(index, false);
        if (ret.is_init()) {
          return tvReturn(tvAsCVarRef(ret));
        }
      }
    } else {
      raise_error("hphp_array_idx: search must be an array");
    }
  }
  return tvReturn(def);
}

static Array::PFUNC_CMP get_cmp_func(int sort_flags, bool ascending) {
  switch (sort_flags) {
  case SORT_NATURAL:
    return ascending ?
      Array::SortNaturalAscending : Array::SortNaturalDescending;
  case SORT_NATURAL_CASE:
    return ascending ?
      Array::SortNaturalCaseAscending: Array::SortNaturalCaseDescending;
  case SORT_NUMERIC:
    return ascending ?
      Array::SortNumericAscending : Array::SortNumericDescending;
  case SORT_STRING:
    return ascending ?
      Array::SortStringAscending : Array::SortStringDescending;
  case SORT_STRING_CASE:
    return ascending ?
      Array::SortStringAscendingCase : Array::SortStringDescendingCase;
  case SORT_LOCALE_STRING:
    return ascending ?
      Array::SortLocaleStringAscending : Array::SortLocaleStringDescending;
  case SORT_REGULAR:
  default:
    return ascending ?
      Array::SortRegularAscending : Array::SortRegularDescending;
  }
}

namespace {
bool array_multisort_impl(
    Variant* arg1,
    Variant* arg2 = nullptr,
    Variant* arg3 = nullptr,
    Variant* arg4 = nullptr,
    Variant* arg5 = nullptr,
    Variant* arg6 = nullptr,
    Variant* arg7 = nullptr,
    Variant* arg8 = nullptr,
    Variant* arg9 = nullptr
) {
  if (!arg1->isArray()) {
    if (arg1->isClsMeth()) {
      raiseIsClsMethWarning("array_multisort", 1);
      return false;
    }
    raise_expected_array_warning("array_multisort");
    return false;
  }

  std::vector<Array::SortData> data;
  std::vector<Array> arrays;
  arrays.reserve(9); // so no resize would happen

  Array::SortData sd;
  sd.original = arg1;
  arrays.push_back(Array(sd.original->getArrayData()));
  sd.array = &arrays.back();
  sd.by_key = false;

  int sort_flags = SORT_REGULAR;
  bool ascending = true;

  auto const handleArg = [&] (Variant* arg) {
    if (!arg || arg->isNull()) return;
    if (arg->isArray()) {
      sd.cmp_func = get_cmp_func(sort_flags, ascending);
      data.push_back(sd);

      sort_flags = SORT_REGULAR;
      ascending = true;

      sd.original = arg;
      arrays.push_back(Array(sd.original->getArrayData()));
      sd.array = &arrays.back();
    } else {
      int n = arg->toInt64();
      if (n == SORT_ASC) {
      } else if (n == SORT_DESC) {
        ascending = false;
      } else {
        sort_flags = n;
      }
    }
  };

  handleArg(arg2);
  handleArg(arg3);
  handleArg(arg4);
  handleArg(arg5);
  handleArg(arg6);
  handleArg(arg7);
  handleArg(arg8);
  handleArg(arg9);

  sd.cmp_func = get_cmp_func(sort_flags, ascending);
  data.push_back(sd);

  return Array::MultiSort(data);
}
} // anonymous namespace

bool HHVM_FUNCTION(array_multisort1,
                   Variant& arg1) {
  return array_multisort_impl(&arg1);
}

bool HHVM_FUNCTION(array_multisort2,
                   Variant& arg1,
                   Variant& arg2) {
  return array_multisort_impl(&arg1, &arg2);
}

bool HHVM_FUNCTION(array_multisort3,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3) {
  return array_multisort_impl(&arg1, &arg2, &arg3);
}

bool HHVM_FUNCTION(array_multisort4,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4) {
  return array_multisort_impl(&arg1, &arg2, &arg3, &arg4);
}

bool HHVM_FUNCTION(array_multisort5,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4,
                   Variant& arg5) {
  return array_multisort_impl(&arg1, &arg2, &arg3, &arg4, &arg5);
}

bool HHVM_FUNCTION(array_multisort6,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4,
                   Variant& arg5,
                   Variant& arg6) {
  return array_multisort_impl(&arg1, &arg2, &arg3, &arg4, &arg5, &arg6);
}

bool HHVM_FUNCTION(array_multisort7,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4,
                   Variant& arg5,
                   Variant& arg6,
                   Variant& arg7) {
  return array_multisort_impl(&arg1, &arg2, &arg3, &arg4, &arg5, &arg6, &arg7);
}

bool HHVM_FUNCTION(array_multisort8,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4,
                   Variant& arg5,
                   Variant& arg6,
                   Variant& arg7,
                   Variant& arg8) {
  return array_multisort_impl(
      &arg1, &arg2, &arg3, &arg4, &arg5, &arg6, &arg7, &arg8);
}

bool HHVM_FUNCTION(array_multisort9,
                   Variant& arg1,
                   Variant& arg2,
                   Variant& arg3,
                   Variant& arg4,
                   Variant& arg5,
                   Variant& arg6,
                   Variant& arg7,
                   Variant& arg8,
                   Variant& arg9) {
  return array_multisort_impl(
      &arg1, &arg2, &arg3, &arg4, &arg5, &arg6, &arg7, &arg8, &arg9);
}

// HH\\dict
Array HHVM_FUNCTION(HH_dict, const Variant& input) {
  return input.toDict();
}

// HH\\keyset
Array HHVM_FUNCTION(HH_keyset, const Variant& input) {
  return input.toKeyset();
}

// HH\\vec
Array HHVM_FUNCTION(HH_vec, const Variant& input) {
  return input.toVec();
}

// HH\\varray
Array HHVM_FUNCTION(HH_varray, const Variant& input) {
  return input.toVec();
}

// HH\\darray
Array HHVM_FUNCTION(HH_darray, const Variant& input) {
  return input.toDict();
}

TypedValue HHVM_FUNCTION(HH_array_key_cast, const Variant& input) {
  auto const op = "array_key_cast";
  switch (input.getType()) {
    case KindOfString: {
      int64_t n;
      auto const& str = input.asCStrRef();
      if (str.get()->isStrictlyInteger(n)) {
        return tvReturn(n);
      }
      return tvReturn(str);
    }

    case KindOfFunc:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Funcs cannot be cast to an array-key"
      );

    case KindOfClass:
      return tvReturn(StrNR(classToStringHelper(input.toClassVal(), op)));
    case KindOfLazyClass:
      return tvReturn(StrNR(lazyClassToStringHelper(input.toLazyClassVal(),
                                                    op)));

    case KindOfInt64:
    case KindOfBoolean:
    case KindOfDouble:
    case KindOfResource:
      return tvReturn(input.toInt64());

    case KindOfUninit:
    case KindOfNull:
      return tvReturn(staticEmptyString());

    case KindOfVec:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Vecs cannot be cast to an array-key"
      );
    case KindOfDict:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Dicts cannot be cast to an array-key"
      );
    case KindOfKeyset:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Keysets cannot be cast to an array-key"
      );
    case KindOfClsMeth:
      SystemLib::throwInvalidArgumentExceptionObject(
        "ClsMeths cannot be cast to an array-key"
      );
    case KindOfRClsMeth:
      SystemLib::throwInvalidArgumentExceptionObject(
        "RClsMeths cannot be cast to an array-key"
      );
    case KindOfObject:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Objects cannot be cast to an array-key"
      );
    case KindOfRFunc:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Reified functions cannot be cast to an array key"
      );
    case KindOfEnumClassLabel:
      SystemLib::throwInvalidArgumentExceptionObject(
        "Enum Class Labels cannot be cast to an array key"
      );
  }
  not_reached();
}

Array HHVM_FUNCTION(merge_xhp_attr_declarations,
                    const Array& arr1,
                    const Array& arr2,
                    const Array& rest) {
  auto ret = Array::CreateDict();
  IterateKV(arr1.get(), [&](TypedValue k, TypedValue v) { ret.set(k, v); });
  IterateKV(arr2.get(), [&](TypedValue k, TypedValue v) { ret.set(k, v); });
  int idx = 2;
  IterateV(
    rest.get(),
    [&](TypedValue arr) {
      if (!tvIsArrayLike(arr)) {
        raise_param_type_warning("__SystemLib\\merge_xhp_attr_declarations",
                                 idx+1, "array-like", arr);
        ret = Array{};
        return true;
      }
      IterateKV(arr.m_data.parr, [&](TypedValue k, TypedValue v) { ret.set(k, v); });
      ++idx;
      return false;
    }
  );
  return ret;
}

///////////////////////////////////////////////////////////////////////////////

struct ArrayExtension final : Extension {
  ArrayExtension() : Extension("array", NO_EXTENSION_VERSION_YET, "hphp_hphpi") {}
  void moduleRegisterNative() override {
    HHVM_RC_INT_SAME(UCOL_DEFAULT);

    HHVM_RC_INT_SAME(UCOL_PRIMARY);
    HHVM_RC_INT_SAME(UCOL_SECONDARY);
    HHVM_RC_INT_SAME(UCOL_TERTIARY);
    HHVM_RC_INT_SAME(UCOL_DEFAULT_STRENGTH);
    HHVM_RC_INT_SAME(UCOL_QUATERNARY);
    HHVM_RC_INT_SAME(UCOL_IDENTICAL);

    HHVM_RC_INT_SAME(UCOL_OFF);
    HHVM_RC_INT_SAME(UCOL_ON);

    HHVM_RC_INT_SAME(UCOL_SHIFTED);
    HHVM_RC_INT_SAME(UCOL_NON_IGNORABLE);

    HHVM_RC_INT_SAME(UCOL_LOWER_FIRST);
    HHVM_RC_INT_SAME(UCOL_UPPER_FIRST);

    HHVM_RC_INT_SAME(UCOL_FRENCH_COLLATION);
    HHVM_RC_INT_SAME(UCOL_ALTERNATE_HANDLING);
    HHVM_RC_INT_SAME(UCOL_CASE_FIRST);
    HHVM_RC_INT_SAME(UCOL_CASE_LEVEL);
    HHVM_RC_INT_SAME(UCOL_NORMALIZATION_MODE);
    HHVM_RC_INT_SAME(UCOL_STRENGTH);
    HHVM_RC_INT_SAME(UCOL_HIRAGANA_QUATERNARY_MODE);
    HHVM_RC_INT_SAME(UCOL_NUMERIC_COLLATION);

    HHVM_RC_INT(CASE_LOWER, static_cast<int64_t>(CaseMode::LOWER));
    HHVM_RC_INT(CASE_UPPER, static_cast<int64_t>(CaseMode::UPPER));

    HHVM_RC_INT(COUNT_NORMAL, static_cast<int64_t>(CountMode::NORMAL));
    HHVM_RC_INT(COUNT_RECURSIVE, static_cast<int64_t>(CountMode::RECURSIVE));

    HHVM_RC_INT_SAME(SORT_ASC);
    HHVM_RC_INT_SAME(SORT_DESC);
    HHVM_RC_INT_SAME(SORT_FLAG_CASE);
    HHVM_RC_INT_SAME(SORT_LOCALE_STRING);
    HHVM_RC_INT_SAME(SORT_NATURAL);
    HHVM_RC_INT_SAME(SORT_NUMERIC);
    HHVM_RC_INT_SAME(SORT_REGULAR);
    HHVM_RC_INT_SAME(SORT_STRING);

    HHVM_RC_INT(TAG_PROVENANCE_HERE_MUTATE_COLLECTIONS,
                arrprov::TagTVFlags::TAG_PROVENANCE_HERE_MUTATE_COLLECTIONS);

    HHVM_FE(array_change_key_case);
    HHVM_FE(array_chunk);
    HHVM_FE(array_column);
    HHVM_FE(array_combine);
    HHVM_FE(array_count_values);
    HHVM_FE(array_fill_keys);
    HHVM_FE(array_fill);
    HHVM_FE(array_flip);
    HHVM_FE(array_key_exists);
    HHVM_FE(key_exists);
    HHVM_FE(array_keys);
    HHVM_FALIAS(__SystemLib\\array_map, array_map);
    HHVM_FE(array_merge_recursive);
    HHVM_FE(array_replace_recursive);
    HHVM_FE(array_replace);
    HHVM_FE(array_pad);
    HHVM_FE(array_pop);
    HHVM_FE(array_product);
    HHVM_FE(array_push);
    HHVM_FE(array_rand);
    HHVM_FE(array_reverse);
    HHVM_FE(array_search);
    HHVM_FE(array_shift);
    HHVM_FE(array_slice);
    HHVM_FE(array_splice);
    HHVM_FE(array_sum);
    HHVM_FE(array_unique);
    HHVM_FE(array_unshift);
    HHVM_FE(array_values);
    HHVM_FE(shuffle);
    HHVM_FE(count);
    HHVM_FE(sizeof);
    HHVM_FE(in_array);
    HHVM_FE(range);
    HHVM_FE(array_diff);
    HHVM_FE(array_udiff);
    HHVM_FE(array_diff_assoc);
    HHVM_FE(array_diff_uassoc);
    HHVM_FE(array_udiff_assoc);
    HHVM_FE(array_udiff_uassoc);
    HHVM_FE(array_diff_key);
    HHVM_FE(array_diff_ukey);
    HHVM_FE(array_intersect);
    HHVM_FE(array_uintersect);
    HHVM_FE(array_intersect_assoc);
    HHVM_FE(array_intersect_uassoc);
    HHVM_FE(array_uintersect_assoc);
    HHVM_FE(array_uintersect_uassoc);
    HHVM_FE(array_intersect_key);
    HHVM_FE(array_intersect_ukey);
    HHVM_FE(sort);
    HHVM_FE(rsort);
    HHVM_FE(asort);
    HHVM_FE(arsort);
    HHVM_FE(ksort);
    HHVM_FE(krsort);
    HHVM_FE(usort);
    HHVM_FE(uasort);
    HHVM_FE(uksort);
    HHVM_FE(natsort);
    HHVM_FE(natcasesort);
    HHVM_FE(i18n_loc_get_default);
    HHVM_FE(i18n_loc_set_default);
    HHVM_FE(i18n_loc_set_attribute);
    HHVM_FE(i18n_loc_set_strength);
    HHVM_FE(i18n_loc_get_error_code);
    HHVM_FE(hphp_array_idx);
    HHVM_FE(array_multisort1);
    HHVM_FE(array_multisort2);
    HHVM_FE(array_multisort3);
    HHVM_FE(array_multisort4);
    HHVM_FE(array_multisort5);
    HHVM_FE(array_multisort6);
    HHVM_FE(array_multisort7);
    HHVM_FE(array_multisort8);
    HHVM_FE(array_multisort9);
    HHVM_FALIAS(HH\\dict, HH_dict);
    HHVM_FALIAS(HH\\vec, HH_vec);
    HHVM_FALIAS(HH\\keyset, HH_keyset);
    HHVM_FALIAS(HH\\varray, HH_varray);
    HHVM_FALIAS(HH\\darray, HH_darray);
    HHVM_FALIAS(HH\\array_key_cast, HH_array_key_cast);
    HHVM_FALIAS(__SystemLib\\merge_xhp_attr_declarations,
                merge_xhp_attr_declarations);
  }
} s_array_extension;

}
