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
#include <optional>
#include <string>
#include <vector>

#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/cprof-record.h"
#include "hphp/runtime/vm/srckey.h"

namespace HPHP {

struct Func;

}

namespace HPHP::jit::cprof {

struct ContProfCheckpointReadResult {
  // Zero when no usable checkpoint was found, otherwise one.
  size_t filesRead{};
  size_t recordsDecoded{};
  std::vector<ContProfProfileRecord> records;
  // Canonical unit manifest from the checkpoint.
  std::vector<std::string> unitPaths;
  // Whole extension batch from the checkpoint, including an empty batch.
  ExtensionRegistry::SerializedExtensions extensions;
};

/*
 * Read jit.cprof from directory. A missing, unreadable, or malformed checkpoint
 * produces an empty result; directory or metadata errors return nullopt.
 */
std::optional<ContProfCheckpointReadResult>
readContProfCheckpointDirectory(const std::string& directory);

/* Reconstruct the runtime start encoded by `translation` for `func`. */
std::optional<SrcKey> contProfTranslationSrcKey(
  const ContProfProfileTranslation& translation,
  const Func& func
);

/* Whether the record still matches the func in the current build. */
bool isContProfProfileRecordCompatible(
  const ContProfProfileRecord&,
  const Func&
);

}
