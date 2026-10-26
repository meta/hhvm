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

#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {

/*
 * A counter registered on first use, with a cached process-lifetime handle.
 * The name must have static storage duration. Unused counters are not exported.
 */
struct ContProfCounter {
  explicit constexpr ContProfCounter(const char* name) : m_name{name} {}

  bool initialized() const {
    return m_counter.load(std::memory_order_acquire) != nullptr;
  }

  ServiceData::ExportedCounter* operator->() const {
    auto counter = m_counter.load(std::memory_order_acquire);
    if (!counter) {
      // Concurrent registrations of the same name return the same handle.
      counter = ServiceData::createCounter(m_name);
      m_counter.store(counter, std::memory_order_release);
    }
    return counter;
  }

private:
  const char* m_name;
  mutable std::atomic<ServiceData::ExportedCounter*> m_counter{nullptr};
};

}
