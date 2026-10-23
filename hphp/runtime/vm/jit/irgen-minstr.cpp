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

#include "hphp/runtime/vm/jit/irgen-minstr.h"
#include "hphp/runtime/vm/jit/irgen.h"

#include "hphp/runtime/base/strings.h"
#include "hphp/runtime/base/collections.h"

#include "hphp/runtime/vm/jit/array-access-profile.h"
#include "hphp/runtime/vm/jit/cow-profile.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"
#include "hphp/runtime/vm/jit/type.h"

#include "hphp/runtime/vm/jit/irgen-arith.h"
#include "hphp/runtime/vm/jit/irgen-call.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-incdec.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"
#include "hphp/runtime/vm/jit/irgen-sprop-global.h"
#include "hphp/runtime/vm/jit/irgen-types.h"

#include "hphp/runtime/vm/jit/irgen-internal.h"

#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/safe-cast.h"
#include "hphp/util/struct-log.h"

#include <fmt/format.h>

namespace HPHP::jit::irgen {

namespace {

//////////////////////////////////////////////////////////////////////

enum class SimpleOp {
  None,
  Vec,
  Dict,
  Keyset,
  String,
  Vector, // c_Vector* or c_ImmVector*
  Map,    // c_Map*
  Pair,   // c_Pair*
};

//////////////////////////////////////////////////////////////////////
// Property information.

struct PropInfo {
  PropInfo(Slot slot,
           uint16_t index,
           bool isConst,
           bool readOnly,
           bool lateInit,
           bool lateInitCheck,
           Type knownType,
           const HPHP::TypeIntersectionConstraint* typeConstraints,
           const Class::Prop* objProp,
           const Class* propClass)
    : slot{slot}
    , index{index}
    , isConst{isConst}
    , readOnly{readOnly}
    , lateInit{lateInit}
    , lateInitCheck{lateInitCheck}
    , knownType{std::move(knownType)}
    , typeConstraints{typeConstraints}
    , objProp{objProp}
    , propClass{propClass}
  {}

