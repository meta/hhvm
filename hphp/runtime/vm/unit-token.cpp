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

#include "hphp/runtime/vm/unit-token.h"

#include "hphp/runtime/vm/unit.h"
#include "hphp/util/assertions.h"

#include <mutex>

namespace HPHP {

namespace {

folly_concurrent_hash_map_simd<const Unit*, std::weak_ptr<UnitToken>, pointer_hash<Unit>> s_unitTokens;

// Serializes token creation, invalidation and destruction so a token can't be
// registered for a Unit after setInvalid() has run for it. Lockless find() on
// the fast path remains safe alongside it.
std::mutex s_unitTokensMutex;

}

UnitToken::UnitToken(const Unit* unit, bool isValid)
  : m_unit(unit)
  , m_persistent(unit->isPersistent()) {
  m_isValid.store(isValid, std::memory_order_release);
}

std::shared_ptr<UnitToken> UnitToken::get(const Unit* unit) {
  if (unit->isPersistent()) return std::shared_ptr<UnitToken>(new UnitToken(unit));

  if (unit->isZombie()) {
    return std::shared_ptr<UnitToken>(new UnitToken(unit, false));
  }

  if (auto it = s_unitTokens.find(unit); it != s_unitTokens.end()) {
    if (auto token = it->second.lock()) return token;
  }

  std::lock_guard<std::mutex> g(s_unitTokensMutex);
  if (unit->isZombie()) {
    return std::shared_ptr<UnitToken>(new UnitToken(unit, false));
  }
  if (auto it = s_unitTokens.find(unit); it != s_unitTokens.end()) {
    if (auto token = it->second.lock()) return token;
  }
  auto token = std::shared_ptr<UnitToken>(new UnitToken(unit));
  s_unitTokens.insert_or_assign(unit, token);
  return token;
}

void UnitToken::setInvalid(const Unit* unit) {
  assertx(!unit->isPersistent());
  // Declared before the lock so that if this is the last reference, the
  // destructor (which takes the lock) runs after it is released.
  std::shared_ptr<UnitToken> token;
  std::lock_guard<std::mutex> g(s_unitTokensMutex);
  if (auto it = s_unitTokens.find(unit); it != s_unitTokens.end()) {
    token = it->second.lock();
    if (token) token->m_isValid.store(false, std::memory_order_release);
    s_unitTokens.erase(unit);
  }
}

void UnitToken::assertNoValidToken(const Unit* unit) {
  if (!debug) return;
  // Declared before the lock for the same reason as in setInvalid().
  std::shared_ptr<UnitToken> token;
  std::lock_guard<std::mutex> g(s_unitTokensMutex);
  if (auto it = s_unitTokens.find(unit); it != s_unitTokens.end()) {
    token = it->second.lock();
    assertx(!token || !token->isValid());
  }
}

UnitToken::~UnitToken() {
  if (m_persistent) return;

  std::lock_guard<std::mutex> g(s_unitTokensMutex);
  // A concurrent get() may have replaced the entry with a live token for the
  // same Unit, which must not be erased.
  if (auto it = s_unitTokens.find(m_unit); it != s_unitTokens.end()) {
    if (it->second.expired()) s_unitTokens.erase(m_unit);
  }
}

}
