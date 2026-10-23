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

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/type-source.h"
#include "hphp/runtime/vm/jit/type.h"

namespace HPHP::jit {

struct IRInstruction;

/*
 * GuardConstraints holds state collected during tracelet formation to determine
 * which type assumptions the region depends on.
 */
struct GuardConstraints {
  /*
   * Maps explicitly emitted tracelet guard assumptions to GuardConstraints.
   * Entries are inserted with a fully generic constraint when the guard is
   * emitted, then tightened when its type is used. Only these instructions
   * have relaxable types; ordinary checks and assertions do not.
   */
  jit::hash_map<const IRInstruction*, GuardConstraint> guards;

  /*
   * Maps certain instructions dealing with locals to the source of the
   * local's type coming into the instruction: usually either a guard or the
   * last known value of the local.
   */
  jit::hash_map<const IRInstruction*, TypeSourceSet> typeSrcs;

  /*
   * Maps AssertLoc/AssertStk/AssertMBase instructions to the type of the
   * location coming into the instruction. It is needed to compute the type of
   * the location after the assertion.
   */
  jit::hash_map<const IRInstruction*, Type> prevTypes;
};

}
