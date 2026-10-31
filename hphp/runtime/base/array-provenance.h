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

#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/types.h"

#include "hphp/util/rds-local.h"


namespace HPHP::arrprov {

///////////////////////////////////////////////////////////////////////////////

namespace TagTVFlags {
constexpr int64_t TAG_PROVENANCE_HERE_MUTATE_COLLECTIONS = 1;
}

/*
 * Recursively mark/unmark the given TV as being a legacy array.
 *
 * This function will recurse through array-like values. It will always stop
 * at objects, including collections.
 *
 * Attempting to mark a vec or dict pre-HADVAs triggers notices. We'll warn
 * at most once per call since extra notices hurt performance for no benefit.
 *
 * This method will return a new TypedValue or modify and inc-ref `in`.
 */
TypedValue markTvRecursively(TypedValue in, bool legacy);

/*
 * Mark/unmark the given TV as being a legacy array.
 *
 * Attempting to mark a vec or dict pre-HADVAs triggers notices.
 *
 * This method will return a new TypedValue or modify and inc-ref `in`.
 */
TypedValue markTvShallow(TypedValue in, bool legacy);

/*
 * Mark/unmark the given TV up to a fixed depth. You probably don't want to
 * use this helper, but we need it for certain constrained cases (mainly for
 * backtrace arrays, which are varrays-of-darrays-of-arbitrary-values).
 *
 * A depth of 0 means no user-provided limit. A depth of 1 is "markTvShallow".
 */
TypedValue markTvToDepth(TypedValue in, bool legacy, uint32_t depth);

///////////////////////////////////////////////////////////////////////////////

}
