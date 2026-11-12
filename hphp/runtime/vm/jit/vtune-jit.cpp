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

#include "hphp/runtime/vm/jit/vtune-jit.h"
#include "hphp/runtime/vm/jit/vtune/jitprofiling.h"

#include <vector>
#include <algorithm>

namespace HPHP::jit {

// Method ids reported to Vtune JIT API should not be less than 1000 (see
// the comment in iJIT_Method_Load structure definition.) We use 1000 for
// trampolines and larger values for normal functions.
static const int MIN_HELPER_ID = 1000;
static const int MIN_METHOD_ID = 2000;

void reportTraceletToVtune(const Unit* unit,
                           const Func* func,
                           const TransRec& tr) {
  iJIT_Method_Load methodInfo;
  memset(&methodInfo, 0, sizeof(methodInfo));

  if (!unit) return;

  methodInfo.method_id = PackedPtr<const Func>::toRaw(tr.src.func());
  assertx(methodInfo.method_id > MIN_METHOD_ID);

  if (func && func->fullName()) {
    char *name = const_cast<char *>(func->fullName()->data());
    if (!strcmp(name, "")) {
      // VTune doesn't like the empty string for a name; it
      // causes it to not record the source file either.
      name = "__pseudoMain";
    }
    methodInfo.method_name = name;
  } else {
    methodInfo.method_name = const_cast<char *>("unknown");
  }

  methodInfo.source_file_name =
    const_cast<char *>(unit->origFilepath()->data());

  // aStart field of tr.bcmapping may point to cold range, so we need to
  // explicitly form mappings for main code and cold

  std::vector<LineNumberInfo> mainLineMap, coldLineMap;

  for (auto& mapping : tr.bcMapping) {
    LineNumberInfo info;

    info.LineNumber = mapping.sk.lineNumber();

    // Note that main code may be generated in the cold code range (see
    // emitBlock in code-gen-x64 genCodeImpl()) so we need to explicitly check
    // the aStart value.
    if (mapping.aStart >= tr.aStart &&
        mapping.aStart < tr.aStart + tr.aLen) {
      info.Offset = mapping.aStart - tr.aStart;
      mainLineMap.push_back(info);
    } else if (mapping.aStart >= tr.acoldStart &&
               mapping.aStart < tr.acoldStart + tr.acoldLen) {
      info.Offset = mapping.aStart - tr.acoldStart;
      coldLineMap.push_back(info);
    }

    info.Offset = mapping.acoldStart - tr.acoldStart;
    coldLineMap.push_back(info);
  }

  auto infoComp = [&](const LineNumberInfo& a,
                      const LineNumberInfo& b) -> bool {
    return a.Offset < b.Offset;
  };

  std::sort(mainLineMap.begin(), mainLineMap.end(), infoComp);
  std::sort(coldLineMap.begin(), coldLineMap.end(), infoComp);

  // Note that at this moment LineNumberInfo structures contain pairs of lines
  // and code offset for the start of the corresponding code, while JIT API
  // treats the offset as the end of this code (and the start offset is taken
  // from the previous element or is 0); need to shift the elements. Also,
  // attribute the prologue (code before the first byte in the mapping) to the
  // first line.

  auto shiftLineMap = [&](std::vector<LineNumberInfo>& lineMap,
                          unsigned regionSize) {
    if (lineMap.size() > 0) {
      LineNumberInfo tmpInfo;
      tmpInfo.Offset = regionSize;
      tmpInfo.LineNumber = lineMap.back().LineNumber;
      lineMap.push_back(tmpInfo);
      for (size_t i = lineMap.size() - 2; i > 0; i--) {
        lineMap[i].LineNumber = lineMap[i - 1].LineNumber;
      }
    }
  };

  shiftLineMap(mainLineMap, tr.aLen);
  shiftLineMap(coldLineMap, tr.acoldLen);

  // Report main body

  methodInfo.method_load_address = tr.aStart;
  methodInfo.method_size = tr.aLen;
  methodInfo.line_number_size = mainLineMap.size();
  methodInfo.line_number_table = &mainLineMap[0];

  iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void *)&methodInfo);

  // Report cold

  methodInfo.method_load_address = tr.acoldStart;
  methodInfo.method_size = tr.acoldLen;
  methodInfo.line_number_size = coldLineMap.size();
  methodInfo.line_number_table = &coldLineMap[0];

  iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void *)&methodInfo);
}

void reportHelperToVtune(const char *name,
                         void *start,
                         void *end) {
  static unsigned int helperNumber = MIN_HELPER_ID;
  iJIT_Method_Load methodInfo;
  memset(&methodInfo, 0, sizeof(methodInfo));

  assertx(helperNumber < MIN_METHOD_ID);
  methodInfo.method_id = helperNumber++;

  methodInfo.method_name = const_cast<char *>(name);
  methodInfo.method_load_address = start;
  methodInfo.method_size = (char*)end - (char*)start;

  iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void *)&methodInfo);
}

}