  Slot slot{kInvalidSlot};
  uint16_t index{0};
  bool isConst{false};
  bool readOnly{false};
  bool lateInit{false};
  bool lateInitCheck{false};
  Type knownType{TCell};
  const HPHP::TypeIntersectionConstraint* typeConstraints{nullptr};
  const Class::Prop* objProp{nullptr};
  const Class* propClass{nullptr};

};

Type knownTypeForProp(const Class::Prop& prop,
                      const Class* propCls,
                      const Class* ctx,
                      bool ignoreLateInit,
                      bool systemLibOnly = false) {
  auto knownType = TCell;
  if (Cfg::Eval::CheckPropTypeHints >= 3) {
    knownType = typeFromPropTC(prop.typeConstraints, propCls, ctx, false, systemLibOnly);
    if (!(prop.attrs & AttrNoImplicitNullable)) knownType |= TInitNull;
  }
  knownType &= typeFromRAT(prop.repoAuthType, ctx);
  // Repo-auth-type doesn't include uninit for AttrLateInit props, so we need to
  // add it after intersecting with it.
  if (prop.attrs & AttrLateInit) {
    // If we're ignoring AttrLateInit, the prop might be uninit, but if we're
    // validating it, we'll never see uninit, so remove it.
    if (ignoreLateInit) {
      knownType |= TUninit;
    } else {
      knownType -= TUninit;
    }
  }
  return knownType;
}

/*
 * Try to find a property offset for the given key in baseClass. Will return
 * std::nullopt if the mapping from baseClass's name to the Class* can change
 * (which happens in sandbox mode when the ctx class is unrelated to baseClass).
 */
Optional<PropInfo>
getPropertyOffset(IRGS& env,
                  const Class* baseClass,
                  Type keyType,
                  bool ignoreLateInit) {
  if (!baseClass) return std::nullopt;

  if (!keyType.hasConstVal(TStr)) return std::nullopt;
  auto const name = keyType.strVal();

  auto const ctx = curClass(env);
  auto const moduleName = curUnit(env)->moduleName();

  // We need to check that baseClass cannot change between requests.
  if (!(baseClass->preClass()->attrs() & AttrPersistent)) {
    if (!ctx) return std::nullopt;
    if (!ctx->classof(baseClass)) {
      if (baseClass->classof(ctx)) {
        // baseClass can change on us in between requests, but since
        // ctx is an ancestor of baseClass we can make the weaker
        // assumption that the object is an instance of ctx
        baseClass = ctx;
      } else {
        // baseClass can change on us in between requests and it is
        // not related to ctx, so bail out
        return std::nullopt;
      }
    }
  }
  auto const propCtx = MemberLookupContext(ctx, moduleName);

  // Lookup the index of the property based on ctx and baseClass
  auto const lookup = baseClass->getDeclPropSlot(propCtx, name);
  auto const slot = lookup.slot;

  // If we couldn't find a property that is accessible in the current context,
  // bail out
  if (slot == kInvalidSlot || !lookup.accessible) return std::nullopt;

  // If it's a declared property we're good to go: even if a subclass redefines
  // an accessible property with the same name it's guaranteed to be at the same
  // offset.

  auto const& prop = baseClass->declProperties()[slot];

  // If we're going to serialize the profile data, we emit a ProfileProp here to
  // profile property accesses.  We only do this here, when we can resolve the
  // property at JIT time, in order to make profiling simpler and cheaper.  And
  // this should cover the vast majority of the property accesses anyway.
  if (env.context.kind == TransKind::Profile && isJitSerializing()) {
    gen(env, ProfileProp, cns(env, prop.baseCls->name()), cns(env, name));
  }

  return PropInfo(
    slot,
    baseClass->propSlotToIndex(slot),
    prop.attrs & AttrIsConst,
    prop.attrs & AttrIsReadonly,
    prop.attrs & AttrLateInit,
    (prop.attrs & AttrLateInit) && !ignoreLateInit,
    knownTypeForProp(prop, baseClass, ctx, ignoreLateInit),
    &prop.typeConstraints,
    &prop,
    prop.cls
  );
}

/*
 * Returns true iff a Prop{X,DX,Q} operation with the given base and key will
 * not write to its tvRef src, using the same set of conditions checked in
 * ObjectData::propImpl(). This allows us to skip the very expensive ratchet
 * operation after intermediate operations.
 */
bool prop_ignores_tvref(IRGS& env, SSATmp* base, const SSATmp* key) {
  // Make sure it's an object of a known class.
  if (!base->isA(TObj) || !base->type().clsSpec().cls()) return false;

  auto cls = base->type().clsSpec().cls();
  auto isDeclared = false;
  auto propClass = cls;

  // If the property name is known, try to look it up and get its RAT.
  if (key->hasConstVal(TStr)) {
    auto const keyStr = key->strVal();
    auto const ctx = MemberLookupContext(curClass(env), curUnit(env)->moduleName());
    auto const lookup = cls->getDeclPropSlot(ctx, keyStr);
    if (lookup.slot != kInvalidSlot && lookup.accessible) {
      isDeclared = true;
      auto const& prop = cls->declProperties()[lookup.slot];
      propClass = prop.cls;
    }
  }

  // Native prop handlers never kick in for declared properties, even if
  // they're unset.
  if (!isDeclared && cls->hasNativePropHandler()) return false;

  if (propClass == cls ||
      env.irb->constrainValue(base, GuardConstraint(propClass).setWeak())) {
    env.irb->constrainValue(base, GuardConstraint(cls));
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

Optional<GuardConstraint> simpleOpConstraint(SimpleOp op) {
  switch (op) {
    case SimpleOp::None:
      return std::nullopt;

    case SimpleOp::Vec:
    case SimpleOp::Dict:
    case SimpleOp::Keyset:
    case SimpleOp::String:
      return GuardConstraint(DataTypeSpecific);

    case SimpleOp::Vector:
      return GuardConstraint(c_Vector::classof());

    case SimpleOp::Map:
      return GuardConstraint(c_Map::classof());

    case SimpleOp::Pair:
      return GuardConstraint(c_Pair::classof());
  }

  always_assert(false);
}

//////////////////////////////////////////////////////////////////////

/*
 * Store the member base pointer.
 */

void stMBase(IRGS& env, SSATmp* base) {
  if (base->isA(TPtr)) base = gen(env, ConvPtrToLval, base);
  assert_flog(base->isA(TLval), "Unexpected mbase: {}", *base->inst());
  gen(env, StMBase, base);
}

/*
 * Get a pointer to the "tvTempBase" field used to materialize values that
 * are created on the fly during a member operation sequence.
 */
SSATmp* tvTempBasePtr(IRGS& env) {
  return gen(env, LdMIStateTempBaseAddr);
}

SSATmp* propTvRefPtr(IRGS& env, SSATmp* base, const SSATmp* key) {
  return prop_ignores_tvref(env, base, key)
    ? cns(env, TNullptr)
    : tvTempBasePtr(env);
}

SSATmp* ptrToUninit(IRGS& env) {
  return cns(env, Type::cns(&immutable_uninit_base, TLvalToConst));
}

//////////////////////////////////////////////////////////////////////
// Intermediate ops

Optional<PropInfo>
getCurrentPropertyOffset(IRGS& env, SSATmp* base, Type keyType,
                         bool ignoreLateInit) {
  if (!(base->isA(TObj) && base->type().clsSpec())) return std::nullopt;
  auto const baseCls = base->type().clsSpec().cls();
  auto const info = getPropertyOffset(env, baseCls, keyType, ignoreLateInit);
  if (!info) return info;

  env.irb->constrainValue(base, GuardConstraint(info->propClass));
  return info;
}

SSATmp* emitPropSpecialized(
  IRGS& env,
  SSATmp* base,
  SSATmp* key,
  MOpMode mode,
  const PropInfo& propInfo
) {
  assertx(base->isA(TObj));

  assertx(IMPLIES(propInfo.lateInitCheck, propInfo.lateInit));
  emitModuleBoundaryCheckKnown(env, propInfo.objProp);
  if (!propInfo.lateInitCheck &&
      (propInfo.lateInit ||
       mode != MOpMode::Warn ||
       !propInfo.knownType.maybe(TUninit))) {
    return ldPropAddr(
      env,
      base,
      nullptr,
      propInfo.propClass,
      propInfo.slot,
      propInfo.knownType
    );
  }

  return cond(
    env,
    [&] (Block* taken) {
      return ldPropAddr(
        env,
        base,
        taken,
        propInfo.propClass,
        propInfo.slot,
        propInfo.knownType
      );
    },
    [&] (SSATmp* addr) { return addr; },
    [&] {
      hint(env, Block::Hint::Unlikely);
      if (propInfo.lateInit) {
        gen(
          env,
          ThrowLateInitPropError,
          cns(env, propInfo.propClass),
          key,
          cns(env, false)
        );
      } else {
        gen(env, ThrowUndefPropException, base, key);
      }
      return cns(env, TBottom);
    }
  );
}

//////////////////////////////////////////////////////////////////////
// "Simple op" handlers.

void checkCollectionBounds(IRGS& env, SSATmp* base,
                           SSATmp* idx, SSATmp* limit) {
  assertx(base->isA(TObj));

  ifThen(
    env,
    [&](Block* taken) {
      auto ok = gen(env, CheckRange, idx, limit);
      gen(env, JmpZero, taken, ok);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowOutOfBounds, base, idx);
    }
  );
}

template<class Finish>
SSATmp* emitVecGet(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  assertx(base->isA(TVec));

  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  ifThen(
    env,
    [&](Block* taken) {
      gen(env, CheckVecBounds, taken, base, key);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowOutOfBounds, base, key);
    }
  );

  auto finishMe = [&](SSATmp* elem) {
    gen(env, IncRef, elem);
    return elem;
  };

  auto const elem = gen(env, LdVecElem, base, key);
  auto const pelem = profiledType(env, elem, [&] { finish(finishMe(elem)); });
  return finishMe(pelem);
}

template<class Finish>
SSATmp* emitVecQuietGet(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  assertx(base->isA(TVec));

  if (key->isA(TStr)) return cns(env, TInitNull);
  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const elem = cond(
    env,
    [&] (Block* taken) {
      gen(env, CheckVecBounds, taken, base, key);
    },
    [&] { return gen(env, LdVecElem, base, key); },
    [&] { return cns(env, TInitNull); }
  );

  auto finishMe = [&](SSATmp* element) {
    gen(env, IncRef, element);
    return element;
  };

  auto const pelem = profiledType(env, elem, [&] { finish(finishMe(elem)); });
  return finishMe(pelem);
}

template<class Finish>
SSATmp* emitDictKeysetGet(IRGS& env, SSATmp* base, SSATmp* key,
                          bool quiet, bool is_dict, Finish finish) {
  assertx(is_dict ? base->isA(TDict) : base->isA(TKeyset));

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto finishMe = [&](SSATmp* elem) {
    gen(env, IncRef, elem);
    return elem;
  };

  auto const mode = quiet ? MOpMode::None : MOpMode::Warn;
  auto const elem = profiledArrayAccess(
    env, base, key, mode,
    [&] (SSATmp* base, SSATmp* key, SSATmp* pos) {
      if (is_dict) return gen(env, DictGetK, base, key, pos);
      return key;
    },
    [&] (SSATmp* key) {
      if (quiet) return cns(env, TInitNull);
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowOutOfBounds, base, key);
      return cns(env, TBottom);
    },
    [&] (SSATmp* key, SizeHintData data) {
      return gen(
        env,
        is_dict
          ? (quiet ? DictGetQuiet : DictGet)
          : (quiet ? KeysetGetQuiet : KeysetGet),
        base,
        key
      );
    },
    [&] (SSATmp* elem) { finish(finishMe(elem)); }
  );
  auto const pelem = profiledType(env, elem, [&] { finish(finishMe(elem)); });
  return finishMe(pelem);
}

template<class Finish>
SSATmp* emitVectorGet(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  auto const size = gen(env, LdVectorSize, base);
  checkCollectionBounds(env, base, key, size);

  auto vec = gen(env, LdColVec, base);
  auto result = gen(env, LdVecElem, vec, key);

  auto const profres = profiledType(env, result, [&] {
    gen(env, IncRef, result);
    finish(result);
  });
  gen(env, IncRef, profres);
  return profres;
}

template<class Finish>
SSATmp* emitPairGet(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  assertx(key->isA(TInt));

  auto const idx = [&] {
    if (key->hasConstVal()) {
      auto keyVal = key->intVal();
      if (keyVal < 0 || keyVal > 1) PUNT(emitPairGet);
    } else {
      checkCollectionBounds(env, base, key, cns(env, 2));
    }
    return key;
  }();

  auto const result = gen(env, LdPairElem, base, idx);
  auto const profres = profiledType(env, result, [&] {
    gen(env, IncRef, result);
    finish(result);
  });
  gen(env, IncRef, profres);
  return profres;
}

SSATmp* emitVecIsset(IRGS& env, SSATmp* base, SSATmp* key) {
  assertx(base->isA(TVec));

  if (key->isA(TStr)) return cns(env, false);
  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  return cond(
    env,
    [&] (Block* taken) {
      gen(env, CheckVecBounds, taken, base, key);
    },
    [&] {
      auto const elem = gen(env, LdVecElem, base, key);
      return isType(env, TNonNull, elem);
    },
    [&] { return cns(env, false); }
  );
}

template <typename Finish>
SSATmp* emitDictIsset(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  assertx(base->isA(TDict));
  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  return profiledArrayAccess(
    env, base, key, MOpMode::Warn,
    [&] (SSATmp* dict, SSATmp* key, SSATmp* pos) {
      auto const elem = gen(env, DictGetK, dict, key, pos);
      return isType(env, TNonNull, elem);
    },
    [&] (SSATmp*) { return cns(env, false); },
    [&] (SSATmp* key, SizeHintData) { return gen(env, DictIsset, base, key); },
    finish
  );
}

template <typename Finish>
SSATmp* emitKeysetIsset(IRGS& env, SSATmp* base, SSATmp* key, Finish finish) {
  assertx(base->isA(TKeyset));
  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  return profiledArrayAccess(
    env, base, key, MOpMode::Warn,
    [&] (SSATmp*, SSATmp*, SSATmp*) { return cns(env, true); },
    [&] (SSATmp*) { return cns(env, false); },
    [&] (SSATmp* key, SizeHintData) { return gen(env, KeysetIsset, base, key); },
    finish
  );
}

//////////////////////////////////////////////////////////////////////

SSATmp* emitIncDecProp(IRGS& env, IncDecOp op, SSATmp* base, SSATmp* key) {
  auto const propInfo =
    getCurrentPropertyOffset(env, base, key->type(), false);

  if (propInfo && !propInfo->isConst) {
    // Special case for when the property is known to be an int.
    if (propInfo->knownType <= TInt) {
      base = emitPropSpecialized(env, base, key, MOpMode::Define, *propInfo);
      auto const prop = gen(env, LdMem, TInt, base);
      auto const result = incDec(env, op, prop);
      // No need for a property type-check because the input is an Int and the
      // result is always an Int.
      assertx(result != nullptr);
      assertx(result->isA(TInt));
      gen(env, StMem, base, result);
      return isPre(op) ? result : prop;
    }
  }

  return gen(env, IncDecProp, IncDecData{op}, base, key);
}

template<class Finish>
SSATmp* emitCGetElem(IRGS& env, SSATmp* base, SSATmp* key,
                     MOpMode mode, SimpleOp simpleOp, Finish finish) {
  assertx(mode == MOpMode::Warn || mode == MOpMode::InOut);
  switch (simpleOp) {
    case SimpleOp::Vec:
      return emitVecGet(env, base, key, finish);
    case SimpleOp::Dict:
      return emitDictKeysetGet(env, base, key, false, true, finish);
    case SimpleOp::Keyset:
      return emitDictKeysetGet(env, base, key, false, false, finish);
    case SimpleOp::String:
      assertx(mode != MOpMode::InOut);
      return gen(env, StringGet, base, key);
    case SimpleOp::Vector:
      assertx(mode != MOpMode::InOut);
      return emitVectorGet(env, base, key, finish);
    case SimpleOp::Pair:
      assertx(mode != MOpMode::InOut);
      return emitPairGet(env, base, key, finish);
    case SimpleOp::Map:
      assertx(mode != MOpMode::InOut);
      return gen(env, MapGet, base, key);
    case SimpleOp::None:
      return gen(env, CGetElem, MOpModeData{mode}, base, key);
  }
  always_assert(false);
}

template<class Finish>
SSATmp* emitCGetElemQuiet(IRGS& env, SSATmp* base, SSATmp* key,
                          MOpMode mode, SimpleOp simpleOp, Finish finish) {
  assertx(mode != MOpMode::Warn);
  switch (simpleOp) {
    case SimpleOp::Vec:
      return emitVecQuietGet(env, base, key, finish);
    case SimpleOp::Dict:
      return emitDictKeysetGet(env, base, key, true, true, finish);
    case SimpleOp::Keyset:
      return emitDictKeysetGet(env, base, key, true, false, finish);
    case SimpleOp::String:
    case SimpleOp::Vector:
    case SimpleOp::Pair:
    case SimpleOp::Map:
      assertx(mode != MOpMode::InOut);
      [[fallthrough]];
    case SimpleOp::None:
      return gen(env, CGetElem, MOpModeData{mode}, base, key);
  }
  always_assert(false);
}

template <typename Finish>
SSATmp* emitIssetElem(IRGS& env, SSATmp* base, SSATmp* key,
                      SimpleOp simpleOp, Finish finish) {
  switch (simpleOp) {
  case SimpleOp::Vec:
    return emitVecIsset(env, base, key);
  case SimpleOp::Dict:
    return emitDictIsset(env, base, key, finish);
  case SimpleOp::Keyset:
    return emitKeysetIsset(env, base, key, finish);
  case SimpleOp::String:
    return gen(env, StringIsset, base, key);
  case SimpleOp::Vector:
    return gen(env, VectorIsset, base, key);
  case SimpleOp::Pair:
    return gen(env, PairIsset, base, key);
  case SimpleOp::Map:
    return gen(env, MapIsset, base, key);
  case SimpleOp::None:
    return gen(env, IssetElem, base, key);
  }

  always_assert(false);
}

/*
 * Determine which simple collection op to use for the given base and key
 * types.
 */
SimpleOp simpleCollectionOp(Type baseType, Type keyType, bool readInst,
                            bool inOut) {
  if (inOut && !(baseType <= TArrLike)) return SimpleOp::None;

  if (baseType <= TVec)    return SimpleOp::Vec;
  if (baseType <= TDict)   return SimpleOp::Dict;
  if (baseType <= TKeyset) return SimpleOp::Keyset;

  if (baseType <= TStr) {
    if (keyType <= TInt) {
      // Don't bother with SetM on strings, because profile data shows it
      // basically never happens.
      if (readInst) return SimpleOp::String;
    }
  } else if (baseType < TObj && baseType.clsSpec()) {
    const Class* klass = baseType.clsSpec().cls();
    auto const isVector = collections::isType(klass, CollectionType::Vector);
    auto const isImmVector =
      collections::isType(klass, CollectionType::ImmVector);
    auto const isPair   = collections::isType(klass, CollectionType::Pair);
    auto const isMap    = collections::isType(klass, CollectionType::Map);
    auto const isImmMap = collections::isType(klass, CollectionType::ImmMap);

    if (isVector || isPair || (isImmVector && readInst)) {
      if (keyType <= TInt) {
        // We don't specialize setting pair elements.
        if (isPair && !readInst) return SimpleOp::None;

        return (isImmVector || isVector) ? SimpleOp::Vector : SimpleOp::Pair;
      }
    } else if ((isMap || (isImmMap && readInst)) &&
               (keyType <= TInt || keyType <= TStr)) {
      return SimpleOp::Map;
    }
  }

  return SimpleOp::None;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * We'd like to use value-type access rather than ref-type access for "lookup"
 * member-op sequences (that is, ones with mode Warn, None, or InOut).
 *
 * This helper converts value-type bases into the refs used by MInstrState.
 * store-elim will usually end up moving these ops off of the main trace.
 */
SSATmp* baseValueToLval(IRGS& env, SSATmp* base) {
  assertx(base->isA(TCell));
  auto const temp = tvTempBasePtr(env);
  gen(env, StMem, temp, base);
  auto const lval = gen(env, ConvPtrToLval, temp);
  return lval;
}

/*
 * Load and fully unpack---i.e., dereference---the member base.
 *
 * Also constrains the base value (and, if applicable, its inner value) to
 * DataTypeSpecific; it's expected that the caller only uses extractBase() when
 * it has a certain useful type.
 */
SSATmp* extractBase(IRGS& env) {
  env.irb->constrainLocation(Location::MBase{}, DataTypeSpecific);
  auto const base = gen(env, LdMem, env.irb->fs().mbase().type, ldMBase(env));
  env.irb->constrainValue(base, DataTypeSpecific);
  return base;
}

/*
 * Constrain the member base's types.
 *
 * This does all the type constraint work of extractBase().  It's used in
 * situations where we need to know the fully-unpacked base's type, but only
 * need the base pointer.
 */
void constrainBase(IRGS& env) { extractBase(env); }

///////////////////////////////////////////////////////////////////////////////

SSATmp* propGenericImpl(IRGS& env, MOpMode mode, SSATmp* base, SSATmp* key,
                          bool nullsafe, ReadonlyOp rop) {
  auto const tvRef = propTvRefPtr(env, base, key);
  if (nullsafe) {
    return gen(env, PropQ, PropData { mode, rop }, base, key, tvRef);
  }

  auto const op = (mode == MOpMode::Define) ? PropDX : PropX;
  return gen(env, op, PropData { mode, rop }, base, key, tvRef);
}

SSATmp* propImpl(IRGS& env, MOpMode mode, SSATmp* key,
                 bool nullsafe, ReadonlyOp op) {
  auto const base = extractBase(env);

  if (mode == MOpMode::Unset && !base->type().maybe(TObj)) {
    return ptrToInitNull(env);
  }

  auto const propInfo =
    getCurrentPropertyOffset(env, base, key->type(), false);
  if (!propInfo || propInfo->isConst || mode == MOpMode::Unset) {
    return propGenericImpl(env, mode, base, key, nullsafe, op);
  }

  auto data = ClassData { propInfo->propClass };
  if (propInfo->readOnly && op == ReadonlyOp::Mutable) {
    if ( mode == MOpMode::Unset || mode == MOpMode::Define) {
      gen(env, ThrowMustBeMutableException, data, key);
    } else {
      gen(env, ThrowMustBeEnclosedInReadonly, data, key);
    }
    return cns(env, TBottom);
  } else if (!propInfo->readOnly && op == ReadonlyOp::CheckROCOW) {
    gen(env, ThrowMustBeReadonlyException, data, key);
    return cns(env, TBottom);
  }

  auto const propPtr = emitPropSpecialized(
    env,
    base,
    key,
    mode,
    *propInfo
  );

  if (op == ReadonlyOp::CheckROCOW ||
    (op == ReadonlyOp::CheckMutROCOW && propInfo->readOnly)) {
    checkPropDimForReadonly(env, propPtr, propInfo->propClass, key);
  }

  return propPtr;
}

///////////////////////////////////////////////////////////////////////////////

namespace {

SSATmp* cowArray(IRGS& env, SSATmp* base) {
  assertx(base->isA(TArrLike));

  static const StaticString s_ArrayCOW{"ArrayCOW"};
  auto const profile = TargetProfile<COWProfile>{
    env.context,
    env.irb->curMarker(),
    s_ArrayCOW.get()
  };

  if (profile.profiling()) {
    gen(env, ProfileArrayCOW, RDSHandleData { profile.handle() }, base);
  }

  using R = COWProfile::Result;
  auto bias = R::None;
  if (profile.optimizing()) bias = profile.data().choose();

  return cond(
    env,
    [&] (Block* taken) { return gen(env, CheckArrayCOW, taken, base); },
    [&] (SSATmp* single) {
      gen(env, StMemMeta, ldMBase(env), single);
      return single;
    },
    [&] {
      if (bias >= R::RarelyCOW) hint(env, Block::Hint::Unlikely);
      decRefNZ(env, base);
      auto const copy = gen(env, CopyArray, base);
      gen(env, StMem, ldMBase(env), copy);
      return copy;
    }
  );
}

}

///////////////////////////////////////////////////////////////////////////////

SSATmp* vecElemImpl(IRGS& env, MOpMode mode, Type baseType, SSATmp* key) {
  assertx(baseType <= TVec);
  assertx(key->isA(TInt) || key->isA(TStr) ||
          !key->type().maybe(TInt | TStr));

  auto const warn = mode == MOpMode::Warn || mode == MOpMode::InOut;
  auto const unset = mode == MOpMode::Unset;
  auto const define = mode == MOpMode::Define;
  auto const quiet = !warn && !unset && !define;

  if (key->isA(TStr) && (unset || quiet)) return ptrToInitNull(env);
  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, extractBase(env), key);
    return cns(env, TBottom);
  }

