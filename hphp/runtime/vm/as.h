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

#include <memory>
#include <stdexcept>
#include <string>

#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/vm/bc-pattern.h"
#include "hphp/runtime/vm/func-emitter.h"

#include <folly/Range.h>

namespace HPHP {

struct UnitEmitter;
struct FuncEmitter;
struct RepoAuthType;
struct SHA1;
struct Extension;

namespace Native {
struct FuncTable;
}

//////////////////////////////////////////////////////////////////////

/*
 * Assemble the contents of `filename' and return a UnitEmitter.
 *
 * If swallowErrors is true then emit a fataling unit for any assembler errors.
 *
 * Minimal documentation is available in as.cpp.
 */
std::unique_ptr<UnitEmitter> assemble_string(
  folly::StringPiece code,
  const char* filename,
  const SHA1&,
  const Extension*,
  const PackageInfo&,
  UnitEmitterAttributes,
  bool swallowErrors = true
);

struct AssemblerError : std::runtime_error {
  explicit AssemblerError(const std::string& msg) : std::runtime_error(msg) {}
  AssemblerError(int where, const std::string& what);
};

struct AssemblerUnserializationError : AssemblerError {
  using AssemblerError::AssemblerError;
};

/*
 * fixup_default_values: This function does a *rough* match of the default value
 * initializers for a function and attempts to construct corresponding default
 * TypedValues for them. It will also attempt to normalize the phpCode using a
 * variable serializer.
 */
template <typename T>
void fixup_default_values(T& state, FuncEmitter* fe) {
  using Atom = BCPattern::Atom;
  using Captures = BCPattern::CaptureVec;

  auto end = fe->bc() + fe->bcPos();
  for (uint32_t paramIdx = 0; paramIdx < fe->params.size(); ++paramIdx) {
    auto& pi = fe->params[paramIdx];
    if (!pi.hasDefaultValue() || pi.funcletOff == kInvalidOffset) continue;

    // Check that the DV initializer is actually setting the local for the
    // parameter being initialized.
    auto checkloc = [&] (PC pc, const Captures&) {
      auto const UNUSED op = decode_op(pc);
      assertx(op == OpSetL || op == OpPopL);
      auto const loc = decode_iva(pc);
      return loc == paramIdx;
    };

    // Look for DV initializers which push a primitive value onto the stack and
    // then immediately use it to set the parameter local and pop it from the
    // stack. Currently the following relatively limited sequences are accepted:
    //
    // Int | String | Double | Null | True | False | Vec | Dict | Keyset
    // SetL loc, PopC | PopL loc
    auto result = BCPattern {
      Atom::alt(
        Atom(OpInt), Atom(OpString), Atom(OpDouble), Atom(OpNull),
        Atom(OpTrue), Atom(OpFalse), Atom(OpVec), Atom(OpDict), Atom(OpKeyset)
      ).capture(),
      Atom::alt(
        Atom(OpPopL).onlyif(checkloc),
        Atom::seq(Atom(OpSetL).onlyif(checkloc), Atom(OpPopC))
      ),
    }.ignore(
      {OpAssertRATL, OpAssertRATStk, OpVerifyParamType}
    ).matchAnchored(fe->bc(), pi.funcletOff, fe->bcPos());

    // Verify that the pattern we matched is either for the last DV initializer,
    // in which case it must end with an Enter that targets the function entry,
    // or is immediately followed by the next DV initializer.
    if (!result.found() || result.getEnd() >= end) continue;
    auto pc = result.getEnd();
    auto off = pc - fe->bc();
    auto const valid = [&] {
      for (uint32_t next = paramIdx + 1; next < fe->params.size(); ++next) {
        auto& npi = fe->params[next];
        if (!npi.hasDefaultValue() || npi.funcletOff == kInvalidOffset) {
          continue;
        }
        return npi.funcletOff == off;
      }
      auto const orig = pc;
      auto const base = fe->bc();
      return decode_op(pc) == OpEnter && orig + decode_raw<Offset>(pc) == base;
    }();
    if (!valid) continue;

    // Use the captured initializer bytecode to construct the default value for
    // this parameter.
    auto capture = result.getCapture(0);
    assertx(capture);

    TypedValue dv = make_tv<KindOfUninit>();
    auto const decode_array = [&] {
      if (auto const arr = state.ue->lookupArrayId(decode_raw<uint32_t>(capture))) {
        dv.m_type = arr->toDataType();
        dv.m_data.parr = const_cast<ArrayData*>(arr);
      }
    };

    switch (decode_op(capture)) {
    case OpNull:   dv = make_tv<KindOfNull>();           break;
    case OpTrue:   dv = make_tv<KindOfBoolean>(true);    break;
    case OpFalse:  dv = make_tv<KindOfBoolean>(false);   break;
    case OpVec:    decode_array(); break;
    case OpDict:   decode_array(); break;
    case OpKeyset: decode_array(); break;
    case OpInt:
      dv = make_tv<KindOfInt64>(decode_raw<int64_t>(capture));
      break;
    case OpDouble:
      dv = make_tv<KindOfDouble>(decode_raw<double>(capture));
      break;
    case OpString:
      if (auto str = state.litstrMap[decode_raw<uint32_t>(capture)]) {
        dv = make_tv<KindOfString>(str);
      }
      break;
    default:
      always_assert(false);
    }

    // Use the variable serializer to construct a serialized version of the
    // default value, matching the behavior of hphpc.
    if (dv.m_type != KindOfUninit) {
      VariableSerializer vs(VariableSerializer::Type::PHPOutput);
      auto str = vs.serialize(tvAsCVarRef(&dv), true);
      pi.defaultValue = dv;
      pi.phpCode = state.ue->mergeLitstr(makeStaticString(str.get()));
    }
  }
}

void parse_default_value(Func::ParamInfo& param, Id id, const StringData* str);

// Sets output on success; throws on failure.
void ParseRepoAuthType(folly::StringPiece input, RepoAuthType& output);

//////////////////////////////////////////////////////////////////////

}
