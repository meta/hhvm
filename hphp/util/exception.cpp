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

#include "hphp/util/exception.h"

#include "hphp/util/string-vsnprintf.h"

#include <cxxabi.h>

#include <typeinfo>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

Exception::Exception(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); format(fmt, ap); va_end(ap);
}

Exception::Exception(const std::string& msg)
  : m_msg(msg) {
}

Exception::Exception(const Exception& e)
  : m_msg(e.m_msg), m_what(e.m_what)
{}

void Exception::format(const char *fmt, va_list ap) {
  string_vsnprintf(m_msg, fmt, ap);
}

///////////////////////////////////////////////////////////////////////////////

const char* Exception::what() const noexcept {
  if (m_what.empty()) {
    m_what = m_msg + "\n";
  }
  return m_what.c_str();
}

///////////////////////////////////////////////////////////////////////////////

std::string current_exception_name() noexcept {
  return __cxxabiv1::__cxa_current_exception_type()->name();
}

///////////////////////////////////////////////////////////////////////////////
}
