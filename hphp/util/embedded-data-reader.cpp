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

#include "hphp/util/embedded-data-reader.h"

#include "hphp/util/current-executable.h"

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/container/F14Map.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/portability/Unistd.h>
#include <folly/ssl/OpenSSLHash.h>

#include <fcntl.h>

#include <array>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

namespace {

struct SectionTable {
  std::string filename;
  // Section name -> (file offset, size).
  folly::F14FastMap<std::string, std::pair<uint64_t, uint64_t>> sections;
};

const SectionTable& sectionTable() {
  static auto const table = [] {
    auto t = new SectionTable;
    t->filename = current_executable_path();

    folly::symbolizer::ElfFile file;
    if (file.openNoThrow(t->filename.c_str()) != 0) return t;

    file.iterateSections([&] (const folly::symbolizer::ElfShdr& shdr) {
      if (auto const name = file.getSectionName(shdr)) {
        t->sections.emplace(name, std::make_pair(shdr.sh_offset, shdr.sh_size));
      }
      return false;
    });
    return t;
  }();
  return *table;
}

}

///////////////////////////////////////////////////////////////////////////////

EmbeddedDataReader::EmbeddedDataReader()
  : m_fd(::open(sectionTable().filename.c_str(), O_RDONLY | O_CLOEXEC))
{}

EmbeddedDataReader::~EmbeddedDataReader() {
  if (m_fd >= 0) ::close(m_fd);
}

std::optional<embedded_data> EmbeddedDataReader::find(const char* section) {
  auto const& table = sectionTable();
  auto const it = table.sections.find(section);
  if (it == table.sections.end()) return std::nullopt;

  embedded_data desc;
  desc.m_filename = table.filename;
  desc.m_start = it->second.first;
  desc.m_len = it->second.second;
  return desc;
}

std::string EmbeddedDataReader::readSection(const char* section) const {
  if (m_fd < 0) return "";
  auto const desc = find(section);
  if (!desc) return "";

  std::string data(desc->m_len, '\0');
  auto const amountRead =
    folly::preadFull(m_fd, data.data(), desc->m_len, desc->m_start);
  if (amountRead < 0 || static_cast<uint64_t>(amountRead) != desc->m_len) {
    return "";
  }
  return data;
}

std::string
EmbeddedDataReader::readSectionForPath(const std::string& path) const {
  std::array<uint8_t, 16> md5;
  folly::ssl::OpenSSLHash::hash(
    folly::range(md5), EVP_md5(), folly::ByteRange{folly::StringPiece{path}});
  auto const section = "ext." + folly::hexlify(md5).substr(0, 12);
  return readSection(section.c_str());
}

///////////////////////////////////////////////////////////////////////////////

}
