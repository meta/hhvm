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

//////////////////////////////////////////////////////////////////////

/*

Welcome to refcount-opts.  Theoretically reading this block comment first will
make the rest of this file make more sense.


-- Overview --

This file contains passes that attempt to reduce the number and strength of
reference counting operations in an IR program.  It uses a few strategies, but
fundamentally most of what's going on is about trying to prove that an IncRef
is post-dominated by a DecRef that provably can't go to zero, with no events in
between that can tell if the IncRef happened, and if so, removing the pair.

This doc comment is going to explain a few concepts, interleaved with
discussion on how they are used by the various analysis and optimization passes
in this module.


-- Must/May Alias Sets --

The passes in this file operate on groups of SSATmp's called "must-alias-set"s
(or often "asets" in the code).  These are sets of SSATmp names that are known
to alias the same object, in a "semi"-flow-insensitive way (see below).  Every
SSATmp that may have a reference counted type is assigned to a must-alias-set.
Crucially, if two SSATmps belong to different must-alias-sets, they still /may/
point to the same object.  For SSATmps a and b, in this module we use
(a&b).maybe(Counted) as a flow-insensitive May-Alias(a,b) predicate: if two
tmps may alias but only in a way that is not reference counted, we don't care
for our purposes.

A subtle point worth considering is that it is possible (and not uncommon) that
some of the SSATmps in one must-alias-set May-Alias some but not all of the
SSATmps in another must-alias-set: the reason is that the must-alias-sets are
still subject to SSA rules about where tmps are defined, and some of the tmps
in a set may be defined by instructions that take conditional jumps if the
object doesn't satisfy some condition (e.g. CheckType).  This is why it may
make sense to think of the must-alias-sets as "semi"-flow-insensitive: it's
globally true that the names all refer to the same object, but the names
involved aren't globally defined.

The first thing this module does is run a function to map SSATmps to their
must-alias-sets, and then, for each must-alias-set S, compute which of the
other must-alias-sets contain any tmps that May-Alias any tmp from S.


-- Weakening DecRefs --

This file contains a relatively cheap pass that can weaken DecRefs into
DecRefNZ by proving that they can't go to zero (unless there is already a bug
in the program).

The way this works is to do a backward dataflow analysis, computing
"will_be_used_again" information.  This dataflow analysis has a boolean for
each must-alias-set, indicating whether all paths from a program point contain
a use of that object in a way that implies their reference count is not zero
(for example, if every path decrefs it again).  Then, it converts any DecRef
instruction on tmps whose must-alias-sets say they "will_be_used_again" to
DecRefNZ.

One rule this pass relies on is that it is illegal to DecRef an object in a way
that takes its refcount to zero, and then IncRef it again after that.  Within
one JIT region, right now we declare it illegal to generate IR that uses an
object after a DecRef that might take it to zero.

Since this pass converts instructions that may (in general) re-enter the
VM---running arbitrary PHP code for a destructor---it's potentially profitable
to run it earlier than other parts of refcount opts.  For example, it can allow
heap memory accesses to be proven redundant that otherwise would not be, and
can prevent the rest of this analysis from assuming some DecRefs can re-enter
that actually can't.


-- RC "lower bounds" --

A lower bound on the reference count of a must-alias-set indicates a known
minimum for the value of its object's count field when the value is counted.
This minimum can be interpreted as a minimum value of the actual integer in
memory at each point, if the program were not modified by this pass. A lower
bound is therefore always non-negative. kInfReferences represents infinity for
an uncounted value, and is unchanged by reference count operations. It has no
unsupported references or memory support. The lower bound has two components; a
supported component, which is known to be exclusive to this must-alias-set (in
the sense that refcount operations on other must-alias-sets will not affect
this one), and an unsupported component, which might be shared between multiple
must-alias sets. For example, if we have two must-alias-sets a and b which may
alias, with lower bound zero, and we see an incref for each, we now know that
they each have a lower bound of at least 2. But a DecRef of either could reduce
both lower bounds to 1 (if they happen to refer to the same thing). We can also
use the unsupported component to account for references held by memory which we
don't track; eg if an SSATmp is stored into an object property, we no longer
attempt to track what happens to that property, so we have to give it an
unsupported_ref; the next time we see an instruction that might affect the
ref counts of untracked memory locations, we have to drop its unsupported_refs.

The first utility of this information is pretty obvious: if a DecRef
instruction is encountered when the lower bound of must-alias-set is greater
than one, that DecRef instruction can be converted to DecRefNZ, since it can't
possibly free the object.  Knowledge of the lower bound is also required for
folding unobservable incref/decref pairs, sinking increfs or hoisting decrefs.

The lower bound must be tracked conservatively to ensure that our
transformations are correct.  This means we can increase a lower bound only
when we see instructions that /must/ imply an increase in the object's count
field, but we must decrease a lower bound whenever we see instructions that
/may/ imply a decrease in the count.  It might clarify this a little to list
the reasons that a must-alias-set's lower bounds can be increased:

   o An explicit IncRef instruction in the instruction stream of a tmp in the
     must-alias-set.

   o Instructions that "produce references" (generally instructions that
     allocate new objects).

   o Some situations with loads from memory (described later).

A must-alias-set's lower bound can be decreased in many situations, including:

   o An explicit DecRef or DecRefNZ of an SSATmp that maps to this
     must-alias-set.

   o In some situations, executing an instruction that could decref pointers
     that live in memory, for example by re-entering and running arbitrary php
     code.  (Memory is discussed more shortly; this concept is called "memory
     support".)

   o An SSATmp in this must-alias-set is passed as an argument to an IR
     instruction that may decref it.  (This is the "consumes reference"
     IRInstruction flag.)

   o We see an instruction that may reduce the lower bound of a different
     must-alias-set, for any reason, and that different set May-Alias this set.

If the last point were the exact rule we used, it would potentially mean lots
of reductions in lower bounds, which could be very pessimistic, so to obviate
the need to do it all the time we introduce an "exclusivity" principle on
tracking lower bounds.  What this principle means is the following: if we see
some reason in the IR to increment the lower bound in an alias set A, we can
/only/ increment the lower bound of A, even if that same information could also
be used to increase the lower bound of other asets.  If we could theoretically
use the same information to increase the lower bound of a different set B, we
can't do that at the same time---we have to choose one to apply it to.  (This
situation comes up with loads and is discussed further in "About Loads".)

This exclusivity principle provides the following rule for dealing with
instructions that may decrease reference counts because of May-Alias
relationships: when we need to decrease the lower bound of a must-alias-set, if
its lower bound is currently greater than its unsupported_refs, we have no
obligation to decrement the lower bound in any other must-alias-set, regardless
of May-Alias relationships. The exclusivity of the supported component of the
lower bound means we know we're just cancelling out something that raised the
lower bound on this set and no other, so the state on other sets can't be
affected.

The pessimistic case still applies, however, if you need to reduce the lower
bound on a must-alias-set S that currently has a lower bound equal to its
unsupported component.  Then all the other sets that May-Alias S must have
their lower bound reduced as well.


-- Memory Support --

This section is going to introduce the "memory support" concept, but details
will be fleshed out further in following sections.

The key idea behind this concept is that we can keep lower bounds higher than
we would otherwise be able to by tracking at least some of the pointers to the
object that may be in memory.  An alternative, conservative approach to stores,
for example, might be to eagerly attempt to reduce the lower bound on the must
alias set for value being stored at the location of the store itself.  By
instead keeping track of the fact that that memory location may contain a
pointer to that must-alias-set until it may be decref'd later, we can keep the
lower bound higher for longer.

The state of memory support for each must-alias-set is a bitvector of the
memory locations AliasAnalysis has identified in the program.  If a bit is set,
it indicates that that memory location may contain a pointer to that must alias
set.  When a must-alias-set has any memory support bits set, it is going to be
analyzed more conservatively than if it doesn't.  And importantly, the memory
support bits are may-information: just because a bit is set, doesn't mean that
we know for sure that memory location contains a pointer to this object.  It
just means that it might, and that our lower bound may have been "kept higher
for longer" using that knowledge at some point.

The primary thing we need to do more conservatively with must-alias-sets that
have memory support is reduce their lower bound if they could be decref'd
through that pointer in memory.  Since this effect on the analysis just reduces
lower bounds, it would never be incorrect to leave the memory support bit set
forever in this situation, which is also conceptually necessary for this to
work as may-information.

However, if we see an instruction that could DecRef one of these objects
through a pointer in memory and its lower_bound is currently above its
unsupported component, we can be sure we've accounted for that may-DecRef
by balancing it with a IncRef of some sort that we've already observed.
In this situation, we can remove the memory support bit to avoid futher
reductions in the lower bound of that set via that memory location.

Since this is may-information that makes analysis more conservative, the memory
support bits should conceptually be or'd at merge points.  It is fine to think
of it that way for general understanding of the analysis here, but in this
implementation we don't actually treat it that way when merging.  Because we
want to be able to quickly find memory-supported must-alias-sets from a given
memory location when analyzing memory effects of IR instructions (i.e. without
iterating every tracked alias set), we restrict the state to require that at
most one must-alias-set is supported by a given memory location during the
analysis.  If we reach situations that would break that restriction, we must
handle it conservatively (using a `pessimized' state, which is discussed some
later, as a last resort).  The merge_memory_support function elaborates on the
details of how this is done.

Another thing to note about memory support is that we may have more bits set
than the current lower bound for an object.  This situation can arise due to
conservatively reducing the lower bound, or due to pure stores happening before
IncRef instructions that raise the lower bound for that new pointer.

Most of the complexity in this analysis is related to instructions that load or
store from memory, and therefore interacts with memory support.  There are
enough details to discuss it futher in next several sections of this doc.


-- About Loads --

On entry to a region, it is assumed that normal VM reference count invariants
hold for all memory---specifically, each reference count on each object in the
heap is exactly the number of live pointers to that object.  And in general,
accesses to memory must maintain this invariant when they are done outside of
small regions that may temporarily break that invariant.  We make use of this
fact to increase object lower bounds.

Accesses to memory within an IR compilation unit may be lowered into
instructions that separate reference count manipulation from stores and loads
(which is necessary for this pass to be able to optimize the former), so we
can't just assume loading a value from somewhere implies that there is a
reference count on the object, since our input program itself may have changed
that.  Furthermore, our input program may contain complex instructions other
than lowered stores that can "store over" memory locations we've already loaded
from, with a decref of the old value, and our analysis pass needs to reduce
lower bounds when we see those situations if we were using that memory location
to increase a lower bound on a loaded value.

To accomplish this, first this module performs a forward dataflow analysis to
compute program locations at which each memory location assigned an id by
AliasAnalysis are known to be "balanced" with regard to reference counting.
The gist of this is that if the last thing to manipulate a memory location must
have been code outside of this region, future loads from the memory location
define SSATmps that we know must have a lower bound of 1, corresponding to the
live pointer in that memory location.  However, when this analysis observes a
PureStore---i.e. a lowered, within-region store that does not imply reference
counting---a future load does not imply anything about the reference count,
because the program may have potentially written a pointer there that is not
yet "balanced" (we would need to see an IncRef or some other instruction
associated with the stored value to know that it has a reference).

Using the results of this analysis, we can add to the lower bound of some
must-alias-sets when we see loads from locations that are known to be balanced
at that program point.  When we do this, we must also track that the object has
a pointer in memory, which could cause a reduction in the lower bound later if
someone could decref it through that pointer, so the location must be added to
the memory support bitvector for that must-alias-set.  Whenever we see complex
instructions that may store to memory with the normal VM semantics of decrefing
old values, if they could overwrite locations that are currently "supporting"
one of our alias sets, we need to remove one from the alias set's lower bound
in case it decided to overwrite (and decref) the pointer that was in memory.

The "exclusivity" guarantee of our lower bounds requires that if we want to
raise the lower bound of an object because it was loaded from a memory location
known to be "balanced", then we only raise the lower bound for this reason on
at most one must-alias-set at a time.  This means if we see a load from a
location that is known to contain a balanced pointer, but we were already using
that location as memory support on a different set, we either need to remove
one from the lower bound of the other set before adding one to the new set, or
leave everything alone.  This commonly happens across php calls right now,
where values must be reloaded from memory because SSATmps can't span calls.

The way this pass currently handles this is the following: if we can reduce the
lower bound on the other set (because it has a non-zero lower bound), we'll
take the first choice, since the previously supported aset will probably not be
used again if we're spanning a call.  On the other hand, if the old set has a
lower bound of zero, so we can't compensate for removing it, we leave
everything alone.


-- Effects of Pure Stores on Memory Support --

There are two main kinds of stores from the perspective of this module.  There
are lowered stores (PureStore) that happen within our IR compilation unit, and
don't imply reference count manipulation, and there are stores that happen with
"hhbc semantics" outside of the visibility of this compilation unit, which
imply decreffing the value that used to live in a memory location as it's
replaced with a new one.  This module needs some understanding of both types,
and both of these types of stores affect memory support, but in different ways.

For any instruction that may do non-lowered stores outside of our unit ("stores
with hhbc semantics"), if the location(s) it may be storing to could be
supporting the lower bound in any must-alias-set, we should remove the support
and decrement the lower bound, because it could DecRef the value in order to
replace it with something else.  If we can't actually reduce the lower bound
(because it's already zero), we must leave the memory support flag alone,
because we haven't really accounted for the reference that lived in that memory
location, and it might not have actually been DecRef'd at that program point,
and could be DecRef'd later after we've seen another IncRef.  If we didn't
leave the bit alone in this situation, the lower bound could end up too high
after a later IncRef.

On the other hand, for a PureStore with a known destination, we don't need to
reduce the lower bound of any set that was supported by that location, since it
never implies a DecRef.  If the input IR program itself is trying to store to
that location "with hhbc semantics", then the program will also explicitly
contain the other lowered parts of this high level store, including any
appropriate loads and DecRefs of the old value, so we won't miss their effects.
So, for a PureStore we can simply mark the location as no-longer providing
memory support on the set it used to, but leave the lower bound alone.

The final case is a PureStore to an unknown location (either because it was not
supplied a AliasAnalysis id, or because it stored to something like a PtrToGen
that could refer to anything in memory).  In this situation, it may or may not
be overwriting a location we had been using for memory support---however, it's
harmless to leave that state alone, with the following rationale:

If it doesn't actually overwrite it, then obviously things are the same, and
we're good.  On the other hand, if it does actually overwrite it, then we don't
need to adjust the lower bound still, because it's a pure store (i.e. for the
same reason we didn't reduce the lower bound in the case above where we knew
where the store was going).  If we do nothing to our state, the only difference
from the known location, then, is that we may have "unnecessarily" left a
must-alias-set marked as getting memory support when it doesn't need to be
anymore.  But the point of marking part of a lower bound as coming from memory
support is just so that future stores (or loads) can potentially /reduce/ its
lower bound, so at worst it could reduce it later when it wouldn't really have
needed to if we had better information about where the store was going.  In
other words, it can be thought of as an optimization to clear the memory
support state when we see a PureStore with a known target location: it's not
required for correctness.


-- Effects of Pure Stores on the Must-Alias-Set Being Stored --

The other thing to take into account with stores is that they put a (possibly
new) pointer in memory, which means it now could be loaded and DecRef'd later,
possibly by code we can't directly see in our compilation unit.  To handle
this, we can divide things into four situations, based on two boolean
attributes: whether or not we have an AliasAnalysis bit for the location being
stored to ("known" vs "unknown" location), and whether or not the lower bound
on the must-alias-set for the value being stored is currently above zero.

The reason the lower bound matters when we see the store is the following:
we've possibly created a pointer in memory, which could be DecRef'd later, but
if the lower bound is zero we don't have a way to account for that, since it
can't go negative.  It's not ok to just ignore this.  Take the following
example, where t1 has a lower bound of zero:

   StMem ptr, t1
   IncRef t1
   IncRef t1
   RaiseWarning "something"  // any instruction that can re-enter and decref t1
   DecRef t1
   ...

If we simply ignored the fact that a new pointer has been created at the store,
that means the lower bound would be two after the two IncRefs, with no memory
support flags.  Then when we see the RaiseWarning, we won't know we need to
reduce the lower bound, since we didn't account for the store, and now we'll
think we can change the DecRef to DecRefNZ, but this is not actually a sound
transformation.

If the input program is not malformed, it will in fact be doing a 'balancing'
IncRef for any new pointers it creates, before anything could access it---in
fact it may have done that before the store, but our analysis in general
could've lost that information in the tracked lower bound because of a
May-Alias decref, or because it was done through an SSATmp that is mapped to a
different must-alias-set that actually is the same object (although we don't
know).

With this in mind, we'll discuss all four cases:

  Unknown target, Zero LB:

     We flag all must-alias-sets as "pessimized".  This state removes all
     support, and sets all lower bounds to zero.

  Known target, Zero LB:

     Unlike the above, this case is not that uncommon.  Since we know where it
     is going, we don't have to give up on everything.  Instead, we leave the
     lower bound at zero, but set a memory support bit for the new location.
     Recall that we can in general have more memory support locations for one
     aset than the tracked lower bound---this is one situation that can cause
     that.

  Unknown target, Non-Zero LB:

     We don't know where the store is going, but we can account for balancing
     the possibly-new pointer.  In this case, we decrement the lower bound and
     just eagerly behave as if the must-alias-set for the stored value may be
     decref'd right there.  Since the lower bound is non-zero, we don't need to
     worry about changing lower bounds in other sets that May-Alias this one,
     because of the "exclusivity" rule for lower bounds.

  Known target, Non-Zero LB:

     Since we know where the new pointer will be, similar to the second case,
     we don't need to reduce the lower bound yet---we can wait until we see an
     instruction that might decref our object through that pointer.  In this
     case, we can just mark the target location as memory support for the
     must-alias-set for the stored value, and leave its lower bound alone.


-- More about Memory --

Another consideration about memory in this module arises from the fact that our
analysis passes make no attempt to track which object pointers may be escaped.
For that matter, much of the optimization we currently do here is removing
redundant reference counting of locals and eval stack slots, which arises from
lowering the HHBC stack machine semantics to HHIR---generally speaking these
values could be accessible through the heap as far as we know.  This is
important because it means that we can make no transformations to the program
that would affect the behavior of increfs or decrefs in memory locations we
aren't tracking, on the off chance they happen to contain a pointer to one of
our tracked objects.

The way we maintain correctness here is to never move or eliminate reference
counting operations unless we know about at least /two/ references to the
object being counted.  The importance of this is easiest to illustrate with
delayed increfs: it is fine to move an incref forward in the IR instruction
stream, as long as nothing could observe the difference between the reference
count the object "should" have, and the one it will have after we delay the
incref.  We need to consider how reachability from the heap can affect this.

If the lower bound at an incref instruction is two or greater, we know we can
push the incref down as much as we want (basically until we reach an exit from
the compilation unit, or until we reach something that may decref the object
and reduce the lower bound).  On the other hand, if the lower bound before the
incref is zero, in order to move the incref forward, we would need to stop at
any instruction that could decref /anything/ in any memory location, since
we're making the assumption that there may be other live pointers to the
object---if we were to push that incref forward, we could change whether other
pointers to the object are considered the last reference, and cause a decref to
free the object when it shouldn't.

The above two cases are relatively straightforward.  The remaining case is when
the lower bound before an incref is one.  It turns out to be safe to sink in
this case, and it fits the idea that we "know about two references".  Whatever
caused the lower bound to be one before the incref will ensure that the
object's liveness is not affected---here's why:

There are two possibilities: the object is either reachable through at least
one unknown pointer, or it isn't.  If it isn't, then the safety of moving the
incref is relatively straight-forward: we'll be pushing the actual /second/
reference down, and it is safe to push it as long as we don't move it through
something that may decref it (or until we reach an exit from the compilation
unit).  For the other possibility, it is sufficient to consider only having one
unknown pointer: in this situation, we're pushing the actual /third/ reference
down, and if anything decrefs the object through the pointer we don't know
about, it will still know not to free it because we left the second reference
alone (whatever was causing our lower bound to be one), and therefore a decref
through this unknown pointer won't think it is removing the last reference.

Also worth discussing is that there are several runtime objects in the VM with
operations that have behavioral differences based on whether the reference count
is greater than one.  For instance, types like KindOfString and KindOfDict do
in place updates when they have a refcount of one. Making sure we don't change
these situations is actually the same condition as discussed above: by the above
scheme for not changing whether pointers we don't know about constitute the last
counted reference to an object, we are both preventing decrefs from going to
zero when they shouldn't, and modifications to objects from failing to COW when
they should.

A fundamental meta-rule that arises out of all the above considerations is that
we cannot move (or remove) increfs unless the lower bound on the incref node is
at least one (meaning after the incref we "know about two references").
Similarly, anything that could reduce the lower bound must observe the refcount
at that point (an NReq{1} usually) so we don't push increfs too far or remove
them when we shouldn't.

*/

