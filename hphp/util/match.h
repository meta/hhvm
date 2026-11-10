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

#include <folly/Overload.h>

namespace HPHP {

//////////////////////////////////////////////////////////////////////

/*
 * This is a utility for short-hand visitors (using lambdas) with
 * std::visit.
 *
 * Usage e.g.:
 *
 *   match<return_type>(
 *     thing,
 *     [&] (TypeA a) { ... },
 *     [&] (TypeB b) { ... }
 *   );
 */

//////////////////////////////////////////////////////////////////////

template <class R, class... Args>
decltype(auto) match(Args&&... args) {
  return folly::variant_match<R>(std::forward<Args>(args)...);
}

template <class... Args>
decltype(auto) match(Args&&... args) {
  return folly::variant_match(std::forward<Args>(args)...);
}

//////////////////////////////////////////////////////////////////////

}

