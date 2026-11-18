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
#include "hphp/hhbbc/type-structure.h"

#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/interp-internal.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-system.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include "hphp/util/configs/eval.h"

namespace HPHP::HHBBC {

//////////////////////////////////////////////////////////////////////

namespace TS = TypeStructure;

//////////////////////////////////////////////////////////////////////

/*
 * NB: The below resolution logic must match the runtime *EXACTLY*.
 *
 * This includes the exact order in which fields are inserted into the
 * array.
 */

namespace {

TRACE_SET_MOD(hhbbc_index)

// Whether the kind represents a type-structure which is inherently
// resolved. We don't need to look at those at all.
bool kind_is_resolved(TS::Kind kind) {
  switch (kind) {
    case TS::Kind::T_int:
    case TS::Kind::T_bool:
    case TS::Kind::T_float:
    case TS::Kind::T_string:
    case TS::Kind::T_num:
    case TS::Kind::T_arraykey:
    case TS::Kind::T_void:
    case TS::Kind::T_null:
    case TS::Kind::T_nothing:
    case TS::Kind::T_noreturn:
    case TS::Kind::T_mixed:
    case TS::Kind::T_dynamic:
    case TS::Kind::T_nonnull:
    case TS::Kind::T_resource:
    case TS::Kind::T_class:
    case TS::Kind::T_interface:
    case TS::Kind::T_xhp:
    case TS::Kind::T_enum:
    case TS::Kind::T_trait:
    case TS::Kind::T_recursiveUnion:
      return true;
    case TS::Kind::T_typevar:
    case TS::Kind::T_darray:
    case TS::Kind::T_varray:
    case TS::Kind::T_varray_or_darray:
    case TS::Kind::T_dict:
    case TS::Kind::T_vec:
    case TS::Kind::T_keyset:
    case TS::Kind::T_vec_or_dict:
    case TS::Kind::T_class_ptr:
    case TS::Kind::T_class_or_classname:
    case TS::Kind::T_any_array:
    case TS::Kind::T_tuple:
    case TS::Kind::T_shape:
    case TS::Kind::T_unresolved:
    case TS::Kind::T_typeaccess:
    case TS::Kind::T_fun:
    case TS::Kind::T_reifiedtype:
    case TS::Kind::T_union:
      return false;
  }
  always_assert(false);
}

const StaticString& key_for_alias_kind(AliasKind kind) {
  switch (kind) {
    case AliasKind::TypeAlias: return s_alias;
    case AliasKind::CaseType: return s_case_type;
  }
}

using Resolution = TypeStructureResolution;

Resolution&& maybe_make_bespoke(Resolution&& r) {
  // convert type structure into bespoke versions if possible
  if (!Cfg::Eval::EmitBespokeTypeStructures) return std::move(r);

  if (auto const t = tv(r.type)) {
    auto const ad = val(*t).parr;
    if (!bespoke::TypeStructure::isValidTypeStructure(ad)) return std::move(r);

    if (auto const ts =
        bespoke::TypeStructure::MakeFromVanillaStatic(ad, false)) {
      r.type = dict_val(ts);
    }
  }
  return std::move(r);
}

// Builder encapsulates the resolution result being built. The user
// can simply use the pre-defined mutations and not worry about
// dealing with type mutations.
struct Builder {
  static Builder dict() {
    return Builder{ Resolution{ dict_empty(), false }};
  }
  static Builder vec() {
    return Builder{ Resolution{ vec_empty(), false }};
  }

  static Builder copy(SArray from, TS::Kind kind) {
    assertx(from->isStatic());
    assertx(from->isDictType());
    auto b = dict()
      .copyModifiers(from)
      .set(s_kind, make_tv<KindOfInt64>((int64_t)kind));
    if (from->exists(s_allows_unknown_fields)) {
      b.set(s_allows_unknown_fields, make_tv<KindOfBoolean>(true));
    }
    return b;
  }

  static Builder attach(Resolution r) { return Builder{std::move(r)}; }

  bool alwaysFails() const { return r.type.is(BBottom); }

  Builder& copyModifiers(SArray from) {
    assertx(from->isStatic());
    assertx(from->isDictType());
    if (from->exists(s_nullable)) set(s_nullable, make_tv<KindOfBoolean>(true));
    if (from->exists(s_soft))     set(s_soft, make_tv<KindOfBoolean>(true));
    return *this;
  }

  Builder& optCopy(const StaticString& key, SArray from) {
    assertx(from->isStatic());
    assertx(from->isDictType());
    auto const v = from->get(key);
    if (!v.is_init()) return *this;
    set(key, v);
    return *this;
  }

  template <typename C, typename F>
  Builder& resolve(const StaticString& key, SArray v,
                   C& c, const F& f) {
    if (!v) return *this;
    if (r.type.is(BBottom)) return *this;
    set(key, f(c, v));
    return *this;
  }

  Builder& set(const StringData* key, const Resolution& o) {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(IMPLIES(o.type.is(BBottom), o.mightFail));
    assertx(r.type.subtypeOf(BDict));
    assertx(o.type.subtypeOf(BDict) || o.type.subtypeOf(BVec));
    assertx(key->isStatic());
    if (r.type.is(BBottom)) return *this;
    if (o.type.is(BBottom)) {
      r.type = TBottom;
      r.mightFail = true;
      return *this;
    }
    auto s = array_like_set(std::move(r.type), sval(key), o.type);
    assertx(!s.second);
    r.type = std::move(s.first);
    r.mightFail |= o.mightFail;
    r.contextSensitive |= o.contextSensitive;
    return *this;
  }

  Builder& set(const StaticString& key, const Resolution& o) {
    return set(key.get(), o);
  }

  Builder& set(const Type& key, const Resolution& o) {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(IMPLIES(o.type.is(BBottom), o.mightFail));
    assertx(r.type.subtypeOf(BDict));
    assertx(o.type.subtypeOf(BDict) || o.type.subtypeOf(BVec));
    assertx(key.subtypeOf(BArrKey));
    if (r.type.is(BBottom)) return *this;
    if (key.is(BBottom) || o.type.is(BBottom)) {
      r.type = TBottom;
      r.mightFail = true;
      return *this;
    }
    auto s = array_like_set(std::move(r.type), key, o.type);
    assertx(!s.second);
    r.type = std::move(s.first);
    r.mightFail |= o.mightFail;
    r.contextSensitive |= o.contextSensitive;
    return *this;
  }

