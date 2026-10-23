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

#include "hphp/runtime/base/config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>

#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/preg.h"
#include "hphp/util/logger.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

namespace {

template<typename T>
void hdfConfigGetSet(const Hdf& hdf, T& values) {
  values.clear();
  for (Hdf child = hdf.firstChild(); child.exists(); child = child.next()) {
    values.insert(child.configGetString(""));
  }
}

template<typename T>
void hdfConfigGetMap(const Hdf& hdf, T& values) {
  values.clear();
  for (Hdf child = hdf.firstChild(); child.exists(); child = child.next()) {
    values[child.getName()] = child.configGetString("");
  }
}

template<typename T>
void hdfConfigGet(const Hdf& hdf, T& values) {
  hdf.configGet(values);
}

void hdfConfigGet(const Hdf& hdf, boost::container::flat_set<std::string>& values) {
  hdfConfigGetSet(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, std::set<std::string, stdltistr>& values) {
  hdfConfigGetSet(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, hphp_fast_string_set& values) {
  hdfConfigGetSet(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, std::map<std::string, std::string,
                    stdltistr>& values) {
  hdfConfigGetMap(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, hphp_string_imap<std::string>& values) {
  hdfConfigGetMap(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, hphp_fast_string_map<std::string>& values) {
  hdfConfigGetMap(hdf, values);
}

void hdfConfigGet(const Hdf& hdf, hphp_fast_string_imap<std::string>& values) {
  hdfConfigGetMap(hdf, values);
}

}

std::string
Config::IniName(const Hdf& config, bool /*prepend_hhvm*/ /* = true */) {
  return Config::IniName(config.getFullPath());
}

std::string Config::IniName(const std::string& config,
                            bool prepend_hhvm /* = true */) {
  std::string out;
  if (prepend_hhvm) {
    out += "hhvm.";
  }
  size_t idx = 0;
  for (auto& c : config) {
    // This is the first or last character
    if (idx == 0 || idx == config.length() - 1) {
      out += tolower(c);
    } else if (!isalpha(c)) {
      // Any . or _ or numeral is just output with no special behavior
      out += c;
    } else {
      if (isupper(c) && isupper(config[idx - 1 ]) && islower(config[idx + 1])) {
      // Handle something like "SSLPort", and c = "P", which will then put
      // the underscore between the "L" and "P"
        out += "_";
        out += tolower(c);
      } else if (islower(c) && isupper(config[idx + 1])) {
      // Handle something like "PathDebug", and c = "h", which will then put
      // the underscore between the "h" and "D"
        out += tolower(c);
        out += "_";
      } else {
      // Otherwise we just output as lower
        out += tolower(c);
      }
    }
    idx++;
  }

  // The HHVM ini option becomes the standard PHP option.
  boost::replace_first(out,
                       "hhvm.server.upload.max_file_uploads",
                       "max_file_uploads");
  // Make sure IPv6 or IPv4 are handled correctly
  boost::replace_first(out, "_i_pv", "_ipv");
  boost::replace_first(out, ".i_pv", ".ipv");
  // urls are special too. Let's not have "ur_ls"
  boost::replace_first(out, "_ur_ls", "_urls");
  boost::replace_first(out, ".ur_ls", ".urls");
  // No use of Eval in our ini strings
  boost::replace_first(out, ".eval.", ".");
  boost::replace_first(out, ".my_sql.", ".mysql.");

  return out;
}

void Config::ParseIniString(const std::string &iniStr, IniSettingMap &ini,
                            const bool constants_only /* = false */ ) {
  Config::SetParsedIni(ini, iniStr, "", constants_only, true);
}

void Config::ParseHdfString(const std::string &hdfStr, Hdf &hdf) {
  hdf.fromString(hdfStr);
}

void Config::ParseConfigFile(const std::string &filename, IniSettingMap &ini,
                             Hdf &hdf, const bool is_system /* = true */) {
  // We don't allow a filename of just ".ini"
  if (boost::ends_with(filename, ".ini") && filename.length() > 4) {
    Config::ParseIniFile(filename, ini, false, is_system);
  } else {
    // For now, assume anything else is an hdf file
    // TODO(#5151773): Have a non-invasive warning if HDF file does not end
    // .hdf
    Config::ParseHdfFile(filename, hdf);
    // Store the filename in the hdf object.
    const std::string node_name = "Metadata.ConfigFileName";
    hdf.set(node_name, filename);
  }
}

void Config::ParseIniFile(const std::string &filename,
                          const bool is_system /* = true */) {
  IniSettingMap ini = IniSettingMap();
  Config::ParseIniFile(filename, ini, false, is_system);
}

void Config::ParseIniFile(const std::string &filename, IniSettingMap &ini,
                          const bool constants_only /* = false */,
                          const bool is_system /* = true */ ) {
    std::ifstream ifs(filename);
    std::string str((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    std::string with_includes;
    Config::ReplaceIncludesWithIni(filename, str, with_includes);
    Config::SetParsedIni(ini, with_includes, filename, constants_only,
                         is_system);
}

void Config::ReplaceIncludesWithIni(const std::string& original_ini_filename,
                                    const std::string& iniStr,
                                    std::string& with_includes) {
  std::istringstream iss(iniStr);
  std::string line;
  while (std::getline(iss, line)) {
    // Handle cases like
    //   #include           ""
    //   ##includefoo barbaz"myconfig.ini" how weird is that
    // Anything that is not a syntactically correct #include "file" after
    // this pre-processing, will be treated as an ini comment and processed
    // as such in the ini parser
    auto pos = line.find_first_not_of(' ');
    if (pos == std::string::npos ||
        line.compare(pos, strlen("#include"), "#include") != 0) {
      // treat as normal ini line, including comment that doesn't start with
      // #include
      with_includes += line + "\n";
      continue;
    }
    pos += strlen("#include");
    auto start = line.find_first_not_of(' ', pos);
    auto end = line.find_last_not_of(' ');
    if ((start == std::string::npos || line[start] != '"') ||
        (end == start || line[end] != '"')) {
      with_includes += line + "\n"; // treat as normal comment
      continue;
    }
    std::string file = line.substr(start + 1, end - start - 1);
    const std::string logger_file = file;
    std::filesystem::path p(file);
    if (!p.is_absolute()) {
      std::filesystem::path opath(original_ini_filename);
      p = opath.parent_path()/p;
    }
    if (std::filesystem::exists(p)) {
      std::ifstream ifs(p.string());
      const std::string contents((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
      Config::ReplaceIncludesWithIni(p.string(), contents, with_includes);
    } else {
      Logger::Warning("ini include file %s not found", logger_file.c_str());
    }
  }
}

void Config::ParseHdfFile(const std::string &filename, Hdf &hdf) {
  hdf.append(filename);
}

void Config::SetParsedIni(IniSettingMap &ini, const std::string confStr,
                          const std::string &filename, bool constants_only,
                          bool is_system) {
  // if we are setting constants, we must be setting system settings
  if (constants_only) {
    assertx(is_system);
  }
  auto parsed_ini = IniSetting::FromStringAsMap(confStr, filename);
  for (ArrayIter iter(parsed_ini.toArray()); iter; ++iter) {
    // most likely a string, but just make sure that we are dealing
    // with something that can be converted to a string
    assertx(iter.first().isScalar());
    ini.set(iter.first().toString(), iter.second());
    if (constants_only) {
      IniSetting::FillInConstant(iter.first().toString().toCppString(),
                                 iter.second());
    } else if (is_system) {
      IniSetting::SetSystem(iter.first().toString().toCppString(),
                            iter.second());
    }
  }
}

// This method must return a char* which is owned by the IniSettingMap
// to avoid issues with the lifetime of the char*
const char* Config::Get(const IniSettingMap &ini, const Hdf& config,
                        const std::string& name /* = "" */,
                        const char *defValue /* = nullptr */,
                        const bool prepend_hhvm /* = true */) {
  auto ini_name = IniName(name, prepend_hhvm);
  Hdf hdf = name != "" ? config[name] : config;
  auto value = ini_iterate(ini, ini_name);
  if (value.isString()) {
    // See generic Get##METHOD below for why we are doing this
    // Note that value is a string, so value.toString() is not
    // a temporary.
    const char* ini_ret = value.toString().data();
    const char* hdf_ret = hdf.configGet(ini_ret);
    if (hdf_ret != ini_ret) {
      ini_ret = hdf_ret;
      IniSetting::SetSystem(ini_name, ini_ret);
    }
    return ini_ret;
  }
  return hdf.configGet(defValue);
}

template<class T> static T variant_init(T v) {
    return v;
}
static int64_t variant_init(uint32_t v) {
    return v;
}

#define CONFIG_BODY(T, METHOD) \
T Config::Get##METHOD(const IniSetting::Map &ini, const Hdf& config, \
                      const std::string &name /* = "" */, \
                      const T defValue /* = 0ish */, \
                      const bool prepend_hhvm /* = true */) { \
  auto ini_name = IniName(name, prepend_hhvm); \
  /* If we don't pass a name, then we just use the raw config as-is. */ \
  /* This could happen when we are at a known leaf of a config node. */ \
  Hdf hdf = name != "" ? config[name] : config; \
  auto value = ini_iterate(ini, ini_name); \
  if (value.isString()) { \
    T ini_ret, hdf_ret; \
    ini_on_update(value.toString(), ini_ret); \
    /* I don't care what the ini_ret was if it isn't equal to what  */ \
    /* is returned back from from an HDF get call, which it will be */ \
    /* if the call just passes back ini_ret because either they are */ \
    /* the same or the hdf option associated with this name does    */ \
    /* not exist.... REMEMBER HDF WINS OVER INI UNTIL WE WIPE HDF   */ \
    hdf_ret = hdf.configGet##METHOD(ini_ret); \
    if (hdf_ret != ini_ret) { \
      ini_ret = hdf_ret; \
      IniSetting::SetSystem(ini_name, variant_init(ini_ret)); \
    } \
    return ini_ret; \
  } \
  /* If there is a value associated with this setting in the hdf config */ \
  /* then return it; otherwise the defValue will be returned as it is   */ \
  /* assigned to the return value for this call when nothing exists     */ \
  return hdf.configGet##METHOD(defValue); \
} \
void Config::Bind(T& loc, const IniSetting::Map &ini, const Hdf& config, \
                  const std::string& name /* = "" */, \
                  const T defValue /* = 0ish */, \
                  const bool prepend_hhvm /* = true */) { \
  loc = Get##METHOD(ini, config, name, defValue, prepend_hhvm); \
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config, \
                   IniName(name, prepend_hhvm), &loc); \
}

CONFIG_BODY(bool, Bool)
CONFIG_BODY(char, Byte)
CONFIG_BODY(unsigned char, UByte)
CONFIG_BODY(int16_t, Int16)
CONFIG_BODY(uint16_t, UInt16)
CONFIG_BODY(int32_t, Int32)
CONFIG_BODY(uint32_t, UInt32)
CONFIG_BODY(int64_t, Int64)
CONFIG_BODY(uint64_t, UInt64)
CONFIG_BODY(double, Double)
CONFIG_BODY(std::string, String)

#define CONTAINER_CONFIG_BODY(T, METHOD) \
T Config::Get##METHOD(const IniSetting::Map& ini, const Hdf& config, \
                      const std::string& name /* = "" */, \
                      const T& defValue /* = T() */, \
                      const bool prepend_hhvm /* = true */) { \
  auto ini_name = IniName(name, prepend_hhvm); \
  Hdf hdf = name != "" ? config[name] : config; \
  T ini_ret, hdf_ret; \
  auto value = ini_iterate(ini, ini_name); \
  if (value.isArray() || value.isObject()) { \
    ini_on_update(value.toVariant(), ini_ret); \
    /** Make sure that even if we have an ini value, that if we also **/ \
    /** have an hdf value, that it maintains its edge as beating out **/ \
    /** ini                                                          **/ \
    if (hdf.exists() && !hdf.isEmpty()) { \
      hdfConfigGet(hdf, hdf_ret); \
      if (hdf_ret != ini_ret) { \
        ini_ret = hdf_ret; \
        IniSetting::SetSystem(ini_name, ini_get(ini_ret)); \
      } \
    } \
    return ini_ret; \
  } \
  if (hdf.exists() && !hdf.isEmpty()) { \
    hdfConfigGet(hdf, hdf_ret); \
    return hdf_ret; \
  } \
  return defValue; \
} \
void Config::Bind(T& loc, const IniSetting::Map& ini, const Hdf& config, \
                  const std::string& name /* = "" */, \
                  const T& defValue /* = T() */, \
                  const bool prepend_hhvm /* = true */) { \
  loc = Get##METHOD(ini, config, name, defValue, prepend_hhvm); \
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config, \
                   IniName(name, prepend_hhvm), &loc); \
}

CONTAINER_CONFIG_BODY(std::vector<int32_t>, Int32Vector)
CONTAINER_CONFIG_BODY(std::vector<std::string>, StrVector)
namespace { using simap = std::unordered_map<std::string, int>; }
CONTAINER_CONFIG_BODY(simap, IntMap)
CONTAINER_CONFIG_BODY(ConfigMap, Map)
CONTAINER_CONFIG_BODY(ConfigFastMap, FastMap)
CONTAINER_CONFIG_BODY(ConfigMapC, MapC)
CONTAINER_CONFIG_BODY(ConfigSet, Set)
CONTAINER_CONFIG_BODY(ConfigSetC, SetC)
CONTAINER_CONFIG_BODY(ConfigFlatSet, FlatSet)
CONTAINER_CONFIG_BODY(ConfigIMap, IMap)
CONTAINER_CONFIG_BODY(ConfigIFastMap, IFastMap)
CONTAINER_CONFIG_BODY(ConfigFastSet, FastSet)

// No `ini` binding yet. Hdf still takes precedence but will be removed
// once we have made all options ini-aware. All new settings should
// use the ini path of this method (i.e., pass a bogus Hdf or keep it null)
void Config::Iterate(std::function<void (const IniSettingMap&,
                                         const Hdf&,
                                         const std::string&)> cb,
                     const IniSettingMap &ini, const Hdf& config,
                     const std::string &name,
                     const bool prepend_hhvm /* = true */) {
  Hdf hdf = name.empty() ? config : config[name];
  if (hdf.exists() && !hdf.isEmpty()) {
    for (Hdf c = hdf.firstChild(); c.exists(); c = c.next()) {
      cb(IniSetting::Map::object, c, "");
    }
  } else {
    Hdf empty;
    auto ini_value = name.empty() ? ini :
      ini_iterate(ini, IniName(name, prepend_hhvm));
    if (ini_value.isArray()) {
      for (ArrayIter iter(ini_value.toArray()); iter; ++iter) {
        cb(iter.second(), empty, iter.first().toString().toCppString());
      }
    }
  }
}

bool valueMatchesPattern(const std::string &value,
                         std::string &pattern,
                         const std::string &suffix) {
  if (!suffix.empty()) pattern += suffix;
  auto const patternString = OptString(
    pattern.c_str(), pattern.size(), CopyString
  );
  auto const error = preg_get_error_message_if_invalid(patternString);
  if (!error.isNull()) {
    throw std::runtime_error(fmt::format(
      "Invalid configuration regex '{}': {}",
      pattern,
      error.toCppString()
    ));
  }
  Variant ret = preg_match(patternString,
                           OptString(value.c_str(), value.size(),
                                     CopyString));
  return ret.toInt64() > 0;
}

bool Config::matchHdfPattern(const std::string &value,
                             const IniSettingMap& ini, Hdf hdfPattern,
                             const std::string& name,
                             const std::string& suffix) {
  std::string pattern = Config::GetString(ini, hdfPattern, name, "", false);
  if (!pattern.empty()) {
    if (!valueMatchesPattern(value, pattern, suffix)) {
      return false;
    }
  }
  return true;
}

bool Config::matchHdfPatternSet(const std::string &value,
                                const IniSettingMap& ini, Hdf hdfPattern,
                                const std::string& name) {
  std::vector<std::string> patterns = Config::GetStrVector(ini,
                                                           hdfPattern,
                                                           name,
                                                           std::vector<std::string>{},
                                                           false);
  auto matched = true;
  if (!patterns.empty()) {
    for (std::string pattern: patterns) {
      if (!pattern.empty()) {
        // PatternSets are applied only to multiline values
        matched &= valueMatchesPattern(value, pattern, "m");
      }
    }
  }
  return matched;
}

}
