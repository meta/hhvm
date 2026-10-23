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

#include "hphp/tools/tc-print/printir-annotation.h"

#include <limits>
#include <fmt/format.h>

using std::string;
using std::vector;

namespace {
using namespace folly;

string typeToName(dynamic::Type type) {
  switch(type) {
    case dynamic::Type::NULLT:  return "null";
    case dynamic::Type::ARRAY:  return "array";
    case dynamic::Type::BOOL:   return "boolean";
    case dynamic::Type::DOUBLE: return "double";
    case dynamic::Type::INT64:  return "int64";
    case dynamic::Type::OBJECT: return "object";
    case dynamic::Type::STRING: return "string";
  }
  always_assert(false);
}

void typeError(const std::string& name,
               const dynamic::Type expectedType,
               const dynamic::Type actualType) {
  throw HPHP::printir::ParseError(
    fmt::format("{} must have type {}, got type {}",
            name,
            typeToName(expectedType),
            typeToName(actualType)));
}

void enumError(const std::string& enumName,
               const std::string& incorrectValue) {
  throw HPHP::printir::ParseError(
    fmt::format("\"{}\" is not a valid value for enum {}",
            incorrectValue,
            enumName));
}

HPHP::jit::TCA parseTCA(const std::string& raw) {
  if (raw == "(null)") return nullptr;

  unsigned long long ull;
  if (std::sscanf(raw.c_str(), "%llx", &ull) != 1) {
    throw HPHP::printir::ParseError(
      fmt::format("Unable to parse value \"{}\" as TCA", raw));
  }

  return reinterpret_cast<jit::TCA>(ull);
}

}