  Builder& set(const StringData* key, TypedValue v) {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(r.type.subtypeOf(BDict));
    assertx(key->isStatic());
    if (r.type.is(BBottom)) return *this;
    auto s = array_like_set(std::move(r.type), sval(key), from_cell(v));
    assertx(!s.second);
    r.type = std::move(s.first);
    return *this;
  }

  Builder& set(const StaticString& key, TypedValue v) {
    return set(key.get(), v);
  }

  Builder& set(const StaticString& key, SString v) {
    assertx(v->isStatic());
    return set(key.get(), make_tv<KindOfString>(const_cast<StringData*>(v)));
  }

  Builder& append(const Resolution& o) {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(IMPLIES(o.type.is(BBottom), o.mightFail));
    assertx(r.type.subtypeOf(BVec));
    assertx(o.type.subtypeOf(BDict) || o.type.subtypeOf(BVec));
    if (r.type.is(BBottom)) return *this;
    if (o.type.is(BBottom)) {
      r.type = TBottom;
      r.mightFail = true;
      return *this;
    }
    auto a = array_like_newelem(std::move(r.type), o.type);
    assertx(!a.second);
    r.type = std::move(a.first);
    r.mightFail |= o.mightFail;
    r.contextSensitive |= o.contextSensitive;
    return *this;
  }

  Builder& remove(const StaticString& key) {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(r.type.subtypeOf(BDict));

    if (r.type.is(BBottom)) return *this;

    if (auto const t = tv(r.type)) {
      auto arr = Array::attach(val(*t).parr);
      arr.remove(key);
      arr.setEvalScalar();
      r.type = dict_val(arr.get());
    } else if (!array_like_elem(r.type, sval(key.get())).first.is(BBottom)) {
      r.type = TDictN;
    }

    return *this;
  }

  Builder& usedThis(bool used = true) {
    if (used) r.contextSensitive = true;
    return *this;
  }

  Resolution&& finish() {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(r.type.subtypeOf(BDict) || r.type.subtypeOf(BVec));
    return std::move(r);
  }

  Resolution&& finishTS() {
    assertx(IMPLIES(r.type.is(BBottom), r.mightFail));
    assertx(r.type.subtypeOf(BDict));
    return maybe_make_bespoke(std::move(r));
  }

private:
  explicit Builder(Resolution r) : r{std::move(r)} {}
  Resolution r;
};

struct TypeWrapper {
  /* implicit */ TypeWrapper(Type t) : type{std::move(t)} {}
  Type type;
  bool operator==(const TypeWrapper& o) const { return equal(type, o.type); }
};

using GenericsMap = hphp_fast_map<std::string, TypeWrapper>;

struct Cache;

// A map used to indicate aliases currently being resolved (used to find
// recursive definitions). Maps from alias name to the type of alias.
using ResolvingMap = SStringToOneT<const php::TypeAlias*>;

struct ResolveCtx {
  ResolveCtx(Context ctx, const IIndex* index,
             Cache* cache, ResolvingMap* resolving)
    : ctx{ctx}, index{index}, cache{cache}, resolving{resolving} {}
  Context ctx;
  const IIndex* index;
  Cache* cache;
  const CollectedInfo* collect = nullptr;
  const php::Class* selfCls = nullptr;
  const php::Class* thisCls = nullptr;
  const GenericsMap* generics = nullptr;
  ResolvingMap* resolving = nullptr;
};

// RAII structure used to add an alias to the ResolvingMap for the current
// context. Removes the alias on delete.
struct WithResolving {
  ~WithResolving() {
    if (m_clsName) m_resolving->erase(m_clsName);
  }
  WithResolving(SString clsName,
                const php::TypeAlias* alias,
                ResolvingMap* resolving) : m_clsName{clsName},
                                           m_resolving{resolving} {
    auto it = resolving->insert_or_assign(clsName, alias);
    if (!it.second) {
      // The name was already in the map - we're not the one to remove it.
      m_clsName = nullptr;
    }
  }
  WithResolving(const WithResolving&) = delete;
  WithResolving& operator=(const WithResolving&) = delete;
private:
  // m_clsName can be null if there was a collision and this isn't the outer
  // WithResolving that originally set the value.
  SString m_clsName;
  ResolvingMap* m_resolving = nullptr;
};

struct Cache {
  struct Key {
    SArray ts;
    const php::Class* selfCls;
    const php::Class* thisCls;
    Optional<GenericsMap> generics;
  };

  struct KeyProxy {
    SArray ts;
    const ResolveCtx& ctx;
  };

  struct GenericsHasher {
    size_t operator()(const std::pair<std::string, TypeWrapper>& p) const {
      return folly::hash::hash_128_to_64(
        folly::hasher<std::string>{}(p.first),
        p.second.type.hash()
      );
    }
  };

  struct KeyHasher {
    using is_transparent = void;

    size_t hashGenericsMap(const GenericsMap* g) const {
      if (!g) return 0;
      return folly::hash::commutative_hash_combine_range_generic(
        1,
        GenericsHasher{},
        g->begin(),
        g->end()
      );
    }

    size_t operator()(const Key& k) const {
      return folly::hash::hash_combine(
        k.ts,
        k.selfCls,
        k.thisCls,
        hashGenericsMap(k.generics.get_pointer())
      );
    }

    size_t operator()(const KeyProxy& k) const {
      return folly::hash::hash_combine(
        k.ts,
        k.ctx.selfCls,
        k.ctx.thisCls,
        hashGenericsMap(k.ctx.generics)
      );
    }
  };

  struct KeyEquals {
    using is_transparent = void;
    bool operator()(const Key& k1, const Key& k2) const {
      return
        std::tie(k1.ts, k1.selfCls, k1.thisCls, k1.generics) ==
        std::tie(k2.ts, k2.selfCls, k2.thisCls, k2.generics);
    }
    bool operator()(const KeyProxy& k1, const Key& k2) const {
      if (std::tie(k1.ts, k1.ctx.selfCls, k1.ctx.thisCls) !=
          std::tie(k2.ts, k2.selfCls, k2.thisCls)) {
        return false;
      }
      if (!k1.ctx.generics) return !k2.generics.has_value();
      if (!k2.generics.has_value()) return false;
      return *k1.ctx.generics == *k2.generics;
    }
  };

  hphp_fast_map<Key, Resolution, KeyHasher, KeyEquals> m_cache;

  Optional<Resolution> enter(const ResolveCtx& ctx, SArray ts) {
    auto const insertion = m_cache.emplace(
      Key{
        ts, ctx.selfCls, ctx.thisCls,
        ctx.generics ? *ctx.generics : Optional<GenericsMap>{}
      },
      Resolution{ TDictN, true }
    );
    if (insertion.second) return std::nullopt;
    return insertion.first->second;
  }