  auto const base = extractBase(env);
  return cond(
    env,
    [&] (Block* taken) { gen(env, CheckVecBounds, taken, base, key); },
    [&] {
      auto const vec = [&] {
        if (define || unset) return cowArray(env, base);
        return base;
      }();
      return gen(env, LdVecElemAddr, vec, key, base);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      if (define || warn) {
        gen(env, ThrowOutOfBounds, base, key);
        return cns(env, TBottom);
      }
      return ptrToInitNull(env);
    }
  );
}

template <typename Finish>
SSATmp* dictElemImpl(IRGS& env, MOpMode mode, Type baseType, SSATmp* key,
                     Finish finish) {
  assertx(baseType <= TDict);

  auto const unset = mode == MOpMode::Unset;
  auto const define = mode == MOpMode::Define;

  auto const base = extractBase(env);

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  if (!define && !unset) {
    return profiledArrayAccess(
      env, base, key, mode,
      [&] (SSATmp* dict, SSATmp* key, SSATmp* pos) {
        return gen(env, ElemDictK, dict, key, pos, dict);
      },
      [&] (SSATmp* key) {
        if (mode == MOpMode::None) return ptrToInitNull(env);
        assertx(mode == MOpMode::Warn || mode == MOpMode::InOut);
        gen(env, ThrowOutOfBounds, base, key);
        return cns(env, TBottom);
      },
      [&] (SSATmp* key, SizeHintData) {
        assertx(
          mode == MOpMode::Warn ||
          mode == MOpMode::None ||
          mode == MOpMode::InOut
        );
        auto const op = mode == MOpMode::None ? DictGetQuiet : DictGet;
        return baseValueToLval(env, gen(env, op, base, key));
      },
      finish
    );
  }

  return profiledArraySet(
    env, ldMBase(env), base, key, mode,
    [&] (SSATmp* dict, SSATmp* key, SSATmp* pos, SSATmp* base, Block*) {
      return gen(env, ElemDictK, dict, key, pos, base);
    },
    [&] (SSATmp* key, SizeHintData) {
      return gen(
        env,
        unset ? ElemDictU : ElemDictD,
        baseType,
        ldMBase(env),
        key
      );
    },
    finish
  );
}

