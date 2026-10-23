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

#include "hphp/runtime/vm/srckey.h"

#include <fmt/format.h>

#include "hphp/runtime/vm/hhbc.h"

namespace HPHP {

std::string SrcKey::showInst() const {
  if (prologue()) return "Prologue";
  if (funcEntry()) return "FuncEntry";
  if (namedParamsFuncEntry()) return "NamedParamsFuncEntry";
  auto const f = func();
  return instrToString(f->at(offset()), f);
}

std::string show(SrcKey sk) {
  auto func = sk.func();
  auto unit = sk.unit();
  const char *filepath = "*anonFile*";
  if (unit->origFilepath()->data() && unit->origFilepath()->size()) {
    filepath = unit->origFilepath()->data();
  }
  return fmt::format("{}:{} in {}(id 0x{:#x})@{: >6}{}{}{}{}{}",
                        filepath, sk.lineNumber(),
                        func->fullName()->data(),
                        sk.funcID().toInt(), sk.printableOffset(),
                        resumeModeShortName(sk.resumeMode()),
                        sk.hasThis()  ? "t" : "",
                        sk.prologue() ? "p" : "",
                        sk.funcEntry() ? "e" : "",
                        sk.namedParamsFuncEntry() ? "n" : "");
}

std::string showShort(SrcKey sk) {
  if (!sk.valid()) return "<invalid SrcKey>";
  return fmt::format(
    "{}(id {:#x})@{}{}{}{}{}{}",
    sk.func()->fullName()->slice(),
    sk.funcID(),
    sk.printableOffset(),
    resumeModeShortName(sk.resumeMode()),
    sk.hasThis()  ? "t" : "",
    sk.prologue() ? "p" : "",
    sk.funcEntry() ? "e" : "",
    sk.namedParamsFuncEntry() ? "n" : ""
  );
}

void sktrace(SrcKey sk, const char *fmt, ...) {
  if (!Trace::enabled) return;

  auto const inst = sk.showInst();
  Trace::trace("%s: %20s ", show(sk).c_str(), inst.c_str());
  va_list a;
  va_start(a, fmt);
  Trace::vtrace(fmt, a);
  va_end(a);
}

std::string SrcKey::getSymbol() const {
  const Func* f = func();

  if (f->isBuiltin()) {
    return f->fullName()->data();
  }

  if (f->isMethod() && !f->cls()) {
    return fmt::format(
      "{}::{}::line-{}",
      f->preClass()->name()->slice(),
      f->name()->slice(),
      lineNumber()
    );
  }

  // methods with a cls() and functions
  return fmt::format(
    "{}::line-{}",
    f->fullName()->slice(),
    lineNumber()
  );
}

}
