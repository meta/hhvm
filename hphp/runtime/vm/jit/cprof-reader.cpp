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

#include "hphp/runtime/vm/jit/cprof-reader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-counter.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/switch-profile.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {

namespace {

constexpr size_t kMaxDirectoryEntries = 1000;
constexpr size_t kMaxDecodedRecords = 200'000;

// Cumulative work across directory reads, including repeated startup attempts.
// A successfully decoded file can still exceed the directory's record budget.
auto const s_filesDiscovered =
  ContProfCounter("jit.cprof.reader.files_discovered");
auto const s_filesLimited =
  ContProfCounter("jit.cprof.reader.files_limited");
auto const s_filesAttempted =
  ContProfCounter("jit.cprof.reader.files_attempted");
auto const s_filesRead =
  ContProfCounter("jit.cprof.reader.files_read");
auto const s_filesRejected =
  ContProfCounter("jit.cprof.reader.files_rejected");
auto const s_recordLimitHits =
  ContProfCounter("jit.cprof.reader.record_limit_hits");
auto const s_filesUnreadRecordLimit =
  ContProfCounter("jit.cprof.reader.files_unread_record_limit");
auto const s_recordsDecoded =
  ContProfCounter("jit.cprof.reader.records_decoded");
auto const s_recordsLimited =
  ContProfCounter("jit.cprof.reader.records_limited");
auto const s_recordsUnique =
  ContProfCounter("jit.cprof.reader.records_unique");
auto const s_recordsDeduplicated =
  ContProfCounter("jit.cprof.reader.records_deduplicated");
auto const s_directoryOpenFailures =
  ContProfCounter("jit.cprof.reader.directory_open_failures");
auto const s_directoryIterationFailures =
  ContProfCounter("jit.cprof.reader.directory_iteration_failures");
auto const s_directoryEntryLimit =
  ContProfCounter("jit.cprof.reader.directory_entry_limit");
auto const s_fileMetadataFailures =
  ContProfCounter("jit.cprof.reader.file_metadata_failures");

// Exactly the first rejection per compatibility check; these count records,
// not individual translations, guards, postconditions, or CFG edges.
auto const s_invalidRecord =
  ContProfCounter("jit.cprof.startup.rejected.invalid_record");
auto const s_keyMismatch =
  ContProfCounter("jit.cprof.startup.rejected.key_mismatch");
auto const s_invalidStart =
  ContProfCounter("jit.cprof.startup.rejected.invalid_start");
auto const s_unknownStackOffset =
  ContProfCounter("jit.cprof.startup.rejected.unknown_stack_offset");
auto const s_nonzeroStackOffset =
  ContProfCounter("jit.cprof.startup.rejected.nonzero_stack_offset");
auto const s_invalidRegion =
  ContProfCounter("jit.cprof.startup.rejected.invalid_region");
auto const s_invalidGuards =
  ContProfCounter("jit.cprof.startup.rejected.invalid_guards");
auto const s_invalidPostconditions =
  ContProfCounter("jit.cprof.startup.rejected.invalid_postconditions");
auto const s_invalidEdges =
  ContProfCounter("jit.cprof.startup.rejected.invalid_edges");

struct CheckpointFile {
  std::string filename;
  std::string path;
  std::filesystem::file_time_type mtime{};
};

struct SelectedRecord {
  ContProfProfileRecord record{};
  std::string filename;
};

bool isBetterCandidate(
  const ContProfProfileRecord& candidate,
  const std::string& candidateFilename,
  const SelectedRecord& current
) {
  auto const candidateExecutions = candidate.functionExecutions();
  auto const currentExecutions = current.record.functionExecutions();

  // The filename makes otherwise identical records deterministic.
  return std::tie(
    candidate.header.capturedAtMs,
    candidateExecutions,
    candidateFilename
  ) > std::tie(
    current.record.header.capturedAtMs,
    currentExecutions,
    current.filename
  );
}

std::optional<std::vector<CheckpointFile>>
discoverCheckpointFiles(const std::string& directory) {
  if (directory.empty()) return std::vector<CheckpointFile>{};

  std::error_code iterationError;
  std::filesystem::directory_iterator iterator{directory, iterationError};
  if (iterationError) {
    s_directoryOpenFailures->increment();
    return std::nullopt;
  }

  std::vector<CheckpointFile> files;
  size_t entriesSeen{};

  auto const end = std::filesystem::directory_iterator{};
  for (;
       !iterationError && iterator != end;
       iterator.increment(iterationError)) {
    if (++entriesSeen > kMaxDirectoryEntries) {
      s_directoryEntryLimit->increment();
      return std::nullopt;
    }

    auto const& entry = *iterator;
    auto const filename = entry.path().filename().native();
    if (!isContProfCheckpointFilename(filename)) continue;

    std::error_code entryError;
    auto const status = entry.symlink_status(entryError);
    if (entryError) {
      s_fileMetadataFailures->increment();
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) continue;

    auto const mtime = entry.last_write_time(entryError);
    if (entryError) {
      s_fileMetadataFailures->increment();
      continue;
    }

    files.push_back({
      filename,
      entry.path().native(),
      mtime,
    });
    s_filesDiscovered->increment();
  }
  if (iterationError) {
    s_directoryIterationFailures->increment();
    return std::nullopt;
  }

  // Limit startup work to the newest checkpoints.
  std::sort(
    files.begin(),
    files.end(),
    [](const CheckpointFile& lhs, const CheckpointFile& rhs) {
      return std::tie(lhs.mtime, lhs.filename) >
        std::tie(rhs.mtime, rhs.filename);
    }
  );
  if (files.size() > kMaxContProfCheckpointFiles) {
    s_filesLimited->addValue(files.size() - kMaxContProfCheckpointFiles);
    files.resize(kMaxContProfCheckpointFiles);
  }

  return files;
}

bool hasValidTargetProfilePayload(
  const Func& func,
  const ContProfTargetProfile& profile
) {
  if (profile.kind != ContProfTargetProfileKind::Switch) {
    return true;
  }

  auto const pc = func.at(profile.bytecodeOffset);
  if (peek_op(pc) != Op::Switch) return false;

  auto const numCases = getImmVector(pc).size();
  // Replay uses the bytecode's case count, so the saved allocation must match.
  return profile.payload.size() ==
    sizeof(SwitchProfile) + SwitchProfile::extraSize(numCases);
}

std::optional<SrcKey> validateTranslationRegion(
  const Func& func,
  SrcKey start,
  const ContProfProfileTranslation& translation
) {
  auto profile = translation.targetProfiles.begin();
  auto last = start;

  for (uint32_t i = 0; i < translation.regionLength; ++i) {
    if (i != 0) last.advance(&func);
    // Function entries are synthetic instructions, not bytecode sites.
    if (last.anyFuncEntry()) continue;

    while (profile != translation.targetProfiles.end() &&
           profile->bytecodeOffset == last.offset()) {
      if (!hasValidTargetProfilePayload(func, *profile)) {
        return std::nullopt;
      }
      ++profile;
    }
  }

  if (profile != translation.targetProfiles.end()) {
    return std::nullopt;
  }

  return last;
}

}

