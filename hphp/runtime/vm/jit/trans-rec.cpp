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

#include <fmt/format.h>

#include "hphp/runtime/vm/jit/trans-rec.h"

#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/func.h"

namespace HPHP { namespace jit {

TransRec::TransRec(SrcKey                      _src,
                   TransID                     transID,
                   TransKind                   _kind,
                   TCA                         _aStart,
                   uint32_t                    _aLen,
                   TCA                         _acoldStart,
                   uint32_t                    _acoldLen,
                   TCA                         _afrozenStart,
                   uint32_t                    _afrozenLen,
                   Annotations&&               _annotations,
                   RegionDescPtr               region,
                   std::vector<TransBCMapping> _bcMapping,
                   bool                        _hasLoop)
  : bcMapping(_bcMapping)
  , annotations(std::move(_annotations))
  , funcName(_src.func()->fullName()->data())
  , src(_src)
  , sha1(_src.func()->unit()->sha1())
  , sn(_src.func()->unit()->sn())
  , aStart(_aStart)
  , acoldStart(_acoldStart)
  , afrozenStart(_afrozenStart)
  , aLen(_aLen)
  , acoldLen(_acoldLen)
  , afrozenLen(_afrozenLen)
  , id(transID)
  , kind(_kind)
  , hasLoop(_hasLoop)
{
  if (funcName.empty()) funcName = "Pseudo-main";

  if (!region) return;

  assertx(!region->empty());
  for (auto& block : region->blocks()) {
    auto sk = block->start();
    blocks.emplace_back(Block {
      sk.unit()->sha1(),
      sk,
      block->last().advanced().offset()
    });
  }

  auto& firstBlock = *region->blocks().front();
  for (auto const& pred : firstBlock.typePreConditions()) {
    guards.emplace_back(show(pred));
  }
}

void TransRec::optimizeForMemory() {
  // Dump large annotations to disk.
  for (int i = 0 ; i < annotations.size(); ++i) {
    auto& annotation = annotations[i];
    if (annotation.second.find_first_of('\n') != std::string::npos ||
        annotation.second.size() > 72) {
      auto saved = writeAnnotation(annotation, /* compress */ true);
      if (saved.length > 0) {
        // Strip directory name from the file name.
        size_t pos = saved.fileName.find_last_of('/');
        if (pos != std::string::npos) {
          saved.fileName = saved.fileName.substr(pos+1);
        }
        auto newAnnotation =
          fmt::format(
            "file:{}:{}:{}",
            saved.fileName, saved.offset, saved.length);
        std::swap(annotation.second, newAnnotation);
      } else {
        annotation.second = "<unknown: write failed>";
      }
    }
  }
}

TransRec::SavedAnnotation
TransRec::writeAnnotation(const Annotation& annotation, bool compress) {
  static jit::fast_map<std::string, size_t> fileWritten;
  SavedAnnotation saved = {
    fmt::format("{}/tc_annotations.txt{}",
                   Cfg::Eval::DumpTCPath,
                   compress ? ".gz" : ""),
    0,
    0
  };
  auto const fileName = saved.fileName.c_str();

  if (fileWritten.emplace(saved.fileName, 0).second) {
    unlink(fileName);
  }

  auto& fileOffset = fileWritten.at(saved.fileName);

  FILE* file = fopen(fileName, "a");
  if (!file) return saved;
  saved.offset = fileOffset;
  if (saved.offset == (off_t)-1) {
    fclose(file);
    return saved;
  }
  auto const& content = annotation.second;
  if (compress) {
    gzFile compressedFile = gzdopen(fileno(file), "a");
    if (!compressedFile) {
      fclose(file);
      return saved;
    }
    auto rv = gzputs(compressedFile, content.c_str());
    if (rv > 0) saved.length = rv;
    gzclose(compressedFile);
  } else {
    if (fputs(content.c_str(), file) >= 0) {
      saved.length = content.length();
    }
    fclose(file);
  }
  fileOffset += saved.length;

  return saved;
}

bool TransRec::isConsistent() const {
  if (!isValid()) return true;

  const auto aEnd       = aStart       + aLen;
  const auto acoldEnd   = acoldStart   + acoldLen;
  const auto afrozenEnd = afrozenStart + afrozenLen;

  for (const auto& b : bcMapping) {
    if (b.aStart < aStart             || b.aStart > aEnd         ||
        b.acoldStart < acoldStart     || b.acoldStart > acoldEnd ||
        b.afrozenStart < afrozenStart || b.afrozenStart > afrozenEnd) {
      return false;
    }
  }
  return true;
}

bool TransRec::contains(TCA tca) const {
  if (!isValid()) return false;
  if (aStart <= tca && tca - aStart < aLen) return true;
  if (acoldStart <= tca && tca - acoldStart < acoldLen) return true;
  if (afrozenStart <= tca && tca - afrozenStart < afrozenLen) return true;
  return false;
}

std::string TransRec::print() const {
  if (!isValid()) return "Translation -1 {\n}\n\n";

  std::string ret;
  std::string funcName = src.func()->fullName()->data();

  // Split up the call to prevent template explosion
  fmt::format_to(std::back_inserter(ret), "Translation {} {{\n"
    "  src.sha1 = {}\n"
    "  src.sn = {}\n"
    "  src.funcName = {}\n"
    "  src.key = {}\n"
    "  src.blocks = {}\n",
    id, sha1, sn, funcName, src.toAtomicInt(),
    blocks.size());

  for (auto const& block : blocks) {
    auto sk = block.sk;
    fmt::format_to(std::back_inserter(ret), "    {} {} {} {} {}\n",
      sk.unit()->sha1(),
      sk.unit()->sn(),
      sk.func()->sn(),
      sk.toAtomicInt(),
      block.bcPast
    );
  }

  fmt::format_to(std::back_inserter(ret), "  src.guards = {}\n", guards.size());

  for (auto const& guard : guards) {
    fmt::format_to(std::back_inserter(ret), "    {}\n", guard);
  }

  fmt::format_to(std::back_inserter(ret), "  kind = {} ({})\n"
    "  hasLoop = {:d}\n"
    "  aStart = {}\n"
    "  aLen = {:#x}\n"
    "  coldStart = {}\n"
    "  coldLen = {:#x}\n"
    "  frozenStart = {}\n"
    "  frozenLen = {:#x}\n",
    static_cast<uint32_t>(kind), show(kind),
    hasLoop,
    fmt::ptr(aStart), aLen,
    fmt::ptr(acoldStart), acoldLen,
    fmt::ptr(afrozenStart), afrozenLen);

  // Prepend any target profile data to annotations list.
  if (auto const profD = profData()) {
    auto targetProfs = profD->getTargetProfiles(id);
    fmt::format_to(std::back_inserter(ret), "  annotations = {}\n",
                  annotations.size() + targetProfs.size());
    for (auto const& tProf : targetProfs) {
      fmt::format_to(std::back_inserter(ret), "     [\"TargetProfile {}: {}\"] = {}\n",
                    tProf.key.bcOff, tProf.key.name->data(), tProf.debugInfo);
    }
  } else {
    fmt::format_to(std::back_inserter(ret), "  annotations = {}\n", annotations.size());
  }
  for (auto const& annotation : annotations) {
    fmt::format_to(std::back_inserter(ret), "     [\"{}\"] = {}\n",
                  annotation.first, annotation.second);
  }

  fmt::format_to(std::back_inserter(ret), "  bcMapping = {}\n", bcMapping.size());

  for (auto const& info : bcMapping) {
    auto sk = info.sk;
    fmt::format_to(std::back_inserter(ret), "    {} {} {} {} {} {} {}\n",
      sk.unit()->sha1(),
      sk.unit()->sn(),
      sk.func()->sn(),
      sk.toAtomicInt(),
      fmt::ptr(info.aStart),
      fmt::ptr(info.acoldStart),
      fmt::ptr(info.afrozenStart)
    );
  }

  ret += "}\n\n";
  return ret;
}

Offset TransRec::bcPast() const {
  return blocks.empty() ? 0 : blocks.back().bcPast;
}

} }
