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

#include "hphp/util/embedded-data.h"

#include <optional>
#include <string>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

/*
 * Reads data embedded in sections of the current executable. The executable
 * stays open for the reader's lifetime, so several sections can be read
 * without re-opening it for each one.
 */
struct EmbeddedDataReader {
  EmbeddedDataReader();
  ~EmbeddedDataReader();

  EmbeddedDataReader(const EmbeddedDataReader&) = delete;
  EmbeddedDataReader& operator=(const EmbeddedDataReader&) = delete;

  /*
   * Returns where the named section is in the current executable, without
   * opening it.
   */
  static std::optional<embedded_data> find(const char* section);

  /*
   * Returns the contents of the named section, or an empty string if there is
   * no such section or it could not be read in full.
   */
  std::string readSection(const char* section) const;

  /*
   * Like readSection(), for the section that the build embeds the file at
   * `path' in.
   */
  std::string readSectionForPath(const std::string& path) const;

private:
  int m_fd;
};

///////////////////////////////////////////////////////////////////////////////

}
