/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
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

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/jit/bc-marker.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/system/systemlib.h"

namespace HPHP::jit {

//////////////////////////////////////////////////////////////////////

inline BCMarker test_marker() {
  return BCMarker{
    SrcKey{SystemLib::getExceptionClass()->getCtor(), 0, ResumeMode::None},
    SBInvOffset{0}, false, TransIDSet{}, nullptr, nullptr, nullptr
  };
}

// A TransContext for use in tests.
const auto test_context =
  TransContext {
    TransIDSet{},
    0,
    TransKind::Live,
    SrcKey{},
    nullptr,
    nullptr,
    PrologueID() };

//////////////////////////////////////////////////////////////////////

}
