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
#include "hphp/hhbbc/interp.h"

#include <algorithm>
#include <string>
#include <utility>


#include "hphp/util/trace.h"

#include "hphp/hhbbc/interp-internal.h"
#include "hphp/hhbbc/optimize.h"
#include "hphp/hhbbc/type-ops.h"

namespace HPHP::HHBBC {

namespace {

//////////////////////////////////////////////////////////////////////

// Represents the "effectful"-ness of a particular member instruction
// operation. This includes operations which are visible outside of
// the function (writing to a static property), but also potential
// throwing. Many different aspects of a member instruction operation
// can have effects, so we combine the effects from the various
// portions and use the final result to determine how to mark the
// bytecode.
enum class Effects {
  None,         // Effect-free
  SideEffect,   // Cannot throw, but has some side-effect
  Throws,       // Might throw an exception
  AlwaysThrows  // Always throws an exception
};

// Combine two effects, in the sense that either of them might happen
// (therefore AlwaysThrows becomes just Throws if mixed with something
// else).
Effects unionEffects(Effects e1, Effects e2) {
  if (e1 == e2) return e1;
  if (e1 == Effects::Throws || e2 == Effects::Throws ||
      e1 == Effects::AlwaysThrows || e2 == Effects::AlwaysThrows) {
    return Effects::Throws;
  }
  return Effects::SideEffect;
}

// There's no good default for Effects when you want to union them
// together. Instead you want to start with the first Effect you
// see. This keeps the Effect from taking a value until you union one
// into it.
using OptEffects = Optional<Effects>;

OptEffects unionEffects(OptEffects e1, Effects e2) {
  if (!e1) return e2;
  return unionEffects(*e1, e2);
}

//////////////////////////////////////////////////////////////////////

/*
 * A note about bases.
 *
 * Generally type inference needs to know two kinds of things about the base to
 * handle effects on tracked locations:
 *
 *   - Could the base be a location we're tracking deeper structure on, so the
 *     next operation actually affects something inside of it.  For example,
 *     could the base be an object with the same type as $this, or an array in a
 *     local variable.
 *
 *   - Could the base be something (regardless of type) that is inside one of
 *     the things we're tracking.  I.e., the base might be whatever (an array or
 *     a bool or something), but living inside a property inside an object with
 *     the same type as $this, or living inside of an array in the local frame.
 *
 * The first cases apply because final operations are going to directly affect
 * the type of these elements.  The second case is because member operations may
 * change the base at each step if it is a defining instruction.
 *
 * Note that both of these cases can apply to the same base in some cases: you
 * might have an object property on $this that could be an object of the type of
 * $this.
 */

//////////////////////////////////////////////////////////////////////

bool couldBeThisObj(ISS& env, const Base& b) {
  if (b.loc == BaseLoc::This) return true;
  if (auto const s = selfCls(env)) {
    return b.type.couldBe(setctx(toobj(*s)));
  }
  return true;
}

bool mustBeThisObj(ISS& env, const Base& b) {
  if (b.loc == BaseLoc::This) return true;
  if (auto const s = selfCls(env)) {
    return b.type.subtypeOf(setctx(toobj(*s)));
  }
  return false;
}

bool mustBeInLocal(const Base& b) {
  return b.loc == BaseLoc::Local;
}

bool mustBeInStack(const Base& b) {
  return b.loc == BaseLoc::Stack;
}

bool mustBeInStatic(const Base& b) {
  return b.loc == BaseLoc::StaticProp;
}

//////////////////////////////////////////////////////////////////////

// Base locations that only occur at the start of a minstr sequence.
bool isInitialBaseLoc(BaseLoc loc) {
  return
    loc == BaseLoc::Local ||
    loc == BaseLoc::Stack ||
    loc == BaseLoc::StaticProp ||
    loc == BaseLoc::This;
}

// Base locations that only occur after the start of a minstr sequence.
bool isDimBaseLoc(BaseLoc loc) {
  return loc == BaseLoc::Elem || loc == BaseLoc::Prop;
}

//////////////////////////////////////////////////////////////////////

/*
 * Update the current base via array_like_set, and return whether the
 * operation can possibly throw.
 */
TriBool array_do_set(ISS& env,
                     const Type& key,
                     const Type& value) {
  auto& base = env.collect.mInstrState.base.type;
  assertx(base.couldBe(BArrLike));

  // array_like_set requires the key to be a TArrKey already. If it's
  // not guaranteed to be, we assume we could throw.
  if (!key.couldBe(BArrKey)) return TriBool::Yes;
  auto const validKey = key.subtypeOf(BArrKey);

  auto set = array_like_set(
    base,
    validKey ? key : intersection_of(key, TArrKey),
    value
  );
  // Check for the presence of TArrLike rather than Bottom, since the
  // base may have contained more than just TArrLike to begin with.
  if (!set.first.couldBe(BArrLike)) {
    assertx(set.second);
    return TriBool::Yes;
  }

  base = std::move(set.first);
  return maybeOrNo(set.second || !validKey);
}

/*
 * Look up the specified key via array_like_elem and return the
 * associated value, along with whether the lookup can throw and if
 * the value is definitely present.
 *
 * If excludeKeyset is true, we remove any TKeyset types from the
 * array before doing the lookup. This is useful for the cases where a
 * keyset would fatal, but other types wouldn't.
 */
struct ElemResult {
  Type elem;
  TriBool throws;
  bool present;
};
ElemResult array_do_elem(ISS& env, const Type& key,
                         bool excludeKeyset = false) {
  auto const& base = env.collect.mInstrState.base.type;
  assertx(base.couldBe(BArrLike));

  // If the key can't possibly be good, or the array is entirely a
  // keyset (and we exclude keysets), we'll always throw.
  if (!key.couldBe(BArrKey)) return ElemResult { TBottom, TriBool::Yes, false };
  if (excludeKeyset && base.subtypeAmong(BKeyset, BArrLike)) {
    return ElemResult { TBottom, TriBool::Yes, false };
  }

  // Otherwise remove the problematic parts of the key or value. If we
  // have to remove anything, assume we could possibly throw.
  auto const validKey = key.subtypeOf(BArrKey);
  auto const validArr = !excludeKeyset || base.subtypeAmong(BKVish, BArrLike);
  auto r = array_like_elem(
    validArr ? base : intersection_of(base, TKVish),
    validKey ? key : intersection_of(key, TArrKey)
  );

  return ElemResult {
    std::move(r.first),
    maybeOrNo(!validKey || !validArr),
    r.second
  };
}

/*
 * Update the current base via array_like_newelem, and return whether
 * the newelem can throw.
 */
TriBool array_do_newelem(ISS& env, const Type& value) {
  auto& base = env.collect.mInstrState.base.type;
  assertx(base.couldBe(BArrLike));

  auto update = array_like_newelem(base, value);
  // Check for the presence of TArrLike rather than Bottom, since the
  // base may have contained more than just TArrLike to begin with.
  if (!update.first.couldBe(BArrLike)) {
    assertx(update.second);
    return TriBool::Yes;
  }

  base = std::move(update.first);
  return maybeOrNo(update.second);
}

//////////////////////////////////////////////////////////////////////

void setLocalForBase(ISS& env, Type ty, LocalId firstKeyLoc) {
  assertx(mustBeInLocal(env.collect.mInstrState.base));
  if (env.collect.mInstrState.base.locLocal == NoLocalId) {
    return killLocals(env);
  }
  FTRACE(4, "      ${} := {}\n",
    env.collect.mInstrState.base.locName
     ? env.collect.mInstrState.base.locName->data()
     : "$<unnamed>",
    show(ty)
  );
  setLoc(
    env,
    env.collect.mInstrState.base.locLocal,
    std::move(ty),
    firstKeyLoc
  );
}

void setStackForBase(ISS& env, Type ty) {
  assertx(mustBeInStack(env.collect.mInstrState.base));

  auto const locSlot = env.collect.mInstrState.base.locSlot;
  FTRACE(4, "      stk[{:02}] := {}\n", locSlot, show(ty));
  assertx(locSlot < env.state.stack.size());

  if (env.undo) {
    env.undo->onStackWrite(locSlot, std::move(env.state.stack[locSlot].type));
  }
  env.state.stack[locSlot] = StackElem { std::move(ty), NoLocalId };
}

void setStaticForBase(ISS& env, Type ty) {
  auto const& base = env.collect.mInstrState.base;
  assertx(mustBeInStatic(base));

  auto const nameTy = base.locName ? sval(base.locName) : TStr;
  FTRACE(
    4, "      ({})::$({}) |= {}\n",
    show(base.locTy), show(nameTy), show(ty)
  );

  mergeStaticProp(env, base.locTy, nameTy, remove_uninit(std::move(ty)));
}

// Run backwards through an array chain doing array_set operations
// to produce the array type that incorporates the effects of any
// intermediate defining dims.
Type currentChainType(ISS& env, Type val) {
  auto it = env.collect.mInstrState.arrayChain.end();
  while (it != env.collect.mInstrState.arrayChain.begin()) {
    if (val.is(BBottom)) break;
    --it;
    assertx(it->base.subtypeOf(BArrLike));
    assertx(it->key.subtypeOf(BArrKey));
    val = array_like_set(it->base, it->key, val).first;
  }
  return val;
}

Type resolveArrayChain(ISS& env, Type val) {
  static UNUSED const char prefix[] = "              ";
  FTRACE(5, "{}chain {}\n", prefix, show(val));
  do {
    if (val.is(BBottom)) {
      // If val is Bottom, the update isn't actually going to happen,
      // so we don't need to unwind the chain.
      env.collect.mInstrState.arrayChain.clear();
      break;
    }
    auto arr = std::move(env.collect.mInstrState.arrayChain.back().base);
    auto key = std::move(env.collect.mInstrState.arrayChain.back().key);
    env.collect.mInstrState.arrayChain.pop_back();
    FTRACE(5, "{}  | {} := {} in {}\n", prefix,
      show(key), show(val), show(arr));
    assertx(arr.subtypeOf(BArrLike));
    assertx(key.subtypeOf(BArrKey));
    val = array_like_set(std::move(arr), key, val).first;
  } while (!env.collect.mInstrState.arrayChain.empty());
  FTRACE(5, "{}  = {}\n", prefix, show(val));
  return val;
}

// Returns true if the base update can be considered "effect-free"
// (IE, updating a static prop base is a side-effect because its
// visible elsewhere. Updating a local or stack slot is not).
bool updateBaseWithType(ISS& env,
                        const Type& ty,
                        LocalId firstKeyLoc = NoLocalId) {
  FTRACE(6, "    updateBaseWithType: {}\n", show(ty));

  if (ty.subtypeOf(BBottom)) return true;

  auto const& base = env.collect.mInstrState.base;

  if (mustBeInLocal(base)) {
    setLocalForBase(env, ty, firstKeyLoc);
    // If we're speculating, a local update is considered a
    // side-effect.
    return !any(env.collect.opts & CollectionOpts::Speculating);
  }
  if (mustBeInStack(base)) {
    setStackForBase(env, ty);
    return true;
  }
  if (mustBeInStatic(base)) {
    setStaticForBase(env, ty);
    return false;
  }

  return true;
}

// Whether its worthwhile to refine the base's type based on knowing
// that certain types would have fatalled (and therefore the base can
// no longer be those types after the op). This is only worthwhile if
// the base is flow sensitive (IE, a local or stack slot). This is
// just an optimization so its always legal to say no.
bool shouldRefineBase(ISS& env) {
  auto const& base = env.collect.mInstrState.base;
  return mustBeInLocal(base) || mustBeInStack(base);
}

void startBase(ISS& env, Base base) {
  auto& oldState = env.collect.mInstrState;
  assertx(oldState.empty());
  assertx(isInitialBaseLoc(base.loc));
  assertx(!base.type.subtypeOf(TBottom));

  oldState.effectFree = env.flags.effectFree;
  oldState.extraPop = false;
  oldState.base = std::move(base);
  FTRACE(5, "    startBase: {}\n", show(*env.ctx.func, oldState.base));
}

// Return true if the base is updated and that update is considered
// "effect-free" (see updateBaseWithType).
bool endBase(ISS& env, bool update = true, LocalId keyLoc = NoLocalId) {
  auto& state = env.collect.mInstrState;
  assertx(!state.empty());

  FTRACE(5, "    endBase: {}\n", show(*env.ctx.func, state.base));

  auto const firstKeyLoc = state.arrayChain.empty()
    ? keyLoc
    : state.arrayChain.data()->keyLoc;
  auto const& ty = state.arrayChain.empty()
    ? state.base.type
    : resolveArrayChain(env, state.base.type);

  auto const effectFree = update
    ? updateBaseWithType(env, ty, firstKeyLoc)
    : true;
  state.base.loc = BaseLoc::None;
  return effectFree;
}

// Return true if the base is updated and that update is considered
// "effect-free" (see updateBaseWithType).
bool moveBase(ISS& env,
              Base newBase,
              bool update = true,
              LocalId keyLoc = NoLocalId) {
  auto& state = env.collect.mInstrState;
  assertx(!state.empty());
  assertx(isDimBaseLoc(newBase.loc));
  assertx(!state.base.type.subtypeOf(BBottom));

  FTRACE(5, "    moveBase: {} -> {}\n",
         show(*env.ctx.func, state.base),
         show(*env.ctx.func, newBase));

  auto const firstKeyLoc = state.arrayChain.empty()
    ? keyLoc
    : state.arrayChain.data()->keyLoc;
  auto const& ty = state.arrayChain.empty()
    ? state.base.type
    : resolveArrayChain(env, state.base.type);

  auto const effectFree = update
    ? updateBaseWithType(env, ty, firstKeyLoc)
    : true;
  state.base = std::move(newBase);
  return effectFree;
}

// Return true if the base is updated and that update is considered
// "effect-free" (see updateBaseWithType).
bool extendArrChain(ISS& env, Type key, Type arr,
                    Type val, LocalId keyLoc = NoLocalId) {
  auto& state = env.collect.mInstrState;
  assertx(!state.empty());
  // NB: The various array operation functions can accept arbitrary
  // types as long as they contain TArrLike.  However we still do not
  // allow putting anything but TArrLike in the chain, since (in
  // general) its not clear how to deal with the other types when
  // resolving the chain. Currently the only case where we set up
  // array chains is ElemD or ElemU. For ElemD, most of the common
  // other types just fatal, so we can simply remove them (including
  // TInitNull). For ElemU we do the same thing, but less types
  // qualify.
  assertx(arr.subtypeOf(BArrLike));
  assertx(key.subtypeOf(BArrKey));
  assertx(!state.base.type.subtypeOf(BBottom));
  assertx(!val.subtypeOf(BBottom));
  assertx(!key.subtypeOf(BBottom));

  state.arrayChain.emplace_back(
    CollectedInfo::MInstrState::ArrayChainEnt{
      std::move(arr),
      std::move(key),
      keyLoc
    }
  );
  state.base.type = std::move(val);

  auto const firstKeyLoc = state.arrayChain.data()->keyLoc;

  FTRACE(5, "    extendArrChain: {}\n", show(*env.ctx.func, state));
  return updateBaseWithType(
    env,
    currentChainType(env, state.base.type),
    firstKeyLoc
  );
}

//////////////////////////////////////////////////////////////////////

// Returns nullptr if it's an unknown key or not a string.
SString mStringKey(const Type& key) {
  auto const v = tv(key);
  return v && v->m_type == KindOfPersistentString ? v->m_data.pstr : nullptr;
}

template<typename Op>
auto update_mkey(const Op& op) { return false; }

template<typename Op>
auto update_mkey(Op& op) -> decltype(op.mkey, true) {
  switch (op.mkey.mcode) {
    case MEC: case MPC: {
      op.mkey.idx++;
      return true;
    }
    default:
      return false;
  }
}

template<typename Op>
auto update_discard(const Op& op) { return false; }

template<typename Op>
auto update_discard(Op& op) -> decltype(op.arg1, true) {
  op.arg1++;
  return true;
}

/*
 * Return the type of the key and whether any promotions happened, or
 * reduce op and return std::nullopt.  Note that when std::nullopt is
 * returned, there is nothing further to do.
 */
template<typename Op>
Optional<std::pair<Type,Promotion>> key_type_or_fixup(ISS& env, Op op) {
  if (env.collect.mInstrState.extraPop) {
    assertx(can_rewind(env));
    auto const mkey = update_mkey(op);
    if (update_discard(op) || mkey) {
      env.collect.mInstrState.extraPop = false;
      reduce(env, op);
      env.collect.mInstrState.extraPop = true;
      return std::nullopt;
    }
  }
  auto const fixup = [&] (Type ty, bool isProp, bool couldBeUninit)
    -> Optional<std::pair<Type,Promotion>> {

    // Handle any classlike key promotions
    auto promoted = promote_classlike_to_key(std::move(ty));
    // We could also promote and potentially throw if we had an uninit
    // local.
    if (couldBeUninit) promoted.second = Promotion::YesMightThrow;
    // If we might throw, we don't want to const prop the key
    if (promoted.second == Promotion::YesMightThrow) return promoted;

    // Fold a constant element key to a literal/int member key. Property keys are
    // never folded, so dynamic-property-access logging still sees $o->$x.
    if (auto const val = tv(promoted.first); val && !isProp) {
      if (isStringType(val->m_type)) {
        op.mkey.mcode = MET;
        op.mkey.litstr = val->m_data.pstr;
        reduce(env, op);
        return std::nullopt;
      }
      if (val->m_type == KindOfInt64) {
        op.mkey.mcode = MEI;
        op.mkey.int64 = val->m_data.num;
        reduce(env, op);
        return std::nullopt;
      }
    }
    return promoted;
  };
  switch (op.mkey.mcode) {
    case MEC: case MPC:
      return fixup(topC(env, op.mkey.idx), op.mkey.mcode == MPC, false);
    case MEL: case MPL: {
      auto couldBeUninit = true;
      if (!peekLocCouldBeUninit(env, op.mkey.local.id)) {
        couldBeUninit = false;
        auto const minLocEquiv = findMinLocEquiv(env, op.mkey.local.id, false);
        if (minLocEquiv != NoLocalId) {
          op.mkey.local = NamedLocal { kInvalidLocalName, minLocEquiv };
          reduce(env, op);
          return std::nullopt;
        }
      }
      return fixup(
        locAsCell(env, op.mkey.local.id),
        op.mkey.mcode == MPL,
        couldBeUninit
      );
    }
    case MW:
      return std::make_pair(TBottom, Promotion::No);
    case MEI:
      return std::make_pair(ival(op.mkey.int64), Promotion::No);
    case MET: case MPT: case MQT:
      return std::make_pair(sval(op.mkey.litstr), Promotion::No);
  }
  not_reached();
}

template<typename Op>
LocalId key_local(ISS& env, Op op) {
  switch (op.mkey.mcode) {
    case MEC: case MPC:
      return topStkLocal(env, op.mkey.idx);
    case MEL: case MPL:
      return op.mkey.local.id;
    case MW:
    case MEI:
    case MET: case MPT: case MQT:
      return NoLocalId;
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

// Helper function for ending the base when we know this member
// instruction will always throw.
Effects endUnreachableBase(ISS& env, Promotion basePromo,
                           LocalId keyLoc = NoLocalId) {
  // If we promoted the base, we still need to reflect that
  if (basePromo != Promotion::No) endBase(env, true, keyLoc);
  return Effects::AlwaysThrows;
}

// Helper function for ending the base. Takes the current aggregated
// effects up until this point, and whether the base underwent any
// promotions. Returns an updated aggregated effects, taking into
// account any effects from the base write-back. The given lambda will
// be called after the base is updated (this is needed because you
// don't want to modify the stack until after the base write back has
// occurred).
template <typename F>
Effects endBaseWithEffects(ISS& env, Effects effects, bool update,
                           Promotion basePromo,
                           const F& finish,
                           LocalId keyLoc = NoLocalId) {
  if (effects == Effects::AlwaysThrows) {
    auto const e = endUnreachableBase(env, basePromo, keyLoc);
    finish();
    return e;
  }
  // End the base. We request a base update if the caller requested,
  // or if a base promotion happened (if the base promoted, we need to
  // record the new type into it).
  auto const effectFree =
    endBase(env, update || basePromo != Promotion::No, keyLoc);
  finish();
  // If we might throw because of base promotion, it doesn't matter
  // what other effects are.
  if (basePromo == Promotion::YesMightThrow) return Effects::Throws;
  // Special case: if we don't have any other effects, but writing to
  // the base is side-effectful, then we can be nothrow (but not
  // effect_free).
  if (!effectFree && effects == Effects::None) return Effects::SideEffect;
  return effects;
}

//////////////////////////////////////////////////////////////////////

// Helper function for Elem (not ElemD or ElemU) operations. Luckily
// the logic for the Dim portion, and the final operation are
// identical, so we can treat them as the same. Return the elem type
// and what effects the access has.
std::pair<Type, Effects> elemHelper(ISS& env, MOpMode mode, Type key) {
  assertx(mode == MOpMode::None ||
          mode == MOpMode::Warn ||
          mode == MOpMode::InOut);

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  // If we're using MOpMode::InOut, then the base has to be a ArrLike
  // or we'll throw.
  auto inOutFail = false;
  if (mode == MOpMode::InOut) {
    if (!base.couldBe(BArrLike)) return { TBottom, Effects::AlwaysThrows };
    if (!base.subtypeOf(BArrLike)) inOutFail = true;
  }

  auto const warnsWithNull =
    BTrue | BNum | BRes | BFunc | BRFunc | BRClsMeth | BClsMeth | BEnumClassLabel;
  auto const justNull = BNull | BFalse;
  auto const DEBUG_ONLY handled =
    warnsWithNull | justNull | BClsMeth |
    BStr | BCls | BLazyCls | BObj | BArrLike;

  static_assert(handled == BCell);

  OptEffects effects;
  auto ty = TBottom;

  // These emit a warning and push InitNull
  if (base.couldBe(warnsWithNull)) {
    effects = unionEffects(effects, Effects::Throws);
    ty |= TInitNull;
  }
  // These silently push InitNull
  if (base.couldBe(justNull)) {
    effects = unionEffects(
      effects,
      inOutFail ? Effects::Throws : Effects::None
    );
    ty |= TInitNull;
  }
  // Strings will return a static string (a character from itself or
  // an empty string). Class-likes will convert to its equivalent string
  // first.
  if (base.couldBe(BStr | BCls | BLazyCls)) {
    auto const isNoThrow =
      !inOutFail &&
      mode != MOpMode::Warn &&
      key.subtypeOf(BArrKey) &&
      (!base.couldBe(BCls | BLazyCls) ||
       Cfg::Eval::RaiseClassConversionNoticeSampleRate == 0);
    effects = unionEffects(
      effects,
      isNoThrow ? Effects::None : Effects::Throws
    );
    ty |= TSStr;
  }
  // These can throw and push anything.
  if (base.couldBe(BObj)) {
    effects = unionEffects(effects, Effects::Throws);
    ty |= TInitCell;
  }
  // If it's an array, we can determine the exact element.
  if (base.couldBe(BArrLike)) {
    auto elem = array_do_elem(env, key);
    if (elem.throws == TriBool::Yes) {
      // Key is bad
      effects = unionEffects(effects, Effects::AlwaysThrows);
    } else {
      auto mightThrow = elem.throws != TriBool::No;
      // If the mode is MOpMode::None, we'll use TInitNull if the key
      // is missing. Otherwise, we'll throw.
      if (!elem.present) {
        if (mode == MOpMode::None) {
          elem.elem |= TInitNull;
        } else {
          mightThrow = true;
        }
      }

      if (elem.elem.is(BBottom)) {
        // Key definitely doesn't exist. We'll always throw. Note that
        // this can't happen if mode is MOpMode::None because we added
        // TInitNull to the type above.
        effects = unionEffects(effects, Effects::AlwaysThrows);
      } else {
        ty |= std::move(elem.elem);
        effects = unionEffects(
          effects,
          !mightThrow && !inOutFail ? Effects::None : Effects::Throws
        );
      }
    }
  }

  assertx(effects.has_value());
  return { std::move(ty), *effects };
}

// Helper function for SetOpElem/IncDecElem final operations. Reads
// the element in the base given by the key, performs the operation
// using `op', then writes the new value back to the base. Returns the
// aggregates effects of the entire operation.
template <typename F>
Effects setOpElemHelper(ISS& env, int32_t nDiscard, const Type& key,
                        LocalId keyLoc, F op) {

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const throws = BNull | BFalse | BStr;
  auto const null =
    BTrue | BNum | BRes | BFunc | BRFunc |
    BCls | BLazyCls | BClsMeth | BRClsMeth | BEnumClassLabel;
  auto const handled = throws | null | BArrLike | BObj;

  static_assert(handled == BCell);

  OptEffects effects;
  auto pushed = TBottom;
  auto refine = BBottom;
  auto update = false;

  // Always throws
  if (base.couldBe(throws)) {
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= throws;
  }
  // Raises a warning and pushes null
  if (base.couldBe(null)) {
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TInitNull;
  }
  // Objects can throw and push anything
  if (base.couldBe(BObj)) {
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TInitCell;
  }
  if (base.couldBe(BArrLike)) {
    auto const unreachable = [&] {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BArrLike;
    };

    [&] {
      // For the array portion, we can analyze the effects precisely
      auto elem = array_do_elem(env, key, true);
      if (elem.elem.is(BBottom) || elem.throws == TriBool::Yes) {
        // Element doesn't exist or key is bad. Always throws.
        unreachable();
        return;
      }

      // Keysets will throw, so we can assume the base does not
      // contain them afterwards.
      if (base.couldBe(BKeyset)) base = remove_keyset(std::move(base));

      // We'll have already have COWed the array if the setop throws,
      // so we need to manually remove staticness and force a base
      // update.
      base = loosen_array_staticness(std::move(base));
      update = true;

      // Perform the op
      auto [toSet, toPush, opEffects] = op(std::move(elem.elem));
      if (opEffects == Effects::AlwaysThrows) {
        // The op will always throw. We've already COWed the base, so we
        // still need to update the base type.
        unreachable();
        return;
      }
      assertx(!toSet.is(BBottom));
      assertx(!toPush.is(BBottom));

      // Write the element back into the array
      auto const set = array_do_set(env, key, toSet);
      if (set == TriBool::Yes) {
        // The set will always throw. In theory this can happen if we do
        // something like read a string out of a keyset, turn it into
        // something that's not an array key, then try to write it back.
        unreachable();
        return;
      }

      auto const maybeThrows =
        !elem.present ||
        elem.throws == TriBool::Maybe ||
        set == TriBool::Maybe;
      effects = unionEffects(
        effects,
        maybeThrows ? Effects::Throws : opEffects
      );
      pushed |= std::move(toPush);
    }();
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());

  // We have to update the base even if we'll always throw to account
  // for potential COWing of the array (it could be in a static for
  // example).
  if (*effects == Effects::AlwaysThrows && update) {
    assertx(pushed.is(BBottom));
    endBase(env, true, keyLoc);
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  }

  return endBaseWithEffects(
    env, *effects, update, Promotion::No,
    [&] {
      discard(env, nDiscard);
      push(env, std::move(pushed));
    },
    keyLoc
  );
}

// Helper function for SetOpNewElem/IncDecNewElemm final
// operations. These all either throw or push null.
Effects setOpNewElemHelper(ISS& env, int32_t nDiscard) {
  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const alwaysThrow =
    BNull | BFalse | BStr | BArrLike | BObj | BClsMeth;
  auto const null =
    BTrue | BNum | BRes | BRFunc | BFunc | BRClsMeth | BCls | BLazyCls| BEnumClassLabel;
  auto const handled = alwaysThrow | null;

  static_assert(handled == BCell);

  OptEffects effects;
  auto pushed = TBottom;
  auto refine = BBottom;
  auto update = false;

  // These always throw
  if (base.couldBe(alwaysThrow)) {
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= alwaysThrow;
  }
  // These raise a warning and push InitNull
  if (base.couldBe(null)) {
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TInitNull;
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());

  return endBaseWithEffects(
    env, *effects, update, Promotion::No,
    [&] {
      discard(env, nDiscard);
      push(env, std::move(pushed));
    }
  );
}

//////////////////////////////////////////////////////////////////////
// intermediate ops

Effects miProp(ISS& env, MOpMode mode, Type key, ReadonlyOp op) {
  auto const name     = mStringKey(key);
  auto const isDefine = mode == MOpMode::Define;
  auto const isUnset  = mode == MOpMode::Unset;
  // PHP5 doesn't modify an unset local if you unset a property or
  // array elem on it, but hhvm does (it promotes it to init-null).
  auto const update = isDefine || isUnset;
  /*
   * MOpMode::Unset Props doesn't promote "emptyish" things to stdClass, or
   * affect arrays, however it can define a property on an object base.  This
   * means we don't need any couldBeInFoo logic, but if the base could actually
   * be $this, and a declared property could be Uninit, we need to merge
   * InitNull.
   *
   * We're trying to handle this case correctly as far as the type inference
   * here is concerned, but the runtime doesn't actually behave this way right
   * now for declared properties.  Note that it never hurts to merge more types
   * than a thisProp could actually be, so this is fine.
   *
   * See TODO(#3602740): unset with intermediate dims on previously declared
   * properties doesn't define them to null.
   */
  if (isUnset && couldBeThisObj(env, env.collect.mInstrState.base)) {
    if (name) {
      if (auto const elem = thisPropType(env, name)) {
        if (elem->couldBe(BUninit)) mergeThisProp(env, name, TInitNull);
      }
    } else {
      mergeEachThisPropRaw(env, [&] (const Type& ty) {
        return ty.couldBe(BUninit) ? TInitNull : TBottom;
      });
    }
  }

  if (mustBeThisObj(env, env.collect.mInstrState.base)) {
    auto const optSelfTy = selfCls(env);
    auto const thisTy    = optSelfTy ? setctx(toobj(*optSelfTy)) : TObj;
    if (name) {
      auto const [ty, effects] = [&] () -> std::pair<Type, Effects> {
        if (update) {
          if (ReadonlyOp::Mutable == op &&
            isDefinitelyThisPropAttr(env, name, AttrIsReadonly)) {
            return { TBottom, Effects::AlwaysThrows };
          }
          if (auto const elem = thisPropType(env, name)) {
            return { *elem, Effects::Throws };
          }
        } else if (auto const propTy = thisPropAsCell(env, name)) {
          if (propTy->subtypeOf(BBottom)) {
            return { TBottom, Effects::AlwaysThrows };
          }
          if (isMaybeThisPropAttr(env, name, AttrLateInit)) {
            return { *propTy, Effects::Throws };
          }
          if (mode == MOpMode::None) {
            return { *propTy, Effects::None };
          }
          auto const elem = thisPropType(env, name);
          assertx(elem.has_value());
          return {
            *propTy,
            elem->couldBe(BUninit) ? Effects::Throws : Effects::None
          };
        }
        auto const raw =
          env.index.lookup_public_prop(thisTy, sval(name));
        return { update ? raw : to_cell(raw), Effects::Throws };
      }();

      if (ty.subtypeOf(BBottom)) return Effects::AlwaysThrows;
      moveBase(
        env,
        Base { ty, BaseLoc::Prop, thisTy, name },
        update
      );
      return effects;
    } else {
      moveBase(env,
               Base { TInitCell, BaseLoc::Prop, thisTy },
               update);
      return Effects::Throws;
    }
  }

  // We know for sure we're going to be in an object property.
  if (env.collect.mInstrState.base.type.subtypeOf(BObj)) {
    auto const raw =
      env.index.lookup_public_prop(
        env.collect.mInstrState.base.type,
        name ? sval(name) : TStr
      );
    auto const ty = update ? raw : to_cell(raw);
    if (ty.subtypeOf(BBottom)) return Effects::AlwaysThrows;
    moveBase(env,
             Base { ty,
                    BaseLoc::Prop,
                    env.collect.mInstrState.base.type,
                    name },
             update);
    return Effects::Throws;
  }

  /*
   * Otherwise, intermediate props with define can promote a null, false, or ""
   * to stdClass.  Those cases, and others, if it's MOpMode::Define, will set
   * the base to a null value in tvScratch.  The base may also legitimately be
   * an object and our next base is in an object property. Conservatively treat
   * all these cases as "possibly" being inside of an object property with
   * "Prop" with locType TCell.
   */
  moveBase(env,
           Base { TInitCell, BaseLoc::Prop, TCell, name },
           update);
  return Effects::Throws;
}

Effects miElem(ISS& env, MOpMode mode, Type key, LocalId keyLoc) {
  auto const isDefine = mode == MOpMode::Define;
  auto const isUnset  = mode == MOpMode::Unset;

  if (!isDefine && !isUnset) {
    // An Elem operation which doesn't mutate the base at all
    auto [elem, effects] = elemHelper(env, mode, std::move(key));
    if (effects != Effects::AlwaysThrows) {
      // Since this Dim is not mutating the base, we don't need to use
      // an array chain here.
      moveBase(
        env, Base { std::move(elem), BaseLoc::Elem }, false, keyLoc
      );
    }
    return effects;
  }

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  // These are similar to endBaseWithEffects, but moves the base or
  // extends the array chain instead.
  auto const move = [&] (Type ty, bool update, Effects effects) {
    if (effects == Effects::AlwaysThrows) {
      return endUnreachableBase(env, Promotion::No, keyLoc);
    }
    auto const effectFree = moveBase(
      env,
      Base { std::move(ty), BaseLoc::Elem },
      update,
      keyLoc
    );
    if (!effectFree && effects == Effects::None) return Effects::SideEffect;
    return effects;
  };
  auto const extend = [&] (Type ty, Effects effects) {
    assertx(effects != Effects::AlwaysThrows);
    if (!key.subtypeOf(BArrKey)) {
      assertx(effects == Effects::Throws);
      key = intersection_of(std::move(key), TArrKey);
      assertx(!key.is(BBottom));
    }
    auto const effectFree =
      extendArrChain(env, std::move(key), base, std::move(ty), keyLoc);
    if (!effectFree && effects == Effects::None) return Effects::SideEffect;
    return effects;
  };

  if (isUnset) {
    // ElemU. These types either always throw, or set the base to
    // Uninit.
    auto const alwaysThrows =
      BCls | BLazyCls | BFunc | BRFunc | BStr | BClsMeth |
      BRClsMeth;
    auto const movesToUninit = BPrim | BRes | BEnumClassLabel;
    auto const handled =
      alwaysThrows | movesToUninit | BObj | BArrLike;

    static_assert(handled == BCell);

    // It is quite unfortunate that InitNull doesn't always throw, as
    // this means we cannot use an array chain with a nullable array
    // (which is common).
    if (base.couldBe(BArrLike) && base.subtypeOf(alwaysThrows | BArrLike)) {
      auto elem = array_do_elem(env, key, true);
      if (elem.throws == TriBool::Yes) {
        // We'll always throw
        return endUnreachableBase(env, Promotion::No, keyLoc);
      }
      if (!elem.present) elem.elem |= TInitNull;
      auto const maybeAlwaysThrows = base.couldBe(alwaysThrows);
      // Keysets will throw, so we can assume the base does not
      // contain them afterwards.
      if (base.couldBe(BKeyset)) base = remove_keyset(std::move(base));
      base = loosen_array_staticness(std::move(base)); // Handle COW
      // We can safely remove the always throws bits, since if we get
      // to the next op, it means they weren't present.
      if (maybeAlwaysThrows) base = remove_bits(std::move(base), alwaysThrows);
      return extend(
        std::move(elem.elem),
        (maybeAlwaysThrows || elem.throws == TriBool::Maybe)
          ? Effects::Throws : Effects::None
      );
    }

    OptEffects effects;
    auto ty = TBottom;
    auto update = false;
    auto refine = BBottom;

    // These always raise an error
    if (base.couldBe(alwaysThrows)) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= alwaysThrows;
    }
    // These silently set the base to uninit
    if (base.couldBe(movesToUninit)) {
      effects = unionEffects(effects, Effects::None);
      ty |= TUninit;
    }
    // Objects can throw and retrieve anything
    if (base.couldBe(BObj)) {
      effects = unionEffects(effects, Effects::Throws);
      ty |= TInitCell;
    }
    // For arrays we can do a precise lookup
    if (base.couldBe(BArrLike)) {
      auto elem = array_do_elem(env, key, true);
      if (elem.throws == TriBool::Yes) {
        // Bad key or the element never exists. We'll always throw.
        effects = unionEffects(effects, Effects::AlwaysThrows);
        refine |= BArrLike;
      } else {
        if (!elem.present) elem.elem |= TInitNull;
        // Keysets will throw, so we can assume the base does not
        // contain them afterwards.
        if (base.couldBe(BKeyset)) base = remove_keyset(std::move(base));
        // Since we're not using an array chain here, we need to
        // pessimize the array (since we won't be able to track
        // further changes to its inner structure).
        base = loosen_array_staticness(loosen_array_values(std::move(base)));
        update = true;
        effects = unionEffects(
          effects,
          elem.throws == TriBool::Maybe
            ? Effects::Throws
            : Effects::None
        );
        ty |= std::move(elem.elem);
      }
    }

    // Refine the base and remove bits that will always throw
    if (refine && shouldRefineBase(env)) {
      base = remove_bits(std::move(base), refine);
      update = true;
    }

    assertx(effects.has_value());

    return move(std::move(ty), update, *effects);
  } else {
    assertx(isDefine);

    // ElemD. These types either always throw, or emit a warning and
    // push InitNull.
    auto const alwaysThrows = BNull | BFalse | BStr;
    auto const warnsAndNull =
      BTrue | BNum | BRes | BFunc | BRFunc | BCls | BLazyCls |
      BClsMeth | BRClsMeth | BEnumClassLabel;
    auto const handled =
      alwaysThrows | warnsAndNull | BObj | BArrLike;

    static_assert(handled == BCell);

    // As a special case, if we just have an array, we can extend an
    // array chain and track the modifications to the inner array
    // structure. We also allow the types which always throw since
    // they don't affect the structure (if we get one at runtime,
    // we'll just throw without modifying anything).
    if (base.couldBe(BArrLike) && base.subtypeOf(alwaysThrows | BArrLike)) {
      auto elem = array_do_elem(env, key, true);
      if (elem.elem.is(BBottom) || elem.throws == TriBool::Yes) {
        // We'll always throw
        return endUnreachableBase(env, Promotion::No, keyLoc);
      }
      auto const maybeAlwaysThrows = base.couldBe(alwaysThrows);
      // Keysets will throw, so we can assume the base does not
      // contain them afterwards.
      if (base.couldBe(BKeyset)) base = remove_keyset(std::move(base));
      base = loosen_array_staticness(std::move(base)); // Handle COW
      // We can safely remove the always throws bits, since if we get
      // to the next op, it means they weren't present.
      if (maybeAlwaysThrows) base = remove_bits(std::move(base), alwaysThrows);
      auto const mightThrow =
        maybeAlwaysThrows ||
        !elem.present ||
        elem.throws == TriBool::Maybe;
      return extend(
        std::move(elem.elem),
        mightThrow ? Effects::Throws : Effects::None
      );
    }

    OptEffects effects;
    auto ty = TBottom;
    auto update = false;
    auto refine = BBottom;

    // These always raise an error
    if (base.couldBe(alwaysThrows)) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= alwaysThrows;
    }
    // These emit a warning and push InitNull
    if (base.couldBe(warnsAndNull)) {
      effects = unionEffects(effects, Effects::Throws);
      ty |= TInitNull;
    }
    // Objects can throw and push anything
    if (base.couldBe(BObj)) {
      effects = unionEffects(effects, Effects::Throws);
      ty |= TInitCell;
    }
    // For arrays we can lookup the specific element
    if (base.couldBe(BArrLike)) {
      auto elem = array_do_elem(env, key, true);
      if (elem.elem.is(BBottom) || elem.throws == TriBool::Yes) {
        // Bad key or the element never exists. We'll always throw.
        effects = unionEffects(effects, Effects::AlwaysThrows);
        refine |= BArrLike;
      } else {
        // Keysets will throw, so we can assume the base does not
        // contain them afterwards.
        if (base.couldBe(BKeyset)) base = remove_keyset(std::move(base));
        // Since we're not using an array chain here, we need to
        // pessimize the array (since we won't be able to track
        // further changes to its inner structure).
        base = loosen_array_staticness(loosen_array_values(std::move(base)));
        update = true;
        effects = unionEffects(
          effects,
          (!elem.present || elem.throws == TriBool::Maybe)
            ? Effects::Throws
            : Effects::None
        );
        ty |= std::move(elem.elem);
      }
    }

    // Refine the base and remove bits that will always throw
    if (refine && shouldRefineBase(env)) {
      base = remove_bits(std::move(base), refine);
      update = true;
    }

    assertx(effects.has_value());

    return move(std::move(ty), update, *effects);
  }
}

Effects miNewElem(ISS& env) {
  // NewElem. These all either throw or raise a warning and set the
  // base to Uninit.
  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const alwaysThrows =
    BNull | BFalse | BArrLike | BObj | BClsMeth;
  auto const uninit =
    BTrue | BNum | BRes | BRFunc | BFunc |
    BRClsMeth | BCls | BLazyCls | BEnumClassLabel;
  auto const handled = alwaysThrows | uninit | BStr;

  static_assert(handled == BCell);

  OptEffects effects;
  auto newBase = TBottom;
  auto update = false;
  auto refine = BBottom;

  if (base.couldBe(alwaysThrows)) {
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= alwaysThrows;
  }
  if (base.couldBe(uninit)) {
    effects = unionEffects(effects, Effects::Throws);
    newBase |= TUninit;
  }
  if (base.couldBe(BStr)) {
    if (is_specialized_string(base) && sval_of(base)->empty()) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BStr;
    } else {
      effects = unionEffects(effects, Effects::Throws);
      newBase |= TUninit;
    }
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());
  assertx(*effects == Effects::Throws || *effects == Effects::AlwaysThrows);
  if (*effects != Effects::AlwaysThrows) {
    moveBase(env, Base { std::move(newBase), BaseLoc::Elem }, update);
  }
  return *effects;
}

//////////////////////////////////////////////////////////////////////
// final prop ops

Effects miFinalIssetProp(ISS& env, int32_t nDiscard, const Type& key) {
  auto const name = mStringKey(key);
  discard(env, nDiscard);

  if (name && mustBeThisObj(env, env.collect.mInstrState.base)) {
    if (auto const pt = thisPropAsCell(env, name)) {
      if (isMaybeThisPropAttr(env, name, AttrLateInit)) {
        // LateInit props can always be maybe unset, except if its never set at
        // all.
        push(env, pt->subtypeOf(BBottom) ? TFalse : TBool);
      } else if (pt->subtypeOf(BNull)) {
        push(env, TFalse);
      } else if (!pt->couldBe(BNull)) {
        push(env, TTrue);
      } else {
        push(env, TBool);
      }
      return Effects::None;
    }
  }

  push(env, TBool);
  return Effects::Throws;
}

Effects miFinalCGetProp(ISS& env, int32_t nDiscard, const Type& key,
                        bool quiet, ReadonlyOp op) {
  auto const name = mStringKey(key);
  discard(env, nDiscard);

  auto const& base = env.collect.mInstrState.base.type;

  if (name) {
    if (mustBeThisObj(env, env.collect.mInstrState.base)) {
      if (auto t = thisPropAsCell(env, name)) {
        if (t->subtypeOf(BBottom)) {
          push(env, TBottom);
          return Effects::AlwaysThrows;
        }
        if (ReadonlyOp::Mutable == op &&
          isDefinitelyThisPropAttr(env, name, AttrIsReadonly)) {
          push(env, TBottom);
          return Effects::AlwaysThrows;
        }
        push(env, std::move(*t));
        if (ReadonlyOp::Mutable == op &&
          isMaybeThisPropAttr(env, name, AttrIsReadonly)) {
          return Effects::Throws;
        }
        if (isMaybeThisPropAttr(env, name, AttrLateInit)) {
          return Effects::Throws;
        }
        if (!isDefinitelyThisPropAttr(env, name, AttrInitialSatisfiesTC)) {
          return Effects::Throws;
        }
        if (quiet) return Effects::None;
        auto const elem = thisPropType(env, name);
        assertx(elem.has_value());
        return elem->couldBe(BUninit) ? Effects::Throws : Effects::None;
      }
    }
    auto ty = [&] {
      if (!base.couldBe(BObj)) return TInitNull;
      auto t = to_cell(
        env.index.lookup_public_prop(
          base.subtypeOf(BObj) ? base : intersection_of(base, TObj),
          sval(name)
        )
      );
      if (!base.subtypeOf(BObj)) t = opt(std::move(t));
      return t;
    }();
    auto const e =
      ty.subtypeOf(BBottom) ? Effects::AlwaysThrows : Effects::Throws;
    push(env, std::move(ty));
    return e;
  }

  push(env, TInitCell);
  return Effects::Throws;
}

Effects miFinalSetProp(ISS& env, int32_t nDiscard, const Type& key, ReadonlyOp op) {
  auto const name = mStringKey(key);
  auto const t1 = unctx(popC(env));

  auto const finish = [&](Type ty) {
    endBase(env);
    discard(env, nDiscard);
    push(env, std::move(ty));
    return Effects::Throws;
  };

  auto const alwaysThrows = [&] {
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  };

  if (ReadonlyOp::Readonly == op &&
    !isMaybeThisPropAttr(env, name, AttrIsReadonly)) {
    return alwaysThrows();
  }

  if (couldBeThisObj(env, env.collect.mInstrState.base)) {
    if (!name) {
      mergeEachThisPropRaw(
        env,
        [&] (const Type& propTy) {
          return propTy.couldBe(BInitCell) ? t1 : TBottom;
        }
      );
    } else {
      mergeThisProp(env, name, t1);
    }
  }

  if (env.collect.mInstrState.base.type.subtypeOf(BObj)) {
    if (t1.subtypeOf(BBottom)) return alwaysThrows();
    moveBase(
      env,
      Base { t1, BaseLoc::Prop, env.collect.mInstrState.base.type, name }
    );
    return finish(t1);
  }

  moveBase(env, Base { TInitCell, BaseLoc::Prop, TCell, name });
  return finish(TInitCell);
}

Effects miFinalSetOpProp(ISS& env, int32_t nDiscard,
                      SetOpOp subop, const Type& key) {
  auto const name = mStringKey(key);
  auto const rhsTy = popC(env);

  auto const& base = env.collect.mInstrState.base.type;

  if (!base.couldBe(BObj)) {
    endBase(env);
    discard(env, nDiscard);
    push(env, TInitNull);
    return Effects::Throws;
  }

  auto lhsTy = [&] {
    if (name) {
      if (mustBeThisObj(env, env.collect.mInstrState.base)) {
        if (auto const t = thisPropAsCell(env, name)) return *t;
      }
      return to_cell(
        env.index.lookup_public_prop(
          base.subtypeOf(BObj) ? base : intersection_of(base, TObj),
          sval(name)
        )
      );
    }
    return TInitCell;
  }();
  if (lhsTy.subtypeOf(BBottom)) {
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  }
  if (!base.subtypeOf(BObj)) lhsTy = opt(std::move(lhsTy));

  auto const resultTy = base.subtypeOf(BObj)
    ? typeSetOp(subop, lhsTy, rhsTy)
    : TInitCell;

  if (resultTy.subtypeOf(BBottom)) {
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  }

  if (couldBeThisObj(env, env.collect.mInstrState.base)) {
    if (name) {
      mergeThisProp(env, name, resultTy);
    } else {
      killThisProps(env);
    }
  }

  endBase(env);
  discard(env, nDiscard);
  push(env, resultTy);
  return Effects::Throws;
}

Effects miFinalIncDecProp(ISS& env, int32_t nDiscard,
                          IncDecOp subop, const Type& key) {
  auto const name = mStringKey(key);

  auto const& base = env.collect.mInstrState.base.type;

  if (!base.couldBe(BObj)) {
    endBase(env);
    discard(env, nDiscard);
    push(env, TInitNull);
    return Effects::Throws;
  }

  auto postPropTy = [&] {
    if (name) {
      if (mustBeThisObj(env, env.collect.mInstrState.base)) {
        if (auto const t = thisPropAsCell(env, name)) return *t;
      }
      return to_cell(
        env.index.lookup_public_prop(
          base.subtypeOf(BObj) ? base : intersection_of(base, TObj),
          sval(name)
        )
      );
    }
    return TInitCell;
  }();
  if (postPropTy.subtypeOf(BBottom)) {
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  }
  if (!base.subtypeOf(BObj)) postPropTy = opt(std::move(postPropTy));

  auto const prePropTy = base.subtypeOf(TObj)
    ? typeIncDec(subop, postPropTy)
    : TInitCell;
  if (prePropTy.subtypeOf(BBottom)) {
    discard(env, nDiscard);
    push(env, TBottom);
    return Effects::AlwaysThrows;
  }

  if (couldBeThisObj(env, env.collect.mInstrState.base)) {
    if (name) {
      mergeThisProp(env, name, prePropTy);
    } else {
      killThisProps(env);
    }
  }

  endBase(env);
  discard(env, nDiscard);
  push(env, isPre(subop) ? prePropTy : postPropTy);
  return Effects::Throws;
}

//////////////////////////////////////////////////////////////////////
// Final elem ops

Effects miFinalCGetElem(ISS& env, int32_t nDiscard,
                        const Type& key, MOpMode mode) {
  auto [elem, effects] = elemHelper(env, mode, key);
  discard(env, nDiscard);
  push(env, std::move(elem));
  return effects;
}

Effects miFinalIssetElem(ISS& env,
                         int32_t nDiscard,
                         const Type& key) {
  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const pushesFalse =
    BNull | BBool | BNum | BRes | BFunc | BRFunc | BRClsMeth | BClsMeth;
  auto const handled = pushesFalse | BArrLike;

  OptEffects effects;
  auto pushed = TBottom;

  if (base.couldBe(pushesFalse)) {
    effects = unionEffects(effects, Effects::None);
    pushed |= TFalse;
  }
  if (base.couldBe(BArrLike)) {
    auto const elem = array_do_elem(env, key);
    if (elem.throws == TriBool::Yes) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
    } else {
      effects = unionEffects(
        effects,
        elem.throws == TriBool::Maybe
          ? Effects::Throws
          : Effects::None
      );

      if (elem.elem.subtypeOf(BNull)) {
        pushed |= TFalse;
      } else if (elem.present && !elem.elem.couldBe(BNull)) {
        pushed |= TTrue;
      } else {
        pushed |= TBool;
      }
    }
  }

