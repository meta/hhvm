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
#include "hphp/hhbbc/representation.h"

#include <boost/algorithm/string.hpp>
#include <memory>
#include <vector>

#include <folly/Conv.h>
#include <fmt/core.h>
#include <folly/String.h>
#include <folly/Range.h>
#include <folly/gen/Base.h>
#include <folly/gen/String.h>

#include "hphp/hhbbc/analyze.h"
#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/context.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/type-structure.h"
#include "hphp/hhbbc/type-system.h"

#include "hphp/util/text-util.h"

namespace HPHP::HHBBC {

//////////////////////////////////////////////////////////////////////

/*
 * Pretty printer for debuggin'.
 */

//////////////////////////////////////////////////////////////////////

namespace {

// Truncate stringization of ArrLikeVals above this size.
constexpr size_t maxArrLikeValSize = 1000;

std::string indent(int level, const std::string& s) {
  // Whee; make as many std::string copies as possible.
  auto const space = std::string(level, ' ');
  return space + boost::trim_copy(
    boost::replace_all_copy(s, "\n", "\n" + space)) + "\n";
}

void appendExnTreeString(const php::Func& func,
                         std::string& ret,
                         ExnNodeId p) {
  do {
    ret += " " + folly::to<std::string>(p);
    p = func.exnNodes[p].parent;
  } while (p != NoExnNodeId);
}

std::string escaped_string(SString str) {
  std::string ret;
  if (!str) {
    ret = "<nullptr>";
    return ret;
  }
  auto const sl = str->slice();
  folly::toAppend("\"", folly::cEscape<std::string>(sl), "\"", &ret);
  return ret;
}

std::string array_string(SArray arr) {
  std::string str;
  staticArrayStreamer(const_cast<ArrayData*>(arr), str);
  return str;
}

}

//////////////////////////////////////////////////////////////////////

namespace php {

std::string local_string(const Func& func, LocalId lid) {
  if (lid == NoLocalId) return "None";
  if (lid == StackDupId) return "Dup";
  if (lid == StackThisId) return "This";
  assertx(lid <= MaxLocalId);
  auto const& loc = func.locals[lid];
  return loc.name
    ? folly::to<std::string>("$", loc.name->data())
    : folly::to<std::string>("$<unnamed:", lid, ">");
}

std::string show(const Func& func, const Bytecode& bc) {
  std::string ret;

  auto append_vsa = [&] (const CompactVector<LSString>& keys) {
    ret += "<";
    auto delim = "";
    for (auto& s : keys) {
      ret += delim + escaped_string(s);
      delim = ",";
    }
    ret += ">";
  };

  auto append_switch = [&] (const SwitchTab& tab) {
    ret += "<";
    for (auto& target : tab) {
      folly::toAppend(target, " ", &ret);
    }
    ret += ">";
  };

  auto append_sswitch = [&] (const SSwitchTab& tab) {
    ret += "<";
    for (auto& kv : tab) {
      folly::toAppend(escaped_string(kv.first), ":", kv.second, " ", &ret);
    }
    ret += ">";
  };

  auto append_mkey = [&](MKey mkey) {
    ret += memberCodeString(mkey.mcode);

    switch (mkey.mcode) {
      case MEL: case MPL:
        folly::toAppend(':', local_string(func, mkey.local.id), &ret);
        break;
      case MEC: case MPC:
        folly::toAppend(':', mkey.idx, &ret);
        break;
      case MEI:
        folly::toAppend(':', mkey.int64, &ret);
        break;
      case MET: case MPT: case MQT:
        folly::toAppend(
          ":\"", escapeStringForCPP(mkey.litstr->data(), mkey.litstr->size()),
          '"', &ret
        );
        break;
      case MW:
        break;
    }
  };

  auto append_lar = [&](const LocalRange& range) {
    if (!range.count) {
      folly::toAppend("L:-", &ret);
    } else {
      folly::toAppend("L:", local_string(func, range.first), "+",
                      range.count, &ret);
    }
  };

  auto append_ita = [&](const IterArgs& ita) {
    auto const print = [&](int32_t local) { return local_string(func, local); };
    folly::toAppend(show(ita, print), &ret);
  };

#define IMM_BLA(n)     ret += " "; append_switch(data.targets);
#define IMM_SLA(n)     ret += " "; append_sswitch(data.targets);
#define IMM_IVA(n)     folly::toAppend(" ", data.arg##n, &ret);
#define IMM_I64A(n)    folly::toAppend(" ", data.arg##n, &ret);
#define IMM_LA(n)      ret += " " + local_string(func, data.loc##n);
#define IMM_NLA(n)     ret += " " + local_string(func, data.nloc##n.id);
#define IMM_ILA(n)     ret += " " + local_string(func, data.loc##n);
#define IMM_IA(n)      folly::toAppend(" iter:", data.iter##n, &ret);
#define IMM_DA(n)      folly::toAppend(" ", data.dbl##n, &ret);
#define IMM_SA(n)      folly::toAppend(" ", escaped_string(data.str##n), &ret);
#define IMM_RATA(n)    folly::toAppend(" ", show(data.rat), &ret);
#define IMM_AA(n)      ret += " " + array_string(data.arr##n);
#define IMM_BA(n)      folly::toAppend(" <blk:", data.target##n, ">", &ret);
#define IMM_OA_IMPL(n) folly::toAppend(" ", subopToName(data.subop##n), &ret);
#define IMM_OA(type)   IMM_OA_IMPL
#define IMM_VSA(n)     ret += " "; append_vsa(data.keys);
#define IMM_KA(n)      ret += " "; append_mkey(data.mkey);
#define IMM_LAR(n)     ret += " "; append_lar(data.locrange);
#define IMM_ITA(n)     ret += " "; append_ita(data.ita);
#define IMM_FCA(n)     do {                                        \
  auto const aeTarget = data.fca.asyncEagerTarget() != NoBlockId   \
    ? fmt::format("<aeblk:{}>", data.fca.asyncEagerTarget())    \
    : "-";                                                         \
  folly::toAppend(                                                 \
    " ", show(data.fca.base(), data.fca.inoutArgs(),               \
              data.fca.readonlyArgs(), kInvalidId, /* TODO(named_params) actually pass data here */ \
              aeTarget, data.fca.context()), &ret);                          \
} while (false);

#define IMM_NA
#define IMM_ONE(x)                IMM_##x(1)
#define IMM_TWO(x, y)             IMM_##x(1); IMM_##y(2);
#define IMM_THREE(x, y, z)        IMM_TWO(x, y); IMM_##z(3);
#define IMM_FOUR(x, y, z, n)      IMM_THREE(x, y, z); IMM_##n(4);
#define IMM_FIVE(x, y, z, n, m)   IMM_FOUR(x, y, z, n); IMM_##m(5);
#define IMM_SIX(x, y, z, n, m, o) IMM_FIVE(x, y, z, n, m); IMM_##o(6);

#define O(opcode, imms, inputs, outputs, flags) \
  case Op::opcode:                              \
    {                                           \
      UNUSED auto const& data = bc.opcode;      \
      UNUSED auto const curOpcode = Op::opcode; \
      folly::toAppend(#opcode, &ret);           \
      IMM_##imms                                \
    }                                           \
    break;

  switch (bc.op) { OPCODES }

#undef O

#undef IMM_BLA
#undef IMM_SLA
#undef IMM_IVA
#undef IMM_I64A
#undef IMM_LA
#undef IMM_NLA
#undef IMM_ILA
#undef IMM_IA
#undef IMM_DA
#undef IMM_SA
#undef IMM_RATA
#undef IMM_AA
#undef IMM_BA
#undef IMM_OA_IMPL
#undef IMM_OA
#undef IMM_KA
#undef IMM_LAR
#undef IMM_FCA

#undef IMM_NA
#undef IMM_ONE
#undef IMM_TWO
#undef IMM_THREE
#undef IMM_FOUR
#undef IMM_FIVE
#undef IMM_SIX

  return ret;
}

std::string show(const Func& func, const Block& block) {
  std::string ret;

  if (block.exnNodeId != NoExnNodeId) {
    ret += "(exnNode:";
    appendExnTreeString(func, ret, block.exnNodeId);
    ret += ")\n";
  }

  for (auto& bc : block.hhbcs) ret += show(func, bc) + "\n";

  if (block.fallthrough != NoBlockId) {
    folly::toAppend(
      "(fallthrough blk:",
      block.fallthrough,
      ")\n",
      &ret
    );
  }

  if (block.throwExit != NoBlockId) {
    ret += fmt::format("(throw: blk:{})\n", block.throwExit);
  }

  return ret;
}

std::string dot_instructions(const Func& func, const Block& b) {
  using namespace folly::gen;
  return from(b.hhbcs)
    | map([&] (const Bytecode& bc) {
        return "\"" + folly::cEscape<std::string>(show(func, bc)) + "\\n\"";
      })
    | unsplit<std::string>("+\n")
    ;
}

// Output DOT-format graph.  Paste into dot -Txlib or similar.
std::string dot_cfg(const php::WideFunc& func) {
  std::string ret;
  for (auto const bid : rpoSortAddDVs(func)) {
    auto const b = func.blocks()[bid].get();
    ret += fmt::format(
      "B{} [ label = \"blk:{}\\n\"+{} ]\n",
      bid, bid, dot_instructions(*func, *b));
    bool outputed = false;
    forEachNormalSuccessor(*b, [&] (BlockId target) {
      ret += fmt::format("B{} -> B{};", bid, target);
      outputed = true;
    });
    if (outputed) ret += "\n";
    if (!is_single_nop(*b) && b->throwExit != NoBlockId) {
      ret += fmt::format("B{} -> B{} [color=red];\n", bid, b->throwExit);
    }
  }
  return ret;
}

std::string show(const Func& f) {
  std::string ret;
  auto const func = php::WideFunc::cns(&f);

#define X(what) if (func->what) folly::toAppend(#what "\n", &ret)
  X(isClosureBody);
  X(isAsync);
  X(isGenerator);
  X(isPairGenerator);
#undef X

  if (getenv("HHBBC_DUMP_DOT")) {
    ret += fmt::format("digraph {} {{\n  node [shape=box];\n{}}}\n",
                  func->name->data(), indent(2, dot_cfg(func)));
  }

  for (auto const bid : func.blockRange()) {
    auto const blk = func.blocks()[bid].get();
    if (blk->dead) continue;
    ret += fmt::format("block #{}\n{}", bid, indent(2, show(*func, *blk)));
  }

  visitExnLeaves(*func, [&] (const php::ExnNode& node) {
    ret += fmt::format("exn node #{} ", node.idx);
    if (node.parent != NoExnNodeId) {
      ret += fmt::format("(^{}) ", node.parent);
    }
    ret += folly::to<std::string>("catch->", node.region.catchEntry) + '\n';
  });

  return ret;
}

std::string show(const Class& cls) {
  std::string ret;
  folly::toAppend(
    "class ",
    cls.name->data(),
    &ret
  );
  if (cls.parentName) {
    folly::toAppend(" extends ", cls.parentName->data(), &ret);
  }
  ret += ":\n";
  for (auto& i : cls.interfaceNames) {
    folly::toAppend("  implements ", i->data(), "\n", &ret);
  }
  for (auto& m : cls.methods) {
    if (!m) continue;
    folly::toAppend(
      "  method ",
      m->name->data(), ":\n",
      indent(4, show(*m)),
      &ret
    );
  }
  return ret;
}

std::string show(const Unit& unit,
                 const std::vector<const php::Class*>& classes,
                 const std::vector<const php::Func*>& funcs) {
  std::string ret;
  folly::toAppend(
    "Unit ", unit.filename->data(), "\n",
    &ret
  );

  for (auto const c : classes) {
    folly::toAppend(
      indent(2, show(*c)),
      &ret
    );
  }

  for (auto const f : funcs) {
    folly::toAppend(
      "  function ", f->name->data(), ":\n",
      indent(4, show(*f)),
      &ret
    );
  }

  folly::toAppend("\n", &ret);
  return ret;
}

std::string show(const Unit& unit, const IIndex& index) {
  std::vector<const php::Class*> classes;
  std::vector<const php::Func*> funcs;
  index.for_each_unit_class(
    unit,
    [&] (const php::Class& c) { classes.emplace_back(&c); }
  );
  index.for_each_unit_func(
    unit,
    [&] (const php::Func& f) { funcs.emplace_back(&f); }
  );

  std::sort(
    begin(classes), end(classes),
    [] (const php::Class* a, const php::Class* b) {
      return string_data_lt_type{}(a->name, b->name);
    }
  );
  std::sort(
    begin(funcs), end(funcs),
    [] (const php::Func* a, const php::Func* b) {
      return string_data_lt{}(a->name, b->name);
    }
  );

  return show(unit, classes, funcs);
}

std::string show(const Unit& unit, const Program& p) {
  std::vector<const php::Class*> classes;
  std::vector<const php::Func*> funcs;
  for (auto const& c : p.classes) {
    if (c->unit != unit.filename) continue;
    classes.emplace_back(c.get());
  }
  for (auto const& f : p.funcs) {
    if (f->unit != unit.filename) continue;
    funcs.emplace_back(f.get());
  }
  return show(unit, classes, funcs);
}

std::string show(const Program& p, const IIndex& index) {
  using namespace folly::gen;
  return from(p.units)
    | map([&] (const std::unique_ptr<php::Unit>& u) { return show(*u, index); })
    | unsplit<std::string>("--------------\n")
    ;
}

std::string show(SrcLoc loc) {
  return fmt::format("{}:{}-{}:{}",
                        loc.start.line, loc.start.col,
                        loc.past.line, loc.past.col);
}

}

//////////////////////////////////////////////////////////////////////

std::string show(const DCls& dcls, bool isObj) {
  auto const lt = [&] {
    assertx(!dcls.isExact());
    return !isObj && !dcls.containsNonRegular() ? "<-" : "<=";
  };

  auto const eq = [&] {
    return isObj || dcls.containsNonRegular() ? "=" : "=-";
  };

  std::string ret;
  if (dcls.isExact()) {
    folly::toAppend(eq(), show(dcls.cls()), &ret);
  } else if (dcls.isSub()) {
    folly::toAppend(lt(), show(dcls.cls()), &ret);
  } else if (dcls.isIsect()) {
    folly::toAppend(
      lt(),
      "{",
      [&] {
        using namespace folly::gen;
        return from(dcls.isect())
          | map([] (res::Class c) { return show(c); })
          | unsplit<std::string>("&");
      }(),
      "}",
      &ret
    );
  } else {
    auto const [e, i] = dcls.isectAndExact();
    folly::toAppend(
      eq(),
      "{",
      show(e),
      "}&",
      lt(),
      "{",
      [&, i=i] {
        using namespace folly::gen;
        return from(*i)
          | map([] (res::Class c) { return show(c); })
          | unsplit<std::string>("&");
      }(),
          "}",
      &ret
    );
  }
  if (dcls.isCtx()) folly::toAppend(" this", &ret);
  return ret;
}

//////////////////////////////////////////////////////////////////////

std::string show(const Type& t) {
  static constexpr int kMaxShowCalls = 10000;
  static thread_local int showCalls = 0;
  static thread_local bool showRoot = true;
  auto const isRoot = showRoot;
  if (isRoot) {
    showCalls = 0;
    showRoot = false;
  }
  SCOPE_EXIT { if (isRoot) showRoot = true; };
  if (++showCalls > kMaxShowCalls) return "...(truncated)";
  /*
   * Type pretty printing
   *
   * When you allow arbitrary trep combinations with specializations,
   * pretty printing the Type in its most concise form can be tricky.
   *
   * treps (in general) are printed by attempting a greedy match among
   * the predefined treps (the ones with names). For a trep b, until
   * there are no bits left in b, find the largest (most bits)
   * predefined trep which is fully contained within b. Add that
   * predefined trep's name to the output list and remove its bits
   * from b. This should produce the shortest list of names (joined
   * with a |) which represents the trep.
   *
   * If a specialization is present, first all of the bits which
   * support that specialization are gathered together (using the
   * above scheme) and printed first. If there is more than one name,
   * they are surrounded with {}. Then the specialization is attached
   * to the end. Any remaining trep bits are then processed (again
   * using the above scheme) and combined with |s as usual. This
   * scheme makes it clear which bits the specialization applies to,
   * and which ones it does not.
   *
   * TInitNull is treated a bit different. Instead of literally
   * TInitNull, it is rendered as a ? prefix. Even a predefined type
   * like TOptInt gets this treatment (it becomes ?Int). If the
   * remaining bits have multiple names, they are again grouped inside
   * a {} and the ? is applied to the front of the grouping. This
   * makes it clear it binds to the group (like ?{Int|Str}), whereas
   * something like ?Int|Str is more ambiguous.
   *
   * Putting it all together, for example, the union of
   * TDict(Int:Int), TKeyset(Int,Int), TInitNull, TBool, and TObj
   * would be rendered as:
   *
   * ?{{Dict|Keyset}(Int:Int)|Bool|Obj}
   */

  // NB: We want this function be usuable even on invalid/malformed
  // types (for example if we want to print the type from within
  // checkInvariants()). Therefore we don't make any assumptions about
  // the Type being sane and don't assert anything. We also just deal
  // with bits and don't copy or manipulate the actual Type.

  // Sorted vector of all the predefined types, from highest bitcount
  // to shortest. This lets us implement a greedy covering of bits to
  // names.
  static auto const sorted = [] {
    std::vector<std::pair<trep, std::string>> types{
      #define X(y, ...) { B##y, #y },
      HHBBC_TYPE_PREDEFINED(X)
      #undef X
    };
    std::sort(
      types.begin(), types.end(),
      [](auto const& a, auto const& b) {
        auto const pop1 = folly::popcount((uint64_t)a.first);
        auto const pop2 = folly::popcount((uint64_t)b.first);
        if (pop1 != pop2) return pop1 > pop2;
        // Special case here: The static/counted axis of array bits
        // has the same size as the empty/non-empty axis of array
        // bits. We want to give preferencial treatment to the
        // empty/non-empty bits to break this symmetric, and also
        // because it produces more natural looking unions. This is
        // purely an aesthetic choice.
        auto const sizish = [] (trep bits) {
          return couldBe(bits, BArrLikeE) != couldBe(bits, BArrLikeN);
        };
        auto const s1 = sizish(a.first);
        auto const s2 = sizish(b.first);
        if (s1 != s2) return s1 > s2;
        return a.second < b.second;
      }
    );
    return types;
  }();

  // Given a trep, produce a string of the predefined types (joined
  // with |) which represent this type. The number of such names is
  // also returned.
  auto const gather = [&] (trep bits) -> std::pair<std::string, size_t> {
    // Below loop only works if there's a bit. If there's not, we know
    // we're Bottom anyways.
    if (!bits) return std::make_pair("Bottom", 1);

    std::string ret;
    size_t count = 0;
    for (auto const& ty : sorted) {
      // Bottom ends up in the list. Just skip it.
      if (!ty.first) continue;
      // Check if this predefined type is covered by our bits.
      if ((ty.first & bits) != ty.first) continue;
      // It is, remove it from the bits and add its name to the list.
      bits -= ty.first;

      if (!ret.empty()) ret += "|";
      // Special handling of Opt* types, turn them into ?.
      if (!ty.second.compare(0, 3, "Opt", 3)) {
        auto temp = ty.second.substr(2);
        temp[0] = '?';
        ret += temp;
      } else {
        ret += ty.second;
      }
      ++count;

      if (!bits) break;
    }
    if (bits) {
      // We'll only get here if we exhausted all the predefined types
      // and didn't consume all the bits. This can only happen with
      // invalid types. However, since we want to be robust, just let
      // it be known something's wrong and continue.
      ret += ret.empty() ? "???" : "|???";
      ++count;
    }
    return std::make_pair(ret, count);
  };

  auto const showElem = [&] (const Type& key, const Type& val) -> std::string {
    return show(key) + ":" + show(val);
  };

  auto const showMapElem = [&] (TypedValue k, const MapElem& m) {
    auto const key = [&] {
      if (isIntType(k.m_type)) return ival(k.m_data.num);
      assertx(k.m_type == KindOfString);
      assertx(!k.m_data.pstr->isRefCounted());
      switch (m.keyStaticness) {
        case TriBool::Yes:   return sval(k.m_data.pstr);
        case TriBool::Maybe: return sval_nonstatic(k.m_data.pstr);
        case TriBool::No:    return sval_counted(k.m_data.pstr);
      }
      always_assert(false);
    }();
    return showElem(key, m.val);
  };

  // Given a trep, first gather together the support bits for any
  // specialization. Add the specialization string, then gather the
  // remaining (non-support) bits. If there's no specialization, just
  // delegate to gather().
  auto const gatherForSpec = [&] (trep bits) {
    // Gather the support and the non-support bits, then combine them
    // into a string (with the spec in the middle).
    auto const impl = [&] (trep mask, const std::string& spec) {
      auto const [specPart, specMatches] = gather(bits & mask);
      auto const [restPart, restMatches] = [&] {
        return (bits & ~mask)
          ? gather(bits & ~mask)
          : std::make_pair(std::string{}, size_t{0});
      }();

      auto const ret = fmt::format(
        "{}{}{}{}{}{}",
        specMatches > 1 ? "{" : "",
        specPart,
        specMatches > 1 ? "}" : "",
        spec,
        restMatches > 0 ? "|" : "",
        restPart
      );
      return std::make_pair(ret, specMatches + restMatches);
    };

    switch (t.m_dataTag) {
    case DataTag::Obj:
      return impl(BObj, show(t.m_data.dobj, true));
    case DataTag::WaitHandle:
      return impl(
        BObj,
        fmt::format(
          "=WaitH<{}{}>",
          show(t.m_data.dwh->inner),
          t.m_data.dwh->canSuspend ? "" : ",eager"
        )
      );
    case DataTag::Cls:
      return impl(BCls, show(t.m_data.dcls, false));
    case DataTag::ArrLikePacked:
      return impl(
        BArrLike,
        fmt::format(
          "({})",
          [&] {
            using namespace folly::gen;
            return from(t.m_data.packed->elems)
              | map([&] (const Type& t) { return show(t); })
              | unsplit<std::string>(",");
          }()
        )
      );
    case DataTag::ArrLikePackedN:
      return impl(
        BArrLike,
        fmt::format("([{}])", show(t.m_data.packedn->type))
      );
    case DataTag::ArrLikeMap:
      return impl(
        BArrLike,
        fmt::format(
          "({}{})",
          [&] {
            using namespace folly::gen;
            return from(t.m_data.map->map)
              | map([&] (const std::pair<TypedValue,MapElem>& kv) {
                  return showMapElem(kv.first, kv.second);
                })
              | unsplit<std::string>(",");
          }(),
          [&] {
            if (!t.m_data.map->hasOptElements()) return std::string{};
            return fmt::format(
              ",...[{}]",
              showElem(t.m_data.map->optKey, t.m_data.map->optVal)
            );
          }()
        )
      );
    case DataTag::ArrLikeMapN:
      return impl(
        BArrLike,
        fmt::format(
          "([{}])",
          showElem(t.m_data.mapn->key, t.m_data.mapn->val)
        )
      );
    case DataTag::ArrLikeVal:
      return impl(
        BArrLike,
        fmt::format(
          "~{}",
          [&] {
            auto s = array_string(t.m_data.aval);
            if (s.size() > maxArrLikeValSize) {
              s.resize(maxArrLikeValSize);
              s += "...(truncated)";
            }
            return s;
          }()
        )
      );
    case DataTag::Str:
      return impl(BStr, fmt::format("={}", escaped_string(t.m_data.sval)));
    case DataTag::LazyCls:
      return impl(BLazyCls,
                  fmt::format("={}",
                  escaped_string(t.m_data.lazyclsval)));
    case DataTag::EnumClassLabel:
      return impl(BEnumClassLabel,
                  fmt::format("={}", escaped_string(t.m_data.eclval)));
    case DataTag::Int: return impl(BInt, fmt::format("={}", t.m_data.ival));
    case DataTag::Dbl: return impl(BDbl, fmt::format("={}", t.m_data.dval));
    case DataTag::None: return gather(bits);
    }
    not_reached();
  };

  // Produce the string, then perform the TInitNull special processing
  // as described above.
  auto ret = [&] {
    auto gathered = gatherForSpec(t.bits());
    // If there trep has TInitNull in it, but the gathering only
    // matched one thing, it was a Opt* type, and it was already
    // turned into ?, so we're done. If not, redo the gathering with
    // the TInitNull removed. Add it to the front of the resultant
    // string.
    if (couldBe(t.bits(), BInitNull) && gathered.second > 1) {
      gathered = gatherForSpec(t.bits() & ~BInitNull);
      return fmt::format(
        "?{}{}{}",
        gathered.second > 1 ? "{" : "",
        gathered.first,
        gathered.second > 1 ? "}" : ""
      );
    }
    return gathered.first;
  }();

  return ret;
}

//////////////////////////////////////////////////////////////////////

std::string show(Context ctx) {
  if (!ctx.func) {
    if (!ctx.cls) {
      if (!ctx.unit) return "-";
      return ctx.unit->toCppString();
    }
    return ctx.cls->name->toCppString();
  }
  auto ret = std::string{};
  if (ctx.cls) {
    ret = ctx.cls->name->toCppString();
    if (ctx.cls != ctx.func->cls) {
      ret += fmt::format("({})", ctx.func->cls->name->data());
    }
    ret += "::";
  }
  ret += ctx.func->name->toCppString();
  return ret;
}

//////////////////////////////////////////////////////////////////////

std::string show(const ConstIndex& idx) {
  return fmt::format("{}:{}", idx.cls->data(), idx.idx);
}

std::string show(const MethRef& m) {
  return fmt::format("{}:{}", m.cls->data(), m.idx);
}

//////////////////////////////////////////////////////////////////////

std::string show(FuncClsUnit fc) {
  if (auto const f = fc.func()) {
    return fmt::format("func {}", f->name->data());
  } else if (auto const c = fc.cls()) {
    return fmt::format("class {}", c->name->data());
  } else if (auto const u = fc.unit()) {
    return fmt::format("unit {}", u->filename->data());
  } else {
    return "(null)";
  }
}

//////////////////////////////////////////////////////////////////////

std::string show(const PropLookupResult& r) {
  return fmt::format(
    "{{{},ty:{},found:{},const:{},late:{},init:{},internal:{}}}",
    r.name->data(),
    show(r.ty),
    show(r.found),
    show(r.isConst),
    show(r.lateInit),
    r.classInitMightRaise,
    show(r.internal)
  );
}

std::string show(const PropMergeResult& r) {
  return fmt::format(
    "{{adjusted:{},throws:{}}}",
    show(r.adjusted),
    show(r.throws)
  );
}

std::string show(const ClsConstLookupResult& r) {
  return fmt::format(
    "{{ty:{},found:{},throw:{}}}",
    show(r.ty),
    show(r.found),
    r.mightThrow
  );
}

std::string show(const ClsTypeConstLookupResult& r) {
  return fmt::format(
    "{{ty:{},fail:{},sens:{},found:{},abstract:{}}}",
    show(r.resolution.type),
    r.resolution.mightFail,
    r.resolution.contextSensitive,
    show(r.found),
    show(r.abstract)
  );
}

std::string show(const ConstraintType& t) {
  const char* cts;
  switch (t.coerceClassToString) {
    case TriBool::Yes: cts = "yes"; break;
    case TriBool::No: cts = "no"; break;
    case TriBool::Maybe: cts = "maybe"; break;
  }
  return fmt::format(
    "ConstraintType{{lower:{}, upper:{}, coerceClassToString:{}, maybeMixed:{}}}",
    show(t.lower),
    show(t.upper),
    cts,
    t.maybeMixed ? "true" : "false"
  );
}

//////////////////////////////////////////////////////////////////////

}
