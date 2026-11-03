<?hh
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

// Don't put anything else in this namespace - use the one below
namespace HH\Lib\_Private\Native {

/** Creates a pipe, and a pair of linked file descriptors (resources). The
 * first file descriptor is connected to the read end of the pipe; the second
 * is connected to the write end.
 *
 * @return `(resource, resource)`
 */
<<__Native>>
function pipe(): varray<resource>;

}

namespace HH\Lib\_Private\_IO {
  <<__Native>>
  function response_write(string $data): int;
  <<__Native>>
  function response_flush(): void;
  <<__Native>>
  function request_read(int $max_bytes): ?string;
}
