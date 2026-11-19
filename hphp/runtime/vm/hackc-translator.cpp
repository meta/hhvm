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
#include "hphp/runtime/vm/hackc-translator.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/opcodes.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/type-alias-emitter.h"
#include "hphp/runtime/vm/unit-gen-helpers.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/random.h"

#include <folly/Random.h>
#include <folly/Range.h>

namespace HPHP {

TRACE_SET_MOD(hackc_translate)

namespace {

using namespace HPHP::hackc;
using namespace HPHP::hackc::hhbc;


const StaticString s_OutOnly("__OutOnly");

struct TranslationState {

  explicit TranslationState(bool isSystemLib): isSystemLib(isSystemLib) {}

  void enterCatch() {
    handler.push(fe->bcPos());
  }

  void enterTry() {
    start.push(fe->bcPos());
  }

  void finishTryCatch() {
    assertx(!start.empty());
    assertx(!handler.empty());
    auto& eh = fe->addEHEnt();
    eh.m_base = start.top();
    eh.m_past = handler.top();
    eh.m_iterId = -1;
    eh.m_handler = handler.top();
    eh.m_end = fe->bcPos();
    start.pop();
    handler.pop();
  }

  Id mergeLitstr(const StringData* sd) {
    auto const id = ue->mergeLitstr(sd);
    litstrMap.emplace(id, sd);
    return id;
  }

  ///////////////////////////////////////////////////////////////////////////////
  // Opcode helpers

  void addLabelJump(const Dummy&);
  void addLabelJump(const Label& label);
  void addLabelJump(const Label label[2]);

  HPHP::RepoAuthType translateRepoAuthType(folly::StringPiece str);
  HPHP::MemberKey translateMemberKey(const hhbc::MemberKey& mkey);

  ///////////////////////////////////////////////////////////////////////////////

  // Locals are 0-indexed u32. In order to differentiate seeing no
  // locals vs seeing a local with id 0, maxUnnamed is max local
  // id + 1.
  void trackMaxUnnamed(uint32_t loc) {
    if (loc + 1 > maxUnnamed) maxUnnamed = loc + 1;
  }

  UnitEmitter* ue;
  FuncEmitter* fe{nullptr};
  PreClassEmitter* pce{nullptr};

  // Used for Execption Handler entry.
  jit::stack<Offset> handler;
  jit::stack<Offset> start;

  struct LabelInfo {
    // Target is the offset of the Label opcode
    Offset target;
    std::vector<Label> dvInits;
    // First offset is the immediate offset of a Label, like a jump target.
    // Second offset is the opcode containing the label immediate.(ie
    // a jmp instruction itself). This is used for computing jmp delta to
    // patch Label immediate.
    std::vector<std::pair<Offset,Offset>> sources;
  };

  std::map<Label, LabelInfo> labelMap;

  // In whole program mode it isn't possible to lookup a litstr in the global
  // table while emitting, so keep a lookaside of litstrs seen by the assembler.
  hphp_fast_map<Id, const StringData*> litstrMap;

  // Max local id encountered. Must be unsigned to match u32 representation
  // in HackC.
  uint32_t maxUnnamed{0};
  Location::Range srcLoc{-1,-1,-1,-1};
  std::vector<std::pair<hhbc::Label, Offset>> labelJumps;
  bool isSystemLib{false};
};

///////////////////////////////////////////////////////////////////////////////
// hhbc::Vector helpers

template <class T>
folly::Range<const T*> range(const Vector<T>& s) {
  return folly::range(s.data, s.data + s.len);
}

///////////////////////////////////////////////////////////////////////////////
// hhbc::Maybe helpers

template<typename T>
Optional<T> maybe(hackc::Maybe<T> m) {
  if (m.tag == hackc::Maybe<T>::Tag::Nothing) return std::nullopt;
  return m.Just._0;
}

template<typename T, typename Fn, typename ElseFn>
auto maybeOrElse(hackc::Maybe<T> m, Fn fn, ElseFn efn) {
  auto opt = maybe(m);
  return opt ? fn(opt.value()) : efn();
}

template<typename T, typename Fn>
auto maybeOrNullOptional(hackc::Maybe<T> m, Fn fn) {
  auto opt = maybe(m);
  return opt ? fn(opt.value()) : std::nullopt;
}

template<typename T, typename Fn>
auto maybeThen(hackc::Maybe<T> m, Fn fn) {
  auto opt = maybe(m);
  if (opt) fn(opt.value());
}

///////////////////////////////////////////////////////////////////////////////

namespace {
struct FatalUnitError : std::runtime_error {
  FatalUnitError(
    const std::string& msg,
    const StringData* filePath,
    Location::Range pos,
    FatalOp op)
    : std::runtime_error(msg)
    , filePath(filePath)
    , pos(pos)
    , op(op)
  {}