template <typename Finish>
SSATmp* keysetElemImpl(IRGS& env, MOpMode mode, Type baseType,
                       SSATmp* key, Finish finish) {
  assertx(baseType <= TKeyset);

  auto const unset = mode == MOpMode::Unset;
  auto const define = mode == MOpMode::Define;

  auto const base = extractBase(env);

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  if (define || unset) {
    gen(
      env,
      ThrowInvalidOperation,
      cns(env, s_InvalidKeysetOperationMsg.get())
    );
    return cns(env, TBottom);
  }

  return profiledArrayAccess(
    env, base, key, mode,
    [&] (SSATmp*, SSATmp* key, SSATmp*) {
      return baseValueToLval(env, key);
    },
    [&] (SSATmp* key) {
      if (mode == MOpMode::None) return ptrToInitNull(env);
      assertx(mode == MOpMode::Warn || mode == MOpMode::InOut);
      gen(env, ThrowOutOfBounds, base, key);
      return cns(env, TBottom);
    },
    [&] (SSATmp* key, SizeHintData) {
      assertx(
        mode == MOpMode::Warn ||
        mode == MOpMode::None ||
        mode == MOpMode::InOut
      );
      auto const op = mode == MOpMode::None ? KeysetGetQuiet : KeysetGet;
      return baseValueToLval(env, gen(env, op, base, key));
    },
    finish
  );
}

const StaticString s_OP_NOT_SUPPORTED_STRING(Strings::OP_NOT_SUPPORTED_STRING);

template <typename Finish>
SSATmp* elemImpl(IRGS& env, MOpMode mode, SSATmp* key, Finish finish) {
  auto const unset = mode == MOpMode::Unset;
  auto const define = mode == MOpMode::Define;

  auto const baseType = env.irb->fs().mbase().type;

  if (baseType <= TDict) {
    return dictElemImpl(env, mode, baseType, key, finish);
  }
  if (baseType <= TVec)    return vecElemImpl(env, mode, baseType, key);
  if (baseType <= TKeyset) {
    return keysetElemImpl(env, mode, baseType, key, finish);
  }

  if (unset) {
    constrainBase(env);
    if (baseType <= TStr) {
      gen(env, RaiseError, cns(env, s_OP_NOT_SUPPORTED_STRING.get()));
      return cns(env, TBottom);
    }
    if (!baseType.maybe(TClsMeth | TArrLike | TObj)) return ptrToUninit(env);
  }

  auto const data = MOpModeData { mode };
  if (define || unset) {
    auto const op = define ? ElemDX : ElemUX;
    return gen(env, op, data, ldMBase(env), key);
  }
  auto const value = gen(env, ElemX, data, extractBase(env), key);
  return baseValueToLval(env, value);
}

template<class Finish>
SSATmp* cGetPropImpl(IRGS& env, SSATmp* base, SSATmp* key,
                     MOpMode mode, bool nullsafe, Finish finish, ReadonlyOp op) {
  auto const propInfo =
    getCurrentPropertyOffset(env, base, key->type(), false);
  if (propInfo) {
    if (propInfo->readOnly && op == ReadonlyOp::Mutable) {
      auto data = ClassData { propInfo->propClass };
      gen(env, ThrowMustBeEnclosedInReadonly, data, key);
      return cns(env, TBottom);
    }
    auto const propAddr =
      emitPropSpecialized(env, base, key, mode, *propInfo);
    auto const result = gen(env, LdMem, propInfo->knownType, propAddr);
    auto const profres = profiledType(env, result, [&] {
      gen(env, IncRef, result);
      finish(result);
    });
    gen(env, IncRef, profres);
    return profres;
  }

  if (nullsafe) return gen(env, CGetPropQ, PropData{mode, op}, base, key);
  return gen(env, CGetProp, PropData{mode, op}, base, key);
}

Block* makeCatchSet(IRGS& env, uint32_t nDiscard) {
  return makeCatchBlock(env, [&]{
    ifElse(
      env,
      [&] (Block* taken) {
        gen(env, UnwindCheckSideExit, taken, fp(env), sp(env));
      },
      [&] {
        // Side-exit from a catch block due to an InvalidSetMException.
        // Note: MIState was already killed by BeginCatch.
        hint(env, Block::Hint::Unused);

        // For consistency with the interpreter, decref the rhs before we decref
        // the stack inputs, and decref the ratchet storage after the stack
        // inputs.
        popDecRef(env, DecRefProfileId::Default, DataTypeGeneric);
        for (int i = 0; i < nDiscard; ++i) {
          popDecRef(env, static_cast<DecRefProfileId>(i), DataTypeGeneric);
        }
        push(env, gen(env, LdUnwinderValue, TCell));
        gen(env, Jmp, makeExit(env, nextSrcKey(env)));
      }
    );
  });
}

SSATmp* setPropImpl(IRGS& env, uint32_t nDiscard, SSATmp* key, ReadonlyOp op) {
  auto const value = topC(env, BCSPRelOffset{0}, DataTypeGeneric);

  auto const base = extractBase(env);

  auto const mode = MOpMode::Define;
  auto const propInfo =
    getCurrentPropertyOffset(env, base, key->type(), true);

  if (propInfo && !propInfo->isConst) {
    if (!propInfo->readOnly && op == ReadonlyOp::Readonly) {
      auto data = ClassData { propInfo->propClass };
      gen(env, ThrowMustBeReadonlyException, data, key);
      return cns(env, TBottom);
    }

    auto const propPtr = emitPropSpecialized(
      env,
      base,
      key,
      mode,
      *propInfo
    );

    SSATmp* newVal = verifyPropType(
      env,
      gen(env, LdObjClass, base),
      propInfo->typeConstraints,
      propInfo->slot,
      value,
      key,
      false
    );

    auto const oldVal = gen(env, LdMem, propInfo->knownType, propPtr);
    gen(env, IncRef, newVal);
    gen(env, StMem, propPtr, newVal);
    decRef(env, oldVal);
  } else {
    gen(
      env,
      SetProp,
      makeCatchSet(env, nDiscard),
      ReadonlyData{op},
      base,
      key,
      value
    );
  }

  return value;
}

void handleStrTestResult(IRGS& env, uint32_t nDiscard, SSATmp* strTestResult) {
  // We expected SetElem's base to not be a Str but might be wrong. Make an
  // exit trace to side exit to the next instruction, replacing our guess with
  // the correct stack output.
  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, CheckNullptr, taken, strTestResult);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      auto const str = gen(env, AssertNonNull, strTestResult);
      popDecRef(env, DecRefProfileId::Default, DataTypeGeneric);
      for (int i = 0; i < nDiscard; ++i) {
        popDecRef(env, static_cast<DecRefProfileId>(i));
      }
      push(env, str);
      gen(env, FinishMemberOp);
      gen(env, Jmp, makeExit(env, nextSrcKey(env)));
    }
  );
}

