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

#include "hphp/runtime/base/stats.h"
#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>
#include <fmt/format.h>

#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/string-data.h"

namespace HPHP {
namespace Stats {

TRACE_SET_MOD(stats)

const char* g_counterNames[] = {
#include "hphp/runtime/vm/stats-opcodeDef.h"
#define STAT(s) #s ,
  STATS
#undef STAT
#undef O
};

using StatGroupMap = hphp_const_char_map<hphp_const_char_map<uint64_t>>;

RDS_LOCAL(StatCounters, rl_counters);
RDS_LOCAL(StatGroupMap*, rl_stat_groups);

void init() {
  if (!enabledAny()) return;
  assertx(*rl_stat_groups == nullptr);
  *rl_stat_groups = new StatGroupMap();
}

static RDS_LOCAL(int64_t, epoch);
void dump() {
  if (!enabledAny()) return;

  auto url = g_context->getRequestUrl(50);
  TRACE(0, "STATS %" PRId64 " %s\n", *epoch, url.c_str());
#include "hphp/runtime/vm/stats-opcodeDef.h"
#define STAT(s) \
  if (!rl_counters->counters[s]) {} else                                  \
    TRACE(0, "STAT %-50s %15" PRId64 "\n", #s, rl_counters->counters[s]);
  STATS
#undef STAT
#undef O

  using StatPair = std::pair<const char*, uint64_t>;
  for (auto const& group : **rl_stat_groups) {
    std::string stats;
    auto const& map = group.second;
    uint64_t total = 0, accum = 0;
    size_t nameWidth = 0;

    std::vector<StatPair> rows(map.begin(), map.end());
    for (auto const& p : rows) {
      nameWidth = std::max(nameWidth, strlen(p.first));
      total += p.second;
    }
    auto gt = [](const StatPair& a, const StatPair& b) {
      return a.second > b.second;
    };
    std::sort(rows.begin(), rows.end(), gt);

    fmt::format_to(std::back_inserter(stats), "{:-^80}\n",
                  fmt::format(" group {} ",group.first, url));
    fmt::format_to(std::back_inserter(stats),
                   "{:>{}}   {:>9} {:>8} {:>8}\n",
                   "name", nameWidth + 2, "count", "% total", "accum %");

    static const auto maxGroupEnv = getenv("HHVM_STATS_GROUPMAX");
    static const auto maxGroup = maxGroupEnv ? atoi(maxGroupEnv) : INT_MAX;

    int counter = 0;
    auto const percent = [](double val) {
      return fmt::format("{:.2f}%", val * 100);
    };
    for (auto const& row : rows) {
      accum += row.second;
      fmt::format_to(std::back_inserter(stats),
                     "{:>{}} : {:9} {:>8} {:>8}\n",
                     row.first, nameWidth + 2, row.second,
                     percent((double)row.second / total),
                     percent((double)accum / total));
      if (++counter >= maxGroup) break;
    }
    FTRACE(0, "{}\n", stats);
  }
}

void clear() {
  if (!enabledAny()) return;
  ++*epoch;
  memset(&rl_counters->counters[0], 0, sizeof(rl_counters->counters));

  assertx(*rl_stat_groups);
  delete *rl_stat_groups;
  *rl_stat_groups = nullptr;
}

void incStatGrouped(const StringData* category, const StringData* name, int n) {
  assertx(*rl_stat_groups);
  (**rl_stat_groups)[category->data()][name->data()] += n;
}

} }