  if (!base.subtypeOf(handled)) {
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TBool;
  }

  assertx(effects.has_value());
  discard(env, nDiscard);
  push(env, std::move(pushed));
  return *effects;
}

Effects miFinalSetElem(ISS& env,
                       int32_t nDiscard,
                       const Type& key,
                       LocalId keyLoc) {
  auto const rhs = popC(env);

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const alwaysThrows = BNull | BFalse;
  auto const pushesNull =
    BTrue | BNum | BRes | BFunc | BRFunc |
    BCls | BLazyCls | BClsMeth | BRClsMeth | BEnumClassLabel;
  auto const handled =
    alwaysThrows | pushesNull | BObj | BStr | BArrLike;

  static_assert(handled == BCell);

  OptEffects effects;
  auto pushed = TBottom;
  auto update = false;
  auto refine = BBottom;

  if (base.couldBe(pushesNull)) {
    // These emit a warning and push null
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TInitNull;
  }
  if (base.couldBe(alwaysThrows)) {
    // These always raise a fatal
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= alwaysThrows;
  }
  if (base.couldBe(BStr)) {
    // String is a special case here. It will throw on empty strings,
    // and otherwise can return null or a static string (and possibly
    // warn). We don't bother predicting the effects of the set on the
    // string here, so be pessimistic and forget what we know about
    // the base in regards to staticness and values.
    if (is_specialized_string(base) && sval_of(base)->empty()) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BStr;
    } else {
      effects = unionEffects(effects, Effects::Throws);
      base = loosen_string_staticness(loosen_string_values(std::move(base)));
      update = true;
      pushed |= TOptSStr;
    }
  }
  if (base.couldBe(BObj)) {
    // Objects can throw but otherwise don't affect the base and push
    // the rhs.
    effects = unionEffects(effects, Effects::Throws);
    pushed |= rhs;
  }
  if (base.couldBe(BArrLike)) {
    // Arrays will set the value and push the right hande side of the
    // assignment (keysets will always fatal because you can't do a
    // set on them).
    auto const doesThrow = array_do_set(env, key, rhs);
    if (doesThrow == TriBool::Yes) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BArrLike;
    } else {
      effects = unionEffects(
        effects,
        doesThrow == TriBool::No ? Effects::None : Effects::Throws
      );
      pushed |= rhs;
      update = true;
    }
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());

  return endBaseWithEffects(
    env, *effects, update, Promotion::No,
    [&] {
      discard(env, nDiscard);
      push(env, std::move(pushed));
    },
    keyLoc
  );
}

