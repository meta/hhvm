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

#pragma once

#include <cstring>

namespace HPHP {

/*
 * Walk a call's sorted named argument names against the callee's sorted named
 * parameter names.
 *
 * Calls onMatch(paramIdx, argIdx) for each parameter an argument names,
 * onMissingParam(paramIdx) for each parameter no argument names, and
 * onExtraArg(argIdx) for each argument naming no parameter. Only a callee
 * declared with a `named t...' parameter can see the last of those; for any
 * other callee an unmatched argument name is an error, diagnosed by
 * checkNamedArgMismatch() before this runs.
 *
 * onMissingParam() returns false to stop the walk.
 */
template<typename OnMatch, typename OnMissingParam, typename OnExtraArg>
void mergeNamedArgs(const Func* callee, const ArrayData* namedArgNames,
                    OnMatch onMatch, OnMissingParam onMissingParam,
                    OnExtraArg onExtraArg) {
  int32_t const numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  int32_t const numNamedParams = callee->numNamedParams();
  auto const namedParamNames = callee->sortedNamedParamNames();

  auto const argNameAt = [&] (int32_t argIdx) {
    return namedArgNames->at(argIdx).val().pstr;
  };

  int32_t argIdx = 0;
  for (int32_t paramIdx = 0; paramIdx < numNamedParams; ++paramIdx) {
    auto const paramName = namedParamNames[paramIdx];
    while (argIdx < numNamedArgs &&
           argNameAt(argIdx)->slice().compare(paramName->slice()) < 0) {
      onExtraArg(argIdx);
      ++argIdx;
    }
    if (argIdx < numNamedArgs && argNameAt(argIdx) == paramName) {
      onMatch(paramIdx, argIdx);
      ++argIdx;
    } else if (!onMissingParam(paramIdx)) {
      return;
    }
  }
  // Everything left sorts after every parameter name, so matches nothing.
  for (; argIdx < numNamedArgs; ++argIdx) onExtraArg(argIdx);
}

/*
 * Number of named arguments that name a named parameter. Smaller than the
 * number of named arguments only for a callee with a `named t...' parameter,
 * which absorbs the rest.
 */
inline uint32_t numMatchedNamedArgs(const Func* callee,
                                    const ArrayData* namedArgNames) {
  if (!callee->hasVariadicNamedParam()) {
    return namedArgNames ? namedArgNames->size() : 0;
  }
  uint32_t matched = 0;
  mergeNamedArgs(
    callee, namedArgNames,
    [&] (int32_t, int32_t) { ++matched; },
    [] (int32_t) { return true; },
    [] (int32_t) {}
  );
  return matched;
}

/*
 * Number of cells the named argument region grows by when it is rewritten into
 * the named parameter region. Negative when named arguments are dropped, which
 * only happens for a callee declared with a `named t...' parameter.
 */
inline int32_t namedParamStackDelta(const Func* callee,
                                    const ArrayData* namedArgNames) {
  int32_t const numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  return static_cast<int32_t>(callee->numNamedParams()) - numNamedArgs;
}

template<typename ThrowUnexpectedNamedArgs,
         typename ThrowNamedArgMismatch, typename ThrowMissingNamedParam>
void checkNamedArgMismatch(const Func* callee, const ArrayData* namedArgNames,
                           ThrowUnexpectedNamedArgs throwUnexpectedNamedArgs,
                           ThrowNamedArgMismatch throwNamedArgMismatch,
                           ThrowMissingNamedParam throwMissingNamedParameter) {
  uint32_t namedParamCount = callee->numNamedParams();
  auto const namedArgNamesSize = namedArgNames == nullptr ? 0 : namedArgNames->size();
  if (callee->hasVariadicNamedParam()) {
    // An argument name matching no named parameter is absorbed by the
    // `named t...' parameter, so only missing required parameters are an error.
    mergeNamedArgs(
      callee, namedArgNames,
      [] (int32_t, int32_t) {},
      [&] (int32_t paramIdx) {
        if (callee->params()[paramIdx].hasDefaultValue()) return true;
        throwMissingNamedParameter(
          callee, callee->sortedNamedParamNames()[paramIdx]);
        return false;
      },
      [] (int32_t) {}
    );
    return;
  }
  if (namedParamCount == 0) {
    if (namedArgNamesSize > 0) {
      // This is a special case inserted to speed up the JIT - the JIT
      // short-circuits inspecting the named params at all if a function doesn't
      // have named params, so matching that behavior ensures the error messages
      // are consistent.
      throwUnexpectedNamedArgs(callee);
    }
    return;
  }
  auto const namedParamNames = callee->sortedNamedParamNames();

  auto paramIdx = 0;
  for (auto argIdx = 0; argIdx < namedArgNamesSize; ++argIdx) {
    auto argName = namedArgNames->at(argIdx).val().pstr;
    while (paramIdx < namedParamCount && namedParamNames[paramIdx] != argName) {
      if (!callee->params()[paramIdx].hasDefaultValue()) {
        throwMissingNamedParameter(callee, namedParamNames[paramIdx]);
        return;
      }
      ++paramIdx;
    }
    if (paramIdx >= namedParamCount) {
      throwNamedArgMismatch(callee, argName);
      return;
    }
    ++paramIdx;
  }
  while (paramIdx < namedParamCount) {
    if (!callee->params()[paramIdx].hasDefaultValue()) {
      throwMissingNamedParameter(callee, namedParamNames[paramIdx]);
      return;
    }
    ++paramIdx;
  }
}

/*
 * Given a stack top that points to the last passed argument for a callee,
 * re-arranges the stack to push numNamedParams - numNamedArgs uninits in
 * the right locations. If the callee expects reified generics, the stack
 * top is expected to point to the reified generics location.
 */
inline void initFuncNamedParamDefaultValues(const Func* callee,
                                            uint32_t posArgc,
                                            const ArrayData* namedArgNames,
                                            TypedValue* stackTop) {
  int32_t numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  bool hasVariadics = posArgc > callee->numPositionalParams();
  auto const numArgs = posArgc + numNamedArgs;
  assertx(callee->numNamedParams() >= numNamedArgs);
  if (callee->numNamedParams() == numNamedArgs) {
    return;
  }

  int32_t delta = callee->numNamedParams() - numNamedArgs;
  assertx(delta > 0);
  // We might have an extra variadics arg if we're in the
  // too-many-args prologue. hasVariadics will handle it properly.
  assertx(numArgs + delta <= callee->numParams() + 1);

  // This logic runs *after* the reified generics checks, meaning that for callees
  // with reified generics, the stack's top will be the reified generics type
  // structure. Move it to its final location.
  if (callee->hasReifiedGenerics()) {
    stackTop[-delta] = *stackTop;
    ++stackTop;
  }

  // We don't need to consider inouts here as they're mutually exclusive with named
  // params.
  // If there are no args present, this will point to the first unassigned location
  // after the stack top, which is the right place to push the first optional
  // named arg.
  auto firstArgLoc = stackTop + numArgs - 1;
  if (hasVariadics) {
    stackTop[-delta] = *stackTop;
  }

  PackedStringPtr const* namedParamNames = callee->sortedNamedParamNames();
  int32_t argIdx = numArgs - 1 - hasVariadics;
  for (int32_t paramIdx = posArgc + callee->numNamedParams() - 1 - hasVariadics; paramIdx >= 0; --paramIdx) {
    assertx(IMPLIES(paramIdx < callee->numNamedParams(), argIdx < numNamedArgs));
    if (paramIdx >= callee->numNamedParams() ||
        (argIdx >= 0 &&
         namedParamNames[paramIdx] == namedArgNames->at(argIdx).val().pstr)) {
      firstArgLoc[-paramIdx] = firstArgLoc[-argIdx];
      argIdx--;
    } else {
      auto const& DEBUG_ONLY param = callee->params()[paramIdx];
      assertx(param.hasDefaultValue());
      // Push uninits for any non-passed named params.
      firstArgLoc[-paramIdx] = make_tv<KindOfUninit>();
    }
  }
}

/*
 * The same rewrite as initFuncNamedParamDefaultValues(), for a callee declared
 * with a `named t...' parameter.
 *
 * That parameter has no name, so nothing in the body can read the arguments it
 * absorbs; they are dropped here. The caller must have allocated
 * namedParamStackDelta() cells when that is positive, and must discard that
 * many when it is negative.
 *
 * On return the region holds exactly numNamedParams() cells at the same
 * addresses initFuncNamedParamDefaultValues() would have placed them, so the
 * rest of func entry sees a layout it can index by parameter.
 */
inline void initVariadicNamedParamValues(const Func* callee,
                                         uint32_t posArgc,
                                         const ArrayData* namedArgNames,
                                         TypedValue* stackTop) {
  assertx(callee->hasVariadicNamedParam());
  int32_t const numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  int32_t const numNamedParams = callee->numNamedParams();
  int32_t const delta = numNamedParams - numNamedArgs;

  // Everything pushed after the named arguments: the positional arguments,
  // including the packed vec of the too-many-args prologue, plus the reified
  // generics cell on top of them.
  int32_t const above = posArgc + (callee->hasReifiedGenerics() ? 1 : 0);
  assertx(IMPLIES(callee->hasReifiedGenerics(), tvIsVec(stackTop)));

  // The interpreter exposes newly allocated cells through vmsp before this
  // helper runs, and the JIT helper can re-enter while dropping arguments.
  // Keep the entire live stack well-formed before any decref below.
  if (delta > 0) {
    for (int32_t i = 1; i <= delta; ++i) {
      stackTop[-i] = make_tv<KindOfUninit>();
    }
  }

  auto const argsBase = stackTop + above + numNamedArgs - 1;
  int32_t numMatched = 0;
  mergeNamedArgs(
    callee, namedArgNames,
    [&] (int32_t, int32_t argIdx) {
      auto const src = &argsBase[-argIdx];
      auto const dst = &argsBase[-numMatched++];
      if (src == dst) return;
      *dst = *src;
      *src = make_tv<KindOfUninit>();
    },
    [] (int32_t) { return true; },
    [&] (int32_t argIdx) {
      auto const arg = &argsBase[-argIdx];
      tvDecRefGen(arg);
      *arg = make_tv<KindOfUninit>();
    }
  );

  auto const newTop = stackTop - delta;
  if (delta > 0) {
    std::memmove(newTop, stackTop, above * sizeof(TypedValue));
  }

  auto const paramsBase = newTop + above + numNamedParams - 1;
  assertx(paramsBase == argsBase);
  auto const namedParamNames = callee->sortedNamedParamNames();
  auto const argNameAt = [&] (int32_t argIdx) {
    return namedArgNames->at(argIdx).val().pstr;
  };
  int32_t argIdx = numNamedArgs - 1;
  int32_t matchedIdx = numMatched - 1;
  for (int32_t paramIdx = numNamedParams - 1; paramIdx >= 0; --paramIdx) {
    auto const paramName = namedParamNames[paramIdx];
    while (argIdx >= 0 &&
           argNameAt(argIdx)->slice().compare(paramName->slice()) > 0) {
      --argIdx;
    }
    auto const dst = &paramsBase[-paramIdx];
    if (argIdx >= 0 && argNameAt(argIdx) == paramName) {
      auto const src = &argsBase[-matchedIdx--];
      if (src != dst) {
        *dst = *src;
        *src = make_tv<KindOfUninit>();
      }
      --argIdx;
    } else {
      assertx(callee->params()[paramIdx].hasDefaultValue());
      *dst = make_tv<KindOfUninit>();
    }
  }
  assertx(matchedIdx == -1);

  if (delta < 0) {
    std::memmove(newTop, stackTop, above * sizeof(TypedValue));
  }
}

}