template <typename Finish>
SSATmp* emitArrayLikeSet(IRGS& env, SSATmp* key, SSATmp* value, Finish finish) {
  auto const baseType = env.irb->fs().mbase().type;
  auto const base = extractBase(env);
  assertx(baseType <= TArrLike);

  auto const isVec = baseType <= TVec;
  auto const isDict = baseType <= TDict;
  auto const isKeyset = baseType <= TKeyset;
  assertx(isVec || isDict || isKeyset);

  if ((isVec && !key->isA(TInt)) ||
      (isDict && !key->isA(TInt | TStr))) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  if (isKeyset) {
    gen(env, ThrowInvalidOperation,
        cns(env, s_InvalidKeysetOperationMsg.get()));
    return cns(env, TBottom);
  }

  auto const finishMe = [&] {
    gen(env, IncRef, value);
    return value;
  };

  if (!isVec) {
    profiledArraySet(
      env, ldMBase(env), base, key, MOpMode::Define,
      [&] (SSATmp* dict, SSATmp* key, SSATmp* pos, SSATmp* base, Block*) {
        auto const elem = gen(env, ElemDictK, dict, key, pos, base);
        decRef(
          env,
          gen(env, LdMem, TInitCell, elem),
          DecRefProfileId::ProfiledArraySet
        );
        gen(env, StMem, elem, value);
        return cns(env, TBottom);
      },
      [&] (SSATmp* key, SizeHintData) {
        auto const newArr = gen(env, DictSet, base, key, value);
        gen(env, StMem, ldMBase(env), newArr);
        return cns(env, TBottom);
      },
      [&] (SSATmp*) { finish(finishMe()); }
    );
  } else {
    ifThenElse(
      env,
      [&] (Block* taken) { gen(env, CheckVecBounds, taken, base, key); },
      [&] {
        auto const vec = cowArray(env, base);
        auto const elem = gen(env, LdVecElemAddr, vec, key, base);
        decRef(
            env,
            gen(env, LdMem, TInitCell, elem),
            DecRefProfileId::ProfiledArraySet
        );
        gen(env, StMem, elem, value);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, ThrowOutOfBounds, base, key);
      }
    );
  }

  return finishMe();
}

void setNewElemVecImpl(IRGS& env, uint32_t nDiscard,
                       SSATmp* value) {
  static const StaticString s_ArrayCOW{"NewElemVecCOW"};
  auto const profile = TargetProfile<COWProfile>{
    env.context,
    env.irb->curMarker(),
    s_ArrayCOW.get()
  };

  auto const base = extractBase(env);
  assertx(base->type() <= TVec);

  if (profile.profiling()) {
    gen(env, ProfileArrayCOW, RDSHandleData { profile.handle() }, base);
  }

  auto emitAddNewElemVec = [&]{
    auto newBase = gen(env, AddNewElemVec, base, value);
    gen(env, StMem, ldMBase(env), newBase);
  };

  using R = COWProfile::Result;
  auto bias = R::None;
  if (profile.optimizing()) bias = profile.data().choose();

  if (bias <= R::UsuallyCOW) {
    emitAddNewElemVec();
  } else {
    ifThen(
      env,
      [&](Block* taken) {
        auto const base = extractBase(env);
        auto const vec = gen(env, CheckArrayCOW, taken, base);
        auto const offset = gen(env, ReserveVecNewElem, taken, vec);
        auto const elemPtr = gen(env, LdVecElemAddr, vec, offset, base);
        gen(env, StMem, elemPtr, value);
      },
      [&] {
        emitAddNewElemVec();
      }
    );
  }

  gen(env, IncRef, value);
}

SSATmp* setNewElemImpl(IRGS& env, uint32_t nDiscard) {
  auto const baseType = env.irb->fs().mbase().type;
  auto value = topC(env, BCSPRelOffset{0}, DataTypeGeneric);

  if (baseType <= TVec) {
    setNewElemVecImpl(env, nDiscard, value);
  } else if (baseType <= TKeyset) {
    auto const base = extractBase(env);
    value = convertClassKey(env, value);
    if (!value->type().maybe(TInt | TStr)) {
      gen(env, ThrowInvalidArrayKey, base, value);
    } else {
      gen(env, IncRef, value);
      auto const newBase = gen(env, AddNewElemKeyset, base, value);
      gen(env, StMem, ldMBase(env), newBase);
    }
  } else {
    // We load the member base pointer before calling makeCatchSet() to avoid
    // mismatched in-states for any catch block edges we emit later on.
    auto const basePtr = ldMBase(env);
    gen(env, SetNewElem, makeCatchSet(env, nDiscard), basePtr, value);
  }
  return value;
}

template <typename Finish>
SSATmp* setElemImpl(IRGS& env, uint32_t nDiscard, SSATmp* key, Finish finish) {
  auto value = topC(env, BCSPRelOffset{0}, DataTypeGeneric);

  auto const baseType = env.irb->fs().mbase().type;
  auto const simpleOp = simpleCollectionOp(baseType, key->type(), false, false);

  if (auto gc = simpleOpConstraint(simpleOp)) {
    env.irb->constrainLocation(Location::MBase{}, *gc);
  }

  switch (simpleOp) {
    case SimpleOp::String:
      always_assert(false && "Bad SimpleOp in setElemImpl");
      break;

    case SimpleOp::Vector:
      gen(env, VectorSet, extractBase(env), key, value);
      gen(env, IncRef, value);
      break;

    case SimpleOp::Map:
      gen(env, MapSet, extractBase(env), key, value);
      gen(env, IncRef, value);
      break;

    case SimpleOp::Vec:
    case SimpleOp::Dict:
    case SimpleOp::Keyset:
      return emitArrayLikeSet(env, key, value, finish);

    case SimpleOp::Pair:
    case SimpleOp::None:
      // We load the member base pointer before calling makeCatchSet() to avoid
      // mismatched in-states for any catch block edges we emit later on.
      auto const basePtr = ldMBase(env);
      auto const result = gen(env, SetElem, baseType,
                              makeCatchSet(env, nDiscard),
                              basePtr, key, value);
      auto const t = result->type();
      if (!baseType.maybe(TStr) || t == TNullptr) {
        // Base is not a string. Result is always value.
      } else if (t == TStaticStr) {
        // Base is a string. Stack result is a new string so we're responsible
        // for decreffing value.
        decRef(env, value);
        value = result;
      } else {
        assertx(t == (TStaticStr | TNullptr));
        // Base might be a string. Emit a check to verify the result before
        // returning the optimistic result.
        handleStrTestResult(env, nDiscard, result);
      }
      break;
  }

  return value;
}

// Dynamic property access ($foo->$bar) is logged in the interpreter; when that
// logging is enabled, interpret these member ops rather than translating them.
bool dynPropAccessNeedsInterp(MemberKey mk) {
  return Cfg::Eval::LogDynamicPropAccessSampleRate != 0 &&
         (mk.mcode == MPC || mk.mcode == MPL);
}

SSATmp* memberKey(IRGS& env, MemberKey mk) {
  // Logged dynamic-prop accesses are interpreted at each emitter's top, never here.
  assertx(!dynPropAccessNeedsInterp(mk));
  auto const res = [&] () -> SSATmp* {
    switch (mk.mcode) {
      case MW:
        return nullptr;
      case MEL: case MPL:
        return ldLocWarn(env, mk.local, DataTypeSpecific);
      case MEC: case MPC:
        return topC(env, BCSPRelOffset{int32_t(mk.iva)});
      case MEI:
        return cns(env, mk.int64);
      case MET: case MPT: case MQT:
        return cns(env, mk.litstr);
    }
    not_reached();
  }();
  if (!res) return nullptr;

  if (!res->type().isKnownDataType()) PUNT(MInstr-KeyNotKnown);
  return convertClassKey(env, res);
}

}

//////////////////////////////////////////////////////////////////////

SSATmp* ldPropAddr(IRGS& env, SSATmp* obj, Block* taken,
                   const Class* cls, Slot slot, const Type& type) {
  assertx(type <= TCell);
  assertx(cls != nullptr);
  assertx(slot != kInvalidSlot);

  auto const data = IndexData { cls->propSlotToIndex(slot) };

  auto const TSerializedAsAPCTypedValue =
      TUninit | TInitNull | TBool | TInt | TDbl | TStr | TLazyCls;
  auto const maybe_lazy = !(type <= TSerializedAsAPCTypedValue) &&
                          cls->mayUseLazyAPCDeserialization();

  if (maybe_lazy) gen(env, DeserializeLazyProp, data, obj);

  return taken
    ? gen(env, LdInitPropAddr, taken, data, type & TInitCell, obj)
    : gen(env, LdPropAddr, data, type, obj);
}

SSATmp* ldClosureArg(IRGS& env, SSATmp* obj, const Class* cls, Slot slot, const Type& type) {
  assertx(type <= TCell);
  assertx(cls != nullptr);
  assertx(slot != kInvalidSlot);

  auto const data = IndexData { cls->propSlotToIndex(slot) };
  return gen(env, LdClosureArg, data, type, obj);
}


