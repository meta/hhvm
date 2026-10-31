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

#include "hphp/util/hash-map.h"
#include "hphp/util/hash-set.h"
#include "hphp/util/hdf.h"


#include <boost/container/flat_set.hpp>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
struct Variant;
struct IniSettingMap;
using ConfigMap = std::map<std::string, std::string>;
using ConfigFastMap = hphp_fast_string_map<std::string>;
using ConfigSet = std::set<std::string>;
// with comparer
using ConfigSetC = std::set<std::string, stdltistr>;
using ConfigMapC = std::map<std::string, std::string, stdltistr>;
using ConfigFlatSet = boost::container::flat_set<std::string>;
using ConfigIMap = hphp_string_imap<std::string>;
using ConfigIFastMap = hphp_fast_string_imap<std::string>;
using ConfigFastSet = hphp_fast_string_set;

struct Config {
  /*
   * Normalizes hdf string names to their ini counterparts
   *
   * We have special handling for a few hdf strings such as those containing
   * MySQL, Eval and IPv[4|6].
   */
  static std::string IniName(const Hdf& config,
                             const bool prepend_hhvm = true);
  static std::string IniName(const std::string& config,
                             const bool prepend_hhvm = true);

  static void ParseConfigFile(const std::string &filename, IniSettingMap &ini,
                              Hdf &hdf, const bool is_system = true);

  static void ParseIniFile(const std::string &filename,
                           const bool is_system = true);
  static void ParseIniFile(const std::string &filename, IniSettingMap &ini,
                           const bool constants_only = false,
                           const bool is_system = true);

  static void ParseHdfFile(const std::string &filename, Hdf &hdf);

  // Parse and process a .ini string (e.g., -d)
  static void ParseIniString(const std::string &iniStr, IniSettingMap &ini,
                             const bool constants_only = false);

  // Parse and process a .hdf string (e.g., -v)
  static void ParseHdfString(const std::string &hdfStr, Hdf &hdf);