//////////////////////////////////////////////////////////////////////

#include "hphp/runtime/vm/jit/opt.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <limits>
#include <array>
#include <tuple>

#include <fmt/format.h>
#include <folly/ScopeGuard.h>
#include <folly/Conv.h>
#include <folly/portability/Stdlib.h>

#include "hphp/util/bitset-array.h"
#include "hphp/util/bitset-utils.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/match.h"
#include "hphp/util/safe-cast.h"
#include "hphp/util/trace.h"

#include "hphp/runtime/ext/asio/ext_wait-handle.h"

#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/state-vector.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/alias-analysis.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/timer.h"

namespace HPHP { namespace jit {

namespace {

TRACE_SET_MOD(hhir_refcount)

//////////////////////////////////////////////////////////////////////
constexpr int32_t kInfReferences = std::numeric_limits<int32_t>::max();
constexpr int32_t kMaxPlausibleReferences = 1048576;

// Helper for removing instructions in the rest of this file---if a debugging
// mode is enabled, it will replace it with a debugging instruction if
// appropriate instead of removing it.
void remove_helper(IRUnit& unit, IRInstruction* inst) {
  if (!Cfg::HHIR::GenerateAsserts) {
    inst->convertToNop();
    return;
  }

  switch (inst->op()) {
  case IncRef:
  case DecRef:
  case DecRefNZ:
  case DecReleaseCheck: {
    inst->setOpcode(DbgAssertRefCount);
    inst->clearExtra();
    auto extra = ASSERT_REASON;
    inst->setExtra(cloneExtra(DbgAssertRefCount, &extra, unit.arena()));
    break;
  }
  default:
    always_assert_flog(
      false,
      "Unsupported remove_helper instruction: {}\n",
      *inst
    );
  }
}

//////////////////////////////////////////////////////////////////////

/*
 * Id's of must-alias-sets.  We use -1 as an invalid id.
 */
using ASetID = int32_t;

struct MustAliasSet {
  explicit MustAliasSet(Type widestType, SSATmp* representative)
    : widestType(widestType)
    , representative(representative)
  {}

  /*
   * Widest type for this MustAliasSet, used for computing may-alias
   * information.
   *
   * Because of how we build MustAliasSets (essentially canonical(), or groups
   * of LdFrameThis instructions), it is guaranteed that this widestType includes all
   * possible values for the set.  However it is not the case that every tmp in
   * the set necessarily has a subtype of widestType, because of situations
   * that can occur with AssertType and interface types.  This does not affect
   * correctness, but it's worth being aware of.
   */
  Type widestType;

  /*
   * A representative of the set.  This is only used for debug tracing, and is
   * currently the first instruction (in an rpo traversal) that defined a tmp
   * in the must-alias-set.  (I.e. it'll be the canonical() tmp, or the first
   * LdFrameThis we saw.)
   */
  SSATmp* representative;

  /*
   * Set of ids of the other MustAliasSets that this set may alias, in a flow
   * insensitive way, and not including this set itself.  This is based only on
   * the type of the representative.  See the comments at the top of this file.
   */
  jit::flat_set<ASetID> may_alias;
};

//////////////////////////////////////////////////////////////////////

// Analysis results for memory locations known to contain balanced reference
// counts.  See populate_mrinfo.
struct MemRefAnalysis {
  struct BlockInfo {
    uint32_t rpoId;
    ALocBits avail_in;
    ALocBits avail_out;
    ALocBits kill;
  };

  explicit MemRefAnalysis(IRUnit& unit) : info(unit, BlockInfo{}) {}

  StateVector<Block,BlockInfo> info;
};

//////////////////////////////////////////////////////////////////////

// Per must-alias-set state information for rc_analyze.
struct ASetInfo {
  /*
   * A lower bound of the actual reference count when this alias set is counted.
   * See "RC lower bounds" in the documentation, there are some subtleties here.
   */
  int32_t lower_bound{0};

  /*
   * An upper bound of the actual reference count of the object that this alias
   * set refers to.
   */
  int32_t upper_bound{kInfReferences};

  /*
   * Sometimes we know the refcount is higher than we've been able to
   * prove; eg when we IncRef something with a lower_bound of zero, we
   * know that the actual lower_bound is 2. When we have
   * unsupported_refs, we have to account for DecRef or DecRefNZ on
   * anything that mayalias this one, and anything other than an
   * irrelevant_inst or an IncRef will kill the unsupported_refs.
   */
  int32_t unsupported_refs{0};

  /*
   * Deferred reductions of this set's lower bound from unbalanced decrefs of
   * may-alias sets. The effective bound is
   * max(lower_bound - pending_reductions, 0).
   * Pending reductions can exceed the lower bound. kInfReferences represents
   * an unbounded number of pending reductions; an infinite lower bound has none.
   */
  int32_t pending_reductions{0};

  /*
   * Set of memory location ids that are being used to support the lower bound
   * of this object.  The purpose of this set is to reduce lower bounds when we
   * see memory events that might decref a pointer: this means it's never
   * incorrect to leave a bit set in memory_support conservatively, but there
   * are situations where we must set bits here or our analysis will be wrong.
   *
   * An important note is that the bits in memory_support can represent memory
   * locations that possibly alias (via ALocMeta::conflicts).  Setting only one
   * bit from the conflict set is sufficient when we know something must be in
   * memory in the set---any memory effects that can affect other may-aliasing
   * locations will still apply to all of them as needed.
   *
   * However, whenever we handle removing memory support, if you need to remove
   * one bit, you generally speaking are going to need to remove the support
   * for the whole conflict set.
   */
  ALocBits memory_support;
};

// State structure for rc_analyze.
struct RCState {
  bool initialized{false};
  bool has_unsupported_refs{false};

  /*
   * The number of asets with non-zero pending reductions.
   */
  uint32_t num_pending_requirements{0};

  jit::vector<ASetInfo> asets;

  /*
   * MemRefAnalysis availability state.  This is just part of this struct for
   * convenience when stepping through RCAnalysis results.  It is used to know
   * when loads can provide memory support.
   */
  ALocBits avail;

  /*
   * Map from AliasClass ids to the must-alias-set that has it as
   * memory_support, if any do.  At most one ASet will be supported by any
   * location at a time, to fit the "exclusivity" condition on lower bounds.
   * The mapped value is -1 if no ASet is currently supported by that location.
   */
  std::array<ASetID,kMaxTrackedALocs> support_map;
};

// The analysis result structure for rc_analyze.
struct RCAnalysis {
  struct BlockInfo {
    uint32_t rpoId;
    RCState state_in;
  };

  explicit RCAnalysis(IRUnit& unit) : info(unit, BlockInfo{}) {}

  StateVector<Block,BlockInfo> info;
};

//////////////////////////////////////////////////////////////////////

struct Env {
  explicit Env(IRUnit& unit)
    : unit(unit)
    , rpoBlocks(rpoSortCfg(unit))
    , idoms(findDominators(unit, rpoBlocks, numberBlocks(unit, rpoBlocks)))
    , ainfo(collect_aliases(unit, rpoBlocks))
    , mrinfo(unit)
    , asetMap(unit, -1)
  {}

  IRUnit& unit;
  BlockList rpoBlocks;
  IdomVector idoms;
  Arena arena;
  AliasAnalysis ainfo;
  MemRefAnalysis mrinfo;

  StateVector<SSATmp,ASetID> asetMap;  // -1 is invalid (not-Counted tmps)
  jit::vector<MustAliasSet> asets;
};

using IncDecBits = BitsetRef;

struct PreBlockInfo {
  enum Bits {
    AltLoc,
    AvlLoc,
    AntLoc,
    AvlIn,
    AvlOut,
    PavlIn,
    PavlOut,
    AntIn,
    AntOut,
    PantIn,
    PantOut,

    NumBitsets
  };
  explicit PreBlockInfo(BitsetRef base) :
      rpoId(0),
      altLoc{base.next(AltLoc)},
      avlLoc{base.next(AvlLoc)},
      antLoc{base.next(AntLoc)},
      avlIn{base.next(AvlIn)},
      avlOut{base.next(AvlOut)},
      pavlIn{base.next(PavlIn)},
      pavlOut{base.next(PavlOut)},
      antIn{base.next(AntIn)},
      antOut{base.next(AntOut)},
      pantIn{base.next(PantIn)},
      pantOut{base.next(PantOut)}
    {}

  PreBlockInfo(const PreBlockInfo& o) = default;
  PreBlockInfo& operator=(const PreBlockInfo& o) {
    if (this != &o) {
      this->~PreBlockInfo();
      new (this) PreBlockInfo(o);
    }
    return *this;
  }

  uint32_t   rpoId;
  uint32_t   genId{};

  // On a DefLabel block, this is a bitvector indicating which SSATmps
  // should be considered equivalent to the corresponding phi inputs
  // so that we can pair IncRefs and DecRefs across the join.
  std::bitset<64> phiPropagate{};

  // Bits set here block Incs and Decs of the corresponding ASetID
  IncDecBits altLoc;
  // We can either pair Incs followed by Decs, or Decs followed by Incs.
  // avlLoc indicates that the leader is locally available, and antLoc
  // indicates that the follower is locally anticipated.
  IncDecBits avlLoc;
  IncDecBits antLoc;

  IncDecBits avlIn;
  IncDecBits avlOut;
  IncDecBits pavlIn;
  IncDecBits pavlOut;

  IncDecBits antIn;
  IncDecBits antOut;
  IncDecBits pantIn;
  IncDecBits pantOut;
};

using BlockState = StateVector<Block, PreBlockInfo>;

struct PreEnv {
  using IncDecKey = std::tuple<Block*,uint32_t,bool>; /* blk, id, at front */
  struct IDKeyCmp {
    bool operator()(const IncDecKey& a, const IncDecKey& b) const {
      if (std::get<0>(a)->id() != std::get<0>(b)->id()) {
        return std::get<0>(a)->id() < std::get<0>(b)->id();
      }
      if (std::get<1>(a) != std::get<1>(b)) {
        return std::get<1>(a) < std::get<1>(b);
      }
      return std::get<2>(a) < std::get<2>(b);
    }
  };
  using InsertMap = jit::flat_map<IncDecKey, SSATmp*, IDKeyCmp>;

  explicit PreEnv(Env& env, RCAnalysis& rca) :
      env{env},
      rca{rca},
      bits{env.unit.numBlocks() * PreBlockInfo::NumBitsets + 1,
           env.asets.size()},
      scratchBits{bits.row(env.unit.numBlocks() * PreBlockInfo::NumBitsets)},
      state{env.unit,
            [this] (size_t i) {
              return bits.row(i * PreBlockInfo::NumBitsets);
            }
      },
      curGen{0},
      avlQ(env.rpoBlocks.size()),
      antQ(env.rpoBlocks.size()) {
    uint32_t id = 0;
    for (auto const blk : env.rpoBlocks) {
      auto& s = state[blk];
      s.rpoId = id;
      avlQ.push(id);
      antQ.push(id);
      ++id;
    }
  }

  Env& env;
  RCAnalysis& rca;
  BitsetArray bits;
  BitsetRef scratchBits;
  BlockState state;
  uint32_t  curGen;
  InsertMap insMap;
  std::vector<std::pair<Block*, SSATmp*>> to_insert;

  std::set<uint32_t> reprocess;
  dataflow_worklist<uint32_t, std::greater<uint32_t>> avlQ;
  dataflow_worklist<uint32_t, std::less<uint32_t>> antQ;
};

struct PreAdderInfo {
  PreAdderInfo(PreEnv& penv,
               PreBlockInfo& blkInfo,
               RCState& state,
               bool incDec) :
      penv{penv}, blkInfo{blkInfo}, state{state}, incDec{incDec} {}

  void remove(Block::iterator iter) {
    auto& inst = *iter;
    auto const id = penv.env.asetMap[inst.src(0)];
    auto it = iter;
    while (true) {
      auto& i2 = *--it;
      if (i2.is(IncRef, DecRef, DecRefNZ) &&
          penv.env.asetMap[i2.src(0)] == id) {
        assertx(incDec == i2.is(IncRef));
        FTRACE(3, "    ** trivial pair: {}, {}\n", inst, i2);
        remove_helper(penv.env.unit, &i2);
        remove_helper(penv.env.unit, &inst);
        blkInfo.avlLoc.reset(id);
        penv.reprocess.insert(blkInfo.rpoId);
        modified = true;
        return;
      }
      assertx(it != inst.block()->begin());
    }
  }
  void setAvlAnt(Block::iterator iter, bool avl) {
    auto& inst = *iter;
    auto const id = penv.env.asetMap[inst.src(0)];
    if (id < 0) {
      assertx(inst.src(0)->type() <= TUncounted);
      FTRACE(3, "    ** kill uncounted inc/dec: {}\n", inst);
      remove_helper(penv.env.unit, &inst);
      return;
    }
    if (avl) {
      FTRACE(4, "     avlLoc: {}\n", id);
      blkInfo.avlLoc.set(id);
      // also set altLoc, so that this block isn't transparent for ANT
      blkInfo.altLoc.set(id);
      return;
    }
    if (blkInfo.avlLoc.test(id)) {
      remove(iter);
      return;
    }
    auto alt = blkInfo.altLoc.test(id);
    if (!alt) {
      FTRACE(4, "     antLoc: {}\n", id);
      blkInfo.antLoc.set(id);
      // also set altLoc, so that this block isn't transparent for AVL
      blkInfo.altLoc.set(id);
    }
    return;
  }

