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

#include "hphp/runtime/base/rds-symbol.h"

#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"

#include <variant>
#include <fmt/format.h>

#include <string>
#include <type_traits>

namespace HPHP::rds {

///////////////////////////////////////////////////////////////////////////////

namespace {

struct SymbolKind {
  std::string operator()(LinkName k) const { return k.type; }
  std::string operator()(LinkID k) const { return k.type; }
  std::string operator()(ClsConstant) const { return "ClsConstant"; }
  std::string operator()(StaticMethod) const { return "StaticMethod"; }
  std::string operator()(StaticMethodF) const { return "StaticMethodF"; }
  std::string operator()(Profile) const { return "Profile"; }
  std::string operator()(SPropCache) const { return "SPropCache"; }
  std::string operator()(StaticMemoValue) const { return "StaticMemoValue"; }
  std::string operator()(StaticMemoCache) const { return "StaticMemoCache"; }
  std::string operator()(LSBMemoValue) const { return "LSBMemoValue"; }
  std::string operator()(LSBMemoCache) const { return "LSBMemoCache"; }
  std::string operator()(TSCache) const { return "TSCache"; }
  std::string operator()(ModuleCache) const { return "ModuleCache"; }
  std::string operator()(ConstMemoCache) const { return "ConstMemoCache"; }
};

struct SymbolRep {
  std::string operator()(LinkName k) const { return k.name->data(); }
  std::string operator()(LinkID) const { return ""; }

  std::string operator()(ClsConstant k) const {
    return k.clsName->data() + std::string("::") + k.cnsName->data();
  }

  std::string operator()(StaticMethod k) const {
    return folly::to<std::string>(
        k.clsName->toCppString(), "::",
        k.methName->toCppString(), ":",
        k.ctxName->toCppString()
    );
  }
  std::string operator()(StaticMethodF k) const {
    return folly::to<std::string>(
        k.clsName->toCppString(), "::",
        k.methName->toCppString(), ":",
        k.ctxName->toCppString()
    );
  }

  std::string operator()(Profile k) const {
    return fmt::format(
      "{}:t{}:{}",
      k.name,
      k.transId,
      k.bcOff
    );
  }
  std::string operator()(SPropCache k) const {
    return k.cls->name()->toCppString() + "::" +
           k.cls->staticProperties()[k.slot].name->toCppString();
  }

  std::string operator()(StaticMemoValue k) const {
    auto const func = Func::fromFuncId(k.funcId);
    return func->fullName()->toCppString();
  }
  std::string operator()(StaticMemoCache k) const {
    auto const func = Func::fromFuncId(k.funcId);
    return func->fullName()->toCppString();
  }

  std::string operator()(LSBMemoValue k) const {
    auto const clsName = k.cls->name()->toCppString();
    auto const funcName = Func::fromFuncId(k.funcId)->fullName()->toCppString();
    return clsName + "::" + funcName;
  }
  std::string operator()(LSBMemoCache k) const {
    auto const clsName = k.cls->name()->toCppString();
    auto const funcName = Func::fromFuncId(k.funcId)->fullName()->toCppString();
    return clsName + "::" + funcName;
  }

  std::string operator()(TSCache k) const {
    auto const func = Func::fromFuncId(k.funcId);
    return func->fullName()->toCppString();
  }

  std::string operator()(ModuleCache k) const {
    return k.name->toCppString();
  }

  std::string operator()(ConstMemoCache k) const {
    auto const func = Func::fromFuncId(k.funcId);
    auto const funcName = func->fullName()->toCppString();
    auto const clsPrefix =
      k.cls ? fmt::format("{}::", k.cls->name()->slice()) : "";
    return fmt::format(
      "{}{}::{}::{}",
      clsPrefix, funcName, fmt::ptr(k.paramVals), k.asyncEager);
  }
};

struct SymbolEq {
  template<class T, class U>
  typename std::enable_if<
    !std::is_same<T,U>::value,
    bool
  >::type operator()(const T&, const U&) const { return false; }

  bool operator()(LinkName k1, LinkName k2) const {
    return strcmp(k1.type, k2.type) == 0 && k1.name->tsame(k2.name);
  }
  bool operator()(LinkID k1, LinkID k2) const {
    return strcmp(k1.type, k2.type) == 0;
  }

  bool operator()(ClsConstant k1, ClsConstant k2) const {
    assertx(k1.clsName->isStatic() && k1.cnsName->isStatic());
    assertx(k2.clsName->isStatic() && k2.cnsName->isStatic());
    return k1.clsName->tsame(k2.clsName) &&
           k1.cnsName == k2.cnsName;
  }

  bool operator()(Profile k1, Profile k2) const {
    assertx(k1.name->isStatic() && k2.name->isStatic());
    return k1.kind == k2.kind &&
           k1.transId == k2.transId &&
           k1.bcOff == k2.bcOff &&
           k1.name == k2.name;
  }

  template<class T>
  typename std::enable_if<
    std::is_same<T,StaticMethod>::value ||
      std::is_same<T,StaticMethodF>::value,
    bool
  >::type operator()(const T& t1, const T& t2) const {
    assertx(t1.clsName->isStatic() && t2.clsName->isStatic());
    assertx(t1.methName->isStatic() && t2.methName->isStatic());
    assertx(t1.ctxName->isStatic() && t2.ctxName->isStatic());
    return t1.clsName->tsame(t2.clsName) &&
           t1.methName == t2.methName &&
           t1.ctxName->tsame(t2.ctxName);
  }