SSATmp* ptrToInitNull(IRGS& env) {
  // Nothing is allowed to write anything to the init null variant, so this
  // inner type is always true.
  return cns(env, Type::cns(&immutable_null_base, TLvalToConst));
}

void mFinalImpl(IRGS& env, int32_t nDiscard, SSATmp* result) {
  for (auto i = 0; i < nDiscard; ++i) {
    popDecRef(env, static_cast<DecRefProfileId>(i));
  }
  if (result) push(env, result);
  gen(env, FinishMemberOp);
}

//////////////////////////////////////////////////////////////////////

namespace {

template <typename Finish>
SSATmp* incDecDictImpl(IRGS& env, IncDecOp op, SSATmp* base, SSATmp* key,
                       Finish finish) {
  assertx(base->isA(TDict));

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const lhsType = arrLikeElemType(
    base->type(),
    key->type(),
    curClass(env)
  ).first;

  if (!lhsType.maybe(TInt)) {
    return gen(
      env,
      IncDecElem,
      IncDecData{op},
      ldMBase(env),
      key
    );
  }

  return profiledArraySet(
    env, ldMBase(env), base, key, MOpMode::Define,
    [&] (SSATmp* dict, SSATmp* key, SSATmp* pos, SSATmp* base, Block* taken) {
      auto const elem = gen(env, ElemDictK, dict, key, pos, base);
      auto const oldVal = gen(
        env,
        CheckType,
        taken,
        TInt,
        gen(env, LdMem, TInitCell, elem)
      );
      auto const newVal = gen(
        env,
        isInc(op) ? AddInt : SubInt,
        oldVal,
        cns(env, 1)
      );
      gen(env, StMem, elem, newVal);
      return isPre(op) ? newVal : oldVal;
    },
    [&] (SSATmp* key, SizeHintData) {
      // We've written the COWed array back to the base already, so
      // set exception boundary to let frame state know that it's
      // okay.
      env.irb->exceptionStackBoundary();
      return gen(
        env,
        IncDecElem,
        IncDecData{op},
        ldMBase(env),
        key
      );
    },
    finish
  );
}

SSATmp* incDecVecImpl(IRGS& env, IncDecOp op, SSATmp* base, SSATmp* key) {
  assertx(base->isA(TVec));

  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const lhsType = arrLikeElemType(
    base->type(),
    key->type(),
    curClass(env)
  ).first;

  if (!lhsType.maybe(TInt)) {
    return gen(
      env,
      IncDecElem,
      IncDecData{op},
      ldMBase(env),
      key
    );
  }

  return cond(
    env,
    [&] (Block* taken) { gen(env, CheckVecBounds, taken, base, key); },
    [&] {
      auto const vec = cowArray(env, base);
      auto const elem = gen(env, LdVecElemAddr, vec, key, base);

      return cond(
        env,
        [&] (Block* taken) {
          return gen(
            env,
            CheckType,
            taken,
            TInt,
            gen(env, LdMem, TInitCell, elem)
          );
        },
        [&] (SSATmp* val) {
          assertx(val->isA(TInt));
          auto const newVal = gen(
            env,
            isInc(op) ? AddInt : SubInt,
            val,
            cns(env, 1)
          );
          gen(env, StMem, elem, newVal);
          return isPre(op) ? newVal : val;
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          // We've written the COWed array back to the base already,
          // so set exception boundary to let frame state know that
          // it's okay.
          env.irb->exceptionStackBoundary();
          return gen(
            env,
            IncDecElem,
            IncDecData{op},
            ldMBase(env),
            key
          );
        }
      );
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowOutOfBounds, base, key);
      return cns(env, TBottom);
    }
  );
}

template <typename Finish>
SSATmp* setOpDictImpl(IRGS& env, SetOpOp op, SSATmp* base, SSATmp* key,
                      SSATmp* rhs, Finish finish) {
  assertx(base->isA(TDict));

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const lhsType = arrLikeElemType(
    base->type(),
    key->type(),
    curClass(env)
  ).first;

  auto const type = simpleSetOpType(op);
  if (!type || !lhsType.maybe(*type) || !rhs->isA(*type)) {
    return gen(
      env,
      SetOpElem,
      SetOpData{op},
      ldMBase(env),
      key,
      rhs
    );
  }

  return profiledArraySet(
    env, ldMBase(env), base, key, MOpMode::Define,
    [&] (SSATmp* dict, SSATmp* key, SSATmp* pos, SSATmp* base, Block* taken) {
      auto const elem = gen(env, ElemDictK, dict, key, pos, base);
      auto const oldVal = gen(
        env,
        CheckType,
        taken,
        *type,
        gen(env, LdMem, TInitCell, elem)
      );
      auto const newVal = simpleSetOpAction(env, op, oldVal, rhs);
      gen(env, StMem, elem, newVal);
      return newVal;
    },
    [&] (SSATmp* key, SizeHintData) {
      // We've written the COWed array back to the base already, so
      // set exception boundary to let frame state know that it's
      // okay.
      env.irb->exceptionStackBoundary();
      return gen(
        env,
        SetOpElem,
        SetOpData{op},
        ldMBase(env),
        key,
        rhs
      );
    },
    finish
  );
}

SSATmp* setOpVecImpl(IRGS& env, SetOpOp op, SSATmp* base, SSATmp* key,
                     SSATmp* rhs) {
  assertx(base->isA(TVec));

  if (!key->isA(TInt)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const lhsType = arrLikeElemType(
    base->type(),
    key->type(),
    curClass(env)
  ).first;

  auto const type = simpleSetOpType(op);
  if (!type || !lhsType.maybe(*type) || !rhs->isA(*type)) {
    return gen(
      env,
      SetOpElem,
      SetOpData{op},
      ldMBase(env),
      key,
      rhs
    );
  }

  return cond(
    env,
    [&] (Block* taken) { gen(env, CheckVecBounds, taken, base, key); },
    [&] {
      auto const vec = cowArray(env, base);
      auto const elem = gen(env, LdVecElemAddr, vec, key, base);

      return cond(
        env,
        [&] (Block* taken) {
          auto const oldVal = gen(
            env,
            CheckType,
            taken,
            *type,
            gen(env, LdMem, TInitCell, elem)
          );
          auto const newVal = simpleSetOpAction(env, op, oldVal, rhs);
          gen(env, StMem, elem, newVal);
          return newVal;
        },
        [&] (SSATmp* ret) { return ret; },
        [&] {
          hint(env, Block::Hint::Unlikely);
          // We've written the COWed array back to the base already, so
          // set exception boundary to let frame state know that it's
          // okay.
          env.irb->exceptionStackBoundary();
          return gen(
            env,
            SetOpElem,
            SetOpData{op},
            ldMBase(env),
            key,
            rhs
          );
        }
      );
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowOutOfBounds, base, key);
      return cns(env, TBottom);
    }
  );
}

}

//////////////////////////////////////////////////////////////////////

Optional<Type> simpleSetOpType(SetOpOp op) {
  switch (op) {
    case SetOpOp::PlusEqual:
    case SetOpOp::MinusEqual:
    case SetOpOp::MulEqual:
    case SetOpOp::AndEqual:
    case SetOpOp::OrEqual:
    case SetOpOp::XorEqual:    return TInt;
    case SetOpOp::ConcatEqual: return TStr;
    case SetOpOp::DivEqual:
    case SetOpOp::ModEqual:
    case SetOpOp::PowEqual:
    case SetOpOp::SlEqual:
    case SetOpOp::SrEqual:
      return std::nullopt;
  }
  always_assert(false);
}

SSATmp* simpleSetOpAction(IRGS& env, SetOpOp op, SSATmp* lhs, SSATmp* rhs) {
  if (debug) {
    auto const type = simpleSetOpType(op);
    always_assert(type.has_value());
    always_assert(lhs->isA(*type));
    always_assert(rhs->isA(*type));
  }

  auto const opc = [&] {
    switch (op) {
      case SetOpOp::PlusEqual:   return AddInt;
      case SetOpOp::MinusEqual:  return SubInt;
      case SetOpOp::MulEqual:    return MulInt;
      case SetOpOp::AndEqual:    return AndInt;
      case SetOpOp::OrEqual:     return OrInt;
      case SetOpOp::XorEqual:    return XorInt;
      case SetOpOp::ConcatEqual: return ConcatStrStr;
      case SetOpOp::DivEqual:
      case SetOpOp::ModEqual:
      case SetOpOp::PowEqual:
      case SetOpOp::SlEqual:
      case SetOpOp::SrEqual:
        break;
    }
    always_assert(false);
  }();
  env.irb->exceptionStackBoundary();
  auto const ret = gen(env, opc, lhs, rhs);
  gen(env, IncRef, ret);
  return ret;
}

//////////////////////////////////////////////////////////////////////