  void exit(const ResolveCtx& ctx, SArray ts, Resolution r) {
    auto const it = m_cache.find(KeyProxy{ts, ctx});
    assertx(it != m_cache.end());
    it->second = std::move(r);
  }
};

// Map a name into an appropriate TCls specialization.
std::pair<Type, bool> name_to_cls_type(ResolveCtx& ctx, SString name) {
  auto const resolveCls = [&] (const php::Class* cls) {
    auto const rcls = ctx.index->resolve_class(cls->name);
    if (!rcls) return TBottom;
    return clsExact(*rcls, true);
  };
  auto const resolveStr = [&] (const StringData* str) {
    auto const rcls = ctx.index->resolve_class(str);
    if (!rcls) return TBottom;
    return clsExact(*rcls, true);
  };

  if (ctx.selfCls) {
    if (name->tsame(s_hh_this.get())) {
      if (ctx.thisCls) return std::make_pair(resolveCls(ctx.thisCls), true);
      auto const rcls = ctx.index->resolve_class(ctx.selfCls->name);
      if (!rcls) return std::make_pair(TBottom, false);
      return std::make_pair(subCls(*rcls, true), false);
    }

    if (name->tsame(s_self.get())) {
      return std::make_pair(resolveCls(ctx.selfCls), false);
    }

    if (name->tsame(s_parent.get())) {
      if (!ctx.selfCls->parentName) return std::make_pair(TBottom, false);
      return std::make_pair(resolveStr(ctx.selfCls->parentName), false);
    }
  }

  while (true) {
    auto const lookup = ctx.index->lookup_class_or_type_alias(name);
    if (lookup.cls) {
      auto const rcls = ctx.index->resolve_class(*lookup.cls);
      if (!rcls) return std::make_pair(TBottom, false);
      return std::make_pair(clsExact(*rcls, true), false);
    }
    if (!lookup.typeAlias) {
      if (!lookup.maybeExists) return std::make_pair(TBottom, false);
      // It might exist, so treat it as an unresolved class.
      auto const rcls = ctx.index->resolve_class(name);
      if (!rcls) return std::make_pair(TBottom, false);
      return std::make_pair(clsExact(*rcls, true), false);
    }
    auto const typeAlias = lookup.typeAlias;
    assertx(typeAlias->typeStructure);
    assertx(typeAlias->typeStructure->exists(s_kind));
    if (!typeAlias->typeStructure->exists(s_classname)) {
      return std::make_pair(TBottom, false);
    }
    auto const tv = typeAlias->typeStructure->get(s_classname);
    assertx(tvIsString(tv));
    name = val(tv).pstr;
  }

  not_reached();
}

Resolution resolve(ResolveCtx&, SArray ts);
Resolution resolve_bespoke(ResolveCtx& ctx, SArray ts);

Resolution resolve_list(ResolveCtx& ctx, SArray ts) {
  assertx(ts->isStatic());
  assertx(ts->isVecType());
  auto const size = ts->size();
  auto b = Builder::vec();
  for (size_t i = 0; i < size; ++i) {
    auto const tv = ts->get(i);
    assertx(tvIsDict(tv));
    auto const r = resolve_bespoke(ctx, val(tv).parr);
    b.append(r);
    if (b.alwaysFails()) break;
  }
  return b.finish();
}

Optional<std::vector<Resolution>> resolve_list_separate(ResolveCtx& ctx,
                                                        SArray ts) {
  assertx(ts->isStatic());
  assertx(ts->isVecType());
  auto const size = ts->size();
  std::vector<Resolution> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    auto const tv = ts->get(i);
    assertx(tvIsDict(tv));
    out.emplace_back(resolve_bespoke(ctx, val(tv).parr));
    if (out.back().type.is(BBottom)) return std::nullopt;
  }
  return out;
}

Optional<Resolution> resolve_typevar_types(ResolveCtx& ctx,
                                           SArray ts,
                                           const php::TypeAlias* typeAlias,
                                           GenericsMap& genericsMap) {
  if (typeAlias->typeStructure->exists(s_typevars) &&
      ts->exists(s_generic_types)) {
    auto const tv = typeAlias->typeStructure->get(s_typevars);
    assertx(tvIsString(tv));
    std::vector<std::string> typevars;
    folly::split(',', val(tv).pstr->data(), typevars);

    auto rgenerics = resolve_list_separate(ctx, get_ts_generic_types(ts));
    if (!rgenerics) return Resolution{ TBottom, true };

    auto d = Builder::dict();
    auto const size = std::min<size_t>(typevars.size(), rgenerics->size());

    for (size_t i = 0; i < size; ++i) {
      d.set(makeStaticString(typevars[i]), (*rgenerics)[i]);
      genericsMap.insert_or_assign(typevars[i], std::move((*rgenerics)[i].type));
    }
    return std::move(d).finish();
  }
  return {};
}

Resolution resolve_fun(ResolveCtx& ctx, SArray ts) {
  return Builder::copy(ts, TS::Kind::T_fun)
    .resolve(s_return_type, get_ts_return_type(ts), ctx, resolve_bespoke)
    .resolve(s_param_types, get_ts_param_types(ts), ctx, resolve_list)
    .resolve(s_variadic_type, get_ts_variadic_type_opt(ts), ctx, resolve_bespoke)
    .optCopy(s_typevars, ts)
    .optCopy(s_alias, ts)
    .optCopy(s_case_type, ts)
    .finish();
}

Resolution resolve_tuple(ResolveCtx& ctx, SArray ts) {
  return Builder::copy(ts, TS::Kind::T_tuple)
    .resolve(s_elem_types, get_ts_elem_types(ts), ctx, resolve_list)
    .resolve(s_optional_elem_types, get_ts_optional_elem_types_opt(ts),
             ctx, resolve_list)
    .resolve(s_variadic_type, get_ts_variadic_type_opt(ts), ctx,
             resolve_bespoke)
    .optCopy(s_typevars, ts)
    .optCopy(s_alias, ts)
    .optCopy(s_case_type, ts)
    .finish();
}

Resolution resolve_arraylike(ResolveCtx& ctx, TS::Kind kind, SArray ts) {
  return Builder::copy(ts, kind)
    .resolve(s_generic_types, get_ts_generic_types_opt(ts), ctx, resolve_list)
    .optCopy(s_typevars, ts)
    .optCopy(s_alias, ts)
    .optCopy(s_case_type, ts)
    .finish();
}

Resolution resolve_typevar(ResolveCtx& ctx, SArray ts) {
  auto const name = get_ts_name(ts);
  if (name->tsame(s_wildcard.get())) {
    return Resolution { dict_val(ts), false };
  }
  if (!ctx.generics) return Resolution { TDictN, false };
  auto const it = ctx.generics->find(name->toCppString());
  if (it == ctx.generics->end()) {
    return Resolution { dict_val(ts), false };
  }
  return Resolution { it->second.type, false };
}

// A shape splat: resolve each element to a concrete (statically-known) type
// structure and merge them with the runtime's merge function, so HHBBC and the
// runtime agree byte-for-byte. If any element is not a statically-known concrete
// type structure we cannot fold it here, so we defer the ENTIRE merge to the
// runtime (return TDictN) rather than partially merging.
//
// A union operand resolves to a (statically concrete) union type structure and
// is distributed by the shared merge, so the merged result may itself be a
// union (shape(...(A|B)) = shape(...A) | shape(...B)); the wrapping below is
// kind-agnostic and carries the modifiers onto whatever the merge returns.
//
// A non-static element can be an unbound type-parameter operand, e.g.
// `shape(...T)`. When `shape_splat_type_parameters` is enabled, HHBBC cannot
// fold such an operand without its runtime type structure, so it defers the
// whole merge to the runtime resolver.
Resolution resolve_shape_splat(ResolveCtx& ctx, SArray ts, SArray splatElems) {
  assertx(splatElems->isStatic());
  assertx(splatElems->isVecType());

  auto const size = splatElems->size();
  req::vector<Array> resolved;
  resolved.reserve(size);
  bool mightFail = false;
  bool contextSensitive = false;
  for (size_t i = 0; i < size; ++i) {
    auto const elem = splatElems->get(i);
    assertx(tvIsDict(elem));
    auto r = resolve(ctx, val(elem).parr);
    if (r.type.is(BBottom)) return Resolution { TBottom, true };
    mightFail |= r.mightFail;
    contextSensitive |= r.contextSensitive;
    auto const c = tv(r.type);
    // Not statically foldable: defer the whole merge to the runtime resolver.
    if (!c) return Resolution { TDictN, true };
    assertx(tvIsDict(*c));
    resolved.emplace_back(Array{val(*c).parr});
  }

  bool invalidType = false;
  auto merged = TypeStructure::mergeResolvedShapeSplat(resolved, invalidType);
  if (invalidType) {
    // Keep the original splat for runtime validation: normalization may have
    // erased the invalid operand, for example when followed by `nothing`.
    return Resolution { TDictN, true };
  }
  merged.setEvalScalar();

  // Carry outer modifiers and typevars/alias/case_type in the same order the
  // runtime applies them after resolveShapeSplat.
  auto res = Builder::attach(Resolution { dict_val(merged.get()), mightFail })
    .copyModifiers(ts)
    .optCopy(s_typevars, ts)
    .optCopy(s_alias, ts)
    .optCopy(s_case_type, ts)
    .finishTS();
  res.contextSensitive |= contextSensitive;
  return res;
}

Resolution resolve_shape(ResolveCtx& ctx, SArray ts) {
  if (auto const splat = get_ts_splat_elem_types_opt(ts)) {
    return resolve_shape_splat(ctx, ts, splat);
  }

  auto const fields = get_ts_fields(ts);
  assertx(fields->isStatic());
  assertx(fields->isDictType());

  auto const size = fields->size();

  auto b = Builder::dict();
  for (size_t i = 0; i < size; ++i) {
    auto const fieldValue = fields->nvGetVal(i);
    assertx(tvIsArrayLike(fieldValue));
    auto const fieldValueArr = val(fieldValue).parr;

    auto const [key, mightFail, sensitive] = [&] {
      auto const fieldKey = fields->nvGetKey(i);
      assertx(tvIsString(fieldKey) || tvIsInt(fieldKey));

      if (!fieldValueArr->exists(s_is_cls_cns)) {
        return std::make_tuple(from_cell(fieldKey), false, false);
      }

      assertx(tvIsString(fieldKey));
      auto const clsCns = val(fieldKey).pstr->toCppString();
      std::string clsNameStr, cnsNameStr;
      folly::split("::", clsCns, clsNameStr, cnsNameStr);

      auto const clsName = makeStaticString(clsNameStr);
      auto const cnsName = makeStaticString(cnsNameStr);

      auto const fails = [&] {
        return std::make_tuple(TBottom, true, false);
      };

      auto const [cls, sensitive] = name_to_cls_type(ctx, clsName);
      if (cls.is(BBottom)) return fails();

      auto lookup = lookupClsConstant(
        *ctx.index,
        ctx.ctx,
        ctx.collect,
        cls,
        sval(cnsName)
      );
      if (lookup.found == TriBool::No) return fails();
      if (!lookup.ty.couldBe(BArrKey | BCls | BLazyCls)) return fails();

      auto const noFail =
        lookup.found == TriBool::Yes &&
        !lookup.mightThrow &&
        lookup.ty.subtypeOf(BArrKey);

      return std::make_tuple(
        intersection_of(loosen_likeness(std::move(lookup.ty)), TArrKey),
        !noFail,
        sensitive
      );
    }();

    auto resolution = [&] {
      auto const toResolve = [&] () -> SArray {
        auto const v = fieldValueArr->get(s_value);
        if (!v.is_init()) return fieldValueArr;
        // Runtime will attempt to coerce this to an array if it's not.
        if (!tvIsDict(v)) return nullptr;
        return val(v).parr;
      }();
      if (!toResolve) return Resolution { TDictN, true };

      auto r = resolve(ctx, toResolve);
      if (!fieldValueArr->exists(s_optional_shape_field)) return r;

      return Builder::attach(std::move(r))
        .set(s_optional_shape_field, make_tv<KindOfBoolean>(true))
        .finish();
    }();
    resolution.mightFail |= mightFail;
    resolution.contextSensitive |= sensitive;

    b.set(key, maybe_make_bespoke(std::move(resolution)));
    if (b.alwaysFails()) break;
  }

  return Builder::copy(ts, TS::Kind::T_shape)
    .set(s_fields, b.finish())
    .resolve(s_variadic_type, get_ts_variadic_type_opt(ts), ctx,
             resolve_bespoke)
    .optCopy(s_typevars, ts)
    .optCopy(s_alias, ts)
    .optCopy(s_case_type, ts)
    .finishTS();
}

Resolution resolve_type_access_list(ResolveCtx& ctx,
                                    const Type& clsType,
                                    SArray accessList,
                                    size_t idx) {
  assertx(!clsType.is(BBottom));
  assertx(accessList->isStatic());
  assertx(accessList->isVecType());
  assertx(accessList->size() > 0);

  auto const cnsName = accessList->at(idx);
  assertx(tvIsString(cnsName));

  // Use a custom resolver. Instead of merely returning the result, we
  // continue down the access-list and resolve the entire thing. This
  // produces *much* better types (since we're unioning together all
  // of the final results, not intermediate steps).
  auto lookup = ctx.index->lookup_class_type_constant(
    clsType, sval(val(cnsName).pstr),
    [&] (const php::Const& cns, const php::Class& thiz) {
      assertx(cns.kind == ConstModifierFlags::Kind::Type);
      assertx(cns.val.has_value());
      assertx(tvIsDict(*cns.val));
      assertx(val(*cns.val).parr->isStatic());

      auto r = [&] {
        if (cns.resolvedTypeStructure) {
          if (!cns.resolvedLocally &&
              (cns.contextInsensitive || cns.cls->tsame(thiz.name))) {
            return Resolution {
              dict_val(cns.resolvedTypeStructure),
              false,
              !cns.contextInsensitive
            };
          }
        }
        ResolveCtx newCtx{ctx.ctx, ctx.index, ctx.cache, ctx.resolving};
        newCtx.selfCls = ctx.index->lookup_const_class(cns);
        if (!newCtx.selfCls) return Resolution{ TDictN, true };
        newCtx.thisCls = &thiz;
        newCtx.generics = ctx.generics;
        return resolve(newCtx, val(*cns.val).parr);
      }();

      r.contextSensitive = false;
      assertx(r.type.subtypeOf(BDictN));
      if (r.type.is(BBottom)) return Resolution{ TBottom, true };
      if (idx == accessList->size() - 1) return r;

      auto const [kindTy, kindPresent] =
        array_like_elem(r.type, sval(s_kind.get()));
      auto const kind = tv(kindTy);
      if (!kind || !tvIsInt(*kind)) return Resolution{ TDictN, true };

      switch ((TS::Kind)val(*kind).num) {
        case TS::Kind::T_class:
        case TS::Kind::T_interface:
        case TS::Kind::T_xhp:
        case TS::Kind::T_trait:
          break;
        default:
          return Resolution{ TBottom, true };
      }

      auto const [clsNameTy, clsNamePresent] =
        array_like_elem(r.type, sval(s_classname.get()));
      auto const clsName = tv(clsNameTy);
      if (!clsName || !tvIsString(*clsName)) {
        return Resolution{ TDictN, true };
      }

      auto const rcls = ctx.index->resolve_class(val(*clsName).pstr);
      if (!rcls) return Resolution{ TBottom, true };

      auto next =
        resolve_type_access_list(ctx, clsExact(*rcls, true), accessList, idx+1);
      next.mightFail |= (!kindPresent || !clsNamePresent || r.mightFail);
      return next;
    }
  );
  assertx(lookup.resolution.type.subtypeOf(BSDictN));
  if (lookup.found == TriBool::No) return Resolution{ TBottom, true };

  auto const mightFail =
    lookup.found != TriBool::Yes ||
    lookup.resolution.mightFail;
  return Resolution{
    std::move(lookup.resolution.type),
    mightFail,
    lookup.resolution.contextSensitive
  };
}

Resolution resolve_type_access(ResolveCtx& ctx, SArray ts) {
  auto const [clsType, sensitive] = name_to_cls_type(ctx, get_ts_root_name(ts));
  if (clsType.is(BBottom)) return Resolution{ TBottom, true };
  return Builder::attach(
    resolve_type_access_list(ctx, clsType, get_ts_access_list(ts), 0)
  ).copyModifiers(ts)
   .optCopy(s_alias, ts)
   .optCopy(s_case_type, ts)
   .usedThis(sensitive)
   .finish();
}

Resolution resolve_refinement_types(ResolveCtx& ctx, SArray ts) {
  auto const refinements = get_ts_refinement_types_opt(ts);
  if (!refinements) return Resolution{ TBottom, false };
  assertx(refinements->isStatic());
  assertx(refinements->isDictType());
  auto const size = refinements->size();
  auto membersBuilder = Builder::dict();
  for (size_t i = 0; i < size; ++i) {
    auto const k = refinements->nvGetKey(i);
    auto const v = refinements->nvGetVal(i);
    assertx(tvIsString(k));
    assertx(tvIsDict(v));
    auto const memberArr = val(v).parr;
    auto memberBuilder = Builder::dict();

    auto const isCtx = memberArr->get(s_is_ctx.get());
    if (isCtx.is_init()) {
      memberBuilder.set(s_is_ctx, isCtx);
    }

    auto const exact = memberArr->get(s_equals.get());
    if (exact.is_init()) {
      assertx(tvIsDict(exact));
      auto exactRes = resolve_bespoke(ctx, val(exact).parr);
      memberBuilder.set(s_equals, exactRes);
    }

    auto const upper = memberArr->get(s_as.get());
    if (upper.is_init()) {
      assertx(tvIsVec(upper));
      auto upperRes = resolve_list(ctx, val(upper).parr);
      memberBuilder.set(s_as, upperRes);
    }

    auto const lower = memberArr->get(s_super.get());
    if (lower.is_init()) {
      assertx(tvIsVec(lower));
      auto lowerRes = resolve_list(ctx, val(lower).parr);
      memberBuilder.set(s_super, lowerRes);
    }

    membersBuilder.set(sval(val(k).pstr), memberBuilder.finish());
  }
  return membersBuilder.finish();
}

Resolution resolve_unresolved(ResolveCtx& ctx, SArray ts) {
  auto b = Builder::copy(ts, TS::Kind::T_unresolved);

  auto const clsName = get_ts_classname(ts);

  auto const setKindAndName = [&] (TS::Kind k, SString n) {
    return Builder::copy(ts, TS::Kind::T_unresolved)
      .set(s_kind, make_tv<KindOfInt64>((int64_t)k))
      .set(s_classname, make_tv<KindOfPersistentString>(n));
  };

  if (clsName->tsame(s_callable.get())) {
    return setKindAndName(TS::Kind::T_class, clsName)
      .optCopy(s_typevars, ts)
      .optCopy(s_alias, ts)
      .optCopy(s_case_type, ts)
      .finish();
  }

  auto const resolvedCls = [&] (SString name, Attr attrs, bool setExact) {
    auto const kind = [&] {
      if (attrs & AttrEnum)      return TypeStructure::Kind::T_enum;
      if (attrs & AttrTrait)     return TypeStructure::Kind::T_trait;
      if (attrs & AttrInterface) return TypeStructure::Kind::T_interface;
      return TypeStructure::Kind::T_class;
    }();

    auto b = setKindAndName(kind, name);
    if (setExact) b.set(s_exact, make_tv<KindOfBoolean>(true));
    b.resolve(s_generic_types, get_ts_generic_types_opt(ts),
              ctx, resolve_list);
    auto refinementRes = resolve_refinement_types(ctx, ts);
    if (!refinementRes.type.is(BBottom)) {
      b.set(s_with_refinements, refinementRes);
    }
    return
      std::move(b)
        .optCopy(s_typevars, ts)
        .optCopy(s_alias, ts)
        .optCopy(s_case_type, ts)
        .finish();
  };

  auto const conservative = [&] {
    // Even if we're returning a conservative result, still process
    // the generic types to ensure we record dependencies on them.
    if (auto const g = get_ts_generic_types_opt(ts)) {
      resolve_list(ctx, g);
    }
    return Resolution{ TDictN, true };
  };

  auto const fail = [&] {
    return Resolution{ TBottom, true };
  };

  if (ctx.selfCls) {
    if (clsName->tsame(s_hh_this.get())) {
      if (ctx.thisCls) {
        auto r = resolvedCls(ctx.thisCls->name, ctx.thisCls->attrs, true);
        r.contextSensitive = true;
        return r;
      }
      if (ctx.selfCls->attrs & AttrNoOverride) {
        return resolvedCls(ctx.selfCls->name, ctx.selfCls->attrs, true);
      }

      auto const rcls = ctx.index->resolve_class(ctx.selfCls->name);
      if (!rcls) return fail();

      Optional<Resolution> resolution;
      auto const knowsChildren = rcls->forEachSubclass(
        [&] (SString name, Attr attrs) {
          if (!resolution) {
            resolution.emplace(resolvedCls(name, attrs, true));
          } else {
            *resolution |= resolvedCls(name, attrs, true);
          }
        }
      );
      if (!knowsChildren) return conservative();
      if (!resolution) return fail();
      return *resolution;
    }

    if (clsName->tsame(s_self.get())) {
      return resolvedCls(ctx.selfCls->name, ctx.selfCls->attrs, false);
    }

    if (clsName->tsame(s_parent.get())) {
      if (!ctx.selfCls->parentName) return fail();
      auto const rcls = ctx.index->resolve_class(ctx.selfCls->parentName);
      if (!rcls) return fail();
      if (auto const cls = rcls->cls()) {
        return resolvedCls(cls->name, cls->attrs, false);
      }
      return conservative();
    }
  }

  const auto lookup = ctx.index->lookup_class_or_type_alias(clsName);
  if (lookup.cls) {
    return resolvedCls(lookup.cls->name, lookup.cls->attrs, false);
  }
  if (!lookup.typeAlias) {
    return lookup.maybeExists
      ? conservative()
      : fail();
  }
  auto const typeAlias = lookup.typeAlias;
  assertx(typeAlias->typeStructure);
  assertx(!typeAlias->typeStructure->empty());
  assertx(typeAlias->typeStructure->isDictType());

  auto ty = ctx.resolving->find(clsName);
  if (ty != ctx.resolving->end()) {
    // The alias is currently being resolved and we are hitting a
    // recursion.
    switch (ty->second->kind) {
      case AliasKind::TypeAlias: {
        // Alias recursion isn't allowed - we could just short-circuit here but
        // instead allow the "normal" code to deal with it (which will end up
        // returning the unresolved type).
        break;
      }
      case AliasKind::CaseType: {
        // This is a recursive case type. It's allowed - but we just need to
        // set a few fields.
        GenericsMap genericsMap;
        auto typevarTypes = resolve_typevar_types(ctx, ts, ty->second,
                                                  genericsMap);
        auto b = Builder::dict()
          .set(s_kind, make_tv<KindOfInt64>((int64_t)TS::Kind::T_recursiveUnion))
          .set(s_case_type, clsName);
        if (typevarTypes) b.set(s_typevar_types, *typevarTypes);
        return std::move(b).finish();
      }
    }
  }

  WithResolving withResolving(clsName, lookup.typeAlias, ctx.resolving);

  auto const resolveTA =
    [&, typeAlias=typeAlias] (const GenericsMap& g,
                              Optional<Resolution> typevarTypes) {
    auto b = [&, typeAlias=typeAlias] {
      assertx(typeAlias->typeStructure);

      auto const adjustTS = [&] (Builder&& b) -> Builder {
        auto const tv = ts->get(s_typevars);
        b.remove(s_typevars);

        b.optCopy(s_alias, ts);
        if (typevarTypes) b.set(s_typevar_types, *typevarTypes);
        if (tv.is_init()) b.set(s_typevars, tv);
        return b.optCopy(s_case_type, ts);
      };

      if (typeAlias->resolvedTypeStructure) {
        assertx(typeAlias->resolvedTypeStructure->isStatic());
        assertx(typeAlias->resolvedTypeStructure->isDictType());
        if (!typeAlias->resolvedLocally) {
          return adjustTS(Builder::attach(
            Resolution { dict_val(typeAlias->resolvedTypeStructure), false }
          ));
        }
      }

      ResolveCtx newCtx{ctx.ctx, ctx.index, ctx.cache, ctx.resolving};
      newCtx.generics = &g;

      const StaticString& key = key_for_alias_kind(typeAlias->kind);
      auto r = resolve(newCtx, typeAlias->typeStructure);
      assertx(!r.contextSensitive);
      auto b = Builder::attach(std::move(r))
        .set(key, make_tv<KindOfPersistentString>(clsName));
      return adjustTS(std::move(b));
    }();

    return b.copyModifiers(ts).finish();
  };

  GenericsMap genericsMap;
  auto typevarTypes = resolve_typevar_types(ctx, ts, typeAlias, genericsMap);
  if (typevarTypes && typevarTypes->type.is(BBottom)) return fail();
  return resolveTA(genericsMap, typevarTypes);
}

Resolution resolve_union(ResolveCtx& ctx, SArray ts) {
  return Builder::copy(ts, TS::Kind::T_union)
    .resolve(s_union_types, get_ts_union_types(ts), ctx, resolve_list)
    .optCopy(s_typevars, ts)
    .finish();
}

Resolution resolve_impl(ResolveCtx& ctx, TS::Kind kind, SArray ts) {
  assertx(ts->isStatic());
  assertx(ts->isDictType());

  switch (kind) {
    case TS::Kind::T_fun:
      return resolve_fun(ctx, ts);
    case TS::Kind::T_tuple:
      return resolve_tuple(ctx, ts);
    case TS::Kind::T_darray:
    case TS::Kind::T_varray:
    case TS::Kind::T_varray_or_darray:
    case TS::Kind::T_dict:
    case TS::Kind::T_vec:
    case TS::Kind::T_keyset:
    case TS::Kind::T_vec_or_dict:
    case TS::Kind::T_any_array:
      return resolve_arraylike(ctx, kind, ts);
    case TS::Kind::T_shape:
      return resolve_shape(ctx, ts);
    case TS::Kind::T_typevar:
      return resolve_typevar(ctx, ts);
    case TS::Kind::T_unresolved:
      return resolve_unresolved(ctx, ts);
    case TS::Kind::T_typeaccess:
      return resolve_type_access(ctx, ts);
    case TS::Kind::T_reifiedtype:
      return Resolution{ TDictN, false };
    case TS::Kind::T_union:
      return resolve_union(ctx, ts);
    case TS::Kind::T_class_ptr:
    case TS::Kind::T_class_or_classname:
      // TODO(T199611023) Resolve generics here when we're ready to allow this
      return Resolution { dict_val(ts), true };
    case TS::Kind::T_int:
    case TS::Kind::T_bool:
    case TS::Kind::T_float:
    case TS::Kind::T_string:
    case TS::Kind::T_num:
    case TS::Kind::T_arraykey:
    case TS::Kind::T_void:
    case TS::Kind::T_null:
    case TS::Kind::T_nothing:
    case TS::Kind::T_noreturn:
    case TS::Kind::T_mixed:
    case TS::Kind::T_dynamic:
    case TS::Kind::T_nonnull:
    case TS::Kind::T_resource:
    case TS::Kind::T_class:
    case TS::Kind::T_interface:
    case TS::Kind::T_xhp:
    case TS::Kind::T_enum:
    case TS::Kind::T_trait:
    case TS::Kind::T_recursiveUnion:
      // These should be already caught by resolve().
      break;
  }
  always_assert(false);
}

Resolution resolve(ResolveCtx& ctx, SArray ts) {
  ITRACE(2, "resolve_type_structure: ({} {}) {}\n",
         ctx.selfCls ? ctx.selfCls->name->toCppString() : "-",
         ctx.thisCls ? ctx.thisCls->name->toCppString() : "-",
         staticArrayStreamer(ts));
  Trace::Indent _;

  // If the type-structure is trivially resolved, we can return it
  // immediately and skip the cache check.
  auto const kind = get_ts_kind(ts);
  if (kind_is_resolved(kind)) {
    ITRACE(2, "no resolution needed\n");
    return Resolution{ dict_val(ts), false };
  }

  if (auto const r = ctx.cache->enter(ctx, ts)) return *r;
  auto const r = resolve_impl(ctx, kind, ts);
  assertx(r.type.subtypeOf(BDictN));
  ctx.cache->exit(ctx, ts, r);
  ITRACE(
    2, "--> {}{}{}\n",
    show(r.type),
    r.mightFail ? " (might fail)" : "",
    r.contextSensitive ? " (ctx sensitive)" : ""
  );
  return r;
}

Resolution resolve_bespoke(ResolveCtx& ctx, SArray ts) {
  return maybe_make_bespoke(resolve(ctx, ts));
}

} // namespace

