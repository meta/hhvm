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

#include "hphp/util/build-info.h"

#include "hphp/util/embedded-data-reader.h"

#include <folly/Conv.h>
#include <folly/Range.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

namespace HPHP {
////////////////////////////////////////////////////////////////////////////////

namespace {

std::atomic<bool> inited;
std::mutex mtx;
std::string repoSchema;
std::string compiler;
std::string buildid;
int64_t timestamp;

////////////////////////////////////////////////////////////////////////////////

/*
 * Initializes the repo schema id and the compiler id from their special
 * sections in the hhvm binary.
 */
void readBuildInfo() {
  if (inited.load(std::memory_order_acquire)) return;
  std::unique_lock<std::mutex> lock(mtx);
  if (inited.load(std::memory_order_acquire)) return;

  EmbeddedDataReader reader;
  auto const get = [&] (const char* section) -> std::string {
    auto data = reader.readSection(section);
    if (data.empty()) return "(UNKNOWN)";
    return data;
  };

  if (auto const env_schema = getenv("HHVM_RUNTIME_REPO_SCHEMA")) {
    repoSchema = env_schema;
  } else {
    repoSchema = get("repo_schema_id");
  }

  compiler = get("compiler_id");
  buildid = get("build_id");

  try {
    timestamp = std::stoll(get("compiler_ts"));
  } catch(std::exception& ) {
    timestamp = 0;
  }

  inited.store(true, std::memory_order_release);
}

template<typename ValueFn>
void replacePlaceholder(std::string& target,
                        const char* placeholder,
                        ValueFn&& value) {
  auto const pos = target.find(placeholder);
  if (pos == std::string::npos) return;
  target.replace(pos, strlen(placeholder), value());
}

}

////////////////////////////////////////////////////////////////////////////////

folly::StringPiece repoSchemaId() {
  readBuildInfo();
  return repoSchema;
}

folly::StringPiece compilerId() {
  readBuildInfo();
  return compiler;
}

int64_t compilerTimestamp() {
  readBuildInfo();
  return timestamp;
}

folly::StringPiece buildId() {
  readBuildInfo();
  return buildid;
}

void replacePlaceholders(std::string& s,
                         const hphp_fast_string_map<std::string>& replaces) {
  replacePlaceholder(s, "%{schema}", [] { return repoSchemaId().begin(); });
  replacePlaceholder(s, "%{euid}", [] {
    return folly::to<std::string>(geteuid());
  });
  replacePlaceholder(s, "%{user}", [] {
    auto user = getenv("SUDO_USER");
    if (user == nullptr) user = getenv("USER");
    return user != nullptr ? user : "%{user}";
  });
  replacePlaceholder(s, "%{pid}", [] {
    return folly::to<std::string>(getpid());
  });
  for (auto& p : replaces) {
    replacePlaceholder(s, p.first.data(), [&] { return p.second; });
  }
}

void replacePlaceholders(std::string& s) {
  replacePlaceholders(s, {});
}

////////////////////////////////////////////////////////////////////////////////
}
