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

#include <folly/hash/Hash.h>

#include "hphp/runtime/vm/func-id.h"

namespace HPHP {

struct Func;

/*
 * A prologue is identified by the called function and the number of arguments
 * that the prologue handles.
 */
struct PrologueID {
  PrologueID(const Func* func, uint32_t nargs)
    : m_func(func)
    , m_nargs(nargs)
  {}

  PrologueID() : PrologueID(nullptr, std::numeric_limits<uint32_t>::max())
  {}

  uint32_t    nargs()  const { return m_nargs; }
  const Func* func()   const { return m_func; }

  bool operator==(const PrologueID& other) const {
    return m_func == other.m_func && m_nargs == other.m_nargs;
  }

  /* implicit */ operator uint64_t() const {
    return uint64_t(m_func.getRaw()) << 32 | m_nargs;
  }

  static PrologueID fromInt(uint64_t i) {
    return PrologueID(PackedPtr<Func>::fromRaw(i >> 32), i & 0xffffffff);
  }

  struct Eq {
    bool operator()(const PrologueID& pid1,
                    const PrologueID& pid2) const {
      return pid1 == pid2;
    }
  };

  struct Hasher {
    size_t operator()(PrologueID pid) const {
      return folly::hash::hash_combine(pid.m_func.getRaw(), pid.nargs());
    }
  };

 private:
  PackedPtr<const Func> m_func;
  uint32_t m_nargs;
};

std::string show(PrologueID pid);

}