//////////////////////////////////////////////////////////////////////

Resolution resolve_type_structure(const ISS& env, SArray ts) {
  assertx(ts->isStatic());
  assertx(ts->isDictType());

  Cache cache;
  ResolvingMap resolving;
  ResolveCtx ctx{env.ctx, &env.index, &cache, &resolving};
  ctx.selfCls = env.ctx.cls;
  ctx.collect = &env.collect;
  return resolve_bespoke(ctx, ts);
}

Resolution resolve_type_structure(const IIndex& index,
                                  const php::Const& cns,
                                  const php::Class& thiz) {
  assertx(cns.kind == ConstModifierFlags::Kind::Type);
  assertx(cns.val.has_value());
  assertx(tvIsDict(*cns.val));
  assertx(val(*cns.val).parr->isStatic());

  if (cns.resolvedTypeStructure) {
    assertx(cns.resolvedTypeStructure->isStatic());
    assertx(cns.resolvedTypeStructure->isDictType());
    if (!cns.resolvedLocally &&
        (cns.contextInsensitive || cns.cls->tsame(thiz.name))) {
      return Resolution {
        dict_val(cns.resolvedTypeStructure),
        false,
        !cns.contextInsensitive
      };
    }
  }

  Cache cache;
  ResolvingMap resolving;
  ResolveCtx ctx{Context{}, &index, &cache, &resolving};
  ctx.selfCls = index.lookup_const_class(cns);
  ctx.thisCls = &thiz;

  // If the self class isn't present we need to be pessimistic.
  if (!ctx.selfCls) return Resolution { TDictN, true };
  return resolve_bespoke(ctx, val(*cns.val).parr);
}