Effects miFinalSetOpElem(ISS& env, int32_t nDiscard,
                         SetOpOp subop, const Type& key,
                         LocalId keyLoc) {
  auto const rhsTy = popC(env);
  return setOpElemHelper(
    env, nDiscard, key, keyLoc,
    [&] (const Type& lhsTy) {
      auto const result = typeSetOp(subop, lhsTy, rhsTy);
      if (result.is(BBottom)) {
        return std::make_tuple(TBottom, TBottom, Effects::AlwaysThrows);
      }
      return std::make_tuple(result, result, Effects::Throws);
    }
  );
}

Effects miFinalIncDecElem(ISS& env, int32_t nDiscard,
                          IncDecOp subop, const Type& key,
                          LocalId keyLoc) {
  return setOpElemHelper(
    env, nDiscard, key, keyLoc,
    [&] (const Type& before) {
      auto const after = typeIncDec(subop, before);
      if (after.is(BBottom)) {
        return std::make_tuple(TBottom, TBottom, Effects::AlwaysThrows);
      }
      return std::make_tuple(
        after,
        isPre(subop) ? after : before,
        before.subtypeOf(BNum) ? Effects::None : Effects::Throws
      );
    }
  );
}

Effects miFinalUnsetElem(ISS& env, int32_t nDiscard, const Type& key) {

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const doesNothing = BNull | BBool | BNum | BRes | BEnumClassLabel;
  auto const alwaysThrows =
    BFunc | BRFunc | BCls | BLazyCls | BStr | BClsMeth | BRClsMeth;
  auto const handled = doesNothing | alwaysThrows | BArrLike | BObj;

  static_assert(handled == BCell);

  OptEffects effects;
  auto update = false;
  auto refine = BBottom;

  // These silently does nothing
  if (base.couldBe(doesNothing)) {
    effects = unionEffects(effects, Effects::None);
  }
  // These always raise an error
  if (base.couldBe(alwaysThrows)) {
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= alwaysThrows;
  }
  // Objects can throw but otherwise do not affect the base
  if (base.couldBe(BObj)) {
    effects = unionEffects(effects, Effects::Throws);
  }
  if (base.couldBe(BArrLike)) {
    // Unset doesn't throw for a missing element on dicts, keysets, or
    // darrays, only if the key is invalid. Vecs and varrays silently do
    // nothing for string keys, but can throw with int keys.
    if (!key.couldBe(BArrKey)) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BArrLike;
    } else {
      auto e = key.subtypeOf(BArrKey) ? Effects::None : Effects::Throws;
      if (base.couldBe(BVec) && key.couldBe(BInt)) e = Effects::Throws;
      effects = unionEffects(effects, e);

      // We purposefully do not model the effects of unset on array
      // structure. This lets us assume that if we have array structure,
      // we also have no tombstones. Pessimize the base which drops array
      // structure and also remove emptiness information.
      if (!base.subtypeAmong(BVec, BArrLike) || key.couldBe(BInt)) {
        base = loosen_array_staticness(loosen_array_values(std::move(base)));
        base = loosen_emptiness(std::move(base));
        update = true;
      }
    }
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());

  return endBaseWithEffects(
    env, *effects, update, Promotion::No,
    [&] { discard(env, nDiscard); }
  );
}

