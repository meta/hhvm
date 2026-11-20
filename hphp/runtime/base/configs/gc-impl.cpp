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

#include "hphp/runtime/base/configs/gc-loader.h"

#include "hphp/util/configs/gc.h"

#include <folly/lang/Bits.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace HPHP::Cfg {

#ifdef HHVM_EAGER_GC
  constexpr bool kEagerGC = true;
#else
  constexpr bool kEagerGC = false;
#endif

bool GCLoader::EnabledDefault() {
  return kEagerGC;
}

bool GCLoader::EagerDefault() {
  return kEagerGC;
}

bool GCLoader::QuarantineDefault() {
  return kEagerGC;
}

int64_t GCLoader::GraceBytesDefault() {
  return Cfg::GC::EnableLazyGC && Cfg::GC::Enabled ? Cfg::GC::MinTrigger : 0;
}

double GCLoader::TriggerPctDefault() {
  return Cfg::GC::EnableLazyGC && Cfg::GC::Enabled ? 0.8 : 0.5;
}

void GCLoader::SlabAllocAlignPostProcess(uint32_t& val) {
  val = std::clamp(folly::nextPowTwo(val), 16U, 4096U);
}

void GCLoader::SlabStartOffsetPostProcess(uint32_t& val) {
  if (val % 16 != 0 || val > 4096) {
    throw std::runtime_error(
      "Eval.SlabStartOffset must be a multiple of 16, at most 4096, got " +
      std::to_string(val));
  }
}

}