std::optional<SrcKey> contProfTranslationSrcKey(
  const ContProfProfileTranslation& translation,
  const Func& func
) {
  switch (translation.startKind) {
    case ContProfStartKind::FuncEntry: {
      if (translation.numEntryArgs() > func.numPositionalParams()) {
        return std::nullopt;
      }

      return SrcKey{
        &func,
        translation.numEntryArgs(),
        false,
        SrcKey::FuncEntryTag{},
      };
    }

    case ContProfStartKind::NamedParamsFuncEntry: {
      if (!func.hasOptionalNamedParameters()) {
        return std::nullopt;
      }

      return SrcKey{
        &func,
        func.numPositionalParams(),
        true,
        SrcKey::FuncEntryTag{},
      };
    }

    case ContProfStartKind::Bytecode: {
      auto const offset = static_cast<Offset>(translation.offset());

      if (!func.contains(offset) || func.isEntry(offset)) {
        return std::nullopt;
      }

      return SrcKey{&func, offset, ResumeMode::None};
    }
  }

  return std::nullopt;
}

std::optional<ContProfCheckpointReadResult>
readContProfCheckpointDirectory(const std::string& directory) {
  auto const files = discoverCheckpointFiles(directory);
  if (!files) return std::nullopt;

  size_t decodedRecords{};
  size_t filesRead{};
  size_t deduplicatedRecords{};
  std::map<ContProfFuncKey, SelectedRecord> selected;
  std::set<std::string> unitPaths;

  for (size_t i = 0; i < files->size(); ++i) {
    auto const& file = (*files)[i];
    s_filesAttempted->increment();
    auto checkpoint = readContProfCheckpointFile(file.path);
    if (!checkpoint) {
      s_filesRejected->increment();
      continue;
    }
    s_filesRead->increment();
    auto& records = checkpoint->records;
    s_recordsDecoded->addValue(records.size());

    if (records.size() > kMaxDecodedRecords - decodedRecords) {
      s_recordLimitHits->increment();
      s_recordsLimited->addValue(records.size());
      // The current file was decoded; only subsequent files remain unread.
      s_filesUnreadRecordLimit->addValue(files->size() - i - 1);
      break;
    }

    ++filesRead;
    decodedRecords += records.size();
    unitPaths.insert(checkpoint->unitPaths.begin(), checkpoint->unitPaths.end());

    for (auto& record : records) {
      auto const [current, inserted] =
        selected.try_emplace(record.header.funcKey);
      if (!inserted) ++deduplicatedRecords;
      if (inserted ||
          isBetterCandidate(record, file.filename, current->second)) {
        current->second = SelectedRecord{std::move(record), file.filename};
      }
    }
  }

  ContProfCheckpointReadResult result{};
  result.filesRead = filesRead;
  result.recordsDecoded = decodedRecords;
  result.unitPaths.assign(unitPaths.begin(), unitPaths.end());
  result.records.reserve(selected.size());

  for (auto& entry : selected) {
    result.records.push_back(std::move(entry.second.record));
  }

  s_recordsUnique->addValue(result.records.size());
  s_recordsDeduplicated->addValue(deduplicatedRecords);
  return result;
}