//////////////////////////////////////////////////////////////////////
// Final new elem ops

Effects miFinalSetNewElem(ISS& env, int32_t nDiscard) {
  auto const rhs = popC(env);

  auto& base = env.collect.mInstrState.base.type;
  assertx(!base.is(BBottom));

  auto const pushesNull =
    BTrue | BNum | BRes | BFunc | BRFunc | BCls |
    BLazyCls | BClsMeth | BRClsMeth | BEnumClassLabel;
  auto const alwaysThrows = BNull | BStr | BFalse;
  auto const handled = pushesNull | alwaysThrows | BObj | BArrLike;

  static_assert(handled == BCell);

  OptEffects effects;
  auto pushed = TBottom;
  auto update = false;
  auto refine = BBottom;

  if (base.couldBe(pushesNull)) {
    // These emit a warning and push null
    effects = unionEffects(effects, Effects::Throws);
    pushed |= TInitNull;
  }
  if (base.couldBe(alwaysThrows)) {
    // These always raise a fatal
    effects = unionEffects(effects, Effects::AlwaysThrows);
    refine |= alwaysThrows;
  }
  if (base.couldBe(BObj)) {
    // Objects can throw but otherwise don't affect the base and push
    // the rhs.
    effects = unionEffects(effects, Effects::Throws);
    pushed |= rhs;
  }
  if (base.couldBe(BArrLike)) {
    // Arrays will add a new element and push the right hand side of the
    // assignment.
    auto const doesThrow = array_do_newelem(env, rhs);
    if (doesThrow == TriBool::Yes) {
      effects = unionEffects(effects, Effects::AlwaysThrows);
      refine |= BArrLike;
    } else {
      effects = unionEffects(
        effects,
        doesThrow == TriBool::No ? Effects::None : Effects::Throws
      );
      pushed |= rhs;
      update = true;
    }
  }

  // Refine the base and remove bits that will always throw
  if (refine && shouldRefineBase(env)) {
    base = remove_bits(std::move(base), refine);
    update = true;
  }

  assertx(effects.has_value());

  return endBaseWithEffects(
    env, *effects, update, Promotion::No,
    [&] {
      discard(env, nDiscard);
      push(env, std::move(pushed));
    }
  );
}

