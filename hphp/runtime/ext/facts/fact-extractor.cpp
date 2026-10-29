/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <fmt/core.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/json/dynamic.h>
#include <folly/logging/xlog.h>

#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/ext/facts/exception.h"
#include "hphp/runtime/ext/facts/fact-extractor.h"
#include "hphp/runtime/ext/facts/thread-factory.h"
#include "hphp/runtime/vm/unit-parser.h"
#include "hphp/util/configs/autoload.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/logger.h"
#include "hphp/util/match.h"
#include "hphp/util/text-util.h"

namespace HPHP {
namespace Facts {

namespace {

// Given a string like "foo bla bla bla ... bla bar", returns a
// printable string like "foo [1234 bytes omitted] bar", where the
// length of the prefix and suffix taken from the string are specified
// by `excerpt_len`. Note that the actual output might be a bit
// longer, due to escaping (e.g., if the string starts with nulls).
std::string summarized_string(std::string_view blob, int excerpt_len) {
  auto s = folly::hexlify(blob);
  std::string to_encode;
  // The 20 bytes of slack is to avoid silly things like:
  // [...2 bytes omitted...]
  // where we might as well just print them.
  if (s.size() < 2 * excerpt_len + 20) {
    to_encode = s;
  } else {
    to_encode = fmt::format(
        "{} [...{} bytes omitted...] {}",
        s.substr(0, excerpt_len),
        s.size() - 2 * excerpt_len,
        s.substr(s.size() - excerpt_len));
  }
  return ::HPHP::escapeStringForCPP(to_encode);
}

hackc::FileFacts decode_facts(const std::string& blob) {
  try {
    return hackc::binary_to_facts(blob);
  } catch (const std::exception& e) {
    throw FactsExtractionExc{fmt::format(
        "{} - blob is \"{}\"", e.what(), summarized_string(blob, 80))};
  }
}

ExtractorFactory* s_extractorFactory = nullptr;

struct SimpleExtractor final : Extractor {
  explicit SimpleExtractor(folly::Executor::KeepAlive<folly::Executor> token)
      : Extractor{std::move(token)} {}

  ~SimpleExtractor() override = default;

  folly::SemiFuture<std::string> get(const PathAndOptionalHash& key) override {
    return folly::via(m_token, [key]() { return facts_binary_from_path(key); });
  }
};

} // namespace

std::string facts_binary_from_path(const PathAndOptionalHash& path) {
  assertx(path.m_path.is_absolute());

  auto const result = extract_facts(
      path.m_path.native(),
      RepoOptions::forFile(path.m_path.c_str()).flags(),
      path.m_hash ? *path.m_hash : "");
  return match<std::string>(
      result,
      [&](const FactsBinaryString& r) { return r.value; },
      [&](const std::string& err) -> std::string {
        throw FactsExtractionExc{err};
      });
}

void setExtractorFactory(ExtractorFactory* factory) {
  s_extractorFactory = factory;
}

std::unique_ptr<Extractor> makeExtractor(
    const folly::Executor::KeepAlive<folly::Executor>& token) {
  // If we defined an external Extractor in closed-source code, use that.
  // Otherwise use the SimpleExtractor.
  if (s_extractorFactory && Cfg::Autoload::EnableExternFactExtractor) {
    XLOG(INFO) << "Creating a external HPHP::Facts::Extractor.";
    return s_extractorFactory->make(token);
  }
  XLOG(INFO) << "Creating an internal HPHP::Facts::SimpleExtractor.";
  return std::make_unique<SimpleExtractor>(token);
}

std::vector<folly::Try<FileFacts>> facts_from_paths(
    const std::filesystem::path& root,
    const std::vector<PathAndOptionalHash>& pathsAndHashes) {
  folly::CPUThreadPoolExecutor exec{
      std::min(
          Cfg::Eval::FactsWorkers,
          static_cast<uint64_t>(pathsAndHashes.size())),
      make_thread_factory("FactExtractor")};

  // If we defined an external Extractor in closed-source code, use that.
  // Otherwise use the SimpleExtractor.
  auto extractor = makeExtractor(folly::getKeepAliveToken(exec));

  std::atomic<int> completed_tasks = 0;
  std::vector<folly::SemiFuture<FileFacts>> factsFutures;
  factsFutures.reserve(pathsAndHashes.size());

  XLOGF(INFO, "Extracting facts for {} files.", pathsAndHashes.size());
  for (int i = 0; i < pathsAndHashes.size(); ++i) {
    auto const& pathAndHash = pathsAndHashes.at(i);
    XLOG_EVERY_N(INFO, 50000) << "Enqueued " << i << " out of "
                              << pathsAndHashes.size() << " updates.";

    assertx(pathAndHash.m_path.is_relative());
    PathAndOptionalHash absPathAndHash{
        root / pathAndHash.m_path, pathAndHash.m_hash};
    factsFutures.push_back(
        folly::via(
            &exec,
            [&extractor, absPathAndHash]() {
              if (UNLIKELY(!absPathAndHash.m_hash)) {
                // We don't know the file's hash yet, so we don't know
                // which key to use to query memcache. We'll try to extract
                // facts from disk instead.
                throw FactsExtractionExc{"No hash provided"};
              }
              return extractor->get(absPathAndHash);
            })
            .thenValue(
                [absPathAndHash](
                    std::string&& factsBinary) -> hackc::FileFacts {
                  auto facts = decode_facts(factsBinary);
                  auto const& hash = *absPathAndHash.m_hash;
                  if (UNLIKELY(facts.sha1sum != hash)) {
                    // The hash we got out of memcache doesn't match the hash
                    // we expected. We'll try to extract facts from disk
                    // instead.
                    throw FactsExtractionExc{fmt::format(
                        "Error extracting {} from memcache: hash '{}' != '{}'",
                        absPathAndHash.m_path.native(),
                        std::string{facts.sha1sum},
                        hash)};
                  }
                  return facts;
                })
            .thenTry([absPathAndHash](folly::Try<hackc::FileFacts>&& facts) {
              if (facts.hasValue()) {
                return *std::move(facts);
              } else {
                XLOGF(
                    WARN,
                    "Error extracting {}: {}",
                    absPathAndHash.m_path.native().c_str(),
                    facts.exception().what().c_str());
                // There might have been a SHA1 mismatch due to a filesystem
                // race. Try again without an expected hash.
                PathAndOptionalHash withoutHash{absPathAndHash.m_path, {}};
                return decode_facts(facts_binary_from_path(withoutHash));
              }
            })
            .thenTry([&completed_tasks,
                      &pathsAndHashes](folly::Try<FileFacts>&& facts) {
              int completed = ++completed_tasks;
              XLOG_EVERY_N(INFO, 50000)
                  << "Finished " << completed << " out of "
                  << pathsAndHashes.size() << " updates.";
              return std::move(facts);
            }));
  }

  XLOG(INFO) << "Done spawning facts_from_paths futures.";
  return folly::collectAll(factsFutures).wait().get();
}

void prefetchDb(const std::filesystem::path& root, const SQLiteKey& dbKey) {
  XLOG(INFO) << "::prefetchDb " << root << " " << dbKey.toString();
  if (s_extractorFactory && Cfg::Autoload::EnableExternFactExtractor) {
    s_extractorFactory->prefetchDb(root, dbKey);
  }
}

} // namespace Facts
} // namespace HPHP
