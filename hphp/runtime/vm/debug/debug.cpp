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

#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/mcgen.h"


#include "hphp/util/configs/eval.h"
#include "hphp/util/current-executable.h"
#include "hphp/util/roar.h"

#include <stdio.h>
#include <string.h>

#include <fmt/format.h>
#include <folly/debugging/symbolizer/Symbolizer.h>

#include <folly/portability/Unistd.h>
#include <folly/Demangle.h>

using namespace HPHP::jit;

namespace HPHP {
namespace Debug {
namespace { DebugInfo* s_info; }

void* DebugInfo::pidMapOverlayStart;
void* DebugInfo::pidMapOverlayEnd;

void initDebugInfo() {
  s_info = new DebugInfo();
}

void destroyDebugInfo() {
  if (s_info) {
    delete s_info;
    s_info = nullptr;
  }
}

DebugInfo* DebugInfo::Get() {
  return s_info;
}

DebugInfo::DebugInfo() {
  m_perfMapName = fmt::format("/tmp/perf-{}.map", getpid());
  if (Cfg::Eval::PerfPidMap) {
    m_perfMapEnabled = true;
    // When running with ROAR, ROAR owns the pid-map file, and HHVM writes to it
    // indirectly through ROAR.
    if (!use_roar) {
      m_perfMap = fopen(m_perfMapName.c_str(), "w");
      m_perfMapEnabled = m_perfMap != nullptr;
    }
  }
  if (Cfg::Eval::PerfJitDump) {
    initPerfJitDump();
  }
  m_dataMapName = fmt::format("/tmp/perf-data-{}.map", getpid());
  if (Cfg::Eval::PerfDataMap) {
    m_dataMap = fopen(m_dataMapName.c_str(), "w");
  }
  generatePidMapOverlay();
}

DebugInfo::~DebugInfo() {
  if (m_perfMap) {
    fclose(m_perfMap);
    if (!Cfg::Eval::KeepPerfPidMap) {
      unlink(m_perfMapName.c_str());
    }
  }

  if (m_perfJitDump) {
    closePerfJitDump();
    if (!Cfg::Eval::KeepPerfPidMap) {
      unlink(m_perfJitDumpName.c_str());
    }
  }

  if (m_dataMap) {
    fclose(m_dataMap);
    if (!Cfg::Eval::KeepPerfPidMap) {
      unlink(m_dataMapName.c_str());
    }
  }
}

void DebugInfo::generatePidMapOverlay() {
  if (!m_perfMapEnabled || !pidMapOverlayStart) return;

  struct SymInfo {
    const char* name;
    uintptr_t addr;
    size_t size;
  };
  std::vector<SymInfo> sorted;

  auto self = current_executable_path();
  using folly::symbolizer::ElfFile;
  ElfFile file;
  using ElfSym = ElfW(Sym);
  using ElfShdr = ElfW(Shdr);
  if (file.openNoThrow(self.c_str()) != ElfFile::kSuccess) return;
  file.iterateSectionsWithType(SHT_SYMTAB, [&] (const ElfShdr& section) {
    auto strings = file.getSectionByIndex(section.sh_link);
    if (!strings) return false;
    file.iterateSymbolsWithType(section, STT_FUNC, [&] (const ElfSym& sym) {
      if (sym.st_shndx == SHN_UNDEF) return false;
      if (!sym.st_name) return false;
      if (sym.st_value >= uintptr_t(pidMapOverlayStart) &&
          sym.st_value <= uintptr_t(pidMapOverlayEnd)) {
        sorted.push_back(SymInfo {
            file.getString(*strings, sym.st_name),
              sym.st_value,
              sym.st_size});
      }
      return false;
    });
    return false;
  });

  std::sort(
    sorted.begin(), sorted.end(),
    [](const SymInfo& a, const SymInfo& b) {
      if (a.addr != b.addr) return a.addr < b.addr;
      if (a.size != b.size) return a.size > b.size;
      return
        strncmp("_ZN4HPHP", a.name, 8) &&
        !strncmp("_ZN4HPHP", b.name, 8);
    }
  );

  for (size_t i = 0; i < sorted.size(); i++) {
    auto const& sym = sorted[i];
    auto demangled = folly::demangle(sym.name);
    unsigned size;
    if (i + 1 < sorted.size()) {
      auto const& s2 = sorted[i + 1];
      size = s2.addr - sym.addr;
    } else {
      size = uintptr_t(pidMapOverlayEnd) - sym.addr;
    }
    if (!size) continue;
    writeToPidMap(uint64_t(sym.addr), size, demangled.c_str());
  }

  return;
}

void DebugInfo::recordStub(TCRange range, const std::string& name) {
  if (range.isAcold()) {
    m_acoldDwarfInfo.addTracelet(range, name, nullptr, -1, false);
  } else {
    m_aDwarfInfo.addTracelet(range, name, nullptr, -1, false);
  }
}

extern "C"
int __roar_api_write_pid_map(uint64_t address, uint64_t size, const char* name);

void DebugInfo::writeToPidMap(uint64_t start, uint64_t size, const char* name) {
  if constexpr (use_roar) {
    __roar_api_write_pid_map(start, size, name);
  } else if (m_perfMapEnabled) {
    fprintf(m_perfMap, "%lx %lx %s\n", start, size, name);
    fflush(m_perfMap);
  }
}

void DebugInfo::recordPerfMap(TCRange range, SrcKey sk, std::string name,
                              jit::CGMeta* meta) {
  if (!m_perfMapEnabled) return;
  if (Cfg::Eval::ProfileBC) return;
  if (name.empty()) {
    name = lookupFunction(sk.func(), sk.prologue(),
                          Cfg::Eval::PerfPidMapIncludeFilePath);
  }
  auto const start = reinterpret_cast<uintptr_t>(range.begin());
  if (use_roar && meta != nullptr) {
    meta->roarPidMapEntries.push_back({start, range.size(), name});
  } else {
    writeToPidMap(start, range.size(), name.c_str());
  }

  //Dump the object code into the specified file
  if (m_perfJitDump) {
    perfJitDumpTrace(range.begin(), range.size(), name.c_str());
  }
}

void DebugInfo::recordBCInstr(TCRange range, uint32_t op, jit::CGMeta& meta) {
  static const char* opcodeName[] = {
#define O(name, imm, push, pop, flags) \
#name,
    OPCODES
#undef O
  };

  static const char* highOpcodeName[] = {
    "OpHighStart",
#define O(name) \
#name,
    HIGH_OPCODES
#undef O
  };


  if (Cfg::Eval::ProfileBC) {
    if (!m_perfMapEnabled) return;
    const char* name;
    if (op < Op_count) {
      name = opcodeName[op];
    } else {
      name = highOpcodeName[op - OpHighStart];
    }
    auto const start = reinterpret_cast<uintptr_t>(range.begin());
    if (use_roar) {
      meta.roarPidMapEntries.push_back({start, range.size(), name});
    } else {
      writeToPidMap(start, range.size(), name);
    }
  }
}

void DebugInfo::recordTracelet(TCRange range, SrcKey sk) {
  if (range.isAcold()) {
    m_acoldDwarfInfo.addTracelet(range, std::nullopt, sk.func(), sk.lineNumber(),
                                 sk.prologue());
  } else {
    m_aDwarfInfo.addTracelet(range, std::nullopt, sk.func(), sk.lineNumber(),
                             sk.prologue());
  }
}

void DebugInfo::recordDataMap(const void* from, const void* to,
                              const std::string& desc) {
  if (!mcgen::initialized()) return;
  if (auto dataMap = Get()->m_dataMap) {
    fprintf(dataMap, "%" PRIxPTR " %" PRIx64 " %s\n",
            uintptr_t(from),
            uint64_t((char*)to - (char*)from),
            desc.c_str());
    fflush(dataMap);
  }
}

void DebugInfo::debugSync() {
  m_aDwarfInfo.syncChunks();
  m_acoldDwarfInfo.syncChunks();
}

std::string lookupFunction(const Func* f,
                           bool inPrologue,
                           bool pseudoWithFileName) {
  // TODO: mangle the namespace and name?
  std::string fname("PHP::");
  if (pseudoWithFileName) {
    fname += f->unit()->origFilepath()->data();
    fname += "::";
  }
  if (!strcmp(f->name()->data(), "")) {
    // TODO: should no longer happen
    fname += "__pseudoMain";
    return fname;
  }
  if (f->isClosureBody()) {
    fname += f->baseCls()->name()->toCppString();
    fname = fname.substr(0, fname.find(';'));
    fname += "::__invoke";
  } else {
    fname += f->fullName()->data();
  }
  if (inPrologue) {
    fname += "$prologue";
  }
  return fname;
}

}
}
