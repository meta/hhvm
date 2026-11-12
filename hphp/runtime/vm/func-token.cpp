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

#include "hphp/runtime/vm/func-token.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/util/assertions.h"

#include <mutex>

namespace HPHP {

namespace {

folly_concurrent_hash_map_simd<const Func*, std::weak_ptr<FuncToken>, pointer_hash<Func>> s_funcTokens;

// Serializes token creation and destruction. weak_ptr is not equality
// comparable, so the map's *_if_equal CAS primitives are unavailable; this lock
// makes the check-then-create and the destructor's erase atomic with respect to
// each other. Lockless find() on the fast path remains safe alongside it.
std::mutex s_funcTokensMutex;

// Unit::startDestroy() only marks free functions and methods of classes defined
// in the unit as Zombie.
bool needsUnitToken(const Func* func) {
  auto const cls = func->cls();
  if (!cls) return false;
  return func->isClosureBody() || cls->preClass()->unit() != func->unit();
}

}

FuncToken::FuncToken(const Func* func, bool isValid)
  : m_func(func)
  , m_persistent(func->isPersistent()) {
  m_isValid.store(isValid, std::memory_order_release);
  if (isValid && !m_persistent && needsUnitToken(func)) {
    m_unitToken = UnitToken::get(func->unit());
  }
}

std::shared_ptr<FuncToken> FuncToken::get(const Func* func) {
  // If the function is persistent, just return a token that points to it
  if (func->isPersistent()) return std::shared_ptr<FuncToken>(new FuncToken(func));

  // If the function is going to be deleted, just return an invalid token
  if (func->atomicFlags().check(Func::Flags::Zombie)) {
    return std::shared_ptr<FuncToken>(new FuncToken(func, false));
  }

  if (auto it = s_funcTokens.find(func); it != s_funcTokens.end()) {
    if (auto token = it->second.lock()) return token;
  }

  std::lock_guard<std::mutex> g(s_funcTokensMutex);
  if (func->atomicFlags().check(Func::Flags::Zombie)) {
    return std::shared_ptr<FuncToken>(new FuncToken(func, false));
  }
  // Re-check under the lock: another thread may have installed a live token
  // between our lockless find above and acquiring the lock.
  if (auto it = s_funcTokens.find(func); it != s_funcTokens.end()) {
    if (auto token = it->second.lock()) return token;
  }
  auto token = std::shared_ptr<FuncToken>(new FuncToken(func));
  s_funcTokens.insert_or_assign(func, token);
  return token;
}

void FuncToken::setInvalid(const Func* func) {
  // Declared before the lock so that if this is the last reference, the
  // destructor (which takes the lock) runs after it is released.
  std::shared_ptr<FuncToken> token;
  std::lock_guard<std::mutex> g(s_funcTokensMutex);
  if (auto it = s_funcTokens.find(func); it != s_funcTokens.end()) {
    token = it->second.lock();
    if (token) token->m_isValid.store(false, std::memory_order_release);
    // Drop the entry so a later get() creates a fresh, valid token instead of
    // handing back this invalidated one.
    s_funcTokens.erase(func);
  }
}

void FuncToken::assertNoValidToken(const Func* func) {
  if (!debug) return;
  // Declared before the lock for the same reason as in setInvalid().
  std::shared_ptr<FuncToken> token;
  std::lock_guard<std::mutex> g(s_funcTokensMutex);
  if (auto it = s_funcTokens.find(func); it != s_funcTokens.end()) {
    token = it->second.lock();
    assertx(!token || !token->isValid());
  }
}

FuncToken::~FuncToken() {
  // If the function is persistent we never added it to the map so no reason to erase it
  if (m_persistent) return;

  std::lock_guard<std::mutex> g(s_funcTokensMutex);
  // Only erase if the entry is still our (now-expired) token. A concurrent
  // get() may have already replaced it with a freshly created live token for
  // the same Func, which we must not clobber.
  if (auto it = s_funcTokens.find(m_func); it != s_funcTokens.end()) {
    if (it->second.expired()) s_funcTokens.erase(m_func);
  }
}

}
