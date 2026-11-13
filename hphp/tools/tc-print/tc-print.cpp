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

#include "hphp/tools/tc-print/tc-print.h"

#include <getopt.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include <fmt/core.h>
#include <folly/json/dynamic.h>
#include <folly/json/DynamicConverter.h>
#include <folly/json/json.h>
#include <folly/Singleton.h>

#include "hphp/util/build-info.h"

#include "hphp/runtime/base/preg.h"
#include "hphp/runtime/base/program-functions.h"

#include "hphp/tools/tc-print/perf-events.h"
#include "hphp/tools/tc-print/printir-annotation.h"
#include "hphp/tools/tc-print/offline-trans-data.h"
#include "hphp/tools/tc-print/offline-code.h"
#include "hphp/tools/tc-print/mappers.h"
#include "hphp/tools/tc-print/repo-wrapper.h"
#include "hphp/tools/tc-print/std-logger.h"
#include "hphp/tools/tc-print/tc-print-logger.h"
#ifdef HHVM_FACEBOOK
#include "hphp/facebook/extensions/scribe/ext_scribe.h"
#include "hphp/tools/tc-print/facebook/db-logger.h"
#endif

using namespace HPHP;
using namespace HPHP::jit;

#define MAX_SYM_LEN       10240

std::string     dumpDir("/tmp");
std::string     configFile;
std::string     profFileName;
std::string     repoFileName;
uint32_t        nTopTrans       = 0;
uint32_t        nTopFuncs       = 0;
bool            useJSON         = false;
bool            creationOrder   = false;
bool            transCFG        = false;
bool            collectBCStats  = false;
bool            inclusiveStats  = false;
bool            verboseStats    = false;
bool            hostOpcodes     = false;
Optional<SHA1> sha1Filter;
PerfEventType   sortBy          = EVENT_CYCLES;
bool            sortByDensity   = false;
bool            sortBySize      = false;
double          helpersMinPercentage = 0;
ExtOpcode       filterByOpcode  = 0;
std::string     kindFilter      = "all";
uint32_t        selectedFuncId  = INVALID_ID;
std::string     selectedFuncName;
TCA             minAddr         = 0;
TCA             maxAddr         = (TCA)-1;
uint32_t        annotationsVerbosity = 2;
uint32_t        numThreads      = 0;
DisasmArch      selectedArch    = hostDisasmArch();
#ifdef HHVM_FACEBOOK
bool            printToDB       = false;
std::string     hiveTable;
#endif

std::vector<uint32_t> transPrintOrder;

RepoWrapper*      g_repo;
OfflineTransData* g_transData;
OfflineCode*   transCode;

std::unique_ptr<AnnotationCache> g_annotations;

const char* kListKeyword = "list";
TCPrintLogger* g_logger;

PerfEventsMap<TCA>     tcaPerfEvents;
PerfEventsMap<TransID> transPerfEvents;

#define NTRANS        (g_transData->getNumTrans())
#define NFUNCS        (g_transData->getNumFuncs())
#define TREC(TID)     (g_transData->getTransRec(TID))

void warnTooFew(const std::string& name,
                uint32_t requested,
                uint32_t available) {
  fprintf(stderr,
          "Requested top %u %s, but there are only %u available.\n",
          requested,
          name.c_str(),
          available);
}

template<typename T>
std::string toString(T value) {
  std::stringstream ss;
  ss << value;
  return ss.str();
}

void usage() {
  printf("Usage: tc-print [OPTIONS]\n"
    "  Options:\n"
    "    -D              : used along with -t, this option sorts the top "
    "translations by density (count / size) of the selected perf event\n"
    "    -d <DIRECTORY>  : looks for dump file in <DIRECTORY> "
    "(default: /tmp)\n"
    "    -c <CONFIG.HDF> : uses the specified config file\n"
    "    -F <FUNC_NAME>  : prints the translations for the given function\n"
    "    -f <FUNC_ID>    : prints the translations for the given "
    "<FUNC_ID>, sorted by start offset\n"
    "    -g <FUNC_ID>    : prints the CFG among the translations for the "
    "given <FUNC_ID>\n"
    "    -p <FILE>       : uses raw profile data from <FILE>\n"
    "    -r <REPO_FILE>  : specifies the bytecode repo file to use\n"
    "    -s              : prints all translations sorted by creation "
    "order\n"
    "    -u <SHA1>       : prints all translations from the specified "
    "unit\n"
    "    -t <NUMBER>     : prints top <NUMBER> translations according to "
    "profiling info\n"
    "    -k <TRANS_KIND> : used with -t, filters only translations of the "
    "given kind, e.g. TransLive (default: all)\n"
    "    -a <ADDR>       : used with -t, filters only events at addresses "
    ">= <ADDR>\n"
    "    -A <ADDR>       : used with -t, filters only events at addresses "
    "<= <ADDR>\n"
    "    -T <NUMBER>     : prints top <NUMBER> functions according to "
    "profiling info\n"
    "    -e <EVENT>      : sorts by the specified perf event. Should either "
    "be one of the predefined events or one passed with -E.\n"
    "                      Pass '%s' to get a list of valid predefined event "
    "types.\n"
    "    -E <EVENTS>     : specifies a new replacement set of perf events. "
    "Pass a comma-separated list of events to replace the default set.\n"
    "    -b              : prints bytecode stats\n"
    "    -B <OPCODE>     : used in conjunction with -e, prints the top "
    "bytecode translation event type. Pass '%s' to get a "
    "list of valid opcodes.\n"
    "    -i              : reports inclusive stats by including helpers "
    "(perf data must include call graph information)\n"
    "    -n <level>      : level of verbosity for annotations. Use 0 for "
    "no annotations, 1 - for inline, 2 - to print all annotations "
    "including from a file (default: 2).\n"
    "    -o              : print host opcodes\n"
    "    -v <PERCENTAGE> : sets the minimum percentage to <PERCENTAGE> "
    "when printing the top helpers (implies -i). The lower the percentage,"
    " the more helpers that will show up.\n"
    "    -P <THREADS>    : number of threads for parallel output "
    "(default: auto-detect, 1 = sequential)\n"
    "    -j              : outputs tc-dump in JSON format (not compatible with "
    "some other flags).\n"
    // TODO(T52857399) - investigate compatibility with other flags
    #ifdef HHVM_FACEBOOK
    "    -H <HIVE_TABLE> : used with -j, write the JSON output to Hive in the "
    "table <HIVE_TABLE>\n"
    "    -x              : log translations to database\n"
    #endif
    "    --arch <ARCH>   : disassemble as <ARCH> "
    "(aarch64/arm64/a64 or x64/x86_64, default: host)\n"
    "    -h              : prints help message\n",
    kListKeyword,
    kListKeyword);
}