  PreEnv& penv;
  PreBlockInfo& blkInfo;
  RCState& state;
  bool modified{false};
  bool incDec;
};

//////////////////////////////////////////////////////////////////////

/*
 * IncRef and DecRef{NZ,} nodes.
 */
struct NInc {
  explicit NInc(IRInstruction* inst) : inst(inst) {}
  IRInstruction* inst;
};
struct NDec {
  explicit NDec(IRInstruction* inst) : inst(inst) {}
  IRInstruction* inst;
};

/*
 * Req nodes mean the reference count of the object may be observed, up to some
 * "level".  The level is a number we have to keep the lower_bound above to
 * avoid changing program behavior.  It will be INT32_MAX on exits from the
 * compilation unit.
 */
struct NReq {
  explicit NReq(int32_t level) : level(level) {}
  int32_t level;
};

struct PreAdder {
  PreAdder() = default;
  explicit PreAdder(PreAdderInfo* info) : info{info} {}

  void operator()(ASetID asetID, const NInc& n) const {
    if (!info) return;
    auto const blk = n.inst->block();
    auto const iter = blk->iteratorTo(n.inst);
    if (info->incDec && info->state.asets[asetID].lower_bound < 1) {
      info->blkInfo.altLoc.set(asetID);
      return;
    }
    info->setAvlAnt(iter, info->incDec);
  }
  void operator()(ASetID asetID, const NDec& n) const {
    if (!info) return;
    auto const blk = n.inst->block();
    auto const iter = blk->iteratorTo(n.inst);
    if (info->state.asets[asetID].lower_bound <= 1) {
      info->blkInfo.altLoc.set(asetID);
      return;
    }
    info->setAvlAnt(iter, !info->incDec);
  }
  void operator()(ASetID asetID, const NReq& n,
                  ASetID aliasID = -1) const {
    if (!info) return;
    /*
     * An NReq requires that the refcount at this point in the code be
     * at least n.level. If we remove an inc/dec pair, we'll reduce
     * the refcount here by 1, so unless lb - 1 >= n.level, we can't
     * remove it. The situation for dec/inc pairs is less clear, and
     * we might even be able to ignore NReqs unless n.level==INT_MAX
     * in that case - but that requires some investigation.
     */
    auto const& aset = info->state.asets[asetID];
    auto lb = aset.lower_bound;
    if (lb == kInfReferences) return;
    if (aliasID != -1) {
      assertx(aliasID != asetID);
      auto const& alias = info->state.asets[aliasID];
      // If the optional aliasID is given, we are assuming that the asetID and
      // aliasID alias each other. Either bound or their independent supported
      // credits can satisfy the requirement. Unsupported credits may overlap
      // support in the other set, so only the supported components can be
      // added.
      if (alias.lower_bound == kInfReferences) return;
      auto const supported = aset.lower_bound - aset.unsupported_refs +
        alias.lower_bound - alias.unsupported_refs;
      lb = std::max({lb, alias.lower_bound, supported});
    }
    if (lb - (info->incDec ? 1 : 0) >= n.level) return;
    FTRACE(4, "     altLoc: {}\n", asetID);
    info->blkInfo.altLoc.set(asetID);
    info->blkInfo.avlLoc.reset(asetID);
  }

