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

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/alignment.h"

#include "hphp/util/data-block.h"

namespace HPHP::jit {

struct CGMeta;

namespace arm {

///////////////////////////////////////////////////////////////////////////////

/*
 * Mirrors the API of align.h.
 */

bool is_aligned(TCA frontier, Alignment alignment);

void align(CodeBlock& cb, CGMeta* meta,
           Alignment alignment, AlignContext context);

constexpr size_t cache_line_size() { return 64; }

/*
 * Get the AlignInfo for `alignment'; used by relocation.
 *
 * SmashCall/SmashJmp are computed on the fly because their nbytes depend on
 * Cfg::Jit::ArmInlineSmashable{Call,Jmp}, which are only available after
 * config parsing (i.e. after the static table is built).
 */
AlignInfo alignment_info(Alignment alignment);

///////////////////////////////////////////////////////////////////////////////

}}

