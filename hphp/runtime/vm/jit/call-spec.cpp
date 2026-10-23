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

#include "hphp/runtime/vm/jit/call-spec.h"

#include "hphp/runtime/vm/jit/arg-group.h"

#include "hphp/util/abi-cxx.h"

#include <fmt/format.h>
#include <folly/Range.h>

namespace HPHP::jit {

namespace {
std::string show_types(const std::vector<Type>& ts) {
  std::string ret = "{";
  auto sep = "";
  for (auto& t : ts) {
    fmt::format_to(std::back_inserter(ret), "{}{}", sep, t);
    sep = ", ";
  }
  return ret + "}";
}

template<typename F>
void verify_return_type(Type ret, const CallDest& dest, F fail) {
  if (dest.type == DestType::TV) {
    // We really want equality here: TCell corresponds to a full TypedValue
    // being returned.
    if (ret == TCell) return;
  } else {
    if (ret <= dest.valueType) return;

    // Back before the JIT knew how to deal with byte-sized registers, we held
    // TBool in a zero-extended, 8-byte register. A number of runtime helpers
    // still return TBool as int64_t or uint64_t, so allow that mismatch here.
    if (dest.valueType <= TBool && ret <= TInt) return;

    // Some JIT types are much more specific than what we can express in C++,
    // so treat certain classes of types as equivalent.
    static std::array<Type, 5> constexpr special_types = {
      TPtr,
      TLval,
      TObj,
      TStr,
      TArrLike,
    };
    for (auto t : special_types) {
      if (ret <= t && dest.valueType.maybe(t)) return;
    }

    // SetElem's helper returns a StringData* that is sometimes statically
    // known to be TNullptr. Pointers in hhir aren't allowed to be null, so the
    // normal Type for StringData* is Str. Check for this special case here
    // rather than making all users of StringData* suboptimal.
    if (ret <= TStr && dest.valueType <= TNullptr) return;
  }

  fail("Return type mismatch");
}
}

bool CallSpec::verifySignature(const CallDest& dest,
                               const std::vector<Type>& args) const {
  auto const type = m_typeKind.ptr();
  if (!type) return true;
  if (kind() != Kind::Direct && kind() != Kind::Smashable) return true;

  auto fail = [&]<typename... Args>(
                  fmt::format_string<Args...> fmtStr, Args&&... fmtArgs) {
    auto const why = fmt::format(fmtStr, std::forward<Args>(fmtArgs)...);
    auto const func = getNativeFunctionName(this->address());
    auto msg = fmt::format(
      "Failed to verify signature for call to {}: {}\n\n", func, why
    );
    fmt::format_to(std::back_inserter(msg), "Arguments:  {} -> {}\nSignature: {} -> {}",
      show_types(args), dest.valueType, show_types(type->params), type->ret
    );
    always_assert_flog(false, "{}", msg);
  };

  verify_return_type(type->ret, dest, fail);

  size_t argi = 0, parami = 0;
  for (; parami < type->params.size() && argi < args.size();
       ++parami, ++argi) {
    auto const param = type->params[parami];
    // TCell (for a TypedValue parameter) and wide TLval are special: one
    // SSATmp represents two argument registers, and the latter is passed as a
    // dummy TBottom argument. Make sure both are present.
    if (param == TCell || param == TLval) {
      if (!(args[argi] <= param)) {
        fail("Incompatible type {} for first half of {} parameter {}",
             args[argi], param, parami);
      }
      if (++argi == args.size()) break;
      if (args[argi] != TBottom) {
        fail(
          "Incompatible type {} for second half of {} parameter {}",
          args[argi], param, parami
        );
      }
    } else if (!(args[argi] <= param)) {
      // A few instructions pass Cls|Nullptr to helpers that take
      // Class*. Handle that special case here.
      if (param <= TCls && args[argi].maybe(TNullptr)) continue;
      // Similarly for ArrayData|Nullptr
      if (param <= TArrLike && args[argi].maybe(TNullptr)) continue;
      // Similarly for Obj|Nullptr
      if (param <= TObj && args[argi].maybe(TNullptr)) continue;
      // Similarly for TPtr|Nullptr
      if (param <= TPtr && args[argi].maybe(TNullptr)) continue;
      // LdObjMethodS takes a TSmashable as uintptr_t.
      if (param <= TInt && args[argi] <= TSmashable) continue;
      fail(
        "Incompatible type {} for {} parameter {}", args[argi], param, parami
      );
    }
  }

  if (parami != type->params.size() || argi != args.size()) {
    fail("Mismatch between argument and parameter counts");
  }

  return true;
}

}