  PreAdderInfo* info = nullptr;
};

//////////////////////////////////////////////////////////////////////

template<class Kill>
void mrinfo_step_impl(Env& env,
                      const IRInstruction& inst,
                      Kill kill) {
  auto do_store = [&] (AliasClass dst, SSATmp* value) {
    /*
     * Pure stores potentially (temporarily) break the heap's reference count
     * invariants on a memory location, but only if the value being stored is
     * possibly counted.
     */
    if (!value || value->type().maybe(TCounted)) {
      kill(env.ainfo.may_alias(canonicalize(dst)));
    }
  };

  auto const effects = memory_effects(inst);
  match(
    effects,
    [&](const IrrelevantEffects&) {},
    [&](const ExitEffects&) {},
    [&](const ReturnEffects&) {},
    [&](const GeneralEffects&) {},
    [&](const PureInlineCall&) {},
    [&](const CallEffects&) {},

    [&](const UnknownEffects&) { kill(ALocBits{}.set()); },
    [&](const PureStore& x) { do_store(x.dst, x.value); },

    /*
     * Note that loads do not kill a location.  In fact, it's possible that the
     * IR program itself could cause a location to not be `balanced' using only
     * PureLoads.  (For example, it could load a local to decref it as part of
     * a return sequence.)
     *
     * It's safe not to add it to the kill set, though, because if the IR
     * program is destroying a memory location, it is already malformed if it
     * loads the location again and then uses it in a way that relies on the
     * pointer still being dereferenceable.  Moreover, in these situations,
     * even though the avail bit from mrinfo will be set on the second load, we
     * won't be able to remove support from the previous aset, and won't raise
     * the lower bound on the new loaded value.
     */
    [&](const PureLoad&) {});
}

// Helper for stepping after we've created a MemRefAnalysis.
void mrinfo_step(Env& env, const IRInstruction& inst, ALocBits& avail) {
  mrinfo_step_impl(
    env,
    inst,
    [&] (ALocBits kill) { avail &= ~kill; }
  );
}

/*
 * Perform an analysis to determine memory locations that are known to hold
 * "balanced" values with respect to reference counting.  This means the
 * location "owns" a reference in the normal sense---i.e. the count on the
 * object is at least one on account of the pointer in that location.
 *
 * Normal ("hhbc-semantics") operations on php values in memory all preserve
 * balanced reference counts (i.e. a pointer in memory corresponds to one value
 * in the count field of the pointee).  However, when we lower hhbc opcodes to
 * HHIR, some opcodes split up the reference counting operations from the
 * memory operations: when we observe a "pure store" instruction, therefore,
 * the location involved may no longer be "balanced" with regard to reference
 * counting.  See further discussion in the doc comment at the top of this
 * file.
 */
void populate_mrinfo(Env& env) {
  FTRACE(1, "populate_mrinfo ---------------------------------------\n");
  FTRACE(3, "locations:\n{}\n", show(env.ainfo));

  /*
   * 1. Compute block summaries.
   */
  for (auto& blk : env.rpoBlocks) {
    for (auto& inst : blk->instrs()) {
      mrinfo_step_impl(
        env,
        inst,
        [&] (ALocBits kill) {
          env.mrinfo.info[blk].kill |= kill;
        }
      );
    }
  }

  FTRACE(3, "summaries:\n{}\n",
    [&] () -> std::string {
      auto ret = std::string{};
      for (auto& blk : env.rpoBlocks) {
        fmt::format_to(std::back_inserter(ret), "  B{: <3}: {}\n",
          blk->id(),
          show(env.mrinfo.info[blk].kill)
        );
      }
      return ret;
    }()
  );

  /*
   * 2. Find fixed point of avail_in:
   *
   *   avail_out = avail_in - kill
   *   avail_in  = isect(pred) avail_out
   *
   * Locations that are marked "avail" mean they imply a non-zero lower bound
   * on the object they point to, if they contain a reference counted type, and
   * assuming they are actually legal to load from.
   */

  auto incompleteQ = dataflow_worklist<uint32_t>(env.rpoBlocks.size());
  for (auto rpoId = uint32_t{0}; rpoId < env.rpoBlocks.size(); ++rpoId) {
    env.mrinfo.info[env.rpoBlocks[rpoId]].rpoId = rpoId;
  }
  // avail_outs all default construct to zeros.
  // avail_in on the entry block (with no preds) will be set to all 1 below.
  incompleteQ.push(0);

  do {
    auto const blk = env.rpoBlocks[incompleteQ.pop()];
    auto& binfo = env.mrinfo.info[blk];

    binfo.avail_in.set();
    blk->forEachPred([&] (Block* pred) {
      binfo.avail_in &= env.mrinfo.info[pred].avail_out;
    });

    auto const old = binfo.avail_out;
    binfo.avail_out = binfo.avail_in & ~binfo.kill;
    if (binfo.avail_out != old) {
      if (auto const t = blk->taken()) {
        incompleteQ.push(env.mrinfo.info[t].rpoId);
      }
      if (auto const n = blk->next()) {
        incompleteQ.push(env.mrinfo.info[n].rpoId);
      }
    }
  } while (!incompleteQ.empty());

  FTRACE(4, "fixed point:\n{}\n",
    [&] () -> std::string {
      auto ret = std::string{};
      for (auto& blk : env.rpoBlocks) {
        fmt::format_to(std::back_inserter(ret), "  B{: <3}: {}\n",
          blk->id(),
          show(env.mrinfo.info[blk].avail_in)
        );
      }
      return ret;
    }()
  );
}

//////////////////////////////////////////////////////////////////////

using HPHP::jit::show;

DEBUG_ONLY std::string show(const BitsetRef& bs) {
  std::string res;
  res.reserve(bs.size());
  for (auto i = bs.size(); i--; ) {
    res.push_back(bs[i] ? '1' : '0');
  }
  return res;
}

/*
 * This helper for weaken_decrefs reports uses of reference-counted values that
 * imply that their reference count cannot be zero (or it would be a bug).
 * This includes any use of an SSATmp that implies the pointer isn't already
 * freed.
 *
 * PureStores are allowed to store the address of an object which has
 * been destroyed (this allowes store-elim to sink stores without
 * worrying about moving them past DecRefs); otherwise a use of an
 * SSATmp indicates that its refcount has not yet hit zero.
 */
template<class Gen>
void weaken_decref_step(const Env& env, const IRInstruction& inst, Gen gen) {
  bool checked = false;
  for (auto i = 0; i < inst.numSrcs(); i++) {
    auto const asetID = env.asetMap[inst.src(i)];
    if (asetID == -1) continue;
    if (!checked) {
      auto const effects = memory_effects(inst);
      if (std::get_if<PureStore>(&effects)) return;
      checked = true;
    }
    gen(asetID);
  }
}

/*
 * Backward pass that weakens DecRefs to DecRefNZ if they cannot go to zero
 * based on future use of the value that they are DecRefing.  See "Weakening
 * DecRefs" in the doc comment at the top of this file.
 */
void weaken_decrefs(Env& env) {
  FTRACE(2, "weaken_decrefs ----------------------------------------\n");
  auto const poBlocks = [&] {
    auto ret = env.rpoBlocks;
    std::reverse(begin(ret), end(ret));
    return ret;
  }();

  /*
   * 0. Initialize block state structures and put all blocks in the worklist.
   */
  auto incompleteQ = dataflow_worklist<uint32_t>(poBlocks.size());
  BitsetArray bits{env.unit.numBlocks() * 3, env.asets.size()};
  struct BlockInfo {
    explicit BlockInfo(BitsetRef base) :
        in_used{base}, out_used{in_used.next()}, gen{out_used.next()} {
    }
    BlockInfo(const BlockInfo&) = default;
    BlockInfo& operator=(const BlockInfo& o) {
      if (this != &o) {
        this->~BlockInfo();
        new (this) BlockInfo{o};
      }
      return *this;
    }
    uint32_t poId{};
    BitsetRef in_used;
    BitsetRef out_used;
    BitsetRef gen;
  };
  StateVector<Block,BlockInfo> blockInfos{
    env.unit,
    [&] (size_t i) {
      return bits.row(i * 3);
    }
  };

  for (auto poId = uint32_t{0}; poId < poBlocks.size(); ++poId) {
    auto const blk = poBlocks[poId];
    blockInfos[blk].poId = poId;
    incompleteQ.push(poId);
  }

  /*
   * 1. Compute a transfer function for each block.  Add must-alias-set ids to
   * the block's GEN set if it has a use in the block that implies its
   * reference count can't be zero on entry to the block.
   */
  for (auto& blk : poBlocks) {
    auto& binfo = blockInfos[blk];
    for (auto& inst : blk->instrs()) {
      weaken_decref_step(env, inst, [&] (ASetID id) {
        binfo.gen.set(id);
      });
    }
  }
  FTRACE(5, "summaries:\n{}\n",
    [&] () -> std::string {
      auto ret = std::string{};
      for (auto& blk : poBlocks) {
        fmt::format_to(std::back_inserter(ret), " B{: <3}: {}\n",
          blk->id(),
          show(blockInfos[blk].gen)
        );
      }
      return ret;
    }()
  );

  /*
   * 2. Compute fixed point on the out_used sets.
   *
   *   out_used = isect(succ) in_used
   *    in_used = out_used | gen
   *
   * Note out_used is empty if there are no successors.
   */
  do {
    auto const blk = poBlocks[incompleteQ.pop()];
    auto& binfo = blockInfos[blk];

    // Update the output set.
    auto const next = blk->next();
    auto const taken = blk->taken();
    if (next || taken) {
      binfo.out_used.set();
      if (next)  binfo.out_used &= blockInfos[next].in_used;
      if (taken) binfo.out_used &= blockInfos[taken].in_used;
    }

    // Propagate it to the in set.
    auto const in_used = binfo.out_used | binfo.gen;
    // Schedule each predecessor if the input set changed.
    if (binfo.in_used != in_used) {
      binfo.in_used.assign(in_used);
      blk->forEachPred([&] (Block* pred) {
        incompleteQ.push(blockInfos[pred].poId);
      });
    }
  } while (!incompleteQ.empty());

  FTRACE(5, "fixed point:\n{}\n",
    [&] () -> std::string {
      auto ret = std::string{};
      for (auto& blk : poBlocks) {
        fmt::format_to(std::back_inserter(ret), "  B{: <3}: {}\n",
          blk->id(),
          show(blockInfos[blk].out_used)
        );
      }
      return ret;
    }()
  );

  /*
   * 3. Convert DecRefs to DecRefNZ when we've proven the tmp must be used
   *    again.
   *
   * Note that this step clobbers out_used
   */
  for (auto& blk : poBlocks) {
    FTRACE(4, "B{}:\n", blk->id());
    auto& will_be_used = blockInfos[blk].out_used;
    for (auto it = blk->instrs().rbegin(); it != blk->instrs().rend(); ++it) {
      auto& inst = *it;
      FTRACE(4, "  {}\n", inst);

      if (inst.is(DecRef)) {
        auto const id = env.asetMap[inst.src(0)];
        if (id != -1 && will_be_used.test(id)) {
          FTRACE(2, "    ** weakening {} to DecRefNZ\n", inst);
          inst.setOpcode(DecRefNZ);
        }
      }

      weaken_decref_step(env, inst, [&] (ASetID id) {
        will_be_used.set(id);
      });
      FTRACE(5, "    {}\n", show(will_be_used));
    }
  }

  FTRACE(2, "\n");
}

//////////////////////////////////////////////////////////////////////

// Helper to determine whether an inc/dec can be moved across
// an instruction.
bool irrelevant_inst(const IRInstruction& inst) {
  auto const effects = memory_effects(inst);
  return match<bool>(
    effects,
    // Pure loads, stores, and IrrelevantEffects do not read or write any
    // object reference counts.
    [&] (const PureLoad&) { return true; },
    [&] (const PureStore&) { return true; },
    [&] (const IrrelevantEffects&) { return true; },
    [&] (const PureInlineCall&) { return true; },

    // Inlining related instructions can manipulate the frame but don't
    // observe reference counts.
    [&] (const GeneralEffects& g) {
      if (inst.is(LeaveInlineFrame, InlineCall, EnterInlineFrame)) {
        return true;
      }
      if (inst.consumesReferences()) return false;

      if (g.loads <= AEmpty &&
          g.backtrace.empty() &&
          g.stores <= AEmpty &&
          g.inout <= AEmpty) {
        return true;
      }
      return false;
    },

    // Everything else may.
    [&] (const CallEffects&)       { return false; },
    [&] (const ReturnEffects&)     { return false; },
    [&] (const ExitEffects&)       { return false; },
    [&] (const UnknownEffects&)    { return false; }
  );
}

//////////////////////////////////////////////////////////////////////

void find_alias_sets(Env& env) {
  FTRACE(2, "find_alias_sets --------------------------------------\n");

  auto add = [&] (SSATmp* tmp) {
    if (!tmp->type().maybe(TCounted)) return;

    auto& id = env.asetMap[tmp];
    if (id != -1) return;

    auto canon = canonical(tmp);
    if (env.asetMap[canon] != -1) {
      id = env.asetMap[canon];
    } else {
      auto const cinst = canon->inst();
      if (cinst->is(DefLabel)) {
        auto const idx = cinst->findDst(canon);
        assertx(idx < cinst->numDsts());

        int pid = -2;
        cinst->block()->forEachSrc(idx, [&](IRInstruction*, SSATmp* src) {
          if (pid == -1) return;
          src = canonical(src);
          if (src == canon) return;
          auto const srcId = env.asetMap[src];
          if (srcId == -1) {
            pid = -1;
            return;
          }
          if (pid == -2) {
            pid = srcId;
          } else if (pid != srcId) {
            pid = -1;
            return;
          }
        });
        if (pid >= 0) id = pid;
      }
      if (id == -1) {
        id = env.asets.size();
        env.asetMap[canon] = id;
        env.asets.push_back(MustAliasSet { canon->type(), canon });
      }
    }

    FTRACE(2,  "  t{} -> {} ({})\n", tmp->id(), id, canon->toString());
  };

  for (auto& blk : env.rpoBlocks) {
    for (auto& inst : blk->instrs()) {
      for (auto src : inst.srcs()) add(src);
      for (auto dst : inst.dsts()) add(dst);
    }
  }

  auto const num_sets = env.asets.size();
  FTRACE(2, "   {} must alias sets\n", num_sets);

  // Populate the may-alias-sets for each must-alias-set.
  jit::vector<jit::flat_set<ASetID>::sequence_type> v(num_sets);
  for (auto i = uint32_t{0}; i < num_sets; ++i) {
    v[i].reserve(num_sets);
    for (auto j = i + 1; j < num_sets; ++j) {
      auto& ai = env.asets[i];
      auto& aj = env.asets[j];
      bool const may_alias =
        (ai.widestType & aj.widestType).maybe(TCounted);
      if (may_alias) {
        v[i].emplace_back(j);
        v[j].emplace_back(i);
      }
    }
  }

  for (auto i = uint32_t{0}; i < num_sets; ++i) {
    env.asets[i].may_alias.adopt_sequence(
        boost::container::ordered_unique_range, std::move(v[i]));
  }

  FTRACE(3, "may-alias-info:\n{}\n",
    [&] () -> std::string {
      auto ret = std::string{};
      for (auto asetID = uint32_t{0}; asetID < num_sets; ++asetID) {
        fmt::format_to(std::back_inserter(ret), "  {: <2}:", asetID);
        for (auto other : env.asets[asetID].may_alias) {
          fmt::format_to(std::back_inserter(ret), " {: <2}", other);
        }
        ret.push_back('\n');
      }
      return ret;
    }()
  );
}

//////////////////////////////////////////////////////////////////////

DEBUG_ONLY bool check_state(const RCState& state) {
  auto num_pending_requirements = uint32_t{0};
  for (auto asetID = uint32_t{0}; asetID < state.asets.size(); ++asetID) {
    auto& set = state.asets[asetID];

    // All reference count bounds are non-negative.
    always_assert(set.pending_reductions >= 0);
    if (set.pending_reductions) ++num_pending_requirements;
    always_assert(set.unsupported_refs >= 0);
    always_assert(set.lower_bound >= set.unsupported_refs);
    always_assert(set.upper_bound >= set.lower_bound);
    always_assert(set.lower_bound == kInfReferences ||
                  set.lower_bound <= kMaxPlausibleReferences);
    always_assert(set.upper_bound == kInfReferences ||
                  set.upper_bound <= kMaxPlausibleReferences);
    always_assert(set.lower_bound != kInfReferences ||
                  (!set.unsupported_refs && !set.pending_reductions &&
                   set.memory_support.none()));

    // If this set has support bits, then the reverse map in the state is
    // consistent with it.
    bitset_for_each_set(
      set.memory_support,
      [&](size_t id) { always_assert(state.support_map[id] == asetID); }
    );
  }

  always_assert(state.num_pending_requirements == num_pending_requirements);

  // Check other direction on the support_map.
  for (auto id = uint32_t{0}; id < state.support_map.size(); ++id) {
    auto const asetID = state.support_map[id];
    if (asetID != -1) {
      always_assert_flog(
        asetID >= 0,
        "asetID {} must be non-negative",
        asetID
      );
      always_assert_flog(
        asetID < state.asets.size(),
        "aset ID {} is out of bounds of state.asets size {}",
        asetID,
        state.asets.size()
      );
      always_assert_flog(
        state.asets[asetID].memory_support[id],
        "expected aset {} to have support in location {}",
        asetID,
        id
      );
    }
  }

  return true;
}

//////////////////////////////////////////////////////////////////////

RCState entry_rc_state(Env& env) {
  auto ret = RCState{};
  ret.initialized = true;
  ret.asets.resize(env.asets.size());
  ret.avail = env.mrinfo.info[env.rpoBlocks.front()].avail_in;
  ret.support_map.fill(-1);
  return ret;
}

bool merge_into(ASetInfo& dst, const ASetInfo& src, bool is_back_edge) {
  auto changed = false;

  auto const lower_bound = std::min(dst.lower_bound, src.lower_bound);
  auto const upper_bound = std::max(dst.upper_bound, src.upper_bound);
  auto const pending_reductions =
    std::max(dst.pending_reductions, src.pending_reductions);

  /*
   * We're going to reduce the memory support to the intersection of
   * src and dst's memory support. to avoid worrying about a single
   * location supporting multiple alias sets.
   *
   * When we do that, we have to (logically) increase both src and
   * dst's unsupported_refs by the numberof support locations removed.
   */
  auto const new_memory_support = dst.memory_support & src.memory_support;
  auto const dst_count = dst.memory_support.count();
  auto const src_count = src.memory_support.count();
  auto const new_count = new_memory_support.count();
  auto const dst_delta = safe_cast<int32_t>(dst_count - new_count);
  auto const src_delta = safe_cast<int32_t>(src_count - new_count);

  auto const dst_unsupported_refs = std::min(
    dst.unsupported_refs + dst_delta, dst.lower_bound);
  auto const src_unsupported_refs = std::min(
    src.unsupported_refs + src_delta, src.lower_bound);
  auto const dst_supported_refs = dst.lower_bound - dst_unsupported_refs;
  auto const src_supported_refs = src.lower_bound - src_unsupported_refs;

  auto const supported_refs = std::min(dst_supported_refs, src_supported_refs);
  auto const unsupported_refs = lower_bound - supported_refs;

  if (dst.unsupported_refs != unsupported_refs) {
    dst.unsupported_refs = unsupported_refs;
    changed = true;
  }

  if (dst.lower_bound != lower_bound) {
    dst.lower_bound = lower_bound;
    changed = true;
  }

  if (dst.upper_bound != upper_bound) {
    // A growing upper bound on a backedge may increase without limit.
    // Widen it to infinity immediately.
    dst.upper_bound = is_back_edge ? kInfReferences : upper_bound;
    changed = true;
  }

  if (dst.pending_reductions != pending_reductions) {
    // Pending reductions may grow without limit on a backedge.
    // Widen the count to infinity immediately.
    dst.pending_reductions = is_back_edge ? kInfReferences : pending_reductions;
    changed = true;
  }

  if (dst_delta) {
    assertx(dst.memory_support != new_memory_support);
    dst.memory_support = new_memory_support;
    changed = true;
  }

  return changed;
}

bool merge_into(Env& /*env*/, RCState& dst, const RCState& src, bool is_back_edge) {
  if (!dst.initialized) {
    dst = src;
    return true;
  }

  always_assert(dst.asets.size() == src.asets.size());

  // We'll reconstruct the support_map vector after merging each aset.
  dst.support_map.fill(-1);

  auto changed = false;

  dst.has_unsupported_refs = false;
  dst.num_pending_requirements = 0;
  for (auto asetID = uint32_t{0}; asetID < dst.asets.size(); ++asetID) {
    auto &daset = dst.asets[asetID];
    if (merge_into(daset, src.asets[asetID], is_back_edge)) {
      FTRACE(3, "  info changed for aset ID {}\n", asetID);
      changed = true;
    }

    if (daset.unsupported_refs) dst.has_unsupported_refs = true;
    if (daset.pending_reductions) ++dst.num_pending_requirements;

    bitset_for_each_set(
      daset.memory_support,
      [&](size_t loc) {
        assertx(dst.support_map[loc] == -1);
        dst.support_map[loc] = asetID;
      }
    );
  }

  assertx(check_state(dst));

  return changed;
}

bool is_same(const RCState &dstState, const RCState& srcState) {
  assertx(srcState.initialized);
  if (!dstState.initialized) return false;
  for (auto asetID = uint32_t{0}; asetID < dstState.asets.size(); ++asetID) {
    auto& dst = dstState.asets[asetID];
    auto& src = srcState.asets[asetID];

    if (dst.lower_bound != src.lower_bound ||
        dst.upper_bound != src.upper_bound ||
        dst.unsupported_refs != src.unsupported_refs ||
        dst.pending_reductions != src.pending_reductions ||
        dst.memory_support != src.memory_support) {
      return false;
    }
  }

  return true;
}

//////////////////////////////////////////////////////////////////////

template <class Fn>
void if_aset(Env& env, SSATmp* tmp, Fn fn) {
  auto const asetID = env.asetMap[tmp];
  if (asetID == -1) { assertx(!tmp->type().maybe(TCounted)); return; }
  fn(asetID);
}

Optional<ASetID> lookup_aset(Env& env, SSATmp* tmp) {
  auto const asetID = env.asetMap[tmp];
  if (asetID != -1) return asetID;
  assertx(!tmp->type().maybe(TCounted));
  return std::nullopt;
}

void reduce_lower_bound(Env& /*env*/, RCState& state, uint32_t asetID) {
  FTRACE(5, "      reduce_lower_bound {}\n", asetID);
  auto& aset = state.asets[asetID];
  if (aset.lower_bound == kInfReferences) return;
  aset.lower_bound = std::max(aset.lower_bound - 1, 0);
  if (aset.unsupported_refs > aset.lower_bound) {
    aset.unsupported_refs = aset.lower_bound;
  }
}

void add_unbalanced_decref(Env& env, RCState& state, ASetID asetID) {
  for (auto id : env.asets[asetID].may_alias) {
    auto& aset = state.asets[id];
    if (aset.lower_bound == kInfReferences) continue;
    if (!aset.pending_reductions) ++state.num_pending_requirements;
    if (aset.pending_reductions < kInfReferences) ++aset.pending_reductions;
  }
}

bool apply_pending_reductions(RCState& state, ASetID asetID) {
  auto& aset = state.asets[asetID];
  if (!aset.pending_reductions) return false;
  assertx(state.num_pending_requirements);
  --state.num_pending_requirements;
  FTRACE(4, "    pending reductions for {}: {}\n",
         asetID, aset.pending_reductions);
  aset.lower_bound = std::max(aset.lower_bound - aset.pending_reductions, 0);
  aset.unsupported_refs = std::min(aset.unsupported_refs, aset.lower_bound);
  aset.pending_reductions = 0;
  return true;
}

void observe_pending_requirements(RCState& state, ASetID asetID,
                                  PreAdder add_node) {
  if (apply_pending_reductions(state, asetID)) add_node(asetID, NReq{1});
}

void observe_pending_requirements(RCState& state, PreAdder add_node) {
  if (!state.num_pending_requirements) return;
  for (auto id = uint32_t{0}; id < state.asets.size(); ++id) {
    observe_pending_requirements(state, id, add_node);
  }
}

/*
 * Note that its ok to clear the memory support and continue in
 * pessimize situations. If later, after increasing the lower bound,
 * we see an event that could DecRef a memory location that was
 * previously in the support, there are two possibilities:
 *
 * - the location really is still in the support, and the event
 *   decreases the refcount; but in that case, our lower_bound doesn't
 *   include the reference coming from that location, so it was
 *   previously at least one too low, and so there's nothing for us to
 *   do.
 *
 * - the location was no longer in the support, and the event has no
 *   effect; again there's nothing for us to do.
 */
void pessimize_one(Env& /*env*/, RCState& state, ASetID asetID,
                   PreAdder add_node) {
  auto& aset = state.asets[asetID];

  // Nothing to pessimize, the value is TUncounted.
  if (aset.lower_bound == kInfReferences) return;

  aset.upper_bound = kInfReferences;
  if (!aset.lower_bound && aset.memory_support.none()) return;
  FTRACE(2, "      {} pessimized\n", asetID);
  if (aset.pending_reductions) --state.num_pending_requirements;
  aset.lower_bound = 0;
  aset.unsupported_refs = 0;
  aset.pending_reductions = 0;
  bitset_for_each_set(
    aset.memory_support,
    [&](size_t id) { state.support_map[id] = -1; }
  );
  aset.memory_support.reset();
  add_node(asetID, NReq{kInfReferences});
}

void pessimize_all(Env& env, RCState& state, PreAdder add_node) {
  FTRACE(3, "    pessimize_all\n");
  observe_pending_requirements(state, add_node);
  for (auto asetID = uint32_t{0}; asetID < state.asets.size(); ++asetID) {
    pessimize_one(env, state, asetID, add_node);
  }
}

void pessimize_upper_bound_one_aset(Env& /*env*/, RCState& state, ASetID asetID) {
  auto& aset = state.asets[asetID];
  FTRACE(2, "      {} upper bound pessimized\n", asetID);
  aset.upper_bound = kInfReferences;
}

void pessimize_upper_bound_all_asets(Env& env, RCState& state) {
  FTRACE(3, "    pessimize_all_upper_bounds\n");
  for (auto asetID = uint32_t{0}; asetID < state.asets.size(); ++asetID) {
    pessimize_upper_bound_one_aset(env, state, asetID);
  }
}

void unsupport_all(Env& /*env*/, RCState& state) {
  FTRACE(3, "    unsupport_all\n");
  for (auto& aset : state.asets) {
    if (aset.lower_bound == kInfReferences) continue;
    aset.unsupported_refs = aset.lower_bound;
    if (aset.unsupported_refs) state.has_unsupported_refs = true;
    bitset_for_each_set(
      aset.memory_support,
      [&](size_t id) { state.support_map[id] = -1; }
    );
    aset.memory_support.reset();
  }
}

void observe(Env& env, RCState& state, ASetID asetID, PreAdder add_node) {
  auto constexpr level = 2;
  add_node(asetID, NReq{level});
  auto const diff = level - state.asets[asetID].lower_bound;
  if (diff <= 1) {
    /*
     * This would be a Req{1} in the may-alias sets, but there's always an
     * implicit Req{1} everywhere because we aren't allowed to change reference
     * counts unless we "know about two references" (see the "more about
     * memory" section in the docs).  The only time we need to explicitly put
     * Req{1} nodes into RCFGs is if we've reduced the lower bound, which we
     * aren't going to do here, so we can just leave it out.
     */
    return;
  }
  FTRACE(3, "    unbalanced observe (diff: {}):\n", diff);
  for (auto may_id : env.asets[asetID].may_alias) {
    add_node(may_id, NReq{diff});
  }
}

void observe_all(Env& env, RCState& state, PreAdder add_node) {
  FTRACE(3, "    observe_all\n");
  observe_pending_requirements(state, add_node);
  for (auto asetID = uint32_t{0}; asetID < state.asets.size(); ++asetID) {
    add_node(asetID, NReq{kInfReferences});
  }
}

void decrease_upper_bound(Env& /*env*/, RCState& state, ASetID asetID) {
  auto& aset = state.asets[asetID];
  if (aset.upper_bound == kInfReferences) return;
  aset.upper_bound--;
  assertx(aset.upper_bound >= aset.lower_bound);
  FTRACE(5, "    decreased upper bound for aset {} by 1: ub({})\n",
    asetID, aset.upper_bound);
}

void increase_upper_bound(Env& /*env*/, RCState& state, ASetID asetID) {
  auto& aset = state.asets[asetID];
  if (aset.upper_bound < kInfReferences) aset.upper_bound++;
  assertx(aset.upper_bound >= aset.lower_bound);
  FTRACE(5, "    increased upper bound for aset {} by 1: new_ub({})\n",
    asetID, aset.upper_bound);
}

void may_incref(Env& env, RCState& state, ASetID asetID) {
  /*
   * We must increase the upper bound of any alias set that could alias with asetID by 1.
   */
  assertx(asetID != -1);
  increase_upper_bound(env, state, asetID);
  for (auto may_id : env.asets[asetID].may_alias) {
    increase_upper_bound(env, state, may_id);
  }
}

void may_decref(Env& env, RCState& state, ASetID asetID, PreAdder add_node) {
  auto& aset = state.asets[asetID];

  // Nothing to decref, the value is TUncounted.
  if (aset.lower_bound == kInfReferences) return;

  auto const balanced = aset.lower_bound > aset.unsupported_refs;
  reduce_lower_bound(env, state, asetID);
  add_node(asetID, NReq{1});
  FTRACE(3, "    {} lb: {}({})\n",
         asetID, aset.lower_bound, aset.unsupported_refs);

  if (balanced) {
    /*
     * The intention here is to reduce just the assumed reference of may-alias sets by 1
     * However, we don't distinguish which of the unsupported references are assumed
     * Therefore, we conservatively decrease the unsupported reference by 1
     */
    FTRACE(4, "    adding balanced decref: {}\n", asetID);
    if (!state.has_unsupported_refs) return;
    for (auto may_id : env.asets[asetID].may_alias) {
      auto& may_aset = state.asets[may_id];
      if (!may_aset.unsupported_refs) continue;
      may_aset.lower_bound -= 1;
      may_aset.unsupported_refs -= 1;
      FTRACE(5, "    dropping unsupported ref for {}: {}({})\n",
             may_id, may_aset.lower_bound, may_aset.unsupported_refs);
    }
  } else {
    FTRACE(4, "    adding unbalanced decref: {}\n", asetID);
    add_unbalanced_decref(env, state, asetID);
  }
}

void kill_unsupported_refs(RCState& state, PreAdder add_node = {}) {
  if (state.has_unsupported_refs) {
    FTRACE(3, "    killing all unsupported refs\n");
    auto id = 0;
    for (auto& aset : state.asets) {
      if (aset.unsupported_refs) {
        assertx(aset.unsupported_refs <= aset.lower_bound);
        aset.lower_bound -= aset.unsupported_refs;
        aset.unsupported_refs = 0;
        if (aset.lower_bound < 2) add_node(id, NReq{1});
        FTRACE(5, "      {} lb: {}(0)\n", id, aset.lower_bound);
      }
      id++;
    }
    state.has_unsupported_refs = false;
  }
}

// Returns true if we actually removed the support (i.e. we accounted
// for it by reducing a lower bound, or increasing unsupported_refs,
// or the location wasn't actually supporting anything right now).
bool reduce_support_bit(Env& env,
                        RCState& state,
                        uint32_t locID,
                        bool may_decref,
                        PreAdder add_node) {
  auto const current_set = state.support_map[locID];
  if (current_set == -1) return true;
  FTRACE(3, "      {} removing support\n", current_set);
  auto& aset = state.asets[current_set];
  if (aset.lower_bound == aset.unsupported_refs) {
    // If decrefs can actually occur here, we should have killed the
    // unsupported refs first.
    assertx(!may_decref || !aset.unsupported_refs);
    /*
     * We can't remove the support bit, and we have no way to account for the
     * reduction in lower bound.  There are two cases to consider:
     *
     *   o If may_decref is false (we're trying to move the support
     *     from this aset to another, but no refcounts are changing)
     *     we simply need to report that we failed.
     *
     *   o If may_decref is true we treat this as an unbalanced decref
     *     and reduce support on all may-alias asets. As we have treated
     *     this as an ordinary decref we can safely remove the memory
     *     support from this aset.
     */
    if (!may_decref) return false;
    FTRACE(4, "    adding unbalanced decref: {}\n", current_set);
    add_unbalanced_decref(env, state, current_set);
  }
  aset.memory_support.reset(locID);
  state.support_map[locID] = -1;
  if (may_decref) {
    reduce_lower_bound(env, state, current_set);
  } else {
    aset.unsupported_refs++;
    state.has_unsupported_refs = true;
    FTRACE(5, "    {} lb: {}({})\n",
           current_set, aset.lower_bound, aset.unsupported_refs);
  }
  // Because our old lower bound is non-zero (from the memory support), we
  // don't need to deal with the possibility of may-alias observes or may-alias
  // decrefs here.
  add_node(current_set, NReq{1});
  return true;
}

// Returns true if reduce_support_bit succeeded on every support bit in the
// set.
bool reduce_support_bits(Env& env,
                         RCState& state,
                         ALocBits set,
                         bool may_decref,
                         PreAdder add_node) {
  FTRACE(2, "    remove: {}\n", show(set));
  auto ret = true;
  bitset_for_each_set(
    set,
    [&](uint32_t locID) {
      if (!reduce_support_bit(env, state, locID, may_decref, add_node)) {
        ret = false;
      }
    }
  );
  return ret;
}

// Returns true if we completely accounted for removing the support.  If it
// returns false, the support bits may still be marked on some must-alias-sets.
// (See pure_load.)
bool reduce_support(Env& env,
                    RCState& state,
                    AliasClass aclass,
                    bool may_decref,
                    PreAdder add_node) {
  if (aclass == AEmpty) return true;
  FTRACE(3, "    reduce support {}{}\n",
         std::string{may_decref ? "(may_decref) " : ""}, show(aclass));
  if (may_decref && state.has_unsupported_refs) {
    kill_unsupported_refs(state, add_node);
  }
  auto const alias = env.ainfo.may_alias(aclass);
  return reduce_support_bits(env, state, alias, may_decref, add_node);
}

void drop_support_bit(Env& /*env*/, RCState& state, uint32_t bit) {
  auto const current_set = state.support_map[bit];
  if (current_set != -1) {
    FTRACE(3, "    {} dropping support {}\n", current_set, bit);
    state.asets[current_set].memory_support.reset(bit);
    state.support_map[bit] = -1;
  }
}

void drop_support_bits(Env& env, RCState& state, ALocBits bits) {
  FTRACE(3, "    drop support {}\n", show(bits));
  bitset_for_each_set(
    bits,
    [&] (uint32_t bit) {
      drop_support_bit(env, state, bit);
    }
  );
}

void create_store_support(Env& env, RCState& state, AliasClass dst, SSATmp* tmp,
                          PreAdder /*add_node*/) {
  if_aset(env, tmp, [&] (ASetID asetID) {
    auto& aset = state.asets[asetID];

    // No memory support tracked for TUncounted.
    if (aset.lower_bound == kInfReferences) return;

    auto const meta = env.ainfo.find(dst);
    if (meta) {
      FTRACE(3, "    {} adding support in {}\n", asetID, show(dst));

      /*
       * This exact bit can't be set anymore (because the caller of this
       * function cleared it via a call to drop_support_bit, if we had a meta).
       *
       * Since we're doing a store, we don't need to worry about whether other
       * bits from meta->conflicts are supporting something, even if one of
       * them is actually supporting this same set.  The reason to consider
       * whether it would matter is that we have to make sure we don't violate
       * the exclusivity rule of lower bounds: this won't happen because we're
       * not increasing our lower bound here, and we're only changing which
       * memory locations should cause reductions in the lower bound later.
       *
       * Basically if we assume the lower bound "exclusivity" property is
       * holding before this store operation, it will still hold after it:
       * we'll still clear the support if we do another load or store from
       * anything inside this conflict set, and memory operations that could
       * decref any one of them will reduce the support for all of them.
       */
      assertx(state.support_map[meta->index] == -1);

      state.support_map[meta->index] = asetID;
      aset.memory_support.set(meta->index);
      return;
    }

    if (aset.lower_bound == aset.unsupported_refs) {
      FTRACE(3, "    {} causing unsupport_all\n", asetID);
      unsupport_all(env, state);
      return;
    }

    FTRACE(3, "    {} store adds an unsupported_ref\n", asetID);
    aset.unsupported_refs++;
    state.has_unsupported_refs = true;
  });
}

//////////////////////////////////////////////////////////////////////

void handle_call(Env& env, RCState& state, const IRInstruction& /*inst*/,
                 const CallEffects& e, PreAdder add_node) {
  // The call can affect any unsupported_refs
  observe_pending_requirements(state, add_node);
  kill_unsupported_refs(state);
  pessimize_upper_bound_all_asets(env, state);

  // Figure out locations the call may cause stores to, then remove any memory
  // support on those locations.
  auto bset = ALocBits{};
  bset |= env.ainfo.may_alias(e.inputs);
  bset |= env.ainfo.may_alias(e.actrec);
  bset |= env.ainfo.may_alias(e.outputs);
  bset |= env.ainfo.may_alias(AHeapAny);
  bset |= env.ainfo.may_alias(ARdsAny);
  reduce_support_bits(env, state, bset, true, add_node);
}

void pure_load(Env& env,
               RCState& state,
               AliasClass src,
               SSATmp* dst,
               PreAdder add_node) {
  auto const meta = env.ainfo.find(src);
  if (!meta) return;
  if (!state.avail.test(meta->index)) return;

  /*
   * If currently supporting a different ASet, try to clear any support there
   * first.  It is possible we won't successfully clear the support if it was
   * supporting a set that currently has a lower bound of zero---in that
   * situation we can't raise the lower bound or support the new loaded set,
   * because it could violate the exclusivity rule.
   */
  FTRACE(2, "    load: {}\n", show(src));
  if (!reduce_support(env, state, src, false, add_node)) {
    FTRACE(2, "    couldn't remove all pre-existing support\n");
    return;
  }

  if_aset(env, dst, [&] (ASetID asetID) {
    FTRACE(2, "    {} adding support: {}\n", asetID, show(src));
    auto& aset = state.asets[asetID];

    // No memory support tracked for TUncounted.
    if (aset.lower_bound == kInfReferences) return;

    ++aset.lower_bound;
    aset.memory_support.set(meta->index);
    state.support_map[meta->index] = asetID;
  });
}

void pure_store(Env& env,
                RCState& state,
                AliasClass dst,
                SSATmp* tmp,
                PreAdder add_node) {
  /*
   * First, handle the effects of the store on memory support.  See the docs
   * above in "Effects of Pure Stores on Memory Support" for an explanation.
   */
  if (auto const meta = env.ainfo.find(dst)) {
    drop_support_bit(env, state, meta->index);
  }

  /*
   * Now handle the effects of the store on the aset for the value being
   * stored.
   */
  if (tmp) create_store_support(env, state, dst, tmp, add_node);
}

//////////////////////////////////////////////////////////////////////

void analyze_mem_effects(Env& env,
                         IRInstruction& inst,
                         RCState& state,
                         PreAdder add_node) {
  auto const effects = canonicalize(memory_effects(inst));
  FTRACE(4, "    mem: {}\n", show(effects));
  match(
    effects,
    [&] (const IrrelevantEffects&) {},

    [&] (const PureInlineCall&) {},

    [&] (const GeneralEffects& x) {
      if (inst.is(CallBuiltin)) {
        observe_pending_requirements(state, add_node);
        kill_unsupported_refs(state, add_node);
        pessimize_upper_bound_all_asets(env, state);
      }

      /*
       * We could have loaded from an untracked location
       * that could be supporting any of the asets
       */
      if (x.loads != AEmpty || x.inout != AEmpty || !x.backtrace.empty()) {
        pessimize_upper_bound_all_asets(env, state);
      }

      // Locations that are killed don't need to be tracked as memory support
      // anymore, because nothing can load that pointer (and then decref it)
      // anymore without storing over the location first.
      drop_support_bits(env, state, env.ainfo.expand(x.kills));
      // Locations in the stores set may be stored to with a 'normal
      // write barrier', decreffing the pointer that used to be there.
      // Various inline related instructions have non-empty stores to
      // prevent store sinking, but don't actually change any
      // refcounts, so we don't need to reduce lower bounds.
      auto const may_decref = !inst.is(LeaveInlineFrame, EnterInlineFrame);
      if (may_decref && (x.stores != AEmpty || x.inout != AEmpty)) {
        observe_pending_requirements(state, add_node);
      }
      reduce_support(env, state, x.stores, may_decref, add_node);
      reduce_support(env, state, x.inout, may_decref, add_node);
      // For the moves set, we have no way to track where the pointers may be
      // moved to, so we need to account for it, via unsupported_refs.
      reduce_support(env, state, x.moves, false, add_node);
    },

    [&] (const ReturnEffects&)     {
      observe_all(env, state, add_node);
    },
    [&] (const ExitEffects&) {
      observe_all(env, state, add_node);
    },
    [&] (const UnknownEffects&) {
      pessimize_all(env, state, add_node);
    },
    [&] (const CallEffects& e) {
      handle_call(env, state, inst, e, add_node);
    },
    [&] (const PureStore& x) {
      pure_store(env, state, x.dst, x.value, add_node);
    },
    [&] (const PureLoad& x) {
      pure_load(env, state, x.src, inst.dst(), add_node);
    }
  );
}

/*
 * Return whether an IRInstruction may decref one of its sources, if its taken
 * edge is traversed.
 */
bool consumes_reference_taken(const IRInstruction& inst, uint32_t srcID) {
  switch (inst.op()) {
  // The following consume some arguments only in the event of an exception.
  case SuspendHookAwaitEF:
  case SuspendHookAwaitEG:
  case SuspendHookCreateCont:
  case ReturnHook:
    return srcID == 0;
  default:
    return false;
  }
}

/*
 * Return whether an IRInstruction may decref one of its sources, if its next
 * edge is traversed, but not if its taken edge is traversed.
 *
 * If an instruction consumes a reference both for taken and next, this
 * function should return false.
 */
bool consumes_reference_next_not_taken(const IRInstruction& inst,
                                       uint32_t srcID) {
  return inst.consumesReference(srcID);
}

/*
 * Return whether an IRInstruction may observe the reference count of a
 * specific argument.
 */
bool observes_reference(const IRInstruction& inst, uint32_t srcID) {
  return
    (consumes_reference_next_not_taken(inst, srcID) &&
     !inst.movesReference(srcID)) ||
    consumes_reference_taken(inst, srcID) ||
    (inst.op() == CheckArrayCOW && srcID == 0);
}

//////////////////////////////////////////////////////////////////////

template<typename Propagate>
void rc_analyze_inst(Env& env,
                     IRInstruction& inst,
                     RCState& state,
                     Propagate propagate,
                     PreAdder add_node) {
  FTRACE(2, "  {}\n", inst);

  switch (inst.op()) {
  case Jmp:
    if (inst.numSrcs()) {
      auto const& defLabel = inst.taken()->front();
      assertx(defLabel.is(DefLabel));
      for (auto i = 0; i < inst.numSrcs(); i++) {
        auto const dstID = lookup_aset(env, defLabel.dst(i));
        if (!dstID) continue;
        auto& dstSet = state.asets[*dstID];
        auto const srcID = lookup_aset(env, inst.src(i));
        if (!srcID) {
          dstSet.upper_bound = kInfReferences;
          FTRACE(3, "    DefLabel dstID {} set ub = {}\n", *dstID, dstSet.upper_bound);
          continue;
        }
        auto& srcSet = state.asets[*srcID];
        dstSet.upper_bound = srcSet.upper_bound;
        FTRACE(3, "    DefLabel dstID {} set ub = {}\n", *dstID, dstSet.upper_bound);
        if (dstSet.lower_bound < srcSet.lower_bound) {
          auto const delta = srcSet.lower_bound - dstSet.lower_bound;
          // Add assumed references to dstSet
          dstSet.unsupported_refs += delta;
          dstSet.lower_bound = srcSet.lower_bound;
          state.has_unsupported_refs = true;
          FTRACE(3,
                 "    {} DefLabel lb({}) += {}\n",
                 *dstID, dstSet.lower_bound, delta);
        }
      }
    }
    propagate(inst.taken());
    return;
  case IncRef:
    if_aset(env, inst.src(0), [&] (ASetID asetID) {
      auto& aset = state.asets[asetID];

      // IncRef on TUncounted has no effects.
      if (aset.lower_bound == kInfReferences) return;

      observe_pending_requirements(state, asetID, add_node);

      if (!aset.lower_bound) {
        /*
         * We incref aset so there must be a ref to it, otherwise the input program is malformed.
         * Therefore, we assume aset has atleast a single reference.
         * This is a special unsupported ref because it may already be accounted for,
         * by the supported component of the aset's may-aliases.
         */
        FTRACE(3, "    {} unsupported_refs += 1\n", asetID);
        assertx(!aset.unsupported_refs);
        aset.lower_bound = aset.unsupported_refs = 1;
        state.has_unsupported_refs = true;
      }
      if (aset.lower_bound < 2) {
        /*
         * Inc/dec PRE compensation must not release an alias before this IncRef.
         * Either set's bound or their combined supported credit can prove it
         * stays live. Dec/inc PRE raises counts, so the pre-increment bound of
         * one already satisfies the requirement in that direction.
         *
         * A later DecRefNZ on an alias may rely on this increment, so observe
         * its earlier pending requirement before adding the new reference.
         * A pre-increment bound of two makes this barrier unnecessary when
         * the values alias; when they differ, this increment cannot supply
         * the NZ guarantee. Later DecRefs apply their own pending reductions.
         */
        for (auto const mayAliasID : env.asets[asetID].may_alias) {
          observe_pending_requirements(state, mayAliasID, add_node);
          add_node(mayAliasID, NReq{1}, asetID);
        }
      }
      add_node(asetID, NInc{&inst});
      may_incref(env, state, asetID);
      ++aset.lower_bound;
      FTRACE(3, "    {} lb: {}({})\n",
             asetID, aset.lower_bound, aset.unsupported_refs);
    });
    return;
  case DecRef:
  case DecRefNZ:
    {
      auto old_lb = int32_t{0};
      if_aset(env, inst.src(0), [&] (ASetID asetID) {
        apply_pending_reductions(state, asetID);
        auto& aset = state.asets[asetID];
        auto src_type = inst.src(0)->type();
        if (inst.op() == DecRefNZ && aset.lower_bound < 2) {
          /*
           * We DecRefNZ aset so there must be at least 2 refs to it, otherwise the input program is malformed.
           * Therefore, we add 2 assumed references to aset.
           * These are special unsupported refs because they may already be accounted for,
           * by the supported component of the aset's may-aliases.
           */

          FTRACE(3, "    {} unsupported_refs += {}\n",
                 asetID, 2 - aset.lower_bound);
          assertx(aset.unsupported_refs <= aset.lower_bound);
          aset.unsupported_refs += 2 - aset.lower_bound;
          aset.lower_bound = 2;
          state.has_unsupported_refs = true;
        }
        add_node(asetID, NDec{&inst});
        old_lb = aset.lower_bound;
        may_decref(env, state, asetID, add_node);
        /*
         * We only modify upper bound for guaranteed counted asets.
         * For asets that could be persistent, the upper bound must be kInfReferences.
         * However, this invariant may be temporarily broken at the moment.
         * One situation is if we are currently within a loop body
         * and the Deflabel block corresponding to the loop beginning hasn't been merged
         * with all of its ancestors because some of them are yet to be processed.
         * It's possible that it has only been merged
         * with the ancestors that provide counted values (upper_bound <= kInfReferences)
         * but not with those that provide uncounted values (upper_bound == kInfReferences).
         */
        if (src_type.subtypeOfAny(TCounted)) {
          decrease_upper_bound(env, state, asetID);
        }
      });
      if (old_lb <= 1) analyze_mem_effects(env, inst, state, add_node);
    }
    return;
  default:
    if (!irrelevant_inst(inst)) {
      observe_pending_requirements(state, add_node);
    }
    break;
  }

  /*
   * Before the decision on whether taken branch is traversed, instructions may
   * observe the reference count of their args.  In general these instructions
   * may make decisions based on whether this is the last reference to a tmp,
   * so we have to observe at level 2.
   */
  for (auto srcID = uint32_t{0}; srcID < inst.numSrcs(); ++srcID) {
    if (observes_reference(inst, srcID)) {
      if_aset(env, inst.src(srcID), [&] (ASetID asetID) {
        observe(env, state, asetID, add_node);
      });
    }
  }

  analyze_mem_effects(env, inst, state, add_node);

  /*
   * Propagate across the taken edge if there is one, first doing any
   * may-decrefs that only occur if the taken edge is traversed.  Right now we
   * conservatively leave the effects of those may-decrefs on the next() path
   * also.
   */
  if (auto const taken = inst.taken()) {
    for (auto srcID = uint32_t{0}; srcID < inst.numSrcs(); ++srcID) {
      if (consumes_reference_taken(inst, srcID)) {
        if_aset(env, inst.src(srcID), [&] (ASetID asetID) {
          may_decref(env, state, asetID, add_node);
        });
      }
    }
    propagate(taken);
  }

  /*
   * Handle consumes that only remove the reference if the taken branch is not
   * traversed, after we've propagated to taken.
   *
   * Note that the call to `consumes_reference_next_not_taken' is false if it
   * is consumed for both next /and/ taken, but in that case we'll have already
   * put the effects into the state above, since `consumes_reference_taken'
   * will be true.
   */
  for (auto srcID = uint32_t{0}; srcID < inst.numSrcs(); ++srcID) {

    // The instruction might incref any of the args arbitrary number of times.
    if_aset(env, inst.src(srcID), [&] (ASetID asetID) {
      pessimize_upper_bound_one_aset(env, state, asetID);
    });

    if (consumes_reference_next_not_taken(inst, srcID)) {
      assertx(!consumes_reference_taken(inst, srcID));
      if_aset(env, inst.src(srcID), [&] (ASetID asetID) {
        if (inst.movesReference(srcID)) {
          auto& aset = state.asets[asetID];
          if (aset.lower_bound == kInfReferences) return;
          if (!aset.lower_bound) {
            // Add an assumed reference
            aset.lower_bound = aset.unsupported_refs = 1;
            state.has_unsupported_refs = true;
          } else if (aset.lower_bound > aset.unsupported_refs) {
            aset.unsupported_refs++;
            state.has_unsupported_refs = true;
          }
          return;
        }
        may_decref(env, state, asetID, add_node);
      });
    }
  }

  /*
   * If the instruction produces a reference, we can assume the SSATmp it
   * defines corresponds to a new increment on the lower_bound.
   */
  if (inst.producesReference()) {
    if_aset(env, inst.dst(), [&] (ASetID asetID) {
      auto& aset = state.asets[asetID];
      assertx(aset.unsupported_refs == 0);
      assertx(aset.lower_bound == 0);
      assertx(aset.upper_bound == kInfReferences);
      aset.lower_bound = 1;
      if (inst.producesNewReference() && inst.dst()->type().subtypeOfAny(TCounted)) {
        aset.upper_bound = 1;
      }
      FTRACE(3, "    {} produced: lb {} ub {}\n",
        asetID, aset.lower_bound, aset.upper_bound
      );
    });
  }
}

/*
 * This is the per-instruction analyze routine for rc_analyze, also used for
 * pre_local_transfer.
 */
template<class Propagate>
void rc_analyze_step(Env& env,
                     IRInstruction& inst,
                     RCState& state,
                     Propagate propagate,
                     PreAdder add_node) {
  rc_analyze_inst(env, inst, state, propagate, add_node);
  // Note: we could use the gen set here to remove support entries when we step
  // the mrinfo, but it's not useful because only CallEffects causes it right
  // now, and SSATmps can't span calls.
  mrinfo_step(env, inst, state.avail);
  assertx(check_state(state));

  if (Cfg::HHIR::InliningAssertMemoryEffects && inst.is(LeaveInlineFrame)) {
    assertx(inst.src(0)->inst()->is(DefCalleeFP));
    auto const fp = inst.src(0);
    auto const callee = fp->inst()->extra<DefCalleeFP>()->func;

    auto const assertDead = [&] (AliasClass acls, const char* what) {
      auto const canon = canonicalize(acls);
      auto const mustBeDead = env.ainfo.expand(canon);
      bitset_for_each_set(
        mustBeDead,
        [&] (size_t id) {
          always_assert_flog(
            state.support_map[id] == -1,
            "Detected that {} location was still used as reference support "
            "after accounting for all effects at an LeaveInlineFrame position\n"
            "    Locations: {}\n",
            what,
            show(mustBeDead)
          );
        }
      );
    };

    // Assert that all of the locals and iterators for this frame as well as
    // the ActRec itself and the minstr state have been marked dead.
    for_each_frame_location(fp, callee, assertDead);
  }
}

//////////////////////////////////////////////////////////////////////

/*
 * Find the fixed point. If reprocess is non-null, add each block we
 * process to it.
 */
void rc_analyze_worklist(Env& env,
                         RCAnalysis& rca,
                         dataflow_worklist<uint32_t>& incompleteQ,
                         std::set<uint32_t>* reprocess) {
  std::set<const Block*> reinited;
  do {
    auto const id = incompleteQ.pop();
    if (reprocess) reprocess->insert(id);
    auto const blk = env.rpoBlocks[id];
    FTRACE(2, "B{}:\n", blk->id());
    auto state = rca.info[blk].state_in;
    auto rpoID = rca.info[blk].rpoId;

    auto propagate = [&] (Block* target) {
      FTRACE(2, "   -> {}\n", target->id());
      auto& tinfo = rca.info[target];
      if (target->numPreds() == 1) {
        // With a single predecessor tinfo.state_in should always
        // simply be a copy of state. Special casing this will give
        // better results than merging when we've made changes to blk
        // or its preds.
        if (!is_same(tinfo.state_in, state)) {
          tinfo.state_in = state;
          incompleteQ.push(tinfo.rpoId);
        }
        return;
      }

      if (reprocess && reinited.insert(target).second) {
        // When we're reprocessing a set of blocks that have been
        // modified, we need to restart the analysis of their
        // successors, to ensure they get the most refined info
        // possible. We could optimize this by storing the out states
        // for each block - but that would cost a lot of memory
        // (potentially two out states for each block).
        FTRACE(2, "   re-init B{}\n", target->id());
        tinfo.state_in = state;
        incompleteQ.push(tinfo.rpoId);
        target->forEachPred(
          [&] (Block* pred) {
            if (pred != blk) incompleteQ.push(rca.info[pred].rpoId);
          }
        );
        return;
      }
      auto const changed = merge_into(env, tinfo.state_in, state, tinfo.rpoId < rpoID);
      if (changed) incompleteQ.push(tinfo.rpoId);
    };

    for (auto& inst : blk->instrs()) {
      rc_analyze_step(env, inst, state, propagate, PreAdder{});
    }
    if (auto const next = blk->next()) propagate(next);
  } while (!incompleteQ.empty());
}

RCAnalysis rc_analyze(Env& env) {
  FTRACE(1, "rc_analyze -----------------------------------------\n");

  auto ret = RCAnalysis{env.unit};

  /*
   * Initialize worklist to contain the entry block, and set up all the
   * BlockInfo rpoIds.
   */
  auto incompleteQ = dataflow_worklist<uint32_t>(env.rpoBlocks.size());
  for (auto rpoId = uint32_t{0}; rpoId < env.rpoBlocks.size(); ++rpoId) {
    auto& binfo          = ret.info[env.rpoBlocks[rpoId]];
    binfo.rpoId          = rpoId;
    binfo.state_in.avail = env.mrinfo.info[env.rpoBlocks[rpoId]].avail_in;
  }
  incompleteQ.push(0);

  /*
   * Initialize the entry block's in state.
   */
  ret.info[env.rpoBlocks[0]].state_in = entry_rc_state(env);

  rc_analyze_worklist(env, ret, incompleteQ, nullptr);

  return ret;
}

//////////////////////////////////////////////////////////////////////

DEBUG_ONLY std::string show_analysis(Env& env, const RCAnalysis& analysis) {
  auto ret = std::string{};

  for (auto asetID = uint32_t{0}; asetID < env.asets.size(); ++asetID) {
    fmt::format_to(std::back_inserter(ret), "{}: {}\n", asetID,
      *env.asets[asetID].representative);
    for (auto& blk : env.rpoBlocks) {
      auto& state = analysis.info[blk].state_in;
      if (!state.initialized) continue;
      auto& sinfo = state.asets[asetID];
      fmt::format_to(std::back_inserter(ret), "B{: <3}:  {: <2}\n",
        blk->id(),
        sinfo.lower_bound
      );
    }
    ret.push_back('\n');
  }

  return ret;
}

//////////////////////////////////////////////////////////////////////

/*
 * sink_incs() is a simple pass that sinks IncRefs of values that may
 * be uncount past some safe instructions.  These are instructions
 * that cannot observe the reference count and so we can sink IncRefs
 * across them regardless of their lower bounds.  The goal is to sink
 * IncRefs past Check* and Assert* instructions that may provide
 * further type information to enable more specialized IncRefs (or
 * even completely eliminate them).
 */

bool can_sink_inc_through(const IRInstruction& inst) {
  switch (inst.op()) {
    // these refine the type
    case AssertType:
    case AssertLoc:
    case AssertStk:  return true;

    // these commonly occur along with type guards
    case LdLoc:
    case LdStk:
    case LdIterPos:
    case LdPtrIterKey:
    case LdPtrIterVal:
    case GetDictPtrIter:
    case GetKeysetPtrIter:
    case BespokeIterGetKey:
    case BespokeIterGetVal:
    case IntAsPtrToElem:
    case DefCalleeFP:
    case EnterInlineFrame:
    case LeaveInlineFrame:
    case InlineCall:
    case FinishMemberOp:
    case Nop:        return true;

    default:         return false;
  }
}

bool sink_incs(Env& env) {
  FTRACE(2, "sink_incs ---------------------------------\n");
  jit::vector<IRInstruction*> incs;

  // Find all IncRefs in the unit.
  for (auto& blk : env.rpoBlocks) {
    for (auto& inst : *blk) {
      if (inst.is(IncRef)) {
        incs.push_back(&inst);
      }
    }
  }

  // Process each of the IncRefs, including new ones that are created
  // along the way.
  auto sunk = false;
  while (!incs.empty()) {
    auto inc = incs.back();
    incs.pop_back();

    auto block  = inc->block();
    auto bcctx  = inc->bcctx();
    auto tmp    = inc->src(0);
    if (!tmp->type().maybe(TUncounted)) continue;

    auto iter = block->iteratorTo(inc);
    auto iterOrigSucc = ++iter;
    while (can_sink_inc_through(*iter)) {
      iter++;
    }

    auto const& succ = *iter;
    if (succ.is(CheckType)) {
      // We've split critical edges, so `next' and 'taken' blocks can't have
      // other predecessors.  Therefore, `block' dominates both the 'next' and
      // the 'taken' block, and so does the block defining `tmp'.
      assertx(block->taken()->numPreds() == 1);
      assertx(block->next()->numPreds() == 1);
      auto const new_taken = env.unit.gen(IncRef, bcctx, tmp);
      auto const new_next  = env.unit.gen(IncRef, bcctx, tmp);
      block->taken()->prepend(new_taken);
      block->next()->prepend(new_next);
      incs.push_back(new_taken);
      incs.push_back(new_next);
      FTRACE(2, "    ** sink_incs: {} -> {}, {}\n",
             *inc, *new_taken, *new_next);
      remove_helper(env.unit, inc);
      sunk = true;
    } else if (succ.is(Jmp)) {
      // try to sink through trivial jumps (to blocks with one predecessor)
      assertx(block->taken() != nullptr);
      if (block->taken()->numPreds() > 1) continue;

      auto const new_taken = env.unit.gen(IncRef, bcctx, tmp);
      block->taken()->prepend(new_taken);
      incs.push_back(new_taken);
      FTRACE(2, "    ** sink_incs: {} -> {}\n", *inc, *new_taken);
      remove_helper(env.unit, inc);

    } else if (iter != iterOrigSucc) {
      // insert the inc right before succ if we advanced any instruction
      auto const new_inc = env.unit.gen(IncRef, bcctx, tmp);
      block->insert(iter, new_inc);
      FTRACE(2, "    ** sink_incs: {} -> {}\n", *inc, *new_inc);
      remove_helper(env.unit, inc);
    }
  }

  return sunk;
}

//////////////////////////////////////////////////////////////////////
// Partial redundancy elimination of IncRef/DecRef pairs
//
// This pass chooses a direction - either look for Incs followed by
// Decs or Decs followed by Incs. It then computes availability for
// the kind that comes first, and anticipability for the kind that
// comes second. Lets assume we're doing Incs follwed by Decs.
//
// An I is partially redundant if it's locally available, and
// partially anticipated out, because on some of the paths from here
// to the exit of the region there is a D with no interference in
// between. Similarly a D is partially redundant if it's locally
// anticipated, and partially available in.
//
// Note that being partially redundant doesn't necessarily mean we can
// improve things - eg in the following case, both I and D are
// partially redundant, but we would have to insert both a D and an I
// to make them redundant; and then we could redo the optimization to
// put things back to how they started.
//
//  AntLoc +---+     +---+
//  AntIn  | D |     |   |
//  PavlIn +---+     +---+ PavlIn
//           ^         ^
//            \       /
//             \     /
//              \   /
//      PantOut +---+
//      PantIn  |   |
//      PavlIn  +---+
//                |
//      PantOut +---+
//      PantIn  |   |
//      PavlIn  +---+
//               ^ ^
//              /   \
//             /     \
//            /       \
// PantOut +---+     +---+ PantOut
// AvlOut  | I |     |   |
// AvlLoc  +---+     +---+
//
// Generally, if we have a partially redundant D we can recursively
// walk its predecessors stopping when we find a place where its fully
// redundant, or where we can insert a D that isn't partially
// redundant, or where we can't insert the D because the SSATmp isn't
// defined, or the D isn't anticipated. If we hit the last case, we
// have to give up on that D, and mark it altered, and not AntIn in
// its block, propagate the attributes again, and start over (which
// may then cause previously redundant Is to be no longer
// redundant). Otherwise the walk must bottom out somewhere in one of
// the other two cases. Anywhere its fully redundant there's nothing
// to do; if its AntOut and PavlIn in at least one successor, we can
// insert a copy.
//
// A couple of examples:
//
// AntLoc +---+     +---+ AntLoc        The Ds are partially redundant,
// AntIn  | D |     | D | AntIn    <--  and we can insert a D to make
// PavlIn +---+     +---+ PavlIn        them fully redundant, so we can
//          ^         ^                 delete them.
//           \       /
//            \     /
//             \   /
//      AntOut +---+
//      AntIn  |   |
//      PavlIn +---+
//               |
//      AntOut +---+
//      AntIn  |   |
//      PavlIn +---+
//              ^ ^
//             /   \
//            /     \
//           /       \
// AntOut +---+     +---+ AntOut      D is AntOut, and I is PavlIn
// AvlOut | I |     |   |         <-- in its successor, so insert D
// AvlLoc +---+     +---+             here. The I is already fully
//                                    redundant, so delete it.
//
//
// AntLoc  +---+
// AntIn   | D |   <-- D is fully redundant, so delete it.
// AvlIn   +---+
//           ^          +---+          I is AvlIn, and D is PantOut
//           |        ->|   |     <--  in its predecessor, so insert
//           |      /   +---+ AvlIn    I here.
//           |    /
//           |  /
// PantOut +-+-+
// PantIn  |   |        +---+          I is AvlIn, and D is PantOut
// AvlIn   +---+      ->|   |     <--  in its predecessor, so insert
//           ^      /   +---+ AvlIn    I here.
//           |    /
//           |  /
// PantOut +-+-+
// PantIn  |   |
// AvlIn   +---+
//           ^
//           |
//           |
// PantOut +-+-+        I is partially redundant, and can be
// AvlOut  | I |   <--  made fully redundant by inserting above,
// AvlLoc  +---+        so delete it.

void pre_local_transfer(PreEnv& penv, bool incDec, Block* blk) {
  FTRACE(3, "Blk(B{}) <-{}\n", blk->id(),
         [&] {
           std::string ret;
           blk->forEachPred([&] (Block* pred) {
               fmt::format_to(std::back_inserter(ret), " B{}", pred->id());
             });
           return ret;
         }());
  auto& s = penv.state[blk];
  auto process = [&] (RCState state) {
    PreAdderInfo adder{penv, s, state, incDec};
    PreAdder preAdder{&adder};
    for (auto iter = blk->begin(); iter != blk->end(); ++iter) {
      auto& inst = *iter;
      if (inst.is(DecRef) || inst.is(DecReleaseCheck)) {
        auto const id = penv.env.asetMap[inst.src(0)];
        if (id < 0) {
          assertx(inst.src(0)->type() <= TUncounted);
          FTRACE(3, "    ** kill uncounted inc/dec: {}\n", inst);
          remove_helper(penv.env.unit, &inst);
          continue;
        }
        if (state.asets[id].lower_bound >= 2) {
          auto const lb = state.asets[id].lower_bound -
                          state.asets[id].pending_reductions;
          if (lb >= 2) {
            FTRACE(2, "    ** decnz:  {}\n", inst);
            if (inst.is(DecReleaseCheck)) {
              Block* inst_block = inst.block();
              inst_block->push_back(penv.env.unit.gen(Jmp, inst.bcctx(), inst.taken()));
            }
            inst.setOpcode(DecRefNZ);
          }
        }
        if (state.asets[id].upper_bound == 1 && inst.is(DecReleaseCheck)) {
          FTRACE(2, "    convert inst to Nop:  {}\n", inst);
          assertx(inst.src(0)->type().subtypeOfAny(TCounted));
          Block* inst_block = inst.block();
          inst_block->push_back(penv.env.unit.gen(Jmp, inst.bcctx(), inst.next()));
          remove_helper(penv.env.unit, &inst);
        }
      }
      if (inst.is(CheckArrayCOW)) {
        auto const id = penv.env.asetMap[inst.src(0)];
        if (id >= 0 && state.asets[id].upper_bound == 1) {
          FTRACE(2, "    convert inst to AssertType:  {}\n", inst);
          assertx(inst.src(0)->type().subtypeOfAny(TCounted));
          auto inst_block = inst.block();
          auto next = inst.next();
          inst.setOpcode(AssertType);
          inst.setTypeParam(inst.dst()->type());
          inst_block->push_back(penv.env.unit.gen(Jmp, inst.bcctx(), next));
        }
      }
      rc_analyze_step(penv.env, inst, state, [&] (Block*) {}, preAdder);
      if (adder.modified) return true;
    }
    return false;
  };

  while (process(penv.rca.info[blk].state_in)) {
    s.avlLoc.reset();
    s.antLoc.reset();
    s.altLoc.reset();
  }

  if (blk->next()) {
    FTRACE(4, "    -> B{}\n", blk->next()->id());
  }
}

void pre_compute_available(PreEnv& penv) {
  while (!penv.avlQ.empty()) {
    auto& blk = penv.env.rpoBlocks[penv.avlQ.pop()];
    auto& s = penv.state[blk];
    auto first = true;
    blk->forEachPred(
      [&] (Block* pred) {
        auto &ps = penv.state[pred];
        if (first) {
          s.avlIn.assign(ps.avlOut);
          s.pavlIn.assign(ps.pavlOut);
        } else {
          s.avlIn &= ps.avlOut;
          s.pavlIn |= ps.pavlOut;
        }
        first = false;
      }
    );
    if (s.phiPropagate.any()) {
      bitset_for_each_set(
        s.phiPropagate,
        [&] (int i) {
          auto avl = true;
          auto pavl = false;
          blk->forEachPred(
            [&] (Block* pred) {
              auto const sid = lookup_aset(penv.env, pred->back().src(i));
              if (!sid) return;
              auto& ps = penv.state[pred];
              if (!ps.avlOut.test(*sid)) avl = false;
              if (ps.pavlOut.test(*sid)) pavl = true;
            }
          );
          auto const id = lookup_aset(penv.env, blk->front().dst(i));
          assertx(id);
          if (avl) s.avlIn.set(*id);
          if (pavl) s.pavlIn.set(*id);
        }
      );
    }
    auto changed = false;

    penv.scratchBits.assign_sub(s.avlIn, s.altLoc);
    penv.scratchBits |= s.avlLoc;
    if (s.avlOut != penv.scratchBits) {
      s.avlOut.assign(penv.scratchBits);
      changed = true;
    }

    penv.scratchBits.assign_sub(s.pavlIn, s.altLoc);
    penv.scratchBits |= s.avlLoc;
    if (s.pavlOut != penv.scratchBits) {
      s.pavlOut.assign(penv.scratchBits);
      changed = true;
    }
    if (changed) {
      auto propagate = [&] (Block* succ) {
        if (!succ) return;
        penv.avlQ.push(penv.state[succ].rpoId);
      };
      propagate(blk->next());
      propagate(blk->taken());
    }
  }
}

void pre_compute_anticipated(PreEnv& penv) {
  while (!penv.antQ.empty()) {
    auto& blk = penv.env.rpoBlocks[penv.antQ.pop()];
    auto& s = penv.state[blk];
    auto first = true;
    blk->forEachSucc(
      [&] (Block* succ) {
        auto &ss = penv.state[succ];
        if (!first) {
          s.antOut &= ss.antIn;
          s.pantOut |= ss.pantIn;
          assertx(!ss.phiPropagate.any());
          return;
        }
        first = false;
        s.antOut.assign(ss.antIn);
        s.pantOut.assign(ss.pantIn);
        if (!ss.phiPropagate.any()) return;
        bitset_for_each_set(
          ss.phiPropagate,
          [&] (int i) {
            auto const did = lookup_aset(penv.env, succ->front().dst(i));
            assertx(did);
            if (!ss.antIn.test(*did)) return;
            auto const sid = lookup_aset(penv.env, blk->back().src(i));
            if (!sid) return;
            s.antOut.set(*sid);
            s.pantOut.set(*sid);
          }
        );
      }
    );
    penv.scratchBits.assign_sub(s.antOut, s.altLoc);
    penv.scratchBits |= s.antLoc;
    auto changed = false;
    if (s.antIn != penv.scratchBits) {
      s.antIn.assign(penv.scratchBits);
      changed = true;
    }
    penv.scratchBits.assign_sub(s.pantOut, s.altLoc);
    penv.scratchBits |= s.antLoc;
    if (s.pantIn != penv.scratchBits) {
      s.pantIn.assign(penv.scratchBits);
      changed = true;
    }
    if (changed) {
      blk->forEachPred(
        [&] (Block* pred) {
          penv.antQ.push(penv.state[pred].rpoId);
        }
      );
    }
  }
}

/*
 * Find places where we can potentially kill IncRef/DecRef pairs
 * across a phi node.
 *
 * Such cases are when the dst of a DefLabel is anticipated, and the
 * src of at least one of the corresponding Jmps is partially available.
 *
 * We also avoid cases where the dst is partially available in to
 * avoid overlap between the srcs and the dsts.
 */
bool pre_adjust_for_phis(PreEnv& penv) {
  auto ret = false;
  for (auto& blk : penv.env.rpoBlocks) {
    auto const& front = blk->front();
    if (!front.is(DefLabel)) continue;

    auto& s = penv.state[blk];
    auto newProp = s.phiPropagate;
    for (auto i = 0; i < front.numDsts(); i++) {
      if (i == s.phiPropagate.size()) break;

      auto const newVal = [&]{
        auto const did = lookup_aset(penv.env, front.dst(i));
        if (!did || !s.antIn.test(*did)) return false;
        bool result = false;
        blk->forEachPred(
          [&] (Block* pred) {
            auto const sid = lookup_aset(penv.env, pred->back().src(i));
            if (!sid) return;
            auto& ps = penv.state[pred];
            if (ps.pavlOut.test(*sid)) result = true;
          }
        );
        return result;
      }();

      if (s.phiPropagate.test(i) == newVal) continue;

      FTRACE(3, "{}setting phiPropagate[{}] in blk({})\n",
             newVal ? "" : "un", i, blk->id());

      newProp.flip(i);

      blk->forEachPred(
        [&] (Block* pred) {
          auto& ps = penv.state[pred];
          penv.antQ.push(ps.rpoId);
        }
      );
    }

    if (s.phiPropagate != newProp) {
      s.phiPropagate = newProp;
      penv.avlQ.push(s.rpoId);
      ret = true;
    }
  }

  return ret;
}

void pre_insert(PreEnv& penv, bool incDec) {
  for (auto& elm : penv.insMap) {
    auto const tmp = elm.second;
    auto const blk = std::get<0>(elm.first);
    auto const id DEBUG_ONLY = std::get<1>(elm.first);
    auto const atFront = std::get<2>(elm.first);
    auto const op = atFront == incDec ? IncRef : DecRef;
    FTRACE(3, "insert: {}({}) at {} in blk({})\n",
           op == IncRef ? "IncRef" : "DecRef",
           id,
           atFront ? "front" : "back",
           blk->id());
    auto const bcctx = (atFront ? blk->begin() : blk->backIter())->bcctx();
    auto const inst = op == IncRef ?
      penv.env.unit.gen(op, bcctx, tmp) :
      penv.env.unit.gen(op, bcctx, DecRefData(-1), tmp);
    if (atFront) {
      blk->prepend(inst);
    } else if (blk->taken()) {
      auto const back = blk->backIter();
      // We're trying to insert at the end of the block, but there's a
      // taken edge, so we have to insert before the last
      // instruction. This could either be an unconditional jmp (which
      // will be irrelevant), or we could be in the blk->numPreds()==1
      // case in pre_find_insertions_helper, where the insertion needs
      // to happen at the start of the block, but can be delayed to
      // the end because it wasn't altered. In either case its safe
      // (and correct) to insert before the last instruction.
      assertx(irrelevant_inst(*back) || !penv.state[blk].altLoc[id]);
      blk->insert(back, inst);
    } else {
      blk->push_back(inst);
    }
    penv.reprocess.insert(penv.state[blk].rpoId);
  }
}

bool pre_insertions_for_delete_recur(PreEnv& penv, Block* blk,
                                     bool atFront, SSATmp* tmp);

bool pre_find_insertions_helper(PreEnv& penv,
                                Block* blk, bool atFront, SSATmp* tmp) {
  auto& s = penv.state[blk];
  if (s.genId == penv.curGen) return true;
  s.genId = penv.curGen;
  auto id = penv.env.asetMap[tmp];
  assertx(id >= 0);
  if (atFront) {
    // If its not available in, there's nothing we can do.
    if (!s.avlIn.test(id)) return false;
    // If its anticipated in, its dead on this path, and we're done.
    if (s.antIn.test(id)) return true;
  } else {
    // If its not anticipated out, there's nothing we can do.
    if (!s.antOut.test(id)) return false;
    // If its available out, its dead on this path, and we're done.
    if (s.avlOut.test(id)) return true;
  }

  auto const insertHere = [&] {
    // If its not partially {anticipated in|available out}, we need to
    // insert it.
    if (!(atFront ? s.pantIn : s.pavlOut).test(id)) {
      // id was pantOut/pavlIn in our predecessor/successor, so it must
      // have had multiple successors/predecessors, and we've split
      // critical edges, so...
      assertx((atFront ? blk->numPreds() : blk->numSuccs()) == 1);
      return true;
    }
    // If it was altered locally, then it would either be
    // antLoc/avlLoc (and hence antIn/avlOut) or it would be
    // !pantIn/pavlOut, both of which were checked above.
    assertx(!s.altLoc.test(id));
    if (atFront) {
      if (blk->numSuccs() == 1) {
        auto const succ = blk->next() ? blk->next() : blk->taken();
        // When succ.avlIn is false, its ok to insert at the end of
        // this block, but its not ok to insert at the start of the
        // next one. Logically, we should insert at the end of this
        // block; but we know its not altLoc, so its ok to insert at
        // the start.
        return !penv.state[succ].avlIn.test(id);
      }
    } else {
      if (blk->numPreds() == 1) {
        auto const pred = blk->preds().back().from();
        // When pred.antOut is false, ts ok to insert at the start of
        // this block, but its not ok to insert at the end of the
        // previous one. Logically, we should insert at the front of
        // this block; but we know its not altLoc, so its ok to insert
        // at the end.
        return !penv.state[pred].antOut.test(id);
      }
    }
    return false;
  }();

  if (insertHere) {
    while (true) {
      auto const defBlock = findDefiningBlock(tmp, penv.env.idoms);
      if (dominates(defBlock, blk, penv.env.idoms)) {
        break;
      }
      // If tmp doesn't dominate this block, we can try a passthrough;
      // otherwise we're done.
      if (!tmp->inst()->isPassthrough()) return false;
      tmp = tmp->inst()->getPassthroughValue();
      assertx(penv.env.asetMap[tmp] == id);
    }
    penv.to_insert.push_back({blk, tmp});
    return true;
  }

  if (atFront) {
    // Its pantIn but not antIn, and not altered, so
    assertx(s.pantOut.test(id) && !s.antOut.test(id));
  } else {
    // Its pavlOut but not avlOut, and not altered, so
    assertx(s.pavlIn.test(id) && !s.avlIn.test(id));
  }
  return pre_insertions_for_delete_recur(penv, blk, atFront, tmp);
}

// Recursive walker for pre_insertions_for_delete.  Essentially, this
// either walks forwards or backwards (depending on atFront), calling
// pre_find_insertions_helper on each block, which will decide either
//
//  - we can't insert in this block (because eg the SSATmp isn't
//    available there, or because we've found a case where we would
//    have to insert both Incs and Decs), in which case we bail out of
//    the walk.
//  - there's no need to insert in this block (because its already
//    Anticipated/Available, depending on atFront)
//  - we need to insert here, in which case it adds it to penv.to_insert.
//
// phi nodes complicate this a little, because as we pass through
// them, we might have to change which SSATmp to follow.
bool pre_insertions_for_delete_recur(PreEnv& penv, Block* blk,
                                     bool atFront, SSATmp* tmp) {
  auto ret = true;
  auto helper = [&] (Block* other) {
    if (ret && !pre_find_insertions_helper(penv, other, atFront, tmp)) {
      ret = false;
    }
  };

  // Check to see if tmp is a src/dst of a phi that we want to follow
  auto find_phi_index = [&] (Block* b) -> int {
    auto const& bitset = penv.state[b].phiPropagate;
    if (!bitset.any()) return -1;
    for (auto i = bitset_find_first(bitset);
         i < bitset.size();
         i = bitset_find_next(bitset, i)) {
      auto const t = atFront ? blk->back().src(i) : blk->front().dst(i);
      if (t == tmp) return i;
    }
    return -1;
  };

  if (atFront) {
    if (auto const succ = blk->taken()) {
      auto i = find_phi_index(succ);
      if (i >= 0) {
        // The SSATmp we're following was the i'th src of a phi;
        // follow the dst on the other side.
        auto const dst = succ->front().dst(i);
        assertx(lookup_aset(penv.env, dst));
        return pre_find_insertions_helper(penv, succ, atFront, dst);
      }
    }
    blk->forEachSucc(helper);
    return ret;
  }

  auto i = find_phi_index(blk);
  if (i < 0) {
    blk->forEachPred(helper);
    return ret;
  }
  // The SSATmp we're following was the i'th dst of a phi; follow the
  // srcs on the other side.
  blk->forEachPred(
    [&] (Block* pred) {
      if (!ret) return;
      auto const src = pred->back().src(i);
      if (lookup_aset(penv.env, src) &&
          !pre_find_insertions_helper(penv, pred, atFront, src)) {
        ret = false;
      }
    }
  );
  return ret;
}

// Try to find places to insert copies of incOrDec that would make
// it redundant. See the ascii art in the comment block headed
// "Partial redundancy elimination of IncRef/DecRef pairs" above.
bool pre_insertions_for_delete(PreEnv& penv,
                               IRInstruction* incOrDec, bool insertAtFront) {
  auto const tmp = incOrDec->src(0);
  auto const id = penv.env.asetMap[tmp];
  auto const blk = incOrDec->block();
  auto& s = penv.state[blk];

  // We're going to do a recursive walk looking for the boundaries of
  // the region we can move incOrDec to; that region could contain
  // loops, so we need to prevent infinite recursion.
  s.genId = ++penv.curGen;

  penv.to_insert.clear();
  // If incOrDec was fully redundant, there's nothing to do.  Note
  // that when we're moving incOrDec forward, insertAtFront will be
  // true, and when we're moving it backward it will be false. So when
  // we're moving forward, we care about AntOut, and backward we care
  // about AvlIn.
  if ((insertAtFront ? s.antOut : s.avlIn).test(id)) return true;

  if (!pre_insertions_for_delete_recur(penv, blk, insertAtFront, tmp)) {
    return false;
  }

  for (auto const& elm : penv.to_insert) {
    auto const key = std::make_tuple(elm.first,
                                     penv.env.asetMap[elm.second],
                                     insertAtFront);
    penv.insMap[key] = elm.second;
  }

  return true;
}

// pre_apply does two separate iterations to convergence.
//
// First it iterates, removing partially redundant incs or decs that
// can't be made redundant from the set, and then repropagating the
// block state. When its done, all the partially redundant incs and
// decs can be removed, and the incs and decs in penv.insMap need to
// be inserted to compensate.
//
// Then, if it did anything, it will recompute the transfer function
// for each changed block, repropagate the block state, and run again,
// since any incs or decs it removed could have prevented other
// optimizations.
bool pre_apply(PreEnv& penv, bool incDec) {
  penv.insMap.clear();
  for (auto& blk : penv.env.rpoBlocks) {
    auto& s = penv.state[blk];

    auto remove = [&] (IncDecBits& del, IRInstruction& inst,
                       bool removeDec, bool insertAtFront) {
      if (removeDec ? inst.is(DecRef, DecRefNZ) : inst.is(IncRef)) {
        auto const id = penv.env.asetMap[inst.src(0)];
        if (id >= 0 && del.test(id)) {
          if (!pre_insertions_for_delete(penv, &inst, insertAtFront)) {
            FTRACE(3, "No insertion for {} ({}) in B{}\n",
                   id, inst.toString(), blk->id());
            if (insertAtFront) {
              s.avlLoc.reset(id);
              penv.avlQ.push(s.rpoId);
            } else {
              s.antLoc.reset(id);
              penv.antQ.push(s.rpoId);
            }
            s.altLoc.set(id);
          }
          del.reset(id);
          return del.none();
        }
      }
      return false;
    };

    auto& delFront = penv.scratchBits;
    delFront.assign_and(s.antLoc, s.pavlIn);
    if (delFront.any()) {
      for (auto& inst : *blk) {
        if (remove(delFront, inst, incDec, false)) break;
      }
      always_assert(delFront.none());
    }

    auto& delBack = penv.scratchBits;
    delBack.assign_and(s.avlLoc, s.pantOut);
    if (delBack.any()) {
      for (auto it = blk->end(); it != blk->begin(); ) {
        if (remove(delBack, *--it, !incDec, true)) break;
      }
      always_assert(delBack.none());
    }
  }

  // If there were any partially redundant ops that couldn't be made
  // redundant, there will be blocks in at least one of the queues
  // that need reprocessing, so return here, and do that.
  if (!penv.avlQ.empty() || !penv.antQ.empty()) {
    FTRACE(4, "Recompute bit vectors after removing pre candidates\n");
    return false;
  }

  // We now know that every partially redundant inc/dec can be
  // removed, and have a list of incs and decs to insert in
  // penv.insMap. So go ahead and delete/insert everything as
  // necessary.
  for (auto& blk : penv.env.rpoBlocks) {
    auto& s = penv.state[blk];
    s.phiPropagate.reset();

    FTRACE(4,
           "PREApply Blk(B{}) <-{}\n"
           "    antIn   : {}\n"
           "    pantIn  : {}\n"
           "    avlIn   : {}\n"
           "    pavlIn  : {}\n"
           "    antLoc  : {}\n"
           "    altered : {}\n"
           "    avlLoc  : {}\n"
           "    antOut  : {}\n"
           "    pantOut : {}\n"
           "    avlOut  : {}\n"
           "    pavlOut : {}\n"
           "  ->{}\n",
           blk->id(),
           [&] {
             std::string ret;
             blk->forEachPred([&] (Block* pred) {
                 fmt::format_to(std::back_inserter(ret), " B{}", pred->id());
               });
             return ret;
           }(),
           show(s.antIn),
           show(s.pantIn),
           show(s.avlIn),
           show(s.pavlIn),
           show(s.antLoc),
           show(s.altLoc),
           show(s.avlLoc),
           show(s.antOut),
           show(s.pantOut),
           show(s.avlOut),
           show(s.pavlOut),
           [&] {
             std::string ret;
             blk->forEachSucc([&] (Block* succ) {
                 fmt::format_to(std::back_inserter(ret), " B{}", succ->id());
               });
             return ret;
           }());

    auto remove = [&] (IncDecBits& del, IRInstruction& inst,
                       bool removeDec, bool insertAtFront) {
      if (removeDec ? inst.is(DecRef, DecRefNZ) : inst.is(IncRef)) {
        auto const id = penv.env.asetMap[inst.src(0)];
        if (id >= 0 && del.test(id)) {
          FTRACE(3, "delete: {} = {}\n", id, inst.toString());
          remove_helper(penv.env.unit, &inst);
          del.reset(id);
          return del.none();
        }
      }
      return false;
    };

    auto& delFront = penv.scratchBits;
    delFront.assign_and(s.antLoc, s.pavlIn);
    if (delFront.any()) {
      for (auto& inst : *blk) {
        if (remove(delFront, inst, incDec, false)) break;
      }
      always_assert(delFront.none());
      penv.reprocess.insert(penv.state[blk].rpoId);
    }

    auto& delBack = penv.scratchBits;
    delBack.assign_and(s.avlLoc, s.pantOut);
    if (delBack.any()) {
      for (auto it = blk->end(); it != blk->begin(); ) {
        if (remove(delBack, *--it, !incDec, true)) break;
      }
      always_assert(delBack.none());
      penv.reprocess.insert(penv.state[blk].rpoId);
    }
  }

  pre_insert(penv, incDec);
  // If we didn't do anything, we're done...
  if (!penv.reprocess.size()) {
    FTRACE(4, "No changes... done\n");
    return true;
  }
  // ... otherwise update the RCAnalysis for the modified blocks
  assertx(penv.avlQ.empty());
  for (auto rpoId : penv.reprocess) {
    penv.avlQ.push(rpoId);
  }
  FTRACE(3, "re-analyze after modifications\n");
  rc_analyze_worklist(penv.env, penv.rca, penv.avlQ, &penv.reprocess);
  // ... then update the local transfer functions for any blocks where
  // it might have changed, and setup the work queues to propagate the
  // changes
  assertx(penv.antQ.empty());
  assertx(penv.avlQ.empty());
  FTRACE(3, "re-compute local transfer after modifications\n");
  auto reprocess = std::move(penv.reprocess);
  for (auto rpoId : reprocess) {
    auto const blk = penv.env.rpoBlocks[rpoId];
    auto& s = penv.state[blk];
    s.avlLoc.reset();
    s.antLoc.reset();
    s.altLoc.reset();
    pre_local_transfer(penv, incDec, blk);
    penv.avlQ.push(rpoId);
    penv.antQ.push(rpoId);
  }
  FTRACE(4, "Redo pre-apply\n");
  return false;
}

/*
 * Partial redundancy elimination for IncRef/DecRef pairs.
 *
 * When incDec is true, we're going to remove incs followed by decs;
 * otherwise decs followed by incs.
 */
void pre_incdecs(Env& env, RCAnalysis& rca, bool incDec) {
  FTRACE(2, "pre_incdecs ({})---------------------------------\n",
         incDec ? "Inc->Dec" : "Dec->Inc");

  PreEnv penv{env, rca};
  for (auto blk : env.rpoBlocks) {
    pre_local_transfer(penv, incDec, blk);
  }

  do {
    FTRACE(4, "pre_compute_available\n");
    pre_compute_available(penv);
    FTRACE(4, "pre_compute_anticipated\n");
    pre_compute_anticipated(penv);
    FTRACE(4, "pre_adjust_for_phis\n");
  } while (pre_adjust_for_phis(penv) ||
           !pre_apply(penv, incDec));
}

//////////////////////////////////////////////////////////////////////

/*
 * This pass delays DecRef* instructions to try to expose more opportunities
 * for eliminating IncRef/DecRef pairs. The idea is to remember the last IncRef
 * tX that was seen and move DecRefs of other SSATmps later, until after a
 * DecRef tX is found.  If any other instruction that is not "irrelevant" is
 * found along the way, the set of DecRefs that are candidates to be moved has
 * to be cleared.  For now, this pass is only applied locally, within a basic
 * block.
 */
void delay_decrefs(IRUnit& unit) {
  FTRACE(2, "delay_decrefs: unit before: {}\n", show(unit));

  auto blocks = rpoSortCfg(unit);
  for (auto block : blocks) {
    IRInstruction* last_incref = nullptr;
    jit::vector<IRInstruction*> other_decrefs;

    for (auto& inst : *block) {
      if (inst.is(IncRef)) {
        last_incref = &inst;
        other_decrefs.clear();
      }

      if (last_incref == nullptr || irrelevant_inst(inst)) continue;

      if (inst.is(DecRef, DecRefNZ)) {
        if (inst.src(0) != last_incref->src(0)) {
          other_decrefs.push_back(&inst);
          continue;
        }
        // Move all the DecRefs in other_decrefs after inst
        auto iter = block->iteratorTo(&inst);
        iter++;
        for (auto other : other_decrefs) {
          auto new_decref = unit.gen(DecRef, other->bcctx(),
                                     *(other->extra<DecRefData>()),
                                     other->src(0));
          FTRACE(2, "delay_decrefs: moving {} after {} as {}\n",
                 *other, inst, *new_decref);
          remove_helper(unit, other);
          block->insert(iter, new_decref);
        }
      }
      // Either we moved other_decrefs or we hit some other relevant
      // instruction.  In either case, we need to forget the other decrefs.
      other_decrefs.clear();
    }
  }
}

//////////////////////////////////////////////////////////////////////
}

//////////////////////////////////////////////////////////////////////

void optimizeRefcounts(IRUnit& unit) {
  Timer timer(Timer::optimize_refcountOpts, unit.logEntry().get_pointer());
  splitCriticalEdges(unit);
  delay_decrefs(unit);

  PassTracer tracer{&unit, Trace::hhir_refcount, "optimizeRefcounts"};
  Env env { unit };

  find_alias_sets(env);
  if (env.asets.size() == 0) return;

  populate_mrinfo(env);
  weaken_decrefs(env);
  {
    auto rca = rc_analyze(env);
    pre_incdecs(env, rca, true);
    pre_incdecs(env, rca, false);
  }
  auto const sunk = sink_incs(env);
  weaken_decrefs(env);

  if (sunk) {
    // We sunk IncRefs past CheckTypes, which could allow us to
    // specialize them.
    insertNegativeAssertTypes(unit, env.rpoBlocks);

    // Remove unreachable blocks before refineTmps
    // asserts certain variants related to block reachability
    bool needsReflow = removeUnreachable(unit);
    if (needsReflow) reflowTypes(unit);

    refineTmps(unit);
  }
}

bool shouldReleaseShallow(const DecRefProfile& data, SSATmp* tmp) {
  if(!tmp->type().isKnownDataType()) {
    return false;
  }
  return isArrayLikeType(tmp->type().toDataType())
    && data.percent(data.arrayOfUncountedReleasedCount())
    > Cfg::HHIR::SpecializedDestructorThreshold;
}

IRInstruction* makeReleaseShallow(
    IRUnit& unit, Block* remainder, IRInstruction* inst, SSATmp* tmp) {
  assertx(inst->is(DecRef));
  auto const block = inst->block();
  auto const bcctx = inst->bcctx();
  auto const next = unit.defBlock(block->profCount());
  next->push_back(unit.gen(ReleaseShallow, bcctx, tmp));
  next->push_back(unit.gen(Jmp, bcctx, remainder));

  auto const decReleaseCheck = unit.gen(DecReleaseCheck, bcctx, *(inst->extra<DecRefData>()), remainder, tmp);
  decReleaseCheck->setNext(next);
  return decReleaseCheck;
}

/*
 * If our profiled type is better than the known type, generate type-specific
 * DecRef codegen. Doing so involves adding a CheckType for the profiled type,
 * then emitting a type-specific DecRef in the next block and a generic one
 * in the taken block.
 */
void optimizeDecRefForProfiledType(
    IRUnit& unit, const DecRefProfile& profile, IRInstruction* inst) {
  assertx(isRealType(profile.datatype));
  assertx(inst->is(DecRef));

  auto const src = inst->src(0);
  auto const bcctx = inst->bcctx();
  auto const block = inst->block();
  auto const it = block->iteratorTo(inst);
  FTRACE(4, "    {}: optimizing for type: {}\n",
      inst->toString(), profile.datatype);

  // "remainder" contains the contents of this block after this DecRef.
  auto const remainder = unit.defBlock(block->profCount(), block->hint());
  remainder->splice(remainder->end(), block, std::next(it), block->end());

  // "taken" contains the generic DecRef if the CheckType fails.
  auto const data = *inst->extra<DecRefData>();
  auto const taken = unit.defBlock(0, Block::Hint::Unlikely);
  taken->push_back(unit.gen(DecRef, bcctx, data, src));
  taken->push_back(unit.gen(Jmp, bcctx, remainder));

  auto const check = unit.gen(
      CheckType, bcctx, Type(profile.datatype), taken, src);
  auto const dst = check->dst();

  // "next" contains the type-specific DecRef if the CheckType succeeds.
  auto const next = unit.defBlock(block->profCount());

  // if this DecRef is eligible for a shallow release replace it with a
  // combination of DecReleaseCheck and ReleaseShallow IR-ops
  if (shouldReleaseShallow(profile, dst)) {
    next->push_back(makeReleaseShallow(unit, remainder, inst, dst));
  } else {
    next->push_back(unit.gen(DecRef, bcctx, data, dst));
    next->push_back(unit.gen(Jmp, bcctx, remainder));
  }

  check->setNext(next);

  // Replace the DecRef with the new CheckType.
  block->insert(it, check);
  block->erase(inst);
}

void optimizeForReleaseShallow(IRUnit& unit, IRInstruction* inst) {
  assertx(inst->is(DecRef));

  auto const src = inst->src(0);
  auto const block = inst->block();
  auto const it = block->iteratorTo(inst);

  auto remainder = unit.defBlock(block->profCount(), block->hint());
  remainder->splice(remainder->end(), block, std::next(it), block->end());

  block->insert(it, makeReleaseShallow(unit, remainder, inst, src));
  block->erase(inst);
}

/*
 * This optimization selectively transforms DecRefs into DecRefNZs, which is
 * valid when destructors are disallowed.  This tranformation avoids checking if
 * the count got to zero and immediately releasing the object. This will use
 * more memory, but it is still OK to do because the tracing GC can later
 * collect the garbage produced.  This transformation can also trigger more
 * copy-on-write for arrays and strings for which there are transitive
 * references inside the object that is not immediately released. To limit these
 * overheads, this optimization is selectively performed based on profiling and
 * it's only applied when the number of times that the DecRef reached 0 is below
 * a certain threshold.  When the type is known to be a string or uncounted, we
 * can be more aggressive because they don't hold internal references to other
 * objects, so a separate threshold is used in that case.
 */
void selectiveWeakenDecRefs(IRUnit& unit) {
  auto blocks = poSortCfg(unit);

  // Union of types that cannot trigger Copy-On-Write for inner elements in case
  // they're not released.  We can be more aggressive when optimizing them here.
  auto const cowFree = TStr | TUncounted;
  auto const TAwaitable = Type::SubObj(c_Awaitable::classof());

  // Keep track of DecRef instructions that are eligible to be optimized using
  // profiled type information. We do the optimization outside the loop because
  // it introduces new control flow.
  jit::vector<std::pair<IRInstruction*, DecRefProfile>> profiledTypeCandidates;
  jit::vector<IRInstruction*> releaseShallowCandidates;

  for (auto& block : blocks) {
    for (auto& inst : *block) {
      if (inst.is(DecRef, DecRefNZ)) {
        const auto& type = inst.src(0)->type();
        const auto profile = decRefProfile(unit.context(), &inst);
        if (profile.optimizing()) {
          const auto data = profile.data();
          const auto decrefdPct = data.percent(data.destroyed()) +
                                  data.percent(data.survived());
          double decrefdPctLimit = inst.src(0)->type() <= cowFree
            ? Cfg::Jit::PGODecRefNopDecPercent
            : Cfg::Jit::PGODecRefNopDecPercentCOW;
          if (decrefdPct < decrefdPctLimit && !(type <= TCounted)) {
            inst.convertToNop();
          } else if (inst.is(DecRef)) {
            const auto destroyPct = data.percent(data.destroyed());
            double destroyPctLimit = inst.src(0)->type() <= cowFree
              ? Cfg::Jit::PGODecRefNZReleasePercent
              : Cfg::Jit::PGODecRefNZReleasePercentCOW;
            if (destroyPct < destroyPctLimit && !(type <= TAwaitable)) {
              inst.setOpcode(DecRefNZ);
            } else if (isRealType(data.datatype)) {
              if (!inst.src(0)->type().isKnownDataType()) {
                profiledTypeCandidates.emplace_back(&inst, data);
              } else if (shouldReleaseShallow(data, inst.src(0))){
                releaseShallowCandidates.emplace_back(&inst);
              }
            }
          }
        }
      }
    }
  }

  for (auto const& pair : profiledTypeCandidates) {
    optimizeDecRefForProfiledType(unit, pair.second, pair.first);
  }

  for (auto inst : releaseShallowCandidates) {
   optimizeForReleaseShallow(unit, inst);
  }
}

}
}