Resolution resolve_type_structure(const IIndex& index,
                                  const CollectedInfo* collect,
                                  const php::TypeAlias& typeAlias) {
  assertx(typeAlias.typeStructure);
  assertx(typeAlias.typeStructure->isStatic());
  assertx(typeAlias.typeStructure->isDictType());

  if (typeAlias.resolvedTypeStructure) {
    assertx(typeAlias.resolvedTypeStructure->isStatic());
    assertx(typeAlias.resolvedTypeStructure->isDictType());
    if (!typeAlias.resolvedLocally) {
      return Resolution { dict_val(typeAlias.resolvedTypeStructure), false };
    }
  }

  Cache cache;
  ResolvingMap resolving;
  WithResolving withResolving(typeAlias.name, &typeAlias, &resolving);
  ResolveCtx ctx{Context{}, &index, &cache, &resolving};
  ctx.collect = collect;
  const StaticString& key = key_for_alias_kind(typeAlias.kind);
  return Builder::attach(resolve(ctx, typeAlias.typeStructure))
    .set(key, make_tv<KindOfPersistentString>(typeAlias.name))
    .finishTS();
}

//////////////////////////////////////////////////////////////////////

SString type_structure_name(SArray ts) {
  assertx(ts->isStatic());
  assertx(ts->isDictType());

  switch (get_ts_kind(ts)) {
    case TS::Kind::T_enum:
    case TS::Kind::T_trait:
    case TS::Kind::T_class:
    case TS::Kind::T_interface:
    case TS::Kind::T_unresolved:
      return get_ts_classname(ts);
    case TS::Kind::T_fun:
    case TS::Kind::T_tuple:
    case TS::Kind::T_darray:
    case TS::Kind::T_varray:
    case TS::Kind::T_varray_or_darray:
    case TS::Kind::T_dict:
    case TS::Kind::T_vec:
    case TS::Kind::T_keyset:
    case TS::Kind::T_vec_or_dict:
    case TS::Kind::T_any_array:
    case TS::Kind::T_shape:
    case TS::Kind::T_typevar:
    case TS::Kind::T_typeaccess:
    case TS::Kind::T_reifiedtype:
    case TS::Kind::T_union:
    case TS::Kind::T_int:
    case TS::Kind::T_bool:
    case TS::Kind::T_float:
    case TS::Kind::T_string:
    case TS::Kind::T_num:
    case TS::Kind::T_arraykey:
    case TS::Kind::T_void:
    case TS::Kind::T_null:
    case TS::Kind::T_nothing:
    case TS::Kind::T_noreturn:
    case TS::Kind::T_mixed:
    case TS::Kind::T_dynamic:
    case TS::Kind::T_nonnull:
    case TS::Kind::T_resource:
    case TS::Kind::T_xhp:
    case TS::Kind::T_recursiveUnion:
    case TS::Kind::T_class_ptr:
    case TS::Kind::T_class_or_classname:
      return nullptr;
  }
}

