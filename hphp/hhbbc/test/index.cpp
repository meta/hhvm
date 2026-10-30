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

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/parse.h"
#include "hphp/hhbbc/representation.h"

#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/extern-worker.h"

namespace HPHP::HHBBC {
namespace {

using namespace extern_worker;
namespace coro = folly::coro;

TEST(Index, MaterializeIndexInput_OutOfOrderBytecode_RestoresProgram) {
  const std::string hhas = R"(
    .class First {
      .method [public] N firstMethod() {
        Int 1
        RetC None
      }
    }
    .class Second {
      .method [public] N secondMethod() {
        String "second"
        RetC None
      }
    }
    .function N firstFunction() {
      Int 3
      RetC None
    }
    .function N secondFunction() {
      String "four"
      RetC None
    }
  )";

  std::unique_ptr<UnitEmitter> ue{assemble_string(
    hhas,
    "materialize.php",
    SHA1{},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults()
  )};
  ASSERT_FALSE(ue->m_fatalUnit);
  auto parsed = parse_unit(*ue);

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
  auto client = std::make_unique<Client>(executor->sticky(), options);

  Index::Input input;
  ASSERT_TRUE(parsed.unit);
  auto const unitName = parsed.unit->filename;
  input.units.emplace_back(Index::Input::UnitMeta{
    coro::blockingWait(client->store(std::move(parsed.unit))),
    unitName
  });

  for (auto& cls : parsed.classes) {
    auto const name = cls->name;
    auto const unit = cls->unit;
    auto bytecode = std::make_unique<php::ClassBytecode>(name);
    for (auto& method : cls->methods) {
      bytecode->methodBCs.emplace_back(
        method->name,
        std::move(method->rawBlocks)
      );
    }

    auto storedClass = coro::blockingWait(client->store(std::move(cls)));
    auto storedBytecode =
      coro::blockingWait(client->store(std::move(bytecode)));
    input.classes.emplace_back(Index::Input::ClassMeta{
      std::move(storedClass),
      name,
      {},
      nullptr,
      {},
      unit,
      {},
      {}
    });
    input.classBC.emplace_back(Index::Input::ClassBytecodeMeta{
      std::move(storedBytecode),
      name
    });
  }

  for (auto& func : parsed.funcs) {
    auto const name = func->name;
    auto const unit = func->unit;
    auto bytecode = std::make_unique<php::FuncBytecode>(
      name,
      std::move(func->rawBlocks)
    );

    auto storedFunc = coro::blockingWait(client->store(std::move(func)));
    auto storedBytecode =
      coro::blockingWait(client->store(std::move(bytecode)));
    input.funcs.emplace_back(Index::Input::FuncMeta{
      std::move(storedFunc),
      name,
      unit,
      false
    });
    input.funcBC.emplace_back(Index::Input::FuncBytecodeMeta{
      std::move(storedBytecode),
      name,
      nullptr,
      false
    });
  }

  std::reverse(begin(input.classBC), end(input.classBC));
  std::reverse(begin(input.funcBC), end(input.funcBC));

  auto disposed = false;
  StructuredLogEntry sample;
  auto program = coro::blockingWait(materialize_index_input(
    std::move(input),
    std::move(executor),
    std::move(client),
    [&] (std::unique_ptr<TicketExecutor>, std::unique_ptr<Client>) {
      disposed = true;
    },
    &sample
  ));

  EXPECT_TRUE(disposed);

  Index index{std::move(program), &sample};
  EXPECT_EQ(index.sample(), &sample);
  ASSERT_EQ(index.program().units.size(), 1);
  ASSERT_EQ(index.program().classes.size(), 2);
  ASSERT_EQ(index.program().funcs.size(), 2);

  std::vector<std::string> classNames;
  std::vector<std::string> funcNames;
  for (auto const& cls : index.program().classes) {
    classNames.emplace_back(cls->name->toCppString());
    ASSERT_EQ(cls->methods.size(), 1);
    EXPECT_TRUE(cls->methods[0]->rawBlocks);
    EXPECT_EQ(index.lookup_class(cls->name), cls.get());
    EXPECT_EQ(index.lookup_class_unit(*cls), index.program().units[0].get());
  }
  for (auto const& func : index.program().funcs) {
    funcNames.emplace_back(func->name->toCppString());
    EXPECT_TRUE(func->rawBlocks);
  }
  std::sort(begin(classNames), end(classNames));
  std::sort(begin(funcNames), end(funcNames));

  const std::vector<std::string> expectedClassNames{"First", "Second"};
  const std::vector<std::string> expectedFuncNames{
    "firstFunction",
    "secondFunction"
  };
  EXPECT_EQ(classNames, expectedClassNames);
  EXPECT_EQ(funcNames, expectedFuncNames);
}

TEST(Index, MaterializeIndexInput_LoadFailure_DisposesResources) {
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
  auto client = std::make_unique<Client>(executor->sticky(), options);

  auto const path = boost::filesystem::temp_directory_path() /
    boost::filesystem::unique_path("hhbbc-materialize-%%%%-%%%%");
  {
    std::ofstream file{path.native(), std::ios::binary};
    ASSERT_TRUE(file);
    file << std::string(1024 * 1024, 'x');
    ASSERT_TRUE(file);
  }
  auto missingUnit = coro::blockingWait(
    client->storeFile(std::filesystem::path{path.native()})
  )
    .cast<std::unique_ptr<php::Unit>>();
  ASSERT_TRUE(boost::filesystem::remove(path));

  Index::Input input;
  input.units.emplace_back(Index::Input::UnitMeta{
    std::move(missingUnit), makeStaticString("missing.php")
  });

  auto disposals = 0;
  EXPECT_THROW(
    coro::blockingWait(materialize_index_input(
      std::move(input),
      std::move(executor),
      std::move(client),
      [&] (std::unique_ptr<TicketExecutor> disposedExecutor,
           std::unique_ptr<Client> disposedClient) {
        ++disposals;
        EXPECT_NE(disposedExecutor, nullptr);
        EXPECT_NE(disposedClient, nullptr);
        disposedClient.reset();
        disposedExecutor.reset();
      },
      nullptr
    )),
    extern_worker::Error
  );
  EXPECT_EQ(disposals, 1);
}

}
}
