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

#pragma once

#include <atomic>
#include <memory>

namespace HPHP {

struct Unit;

/*
 * A handle that lets code outliving a request, such as a queued JIT job,
 * check whether a Unit has been destroyed without touching the Unit.
 *
 * The token goes invalid when Unit::startDestroy() runs. If getUnit() returns
 * non-null inside a Treadmill session, the Unit stays allocated until that
 * session ends.
 */
struct UnitToken {

  ~UnitToken();

  static std::shared_ptr<UnitToken> get(const Unit* unit);
  static void setInvalid(const Unit* unit);
  static void assertNoValidToken(const Unit* unit);

  const Unit* getUnit() const {
    if (!isValid()) return nullptr;
    return m_unit;
  }

 private:
  explicit UnitToken(const Unit* unit, bool isValid = true);

  bool isValid() const {
    return m_isValid.load(std::memory_order_acquire);
  }

  const Unit* m_unit;
  std::atomic<bool> m_isValid;
  bool m_persistent;
};

}