Effects miFinalSetOpNewElem(ISS& env, int32_t nDiscard) {
  popC(env);
  return setOpNewElemHelper(env, nDiscard);
}

Effects miFinalIncDecNewElem(ISS& env, int32_t nDiscard) {
  return setOpNewElemHelper(env, nDiscard);
}

//////////////////////////////////////////////////////////////////////

// Translate the aggregated effects of the instruction into the
// appropriate interp-state actions. Returns true if the instruction
// is totally effect-free.
bool handleEffects(ISS& env, Effects effects, Promotion keyPromotion) {
  auto const effectFree = [&]{
    switch (effects) {
      case Effects::None:
        if (keyPromotion == Promotion::YesMightThrow) return false;
        effect_free(env);
        return true;
      case Effects::SideEffect:
        if (keyPromotion != Promotion::YesMightThrow) nothrow(env);
        return false;
      case Effects::Throws:
        return false;
      case Effects::AlwaysThrows:
        unreachable(env);
        return false;
    }
    always_assert(false);
  }();

  if (env.flags.wasPEI && env.blk.throwExit != NoBlockId) {
    // Minstr opcodes may throw after side-effects.
    assertx(!effectFree);
    auto const state = with_throwable_only(env.index, env.state);
    env.propagate(env.blk.throwExit, &state);
  }

  return effectFree;
}

}