bool isContProfProfileRecordCompatible(
  const ContProfProfileRecord& record,
  const Func& func
) {
  if (!isValidContProfProfileRecord(record)) {
    s_invalidRecord->increment();
    return false;
  }

  auto const currentKey = makeContProfFuncKey(func);
  if (!currentKey || *currentKey != record.header.funcKey) {
    s_keyMismatch->increment();
    return false;
  }

  std::vector<SrcKey> starts;
  std::vector<SrcKey> lasts;
  starts.reserve(record.translations.size());
  lasts.reserve(record.translations.size());
  for (auto const& translation : record.translations) {
    auto const start = contProfTranslationSrcKey(translation, func);
    if (!start) {
      s_invalidStart->increment();
      return false;
    }

    if (!start->anyFuncEntry()) {
      // Replay supports only empty-stack starts.
      auto const stackOffset = mcgen::offsetAtLocation(*start);
      if (!stackOffset) {
        s_unknownStackOffset->increment();
        return false;
      }
      if (*stackOffset != SBInvOffset{0}) {
        s_nonzeroStackOffset->increment();
        return false;
      }
    }

    auto const region = validateTranslationRegion(func, *start, translation);
    if (!region) {
      s_invalidRegion->increment();
      return false;
    }

    starts.push_back(*start);
    lasts.push_back(*region);

    auto const& guards = translation.localTypeGuards;

    if (!guards.empty() && start->funcEntry()) {
      auto const numEntryArgs = start->numEntryArgs();
      if (numEntryArgs < func.numRequiredPositionalParams() ||
          start->trivialDVFuncEntry()) {
        s_invalidGuards->increment();
        return false;
      }
    }

    auto const localLimit = start->anyFuncEntry()
      ? func.numFuncEntryInputs()
      : static_cast<uint32_t>(func.numLocals());

    for (auto const& guard : guards) {
      if (guard.localId >= localLimit) {
        s_invalidGuards->increment();
        return false;
      }
    }

    for (auto const& post : translation.localPostConditions) {
      if (post.localId >= func.numLocals()) {
        s_invalidPostconditions->increment();
        return false;
      }
    }
  }

  for (size_t i = 0; i < record.translations.size(); ++i) {
    for (auto const predecessor : record.translations[i].incoming) {
      auto const last = lasts[predecessor];

      if (last.funcEntry() &&
          last.numEntryArgs() < func.numRequiredPositionalParams()) {
        s_invalidEdges->increment();
        return false;
      }

      if (!last.succSrcKeys().contains(starts[i])) {
        s_invalidEdges->increment();
        return false;
      }
    }
  }

  return true;
}

}
