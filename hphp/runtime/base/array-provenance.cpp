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

#include "hphp/runtime/base/array-provenance.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/tv-array-like.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/vm/bytecode.h"


#include <type_traits>

namespace HPHP::arrprov {

TRACE_SET_MOD(runtime)

///////////////////////////////////////////////////////////////////////////////

namespace {

static auto const kMaxMutationStackDepth = 512;

// NOTE: Setting a max_depth of 0 means that there's no user-provided limit.
// (We'll still stop at kMaxMutationStackDepth above for performance reasons.)
template <typename Mutation>
struct MutationState {
  Mutation& mutation;
  const char* function_name;
  uint32_t max_depth = 0;
  bool raised_stack_notice = false;
};

template <typename State>
ArrayData* apply_mutation(TypedValue tv, State& state,
                          bool cow = false, uint32_t depth = 0);

template <typename Array>
tv_lval LvalAtIterPos(ArrayData* ad, ssize_t pos) {
  if constexpr (std::is_same<Array, VanillaVec>::value) {
    return VanillaVec::LvalUncheckedInt(ad, pos);
  } else {
    static_assert(std::is_same<Array, VanillaDict>::value);
    return &VanillaDict::as(ad)->data()[pos].data;
  }
}

template <typename Array, typename State>
ArrayData* apply_mutation_fast(ArrayData* in, ArrayData* result,
                               State& state, bool cow, uint32_t depth) {
  auto const end = Array::IterEnd(in);
  for (auto pos = Array::IterBegin(in); pos != end;
       pos = Array::IterAdvance(in, pos)) {
    auto const prev = *LvalAtIterPos<Array>(in, pos);
    auto const ad = apply_mutation(prev, state, cow, depth + 1);
    if (!ad) continue;

    auto const next = make_array_like_tv(ad);
    result = result ? result : cow ? Array::Copy(in) : in;
    assertx(result->hasExactlyOneRef());
    assertx(Array::IterEnd(result) == Array::IterEnd(in));
    tvMove(next, LvalAtIterPos<Array>(result, pos));
  }
  FTRACE(1, "Depth {}: {} {}\n", depth,
         result && result != in ? "copy" : "reuse", in);
  return result == in ? nullptr : result;
}

template <typename State>
ArrayData* apply_mutation_slow(ArrayData* in, ArrayData* result,
                               State& state, bool cow, uint32_t depth) {
  // Careful! Even IterateKV will take a refcount on unknown arrays.
  // In order to do the mutation in place when possible, we iterate by hand.
  auto const end = in->iter_end();
  for (auto pos = in->iter_begin(); pos != end; pos = in->iter_advance(pos)) {
    auto const prev = in->nvGetVal(pos);
    auto const ad = apply_mutation(prev, state, cow, depth + 1);
    if (!ad) continue;

    // TODO(kshaunak): We can avoid the copy here if !cow by modifying all
    // of these mutation helpers to have "setMove" semantics. But it doesn't
    // affect algorithmic complexity, since we already do O(n) iteration.
    auto const escalated = result ? result : in;
    if (escalated == in) in->incRefCount();

    auto const key = in->nvGetKey(pos);
    auto const next = make_array_like_tv(ad);
    result = escalated->setMove(key, next);
    assertx(result->hasExactlyOneRef());
  }
  FTRACE(1, "Depth {}: {} {}\n", depth,
         result && result != in ? "copy" : "reuse", in);
  return result == in ? nullptr : result;
}

template <typename State>
ArrayData* apply_mutation_to_array(ArrayData* in, State& state,
                                   bool cow, uint32_t depth) {
  FTRACE(1, "Depth {}: mutating {} (cow = {})\n", depth, in, cow);

  // Apply the mutation to the top-level array.
  cow |= in->cowCheck();
  auto result = state.mutation(in, cow);
  if (state.max_depth == depth + 1) {
    FTRACE(1, "Depth {}: {} {}\n", depth, result ? "copy" : "reuse", in);
    return result;
  }

  // Recursively apply the mutation to the array's contents. For efficiency,
  // we do the layout check outside of the iteration loop.
  if (in->isVanillaVec()) {
    return apply_mutation_fast<VanillaVec>(in, result, state, cow, depth);
  } else if (in->isVanillaDict()) {
    return apply_mutation_fast<VanillaDict>(in, result, state, cow, depth);
  }
  return apply_mutation_slow(in, result, state, cow, depth);
}

// This function applies `state.mutation` to `tv` to get a modified array-like.
// Then, if `state.recursive` is set, it recursively applies the mutation to
// the values of the array-like. It does so with the minimum number of copies,
// mutating each array in-place if possible.
//
// `state.mutation` should take an ArrayData* and a `cow` param. If it can
// mutate the array in place (that is, either `cow` is false or no mutation is
// needed at all), it should do so and return nullptr. Otherwise, it must copy
// the array, mutate it, and return the updated result.
//
// We pass the mutation callback a `cow` param instead of checking ad->cowCheck
// to handle cases such as a refcount 1 array contained in a refcount 2 array;
// even though cowCheck return false for the refcount 1 array, we still need to
// copy it to get a new value to store in the COW-ed containing array.
template <typename State>
ArrayData* apply_mutation(TypedValue tv, State& state,
                          bool cow, uint32_t depth) {
  if (depth == kMaxMutationStackDepth) {
    if (!state.raised_stack_notice) {
      raise_notice("%s stack depth exceeded!", state.function_name);
      state.raised_stack_notice = true;
    }
    return nullptr;
  }
  if (!tvIsVec(tv) && !tvIsDict(tv)) return nullptr;
  auto const arr = val(tv).parr;
  return apply_mutation_to_array(arr, state, cow, depth);
}

TypedValue markTvImpl(TypedValue in, bool legacy, uint32_t depth) {
  // Closure state: whether or not we've raised notices for array-likes.
  auto raised_non_hack_array_notice = false;
  auto warn_once = [](bool& warned, const char* message) {
    if (!warned) raise_warning("%s", message);
    warned = true;
  };

  // The closure: tag vecs and dicts and notice on any other array-like inputs.
  auto const mark_tv = [&](ArrayData* ad, bool cow) -> ArrayData* {
    if (!ad->isVecType() && !ad->isDictType()) {
      warn_once(raised_non_hack_array_notice,
                "array_mark_legacy expects a dict or vec");
      return nullptr;
    }
    auto const result = ad->setLegacyArray(cow, true);
    return result == ad ? nullptr : result;
  };

  // Unmark legacy vecs/dicts to silence logging,
  // e.g. while casting to regular vecs or dicts.
  auto const unmark_tv = [&](ArrayData* ad, bool cow) -> ArrayData* {
    if (!ad->isVecType() && !ad->isDictType()) {
      return nullptr;
    }
    auto const result = ad->setLegacyArray(cow, false);
    return result == ad ? nullptr : result;
  };

  auto const ad = [&] {
    if (legacy) {
      auto state = MutationState<decltype(mark_tv)>{
        mark_tv, "array_mark_legacy", depth};
      return apply_mutation(in, state);
    } else {
      auto state = MutationState<decltype(unmark_tv)>{
        unmark_tv, "array_unmark_legacy", depth};
      return apply_mutation(in, state);
    }
  }();
  return ad ? make_array_like_tv(ad) : tvReturn(tvAsCVarRef(&in));
}

}

TypedValue tagTvRecursively(TypedValue in, int64_t flags) {
  return tvReturn(tvAsCVarRef(&in));
}

TypedValue markTvRecursively(TypedValue in, bool legacy) {
  return markTvImpl(in, legacy, 0);
}

TypedValue markTvShallow(TypedValue in, bool legacy) {
  return markTvImpl(in, legacy, 1);
}

TypedValue markTvToDepth(TypedValue in, bool legacy, uint32_t depth) {
  return markTvImpl(in, legacy, depth);
}

///////////////////////////////////////////////////////////////////////////////

}
