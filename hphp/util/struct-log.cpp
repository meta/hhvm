/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2016 Facebook, Inc. (http://www.facebook.com)          |
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

#include "hphp/util/struct-log.h"

#include <sstream>

#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/json/json.h>

#include "hphp/util/assertions.h"
#include "hphp/util/stack-trace.h"

namespace HPHP {

StructuredLogEntry::StructuredLogEntry()
  : ints(folly::dynamic::object())
  , strs(folly::dynamic::object())
  , sets(folly::dynamic::object())
  , vecs(folly::dynamic::object())
{
}

void StructuredLogEntry::setInt(folly::StringPiece key, int64_t value) {
  ints[key] = value;
}

void StructuredLogEntry::setStr(folly::StringPiece key,
                                folly::StringPiece value) {
  strs[key] = value;
}

void StructuredLogEntry::setSet(folly::StringPiece key,
                                const std::set<folly::StringPiece>& value) {
  sets[key] = folly::dynamic::object();
  for (auto const& v : value) sets[key][v] = 1;
}

void StructuredLogEntry::setVec(folly::StringPiece key,
                                const std::vector<folly::StringPiece>& value) {
  folly::dynamic arr = folly::dynamic::array();
  arr.resize(value.size());
  for (int i = 0; i < value.size(); ++i) {
    arr[i] = value[i];
  }
  vecs[key] = arr;
}

void StructuredLogEntry::setStackTrace(folly::StringPiece key,
                                       const StackTrace& st) {
  std::vector<folly::StringPiece> stackFrames;
  folly::split('\n', st.toString(), stackFrames);
  for (auto& frame : stackFrames) {
    const char* p = frame.begin();
    while (*p == '#' || *p == ' ' || (*p >= '0' && *p <= '9')) ++p;
    frame = folly::StringPiece(p, frame.end());
  }
  setVec(key, stackFrames);
}

void StructuredLogEntry::setProcessUuid(folly::StringPiece key) {
  static std::string uuid{boost::uuids::to_string(
    boost::uuids::random_generator()()
  )};
  setStr(key, uuid);
}

void StructuredLogEntry::clear() {
  ints = folly::dynamic::object();
  strs = folly::dynamic::object();
  sets = folly::dynamic::object();
  vecs = folly::dynamic::object();
}

namespace StructuredLog {
namespace {
LogFn s_log = nullptr;
RecordGlobalsFn s_recordGlobals = nullptr;
}

bool enabled() {
  return s_log != nullptr;
}

bool coinflip(uint32_t rate) {
  return enabled() && rate > 0 && folly::Random::rand32(rate) == 0;
}

void enable(LogFn log, RecordGlobalsFn globals) {
  assertx(log && globals);
  s_log = log;
  s_recordGlobals = globals;
}

void log(const std::string& tableName, const StructuredLogEntry& cols) {
  if (enabled()) s_log(tableName, cols);
}

void recordRequestGlobals(StructuredLogEntry& cols) {
  if (enabled()) s_recordGlobals(cols);
}
}

std::string show(const StructuredLogEntry& cols) {
  folly::dynamic out = cols.strs;
  out["ints"] = cols.ints;
  out["sets"] = cols.sets;
  out["vecs"] = cols.vecs;
  return folly::toJson(out);
}

}
