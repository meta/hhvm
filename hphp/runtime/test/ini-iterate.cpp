#include <gtest/gtest.h>

#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/ini-setting.h"

#include "hphp/util/configs/jit.h"

namespace HPHP {

TEST(IniSetting, ini_iterate) {
  std::string inistr =
    "hhvm.ip_block_map[0][location] = /test\n"
    "hhvm.ip_block_map[0][allow_first] = true\n"
    "hhvm.ip_block_map[0][ip][allow][0] = 127.0.0.1\n"
    "hhvm.ip_block_map[0][ip][deny][0] = 8.32.0.0/24\n"
    "hhvm.ip_block_map[0][ip][deny][1] = "
    "aaaa:bbbb:cccc:dddd:eeee:ffff:1111::/80\n"
    "hhvm.ip_block_map[1][location] = /blah\n"
    "hhvm.ip_block_map[1][allow_first] = true\n"
    "hhvm.ip_block_map[1][ip][allow][0] = 127.10.10.10\n"
    "hhvm.ip_block_map[1][ip][deny][0] = 255.255.255.255\n"
    "hhvm.server.apc.ttl_limit = 1000\n"
    "hhvm.jit_mature_size = 22222222\n";

  IniSettingMap ini;
  Config::ParseIniString(inistr, ini);

  auto value = ini_iterate(ini, "hhvm.ip_block_map.0.location");
  EXPECT_EQ("/test", value.toString().toCppString());
  value = ini_iterate(ini, "hhvm.ip_block_map.1.ip.allow.0");
  EXPECT_EQ("127.10.10.10", value.toString().toCppString());
  value = ini_iterate(ini, "hhvm.server.apc.ttl_limit");
  EXPECT_EQ("1000", value.toString().toCppString());
  value = ini_iterate(ini, "hhvm.server.bogus.ttl_limit");
  EXPECT_TRUE(value.isNull());
  value = ini_iterate(ini, "hhvm.ip_block_map.0.ip.deny.0");
  EXPECT_EQ("8.32.0.0/24", value.toString().toCppString());
  value = ini_iterate(ini, "hhvm.ip_block_map.0.ip.deny.1");
  EXPECT_EQ("aaaa:bbbb:cccc:dddd:eeee:ffff:1111::/80",
            value.toString().toCppString());
  value = ini_iterate(ini, "hhvm.ip_block_map.1.ip.allow.2");
  EXPECT_TRUE(value.isNull());
  value = ini_iterate(init_null(), "hhvm.ip_block_map");
  EXPECT_TRUE(value.isNull());
  value = ini_iterate(ini, "hhvm.ip_block_map");
  EXPECT_TRUE(value.isArray());
  EXPECT_EQ(2, value.toArray().size());

  // Check some runtime options
  EXPECT_EQ(22222222, Cfg::Jit::MatureSize);
}

}
