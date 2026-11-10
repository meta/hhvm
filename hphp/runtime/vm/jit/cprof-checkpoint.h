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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <folly/Range.h>

#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/cprof-record.h"

namespace HPHP::jit::cprof {

constexpr auto kContProfCheckpointFilename = "jit.cprof";

/* Whether the process-wide checkpoint writer is accepting profile data. */
bool contProfActive();

/*
 * Start and stop the process-wide checkpoint writer. Writer counters describe
 * lifecycle/flush attempts; writer.active reports the running thread state,
 * independently of the capture eligibility checked by contProfActive().
 */
void startContProfCheckpointWriter();
void stopContProfCheckpointWriter();

/* Encoded batch limit, not a bound on serializer scans or temporary buffers. */
constexpr size_t kMaxContProfExtensionPayloadBytes = 32U << 20;

struct ContProfCheckpoint {
  std::vector<ContProfProfileRecord> records;
  std::vector<std::string> unitPaths;
  std::optional<ExtensionRegistry::SerializedExtensions> extensionData;
};

/*
 * Encode key-ordered records, canonicalize the unit preload manifest, and
 * preserve optional opaque extension payloads in dependency order.
 */
std::optional<std::vector<uint8_t>> serializeContProfCheckpoint(
  const std::vector<ContProfProfileRecord>& records,
  const std::vector<std::string>& unitPaths = {},
  const std::optional<ExtensionRegistry::SerializedExtensions>& extensionData =
    std::nullopt);

/* Decode a checkpoint blob, validating untrusted input. */
std::optional<ContProfCheckpoint>
deserializeContProfCheckpoint(folly::ByteRange);

/* Atomically replace path with a checkpoint of records. */
bool writeContProfCheckpointFile(
  const std::string& path,
  const std::vector<ContProfProfileRecord>& records,
  const std::vector<std::string>& unitPaths = {},
  const std::optional<ExtensionRegistry::SerializedExtensions>& extensionData =
    std::nullopt);

/* Read the checkpoint at path; nullopt if missing or malformed. */
std::optional<ContProfCheckpoint>
readContProfCheckpointFile(const std::string& path);

}
