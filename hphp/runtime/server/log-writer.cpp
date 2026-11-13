/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
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

#include "hphp/runtime/server/log-writer.h"

#include <cctype>
#include <sstream>

#include "hphp/util/logger.h"
#include "hphp/runtime/base/init-fini-node.h"


namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
std::string FieldGenerator::escapeData(const char* s, int len) {
  static const char digits[] = "0123456789abcdef";
  std::ostringstream out;
  for (int i = 0; i < len; i++) {
    unsigned char uc = *s++;
    switch (uc) {
      case '"':  out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b";  break;
      case '\f': out << "\\f";  break;
      case '\n': out << "\\n";  break;
      case '\r': out << "\\r";  break;
      case '\t': out << "\\t";  break;
      default:
        if (uc >= ' ' && (uc & 127) == uc) {
          out << (char)uc;
        } else {
          out << "\\x" << digits[(uc >> 4) & 15] << digits[(uc >> 0) & 15];
        }
        break;
    }
  }
  return out.str();
}

ClassicWriter::~ClassicWriter() {
  if (m_filelog != nullptr && m_channel == LogChannel::REGULAR) {
    if (m_logdata.file[0] == '|') {
      pclose(m_filelog);
    } else {
      fclose(m_filelog);
    }
  }
}

void ClassicWriter::init(const std::string& username,
                         AccessLog::GetThreadDataFunc fn) {
  m_threadDataFn = fn;
  if (m_channel == LogChannel::CRONOLOG) {
    assertx(!m_logdata.file.empty());
    m_cronolog = std::make_unique<Cronolog>();
    if (m_logdata.file.find('%') != std::string::npos) {
      m_cronolog->m_template = m_logdata.file;
      m_cronolog->setPeriodicity();
      if (m_logdata.periodMultiplier) {
        m_cronolog->m_periodMultiple = m_logdata.periodMultiplier;
      }

      m_cronolog->m_linkName = m_logdata.symLink;
      Cronolog::changeOwner(username, m_logdata.symLink);
    } else {
      m_cronolog->m_file = fopen(m_logdata.file.c_str(), "a");
    }
  } else if (m_channel == LogChannel::REGULAR) {
    assertx(!m_logdata.file.empty());
    if (m_logdata.file[0] == '|') {
      std::string plog = m_logdata.file.substr(1);
      m_filelog = popen(plog.c_str(), "w");
    } else {
      m_filelog = fopen(m_logdata.file.c_str(), "a");
    }
    if (!m_filelog) {
      Logger::Error("Couldn't open access log file %s", m_logdata.file.c_str());
    }
  }
}

void ClassicWriter::write(Transport* transport, const VirtualHost* vhost) {
  auto outfile = getOutputFile();
  if (!outfile) return;
  char c;
  std::ostringstream out;
  const auto* format = m_logdata.format.c_str();
  FieldGenerator fieldGen{
    transport,
    vhost,
    (m_threadDataFn ? m_threadDataFn() : nullptr)
  };
  while ((c = *format++)) {
    if (c != '%') {
      out << c;
      continue;
    }
    if (parseConditions(format, transport->getResponseCode())) {
      std::string field;
      std::string arg = parseArgument(format);
      while (!std::isalpha(*format)) format++;
      if (fieldGen.gen<std::string>(*format++, arg, field)) {
        out << field;
      } else {
        out << '-';
      }
    } else {
      skipField(format);
      out << '-';
    }
  }
  out << std::endl;

  auto str = out.str();
  int nbytes = fwrite(str.data(), 1, str.size(), outfile);
  fflush(outfile);

  if (m_channel != LogChannel::REGULAR || m_logdata.file[0] != '|') {
    recordWriteAndMaybeDropCaches(outfile, nbytes);
  }
}

const std::string ClassicWriter::handle = "__classic";

bool ClassicWriter::parseConditions(const char*& format, int code) {
  bool wantMatch = true;
  if (*format == '!') {
    wantMatch = false;
    format++;
  } else if (!isdigit(*format)) {
    // No conditions
    return true;
  }
  char buf[4];
  buf[3] = '\0';

  bool matched = false;
  while (isdigit(*format)) {
    buf[0] = format[0];
    buf[1] = format[1];
    buf[2] = format[2];
    if (folly::to<int>(buf) == code) {
      matched = true;
      break;
    }
    format+=4;
  }
  while (!(*format == '{' || isalpha(*format))) {
    format++;
  }
  return wantMatch == matched;
}

std::string ClassicWriter::parseArgument(const char*& format) {
  if (*format != '{') return std::string();
  format++;
  const char *start = format;
  while (*format != '}') { format++; }
  std::string res(start, format - start);
  format++;
  return res;
}

void ClassicWriter::skipField(const char*& format) {
  // Skip argument
  if (*format == '{') {
    while (*format != '}') { format++; }
    format++;
  }
  // Find control letter
  while (!isalpha(*format)) { format++; }
  // Skip it
  format++;
}

static InitFiniNode registerClassicWriter(
  []() { AccessLogFileData::registerWriter(
      ClassicWriter::handle,
      [](const AccessLogFileData& alfd, LogChannel chan) {
        return std::make_unique<ClassicWriter>(alfd, chan);
      }
  );},
  InitFiniNode::When::ServerPreInit
);
///////////////////////////////////////////////////////////////////////////////
}