//////////////////////////////////////////////////////////////////////

void type_structure_references(SArray ts, SStringSet& names) {
  if (!ts) return;

  assertx(ts->isStatic());
  assertx(ts->isDictType());

  auto const onList = [&names] (SArray list) {
    if (!list) return;
    auto const size = list->size();
    for (size_t i = 0; i < size; ++i) {
      auto const tv = list->get(i);
      assertx(tvIsDict(tv));
      type_structure_references(val(tv).parr, names);
    }
  };

  auto const onShape = [&names] (SArray shape) {
    if (!shape) return;
    auto const fields = get_ts_fields(shape);
    auto const size = fields->size();
    for (size_t i = 0; i < size; ++i) {
      assertx(tvIsArrayLike(fields->nvGetVal(i)));
      auto const key = fields->nvGetKey(i);
      auto const value = val(fields->nvGetVal(i)).parr;

      auto const v = value->get(s_value);
      if (v.is_init() && tvIsDict(v)) {
        type_structure_references(val(v).parr, names);
      }

      if (!value->exists(s_is_cls_cns)) continue;
      assertx(tvIsString(key));
      auto const clsCns = val(key).pstr->toCppString();
      std::string clsNameStr, cnsNameStr;
      folly::split("::", clsCns, clsNameStr, cnsNameStr);
      names.emplace(makeStaticString(clsNameStr));
    }
  };

  auto const onRefinements = [&names, &onList] (SArray ts) {
    auto const refinements = get_ts_refinement_types_opt(ts);
    if (!refinements) return;
    auto const size = refinements->size();
    for (size_t i = 0; i < size; ++i) {
      auto const v = refinements->nvGetVal(i);
      assertx(tvIsDict(v));
      auto const memberArr = val(v).parr;
      auto const exact = memberArr->get(s_equals.get());
      if (exact.is_init()) {
        assertx(tvIsDict(exact));
        type_structure_references(val(exact).parr, names);
      }
      auto const upper = memberArr->get(s_as.get());
      if (upper.is_init()) {
        assertx(tvIsVec(upper));
        onList(val(upper).parr);
      }
      auto const lower = memberArr->get(s_super.get());
      if (lower.is_init()) {
        assertx(tvIsVec(lower));
        onList(val(lower).parr);
      }
    }
  };

  switch (get_ts_kind(ts)) {
    case TS::Kind::T_enum:
    case TS::Kind::T_trait:
    case TS::Kind::T_class:
    case TS::Kind::T_interface:
    case TS::Kind::T_unresolved:
    case TS::Kind::T_xhp:
      names.emplace(get_ts_classname(ts));
      onList(get_ts_generic_types_opt(ts));
      onRefinements(ts);
      break;
    case TS::Kind::T_fun:
      type_structure_references(get_ts_return_type(ts), names);
      onList(get_ts_param_types(ts));
      type_structure_references(get_ts_variadic_type_opt(ts), names);
      break;
    case TS::Kind::T_tuple:
      onList(get_ts_elem_types(ts));
      onList(get_ts_optional_elem_types_opt(ts));
      type_structure_references(get_ts_variadic_type_opt(ts), names);
      break;
    case TS::Kind::T_darray:
    case TS::Kind::T_varray:
    case TS::Kind::T_varray_or_darray:
    case TS::Kind::T_dict:
    case TS::Kind::T_vec:
    case TS::Kind::T_keyset:
    case TS::Kind::T_vec_or_dict:
    case TS::Kind::T_any_array:
      onList(get_ts_generic_types_opt(ts));
      break;
    case TS::Kind::T_shape:
      if (auto const splat = get_ts_splat_elem_types_opt(ts)) {
        // A shape splat: collect references from each element type structure.
        onList(splat);
      } else {
        onShape(ts);
        type_structure_references(get_ts_variadic_type_opt(ts), names);
      }
      break;
    case TS::Kind::T_typeaccess:
      names.emplace(get_ts_root_name(ts));
      break;
    case TS::Kind::T_union:
      onList(get_ts_union_types(ts));
      break;
    case TS::Kind::T_resource:
    case TS::Kind::T_recursiveUnion:
    case TS::Kind::T_class_ptr:
    case TS::Kind::T_class_or_classname:
    case TS::Kind::T_typevar:
    case TS::Kind::T_reifiedtype:
    case TS::Kind::T_int:
    case TS::Kind::T_bool:
    case TS::Kind::T_float:
    case TS::Kind::T_string:
    case TS::Kind::T_num:
    case TS::Kind::T_arraykey:
    case TS::Kind::T_void:
    case TS::Kind::T_null:
    case TS::Kind::T_nothing:
    case TS::Kind::T_noreturn:
    case TS::Kind::T_mixed:
    case TS::Kind::T_dynamic:
    case TS::Kind::T_nonnull:
      break;
  }
}

//////////////////////////////////////////////////////////////////////

}