  const StringData* filePath;
  Location::Range pos;
  FatalOp op;
};

Location::Range locationFromSpan(const Span& span) {
  return Location::Range {
    static_cast<int>(span.line_begin), -1,
    static_cast<int>(span.line_end), -1
  };
}

Location::Range locationFromSrcLoc(const SrcLoc& srcloc) {
  return Location::Range {
    srcloc.line_begin, srcloc.col_begin,
    srcloc.line_end, srcloc.col_end
  };
}

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////
// hhbc::Str Helpers

std::string toString(const Vector<uint8_t>& str) {
  assertx(str.data != nullptr);
  return std::string{(const char*)str.data, str.len};
}

std::string toString(BytesId id) {
  auto const b = deref_bytes(id);
  return std::string{(const char*)b.data(), b.size()};
}

folly::StringPiece toStringPiece(BytesId id) {
  auto const b = deref_bytes(id);
  return folly::StringPiece{(const char*)b.data(), b.size()};
}

StringData* toStaticString(const Vector<uint8_t>& str) {
  return makeStaticString((char*)str.data, str.len);
}

StringData* toStaticString(BytesId id, uint32_t start = 0) {
  auto const b = deref_bytes(id);
  return makeStaticString((char*)b.data() + start, b.size() - start);
}

// TODO(@voork): NamedLocals are still prefixed with '$'.
StringData* toNamedLocalStaticString(StringId id) {
  return toStaticString(id, 1);
}

StringData* makeDocComment(const Vector<uint8_t>& str) {
  if (Cfg::Eval::GenerateDocComments) return toStaticString(str);
  return staticEmptyString();
}

///////////////////////////////////////////////////////////////////////////////

using kind = hhbc::TypedValue::Tag;

HPHP::TypedValue toTypedValue(const hackc::hhbc::TypedValue& tv) {
  auto hphp_tv = [&]() {
    switch(tv.tag) {
      case kind::Uninit:
        return make_tv<KindOfUninit>();
      case kind::Int:
        return make_tv<KindOfInt64>(tv.Int._0);
      case kind::Bool:
        return make_tv<KindOfBoolean>(tv.Bool._0);
      case kind::Float: {
        return make_tv<KindOfDouble>(tv.Float._0);
      }
      case kind::String: {
        auto const s = toStaticString(tv.String._0);
        return make_tv<KindOfString>(s);
      }
      case kind::EnumClassLabel: {
        auto const l = toStaticString(tv.EnumClassLabel._0);
        return make_tv<KindOfEnumClassLabel>(l);
      }
      case kind::Null:
        return make_tv<KindOfNull>();
      case kind::Vec: {
        VecInit v(tv.Vec._0._0->len);
        auto set = range(*tv.Vec._0._0);
        for (auto const& elt : set) {
          v.append(toTypedValue(elt));
        }
        auto tv = v.create();
        ArrayData::GetScalarArray(&tv);
        return make_tv<KindOfVec>(tv);
      }
      case kind::Dict: {
        DictInit d(tv.Dict._0._0->len);
        auto set = range(*tv.Dict._0._0);
        for (auto const& elt : set) {
          switch (elt.key.tag) {
            case kind::Int:
              d.set(elt.key.Int._0, toTypedValue(elt.value));
              break;
            case kind::String: {
              auto const s = toStaticString(elt.key.String._0);
              d.set(s, toTypedValue(elt.value));
              break;
            }
            case kind::LazyClass:{
              if (folly::Random::oneIn(
                    Cfg::Eval::RaiseClassConversionNoticeSampleRate, threadLocalRng64())) {
                raise_class_to_string_conversion_notice("dict key");
              }
              auto const s = toStaticString(elt.key.LazyClass._0._0);
              d.set(s, toTypedValue(elt.value));
              break;
            }
            default:
              always_assert(false);
          }
        }
        auto tv = d.create();
        ArrayData::GetScalarArray(&tv);
        return make_tv<KindOfDict>(tv);
      }
      case kind::Keyset: {
        KeysetInit k(tv.Keyset._0._0->len);
        auto set = range(*tv.Keyset._0._0);
        for (auto const& elt : set) {
          k.add(toTypedValue(elt));
        }
        auto tv = k.create();
        ArrayData::GetScalarArray(&tv);
        return make_tv<KindOfKeyset>(tv);
      }
      case kind::LazyClass: {
        auto const lc = LazyClassData::create(toStaticString(tv.LazyClass._0._0));
        return make_tv<KindOfLazyClass>(lc);
      }
    }
    not_reached();
  }();
  auto avail = Cfg::Eval::AssemblerMaxScalarSize;
  checkSize(hphp_tv, avail);
  return hphp_tv;
}

///////////////////////////////////////////////////////////////////////////////
// Field translaters

void translateUserAttributes(const Vector<hhbc::Attribute>& attributes,
                             UserAttributeMap& userAttrs) {
  Trace::Indent indent;
  auto attrs = range(attributes);
  for (auto const& attr : attrs) {
    auto const name = toStaticString(attr.name._0);
    VecInit v(attr.arguments.len);
    auto args = range(attr.arguments);
    for (auto const& arg : args) {
      v.append(toTypedValue(arg));
    }
    auto tv = v.create();
    ArrayData::GetScalarArray(&tv);
    userAttrs[name] = make_tv<KindOfVec>(tv);
  };
}

void translateSymbolInfo(const Vector<StringId>& missing,
    const Vector<StringId>& error, UnitEmitter& ue) {
  Trace::Indent indent;
  for (auto const& m : range(missing)) {
    ue.m_missingSyms.emplace_back(toStaticString(m));
  }
  for (auto const& e : range(error)) {
    ue.m_errorSyms.emplace_back(toStaticString(e));
  }
}

template<bool isEnum=false>
std::pair<const StringData*, TypeConstraint> translateTypeInfo(const hhbc::TypeInfo& t) {
  auto const user_type = maybeOrElse(t.user_type,
    [&](BytesId s) {return toStaticString(s);},
    [&]() {return staticEmptyString();});

  auto const type_name = isEnum
    ? user_type
    : maybeOrElse(t.type_constraint.name,
        [&](BytesId s) {return toStaticString(s);},
        [&]() {return nullptr;});

  auto flags = t.type_constraint.flags;
  return std::make_pair(user_type, TypeConstraint{type_name, flags});
}

void translateTypedef(TranslationState& ts, const hhbc::Typedef& t) {
  UserAttributeMap userAttrs;
  translateUserAttributes(t.attributes, userAttrs);
  auto attrs = t.attrs;
  assertx(IMPLIES(ts.isSystemLib, attrs & AttrPersistent));
  auto const name = toStaticString(t.name._0);

  auto const tis = range(t.type_info_union);
  assertx(!tis.empty());

  auto value = [&] {
    if (tis.size() == 1) {
      return translateTypeInfo(tis[0]).second;
    }
    if (Cfg::Eval::TreatCaseTypesAsMixed) {
      return TypeConstraint::makeMixed();
    }

    TinyVector<TypeConstraint, 1> parts;
    parts.reserve(tis.size());
    for (auto const& ti : tis) {
      auto const ty = translateTypeInfo(ti).second;
      auto const tname = ty.typeName();
      if (tname && !tname->empty()) {
        parts.emplace_back(ty);
      } else {
        parts.emplace_back(TypeConstraint::makeMixed());
      }
    }
    try {
      return TypeConstraint::makeUnion(name, parts);
    } catch (FatalErrorException& ex) {
      throw FatalUnitError(
        ex.what(), ts.ue->m_filepath,
        locationFromSpan(t.span),
        FatalOp::Runtime
      );
    }
  }();

  auto tys = toTypedValue(t.type_structure);
  assertx(isArrayLikeType(tys.m_type));
  tvAsVariant(&tys).setEvalScalar();

  auto te = ts.ue->newTypeAliasEmitter(name->toCppString());
  te->init(
    t.span.line_begin,
    t.span.line_end,
    attrs,
    value,
    t.case_type ? AliasKind::CaseType : AliasKind::TypeAlias,
    ArrNR{tys.m_data.parr},
    Array{}
  );

  te->setUserAttributes(userAttrs);
}

template<bool isType>
void addConstant(TranslationState& ts,
                 const StringData* name,
                 Optional<hhbc::TypedValue> tv,
                 bool isAbstract) {
  auto const kind = isType
    ? ConstModifierFlags::Kind::Type
    : ConstModifierFlags::Kind::Value;

  if (!tv) {
    assertx(isAbstract);
    ts.pce->addAbstractConstant(name, kind, false);
    return;
  }

  auto tvInit = toTypedValue(tv.value());
  tvAsVariant(&tvInit).setEvalScalar();
  ts.pce->addConstant(name,
                      nullptr,
                      &tvInit,
                      Array{},
                      kind,
                      PreClassEmitter::Const::Invariance::None,
                      false,
                      isAbstract);
}

TypeIntersectionConstraint upperBoundsHelper(
  const UpperBoundMap& ubs,
  const UpperBoundMap& classUbs,
  const hphp_fast_set<const StringData*>& shadowedTParams,
  const TypeConstraint& tc,
  bool hasReifiedGenerics
) {
  auto currUBs = getRelevantUpperBounds(tc, ubs, classUbs, shadowedTParams);
  if (currUBs.size() != 1 || hasReifiedGenerics) {
    currUBs.emplace(currUBs.begin(), tc);
  }
  assertx(!currUBs.empty());
  return TypeIntersectionConstraint(std::move(currUBs));
}

template <bool checkPce=false>
std::pair<bool, TypeIntersectionConstraint> upperBoundsHelper(
  TranslationState& ts,
  const UpperBoundMap& ubs,
  const UpperBoundMap& classUbs,
  const hphp_fast_set<const StringData*>& shadowedTParams,
  const TypeConstraint& tc,
  const UserAttributeMap& userAttrs
) {
  auto const hasReifiedGenerics = [&]() {
    if (userAttrs.find(s___Reified.get()) != userAttrs.end()) return true;
    if (checkPce) {
      return ts.pce->userAttributes().find(s___Reified.get()) !=
             ts.pce->userAttributes().end();
    }
    return false;
  }();

  return std::make_pair(
    hasReifiedGenerics,
    upperBoundsHelper(ubs, classUbs, shadowedTParams, tc, hasReifiedGenerics)
  );
}

void translateClassConstant(TranslationState& ts, const hhbc::Constant& c) {
  auto const name = toStaticString(c.name._0);
  auto const tv = maybe(c.value);
  addConstant<false>(ts, name, tv, c.attrs & AttrAbstract);
}

void translateTypeConstant(TranslationState& ts, const hhbc::TypeConstant& c) {
  auto const name = toStaticString(c.name);
  auto const tv = maybe(c.initializer);
  addConstant<true>(ts, name, tv, c.is_abstract);
}

void translateCtxConstant(TranslationState& ts, const hhbc::CtxConstant& c) {
  auto const name = toStaticString(c.name);
  bool isAbstract = c.is_abstract;
  auto coeffects = PreClassEmitter::Const::CoeffectsVec{};

  auto recognized = range(c.recognized);
  for (auto const& r : recognized) {
    coeffects.push_back(toStaticString(r));
  }
  auto unrecognized = range(c.unrecognized);
  for (auto const& u : unrecognized) {
    coeffects.push_back(toStaticString(u));
  }

  // T112974443: temporarily drop the abstract ones until runtime is fixed
  if (isAbstract && !Cfg::Eval::EnableAbstractContextConstants) return;
  DEBUG_ONLY auto added =
    ts.pce->addContextConstant(name, std::move(coeffects), isAbstract);
  assertx(added);
}

void translateProperty(TranslationState& ts, const hhbc::Property& p, const UpperBoundMap& classUbs) {
  UserAttributeMap userAttributes;
  translateUserAttributes(p.attributes, userAttributes);

  auto const heredoc = maybeOrElse(p.doc_comment,
    [&](const Vector<uint8_t>& s) {return makeDocComment(s);},
    [&]() {return staticEmptyString();});

  auto [userTy, typeConstraint] = translateTypeInfo(p.type_info);

   auto const hasReifiedGenerics =
       userAttributes.find(s___Reified.get()) != userAttributes.end();
  // T112889109: Passing in {} as the third argument here exposes a gcc compiler bug.
  auto tic = upperBoundsHelper(
      classUbs, classUbs, {}, typeConstraint, hasReifiedGenerics);

  auto const tv = maybeOrElse(p.initial_value,
    [&](hhbc::TypedValue& s) {
      auto hphpTv = toTypedValue(s);
      tvAsVariant(&hphpTv).setEvalScalar();
      return hphpTv;
    },
    [&]() {return make_tv<KindOfNull>();});

  auto const name = toStaticString(p.name._0);
  ITRACE(2, "Translating property {} {}\n", name, tv.pretty());

  assertx(!tic.isTop());
  ts.pce->addProperty(
    name,
    p.flags,
    userTy,
    std::move(tic),
    heredoc,
    &tv,
    HPHP::RepoAuthType{},
    userAttributes
  );
}

void translateClassBody(TranslationState& ts,
                        const hhbc::ClassImpl<hhbc::BcRepr>& c,
                        const UpperBoundMap& classUbs) {
  auto props = range(c.properties);
  for (auto const& p : props) {
    translateProperty(ts, p, classUbs);
  }
  auto constants = range(c.constants);
  for (auto const& cns : constants) {
    translateClassConstant(ts, cns);
  }
  auto tconstants = range(c.type_constants);
  for (auto const& t : tconstants) {
    translateTypeConstant(ts, t);
  }
  auto cconstants = range(c.ctx_constants);
  for (auto const& ctx : cconstants) {
    translateCtxConstant(ts, ctx);
  }
}

void translateUbs(const hhbc::UpperBound& ub, UpperBoundMap& ubs) {
  auto const& name = toStaticString(ub.name._0);

  auto infos = range(ub.bounds);
  for (auto const& i : infos) {
    ubs[name].emplace_back(translateTypeInfo(i).second);
  }
}

void translateEnumType(TranslationState& ts, const Maybe<hhbc::TypeInfo>& t) {
  auto const tOpt = maybe(t);
  if (tOpt) {
    ts.pce->setEnumBaseTy(translateTypeInfo<true>(tOpt.value()).second);
  }
}

void translateRequirements(TranslationState& ts, const hhbc::Requirement& req) {
  auto const name = toStaticString(req.name._0);
  auto const requirementKind = [&] {
    switch (req.kind) {
      case hhbc::TraitReqKind::MustExtend: return PreClass::RequirementExtends;
      case hhbc::TraitReqKind::MustImplement: return PreClass::RequirementImplements;
      case hhbc::TraitReqKind::MustBeClass: return PreClass::RequirementClass;
      case hhbc::TraitReqKind::MustBeAs: return PreClass::RequirementThisAs;
    }
    not_reached();
  }();

  ts.pce->addClassRequirement(PreClass::ClassRequirement(name, requirementKind));
}

HPHP::RepoAuthType TranslationState::translateRepoAuthType(folly::StringPiece str) {
  using T = HPHP::RepoAuthType::Tag;

  auto const [tag, what] = [&] {
    #define TAG(x, name) \
      if (str.startsWith(name)) return std::make_pair(T::x, name);
      REPO_AUTH_TYPE_TAGS(TAG)
    #undef TAG
    not_reached();
  }();

  if (HPHP::RepoAuthType::tagHasName(tag)) {
    str.removePrefix(what);
    auto const name = makeStaticString(str.data());
    mergeLitstr(name);
    return HPHP::RepoAuthType{tag, name};
  }

  return HPHP::RepoAuthType{tag};
}

////////////////////////////////////////////////////////////////////

// Do nothing for dummy values. Used for MemoGetEager where both labels
// are passed through first BA.
void TranslationState::addLabelJump(const Dummy&) {}

void TranslationState::addLabelJump(const Label& label) {
  labelJumps.emplace_back(label, fe->bcPos());
  fe->emitInt32(0);
}

// MemoGetEager passes both labels in first BA opcode
void TranslationState::addLabelJump(const Label label[2]) {
  addLabelJump(label[0]);
  addLabelJump(label[1]);
}

// labelOpcodeOff is the offset of the label opcode.
// immOff is the offset where the label appears as an immediate, ie as a Jump target.
void updateLabelSourceforJump(TranslationState& ts,
                              const Label& label,
                              Offset immOff,
                              Offset labelOpcodeOff) {
  auto& labelInfo = ts.labelMap[label];
  labelInfo.sources.emplace_back(immOff, labelOpcodeOff);
}

HPHP::MemberKey TranslationState::translateMemberKey(const hhbc::MemberKey& mkey) {
  switch (mkey.tag) {
    case hhbc::MemberKey::Tag::EC: {
      auto const iva = static_cast<int32_t>(mkey.EC._0);
      return HPHP::MemberKey(MemberCode::MEC, iva, mkey.EC._1);
    }
    case hhbc::MemberKey::Tag::PC: {
      auto const iva = static_cast<int32_t>(mkey.PC._0);
      return HPHP::MemberKey(MemberCode::MPC, iva, mkey.PC._1);
    }
    case hhbc::MemberKey::Tag::EL: {
      auto const id = static_cast<int32_t>(mkey.EL._0.idx);
      NamedLocal local{ id, id };
      trackMaxUnnamed(id);
      return HPHP::MemberKey(MemberCode::MEL, local, mkey.EL._1);
    }
    case hhbc::MemberKey::Tag::PL: {
      auto const id = static_cast<int32_t>(mkey.PL._0.idx);
      NamedLocal local{ id, id };
       trackMaxUnnamed(id);
      return HPHP::MemberKey(MemberCode::MPL, local, mkey.PL._1);
    }
    case hhbc::MemberKey::Tag::ET:
      return HPHP::MemberKey(
        MemberCode::MET,
        toStaticString(mkey.ET._0),
        mkey.ET._1
      );
    case hhbc::MemberKey::Tag::PT: {
      auto const name = toStaticString(mkey.PT._0._0);
      return HPHP::MemberKey(MemberCode::MPT, name, mkey.PT._1);
    }
    case hhbc::MemberKey::Tag::QT: {
      auto const name = toStaticString(mkey.QT._0._0);
      return HPHP::MemberKey(MemberCode::MQT, name, mkey.QT._1);
    }
    case hhbc::MemberKey::Tag::EI:
      return HPHP::MemberKey(MemberCode::MEI, mkey.EI._0, mkey.EI._1);
    case hhbc::MemberKey::Tag::W:
      return HPHP::MemberKey();
  }
  not_reached();
}

void handleIVA(TranslationState& ts, const uint32_t& iva) {
  ts.fe->emitIVA(iva);
}

void handleIVA(TranslationState& ts, const Local& loc) {
  ts.trackMaxUnnamed(loc.idx);
  ts.fe->emitIVA(loc.idx);
}

void handleLA(TranslationState& ts, const Local& loc) {
  ts.trackMaxUnnamed(loc.idx);
  ts.fe->emitIVA(loc.idx);
}

void handleILA(TranslationState& ts, const Local& loc) {
  ts.trackMaxUnnamed(loc.idx);
  ts.fe->emitIVA(loc.idx);
}

void handleNLA(TranslationState& ts, const Local& local) {
  auto const id = static_cast<int32_t>(local.idx);
  NamedLocal loc{ id, id };
  ts.trackMaxUnnamed(id);
  ts.fe->emitNamedLocal(loc);
}

void handleBLA(TranslationState& ts, const Vector<Label>& labels) {
  ts.fe->emitIVA(labels.len);
  auto targets = range(labels);
  for (auto const& t : targets) {
    ts.addLabelJump(t);
  }
}

template <typename T>
void handleBA(TranslationState& ts, const T& labels) {
  ts.addLabelJump(labels);
}

void handleITA(TranslationState& ts, const hhbc::IterArgs& ita) {
  HPHP::IterArgs ia(
    ita.flags,
    ita.iter_id.idx
  );
  encodeIterArgs(*ts.fe, ia);
}

void handleVSA(TranslationState& ts, const Vector<BytesId>& arr) {
  ts.fe->emitIVA(arr.len);
  auto strings = range(arr);
  for (auto const& s : strings) {
    auto const str = toStaticString(s);
    ts.fe->emitInt32(ts.mergeLitstr(str));
  }
}

void handleRATA(TranslationState& ts, const hhbc::RepoAuthType& rat) {
  auto str = toStringPiece(rat);
  auto const rat_ = ts.translateRepoAuthType(str);
  encodeRAT(*ts.fe, *ts.ue, rat_);
}

void handleKA(TranslationState& ts, const hhbc::MemberKey& mkey) {
  auto const mkey_ = ts.translateMemberKey(mkey);
  encode_member_key(mkey_, *ts.fe, *ts.ue);
}

inline bool operator==(const hhbc::Local& a, const hhbc::Local& b) {
  return a.idx == b.idx;
}

inline bool operator==(const hhbc::LocalRange& a, const hhbc::LocalRange& b) {
  return a.start == b.start && a.len == b.len;
}

void handleLAR(TranslationState& ts, const hhbc::LocalRange& lar) {
  auto const lar_ = [&]() {
    if (lar == LocalRange_EMPTY) {
      return HPHP::LocalRange{0, 0};
    }
    auto const firstLoc = lar.start.idx;
    auto const len = lar.len;
    ts.trackMaxUnnamed(firstLoc + len - 1);
    return HPHP::LocalRange{firstLoc, len};
  }();
  encodeLocalRange(*ts.fe, lar_);
}

void handleAA(TranslationState& ts, const hhbc::TypedValue& value) {
  // hhbc::Unit does not have an adata table. Intern each array value,
  // and use the id assigned by UnitEmitter:mergeArray().
  auto tv = toTypedValue(value);
  assertx(isArrayLikeType(tv.type()));
  auto arr = tv.val().parr;
  ArrayData::GetScalarArray(&arr);
  ts.fe->emitInt32(ts.ue->mergeArray(arr));
}

void handleFCA(TranslationState& ts, const hhbc::FCallArgs& fca) {
  auto const internalFlags =
    static_cast<FCallArgsFlags>(fca.flags & HPHP::FCallArgs::kInternalFlags);
  auto const fcaBase = FCallArgsBase(internalFlags, fca.num_args, fca.num_rets);
  auto const async_eager_target = fca.async_eager_target;
  auto const io = fca.inouts;
  auto const readonly = fca.readonly;
  auto const ctx = toStaticString(fca.context);
  auto const numBytes = (fca.num_args + 7) / 8;
  encodeFCallArgs(
    *ts.fe, fcaBase,
    io.len,
    [&] {
      auto inouts = range(io);
      auto result = std::make_unique<uint8_t[]>(numBytes);
      for (int i = 0; i < fca.num_args; i++) {
        result[i / 8] |= inouts[i] << (i % 8);
      }
      for (int i = 0; i < numBytes; i++) {
        ts.fe->emitByte(result[i]);
      }
    },
    readonly.len,
    [&] {
      auto refs = range(readonly);
      auto result = std::make_unique<uint8_t[]>(numBytes);
      for (int i = 0; i < fca.num_args; i++) {
        result[i / 8] |= refs[i] << (i % 8);
      }
      for (int i = 0; i < numBytes; i++) {
        ts.fe->emitByte(result[i]);
      }
    },
    fca.flags & FCallArgsFlags::HasNamedArgs,
    [&] {
      VecInit names(fca.named_arg_names.len);
      auto set = range(fca.named_arg_names);
      for (auto const& elt : set) {
        names.append(make_tv<KindOfString>(toStaticString(elt)));
      }
      auto name_arr = names.create();
      ArrayData::GetScalarArray(&name_arr);
      ts.fe->emitInt32(ts.ue->mergeArray(name_arr));
    },
    fca.flags & FCallArgsFlags::HasAsyncEagerOffset,
    [&] { ts.addLabelJump(async_eager_target); },
    fca.flags & FCallArgsFlags::ExplicitContext,
    [&] {
      auto const id = ts.mergeLitstr(ctx);
      ts.litstrMap.emplace(id, ctx);
      ts.fe->emitInt32(id);
    }
  );
  if (!fcaBase.hasUnpack()) {
    ts.fe->containsCalls = true;
  }
}

StringId getStrfromSA(const ClassName& c) {
  return c._0;
}
StringId getStrfromSA(const FunctionName& f) {
  return f._0;
}
StringId getStrfromSA(const MethodName& m) {
  return m._0;
}
StringId getStrfromSA(const ConstName& c) {
  return c._0;
}
StringId getStrfromSA(const PropName& p) {
  return p._0;
}
BytesId getStrfromSA(const BytesId s) {
  return s;
}

template <typename T>
void handleSA(TranslationState& ts, const T& str) {
  auto const sd = toStaticString(getStrfromSA(str));
  auto const id = ts.mergeLitstr(sd);
  ts.fe->emitInt32(id);
}

void handleIA(TranslationState& ts, const IterId& id) {
  ts.fe->emitIVA(id.idx);
}

void handleDA(TranslationState& ts, const FloatBits& fb) {
  ts.fe->emitDouble(fb);
}

void handleI64A(TranslationState& ts, const int64_t& i) {
  ts.fe->emitInt64(i);
}

template<typename T>
void handleOA(TranslationState& ts, T subop) {
  ts.fe->emitByte(static_cast<uint8_t>(subop));
}

//////////////////////////////////////////////////////////////////////

#define IMM_NA
#define IMM_ONE(t) handle##t(ts, body._0);
#define IMM_TWO(t1, t2) IMM_ONE(t1); ++immIdx; handle##t2(ts, body._1)
#define IMM_THREE(t1, t2, t3) IMM_TWO(t1, t2); ++immIdx; handle##t3(ts, body._2)
#define IMM_FOUR(t1, t2, t3, t4) IMM_THREE(t1, t2, t3); ++immIdx; handle##t4(ts, body._3)
#define IMM_FIVE(t1, t2, t3, t4, t5) IMM_FOUR(t1, t2, t3, t4); ++immIdx; handle##t5(ts, body._4)
#define IMM_SIX(t1, t2, t3, t4, t5, t6) IMM_FIVE(t1, t2, t3, t4, t5); ++immIdx; handle##t6(ts, body._5)

#define handleOA(imm) handleOA<imm>
#define handleSLA(ts, imm) do {                           \
  auto const targets = body._1;                           \
  auto const cases = body._0;                             \
  ts.fe->emitIVA(cases.len);                              \
  for (int i = 0; i < cases.len-1; i++) {                 \
    auto const caseName = toStaticString(cases.data[i]);  \
    auto const caseId = ts.mergeLitstr(caseName);         \
    ts.fe->emitInt32(caseId);                             \
    ts.addLabelJump(targets.data[i]);                     \
  }                                                       \
  ts.fe->emitInt32(-1);                                   \
  ts.addLabelJump(targets.data[cases.len-1]);             \
} while (0)

#define MAYBE_IMM_ONE(...)
#define MAYBE_IMM_TWO(...)
#define MAYBE_IMM_THREE(...)
#define MAYBE_IMM_FOUR(...)
#define MAYBE_IMM_FIVE(...)
#define MAYBE_IMM_SIX(...)
#define MAYBE_IMM_NA NO_

#define BODY(name) UNUSED auto const body = o.name;
#define NO_BODY(...)

// Conditionally create a body variable if this opcode has inputs we need to
// access off of it. For opcodes with no inputs (NA), there is no body struct
// created, so calling o.name as above will fail. You can see the C-headers
// for all opcodes in P1053747912.
//
// Use __VA_ARGS__ as a workaround to expand MAYBE_IMM_#imm.
// Without this, it would expand to:
// MAYBE_IMM_NABODY(Await)  \\ garbage
// instead of:
// MAYBE_IMM_NA -> NO_ -> NO_BODY(Await)
#define GEN_BODY2(id, name) id##BODY(name)
#define GEN_BODY(...) GEN_BODY2(__VA_ARGS__)

// Rust cbindgen generates C headers for opcode structs that look like
// P1053747912. These headers are stored in hhbc-unit.h which is included
// in a few places in HHVM. We generate emitters for the opcodes
// using opcodes.h, which is also what HackC uses to generate these rust
// structs, with a few differences noted in hhbc.rs.
#define O(name, imm, pop, push, flags)                                 \
  void translateOpcode##name(TranslationState& ts, const Opcode& o) {  \
    ITRACE(5, "translate Opcode {} \n", #name);                        \
    UNUSED const Offset curOpcodeOff = ts.fe->bcPos();                 \
    ts.fe->emitOp(Op##name);                                           \
                                                                       \
    UNUSED size_t immIdx = 0;                                          \
    GEN_BODY(MAYBE_IMM_##imm, name)                                    \
    IMM_##imm;                                                         \
                                                                       \
    for (auto& kv : ts.labelJumps) {                                   \
      updateLabelSourceforJump(ts, kv.first, kv.second, curOpcodeOff); \
    }                                                                  \
                                                                       \
    /* Record source location. */                                      \
    ts.fe->recordSourceLocation(ts.srcLoc, curOpcodeOff);              \
    ts.labelJumps.clear();                                             \
  }
OPCODES

#undef O
#undef handleSLA
#undef handleOA

using translatorFunc = void (*)(TranslationState& ts, const Opcode& o);
hphp_hash_map<Opcode::Tag, translatorFunc> opcodeTranslators;

void initializeMap() {
  #define O(name, imm, pop, push, flags) \
    opcodeTranslators[Opcode::Tag::name] = translateOpcode##name;
  OPCODES
  #undef O
}

struct Initializer {
  Initializer() { initializeMap(); }
} initializer;

//////////////////////////////////////////////////////////////////////

void translateOpcodeInstruction(TranslationState& ts, const Opcode& o) {
  return opcodeTranslators[o.tag](ts, o);
}

void translateSrcLoc(TranslationState& ts, const SrcLoc& srcloc) {
  ts.srcLoc = locationFromSrcLoc(srcloc);
}

void translatePseudoInstruction(TranslationState& ts, const Pseudo& p) {
  switch (p.tag) {
    case Pseudo::Tag::Break:
    case Pseudo::Tag::Continue:
      return;
    case Pseudo::Tag::Label:
      ts.labelMap[p.Label._0].target = ts.fe->bcPos();
      return;
    case Pseudo::Tag::SrcLoc:
      return translateSrcLoc(ts, p.SrcLoc._0);
    // .try {
    case Pseudo::Tag::TryCatchBegin:
      return ts.enterTry();
    // } .catch {
    case Pseudo::Tag::TryCatchMiddle:
      return ts.enterCatch();
    // }
    case Pseudo::Tag::TryCatchEnd:
      return ts.finishTryCatch();
  }
}

void translateInstruction(TranslationState& ts, const Instruct& i) {
  switch (i.tag) {
    case Instruct::Tag::Opcode:
      return translateOpcodeInstruction(ts, i.Opcode._0);
    case Instruct::Tag::Pseudo:
      return translatePseudoInstruction(ts, i.Pseudo._0);
  }
}

void translateDefaultParameterValue(TranslationState& ts,
                                    const hhbc::DefaultValue& dv,
                                    Func::ParamInfo& param) {
  auto const str = toStaticString(dv.expr);
  ts.labelMap[dv.label].dvInits.push_back(ts.fe->params.size());
  auto id = ts.ue->mergeLitstr(str);
  parse_default_value(param, id, str);
}

void translateParameter(TranslationState& ts,
                        const UpperBoundMap& ubs,
                        const UpperBoundMap& classUbs,
                        const hphp_fast_set<const StringData*>& shadowedTParams,
                        bool hasReifiedGenerics,
                        const hhbc::ParamEntry& e) {
  auto& p = e.param;
  Func::ParamInfo param;
  translateUserAttributes(p.user_attributes, param.userAttributes);
  if (p.is_variadic || p.is_splat) {
    assertx(ts.fe->attrs & AttrVariadicParam);
    param.setFlag(Func::ParamInfo::Flags::Variadic);
  }
  if (p.is_inout) {
    param.setFlag(Func::ParamInfo::Flags::InOut);

    if (ts.fe->isNative && param.userAttributes.contains(s_OutOnly.get())) {
      param.setFlag(Func::ParamInfo::Flags::OutOnly);
    }
  }
  if (p.is_readonly) param.setFlag(Func::ParamInfo::Flags::Readonly);
  if (p.is_optional) param.setFlag(Func::ParamInfo::Flags::Optional);
  if (p.is_splat) param.setFlag(Func::ParamInfo::Flags::Splat);
  if (p.is_named) param.setFlag(Func::ParamInfo::Flags::Named);

  TypeConstraint tc;
  std::tie(param.userType, tc) = maybeOrElse(p.type_info,
    [&](hhbc::TypeInfo& ti) {
      auto [userType, tc] = translateTypeInfo(ti);
      return std::make_pair(ts.ue->mergeLitstr(userType), tc);
    },
    [&]() {return std::make_pair(kInvalidId, TypeConstraint{});});

  param.typeConstraints = upperBoundsHelper(
    ubs, classUbs, shadowedTParams, tc, hasReifiedGenerics);
  assertx(!param.typeConstraints.isTop());
  auto const dv = maybe(e.dv);
  if (dv) translateDefaultParameterValue(ts, dv.value(), param);
  auto const name = toNamedLocalStaticString(p.name);
  ts.fe->appendParam(name, param);
}

void translateFunctionBody(TranslationState& ts,
                           const hhbc::BodyImpl<hhbc::BcRepr>& b,
                           const UpperBoundMap& ubs,
                           const UpperBoundMap& classUbs,
                           const hphp_fast_set<const StringData*>& shadowedTParams,
                           bool hasReifiedGenerics) {
  ts.fe->isMemoizeWrapper = b.is_memoize_wrapper | b.is_memoize_wrapper_lsb;
  ts.fe->isMemoizeWrapperLSB = b.is_memoize_wrapper_lsb;
  ts.fe->setNumIterators(b.num_iters);
  auto params = range(b.repr.params);
  for (auto const& p : params) {
    translateParameter(ts, ubs, classUbs, shadowedTParams, hasReifiedGenerics, p);
  }

  auto instrs = range(b.repr.instrs);
  for (auto const& instr : instrs) {
    translateInstruction(ts, instr);
  }

  auto const dc = maybe(b.doc_comment);
  if (dc) ts.fe->docComment = makeDocComment(dc.value());

  // Parsing parameters must come before decl_vars
  auto decl_vars = range(b.repr.decl_vars);
  for (auto const& dv : decl_vars) {
    auto const dvName = toNamedLocalStaticString(dv);
    ts.fe->allocVarId(dvName);
  }

  for (auto const& label : ts.labelMap) {
    for (auto const& dvinit : label.second.dvInits) {
      ts.fe->params[dvinit].funcletOff = label.second.target;
    }

    for (auto const& source : label.second.sources) {
      // source.second is the label opcode offset
      // source.first is the offset of where the label is used as an immediate
      ts.fe->emitInt32(label.second.target - source.second, source.first);
    }
  }

  ts.fe->maxStackCells = b.repr.stack_depth;

  // finish function
  while (ts.fe->numLocals() < ts.maxUnnamed) {
    ts.fe->allocUnnamedLocal();
  }

  ts.fe->finish();
  ts.labelMap.clear();
  assertx(ts.start.empty());
  assertx(ts.handler.empty());
  ts.maxUnnamed = 0;
}

static StaticString s_native("__Native");


/*
 * Checks whether the current function is native by looking at the user
 * attribute map and sets the isNative flag accoringly
 * If the give function is op code implementation, then isNative is not set
 */
void checkNative(TranslationState& ts) {
  if (ts.fe->userAttributes.contains(s_native.get())) {
    ts.fe->isNative =
      !(ts.fe->parseNativeAttributes(ts.fe->attrs) & Native::AttrOpCodeImpl);
  }
}

void translateCoeffects(TranslationState& ts, const hhbc::Coeffects& coeffects) {
  auto static_coeffects = range(coeffects.static_coeffects);
  for (auto const& c : static_coeffects) {
    auto const coeffectStr = CoeffectsConfig::fromHackCCtx(c);
    ts.fe->staticCoeffects.push_back(makeStaticString(coeffectStr));
  }

  auto unenforced_coeffects = range(coeffects.unenforced_static_coeffects);
  for (auto const& c : unenforced_coeffects) {
    ts.fe->staticCoeffects.push_back(toStaticString(c));
  }

  auto fun_param = range(coeffects.fun_param);
  for (auto const& f : fun_param) {
    ts.fe->coeffectRules.emplace_back(
      CoeffectRule(CoeffectRule::FunParam{}, f));
  }

  auto cc_param = range(coeffects.cc_param);
  for (auto const& c : cc_param) {
    ts.fe->coeffectRules.emplace_back(
      CoeffectRule(CoeffectRule::CCParam{}, c.index, toStaticString(c.ctx_name)));
  }

  auto cc_this_vec = range(coeffects.cc_this);
  for (auto const& cc_this : cc_this_vec) {
    auto const& types = cc_this.types;

    std::vector<PackedStringPtr> names;
    names.reserve(types.len - 1);
    for (int i = 0; i < types.len - 1; i++) {
      names.push_back(toStaticString(types.data[i]));
    }
    auto const ctx_name = toStaticString(types.data[types.len - 1]);
    ts.fe->coeffectRules.emplace_back(
        CoeffectRule(CoeffectRule::CCThis{}, names, ctx_name));
  }

  auto cc_reified_vec = range(coeffects.cc_reified);
  for (auto const& cc_reified : cc_reified_vec) {
    auto const isClass = cc_reified.is_class;
    auto const pos = cc_reified.index;
    auto const& types = cc_reified.types;

    std::vector<PackedStringPtr> names;
    names.reserve(types.len - 1);
    for (int i = 0; i < types.len - 1; i++) {
      names.push_back(toStaticString(types.data[i]));
    }

    auto const ctx_name = toStaticString(types.data[types.len - 1]);
     ts.fe->coeffectRules.emplace_back(
        CoeffectRule(CoeffectRule::CCReified{}, isClass, pos, names, ctx_name));
  }

  if (coeffects.closure_parent_scope) {
    ts.fe->coeffectRules.emplace_back(CoeffectRule(CoeffectRule::ClosureParentScope{}));
  }

  if (coeffects.generator_this) {
    ts.fe->coeffectRules.emplace_back(CoeffectRule(CoeffectRule::GeneratorThis{}));
  }

if (coeffects.caller) {
    ts.fe->coeffectRules.emplace_back(CoeffectRule(CoeffectRule::Caller{}));
  }
}

std::pair<std::vector<PackedStringPtr>, hphp_fast_set<const StringData*>>
translateTParamInfo(const Vector<TParamInfo>& tParamInfo) {
  std::vector<PackedStringPtr> typeParamNames;
  hphp_fast_set<const StringData*> shadowedTParams;
  for (auto const& t : range(tParamInfo)) {
    auto const name = toStaticString(t.name._0);
    typeParamNames.emplace_back(name);
    if (t.shadows_class_tparam) {
      shadowedTParams.insert(name);
    }
  }
  return std::make_pair(typeParamNames, shadowedTParams);
}

void translateFunction(TranslationState& ts,
    const hhbc::FunctionImpl<hhbc::BcRepr>& f) {
  UpperBoundMap ubs;
  auto upper_bounds = range(f.body.upper_bounds);
  for (auto const& u : upper_bounds) {
    translateUbs(u, ubs);
  }

  UserAttributeMap userAttrs;
  translateUserAttributes(f.body.attributes, userAttrs);

  Attr attrs = f.body.attrs;
  assertx(IMPLIES(ts.isSystemLib, attrs & AttrBuiltin));

  auto const name = toStaticString(f.name._0);
  SCOPE_ASSERT_DETAIL("translate function") {return name->data();};

  auto [typeParamNames, _] = translateTParamInfo(f.body.tparam_info);

  ts.fe = ts.ue->newFuncEmitter(name);
  ts.fe->typeParamNames = std::move(typeParamNames);
  ts.fe->init(f.body.span.line_begin, f.body.span.line_end, attrs, nullptr, ts.ue->isSystemLib());
  ts.fe->isGenerator = (bool)(f.flags & hhbc::FunctionFlags_GENERATOR);
  ts.fe->isAsync = (bool)(f.flags & hhbc::FunctionFlags_ASYNC);
  ts.fe->isPairGenerator = (bool)(f.flags & hhbc::FunctionFlags_PAIR_GENERATOR);
  ts.fe->userAttributes = userAttrs;

  translateCoeffects(ts, f.body.coeffects);

  auto retTypeInfo = maybeOrElse(f.body.return_type,
      [&](hhbc::TypeInfo& ti) {return translateTypeInfo(ti);},
      [&]() {return std::make_pair(nullptr, TypeConstraint{});});

  auto [hasReifiedGenerics, tic] =
    upperBoundsHelper(ts, ubs, {}, {}, retTypeInfo.second, userAttrs);
  assertx(!tic.isTop());

  ts.fe->retUserType = ts.ue->mergeLitstr(retTypeInfo.first);
  ts.fe->retTypeConstraints = std::move(tic);
  ts.srcLoc = locationFromSpan(f.body.span);
  checkNative(ts);
  translateFunctionBody(ts, f.body, ubs, {}, {}, hasReifiedGenerics);
}

void translateShadowedTParams(TParamNameVec& vec, const Vector<ClassName>& tpms) {
  auto tparams = range(tpms);
  for (auto const& t : tparams) {
    vec.push_back(toStaticString(t._0));
  }
}

void translateMethod(TranslationState& ts,
    const hhbc::MethodImpl<hhbc::BcRepr>& m, const UpperBoundMap& classUbs) {
  UpperBoundMap ubs;
  auto upper_bounds = range(m.body.upper_bounds);
  for (auto const& u : upper_bounds) {
    translateUbs(u, ubs);
  }

  Attr attrs = m.body.attrs;
  assertx(IMPLIES(ts.isSystemLib, attrs & AttrBuiltin));
  auto const name = toStaticString(m.name._0);
  auto [typeParamNames, shadowedTParams] = translateTParamInfo(
    m.body.tparam_info
  );
  ts.fe = ts.ue->newMethodEmitter(name, ts.pce);
  ts.fe->typeParamNames = std::move(typeParamNames);
  ts.pce->addMethod(ts.fe);
  ts.fe->init(m.body.span.line_begin, m.body.span.line_end, attrs, nullptr, ts.ue->isSystemLib());
  ts.fe->isGenerator = (bool)(m.flags & hhbc::MethodFlags_IS_GENERATOR);
  ts.fe->isAsync = (bool)(m.flags & hhbc::MethodFlags_IS_ASYNC);
  ts.fe->isPairGenerator = (bool)(m.flags & hhbc::MethodFlags_IS_PAIR_GENERATOR);
  ts.fe->isClosureBody = (bool)(m.flags & hhbc::MethodFlags_IS_CLOSURE_BODY);

  UserAttributeMap userAttrs;
  translateUserAttributes(m.body.attributes, userAttrs);
  ts.fe->userAttributes = userAttrs;

  translateCoeffects(ts, m.body.coeffects);

  auto retTypeInfo = maybeOrElse(m.body.return_type,
    [&](hhbc::TypeInfo& ti) {return translateTypeInfo(ti);},
    [&]() {return std::make_pair(nullptr, TypeConstraint{});});

  auto [hasReifiedGenerics, tic] =
    upperBoundsHelper<true>(ts, ubs, classUbs, shadowedTParams,
        retTypeInfo.second, userAttrs);

  assertx(!tic.isTop());
  ts.srcLoc = locationFromSpan(m.body.span);
  ts.fe->retUserType = ts.ue->mergeLitstr(retTypeInfo.first);
  ts.fe->retTypeConstraints = std::move(tic);
  checkNative(ts);
  translateFunctionBody(ts, m.body, ubs, classUbs, shadowedTParams, hasReifiedGenerics);
}

void translateClass(TranslationState& ts,
    const hhbc::ClassImpl<hhbc::BcRepr>& c) {
  UpperBoundMap classUbs;
  auto upper_bounds = range(c.upper_bounds);
  for (auto const& u : upper_bounds) {
    translateUbs(u, classUbs);
  }

  std::string name = toString(c.name._0);
  ITRACE(1, "Translating class {}\n", name);
  ts.pce = ts.ue->newPreClassEmitter(name);

  UserAttributeMap userAttrs;
  ITRACE(2, "Translating attribute list {}\n", c.attributes.len);
  translateUserAttributes(c.attributes, userAttrs);
  auto attrs = c.flags;
  assertx(IMPLIES(ts.isSystemLib, attrs & AttrPersistent &&
                                  attrs & AttrBuiltin));

  auto const parentName = maybeOrElse(c.base,
    [&](ClassName& s) { return toStaticString(s._0); },
    [&]() { return staticEmptyString(); });

  ts.pce->init(c.span.line_begin,
               c.span.line_end,
               attrs,
               parentName,
               staticEmptyString(),
               ts.ue->isSystemLib());

  auto const dc = maybe(c.doc_comment);
  if (dc) ts.pce->setDocComment(makeDocComment(dc.value()));
  ts.pce->setUserAttributes(userAttrs);

  std::vector<PackedStringPtr> names;
  for (auto const& tparam: range(c.tparams)) {
    names.emplace_back(toStaticString(tparam._0));
  }
  ts.pce->setTypeParamNames(std::move(names));

  auto impls = range(c.implements);
  for (auto const& i : impls) {
    ts.pce->addInterface(toStaticString(i._0));
  }
  auto incl = range(c.enum_includes);
  for (auto const& in : incl) {
    ts.pce->addEnumInclude(toStaticString(in._0));
  }

  auto requirements = range(c.requirements);
  for (auto const& r : requirements) {
    translateRequirements(ts, r);
  }

  auto methods = range(c.methods);
  for (auto const& m : methods) {
    translateMethod(ts, m, classUbs);
  }

  auto uses = range(c.uses);
  for (auto const& u : uses) {
    ts.pce->addUsedTrait(toStaticString(u._0));
  }

  translateEnumType(ts, c.enum_type);
  translateClassBody(ts, c, classUbs);
}

void translateConstant(TranslationState& ts, const hhbc::Constant& c) {
  HPHP::Constant constant;
  constant.name = toStaticString(c.name._0);
  constant.attrs = c.attrs;
  assertx(IMPLIES(ts.isSystemLib, c.attrs & AttrPersistent));

  constant.val = maybeOrElse(c.value,
    [&](hhbc::TypedValue& tv) {return toTypedValue(tv);},
    [&]() {return make_tv<KindOfNull>();});

  ts.ue->addConstant(constant);
}

void translateModuleUse(TranslationState& ts, const Optional<ModuleName>& name) {
  if (!name) return;
  ts.ue->m_moduleName = toStaticString(name.value()._0);
}


void translateModule(TranslationState& ts, const hhbc::Module& m) {
  UserAttributeMap userAttrs;
  translateUserAttributes(m.attributes, userAttrs);

  ts.ue->addModule(HPHP::Module{
    toStaticString(m.name._0),
    maybeOrElse(m.doc_comment,
      [&](const Vector<uint8_t>& s) {return makeDocComment(s);},
      [&]() {return staticEmptyString();}),
    static_cast<int>(m.span.line_begin),
    static_cast<int>(m.span.line_end),
    Attr(AttrNone),
    userAttrs,
  });
}

std::string translateIncludePath(const hhbc::IncludePath&  i) {
  switch (i.tag) {
    case IncludePath::Tag::Absolute: {
      return toString(i.Absolute._0);
    }
    case IncludePath::Tag::SearchPathRelative: {
      return toString(i.SearchPathRelative._0);
    }
    case IncludePath::Tag::IncludeRootRelative: {
      auto const body = toString(i.IncludeRootRelative._0);
      auto const body1 = toString(i.IncludeRootRelative._1);
      return body + body1;
    }
    case IncludePath::Tag::DocRootRelative: {
      return toString(i.DocRootRelative._0);
    }
  }
  not_reached();
}

void translateSymbolRefs(TranslationState& ts, const hhbc::SymbolRefs& s) {
  CompactVector<std::string> includes;
  auto includes_ = range(s.includes);
  includes.reserve(includes_.size());
  for (auto const& i : includes_) {
    includes.push_back(translateIncludePath(i));
  }
  ts.ue->m_symbol_refs.emplace_back(SymbolRef::Include, std::move(includes));

  CompactVector<std::string> constants;
  auto constants_ = range(s.constants);
  constants.reserve(constants_.size());
  for (auto const& c : constants_) {
    constants.push_back(toString(c._0));
  }
  ts.ue->m_symbol_refs.emplace_back(SymbolRef::Constant, std::move(constants));

  CompactVector<std::string> functions;
  auto functions_ = range(s.functions);
  functions.reserve(functions_.size());
  for (auto const& f : functions_) {
    functions.push_back(toString(f._0));
  }
  ts.ue->m_symbol_refs.emplace_back(SymbolRef::Function, std::move(functions));

  CompactVector<std::string> classes;
  auto classes_ = range(s.classes);
  classes.reserve(classes_.size());
  for (auto const& c : classes_) {
    classes.push_back(toString(c._0));
  }
  ts.ue->m_symbol_refs.emplace_back(SymbolRef::Class, std::move(classes));
}

void translate(TranslationState& ts, const hhbc::Unit& unit) {
  translateModuleUse(ts, maybe(unit.module_use));

  auto funcs = range(unit.functions);
  for (auto const& f : funcs) {
    translateFunction(ts, f);
  }

  auto classes = range(unit.classes);
  for (auto const& c : classes) {
    translateClass(ts, c);
  }

  auto constants = range(unit.constants);
  for (auto const& c : constants) {
    translateConstant(ts, c);
  }

  auto typedefs = range(unit.typedefs);
  for (auto const& t : typedefs) {
    translateTypedef(ts, t);
  }

  translateSymbolRefs(ts, unit.symbol_refs);

  auto modules = range(unit.modules);
  for (auto const& m : modules) {
    translateModule(ts, m);
  }

  translateUserAttributes(unit.file_attributes, ts.ue->m_fileAttributes);
  translateSymbolInfo(unit.missing_symbols, unit.error_symbols, *ts.ue);
  maybeThen(unit.fatal, [&](Fatal fatal) {
    auto const msg = toString(fatal.message);
    throw FatalUnitError(
      msg, ts.ue->m_filepath,
      locationFromSrcLoc(fatal.loc),
      fatal.op
    );
  });

  for (auto& fe : ts.ue->fevec()) fixup_default_values(ts, fe.get());
  for (auto const pce : ts.ue->preclasses()) {
    for (auto fe : pce->methods()) fixup_default_values(ts, fe);
  }
}
}

std::unique_ptr<UnitEmitter> unitEmitterFromHackCUnit(
  const hhbc::Unit& unit,
  const char* filename,
  const SHA1& sha1,
  const SHA1& bcSha1,
  const Extension* extension,
  bool swallowErrors,
  const PackageInfo& packageInfo,
  UnitEmitterAttributes attributes
) {
  auto ue = std::make_unique<UnitEmitter>(
    sha1, bcSha1, packageInfo, attributes
  );
  StringData* sd = makeStaticString(filename);
  ue->m_filepath = sd;
  ue->m_extension = extension;
  bool isSystemLib = FileUtil::isSystemName(sd->slice());

  try {
    auto ts = TranslationState(isSystemLib);
    ts.ue = ue.get();
    translate(ts, unit);
    ue->finish();
  } catch (const FatalUnitError& e) {
    ue = createFatalUnit(e.filePath, sha1, e.op, e.what(), e.pos);
  } catch (const FatalErrorException& e) {
    if (!swallowErrors) throw;
    ue = createFatalUnit(sd, sha1, FatalOp::Runtime, e.what());
  } catch (const TranslationFatal& e) {
    if (!swallowErrors) throw;
    ue = createFatalUnit(sd, sha1, FatalOp::Runtime, e.what());
  } catch (const std::exception& e) {
    if (!swallowErrors) throw;
    ue = createFatalUnit(sd, sha1, FatalOp::Runtime, e.what());
  }
  return ue;
}
}
