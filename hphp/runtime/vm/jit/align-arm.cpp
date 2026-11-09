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

#include "hphp/runtime/vm/jit/align-arm.h"

#include "hphp/runtime/vm/jit/align-internal.h"
#include "hphp/runtime/vm/jit/smashable-instr-arm.h"

#include "hphp/util/data-block.h"
#include "hphp/vixl/hphp-compat.h"

#include <folly/Bits.h>

#include <utility>

namespace HPHP::jit::arm {

///////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * Targets of jmps on arm must be aligned to instruction size
 */
constexpr size_t kJmpTargetAlign = vixl::kInstructionSize;

struct AlignImpl {
  /*
   * Don't align the smashables on arm.  The sensitive part of the instruction is
   * the literal which is stored out of line.
   */
  static constexpr size_t SmashableAlignTo = 0;
  // SmashCall/SmashJmp use placeholder nbytes here because the real values
  // come from smashable*LenActual() which reads Cfg flags unavailable at
  // static-init time.  The templates (is_aligned/align) that read s_table
  // directly are no-ops for these entries since SmashableAlignTo == 0.
  // alignment_info() below computes the correct nbytes on the fly.
  static constexpr AlignInfo s_table[] = {
    { cache_line_size(),  cache_line_size(),      0 }, /* CacheLine */
    { cache_line_size(),  cache_line_size() / 2,  0 }, /* CacheLineRoundUp */
    { kJmpTargetAlign,    kJmpTargetAlign,        0 }, /* JmpTarget */
    { 8,                  8,                      0 }, /* QWord Lit */
    { SmashableAlignTo,   smashableMovqLen(),     kSmashMovqImmOff },
    { SmashableAlignTo,   smashableCmpqLen(),     kSmashCmpqImmOff },
    { SmashableAlignTo,   0,                      0 }, /* SmashCall placeholder */
    { SmashableAlignTo,   0,                      0 }, /* SmashJmp placeholder */
    { SmashableAlignTo,   smashableJccLen(),      0 }, /* SmashJcc */
    { SmashableAlignTo,   smashableInterceptLen(), 0 },
  };

  static void pad(CodeBlock& cb, AlignContext context, size_t bytes) {
    vixl::MacroAssembler a { cb };
    auto const begin = cb.frontier();

    switch (context) {
      case AlignContext::Live: {
        assertx(((bytes & 3) == 0) && "alignment must be multiple of 4");
        for (; bytes > 0; bytes -= 4) {
          a.Nop();
        }
        cb.sync(begin);
        return;
      }
      case AlignContext::Dead: {
        if (bytes > 4) {
          a.Brk();
          bytes -= 4;
        }
        cb.sync(begin);
        if (bytes > 0) pad(cb, AlignContext::Live, bytes);
        return;
      }
    }
    not_reached();
  }
};


}

///////////////////////////////////////////////////////////////////////////////

bool is_aligned(TCA frontier, Alignment alignment) {
  return jit::is_aligned<AlignImpl>(frontier, alignment);
}

void align(CodeBlock& cb, CGMeta* meta,
           Alignment alignment, AlignContext context) {
  return jit::align<AlignImpl>(cb, meta, alignment, context);
}

AlignInfo alignment_info(Alignment alignment) {
  switch (alignment) {
    case Alignment::SmashCall:
      return { AlignImpl::SmashableAlignTo, smashableCallLenActual(), 0 };
    case Alignment::SmashJmp:
      return { AlignImpl::SmashableAlignTo, smashableJmpLenActual(), 0 };
    default:
      return AlignImpl::s_table[static_cast<uint32_t>(alignment)];
  }
}

///////////////////////////////////////////////////////////////////////////////

}