namespace folly {
using namespace HPHP;

jit::Opcode DynamicConverter<jit::Opcode>::convert(const dynamic& d) {
  auto const rawOpcode = convertTo<string>(d);
  auto const maybeOpcode = jit::nameToOpcode(rawOpcode);
  if (!maybeOpcode) enumError("Opcode", rawOpcode);

  return *maybeOpcode;
}

printir::SSATmp DynamicConverter<printir::SSATmp>::convert(const dynamic& d) {
  if (!d.isObject()) {
    typeError("SSATmp", dynamic::Type::OBJECT, d.type());
  }

  auto const id = convertTo<uint32_t>(d.getDefault("id", {}));
  auto typeStr = convertTo<string>(d.getDefault("type", {}));

  return printir::SSATmp{id, std::move(typeStr)};
}

printir::PhiPseudoInstr DynamicConverter<printir::PhiPseudoInstr>::convert(
  const dynamic& d) {
  if (!d.isObject()) {
    typeError("PhiPseudoInstr", dynamic::Type::OBJECT, d.type());
  }

  vector<std::pair<printir::SSATmp, printir::BlockId>> srcs;
  for (auto const& item : d.getDefault("srcs", {})) {
    auto src = convertTo<printir::SSATmp>(item["src"]);
    auto const id = convertTo<printir::BlockId>(
      item.getDefault("label").getDefault("id", {}));

    srcs.emplace_back(std::move(src), id);
  }

  auto dst = convertTo<printir::SSATmp>(d.getDefault("dst", {}));

  return printir::PhiPseudoInstr{std::move(srcs), std::move(dst)};
}

printir::TCRange DynamicConverter<printir::TCRange>::convert(const dynamic& d) {
  if (!d.isObject()) {
    typeError("TCRange", dynamic::Type::OBJECT, d.type());
  }

  auto const rawArea = convertTo<string>(d.getDefault("area", {}));
  auto const maybeArea = jit::nameToAreaIndex(rawArea);
  if (!maybeArea) enumError("AreaIndex", rawArea);
  auto const area = *maybeArea;

  auto const start = parseTCA(convertTo<string>(d.getDefault("start", {})));
  auto const end = parseTCA(convertTo<string>(d.getDefault("end", {})));
  auto disasm = convertTo<string>(d.getDefault("disasm",{}));

  return printir::TCRange{area, start, end, std::move(disasm)};
}

printir::Profile DynamicConverter<printir::Profile>::convert(const dynamic& d) {
  auto const offset = convertTo<Offset>(d.getDefault("offset", {}));
  auto const name = convertTo<string>(d.getDefault("name", {}));
  auto const& data = d.getDefault("data");

  return printir::Profile{offset, name, data};
}

printir::Instr DynamicConverter<printir::Instr>::convert(const dynamic& d) {
  if (!d.isObject()) {
    typeError("Instr", dynamic::Type::OBJECT, d.type());
  }

  auto maybeMarker = convertTo<HPHP::Optional<string>>(
    d.getDefault("marker").getDefault("raw", {}));
  auto phiPseudoInstrs = convertTo<vector<printir::PhiPseudoInstr>>(
    d.getDefault("phiPseudoInstrs", dynamic::array));
  auto const opcode = convertTo<jit::Opcode>(d.getDefault("opcodeName", {}));
  auto typeParam = convertTo<HPHP::Optional<string>>(
    d.getDefault("typeParam", {}));
  auto guard = convertTo<HPHP::Optional<string>>(d.getDefault("guard", {}));
  auto extra = convertTo<HPHP::Optional<string>>(d.getDefault("extra", {}));
  auto const id = convertTo<uint32_t>(d.getDefault("id", {}));
  auto dsts = convertTo<vector<printir::SSATmp>>(
    d.getDefault("dsts", {}));
  auto const maybeTaken = d.getDefault("taken", {});
  auto taken = convertTo<HPHP::Optional<printir::BlockId>>(
    (maybeTaken.isNull() ? dynamic() : maybeTaken.getDefault("id", {})));
  auto const offset = convertTo<Offset>(d.getDefault("offset", {}));
  auto profileData = convertTo<vector<printir::Profile>>(
    d.getDefault("profileData", {}));
  auto const startLine = convertTo<int>(d.getDefault("startLine", {}));
  auto tcRanges = convertTo<vector<printir::TCRange>>(
    d.getDefault("tc_ranges", dynamic::array));
  for (auto& tcr : tcRanges) tcr.parentInstrId = id;

  // TODO(T52857257) - rewrite to maybe use std::variant?
  //                      other options to standardize?
  HPHP::Optional<vector<printir::SSATmp>> srcs;
  HPHP::Optional<string> counterName;
  auto const& d_srcs = d.getDefault("srcs", {});
  if (d_srcs.isArray()) {
    srcs = convertTo<vector<printir::SSATmp>>(d_srcs);
  } else if (d_srcs.isObject()) {
    counterName = convertTo<HPHP::Optional<string>>(
      d_srcs.getDefault("counterName", {}));
  } else {
    // Unique error case, no one "expected" type, but we know this current shape
    // is incorrect
    throw HPHP::printir::ParseError(
      fmt::format("Field \"srcs\" has incorrect type {}", d_srcs.typeName()));
  }

  return printir::Instr{id, std::move(maybeMarker), std::move(phiPseudoInstrs),
                        opcode, std::move(typeParam), std::move(guard),
                        std::move(extra), std::move(taken), std::move(tcRanges),
                        std::move(dsts), offset, std::move(profileData),
                        std::move(srcs), std::move(counterName), startLine};
}

printir::Block DynamicConverter<printir::Block>::convert(const dynamic& d) {
  if (!d.isObject()) typeError("Block", dynamic::Type::OBJECT, d.type());

  auto const& label = d.getDefault("label");
  auto const id = convertTo<printir::BlockId>(label.getDefault("id", {}));
  auto const isCatch = convertTo<bool>(label.getDefault("isCatch", {}));

  auto const rawHint = convertTo<string>(label.getDefault("hint", {}));
  auto const maybeHint = jit::nameToHint(rawHint);
  if (!maybeHint) enumError("Hint", rawHint);
  auto const hint = *maybeHint;

  auto const rawArea = convertTo<string>(d.getDefault("area", {}));
  auto const maybeArea = jit::nameToAreaIndex(rawArea);
  if (!maybeArea) enumError("AreaIndex", rawArea);
  auto const area = *maybeArea;

  auto const profCount = convertTo<uint64_t>(d.getDefault("profCount", {}));
  auto preds = convertTo<vector<printir::BlockId>>(
    d.getDefault("preds", {}));
  auto const maybeNext = d.getDefault("next", {});
  auto maybeNextId = convertTo<HPHP::Optional<printir::BlockId>>(
    (maybeNext.isNull() ? dynamic() : maybeNext.getDefault("id", {})));

  auto instrs = convertTo<vector<printir::Instr>>(d.getDefault("instrs", {}));
  for (auto& instr : instrs) {
    instr.parentBlockId = id;
    for (auto& phi : instr.phiPseudoInstrs) phi.parentBlockId = id;
    for (auto& tcr : instr.tcRanges) tcr.parentBlockId = id;
  }

  return printir::Block{id, isCatch, hint, profCount, std::move(preds),
                        std::move(maybeNextId), std::move(instrs), area};
}

printir::SrcKey DynamicConverter<printir::SrcKey>::convert(const dynamic& d) {
  if (!d.isObject()) typeError("SrcKey", dynamic::Type::OBJECT, d.type());

  auto funcStr = convertTo<string>(d.getDefault("func", {}));
  auto unitStr = convertTo<string>(d.getDefault("unit", {}));
  auto const prologue = convertTo<bool>(d.getDefault("prologue", {}));
  auto const offset = convertTo<int>(d.getDefault("offset", {}));
  auto const hasThis = convertTo<bool>(d.getDefault("hasThis", {}));

  auto const rawResumeMode = convertTo<string>(d.getDefault("resumeMode", {}));
  auto const maybeResumeMode = nameToResumeMode(rawResumeMode);
  if (!maybeResumeMode) enumError("ResumeMode", rawResumeMode);
  auto const resumeMode = *maybeResumeMode;
  auto const startLine = convertTo<int>(d.getDefault("startLine", {}));

  return printir::SrcKey{std::move(funcStr), std::move(unitStr), prologue,
                         offset, resumeMode, hasThis, startLine};
}

printir::TransContext DynamicConverter<printir::TransContext>::convert(
  const dynamic& d) {
  if (!d.isObject()) typeError("TransContext", dynamic::Type::OBJECT, d.type());

  auto const rawTransKind = convertTo<string>(d.getDefault("kind", {}));
  auto const maybeTransKind = jit::nameToTransKind(rawTransKind);
  if (!maybeTransKind) enumError("TransKind", rawTransKind);
  auto transKind = *maybeTransKind;

  auto const id = convertTo<TransID>(d.getDefault("id", {}));
  auto const optIndex = convertTo<int>(d.getDefault("optIndex", {}));
  auto srcKey = convertTo<printir::SrcKey>(d.getDefault("srcKey", {}));
  auto funcName = convertTo<string>(d.getDefault("funcName", {}));
  auto sourceFile = convertTo<string>(d.getDefault("sourceFile", {}));
  auto const startLine = convertTo<int>(d.getDefault("startLine", {}));
  auto const endLine = convertTo<int>(d.getDefault("endLine", {}));

  return printir::TransContext{std::move(transKind), id, optIndex,
                               std::move(srcKey), std::move(funcName),
                               std::move(sourceFile), startLine, endLine};
}

printir::InliningDecision DynamicConverter<printir::InliningDecision>::convert(
  const dynamic& d) {
  if (!d.isObject()) {
    typeError("InliningDecision", dynamic::Type::OBJECT, d.type());
  }

  auto const wasInlined = convertTo<bool>(d.getDefault("wasInlined", {}));
  auto const offset = convertTo<Offset>(d.getDefault("offset", {}));
  auto callerName = convertTo<HPHP::Optional<string>>(d.getDefault("caller", {}));
  auto calleeName = convertTo<HPHP::Optional<string>>(d.getDefault("callee", {}));
  auto reason = convertTo<string>(d.getDefault("reason", {}));

  return printir::InliningDecision{wasInlined, offset, std::move(callerName),
                                   std::move(calleeName), std::move(reason)};
}

printir::Unit DynamicConverter<printir::Unit>::convert(const dynamic& d) {
  if (!d.isObject()) typeError("Unit", dynamic::Type::OBJECT, d.type());

  auto transContext = convertTo<printir::TransContext>(
    d.getDefault("translation", {}));

  std::unordered_map<unsigned int, printir::Block> blocks;
  for (auto const& block : d.getDefault("blocks", {})) {
    auto const convertedBlock = convertTo<printir::Block>(block);
    auto const blockId = convertedBlock.id;
    auto const success = blocks.emplace(blockId,
                                        convertedBlock);
    if (!success.second) {
      throw HPHP::printir::ParseError(
        fmt::format("Block has repeated id: {}", blockId));
    }
  }

  auto inliningDecisions = convertTo<vector<printir::InliningDecision>>(
    d.getDefault("inliningDecisions", dynamic::array));

  return printir::Unit{std::move(transContext), std::move(blocks),
                       std::move(inliningDecisions)};
}

dynamic DynamicConstructor<jit::Opcode>::construct(const jit::Opcode& opc) {
  return jit::opcodeName(opc);
}

dynamic
DynamicConstructor<printir::SSATmp>::construct(const printir::SSATmp& s) {
  return dynamic::object("id", s.id)
                        ("type", s.type);
}

dynamic DynamicConstructor<printir::PhiPseudoInstr>::construct(
  const printir::PhiPseudoInstr& p) {
  return dynamic::object("srcs", toDynamic(p.srcs))
                        ("dst", toDynamic(p.dst));
}

dynamic DynamicConstructor<printir::TCRange>::construct(
  const printir::TCRange& t) {
  return dynamic::object("area", jit::areaAsString(t.area))
                        ("start", fmt::format("{}", static_cast<void*>(t.start)))
                        ("end", fmt::format("{}", static_cast<void*>(t.end)))
                        ("disasm", t.disasm);
}

dynamic DynamicConstructor<printir::Profile>::construct(
  const printir::Profile& p) {
  return dynamic::object("offset", p.offset)
                        ("name", p.name)
                        ("data", p.data);
}

dynamic DynamicConstructor<printir::Instr>::construct(const printir::Instr& i) {
  return dynamic::object("rawMarker", toDynamic(i.rawMarker))
                        ("phiPseudoInstrs", toDynamic(i.phiPseudoInstrs))
                        ("opcode", toDynamic(i.opcode))
                        ("typeParam", toDynamic(i.typeParam))
                        ("guard", toDynamic(i.guard))
                        ("extra", toDynamic(i.extra))
                        ("id", toDynamic(i.id))
                        ("taken", toDynamic(i.taken))
                        ("tcRanges", toDynamic(i.tcRanges))
                        ("dsts", toDynamic(i.dsts))
                        ("offset", i.offset)
                        ("startLine", i.startLine)
                        ("profileData", toDynamic(i.profileData))
                        ("srcs", toDynamic(i.srcs))
                        ("counterName", toDynamic(i.counterName));
}

dynamic DynamicConstructor<printir::Block>::construct(const printir::Block& b) {
  dynamic instrMap = dynamic::object();
  for (auto const& item: b.instrs) {
    instrMap[fmt::format("{}", item.id)] = toDynamic(item);
  }

  return dynamic::object("id", b.id)
                        ("isCatch", b.isCatch)
                        ("hint", jit::blockHintName(b.hint))
                        ("profCount", b.profCount)
                        ("next", toDynamic(b.next))
                        ("instrs", instrMap);
}

dynamic DynamicConstructor<printir::SrcKey>::construct(
  const printir::SrcKey& s) {
  return dynamic::object("funcStr", s.funcStr)
                        ("unitStr", s.unitStr)
                        ("prologue", s.prologue)
                        ("offset", s.offset)
                        ("resumeMode", resumeModeShortName(s.resumeMode))
                        ("hasThis", s.hasThis)
                        ("startLine", s.startLine);
}

dynamic DynamicConstructor<printir::TransContext>::construct(
  const printir::TransContext& t) {
  return dynamic::object("kind", show(t.kind))
                        ("id", t.id)
                        ("optIndex", t.optIndex)
                        ("srcKey", toDynamic(t.srcKey))
                        ("funcName", t.funcName)
                        ("sourceFile", t.sourceFile)
                        ("startLine", t.startLine)
                        ("endLine", t.endLine);
}

dynamic DynamicConstructor<printir::InliningDecision>::construct(
  const printir::InliningDecision& i) {
  return dynamic::object("wasInlined", i.wasInlined)
                        ("offset", i.offset)
                        ("callerName", toDynamic(i.callerName))
                        ("calleeName", toDynamic(i.calleeName))
                        ("reason", i.reason);
}

dynamic DynamicConstructor<printir::Unit>::construct(const printir::Unit &u) {
  dynamic blockMap = dynamic::object();
  for (auto const& item: u.blocks) {
    blockMap[fmt::format("{}", item.first)] = toDynamic(item.second);
  }

  return dynamic::object("transContext", toDynamic(u.transContext))
                        ("blocks", blockMap)
                        ("inliningDecisions", toDynamic(u.inliningDecisions));
}

} // namespace folly