namespace interp_step {

//////////////////////////////////////////////////////////////////////
// Base operations

void in(ISS& env, const bc::BaseSC& op) {
  auto tcls = topC(env, op.arg2);
  auto const tname = topC(env, op.arg1);

  // We'll raise an error if its not a class
  if (!tcls.couldBe(BCls)) return unreachable(env);

  // Lookup what we know about the property
  auto lookup = env.index.lookup_static(
    env.ctx,
    env.collect.props,
    tcls,
    tname
  );

  // If we definitely didn't find anything, we'll definitely throw
  if (lookup.found == TriBool::No || lookup.ty.subtypeOf(BBottom)) {
    return unreachable(env);
  }

  // Whether we might potentially throw because of AttrConst
  auto mightConstThrow = false;
  switch (op.subop3) {
    case MOpMode::Define:
    case MOpMode::Unset:
    case MOpMode::InOut:
      // If its definitely const, we'll always throw. Otherwise we'll
      // potentially throw if there's a chance its AttrConst.
      if (lookup.isConst == TriBool::Yes) return unreachable(env);
      mightConstThrow = lookup.isConst == TriBool::Maybe;
      break;
    case MOpMode::None:
    case MOpMode::Warn:
      // These don't mutate the base, so AttrConst does not apply
      break;
  }

  // Whether we might potentially throw because of AttrIsReadonly
  if (ReadonlyOp::Mutable == op.subop4 && lookup.readOnly == TriBool::Yes) {
    return unreachable(env);
  }
  if (ReadonlyOp::CheckROCOW == op.subop4 && lookup.readOnly == TriBool::No) {
    return unreachable(env);
  }

  auto mightBeObj = lookup.ty.couldBe(BObj);
  auto const mightMutableThrow = op.subop4 == ReadonlyOp::Mutable &&
    (lookup.readOnly == TriBool::Maybe || lookup.readOnly == TriBool::Yes);
  auto const mightROCOWThrow = op.subop4 == ReadonlyOp::CheckROCOW && mightBeObj;
  auto const mightMutROCOWThrow = op.subop4 == ReadonlyOp::CheckMutROCOW &&
    (lookup.readOnly == TriBool::Maybe || lookup.readOnly == TriBool::Yes) && mightBeObj;

  auto const mightReadOnlyThrow =
    mightMutableThrow || mightROCOWThrow || mightMutROCOWThrow;

  // Loading the base from a static property can be considered
  // effect_free if there's no possibility of throwing. This requires
  // a definitely found, non-AttrLateInit property with normal class
  // initialization, and both the class and name have to be the normal
  // types.
  if (lookup.found == TriBool::Yes &&
      lookup.lateInit == TriBool::No &&
      lookup.internal == TriBool::No &&
      !lookup.classInitMightRaise &&
      !mightConstThrow &&
      !mightReadOnlyThrow &&
      tcls.subtypeOf(BCls) &&
      tname.subtypeOf(BStr)) {

    // If we're not mutating the base, and the base is a constant,
    // turn it into a BaseC with the appropriate constant on the
    // stack.
    if (op.subop3 == MOpMode::Warn || op.subop3 == MOpMode::None) {
      if (auto const v = tv(lookup.ty)) {
        if (can_rewind(env) && can_constprop(env)) {
          reduce(env, gen_constant(*v), bc::BaseC { 0, op.subop3 });
          env.collect.mInstrState.extraPop = true;
          return;
        }
      }
    }

    effect_free(env);
  }

  return startBase(
    env,
    Base {
      std::move(lookup.ty),
      BaseLoc::StaticProp,
      std::move(tcls),
      lookup.name
    }
  );
}

void in(ISS& env, const bc::BaseL& op) {
  auto ty = peekLocRaw(env, op.nloc1.id);
  auto throws = false;

  if (ReadonlyOp::CheckROCOW == op.subop3 && ty.couldBe(BObj)) throws = true;

  // An Uninit local base can raise a notice.
  if (!ty.couldBe(BUninit)) {
    // If we're not mutating the base, and the base is a constant,
    // turn it into a BaseC with the appropriate constant on the
    // stack.
    if (op.subop2 == MOpMode::Warn || op.subop2 == MOpMode::None) {
      if (auto const v = tv(ty)) {
        if (can_rewind(env) && can_constprop(env)) {
          reduce(env, gen_constant(*v), bc::BaseC { 0, op.subop2 });
          env.collect.mInstrState.extraPop = true;
          return;
        }
      }

      // Try to find an equivalent local to use instead
      auto const minLocEquiv = findMinLocEquiv(env, op.nloc1.id, false);
      if (minLocEquiv != NoLocalId) {
        return reduce(
          env,
          bc::BaseL {
            NamedLocal { kInvalidLocalName, minLocEquiv },
            op.subop2,
            op.subop3,
          }
        );
      }
    }

    if (!throws) effect_free(env);
  } else if (op.subop2 != MOpMode::Warn) {
    // The local could be Uninit, but we won't warn about it anyways.
    if (!throws) effect_free(env);
  }

  mayReadLocal(env, op.nloc1.id);
  if (ty.subtypeOf(BBottom)) return unreachable(env);

  startBase(
    env,
    Base {
      std::move(ty),
      BaseLoc::Local,
      TBottom,
      op.nloc1.name != kInvalidLocalName
        ? env.ctx.func->locals[op.nloc1.name].name
        : nullptr,
      op.nloc1.id
    }
  );
}

void in(ISS& env, const bc::BaseC& op) {
  assertx(op.arg1 < env.state.stack.size());
  auto ty = topC(env, op.arg1);
  if (ty.subtypeOf(BBottom)) return unreachable(env);
  effect_free(env);
  startBase(
    env,
    Base {
      std::move(ty),
      BaseLoc::Stack,
      TBottom,
      SString{},
      NoLocalId,
      (uint32_t)env.state.stack.size() - op.arg1 - 1
    }
  );
}

void in(ISS& env, const bc::BaseH&) {
  auto const ty = thisTypeNonNull(env);
  if (ty.subtypeOf(BBottom)) return unreachable(env);
  effect_free(env);
  startBase(env, Base{ty, BaseLoc::This});
}

//////////////////////////////////////////////////////////////////////
// Intermediate operations

void in(ISS& env, const bc::Dim& op) {
  auto key = key_type_or_fixup(env, op);
  if (!key) return;

  auto const effects = [&] {
    if (mcodeIsProp(op.mkey.mcode)) {
      return miProp(env, op.subop1, std::move(key->first), op.mkey.rop);
    } else if (mcodeIsElem(op.mkey.mcode)) {
      return miElem(env, op.subop1, std::move(key->first), key_local(env, op));
    } else {
      return miNewElem(env);
    }
  }();

  if (effects != Effects::None) {
    env.collect.mInstrState.effectFree = false;
  }
  if (!handleEffects(env, effects, key->second)) return;

  // This instruction must be effect free
  assertx(env.flags.effectFree);
  assertx(!env.state.unreachable);

  // If the base is a constant, and we're not mutating the base, and
  // if the entire minstr sequence up until now has been effect-free,
  // we can remove the entire sequence up until now. We replace it
  // with just a BaseC on the constant pushed onto the stack.
  if ((op.subop1 == MOpMode::None || op.subop1 == MOpMode::Warn) &&
      env.collect.mInstrState.effectFree &&
      will_reduce(env) &&
      can_rewind(env) &&
      can_constprop(env) &&
      is_scalar(env.collect.mInstrState.base.type)) {
    // Find the base instruction which started the sequence.
    for (int i = 0; ; i++) {
      auto const last = last_op(env, i);
      if (!last) break;
      if (isMemberBaseOp(last->op)) {
        auto const base = *last;
        rewind(env, i + 1);
        // We'll need to push the constant onto the stack. If the
        // sequence originally started with a BaseC
        // instruction, we can just pop off the original value and
        // replace it with the constant. This leaves all offsets the
        // same. If not, we push the constant and set extraPop, which
        // makes us increment all of the offsets when we reprocess
        // them.
        auto const reuseStack =
          [&] {
            switch (base.op) {
              case Op::BaseC:  return base.BaseC.arg1 == 0;
              default: return false;
            }
          }();
        assertx(!env.collect.mInstrState.extraPop || reuseStack);
        auto const extraPop = !reuseStack || env.collect.mInstrState.extraPop;
        env.collect.mInstrState.clear();
        if (reuseStack) reduce(env, bc::PopC {});
        auto const v = tv(env.collect.mInstrState.base.type);
        assertx(v);
        reduce(env, gen_constant(*v), bc::BaseC { 0, op.subop1 });
        env.collect.mInstrState.extraPop = extraPop;
        return;
      }
      if (!isMemberDimOp(last->op)) break;
    }
  }
}

//////////////////////////////////////////////////////////////////////
// Final operations

const StaticString s_classname("classname");
const StaticString s_type_structure("HH\\type_structure");
const StaticString s_type_structure_classname("HH\\type_structure_classname");

void in(ISS& env, const bc::QueryM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const nDiscard = op.arg1;

  auto const effects = [&] {
    if (mcodeIsProp(op.mkey.mcode)) {
      // We don't currently do anything different for nullsafe query ops.
      switch (op.subop2) {
        case QueryMOp::CGet:
        case QueryMOp::CGetQuiet: {
          return miFinalCGetProp(env, nDiscard, key->first,
                                 op.subop2 == QueryMOp::CGetQuiet, op.mkey.rop);
        }
        case QueryMOp::Isset:
          return miFinalIssetProp(env, nDiscard, key->first);
        case QueryMOp::InOut:
          always_assert(false);
      }
      always_assert(false);
    } else if (mcodeIsElem(op.mkey.mcode)) {
      switch (op.subop2) {
        case QueryMOp::InOut:
        case QueryMOp::CGet:
        case QueryMOp::CGetQuiet:
          return miFinalCGetElem(
            env, nDiscard, key->first, getQueryMOpMode(op.subop2)
          );
        case QueryMOp::Isset:
          return miFinalIssetElem(env, nDiscard, key->first);
      }
      always_assert(false);
    } else {
      // QueryMNewElem will always throw without doing any work.
      discard(env, nDiscard);
      push(env, TBottom);
      return Effects::AlwaysThrows;
    }
  }();

  // Try to detect type_structure(cls_name, cns_name)['classname'] and
  // reduce this to type_structure_classname(cls_name, cns_name)
  if (mcodeIsElem(op.mkey.mcode) &&
      op.subop2 == QueryMOp::CGet &&
      nDiscard == 1 &&
      op.mkey.mcode == MemberCode::MET &&
      op.mkey.litstr == s_classname.get() &&
      can_rewind(env)) {
    if (auto const last = last_op(env, 0)) {
      if (last->op == Op::BaseC) {
        if (auto const prev = last_op(env, 1)) {
          if (prev->op == Op::FCallFuncD &&
              prev->FCallFuncD.str2 == s_type_structure.get() &&
              prev->FCallFuncD.fca.numArgs() == 2) {
            auto const params = prev->FCallFuncD.fca.numArgs();
            rewind(env, op); // querym
            rewind(env, 2);  // basec + fcallfuncd
            env.collect.mInstrState.clear();
            return reduce(
              env,
              bc::FCallFuncD {
                FCallArgs(params),
                s_type_structure_classname.get()
              }
            );
          }
        }
      }
    }
  }

  if (effects != Effects::None) env.collect.mInstrState.effectFree = false;

  // For the QueryM ops, its our responsibility to call endBase()
  // (unless we'll always throw).

  if (!handleEffects(env, effects, key->second)) {
    if (effects != Effects::AlwaysThrows) endBase(env, false);
    return;
  }

  assertx(env.flags.effectFree);
  assertx(!env.state.unreachable);

  // If the QueryM produced a constant without any possible
  // side-ffects, we can replace the entire thing with the constant.
  if (env.collect.mInstrState.effectFree &&
      can_rewind(env) &&
      can_constprop(env) &&
      is_scalar(topC(env))) {
    for (int i = 0; ; i++) {
      auto const last = last_op(env, i);
      if (!last) break;
      if (isMemberBaseOp(last->op)) {
        auto const v = tv(topC(env));
        rewind(env, op);
        rewind(env, i + 1);
        env.collect.mInstrState.clear();
        BytecodeVec bcs{nDiscard, bc::PopC{}};
        bcs.push_back(gen_constant(*v));
        return reduce(env, std::move(bcs));
      }
      if (!isMemberDimOp(last->op)) break;
    }
  }
  endBase(env, false);
}

void in(ISS& env, const bc::SetM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;

  auto const effects = [&] {
    if (mcodeIsProp(op.mkey.mcode)) {
      return miFinalSetProp(env, op.arg1, key->first, op.mkey.rop);
    } else if (mcodeIsElem(op.mkey.mcode)) {
      return miFinalSetElem(env, op.arg1, key->first, key_local(env, op));
    } else {
      return miFinalSetNewElem(env, op.arg1);
    }
  }();
  handleEffects(env, effects, key->second);
}

void in(ISS& env, const bc::SetRangeM& op) {
  popC(env);
  popC(env);
  popC(env);
  discard(env, op.arg1);
  auto& base = env.collect.mInstrState.base.type;
  if (!base.couldBe(BStr)) return unreachable(env);
  base = loosen_staticness(loosen_values(std::move(base)));
  endBase(env);
}

void in(ISS& env, const bc::IncDecM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;

  auto const effects = [&] {
    if (mcodeIsProp(op.mkey.mcode)) {
      return miFinalIncDecProp(env, op.arg1, op.subop2, key->first);
    } else if (mcodeIsElem(op.mkey.mcode)) {
      return miFinalIncDecElem(
        env, op.arg1, op.subop2, key->first, key_local(env, op)
      );
    } else {
      return miFinalIncDecNewElem(env, op.arg1);
    }
  }();
  handleEffects(env, effects, key->second);
}

void in(ISS& env, const bc::SetOpM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;

  auto const effects = [&] {
    if (mcodeIsProp(op.mkey.mcode)) {
      return miFinalSetOpProp(env, op.arg1, op.subop2, key->first);
    } else if (mcodeIsElem(op.mkey.mcode)) {
      return miFinalSetOpElem(
        env, op.arg1, op.subop2, key->first, key_local(env, op)
      );
    } else {
      return miFinalSetOpNewElem(env, op.arg1);
    }
  }();
  handleEffects(env, effects, key->second);
}

void in(ISS& env, const bc::UnsetM& op) {
  assertx(mcodeIsElem(op.mkey.mcode));
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;

  auto const effects = miFinalUnsetElem(env, op.arg1, key->first);
  handleEffects(env, effects, key->second);
}

}

//////////////////////////////////////////////////////////////////////

}