bool propertyMayBeCountable(const Class::Prop& prop) {
  // We can't call `knownTypeForProp` for type-hints that involve objects
  // here because the classes they refer to may not yet be defined. We return
  // `true` for these cases. Doing so doesn't cause unnecessary pessimization,
  // because subtypes of Object are going to be countable anyway.
  if (prop.repoAuthType.name()) return true;

  // Countedness must not change based on class load order. In persistent mode a
  // referenced class may not be loaded yet, so only trust systemlib type
  // constraints.
  auto const systemLibOnly = Cfg::Eval::ForceAllPersistent;
  auto const type = knownTypeForProp(prop, nullptr, nullptr, true, systemLibOnly);
  return type.maybe(jit::TCounted);
}

//////////////////////////////////////////////////////////////////////

void emitBaseSC(IRGS& env,
                uint32_t propIdx,
                uint32_t clsIdx,
                MOpMode mode,
                ReadonlyOp op) {
  auto const cls = topC(env, BCSPRelOffset{safe_cast<int32_t>(clsIdx)});
  if (!cls->isA(TCls)) PUNT(BaseSC-NotClass);

  auto const name = top(env, BCSPRelOffset{safe_cast<int32_t>(propIdx)});
  if (!name->isA(TStr)) PUNT(BaseS-non-string-name);

  assertx(mode != MOpMode::InOut);
  auto const writeMode = mode == MOpMode::Define || mode == MOpMode::Unset;

  gen(env, StMROProp, cns(env, false));
  const LdClsPropOptions opts { op, true, false, writeMode };
  auto const lookup = ldClsPropAddr(env, cls, name, opts);
  stMBase(env, lookup.propPtr);
}

void emitBaseL(IRGS& env, NamedLocal loc, MOpMode mode, ReadonlyOp op) {
  auto base = ldLoc(env, loc.id, DataTypeGeneric);
  if (!base->type().isKnownDataType()) PUNT(unknown-BaseL);

  if (base->isA(TUninit) && mode == MOpMode::Warn) {
    auto const baseName = curFunc(env)->localVarName(loc.name);
    env.irb->constrainLocal(loc.id, DataTypeSpecific,
                            "emitBaseL: Uninit base local");
    gen(env, ThrowUninitLoc, cns(env, baseName));
  }

  if (op == ReadonlyOp::CheckROCOW || op == ReadonlyOp::CheckMutROCOW) {
    gen(env, StMROProp, cns(env, true));
    if (base->isA(TObj)) {
      auto const baseName = curFunc(env)->localVarName(loc.name);
      gen(env, ThrowLocalMustBeValueTypeException, cns(env, baseName));
    }
  } else {
    gen(env, StMROProp, cns(env, false));
  }

  stMBase(env, ldLocAddr(env, loc.id));
}

void emitBaseC(IRGS& env, uint32_t idx, MOpMode mode) {
  auto const bcOff = BCSPRelOffset{safe_cast<int32_t>(idx)};
  stMBase(env, ldStkAddr(env, bcOff));
  gen(env, StMROProp, cns(env, false));
}

void emitBaseH(IRGS& env) {
  if (!curClass(env)) return interpOne(env);

  auto const base = ldThis(env);
  stMBase(env, baseValueToLval(env, base));
  gen(env, StMROProp, cns(env, false));
}

void emitDim(IRGS& env, MOpMode mode, MemberKey mk) {
  if (dynPropAccessNeedsInterp(mk)) return interpOne(env);
  auto const key = memberKey(env, mk);

  auto const finish = [&] (SSATmp* base) {
    stMBase(env, base);
    if (mcodeIsElem(mk.mcode)) checkElemDimForReadonly(env);
  };

  auto const base = [&] {
    if (mcodeIsProp(mk.mcode)) return propImpl(env, mode, key, mk.mcode == MQT, mk.rop);
    if (mcodeIsElem(mk.mcode)) return elemImpl(env, mode, key, finish);
    PUNT(DimNewElem);
  }();
  finish(base);
}

void emitQueryM(IRGS& env, uint32_t nDiscard, QueryMOp query, MemberKey mk) {
  if (dynPropAccessNeedsInterp(mk)) return interpOne(env);
  if (mk.mcode == MW) PUNT(QueryNewElem);
  auto key = memberKey(env, mk);
  auto const baseType = env.irb->fs().mbase().type;
  if (baseType <= TClsMeth) {
    PUNT(QueryM_is_ClsMeth);
  }
  auto simpleOp = SimpleOp::None;

  if (mcodeIsElem(mk.mcode)) {
    simpleOp = simpleCollectionOp(baseType, key->type(), true,
                                  query == QueryMOp::InOut);

    if (simpleOp != SimpleOp::None) {
      if (auto const tc = simpleOpConstraint(simpleOp)) {
        env.irb->constrainLocation(Location::MBase{}, *tc);
      }
    }
  }

  auto const finish = [&] (SSATmp* result) {
    mFinalImpl(env, nDiscard, result);
  };

  auto const result = [&]() -> SSATmp* {
    switch (query) {
      case QueryMOp::InOut:
      case QueryMOp::CGet: {
        auto const mode = getQueryMOpMode(query);
        always_assert_flog(
          mode != MOpMode::InOut || mcodeIsElem(mk.mcode),
          "QueryOp InOut can only be used with Elem codes"
        );
        return mcodeIsProp(mk.mcode)
          ? cGetPropImpl(env, extractBase(env), key,
                         mode, mk.mcode == MQT, finish, mk.rop)
          : emitCGetElem(env, extractBase(env), key, mode, simpleOp, finish);
      }

      case QueryMOp::CGetQuiet: {
        auto const mode = getQueryMOpMode(query);
        return mcodeIsProp(mk.mcode)
          ? cGetPropImpl(
              env, extractBase(env), key, mode, mk.mcode == MQT,
              finish, mk.rop)
          : emitCGetElemQuiet(
              env, extractBase(env), key, mode, simpleOp, finish
            );
      }

      case QueryMOp::Isset:
        return mcodeIsProp(mk.mcode)
          ? gen(env, IssetProp, extractBase(env), key)
          : emitIssetElem(env, extractBase(env), key, simpleOp, finish);
    }
    not_reached();
  }();
  finish(result);
}

void emitSetM(IRGS& env, uint32_t nDiscard, MemberKey mk) {
  if (dynPropAccessNeedsInterp(mk)) return interpOne(env);
  auto const key = memberKey(env, mk);
  auto const baseType = env.irb->fs().mbase().type;
  if (baseType <= TClsMeth) {
    PUNT(SetM_is_ClsMeth);
  }

  auto const finish = [&] (SSATmp* result) {
    popC(env, DataTypeGeneric);
    mFinalImpl(env, nDiscard, result);
  };

  auto const result =
    mk.mcode == MW        ? setNewElemImpl(env, nDiscard) :
    mcodeIsElem(mk.mcode) ? setElemImpl(env, nDiscard, key, finish) :
                            setPropImpl(env, nDiscard, key, mk.rop);
  finish(result);
}

void emitSetRangeM(IRGS& env,
                   uint32_t nDiscard,
                   uint32_t size,
                   SetRangeOp op) {
  auto const count = gen(env, ConvTVToInt, topC(env));
  auto const src = topC(env, BCSPRelOffset{1});
  auto const offset = gen(env, ConvTVToInt, topC(env, BCSPRelOffset{2}));
  auto const reverse = op == SetRangeOp::Reverse;

  gen(
    env, reverse ? SetRangeRev : SetRange,
    ldMBase(env), offset, src, count, cns(env, size)
  );
  mFinalImpl(env, nDiscard + 3, nullptr);
}

void emitIncDecM(IRGS& env, uint32_t nDiscard, IncDecOp incDec, MemberKey mk) {
  if (dynPropAccessNeedsInterp(mk)) return interpOne(env);
  auto key = memberKey(env, mk);

  auto const finish = [&] (SSATmp* result) {
    mFinalImpl(env, nDiscard, result);
  };

  auto const result = [&] {
    if (mcodeIsProp(mk.mcode)) {
      return emitIncDecProp(env, incDec, extractBase(env), key);
    }
    if (mcodeIsElem(mk.mcode)) {
      auto const base = extractBase(env);
      if (base->isA(TDict)) {
        return incDecDictImpl(env, incDec, base, key, finish);
      }
      if (base->isA(TVec)) return incDecVecImpl(env, incDec, base, key);
      if (base->isA(TKeyset)) {
        gen(
          env,
          ThrowInvalidOperation,
          cns(env, s_InvalidKeysetOperationMsg.get())
        );
        return cns(env, TBottom);
      }
      return gen(env, IncDecElem, IncDecData{incDec}, ldMBase(env), key);
    }
    PUNT(IncDecNewElem);
  }();
  finish(result);
}

