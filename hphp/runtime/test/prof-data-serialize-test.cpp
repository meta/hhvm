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
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/prof-data-serialize.h"

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <utility>

namespace HPHP::jit {
namespace {

TEST(ProfDataSerialize, InitializerIdentity) {
  auto const oldHHBBC = std::exchange(Cfg::Eval::UseHHBBC, false);
  SCOPE_EXIT { Cfg::Eval::UseHHBBC = oldHHBBC; };

  UnitEmitter ue{
    SHA1{101}, SHA1{102}, RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults()
  };
  ue.m_filepath = makeStaticString("profile-initializer-identity.php");
  auto const addClass = [&](const char* name, Attr attrs) {
    auto const pce = ue.newPreClassEmitter(name);
    pce->init(1, 1, attrs, staticEmptyString(), staticEmptyString(), false);
    for (auto const initName : {s_86pinit.get(), s_86sinit.get(), s_86linit.get()}) {
      auto const fe = ue.newMethodEmitter(initName, pce);
      fe->init(1, 1, Attr(AttrPrivate | AttrStatic), nullptr, false);
      fe->retTypeConstraints = TypeIntersectionConstraint{TypeConstraint{}};
      fe->recordSourceLocation(HPHP::Location::Range{1, 1, 1, 1}, 0);
      fe->emitOp(Op::Null);
      fe->emitOp(Op::RetC);
      fe->emitByte(static_cast<uint8_t>(VerifyRetKind::None));
      fe->maxStackCells = 1;
      fe->finish();
      EXPECT_TRUE(pce->addMethod(fe));
    }
    return pce;
  };
  auto const trait = addClass("ProfileInitTrait", AttrTrait);
  auto const owner = addClass("ProfileInitOwner", AttrNone);
  owner->addUsedTrait(trait->name());
  ue.finish();
  std::unique_ptr<Unit> unit{ue.create()};
  SCOPE_EXIT {
    for (auto const pce : {trait, owner}) {
      auto const named = NamedType::getNoCreate(pce->name());
      if (named && named->getCachedClass()) named->setCachedClass(nullptr);
    }
  };
  for (auto const pce : {trait, owner}) {
    ASSERT_NE(Class::def(unit->lookupPreClass(pce->name()), true), nullptr);
  }
  auto const cls = Class::lookup(owner->name());
  ASSERT_NE(cls, nullptr);
  auto const vectors = {&cls->pinitVec(), &cls->sinitVec(), &cls->linitVec()};
  for (auto const vec : vectors) {
    ASSERT_EQ(vec->size(), 2);
    auto const first = (*vec)[0];
    ASSERT_NE(first, vec->back());
    ASSERT_EQ(first->implCls(), cls);
    ASSERT_NE(cls->getMethodSafe(first->methodSlot()), first);
  }

  folly::test::TemporaryDirectory temp{"profile-init-codec"};
  auto const path = (temp.path() / "jit.prof").native();
  ProfDataSerializer::Id unitId;
  {
    ProfDataSerializer ser{path, ProfDataSerializer::FileMode::Create};
    unitId = ser.serialize(unit.get()).first;
    write_raw(ser, uint32_t{0});
    write_raw(ser, off_t{0});
    for (auto const vec : vectors) {
      for (auto const f : *vec) write_func(ser, f);
    }
    ser.finalize();
  }
  ProfDataDeserializer des{path};
  des.record(unitId, unit.get());
  read_raw<uint32_t>(des);
  des.setExtraDataOffset(read_raw<off_t>(des));
  for (auto const vec : vectors) {
    for (auto const f : *vec) {
      EXPECT_EQ(read_func(des), f) << f->fullName()->data();
    }
  }
  EXPECT_TRUE(des.done());
}

TEST(ProfDataSerialize, RejectInvalidMagicBeforeLoading) {
  folly::test::TemporaryDirectory temp{"invalid-profile-magic"};
  auto const path = (temp.path() / "jit.prof").native();
  {
    ProfDataSerializer ser{path, ProfDataSerializer::FileMode::Create};
    write_raw(ser, uint32_t{0});
    write_raw(ser, off_t{0});
    ser.finalize();
  }
  for (bool rds : {false, true}) {
    auto const result = deserializeProfData(path, 1, rds);
    EXPECT_NE(result.find("Not a profile-data dump"), std::string::npos);
  }
  ASSERT_EQ(std::rename(path.c_str(), (path + ".part2").c_str()), 0);
  for (bool rds : {false, true}) {
    EXPECT_FALSE(tryDeserializePartialProfData(path, 1, rds));
  }
}

} // namespace
} // namespace HPHP::jit