  bool operator()(SPropCache k1, SPropCache k2) const {
    return k1.cls == k2.cls && k1.slot == k2.slot;
  }

  bool operator()(StaticMemoValue k1, StaticMemoValue k2) const {
    return k1.funcId == k2.funcId;
  }

  bool operator()(StaticMemoCache k1, StaticMemoCache k2) const {
    return k1.funcId == k2.funcId;
  }

  bool operator()(LSBMemoValue k1, LSBMemoValue k2) const {
    return k1.cls == k2.cls && k1.funcId == k2.funcId;
  }

  bool operator()(LSBMemoCache k1, LSBMemoCache k2) const {
    return k1.cls == k2.cls && k1.funcId == k2.funcId;
  }

  bool operator()(TSCache k1, TSCache k2) const {
    return k1.funcId == k2.funcId;
  }

  bool operator()(ModuleCache k1, ModuleCache k2) const {
    return k1.name->same(k2.name);
  }

  bool operator()(ConstMemoCache k1, ConstMemoCache k2) const {
    return k1.funcId == k2.funcId &&
           k1.cls == k2.cls &&
           k1.paramVals == k2.paramVals &&
           k1.asyncEager == k2.asyncEager;
  }
};

struct SymbolHash {
  // NOTE: Any hash functions that are not stable across HHVM
  // restarts should be overridden with an appropriate hash in
  // SymbolStableHash below.
  size_t operator()(LinkName k) const {
    return folly::hash::hash_combine(
      std::string{k.type}, k.name->hash());
  }
  size_t operator()(LinkID k) const {
    return folly::hash::hash_combine(std::string{k.type});
  }

  size_t operator()(ClsConstant k) const {
    return folly::hash::hash_128_to_64(
      k.clsName->hash(),
      k.cnsName->hash()
    );
  }

  size_t operator()(Profile k) const {
    return folly::hash::hash_combine(
      static_cast<int>(k.kind),
      k.transId,
      k.bcOff,
      k.name->hash()
    );
  }

  size_t operator()(StaticMethod k)  const {
    return folly::hash::hash_combine(
      k.clsName->hash(), k.methName->hash(), k.ctxName->hash()
    );
  }
  size_t operator()(StaticMethodF k)  const {
    return folly::hash::hash_combine(
      k.clsName->hash(), k.methName->hash(), k.ctxName->hash()
    );
  }

  size_t operator()(SPropCache k) const {
    return folly::hash::hash_combine(
      k.cls.get(), k.slot
    );
  }

  size_t operator()(StaticMemoValue k) const {
    return std::hash<FuncId>()(k.funcId);
  }
  size_t operator()(StaticMemoCache k) const {
    return std::hash<FuncId>()(k.funcId);
  }

  size_t operator()(LSBMemoValue k) const {
    return folly::hash::hash_combine(
      k.cls.get(), std::hash<FuncId>()(k.funcId)
    );
  }
  size_t operator()(LSBMemoCache k) const {
    return folly::hash::hash_combine(
      k.cls.get(), std::hash<FuncId>()(k.funcId)
    );
  }

  size_t operator()(TSCache k) const {
    return std::hash<FuncId>()(k.funcId);
  }

  size_t operator()(ModuleCache k) const {
    return k.name->hash();
  }

  size_t operator()(ConstMemoCache k) const {
    auto const clsHash = k.cls ? k.cls->stableHash() : 0;
    return folly::hash::hash_combine(
      std::hash<FuncId>()(k.funcId),
      clsHash,
      std::hash<const ArrayData*>()(k.paramVals),
      std::hash<bool>()(k.asyncEager)
    );
  }
};

struct SymbolStableHash : SymbolHash {
  using SymbolHash::operator();
  size_t operator()(SPropCache k) const {
    return folly::hash::hash_combine(
      k.cls->stableHash(), k.slot
    );
  }
  size_t operator()(StaticMemoValue k) const {
    return Func::fromFuncId(k.funcId)->stableHash();
  }
  size_t operator()(StaticMemoCache k) const {
    return Func::fromFuncId(k.funcId)->stableHash();
  }
  size_t operator()(LSBMemoValue k) const {
    return folly::hash::hash_combine(
      k.cls->stableHash(), Func::fromFuncId(k.funcId)->stableHash()
    );
  }
  size_t operator()(LSBMemoCache k) const {
    return folly::hash::hash_combine(
      k.cls->stableHash(), Func::fromFuncId(k.funcId)->stableHash()
    );
  }
  size_t operator()(TSCache k) const {
    return Func::fromFuncId(k.funcId)->stableHash();
  }
};

}

std::string symbol_kind(const Symbol& sym) {
  return std::visit(SymbolKind(), sym);
}
std::string symbol_rep(const Symbol& sym) {
  return std::visit(SymbolRep(), sym);
}
bool symbol_eq(const Symbol& sym1, const Symbol& sym2) {
  return std::visit(SymbolEq(), sym1, sym2);
}
size_t symbol_hash(const Symbol& sym) {
  return std::visit(SymbolHash(), sym);
}
size_t symbol_stable_hash(const Symbol& sym) {
  return std::visit(SymbolStableHash(), sym);
}

///////////////////////////////////////////////////////////////////////////////

}
