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

#include "hphp/runtime/vm/jit/punt.h"

#include <fmt/format.h>

#include "hphp/runtime/vm/func.h"

namespace HPHP::jit {
//////////////////////////////////////////////////////////////////////

FailedIRGen::FailedIRGen(const char* _file, int _line, const char* _func)
  : std::runtime_error(fmt::format("FailedIRGen @ {}:{} in {}",
                                     _file, _line, _func))
  , file(_file)
  , line(_line)
  , func(_func)
{}

FailedTraceGen::FailedTraceGen(const char* file, int line, const char* why)
  : std::runtime_error(fmt::format("FailedTraceGen @ {}:{} - {}",
                                     file, line, why))
{}

RetryIRGen::RetryIRGen(const char* why)
  : std::runtime_error(why)
{}

//////////////////////////////////////////////////////////////////////
}