  /**
   * Prefer the Bind() over the GetFoo() as it makes ini_get() work too.
   * These Bind()s should be used for ini settings. Specifically, they
   * should be used when the bound setting is needed before the main ini
   * processing pass. Unlike IniSetting::Bind, these bindings will fetch the
   * value in an ini setting if it is set otherwise it will use the defValue.
   */
  static void Bind(bool& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const bool defValue = false,
                   const bool prepend_hhvm = true);
  static void Bind(const char*& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const char *defValue = nullptr,
                   const bool prepend_hhvm = true);
  static void Bind(std::string& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const std::string defValue = "",
                   const bool prepend_hhvm = true);
  static void Bind(char& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const char defValue = 0, const bool prepend_hhvm = true);
  static void Bind(unsigned char& loc,const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const unsigned char defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(int16_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const int16_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(uint16_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const uint16_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(int32_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const int32_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(uint32_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const uint32_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(int64_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const int64_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(uint64_t& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const uint64_t defValue = 0,
                   const bool prepend_hhvm = true);
  static void Bind(double& loc, const IniSettingMap &ini,
                   const Hdf& config, const std::string& name = "",
                   const double defValue = 0,
                   const bool prepend_hhvm = true);
  static void
  Bind(std::vector<int32_t>& loc, const IniSettingMap& ini,
       const Hdf& config, const std::string& name = "",
       const std::vector<int32_t>& defValue = std::vector<int32_t>(),
       const bool prepend_hhvm = true);
  static void
  Bind(std::vector<std::string>& loc, const IniSettingMap& ini,
       const Hdf& config, const std::string& name = "",
       const std::vector<std::string>& defValue = std::vector<std::string>(),
       const bool prepend_hhvm = true);
  static void
  Bind(std::unordered_map<std::string, int>& loc,
       const IniSettingMap& ini, const Hdf& config,
       const std::string& name = "",
       const std::unordered_map<std::string, int>& defValue =
         std::unordered_map<std::string, int>{},
       const bool prepend_hhvm = true);
  static void Bind(ConfigMap& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigMap& defValue = ConfigMap(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigFastMap& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigFastMap& defValue = ConfigFastMap(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigMapC& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigMapC& defValue = ConfigMapC(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigSet& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigSet& defValue = ConfigSet(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigSetC& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigSetC& defValue = ConfigSetC(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigIMap& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigIMap& defValue = ConfigIMap(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigIFastMap& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigIFastMap& defValue = ConfigIFastMap(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigFlatSet& loc, const IniSettingMap& ini,
                   const Hdf& config, const std::string& name = "",
                   const ConfigFlatSet& defValue = ConfigFlatSet(),
                   const bool prepend_hhvm = true);
  static void Bind(ConfigFastSet& loc, const IniSettingMap& ini, const Hdf& config,
                   const std::string& name = "",
                   const ConfigFastSet& defValue = ConfigFastSet(),
                   const bool prepend_hhvm = true);

  /**
   * Master Get Methods to get values associated with an ini or hdf setting.
   * These methods just get the value. They do not bind to a variable for
   * enabling ini_get()
   */
  static bool GetBool(const IniSettingMap &ini, const Hdf& config,
                      const std::string& name = "",
                      const bool defValue = false,
                      const bool prepend_hhvm = true);
  static const char *Get(const IniSettingMap &ini, const Hdf& config,
                         const std::string& name = "",
                         const char *defValue = nullptr,
                         const bool prepend_hhvm = true);
  static std::string GetString(const IniSettingMap &ini, const Hdf& config,
                               const std::string& name = "",
                               const std::string defValue = "",
                               const bool prepend_hhvm = true);
  static char GetByte(const IniSettingMap &ini, const Hdf& config,
                      const std::string& name = "", const char defValue = 0,
                      const bool prepend_hhvm = true);
  static unsigned char GetUByte(const IniSettingMap &ini, const Hdf& config,
                                const std::string& name = "",
                                const unsigned char defValue = 0,
                                const bool prepend_hhvm = true);
  static int16_t GetInt16(const IniSettingMap &ini, const Hdf& config,
                          const std::string& name = "",
                          const int16_t defValue = 0,
                          const bool prepend_hhvm = true);
  static uint16_t GetUInt16(const IniSettingMap &ini, const Hdf& config,
                            const std::string& name = "",
                            const uint16_t defValue = 0,
                            const bool prepend_hhvm = true);
  static int32_t GetInt32(const IniSettingMap &ini, const Hdf& config,
                          const std::string& name = "",
                          const int32_t defValue = 0,
                          const bool prepend_hhvm = true);
  static uint32_t GetUInt32(const IniSettingMap &ini, const Hdf& config,
                            const std::string& name = "",
                            const uint32_t defValue = 0,
                            const bool prepend_hhvm = true);
  static int64_t GetInt64(const IniSettingMap &ini, const Hdf& config,
                          const std::string& name = "",
                          const int64_t defValue = 0,
                          const bool prepend_hhvm = true);
  static uint64_t GetUInt64(const IniSettingMap &ini, const Hdf& config,
                            const std::string& name = "",
                            const uint64_t defValue = 0,
                            const bool prepend_hhvm = true);
  static double GetDouble(const IniSettingMap &ini, const Hdf& config,
                          const std::string& name = "",
                          const double defValue = 0,
                          const bool prepend_hhvm = true);
  static std::vector<int32_t>
  GetInt32Vector(const IniSettingMap& ini, const Hdf& config,
                 const std::string& name = "",
                 const std::vector<int32_t>& def = std::vector<int32_t>{},
                 const bool prepend_hhvm = true);
  static std::vector<std::string>
  GetStrVector(const IniSettingMap& ini, const Hdf& config,
               const std::string& name = "",
               const std::vector<std::string>& def = std::vector<std::string>{},
               const bool prepend_hhvm = true);
  static std::unordered_map<std::string, int>
  GetIntMap(const IniSettingMap& ini, const Hdf& config,
            const std::string& name = "",
            const std::unordered_map<std::string, int>& defValue =
              std::unordered_map<std::string, int>{},
            const bool prepend_hhvm = true);
  static ConfigMap GetMap(const IniSettingMap& ini, const Hdf& config,
                          const std::string& name = "",
                          const ConfigMap& defValue = ConfigMap(),
                          const bool prepend_hhvm = true);
  static ConfigFastMap GetFastMap(const IniSettingMap& ini, const Hdf& config,
                                  const std::string& name = "",
                                  const ConfigFastMap& defValue = ConfigFastMap(),
                                  const bool prepend_hhvm = true);
  static ConfigMapC GetMapC(const IniSettingMap& ini, const Hdf& config,
                          const std::string& name = "",
                          const ConfigMapC& defValue = ConfigMapC(),
                          const bool prepend_hhvm = true);
  static ConfigSet GetSet(const IniSettingMap& ini, const Hdf& config,
                          const std::string& name = "",
                          const ConfigSet& defValue = ConfigSet(),
                          const bool prepend_hhvm = true);
  static ConfigSetC GetSetC(const IniSettingMap& ini, const Hdf& config,
                            const std::string& name = "",
                            const ConfigSetC& defValue = ConfigSetC(),
                            const bool prepend_hhvm = true);
  static ConfigIMap GetIMap(const IniSettingMap& ini, const Hdf& config,
                            const std::string& name = "",
                            const ConfigIMap& defValue = ConfigIMap(),
                            const bool prepend_hhvm = true);
  static ConfigIFastMap GetIFastMap(const IniSettingMap& ini, const Hdf& config,
                                    const std::string& name = "",
                                    const ConfigIFastMap& defValue = ConfigIFastMap(),
                                    const bool prepend_hhvm = true);
  static ConfigFlatSet GetFlatSet(const IniSettingMap& ini, const Hdf& config,
                                  const std::string& name = "",
                                  const ConfigFlatSet& defValue
                                    = ConfigFlatSet(),
                                  const bool prepend_hhvm = true);
  static ConfigFastSet GetFastSet(const IniSettingMap& ini, const Hdf& config,
                                  const std::string& name = "",
                                  const ConfigFastSet& defValue = ConfigFastSet(),
                                  const bool prepend_hhvm = true);

  /**
   * Use the Iterate method for iterating over options that are stored as
   * objects in runtime options (e.g. FilesMatch). This function iterates over
   * the settings passed as ini/hdf, calls back to, generally, the constructor
   * of the object in question.
   *
   * Note: For now, we are not `ini_get()` enabling these type of options as
   * it is not trivial to come up with a non-hacky and workable way to store
   * the data correctly. Also, as usual, Hdf takes priority.
   */
  static void Iterate(std::function<void (const IniSettingMap&,
                                          const Hdf&,
                                          const std::string&)> cb,
                      const IniSettingMap &ini, const Hdf& config,
                      const std::string &name, const bool prepend_hhvm = true);

  static bool matchHdfPattern(const std::string &value,
                              const IniSettingMap& ini, Hdf hdfPattern,
                              const std::string& name,
                              const std::string& suffix = "");

  static bool matchHdfPatternSet(const std::string &value,
                                 const IniSettingMap& ini, Hdf hdfPattern,
                                 const std::string& name);

  private:

  static void SetParsedIni(IniSettingMap &ini, const std::string confStr,
                           const std::string &filename, bool constants_only,
                           bool is_system);

  static void
  StringInsert(std::vector<std::string>& values, const std::string& /*key*/,
               const std::string& value) {
    values.push_back(value);
  }
  static void
  StringInsert(boost::container::flat_set<std::string>& values,
               const std::string& /*key*/, const std::string& value) {
    values.insert(value);
  }
  static void
  StringInsert(std::set<std::string, stdltistr>& values,
               const std::string& /*key*/, const std::string& value) {
    values.insert(value);
  }
  static void
  StringInsert(std::set<std::string>& values, const std::string& /*key*/,
               const std::string& value) {
    values.insert(value);
  }
  static void StringInsert(std::map<std::string, std::string> &values,
                           const std::string &key,
                           const std::string &value) {
    values[key] = value;
  }
  static void StringInsert(std::map<std::string, std::string,
                           stdltistr> &values,
                           const std::string &key,
                           const std::string &value) {
    values[key] = value;
  }
  static void StringInsert(hphp_string_imap<std::string> &values,
                           const std::string &key,
                           const std::string &value) {
    values[key] = value;
  }
  static void ReplaceIncludesWithIni(const std::string& original_ini_filename,
                                     const std::string& iniStr,
                                     std::string& with_includes);
};

}
