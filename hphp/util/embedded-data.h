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

#include <cstdint>
#include <string>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

struct embedded_data {
  std::string m_filename;
  uint64_t m_start;
  uint64_t m_len;
#ifdef _MSC_VER
  void* m_handle;
#endif
};

/*
 * dlopen() the embedded shared object given by `desc'.
 *
 * Unfortunately, there's no way to do the equivalent of dlopen() on data
 * within another file, or even in memory.  This means we have to copy the
 * section into a temporary file and then dlopen() that.
 *
 * If trust is true, than any existing file with the same name as the first
 * extract path will be used without extracting the data. Otherwise if the file
 * exists, its contents will be verified before using. If the file does not
 * exist, it will be created and the embedded data copied into it. If this fails
 * for any reason, the fallback path is used, if provided.
 *
 * Returns the result of dlopen() on success, else nullptr.  Also logs the
 * failure condition with Logger on failure.
 */
void* dlopen_embedded_data(const embedded_data& desc,
                           std::string extractPath,
                           std::string fallbackPath,
                           const std::string& buildId,
                           bool trust);

/*
 * Clean up any fallback files that we created at process shutdown time.
 */
void embedded_data_cleanup();


///////////////////////////////////////////////////////////////////////////////

}