SSATmp* inlineSetOp(IRGS& env, SetOpOp op, SSATmp* lhs, SSATmp* rhs) {
  auto const maybeOp = [&]() -> Optional<Op> {
    switch (op) {
    case SetOpOp::PlusEqual:   return Op::Add;
    case SetOpOp::MinusEqual:  return Op::Sub;
    case SetOpOp::MulEqual:    return Op::Mul;
    case SetOpOp::DivEqual:    return std::nullopt;
    case SetOpOp::ConcatEqual: return std::nullopt;
    case SetOpOp::ModEqual:    return std::nullopt;
    case SetOpOp::PowEqual:    return std::nullopt;
    case SetOpOp::AndEqual:    return Op::BitAnd;
    case SetOpOp::OrEqual:     return Op::BitOr;
    case SetOpOp::XorEqual:    return Op::BitXor;
    case SetOpOp::SlEqual:     return std::nullopt;
    case SetOpOp::SrEqual:     return std::nullopt;
    }
    not_reached();
  }();

  if (!maybeOp) return nullptr;

  auto const bcOp = *maybeOp;
  if (!areBinaryArithTypesSupported(bcOp, lhs->type(), rhs->type())) {
    return nullptr;
  }

  auto const hhirOp = isBitOp(bcOp)
    ? bitOp(bcOp)
    : promoteBinaryDoubles(env, bcOp, lhs, rhs);

  auto result = gen(env, hhirOp, lhs, rhs);
  assertx(result->isA(TUncounted));
  return result;
}

template<class Finish>
SSATmp* setOpPropImpl(IRGS& env, SetOpOp op, SSATmp* base,
                      SSATmp* key, SSATmp* rhs, Finish finish) {
  auto const propInfo =
    getCurrentPropertyOffset(env, base, key->type(), false);

  if (propInfo && !propInfo->isConst) {
    auto const propPtr = emitPropSpecialized(
      env,
      base,
      key,
      MOpMode::Define,
      *propInfo
    );

    auto const lhs = gen(env, LdMem, propInfo->knownType, propPtr);

    auto const handleShuffle = [&](SSATmp* result) {
      verifyPropType(
        env,
        gen(env, LdObjClass, base),
        propInfo->typeConstraints,
        propInfo->slot,
        result,
        key,
        false
      );
      gen(env, StMem, propPtr, result);
      auto const finishMe = [&] (SSATmp* oldVal) {
        gen(env, DecRef, DecRefData{}, oldVal);
        gen(env, IncRef, result);
        return result;
      };
      auto const plhs = profiledType(env, lhs, [&] {
        finishMe(lhs);
        finish(result);
      });
      return finishMe(plhs);
    };

    if (auto const result = inlineSetOp(env, op, lhs, rhs)) {
      assertx(!result->type().maybe(TClsMeth));
      return handleShuffle(result);
    }

    /*
     * If we statically know this SetOp cannot violate the type-hint, we can
     * skip the type-hint validation. Otherwise we need to use the (slower)
     * runtime helpers which also perform a type-hint validation. These might
     * need to perform the SetOp on a temporary, which can trigger COW where we
     * wouldn't otherwise.
     *
     * This is only really important for concat, so that's the only special case
     * we deal with right now. If the lhs is already a string (and therefore
     * must satisfy the type-hint), or the type-hint always allows strings
     * (concats will always produce a string, regardless of the rhs), we know a
     * check isn't necessary.
     */
    auto const fast = [&]{
      if (Cfg::Eval::CheckPropTypeHints <= 0) return true;
      if (!propInfo->typeConstraints ||
          !propInfo->typeConstraints->isCheckable()) {
        return true;
      }
      if (op != SetOpOp::ConcatEqual) return false;
      if (propInfo->knownType <= TStr) return true;
      return propInfo->typeConstraints->alwaysPasses(KindOfString);
    }();

    if (!fast) {
      return handleShuffle(gen(env, OutlineSetOp, SetOpData{op}, lhs, rhs));
    } else {
      gen(env, SetOpTV, SetOpData{op}, propPtr, rhs);
    }
    auto newVal = gen(env, LdMem, propInfo->knownType, propPtr);
    auto const pNewVal = profiledType(env, newVal, [&] {
      gen(env, IncRef, newVal);
      finish(newVal);
    });
    gen(env, IncRef, pNewVal);
    return pNewVal;
  }

  return gen(env, SetOpProp, SetOpData{op}, base, key, rhs);
}

void emitSetOpM(IRGS& env, uint32_t nDiscard, SetOpOp op, MemberKey mk) {
  if (dynPropAccessNeedsInterp(mk)) return interpOne(env);
  auto key = memberKey(env, mk);
  auto rhs = topC(env);

  auto const finish = [&] (SSATmp* result) {
    popDecRef(env);
    mFinalImpl(env, nDiscard, result);
  };
  auto const result = [&] {
    if (mcodeIsProp(mk.mcode)) {
      return setOpPropImpl(env, op, extractBase(env), key, rhs, finish);
    }
    if (mcodeIsElem(mk.mcode)) {
      auto const base = extractBase(env);
      if (base->isA(TDict)) {
        return setOpDictImpl(env, op, base, key, rhs, finish);
      }
      if (base->isA(TVec)) return setOpVecImpl(env, op, base, key, rhs);
      if (base->isA(TKeyset)) {
        gen(
          env,
          ThrowInvalidOperation,
          cns(env, s_InvalidKeysetOperationMsg.get())
        );
        return cns(env, TBottom);
      }
      return gen(env, SetOpElem, SetOpData{op}, ldMBase(env), key, rhs);
    }
    PUNT(SetOpNewElem);
  }();

  finish(result);
}

void emitUnsetM(IRGS& env, uint32_t nDiscard, MemberKey mk) {
  assertx(mcodeIsElem(mk.mcode));
  auto const key = memberKey(env, mk);
  assertx(key->type().isKnownDataType());

  auto const base = extractBase(env);

  if (base->type().subtypeOfAny(TVec, TDict, TKeyset)) {
    if (!key->type().subtypeOfAny(TInt, TStr)) {
      gen(env, ThrowInvalidArrayKey, base, key);
      return;
    }

    auto const newArr = gen(env, BespokeUnset, base, key);
    gen(env, StMem, ldMBase(env), newArr);
  } else {
    // Slow path for handling collections and weird base types.
    gen(env, UnsetElem, ldMBase(env), key);
  }

  mFinalImpl(env, nDiscard, nullptr);
}

//////////////////////////////////////////////////////////////////////

void logArrayAccessProfile(IRGS& env, SSATmp* arr, SSATmp* key,
                           MOpMode mode, const ArrayAccessProfile& profile) {
  // We generate code for many accesses each time we call retranslateAll.
  // We don't want production webservers to log when they do so.
  if (!Cfg::Eval::LogArrayAccessProfile) return;
  if (env.inlineState.conjure) return;

  auto const marker  = makeMarker(env, curSrcKey(env));
  assertx(marker.hasFunc());
  auto const func = marker.func();

  std::vector<std::string> inline_state_string;
  std::vector<folly::StringPiece> inline_state;
  for (auto const& frame : env.inlineState.frames) {
    inline_state_string.push_back(show(frame.callerSk));
    inline_state.push_back(inline_state_string.back());
  }

  StructuredLogEntry entry;
  entry.setStr("marker", marker.show());
  entry.setStr("profile", profile.toString());
  entry.setStr("source_func", func->fullName()->data());
  entry.setStr("source_file", func->filename()->data());
  entry.setInt("source_line", marker.sk().lineNumber());
  entry.setInt("prof_count", curProfCount(env));
  entry.setInt("inline_depth", inlineDepth(env));
  entry.setVec("inline_state", inline_state);
  entry.setStr("arr_type", arr->type().toString());
  entry.setStr("key_type", key->type().toString());
  entry.setStr("hhbc", instrToString(marker.sk().pc(), func));
  entry.setStr("mode", subopToName(mode));

  StructuredLog::log("hhvm_array_accesses", entry);
}

void annotArrayAccessProfile(IRGS& env,
                             SSATmp* arr,
                             SSATmp* key,
                             const ArrayAccessProfile& profile,
                             const ArrayAccessProfile::Result& result) {
  if (!Cfg::Eval::DumpArrAccProf) return;

  auto const fnName = curFunc(env)->fullName()->data();

  env.unit.annotationData->add(
    "ArrAccProf",
    fmt::format("BC={} FN={}: {}: {}: {}: {}\n",
                   bcOff(env), fnName, *arr, *key, profile, result)
  );
}

//////////////////////////////////////////////////////////////////////

void checkElemDimForReadonly(IRGS& env) {
  ifElse(
    env,
    [&] (Block* taken) {
      gen(env, CheckMROProp, taken);
      auto const base = gen(env, LdMem, TCell, ldMBase(env));
      gen(env, CheckType, TObj, taken, base);
    },
    [&] {
      env.irb->exceptionStackBoundary();
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowCannotModifyReadonlyCollection);
    }
  );
}

void checkPropDimForReadonly(IRGS& env, SSATmp* propPtr, const Class* cls,
                             SSATmp* propName) {
  gen(env, StMROProp, cns(env, true));

  ifElse(
    env,
    [&] (Block* taken) {
      auto const prop = gen(env, LdMem, TCell, propPtr);
      gen(env, CheckType, TObj, taken, prop);
    },
    [&] {
      env.irb->exceptionStackBoundary();
      hint(env, Block::Hint::Unlikely);
      auto const data = ClassData { cls };
      gen(env, ThrowMustBeValueTypeException, data, propName);
    }
  );
}

//////////////////////////////////////////////////////////////////////

}
