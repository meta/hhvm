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

#include "hphp/util/hash-set.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/container/flat_set.hpp>

namespace HPHP {

struct Hdf;
struct IniSettingMap;
struct RepoGlobalData;
struct RepoOptionsFlags;

namespace hackc {
struct HhbcFlags;
struct ParserFlags;
}

namespace Cfg {

enum class InterpDispatchMode : uint8_t {
  ComputedGoto,
  TailCall,
  Alternate,
};

extern InterpDispatchMode interpDispatchMode;

void Load(const IniSettingMap& ini, const Hdf& config);
void LoadForCompiler(const IniSettingMap& ini, const Hdf& config);
void LoadFromGlobalData(const RepoGlobalData& gd);
void LoadFromGlobalDataOnlyHHBBC(const RepoGlobalData& gd);
void StoreToGlobalData(RepoGlobalData& gd);

void GetRepoOptionsFlags(RepoOptionsFlags& flags, const IniSettingMap& ini, const Hdf& config);
void GetRepoOptionsFlagsFromConfig(RepoOptionsFlags& flags, const Hdf& config,
                                   const RepoOptionsFlags& default_flags);
void GetRepoOptionsFlagsForSystemlib(RepoOptionsFlags& flags);
void InitHackcHHBCFlags(const RepoOptionsFlags& repo_flags, hackc::HhbcFlags& flags);
void InitHackcParserFlags(const RepoOptionsFlags& repo_flags, hackc::ParserFlags& flags);

using Int32Vector = std::vector<int32_t>;
using StringStringMap = std::map<std::string, std::string>;
using StringVector = std::vector<std::string>;
using StringSet = std::set<std::string>;
using StringBoostFlatSet = boost::container::flat_set<std::string>;
using StringHphpFastSet = hphp_fast_set<std::string>;

}

}