void printValidBytecodes() {
  g_logger->printGeneric("<OPCODE>:\n");
  auto validOpcodes = getValidOpcodeNames();
  for (size_t i = 0; i < validOpcodes.size(); i++) {
    g_logger->printGeneric("  * %s\n", validOpcodes[i].first.c_str());
  }
}

void printValidEventTypes() {
  g_logger->printGeneric("<EVENT>:\n");
  // Note: The -e list option is only for printing the predefined (default)
  //       event types; thus ranging from [0,NUM_PREDEFINED_EVENT_TYPES).
  for (size_t i = 0; i < NUM_PREDEFINED_EVENT_TYPES; i++) {
    g_logger->printGeneric("  * %s\n",
                           eventTypeToCommandLineArgument((PerfEventType)i));
  }
}

void parseOptions(int argc, char *argv[]) {
  int c;
  opterr = 0;
  char* sortByArg = nullptr;

  static struct option long_options[] = {
    {"arch", required_argument, 0, 0},
    {0, 0, 0, 0}
  };

  int option_index = 0;
  while ((c = getopt_long(argc, argv,
                     ":hDd:F:f:G:g:ip:st:u:S:T:o:r:e:E:bB:v:k:a:A:n:jP:H:xc:",
                     long_options, &option_index))
         != -1) {
    switch (c) {
      case 0: {
        // Long option
        std::string optName = long_options[option_index].name;
        if (optName == "arch") {
          std::string archStr = optarg;
          // Case-insensitive comparison
          std::transform(archStr.begin(), archStr.end(), archStr.begin(),
                         ::tolower);
          if (archStr == "aarch64" || archStr == "arm64" || archStr == "a64") {
            selectedArch = DisasmArch::A64;
          } else if (archStr == "x64" || archStr == "x86_64") {
            selectedArch = DisasmArch::X64;
          } else {
            fprintf(stderr, "Error: unknown architecture '%s'\n", optarg);
            usage();
            exit(1);
          }
        }
        break;
      }
      case 'A':
        if (sscanf(optarg, "%p", &maxAddr) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'a':
        if (sscanf(optarg, "%p", &minAddr) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'h':
        usage();
        exit(0);
      case 'd':
        dumpDir = optarg;
        break;
      case 'c':
        configFile = optarg;
        break;
      case 'F':
        creationOrder = true;
        selectedFuncName = optarg;
        break;
      case 'f':
        creationOrder = true;
        if (sscanf(optarg, "%u", &selectedFuncId) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'G':
        transCFG = true;
        selectedFuncName = optarg;
        break;
      case 'g':
        transCFG = true;
        if (sscanf(optarg, "%u", &selectedFuncId) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'p':
        profFileName = optarg;
        break;
      case 'r':
        repoFileName = optarg;
        break;
      case 's':
        creationOrder = true;
        break;
      case 't':
        if (sscanf(optarg, "%u", &nTopTrans) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'u':
        if (strlen(optarg) == 32) {
          sha1Filter = SHA1(optarg);
        } else {
          usage();
          exit(1);
        }
        break;
      case 'S':
        sortBySize = true;
        [[fallthrough]];
      case 'T':
        if (sscanf(optarg, "%u", &nTopFuncs) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'k':
        kindFilter = optarg;
        break;
      case 'D':
        sortByDensity = true;
        break;
      case 'e':
        if (!strcmp(optarg, kListKeyword)) {
          printValidEventTypes();
          exit(0);
        }
        // Will lookup event type after parsing all args
        sortByArg = optarg;
        break;
      case 'E':
        {
          char* p = strtok(optarg, ",");
          while (p) {
            addEventType(std::string(p));
            p = strtok(nullptr, ",");
          }
        }
        break;
      case 'b':
        collectBCStats = true;
        break;
      case 'B':
        if (!strcmp(optarg, kListKeyword)) {
          printValidBytecodes();
          exit(0);
        }
        filterByOpcode = stringToExtOpcode(optarg);
        if (!filterByOpcode) {
          usage();
          exit(1);
        }
        break;
      case 'i':
        inclusiveStats = true;
        break;
      case 'n':
        if (sscanf(optarg, "%u", &annotationsVerbosity) != 1) {
          usage();
          exit(1);
        }
        break;
      case 'v':
        verboseStats = true;
        // -v implies -i
        inclusiveStats = true;
        if (sscanf(optarg, "%lf", &helpersMinPercentage) != 1) {
          usage();
          exit(1);
        }
        break;
      case ':':
        // Missing argument for any short options
        fprintf (stderr, "Error: %s expects an argument\n\n", argv[optind - 1]);
        usage();
        exit(1);
      case '?':
        // Unrecognized option, short or long
        fprintf (stderr, "Error: unrecognized option %s\n\n", argv[optind - 1]);
        usage();
        exit(1);
      case 'o':
        hostOpcodes = true;
        break;
      case 'j':
        useJSON = true;
        break;
      case 'P':
        if (sscanf(optarg, "%u", &numThreads) != 1) {
          usage();
          exit(1);
        }
        break;
      #ifdef HHVM_FACEBOOK
      case 'x':
        printToDB = true;
        break;
      case 'H':
        hiveTable = optarg;
        break;
      #endif
      default:
        usage();
        exit(1);
    }
  }

  // Lookup the event type of a event to sort by was specified with -e
  if (sortByArg) {
    sortBy = commandLineArgumentToEventType(sortByArg);
    if (sortBy < getFirstEventType() || sortBy >= getNumEventTypes()) {
      usage();
      exit(1);
    }
  } else {
    // If not specified, then default the sorting to the first event type.
    // If using predefined events, then EVENT_CYCLES.
    sortBy = static_cast<PerfEventType>(getFirstEventType());
  }
}

void sortTrans() {
  for (uint32_t tid = 0; tid < NTRANS; tid++) {
    const auto trec = TREC(tid);
    if (trec->isValid() &&
        (selectedFuncId == INVALID_ID ||
         selectedFuncId == FuncId{trec->src.func()}.toInt()) &&
        (kindFilter == "all" || kindFilter == show(trec->kind).c_str())) {
      transPrintOrder.push_back(tid);
    }
  }
  std::sort(transPrintOrder.begin(), transPrintOrder.end(),
            [] (uint32_t tid1, uint32_t tid2) {
              auto const tr1 = TREC(tid1);
              auto const tr2 = TREC(tid2);

              if (tr1->aStart != tr2->aStart) {
                return std::less<void*>{}(tr1->aStart, tr2->aStart);
              }
              return tid1 < tid2;
            });
}

void loadPerfEvents() {
  FILE* profFile;

  profFile = fopen(profFileName.c_str(), "rt");

  if (!profFile) {
    error("Error opening file " + profFileName);
  }

  char   program[MAX_SYM_LEN];
  char   eventCaption[MAX_SYM_LEN];
  char   line[2*MAX_SYM_LEN];
  TCA    addr;
  vector<uint32_t> tcSamples(getNumEventTypes(), 0);
  vector<uint32_t> hhvmSamples(getNumEventTypes(), 0);
  size_t numEntries = 0;
  PerfEventType eventType = EVENT_NULL;
  // samplesPerKind[event][kind], samplesPerTCRegion[event][TCRegion]
  vector< vector<uint32_t> > samplesPerKind(getNumEventTypes(),
                                            vector<uint32_t>(NumTransKinds, 0));
  vector< vector<uint32_t> > samplesPerTCRegion(getNumEventTypes(),
                                                vector<uint32_t>(TCRCount, 0));

  while (fgets(line, 2*MAX_SYM_LEN, profFile) != nullptr) {
    always_assert(sscanf(line, "%s %s %lu", program, eventCaption, &numEntries)
                  == 3);
    always_assert(numEntries);

    std::vector<std::pair<TCA,std::string>> entries;

    for (size_t i = 0; i < numEntries; i++) {
      fscanf(profFile, "%p %s\n", &addr, line);
      entries.push_back(std::pair<TCA,std::string>(addr, line));
    }

    // Strip :p+ PEBS suffix.
    if (auto pos = strchr(eventCaption, ':')) {
      *pos = '\0';
    }

    if (strncmp(program, "hhvm", 4) == 0) {
      eventType = perfScriptOutputToEventType(eventCaption);
      if (eventType == EVENT_NULL) {
        error(fmt::format("loadProfData: invalid event caption {}",
                             eventCaption));
      }

      hhvmSamples[eventType]++;

      size_t selIdx = 0;
      addr = entries[0].first;

      if (inclusiveStats) {
        for (size_t i = 0; i < entries.size(); i++) {
          if (g_transData->isAddrInSomeTrans(entries[i].first)) {
            addr = entries[i].first;
            selIdx = i;
            break;
          }
        }
      }

      if (!(minAddr <= addr && addr <= maxAddr)) continue;
      if (!g_transData->isAddrInSomeTrans(addr)) continue;
      TransID transId = g_transData->getTransContaining(addr);
      always_assert(transId != INVALID_ID);
      tcSamples[eventType]++;

      const TransRec* trec = g_transData->getTransRec(transId);
      TransKind kind = trec->kind;
      samplesPerKind[eventType][static_cast<uint32_t>(kind)]++;
      TCRegion region = transCode->findTCRegionContaining(addr);
      always_assert(region != TCRCount);
      samplesPerTCRegion[eventType][region]++;

      std::vector<std::string> stackTrace;
      if (verboseStats) {
        for (size_t i = 0; i < selIdx; i++) {

          if (!strcmp(entries[i].second.c_str(), "[unknown]")) {

            // Append the address to disambiguate.
            entries[i].second += std::string("@")
                              +  toString((void*)entries[i].first);
          }

          stackTrace.push_back(entries[i].second);
        }
        reverse(stackTrace.begin(), stackTrace.end());
      }

      if (selIdx) addr--;
      tcaPerfEvents.addEvent(addr, (PerfEvent){eventType, 1}, stackTrace);
    }
  }

  AddrToTransMapper transMapper(g_transData);
  transPerfEvents = tcaPerfEvents.mapTo(transMapper);

  g_logger->printGeneric("# Number of hhvm samples read "
                         "(%% in TC) from file %s\n",
                         profFileName.c_str());

  for (size_t i = getFirstEventType(); i < getNumEventTypes(); i++) {
    if (!hhvmSamples[i]) continue;

    g_logger->printGeneric("#  %-19s TOTAL: %10u (%u in TC = %5.2lf%%)\n",
                           eventTypeToCommandLineArgument((PerfEventType)i),
                           hhvmSamples[i],
                           tcSamples[i],
                           100.0 * tcSamples[i] / hhvmSamples[i]);

    for (size_t j = 0; j < NumTransKinds; ++j) {
      auto ct = samplesPerKind[i][j];
      if (!ct) continue;
      std::string kind = show(static_cast<TransKind>(j));
      g_logger->printGeneric("# %26s:             %-8u (%5.2lf%%)\n",
                             kind.c_str(), ct, 100.0 * ct / tcSamples[i]);
    }
    g_logger->printGeneric("#\n");
  }
  g_logger->printGeneric("\n");

  // print per-TCRegion information

  // header
  g_logger->printGeneric("# TCRegion ");
  for (size_t i = getFirstEventType(); i < getNumEventTypes(); i++) {
    g_logger->printGeneric("%17s ",
                           eventTypeToCommandLineArgument((PerfEventType)i));
  }
  g_logger->printGeneric("\n");

  // HW events for each region
  for (size_t i = 0; i < TCRCount; i++) {
    g_logger->printGeneric("# %8s ",
                           tcRegionToString(static_cast<TCRegion>(i)).c_str());
    for (size_t j = getFirstEventType(); j < getNumEventTypes(); j++) {
      auto ct = samplesPerTCRegion[j][i];
      g_logger->printGeneric("%8u (%5.2lf%%) ", ct,
                             ct ? (100.0 * ct / tcSamples[j]) : 0);
    }
    g_logger->printGeneric("\n");
  }
  g_logger->printGeneric("#\n\n");

  fclose(profFile);
}

void loadProfData() {
  if (!profFileName.empty()) {
    loadPerfEvents();
  }
}

std::string getDisasmStr(OfflineCode* code,
                         TCA startAddr,
                         uint32_t len,
                         const std::vector<TransBCMapping>& bcMap,
                         const PerfEventsMap<TCA>& perfEvents,
                         bool hostOpcodes) {
  std::ostringstream os;
  code->printDisasm(os, startAddr, len, bcMap, perfEvents, hostOpcodes);
  return os.str();
}

namespace get_json {

using folly::dynamic;

std::string show(TCA tca) {
  return fmt::format("{}", static_cast<void*>(tca));
}

dynamic getAnnotation(const Annotations& annotations) {
  // for JSON outputs, we only care about the annotation "after code gen"
  std::string annotationStr;
  for (auto const& annotation : annotations) {
    if (annotation.first == " after code gen ") {
      annotationStr = annotation.second;
      break;
    }
  }
  if (annotationStr.empty()) return dynamic();

  auto const rawValue = g_annotations->getAnnotation(annotationStr);
  if (rawValue.subpiece(0, 5) != "json:") return rawValue;

  try {
    return folly::parseJson(rawValue.subpiece(5));
  }
  catch (const std::runtime_error& re){
    std::cerr << re.what() << std::endl;
    std::cerr << "Parsing annotation as JSON failed. "
              << "Annotation included as raw value\n";
    return rawValue;
  }
}

dynamic getTransRec(const TransRec* tRec,
                    const PerfEventsMap<TransID>& transPerfEvents) {
  auto const guards = dynamic::array_range(tRec->guards);

  auto const sk = tRec->src;
  auto const resumeMode = [&] {
    switch (sk.resumeMode()) {
      case ResumeMode::None: return "None";
      case ResumeMode::Async: return "Async";
      case ResumeMode::GenIter: return "GenIter";
    }
    always_assert(false);
  }();

  auto const offset = sk.prologue() || sk.funcEntry()
    ? 0  // Unable to lookup entry offset, assume main entry.
    : sk.offset();

  dynamic src = dynamic::object("sha1", tRec->sha1.toString())
                                     ("funcId", FuncId{sk.func()}.toInt())
                                     ("funcName", tRec->funcName)
                                     ("resumeMode", resumeMode)
                                     ("hasThis", sk.hasThis())
                                     ("prologue", sk.prologue())
                                     ("funcEntry", sk.funcEntry())
                                     ("bcStartOffset", offset)
                                     ("guards", guards);

  if (sk.prologue() || sk.funcEntry()) {
    src["numEntryArgs"] = sk.numEntryArgs();
  }

  const dynamic result = dynamic::object("id", tRec->id)
                                        ("src", src)
                                        ("kind", show(tRec->kind))
                                        ("hasLoop", tRec->hasLoop)
                                        ("aStart", show(tRec->aStart))
                                        ("aLen", tRec->aLen)
                                        ("coldStart", show(tRec->acoldStart))
                                        ("coldLen", tRec->acoldLen)
                                        ("frozenStart",
                                         show(tRec->afrozenStart))
                                        ("frozenLen", tRec->afrozenLen);

  return result;
}

dynamic getPerfEvents(const PerfEventsMap<TransID>& transPerfEvents,
                      const TransID transId) {
  dynamic eventsObj = dynamic::object();
  auto const events = transPerfEvents.getAllEvents(transId);

  bool hasEvents = false;
  for (auto const c : events) {
    if (c) hasEvents = true;
  }
  if (!hasEvents) return eventsObj;

  auto const numEvents = getNumEventTypes();
  for (int i = 0; i < numEvents; i++) {
    auto const event = static_cast<PerfEventType>(i);
    auto const eventName = eventTypeToCommandLineArgument(event);
    eventsObj[eventName] = events[i];
  }

  return eventsObj;
}

dynamic getTrans(TransID transId) {
  always_assert(transId < NTRANS);

  auto const* tRec = TREC(transId);
  if (!tRec->isValid()) return dynamic();

  auto const transRec = getTransRec(tRec, transPerfEvents);
  auto const perfEvents = getPerfEvents(transPerfEvents, transId);

  dynamic blocks = dynamic::array;
  for (auto const& block : tRec->blocks) {
    std::stringstream byteInfo; // TODO(T52857125) - translate to actual data

    auto sk = block.sk;
    if (sk.valid()) {
      assertx(!sk.prologue());
      if (sk.funcEntry()) sk.advance();
      sk.func()->prettyPrint(
        byteInfo,
        HPHP::Func::PrintOpts()
          .noName()
          .noMetadata()
          .range(sk.offset(), block.bcPast)
      );
    }

    auto const offset = [&]() {
      if (!sk.prologue() && !sk.funcEntry()) return sk.offset();
      if (sk.valid()) return sk.entryOffset();
      // Unable to lookup entry offset, assume main entry.
      return 0;
    }();

    blocks.push_back(dynamic::object("sha1", block.sha1.toString())
                                    ("start", offset)
                                    ("end", block.bcPast)
                                    ("unit", sk.valid() ?
                                             byteInfo.str() :
                                             dynamic()));
  }

  auto const annotationDynamic = getAnnotation(tRec->annotations);

  auto const maybeUnit = [&]() -> Optional<printir::Unit> {
    if (!annotationDynamic.isObject()) return std::nullopt;
    try {
      return folly::convertTo<printir::Unit>(annotationDynamic);
    }
    catch (const printir::ParseError& pe) {
      std::cerr << pe.what() << std::endl;
      return std::nullopt;
    }
  }();

  const dynamic mainDisasm = tRec->aLen ?
                             transCode->getDisasm(tRec->aStart,
                                                  tRec->aLen,
                                                  tRec->bcMapping,
                                                  tcaPerfEvents,
                                                  hostOpcodes,
                                                  maybeUnit) :
                             dynamic();

  auto const coldIsFrozen = tRec->acoldStart == tRec->afrozenStart;
  const dynamic coldDisasm = !coldIsFrozen && tRec->acoldLen ?
                             transCode->getDisasm(tRec->acoldStart,
                                                  tRec->acoldLen,
                                                  tRec->bcMapping,
                                                  tcaPerfEvents,
                                                  hostOpcodes,
                                                  maybeUnit) :
                             dynamic();
  const dynamic frozenDisasm = tRec -> afrozenLen ?
                               transCode->getDisasm(tRec->afrozenStart,
                                                    tRec->afrozenLen,
                                                    tRec->bcMapping,
                                                    tcaPerfEvents,
                                                    hostOpcodes,
                                                    maybeUnit) :
                               dynamic();
  const dynamic disasmObj = dynamic::object("main", mainDisasm)
                                           ("cold", coldDisasm)
                                           ("frozen", frozenDisasm);

  return dynamic::object("transRec", transRec)
                        ("blocks", blocks)
                        ("archName", transCode->getArchName())
                        ("regions", disasmObj)
                        ("perfEvents", perfEvents)
                        ("transId", transId)
                        ("ir_annotation", maybeUnit ?
                          folly::toDynamic(*maybeUnit) :
                          annotationDynamic);
  // if annotation fails to parse, give raw to the user in annotation field
}

dynamic getTC() {
  auto const ntrans = NTRANS;
  std::vector<dynamic> results(ntrans);
  if (ntrans == 0) {
    return dynamic::object("repoSchema", repoSchemaId().begin())
                          ("translations", dynamic::array);
  }

  if (numThreads <= 1) {
    for (uint32_t t = 0; t < ntrans; t++) {
      results[t] = getTrans(t);
    }
  } else {
    auto const actualThreads = std::min(numThreads, ntrans);
    std::vector<std::thread> threads;
    auto const chunkSize = (ntrans + actualThreads - 1) / actualThreads;
    for (uint32_t tid = 0; tid < actualThreads; tid++) {
      auto const begin = tid * chunkSize;
      auto const end = std::min(begin + chunkSize, ntrans);
      if (begin >= ntrans) break;
      threads.emplace_back([begin, end, &results] {
        for (uint32_t t = begin; t < end; t++) {
          results[t] = getTrans(t);
        }
      });
    }
    for (auto& t : threads) t.join();
  }

  dynamic translations = dynamic::array;
  for (uint32_t t = 0; t < ntrans; t++) {
    translations.push_back(std::move(results[t]));
  }

  return dynamic::object("repoSchema", repoSchemaId().begin())
                        ("translations", translations);
}
}

// Thread-safe version: generates all output for a translation into a string.
std::string generateTransOutput(TransID transId) {
  always_assert(transId < NTRANS);

  std::ostringstream os;

  os << "\n====================\n";
  g_transData->printTransRec(os, transId, transPerfEvents);

  const TransRec* tRec = TREC(transId);
  if (!tRec->isValid()) return os.str();

  if (!tRec->blocks.empty()) {
    os << "----------\nbytecode:\n----------\n";
    const Func* curFunc = nullptr;
    for (auto& block : tRec->blocks) {
      std::stringstream byteInfo;
      auto sk = block.sk;
      if (!sk.valid()) {
        byteInfo << fmt::format(
          "<<< couldn't find func in {} to print bytecode range [{}{},{}) >>>\n",
          block.sha1.toString(),
          block.sk.prologue() || block.sk.funcEntry() ? "numEntryArgs " : "",
          block.sk.prologue() || block.sk.funcEntry()
            ? block.sk.numEntryArgs() : block.sk.offset(),
          block.bcPast);
        continue;
      }

      if (sk.func() != curFunc) {
        curFunc = sk.func();
        byteInfo << '\n';
        curFunc->prettyPrint(
          byteInfo,
          Func::PrintOpts().noMetadata().noBytecode()
        );
      }

      assertx(!sk.prologue());
      if (sk.funcEntry()) sk.advance();

      curFunc->prettyPrint(
        byteInfo,
        Func::PrintOpts()
          .noName()
          .noMetadata()
          .range(sk.offset(), block.bcPast)
      );
      os << byteInfo.str();
    }
  }

  os << fmt::format("----------\n{}: main\n----------\n",
                       transCode->getArchName());
  os << getDisasmStr(transCode,
                     tRec->aStart,
                     tRec->aLen,
                     tRec->bcMapping,
                     tcaPerfEvents,
                     hostOpcodes);

  os << fmt::format("----------\n{}: cold\n----------\n",
                       transCode->getArchName());
  if (tRec->acoldStart != tRec->afrozenStart) {
    os << getDisasmStr(transCode,
                       tRec->acoldStart,
                       tRec->acoldLen,
                       tRec->bcMapping,
                       tcaPerfEvents,
                       hostOpcodes);
  }

  os << fmt::format("----------\n{}: frozen\n----------\n",
                       transCode->getArchName());
  os << getDisasmStr(transCode,
                     tRec->afrozenStart,
                     tRec->afrozenLen,
                     tRec->bcMapping,
                     tcaPerfEvents,
                     hostOpcodes);
  os << "----------\n";

  return os.str();
}

// Generate results in parallel and consume them in order. Workers fill slots
// and mark them ready; the main thread prints each slot as soon as it's
// available, then frees its memory. This bounds peak memory to roughly
// (nThreads * avg_result_size) instead of (count * avg_result_size).
//
// generate(i) produces the string for item i.
// consume(i, string) is called in order on the main thread.
template<typename GenF, typename ConF>
void parallelForEach(uint32_t count,
                     uint32_t nThreads,
                     GenF&& generate,
                     ConF&& consume) {
  if (count == 0) return;

  if (nThreads <= 1) {
    for (uint32_t i = 0; i < count; i++) {
      consume(i, generate(i));
    }
    return;
  }

  struct Slot {
    std::atomic<bool> ready{false};
    std::string data;
  };
  std::vector<Slot> slots(count);

  auto const actualThreads = std::min(nThreads, count);
  std::vector<std::thread> threads;
  auto const chunkSize = (count + actualThreads - 1) / actualThreads;
  for (uint32_t tid = 0; tid < actualThreads; tid++) {
    auto const begin = tid * chunkSize;
    auto const end = std::min(begin + chunkSize, count);
    if (begin >= count) break;
    threads.emplace_back([begin, end, &slots, &generate] {
      for (uint32_t i = begin; i < end; i++) {
        slots[i].data = generate(i);
        slots[i].ready.store(true, std::memory_order_release);
      }
    });
  }

  // Consume results in order as they become ready.
  for (uint32_t i = 0; i < count; i++) {
    while (!slots[i].ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    consume(i, std::move(slots[i].data));
  }

  for (auto& t : threads) t.join();
}


void printCFGOutArcs(TransID transId) {
  std::vector<TCA> jmpTargets;

  TCA fallThru = transCode->getTransJmpTargets(
    g_transData->getTransRec(transId), &jmpTargets);

  auto const srcFuncId = FuncId{TREC(transId)->src.func()};

  for (size_t i = 0; i < jmpTargets.size(); i++) {
    TransID targetId = g_transData->getTransStartingAt(jmpTargets[i]);
    if (targetId != INVALID_ID &&
        // filter jumps to prologues of other funcs
        FuncId{TREC(targetId)->src.func()} == srcFuncId &&
        TREC(targetId)->kind != TransKind::Anchor) {

      bool retrans = TREC(transId)->src == TREC(targetId)->src;
      const char* color;
      if (retrans) {
        color = "darkorange";
      } else if (jmpTargets[i] == fallThru) {
        color = "brown";
      } else {
        color = "green4";
      }
      g_logger->printGeneric("t%u -> t%u [color=%s] ;\n", transId, targetId,
                             color);
    }
  }
}


void printCFG() {
  std::vector<TransID> inodes;

  g_logger->printGeneric("digraph CFG {\n");

  g_transData->findFuncTrans(selectedFuncId, &inodes);

  // Print nodes
  for (auto& tid : inodes) {
    auto const sk = TREC(tid)->src;
    if (sk.prologue()) {
      g_logger->printGeneric(
        "t%u [shape=invtrapezium,label=\"T: %u\\nprologue\","
        "style=filled,color=blue];\n",
        tid, tid
      );
      continue;
    }

    auto const bcStart = sk.prologue() || sk.funcEntry()
      ? fmt::format("numEntryArgs {}", TREC(tid)->src.numEntryArgs())
      : TREC(tid)->src.printableOffset();
    uint32_t bcStop = TREC(tid)->bcPast();
    auto const shape = [&] {
      switch (TREC(tid)->kind) {
        case TransKind::Optimize: return "oval";
        case TransKind::Profile:  return "hexagon";
        default:                  return "box";
      }
    }();
    g_logger->printGeneric(
      "t%u [shape=%s,label=\"T: %u\\nbc: [%s-%d)\",style=filled];\n",
      tid, shape, tid, bcStart.c_str(), bcStop
    );
  }

  // Print arcs
  for (uint32_t i = 0; i < inodes.size(); i++) {
    uint32_t tid = inodes[i];
    printCFGOutArcs(tid);
  }

  g_logger->printGeneric("}\n");
}

void printTopFuncs() {
  if (!nTopFuncs) return;
  std::map<FuncId,std::string> funcStrs;
  for (uint32_t t = 0; t < NTRANS; t++) {
    auto tRec = TREC(t);
    if (!tRec->isValid()) continue;
    auto funcId = FuncId{tRec->src.func()};
    if (funcStrs.contains(funcId)) continue;
    auto funcName = tRec->funcName;
    funcStrs[funcId] = fmt::format("{:<6}: {}", funcId.toInt(), funcName);
  }
  TransToFuncMapper funcMapper(g_transData);
  PerfEventsMap<FuncId> funcPerfEvents = transPerfEvents.mapTo(funcMapper);
  funcPerfEvents.printEventsSummary(sortBy,
                                    "Function",
                                    72,
                                    nTopFuncs,
                                    verboseStats,
                                    helpersMinPercentage,
                                    funcStrs);
}

void printTopFuncsBySize() {
  std::unordered_map<FuncId, size_t> funcSize;
  for (TransID t = 0; t < NTRANS; t++) {
    const auto trec = TREC(t);
    if (trec->isValid()) {
      const auto funcId = FuncId{trec->src.func()};
      funcSize[funcId] += trec->aLen;
    }
  }
  std::vector<std::pair<FuncId, size_t>> topN(nTopFuncs);
  std::partial_sort_copy(funcSize.begin(), funcSize.end(),
                         topN.begin(), topN.end(),
    [&](auto const& a, auto const& b) {
      return a.second > b.second;
  });
  g_logger->printGeneric("FuncID:   \tSize (total aLen in bytes):\n");
  for (auto const& [fid, size] : topN) {
    g_logger->printGeneric("%10u\t%10lu\n", fid.toInt(), size);
  }
}

struct CompTrans {
  private:
  const PerfEventsMap<TransID>& transPerfEvents;
  const PerfEventType           etype;

  public:
  CompTrans(const PerfEventsMap<TransID>& _transPerfEvents,
            PerfEventType _etype) :
    transPerfEvents(_transPerfEvents), etype(_etype) {}

  bool operator()(TransID t1, TransID t2) const {
    const auto count1 = transPerfEvents.getEventCount(t1, etype);
    const auto count2 = transPerfEvents.getEventCount(t2, etype);
    if (sortByDensity) {
      const auto size1 = TREC(t1)->aLen;
      const auto size2 = TREC(t2)->aLen;
      return count1 * size2 > count2 * size1;
    }
    return count1 > count2;
  }
};

void printTopTrans() {
  if (!nTopTrans) return;

  // The summary currently includes all translations, so it's misleading
  // if we're filtering a specific kind of translations or address range.
  // It also doesn't sort by density, so do print it if sortByDensity is set.
  if (kindFilter == "all" && minAddr == 0 && maxAddr == (TCA)-1 &&
      !sortByDensity) {
    transPerfEvents.printEventsSummary(sortBy,
                                       "TransId",
                                       12,
                                       nTopTrans,
                                       verboseStats,
                                       helpersMinPercentage);
  }

  // Sort and print the top translations.
  std::vector<TransID> transIds;

  for (TransID t = 0; t < NTRANS; t++) {
    if (TREC(t)->isValid() &&
        (kindFilter == "all" || kindFilter == show(TREC(t)->kind).c_str()) &&
        ((minAddr <= TREC(t)->aStart     && TREC(t)->aStart      <= maxAddr) ||
         (minAddr <= TREC(t)->acoldStart && TREC(t)->acoldStart <= maxAddr))) {
      transIds.push_back(t);
    }
  }

  CompTrans compTrans(transPerfEvents, sortBy);
  sort(transIds.begin(), transIds.end(), compTrans);

  size_t nPrint = nTopTrans;
  if (transIds.size() < nTopTrans) {
    fprintf(stderr, "Warning: too few translations selected (%lu)\n",
            transIds.size());
    nPrint = transIds.size();
  }

  parallelForEach(
    nPrint, numThreads,
    [&](uint32_t i) { return generateTransOutput(transIds[i]); },
    [](uint32_t, std::string s) { std::cout << s; });
}

void printBytecodeStats(const OfflineTransData* tdata,
                        const PerfEventsMap<TCA>& events,
                        PerfEventType etype) {

  if (!g_repo) error("printBytecodeStats: null repo");
  if (!tdata)  error("printBytecodeStats: null g_transData");

  AddrToBcMapper bcMapper(tdata);
  PerfEventsMap<ExtOpcode> bcPerfEvents = events.mapTo(bcMapper);

  std::map<ExtOpcode,std::string> opcodeToName;
  PerfEventsMap<ExtOpcode>::const_iterator it;

  for (it = bcPerfEvents.begin(); it != bcPerfEvents.end(); it++) {
    opcodeToName[it->first] = extOpcodeToString(it->first);
  }

  bcPerfEvents.printEventsSummary(etype,
                                  "Opcode",
                                  16,
                                  PerfEventsMap<ExtOpcode>::kAllEntries,
                                  verboseStats,
                                  helpersMinPercentage,
                                  opcodeToName);
}

void printTopBytecodes(const OfflineTransData* tdata,
                       OfflineCode* olCode,
                       const PerfEventsMap<TCA>& samples,
                       PerfEventType etype,
                       ExtOpcode filterBy) {

  always_assert(etype >= getFirstEventType() && etype < getNumEventTypes());

  AddrToTransFragmentMapper mapper(tdata, filterBy);
  PerfEventsMap<TransFragment> tfragPerfEvents = samples.mapTo(mapper);

  std::vector<std::pair<uint64_t, TransFragment> > ranking;
  PerfEventsMap<TransFragment>::const_iterator it;

  for (it = tfragPerfEvents.begin(); it != tfragPerfEvents.end(); it++) {
    ranking.push_back(std::make_pair(it->second[etype], it->first));
  }

  sort(ranking.rbegin(), ranking.rend());

  for (size_t i = 0; i < ranking.size(); i++) {
    const TransFragment& tfrag = ranking[i].second;
    const TransRec* trec = tdata->getTransRec(tfrag.tid);

    Unit* unit = g_repo->getUnit(trec->sn);
    always_assert(unit);

    g_logger->printGeneric("\n====================\n");
    g_logger->printGeneric("{\n");
    g_logger->printGeneric("  FuncID  = %u\n", FuncId{trec->src.func()}.toInt());
    g_logger->printGeneric("  TransID = %u\n", tfrag.tid);
    tfragPerfEvents.printEventsHeader(tfrag);
    g_logger->printGeneric("}\n\n");

    g_logger->printGeneric("----------\n%s: main\n----------\n",
                           olCode->getArchName());
    g_logger->printAsm("%s", getDisasmStr(olCode,
                                          tfrag.aStart,
                                          tfrag.aLen,
                                          trec->bcMapping,
                                          samples,
                                          hostOpcodes).c_str());

    g_logger->printGeneric("----------\n%s: cold\n----------\n",
                           olCode->getArchName());
    g_logger->printAsm("%s", getDisasmStr(olCode,
                                          tfrag.acoldStart,
                                          tfrag.acoldLen,
                                          trec->bcMapping,
                                          samples,
                                          hostOpcodes).c_str());

    g_logger->printGeneric("----------\n%s: frozen\n----------\n",
                           olCode->getArchName());
    g_logger->printAsm("%s", getDisasmStr(olCode,
                                          tfrag.afrozenStart,
                                          tfrag.afrozenLen,
                                          trec->bcMapping,
                                          samples,
                                          hostOpcodes).c_str());
  }
}

static uint32_t findSelectedFuncId() {
  if (selectedFuncName.empty()) return INVALID_ID;
  for (uint32_t t = 0; t < NTRANS; t++) {
    auto tRec = TREC(t);
    if (tRec->isValid() && tRec->funcName == selectedFuncName) {
      return FuncId{tRec->src.func()}.toInt();
    }
  }
  return INVALID_ID;
}

int main(int argc, char *argv[]) {
  StaticString::CreateAll();
  folly::SingletonVault::singleton()->registrationComplete();

  pcre_init();

  // Initially use stdout logger while parsing arguments. Optionally switch
  // to DBLogger below if the printToDb argument was passed.
  StdLogger stdoutlogger{};
  g_logger = &stdoutlogger;

  parseOptions(argc, argv);

  // Resolve thread count: 0 means auto-detect.
  if (numThreads == 0) {
    numThreads = std::max(1u, std::thread::hardware_concurrency());
  }

  #ifdef HHVM_FACEBOOK
  Optional<DBLogger> dblogger = std::nullopt;
  if (printToDB) {
    dblogger = DBLogger{};
    g_logger = &dblogger.value();
    g_logger->printGeneric("Printing to database");
  }
  #endif

  g_transData = new OfflineTransData(dumpDir);
  transCode = new OfflineCode(dumpDir,
                              g_transData->getMainBase(),
                              g_transData->getColdBase(),
                              g_transData->getFrozenBase(),
                              g_transData->getBlockMap(),
                              selectedArch);
  Hdf config = !configFile.empty()
    ? Hdf{configFile}
    : Hdf{};
  g_repo = new RepoWrapper(g_transData->getRepoSchema(),
                           repoFileName,
                           config,
                           !useJSON);
  g_transData->loadTCData(g_repo);
  g_annotations = std::make_unique<AnnotationCache>(dumpDir);

  loadProfData();

  g_transData->setAnnotationsVerbosity(annotationsVerbosity);

  if (selectedFuncId == INVALID_ID) {
    selectedFuncId = findSelectedFuncId();
  }

  if (nTopFuncs) {
    if (nTopFuncs > NFUNCS) {
      warnTooFew("functions", nTopFuncs, NFUNCS);
      nTopFuncs = NFUNCS;
    }
    if (sortBySize) {
      printTopFuncsBySize();
    } else {
      printTopFuncs();
    }
  } else if (nTopTrans) {
    if (nTopTrans > NTRANS) {
      warnTooFew("translations", nTopTrans, NTRANS);
      nTopTrans = NTRANS;
    }
    printTopTrans();
  } else if (transCFG) {
    printCFG();
  } else if (creationOrder) {
    // Print translations (all or for a given funcId) in the order
    // they were created.
    sortTrans();
    auto const n = static_cast<uint32_t>(transPrintOrder.size());
    parallelForEach(
      n, numThreads,
      [&](uint32_t i) { return generateTransOutput(transPrintOrder[i]); },
      [](uint32_t, std::string s) { std::cout << s; });
  } else if (collectBCStats) {
    printBytecodeStats(g_transData, tcaPerfEvents, sortBy);
  } else if (filterByOpcode) {
    printTopBytecodes(g_transData,
                      transCode,
                      tcaPerfEvents,
                      sortBy,
                      filterByOpcode);
  } else if (useJSON) {
    auto const tcObj = get_json::getTC();

    auto const jsonStr = folly::toJson(tcObj);
    std::cout << jsonStr << std::endl;

    #ifdef HHVM_FACEBOOK
    if (!hiveTable.empty()) {
      auto const uuid = boost::uuids::random_generator()();
      auto const uuidStr = boost::uuids::to_string(uuid);

      for (auto const& translation : tcObj["translations"]) {
        StructuredLogEntry entry;
        entry.force_init = true;

        entry.setStr("translation_json", folly::toJson(translation));
        entry.setInt("trans_id", translation["transId"].asInt());
        entry.setStr("repoSchema", repoSchemaId().begin());
        entry.setStr("func_name",
                     translation["transRec"]["src"]["funcName"].asString());
        entry.setStr("username" , std::string(getlogin()));
        entry.setStr("uuid", uuidStr);

        writeStructLogLazyInit(hiveTable, entry);
      }
    }
    #endif
  } else {
    // Print all translations in original order, filtered by unit if desired.
    // Build list of translation IDs to print.
    std::vector<TransID> defaultTransIds;
    for (uint32_t t = 0; t < NTRANS; t++) {
      auto tRec = TREC(t);
      if (!tRec->isValid()) continue;
      if (tRec->kind == TransKind::Anchor) continue;
      if (sha1Filter && tRec->sha1 != *sha1Filter) continue;
      defaultTransIds.push_back(t);
    }
    auto const n = static_cast<uint32_t>(defaultTransIds.size());
    parallelForEach(
      n, numThreads,
      [&](uint32_t i) {
        return generateTransOutput(defaultTransIds[i]);
      },
      [&](uint32_t i, std::string s) {
        std::cout << s;
        auto tRec = TREC(defaultTransIds[i]);
        bool opt = (tRec->kind == TransKind::OptPrologue
                          || tRec->kind == TransKind::Optimize);
        g_logger->flushTranslation(tRec->funcName, opt);
      });
  }

  delete g_transData;
  delete transCode;
  delete g_repo;

  return 0;
}
