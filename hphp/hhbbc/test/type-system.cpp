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
#include "hphp/hhbbc/type-system.h"

#include <gtest/gtest.h>
#include <boost/range/join.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <folly/Lazy.h>
#include <fmt/format.h>

#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/context.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/hhbbc.h"
#include "hphp/hhbbc/interp.h"
#include "hphp/hhbbc/misc.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/parallel.h"
#include "hphp/hhbbc/parse.h"
#include "hphp/hhbbc/index.h"

#include "hphp/runtime/base/array-init.h"

#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/tv-comparisons.h"

#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/extern-worker.h"

namespace HPHP::HHBBC {

using namespace extern_worker;
namespace coro = folly::coro;

void PrintTo(const Type& t, ::std::ostream* os) { *os << show(t); }
void PrintTo(Emptiness e, ::std::ostream* os) {
  switch (e) {
    case Emptiness::Empty:    *os << "empty"; break;
    case Emptiness::NonEmpty: *os << "non-empty"; break;
    case Emptiness::Maybe:    *os << "maybe"; break;
    default: always_assert(false);
  }
}

inline bool operator==(const Type& t1, const Type& t2) {
  return t1.equalNoContext(t2);
}
inline bool operator!=(const Type& t1, const Type& t2) {
  return !t1.equalNoContext(t2);
}

Type make_obj_for_testing(trep, res::Class, bool, bool, bool);
Type make_cls_for_testing(trep, res::Class, bool, bool, bool, bool);
Type make_arrval_for_testing(trep, SArray);
Type make_arrpacked_for_testing(trep, std::vector<Type>);
Type make_arrpackedn_for_testing(trep, Type);
Type make_arrmap_for_testing(trep, MapElems, Type, Type);
Type make_arrmapn_for_testing(trep, Type, Type);

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_Awaitable("HH\\Awaitable");
const StaticString s_AwaitableChild("HH\\AwaitableChild");
const StaticString s_AwaitableChild2("HH\\AwaitableChild2");

const StaticString s_test("test");
const StaticString s_C("C");

const StaticString s_ChildClosure1("Closure$ChildClosure1");
const StaticString s_ChildClosure2("Closure$ChildClosure2");
const StaticString s_ChildClosure3("Closure$ChildClosure3");

#define TEST_CLASSES                            \
  X(Closure)                                    \
                                                \
  X(TestClass)                                  \
  X(TestClassDeriver)                           \
  X(Base)                                       \
  X(A)                                          \
  X(AA)                                         \
  X(AB)                                         \
  X(B)                                          \
  X(BA)                                         \
  X(BB)                                         \
  X(BAA)                                        \
  X(BAADeriver)                                 \
  X(IBase)                                      \
  X(IA)                                         \
  X(IAA)                                        \
  X(IB)                                         \
                                                \
  X(I_A)                                        \
  X(I_B)                                        \
  X(I_C)                                        \
  X(I_D)                                        \
  X(I_E)                                        \
  X(I_F)                                        \
  X(I_G)                                        \
  X(I_H)                                        \
  X(I_I)                                        \
  X(I_J)                                        \
  X(I_K)                                        \
                                                \
  X(X1)                                         \
  X(X2)                                         \
  X(X3)                                         \
  X(X4)                                         \
  X(X5)                                         \
  X(X6)                                         \
  X(X7)                                         \
  X(X8)                                         \
  X(X9)                                         \
  X(X10)                                        \
  X(X11)                                        \
  X(X12)                                        \
  X(X13)                                        \
  X(X14)                                        \
  X(X15)                                        \
  X(X16)                                        \
  X(X17)                                        \
  X(X18)                                        \
  X(X19)                                        \
  X(X20)                                        \
  X(X21)                                        \
  X(X22)                                        \
  X(X23)                                        \
  X(X24)                                        \
  X(X25)                                        \
  X(X26)                                        \
  X(X27)                                        \
  X(X28)                                        \
  X(X29)                                        \
                                                \
  X(Y1)                                         \
  X(Y2)                                         \
  X(Y3)                                         \
  X(Y4)                                         \
  X(Y5)                                         \
  X(Y6)                                         \
  X(Y7)                                         \
  X(Y8)                                         \
  X(Y9)                                         \
                                                \
  X(Z1)                                         \
  X(Z2)                                         \
  X(Z3)                                         \
  X(Z4)                                         \
  X(Z5)                                         \
  X(Z6)                                         \
  X(Z7)                                         \
  X(Z8)                                         \
  X(Z9)                                         \
  X(Z10)                                        \
                                                \
  X(I_Y1)                                       \
  X(I_Y2)                                       \
  X(I_Y3)                                       \
                                                \
  X(I_Z1)                                       \
  X(I_Z2)                                       \
  X(I_Z3)                                       \
  X(I_Z4)                                       \
  X(I_Z5)                                       \
  X(I_Z6)                                       \
                                                \
  X(ICanon1)                                    \
  X(ICanon2)                                    \
  X(ICanon3)                                    \
  X(ICanon4)                                    \
  X(ICanon5)                                    \
  X(ICanon6)                                    \
  X(ICanon7)                                    \
  X(ICanon8)                                    \
  X(ICanon9)                                    \
  X(ICanon10)                                   \
  X(ICanon11)                                   \
  X(ICanon12)                                   \
  X(ICanon13)                                   \
  X(ICanon14)                                   \
                                                \
  X(Canon1)                                     \
  X(Canon2)                                     \
  X(Canon3)                                     \
  X(Canon4)                                     \
  X(Canon5)                                     \
  X(Canon6)                                     \
  X(Canon7)                                     \
  X(Canon8)                                     \
  X(Canon9)                                     \
  X(Canon10)                                    \
  X(Canon11)                                    \
  X(Canon12)                                    \
  X(Canon13)                                    \
  X(Canon14)                                    \
  X(Canon15)                                    \
  X(Canon16)                                    \
  X(Canon17)                                    \
  X(Canon18)                                    \
                                                \
  X(T1)                                         \
  X(T2)                                         \
  X(T3)                                         \
  X(T4)                                         \
                                                \
  X(Abs1)                                       \
  X(Abs2)                                       \
  X(Abs3)                                       \
  X(Abs4)                                       \
  X(Abs5)                                       \
  X(Abs6)                                       \
                                                \
  X(T1_C1)                                      \
  X(T4_C1)                                      \
                                                \
  X(Abs3_C1)                                    \
  X(Abs3_C2)                                    \
  X(Abs3_C3)                                    \
                                                \
  X(Abs4_C1)                                    \
  X(Abs4_C2)                                    \
                                                \
  X(Abs6_C1)                                    \
                                                \
  X(Abs5_P)                                     \
  X(Abs6_P)                                     \
                                                \
  Y(Foo1)                                       \
  Y(Foo2)                                       \
  Y(Foo3)                                       \
                                                \
  Z(U1)                                         \
  Z(U2)                                         \

#define X(name)                                 \
  const StaticString s_##name(#name);
#define Y(name)                                 \
  const StaticString s_##name(#name);
#define Z(name)                                 \
  const StaticString s_##name(#name);
  TEST_CLASSES
#undef Z
#undef Y
#undef X

// A test program so we can actually test things involving object or
// class types.
Index make_index() {
  std::string hhas = R"(
    # Technically this should be provided by systemlib, but it's the
    # only one we have to make sure the type system can see for unit
    # test purposes, so we can just define it here.  We don't need to
    # give it any of its functions currently.
    .class [abstract builtin] HH\Awaitable {
    }

    .class [builtin] HH\AwaitableChild extends HH\Awaitable {
    }

    .class [builtin] HH\AwaitableChild2 extends HH\Awaitable {
    }

    .class [builtin] Closure {
    }

    .class Closure$ChildClosure1 extends Closure {
      .method [public static] N __invoke() isClosureBody {
        Null
        RetC None
      }
    }
    .class Closure$ChildClosure2 extends Closure {
      .method [public static] N __invoke() isClosureBody {
        Null
        RetC None
      }
    }
    .class Closure$ChildClosure3 extends Closure {
      .method [public static] N __invoke() isClosureBody {
        Null
        RetC None
      }
    }

    .class [interface ] IBase {
    }

    .class [interface] IA implements (IBase) {
    }

    .class [interface] IB implements (IBase) {
    }

    .class [interface] IAA implements (IA) {
    }

    .class Base {
      .default_ctor;
    }

    .class  A extends Base implements (IA) {
      .default_ctor;
    }

    .class [no_override] AA extends A implements (IAA) {
      .default_ctor;
    }

    .class [no_override] AB extends A {
      .default_ctor;
    }

    .class B extends Base {
      .default_ctor;
    }

    .class BA extends B {
      .default_ctor;
    }

    .class [no_override] BB extends B {
      .default_ctor;
    }

    .class BAA extends BA {
      .default_ctor;
    }

    # Make sure BAA doesn't get AttrNoOverride:
    .class BAADeriver extends BAA {
      .default_ctor;
    }

    .class TestClass {
      .default_ctor;
    }

    # Make sure TestClass doesn't get AttrNoOverride:
    .class TestClassDeriver extends TestClass {
      .default_ctor;
    }

    .class [interface] I_A {
    }
    .class [interface] I_B {
    }
    .class [interface] I_C {
    }
    .class [interface] I_D {
    }
    .class [interface] I_E {
    }
    .class [interface] I_F {
    }
    .class [interface] I_G {
    }
    .class [interface] I_H {
    }
    .class [interface] I_I {
    }
    .class [interface] I_J {
    }
    .class [interface] I_K {
    }

    .class X1 {
    }
    .class X2 extends X1 implements (I_A) {
    }
    .class X3 extends X1 implements (I_B) {
    }
    .class X4 extends X1 implements (I_C) {
    }
    .class X5 extends X1 implements (I_D) {
    }
    .class X6 extends X2 implements (I_E) {
    }
    .class X7 extends X6 implements (I_I) {
    }
    .class X8 extends X6 implements (I_J) {
    }
    .class X9 extends X5 implements (I_I) {
    }
    .class X10 extends X2 implements (I_D) {
    }
    .class X11 extends X10 implements (I_I) {
    }
    .class X12 extends X2 implements (I_F) {
    }
    .class X13 extends X12 implements (I_J) {
    }
    .class X14 extends X2 implements (I_J) {
    }
    .class X15 extends X2 implements (I_G) {
    }
    .class X16 extends X1 implements (I_G) {
    }
    .class X17 extends X3 implements (I_G) {
    }
    .class X18 extends X3 implements (I_H) {
    }
    .class X19 extends X3 implements (I_K) {
    }
    .class X20 extends X1 implements (I_K) {
    }
    .class X21 {
    }
    .class X22 extends X21 implements (I_A) {
    }
    .class X23 extends X21 implements (I_B) {
    }
    .class X24 extends X21 implements (I_C) {
    }
    .class X25 extends X21 implements (I_E) {
    }
    .class X26 extends X21 implements (I_F) {
    }
    .class X27 extends X21 implements (I_H) {
    }
    .class X28 extends X21 {
    }
    .class X29 extends X1 {
    }

    .class [interface] I_Y1 {
    }
    .class [interface] I_Y2 {
    }
    .class [interface] I_Y3 {
    }

    .class Y1 implements (I_Y2 I_Y3) {
    }
    .class Y2 extends Y1 {
    }
    .class Y3 extends Y1 {
    }
    .class Y4 extends Y2 implements (I_Y1) {
    }
    .class Y5 extends Y3 implements (I_Y1) {
    }
    .class Y6 implements (I_Y2 I_Y3) {
    }
    .class Y7 implements (I_Y1 I_Y2 I_Y3) {
    }
    .class Y8 implements (I_Y2) {
    }
    .class Y9 implements (I_Y3) {
    }

    .class [interface] I_Z1 {
    }
    .class [interface] I_Z2 {
    }
    .class [interface] I_Z3 {
    }
    .class [interface] I_Z4 {
    }
    .class [interface] I_Z5 {
    }
    .class [interface] I_Z6 {
    }

    .class [abstract] Z1 implements (I_Z2) {
    }
    .class Z2 extends Z1 implements (I_Z1) {
    }
    .class Z3 extends Z1 implements (I_Z1) {
    }
    .class Z4 {
    }
    .class [abstract] Z5 extends Z4 implements (I_Z4) {
    }
    .class Z6 extends Z4 implements (I_Z3 I_Z4) {
    }
    .class Z7 implements (I_Z5 I_Z6) {
    }
    .class Z8 implements (I_Z5 I_Z6) {
    }
    .class Z9 implements (I_Z5) {
    }
    .class [abstract] Z10 implements (I_Z6) {
    }

    .class [interface] ICanon1 {
    }

    .class [interface] ICanon2 implements (ICanon1) {
    }

    .class [interface] ICanon3 {
    }

    .class [interface] ICanon4 {
    }

    .class [interface] ICanon5 {
    }

    .class [interface] ICanon6 implements (ICanon5) {
    }

    .class [interface] ICanon7 {
    }

    .class [interface] ICanon8 {
    }

    .class [interface] ICanon9 {
    }

    .class [interface] ICanon10 {
    }

    .class [interface] ICanon11 {
    }

    .class [interface] ICanon12 {
    }

    .class [interface] ICanon13 {
    }

    .class [interface] ICanon14 {
    }

    .class Canon1 implements (ICanon3 ICanon4) {
    }

    .class Canon2 implements (ICanon3 ICanon4) {
    }

    .class Canon3 implements (ICanon6) {
    }

    .class Canon4 implements (ICanon9 ICanon10) {
    }

    .class [abstract] Canon5 implements (ICanon11 ICanon12) {
    }

    .class [abstract] Canon6 implements (ICanon11 ICanon12) {
    }

    .class Canon7 implements (ICanon11) {
    }

    .class Canon8 implements (ICanon12) {
    }

    .class Canon9 implements (ICanon11) {
    }

    .class Canon10 implements (ICanon12 ICanon13) {
    }

    .class [abstract] Canon11 implements (ICanon12 ICanon13) {
    }

    .class Canon12 implements (ICanon11 ICanon13) {
    }

    .class Canon13 implements (ICanon11 ICanon13) {
    }

    .class [abstract] Canon14 {
    }

    .class [abstract] Canon15 extends Canon14 {
    }

    .class Canon16 extends Canon14 implements (ICanon14) {
    }

    .class Canon17 extends Canon14 implements (ICanon14) {
    }

    .class Canon18 implements (ICanon14) {
    }

    .class [trait "__NoFlatten"("""v:0:{}""")] T1 {
    }

    .class [trait "__NoFlatten"("""v:0:{}""")] T2 implements (ICanon7) {
    }

    .class [trait "__NoFlatten"("""v:0:{}""")] T3 implements (ICanon7) {
    }

    .class [trait "__NoFlatten"("""v:0:{}""")] T4 implements (ICanon8) {
    }

    .class [abstract] Abs1 implements (ICanon7) {
    }

    .class [abstract] Abs2 {
      .use T4;
    }

    .class [abstract] Abs3 {
    }

    .class [abstract] Abs4 {
    }

    .class T1_C1 {
      .use T1;
    }

    .class T4_C1 {
      .use T4;
    }

    .class Abs3_C1 extends Abs3 {
    }
    .class Abs3_C2 extends Abs3_C1 {
    }
    .class [abstract] Abs3_C3 extends Abs3 {
    }

    .class Abs4_C1 extends Abs4 {
    }
    .class Abs4_C2 extends Abs4 {
    }

    .class Abs5_P {
    }
    .class [abstract] Abs5 extends Abs5_P {
    }

    .class Abs6_P {
    }
    .class [abstract] Abs6 extends Abs6_P {
    }
    .class Abs6_C1 extends Abs6 {
    }

    .class U1 {
    }
    .class U2 extends U1 {
    }

    .function N test() {
      CreateCl 0 "Closure$ChildClosure1"
      CreateCl 0 "Closure$ChildClosure2"
      CreateCl 0 "Closure$ChildClosure3"
      PopC
      PopC
      PopC
      Int 1
      RetC None
    }
  )";

  // Add definitions for builtins to satisfy various assertions.
  for (auto const f : special_builtins()) {
    fmt::format_to(std::back_inserter(hhas), "    .function N {}() {{\n"
      "      Null\n"
      "      RetC None\n"
      "    }}\n",
      f->slice()
    );
  }

  Logger::LogLevel = Logger::LogNone;

  HHBBC::parallel::num_threads = 1;
  HHBBC::parallel::final_threads = 1;

  std::unique_ptr<UnitEmitter> ue{assemble_string(
    hhas,
    "ignore.php",
    SHA1("1234543212345432123454321234543212345432"),
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults()
  )};
  always_assert(!ue->m_fatalUnit);

  auto parse = parse_unit(*ue);

  Index::Input indexInput;

  auto executor = std::make_unique<TicketExecutor>(
    "HHBBCWorker",
    0,
    1,
    [] {
      hphp_thread_init();
      hphp_session_init(Treadmill::SessionKind::HHBBC);
    },
    [] {
      hphp_context_exit();
      hphp_session_exit();
      hphp_thread_exit();
    },
    std::chrono::minutes{15}
  );

  extern_worker::Options options;
  options.setUseSubprocess(extern_worker::Options::UseSubprocess::Always);

  auto client = std::make_unique<Client>(
    executor->sticky(),
    options
  );

  if (parse.unit) {
    auto const name = parse.unit->filename;
    auto stored = coro::blockingWait(client->store(std::move(parse.unit)));
    indexInput.units.emplace_back(
      Index::Input::UnitMeta{
        std::move(stored),
        name
      }
    );
  }

  auto const onCls = [&] (std::unique_ptr<php::Class> c) {
    auto const name = c->name;
    auto deps = Index::Input::makeDeps(*c);
    auto const unit = c->unit;
    auto const closureDeclFunc = c->closureDeclFunc;

    std::vector<SString> closures;
    for (auto const& clo : c->closures) {
      closures.emplace_back(clo->name);
    }

    auto bytecode = std::make_unique<php::ClassBytecode>();
    for (auto& meth : c->methods) {
      bytecode->methodBCs.emplace_back(meth->name, std::move(meth->rawBlocks));
    }

    auto stored = coro::blockingWait(client->store(std::move(c)));
    auto storedBC = coro::blockingWait(client->store(std::move(bytecode)));

    indexInput.classes.emplace_back(
      Index::Input::ClassMeta{
        std::move(stored),
        name,
        std::move(deps),
        closureDeclFunc,
        std::move(closures),
        unit
      }
    );
    indexInput.classBC.emplace_back(
      Index::Input::ClassBytecodeMeta{
        std::move(storedBC),
        name
      }
    );
  };

  for (auto& c : parse.classes) {
    for (auto& clo : c->closures) onCls(std::move(clo));
    onCls(std::move(c));
  }

  for (auto& f : parse.funcs) {
    auto const name = f->name;
    auto const unit = f->unit;
    auto bytecode =
      std::make_unique<php::FuncBytecode>(f->name, std::move(f->rawBlocks));
    auto stored = coro::blockingWait(client->store(std::move(f)));
    auto storedBC = coro::blockingWait(client->store(std::move(bytecode)));
    indexInput.funcs.emplace_back(
      Index::Input::FuncMeta{
        std::move(stored),
        name,
        unit,
        false
      }
    );
    indexInput.funcBC.emplace_back(
      Index::Input::FuncBytecodeMeta{
        std::move(storedBC),
        name,
        nullptr
      }
    );
  }

  Index index{
    std::move(indexInput),
    HHBBC::Config::get(RepoGlobalData{}),
    std::move(executor),
    std::move(client),
    [] (std::unique_ptr<TicketExecutor>,
        std::unique_ptr<Client>) {},
    nullptr
  };
  index.make_local();
  return index;
}

//////////////////////////////////////////////////////////////////////

Type make_specialized_string(trep bits, SString s) {
  return set_trep_for_testing(sval(s), bits);
}

Type make_specialized_lazycls(trep bits, SString s) {
  return set_trep_for_testing(lazyclsval(s), bits);
}

Type make_specialized_enumclasslabel(trep bits, SString s) {
  return set_trep_for_testing(enumclasslabelval(s), bits);
}

Type make_specialized_int(trep bits, int64_t i) {
  return set_trep_for_testing(ival(i), bits);
}

Type make_specialized_double(trep bits, double d) {
  return set_trep_for_testing(dval(d), bits);
}

Type make_specialized_wait_handle(trep bits, Type inner) {
  return set_trep_for_testing(wait_handle(std::move(inner)), bits);
}

Type make_specialized_exact_object(trep bits, res::Class cls,
                                   bool isCtx = false,
                                   bool canonicalize = true) {
  return make_obj_for_testing(bits, cls, false, isCtx, canonicalize);
}

Type make_specialized_sub_object(trep bits, res::Class cls,
                                 bool isCtx = false,
                                 bool canonicalize = true) {
  return make_obj_for_testing(bits, cls, true, isCtx, canonicalize);
}

Type make_specialized_exact_class(trep bits, res::Class cls,
                                  bool isCtx = false,
                                  bool canonicalize = true,
                                  bool nonReg = true) {
  return make_cls_for_testing(bits, cls, false, isCtx, canonicalize, nonReg);
}

Type make_specialized_sub_class(trep bits, res::Class cls,
                                bool isCtx = false,
                                bool canonicalize = true,
                                bool nonReg = true) {
  return make_cls_for_testing(bits, cls, true, isCtx, canonicalize, nonReg);
}

Type make_specialized_arrval(trep bits, SArray ar) {
  return make_arrval_for_testing(bits, ar);
}

Type make_specialized_arrpacked(trep bits,
                                std::vector<Type> elems,
                                Optional<LegacyMark> mark = std::nullopt) {
  return make_arrpacked_for_testing(bits, std::move(elems), mark);
}

Type make_specialized_arrpackedn(trep bits, Type type) {
  return make_arrpackedn_for_testing(bits, std::move(type));
}

Type make_specialized_arrmap(trep bits, MapElems elems,
                             Type optKey = TBottom, Type optVal = TBottom,
                             Optional<LegacyMark> mark = std::nullopt) {
  return make_arrmap_for_testing(
    bits, std::move(elems), std::move(optKey), std::move(optVal), mark
  );
}

Type make_specialized_arrmapn(trep bits, Type key, Type val) {
  return make_arrmapn_for_testing(bits, std::move(key), std::move(val));
}

trep get_bits(const Type& t) { return get_trep_for_testing(t); }

Type make_unmarked(Type t) {
  if (!t.couldBe(BVec|BDict)) return t;
  return set_mark_for_testing(t, LegacyMark::Unmarked);
}

void make_unmarked(std::vector<Type>& types) {
  for (auto& t : types) t = make_unmarked(std::move(t));
}

//////////////////////////////////////////////////////////////////////

Type sval(const StaticString& s) { return HPHP::HHBBC::sval(s.get()); }
Type sval_nonstatic(const StaticString& s) {
  return HPHP::HHBBC::sval_nonstatic(s.get());
}
Type sval_counted(const StaticString& s) {
  return HPHP::HHBBC::sval_counted(s.get());
}

Type lazyclsval(const StaticString& s) {
  return HPHP::HHBBC::lazyclsval(s.get());
}

Type enumclasslabelval(const StaticString& s) {
  return HPHP::HHBBC::enumclasslabelval(s.get());
}

TypedValue tv(SString s) { return make_tv<KindOfPersistentString>(s); }
TypedValue tv(const StaticString& s) { return tv(s.get()); }
TypedValue tv(int64_t i) { return make_tv<KindOfInt64>(i); }

std::pair<TypedValue, MapElem> map_elem(int64_t i, Type t) {
  return {tv(i), MapElem::IntKey(std::move(t))};
}
std::pair<TypedValue, MapElem> map_elem(SString s, Type t) {
  return {tv(s), MapElem::SStrKey(std::move(t))};
}
std::pair<TypedValue, MapElem> map_elem(const StaticString& s, Type t) {
  return {tv(s), MapElem::SStrKey(std::move(t))};
}

std::pair<TypedValue, MapElem> map_elem_nonstatic(const StaticString& s,
                                                  Type t) {
  return {tv(s), MapElem::StrKey(std::move(t))};
}
std::pair<TypedValue, MapElem> map_elem_counted(const StaticString& s,
                                                Type t) {
  return {tv(s), MapElem::CStrKey(std::move(t))};
}

template<typename... Args>
SArray static_vec(Args&&... args) {
  auto ar = make_vec_array(std::forward<Args>(args)...);
  return ArrayData::GetScalarArray(std::move(ar));
}

template<typename... Args>
SArray static_dict(Args&&... args) {
  auto ar = make_dict_array(std::forward<Args>(args)...);
  return ArrayData::GetScalarArray(std::move(ar));
}

template<typename... Args>
SArray static_keyset(Args&&... args) {
  auto ar = make_keyset_array(std::forward<Args>(args)...);
  return ArrayData::GetScalarArray(std::move(ar));
}

//////////////////////////////////////////////////////////////////////

auto const predefined = folly::lazy([]{
  std::vector<std::pair<trep, Type>> types{
#define X(y, ...) { B##y, T##y },
    HHBBC_TYPE_PREDEFINED(X)
#undef X
  };
  types.emplace_back(BInt|BObj, Type{BInt|BObj});
  types.emplace_back(BKeysetN|BVecN, Type{BKeysetN|BVecN});
  types.emplace_back(BKeysetN|BDictN, Type{BKeysetN|BDictN});
  types.emplace_back(BKeysetE|BVecE, Type{BKeysetE|BVecE});
  types.emplace_back(BKeysetE|BDictE, Type{BKeysetE|BDictE});
  return types;
});

auto const optionals = folly::lazy([]{
  std::vector<Type> opts;
  for (auto const& p : predefined()) {
    if (p.first == BTop) continue;
    if (!couldBe(p.first, BInitNull) || subtypeOf(p.first, BInitNull)) continue;
    opts.emplace_back(p.second);
  }
  return opts;
});

// In the sense of "non-union type", not the sense of TPrim.
auto const primitives = folly::lazy([]{
  return std::vector<Type>{
#define X(y) T##y,
    HHBBC_TYPE_SINGLE(X)
#undef X
  };
});

std::vector<Type> withData(const Index& index) {
  std::vector<Type> types;

  auto const clsA = index.resolve_class(s_A.get());
  if (!clsA || !clsA->isComplete()) ADD_FAILURE();
  auto const clsAA = index.resolve_class(s_AA.get());
  if (!clsAA || !clsAA->isComplete()) ADD_FAILURE();
  auto const clsAB = index.resolve_class(s_AB.get());
  if (!clsAB || !clsAB->isComplete()) ADD_FAILURE();

  auto const clsIBase = index.resolve_class(s_IBase.get());
  if (!clsIBase || !clsIBase->isComplete()) ADD_FAILURE();
  auto const clsIA = index.resolve_class(s_IA.get());
  if (!clsIA || !clsIA->isComplete()) ADD_FAILURE();
  auto const clsIAA = index.resolve_class(s_IAA.get());
  if (!clsIAA || !clsIAA->isComplete()) ADD_FAILURE();
  auto const clsIB = index.resolve_class(s_IB.get());
  if (!clsIB || !clsIB->isComplete()) ADD_FAILURE();

  auto const clsI_A = index.resolve_class(s_I_A.get());
  if (!clsI_A || !clsI_A->isComplete()) ADD_FAILURE();
  auto const clsI_B = index.resolve_class(s_I_B.get());
  if (!clsI_B || !clsI_B->isComplete()) ADD_FAILURE();
  auto const clsI_C = index.resolve_class(s_I_C.get());
  if (!clsI_C || !clsI_C->isComplete()) ADD_FAILURE();
  auto const clsI_D = index.resolve_class(s_I_D.get());
  if (!clsI_D || !clsI_D->isComplete()) ADD_FAILURE();
  auto const clsI_E = index.resolve_class(s_I_E.get());
  if (!clsI_E || !clsI_E->isComplete()) ADD_FAILURE();
  auto const clsI_F = index.resolve_class(s_I_F.get());
  if (!clsI_F || !clsI_F->isComplete()) ADD_FAILURE();
  auto const clsI_G = index.resolve_class(s_I_G.get());
  if (!clsI_G || !clsI_G->isComplete()) ADD_FAILURE();
  auto const clsI_H = index.resolve_class(s_I_H.get());
  if (!clsI_H || !clsI_H->isComplete()) ADD_FAILURE();
  auto const clsI_I = index.resolve_class(s_I_I.get());
  if (!clsI_I || !clsI_I->isComplete()) ADD_FAILURE();
  auto const clsI_J = index.resolve_class(s_I_J.get());
  if (!clsI_J || !clsI_J->isComplete()) ADD_FAILURE();
  auto const clsI_K = index.resolve_class(s_I_K.get());
  if (!clsI_K || !clsI_K->isComplete()) ADD_FAILURE();

  auto const clsX1 = index.resolve_class(s_X1.get());
  if (!clsX1 || !clsX1->isComplete()) ADD_FAILURE();
  auto const clsX2 = index.resolve_class(s_X2.get());
  if (!clsX2 || !clsX2->isComplete()) ADD_FAILURE();
  auto const clsX3 = index.resolve_class(s_X3.get());
  if (!clsX3 || !clsX3->isComplete()) ADD_FAILURE();
  auto const clsX4 = index.resolve_class(s_X4.get());
  if (!clsX4 || !clsX4->isComplete()) ADD_FAILURE();
  auto const clsX7 = index.resolve_class(s_X7.get());
  if (!clsX7 || !clsX7->isComplete()) ADD_FAILURE();
  auto const clsX11 = index.resolve_class(s_X11.get());
  if (!clsX11 || !clsX11->isComplete()) ADD_FAILURE();
  auto const clsX21 = index.resolve_class(s_X21.get());
  if (!clsX21 || !clsX21->isComplete()) ADD_FAILURE();

  auto const clsFoo1 = res::Class::getUnresolved(s_Foo1.get());
  if (clsFoo1.isComplete()) ADD_FAILURE();
  auto const clsFoo2 = res::Class::getUnresolved(s_Foo2.get());
  if (clsFoo2.isComplete()) ADD_FAILURE();

  auto const svec1 = static_vec(s_A.get(), s_B.get());
  auto const svec2 = static_vec(123, 456);
  auto const sdict1 = static_dict(s_A.get(), s_B.get(), s_C.get(), 123);
  auto const sdict2 = static_dict(100, s_A.get(), 200, s_C.get());
  auto const skeyset1 = static_keyset(s_A.get(), s_B.get());
  auto const skeyset2 = static_keyset(123, 456);

  auto const support = BStr | BDbl | BInt | BCls | BObj |
                       BArrLikeN | BLazyCls | BEnumClassLabel;
  auto const nonSupport = BCell & ~support;

  auto const add = [&] (trep b) {
    types.emplace_back(Type{b});

    if (b == BTop) return;

    if (couldBe(b, BStr) && subtypeOf(b, BStr | nonSupport)) {
      types.emplace_back(make_specialized_string(b, s_A.get()));
      types.emplace_back(make_specialized_string(b, s_B.get()));
    }
    if (couldBe(b, BLazyCls) && subtypeOf(b, BLazyCls | nonSupport)) {
      types.emplace_back(make_specialized_lazycls(b, s_A.get()));
      types.emplace_back(make_specialized_lazycls(b, s_B.get()));
    }
    if (couldBe(b, BEnumClassLabel) && subtypeOf(b, BEnumClassLabel | nonSupport)) {
      types.emplace_back(make_specialized_enumclasslabel(b, s_A.get()));
      types.emplace_back(make_specialized_enumclasslabel(b, s_B.get()));
    }
    if (couldBe(b, BInt) && subtypeOf(b, BInt | nonSupport)) {
      types.emplace_back(make_specialized_int(b, 123));
      types.emplace_back(make_specialized_int(b, 456));
    }
    if (couldBe(b, BDbl) && subtypeOf(b, BDbl | nonSupport)) {
      types.emplace_back(make_specialized_double(b, 3.141));
      types.emplace_back(make_specialized_double(b, 2.718));
    }
    if (couldBe(b, BObj) && subtypeOf(b, BObj | nonSupport)) {
      types.emplace_back(make_specialized_wait_handle(b, TInt));
      types.emplace_back(make_specialized_wait_handle(b, TStr));
      types.emplace_back(make_specialized_wait_handle(b, TArrKey));
      types.emplace_back(make_specialized_wait_handle(b, TBottom));

      types.emplace_back(make_specialized_exact_object(b, *clsA));
      types.emplace_back(make_specialized_exact_object(b, *clsAA));
      types.emplace_back(make_specialized_exact_object(b, *clsAB));

      types.emplace_back(make_specialized_exact_object(b, *clsIBase));
      types.emplace_back(make_specialized_exact_object(b, *clsIA));
      types.emplace_back(make_specialized_exact_object(b, *clsIAA));
      types.emplace_back(make_specialized_exact_object(b, *clsIB));

      types.emplace_back(make_specialized_sub_object(b, *clsA));
      types.emplace_back(make_specialized_sub_object(b, *clsAA));
      types.emplace_back(make_specialized_sub_object(b, *clsAB));

      auto const subIBase = make_specialized_sub_object(b, *clsIBase);
      auto const subIA = make_specialized_sub_object(b, *clsIA);
      auto const subIAA = make_specialized_sub_object(b, *clsIAA);
      auto const subIB = make_specialized_sub_object(b, *clsIB);
      types.emplace_back(subIBase);
      types.emplace_back(subIA);
      types.emplace_back(subIAA);
      types.emplace_back(subIB);

      if (subtypeOf(b, BInitCell)) {
        types.emplace_back(make_specialized_wait_handle(b, subIBase));
        types.emplace_back(make_specialized_wait_handle(b, subIA));
        types.emplace_back(make_specialized_wait_handle(b, subIAA));
        types.emplace_back(make_specialized_wait_handle(b, subIB));
      }

      types.emplace_back(make_specialized_exact_object(b, *clsA, true));
      types.emplace_back(make_specialized_exact_object(b, *clsAA, true));
      types.emplace_back(make_specialized_exact_object(b, *clsAB, true));

      types.emplace_back(make_specialized_sub_object(b, *clsA, true));
      types.emplace_back(make_specialized_sub_object(b, *clsAA, true));
      types.emplace_back(make_specialized_sub_object(b, *clsAB, true));

      auto const dobj =
        dobj_of(make_specialized_wait_handle(b, TArrKey));
      if (!dobj.cls().isComplete()) ADD_FAILURE();
      types.emplace_back(make_specialized_sub_object(b, dobj.cls()));
      types.emplace_back(make_specialized_exact_object(b, dobj.cls()));

      types.emplace_back(make_specialized_sub_object(b, *clsI_A));
      types.emplace_back(make_specialized_exact_object(b, *clsI_A));
      types.emplace_back(make_specialized_sub_object(b, *clsI_B));
      types.emplace_back(make_specialized_exact_object(b, *clsI_B));
      types.emplace_back(make_specialized_sub_object(b, *clsI_C));
      types.emplace_back(make_specialized_sub_object(b, *clsI_E));
      types.emplace_back(make_specialized_sub_object(b, *clsI_F));
      types.emplace_back(make_specialized_sub_object(b, *clsI_G));
      types.emplace_back(make_specialized_sub_object(b, *clsI_H));
      types.emplace_back(make_specialized_sub_object(b, *clsI_J));
      types.emplace_back(make_specialized_sub_object(b, *clsI_K));

      types.emplace_back(make_specialized_sub_object(b, *clsX1));
      types.emplace_back(make_specialized_sub_object(b, *clsX2));
      types.emplace_back(make_specialized_exact_object(b, *clsX2));
      types.emplace_back(make_specialized_sub_object(b, *clsX3));
      types.emplace_back(make_specialized_exact_object(b, *clsX3));
      types.emplace_back(make_specialized_sub_object(b, *clsX4));
      types.emplace_back(make_specialized_sub_object(b, *clsX7));
      types.emplace_back(make_specialized_sub_object(b, *clsX11));
      types.emplace_back(make_specialized_sub_object(b, *clsX21));

      types.emplace_back(make_specialized_exact_object(b, clsFoo1));
      types.emplace_back(make_specialized_sub_object(b, clsFoo1));
      types.emplace_back(make_specialized_exact_object(b, clsFoo2));
      types.emplace_back(make_specialized_sub_object(b, clsFoo2));
    }
    if (couldBe(b, BCls) && subtypeOf(b, BCls | nonSupport)) {
      types.emplace_back(make_specialized_exact_class(b, *clsA));
      types.emplace_back(make_specialized_exact_class(b, *clsAA));
      types.emplace_back(make_specialized_exact_class(b, *clsAB));

      types.emplace_back(make_specialized_sub_class(b, *clsA));
      types.emplace_back(make_specialized_sub_class(b, *clsAA));
      types.emplace_back(make_specialized_sub_class(b, *clsAB));

      types.emplace_back(make_specialized_exact_class(b, *clsA, true));
      types.emplace_back(make_specialized_exact_class(b, *clsAA, true));
      types.emplace_back(make_specialized_exact_class(b, *clsAB, true));

      types.emplace_back(make_specialized_sub_class(b, *clsA, true));
      types.emplace_back(make_specialized_sub_class(b, *clsAA, true));
      types.emplace_back(make_specialized_sub_class(b, *clsAB, true));
    }

    if (couldBe(b, BArrLikeN) && subtypeOf(b, BArrLikeN | nonSupport)) {
      if (subtypeAmong(b, BKeysetN, BArrLikeN)) {
        types.emplace_back(make_specialized_arrpacked(b, {ival(0), ival(1)}));
        types.emplace_back(make_specialized_arrpackedn(b, TInt));
        types.emplace_back(make_specialized_arrmapn(b, TInt, TInt));
        types.emplace_back(make_specialized_arrmap(b, {map_elem(1, ival(1))}));
        types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, sval(s_A))}));

        if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
          types.emplace_back(make_specialized_arrmapn(b, TSStr, TSStr));
        } else {
          types.emplace_back(make_specialized_arrmapn(b, TStr, TStr));
        }
      } else if (couldBe(b, BKeysetN)) {
        types.emplace_back(make_specialized_arrpacked(b, {TInt, TInt}));
        types.emplace_back(make_specialized_arrpacked(b, {TInitPrim, TInitPrim}));
        types.emplace_back(make_specialized_arrpackedn(b, TInt));
        types.emplace_back(make_specialized_arrpackedn(b, TInitPrim));

        if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
          types.emplace_back(make_specialized_arrpacked(b, {TInitUnc, TInitUnc}));
          types.emplace_back(make_specialized_arrpackedn(b, TUncArrKey));
          types.emplace_back(make_specialized_arrmapn(b, TInt, TUncArrKey));
          types.emplace_back(make_specialized_arrmapn(b, TUncArrKey, TSStr));
          types.emplace_back(make_specialized_arrmapn(b, TUncArrKey, TUncArrKey));
        } else {
          types.emplace_back(make_specialized_arrpacked(b, {TInitCell, TInitCell}));
          types.emplace_back(make_specialized_arrpackedn(b, TArrKey));
          types.emplace_back(make_specialized_arrpackedn(b, union_of(TObj, TInt)));
          types.emplace_back(make_specialized_arrmapn(b, TInt, TArrKey));
          types.emplace_back(make_specialized_arrmapn(b, TArrKey, TStr));
          types.emplace_back(make_specialized_arrmapn(b, TArrKey, TArrKey));
        }

        if (!couldBe(b, BVecN)) {
          types.emplace_back(make_specialized_arrmap(b, {map_elem(1, TInt)}));
          if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TSStr)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TInt, TSStr));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TSStr, TInt));
          } else {
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TStr)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitCell)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TInt, TStr));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TStr, TInt));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TStr, TObj));
          }
        }
      } else {
        types.emplace_back(make_specialized_arrpacked(b, {TInt, TInt}));
        types.emplace_back(make_specialized_arrpackedn(b, TInt));

        if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
          types.emplace_back(make_specialized_arrpacked(b, {TSStr, TSStr}));
          types.emplace_back(make_specialized_arrpacked(b, {TInitUnc, TInitUnc}));
          types.emplace_back(make_specialized_arrpackedn(b, TSStr));
          types.emplace_back(make_specialized_arrpackedn(b, TUncArrKey));
        } else {
          types.emplace_back(make_specialized_arrpacked(b, {TStr, TStr}));
          types.emplace_back(make_specialized_arrpacked(b, {TInitCell, TInitCell}));
          types.emplace_back(make_specialized_arrpackedn(b, TStr));
          types.emplace_back(make_specialized_arrpackedn(b, TArrKey));
          types.emplace_back(make_specialized_arrpackedn(b, TObj));
        }

        if (!subtypeAmong(b, BVecN, BArrLikeN)) {
          types.emplace_back(make_specialized_arrmapn(b, TInt, TInt));
          if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
            if (!couldBe(b, BVecN)) {
              types.emplace_back(make_specialized_arrmapn(b, TSStr, TSStr));
            }
            types.emplace_back(make_specialized_arrmapn(b, TUncArrKey, TUncArrKey));
          } else {
            if (!couldBe(b, BVecN)) {
              types.emplace_back(make_specialized_arrmapn(b, TStr, TStr));
            }
            types.emplace_back(make_specialized_arrmapn(b, TArrKey, TArrKey));
            types.emplace_back(make_specialized_arrmapn(b, TArrKey, TObj));
          }
        }

        if (!couldBe(b, BVecN)) {
          if (subtypeAmong(b, BSArrLikeN, BArrLikeN)) {
            types.emplace_back(make_specialized_arrmap(b, {map_elem(1, TSStr)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TInt, TSStr));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TSStr, TInt));
          } else {
            types.emplace_back(make_specialized_arrmap(b, {map_elem(1, TStr)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitCell)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TObj)}));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TInt, TStr));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TStr, TInt));
            types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInitUnc)}, TStr, TObj));
          }
          types.emplace_back(make_specialized_arrmap(b, {map_elem(s_A, TInt)}));
        }
      }

      if (subtypeAmong(b, BSVecN, BArrLikeN)) {
        types.emplace_back(make_specialized_arrval(b, svec1));
        types.emplace_back(make_specialized_arrval(b, svec2));
      }
      if (subtypeAmong(b, BSDictN, BArrLikeN)) {
        types.emplace_back(make_specialized_arrval(b, sdict1));
        types.emplace_back(make_specialized_arrval(b, sdict2));
      }
      if (subtypeAmong(b, BSKeysetN, BArrLikeN)) {
        types.emplace_back(make_specialized_arrval(b, skeyset1));
        types.emplace_back(make_specialized_arrval(b, skeyset2));
      }
    }
  };

  for (auto const& t : predefined()) add(t.first);

  make_unmarked(types);
  return types;
}

auto const specialized_arrays = folly::lazy([]{
  std::vector<Type> types;

  auto const add = [&] (trep b) {
    if (!b) return;

    types.emplace_back(Type{b});

    auto const containsUncounted = [] (trep bits) {
      if ((bits & BVecN)    == BSVecN)    return true;
      if ((bits & BDictN)   == BSDictN)   return true;
      if ((bits & BKeysetN) == BSKeysetN) return true;
      return false;
    };

    auto const keyBits = [&containsUncounted] (trep bits) {
      auto upper = BBottom;
      auto lower = BArrKey;

      if (couldBe(bits, BVec)) {
        upper |= BInt;
        lower &= BInt;
        bits &= ~BVec;
      }
      if (couldBe(bits, BArrLikeN)) {
        if (subtypeOf(bits, BSArrLikeN)) {
          upper |= BUncArrKey;
          lower &= BUncArrKey;
        } else {
          upper |= BArrKey;
          lower &= containsUncounted(bits) ? BUncArrKey : BArrKey;
        }
      }
      return std::make_pair(upper, lower);
    }(b);

    auto const calcValBits = [&containsUncounted] (trep bits, bool packed) {
      auto upper = BBottom;
      auto lower = BInitCell;

      if (couldBe(bits, BKeysetN)) {
        if (packed) {
          upper |= BInt;
          lower &= BInt;
        } else if (subtypeAmong(bits, BSKeysetN, BKeysetN)) {
          upper |= BUncArrKey;
          lower |= BUncArrKey;
        } else {
          upper |= BArrKey;
          lower &= BArrKey;
        }
        bits &= ~BKeysetN;
      }
      if (couldBe(bits, BArrLikeN)) {
        if (subtypeOf(bits, BSArrLikeN)) {
          upper |= BInitUnc;
          lower &= BInitUnc;
        } else {
          upper |= BInitCell;
          lower &= containsUncounted(bits) ? BInitUnc : BInitCell;
        }
      }
      return std::make_pair(upper, lower);
    };
    auto const packedValBits = calcValBits(b, true);
    auto const valBits = calcValBits(b, false);

    auto const packedn = [&] (const Type& t) {
      if (!t.subtypeOf(packedValBits.first)) return;
      if (!t.couldBe(packedValBits.second)) return;
      if (subtypeOf(b, BVecN) && !t.strictSubtypeOf(packedValBits.first)) {
        return;
      }
      types.emplace_back(make_specialized_arrpackedn(b, t));
    };
    packedn(TInt);
    packedn(TSStr);
    packedn(TStr);
    packedn(TUncArrKey);
    packedn(TArrKey);
    packedn(TObj);
    packedn(TInitUnc);
    packedn(TInitCell);
    packedn(Type{BInt|BObj});
    packedn(Type{BInitCell & ~BObj});
    packedn(Type{BInitUnc & ~BSStr});

    auto const packed = [&] (const std::vector<Type>& elems) {
      for (size_t i = 0; i < elems.size(); ++i) {
        auto const& t = elems[i];
        if (!t.subtypeOf(packedValBits.first)) return;
        if (!t.couldBe(packedValBits.second)) return;
        if (couldBe(b, BKeysetN) && !t.couldBe(ival(i))) return;
        if (subtypeOf(b, BKeysetN) && !t.subtypeOf(ival(i))) return;
      }
      types.emplace_back(make_specialized_arrpacked(b, elems));
    };
    packed({TInt, TInt});
    packed({TSStr, TSStr});
    packed({TStr, TStr});
    packed({TUncArrKey, TUncArrKey});
    packed({TArrKey, TArrKey});
    packed({TObj, TObj});
    packed({TInitUnc, TInitUnc});
    packed({TInitCell, TInitCell});
    packed({Type{BInt|BObj}, Type{BInt|BObj}});
    packed({TInt, TObj});
    packed({TSStr, TStr});
    packed({ival(0)});
    packed({ival(0), ival(1)});
    packed({ival(100), ival(200)});
    packed({union_of(TObj,ival(0)), union_of(TObj,ival(1))});
    packed({TInt, TInt, TInt});
    packed({TObj, TObj, TObj});
    packed({TArrKey, TArrKey, TArrKey});

    auto const mapn = [&] (const Type& key, const Type& val) {
      if (subtypeOf(b, BVecN)) return;
      if (!key.subtypeOf(keyBits.first)) return;
      if (!key.couldBe(keyBits.second)) return;
      if (!val.subtypeOf(valBits.first)) return;
      if (!val.couldBe(valBits.second)) return;
      if (!key.strictSubtypeOf(keyBits.first) &&
          !val.strictSubtypeOf(valBits.first)) return;
      if (couldBe(b, BKeysetN) && !key.couldBe(val)) return;
      if (subtypeOf(b, BKeysetN) && key != val) return;
      types.emplace_back(make_specialized_arrmapn(b, key, val));
    };
    mapn(TInt, TInt);
    mapn(TSStr, TSStr);
    mapn(TStr, TStr);
    mapn(TUncArrKey, TUncArrKey);
    mapn(TArrKey, TArrKey);
    mapn(TUncArrKey, TInt);
    mapn(TUncArrKey, TSStr);
    mapn(TInt, TUncArrKey);
    mapn(TSStr, TUncArrKey);
    mapn(TInt, TSStr);
    mapn(TSStr, TInt);
    mapn(TUncArrKey, TObj);
    mapn(TArrKey, TObj);
    mapn(TUncArrKey, TInitUnc);
    mapn(TArrKey, TInitUnc);
    mapn(TUncArrKey, TInitCell);
    mapn(TArrKey, TInitCell);
    mapn(TInt, Type{BInt|BObj});
    mapn(TSStr, Type{BInt|BObj});
    mapn(TInt, TInitUnc);
    mapn(TSStr, TInitUnc);
    mapn(TArrKey, Type{BInitCell & ~BObj});
    mapn(TUncArrKey, Type{BInitUnc & ~BSStr});
    mapn(TInt, TInitCell);
    mapn(TStr, TInitCell);

    auto const map = [&] (const MapElems& elems,
                          const Type& optKey = TBottom,
                          const Type& optVal = TBottom) {
      if (couldBe(b, BVecN)) return;

      for (auto const& e : elems) {
        if (!e.second.val.subtypeOf(valBits.first)) return;
        if (!e.second.val.couldBe(valBits.second)) return;

        auto const key = [&] {
          if (isIntType(e.first.m_type)) return ival(e.first.m_data.num);
          switch (e.second.keyStaticness) {
            case TriBool::Yes:   return HPHP::HHBBC::sval(e.first.m_data.pstr);
            case TriBool::Maybe: return HPHP::HHBBC::sval_nonstatic(e.first.m_data.pstr);
            case TriBool::No:    return HPHP::HHBBC::sval_counted(e.first.m_data.pstr);
          }
          always_assert(false);
        }();

        if (!key.subtypeOf(keyBits.first)) return;
        if (!key.couldBe(keyBits.second)) return;

        if (couldBe(b, BKeysetN) && !key.couldBe(e.second.val)) return;
        if (subtypeOf(b, BKeysetN) && key != e.second.val) return;
      }
      if (!optKey.is(BBottom)) {
        if (!optKey.subtypeOf(keyBits.first)) return;
        if (!optVal.subtypeOf(valBits.first)) return;
        if (subtypeOf(b, BKeysetN) && optKey != optVal) return;
      }
      types.emplace_back(make_specialized_arrmap(b, elems, optKey, optVal));
    };
    map({map_elem(s_A, TInt)});
    map({map_elem(s_A, TSStr)});
    map({map_elem(s_A, TStr)});
    map({map_elem(s_A, sval(s_A))});
    map({map_elem(s_A, sval_nonstatic(s_A))});
    map({map_elem(s_A, TUncArrKey)});
    map({map_elem(s_A, TArrKey)});
    map({map_elem(s_A, TObj)});
    map({map_elem(s_A, TInitUnc)});
    map({map_elem(s_A, TInitCell)});
    map({map_elem(s_A, Type{BInt|BObj})});
    map({map_elem(s_A, TInt)}, TInt, TInt);
    map({map_elem(s_A, TInt)}, TInt, TSStr);
    map({map_elem(s_A, TInt)}, TInt, TStr);
    map({map_elem(s_A, TInt)}, TInt, TUncArrKey);
    map({map_elem(s_A, TInt)}, TInt, TArrKey);
    map({map_elem(s_A, TInt)}, TInt, TObj);
    map({map_elem(s_A, TInt)}, TInt, TInitUnc);
    map({map_elem(s_A, TInt)}, TInt, TInitCell);
    map({map_elem(s_A, TInt)}, TSStr, TInt);
    map({map_elem(s_A, TInt)}, TSStr, TSStr);
    map({map_elem(s_A, TInt)}, TSStr, TStr);
    map({map_elem(s_A, TInt)}, TSStr, TUncArrKey);
    map({map_elem(s_A, TInt)}, TSStr, TArrKey);
    map({map_elem(s_A, TInt)}, TSStr, TObj);
    map({map_elem(s_A, TInt)}, TSStr, TInitUnc);
    map({map_elem(s_A, TInt)}, TSStr, TInitCell);
    map({map_elem(s_A, sval(s_A))}, TInt, Type{BInt|BObj});
    map({map_elem(s_B, TInt)});
    map({map_elem(s_B, sval(s_B))});
    map({map_elem(123, TInt)});
    map({map_elem(123, TSStr)});
    map({map_elem(123, TStr)});
    map({map_elem(123, ival(123))});
    map({map_elem(123, TUncArrKey)});
    map({map_elem(123, TArrKey)});
    map({map_elem(123, TObj)});
    map({map_elem(123, TInitUnc)});
    map({map_elem(123, TInitCell)});
    map({map_elem(123, Type{BInt|BObj})});
    map({map_elem(std::numeric_limits<int64_t>::max(), TStr)});
    map({map_elem_nonstatic(s_A, TInt)});
    map({map_elem_nonstatic(s_A, TSStr)});
    map({map_elem_nonstatic(s_A, TStr)});
    map({map_elem_nonstatic(s_A, sval(s_A))});
    map({map_elem_nonstatic(s_A, sval_nonstatic(s_A))});
    map({map_elem_nonstatic(s_A, Type{BStr|BObj})});
  };

  auto const bits = std::vector<trep>{
    BCVecN,
    BSVecN,
    BCDictN,
    BSDictN,
    BCKeysetN,
    BSKeysetN,
  };

  auto const subsetSize = 1ULL << bits.size();
  for (size_t i = 0; i < subsetSize; ++i) {
    auto b = BBottom;
    for (size_t j = 0; j < bits.size(); ++j) {
      if (i & (1ULL << j)) b |= bits[j];
    }
    add(b);
  }

  auto const svec1 = static_vec(s_A.get(), s_B.get());
  auto const svec2 = static_vec(123, 456);
  auto const sdict1 = static_dict(s_A.get(), s_B.get(), s_C.get(), 123);
  auto const sdict2 = static_dict(100, s_A.get(), 200, s_C.get());
  auto const skeyset1 = static_keyset(s_A.get(), s_B.get());
  auto const skeyset2 = static_keyset(123, 456);

  types.emplace_back(make_specialized_arrval(BSVecN, svec1));
  types.emplace_back(make_specialized_arrval(BSVecN, svec2));
  types.emplace_back(make_specialized_arrval(BSDictN, sdict1));
  types.emplace_back(make_specialized_arrval(BSDictN, sdict2));
  types.emplace_back(make_specialized_arrval(BSKeysetN, skeyset1));
  types.emplace_back(make_specialized_arrval(BSKeysetN, skeyset2));

  types.emplace_back(make_specialized_arrpackedn(BVecN|BKeysetN, ival(0)));

  make_unmarked(types);
  return types;
});

std::vector<Type> specializedClasses(const Index& index) {
  hphp_fast_set<Type, TypeHasher> types;
  auto const addExactSub = [&] (res::Class c) {
    types.emplace(objExact(c));
    types.emplace(subObj(c));
    types.emplace(clsExact(c, true));
    types.emplace(subCls(c, true));
    types.emplace(clsExact(c, false));
    types.emplace(subCls(c, false));
  };

#define X(name)                                                         \
  {                                                                     \
    auto const cls = index.resolve_class(s_##name.get());               \
    if (!cls || !cls->isComplete()) ADD_FAILURE();                      \
    addExactSub(*cls);                                                  \
  }
#define Y(name)                                                         \
  {                                                                     \
    auto const cls = res::Class::getUnresolved(s_##name.get());         \
    if (cls.isComplete()) ADD_FAILURE();                                \
    addExactSub(cls);                                                   \
  }
#define Z(name)                                                         \
  {                                                                     \
    auto cls = index.resolve_class(s_##name.get());                     \
    if (!cls || !cls->isComplete()) ADD_FAILURE();                      \
    cls->makeConservativeForTest();                                     \
    if (cls->hasCompleteChildren()) ADD_FAILURE();                      \
    addExactSub(*cls);                                                  \
  }
  TEST_CLASSES
#undef Z
#undef Y
#undef X

  auto const childClo1 = index.resolve_class(s_ChildClosure1.get());
  if (!childClo1 || !childClo1->isComplete()) ADD_FAILURE();
  auto const childClo2 = index.resolve_class(s_ChildClosure2.get());
  if (!childClo2 || !childClo2->isComplete()) ADD_FAILURE();
  auto const childClo3 = index.resolve_class(s_ChildClosure3.get());
  if (!childClo3 || !childClo3->isComplete()) ADD_FAILURE();

  addExactSub(*childClo1);
  addExactSub(*childClo2);
  addExactSub(*childClo3);

  auto const awaitable = res::Class::get(s_Awaitable.get());
  addExactSub(awaitable);

  auto const clsX2 = index.resolve_class(s_X2.get());
  auto const clsX6 = index.resolve_class(s_X6.get());
  auto const clsX18 = index.resolve_class(s_X18.get());
  auto const clsY3 = index.resolve_class(s_Y3.get());

  types.emplace(make_specialized_wait_handle(BObj, TInt));
  types.emplace(make_specialized_wait_handle(BObj, TStr));
  types.emplace(make_specialized_wait_handle(BObj, TArrKey));
  types.emplace(make_specialized_wait_handle(BObj, TBottom));

  types.emplace(make_specialized_wait_handle(BObj, subObj(*clsX2)));
  types.emplace(make_specialized_wait_handle(BObj, subObj(*clsX6)));
  types.emplace(make_specialized_wait_handle(BObj, subObj(*clsX18)));
  types.emplace(make_specialized_wait_handle(BObj, subObj(*clsY3)));
  types.emplace(make_specialized_wait_handle(BObj, objExact(*clsX2)));
  types.emplace(make_specialized_wait_handle(BObj, objExact(*clsX6)));
  types.emplace(make_specialized_wait_handle(BObj, objExact(*clsX18)));
  types.emplace(make_specialized_wait_handle(BObj, objExact(*clsY3)));

  types.emplace(TObj);
  types.emplace(TCls);

  while (true) {
    auto combinations = types;
    for (auto const& a : types) {
      for (auto const& b : types) {
        combinations.emplace(union_of(a, b));
        combinations.emplace(intersection_of(a, b));
      }
    }
    if (types.size() == combinations.size()) break;
    types = std::move(combinations);
  }
  return std::vector<Type>{begin(types), end(types)};
}

std::vector<Type> allCases(const Index& index) {
  auto types = withData(index);
  auto const specialized = specialized_arrays();
  types.insert(types.end(), specialized.begin(), specialized.end());
  auto const specializedCls = specializedClasses(index);
  types.insert(types.end(), specializedCls.begin(), specializedCls.end());
  return types;
}

auto const allBits = folly::lazy([]{
  std::vector<trep> bits;

  for (auto const& p : predefined()) {
    bits.emplace_back(p.first);
    if (!(p.first & BInitNull)) bits.emplace_back(p.first | BInitNull);
  }

  auto const arrbits = std::vector<trep>{
    BCVecN,
    BSVecN,
    BCDictN,
    BSDictN,
    BCKeysetN,
    BSKeysetN,
  };

  auto const subsetSize = 1ULL << arrbits.size();
  for (size_t i = 0; i < subsetSize; ++i) {
    auto b = BBottom;
    for (size_t j = 0; j < arrbits.size(); ++j) {
      if (i & (1ULL << j)) b |= arrbits[j];
    }
    bits.emplace_back(b);
    bits.emplace_back(b | BInitNull);
  }

  bits.emplace_back(BObj | BInt);
  bits.emplace_back(BObj | BInt | BInitNull);

  return bits;
});

//////////////////////////////////////////////////////////////////////

}

TEST(Type, Bits) {
  auto index = make_index();

  for (auto const& t : predefined()) {
    EXPECT_EQ(Type{t.first}, t.second);
    EXPECT_TRUE(t.second.is(t.first));
    EXPECT_TRUE(t.second.subtypeOf(t.first));
    EXPECT_FALSE(t.second.strictSubtypeOf(t.first));
    if (t.first != BBottom) {
      EXPECT_TRUE(t.second.couldBe(t.first));
    }
  }

  for (auto const& t : allCases(index)) {
    for (auto const b : allBits()) {
      EXPECT_EQ(t.subtypeOf(b), t.subtypeOf(Type{b}));
      EXPECT_EQ(loosen_mark_for_testing(t).strictSubtypeOf(b),
                loosen_mark_for_testing(t).strictSubtypeOf(Type{b}));
      if (!is_specialized_array_like(t)) {
        EXPECT_EQ(t.couldBe(b), t.couldBe(Type{b}));
      } else if (!t.couldBe(b)) {
        EXPECT_FALSE(t.couldBe(Type{b}));
      }
    }
  }
}

TEST(Type, Top) {
  auto index = make_index();

  // Everything is a subtype of Top, couldBe Top, and the union of Top
  // with anything is Top.
  for (auto const& t : allCases(index)) {
    if (!t.is(BTop)) {
      EXPECT_FALSE(TTop.subtypeOf(t));
    }
    EXPECT_FALSE(TTop.strictSubtypeOf(t));
    EXPECT_TRUE(t.subtypeOf(BTop));
    if (!t.is(BBottom)) {
      EXPECT_TRUE(TTop.couldBe(t));
      EXPECT_TRUE(t.couldBe(BTop));
    }
    EXPECT_EQ(union_of(t, TTop), TTop);
    EXPECT_EQ(union_of(TTop, t), TTop);
    EXPECT_EQ(intersection_of(TTop, t), t);
    EXPECT_EQ(intersection_of(t, TTop), t);
  }
}

TEST(Type, Bottom) {
  auto index = make_index();

  // Bottom is a subtype of everything, nothing couldBe Bottom, and
  // the union_of anything with Bottom is itself.
  for (auto const& t : allCases(index)) {
    EXPECT_TRUE(TBottom.subtypeOf(t));
    EXPECT_FALSE(TBottom.couldBe(t));
    if (!t.is(BBottom)) {
      EXPECT_FALSE(t.subtypeOf(BBottom));
    }
    EXPECT_FALSE(t.strictSubtypeOf(BBottom));
    EXPECT_FALSE(t.couldBe(BBottom));
    EXPECT_EQ(union_of(t, TBottom), t);
    EXPECT_EQ(union_of(TBottom, t), t);
    EXPECT_EQ(intersection_of(TBottom, t), TBottom);
    EXPECT_EQ(intersection_of(t, TBottom), TBottom);
  }
}

TEST(Type, Prims) {
  auto index = make_index();

  // All pairs of non-equivalent primitives are not related by either
  // subtypeOf or couldBe, except for wait-handles, which always could
  // be each other.
  for (auto const& t1 : primitives()) {
    for (auto const& t2 : primitives()) {
      if (t1 == t2) continue;

      auto const test = [] (const Type& a, const Type& b, bool isWaitHandle) {
        EXPECT_TRUE(!a.subtypeOf(b));
        EXPECT_TRUE(!a.strictSubtypeOf(b));
        EXPECT_TRUE(!b.subtypeOf(a));
        EXPECT_TRUE(!b.strictSubtypeOf(a));
        if (!isWaitHandle) {
          EXPECT_TRUE(!a.couldBe(b));
          EXPECT_TRUE(!b.couldBe(a));
          EXPECT_EQ(intersection_of(a, b), TBottom);
        } else {
          EXPECT_TRUE(a.couldBe(b));
          EXPECT_TRUE(b.couldBe(a));
          EXPECT_NE(intersection_of(a, b), TBottom);
        }
      };

      test(t1, t2, false);

      if (t1.is(BUninit) || t2.is(BUninit)) continue;

      test(wait_handle(t1), wait_handle(t2), true);

      const std::vector<Type> arrays1{
        dict_packed({t1, t1}),
        dict_packedn(t1),
        dict_map({map_elem(5, t1)}),
        dict_n(TInt, t1)
      };
      const std::vector<Type> arrays2{
        dict_packed({t2, t2}),
        dict_packedn(t2),
        dict_map({map_elem(5, t2)}),
        dict_n(TInt, t2)
      };
      for (auto const& a1 : arrays1) {
        for (auto const& a2 : arrays2) test(a1, a2, false);
      }
    }
  }
}

namespace {

void test_basic_operators(const std::vector<Type>& types) {
  for (auto const& t : types) {
    EXPECT_EQ(t, t);
    EXPECT_TRUE(t.equal(t));
    EXPECT_TRUE(t.subtypeOf(t));
    EXPECT_TRUE(t.moreRefined(t));
    EXPECT_FALSE(t.strictSubtypeOf(t));
    EXPECT_FALSE(t.strictlyMoreRefined(t));
    if (!t.is(BBottom)) {
      EXPECT_TRUE(t.couldBe(t));
    }
    EXPECT_EQ(union_of(t, t), t);
    EXPECT_EQ(intersection_of(t, t), t);
    if (!t.is(BBottom)) {
      EXPECT_TRUE(opt(t).couldBe(t));
    }
    EXPECT_TRUE(t.subtypeOf(opt(t)));
    EXPECT_EQ(opt(t), union_of(t, TInitNull));
    if (t.subtypeOf(BCell)) {
      EXPECT_EQ(unopt(t), remove_bits(t, BInitNull));
      EXPECT_FALSE(unopt(t).couldBe(BInitNull));
    }
    if (!t.couldBe(BInitNull)) {
      EXPECT_EQ(t, unopt(opt(t)));
    }
  }

  auto const isCtxful = [] (const Type& t1, const Type& t2) {
    if (is_specialized_obj(t1) && is_specialized_obj(t2)) {
      return dobj_of(t1).isCtx() != dobj_of(t2).isCtx();
    }
    if (!is_specialized_cls(t1) || !is_specialized_cls(t2)) return false;
    return dcls_of(t1).isCtx() != dcls_of(t2).isCtx();
  };

  for (size_t i1 = 0; i1 < types.size(); ++i1) {
    for (size_t i2 = 0; i2 < types.size(); ++i2) {
      auto const& t1 = types[i1];
      auto const& t2 = types[i2];

      auto const ctxful = isCtxful(t1, t2);

      auto const equivRefined = t1.equal(t2);
      auto const moreRefined = t1.moreRefined(t2);
      auto const couldBe = t1.couldBe(t2);

      if (t1.strictlyMoreRefined(t2)) {
        EXPECT_FALSE(equivRefined);
        EXPECT_TRUE(moreRefined);
      }
      if (t1.strictSubtypeOf(t2)) {
        EXPECT_NE(t1, t2);
        EXPECT_TRUE(t1.subtypeOf(t2));
      }

      if (equivRefined) {
        EXPECT_TRUE(t1 == t2);
      }
      if (moreRefined) {
        EXPECT_TRUE(t1.subtypeOf(t2));
      }
      if (t1.strictlyMoreRefined(t2)) {
        EXPECT_TRUE(t1.strictSubtypeOf(t2) || t1 == t2);
      }

      if (!ctxful) {
        EXPECT_EQ(t1 == t2, equivRefined);
        EXPECT_EQ(t1.subtypeOf(t2), moreRefined);
        EXPECT_EQ(t1.strictSubtypeOf(t2), t1.strictlyMoreRefined(t2));
      }

      if (i1 == i2) {
        EXPECT_TRUE(equivRefined);
        EXPECT_FALSE(t1.strictlyMoreRefined(t2));
        EXPECT_FALSE(t1.strictSubtypeOf(t2));
      }

      auto const uni = union_of(t1, t2);
      auto const isect = intersection_of(t1, t2);

      EXPECT_EQ(t1 == t2, t2 == t1);
      EXPECT_EQ(equivRefined, t2.equal(t1));
      EXPECT_EQ(couldBe, t2.couldBe(t1));
      EXPECT_EQ(uni, union_of(t2, t1));
      EXPECT_EQ(isect, intersection_of(t2, t1));

      EXPECT_TRUE(t1.moreRefined(uni));
      EXPECT_TRUE(t2.moreRefined(uni));
      EXPECT_TRUE(isect.moreRefined(t1));
      EXPECT_TRUE(isect.moreRefined(t2));
      EXPECT_TRUE(intersection_of(uni, t1).equal(t1));
      EXPECT_TRUE(intersection_of(uni, t2).equal(t2));

      if (moreRefined) {
        EXPECT_TRUE(uni.equal(t2));
        EXPECT_TRUE(isect.equal(t1));
      }

      if (couldBe) {
        if (!is_specialized_array_like(t1) && !is_specialized_array_like(t2)) {
          EXPECT_FALSE(isect.is(BBottom));
        }
      } else {
        EXPECT_TRUE(isect.is(BBottom));
      }

      if (!t1.is(BBottom)) {
        if (moreRefined) {
          EXPECT_TRUE(couldBe);
        }
        if (!couldBe) {
          EXPECT_FALSE(moreRefined);
        }
        EXPECT_TRUE(t1.couldBe(uni));
      }

      if (!isect.is(BBottom)) {
        EXPECT_TRUE(isect.couldBe(t1));
      }
    }
  }

  for (auto const& t : types) {
    if (t.couldBe(BVec)) {
      EXPECT_FALSE(intersection_of(t, TVec).is(BBottom));
    }
    if (t.couldBe(BDict)) {
      EXPECT_FALSE(intersection_of(t, TDict).is(BBottom));
    }
    if (t.couldBe(BKeyset)) {
      EXPECT_FALSE(intersection_of(t, TKeyset).is(BBottom));
    }
  }
}

}

TEST(Type, BasicOperators) {
  auto index = make_index();
  test_basic_operators(withData(index));

  EXPECT_EQ(union_of(ival(0),TStr), TArrKey);
  EXPECT_EQ(union_of(TInt,sval(s_A)), TUncArrKey);
}

TEST(Type, SpecializedClasses) {
  auto index = make_index();
  auto const types = specializedClasses(index);

  test_basic_operators(types);

  for (auto const& t1 : types) {
    for (auto const& t2 : types) {
      if (!t2.subtypeOf(t1)) continue;
      for (auto const& t3 : types) {
        auto const superU = union_of(t1, t3);
        auto const superI = intersection_of(t1, t3);
        for (auto const& t4 : types) {
          if (!t4.subtypeOf(t3)) continue;
          EXPECT_TRUE(union_of(t2, t4).subtypeOf(superU));
          auto const isect = intersection_of(t2, t4);
          EXPECT_TRUE(isect.subtypeOf(superI));
        }
      }
    }
  }
}

TEST(Type, Closure) {
  auto index = make_index();

  auto const closureCls = index.resolve_class(s_Closure.get());
  if (!closureCls || closureCls->hasCompleteChildren()) ADD_FAILURE();

  auto const childCls1 = index.resolve_class(s_ChildClosure1.get());
  if (!childCls1 || !childCls1->isComplete()) ADD_FAILURE();
  auto const childCls2 = index.resolve_class(s_ChildClosure2.get());
  if (!childCls2 || !childCls2->isComplete()) ADD_FAILURE();
  auto const childCls3 = index.resolve_class(s_ChildClosure3.get());
  if (!childCls3 || !childCls3->isComplete()) ADD_FAILURE();

  auto const closure = subObj(*closureCls);
  auto const child1 = objExact(*childCls1);
  auto const child2 = objExact(*childCls2);
  auto const child3 = objExact(*childCls3);

  EXPECT_TRUE(child1.subtypeOf(closure));
  EXPECT_TRUE(child2.subtypeOf(closure));
  EXPECT_TRUE(child3.subtypeOf(closure));

  EXPECT_FALSE(closure.subtypeOf(child1));
  EXPECT_FALSE(closure.subtypeOf(child2));
  EXPECT_FALSE(closure.subtypeOf(child3));

  EXPECT_FALSE(child1.subtypeOf(child2));
  EXPECT_FALSE(child1.subtypeOf(child3));
  EXPECT_FALSE(child2.subtypeOf(child3));

  EXPECT_TRUE(child1.couldBe(closure));
  EXPECT_TRUE(child2.couldBe(closure));
  EXPECT_TRUE(child3.couldBe(closure));

  EXPECT_TRUE(closure.couldBe(child1));
  EXPECT_TRUE(closure.couldBe(child2));
  EXPECT_TRUE(closure.couldBe(child3));

  EXPECT_FALSE(child1.couldBe(child2));
  EXPECT_FALSE(child1.couldBe(child3));
  EXPECT_FALSE(child2.couldBe(child3));

  EXPECT_EQ(union_of(child1, closure), closure);
  EXPECT_EQ(union_of(child2, closure), closure);
  EXPECT_EQ(union_of(child3, closure), closure);

  EXPECT_EQ(union_of(child1, child2), closure);
  EXPECT_EQ(union_of(child1, child3), closure);
  EXPECT_EQ(union_of(child2, child3), closure);

  EXPECT_EQ(intersection_of(child1, closure), child1);
  EXPECT_EQ(intersection_of(child2, closure), child2);
  EXPECT_EQ(intersection_of(child3, closure), child3);
}

TEST(Type, SpecializedArrays) {
  test_basic_operators(specialized_arrays());

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BVecN|BKeysetN, TArrKey, TStr),
      make_specialized_arrmapn(BDictN|BKeysetN, TInt, TArrKey)
    ),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BArrLikeN, TArrKey, TStr),
      make_specialized_arrmapn(BDictN|BKeysetN, TInt, TArrKey)
    ),
    make_specialized_arrmapn(BDictN, TInt, TStr)
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BVecN|BDictN, TArrKey, TInt),
      make_specialized_arrmapn(BVecN|BKeysetN, TArrKey, TInt)
    ),
    make_specialized_arrpackedn(BVecN, TInt)
  );

  {
    auto const map1 = make_specialized_arrmap(BDictN, {map_elem(s_A, ival(123))});
    auto const map2 = make_specialized_arrmap(BDictN, {map_elem_nonstatic(s_A, ival(123))});
    EXPECT_NE(map1, map2);
    EXPECT_TRUE(map1.couldBe(map2));
    EXPECT_TRUE(map2.couldBe(map1));
    EXPECT_TRUE(map1.subtypeOf(map2));
    EXPECT_FALSE(map2.subtypeOf(map1));
    EXPECT_EQ(union_of(map1, map2), map2);
    EXPECT_EQ(intersection_of(map1, map2), map1);
  }

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpacked(BVecN, {TStr, TArrKey, Type{BInt|BObj}, TInitCell}),
      TSArrLikeN
    ),
    make_specialized_arrpacked(BSVecN, {TSStr, TUncArrKey, TInt, TInitUnc})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpacked(BVecN, {TStr, TArrKey, TObj, TInitCell}),
      TSArrLikeN
    ),
    TBottom
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpacked(BVecN|BKeysetN, {TArrKey, TArrKey}),
      TKeysetN
    ),
    make_specialized_arrpacked(BKeysetN, {ival(0), ival(1)})
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpacked(BDictN|BVecN, {TObj}),
      Type{BSDictN|BVecN}
    ),
    make_specialized_arrpacked(BVecN, {TObj})
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpacked(BSDictN|BVecN, {TInitCell}),
      make_specialized_arrpacked(BCDictN|BSVecN, {TInitCell})
    ),
    make_specialized_arrpacked(BSVecN, {TInitUnc})
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BVecN, TStr),
      TSArrLikeN
    ),
    make_specialized_arrpackedn(BSVecN, TSStr)
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BVecN, TObj),
      TSArrLikeN
    ),
    TBottom
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BVecN|BKeysetN, TArrKey),
      TKeysetN
    ),
    make_specialized_arrpackedn(BKeysetN, TInt)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BDictN|BVecN, TObj),
      Type{BSDictN|BVecN}
    ),
    make_specialized_arrpackedn(BVecN, TObj)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BDictN|BSVecN, Type{BInitCell & ~BObj}),
      make_specialized_arrpackedn(BSDictN|BCVecN, Type{BInitCell & ~BObj})
    ),
    make_specialized_arrpackedn(BSDictN, TInitUnc)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BVecN, TArrKey, TObj),
      TVecN
    ),
    make_specialized_arrpackedn(BVecN, TObj)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN, TArrKey, TStr),
      TSDictN
    ),
    make_specialized_arrmapn(BSDictN, TUncArrKey, TSStr)
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN, TArrKey, TObj),
      TSDictN
    ),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN, TCStr, TStr),
      TSDictN
    ),
    TBottom
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BKeysetN, TStr, TArrKey),
      make_specialized_arrmapn(BVecN|BKeysetN, TArrKey, TArrKey)
    ),
    make_specialized_arrmapn(BKeysetN, TStr, TStr)
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BKeysetN, TStr, TArrKey),
      make_specialized_arrmapn(BVecN|BKeysetN, TArrKey, TInt)
    ),
    TBottom
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BKeysetN, TStr, TArrKey),
      make_specialized_arrmapn(BDictN|BKeysetN, TArrKey, TInt)
    ),
    make_specialized_arrmapn(BDictN, TStr, TInt)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BCKeysetN, TCStr, TCStr),
      Type{BDictN|BSKeysetN}
    ),
    make_specialized_arrmapn(BDictN, TCStr, TCStr)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BDictN|BKeysetN, TArrKey, Type{BInitCell & ~BObj}),
      make_specialized_arrmapn(BSDictN|BVecN, TUncArrKey, Type{BInitCell & ~BObj})
    ),
    TSDictN
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmapn(BCDictN|BSVecN, TArrKey, Type{BInitCell & ~BObj}),
      make_specialized_arrmapn(BSDictN|BVecN, TUncArrKey, Type{BInitCell & ~BObj})
    ),
    TSVecN
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN, {map_elem(s_A, TStr)}),
      TSDictN
    ),
    make_specialized_arrmap(BSDictN, {map_elem(s_A, TSStr)})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN, {map_elem_nonstatic(s_A, TStr)}),
      TSDictN
    ),
    make_specialized_arrmap(BSDictN, {map_elem(s_A, TSStr)})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN, {map_elem_counted(s_A, TStr)}),
      TSDictN
    ),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN, {map_elem(123, TStr)}),
      TSDictN
    ),
    make_specialized_arrmap(BSDictN, {map_elem(123, TSStr)})
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BCDictN|BKeysetN, {map_elem(s_A, TArrKey)}),
      make_specialized_arrmap(BSDictN|BKeysetN, {map_elem(s_A, TArrKey)})
    ),
    make_specialized_arrmap(BKeysetN, {map_elem(s_A, sval(s_A))})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BCDictN|BKeysetN, {map_elem_nonstatic(s_A, sval(s_A))}),
      make_specialized_arrmap(BSDictN|BKeysetN, {map_elem_nonstatic(s_A, sval(s_A))})
    ),
    make_specialized_arrmap(BKeysetN, {map_elem(s_A, sval(s_A))})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BSDictN|BKeysetN, {map_elem(s_A, TArrKey)}),
      make_specialized_arrmap(BCDictN|BKeysetN, {map_elem_nonstatic(s_A, TCStr)})
    ),
    TBottom
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN|BKeysetN, {map_elem_counted(s_A, TCStr)}),
      Type{BDictN|BSKeysetN}
    ),
    make_specialized_arrmap(BDictN, {map_elem_counted(s_A, TCStr)})
  );
  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN|BKeysetN, {map_elem_nonstatic(s_A, TCStr)}),
      make_specialized_arrmap(BDictN|BSKeysetN, {map_elem(s_A, TArrKey)})
    ),
    make_specialized_arrmap(BDictN, {map_elem(s_A, TCStr)})
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrmap(BDictN, {map_elem(s_A, TInitCell)}, TArrKey, TInitCell),
      make_specialized_arrmap(BSDictN|BKeysetN, {map_elem(s_A, TArrKey)}, TArrKey, TArrKey)
    ),
    make_specialized_arrmap(BSDictN, {map_elem(s_A, TUncArrKey)}, TUncArrKey, TUncArrKey)
  );

  EXPECT_EQ(
    intersection_of(
      make_specialized_arrpackedn(BVecN|BKeysetN, ival(0)),
      TKeyset
    ),
    make_specialized_arrpacked(BKeysetN, {ival(0)})
  );

  {
    auto const vec1 = make_specialized_arrval(BSVecN, static_vec(s_A.get()));
    auto const dict1 = make_specialized_arrval(BSDictN, static_dict(s_B.get(), 1));
    EXPECT_EQ(union_of(vec1, TSKeysetN),
              make_unmarked(make_specialized_arrmapn(BSKeysetN|BSVecN, TUncArrKey, TUncArrKey)));
    EXPECT_EQ(union_of(vec1, TKeysetN),
              make_unmarked(make_specialized_arrmapn(BKeysetN|BSVecN, TArrKey, TArrKey)));
    EXPECT_EQ(union_of(union_of(vec1, dict1), TKeysetN),
              union_of(union_of(vec1, TKeysetN), dict1));
  }
}

TEST(Type, Split) {
  auto index = make_index();

  auto const test = [] (auto f, trep bits, const Type& split,
                        const Type& rest, const Type& orig) {
    EXPECT_TRUE(split.moreRefined(orig));
    EXPECT_TRUE(rest.moreRefined(orig));
    EXPECT_FALSE(split.couldBe(rest));
    EXPECT_TRUE(union_of(split, rest).equal(orig));

    if (orig.couldBe(bits)) {
      EXPECT_TRUE(split.subtypeOf(bits));
    } else {
      EXPECT_EQ(split, TBottom);
    }
    EXPECT_FALSE(rest.couldBe(bits));
    if (orig.subtypeOf(bits)) {
      EXPECT_EQ(rest, TBottom);
    }

    if (f(orig)) {
      EXPECT_TRUE(f(split));
      EXPECT_FALSE(rest.hasData());
    } else if (orig.hasData()) {
      EXPECT_FALSE(split.hasData());
      EXPECT_TRUE(rest.hasData());
    } else {
      EXPECT_FALSE(split.hasData());
      EXPECT_FALSE(rest.hasData());
    }
  };

  auto const& types = allCases(index);
  for (auto const& t : types) {
    if (!t.subtypeOf(BCell)) continue;

    auto const [obj, objRest] = split_obj(t);
    test(is_specialized_obj, BObj, obj, objRest, t);

    auto const [cls, clsRest] = split_cls(t);
    test(is_specialized_cls, BCls, cls, clsRest, t);

    auto const [arr, arrRest] = split_array_like(t);
    test(is_specialized_array_like, BArrLike, arr, arrRest, t);

    auto const [str, strRest] = split_string(t);
    test(is_specialized_string, BStr, str, strRest, t);

    auto const [lcls, lclsRest] = split_lazycls(t);
    test(is_specialized_lazycls, BLazyCls, lcls, lclsRest, t);
  }

  auto split = split_array_like(Type{BDictN|BInt});
  EXPECT_EQ(split.first, TDictN);
  EXPECT_EQ(split.second, TInt);

  split = split_array_like(TVecE);
  EXPECT_EQ(split.first, TVecE);
  EXPECT_EQ(split.second, TBottom);

  split = split_array_like(TInt);
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, TInt);

  split = split_array_like(ival(123));
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, ival(123));

  split = split_array_like(union_of(TKeyset,ival(123)));
  EXPECT_EQ(split.first, TKeyset);
  EXPECT_EQ(split.second, TInt);

  split = split_array_like(make_specialized_arrmapn(BDictN, TStr, TObj));
  EXPECT_EQ(split.first, make_specialized_arrmapn(BDictN, TStr, TObj));
  EXPECT_EQ(split.second, TBottom);

  split = split_array_like(make_specialized_arrmapn(BDictN|BFalse, TStr, TObj));
  EXPECT_EQ(split.first, make_specialized_arrmapn(BDictN, TStr, TObj));
  EXPECT_EQ(split.second, TFalse);

  auto const clsA = index.resolve_class(s_A.get());
  if (!clsA || !clsA->isComplete()) ADD_FAILURE();

  split = split_obj(Type{BObj|BInt});
  EXPECT_EQ(split.first, TObj);
  EXPECT_EQ(split.second, TInt);

  split = split_obj(TObj);
  EXPECT_EQ(split.first, TObj);
  EXPECT_EQ(split.second, TBottom);

  split = split_obj(TInt);
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, TInt);

  split = split_obj(ival(123));
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, ival(123));

  split = split_obj(union_of(TObj,ival(123)));
  EXPECT_EQ(split.first, TObj);
  EXPECT_EQ(split.second, TInt);

  split = split_obj(make_specialized_sub_object(BObj, *clsA));
  EXPECT_EQ(split.first, make_specialized_sub_object(BObj, *clsA));
  EXPECT_EQ(split.second, TBottom);

  split = split_obj(make_specialized_sub_object(BObj|BFalse, *clsA));
  EXPECT_EQ(split.first, make_specialized_sub_object(BObj, *clsA));
  EXPECT_EQ(split.second, TFalse);

  split = split_cls(Type{BCls|BInt});
  EXPECT_EQ(split.first, TCls);
  EXPECT_EQ(split.second, TInt);

  split = split_cls(TCls);
  EXPECT_EQ(split.first, TCls);
  EXPECT_EQ(split.second, TBottom);

  split = split_cls(TInt);
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, TInt);

  split = split_cls(ival(123));
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, ival(123));

  split = split_cls(union_of(TCls,ival(123)));
  EXPECT_EQ(split.first, TCls);
  EXPECT_EQ(split.second, TInt);

  split = split_cls(make_specialized_sub_class(BCls, *clsA));
  EXPECT_EQ(split.first, make_specialized_sub_class(BCls, *clsA));
  EXPECT_EQ(split.second, TBottom);

  split = split_cls(make_specialized_sub_class(BCls|BFalse, *clsA));
  EXPECT_EQ(split.first, make_specialized_sub_class(BCls, *clsA));
  EXPECT_EQ(split.second, TFalse);

  split = split_string(TStr);
  EXPECT_EQ(split.first, TStr);
  EXPECT_EQ(split.second, TBottom);

  split = split_string(TInt);
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, TInt);

  split = split_string(sval(s_A));
  EXPECT_EQ(split.first, sval(s_A));
  EXPECT_EQ(split.second, TBottom);

  split = split_string(ival(123));
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, ival(123));

  split = split_string(Type{BStr|BInt});
  EXPECT_EQ(split.first, TStr);
  EXPECT_EQ(split.second, TInt);

  split = split_string(union_of(sval(s_A), TFalse));
  EXPECT_EQ(split.first, sval(s_A));
  EXPECT_EQ(split.second, TFalse);

  split = split_string(union_of(TStr, ival(123)));
  EXPECT_EQ(split.first, TStr);
  EXPECT_EQ(split.second, TInt);

  split = split_lazycls(TInt);
  EXPECT_EQ(split.first, TBottom);
  EXPECT_EQ(split.second, TInt);

  split = split_lazycls(Type{BStr|BLazyCls});
  EXPECT_EQ(split.first, TLazyCls);
  EXPECT_EQ(split.second, TStr);

  split = split_lazycls(union_of(lazyclsval(s_A), TFalse));
  EXPECT_EQ(split.first, lazyclsval(s_A));
  EXPECT_EQ(split.second, TFalse);
}

TEST(Type, Remove) {
  auto index = make_index();

  auto const test = [] (auto f, trep bits, const Type& removed,
                        const Type& orig) {
    EXPECT_TRUE(removed.moreRefined(orig));
    if (orig.couldBe(bits)) {
      EXPECT_TRUE(removed.strictlyMoreRefined(orig));
    }
    EXPECT_FALSE(removed.couldBe(bits));

    if (f(orig) || !orig.hasData()) {
      EXPECT_FALSE(removed.hasData());
    } else {
      EXPECT_TRUE(removed.hasData());
    }
  };

  auto const& types = allCases(index);
  for (auto const& t : types) {
    if (!t.subtypeOf(BCell)) continue;
    test(is_specialized_int, BInt, remove_int(t), t);
    test(is_specialized_double, BDbl, remove_double(t), t);
    test(is_specialized_string, BStr, remove_string(t), t);
    test(is_specialized_lazycls, BLazyCls, remove_lazycls(t), t);
    test(is_specialized_cls, BCls, remove_cls(t), t);
    test(is_specialized_obj, BObj, remove_obj(t), t);

    EXPECT_EQ(remove_int(t), remove_bits(t, BInt));
    EXPECT_EQ(remove_double(t), remove_bits(t, BDbl));
    EXPECT_EQ(remove_string(t), remove_bits(t, BStr));
    EXPECT_EQ(remove_lazycls(t), remove_bits(t, BLazyCls));
    EXPECT_EQ(remove_cls(t), remove_bits(t, BCls));
    EXPECT_EQ(remove_obj(t), remove_bits(t, BObj));

    EXPECT_FALSE(is_specialized_array_like(remove_bits(t, BArrLikeN)));
    if (t.couldBe(BDictN)) {
      EXPECT_FALSE(is_specialized_array_like(remove_bits(t, BDictN)));
    }

    EXPECT_FALSE(remove_keyset(t).couldBe(BKeyset));
    if (!t.couldBe(BKeyset)) {
      EXPECT_EQ(remove_keyset(t), t);
    }
    if (t.subtypeAmong(BKeyset, BArrLike)) {
      EXPECT_FALSE(is_specialized_array_like(remove_keyset(t)));
    }
  }

  EXPECT_EQ(remove_int(TStr), TStr);
  EXPECT_EQ(remove_int(TInt), TBottom);
  EXPECT_EQ(remove_int(Type{BStr|BInt}), TStr);
  EXPECT_EQ(remove_int(ival(123)), TBottom);
  EXPECT_EQ(remove_int(dval(1.23)), dval(1.23));
  EXPECT_EQ(remove_int(union_of(ival(123),TDbl)), TDbl);
  EXPECT_EQ(remove_int(union_of(TInt,dval(1.23))), TDbl);

  EXPECT_EQ(remove_double(TStr), TStr);
  EXPECT_EQ(remove_double(TDbl), TBottom);
  EXPECT_EQ(remove_double(Type{BStr|BDbl}), TStr);
  EXPECT_EQ(remove_double(dval(1.23)), TBottom);
  EXPECT_EQ(remove_double(ival(123)), ival(123));
  EXPECT_EQ(remove_double(union_of(ival(123),TDbl)), TInt);
  EXPECT_EQ(remove_double(union_of(TInt,dval(1.23))), TInt);

  EXPECT_EQ(remove_string(TInt), TInt);
  EXPECT_EQ(remove_string(TStr), TBottom);
  EXPECT_EQ(remove_string(Type{BStr|BInt}), TInt);
  EXPECT_EQ(remove_string(ival(123)), ival(123));
  EXPECT_EQ(remove_string(sval(s_A)), TBottom);
  EXPECT_EQ(remove_string(union_of(ival(123),TStr)), TInt);
  EXPECT_EQ(remove_string(union_of(TInt,sval(s_A))), TInt);

  EXPECT_EQ(remove_lazycls(TStr), TStr);
  EXPECT_EQ(remove_lazycls(TLazyCls), TBottom);
  EXPECT_EQ(remove_lazycls(Type{BLazyCls|BStr}), TStr);
  EXPECT_EQ(remove_lazycls(lazyclsval(s_A)), TBottom);
  EXPECT_EQ(remove_lazycls(union_of(TInt,lazyclsval(s_A))), TInt);

  auto const clsA = index.resolve_class(s_A.get());
  if (!clsA || !clsA->isComplete()) ADD_FAILURE();

  EXPECT_EQ(remove_cls(TInt), TInt);
  EXPECT_EQ(remove_cls(TCls), TBottom);
  EXPECT_EQ(remove_cls(Type{BCls|BInt}), TInt);
  EXPECT_EQ(remove_cls(ival(123)), ival(123));
  EXPECT_EQ(remove_cls(make_specialized_sub_class(BCls, *clsA)), TBottom);
  EXPECT_EQ(remove_cls(union_of(ival(123),TCls)), TInt);
  EXPECT_EQ(remove_cls(make_specialized_sub_class(BCls|BFalse, *clsA)), TFalse);

  EXPECT_EQ(remove_obj(TInt), TInt);
  EXPECT_EQ(remove_obj(TObj), TBottom);
  EXPECT_EQ(remove_obj(Type{BInt|BObj}), TInt);
  EXPECT_EQ(remove_obj(ival(123)), ival(123));
  EXPECT_EQ(remove_obj(make_specialized_sub_object(BObj, *clsA)), TBottom);
  EXPECT_EQ(remove_obj(union_of(ival(123),TObj)), TInt);
  EXPECT_EQ(remove_obj(make_specialized_sub_object(BObj|BFalse, *clsA)), TFalse);
}

TEST(Type, Prim) {
  const std::initializer_list<std::pair<Type, Type>> subtype_true{
    { TInt,      TPrim },
    { TBool,     TPrim },
    { TNum,      TPrim },
    { TInitNull, TPrim },
    { TDbl,      TPrim },
    { dval(0.0), TPrim },
    { ival(0),   TPrim },
    { TNull,     TPrim },
    { TInt,      TInitPrim },
    { TBool,     TInitPrim },
    { TNum,      TInitPrim },
    { TInitNull, TInitPrim },
    { TDbl,      TInitPrim },
    { dval(0.0), TInitPrim },
    { ival(0),   TInitPrim },
  };

  const std::initializer_list<std::pair<Type, Type>> subtype_false{
    { sval(s_test), TPrim },
    { TSStr, TPrim },
    { TNull, TInitPrim }, // TNull could be uninit
    { TPrim, TBool },
    { TPrim, TInt },
    { TPrim, TNum },
    { TInitPrim, TNum },
    { TUnc, TPrim },
    { TUnc, TInitPrim },
    { TInitUnc, TPrim },
    { TSStr, TInitPrim },
    { TRes, TPrim },
    { TObj, TPrim },
    { TRFunc, TPrim },
    { TPrim, dval(0.0) },
    { TCls, TInitPrim },
    { TFunc, TInitPrim },
    { TLazyCls, TInitPrim },
    { TEnumClassLabel, TInitPrim },
  };

  const std::initializer_list<std::pair<Type, Type>> couldbe_true{
    { TPrim, TInt },
    { TPrim, TBool },
    { TPrim, TNum },
    { TInitPrim, TNum },
    { TInitPrim, TFalse },
    { TPrim, TCell },
    { TPrim, TOptObj },
    { TPrim, TOptFalse },
  };

  const std::initializer_list<std::pair<Type, Type>> couldbe_false{
    { TPrim, TSStr },
    { TInitPrim, TSStr },
    { TInitPrim, sval(s_test) },
    { TInitPrim, lazyclsval(s_test) },
    { TPrim, sval(s_test) },
    { TInitPrim, TUninit },
    { TPrim, TObj },
    { TPrim, TRes },
    { TPrim, TRFunc },
    { TPrim, TFunc },
  };

  for (auto kv : subtype_true) {
    EXPECT_TRUE(kv.first.subtypeOf(kv.second))
      << show(kv.first) << " subtypeOf " << show(kv.second);
  }

  for (auto kv : subtype_false) {
    EXPECT_FALSE(kv.first.subtypeOf(kv.second))
      << show(kv.first) << " !subtypeOf " << show(kv.second);
  }

  for (auto kv : couldbe_true) {
    EXPECT_TRUE(kv.first.couldBe(kv.second))
      << show(kv.first) << " couldbe " << show(kv.second);
    EXPECT_TRUE(kv.second.couldBe(kv.first))
      << show(kv.first) << " couldbe " << show(kv.second);
  }

  for (auto kv : couldbe_false) {
    EXPECT_FALSE(kv.first.couldBe(kv.second))
      << show(kv.first) << " !couldbe " << show(kv.second);
    EXPECT_FALSE(kv.second.couldBe(kv.first))
      << show(kv.first) << " !couldbe " << show(kv.second);
  }

  EXPECT_FALSE(TClsMeth.subtypeOf(TInitPrim));
  EXPECT_FALSE(TPrim.couldBe(TClsMeth));
}

TEST(Type, CouldBeValues) {
  EXPECT_FALSE(ival(2).couldBe(ival(3)));
  EXPECT_TRUE(ival(2).couldBe(ival(2)));

  auto const packed_dict = static_dict(0, 42, 1, 23, 2, 12);
  auto const dict = static_dict(s_A.get(), s_B.get(), s_test.get(), 12);

  EXPECT_FALSE(dict_val(packed_dict).couldBe(dict_val(dict)));
  EXPECT_TRUE(dict_val(packed_dict).couldBe(dict_val(packed_dict)));
  EXPECT_TRUE(dval(2.0).couldBe(dval(2.0)));
  EXPECT_FALSE(dval(2.0).couldBe(dval(3.0)));

  EXPECT_FALSE(sval(s_test).couldBe(sval(s_A)));
  EXPECT_TRUE(sval(s_test).couldBe(sval(s_test)));
  EXPECT_FALSE(
    sval_nonstatic(s_test).couldBe(sval_nonstatic(s_A))
  );
  EXPECT_TRUE(
    sval_nonstatic(s_test).couldBe(sval_nonstatic(s_test))
  );
  EXPECT_TRUE(sval(s_test).couldBe(sval_nonstatic(s_test)));
  EXPECT_TRUE(sval_nonstatic(s_test).couldBe(sval(s_test)));
  EXPECT_FALSE(sval(s_test.get()).couldBe(sval_nonstatic(s_A)));
  EXPECT_FALSE(sval_nonstatic(s_test).couldBe(sval(s_A)));

  EXPECT_FALSE(lazyclsval(s_test).couldBe(lazyclsval(s_A)));
  EXPECT_TRUE(lazyclsval(s_test).couldBe(lazyclsval(s_test)));
}

TEST(Type, Unc) {
  EXPECT_TRUE(TInt.subtypeOf(BInitUnc));
  EXPECT_TRUE(TInt.subtypeOf(BUnc));
  EXPECT_TRUE(TDbl.subtypeOf(BInitUnc));
  EXPECT_TRUE(TDbl.subtypeOf(BUnc));
  EXPECT_TRUE(dval(3.0).subtypeOf(BInitUnc));
  EXPECT_TRUE(TClsMeth.subtypeOf(BInitUnc));

  const std::initializer_list<std::tuple<Type, Type, bool>> tests{
    { TUnc, TInitUnc, true },
    { TUnc, TInitCell, true },
    { TUnc, TCell, true },
    { TInitUnc, TInt, true },
    { TInitUnc, TOptInt, true },
    { TInitUnc, opt(ival(2)), true },
    { TUnc, TInt, true },
    { TUnc, TOptInt, true },
    { TUnc, opt(ival(2)), true },
    { TNum, TUnc, true },
    { TNum, TInitUnc, true },
    { TUncArrKey, TInitUnc, true },
    { TClsMeth, TInitUnc, true },
  };
  for (auto const& t : tests) {
    auto const& ty1 = std::get<0>(t);
    auto const& ty2 = std::get<1>(t);
    if (std::get<2>(t)) {
      EXPECT_TRUE(ty1.couldBe(ty2))
        << show(ty1) << " couldBe " << show(ty2);
    } else {
      EXPECT_FALSE(ty1.couldBe(ty2))
        << show(ty1) << " !couldBe " << show(ty2);
    }
  }
}

TEST(Type, DblNan) {
  auto const qnan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(dval(qnan).subtypeOf(dval(qnan)));
  EXPECT_TRUE(dval(qnan).couldBe(dval(qnan)));
  EXPECT_FALSE(dval(qnan).strictSubtypeOf(dval(qnan)));
  EXPECT_EQ(dval(qnan), dval(qnan));
  EXPECT_EQ(union_of(dval(qnan), dval(qnan)), dval(qnan));
  EXPECT_EQ(intersection_of(dval(qnan), dval(qnan)), dval(qnan));
}

TEST(Type, ObjToCls) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (t.is(BBottom) || !t.subtypeOf(BCls)) continue;
    auto const obj = toobj(t);
    EXPECT_TRUE(obj.subtypeOf(BObj));
    if (!is_specialized_cls(t)) {
      EXPECT_EQ(obj, TObj);
    } else {
      EXPECT_TRUE(obj.is(BBottom) || is_specialized_obj(obj));
    }
  }

  for (auto const& t : all) {
    if (t.is(BBottom) || !t.subtypeOf(BObj)) continue;
    auto const cls = objcls(t);
    EXPECT_TRUE(cls.subtypeOf(BCls));
    if (!is_specialized_obj(t)) {
      EXPECT_EQ(cls, TCls);
    } else {
      EXPECT_TRUE(is_specialized_cls(cls));
      if (is_specialized_cls(cls)) {
        auto const& dcls = dcls_of(cls);
        EXPECT_FALSE(dcls.containsNonRegular());
      }
    }
  }

  for (auto const& t : all) {
    if (t.is(BBottom) || !t.subtypeOf(BCls)) continue;
    auto const obj = toobj(t);
    if (obj.is(BBottom)) continue;
    auto const cls = objcls(obj);
    EXPECT_TRUE(cls.subtypeOf(t));
    if (!is_specialized_cls(cls)) continue;
    auto const& dcls = dcls_of(cls);
    EXPECT_FALSE(dcls.containsNonRegular());
    auto const obj2 = toobj(cls);
    EXPECT_EQ(obj, obj2);
  }

  auto const clsA = index.resolve_class(s_A.get());
  if (!clsA || !clsA->isComplete()) ADD_FAILURE();
  auto const clsICanon1 = index.resolve_class(s_ICanon1.get());
  if (!clsICanon1 || !clsICanon1->isComplete()) ADD_FAILURE();
  auto const clsICanon5 = index.resolve_class(s_ICanon5.get());
  if (!clsICanon5 || !clsICanon5->isComplete()) ADD_FAILURE();
  auto const clsCanon3 = index.resolve_class(s_Canon3.get());
  if (!clsCanon3 || !clsCanon3->isComplete()) ADD_FAILURE();
  auto const clsT1 = index.resolve_class(s_T1.get());
  if (!clsT1 || !clsT1->isComplete()) ADD_FAILURE();
  auto const clsT4_C1 = index.resolve_class(s_T4_C1.get());
  if (!clsT4_C1 || !clsT4_C1->isComplete()) ADD_FAILURE();
  auto const clsICanon8 = index.resolve_class(s_ICanon8.get());
  if (!clsICanon8 || !clsICanon8->isComplete()) ADD_FAILURE();
  auto const clsAbs4 = index.resolve_class(s_Abs4.get());
  if (!clsAbs4 || !clsAbs4->isComplete()) ADD_FAILURE();
  auto const clsAbs5_P = index.resolve_class(s_Abs5_P.get());
  if (!clsAbs5_P || !clsAbs5_P->isComplete()) ADD_FAILURE();
  auto const clsAbs6_P = index.resolve_class(s_Abs6_P.get());
  if (!clsAbs6_P || !clsAbs6_P->isComplete()) ADD_FAILURE();
  auto const clsICanon11 = index.resolve_class(s_ICanon11.get());
  if (!clsICanon11 || !clsICanon11->isComplete()) ADD_FAILURE();
  auto const clsICanon12 = index.resolve_class(s_ICanon12.get());
  if (!clsICanon12 || !clsICanon12->isComplete()) ADD_FAILURE();
  auto const clsICanon13 = index.resolve_class(s_ICanon13.get());
  if (!clsICanon13 || !clsICanon13->isComplete()) ADD_FAILURE();
  auto const clsCanon10 = index.resolve_class(s_Canon10.get());
  if (!clsCanon10 || !clsCanon10->isComplete()) ADD_FAILURE();

  auto const clsFoo1 = res::Class::getUnresolved(s_Foo1.get());
  if (clsFoo1.isComplete()) ADD_FAILURE();
  auto const clsFoo2 = res::Class::getUnresolved(s_Foo2.get());
  if (clsFoo2.isComplete()) ADD_FAILURE();

  auto const awaitable = res::Class::get(s_Awaitable.get());

  EXPECT_EQ(toobj(TCls), TObj);
  EXPECT_EQ(toobj(make_specialized_sub_class(BCls, *clsA)),
            make_specialized_sub_object(BObj, *clsA));
  EXPECT_EQ(toobj(make_specialized_exact_class(BCls, *clsA)),
            make_specialized_exact_object(BObj, *clsA));

  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsICanon1, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsICanon1, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsICanon5, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsICanon5, false, false)),
    make_specialized_exact_object(BObj, *clsCanon3, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsT1, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsICanon8, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsICanon8, false, false)),
    make_specialized_exact_object(BObj, *clsT4_C1, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsAbs4, false, false)),
    TBottom
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsAbs4, false, false)),
    make_specialized_sub_object(BObj, *clsAbs4, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsAbs5_P, false, false, false)),
    make_specialized_exact_object(BObj, *clsAbs5_P, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsAbs5_P, false, false)),
    make_specialized_exact_object(BObj, *clsAbs5_P, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_exact_class(BCls, *clsAbs6_P, false, false, false)),
    make_specialized_exact_object(BObj, *clsAbs6_P, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, *clsAbs6_P, false, false)),
    make_specialized_sub_object(BObj, *clsAbs6_P, false, false)
  );
  EXPECT_EQ(
    toobj(make_specialized_sub_class(BCls, clsFoo1, false, false)),
    make_specialized_sub_object(BObj, clsFoo1, false, false)
  );

  EXPECT_EQ(
    toobj(
      intersection_of(
        make_specialized_sub_class(BCls, clsFoo1, false, false),
        make_specialized_sub_class(BCls, clsFoo2, false, false)
      )
    ),
    intersection_of(
      make_specialized_sub_object(BObj, clsFoo1, false, false),
      make_specialized_sub_object(BObj, clsFoo2, false, false)
    )
  );

  EXPECT_EQ(
    toobj(
      intersection_of(
        make_specialized_sub_class(BCls, *clsICanon11, false, false),
        make_specialized_sub_class(BCls, *clsICanon12, false, false)
      )
    ),
    TBottom
  );

  EXPECT_EQ(
    toobj(
      intersection_of(
        make_specialized_sub_class(BCls, *clsICanon12, false, false),
        make_specialized_sub_class(BCls, *clsICanon13, false, false)
      )
    ),
    make_specialized_exact_object(BObj, *clsCanon10, false, false)
  );

  EXPECT_EQ(objcls(TObj), TCls);
  EXPECT_EQ(objcls(make_specialized_sub_object(BObj, *clsA)),
            make_specialized_sub_class(BCls, *clsA));
  EXPECT_EQ(objcls(make_specialized_exact_object(BObj, *clsA)),
            make_specialized_exact_class(BCls, *clsA));
  EXPECT_EQ(objcls(make_specialized_wait_handle(BObj, TInt)),
            make_specialized_sub_class(BCls, awaitable, false, true, false));

  EXPECT_EQ(
    objcls(make_specialized_sub_object(BObj, *clsICanon11)),
    make_specialized_sub_class(BCls, *clsICanon11, false, false, false)
  );

  EXPECT_EQ(
    objcls(
      intersection_of(
        make_specialized_sub_object(BObj, *clsICanon11),
        make_specialized_sub_object(BObj, *clsICanon13)
      )
    ),
    intersection_of(
      make_specialized_sub_class(BCls, *clsICanon11, false, false, false),
      make_specialized_sub_class(BCls, *clsICanon13, false, false, false)
    )
  );
}

TEST(Type, Option) {
  auto index = make_index();
  EXPECT_TRUE(TTrue.subtypeOf(BOptTrue));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptTrue));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptTrue));

  EXPECT_TRUE(TFalse.subtypeOf(BOptFalse));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptFalse));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptFalse));

  EXPECT_TRUE(TFalse.subtypeOf(BOptBool));
  EXPECT_TRUE(TTrue.subtypeOf(BOptBool));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptBool));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptBool));

  EXPECT_TRUE(ival(3).subtypeOf(BOptInt));
  EXPECT_TRUE(TInt.subtypeOf(BOptInt));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptInt));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptInt));

  EXPECT_TRUE(TDbl.subtypeOf(BOptDbl));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptDbl));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptDbl));
  EXPECT_TRUE(dval(3.0).subtypeOf(BOptDbl));

  EXPECT_TRUE(sval(s_test).subtypeOf(BOptSStr));
  EXPECT_TRUE(sval(s_test).subtypeOf(BOptStr));
  EXPECT_TRUE(sval_nonstatic(s_test).subtypeOf(BOptStr));
  EXPECT_TRUE(TSStr.subtypeOf(BOptSStr));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptSStr));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptSStr));
  EXPECT_TRUE(!TStr.subtypeOf(BOptSStr));
  EXPECT_TRUE(TStr.couldBe(BOptSStr));

  EXPECT_TRUE(TStr.subtypeOf(BOptStr));
  EXPECT_TRUE(TSStr.subtypeOf(BOptStr));
  EXPECT_TRUE(sval(s_test).subtypeOf(BOptStr));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptStr));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptStr));

  EXPECT_TRUE(lazyclsval(s_test).subtypeOf(BOptLazyCls));
  EXPECT_TRUE(TLazyCls.subtypeOf(BOptLazyCls));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptLazyCls));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptLazyCls));

  EXPECT_TRUE(enumclasslabelval(s_test).subtypeOf(BOptEnumClassLabel));
  EXPECT_TRUE(TEnumClassLabel.subtypeOf(BOptEnumClassLabel));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptEnumClassLabel));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptEnumClassLabel));

  EXPECT_TRUE(TObj.subtypeOf(BOptObj));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptObj));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptObj));

  EXPECT_TRUE(TRes.subtypeOf(BOptRes));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptRes));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptRes));

  EXPECT_TRUE(TClsMeth.subtypeOf(BOptClsMeth));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptClsMeth));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptClsMeth));

  EXPECT_TRUE(TRClsMeth.subtypeOf(BOptRClsMeth));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptRClsMeth));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptRClsMeth));

  EXPECT_TRUE(TLazyCls.subtypeOf(BOptLazyCls));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptLazyCls));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptLazyCls));

  EXPECT_TRUE(TArrKey.subtypeOf(BOptArrKey));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptArrKey));
  EXPECT_TRUE(!TUninit.subtypeOf(BOptArrKey));

  for (auto const& t : optionals()) {
    EXPECT_EQ(t, opt(unopt(t)));
  }

  EXPECT_TRUE(wait_handle(opt(dval(2.0))).couldBe(
    wait_handle(dval(2.0))));
}

TEST(Type, OptUnionOf) {
  EXPECT_EQ(opt(ival(2)), union_of(ival(2), TInitNull));
  EXPECT_EQ(opt(dval(2.0)), union_of(TInitNull, dval(2.0)));
  EXPECT_EQ(opt(sval(s_test)), union_of(sval(s_test), TInitNull));
  EXPECT_EQ(opt(lazyclsval(s_test)), union_of(lazyclsval(s_test), TInitNull));
  EXPECT_EQ(opt(sval_nonstatic(s_test)),
            union_of(sval_nonstatic(s_test), TInitNull));
  EXPECT_EQ(opt(sval(s_test)), union_of(TInitNull, sval(s_test)));
  EXPECT_EQ(opt(lazyclsval(s_test)), union_of(TInitNull, lazyclsval(s_test)));
  EXPECT_EQ(opt(sval_nonstatic(s_test)),
            union_of(TInitNull, sval_nonstatic(s_test)));

  EXPECT_EQ(TOptBool, union_of(TOptFalse, TOptTrue));
  EXPECT_EQ(TOptBool, union_of(TOptTrue, TOptFalse));

  EXPECT_EQ(TOptSStr,
            union_of(opt(sval(s_test)), opt(sval(s_TestClass))));
  EXPECT_EQ(TOptStr,
            union_of(opt(sval_nonstatic(s_test)),
                     opt(sval_nonstatic(s_TestClass))));

  EXPECT_EQ(TOptInt, union_of(opt(ival(2)), opt(ival(3))));
  EXPECT_EQ(TOptDbl, union_of(opt(dval(2.0)), opt(dval(3.0))));
  EXPECT_EQ(TOptNum, union_of(TInitNull, TNum));

  EXPECT_EQ(TOptTrue, union_of(TInitNull, TTrue));
  EXPECT_EQ(TOptFalse, union_of(TInitNull, TFalse));
  EXPECT_EQ(TOptRes, union_of(TInitNull, TRes));

  EXPECT_EQ(TOptTrue, union_of(TOptTrue, TTrue));
  EXPECT_EQ(TOptFalse, union_of(TOptFalse, TFalse));
  EXPECT_EQ(TOptBool, union_of(TOptTrue, TFalse));

  EXPECT_EQ(TOptClsMeth, union_of(TInitNull, TClsMeth));
  EXPECT_EQ(TOptRClsMeth, union_of(TInitNull, TRClsMeth));

  auto index = make_index();
  auto const rcls = res::Class::get(s_Awaitable.get());

  EXPECT_TRUE(union_of(TObj, opt(objExact(rcls))) == TOptObj);

  auto wh1 = wait_handle(TInt);
  auto wh2 = wait_handle(ival(2));
  auto wh3 = wait_handle(ival(3));

  EXPECT_TRUE(union_of(wh1, wh2) == wh1);
  auto owh1 = opt(wh1);
  auto owh2 = opt(wh2);
  auto owh3 = opt(wh3);

  EXPECT_TRUE(union_of(owh1, owh2) == owh1);
  EXPECT_TRUE(union_of(owh1, wh2) == owh1);
  EXPECT_TRUE(union_of(owh2, wh1) == owh1);

  EXPECT_TRUE(union_of(wh1, owh2) == owh1);
  EXPECT_TRUE(union_of(wh2, owh1) == owh1);

  EXPECT_TRUE(union_of(wh2, owh3) == owh1);
  EXPECT_TRUE(union_of(owh2, wh3) == owh1);
}

TEST(Type, TV) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    EXPECT_EQ(is_scalar(t), tv(t).has_value());
    if (!t.hasData() && !t.subtypeOf(BNull | BBool | BArrLikeE)) {
       EXPECT_FALSE(tv(t).has_value());
    }

    if (t.couldBe(BCounted & ~(BArrLike | BStr)) ||
        (t.couldBe(BStr) && t.subtypeAmong(BCStr, BStr)) ||
        (t.couldBe(BVec) && t.subtypeAmong(BCVec, BVec)) ||
        (t.couldBe(BDict) && t.subtypeAmong(BCDict, BDict)) ||
        (t.couldBe(BKeyset) && t.subtypeAmong(BCKeyset, BKeyset))) {
      EXPECT_FALSE(is_scalar(t));
      EXPECT_FALSE(tv(t).has_value());
    }

    if (t.couldBe(BInitNull) && !t.subtypeOf(BInitNull)) {
      EXPECT_FALSE(tv(t).has_value());
    }

    if (!t.subtypeOf(BInitNull)) {
      EXPECT_FALSE(is_scalar(opt(t)));
      EXPECT_FALSE(tv(opt(t)).has_value());
    }

    if (t.couldBe(BArrLikeE)) {
      if (!t.subtypeAmong(BVecE, BArrLike) &&
          !t.subtypeAmong(BDictE, BArrLike) &&
          !t.subtypeAmong(BKeysetE, BArrLike)) {
        EXPECT_FALSE(tv(t).has_value());
      }
    }
  }

  auto const test = [&] (const Type& t, TypedValue d) {
    auto const val = tv(t);
    EXPECT_TRUE(val && tvSame(*val, d));
  };
  test(TUninit, make_tv<KindOfUninit>());
  test(TInitNull, make_tv<KindOfNull>());
  test(TTrue, make_tv<KindOfBoolean>(true));
  test(TFalse, make_tv<KindOfBoolean>(false));
  test(vec_empty(), make_array_like_tv(staticEmptyVec()));
  test(some_vec_empty(), make_array_like_tv(staticEmptyVec()));
  test(dict_empty(), make_array_like_tv(staticEmptyDictArray()));
  test(some_dict_empty(), make_array_like_tv(staticEmptyDictArray()));
  test(keyset_empty(), make_array_like_tv(staticEmptyKeysetArray()));
  test(some_keyset_empty(), make_array_like_tv(staticEmptyKeysetArray()));
  test(ival(123), make_tv<KindOfInt64>(123));
  test(dval(3.141), make_tv<KindOfDouble>(3.141));
  test(sval(s_A), tv(s_A));
  test(lazyclsval(s_A), tv(s_A));
  test(vec_val(static_vec(123, 456, 789)),
       make_array_like_tv(const_cast<ArrayData*>(static_vec(123, 456, 789))));
  test(make_specialized_arrpacked(BDictN, {ival(1), ival(2), ival(3)}, LegacyMark::Unmarked),
       make_array_like_tv(const_cast<ArrayData*>(static_dict(0, 1, 1, 2, 2, 3))));
  test(make_specialized_arrpacked(BKeysetN, {ival(0), ival(1)}),
       make_array_like_tv(const_cast<ArrayData*>(static_keyset(0, 1))));

  test(
    make_specialized_arrmap(
      BDictN,
      {map_elem(s_A, ival(1)), map_elem(s_B, ival(2))},
      TBottom, TBottom,
      LegacyMark::Unmarked
    ),
    make_array_like_tv(const_cast<ArrayData*>(static_dict(s_A.get(), 1, s_B.get(), 2)))
  );
  test(
    make_specialized_arrmap(
      BDictN,
      {map_elem_nonstatic(s_A, ival(1)), map_elem_nonstatic(s_B, ival(2))},
      TBottom, TBottom,
      LegacyMark::Unmarked
    ),
    make_array_like_tv(const_cast<ArrayData*>(static_dict(s_A.get(), 1, s_B.get(), 2)))
  );

  EXPECT_FALSE(tv(TOptTrue).has_value());
  EXPECT_FALSE(tv(TOptFalse).has_value());
  EXPECT_FALSE(tv(TNull).has_value());
  EXPECT_FALSE(tv(union_of(dict_empty(), vec_empty())).has_value());
  EXPECT_FALSE(tv(make_specialized_int(BInt|BFalse, 123)).has_value());
  EXPECT_FALSE(tv(make_specialized_string(BStr|BFalse, s_A.get())).has_value());
  EXPECT_FALSE(tv(make_specialized_lazycls(BLazyCls|BFalse, s_A.get())).has_value());
  EXPECT_FALSE(tv(make_specialized_enumclasslabel(BEnumClassLabel|BFalse, s_A.get())).has_value());
  EXPECT_FALSE(
    tv(
      make_specialized_arrmap(
        BDict,
        {map_elem(s_A, ival(1)), map_elem(s_B, ival(2))},
        TBottom, TBottom,
        LegacyMark::Unmarked
      )
    ).has_value()
  );
  EXPECT_FALSE(
    tv(
      make_specialized_arrmap(
        BDictN,
        {map_elem_counted(s_A, ival(1)), map_elem_counted(s_B, ival(2))},
        TBottom, TBottom,
        LegacyMark::Unmarked
      )
    ).has_value()
  );

  EXPECT_FALSE(tv(sval_counted(s_A)).has_value());
  EXPECT_FALSE(tv(TCDictE).has_value());
  EXPECT_FALSE(tv(TCVecE).has_value());
  EXPECT_FALSE(tv(make_specialized_arrpacked(BVecN, {sval_counted(s_A)})).has_value());
  EXPECT_FALSE(tv(make_specialized_arrpacked(BCVec, {ival(123)})).has_value());
  EXPECT_FALSE(
    tv(
      make_specialized_arrmap(
        BCDictN,
        {map_elem(s_A, ival(1)), map_elem(s_B, ival(2))},
        TBottom, TBottom,
        LegacyMark::Unmarked
      )
    ).has_value()
  );

  for (auto const& t : all) {
    EXPECT_EQ(is_scalar_counted(t), tvCounted(t).has_value());
    if (!t.hasData() && !t.subtypeOf(BNull | BBool | BArrLikeE)) {
      EXPECT_FALSE(tvCounted(t).has_value());
    }

    if (is_scalar(t)) {
      EXPECT_TRUE(is_scalar_counted(t));
      EXPECT_TRUE(tvCounted(t).has_value());
    }
    if (!is_scalar_counted(t)) {
      EXPECT_FALSE(is_scalar(t));
      EXPECT_FALSE(tv(t).has_value());
    }

    if (!(t.couldBe(BStr) && t.subtypeAmong(BCStr, BStr)) &&
        !(t.couldBe(BVec) && t.subtypeAmong(BCVec, BVec)) &&
        !(t.couldBe(BDict) && t.subtypeAmong(BCDict, BDict)) &&
        !(t.couldBe(BKeyset) && t.subtypeAmong(BCKeyset, BKeyset))) {
      EXPECT_EQ(is_scalar(t), is_scalar_counted(t));
      EXPECT_EQ(tv(t).has_value(), tvCounted(t).has_value());
    }

    if (t.couldBe(BInitNull) && !t.subtypeOf(BInitNull)) {
      EXPECT_FALSE(tvCounted(t).has_value());
    }

    if (!t.subtypeOf(BInitNull)) {
      EXPECT_FALSE(is_scalar_counted(opt(t)));
      EXPECT_FALSE(tvCounted(opt(t)).has_value());
    }

    if (t.couldBe(BArrLikeE)) {
      if (!t.subtypeAmong(BVecE, BArrLike) &&
          !t.subtypeAmong(BDictE, BArrLike) &&
          !t.subtypeAmong(BKeysetE, BArrLike)) {
        EXPECT_FALSE(tv(t).has_value());
      }
    }
  }

  auto const testC = [&] (const Type& t, TypedValue d) {
    auto const val = tvCounted(t);
    EXPECT_TRUE(val && tvSame(*val, d));
  };
  testC(make_unmarked(TCVecE), make_array_like_tv(staticEmptyVec()));
  testC(make_unmarked(TCDictE), make_array_like_tv(staticEmptyDictArray()));
  testC(TCKeysetE, make_array_like_tv(staticEmptyKeysetArray()));
  testC(sval_counted(s_A), tv(s_A));
  testC(
    make_unmarked(make_specialized_arrpacked(BVecN, {sval_counted(s_A)})),
    make_array_like_tv(const_cast<ArrayData*>(static_vec(s_A.get())))
  );
  testC(
    make_unmarked(make_specialized_arrpacked(BCVecN, {sval(s_A)})),
    make_array_like_tv(const_cast<ArrayData*>(static_vec(s_A.get())))
  );
  testC(
    make_specialized_arrmap(
      BDictN,
      {map_elem_counted(s_A, ival(1)), map_elem_counted(s_B, ival(2))},
      TBottom, TBottom,
      LegacyMark::Unmarked
    ),
    make_array_like_tv(const_cast<ArrayData*>(static_dict(s_A.get(), 1, s_B.get(), 2)))
  );
  testC(
    make_specialized_arrmap(
      BCDictN,
      {map_elem(s_A, ival(1)), map_elem(s_B, ival(2))},
      TBottom, TBottom,
      LegacyMark::Unmarked
    ),
    make_array_like_tv(const_cast<ArrayData*>(static_dict(s_A.get(), 1, s_B.get(), 2)))
  );
}

TEST(Type, OptCouldBe) {
  for (auto const& t : optionals()) {
    if (t.subtypeOf(BInitNull)) continue;
    EXPECT_TRUE(t.couldBe(unopt(t)));
  }

  const std::initializer_list<std::pair<Type, Type>> true_cases{
    { opt(sval(s_test)), TStr },
    { opt(sval(s_test)), TInitNull },
    { opt(sval(s_test)), TSStr },
    { opt(sval(s_test)), sval(s_test) },
    { opt(sval(s_test)), sval_nonstatic(s_test) },

    { opt(sval_nonstatic(s_test)), TStr },
    { opt(sval_nonstatic(s_test)), TInitNull },
    { opt(sval_nonstatic(s_test)), TSStr },
    { opt(sval_nonstatic(s_test)), sval_nonstatic(s_test) },
    { opt(sval_nonstatic(s_test)), sval(s_test) },

    { opt(lazyclsval(s_test)), TLazyCls },
    { opt(lazyclsval(s_test)), TInitNull },
    { opt(lazyclsval(s_test)), lazyclsval(s_test) },
    { opt(sval(s_test)), sval_nonstatic(s_test) },

    { opt(enumclasslabelval(s_test)), TEnumClassLabel },
    { opt(enumclasslabelval(s_test)), TInitNull },
    { opt(enumclasslabelval(s_test)), enumclasslabelval(s_test) },

    { opt(ival(2)), TInt },
    { opt(ival(2)), TInitNull },
    { opt(ival(2)), ival(2) },

    { opt(dval(2.0)), TDbl },
    { opt(dval(2.0)), TInitNull },
    { opt(dval(2.0)), dval(2) },

    { opt(TFalse), TBool },
    { opt(TFalse), TFalse },

    { opt(TTrue), TBool },
    { opt(TTrue), TTrue },

    { opt(TDbl), opt(TNum) },
    { TDbl, opt(TNum) },
    { TNum, opt(TDbl) },

    { opt(TInt), TNum },
    { TInt, opt(TNum) },
    { opt(TDbl), TNum },
  };

  for (auto kv : true_cases) {
    EXPECT_TRUE(kv.first.couldBe(kv.second))
      << show(kv.first) << " couldBe " << show(kv.second)
      << " should be true";
  }

  const std::initializer_list<std::pair<Type, Type>> false_cases{
    { opt(ival(2)), TDbl },
    { opt(dval(2.0)), TInt },
    { opt(TFalse), TTrue },
    { opt(TTrue), TFalse },
    { TFalse, opt(TNum) },
  };

  for (auto kv : false_cases) {
    EXPECT_TRUE(!kv.first.couldBe(kv.second))
      << show(kv.first) << " couldBe " << show(kv.second)
      << " should be false";
  }

  for (auto kv : boost::join(true_cases, false_cases)) {
    EXPECT_EQ(kv.first.couldBe(kv.second), kv.second.couldBe(kv.first))
      << show(kv.first) << " couldBe " << show(kv.second)
      << " wasn't reflexive";
  }

  for (auto const& x : optionals()) {
    if (!x.subtypeOf(BInitNull)) {
      EXPECT_TRUE(x.couldBe(unopt(x)));
    }
    EXPECT_TRUE(x.couldBe(BInitNull));
    for (auto const& y : optionals()) {
      EXPECT_TRUE(x.couldBe(y));
    }
  }
}

TEST(Type, ArrayLikeElem) {
  auto index = make_index();

  const std::vector<Type> keys{
    TInt,
    TStr,
    TSStr,
    TCStr,
    TArrKey,
    TUncArrKey,
    sval(s_A),
    sval(s_B),
    sval(s_C),
    sval_nonstatic(s_A),
    sval_counted(s_A),
    ival(0),
    ival(1),
    ival(123),
    ival(777),
    ival(-1),
    ival(std::numeric_limits<int64_t>::max()),
    union_of(sval(s_A),TInt),
    union_of(sval(s_B),TInt),
    union_of(sval(s_C),TInt),
    union_of(sval_counted(s_A),TInt),
    union_of(ival(0),TStr),
    union_of(ival(1),TStr),
    union_of(ival(123),TStr),
    union_of(ival(777),TStr),
    union_of(ival(-1),TStr),
    union_of(ival(std::numeric_limits<int64_t>::max()),TStr)
  };

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.couldBe(BArrLike)) continue;

    EXPECT_EQ(array_like_elem(t, TStr), array_like_elem(t, TSStr));
    EXPECT_EQ(array_like_elem(t, TStr), array_like_elem(t, TCStr));
    EXPECT_EQ(array_like_elem(t, TArrKey), array_like_elem(t, TUncArrKey));
    EXPECT_EQ(array_like_elem(t, TArrKey), array_like_elem(t, Type{BInt|BCStr}));
    EXPECT_EQ(array_like_elem(t, sval(s_A)), array_like_elem(t, sval_nonstatic(s_A)));
    EXPECT_EQ(array_like_elem(t, sval(s_A)), array_like_elem(t, sval_counted(s_A)));
    EXPECT_EQ(array_like_elem(t, union_of(TInt,sval(s_A))),
              array_like_elem(t, union_of(TInt,sval_nonstatic(s_A))));
    EXPECT_EQ(array_like_elem(t, union_of(TInt,sval(s_A))),
              array_like_elem(t, union_of(TInt,sval_counted(s_A))));

    for (auto const& key : keys) {
      auto const elem = array_like_elem(t, key);

      if (elem.first.is(BBottom)) {
        EXPECT_FALSE(elem.second);
      }
      if (t.couldBe(BArrLikeE)) {
        EXPECT_FALSE(elem.second);
      }
      if (!is_specialized_array_like(t)) {
        EXPECT_FALSE(elem.second);
        if (!t.couldBe(BKeysetN)) {
          EXPECT_FALSE(elem.first.hasData());
        }
      }
      if (!key.hasData()) {
        EXPECT_FALSE(elem.second);
      }
      if (elem.second) {
        EXPECT_TRUE(is_scalar_counted(key));
        EXPECT_TRUE(is_specialized_array_like_arrval(t) ||
                    is_specialized_array_like_map(t) ||
                    (key.subtypeOf(BInt) &&
                     is_specialized_int(key) &&
                     is_specialized_array_like_packed(t)));
      }

      EXPECT_TRUE(elem.first.subtypeOf(BInitCell));
      if (!t.couldBe(BArrLikeN)) {
        EXPECT_TRUE(elem.first.is(BBottom));
      }
      if (t.subtypeAmong(BSArrLikeN, BArrLikeN)) {
        EXPECT_TRUE(elem.first.subtypeOf(BInitUnc));
      }
      if (t.subtypeAmong(BKeysetN, BArrLikeN)) {
        EXPECT_TRUE(elem.first.subtypeOf(BArrKey));
      }
      if (t.subtypeAmong(BSKeysetN, BArrLikeN)) {
        EXPECT_TRUE(elem.first.subtypeOf(BUncArrKey));
      }
      if (t.subtypeAmong(BKeysetN, BArrLikeN)) {
        EXPECT_TRUE(elem.first.subtypeOf(loosen_staticness(key)));
      }

      if (t.subtypeAmong(BVecN, BArrLikeN) && !key.couldBe(BInt)) {
        EXPECT_TRUE(elem.first.is(BBottom));
      }
      if ((is_specialized_array_like_packedn(t) ||
           is_specialized_array_like_packed(t)) && !key.couldBe(BInt)) {
        EXPECT_TRUE(elem.first.is(BBottom));
      }
      if ((is_specialized_array_like_packedn(t) ||
           is_specialized_array_like_packed(t)) &&
          is_specialized_int(key) && ival_of(key) < 0) {
        EXPECT_TRUE(elem.first.is(BBottom));
      }

      if (t.subtypeOf(BCell)) {
        auto const arr = split_array_like(t).first;
        EXPECT_EQ(array_like_elem(arr, key), elem);
      }

      auto const unionTest = [&] (const Type& key2) {
        auto const elem2 = array_like_elem(t, key2);
        auto const elem3 = array_like_elem(t, union_of(key, key2));
        EXPECT_EQ(elem3.first, union_of(elem.first, elem2.first));
        EXPECT_EQ(elem3.second, elem.second && elem2.second);
      };
      if (!key.hasData() || is_specialized_int(key)) unionTest(TInt);
      if (!key.hasData() || is_specialized_string(key)) unionTest(TStr);
      if (!key.hasData()) unionTest(TArrKey);
    }
  }

  auto const staticVec = static_vec(s_A, 100, s_B);
  auto const staticDict = static_dict(s_A, 100, 200, s_B, s_C, s_BA);
  auto const staticKeyset = static_keyset(s_A, 100, s_B);

  auto const mapElems1 = MapElems{
    map_elem(s_A, ival(100)),
    map_elem(200, sval(s_B)),
    map_elem(s_C, sval(s_BA))
  };
  auto const mapElems2 = MapElems{
    map_elem_nonstatic(s_A, ival(100)),
    map_elem(200, sval(s_B)),
    map_elem_nonstatic(s_C, sval(s_BA))
  };
  auto const mapElems3 = MapElems{
    map_elem(s_A, TObj),
    map_elem(s_B, TArrKey)
  };
  auto const mapElems4 = MapElems{
    map_elem(100, TObj),
    map_elem(200, TFalse)
  };

  const std::vector<std::tuple<Type, Type, Type, bool>> tests{
    { TVecE, TInt, TBottom, false },
    { TVecE, TStr, TBottom, false },
    { TVecE, TArrKey, TBottom, false },
    { TVecN, TInt, TInitCell, false },
    { TVec, TInt, TInitCell, false },
    { TSVecN, TInt, TInitUnc, false },
    { TVecN, TStr, TBottom, false },
    { TVecN, ival(-1), TBottom, false },
    { TVecN, ival(0), TInitCell, false },
    { TSVecN, ival(0), TInitUnc, false },
    { TVecN, TArrKey, TInitCell, false },
    { TSVecN, TArrKey, TInitUnc, false },
    { TVecN, union_of(ival(-1),TStr), TInitCell, false },
    { TVecN, union_of(ival(0),TStr), TInitCell, false },
    { TVecN, union_of(TInt,sval(s_A)), TInitCell, false },

    { TDictE, TInt, TBottom, false },
    { TDictE, TStr, TBottom, false },
    { TDictE, TArrKey, TBottom, false },
    { TDictN, TInt, TInitCell, false },
    { TDict, TInt, TInitCell, false },
    { TDict, TStr, TInitCell, false },
    { TSDictN, TInt, TInitUnc, false },
    { TDictN, TStr, TInitCell, false },
    { TSDictN, TStr, TInitUnc, false },
    { TDictN, ival(-1), TInitCell, false },
    { TSDictN, ival(-1), TInitUnc, false },
    { TDictN, ival(0), TInitCell, false },
    { TSDictN, ival(0), TInitUnc, false },
    { TDictN, sval(s_A), TInitCell, false },
    { TSDictN, sval(s_A), TInitUnc, false },
    { TDictN, union_of(ival(-1),TStr), TInitCell, false },
    { TDictN, union_of(ival(0),TStr), TInitCell, false },
    { TDictN, union_of(TInt,sval(s_A)), TInitCell, false },
    { TDictN, TArrKey, TInitCell, false },
    { TSDictN, TArrKey, TInitUnc, false },

    { TKeysetE, TInt, TBottom, false },
    { TKeysetE, TStr, TBottom, false },
    { TKeysetE, TArrKey, TBottom, false },
    { TKeysetN, TInt, TInt, false },
    { TKeyset, TStr, TStr, false },
    { TKeyset, TSStr, TStr, false },
    { TKeyset, TCStr, TStr, false },
    { TKeyset, TInt, TInt, false },
    { TKeyset, TArrKey, TArrKey, false },
    { TKeyset, TUncArrKey, TArrKey, false },
    { TSKeyset, TArrKey, TUncArrKey, false },
    { TSKeyset, TStr, TSStr, false },
    { TSKeyset, TInt, TInt, false },
    { TKeysetN, ival(-1), ival(-1), false },
    { TSKeysetN, ival(-1), ival(-1), false },
    { TKeysetN, ival(0), ival(0), false },
    { TSKeysetN, ival(0), ival(0), false },
    { TKeysetN, sval(s_A), sval_nonstatic(s_A), false },
    { TSKeysetN, sval(s_A), sval(s_A), false },
    { TKeysetN, sval_nonstatic(s_A), sval_nonstatic(s_A), false },
    { TSKeysetN, sval_nonstatic(s_A), sval(s_A), false },
    { TKeysetN, union_of(ival(0),TStr), union_of(ival(0),TStr), false },
    { TSKeysetN, union_of(ival(0),TStr), union_of(ival(0),TSStr), false },
    { TKeysetN, union_of(TInt,sval(s_A)), union_of(TInt,sval_nonstatic(s_A)), false },
    { TSKeysetN, union_of(TInt,sval(s_A)), union_of(TInt,sval(s_A)), false },

    { make_specialized_arrval(BSVecN, staticVec), TInt, TUncArrKey, false },
    { make_specialized_arrval(BSVecN, staticVec), TStr, TBottom, false },
    { make_specialized_arrval(BSVecN, staticVec), TArrKey, TUncArrKey, false },
    { make_specialized_arrval(BSVecN, staticVec), ival(0), sval(s_A), true },
    { make_specialized_arrval(BSVec, staticVec), ival(0), sval(s_A), false },
    { make_specialized_arrval(BSVecN, staticVec), ival(1), ival(100), true },
    { make_specialized_arrval(BSVec, staticVec), ival(1), ival(100), false },
    { make_specialized_arrval(BSVecN, staticVec), ival(3), TBottom, false },
    { make_specialized_arrval(BSVecN, staticVec), ival(-1), TBottom, false },
    { make_specialized_arrval(BSVecN, staticVec), sval(s_A), TBottom, false },
    { make_specialized_arrval(BSVecN, staticVec), union_of(ival(0),TStr), TUncArrKey, false },
    { make_specialized_arrval(BSVecN, staticVec), union_of(ival(1),TStr), TUncArrKey, false },
    { make_specialized_arrval(BSVecN, staticVec), union_of(TInt,sval(s_A)), TUncArrKey, false },

    { make_specialized_arrval(BSDictN, staticDict), TInt, sval(s_B), false },
    { make_specialized_arrval(BSDictN, staticDict), TStr, union_of(sval(s_BA),TInt), false },
    { make_specialized_arrval(BSDictN, staticDict), TCStr, union_of(sval(s_BA),TInt), false },
    { make_specialized_arrval(BSDictN, staticDict), TArrKey, TUncArrKey, false },
    { make_specialized_arrval(BSDictN, staticDict), ival(0), TBottom, false },
    { make_specialized_arrval(BSDictN, staticDict), ival(-1), TBottom, false },
    { make_specialized_arrval(BSDictN, staticDict), ival(200), sval(s_B), true },
    { make_specialized_arrval(BSDict, staticDict), ival(200), sval(s_B), false },
    { make_specialized_arrval(BSDictN, staticDict), sval(s_A), ival(100), true },
    { make_specialized_arrval(BSDict, staticDict), sval(s_A), ival(100), false },
    { make_specialized_arrval(BSDictN, staticDict), sval_counted(s_A), ival(100), true },
    { make_specialized_arrval(BSDictN, staticDict), sval(s_C), sval(s_BA), true },
    { make_specialized_arrval(BSDict, staticDict), sval(s_C), sval(s_BA), false },
    { make_specialized_arrval(BSDictN, staticDict), sval_counted(s_C), sval(s_BA), true },
    { make_specialized_arrval(BSDictN, staticDict), union_of(ival(0),TStr), union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrval(BSDictN, staticDict), union_of(ival(100),TStr), union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrval(BSDictN, staticDict), union_of(TInt,sval(s_A)), union_of(TInt,sval(s_B)), false },
    { make_specialized_arrval(BSDictN, staticDict), union_of(TInt,sval(s_C)), TUncArrKey, false },

    { make_specialized_arrval(BSKeysetN, staticKeyset), TInt, ival(100), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), TStr, TSStr, false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), TArrKey, TUncArrKey, false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), ival(0), TBottom, false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), sval(s_C), TBottom, false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), ival(100), ival(100), true },
    { make_specialized_arrval(BSKeyset, staticKeyset), ival(100), ival(100), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), sval(s_A), sval(s_A), true },
    { make_specialized_arrval(BSKeyset, staticKeyset), sval(s_A), sval(s_A), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), sval_counted(s_A), sval(s_A), true },
    { make_specialized_arrval(BSKeysetN, staticKeyset), sval(s_B), sval(s_B), true },
    { make_specialized_arrval(BSKeyset, staticKeyset), sval(s_B), sval(s_B), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), sval_counted(s_B), sval(s_B), true },
    { make_specialized_arrval(BSKeysetN, staticKeyset), union_of(ival(0),TStr), TUncArrKey, false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), union_of(ival(100),TStr), union_of(ival(100),TSStr), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), union_of(TInt,sval(s_A)), union_of(TInt,sval(s_A)), false },
    { make_specialized_arrval(BSKeysetN, staticKeyset), union_of(TInt,sval(s_B)), union_of(TInt,sval(s_B)), false },

    { make_specialized_arrpackedn(BDictN, TInitCell), TInt, TInitCell, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), TStr, TBottom, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), TArrKey, TInitCell, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), ival(-1), TBottom, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), sval(s_A), TBottom, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), ival(0), TInitCell, false },
    { make_specialized_arrpackedn(BSDictN, TInitUnc), ival(0), TInitUnc, false },
    { make_specialized_arrpackedn(BDictN, TObj), ival(0), TObj, false },
    { make_specialized_arrpackedn(BDict, TObj), ival(0), TObj, false },
    { make_specialized_arrpackedn(BDictN, TSStr), ival(0), TSStr, false },
    { make_specialized_arrpackedn(BDictN, TObj), union_of(ival(-1),TStr), TObj, false },
    { make_specialized_arrpackedn(BDictN, TObj), union_of(ival(0),TStr), TObj, false },
    { make_specialized_arrpackedn(BDictN, TObj), union_of(TInt,sval(s_A)), TObj, false },

    { make_specialized_arrpacked(BDictN, {TInitCell}), TInt, TInitCell, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), TStr, TBottom, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), TArrKey, TInitCell, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), ival(-1), TBottom, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), sval(s_A), TBottom, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), ival(1), TBottom, false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), ival(0), TInitCell, true },
    { make_specialized_arrpacked(BDict, {TInitCell}), ival(0), TInitCell, false },
    { make_specialized_arrpacked(BSDictN, {TInitUnc}), ival(0), TInitUnc, true },
    { make_specialized_arrpacked(BSDict, {TInitUnc}), ival(0), TInitUnc, false },
    { make_specialized_arrpacked(BDictN, {TObj}), ival(0), TObj, true },
    { make_specialized_arrpacked(BDictN, {TSStr}), TInt, TSStr, false },
    { make_specialized_arrpacked(BDictN, {TObj}), union_of(ival(1),TStr), TObj, false },
    { make_specialized_arrpacked(BDictN, {TObj}), union_of(ival(0),TStr), TObj, false },
    { make_specialized_arrpacked(BDictN, {TObj}), union_of(TInt,sval(s_A)), TObj, false },
    { make_specialized_arrpacked(BDictN, {TObj, TInt}), TInt, Type{BObj|BInt}, false },
    { make_specialized_arrpacked(BDictN, {TObj, TInt}), ival(0), TObj, true },
    { make_specialized_arrpacked(BDictN, {TObj, TInt}), ival(1), TInt, true },
    { make_specialized_arrpacked(BDictN, {TObj, TInt}), ival(2), TBottom, false },

    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TInt, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TSStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TCStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TArrKey, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), TUncArrKey, TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), ival(0), TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), ival(-1), TObj, false },
    { make_specialized_arrmapn(BDictN, TArrKey, TObj), sval(s_A), TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TInt, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TSStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TCStr, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TArrKey, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TObj), TUncArrKey, TObj, false },
    { make_specialized_arrmapn(BDictN, TUncArrKey, TSStr), TInt, TSStr, false },
    { make_specialized_arrmapn(BDictN, TInt, TObj), TInt, TObj, false },
    { make_specialized_arrmapn(BDictN, TInt, TObj), TStr, TBottom, false },
    { make_specialized_arrmapn(BDictN, TInt, TObj), TArrKey, TObj, false },

    { make_specialized_arrmap(BDictN, mapElems1), TInt, sval(s_B), false },
    { make_specialized_arrmap(BDictN, mapElems1), TStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems1), TSStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems1), TCStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems2), TInt, sval(s_B), false },
    { make_specialized_arrmap(BDictN, mapElems2), TStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems2), TSStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems2), TCStr, union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems1), TArrKey, TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems1), TUncArrKey, TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems2), TArrKey, TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems2), TUncArrKey, TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems3), TInt, TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems3), TStr, Type{BObj|BArrKey}, false },
    { make_specialized_arrmap(BDictN, mapElems3), ival(100), TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems1), ival(0), TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems1), sval(s_B), TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems1), ival(200), sval(s_B), true },
    { make_specialized_arrmap(BDict, mapElems1), ival(200), sval(s_B), false },
    { make_specialized_arrmap(BDictN, mapElems1), sval(s_A), ival(100), true },
    { make_specialized_arrmap(BDict, mapElems1), sval(s_A), ival(100), false },
    { make_specialized_arrmap(BDictN, mapElems1), sval_nonstatic(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems1), sval_counted(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems2), ival(200), sval(s_B), true },
    { make_specialized_arrmap(BDictN, mapElems2), sval(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems2), sval_nonstatic(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems2), sval_counted(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems3), union_of(ival(0),TStr), Type{BObj|BArrKey}, false },
    { make_specialized_arrmap(BDictN, mapElems3), union_of(TInt,sval(s_BA)), Type{BArrKey|BObj}, false },
    { make_specialized_arrmap(BDictN, mapElems3), union_of(TInt,sval(s_A)), Type{BArrKey|BObj}, false },
    { make_specialized_arrmap(BDictN, mapElems3), union_of(TInt,sval(s_B)), Type{BArrKey|BObj}, false },
    { make_specialized_arrmap(BDictN, mapElems1), union_of(ival(0),TStr), union_of(TInt,sval(s_BA)), false },
    { make_specialized_arrmap(BDictN, mapElems1), union_of(TInt,sval(s_BA)), TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems1), union_of(TInt,sval(s_A)), union_of(TInt,sval(s_B)), false },
    { make_specialized_arrmap(BDictN, mapElems1), union_of(ival(200),TStr), TUncArrKey, false },
    { make_specialized_arrmap(BDictN, mapElems4), union_of(ival(100),TStr), Type{BObj|BFalse}, false },
    { make_specialized_arrmap(BDictN, mapElems4), union_of(ival(200),TStr), Type{BObj|BFalse}, false },
    { make_specialized_arrmap(BDictN, mapElems1, TInt, TObj), ival(0), TObj, false },
    { make_specialized_arrmap(BDictN, mapElems1, TSStr, TObj), sval(s_BA), TObj, false },
    { make_specialized_arrmap(BDictN, mapElems1, TSStr, TObj), sval_nonstatic(s_BA), TObj, false },
    { make_specialized_arrmap(BDictN, mapElems1, TSStr, TObj), sval_counted(s_BA), TObj, false },
    { make_specialized_arrmap(BDictN, mapElems1, TSStr, TObj), ival(0), TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems1, TInt, TObj), sval(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems1, TStr, TObj), sval(s_A), ival(100), true },
    { make_specialized_arrmap(BDictN, mapElems3, TInt, TFalse), TInt, TFalse, false },
    { make_specialized_arrmap(BDictN, mapElems3, TInt, TFalse), ival(0), TFalse, false },
    { make_specialized_arrmap(BDictN, mapElems3, TStr, TFalse), TInt, TBottom, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), TStr, TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), TCStr, TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), TSStr, TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), sval(s_A), TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), sval_nonstatic(s_A), TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TSStr, TNum), sval_counted(s_A), TNum, false },
    { make_specialized_arrmap(BDictN, mapElems4, TStr, TNum), union_of(ival(0),TStr), Type{BNum|BFalse|BObj}, false },
    { make_specialized_arrmap(BDictN, mapElems4, TStr, TNum), union_of(ival(100),TStr), Type{BObj|BNum|BFalse}, false },
    { make_specialized_arrmap(BDictN, mapElems3, TInt, TNum), union_of(TInt,sval(s_BA)), Type{BObj|BNum|BStr}, false },
    { make_specialized_arrmap(BDictN, mapElems3, TInt, TNum), union_of(TInt,sval(s_A)), Type{BObj|BNum|BStr}, false }
  };
  for (auto const& t : tests) {
    auto const elem = array_like_elem(std::get<0>(t), std::get<1>(t));
    EXPECT_EQ(elem.first, std::get<2>(t));
    EXPECT_EQ(elem.second, std::get<3>(t));
  }
}

TEST(Type, ArrayLikeNewElem) {
  auto index = make_index();

  const std::vector<Type> values{
    TInt,
    TStr,
    TSStr,
    TCStr,
    TUncArrKey,
    TArrKey,
    ival(0),
    ival(1),
    ival(123),
    ival(777),
    ival(-1),
    ival(std::numeric_limits<int64_t>::max()),
    sval(s_A),
    sval(s_B),
    sval(s_C),
    sval_nonstatic(s_A),
    sval_counted(s_A),
    TObj,
    TInitUnc,
    TInitCell,
    Type{BObj|BInt},
    union_of(sval(s_A),TInt),
    union_of(sval(s_B),TInt),
    union_of(sval(s_C),TInt),
    union_of(sval_counted(s_A),TInt),
    union_of(ival(0),TStr),
    union_of(ival(1),TStr),
    union_of(ival(123),TStr),
    union_of(ival(777),TStr),
    union_of(ival(-1),TStr),
    union_of(ival(std::numeric_limits<int64_t>::max()),TStr)
  };

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.subtypeOf(BCell) || !t.couldBe(BArrLike)) continue;

    for (auto const& v : values) {
      auto const newelem = array_like_newelem(t, v);
      EXPECT_FALSE(newelem.first.couldBe(BArrLikeE));

      if (!newelem.first.couldBe(BArrLike)) {
        EXPECT_TRUE(newelem.second);
      } else {
        EXPECT_FALSE(newelem.first.subtypeAmong(BSArrLike, BArrLike));
        if (!t.subtypeAmong(BKeyset, BArrLike) &&
            !array_like_elem(newelem.first, ival(std::numeric_limits<int64_t>::max())).second) {
          EXPECT_TRUE(v.subtypeOf(array_like_elem(newelem.first, TInt).first));
        }
      }

      if (t.subtypeAmong(BVec, BArrLike)) {
        EXPECT_FALSE(newelem.second);
      }

      if (!t.couldBe(BArrLikeN) && !t.couldBe(BKeyset)) {
        EXPECT_TRUE(newelem.first.couldBe(BArrLike));
        EXPECT_TRUE(is_specialized_array_like_packed(newelem.first));
        EXPECT_EQ(array_like_elem(newelem.first, ival(0)).first, v);
        auto const size = arr_size(split_array_like(newelem.first).first);
        EXPECT_TRUE(size && *size == 1);
        EXPECT_EQ(array_like_elem(newelem.first, ival(1)).first, TBottom);
        EXPECT_FALSE(newelem.second);
      }

      auto [arr, rest] = split_array_like(t);
      auto const elem2 = array_like_newelem(arr, v);
      EXPECT_EQ(newelem.first, union_of(elem2.first, rest));
      EXPECT_EQ(newelem.second, elem2.second);
    }
  }

  auto const mapElem1 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj)
  };
  auto const mapElem2 = MapElems{
    map_elem(s_A, TInt),
    map_elem(100, TObj),
    map_elem(s_B, TFalse),
    map_elem(50, TObj),
  };
  auto const mapElem3 = MapElems{
    map_elem(std::numeric_limits<int64_t>::max(), TInitCell)
  };
  auto const mapElem4 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(int64_t(0), TFalse)
  };
  auto const mapElem5 = MapElems{
    map_elem(s_A, TInt),
    map_elem(100, TObj),
    map_elem(s_B, TFalse),
    map_elem(50, TObj),
    map_elem(101, TInitCell)
  };
  auto const mapElem6 = MapElems{
    map_elem(s_A, ival(100)),
    map_elem(200, sval(s_B)),
    map_elem(s_C, sval(s_BA)),
    map_elem(201, TInt)
  };
  auto const mapElem7 = MapElems{
    map_elem(1, ival(1))
  };
  auto const mapElem8 = MapElems{
    map_elem(s_A, sval(s_A))
  };
  auto const mapElem9 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(s_A, sval(s_A))
  };
  auto const mapElem10 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem(s_A, sval(s_A))
  };
  auto const mapElem11 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem_nonstatic(s_A, sval_nonstatic(s_A))
  };
  auto const mapElem12 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem_nonstatic(s_A, sval_nonstatic(s_A))
  };
  auto const mapElem13 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(-1, ival(-1))
  };
  auto const mapElem14 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem(3, ival(3))
  };
  auto const mapElem15 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem(s_A, sval(s_A)),
    map_elem(100, ival(100))
  };
  auto const mapElem16 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem(s_A, sval(s_A)),
    map_elem_nonstatic(s_B, sval_nonstatic(s_B))
  };
  auto const mapElem17 = MapElems{
    map_elem(int64_t(0), ival(0)),
    map_elem(1, ival(1)),
    map_elem(s_A, sval(s_A)),
    map_elem(s_B, sval(s_B))
  };
  auto const mapElem18 = MapElems{
    map_elem(1, ival(1)),
    map_elem(s_A, sval(s_A))
  };
  auto const mapElem19 = MapElems{
    map_elem_nonstatic(s_A, sval_nonstatic(s_A))
  };

  auto const staticVec = static_vec(s_A, s_B, s_C);
  auto const staticDict = static_dict(s_A, 100, 200, s_B, s_C, s_BA);

  const std::vector<std::tuple<Type, Type, Type, bool>> tests{
    { TVecE, TObj, make_specialized_arrpacked(BVecN, {TObj}), false },
    { TSVecE, TObj, make_specialized_arrpacked(BVecN, {TObj}), false },
    { TCVecE, TObj, make_specialized_arrpacked(BVecN, {TObj}), false },
    { TVecN, TObj, TVecN, false },
    { TSVecN, TObj, TVecN, false },
    { TCVecN, TObj, TVecN, false },
    { TVec, TObj, TVecN, false },
    { TSVec, TObj, TVecN, false },
    { TCVec, TObj, TVecN, false },
    { TDictE, TObj, make_specialized_arrpacked(BDictN, {TObj}), false },
    { TSDictE, TObj, make_specialized_arrpacked(BDictN, {TObj}), false },
    { TCDictE, TObj, make_specialized_arrpacked(BDictN, {TObj}), false },
    { TDictN, TObj, TDictN, true },
    { TSDictN, TObj, TDictN, true },
    { TCDictN, TObj, TDictN, true },
    { TDict, TObj, TDictN, true },
    { TSDict, TObj, TDictN, true },
    { TCDict, TObj, TDictN, true },
    { TKeysetE, TObj, TBottom, true },
    { TKeysetN, TObj, TBottom, true },
    { TKeyset, TObj, TBottom, true },
    { TSKeysetE, TFalse, TBottom, true },
    { TSKeysetN, TFalse, TBottom, true },
    { TSKeyset, TFalse, TBottom, true },
    { TSKeysetE, TInt, make_specialized_arrmapn(BKeysetN, TInt, TInt), false },
    { TKeysetE, TInitCell, TKeysetN, true },
    { TKeysetE, TInitUnc, make_specialized_arrmapn(BKeysetN, TUncArrKey, TUncArrKey), true },
    { TKeysetE, ival(0), make_specialized_arrpacked(BKeysetN, {ival(0)}), false },
    { TSKeysetE, ival(1), make_specialized_arrmap(BKeysetN, mapElem7), false },
    { TKeysetE, sval(s_A), make_specialized_arrmap(BKeysetN, mapElem8), false },
    { TKeysetE, TCls, make_specialized_arrmapn(BKeysetN, TSStr, TSStr), true },
    { TKeysetE, TLazyCls, make_specialized_arrmapn(BKeysetN, TSStr, TSStr), true },
    { TKeysetN, TInt, TKeysetN, false },
    { TKeyset, TInt, TKeysetN, false },
    { TSKeysetN, TInt, TKeysetN, false },
    { TSKeyset, TInt, TKeysetN, false },
    { TCKeysetN, TInt, TKeysetN, false },
    { TCKeyset, TInt, TKeysetN, false },
    { TKeysetN, TLazyCls, TKeysetN, true },
    { TKeysetN, TCls, TKeysetN, true },

    { make_specialized_arrval(BSVecN, staticVec),
      TInt, make_specialized_arrpacked(BVecN, {sval(s_A), sval(s_B), sval(s_C), TInt}), false },
    { make_specialized_arrval(BSVec, staticVec), TInt, make_specialized_arrpackedn(BVecN, TUncArrKey), false },
    { make_specialized_arrval(BSDictN, staticDict), TInt, make_specialized_arrmap(BDictN, mapElem6), false },
    { make_specialized_arrval(BSDict, staticDict), TInt, make_specialized_arrmapn(BDictN, TUncArrKey, TUncArrKey), false },

    { make_specialized_arrpackedn(BVecN, TInt), TStr, make_specialized_arrpackedn(BVecN, TArrKey), false },
    { make_specialized_arrpackedn(BSVecN, TSStr), TInt, make_specialized_arrpackedn(BVecN, TUncArrKey), false },
    { make_specialized_arrpackedn(BVecN, Type{BInitCell & ~BObj}), TObj, TVecN, false },
    { make_specialized_arrpackedn(BVec, TInt), TStr, make_specialized_arrpackedn(BVecN, TArrKey), false },
    { make_specialized_arrpackedn(BSVec, TSStr), TInt, make_specialized_arrpackedn(BVecN, TUncArrKey), false },
    { make_specialized_arrpackedn(BVec, Type{BInitCell & ~BObj}), TObj, TVecN, false },
    { make_specialized_arrpackedn(BDictN, TInt), TStr, make_specialized_arrpackedn(BDictN, TArrKey), false },
    { make_specialized_arrpackedn(BSDictN, TSStr), TInt, make_specialized_arrpackedn(BDictN, TUncArrKey), false },
    { make_specialized_arrpackedn(BDictN, Type{BInitCell & ~BObj}), TObj, make_specialized_arrpackedn(BDictN, TInitCell), false },
    { make_specialized_arrpackedn(BDict, TInt), TStr, make_specialized_arrpackedn(BDictN, TArrKey), false },
    { make_specialized_arrpackedn(BSDict, TSStr), TInt, make_specialized_arrpackedn(BDictN, TUncArrKey), false },
    { make_specialized_arrpackedn(BDict, Type{BInitCell & ~BObj}), TObj, make_specialized_arrpackedn(BDictN, TInitCell), false },
    { make_specialized_arrpackedn(BKeyset, TInt), TInt, make_specialized_arrmapn(BKeysetN, TInt, TInt), false },
    { make_specialized_arrpackedn(BKeyset, TInt), TStr, TKeysetN, false },
    { make_specialized_arrpackedn(BKeyset, TInt), TSStr, make_specialized_arrmapn(BKeysetN, TUncArrKey, TUncArrKey), false },
    { make_specialized_arrpackedn(BKeyset, TInt), sval(s_A),
      make_specialized_arrmapn(BKeysetN, union_of(TInt,sval(s_A)), union_of(TInt,sval(s_A))), false },
    { make_specialized_arrpackedn(BKeyset, TInt), ival(0), make_specialized_arrpackedn(BKeysetN, TInt), false },
    { make_specialized_arrpackedn(BKeyset, TInt), ival(1), make_specialized_arrmapn(BKeysetN, TInt, TInt), false },
    { make_specialized_arrpackedn(BKeysetN, TInt), ival(1), make_specialized_arrpackedn(BKeysetN, TInt), false },

    { make_specialized_arrpacked(BVecN, {TObj}), TStr, make_specialized_arrpacked(BVecN, {TObj, TStr}), false },
    { make_specialized_arrpacked(BSVecN, {TInt}), TStr, make_specialized_arrpacked(BVecN, {TInt, TStr}), false },
    { make_specialized_arrpacked(BVec, {TObj}), TStr, make_specialized_arrpackedn(BVecN, Type{BStr|BObj}), false },
    { make_specialized_arrpacked(BSVec, {TInt}), TStr, make_specialized_arrpackedn(BVecN, TArrKey), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0)}), TStr, TKeysetN, false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), TStr, TKeysetN, false },
    { make_specialized_arrpacked(BKeyset, {ival(0)}), sval(s_A),
      make_specialized_arrmapn(BKeysetN, union_of(TInt,sval(s_A)), union_of(TInt,sval(s_A))), false },
    { make_specialized_arrpacked(BKeyset, {ival(0), ival(1)}), sval(s_A),
      make_specialized_arrmapn(BKeysetN, union_of(TInt,sval(s_A)), union_of(TInt,sval(s_A))), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0)}), sval(s_A), make_specialized_arrmap(BKeysetN, mapElem9), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), sval(s_A), make_specialized_arrmap(BKeysetN, mapElem10), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0)}), sval_nonstatic(s_A), make_specialized_arrmap(BKeysetN, mapElem11), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), sval_nonstatic(s_A), make_specialized_arrmap(BKeysetN, mapElem12), false },
    { make_specialized_arrpacked(BKeyset, {ival(0)}), ival(-1), make_specialized_arrmapn(BKeysetN, TInt, TInt), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0)}), ival(-1), make_specialized_arrmap(BKeysetN, mapElem13), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), ival(0), make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), ival(1), make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), ival(2), make_specialized_arrpacked(BKeysetN, {ival(0),ival(1),ival(2)}), false },
    { make_specialized_arrpacked(BKeyset, {ival(0),ival(1)}), ival(3), make_specialized_arrmapn(BKeysetN, TInt, TInt), false },
    { make_specialized_arrpacked(BKeysetN, {ival(0),ival(1)}), ival(3), make_specialized_arrmap(BKeysetN, mapElem14), false },
    { make_specialized_arrpacked(BKeyset, {ival(0)}), ival(0), make_specialized_arrpacked(BKeysetN, {ival(0)}), false },
    { make_specialized_arrpacked(BKeyset, {ival(0),ival(1)}), ival(0), make_specialized_arrpackedn(BKeysetN, TInt), false },
    { make_specialized_arrpacked(BKeyset, {ival(0)}), ival(1), make_specialized_arrmapn(BKeysetN, TInt, TInt), false },

    { make_specialized_arrmapn(BDictN, TInt, TObj), TStr, make_specialized_arrmapn(BDictN, TInt, Type{BObj|BStr}), true },
    { make_specialized_arrmapn(BSDictN, TSStr, TSStr), TInt, make_specialized_arrmapn(BDictN, TUncArrKey, TUncArrKey), true },
    { make_specialized_arrmapn(BDict, TArrKey, TStr), Type{BInitCell & ~BStr}, TDictN, true },
    { make_specialized_arrmapn(BDict, TStr, TInitCell), TInitCell, TDictN, true },
    { make_specialized_arrmapn(BKeysetN, TInt, TInt), TStr, TKeysetN, false },
    { make_specialized_arrmapn(BKeyset, TInt, TInt), TStr, TKeysetN, false },
    { make_specialized_arrmapn(BKeysetN, TCStr, TCStr), TSStr, make_specialized_arrmapn(BKeysetN, TStr, TStr), false },
    { make_specialized_arrmapn(BKeyset, TCStr, TCStr), TSStr, make_specialized_arrmapn(BKeysetN, TStr, TStr), false },

    { make_specialized_arrmap(BDictN, mapElem1), TFalse, make_specialized_arrmap(BDictN, mapElem4), false },
    { make_specialized_arrmap(BDictN, mapElem2), TInitCell, make_specialized_arrmap(BDictN, mapElem5), false },
    { make_specialized_arrmap(BDictN, mapElem3), TFalse, make_specialized_arrmap(BDictN, mapElem3), true },
    { make_specialized_arrmap(BDictN, mapElem1, TStr, TInt),
      TFalse, make_specialized_arrmap(BDictN, mapElem1, TArrKey, Type{BInt|BFalse}), true },
    { make_specialized_arrmap(BDictN, mapElem1, ival(10), TInt),
      TFalse, make_specialized_arrmap(BDictN, mapElem1, TInt, Type{BInt|BFalse}), false },
    { make_specialized_arrmap(BDictN, mapElem1, ival(std::numeric_limits<int64_t>::max()), TInt),
      TFalse, make_specialized_arrmap(BDictN, mapElem1, TInt, Type{BInt|BFalse}), true },
    { make_specialized_arrmap(BDict, mapElem1), TFalse,
      make_specialized_arrmapn(BDictN, TUncArrKey, Type{BInt|BObj|BFalse}), false },
    { make_specialized_arrmap(BDict, mapElem3), TFalse,
      make_specialized_arrmapn(BDictN, TInt, TInitCell), true },
    { make_specialized_arrmap(BKeysetN, mapElem10), TInt, make_specialized_arrmap(BKeysetN, mapElem10, TInt, TInt), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), TSStr, make_specialized_arrmap(BKeysetN, mapElem10, TSStr, TSStr), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, TInt, TInt), TStr, make_specialized_arrmap(BKeysetN, mapElem10, TArrKey, TArrKey), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), ival(1), make_specialized_arrmap(BKeysetN, mapElem10), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), sval(s_A), make_specialized_arrmap(BKeysetN, mapElem10), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), sval_nonstatic(s_A), make_specialized_arrmap(BKeysetN, mapElem10), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), ival(100), make_specialized_arrmap(BKeysetN, mapElem15), false },
    { make_specialized_arrmap(BKeysetN, mapElem10), sval_nonstatic(s_B), make_specialized_arrmap(BKeysetN, mapElem16), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, TStr, TStr), ival(100),
      make_specialized_arrmap(BKeysetN, mapElem10, union_of(ival(100),TStr), union_of(ival(100),TStr)), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, TInt, TInt), sval(s_B),
      make_specialized_arrmap(BKeysetN, mapElem10, union_of(TInt,sval(s_B)), union_of(TInt,sval(s_B))), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, ival(100), ival(100)), TStr,
      make_specialized_arrmap(BKeysetN, mapElem10, union_of(ival(100),TStr), union_of(ival(100),TStr)), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, ival(100), ival(100)), ival(100), make_specialized_arrmap(BKeysetN, mapElem15), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, sval(s_B), sval(s_B)), sval(s_B), make_specialized_arrmap(BKeysetN, mapElem17), false },
    { make_specialized_arrmap(BKeysetN, mapElem10, sval(s_B), sval(s_B)), sval_nonstatic(s_B), make_specialized_arrmap(BKeysetN, mapElem16), false },
    { make_specialized_arrmap(BKeyset, mapElem10), TInt, make_specialized_arrmapn(BKeysetN, union_of(sval(s_A),TInt), union_of(sval(s_A),TInt)), false },
    { make_specialized_arrmap(BKeyset, mapElem16), TStr, TKeysetN, false },
    { make_specialized_arrmap(BKeyset, mapElem7), TStr, TKeysetN, false },
    { make_specialized_arrmap(BKeyset, mapElem10), ival(1), make_specialized_arrmapn(BKeysetN, union_of(sval(s_A),TInt), union_of(sval(s_A),TInt)), false },
    { make_specialized_arrmap(BKeyset, mapElem10), ival(0), make_specialized_arrmapn(BKeysetN, union_of(sval(s_A),TInt), union_of(sval(s_A),TInt)), false },
    { make_specialized_arrmap(BKeyset, mapElem7), ival(1), make_specialized_arrmap(BKeysetN, mapElem7), false },
    { make_specialized_arrmap(BKeyset, mapElem18), ival(1), make_specialized_arrmap(BKeysetN, mapElem7, sval(s_A), sval(s_A)), false },
    { make_specialized_arrmap(BKeyset, mapElem8), sval_nonstatic(s_A), make_specialized_arrmap(BKeysetN, mapElem19), false },
    { make_specialized_arrmap(BKeysetN, mapElem7), union_of(ival(1),TStr), make_specialized_arrmap(BKeysetN, mapElem7, TArrKey, TArrKey), false },
  };

  auto old = Cfg::Eval::RaiseClassConversionNoticeSampleRate;
  Cfg::Eval::RaiseClassConversionNoticeSampleRate = 1;
  SCOPE_EXIT { Cfg::Eval::RaiseClassConversionNoticeSampleRate = old; };

  for (auto const& t : tests) {
    auto const elem = array_like_newelem(std::get<0>(t), std::get<1>(t));
    EXPECT_EQ(loosen_mark_for_testing(elem.first), std::get<2>(t));
    EXPECT_EQ(elem.second, std::get<3>(t));
  }
}

TEST(Type, ArrayLikeSetElem) {
  auto index = make_index();

  const std::vector<Type> keys{
    TInt,
    TStr,
    TSStr,
    TCStr,
    TArrKey,
    TUncArrKey,
    sval(s_A),
    sval(s_B),
    sval(s_C),
    sval_nonstatic(s_A),
    sval_counted(s_A),
    ival(0),
    ival(1),
    ival(123),
    ival(777),
    ival(-1),
    ival(std::numeric_limits<int64_t>::max()),
    union_of(sval(s_A),TInt),
    union_of(sval(s_B),TInt),
    union_of(sval(s_C),TInt),
    union_of(sval_counted(s_A),TInt),
    union_of(ival(0),TStr),
    union_of(ival(1),TStr),
    union_of(ival(123),TStr),
    union_of(ival(777),TStr),
    union_of(ival(-1),TStr),
    union_of(ival(std::numeric_limits<int64_t>::max()),TStr)
  };

  const std::vector<Type> values{
    TInt,
    TStr,
    TSStr,
    TObj,
    TInitUnc,
    TInitCell
  };

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.subtypeOf(BCell) || !t.couldBe(BArrLike)) continue;

    for (auto const& k : keys) {
      for (auto const& v : values) {
        auto const set = array_like_set(t, k, v);
        EXPECT_FALSE(set.first.couldBe(BArrLikeE));

        if (!set.first.couldBe(BArrLike)) {
          EXPECT_TRUE(set.second);
        } else {
          EXPECT_FALSE(set.first.subtypeAmong(BSArrLike, BArrLike));
        }

        if (t.subtypeAmong(BKeyset, BArrLike)) {
          EXPECT_FALSE(set.first.couldBe(BArrLike));
        }
        if (t.couldBe(BKeyset)) {
          EXPECT_TRUE(set.second);
        }
        if (t.subtypeAmong(BDict, BArrLike)) {
          EXPECT_FALSE(set.second);
        }
        if (t.subtypeAmong(BVec, BArrLike)) {
          if (!k.couldBe(BInt) || !t.couldBe(BVecN)) {
            EXPECT_FALSE(set.first.couldBe(BArrLike));
          }
        }

        if (set.first.couldBe(BArrLike)) {
          EXPECT_TRUE(v.subtypeOf(array_like_elem(set.first, k).first));
        }

        auto [arr, rest] = split_array_like(t);
        auto const set2 = array_like_set(arr, k, v);
        EXPECT_EQ(set.first, union_of(set2.first, rest));
        EXPECT_EQ(set.second, set2.second);
      }
    }
  }

  auto const mapElem1 = MapElems{
    map_elem(s_A, TObj)
  };
  auto const mapElem2 = MapElems{
    map_elem_nonstatic(s_A, TObj)
  };
  auto const mapElem3 = MapElems{
    map_elem(int64_t(0), TInt),
    map_elem(1, TObj),
    map_elem(s_A, TFalse)
  };
  auto const mapElem4 = MapElems{
    map_elem(int64_t(0), TInt),
    map_elem(-1, TStr)
  };
  auto const mapElem5 = MapElems{
    map_elem(int64_t(0), TInt),
    map_elem(2, TStr)
  };
  auto const mapElem6 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TFalse)
  };
  auto const mapElem7 = MapElems{
    map_elem_counted(s_A, TInt),
    map_elem_counted(s_B, TObj),
    map_elem(100, TFalse)
  };
  auto const mapElem8 = MapElems{
    map_elem(s_A, Type{BInt|BTrue}),
    map_elem(s_B, Type{BObj|BTrue}),
    map_elem(100, TFalse)
  };
  auto const mapElem9 = MapElems{
    map_elem_counted(s_A, Type{BInt|BTrue}),
    map_elem_counted(s_B, Type{BObj|BTrue}),
    map_elem(100, TFalse)
  };
  auto const mapElem10 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TBool)
  };
  auto const mapElem11 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TFalse),
    map_elem(100, TFalse)
  };
  auto const mapElem12 = MapElems{
    map_elem_counted(s_A, TInt),
    map_elem_counted(s_B, TFalse),
    map_elem(100, TFalse)
  };
  auto const mapElem13 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TFalse),
    map_elem(s_BA, TInt)
  };
  auto const mapElem14 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TFalse),
    map_elem_counted(s_BA, TInt)
  };
  auto const mapElem15 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TFalse),
    map_elem(s_BA, TTrue)
  };
  auto const mapElem16 = MapElems{
    map_elem(s_A, TInt),
    map_elem(s_B, TObj),
    map_elem(100, TFalse),
    map_elem_nonstatic(s_BA, TTrue)
  };
  auto const mapElem17 = MapElems{
    map_elem(s_A, TStr)
  };
  auto const mapElem18 = MapElems{
    map_elem_nonstatic(s_A, TStr)
  };
  auto const mapElem19 = MapElems{
    map_elem(s_A, union_of(ival(100),TFalse)),
    map_elem(s_B, union_of(ival(200),TFalse))
  };
  auto const mapElem20 = MapElems{
    map_elem(s_A, ival(100)),
    map_elem(s_B, ival(300))
  };
  auto const mapElem21 = MapElems{
    map_elem(1, TSStr)
  };
  auto const mapElem22 = MapElems{
    map_elem(1, TUncArrKey)
  };

  auto const staticVec1 = static_vec(s_A, s_B, 100);
  auto const staticDict1 = static_dict(s_A, 100, s_B, 200);

  const std::vector<std::tuple<Type, Type, Type, Type, bool>> tests{
    { TVecN, TStr, TInitCell, TBottom, true },
    { TVecE, TStr, TInitCell, TBottom, true },
    { TVec, TStr, TInitCell, TBottom, true },
    { TVecE, TInt, TInitCell, TBottom, true },
    { TVecN, ival(-1), TInitCell, TBottom, true },
    { TSVec, TInt, TInitCell, TVecN, true },
    { TSVecN, TInt, TInitCell, TVecN, true },
    { TVec, TInt, TInitCell, TVecN, true },
    { TVecN, TInt, TInitCell, TVecN, true },
    { TVecN, ival(0), TInitCell, TVecN, true },
    { TDictE, TInt, TStr, make_specialized_arrmapn(BDictN, TInt, TStr), false },
    { TSDictE, TInt, TStr, make_specialized_arrmapn(BDictN, TInt, TStr), false },
    { TCDictE, TInt, TStr, make_specialized_arrmapn(BDictN, TInt, TStr), false },
    { TDictE, TArrKey, TInitCell, TDictN, false },
    { TDictE, ival(0), TObj, make_specialized_arrpacked(BDictN, {TObj}), false },
    { TDictE, sval(s_A), TObj, make_specialized_arrmap(BDictN, mapElem1), false },
    { TDictE, sval_nonstatic(s_A), TObj, make_specialized_arrmap(BDictN, mapElem2), false },
    { TDictN, TInt, TStr, TDictN, false },
    { TDict, TInt, TStr, TDictN, false },
    { TSDict, TInt, TStr, TDictN, false },
    { TSDictN, TInt, TStr, TDictN, false },
    { TKeysetN, TArrKey, TArrKey, TBottom, true },
    { TKeysetE, TArrKey, TArrKey, TBottom, true },
    { TKeyset, TArrKey, TArrKey, TBottom, true },
    { TSKeysetN, TArrKey, TArrKey, TBottom, true },
    { TSKeysetE, TArrKey, TArrKey, TBottom, true },
    { TSKeyset, TArrKey, TArrKey, TBottom, true },

    { make_specialized_arrval(BSVecN, staticVec1), TInt, TFalse,
      make_specialized_arrpacked(BVecN, {union_of(sval(s_A),TFalse),union_of(sval(s_B),TFalse),union_of(ival(100),TFalse)}), true },
    { make_specialized_arrval(BSVecN, staticVec1), ival(1), TFalse,
      make_specialized_arrpacked(BVecN, {sval(s_A),TFalse,ival(100)}), false },
    { make_specialized_arrval(BSDictN, staticDict1), TStr, TFalse, make_specialized_arrmap(BDictN, mapElem19, TStr, TFalse), false },
    { make_specialized_arrval(BSDictN, staticDict1), sval(s_B), ival(300), make_specialized_arrmap(BDictN, mapElem20), false },

    { make_specialized_arrpackedn(BVecN, TObj), TInt, TStr, make_specialized_arrpackedn(BVecN, Type{BObj|BStr}), true },
    { make_specialized_arrpackedn(BVecN, Type{BInitCell & ~BObj}), TInt, TObj, TVecN, true },
    { make_specialized_arrpackedn(BVecN, TObj), ival(-1), TStr, TBottom, true },
    { make_specialized_arrpackedn(BVecN, TObj), ival(0), TStr, make_specialized_arrpackedn(BVecN, Type{BObj|BStr}), false },
    { make_specialized_arrpackedn(BVec, TObj), ival(0), TStr, make_specialized_arrpackedn(BVecN, Type{BObj|BStr}), true },
    { make_specialized_arrpackedn(BVecN, TObj), ival(1), TStr, make_specialized_arrpackedn(BVecN, Type{BObj|BStr}), true },
    { make_specialized_arrpackedn(BVecN, TObj), union_of(ival(0),TStr), TStr, make_specialized_arrpackedn(BVecN, Type{BObj|BStr}), true },
    { make_specialized_arrpackedn(BDictN, TObj), TInt, TStr, make_specialized_arrmapn(BDictN, TInt, Type{BObj|BStr}), false },
    { make_specialized_arrpackedn(BDictN, Type{BInitCell & ~BObj}), TArrKey, TObj, TDictN, false },
    { make_specialized_arrpackedn(BDictN, TInitCell), TArrKey, TInitCell, TDictN, false },
    { make_specialized_arrpackedn(BDictN, TObj), sval(s_A), TObj, make_specialized_arrmapn(BDictN, union_of(TInt,sval(s_A)), TObj), false },
    { make_specialized_arrpackedn(BDictN, TObj), ival(0), TStr, make_specialized_arrpackedn(BDictN, Type{BObj|BStr}), false },
    { make_specialized_arrpackedn(BDictN, TObj), ival(1), TStr, make_specialized_arrpackedn(BDictN, Type{BObj|BStr}), false },
    { make_specialized_arrpackedn(BDict, TObj), ival(1), TStr, make_specialized_arrmapn(BDictN, TInt, Type{BObj|BStr}), false },

    { make_specialized_arrpacked(BVecN, {TStr}), TInt, TInt, make_specialized_arrpacked(BVecN, {TArrKey}), true },
    { make_specialized_arrpacked(BVecN, {TStr, TObj}), TInt, TInt, make_specialized_arrpacked(BVecN, {TArrKey, Type{BObj|BInt}}), true },
    { make_specialized_arrpacked(BVecN, {TStr}), ival(-1), TInt, TBottom, true },
    { make_specialized_arrpacked(BVecN, {TStr}), ival(1), TInt, TBottom, true },
    { make_specialized_arrpacked(BVecN, {TStr, TObj}), ival(1), TInt, make_specialized_arrpacked(BVecN, {TStr, TInt}), false },
    { make_specialized_arrpacked(BVec, {TStr, TObj}), ival(1), TInt, make_specialized_arrpacked(BVecN, {TStr, TInt}), true },
    { make_specialized_arrpacked(BVecN, {TStr, TObj}), union_of(ival(1),TStr), TInt, make_specialized_arrpacked(BVecN, {TArrKey, Type{BInt|BObj}}), true },
    { make_specialized_arrpacked(BDictN, {TObj}), TInt, TStr, make_specialized_arrmapn(BDictN, TInt, Type{BObj|BStr}), false },
    { make_specialized_arrpacked(BDictN, {TObj}), TStr, TObj, make_specialized_arrmapn(BDictN, union_of(ival(0),TStr), TObj), false },
    { make_specialized_arrpacked(BDictN, {TInitCell}), TStr, TInitCell, TDictN, false },
    { make_specialized_arrpacked(BDictN, {TInitCell, TInitCell}), TStr, TInitCell, TDictN, false },
    { make_specialized_arrpacked(BDictN, {TInt, TObj}), sval(s_A), TFalse, make_specialized_arrmap(BDictN, mapElem3), false },
    { make_specialized_arrpacked(BDict, {TInt, TObj}), sval(s_A), TFalse, make_specialized_arrmapn(BDictN, union_of(TInt,sval(s_A)), Type{BInt|BObj|BFalse}), false },
    { make_specialized_arrpacked(BDictN, {TInt}), ival(-1), TStr, make_specialized_arrmap(BDictN, mapElem4), false },
    { make_specialized_arrpacked(BDictN, {TInt}), ival(2), TStr, make_specialized_arrmap(BDictN, mapElem5), false },
    { make_specialized_arrpacked(BDict, {TInt}), ival(-1), TStr, make_specialized_arrmapn(BDictN, TInt, TArrKey), false },
    { make_specialized_arrpacked(BDict, {TInt}), ival(2), TStr, make_specialized_arrmapn(BDictN, TInt, TArrKey), false },
    { make_specialized_arrpacked(BDictN, {TInt}), ival(0), TStr, make_specialized_arrpacked(BDictN, {TStr}), false },
    { make_specialized_arrpacked(BDictN, {TInt}), ival(1), TStr, make_specialized_arrpacked(BDictN, {TInt, TStr}), false },

    { make_specialized_arrmapn(BDictN, TStr, TInt), TStr, TStr, make_specialized_arrmapn(BDictN, TStr, TArrKey), false },
    { make_specialized_arrmapn(BDictN, TStr, TInt), TInt, TInt, make_specialized_arrmapn(BDictN, TArrKey, TInt), false },
    { make_specialized_arrmapn(BDictN, TStr, Type{BInitCell & ~BObj}), TInt, TObj, TDictN, false },
    { make_specialized_arrmapn(BDict, TStr, TInt), TStr, TStr, make_specialized_arrmapn(BDictN, TStr, TArrKey), false },
    { make_specialized_arrmapn(BDict, TStr, TInt), TInt, TInt, make_specialized_arrmapn(BDictN, TArrKey, TInt), false },
    { make_specialized_arrmapn(BDict, TStr, Type{BInitCell & ~BObj}), TInt, TObj, TDictN, false },

    { make_specialized_arrmap(BDictN, mapElem6), TStr, TTrue, make_specialized_arrmap(BDictN, mapElem8, TStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem6), TCStr, TTrue, make_specialized_arrmap(BDictN, mapElem8, TCStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem6), TSStr, TTrue, make_specialized_arrmap(BDictN, mapElem8, TSStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem7), TStr, TTrue, make_specialized_arrmap(BDictN, mapElem9, TStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem7), TCStr, TTrue, make_specialized_arrmap(BDictN, mapElem9, TCStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem7), TSStr, TTrue, make_specialized_arrmap(BDictN, mapElem9, TSStr, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem6), TInt, TTrue, make_specialized_arrmap(BDictN, mapElem10, TInt, TTrue), false },
    { make_specialized_arrmap(BDictN, mapElem6), sval(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem11), false },
    { make_specialized_arrmap(BDictN, mapElem6), sval_nonstatic(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem11), false },
    { make_specialized_arrmap(BDictN, mapElem6), sval_counted(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem11), false },
    { make_specialized_arrmap(BDictN, mapElem7), sval(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem12), false },
    { make_specialized_arrmap(BDictN, mapElem7), sval_nonstatic(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem12), false },
    { make_specialized_arrmap(BDictN, mapElem7), sval_counted(s_B), TFalse, make_specialized_arrmap(BDictN, mapElem12), false },
    { make_specialized_arrmap(BDictN, mapElem6), sval(s_BA), TInt, make_specialized_arrmap(BDictN, mapElem13), false },
    { make_specialized_arrmap(BDictN, mapElem6), sval_counted(s_BA), TInt, make_specialized_arrmap(BDictN, mapElem14), false },
    { make_specialized_arrmap(BDictN, mapElem6, TInt, TTrue), sval(s_BA), TFalse,
      make_specialized_arrmap(BDictN, mapElem6, union_of(TInt,sval(s_BA)), TBool), false },
    { make_specialized_arrmap(BDictN, mapElem6, sval(s_BA), TFalse), sval(s_BA), TTrue,
      make_specialized_arrmap(BDictN, mapElem15), false },
    { make_specialized_arrmap(BDictN, mapElem6, sval_counted(s_BA), TFalse), sval(s_BA), TTrue,
      make_specialized_arrmap(BDictN, mapElem16), false },
    { make_specialized_arrmap(BDictN, mapElem6, sval(s_BA), TFalse), sval_counted(s_BA), TTrue,
      make_specialized_arrmap(BDictN, mapElem16), false },
    { make_specialized_arrmap(BDict, mapElem6), TStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem6), TSStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TSStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem6), TCStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem7), TStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem7), TSStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem7), TCStr, TTrue,
      make_specialized_arrmapn(BDictN, union_of(ival(100),TCStr), Type{BInt|BObj|BBool}), false },
    { make_specialized_arrmap(BDict, mapElem6), sval(s_A), TStr,
      make_specialized_arrmap(BDictN, mapElem17, union_of(TInt,sval(s_B)), Type{BObj|BFalse}), false },
    { make_specialized_arrmap(BDict, mapElem6), sval_counted(s_A), TStr,
      make_specialized_arrmap(BDictN, mapElem18, union_of(TInt,sval(s_B)), Type{BObj|BFalse}), false },
    { make_specialized_arrmap(BDict, mapElem6, TInt, TTrue), sval(s_A), TStr,
      make_specialized_arrmap(BDictN, mapElem17, union_of(TInt,sval(s_B)), Type{BObj|BBool}), false },
    { make_specialized_arrmap(BDictN, mapElem21), union_of(ival(1),TStr), TInt,
      make_specialized_arrmap(BDictN, mapElem22, TArrKey, TInt), false },
  };
  for (auto const& t : tests) {
    auto const elem = array_like_set(std::get<0>(t), std::get<1>(t), std::get<2>(t));
    EXPECT_EQ(loosen_mark_for_testing(elem.first), std::get<3>(t));
    EXPECT_EQ(elem.second, std::get<4>(t));
  }
}

TEST(Type, SpecificExamples) {
  // Random examples to stress option types, values, etc:

  EXPECT_TRUE(!TInt.subtypeOf(ival(1)));

  EXPECT_TRUE(TInitCell.couldBe(ival(1)));
  EXPECT_TRUE(ival(2).subtypeOf(BInt));
  EXPECT_TRUE(!ival(2).subtypeOf(BBool));
  EXPECT_TRUE(ival(3).subtypeOf(BInt));
  EXPECT_TRUE(TInt.subtypeOf(BInt));
  EXPECT_TRUE(!TBool.subtypeOf(BInt));
  EXPECT_TRUE(TInitNull.subtypeOf(BOptInt));
  EXPECT_TRUE(!TNull.subtypeOf(BOptInt));
  EXPECT_TRUE(TNull.couldBe(BOptInt));
  EXPECT_TRUE(TNull.couldBe(BOptBool));

  EXPECT_TRUE(TInitNull.subtypeOf(BInitCell));
  EXPECT_TRUE(TInitNull.subtypeOf(BCell));
  EXPECT_TRUE(!TUninit.subtypeOf(BInitNull));

  EXPECT_TRUE(ival(3).subtypeOf(BInt));
  EXPECT_TRUE(ival(3).subtypeOf(opt(ival(3))));
  EXPECT_TRUE(ival(3).couldBe(opt(ival(3))));
  EXPECT_TRUE(ival(3).couldBe(BInt));
  EXPECT_TRUE(TInitNull.couldBe(opt(ival(3))));
  EXPECT_TRUE(TNull.couldBe(opt(ival(3))));
  EXPECT_TRUE(TInitNull.subtypeOf(opt(ival(3))));
  EXPECT_TRUE(!TNull.subtypeOf(opt(ival(3))));

  EXPECT_EQ(intersection_of(TClsMeth, TInitUnc), TClsMeth);

  auto const test_map_a = MapElems{map_elem(s_A, TDbl), map_elem(s_B, TBool)};
  auto const test_map_b = MapElems{map_elem(s_A, TObj), map_elem(s_B, TRes)};

  auto const disjointArrSpecs = std::vector<Type>{
    dict_packedn(TInt),
    dict_packedn(TStr),
    dict_packed({TDbl}),
    dict_packed({TBool}),
    dict_n(TStr, TStr),
    dict_n(TStr, TInt),
    dict_map(test_map_a),
    dict_map(test_map_b)
  };
  for (auto const& t1 : disjointArrSpecs) {
    for (auto const& t2 : disjointArrSpecs) {
      if (t1 == t2) continue;
      EXPECT_FALSE(t1.couldBe(t2));
      EXPECT_FALSE(t2.couldBe(t1));

      auto const t3 = union_of(t1, TDictE);
      auto const t4 = union_of(t2, TDictE);
      EXPECT_TRUE(t3.couldBe(t4));
      EXPECT_TRUE(t4.couldBe(t3));
      EXPECT_FALSE(t3.subtypeOf(t4));
      EXPECT_FALSE(t4.subtypeOf(t3));
      EXPECT_EQ(intersection_of(t3, t4), TDictE);
      EXPECT_EQ(intersection_of(t4, t3), TDictE);

      auto const t5 = opt(t1);
      auto const t6 = opt(t2);
      EXPECT_TRUE(t5.couldBe(t6));
      EXPECT_TRUE(t6.couldBe(t5));
      EXPECT_FALSE(t5.subtypeOf(t6));
      EXPECT_FALSE(t6.subtypeOf(t5));
      EXPECT_EQ(intersection_of(t5, t6), TInitNull);
      EXPECT_EQ(intersection_of(t6, t5), TInitNull);

      auto const t7 = opt(t3);
      auto const t8 = opt(t4);
      EXPECT_TRUE(t7.couldBe(t8));
      EXPECT_TRUE(t8.couldBe(t7));
      EXPECT_FALSE(t7.subtypeOf(t8));
      EXPECT_FALSE(t8.subtypeOf(t7));
      EXPECT_EQ(intersection_of(t7, t8), opt(TDictE));
      EXPECT_EQ(intersection_of(t8, t7), opt(TDictE));
    }
  }
}

TEST(Type, IndexBased) {
  auto idx = make_index();

  auto const cls = idx.resolve_class(s_TestClass.get());
  if (!cls) ADD_FAILURE();
  auto const clsBase = idx.resolve_class(s_Base.get());
  if (!clsBase) ADD_FAILURE();

  auto const objExactTy = objExact(*cls);
  auto const subObjTy   = subObj(*cls);
  auto const clsExactTy = clsExact(*cls, true);
  auto const subClsTy   = subCls(*cls, true);
  auto const objExactBaseTy = objExact(*clsBase);
  auto const subObjBaseTy   = subObj(*clsBase);

  // Basic relationship between the class types and object types.
  EXPECT_EQ(objcls(objExactTy), clsExactTy);
  EXPECT_EQ(objcls(subObjTy), subClsTy);

  // =TestClass <: <=TestClass, and not vice versa.
  EXPECT_TRUE(objExactTy.subtypeOf(subObjTy));
  EXPECT_TRUE(!subObjTy.subtypeOf(objExactTy));
  // =TestClass <: <=TestClass, and not vice versa.
  EXPECT_TRUE(clsExactTy.subtypeOf(subClsTy));
  EXPECT_TRUE(!subClsTy.subtypeOf(clsExactTy));

  // =TestClass couldBe <= TestClass, and vice versa.
  EXPECT_TRUE(objExactTy.couldBe(subObjTy));
  EXPECT_TRUE(subObjTy.couldBe(objExactTy));
  EXPECT_TRUE(clsExactTy.couldBe(subClsTy));
  EXPECT_TRUE(subClsTy.couldBe(clsExactTy));

  // Foo= and Foo<= are both subtypes of Foo, and couldBe Foo.
  EXPECT_TRUE(objExactTy.subtypeOf(BObj));
  EXPECT_TRUE(subObjTy.subtypeOf(BObj));
  EXPECT_TRUE(objExactTy.couldBe(BObj));
  EXPECT_TRUE(subObjTy.couldBe(BObj));
  EXPECT_TRUE(TObj.couldBe(objExactTy));
  EXPECT_TRUE(TObj.couldBe(subObjTy));
  EXPECT_TRUE(clsExactTy.subtypeOf(BCls));
  EXPECT_TRUE(subClsTy.subtypeOf(BCls));
  EXPECT_TRUE(clsExactTy.couldBe(BCls));
  EXPECT_TRUE(subClsTy.couldBe(BCls));
  EXPECT_TRUE(TCls.couldBe(clsExactTy));
  EXPECT_TRUE(TCls.couldBe(subClsTy));

  // These checks are relevant for class to key conversions
  EXPECT_TRUE(clsExactTy.subtypeOf(BOptCls | BOptLazyCls));
  EXPECT_TRUE(subClsTy.subtypeOf(BOptCls | BOptLazyCls));
  EXPECT_TRUE(TCls.subtypeOf(BOptCls | BOptLazyCls));
  EXPECT_TRUE(TLazyCls.subtypeOf(BOptCls | BOptLazyCls));
  EXPECT_TRUE(clsExactTy.couldBe(BOptCls | BOptLazyCls));
  EXPECT_TRUE(subClsTy.couldBe(BOptCls | BOptLazyCls));
  auto keyTy1 = union_of(clsExactTy, sval(s_TestClass));
  EXPECT_TRUE(keyTy1.couldBe(BOptCls | BOptLazyCls));
  auto keyTy2 = union_of(TLazyCls, sval(s_TestClass));
  EXPECT_TRUE(keyTy2.couldBe(BOptCls | BOptLazyCls));
  EXPECT_FALSE(TSStr.couldBe(BOptCls | BOptLazyCls));
  EXPECT_FALSE(TStr.couldBe(BOptCls | BOptLazyCls));


  // Obj= and Obj<= both couldBe ?Obj, and vice versa.
  EXPECT_TRUE(objExactTy.couldBe(BOptObj));
  EXPECT_TRUE(subObjTy.couldBe(BOptObj));
  EXPECT_TRUE(TOptObj.couldBe(objExactTy));
  EXPECT_TRUE(TOptObj.couldBe(subObjTy));

  // Obj= and Obj<= are subtypes of ?Obj.
  EXPECT_TRUE(objExactTy.subtypeOf(BOptObj));
  EXPECT_TRUE(subObjTy.subtypeOf(BOptObj));

  // Obj= is a subtype of ?Obj=, and also ?Obj<=.
  EXPECT_TRUE(objExactTy.subtypeOf(opt(objExactTy)));
  EXPECT_TRUE(objExactTy.subtypeOf(opt(subObjTy)));
  EXPECT_TRUE(!opt(objExactTy).subtypeOf(objExactTy));
  EXPECT_TRUE(!opt(subObjTy).subtypeOf(objExactTy));

  // Obj= couldBe ?Obj= and ?Obj<=, and vice versa.
  EXPECT_TRUE(objExactTy.couldBe(opt(objExactTy)));
  EXPECT_TRUE(opt(objExactTy).couldBe(objExactTy));
  EXPECT_TRUE(objExactTy.couldBe(opt(subObjTy)));
  EXPECT_TRUE(opt(subObjTy).couldBe(objExactTy));

  // Obj<= is not a subtype of ?Obj=, it is overlapping but
  // potentially contains other types.  (We might eventually check
  // whether objects are final as part of this, but not right now.)
  EXPECT_TRUE(!subObjTy.subtypeOf(opt(objExactTy)));
  EXPECT_TRUE(!opt(objExactTy).subtypeOf(subObjTy));

  // Obj<= couldBe ?Obj= and vice versa.
  EXPECT_TRUE(subObjTy.couldBe(opt(objExactTy)));
  EXPECT_TRUE(opt(objExactTy).couldBe(subObjTy));

  // ?Obj<=, ?Obj=, ?Foo<= and ?Foo= couldBe each other
  EXPECT_TRUE(opt(subObjTy).couldBe(opt(objExactBaseTy)));
  EXPECT_TRUE(opt(objExactBaseTy).couldBe(opt(subObjTy)));
  EXPECT_TRUE(opt(subObjTy).couldBe(opt(subObjBaseTy)));
  EXPECT_TRUE(opt(subObjBaseTy).couldBe(opt(subObjTy)));
  EXPECT_TRUE(opt(objExactTy).couldBe(opt(objExactBaseTy)));
  EXPECT_TRUE(opt(objExactBaseTy).couldBe(opt(objExactTy)));
  EXPECT_TRUE(opt(objExactTy).couldBe(opt(subObjBaseTy)));
  EXPECT_TRUE(opt(subObjBaseTy).couldBe(opt(objExactTy)));
}

TEST(Type, Hierarchies) {
  auto idx = make_index();

  // load classes in hierarchy
  auto const clsBase = idx.resolve_class(s_Base.get());
  if (!clsBase) ADD_FAILURE();
  auto const clsA = idx.resolve_class(s_A.get());
  if (!clsA) ADD_FAILURE();
  auto const clsB = idx.resolve_class(s_B.get());
  if (!clsB) ADD_FAILURE();
  auto const clsAA = idx.resolve_class(s_AA.get());
  if (!clsAA) ADD_FAILURE();
  auto const clsAB = idx.resolve_class(s_AB.get());
  if (!clsAB) ADD_FAILURE();
  auto const clsBA = idx.resolve_class(s_BA.get());
  if (!clsBA) ADD_FAILURE();
  auto const clsBB = idx.resolve_class(s_BB.get());
  if (!clsBB) ADD_FAILURE();
  auto const clsBAA = idx.resolve_class(s_BAA.get());
  if (!clsBAA) ADD_FAILURE();
  auto const clsTestClass = idx.resolve_class(s_TestClass.get());
  if (!clsTestClass) ADD_FAILURE();

  // make *exact type* and *sub type* types and objects for all loaded classes
  auto const objExactBaseTy = objExact(*clsBase);
  auto const subObjBaseTy   = subObj(*clsBase);
  auto const clsExactBaseTy = clsExact(*clsBase, true);
  auto const subClsBaseTy   = subCls(*clsBase, true);

  auto const objExactATy    = objExact(*clsA);
  auto const subObjATy      = subObj(*clsA);
  auto const clsExactATy    = clsExact(*clsA, true);
  auto const subClsATy      = subCls(*clsA, true);

  auto const objExactAATy    = objExact(*clsAA);
  auto const subObjAATy      = subObj(*clsAA);
  auto const clsExactAATy    = clsExact(*clsAA, true);
  auto const subClsAATy      = subCls(*clsAA, true);

  auto const objExactABTy    = objExact(*clsAB);
  auto const subObjABTy      = subObj(*clsAB);
  auto const clsExactABTy    = clsExact(*clsAB, true);
  auto const subClsABTy      = subCls(*clsAB, true);

  auto const objExactBTy    = objExact(*clsB);
  auto const subObjBTy      = subObj(*clsB);
  auto const clsExactBTy    = clsExact(*clsB, true);
  auto const subClsBTy      = subCls(*clsB, true);

  auto const objExactBATy    = objExact(*clsBA);
  auto const subObjBATy      = subObj(*clsBA);
  auto const clsExactBATy    = clsExact(*clsBA, true);
  auto const subClsBATy      = subCls(*clsBA, true);

  auto const objExactBBTy    = objExact(*clsBB);
  auto const subObjBBTy      = subObj(*clsBB);
  auto const clsExactBBTy    = clsExact(*clsBB, true);
  auto const subClsBBTy      = subCls(*clsBB, true);

  auto const objExactBAATy    = objExact(*clsBAA);
  auto const subObjBAATy      = subObj(*clsBAA);
  auto const clsExactBAATy    = clsExact(*clsBAA, true);
  auto const subClsBAATy      = subCls(*clsBAA, true);

  auto const objExactTestClassTy = objExact(*clsTestClass);
  auto const subObjTestClassTy   = subObj(*clsTestClass);
  auto const clsExactTestClassTy = clsExact(*clsTestClass, true);
  auto const subClsTestClassTy   = subCls(*clsTestClass, true);

  // check that type from object and type are the same (obnoxious test)
  EXPECT_EQ(objcls(objExactBaseTy), clsExactBaseTy);
  EXPECT_EQ(objcls(subObjBaseTy), subClsBaseTy);
  EXPECT_EQ(objcls(objExactATy), clsExactATy);
  EXPECT_EQ(objcls(subObjATy), subClsATy);
  EXPECT_EQ(objcls(objExactAATy), clsExactAATy);
  EXPECT_EQ(objcls(subObjAATy), subClsAATy);
  EXPECT_EQ(objcls(objExactABTy), clsExactABTy);
  EXPECT_EQ(objcls(subObjABTy), subClsABTy);
  EXPECT_EQ(objcls(objExactBTy), clsExactBTy);
  EXPECT_EQ(objcls(subObjBTy), subClsBTy);
  EXPECT_EQ(objcls(objExactBATy), clsExactBATy);
  EXPECT_EQ(objcls(subObjBATy), subClsBATy);
  EXPECT_EQ(objcls(objExactBBTy), clsExactBBTy);
  EXPECT_EQ(objcls(subObjBBTy), subClsBBTy);
  EXPECT_EQ(objcls(objExactBAATy), clsExactBAATy);
  EXPECT_EQ(objcls(subObjBAATy), subClsBAATy);

  // both subobj(A) and subcls(A) of no_override class A change to exact types
  EXPECT_EQ(objcls(objExactABTy), subClsABTy);
  EXPECT_EQ(objcls(subObjABTy), clsExactABTy);

  // a T= is a subtype of itself but not a strict subtype
  // also a T= is in a "could be" relationship with itself.
  EXPECT_TRUE(objcls(objExactBaseTy).subtypeOf(clsExactBaseTy));
  EXPECT_FALSE(objcls(objExactBaseTy).strictSubtypeOf(objcls(objExactBaseTy)));
  EXPECT_TRUE(objcls(objExactBAATy).subtypeOf(clsExactBAATy));
  EXPECT_FALSE(clsExactBAATy.strictSubtypeOf(objcls(objExactBAATy)));
  EXPECT_TRUE(clsExactBAATy.couldBe(clsExactBAATy));

  // Given the hierarchy A <- B <- C where A is the base then:
  // B= is not in any subtype relationshipt with a A= or C=.
  // Neither they are in "could be" relationships.
  // Overall T= sets are always disjoint.
  EXPECT_FALSE(objcls(objExactBATy).subtypeOf(clsExactBaseTy));
  EXPECT_FALSE(objcls(objExactBATy).subtypeOf(clsExactBTy));
  EXPECT_FALSE(objcls(objExactBATy).subtypeOf(clsExactBAATy));
  EXPECT_FALSE(clsExactBATy.strictSubtypeOf(objcls(objExactBaseTy)));
  EXPECT_FALSE(clsExactBATy.strictSubtypeOf(objcls(objExactBTy)));
  EXPECT_FALSE(clsExactBATy.strictSubtypeOf(objcls(objExactBAATy)));
  EXPECT_FALSE(clsExactBATy.couldBe(objcls(objExactBaseTy)));
  EXPECT_FALSE(objcls(objExactBATy).couldBe(clsExactBTy));
  EXPECT_FALSE(clsExactBATy.couldBe(objcls(objExactBAATy)));

  // any T= is both a subtype and strict subtype of T<=.
  // Given the hierarchy A <- B <- C where A is the base then:
  // C= is a subtype and a strict subtype of B<=, ?B<=, A<= and ?A<=.
  // The "could be" relationship also holds.
  EXPECT_TRUE(objcls(objExactATy).subtypeOf(subClsATy));
  EXPECT_TRUE(objcls(objExactBAATy).subtypeOf(subClsBaseTy));
  EXPECT_TRUE(objExactBAATy.subtypeOf(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(objExactBAATy).subtypeOf(subClsBTy));
  EXPECT_TRUE(objExactBAATy.subtypeOf(opt(subObjBTy)));
  EXPECT_TRUE(clsExactBAATy.subtypeOf(objcls(subObjBATy)));
  EXPECT_TRUE(objExactBAATy.subtypeOf(opt(subObjBATy)));
  EXPECT_TRUE(clsExactBAATy.subtypeOf(objcls(subObjBAATy)));
  EXPECT_TRUE(objExactBAATy.subtypeOf(opt(subObjBAATy)));
  EXPECT_TRUE(objcls(objExactATy).strictSubtypeOf(subClsATy));
  EXPECT_TRUE(objcls(objExactBAATy).strictSubtypeOf(subClsBaseTy));
  EXPECT_TRUE(objExactBAATy.strictSubtypeOf(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(objExactBAATy).strictSubtypeOf(subClsBTy));
  EXPECT_TRUE(objExactBAATy.strictSubtypeOf(opt(subObjBTy)));
  EXPECT_TRUE(clsExactBAATy.strictSubtypeOf(objcls(subObjBATy)));
  EXPECT_TRUE(objExactBAATy.strictSubtypeOf(opt(subObjBATy)));
  EXPECT_TRUE(clsExactBAATy.strictSubtypeOf(objcls(subObjBAATy)));
  EXPECT_TRUE(objExactBAATy.strictSubtypeOf(opt(subObjBAATy)));
  EXPECT_TRUE(objcls(objExactATy).couldBe(subClsATy));
  EXPECT_TRUE(objcls(objExactBAATy).couldBe(subClsBaseTy));
  EXPECT_TRUE(objExactBAATy.couldBe(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(objExactBAATy).couldBe(subClsBTy));
  EXPECT_TRUE(objExactBAATy.couldBe(opt(subObjBTy)));
  EXPECT_TRUE(clsExactBAATy.couldBe(objcls(subObjBATy)));
  EXPECT_TRUE(objExactBAATy.couldBe(opt(subObjBATy)));
  EXPECT_TRUE(clsExactBAATy.couldBe(objcls(subObjBAATy)));
  EXPECT_TRUE(objExactBAATy.couldBe(opt(subObjBAATy)));

  // a T<= is a subtype of itself but not a strict subtype
  // also a T<= is in a "could be" relationship with itself
  EXPECT_TRUE(objcls(subObjBaseTy).subtypeOf(subClsBaseTy));
  EXPECT_FALSE(objcls(subObjBaseTy).strictSubtypeOf(objcls(subObjBaseTy)));
  EXPECT_TRUE(objcls(subObjBAATy).subtypeOf(subClsBAATy));
  EXPECT_FALSE(subClsBAATy.strictSubtypeOf(objcls(subObjBAATy)));
  EXPECT_TRUE(subClsBAATy.couldBe(subClsBAATy));

  // a T<= type is in no subtype relationship with T=.
  // However a T<= is in a "could be" relationship with T=.
  EXPECT_FALSE(objcls(subObjATy).subtypeOf(clsExactATy));
  EXPECT_FALSE(objcls(subObjATy).strictSubtypeOf(clsExactATy));
  EXPECT_TRUE(clsExactATy.couldBe(objcls(subObjATy)));

  // Given 2 types A and B in no inheritance relationship then
  // A<= and B<= are in no subtype or "could be" relationship.
  // Same if one of the 2 types is an optional type
  EXPECT_FALSE(objcls(subObjATy).subtypeOf(clsExactBTy));
  EXPECT_FALSE(objcls(subObjATy).strictSubtypeOf(clsExactBTy));
  EXPECT_FALSE(subObjATy.subtypeOf(opt(objExactBTy)));
  EXPECT_FALSE(subObjATy.strictSubtypeOf(opt(objExactBTy)));
  EXPECT_FALSE(clsExactATy.couldBe(objcls(subObjBTy)));
  EXPECT_FALSE(objExactATy.couldBe(opt(subObjBTy)));
  EXPECT_FALSE(objcls(subObjBTy).subtypeOf(clsExactATy));
  EXPECT_FALSE(subObjBTy.subtypeOf(opt(objExactATy)));
  EXPECT_FALSE(objcls(subObjBTy).strictSubtypeOf(clsExactATy));
  EXPECT_FALSE(subObjBTy.strictSubtypeOf(opt(objExactATy)));
  EXPECT_FALSE(clsExactBTy.couldBe(objcls(subObjATy)));
  EXPECT_FALSE(objExactBTy.couldBe(opt(subObjATy)));

  // Given the hierarchy A <- B <- C where A is the base then:
  // C<= is a subtype and a strict subtype of B<=, ?B<=, A<= and ?A<=.
  // It is also in a "could be" relationship with all its ancestors
  // (including optional)
  EXPECT_TRUE(objcls(subObjBAATy).subtypeOf(subClsBaseTy));
  EXPECT_TRUE(subObjBAATy.subtypeOf(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(subObjBAATy).subtypeOf(subClsBTy));
  EXPECT_TRUE(subObjBAATy.subtypeOf(opt(subObjBTy)));
  EXPECT_TRUE(subClsBAATy.subtypeOf(objcls(subObjBATy)));
  EXPECT_TRUE(subObjBAATy.subtypeOf(opt(subObjBATy)));
  EXPECT_TRUE(objcls(subObjBAATy).strictSubtypeOf(subClsBaseTy));
  EXPECT_TRUE(subObjBAATy.strictSubtypeOf(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(subObjBAATy).strictSubtypeOf(subClsBTy));
  EXPECT_TRUE(subObjBAATy.strictSubtypeOf(opt(subObjBTy)));
  EXPECT_TRUE(subClsBAATy.strictSubtypeOf(objcls(subObjBATy)));
  EXPECT_TRUE(subObjBAATy.strictSubtypeOf(opt(subObjBATy)));
  EXPECT_TRUE(objcls(subObjBAATy).couldBe(subClsBaseTy));
  EXPECT_TRUE(subObjBAATy.couldBe(opt(subObjBaseTy)));
  EXPECT_TRUE(objcls(subObjBAATy).couldBe(subClsBTy));
  EXPECT_TRUE(subObjBAATy.couldBe(opt(subObjBTy)));
  EXPECT_TRUE(subClsBAATy.couldBe(objcls(subObjBATy)));
  EXPECT_TRUE(subObjBAATy.couldBe(opt(subObjBATy)));

  // Given the hierarchy A <- B <- C where A is the base then:
  // A<= is not in a subtype neither a strict subtype with B<=, ?B<=, A<=
  // ?A<=. However A<= is in a "could be" relationship with all its
  // children (including optional)
  EXPECT_FALSE(objcls(subObjBaseTy).subtypeOf(subClsATy));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjATy)));
  EXPECT_FALSE(objcls(subObjBaseTy).subtypeOf(subClsBTy));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjBTy)));
  EXPECT_FALSE(subClsBaseTy.subtypeOf(objcls(subObjAATy)));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjAATy)));
  EXPECT_FALSE(subClsBaseTy.subtypeOf(objcls(subObjABTy)));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjABTy)));
  EXPECT_FALSE(objcls(subObjBaseTy).subtypeOf(subClsBATy));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjBATy)));
  EXPECT_FALSE(subClsBaseTy.subtypeOf(objcls(subObjBBTy)));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjBBTy)));
  EXPECT_FALSE(subClsBaseTy.subtypeOf(objcls(subObjBAATy)));
  EXPECT_FALSE(subObjBaseTy.subtypeOf(opt(subObjBAATy)));
  EXPECT_FALSE(objcls(subObjBaseTy).strictSubtypeOf(subClsATy));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjATy)));
  EXPECT_FALSE(objcls(subObjBaseTy).strictSubtypeOf(subClsBTy));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjBTy)));
  EXPECT_FALSE(subClsBaseTy.strictSubtypeOf(objcls(subObjAATy)));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjAATy)));
  EXPECT_FALSE(subClsBaseTy.strictSubtypeOf(objcls(subObjABTy)));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjABTy)));
  EXPECT_FALSE(objcls(subObjBaseTy).strictSubtypeOf(subClsBATy));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjBATy)));
  EXPECT_FALSE(subClsBaseTy.strictSubtypeOf(objcls(subObjBBTy)));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjBBTy)));
  EXPECT_FALSE(subClsBaseTy.strictSubtypeOf(objcls(subObjBAATy)));
  EXPECT_FALSE(subObjBaseTy.strictSubtypeOf(opt(subObjBAATy)));
  EXPECT_TRUE(objcls(subObjBaseTy).couldBe(subClsATy));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjATy)));
  EXPECT_TRUE(objcls(subObjBaseTy).couldBe(subClsBTy));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjBTy)));
  EXPECT_TRUE(subClsBaseTy.couldBe(objcls(subObjAATy)));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjAATy)));
  EXPECT_TRUE(subClsBaseTy.couldBe(objcls(subObjABTy)));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjABTy)));
  EXPECT_TRUE(objcls(subObjBaseTy).couldBe(subClsBATy));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjBATy)));
  EXPECT_TRUE(subClsBaseTy.couldBe(objcls(subObjBBTy)));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjBBTy)));
  EXPECT_TRUE(subClsBaseTy.couldBe(objcls(subObjBAATy)));
  EXPECT_TRUE(subObjBaseTy.couldBe(opt(subObjBAATy)));

  // check union_of
  // union of subCls
  EXPECT_EQ(union_of(subClsATy, subClsBTy), subClsBaseTy);
  EXPECT_EQ(union_of(subClsAATy, subClsABTy), subClsATy);
  EXPECT_EQ(union_of(subClsATy, subClsBAATy), subClsBaseTy);
  EXPECT_EQ(union_of(subClsBAATy, subClsBTy), subClsBTy);
  EXPECT_EQ(union_of(subClsBAATy, subClsBBTy), subClsBTy);
  EXPECT_EQ(union_of(subClsAATy, subClsBaseTy), subClsBaseTy);
  EXPECT_EQ(union_of(subClsAATy, subClsTestClassTy), TCls);
  // union of subCls and clsExact mixed
  EXPECT_EQ(union_of(clsExactATy, subClsBTy), subClsBaseTy);
  EXPECT_EQ(union_of(subClsAATy, clsExactABTy), subClsATy);
  EXPECT_EQ(union_of(clsExactATy, subClsBAATy), subClsBaseTy);
  EXPECT_EQ(union_of(subClsBAATy, clsExactBTy), subClsBTy);
  EXPECT_EQ(union_of(clsExactBAATy, subClsBBTy), subClsBTy);
  EXPECT_EQ(union_of(subClsAATy, clsExactBaseTy), subClsBaseTy);
  EXPECT_EQ(union_of(clsExactAATy, subClsTestClassTy), TCls);
  // union of clsExact
  EXPECT_EQ(union_of(clsExactATy, clsExactBTy), subClsBaseTy);
  EXPECT_EQ(union_of(clsExactAATy, clsExactABTy), subClsATy);
  EXPECT_EQ(union_of(clsExactATy, clsExactBAATy), subClsBaseTy);
  EXPECT_EQ(union_of(clsExactBAATy, clsExactBTy), subClsBTy);
  EXPECT_EQ(union_of(clsExactBAATy, clsExactBBTy), subClsBTy);
  EXPECT_EQ(union_of(clsExactAATy, clsExactBaseTy), subClsBaseTy);
  EXPECT_EQ(union_of(clsExactAATy, subClsTestClassTy), TCls);
  // union of subObj
  EXPECT_EQ(union_of(subObjATy, subObjBTy), subObjBaseTy);
  EXPECT_EQ(union_of(subObjAATy, subObjABTy), subObjATy);
  EXPECT_EQ(union_of(subObjATy, subObjBAATy), subObjBaseTy);
  EXPECT_EQ(union_of(subObjBAATy, subObjBTy), subObjBTy);
  EXPECT_EQ(union_of(subObjBAATy, subObjBBTy), subObjBTy);
  EXPECT_EQ(union_of(subObjAATy, subObjBaseTy), subObjBaseTy);
  EXPECT_EQ(union_of(subObjAATy, subObjTestClassTy), TObj);
  // union of subObj and objExact mixed
  EXPECT_EQ(union_of(objExactATy, subObjBTy), subObjBaseTy);
  EXPECT_EQ(union_of(subObjAATy, objExactABTy), subObjATy);
  EXPECT_EQ(union_of(objExactATy, subObjBAATy), subObjBaseTy);
  EXPECT_EQ(union_of(subObjBAATy, objExactBTy), subObjBTy);
  EXPECT_EQ(union_of(objExactBAATy, subObjBBTy), subObjBTy);
  EXPECT_EQ(union_of(subObjAATy, objExactBaseTy), subObjBaseTy);
  EXPECT_EQ(union_of(objExactAATy, subObjTestClassTy), TObj);
  // union of objExact
  EXPECT_EQ(union_of(objExactATy, objExactBTy), subObjBaseTy);
  EXPECT_EQ(union_of(objExactAATy, objExactABTy), subObjATy);
  EXPECT_EQ(union_of(objExactATy, objExactBAATy), subObjBaseTy);
  EXPECT_EQ(union_of(objExactBAATy, objExactBTy), subObjBTy);
  EXPECT_EQ(union_of(objExactBAATy, objExactBBTy), subObjBTy);
  EXPECT_EQ(union_of(objExactAATy, objExactBaseTy), subObjBaseTy);
  EXPECT_EQ(union_of(objExactAATy, objExactTestClassTy), TObj);
  // optional sub obj
  EXPECT_EQ(union_of(opt(subObjATy), opt(subObjBTy)), opt(subObjBaseTy));
  EXPECT_EQ(union_of(subObjAATy, opt(subObjABTy)), opt(subObjATy));
  EXPECT_EQ(union_of(opt(subObjATy), subObjBAATy), opt(subObjBaseTy));
  EXPECT_EQ(union_of(opt(subObjBAATy), opt(subObjBTy)), opt(subObjBTy));
  EXPECT_EQ(union_of(opt(subObjBAATy), subObjBBTy), opt(subObjBTy));
  EXPECT_EQ(union_of(opt(subObjAATy), opt(subObjBaseTy)), opt(subObjBaseTy));
  EXPECT_EQ(union_of(subObjAATy, opt(subObjTestClassTy)), opt(TObj));
  // optional sub and exact obj mixed
  EXPECT_EQ(union_of(opt(objExactATy), subObjBTy), opt(subObjBaseTy));
  EXPECT_EQ(union_of(subObjAATy, opt(objExactABTy)), opt(subObjATy));
  EXPECT_EQ(union_of(opt(objExactATy), objExactBAATy), opt(subObjBaseTy));
  EXPECT_EQ(union_of(subObjBAATy, opt(objExactBTy)), opt(subObjBTy));
  EXPECT_EQ(union_of(opt(subObjBAATy), objExactBBTy), opt(subObjBTy));
  EXPECT_EQ(union_of(objExactAATy, opt(objExactBaseTy)), opt(subObjBaseTy));
  EXPECT_EQ(union_of(opt(subObjAATy), objExactTestClassTy), opt(TObj));
}

TEST(Type, Interface) {
  auto idx = make_index();

  // load classes in hierarchy
  auto const clsIA = idx.resolve_class(s_IA.get());
  if (!clsIA) ADD_FAILURE();
  auto const clsIB = idx.resolve_class(s_IB.get());
  if (!clsIB) ADD_FAILURE();
  auto const clsIAA = idx.resolve_class(s_IAA.get());
  if (!clsIAA) ADD_FAILURE();
  auto const clsA = idx.resolve_class(s_A.get());
  if (!clsA) ADD_FAILURE();
  auto const clsAA = idx.resolve_class(s_AA.get());
  if (!clsAA) ADD_FAILURE();

  // make sometypes and objects
  auto const subObjIATy   = subObj(*clsIA);
  auto const subClsIATy   = subCls(*clsIA, true);
  auto const subObjIAATy  = subObj(*clsIAA);
  auto const subClsIAATy  = subCls(*clsIAA, true);
  auto const subObjIBTy   = subObj(*clsIB);
  auto const subObjATy    = subObj(*clsA);
  auto const clsExactATy  = clsExact(*clsA, true);
  auto const subClsATy    = subCls(*clsA, true);
  auto const subObjAATy   = subObj(*clsAA);
  auto const subClsAATy   = subCls(*clsAA, true);
  auto const exactObjATy  = objExact(*clsA);
  auto const exactObjAATy = objExact(*clsAA);

  EXPECT_TRUE(subClsATy.subtypeOf(objcls(subObjIATy)));
  EXPECT_TRUE(subClsATy.couldBe(objcls(subObjIATy)));
  EXPECT_EQ(objcls(subObjATy), subClsATy);
  EXPECT_TRUE(subClsAATy.subtypeOf(objcls(subObjIAATy)));
  EXPECT_TRUE(subClsAATy.couldBe(objcls(subObjIAATy)));
  EXPECT_EQ(objcls(subObjAATy), objcls(subObjIAATy));

  EXPECT_FALSE(subClsATy.subtypeOf(objcls(subObjIAATy)));
  EXPECT_FALSE(objcls(subObjATy).strictSubtypeOf(objcls(subObjIAATy)));

  EXPECT_EQ(intersection_of(subObjIAATy, subObjAATy), subObjAATy);
  EXPECT_EQ(intersection_of(subObjIAATy, exactObjAATy), exactObjAATy);
  EXPECT_EQ(intersection_of(subObjIAATy, exactObjATy), TBottom);
  EXPECT_EQ(intersection_of(subObjIAATy, subObjATy), exactObjAATy);
  EXPECT_EQ(intersection_of(subObjIAATy, subObjIBTy), TBottom);

  EXPECT_FALSE(clsExactATy.couldBe(objcls(subObjIAATy)));

  EXPECT_TRUE(union_of(opt(exactObjATy), opt(subObjIATy)) == opt(subObjIATy));
  // Since we have invariants in the index that types only improve, it is
  // important that the below union is more or equally refined than the
  // above union.
  EXPECT_TRUE(union_of(opt(exactObjATy), subObjIATy) == opt(subObjIATy));
}

TEST(Type, Canonicalization) {
  auto idx = make_index();

  auto const clsICanon1 = idx.resolve_class(s_ICanon1.get());
  if (!clsICanon1) ADD_FAILURE();
  auto const clsICanon2 = idx.resolve_class(s_ICanon2.get());
  if (!clsICanon2) ADD_FAILURE();
  auto const clsICanon3 = idx.resolve_class(s_ICanon3.get());
  if (!clsICanon3) ADD_FAILURE();
  auto const clsICanon4 = idx.resolve_class(s_ICanon4.get());
  if (!clsICanon4) ADD_FAILURE();
  auto const clsICanon5 = idx.resolve_class(s_ICanon5.get());
  if (!clsICanon5) ADD_FAILURE();
  auto const clsICanon6 = idx.resolve_class(s_ICanon6.get());
  if (!clsICanon6) ADD_FAILURE();
  auto const clsCanon3 = idx.resolve_class(s_Canon3.get());
  if (!clsCanon3) ADD_FAILURE();
  auto const clsT1 = idx.resolve_class(s_T1.get());
  if (!clsT1) ADD_FAILURE();
  auto const clsICanon7 = idx.resolve_class(s_ICanon7.get());
  if (!clsICanon7) ADD_FAILURE();
  auto const clsAbs1 = idx.resolve_class(s_Abs1.get());
  if (!clsAbs1) ADD_FAILURE();
  auto const clsICanon8 = idx.resolve_class(s_ICanon8.get());
  if (!clsICanon8) ADD_FAILURE();
  auto const clsT4_C1 = idx.resolve_class(s_T4_C1.get());
  if (!clsT4_C1) ADD_FAILURE();
  auto const clsAbs3 = idx.resolve_class(s_Abs3.get());
  if (!clsAbs3) ADD_FAILURE();
  auto const clsAbs3_C1 = idx.resolve_class(s_Abs3_C1.get());
  if (!clsAbs3_C1) ADD_FAILURE();
  auto const clsAbs4 = idx.resolve_class(s_Abs4.get());
  if (!clsAbs4) ADD_FAILURE();
  auto const clsAbs5_P = idx.resolve_class(s_Abs5_P.get());
  if (!clsAbs5_P) ADD_FAILURE();
  auto const clsAbs6_P = idx.resolve_class(s_Abs6_P.get());
  if (!clsAbs6_P) ADD_FAILURE();
  auto const clsICanon9 = idx.resolve_class(s_ICanon9.get());
  if (!clsICanon9) ADD_FAILURE();
  auto const clsICanon10 = idx.resolve_class(s_ICanon10.get());
  if (!clsICanon10) ADD_FAILURE();
  auto const clsCanon4 = idx.resolve_class(s_Canon4.get());
  if (!clsCanon4) ADD_FAILURE();
  auto const clsZ1 = idx.resolve_class(s_Z1.get());
  if (!clsZ1) ADD_FAILURE();
  auto const clsIZ1 = idx.resolve_class(s_I_Z1.get());
  if (!clsIZ1) ADD_FAILURE();
  auto const clsCanon14 = idx.resolve_class(s_Canon14.get());
  if (!clsCanon14) ADD_FAILURE();

  auto const clsFoo1 = res::Class::getUnresolved(s_Foo1.get());
  if (clsFoo1.isComplete()) ADD_FAILURE();

  EXPECT_EQ(subObj(*clsICanon1), TBottom);
  EXPECT_EQ(objExact(*clsICanon1), TBottom);
  EXPECT_EQ(subCls(*clsICanon1, true), make_specialized_sub_class(BCls, *clsICanon1, false, false));
  EXPECT_EQ(clsExact(*clsICanon1, true), make_specialized_exact_class(BCls, *clsICanon1, false, false));
  EXPECT_EQ(subCls(*clsICanon1, false), TBottom);
  EXPECT_EQ(clsExact(*clsICanon1, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon2), TBottom);
  EXPECT_EQ(objExact(*clsICanon2), TBottom);
  EXPECT_EQ(subCls(*clsICanon2, true), make_specialized_exact_class(BCls, *clsICanon2, false, false));
  EXPECT_EQ(clsExact(*clsICanon2, true), make_specialized_exact_class(BCls, *clsICanon2, false, false));
  EXPECT_EQ(subCls(*clsICanon2, false), TBottom);
  EXPECT_EQ(clsExact(*clsICanon2, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon3), make_specialized_sub_object(BObj, *clsICanon3, false, false));
  EXPECT_EQ(objExact(*clsICanon3), TBottom);
  EXPECT_EQ(subCls(*clsICanon3, true), make_specialized_sub_class(BCls, *clsICanon3, false, false));
  EXPECT_EQ(clsExact(*clsICanon3, true), make_specialized_exact_class(BCls, *clsICanon3, false, false));
  EXPECT_EQ(subCls(*clsICanon3, false), make_specialized_sub_class(BCls, *clsICanon3, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon3, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon4), make_specialized_sub_object(BObj, *clsICanon3, false, false));
  EXPECT_EQ(objExact(*clsICanon4), TBottom);
  EXPECT_EQ(subCls(*clsICanon4, true), make_specialized_sub_class(BCls, *clsICanon4, false, false));
  EXPECT_EQ(clsExact(*clsICanon4, true), make_specialized_exact_class(BCls, *clsICanon4, false, false));
  EXPECT_EQ(subCls(*clsICanon4, false), make_specialized_sub_class(BCls, *clsICanon3, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon4, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon5), make_specialized_exact_object(BObj, *clsCanon3, false, false));
  EXPECT_EQ(objExact(*clsICanon5), TBottom);
  EXPECT_EQ(subCls(*clsICanon5, true), make_specialized_sub_class(BCls, *clsICanon5, false, false));
  EXPECT_EQ(clsExact(*clsICanon5, true), make_specialized_exact_class(BCls, *clsICanon5, false, false));
  EXPECT_EQ(subCls(*clsICanon5, false), make_specialized_exact_class(BCls, *clsCanon3, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon5, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon6), make_specialized_exact_object(BObj, *clsCanon3, false, false));
  EXPECT_EQ(objExact(*clsICanon6), TBottom);
  EXPECT_EQ(subCls(*clsICanon6, true), make_specialized_sub_class(BCls, *clsICanon6, false, false));
  EXPECT_EQ(clsExact(*clsICanon6, true), make_specialized_exact_class(BCls, *clsICanon6, false, false));
  EXPECT_EQ(subCls(*clsICanon6, false), make_specialized_exact_class(BCls, *clsCanon3, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon6, false), TBottom);

  EXPECT_EQ(subObj(clsFoo1), make_specialized_sub_object(BObj, clsFoo1, false, false));
  EXPECT_EQ(objExact(clsFoo1), make_specialized_exact_object(BObj, clsFoo1, false, false));
  EXPECT_EQ(subCls(clsFoo1, true), make_specialized_sub_class(BCls, clsFoo1, false, false));
  EXPECT_EQ(clsExact(clsFoo1, true), make_specialized_exact_class(BCls, clsFoo1, false, false));
  EXPECT_EQ(subCls(clsFoo1, false), make_specialized_sub_class(BCls, clsFoo1, false, false, false));
  EXPECT_EQ(clsExact(clsFoo1, false), make_specialized_exact_class(BCls, clsFoo1, false, false, false));

  EXPECT_EQ(subObj(*clsT1), TBottom);
  EXPECT_EQ(objExact(*clsT1), TBottom);
  EXPECT_EQ(subCls(*clsT1, true), make_specialized_exact_class(BCls, *clsT1, false, false));
  EXPECT_EQ(clsExact(*clsT1, true), make_specialized_exact_class(BCls, *clsT1, false, false));
  EXPECT_EQ(subCls(*clsT1, false), TBottom);
  EXPECT_EQ(clsExact(*clsT1, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon7), TBottom);
  EXPECT_EQ(objExact(*clsICanon7), TBottom);
  EXPECT_EQ(subCls(*clsICanon7, true), make_specialized_sub_class(BCls, *clsICanon7, false, false));
  EXPECT_EQ(clsExact(*clsICanon7, true), make_specialized_exact_class(BCls, *clsICanon7, false, false));
  EXPECT_EQ(subCls(*clsICanon7, false), TBottom);
  EXPECT_EQ(clsExact(*clsICanon7, false), TBottom);

  EXPECT_EQ(subObj(*clsAbs1), TBottom);
  EXPECT_EQ(objExact(*clsAbs1), TBottom);
  EXPECT_EQ(subCls(*clsAbs1, true), make_specialized_exact_class(BCls, *clsAbs1, false, false));
  EXPECT_EQ(clsExact(*clsAbs1, true), make_specialized_exact_class(BCls, *clsAbs1, false, false));
  EXPECT_EQ(subCls(*clsAbs1, false), TBottom);
  EXPECT_EQ(clsExact(*clsAbs1, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon8), make_specialized_exact_object(BObj, *clsT4_C1, false, false));
  EXPECT_EQ(objExact(*clsICanon8), TBottom);
  EXPECT_EQ(subCls(*clsICanon8, true), make_specialized_sub_class(BCls, *clsICanon8, false, false));
  EXPECT_EQ(clsExact(*clsICanon8, true), make_specialized_exact_class(BCls, *clsICanon8, false, false));
  EXPECT_EQ(subCls(*clsICanon8, false), make_specialized_exact_class(BCls, *clsT4_C1, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon8, false), TBottom);

  EXPECT_EQ(subObj(*clsAbs3), make_specialized_sub_object(BObj, *clsAbs3_C1, false, false));
  EXPECT_EQ(objExact(*clsAbs3), TBottom);
  EXPECT_EQ(subCls(*clsAbs3, true), make_specialized_sub_class(BCls, *clsAbs3, false, false));
  EXPECT_EQ(clsExact(*clsAbs3, true), make_specialized_exact_class(BCls, *clsAbs3, false, false));
  EXPECT_EQ(subCls(*clsAbs3, false), make_specialized_sub_class(BCls, *clsAbs3_C1, false, false, false));
  EXPECT_EQ(clsExact(*clsAbs3, false), TBottom);

  EXPECT_EQ(subObj(*clsAbs4), make_specialized_sub_object(BObj, *clsAbs4, false, false));
  EXPECT_EQ(objExact(*clsAbs4), TBottom);
  EXPECT_EQ(subCls(*clsAbs4, true), make_specialized_sub_class(BCls, *clsAbs4, false, false));
  EXPECT_EQ(clsExact(*clsAbs4, true), make_specialized_exact_class(BCls, *clsAbs4, false, false));
  EXPECT_EQ(subCls(*clsAbs4, false), make_specialized_sub_class(BCls, *clsAbs4, false, false, false));
  EXPECT_EQ(clsExact(*clsAbs4, false), TBottom);

  EXPECT_EQ(subObj(*clsAbs5_P), make_specialized_exact_object(BObj, *clsAbs5_P, false, false));
  EXPECT_EQ(objExact(*clsAbs5_P), make_specialized_exact_object(BObj, *clsAbs5_P, false, false));
  EXPECT_EQ(subCls(*clsAbs5_P, true), make_specialized_sub_class(BCls, *clsAbs5_P, false, false));
  EXPECT_EQ(clsExact(*clsAbs5_P, true), make_specialized_exact_class(BCls, *clsAbs5_P, false, false, false));
  EXPECT_EQ(subCls(*clsAbs5_P, false), make_specialized_exact_class(BCls, *clsAbs5_P, false, false, false));
  EXPECT_EQ(clsExact(*clsAbs5_P, false), make_specialized_exact_class(BCls, *clsAbs5_P, false, false, false));

  EXPECT_EQ(subObj(*clsAbs6_P), make_specialized_sub_object(BObj, *clsAbs6_P, false, false));
  EXPECT_EQ(objExact(*clsAbs6_P), make_specialized_exact_object(BObj, *clsAbs6_P, false, false));
  EXPECT_EQ(subCls(*clsAbs6_P, true), make_specialized_sub_class(BCls, *clsAbs6_P, false, false));
  EXPECT_EQ(clsExact(*clsAbs6_P, true), make_specialized_exact_class(BCls, *clsAbs6_P, false, false, false));
  EXPECT_EQ(subCls(*clsAbs6_P, false), make_specialized_sub_class(BCls, *clsAbs6_P, false, false, false));
  EXPECT_EQ(clsExact(*clsAbs6_P, false), make_specialized_exact_class(BCls, *clsAbs6_P, false, false, false));

  EXPECT_EQ(subObj(*clsICanon9), make_specialized_exact_object(BObj, *clsCanon4, false, false));
  EXPECT_EQ(objExact(*clsICanon9), TBottom);
  EXPECT_EQ(subCls(*clsICanon9, true), make_specialized_sub_class(BCls, *clsICanon9, false, false));
  EXPECT_EQ(clsExact(*clsICanon9, true), make_specialized_exact_class(BCls, *clsICanon9, false, false));
  EXPECT_EQ(subCls(*clsICanon9, false), make_specialized_exact_class(BCls, *clsCanon4, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon9, false), TBottom);

  EXPECT_EQ(subObj(*clsICanon10), make_specialized_exact_object(BObj, *clsCanon4, false, false));
  EXPECT_EQ(objExact(*clsICanon10), TBottom);
  EXPECT_EQ(subCls(*clsICanon10, true), make_specialized_sub_class(BCls, *clsICanon10, false, false));
  EXPECT_EQ(clsExact(*clsICanon10, true), make_specialized_exact_class(BCls, *clsICanon10, false, false));
  EXPECT_EQ(subCls(*clsICanon10, false), make_specialized_exact_class(BCls, *clsCanon4, false, false, false));
  EXPECT_EQ(clsExact(*clsICanon10, false), TBottom);

  EXPECT_EQ(subObj(*clsZ1), make_specialized_sub_object(BObj, *clsZ1, false, false));
  EXPECT_EQ(objExact(*clsZ1), TBottom);
  EXPECT_EQ(subCls(*clsZ1, true), make_specialized_sub_class(BCls, *clsZ1, false, false));
  EXPECT_EQ(clsExact(*clsZ1, true), make_specialized_exact_class(BCls, *clsZ1, false, false));
  EXPECT_EQ(subCls(*clsZ1, false), make_specialized_sub_class(BCls, *clsZ1, false, false, false));
  EXPECT_EQ(clsExact(*clsZ1, false), TBottom);

  EXPECT_EQ(subObj(*clsIZ1), make_specialized_sub_object(BObj, *clsZ1, false, false));
  EXPECT_EQ(objExact(*clsIZ1), TBottom);
  EXPECT_EQ(subCls(*clsIZ1, true), make_specialized_sub_class(BCls, *clsIZ1, false, false));
  EXPECT_EQ(clsExact(*clsIZ1, true), make_specialized_exact_class(BCls, *clsIZ1, false, false));
  EXPECT_EQ(subCls(*clsIZ1, false), make_specialized_sub_class(BCls, *clsZ1, false, false, false));
  EXPECT_EQ(clsExact(*clsIZ1, false), TBottom);

  EXPECT_EQ(subObj(*clsCanon14), make_specialized_sub_object(BObj, *clsCanon14, false, false));
  EXPECT_EQ(objExact(*clsCanon14), TBottom);
  EXPECT_EQ(subCls(*clsCanon14, true), make_specialized_sub_class(BCls, *clsCanon14, false, false));
  EXPECT_EQ(clsExact(*clsCanon14, true), make_specialized_exact_class(BCls, *clsCanon14, false, false));
  EXPECT_EQ(subCls(*clsCanon14, false), make_specialized_sub_class(BCls, *clsCanon14, false, false, false));
  EXPECT_EQ(clsExact(*clsCanon14, false), TBottom);
}

TEST(Type, WaitH) {
  auto index = make_index();

  auto const rcls   = res::Class::get(s_Awaitable.get());
  auto const twhobj = subObj(rcls);

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.subtypeOf(BInitCell)) continue;
    auto const wh = wait_handle(t);
    if (t.strictSubtypeOf(BInitCell)) {
      EXPECT_TRUE(is_specialized_wait_handle(wh));
    } else {
      EXPECT_FALSE(is_specialized_wait_handle(wh));
      EXPECT_TRUE(is_specialized_obj(wh));
      EXPECT_EQ(wh, twhobj);
    }
    EXPECT_TRUE(wh.couldBe(twhobj));
    EXPECT_TRUE(wh.subtypeOf(twhobj));
    EXPECT_TRUE(wh.subtypeOf(wait_handle(TInitCell)));
  }

  // union_of(WaitH<A>, WaitH<B>) == WaitH<union_of(A, B)>
  for (auto const& p1 : predefined()) {
    for (auto const& p2 : predefined()) {
      auto const& t1 = p1.second;
      auto const& t2 = p2.second;
      if (!t1.subtypeOf(BInitCell) || !t2.subtypeOf(BInitCell)) continue;
      auto const u1 = union_of(t1, t2);
      auto const u2 = union_of(wait_handle(t1), wait_handle(t2));
      if (u1.strictSubtypeOf(BInitCell)) {
        EXPECT_TRUE(is_specialized_wait_handle(u2));
        EXPECT_EQ(wait_handle_inner(u2), u1);
        EXPECT_EQ(wait_handle(u1), u2);
      } else {
        EXPECT_FALSE(is_specialized_wait_handle(u2));
        EXPECT_TRUE(is_specialized_obj(u2));
        EXPECT_EQ(u2, twhobj);
      }

      if (t1.subtypeOf(t2)) {
        EXPECT_TRUE(wait_handle(t1).subtypeOf(wait_handle(t2)));
      } else {
        EXPECT_FALSE(wait_handle(t1).subtypeOf(wait_handle(t2)));
      }

      // Two wait handles can always be each other
      EXPECT_TRUE(wait_handle(t1).couldBe(wait_handle(t2)));
      auto const isect =
        intersection_of(wait_handle(t1), wait_handle(t2));
      EXPECT_FALSE(isect.is(BBottom));
      EXPECT_TRUE(isect.subtypeOf(wait_handle(t1)));
      EXPECT_TRUE(isect.subtypeOf(wait_handle(t2)));
    }
  }

  // union_of(?WaitH<A>, ?WaitH<B>) == ?WaitH<union_of(A, B)>
  for (auto const& p1 : predefined()) {
    for (auto const& p2 : predefined()) {
      auto const& t1 = p1.second;
      auto const& t2 = p2.second;
      if (t1.is(BBottom) || t2.is(BBottom)) continue;
      if (!t1.subtypeOf(BInitCell) || !t2.subtypeOf(BInitCell)) continue;
      auto const w1 = opt(wait_handle(t1));
      auto const w2 = opt(wait_handle(t2));
      auto const u1 = union_of(w1, w2);
      auto const u2 = opt(wait_handle(union_of(t1, t2)));
      EXPECT_EQ(u1, u2);
    }
  }

  // Some test cases with optional wait handles.
  auto const optWH = opt(wait_handle(ival(2)));
  EXPECT_TRUE(TInitNull.subtypeOf(optWH));
  EXPECT_TRUE(optWH.subtypeOf(BOptObj));
  EXPECT_TRUE(optWH.subtypeOf(opt(twhobj)));
  EXPECT_TRUE(wait_handle(ival(2)).subtypeOf(optWH));
  EXPECT_FALSE(optWH.subtypeOf(wait_handle(ival(2))));
  EXPECT_TRUE(optWH.couldBe(wait_handle(ival(2))));

  // union_of(WaitH<T>, Obj<=Awaitable) == Obj<=Awaitable
  for (auto const& t : all) {
    if (!t.subtypeOf(BInitCell)) continue;
    auto const u = union_of(wait_handle(t), twhobj);
    EXPECT_EQ(u, twhobj);
  }

  for (auto const& t : all) {
    if (!t.subtypeOf(BInitCell)) continue;
    auto const u1 = union_of(wait_handle(t), TInitNull);
    auto const u2 = union_of(TInitNull, wait_handle(t));
    EXPECT_EQ(u1, u2);
    if (t.strictSubtypeOf(BInitCell)) {
      EXPECT_TRUE(is_specialized_wait_handle(u1));
      EXPECT_TRUE(is_specialized_wait_handle(u2));
    } else {
      EXPECT_FALSE(is_specialized_wait_handle(u1));
      EXPECT_FALSE(is_specialized_wait_handle(u2));
      EXPECT_EQ(u1, opt(twhobj));
      EXPECT_EQ(u2, opt(twhobj));
     }
  }

  for (auto const& t : all) {
    if (!t.subtypeOf(BInitCell)) continue;
    auto const wh = wait_handle(t);
    EXPECT_EQ(intersection_of(wh, twhobj), wh);
  }

  EXPECT_EQ(
    intersection_of(wait_handle(TInt), wait_handle(TStr)),
    wait_handle(TBottom)
  );
  EXPECT_TRUE(wait_handle(TInt).couldBe(wait_handle(TStr)));

  auto const eagerWH = wait_handle(TInitCell, false);
  EXPECT_TRUE(is_specialized_wait_handle(eagerWH));
  EXPECT_EQ(wait_handle_inner(eagerWH), TInitCell);
  EXPECT_FALSE(wait_handle_can_suspend(eagerWH));
  EXPECT_TRUE(wait_handle_can_suspend(wait_handle(TInitCell)));
  EXPECT_TRUE(eagerWH.subtypeOf(wait_handle(TInitCell)));
  EXPECT_FALSE(wait_handle(TInitCell).subtypeOf(eagerWH));

  auto const eagerInt = wait_handle(TInt, false);
  auto const eagerStr = wait_handle(TStr, false);
  auto const unknownInt = wait_handle(TInt);
  EXPECT_EQ(set_wait_handle_can_suspend(unknownInt, false), eagerInt);
  EXPECT_EQ(set_wait_handle_can_suspend(eagerInt, true), unknownInt);
  EXPECT_EQ(set_wait_handle_can_suspend(eagerWH, true), twhobj);
  EXPECT_TRUE(wait_handle_can_suspend(union_of(eagerInt, unknownInt)));
  EXPECT_FALSE(wait_handle_can_suspend(intersection_of(eagerInt, unknownInt)));

  EXPECT_EQ(
    union_of(eagerInt, eagerStr),
    wait_handle(TArrKey, false)
  );
  EXPECT_EQ(
    intersection_of(eagerInt, eagerStr),
    wait_handle(TBottom, false)
  );
  EXPECT_EQ(union_of(eagerWH, unknownInt), twhobj);
  EXPECT_EQ(intersection_of(eagerWH, unknownInt), eagerInt);

  EXPECT_EQ(union_of(eagerInt, twhobj), twhobj);
  EXPECT_EQ(intersection_of(eagerInt, twhobj), eagerInt);

  auto const eagerOrInt = union_of(eagerInt, TInt);
  EXPECT_FALSE(is_specialized_wait_handle(eagerOrInt));
  EXPECT_TRUE(wait_handle_can_suspend(eagerOrInt));

  auto const eagerOrVec = union_of(eagerInt, TVec);
  EXPECT_FALSE(is_specialized_wait_handle(eagerOrVec));
  EXPECT_TRUE(wait_handle_can_suspend(eagerOrVec));

  auto const awaitableChild = index.resolve_class(s_AwaitableChild.get());
  ASSERT_TRUE(awaitableChild && awaitableChild->isComplete());
  auto const exactAwaitableChild = objExact(*awaitableChild);
  EXPECT_TRUE(exactAwaitableChild.subtypeOf(twhobj));
  EXPECT_FALSE(exactAwaitableChild.subtypeOf(eagerWH));
  EXPECT_TRUE(
    wait_handle_can_suspend(union_of(exactAwaitableChild, eagerWH))
  );
  auto const eagerAwaitableChild =
    intersection_of(eagerWH, exactAwaitableChild);
  EXPECT_TRUE(is_specialized_wait_handle(eagerAwaitableChild));
  EXPECT_FALSE(wait_handle_can_suspend(eagerAwaitableChild));
  EXPECT_EQ(wait_handle_inner(eagerAwaitableChild), TInitCell);
  EXPECT_TRUE(eagerAwaitableChild.subtypeOf(eagerWH));
  EXPECT_TRUE(eagerAwaitableChild.subtypeOf(exactAwaitableChild));
  auto const ctxEagerAwaitableChild = setctx(eagerAwaitableChild, true);
  EXPECT_TRUE(dobj_of(ctxEagerAwaitableChild).isCtx());
  EXPECT_TRUE(unctx(ctxEagerAwaitableChild).equal(eagerAwaitableChild));
  EXPECT_TRUE(
    return_with_context(ctxEagerAwaitableChild, exactAwaitableChild)
      .equal(eagerAwaitableChild)
  );

  auto const awaitableChild2 = index.resolve_class(s_AwaitableChild2.get());
  ASSERT_TRUE(awaitableChild2 && awaitableChild2->isComplete());
  auto const exactAwaitableChild2 = objExact(*awaitableChild2);
  auto const eagerAwaitableChild2 =
    intersection_of(eagerWH, exactAwaitableChild2);
  EXPECT_EQ(
    union_of(eagerAwaitableChild, eagerAwaitableChild2),
    eagerWH
  );
  EXPECT_EQ(
    intersection_of(eagerAwaitableChild, eagerAwaitableChild2),
    TBottom
  );

  auto const clsX2 = index.resolve_class(s_X2.get());
  ASSERT_TRUE(clsX2 && clsX2->isComplete());
  auto const subX2 = subObj(*clsX2);
  auto const exactX2 = objExact(*clsX2);
  EXPECT_EQ(intersection_of(eagerInt, subX2), TBottom);
  EXPECT_EQ(union_of(eagerInt, exactX2), TObj);

  const std::vector<Type> eagerTypes{
    eagerWH,
    eagerInt,
    eagerStr,
    unknownInt,
    wait_handle(TStr),
    opt(eagerInt),
    eagerOrInt,
    eagerOrVec,
    twhobj,
    exactAwaitableChild,
    exactAwaitableChild2,
    eagerAwaitableChild,
    eagerAwaitableChild2,
    ctxEagerAwaitableChild,
    subX2,
    exactX2,
    TObj,
    TInt,
    TVec,
  };
  test_basic_operators(eagerTypes);
}

TEST(Type, FromHNIConstraint) {
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotNullableTypeName(AnnotType::Resource))), TOptRes);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Resource))), TRes);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Bool))), TBool);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotNullableTypeName(AnnotType::Bool))), TOptBool);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Int))), TInt);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Float))), TDbl);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotNullableTypeName(AnnotType::Float))), TOptDbl);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Mixed))), TInitCell);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::ArrayKey))), TArrKey);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotNullableTypeName(AnnotType::ArrayKey))), TOptArrKey);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotTypeName(AnnotType::Nonnull))), TNonNull);
  EXPECT_EQ(from_hni_constraint(makeStaticString(annotNullableTypeName(AnnotType::Nonnull))), TInitCell);

  // These are conservative, but we're testing them that way.  If we
  // make the function better later we'll remove the tests.
  EXPECT_EQ(from_hni_constraint(makeStaticString("stdClass")), TCell);
  EXPECT_EQ(from_hni_constraint(makeStaticString("?stdClass")), TCell);
  EXPECT_EQ(from_hni_constraint(makeStaticString("fooooo")), TCell);
  EXPECT_EQ(from_hni_constraint(makeStaticString("")), TCell);
}

TEST(Type, DictPacked1) {
  auto const a1 = dict_packed({ival(2), TSStr, TInt});
  auto const a2 = dict_packed({TInt,    TStr,  TInitCell});
  auto const s1 = sdict_packed({ival(2), TSStr, TInt});
  auto const s2 = sdict_packed({TInt,    TSStr,  TInitUnc});

  for (auto& a : { a1, s1, a2, s2 }) {
    EXPECT_TRUE(a.subtypeOf(BDict));
    EXPECT_TRUE(a.subtypeOf(a));
    EXPECT_EQ(a, a);
  }

  // Subtype stuff.

  EXPECT_TRUE(a1.subtypeOf(BDict));
  EXPECT_FALSE(a1.subtypeOf(BSDict));

  EXPECT_TRUE(s1.subtypeOf(BDict));
  EXPECT_TRUE(s1.subtypeOf(BSDict));

  EXPECT_TRUE(a1.subtypeOf(a2));
  EXPECT_TRUE(s1.subtypeOf(s2));
  EXPECT_TRUE(s1.subtypeOf(a1));

  // Could be stuff.

  EXPECT_TRUE(s1.couldBe(a1));
  EXPECT_TRUE(s2.couldBe(a2));

  EXPECT_TRUE(a1.couldBe(a2));
  EXPECT_TRUE(a2.couldBe(a1));
  EXPECT_TRUE(s1.couldBe(a2));
  EXPECT_TRUE(s2.couldBe(a1));

  EXPECT_TRUE(s1.couldBe(s2));
  EXPECT_TRUE(s2.couldBe(s1));
}

TEST(Type, OptDictPacked1) {
  auto const a1 = opt(dict_packed({ival(2), TSStr, TInt}));
  auto const a2 = opt(dict_packed({TInt,    TStr,  TInitCell}));
  auto const s1 = opt(sdict_packed({ival(2), TSStr, TInt}));
  auto const s2 = opt(sdict_packed({TInt,    TSStr,  TInitUnc}));

  for (auto& a : { a1, s1, a2, s2 }) {
    EXPECT_TRUE(a.subtypeOf(BOptDict));
    EXPECT_TRUE(a.subtypeOf(a));
    EXPECT_EQ(a, a);
  }

  // Subtype stuff.

  EXPECT_TRUE(a1.subtypeOf(BOptDict));
  EXPECT_FALSE(a1.subtypeOf(BOptSDict));

  EXPECT_TRUE(s1.subtypeOf(BOptDict));
  EXPECT_TRUE(s1.subtypeOf(BOptSDict));

  EXPECT_TRUE(a1.subtypeOf(a2));
  EXPECT_TRUE(s1.subtypeOf(s2));
  EXPECT_TRUE(s1.subtypeOf(a1));

  // Could be stuff.

  EXPECT_TRUE(s1.couldBe(a1));
  EXPECT_TRUE(s2.couldBe(a2));

  EXPECT_TRUE(a1.couldBe(a2));
  EXPECT_TRUE(a2.couldBe(a1));
  EXPECT_TRUE(s1.couldBe(a2));
  EXPECT_TRUE(s2.couldBe(a1));

  EXPECT_TRUE(s1.couldBe(s2));
  EXPECT_TRUE(s2.couldBe(s1));
}

TEST(Type, DictPacked2) {
  {
    auto const a1 = dict_packed({TInt, TInt, TDbl});
    auto const a2 = dict_packed({TInt, TInt});
    EXPECT_FALSE(a1.subtypeOf(a2));
    EXPECT_FALSE(a1.couldBe(a2));
  }

  {
    auto const a1 = dict_packed({TInitCell, TInt});
    auto const a2 = dict_packed({TInt, TInt});
    EXPECT_TRUE(a1.couldBe(a2));
    EXPECT_TRUE(a2.subtypeOf(a1));
  }

  auto const packedDict = static_dict(0, 42, 1, 23, 2, 12);

  {
    auto const a1 = dict_packed({TInt, TInt, TInt});
    auto const s1 = sdict_packed({TInt, TInt, TInt});
    auto const s2 = dict_val(packedDict);
    EXPECT_TRUE(s2.subtypeOf(a1));
    EXPECT_TRUE(s2.subtypeOf(s1));
    EXPECT_TRUE(s2.couldBe(a1));
    EXPECT_TRUE(s2.couldBe(s1));
  }

  {
    auto const s1 = sdict_packed({ival(42), ival(23), ival(12)});
    auto const s2 = dict_val(packedDict);
    auto const s3 = sdict_packed({TInt});
    auto const a4 = sdict_packed({TInt});
    auto const a5 = dict_packed({ival(42), ival(23), ival(12)});
    EXPECT_TRUE(s2.subtypeOf(s1));
    EXPECT_NE(s1, s2);
    EXPECT_FALSE(s2.subtypeOf(s3));
    EXPECT_FALSE(s2.couldBe(s3));
    EXPECT_FALSE(s2.subtypeOf(s3));
    EXPECT_FALSE(s2.couldBe(s3));
    EXPECT_TRUE(s2.couldBe(s1));
    EXPECT_TRUE(s2.couldBe(a5));
    EXPECT_TRUE(s2.subtypeOf(a5));
    EXPECT_FALSE(a5.subtypeOf(s2));
  }
}

TEST(Type, DictPackedUnion) {
  {
    auto const a1 = dict_packed({TInt, TDbl});
    auto const a2 = dict_packed({TDbl, TInt});
    EXPECT_EQ(union_of(a1, a2), dict_packed({TNum, TNum}));
  }

  {
    auto const s1 = sdict_packed({TInt, TDbl});
    auto const s2 = sdict_packed({TDbl, TInt});
    EXPECT_EQ(union_of(s1, s1), s1);
    EXPECT_EQ(union_of(s1, s2), sdict_packed({TNum, TNum}));
  }

  {
    auto const s1 = sdict_packed({TInt});
    auto const s2 = sdict_packed({TDbl, TDbl});
    EXPECT_EQ(union_of(s1, s2), sdict_packedn(TNum));
  }

  auto const packedDict1 = static_dict(0, 42, 1, 23, 2, 12);
  auto const packedDict2 = static_dict(0, 42, 1, 23.0, 2, 12);

  {
    auto const s1 = dict_val(packedDict1);
    auto const s2 = sdict_packed({TInt, TInt, TInt});
    auto const s3 = sdict_packed({TInt, TNum, TInt});
    EXPECT_EQ(union_of(s1, s2), s2);
    EXPECT_EQ(union_of(s1, s3), s3);
  }

  {
    auto const s1  = sdict_packed({TInt});
    auto const os1 = opt(s1);
    EXPECT_EQ(union_of(s1, TInitNull), os1);
    EXPECT_EQ(union_of(os1, s1), os1);
    EXPECT_EQ(union_of(TInitNull, s1), os1);
    EXPECT_EQ(union_of(os1, os1), os1);
  }

  {
    auto const s1 = sdict_packed({TInt});
    EXPECT_EQ(union_of(s1, TSDict), TSDict);
  }

  {
    auto const s1 = dict_val(packedDict1);
    auto const s2 = dict_val(packedDict2);
    EXPECT_EQ(
      loosen_mark_for_testing(union_of(s1, s2)),
      loosen_mark_for_testing(
        sdict_packed({ival(42), union_of(ival(23), TDbl), ival(12)})
      )
    );
  }
}

TEST(Type, DictPackedN) {
  auto const packedDict = static_dict(0, 42, 1, 23, 2, 12);
  auto const a1 = dict_val(packedDict);
  auto const a2 = sdict_packed({TInt, TInt});
  EXPECT_EQ(union_of(a1, a2), sdict_packedn(TInt));

  auto const s2 = sdict_packed({TInt, TInt});
  EXPECT_TRUE(s2.subtypeOf(sdict_packedn(TInt)));
  EXPECT_FALSE(s2.subtypeOf(sdict_packedn(TDbl)));
  EXPECT_TRUE(s2.subtypeOf(sdict_packedn(TNum)));
  EXPECT_TRUE(s2.subtypeOf(dict_packedn(TInt)));
  EXPECT_TRUE(s2.subtypeOf(opt(dict_packedn(TInt))));

  EXPECT_TRUE(s2.couldBe(dict_packedn(TInt)));
  EXPECT_TRUE(s2.couldBe(dict_packedn(TInitCell)));

  auto const sn1 = sdict_packedn(TInt);
  auto const sn2 = sdict_packedn(TInitNull);

  EXPECT_EQ(union_of(sn1, sn2), sdict_packedn(TOptInt));
  EXPECT_EQ(union_of(sn1, TInitNull), opt(sn1));
  EXPECT_EQ(union_of(TInitNull, sn1), opt(sn1));
  EXPECT_FALSE(sn2.couldBe(sn1));
  EXPECT_FALSE(sn1.couldBe(sn2));

  auto const sn3 = sdict_packedn(TInitUnc);
  EXPECT_TRUE(sn1.couldBe(sn3));
  EXPECT_TRUE(sn2.couldBe(sn3));
  EXPECT_TRUE(sn3.couldBe(sn1));
  EXPECT_TRUE(sn3.couldBe(sn2));

  EXPECT_TRUE(s2.couldBe(sn3));
  EXPECT_TRUE(s2.couldBe(sn1));
  EXPECT_FALSE(s2.couldBe(sn2));
}

TEST(Type, DictStruct) {
  auto const test_map_a          = MapElems{map_elem(s_test, ival(2))};
  auto const test_map_b          = MapElems{map_elem(s_test, TInt)};
  auto const test_map_c          = MapElems{map_elem(s_test, ival(2)),
                                            map_elem(s_A, TInt),
                                            map_elem(s_B, TDbl)};

  auto const ta = dict_map(test_map_a);
  auto const tb = dict_map(test_map_b);
  auto const tc = dict_map(test_map_c);

  EXPECT_FALSE(ta.subtypeOf(tc));
  EXPECT_FALSE(tc.subtypeOf(ta));
  EXPECT_TRUE(ta.subtypeOf(tb));
  EXPECT_FALSE(tb.subtypeOf(ta));
  EXPECT_TRUE(ta.couldBe(tb));
  EXPECT_TRUE(tb.couldBe(ta));
  EXPECT_FALSE(tc.couldBe(ta));
  EXPECT_FALSE(tc.couldBe(tb));

  EXPECT_TRUE(ta.subtypeOf(BDict));
  EXPECT_TRUE(tb.subtypeOf(BDict));
  EXPECT_TRUE(tc.subtypeOf(BDict));

  auto const sa = sdict_map(test_map_a);
  auto const sb = sdict_map(test_map_b);
  auto const sc = sdict_map(test_map_c);

  EXPECT_FALSE(sa.subtypeOf(sc));
  EXPECT_FALSE(sc.subtypeOf(sa));
  EXPECT_TRUE(sa.subtypeOf(sb));
  EXPECT_FALSE(sb.subtypeOf(sa));
  EXPECT_TRUE(sa.couldBe(sb));
  EXPECT_TRUE(sb.couldBe(sa));
  EXPECT_FALSE(sc.couldBe(sa));
  EXPECT_FALSE(sc.couldBe(sb));

  EXPECT_TRUE(sa.subtypeOf(BSDict));
  EXPECT_TRUE(sb.subtypeOf(BSDict));
  EXPECT_TRUE(sc.subtypeOf(BSDict));

  auto const testDict = static_dict(s_A.get(), s_B.get(), s_test.get(), 12);

  auto const test_map_d    = MapElems{map_elem(s_A, sval(s_B)), map_elem(s_test, ival(12))};
  auto const sd = sdict_map(test_map_d);
  EXPECT_TRUE(dict_val(testDict).subtypeOf(sd));

  auto const test_map_e    = MapElems{map_elem(s_A, TSStr), map_elem(s_test, TNum)};
  auto const se = sdict_map(test_map_e);
  EXPECT_TRUE(dict_val(testDict).subtypeOf(se));
  EXPECT_TRUE(se.couldBe(dict_val(testDict)));
}

TEST(Type, DictMapN) {
  auto const test_map =
    dict_val(static_dict(s_A.get(), s_B.get(), s_test.get(), 12));
  EXPECT_TRUE(test_map != dict_n(TSStr, TInitUnc));
  EXPECT_TRUE(test_map.subtypeOf(dict_n(TSStr, TInitUnc)));
  EXPECT_TRUE(test_map.subtypeOf(sdict_n(TSStr, TInitUnc)));
  EXPECT_TRUE(sdict_packedn({TInt}).subtypeOf(dict_n(TInt, TInt)));
  EXPECT_TRUE(sdict_packed({TInt}).subtypeOf(dict_n(TInt, TInt)));

  auto const test_map_a    = MapElems{map_elem(s_test, ival(2))};
  auto const tstruct       = sdict_map(test_map_a);

  EXPECT_TRUE(tstruct.subtypeOf(dict_n(TSStr, ival(2))));
  EXPECT_TRUE(tstruct.subtypeOf(dict_n(TSStr, TInt)));
  EXPECT_TRUE(tstruct.subtypeOf(sdict_n(TSStr, TInt)));
  EXPECT_TRUE(tstruct.subtypeOf(dict_n(TStr, TInt)));

  EXPECT_TRUE(test_map.couldBe(dict_n(TSStr, TInitCell)));
  EXPECT_FALSE(test_map.couldBe(dict_n(TSStr, TStr)));
  EXPECT_FALSE(test_map.couldBe(dict_n(TSStr, TObj)));

  EXPECT_FALSE(test_map.couldBe(dict_val(staticEmptyDictArray())));
  EXPECT_FALSE(dict_n(TSStr, TInt).couldBe(dict_val(staticEmptyDictArray())));

  EXPECT_TRUE(sdict_packedn(TInt).couldBe(sdict_n(TInt, TInt)));
  EXPECT_FALSE(sdict_packedn(TInt).couldBe(dict_n(TInt, TObj)));

  EXPECT_TRUE(tstruct.couldBe(sdict_n(TSStr, TInt)));
  EXPECT_FALSE(tstruct.couldBe(dict_n(TSStr, TObj)));
}

TEST(Type, DictEquivalentRepresentations) {
  {
    auto const simple = dict_val(static_dict(0, 42, 1, 23, 2, 12));
    auto const bulky  = sdict_packed({ival(42), ival(23), ival(12)});
    EXPECT_NE(simple, bulky);
    EXPECT_TRUE(simple.subtypeOf(bulky));
  }

  {
    auto const simple =
      dict_val(static_dict(s_A.get(), s_B.get(), s_test.get(), 12));

    auto const map    = MapElems{map_elem(s_A, sval(s_B)), map_elem(s_test, ival(12))};
    auto const bulky  = sdict_map(map);

    EXPECT_NE(simple, bulky);
    EXPECT_TRUE(simple.subtypeOf(bulky));
  }
}

TEST(Type, DictUnions) {
  auto const test_map_a    = MapElems{map_elem(s_test, ival(2))};
  auto const tstruct       = sdict_map(test_map_a);

  auto const test_map_b    = MapElems{map_elem(s_test, TInt)};
  auto const tstruct2      = sdict_map(test_map_b);

  auto const test_map_c    = MapElems{map_elem(s_A, TInt)};
  auto const tstruct3      = sdict_map(test_map_c);

  auto const test_map_d    = MapElems{map_elem(s_A, TInt), map_elem(s_test, TDbl)};
  auto const tstruct4      = sdict_map(test_map_d);

  auto const packed_int = dict_packedn(TInt);

  EXPECT_EQ(union_of(tstruct, packed_int),
            dict_n(union_of(sval(s_test), TInt), TInt));
  EXPECT_EQ(union_of(tstruct, tstruct2), tstruct2);
  EXPECT_EQ(union_of(tstruct, tstruct3), sdict_n(TSStr, TInt));
  EXPECT_EQ(union_of(tstruct, tstruct4), sdict_n(TSStr, TNum));

  EXPECT_EQ(union_of(sdict_packed({TInt, TDbl, TDbl}), sdict_packedn(TDbl)),
            sdict_packedn(TNum));
  EXPECT_EQ(union_of(sdict_packed({TInt, TDbl}), tstruct),
            sdict_n(union_of(sval(s_test), TInt), TNum));

  EXPECT_EQ(union_of(dict_n(TInt, TTrue), dict_n(TStr, TFalse)),
            dict_n(TArrKey, TBool));

  auto const dict_val1 = dict_val(static_dict(0, 42, 1, 23, 2, 12));
  auto const dict_val2 = dict_val(static_dict(0, 1, 1, 2, 2, 3, 3, 4, 4, 5));
  EXPECT_EQ(
    loosen_mark_for_testing(union_of(dict_val1, dict_val2)),
    loosen_mark_for_testing(sdict_packedn(TInt))
  );
}

TEST(Type, DictIntersections) {
  auto const test_map_a    = MapElems{map_elem(s_test, ival(2))};
  auto const tstruct       = sdict_map(test_map_a);

  auto const test_map_b    = MapElems{map_elem(s_test, TInt)};
  auto const tstruct2      = sdict_map(test_map_b);

  auto const test_map_c    = MapElems{map_elem(s_A, TInt)};
  auto const tstruct3      = sdict_map(test_map_c);

  auto const test_map_d    = MapElems{map_elem(s_A, TInt), map_elem(s_test, TDbl)};
  auto const tstruct4      = sdict_map(test_map_d);

  auto const test_map_e    = MapElems{map_elem(s_A, TInt), map_elem(s_B, TDbl)};
  auto const tstruct5      = sdict_map(test_map_e);

  auto const test_map_f    = MapElems{map_elem(s_A, TUncArrKey), map_elem(s_B, TInt)};
  auto const tstruct6      = sdict_map(test_map_f);

  auto const test_map_g    = MapElems{map_elem(s_A, TSStr), map_elem(s_B, TUncArrKey)};
  auto const tstruct7      = sdict_map(test_map_g);

  auto const test_map_h    = MapElems{map_elem(s_A, TSStr), map_elem(s_B, TInt)};
  auto const tstruct8      = sdict_map(test_map_h);

  auto const test_map_i    = MapElems{map_elem(s_A, TStr), map_elem(s_B, TInt), map_elem(s_BB, TVec)};
  auto const tstruct9      = dict_map(test_map_i);

  auto const test_map_j    = MapElems{map_elem(s_A, TSStr), map_elem(s_B, TInt), map_elem(s_BB, TSVec)};
  auto const tstruct10     = sdict_map(test_map_j);

  auto const test_map_k    = MapElems{map_elem(s_A, TSStr), map_elem(s_B, TInt), map_elem(s_BB, TObj)};
  auto const tstruct11     = dict_map(test_map_k);

  auto const mapn_str_int = dict_n(TStr, TInt);

  EXPECT_EQ(intersection_of(tstruct,  mapn_str_int), tstruct);
  EXPECT_EQ(intersection_of(tstruct2, mapn_str_int), tstruct2);
  EXPECT_EQ(intersection_of(tstruct3, mapn_str_int), tstruct3);
  EXPECT_EQ(intersection_of(tstruct4, mapn_str_int), TBottom);
  EXPECT_EQ(intersection_of(tstruct,  tstruct2),     tstruct);
  EXPECT_EQ(intersection_of(tstruct,  tstruct3),     TBottom);
  EXPECT_EQ(intersection_of(tstruct4, tstruct5),     TBottom);
  EXPECT_EQ(intersection_of(tstruct6, tstruct7),     tstruct8);
  EXPECT_EQ(intersection_of(tstruct8, tstruct),      TBottom);

  EXPECT_EQ(intersection_of(sdict_packed({TNum, TDbl, TNum}),
                            sdict_packedn(TDbl)),
            sdict_packed({TDbl, TDbl, TDbl}));
  EXPECT_EQ(intersection_of(sdict_packed({TNum, TDbl, TNum}),
                            sdict_packed({TDbl, TNum, TInt})),
            sdict_packed({TDbl, TDbl, TInt}));
  EXPECT_EQ(intersection_of(TSDictN, dict_n(TArrKey, TInitCell)),
            sdict_n(TUncArrKey, TInitUnc));
  EXPECT_EQ(intersection_of(TSDictN, dict_n(TArrKey, TInitCell)),
            sdict_n(TUncArrKey, TInitUnc));
  EXPECT_EQ(intersection_of(tstruct9, TSDictN), tstruct10);
  EXPECT_EQ(intersection_of(TSDictN, dict_packed({TStr, TVec, TInt, TInitCell})),
                            sdict_packed({TSStr, TSVec, TInt, TInitUnc}));
  EXPECT_EQ(intersection_of(dict_packedn(TStr), TSDictN), sdict_packedn(TSStr));
  EXPECT_EQ(intersection_of(TSDictN, dict_packedn(TObj)), TBottom);
  EXPECT_EQ(intersection_of(dict_packed({TStr, TInt, TObj}), TSDictN), TBottom);
  EXPECT_EQ(intersection_of(TSDictN, tstruct11), TBottom);
  EXPECT_EQ(
    intersection_of(dict_n(TArrKey, TObj), TSDictN),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(union_of(dict_n(TInt, TObj), TDictE),
                    union_of(dict_packed({TInt, TObj}), TDictE)),
    TDictE
  );
  EXPECT_EQ(
    intersection_of(opt(dict_n(TInt, TObj)), TUnc),
    TInitNull
  );
  EXPECT_EQ(
    intersection_of(opt(dict_packedn(TObj)), TInitUnc),
    TInitNull
  );
  EXPECT_EQ(
    intersection_of(union_of(dict_packed({TInt, TObj}), TDictE), TUnc),
    TSDictE
  );
  EXPECT_EQ(
    intersection_of(opt(union_of(dict_packed({TInt, TObj}), TDictE)), TUnc),
    opt(TSDictE)
  );
}

TEST(Type, DictOfDict) {
  auto const t1 = dict_n(TSStr, dict_n(TInt, TSStr));
  auto const t2 = dict_n(TSStr, TDict);
  auto const t3 = dict_n(TSStr, dict_packedn(TSStr));
  auto const t4 = dict_n(TSStr, dict_n(TSStr, TSStr));
  EXPECT_TRUE(t1.subtypeOf(t2));
  EXPECT_TRUE(t1.couldBe(t3));
  EXPECT_FALSE(t1.subtypeOf(t3));
  EXPECT_TRUE(t3.subtypeOf(t1));
  EXPECT_TRUE(t3.subtypeOf(t2));
  EXPECT_FALSE(t1.couldBe(t4));
  EXPECT_FALSE(t4.couldBe(t3));
  EXPECT_TRUE(t4.subtypeOf(t2));
}

TEST(Type, WideningAlreadyStable) {
  // A widening union on types that are already stable should not move
  // the type anywhere.
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    EXPECT_EQ(widening_union(t, t), t);
  }

  auto deepPacked = svec({TInt});
  auto deepPackedN = svec_n(TInt);
  auto deepMap = sdict_map({map_elem(s_A, TInt)});
  auto deepMapN = sdict_n(TInt, TInt);
  for (size_t i = 0; i < 10; ++i) {
    deepPacked = widening_union(deepPacked, svec({deepPacked}));
    deepPackedN = widening_union(deepPackedN, svec_n(deepPackedN));
    deepMap = widening_union(deepMap, sdict_map({map_elem(s_A, deepMap)}, TSStr, deepMap));
    deepMapN = widening_union(deepMapN, sdict_n(TInt, deepMapN));
  }
  EXPECT_EQ(deepPacked, widening_union(deepPacked, svec({deepPacked})));
  EXPECT_EQ(deepPackedN, widening_union(deepPackedN, svec_n(deepPackedN)));
  EXPECT_EQ(deepMap, widening_union(deepMap, sdict_map({map_elem(s_A, deepMap)}, TSStr, deepMap)));
  EXPECT_EQ(deepMapN, widening_union(deepMapN, sdict_n(TInt, deepMapN)));
}

TEST(Type, EmptyDict) {
  {
    auto const possible_e = union_of(dict_packedn(TInt), dict_empty());
    EXPECT_TRUE(possible_e.couldBe(dict_empty()));
    EXPECT_TRUE(possible_e.couldBe(dict_packedn(TInt)));
    EXPECT_EQ(array_like_elem(possible_e, ival(0)).first, TInt);
  }

  {
    auto const possible_e = union_of(dict_packed({TInt, TInt}), dict_empty());
    EXPECT_TRUE(possible_e.couldBe(dict_empty()));
    EXPECT_TRUE(possible_e.couldBe(dict_packed({TInt, TInt})));
    EXPECT_FALSE(possible_e.couldBe(dict_packed({TInt, TInt, TInt})));
    EXPECT_FALSE(possible_e.subtypeOf(dict_packedn(TInt)));
    EXPECT_EQ(array_like_elem(possible_e, ival(0)).first, TInt);
    EXPECT_EQ(array_like_elem(possible_e, ival(1)).first, TInt);
  }

  {
    auto const estat = union_of(sdict_packedn(TInt), dict_empty());
    EXPECT_TRUE(estat.couldBe(dict_empty()));
    EXPECT_TRUE(estat.couldBe(sdict_packedn(TInt)));
    EXPECT_FALSE(estat.subtypeOf(sdict_packedn(TInt)));
    EXPECT_FALSE(estat.subtypeOf(BSDictE));
    EXPECT_TRUE(estat.couldBe(BSDictE));
  }

  EXPECT_EQ(
    loosen_mark_for_testing(array_like_newelem(dict_empty(), ival(142)).first),
    loosen_mark_for_testing(dict_packed({ival(142)}))
  );
}

TEST(Type, ArrKey) {
  EXPECT_TRUE(TInt.subtypeOf(BArrKey));
  EXPECT_TRUE(TStr.subtypeOf(BArrKey));
  EXPECT_TRUE(ival(0).subtypeOf(BArrKey));
  EXPECT_TRUE(sval(s_test).subtypeOf(BArrKey));
  EXPECT_TRUE(sval_nonstatic(s_test).subtypeOf(BArrKey));

  EXPECT_TRUE(TInt.subtypeOf(BUncArrKey));
  EXPECT_TRUE(TSStr.subtypeOf(BUncArrKey));
  EXPECT_TRUE(ival(0).subtypeOf(BUncArrKey));
  EXPECT_TRUE(sval(s_test).subtypeOf(BUncArrKey));

  EXPECT_TRUE(TArrKey.subtypeOf(BInitCell));
  EXPECT_TRUE(TUncArrKey.subtypeOf(BInitCell));
  EXPECT_TRUE(TOptArrKey.subtypeOf(BInitCell));
  EXPECT_TRUE(TOptUncArrKey.subtypeOf(BInitCell));

  EXPECT_TRUE(TUncArrKey.subtypeOf(BInitUnc));
  EXPECT_TRUE(TOptUncArrKey.subtypeOf(BInitUnc));

  EXPECT_EQ(union_of(TInt, TStr), TArrKey);
  EXPECT_EQ(union_of(TInt, TSStr), TUncArrKey);
  EXPECT_EQ(union_of(TArrKey, TInitNull), TOptArrKey);
  EXPECT_EQ(union_of(TUncArrKey, TInitNull), TOptUncArrKey);

  EXPECT_EQ(opt(TArrKey), TOptArrKey);
  EXPECT_EQ(opt(TUncArrKey), TOptUncArrKey);
  EXPECT_EQ(unopt(TOptArrKey), TArrKey);
  EXPECT_EQ(unopt(TOptUncArrKey), TUncArrKey);
}

TEST(Type, LoosenStaticness) {
  auto index = make_index();

  auto const& all = allCases(index);

  for (auto const& t : all) {
    if (!t.couldBe(BStr | BArrLike)) {
      EXPECT_EQ(loosen_staticness(t), t);
    }

    if (!t.subtypeOf(BCell)) continue;
    auto const [obj, objRest] = split_obj(t);
    auto const [str, strRest] = split_string(objRest);
    auto const [arr, rest] = split_array_like(strRest);
    EXPECT_EQ(loosen_staticness(rest), rest);
    EXPECT_EQ(
      loosen_staticness(t),
      union_of(
        loosen_staticness(obj),
        loosen_staticness(str),
        loosen_staticness(arr),
        rest
      )
    );
    if (!t.is(BBottom)) {
      EXPECT_FALSE(loosen_staticness(t).subtypeOf(BSStr | BSArrLike));
      EXPECT_FALSE(loosen_staticness(t).subtypeOf(BCStr | BCArrLike));
    }
  }

  auto const test = [&] (const Type& a, const Type& b) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(opt(a))),
              loosen_mark_for_testing(opt(b)));
    if (a.strictSubtypeOf(BInitCell)) {
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(wait_handle(a))),
                loosen_mark_for_testing(wait_handle(b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_packedn(a))),
                loosen_mark_for_testing(dict_packedn(b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_packed({a}))),
                loosen_mark_for_testing(dict_packed({b})));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_n(TSStr, a))),
                loosen_mark_for_testing(dict_n(TStr, b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_map({map_elem(s_A, a)}, TInt, a))),
                loosen_mark_for_testing(dict_map({map_elem_nonstatic(s_A, b)}, TInt, b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_map({map_elem(s_A, a)}, TSStr, a))),
                loosen_mark_for_testing(dict_map({map_elem_nonstatic(s_A, b)}, TStr, b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(dict_map({map_elem_counted(s_A, a)}, TSStr, a))),
                loosen_mark_for_testing(dict_map({map_elem_nonstatic(s_A, b)}, TStr, b)));
    }
    if (a.strictSubtypeOf(BUnc)) {
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_packedn(a))),
                loosen_mark_for_testing(dict_packedn(b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_packedn(a))),
                loosen_mark_for_testing(dict_packedn(b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_packed({a}))),
                loosen_mark_for_testing(dict_packed({b})));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_n(TSStr, a))),
                loosen_mark_for_testing(dict_n(TStr, b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_map({map_elem(s_A, a)}, TInt, a))),
                loosen_mark_for_testing(dict_map({map_elem_nonstatic(s_A, b)}, TInt, b)));
      EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(sdict_map({map_elem(s_A, a)}, TSStr, a))),
                loosen_mark_for_testing(dict_map({map_elem_nonstatic(s_A, b)}, TStr, b)));
    }
  };

  for (auto const& t : all) {
    if (t.is(BBottom) || !t.subtypeOf(BInitCell)) continue;
    test(t, loosen_staticness(t));
  }

  auto const test_map1 = MapElems{map_elem(s_A, TInt)};
  auto const test_map2 = MapElems{map_elem_nonstatic(s_A, TInt)};
  auto const test_map3 = MapElems{map_elem_counted(s_A, TInt)};
  std::vector<std::pair<Type, Type>> tests = {
    { TSStr, TStr },
    { TSVecE, TVecE },
    { TSVecN, TVecN },
    { TSVec, TVec },
    { TSDictE, TDictE },
    { TSDictN, TDictN },
    { TSDict, TDict },
    { TSVecE, TVecE },
    { TSVecN, TVecN },
    { TSVec, TVec },
    { TSDictE, TDictE },
    { TSDictN, TDictN },
    { TSDict, TDict },
    { TSKeysetE, TKeysetE },
    { TSKeysetN, TKeysetN },
    { TSKeyset, TKeyset },
    { TUncArrKey, TArrKey },
    { TUnc,
      Type{BInitNull|BArrLike|BArrKey|BBool|BCls|BDbl|BFunc|BLazyCls|BEnumClassLabel|BClsMeth|BUninit} },
    { TInitUnc,
      Type{BInitNull|BArrLike|BArrKey|BBool|BCls|BDbl|BFunc|BLazyCls|BEnumClassLabel|BClsMeth} },
    { ival(123), ival(123) },
    { sval(s_test), sval_nonstatic(s_test) },
    { sdict_packedn(TInt), dict_packedn(TInt) },
    { sdict_packed({TInt, TBool}), dict_packed({TInt, TBool}) },
    { sdict_n(TSStr, TInt), dict_n(TStr, TInt) },
    { sdict_n(TInt, TSDictN), dict_n(TInt, TDictN) },
    { sdict_map(test_map1), dict_map(test_map2) },
    { dict_map(test_map3), dict_map(test_map2) },
    { TClsMeth, TClsMeth },
    { TObj, TObj },
    { TRes, TRes },
    { TInitCell, TInitCell },
    { vec_n(Type{BInitCell & ~BCStr}), TVecN },
    { dict_n(TArrKey, Type{BInitCell & ~BCStr}), TDictN },
    { dict_n(Type{BInt|BSStr}, TInitCell), TDictN },
    { wait_handle(Type{BInitCell & ~BCStr}), wait_handle(TInitCell) },
    { vec_val(static_vec(s_A.get(), 123, s_B.get(), 456)),
      vec({sval_nonstatic(s_A), ival(123), sval_nonstatic(s_B), ival(456)}) },
    { sdict_map({map_elem(s_A, TSStr)}, TSStr, TSStr), dict_map({map_elem_nonstatic(s_A, TStr)}, TStr, TStr) },
    { dict_val(static_dict(s_A.get(), s_A.get(), s_B.get(), s_B.get())),
      dict_map({map_elem_nonstatic(s_A, sval_nonstatic(s_A)), map_elem_nonstatic(s_B, sval_nonstatic(s_B))}) },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_staticness(p.first)),
              loosen_mark_for_testing(p.second));
    test(p.first, p.second);
  }
}

TEST(Type, LoosenStringStaticness) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.couldBe(BStr)) {
      EXPECT_EQ(loosen_string_staticness(t), t);
    } else {
      EXPECT_FALSE(loosen_string_staticness(t).subtypeAmong(BSStr, BStr));
      EXPECT_FALSE(loosen_string_staticness(t).subtypeAmong(BCStr, BStr));
    }

    if (!t.subtypeOf(BCell)) continue;
    auto const [str, rest] = split_string(t);
    EXPECT_EQ(loosen_string_staticness(rest), rest);
    EXPECT_EQ(
      loosen_string_staticness(t),
      union_of(loosen_string_staticness(str), rest)
    );
  }

  const std::vector<std::pair<Type, Type>> tests = {
    { TSStr, TStr },
    { TCStr, TStr },
    { TStr, TStr },
    { sval(s_A), sval_nonstatic(s_A) },
    { sval_counted(s_A), sval_nonstatic(s_A) },
    { sval_nonstatic(s_A), sval_nonstatic(s_A) },
    { TUncArrKey, TArrKey },
    { TArrKey, TArrKey },
    { union_of(TCStr,TInt), TArrKey },
    { TInt, TInt },
    { TObj, TObj },
    { TSArrLike, TSArrLike },
    { TCArrLike, TCArrLike },
    { TCell, TCell },
    { ival(1), ival(1) },
    { union_of(sval(s_A),TInt), union_of(sval_nonstatic(s_A),TInt) },
    { union_of(sval_counted(s_A),TInt), union_of(sval_nonstatic(s_A),TInt) },
    { union_of(sval_nonstatic(s_A),TInt), union_of(sval_nonstatic(s_A),TInt) },
    { union_of(ival(1),TSStr), union_of(ival(1),TStr) },
    { union_of(ival(1),TCStr), union_of(ival(1),TStr) },
    { union_of(ival(1),TStr), union_of(ival(1),TStr) },
    { TInitUnc, Type{(BInitUnc & ~BSStr) | BStr} },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_string_staticness(p.first), p.second);
  }
}

TEST(Type, LoosenArrayStaticness) {
  auto index = make_index();

  auto const& all = allCases(index);

  for (auto const& t : all) {
    if (!t.couldBe(BArrLike)) {
      EXPECT_EQ(loosen_array_staticness(t), t);
    } else {
      EXPECT_FALSE(loosen_array_staticness(t).subtypeAmong(BSArrLike, BArrLike));
      EXPECT_FALSE(loosen_array_staticness(t).subtypeAmong(BCArrLike, BArrLike));
    }

    if (!t.subtypeOf(BCell)) continue;
    auto const [arr, rest] = split_array_like(t);
    EXPECT_EQ(loosen_array_staticness(rest), rest);
    EXPECT_EQ(
      loosen_array_staticness(t),
      union_of(loosen_array_staticness(arr), rest)
    );
  }

  for (auto const& t : all) {
    if (t.is(BBottom) || !t.subtypeOf(BInitCell)) continue;

    EXPECT_EQ(loosen_array_staticness(opt(t)), opt(loosen_array_staticness(t)));

    if (t.strictSubtypeOf(BInitCell)) {
      EXPECT_EQ(loosen_array_staticness(wait_handle(t)), wait_handle(t));
      EXPECT_EQ(loosen_array_staticness(dict_packedn(t)), dict_packedn(t));
      EXPECT_EQ(loosen_array_staticness(dict_packed({t})), dict_packed({t}));
      EXPECT_EQ(loosen_array_staticness(dict_n(TSStr, t)), dict_n(TSStr, t));
      EXPECT_EQ(loosen_array_staticness(dict_map({map_elem(s_A, t)}, TSStr, t)),
                dict_map({map_elem(s_A, t)}, TSStr, t));
      EXPECT_EQ(loosen_array_staticness(dict_map({map_elem_counted(s_A, t)}, TSStr, t)),
                dict_map({map_elem_counted(s_A, t)}, TSStr, t));
    }
    if (t.strictSubtypeOf(BUnc)) {
      EXPECT_EQ(loosen_array_staticness(sdict_packedn(t)), dict_packedn(t));
      EXPECT_EQ(loosen_array_staticness(sdict_packed({t})), dict_packed({t}));
      EXPECT_EQ(loosen_array_staticness(sdict_n(TSStr, t)), dict_n(TSStr, t));
      EXPECT_EQ(loosen_array_staticness(sdict_map({map_elem(s_A, t)}, TSStr, t)),
                dict_map({map_elem(s_A, t)}, TSStr, t));
    }
  }

  auto const test_map1 = MapElems{map_elem(s_A, TInt)};
  auto const test_map2 = MapElems{map_elem_nonstatic(s_A, TInt)};
  auto const test_map3 = MapElems{map_elem_counted(s_A, TInt)};
  std::vector<std::pair<Type, Type>> tests = {
    { TSStr, TSStr },
    { TCStr, TCStr},
    { TSVecE, TVecE },
    { TSVecN, vec_n(TInitUnc) },
    { TSVec, union_of(vec_n(TInitUnc), TVecE) },
    { TSDictE, TDictE },
    { TSDictN, dict_n(TUncArrKey, TInitUnc) },
    { TSDict, union_of(dict_n(TUncArrKey, TInitUnc), TDictE) },
    { TSKeysetE, TKeysetE },
    { TSKeysetN, keyset_n(TUncArrKey) },
    { TSKeyset, union_of(keyset_n(TUncArrKey), TKeysetE) },
    { TSArrLike, make_arrmapn_for_testing(BArrLike, TUncArrKey, TInitUnc) },
    { TCArrLike, TArrLike },
    { TUncArrKey, TUncArrKey },
    { Type{BSVec|BInt}, Type{BVec|BInt} },
    { TUnc, Type{(BUnc & ~BSArrLike) | BArrLike} },
    { TInitUnc, Type{(BInitUnc & ~BSArrLike) | BArrLike} },
    { ival(123), ival(123) },
    { sval(s_test), sval(s_test) },
    { sdict_packedn(TInt), dict_packedn(TInt) },
    { sdict_packed({TInt, TBool}), dict_packed({TInt, TBool}) },
    { sdict_n(TSStr, TInt), dict_n(TSStr, TInt) },
    { sdict_n(TInt, TSDictN), dict_n(TInt, TSDictN) },
    { sdict_map(test_map1), dict_map(test_map1) },
    { dict_map(test_map2), dict_map(test_map2) },
    { dict_map(test_map3), dict_map(test_map3) },
    { TClsMeth, TClsMeth },
    { TObj, TObj },
    { TRes, TRes },
    { TInitCell, TInitCell },
    { vec_n(Type{BInitCell & ~BCArrLike}), vec_n(Type{BInitCell & ~BCArrLike}) },
    { dict_n(TArrKey, Type{BInitCell & ~BCArrLike}), dict_n(TArrKey, Type{BInitCell & ~BCArrLike}) },
    { wait_handle(Type{BInitCell & ~BCArrLike}), wait_handle(Type{BInitCell & ~BCArrLike}) },
    { vec_val(static_vec(s_A.get(), 123, s_B.get(), 456)),
      vec({sval(s_A), ival(123), sval(s_B), ival(456)}) },
    { sdict_map({map_elem(s_A, TSStr)}, TSStr, TSStr), dict_map({map_elem(s_A, TSStr)}, TSStr, TSStr) },
    { dict_val(static_dict(s_A.get(), s_A.get(), s_B.get(), s_B.get())),
      dict_map({map_elem(s_A, sval(s_A)), map_elem(s_B, sval(s_B))}) },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_array_staticness(p.first)),
              loosen_mark_for_testing(p.second));
  }
}

TEST(Type, Emptiness) {
  auto index = make_index();

  std::vector<std::pair<Type, Emptiness>> tests{
    { TInitNull, Emptiness::Empty },
    { TUninit, Emptiness::Empty },
    { TFalse, Emptiness::Empty },
    { TVecE, Emptiness::Empty },
    { TSKeysetE, Emptiness::Empty },
    { TDictE, Emptiness::Empty },
    { TDict, Emptiness::Maybe },
    { TTrue, Emptiness::NonEmpty },
    { TVecN, Emptiness::NonEmpty },
    { TDictN, Emptiness::NonEmpty },
    { TArrLikeN, Emptiness::NonEmpty },
    { TArrLike, Emptiness::Maybe },
    { TObj, Emptiness::Maybe },
    { wait_handle(TInt), Emptiness::NonEmpty },
    { ival(0), Emptiness::Empty },
    { ival(1), Emptiness::NonEmpty },
    { opt(ival(0)), Emptiness::Empty },
    { opt(ival(1)), Emptiness::Maybe },
    { sempty(), Emptiness::Empty },
    { sval(s_A), Emptiness::NonEmpty },
    { lazyclsval(s_A), Emptiness::NonEmpty },
    { dval(3.14), Emptiness::NonEmpty },
    { dval(0), Emptiness::Empty },
    { TInitCell, Emptiness::Maybe },
    { TInt, Emptiness::Maybe },
    { TStr, Emptiness::Maybe },
    { TLazyCls, Emptiness::NonEmpty },
    { TCls, Emptiness::NonEmpty },
    { TEnumClassLabel, Emptiness::NonEmpty },
    { TDbl, Emptiness::Maybe }
  };
  for (auto const& p : tests) {
    EXPECT_EQ(emptiness(p.first).first, p.second);
  }
}

TEST(Type, AssertNonEmptiness) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.subtypeOf(BCell)) continue;

    switch (emptiness(t).first) {
      case Emptiness::Empty:
        EXPECT_EQ(assert_nonemptiness(t), TBottom);
        break;
      case Emptiness::Maybe:
        EXPECT_NE(emptiness(assert_nonemptiness(t)).first, Emptiness::Empty);
        break;
      case Emptiness::NonEmpty:
        EXPECT_EQ(assert_nonemptiness(t), t);
        break;
    }

    if (!is_specialized_int(t) &&
        !is_specialized_double(t) &&
        !is_specialized_string(t) &&
        !t.couldBe(BNull | BFalse | BArrLikeE)) {
      EXPECT_EQ(assert_nonemptiness(t), t);
    }
    EXPECT_FALSE(assert_nonemptiness(t).couldBe(BNull | BFalse | BArrLikeE));
  }

  std::vector<std::pair<Type, Type>> tests{
    { TInitNull, TBottom },
    { TUninit, TBottom },
    { TFalse, TBottom },
    { TTrue, TTrue },
    { TBool, TTrue },
    { TVecE, TBottom },
    { TVec, TVecN },
    { TVecN, TVecN },
    { TDictE, TBottom },
    { TDictN, TDictN },
    { TDict, TDictN },
    { TArrLikeE, TBottom },
    { TArrLikeN, TArrLikeN },
    { TArrLike, TArrLikeN },
    { TObj, TObj },
    { Type{BInt|BFalse}, TInt },
    { wait_handle(TInt), wait_handle(TInt) },
    { ival(0), TBottom },
    { ival(1), ival(1) },
    { sempty(), TBottom },
    { sval(s_A), sval(s_A) },
    { lazyclsval(s_A), lazyclsval(s_A) },
    { dval(3.14), dval(3.14) },
    { dval(0), TBottom },
    { opt(ival(0)), TBottom },
    { opt(ival(1)), ival(1) },
    { TInitCell, Type{BInitCell & ~(BNull | BFalse | BArrLikeE)} },
    { TInt, TInt },
    { TStr, TStr },
    { TLazyCls, TLazyCls },
    { TEnumClassLabel, TEnumClassLabel },
    { TDbl, TDbl },
    { union_of(ival(1),TStr), union_of(ival(1),TStr) },
    { union_of(ival(0),TStr), TArrKey },
    { union_of(ival(0),TDictE), TBottom }
  };
  for (auto const& p : tests) {
    EXPECT_EQ(assert_nonemptiness(p.first), p.second);
  }
}

TEST(Type, AssertEmptiness) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.subtypeOf(BCell)) continue;

    switch (emptiness(t).first) {
      case Emptiness::Empty:
        EXPECT_EQ(assert_emptiness(t), t);
        break;
      case Emptiness::Maybe:
        EXPECT_NE(emptiness(assert_emptiness(t)).first, Emptiness::NonEmpty);
        break;
      case Emptiness::NonEmpty:
        EXPECT_EQ(assert_emptiness(t), TBottom);
        break;
    }

    EXPECT_EQ(t.couldBe(BInitNull), assert_emptiness(t).couldBe(BInitNull));
    EXPECT_FALSE(assert_emptiness(t).couldBe(BTrue | BArrLikeN));
  }

  std::vector<std::pair<Type, Type>> tests{
    { TInitNull, TInitNull },
    { TUninit, TUninit },
    { TFalse, TFalse },
    { TTrue, TBottom },
    { TBool, TFalse },
    { TVecE, TVecE },
    { TVec, TVecE },
    { TVecN, TBottom },
    { TDictE, TDictE },
    { TDictN, TBottom },
    { TDict, TDictE },
    { TArrLikeE, TArrLikeE },
    { TArrLikeN, TBottom },
    { TArrLike, TArrLikeE },
    { TObj, TObj },
    { Type{BInt|BFalse}, union_of(ival(0),TFalse) },
    { Type{BInt|BTrue}, ival(0) },
    { Type{BInt|BBool}, union_of(ival(0),TFalse) },
    { wait_handle(TInt), TBottom },
    { ival(0), ival(0) },
    { ival(1), TBottom },
    { sempty(), sempty() },
    { sempty_nonstatic(), sempty_nonstatic() },
    { sval(s_A), TBottom },
    { lazyclsval(s_A), TBottom },
    { dval(3.14), TBottom },
    { dval(0), dval(0) },
    { opt(ival(0)), opt(ival(0)) },
    { opt(ival(1)), TInitNull },
    { TInt, ival(0) },
    { TStr, TStr },
    { TSStr, TSStr },
    { TLazyCls, TBottom },
    { TEnumClassLabel, TBottom },
    { TDbl, dval(0) },
    { union_of(ival(1),TStr), TArrKey },
    { union_of(ival(0),TStr), union_of(TInt,sempty_nonstatic()) },
    { union_of(ival(0),TDictE), union_of(ival(0),TDictE) },
    { union_of(ival(0),TDictN), ival(0) },
    { dict_n(TArrKey, TInt), TBottom },
    { union_of(dict_n(TArrKey, TInt),TDictE), TDictE }
  };
  for (auto const& p : tests) {
    EXPECT_EQ(assert_emptiness(p.first), p.second);
  }
}

TEST(Type, LoosenEmptiness) {
  auto index = make_index();

  auto const clsA = index.resolve_class(s_A.get());

  auto const& all = allCases(index);

  for (auto const& t : all) {
    if (!t.couldBe(BArrLike)) {
      EXPECT_EQ(loosen_emptiness(t), t);
    } else {
      EXPECT_FALSE(loosen_emptiness(t).subtypeAmong(BArrLikeE, BArrLike));
      EXPECT_FALSE(loosen_emptiness(t).subtypeAmong(BArrLikeN, BArrLike));
    }

    if (!t.subtypeOf(BCell)) continue;
    auto const [arr, rest] = split_array_like(t);
    EXPECT_EQ(
      loosen_emptiness(t),
      union_of(loosen_emptiness(arr), loosen_emptiness(rest))
    );
  }

  auto const test_map    = MapElems{map_elem(s_A, TInt)};
  std::vector<std::pair<Type, Type>> tests = {
    { TSVecE, TSVec },
    { TSVecN, TSVec },
    { TVecE, TVec },
    { TVecN, TVec },
    { TSDictE, TSDict },
    { TSDictN, TSDict },
    { TDictE, TDict },
    { TDictN, TDict },
    { TSKeysetE, TSKeyset },
    { TSKeysetN, TSKeyset },
    { TKeysetE, TKeyset },
    { TKeysetN, TKeyset },
    { TSVecE, TSVec },
    { TSVecN, TSVec },
    { TVecE, TVec },
    { TVecN, TVec },
    { TSDictE, TSDict },
    { TSDictN, TSDict },
    { TDictE, TDict },
    { TDictN, TDict },
    { dict_packedn(TInt), union_of(TDictE, dict_packedn(TInt)) },
    { dict_packed({TInt, TBool}), union_of(TDictE, dict_packed({TInt, TBool})) },
    { dict_n(TStr, TInt), union_of(TDictE, dict_n(TStr, TInt)) },
    { dict_map(test_map), union_of(TDictE, dict_map(test_map)) },
    { TSArrLikeE, TSArrLike },
    { TSArrLikeN, TSArrLike },
    { TArrLikeE, TArrLike },
    { TArrLikeN, TArrLike },
    { union_of(make_specialized_exact_object(BObj, *clsA), TSDictE),
      union_of(TObj, TSDict) },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_emptiness(p.first)),
              loosen_mark_for_testing(p.second));
    EXPECT_EQ(loosen_mark_for_testing(loosen_emptiness(opt(p.first))),
              loosen_mark_for_testing(opt(p.second)));
  }
}

TEST(Type, LoosenValues) {
  auto index = make_index();

  auto const& all = allCases(index);

  for (auto const& t : all) {
    if (t.couldBe(BBool)) {
      EXPECT_FALSE(loosen_values(t).subtypeAmong(BFalse, BBool));
      EXPECT_FALSE(loosen_values(t).subtypeAmong(BTrue, BBool));
      EXPECT_EQ(get_bits(t) & ~BBool, get_bits(loosen_values(t)) & ~BBool);
    } else {
      EXPECT_EQ(get_bits(t), get_bits(loosen_values(t)));
    }

    if (is_specialized_string(t) ||
        is_specialized_int(t) ||
        is_specialized_double(t) ||
        is_specialized_lazycls(t) ||
        is_specialized_ecl(t) ||
        is_specialized_array_like(t)) {
      EXPECT_FALSE(loosen_values(t).hasData());
    } else if (!t.couldBe(BBool)) {
      EXPECT_EQ(loosen_values(t), t);
    }

    EXPECT_TRUE(t.subtypeOf(loosen_values(t)));
    EXPECT_FALSE(loosen_values(t).strictSubtypeOf(t));
  }

  EXPECT_TRUE(loosen_values(TTrue) == TBool);
  EXPECT_TRUE(loosen_values(TFalse) == TBool);
  EXPECT_TRUE(loosen_values(TOptTrue) == TOptBool);
  EXPECT_TRUE(loosen_values(TOptFalse) == TOptBool);

  auto const test_map = MapElems{map_elem(s_A, TInt)};
  std::vector<std::pair<Type, Type>> tests = {
    { ival(123), TInt },
    { dval(3.14), TDbl },
    { sval(s_test), TSStr },
    { sval_nonstatic(s_test), TStr },
    { lazyclsval(s_test), TLazyCls },
    { enumclasslabelval(s_test), TEnumClassLabel },
    { dict_val(static_dict(0, 42, 1, 23, 2, 12)), TSDictN },
    { dict_packedn(TInt), TDictN },
    { dict_packed({TInt, TBool}), TDictN },
    { dict_n(TStr, TInt), TDictN },
    { dict_map(test_map), TDictN },
    { Type{BFalse|BInt}, Type{BBool|BInt} },
    { union_of(ival(123),TTrue), Type{BInt|BBool} },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_values(p.first)), p.second);
    EXPECT_EQ(loosen_mark_for_testing(loosen_values(opt(p.first))), opt(p.second));
  }

  auto const cls = index.resolve_class(s_TestClass.get());
  EXPECT_TRUE(!!cls);

  EXPECT_TRUE(loosen_values(objExact(*cls)) == objExact(*cls));
  EXPECT_TRUE(loosen_values(subObj(*cls)) == subObj(*cls));
  EXPECT_TRUE(loosen_values(clsExact(*cls, true)) == clsExact(*cls, true));
  EXPECT_TRUE(loosen_values(subCls(*cls, true)) == subCls(*cls, true));

  EXPECT_TRUE(loosen_values(opt(objExact(*cls))) == opt(objExact(*cls)));
  EXPECT_TRUE(loosen_values(opt(subObj(*cls))) == opt(subObj(*cls)));
}

TEST(Type, LoosenArrayValues) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    EXPECT_EQ(get_bits(t), get_bits(loosen_array_values(t)));
    if (!is_specialized_array_like(t)) {
      EXPECT_EQ(loosen_array_values(t), t);
    }
    EXPECT_FALSE(is_specialized_array_like(loosen_array_values(t)));
    EXPECT_TRUE(t.subtypeOf(loosen_array_values(t)));
    EXPECT_FALSE(loosen_array_values(t).strictSubtypeOf(t));
  }
}

TEST(Type, LoosenStringValues) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    EXPECT_EQ(get_bits(t), get_bits(loosen_string_values(t)));
    if (!is_specialized_string(t)) {
      EXPECT_EQ(loosen_string_values(t), t);
    }
    EXPECT_FALSE(is_specialized_string(loosen_string_values(t)));
    EXPECT_TRUE(t.subtypeOf(loosen_string_values(t)));
    EXPECT_FALSE(loosen_string_values(t).strictSubtypeOf(t));
  }
}

TEST(Type, AddNonEmptiness) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.couldBe(BArrLikeE)) {
      EXPECT_EQ(add_nonemptiness(t), t);
    } else {
      EXPECT_EQ(get_bits(t) & ~BArrLike,
                get_bits(add_nonemptiness(t)) & ~BArrLike);
      EXPECT_FALSE(add_nonemptiness(t).subtypeAmong(BArrLikeE, BArrLike));
      EXPECT_TRUE(add_nonemptiness(t).couldBe(BArrLikeN));
      EXPECT_TRUE(t.subtypeOf(add_nonemptiness(t)));
    }

    EXPECT_EQ(t.hasData(), add_nonemptiness(t).hasData());

    if (!t.subtypeOf(BCell)) continue;
    auto const [arr, rest] = split_array_like(t);
    EXPECT_EQ(
      add_nonemptiness(t),
      union_of(add_nonemptiness(arr), add_nonemptiness(rest))
    );
  }

  auto const test_map    = MapElems{map_elem(s_A, TInt)};
  std::vector<std::pair<Type, Type>> tests = {
    { TVecE, TVec },
    { TSVecE, TSVec },
    { TVecN, TVecN },
    { TSVecN, TSVecN },
    { TDictE, TDict },
    { TSDictE, TSDict },
    { TDictN, TDictN },
    { TSDictN, TSDictN },
    { TKeysetE, TKeyset },
    { TSKeysetE, TSKeyset },
    { TKeysetN, TKeysetN },
    { TSKeysetN, TSKeysetN },
    { TVecE, TVec },
    { TSVecE, TSVec },
    { TVecN, TVecN },
    { TSVecN, TSVecN },
    { TDictE, TDict },
    { TSDictE, TSDict },
    { TDictN, TDictN },
    { TSDictN, TSDictN },
    { TSArrLikeE, TSArrLike },
    { TArrLikeE, TArrLike },
    { TSArrLikeN, TSArrLikeN },
    { TArrLikeN, TArrLikeN },
    { dict_packedn(TInt), dict_packedn(TInt) },
    { dict_packed({TInt, TBool}), dict_packed({TInt, TBool}) },
    { dict_n(TStr, TInt), dict_n(TStr, TInt) },
    { dict_map(test_map), dict_map(test_map) },
    { vec_val(static_vec(s_A.get(), 123, s_B.get(), 456)),
      vec_val(static_vec(s_A.get(), 123, s_B.get(), 456)) },
    { TInitCell, TInitCell },
    { TObj, TObj },
    { Type{BVecE|BInt}, Type{BVec|BInt} },
    { Type{BVecN|BInt}, Type{BVecN|BInt} },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(add_nonemptiness(p.first), p.second);
    EXPECT_EQ(add_nonemptiness(opt(p.first)), opt(p.second));
  }
}

TEST(Type, LoosenVecOrDict) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.couldBe(BKVish)) {
      EXPECT_EQ(loosen_vec_or_dict(t), t);
    } else {
      EXPECT_EQ(get_bits(t) & ~BArrLike,
                get_bits(loosen_vec_or_dict(t)) & ~BArrLike);
      EXPECT_TRUE(t.subtypeOf(loosen_vec_or_dict(t)));
    }

    if (!t.couldBe(BKeysetN)) {
      EXPECT_FALSE(is_specialized_array_like(loosen_vec_or_dict(t)));
    }

    if (!t.subtypeOf(BCell)) continue;
    auto const [arr, rest] = split_array_like(t);
    EXPECT_EQ(
      loosen_vec_or_dict(t),
      union_of(loosen_vec_or_dict(arr), loosen_vec_or_dict(rest))
    );
  }

  auto const vecOrDict = union_of(TVec, TDict);
  std::vector<std::pair<Type, Type>> tests = {
    { TSVecE, vecOrDict },
    { TSVecN, vecOrDict },
    { TVecE, vecOrDict },
    { TVecN, vecOrDict },
    { TSVec, vecOrDict },
    { TVec, vecOrDict },
    { TSDictE, vecOrDict },
    { TSDictN, vecOrDict },
    { TDictE, vecOrDict },
    { TDictN, vecOrDict },
    { TSDict, vecOrDict },
    { TDict, vecOrDict },
    { TSKeysetE, TSKeysetE },
    { TSKeysetN, TSKeysetN },
    { TKeysetE, TKeysetE },
    { TKeysetN, TKeysetN },
    { TSKeyset, TSKeyset },
    { TKeyset, TKeyset },
    { TSArrLikeE, union_of(vecOrDict, TSKeysetE) },
    { TSArrLikeN, union_of(vecOrDict, TSKeysetN) },
    { TArrLikeE, union_of(vecOrDict, TKeysetE) },
    { TArrLikeN, union_of(vecOrDict, TKeysetN) },
    { TSArrLike, union_of(vecOrDict, TSKeyset) },
    { TArrLike, TArrLike },
    { TInitCell, TInitCell },
    { TObj, TObj },
    { TInt, TInt },
    { ival(123), ival(123) },
    { dict_packedn(TInt), vecOrDict },
    { dict_packed({TInt, TBool}), vecOrDict },
    { dict_n(TStr, TInt), vecOrDict },
    { dict_map({map_elem(s_A, TInt)}), vecOrDict },
    { vec_val(static_vec(s_A.get(), 123, s_B.get(), 456)), vecOrDict },
    { Type{BVecE|BInt}, union_of(vecOrDict, TInt) },
    { Type{BVecN|BInt}, union_of(vecOrDict, TInt) },
 };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_vec_or_dict(p.first), p.second);
    EXPECT_EQ(loosen_vec_or_dict(opt(p.first)), opt(p.second));
  }
}

TEST(Type, Scalarize) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!is_scalar(t)) continue;
    EXPECT_EQ(scalarize(t), from_cell(*tv(t)));
    EXPECT_TRUE(scalarize(t).subtypeOf(BUnc));
    EXPECT_EQ(scalarize(t).hasData(), t.hasData());
    if (!t.hasData() && !t.subtypeOf(BArrLikeE)) {
      EXPECT_EQ(scalarize(t), t);
    }
    if (is_specialized_int(t) || is_specialized_double(t) ||
        is_specialized_lazycls(t) || is_specialized_ecl(t)) {
      EXPECT_EQ(scalarize(t), t);
    }
    if (is_specialized_string(t)) {
      EXPECT_TRUE(scalarize(t).subtypeOf(BSStr));
    }
    if (is_specialized_array_like(t)) {
      EXPECT_TRUE(scalarize(t).subtypeOf(BSArrLikeN));
    }
  }

  std::vector<std::pair<Type, Type>> tests = {
    { TUninit, TUninit },
    { TInitNull, TInitNull },
    { TFalse, TFalse },
    { TTrue, TTrue },
    { TSVecE, TSVecE },
    { TVecE, TSVecE },
    { TSDictE, TSDictE },
    { TDictE, TSDictE },
    { TSKeysetE, TSKeysetE },
    { TKeysetE, TSKeysetE },
    { ival(123), ival(123) },
    { sval(s_A), sval(s_A) },
    { sval_nonstatic(s_A), sval(s_A) },
    { dval(3.14), dval(3.14) },
    { make_specialized_arrval(BSVecN, static_vec(100, 200)),
      make_specialized_arrval(BSVecN, static_vec(100, 200)) },
    { make_specialized_arrval(BSDictN, static_dict(s_A.get(), 100, s_B.get(), 200)),
      make_specialized_arrval(BSDictN, static_dict(s_A.get(), 100, s_B.get(), 200)) },
    { make_specialized_arrpacked(BVecN, {sval_nonstatic(s_A)}),
      make_specialized_arrval(BSVecN, static_vec(s_A.get())) },
    { make_specialized_arrpacked(BDictN, {sval_nonstatic(s_A)}),
      make_specialized_arrval(BSDictN, static_dict(0, s_A.get())) },
    { make_specialized_arrmap(BDictN, {map_elem_nonstatic(s_A, sval_nonstatic(s_B))}),
      make_specialized_arrval(BSDictN, static_dict(s_A.get(), s_B.get())) },
  };
  for (auto const& p : tests) {
    EXPECT_EQ(scalarize(make_unmarked(p.first)), make_unmarked(p.second));
  }
}

TEST(Type, StrValues) {
  auto const t1 = sval(s_test);
  auto const t2 = sval_nonstatic(s_test);
  auto const t3 = sval(s_A);
  auto const t4 = sval_nonstatic(s_test);
  auto const t5 = sval_nonstatic(s_A);

  EXPECT_TRUE(t1.subtypeOf(t2));
  EXPECT_TRUE(t1.subtypeOf(TSStr));
  EXPECT_TRUE(t1.subtypeOf(TStr));
  EXPECT_FALSE(t1.subtypeOf(t3));

  EXPECT_FALSE(t2.subtypeOf(t1));
  EXPECT_FALSE(t2.subtypeOf(TSStr));
  EXPECT_TRUE(t2.subtypeOf(TStr));
  EXPECT_FALSE(t2.subtypeOf(t3));
  EXPECT_TRUE(t2.subtypeOf(t4));
  EXPECT_FALSE(t2.subtypeOf(t5));

  EXPECT_FALSE(TStr.subtypeOf(t1));
  EXPECT_FALSE(TSStr.subtypeOf(t2));
  EXPECT_FALSE(TStr.subtypeOf(t2));
  EXPECT_FALSE(TSStr.subtypeOf(t2));
  EXPECT_FALSE(t2.subtypeOf(t1));
  EXPECT_FALSE(t3.subtypeOf(t2));
  EXPECT_TRUE(t4.subtypeOf(t2));
  EXPECT_FALSE(t5.subtypeOf(t2));

  EXPECT_TRUE(t1.couldBe(t2));
  EXPECT_FALSE(t1.couldBe(t3));
  EXPECT_TRUE(t1.couldBe(TStr));
  EXPECT_TRUE(t1.couldBe(TSStr));

  EXPECT_TRUE(t2.couldBe(t1));
  EXPECT_FALSE(t2.couldBe(t3));
  EXPECT_TRUE(t2.couldBe(t4));
  EXPECT_FALSE(t2.couldBe(t5));
  EXPECT_TRUE(t2.couldBe(TStr));
  EXPECT_TRUE(t2.couldBe(TSStr));

  EXPECT_TRUE(TSStr.couldBe(t1));
  EXPECT_TRUE(TStr.couldBe(t1));
  EXPECT_TRUE(TSStr.couldBe(t2));
  EXPECT_TRUE(TStr.couldBe(t2));
  EXPECT_FALSE(t3.couldBe(t1));
  EXPECT_FALSE(t3.couldBe(t2));
  EXPECT_TRUE(t4.couldBe(t2));
  EXPECT_FALSE(t5.couldBe(t2));

  EXPECT_EQ(union_of(t1, t1), t1);
  EXPECT_EQ(union_of(t2, t2), t2);
  EXPECT_EQ(union_of(t1, t2), t2);
  EXPECT_EQ(union_of(t2, t1), t2);
  EXPECT_EQ(union_of(t1, t3), TSStr);
  EXPECT_EQ(union_of(t3, t1), TSStr);
  EXPECT_EQ(union_of(t2, t3), TStr);
  EXPECT_EQ(union_of(t3, t2), TStr);
  EXPECT_EQ(union_of(t2, t4), t2);
  EXPECT_EQ(union_of(t4, t2), t2);
  EXPECT_EQ(union_of(t2, t5), TStr);
  EXPECT_EQ(union_of(t5, t2), TStr);
}

TEST(Type, DictMapOptValues) {
  auto const test_map_a = MapElems{map_elem(s_A, TInt), map_elem(s_B, TDbl)};
  auto const test_map_b = MapElems{map_elem(s_A, TInt)};
  auto const test_map_c = MapElems{map_elem(s_A, TInt), map_elem(s_test, TInt)};
  auto const test_map_d = MapElems{map_elem(s_test, TInt), map_elem(s_A, TInt)};
  auto const test_map_e = MapElems{map_elem(s_A, TInt), map_elem(s_B, TObj)};
  auto const test_map_f = MapElems{map_elem(10, TInt), map_elem(11, TDbl)};
  auto const test_map_g = MapElems{map_elem(s_A, TArrKey)};
  auto const test_map_h = MapElems{map_elem(s_A, TInt), map_elem(s_B, TStr)};
  auto const test_map_i = MapElems{map_elem(s_A, TInt), map_elem(s_B, TDbl), map_elem(s_test, TStr)};
  auto const test_map_j = MapElems{map_elem(s_A, TInt), map_elem(s_B, TDbl), map_elem(s_test, TObj)};

  EXPECT_EQ(dict_map(test_map_a, TInt, TSStr), dict_map(test_map_a, TInt, TSStr));
  EXPECT_NE(dict_map(test_map_a, TInt, TSStr), dict_map(test_map_a, TInt, TStr));

  EXPECT_FALSE(
    dict_map(test_map_c, TSStr, TInt).subtypeOf(dict_map(test_map_d, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_map(test_map_e, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_b, TSStr, TInt).subtypeOf(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_map(test_map_b, TSStr, TInt))
  );
  EXPECT_TRUE(
    sdict_map(test_map_a, TSStr, TInt).subtypeOf(sdict_map(test_map_b, TSStr, TNum))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_map(test_map_b, TInt, TNum))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_map(test_map_a, TStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TStr, TInt).subtypeOf(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TNum).subtypeOf(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_n(TStr, TNum))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).subtypeOf(dict_n(TStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_f, TSStr, TInt).subtypeOf(dict_n(TInt, TNum))
  );
  EXPECT_FALSE(dict_map(test_map_a).subtypeOf(dict_n(TInt, TNum)));
  EXPECT_FALSE(
    dict_n(TSStr, TInt).subtypeOf(dict_map(test_map_a, TSStr, TInt))
  );

  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_a, TSStr, TNum))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TNum).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TArrKey, TInt).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_a, TArrKey, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_a, TInt, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TInt, TInt).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a, TSStr, TDbl).couldBe(dict_map(test_map_a, TSStr, TObj))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_c, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_c, TSStr, TInt).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_a, TSStr, TInt).couldBe(dict_map(test_map_e, TSStr, TInt))
  );
  EXPECT_FALSE(
    dict_map(test_map_e, TSStr, TInt).couldBe(dict_map(test_map_a, TSStr, TInt))
  );
  EXPECT_TRUE(
    dict_map(test_map_a).couldBe(dict_map(test_map_b, TSStr, TDbl))
  );
  EXPECT_TRUE(
    dict_map(test_map_b, TSStr, TDbl).couldBe(dict_map(test_map_a))
  );
  EXPECT_FALSE(
    dict_map(test_map_a).couldBe(dict_map(test_map_b, TSStr, TObj))
  );
  EXPECT_FALSE(
    dict_map(test_map_b, TSStr, TObj).couldBe(dict_map(test_map_a))
  );

  EXPECT_EQ(
    union_of(sdict_map(test_map_a), sdict_map(test_map_b)),
    sdict_map(test_map_b, sval(s_B), TDbl)
  );
  EXPECT_EQ(
    union_of(sdict_map(test_map_a), sdict_map(test_map_c)),
    sdict_map(test_map_b, TSStr, TNum)
  );
  EXPECT_EQ(
    union_of(dict_map(test_map_a, TInt, TStr), dict_map(test_map_a, TStr, TInt)),
    dict_map(test_map_a, TArrKey, TArrKey)
  );
  EXPECT_EQ(
    union_of(dict_map(test_map_c), dict_map(test_map_d)),
    dict_n(TSStr, TInt)
  );
  EXPECT_EQ(
    union_of(dict_map(test_map_c, TInt, TInt), dict_map(test_map_d, TInt, TInt)),
    dict_n(TUncArrKey, TInt)
  );
  EXPECT_EQ(
    union_of(
      dict_map(test_map_c, TSStr, TDbl),
      dict_map(test_map_d, TSStr, TDbl)
    ),
    dict_n(TSStr, TNum)
  );
  EXPECT_EQ(
    union_of(dict_map(test_map_c, TSStr, TDbl), dict_packed({TInt})),
    dict_n(union_of(ival(0),TSStr), TNum)
  );
  EXPECT_EQ(
    union_of(sdict_map(test_map_c, TSStr, TDbl), sdict_packedn(TInt)),
    sdict_n(TUncArrKey, TNum)
  );
  EXPECT_EQ(
    union_of(dict_map(test_map_c, TInt, TDbl), dict_n(TSStr, TInt)),
    dict_n(TUncArrKey, TNum)
  );

  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TSStr, TInt),
      dict_map(test_map_a, TSStr, TInt)
    ),
    dict_map(test_map_a, TSStr, TInt)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TSStr, TArrKey),
      dict_map(test_map_a, TSStr, TInt)
    ),
    dict_map(test_map_a, TSStr, TInt)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TSStr, TInt),
      dict_map(test_map_a, TArrKey, TInt)
    ),
    dict_map(test_map_a, TSStr, TInt)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TSStr, TInt),
      dict_map(test_map_a, TInt, TInt)
    ),
    dict_map(test_map_a)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TInt, TStr),
      dict_map(test_map_a, TInt, TInt)
    ),
    dict_map(test_map_a)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TInt, TInt),
      dict_map(test_map_e, TInt, TInt)
    ),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_a), dict_map(test_map_b, TSStr, TNum)),
    dict_map(test_map_a)
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_b, TSStr, TNum), dict_map(test_map_a)),
    dict_map(test_map_a)
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_a), dict_map(test_map_b, TSStr, TObj)),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_b, TSStr, TObj), dict_map(test_map_a)),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_a, TSStr, TObj), dict_n(TSStr, TObj)),
    TBottom
  );
  EXPECT_EQ(
    intersection_of(dict_map(test_map_a, TSStr, TObj), dict_n(TSStr, TNum)),
    dict_map(test_map_a)
  );
  EXPECT_EQ(
    intersection_of(
      dict_map(test_map_a, TSStr, TInitCell),
      dict_n(TSStr, TNum)
    ),
    dict_map(test_map_a, TSStr, TNum)
  );

  EXPECT_EQ(
    array_like_set(dict_map(test_map_b), TSStr, TStr).first,
    dict_map(test_map_g, TSStr, TStr)
  );
  EXPECT_EQ(
    array_like_set(dict_map(test_map_a), sval(s_B), TStr).first,
    dict_map(test_map_h)
  );
  EXPECT_EQ(
    array_like_set(dict_map(test_map_a), sval(s_test), TStr).first,
    dict_map(test_map_i)
  );
  EXPECT_EQ(
    array_like_set(dict_map(test_map_a, TSStr, TInt), sval(s_test), TStr).first,
    dict_map(test_map_a, TSStr, TArrKey)
  );
  EXPECT_EQ(
    array_like_set(
      dict_map(test_map_a, sval(s_test), TInt),
      sval(s_test),
      TObj
    ).first,
    dict_map(test_map_j)
  );
}

TEST(Type, ContextDependent) {
  // This only covers basic cases involving objects.  More testing should
  // be added for non object types, and nested types.
  auto idx = make_index();

  // load classes in hierarchy  Base -> B -> BB
  auto const clsBase = idx.resolve_class(s_Base.get());
  if (!clsBase) ADD_FAILURE();
  auto const clsB = idx.resolve_class(s_B.get());
  if (!clsB) ADD_FAILURE();
  auto const clsBB = idx.resolve_class(s_BB.get());
  if (!clsBB) ADD_FAILURE();
  // Unrelated class.
  auto const clsUn = idx.resolve_class(s_TestClass.get());
  if (!clsUn) ADD_FAILURE();

  auto const objExactBaseTy     = objExact(*clsBase);
  auto const thisObjExactBaseTy = setctx(objExact(*clsBase));
  auto const subObjBaseTy       = subObj(*clsBase);
  auto const thisSubObjBaseTy   = setctx(subObj(*clsBase));

  auto const objExactBTy        = objExact(*clsB);
  auto const thisObjExactBTy    = setctx(objExact(*clsB));
  auto const subObjBTy          = subObj(*clsB);
  auto const thisSubObjBTy      = setctx(subObj(*clsB));
  auto const clsExactBTy        = clsExact(*clsB, true);
  auto const thisClsExactBTy    = setctx(clsExact(*clsB, true));
  auto const subClsBTy          = subCls(*clsB, true);
  auto const thisSubClsBTy      = setctx(subCls(*clsB, true));

  auto const objExactBBTy       = objExact(*clsBB);
  auto const thisObjExactBBTy   = setctx(objExact(*clsBB));
  auto const subObjBBTy         = subObj(*clsBB);
  auto const thisSubObjBBTy     = setctx(subObj(*clsBB));
  auto const clsExactBBTy       = clsExact(*clsBB, true);
  auto const thisClsExactBBTy   = setctx(clsExact(*clsBB, true));
  auto const subClsBBTy         = subCls(*clsBB, true);
  auto const thisSubClsBBTy     = setctx(subCls(*clsBB, true));

  auto const objExactUnTy       = objExact(*clsUn);
  auto const thisObjExactUnTy   = setctx(objExact(*clsUn));
  auto const subObjUnTy         = subObj(*clsUn);
  auto const thisSubObjUnTy     = setctx(subObj(*clsUn));

#define REFINE_EQ(A, B) \
  EXPECT_TRUE((A).equal((B)))
#define REFINE_NEQ(A, B) \
  EXPECT_FALSE((A).equal((B)))

  // check that improving any non context dependent type does not change the
  // type whether or not the context is related.
  REFINE_EQ(return_with_context(objExactBaseTy, objExactBTy),
            objExactBaseTy);
  REFINE_EQ(return_with_context(subObjBaseTy, objExactBTy),
            subObjBaseTy);
  REFINE_EQ(return_with_context(objExactBTy, objExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(subObjBTy, objExactBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(objExactBBTy, objExactBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(subObjBBTy, objExactBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(objExactUnTy, objExactBTy),
            objExactUnTy);
  REFINE_EQ(return_with_context(subObjUnTy, objExactBTy),
            subObjUnTy);
  REFINE_EQ(return_with_context(objExactBaseTy, clsExactBTy),
            objExactBaseTy);
  REFINE_EQ(return_with_context(subObjBaseTy, clsExactBTy),
            subObjBaseTy);
  REFINE_EQ(return_with_context(objExactBTy, clsExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(subObjBTy, clsExactBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(objExactBBTy, clsExactBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(subObjBBTy, clsExactBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(objExactUnTy, clsExactBTy),
            objExactUnTy);
  REFINE_EQ(return_with_context(subObjUnTy, clsExactBTy),
            subObjUnTy);

  // With sub.
  REFINE_EQ(return_with_context(objExactBaseTy, subObjBTy),
            objExactBaseTy);
  REFINE_EQ(return_with_context(subObjBaseTy, subObjBTy),
            subObjBaseTy);
  REFINE_EQ(return_with_context(objExactBTy, subObjBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(subObjBTy, subObjBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(objExactBBTy, subObjBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(subObjBBTy, subObjBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(objExactUnTy, subObjBTy),
            objExactUnTy);
  REFINE_EQ(return_with_context(subObjUnTy, subObjBTy),
            subObjUnTy);
  REFINE_EQ(return_with_context(objExactBaseTy, subClsBTy),
            objExactBaseTy);
  REFINE_EQ(return_with_context(subObjBaseTy, subClsBTy),
            subObjBaseTy);
  REFINE_EQ(return_with_context(objExactBTy, subClsBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(subObjBTy, subClsBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(objExactBBTy, subClsBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(subObjBBTy, subClsBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(objExactUnTy, subClsBTy),
            objExactUnTy);
  REFINE_EQ(return_with_context(subObjUnTy, subClsBTy),
            subObjUnTy);

  // Improvements (exact)
  REFINE_EQ(return_with_context(thisObjExactBaseTy, objExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, objExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, objExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, objExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, objExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBBTy, objExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactUnTy, objExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, objExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, clsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, clsExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, clsExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, clsExactBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, clsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBBTy, clsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactUnTy, clsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, clsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, thisObjExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, thisObjExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, thisObjExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, thisObjExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, thisObjExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBBTy, thisObjExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactUnTy, thisObjExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, thisObjExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, thisClsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, thisClsExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, thisClsExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, thisClsExactBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, thisClsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBBTy, thisClsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactUnTy, thisClsExactBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, thisClsExactBTy),
            TBottom);

  // Improvements (sub)
  REFINE_EQ(return_with_context(thisObjExactBaseTy, subObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, subObjBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, subObjBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, subObjBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, subObjBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(thisSubObjBBTy, subObjBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(thisObjExactUnTy, subObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, subObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, subClsBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, subClsBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, subClsBTy),
            objExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, subClsBTy),
            subObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, subClsBTy),
            objExactBBTy);
  REFINE_EQ(return_with_context(thisSubObjBBTy, subClsBTy),
            subObjBBTy);
  REFINE_EQ(return_with_context(thisObjExactUnTy, subClsBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, subClsBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, thisSubObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, thisSubObjBTy),
            thisSubObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, thisSubObjBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, thisSubObjBTy),
            thisSubObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, thisSubObjBTy),
            thisObjExactBBTy);
  REFINE_EQ(return_with_context(thisSubObjBBTy, thisSubObjBTy),
            thisSubObjBBTy);
  REFINE_EQ(return_with_context(thisObjExactUnTy, thisSubObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, thisSubObjBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisObjExactBaseTy, thisSubClsBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjBaseTy, thisSubClsBTy),
            thisSubObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBTy, thisSubClsBTy),
            thisObjExactBTy);
  REFINE_EQ(return_with_context(thisSubObjBTy, thisSubClsBTy),
            thisSubObjBTy);
  REFINE_EQ(return_with_context(thisObjExactBBTy, thisSubClsBTy),
            thisObjExactBBTy);
  REFINE_EQ(return_with_context(thisSubObjBBTy, thisSubClsBTy),
            thisSubObjBBTy);
  REFINE_EQ(return_with_context(thisObjExactUnTy, thisSubClsBTy),
            TBottom);
  REFINE_EQ(return_with_context(thisSubObjUnTy, thisSubClsBTy),
            TBottom);

  // Optional type preservation.
  REFINE_EQ(return_with_context(opt(subObjBaseTy), objExactBTy),
            opt(subObjBaseTy));
  REFINE_EQ(return_with_context(opt(subObjBaseTy), clsExactBTy),
            opt(subObjBaseTy));
  REFINE_EQ(return_with_context(opt(subObjBaseTy), subObjBTy),
            opt(subObjBaseTy));
  REFINE_EQ(return_with_context(opt(subObjBaseTy), subClsBTy),
            opt(subObjBaseTy));
  REFINE_EQ(return_with_context(opt(thisSubObjBaseTy), objExactBTy),
            opt(objExactBTy));
  REFINE_EQ(return_with_context(opt(thisSubObjBaseTy), clsExactBTy),
            opt(objExactBTy));
  REFINE_EQ(return_with_context(opt(thisSubObjBaseTy), thisObjExactBTy),
            opt(thisObjExactBTy));
  REFINE_EQ(return_with_context(opt(thisSubObjBaseTy), thisClsExactBTy),
            opt(thisObjExactBTy));


  // Refinedness operators.
  REFINE_EQ(objExactBTy, objExactBTy);
  REFINE_EQ(subObjBTy, subObjBTy);
  REFINE_EQ(clsExactBTy, clsExactBTy);
  REFINE_EQ(subClsBTy, subClsBTy);
  REFINE_EQ(thisObjExactBTy, thisObjExactBTy);
  REFINE_EQ(thisSubObjBTy, thisSubObjBTy);
  REFINE_EQ(thisClsExactBTy, thisClsExactBTy);
  REFINE_EQ(thisSubClsBTy, thisSubClsBTy);

  REFINE_NEQ(objExactBTy, thisObjExactBTy);
  REFINE_NEQ(subObjBTy, thisSubObjBTy);
  REFINE_NEQ(clsExactBTy, thisClsExactBTy);
  REFINE_NEQ(subClsBTy, thisSubClsBTy);
  REFINE_NEQ(thisObjExactBTy, objExactBTy);
  REFINE_NEQ(thisSubObjBTy, subObjBTy);
  REFINE_NEQ(thisClsExactBTy, clsExactBTy);
  REFINE_NEQ(thisSubClsBTy, subClsBTy);

  EXPECT_FALSE(objExactBTy.moreRefined(thisObjExactBTy));
  EXPECT_FALSE(subObjBTy.moreRefined(thisSubObjBTy));
  EXPECT_FALSE(clsExactBTy.moreRefined(thisClsExactBTy));
  EXPECT_FALSE(subClsBTy.moreRefined(thisSubClsBTy));

  EXPECT_TRUE(thisObjExactBTy.moreRefined(objExactBTy));
  EXPECT_TRUE(thisSubObjBTy.moreRefined(subObjBTy));
  EXPECT_TRUE(thisClsExactBTy.moreRefined(clsExactBTy));
  EXPECT_TRUE(thisSubClsBTy.moreRefined(subClsBTy));

  EXPECT_TRUE(thisObjExactBTy.moreRefined(thisObjExactBTy));
  EXPECT_TRUE(thisSubObjBTy.moreRefined(thisSubObjBTy));
  EXPECT_TRUE(thisClsExactBTy.moreRefined(thisClsExactBTy));
  EXPECT_TRUE(thisSubClsBTy.moreRefined(thisSubClsBTy));

  EXPECT_FALSE(thisObjExactBTy.strictlyMoreRefined(thisObjExactBTy));
  EXPECT_FALSE(thisSubObjBTy.strictlyMoreRefined(thisSubObjBTy));
  EXPECT_FALSE(thisClsExactBTy.strictlyMoreRefined(thisClsExactBTy));
  EXPECT_FALSE(thisSubClsBTy.strictlyMoreRefined(thisSubClsBTy));

  EXPECT_FALSE(thisObjExactBBTy.strictlyMoreRefined(thisObjExactBTy));
  EXPECT_TRUE(thisSubObjBBTy.strictlyMoreRefined(thisSubObjBTy));
  EXPECT_FALSE(thisClsExactBBTy.strictlyMoreRefined(thisClsExactBTy));
  EXPECT_TRUE(thisSubClsBBTy.strictlyMoreRefined(thisSubClsBTy));

  EXPECT_FALSE(thisObjExactBTy.strictlyMoreRefined(thisObjExactBBTy));
  EXPECT_FALSE(thisSubObjBTy.strictlyMoreRefined(thisSubObjBBTy));
  EXPECT_FALSE(thisClsExactBTy.strictlyMoreRefined(thisClsExactBBTy));
  EXPECT_FALSE(thisSubClsBTy.strictlyMoreRefined(thisSubClsBBTy));

  EXPECT_FALSE(objExactBBTy.strictlyMoreRefined(thisObjExactBTy));
  EXPECT_FALSE(subObjBBTy.strictlyMoreRefined(thisSubObjBTy));
  EXPECT_FALSE(clsExactBBTy.strictlyMoreRefined(thisClsExactBTy));
  EXPECT_FALSE(subClsBBTy.strictlyMoreRefined(thisSubClsBTy));

  // Normal equality should still hold.
  EXPECT_EQ(objExactBTy, thisObjExactBTy);
  EXPECT_EQ(subObjBTy, thisSubObjBTy);
  EXPECT_EQ(clsExactBTy, thisClsExactBTy);
  EXPECT_EQ(subClsBTy, thisSubClsBTy);
  EXPECT_EQ(thisObjExactBTy, objExactBTy);
  EXPECT_EQ(thisSubObjBTy, subObjBTy);
  EXPECT_EQ(thisClsExactBTy, clsExactBTy);
  EXPECT_EQ(thisSubClsBTy, subClsBTy);

  auto const& types = allCases(idx);
  auto const test = [&] (const Type& context) {
    for (auto const& t: types) {
      if (!t.subtypeOf(BInitCell)) continue;
      auto const [obj, objRest] = split_obj(t);
      auto const [cls, clsRest] = split_cls(objRest);
      REFINE_EQ(
        return_with_context(t, context),
        union_of(
          return_with_context(obj, context),
          return_with_context(cls, context),
          clsRest
        )
      );
    }
  };
  test(objExactBTy);
  test(clsExactBTy);
  test(subObjBTy);
  test(subClsBTy);
  test(thisObjExactBTy);
  test(thisClsExactBTy);
  test(thisSubObjBTy);
  test(thisSubClsBTy);

#undef REFINE_NEQ
#undef REFINE_EQ
}

TEST(Type, ArrLike) {
  const std::initializer_list<std::pair<Type, Type>> subtype_true{
    // Expect all static arrays to be subtypes
    { TSKeyset, TArrLike },
    { TSDict,   TArrLike },
    { TSVec,    TArrLike },
    // Expect other arrays to be subtypes
    { TKeyset,  TArrLike },
    { TDict,    TArrLike },
    { TVec,     TArrLike },
  };

  const std::initializer_list<std::pair<Type, Type>> subtype_false{
    // ClsMeth is not an array
    { TClsMeth, TArrLike },
    // Ints are not arrays
    { TInt,     TArrLike },
    // ArrLike doesn't contain null
    { TOptVec,  TArrLike },
  };

  const std::initializer_list<std::pair<Type, Type>> couldbe_true{
    { TArrLike, TOptKeysetE },
  };

  const std::initializer_list<std::pair<Type, Type>> couldbe_false{
    { TArrLike, TPrim },
    { TArrLike, TNull },
  };

  for (auto kv : subtype_true) {
    EXPECT_TRUE(kv.first.subtypeOf(kv.second))
      << show(kv.first) << " subtypeOf " << show(kv.second);
  }

  for (auto kv : subtype_false) {
    EXPECT_FALSE(kv.first.subtypeOf(kv.second))
      << show(kv.first) << " !subtypeOf " << show(kv.second);
  }

  for (auto kv : couldbe_true) {
    EXPECT_TRUE(kv.first.couldBe(kv.second))
      << show(kv.first) << " couldbe " << show(kv.second);
    EXPECT_TRUE(kv.second.couldBe(kv.first))
      << show(kv.first) << " couldbe " << show(kv.second);
  }

  for (auto kv : couldbe_false) {
    EXPECT_FALSE(kv.first.couldBe(kv.second))
      << show(kv.first) << " !couldbe " << show(kv.second);
    EXPECT_FALSE(kv.second.couldBe(kv.first))
      << show(kv.first) << " !couldbe " << show(kv.second);
  }
}

TEST(Type, LoosenLikeness) {
  auto index = make_index();

  auto const& all = allCases(index);
  for (auto const& t : all) {
    if (!t.couldBe(BCls | BLazyCls | BClsMeth)) {
      EXPECT_EQ(loosen_likeness(t), t);
    } else {
      auto u = BBottom;
      if (t.couldBe(BCls | BLazyCls)) u |= BSStr;
      EXPECT_EQ(loosen_likeness(t), union_of(t, Type{u}));
    }
  }

  std::vector<std::pair<Type, Type>> tests{
    { TClsMeth, Type{BClsMeth} },
    { TCls, Type{BCls|BSStr} },
    { TLazyCls, Type{BLazyCls|BSStr} },
    { lazyclsval(s_A), Type{BLazyCls|BSStr} },
    { TInt, TInt },
    { Type{BInt|BCls}, Type{BCls|BSStr|BInt} },
    { Type{BInt|BLazyCls}, Type{BLazyCls|BSStr|BInt} }
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_likeness(p.first), p.second);
  }
}

TEST(Type, LoosenLikenessRecursively) {
  auto index = make_index();

  auto const test = [&] (const Type& t) {
    if (!t.subtypeOf(BInitCell)) return;

    if (!t.is(BBottom)) {
      EXPECT_EQ(loosen_likeness_recursively(opt(t)),
                opt(loosen_likeness_recursively(t)));
      EXPECT_EQ(
        loosen_likeness_recursively(wait_handle(t)),
        wait_handle(loosen_likeness_recursively(t)));
      EXPECT_EQ(
        loosen_likeness_recursively(vec_n(t)),
        vec_n(loosen_likeness_recursively(t)));
      EXPECT_EQ(
        loosen_likeness_recursively(vec({t})),
        vec({loosen_likeness_recursively(t)}));
      EXPECT_EQ(
        loosen_likeness_recursively(dict_n(TArrKey, t)),
        dict_n(TArrKey, loosen_likeness_recursively(t)));
      EXPECT_EQ(
        loosen_likeness_recursively(dict_map({map_elem(s_A, t)}, TArrKey, t)),
        dict_map({map_elem(s_A, loosen_likeness_recursively(t))},
                 TArrKey, loosen_likeness_recursively(t)));
    }

    if (t.couldBe(BArrLikeN | BObj)) return;

    auto u = BBottom;
    if (t.couldBe(BCls | BLazyCls)) u |= BSStr;
    EXPECT_EQ(loosen_likeness_recursively(t), union_of(t, Type{u}));
  };

  auto const almostAll1 = Type{BInitCell & ~BSStr};
  auto const almostAll2 = Type{BInitCell};

  auto const& all = allCases(index);
  for (auto const& t : all) test(t);
  test(almostAll1);
  test(almostAll2);

  std::vector<std::pair<Type, Type>> tests{
    { TClsMeth, Type{BClsMeth} },
    { TCls, Type{BCls|BSStr} },
    { TLazyCls, Type{BLazyCls|BSStr} },
    { TInt, TInt },
    { TEnumClassLabel, TEnumClassLabel },
    { Type{BInt|BCls}, Type{BCls|BSStr|BInt} },
    { wait_handle(TInt), wait_handle(TInt) },
    { wait_handle(TCls), wait_handle(Type{BCls|BSStr}) },
    { wait_handle(TClsMeth), wait_handle(Type{BClsMeth}) },
    { vec_n(TInt), vec_n(TInt) },
    { vec_n(TCls), vec_n(Type{BCls|BSStr}) },
    { vec_n(TClsMeth), vec_n(Type{BClsMeth}) },
    { vec({TInt}), vec({TInt}) },
    { vec({TCls}), vec({Type{BCls|BSStr}}) },
    { vec({TClsMeth}), vec({Type{BClsMeth}}) },
    { dict_n(TArrKey, TInt), dict_n(TArrKey, TInt) },
    { dict_n(TArrKey, TCls), dict_n(TArrKey, Type{BCls|BSStr}) },
    { dict_n(TArrKey, TClsMeth), dict_n(TArrKey, Type{BClsMeth}) },
    { dict_map({map_elem(s_A, TInt)}, TArrKey, TInt),
      dict_map({map_elem(s_A, TInt)}, TArrKey, TInt) },
    { dict_map({map_elem(s_A, TCls)}, TArrKey, TCls),
      dict_map({map_elem(s_A, Type{BCls|BSStr})}, TArrKey, Type{BCls|BSStr}) },
    { dict_map({map_elem(s_A, TClsMeth)}, TArrKey, TClsMeth),
      dict_map({map_elem(s_A, Type{BClsMeth})}, TArrKey, Type{BClsMeth}) },
    { vec_n(almostAll1), TVecN },
    { vec_n(almostAll2), TVecN },
    { dict_n(TArrKey, almostAll1), TDictN },
    { dict_n(TArrKey, almostAll2), TDictN }
  };
  for (auto const& p : tests) {
    EXPECT_EQ(loosen_mark_for_testing(loosen_likeness_recursively(p.first)),
              loosen_mark_for_testing(p.second));
  }
}

TEST(Type, PromoteClassish) {
  auto const index = make_index();

  auto const clsA = index.resolve_class(s_A.get());
  if (!clsA || !clsA->isComplete()) ADD_FAILURE();

  std::vector<std::pair<Type, Type>> tests{
    { TCls, TSStr },
    { TLazyCls, TSStr },
    { TOptCls, TOptSStr },
    { TOptLazyCls, TOptSStr },
    { TStr, TStr },
    { TInt, TInt },
    { TObj, TObj },
    { TOptStr, TOptStr },
    { TEnumClassLabel, TEnumClassLabel },
    { TBottom, TBottom },
    { union_of(TCls, TLazyCls), TSStr },
    { union_of(TLazyCls, TStr), TStr },
    { union_of(TCls, TSStr), TSStr },
    { union_of(TInt, TLazyCls), union_of(TInt, TSStr) },
    { make_specialized_int(BInt, 100), make_specialized_int(BInt, 100) },
    { make_specialized_string(BStr, s_C.get()), make_specialized_string(BStr, s_C.get()) },
    { make_specialized_lazycls(BLazyCls, s_C.get()), make_specialized_string(BSStr, s_C.get()) },
    { make_specialized_lazycls(BLazyCls|BBool, s_C.get()), make_specialized_string(BSStr|BBool, s_C.get()) },
    { make_specialized_exact_class(BCls, *clsA), make_specialized_string(BSStr, s_A.get()) },
    { make_specialized_exact_class(BCls|BBool, *clsA), make_specialized_string(BSStr|BBool, s_A.get()) },
    { make_specialized_sub_class(BCls, *clsA), TSStr },
    { make_specialized_sub_class(BCls|BBool, *clsA), union_of(TSStr, TBool) }
  };

  for (auto const& p : tests) {
    EXPECT_EQ(promote_classish(p.first), p.second);
  }
}

TEST(Type, IterTypes) {
  auto const elem1 = map_elem(s_A, TObj);
  auto const elem2 = map_elem_nonstatic(s_B, TInt);
  auto const sdict1 = static_dict(s_A, 100);
  auto const sdict2 = static_dict(s_A, 100, s_B, 200);

  std::vector<std::pair<Type, IterTypes>> tests{
    { TInt, { TBottom, TBottom, IterTypes::Count::Empty, true, true } },
    { TInitNull, { TBottom, TBottom, IterTypes::Count::Empty, true, true } },
    { Type{BObj|BArrLike}, { TInitCell, TInitCell, IterTypes::Count::Any, true, true } },
    { Type{BInt|BArrLike}, { TInitCell, TInitCell, IterTypes::Count::Any, true, false } },
    { TVecE, { TBottom, TBottom, IterTypes::Count::Empty, false, false } },
    { TOptVecE, { TBottom, TBottom, IterTypes::Count::Empty, true, false } },

    { TSVec, { TInt, TInitUnc, IterTypes::Count::Any, false, false } },
    { TOptSVec, { TInt, TInitUnc, IterTypes::Count::Any, true, false } },
    { TSVecN, { TInt, TInitUnc, IterTypes::Count::NonEmpty, false, false } },
    { TOptSVecN, { TInt, TInitUnc, IterTypes::Count::Any, true, false } },

    { TSKeyset, { TUncArrKey, TUncArrKey, IterTypes::Count::Any, false, false } },
    { TOptSKeyset, { TUncArrKey, TUncArrKey, IterTypes::Count::Any, true, false } },
    { TSKeysetN, { TUncArrKey, TUncArrKey, IterTypes::Count::NonEmpty, false, false } },
    { TOptSKeysetN, { TUncArrKey, TUncArrKey, IterTypes::Count::Any, true, false } },

    { TSArrLike, { TUncArrKey, TInitUnc, IterTypes::Count::Any, false, false } },
    { TOptSArrLike, { TUncArrKey, TInitUnc, IterTypes::Count::Any, true, false } },
    { TSArrLikeN, { TUncArrKey, TInitUnc, IterTypes::Count::NonEmpty, false, false } },
    { TOptSArrLikeN, { TUncArrKey, TInitUnc, IterTypes::Count::Any, true, false } },

    { TVec, { TInt, TInitCell, IterTypes::Count::Any, false, false } },
    { TOptVec, { TInt, TInitCell, IterTypes::Count::Any, true, false } },
    { TVecN, { TInt, TInitCell, IterTypes::Count::NonEmpty, false, false } },
    { TOptVecN, { TInt, TInitCell, IterTypes::Count::Any, true, false } },

    { TKeyset, { TArrKey, TArrKey, IterTypes::Count::Any, false, false } },
    { TOptKeyset, { TArrKey, TArrKey, IterTypes::Count::Any, true, false } },
    { TKeysetN, { TArrKey, TArrKey, IterTypes::Count::NonEmpty, false, false } },
    { TOptKeysetN, { TArrKey, TArrKey, IterTypes::Count::Any, true, false } },

    { TArrLike, { TArrKey, TInitCell, IterTypes::Count::Any, false, false } },
    { TOptArrLike, { TArrKey, TInitCell, IterTypes::Count::Any, true, false } },
    { TArrLikeN, { TArrKey, TInitCell, IterTypes::Count::NonEmpty, false, false } },
    { TOptArrLikeN, { TArrKey, TInitCell, IterTypes::Count::Any, true, false } },

    { make_specialized_arrval(BSDict, sdict1), { sval(s_A), ival(100), IterTypes::Count::ZeroOrOne, false, false } },
    { make_specialized_arrval(BOptSDict, sdict1), { sval(s_A), ival(100), IterTypes::Count::ZeroOrOne, true, false } },
    { make_specialized_arrval(BSDictN, sdict1), { sval(s_A), ival(100), IterTypes::Count::Single, false, false } },
    { make_specialized_arrval(BOptSDictN, sdict1), { sval(s_A), ival(100), IterTypes::Count::ZeroOrOne, true, false } },

    { make_specialized_arrval(BSDict, sdict2), { TSStr, TInt, IterTypes::Count::Any, false, false } },
    { make_specialized_arrval(BOptSDict, sdict2), { TSStr, TInt, IterTypes::Count::Any, true, false } },
    { make_specialized_arrval(BSDictN, sdict2), { TSStr, TInt, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrval(BOptSDictN, sdict2), { TSStr, TInt, IterTypes::Count::Any, true, false } },

    { make_specialized_arrpackedn(BVec, TObj), { TInt, TObj, IterTypes::Count::Any, false, false } },
    { make_specialized_arrpackedn(BOptVec, TObj), { TInt, TObj, IterTypes::Count::Any, true, false } },
    { make_specialized_arrpackedn(BVecN, TObj), { TInt, TObj, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrpackedn(BOptVecN, TObj), { TInt, TObj, IterTypes::Count::Any, true, false } },

    { make_specialized_arrpacked(BVec, {TObj}), { ival(0), TObj, IterTypes::Count::ZeroOrOne, false, false } },
    { make_specialized_arrpacked(BOptVec, {TObj}), { ival(0), TObj, IterTypes::Count::ZeroOrOne, true, false } },
    { make_specialized_arrpacked(BVecN, {TObj}), { ival(0), TObj, IterTypes::Count::Single, false, false } },
    { make_specialized_arrpacked(BOptVecN, {TObj}), { ival(0), TObj, IterTypes::Count::ZeroOrOne, true, false } },

    { make_specialized_arrpacked(BVec, {TObj,TStr}), { TInt, Type{BObj|BStr}, IterTypes::Count::Any, false, false } },
    { make_specialized_arrpacked(BOptVec, {TObj,TStr}), { TInt, Type{BObj|BStr}, IterTypes::Count::Any, true, false } },
    { make_specialized_arrpacked(BVecN, {TObj,TStr}), { TInt, Type{BObj|BStr}, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrpacked(BOptVecN, {TObj,TStr}), { TInt, Type{BObj|BStr}, IterTypes::Count::Any, true, false } },

    { make_specialized_arrmapn(BDict, TStr, TObj), { TStr, TObj, IterTypes::Count::Any, false, false } },
    { make_specialized_arrmapn(BOptDict, TStr, TObj), { TStr, TObj, IterTypes::Count::Any, true, false } },
    { make_specialized_arrmapn(BDictN, TStr, TObj), { TStr, TObj, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrmapn(BOptDictN, TStr, TObj), { TStr, TObj, IterTypes::Count::Any, true, false } },

    { make_specialized_arrmap(BDict, {elem1}), { sval(s_A), TObj, IterTypes::Count::ZeroOrOne, false, false } },
    { make_specialized_arrmap(BOptDict, {elem1}), { sval(s_A), TObj, IterTypes::Count::ZeroOrOne, true, false } },
    { make_specialized_arrmap(BDictN, {elem1}), { sval(s_A), TObj, IterTypes::Count::Single, false, false } },
    { make_specialized_arrmap(BOptDictN, {elem1}), { sval(s_A), TObj, IterTypes::Count::ZeroOrOne, true, false } },

    { make_specialized_arrmap(BDict, {elem2}), { sval_nonstatic(s_B), TInt, IterTypes::Count::ZeroOrOne, false, false } },
    { make_specialized_arrmap(BOptDict, {elem2}), { sval_nonstatic(s_B), TInt, IterTypes::Count::ZeroOrOne, true, false } },
    { make_specialized_arrmap(BDictN, {elem2}), { sval_nonstatic(s_B), TInt, IterTypes::Count::Single, false, false } },
    { make_specialized_arrmap(BOptDictN, {elem2}), { sval_nonstatic(s_B), TInt, IterTypes::Count::ZeroOrOne, true, false } },

    { make_specialized_arrmap(BDict, {elem1,elem2}), { TStr, Type{BObj|BInt}, IterTypes::Count::Any, false, false } },
    { make_specialized_arrmap(BOptDict, {elem1,elem2}), { TStr, Type{BObj|BInt}, IterTypes::Count::Any, true, false } },
    { make_specialized_arrmap(BDictN, {elem1,elem2}), { TStr, Type{BObj|BInt}, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrmap(BOptDictN, {elem1,elem2}), { TStr, Type{BObj|BInt}, IterTypes::Count::Any, true, false } },

    { make_specialized_arrmap(BDict, {elem1}, TInt, TInt), { union_of(sval(s_A),TInt), Type{BObj|BInt}, IterTypes::Count::Any, false, false } },
    { make_specialized_arrmap(BOptDict, {elem1}, TInt, TInt), { union_of(sval(s_A),TInt), Type{BObj|BInt}, IterTypes::Count::Any, true, false } },
    { make_specialized_arrmap(BDictN, {elem1}, TInt, TInt), { union_of(sval(s_A),TInt), Type{BObj|BInt}, IterTypes::Count::NonEmpty, false, false } },
    { make_specialized_arrmap(BOptDictN, {elem1}, TInt, TInt), { union_of(sval(s_A),TInt), Type{BObj|BInt}, IterTypes::Count::Any, true, false } },
  };

  for (auto const& p : tests) {
    auto const iter = iter_types(p.first);
    EXPECT_EQ(iter.key, p.second.key);
    EXPECT_EQ(iter.value, p.second.value);
    EXPECT_EQ(iter.count, p.second.count) << show(p.first);
    EXPECT_EQ(iter.mayThrowOnInit, p.second.mayThrowOnInit);
    EXPECT_EQ(iter.mayThrowOnGetOrNext, p.second.mayThrowOnGetOrNext);
  }
}

TEST(Type, MightHaveDCls) {
  auto index = make_index();

  // Types that never contain DCls
  EXPECT_FALSE(might_have_dcls(TInt));
  EXPECT_FALSE(might_have_dcls(TDbl));
  EXPECT_FALSE(might_have_dcls(TStr));
  EXPECT_FALSE(might_have_dcls(TBool));
  EXPECT_FALSE(might_have_dcls(TNull));
  EXPECT_FALSE(might_have_dcls(TBottom));
  EXPECT_FALSE(might_have_dcls(TTop));
  EXPECT_FALSE(might_have_dcls(TUninit));
  EXPECT_FALSE(might_have_dcls(TInitNull));
  EXPECT_FALSE(might_have_dcls(TFalse));
  EXPECT_FALSE(might_have_dcls(TTrue));
  EXPECT_FALSE(might_have_dcls(ival(42)));
  EXPECT_FALSE(might_have_dcls(dval(3.14)));
  EXPECT_FALSE(might_have_dcls(sval(s_A.get())));

  // LazyClass and EnumClassLabel should not contain DCls
  EXPECT_FALSE(might_have_dcls(TLazyCls));
  EXPECT_FALSE(might_have_dcls(TEnumClassLabel));

  // Array values without objects should not contain DCls
  EXPECT_FALSE(might_have_dcls(vec_val(static_vec(1, 2, 3))));
  EXPECT_FALSE(might_have_dcls(dict_val(static_dict(s_A, 100, s_B, 200))));

  // Types that might contain DCls - Object types
  auto const cls = index.resolve_class(s_Base.get());
  if (!cls) ADD_FAILURE();
  EXPECT_TRUE(might_have_dcls(objExact(*cls)));
  EXPECT_TRUE(might_have_dcls(subObj(*cls)));
  // Note: TObj and TOptObj have DataTag::None (no specialization), so might_have_dcls returns false
  // because there's no actual DCls stored in the type - only bits.
  EXPECT_FALSE(might_have_dcls(TObj));
  EXPECT_FALSE(might_have_dcls(TOptObj));

  // Types that might contain DCls - Class types
  EXPECT_TRUE(might_have_dcls(clsExact(*cls, false)));
  EXPECT_TRUE(might_have_dcls(subCls(*cls, false)));
  EXPECT_TRUE(might_have_dcls(clsExact(*cls, true)));
  EXPECT_TRUE(might_have_dcls(subCls(*cls, true)));
  // TCls and TOptCls also have DataTag::None
  EXPECT_FALSE(might_have_dcls(TCls));
  EXPECT_FALSE(might_have_dcls(TOptCls));

  // Arrays that contain objects/classes should return true
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpacked(BVec, {objExact(*cls)})));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpacked(BVec, {TObj})));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpackedn(BVec, TObj)));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpackedn(BVec, clsExact(*cls, false))));

  auto const map = MapElems{map_elem(s_A, objExact(*cls))};
  EXPECT_TRUE(might_have_dcls(make_specialized_arrmap(BDict, map)));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrmapn(BDict, TStr, TObj)));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrmapn(BDict, TStr, TCls)));

  // WaitHandles containing objects should return true
  EXPECT_TRUE(might_have_dcls(make_specialized_wait_handle(BObj, objExact(*cls))));
  EXPECT_TRUE(might_have_dcls(make_specialized_wait_handle(BObj, TObj)));

  // Note: might_have_dcls is conservative for array specializations - it returns true
  // for any specialized array type without checking the element types deeply.
  // Arrays with non-object elements still return true if they have specializations.
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpacked(BVec, {TInt, TStr})));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrpackedn(BVec, TInt)));
  EXPECT_TRUE(might_have_dcls(make_specialized_arrmapn(BDict, TStr, TInt)));

  // Union types - TObj and TCls are just bits, so unions with them don't have DCls
  EXPECT_FALSE(might_have_dcls(union_of(TInt, TStr)));
  EXPECT_FALSE(might_have_dcls(union_of(TInt, TObj)));
  EXPECT_FALSE(might_have_dcls(union_of(TStr, TCls)));
  // Note: Unions involving specialized types may lose specialization and revert to just bits
}

TEST(Type, SerializeClasses) {
  auto index = make_index();

  auto const baseCls = index.resolve_class(s_Base.get());
  auto const aCls = index.resolve_class(s_A.get());
  auto const aaCls = index.resolve_class(s_AA.get());
  if (!baseCls || !aCls || !aaCls) ADD_FAILURE();

  // Test exact object serialization
  auto obj1 = objExact(*baseCls);
  auto obj1Serialized = serialize_classes(obj1);
  EXPECT_TRUE(might_have_dcls(obj1Serialized));

  // Test sub-object serialization
  auto obj2 = subObj(*baseCls);
  auto obj2Serialized = serialize_classes(obj2);
  EXPECT_TRUE(might_have_dcls(obj2Serialized));

  // Test exact class serialization
  auto cls1 = clsExact(*baseCls, false);
  auto cls1Serialized = serialize_classes(cls1);
  EXPECT_TRUE(might_have_dcls(cls1Serialized));

  // Test sub-class serialization
  auto cls2 = subCls(*baseCls, false);
  auto cls2Serialized = serialize_classes(cls2);
  EXPECT_TRUE(might_have_dcls(cls2Serialized));

  // Test serialization in arrays - packed arrays
  auto arrWithObj = make_specialized_arrpacked(BVec, {objExact(*baseCls), TInt});
  auto arrWithObjSerialized = serialize_classes(arrWithObj);
  EXPECT_TRUE(might_have_dcls(arrWithObjSerialized));

  // Test serialization in arrays - packedN arrays
  auto arrPackedN = make_specialized_arrpackedn(BVec, subObj(*baseCls));
  auto arrPackedNSerialized = serialize_classes(arrPackedN);
  EXPECT_TRUE(might_have_dcls(arrPackedNSerialized));

  // Test serialization in arrays - map arrays
  auto const map = MapElems{map_elem(s_A, objExact(*aCls))};
  auto arrMap = make_specialized_arrmap(BDict, map);
  auto arrMapSerialized = serialize_classes(arrMap);
  EXPECT_TRUE(might_have_dcls(arrMapSerialized));

  // Test serialization in arrays - mapN arrays
  auto arrMapN = make_specialized_arrmapn(BDict, TStr, clsExact(*baseCls, false));
  auto arrMapNSerialized = serialize_classes(arrMapN);
  EXPECT_TRUE(might_have_dcls(arrMapNSerialized));

  // Test serialization in WaitHandles
  auto wh = make_specialized_wait_handle(BObj, objExact(*aaCls));
  auto whSerialized = serialize_classes(wh);
  EXPECT_TRUE(might_have_dcls(whSerialized));

  // Test that serialization is idempotent for types without DCls
  auto intType = TInt;
  auto intTypeSerialized = serialize_classes(intType);
  EXPECT_EQ(intType, intTypeSerialized);

  auto strType = TStr;
  auto strTypeSerialized = serialize_classes(strType);
  EXPECT_EQ(strType, strTypeSerialized);

  // Test serialization doesn't change types without objects/classes
  // Note: Arrays with specializations still report might_have_dcls=true even without objects
  auto arrNoObj = make_specialized_arrpacked(BVec, {TInt, TStr});
  auto arrNoObjSerialized = serialize_classes(arrNoObj);
  // Both should report true because they have array specializations
  EXPECT_TRUE(might_have_dcls(arrNoObj));
  EXPECT_TRUE(might_have_dcls(arrNoObjSerialized));
}

//////////////////////////////////////////////////////////////////////

}
