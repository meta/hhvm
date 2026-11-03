#include "hphp/runtime/server/request-body-registry.h"
#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/ini-setting.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <folly/portability/GTest.h>

namespace HPHP {
namespace {

using Registry = RequestBodyRegistry;

Registry::Profile profile(Registry::PathMode mode = Registry::PathMode::WWW) {
  Registry::Route route;
  route.virtualHosts = {"default", "alias"};
  route.hosts = {"api.example.test", "api.*.example.test"};
  route.path = "/v1/items/{item}/uploads";
  route.mode = mode;
  Registry::Profile result;
  result.name = "uploads";
  result.routes.push_back(std::move(route));
  return result;
}

void configureProfile(Hdf& hdf) {
  auto config = hdf["Server"]["RequestBodyReaders"][0];
  config["Name"] = "uploads";
  auto route = config["Routes"][0];
  route["VirtualHosts"][0] = "default";
  route["Hosts"][0] = "api.example.test";
  route["Hosts"][1] = "api.alias.example.test";
  route["Path"] = "/v1/items/{item}/uploads";
}

TEST(RequestBodyRegistryTest, configuration_loader_preserves_defaults_and_limits) {
  IniSettingMap ini;
  Hdf hdf;
  EXPECT_TRUE(Registry::load(ini, hdf)->empty());
  configureProfile(hdf);
  auto registry = Registry::load(ini, hdf);
  auto match = registry->select("default", "api.example.test", "POST",
                                "/v1/items/a/uploads", true);
  ASSERT_TRUE(match);
  EXPECT_EQ("uploads", match->profile->name);
  EXPECT_EQ(65536, match->profile->highWaterMark);
  EXPECT_EQ(1, match->profile->capacity);
  auto alias = registry->select("default", "api.alias.example.test", "POST",
                                "/v1/items/b/uploads", true);
  ASSERT_TRUE(alias);
  EXPECT_EQ(match->profile, alias->profile);
  EXPECT_FALSE(registry->select("default", "api.example.test", "POST",
                                "/V1/ITEMS/a/UPLOADS", true));

  auto config = hdf["Server"]["RequestBodyReaders"][0];
  config["QueueHighWaterMark"] = 24;
  config["Capacity"] = 2;
  config["Routes"][0]["PathMode"] = "www";
  registry = Registry::load(ini, hdf);
  match = registry->select("default", "api.example.test", "POST",
                           "/V1/ITEMS/a/UPLOADS", true);
  ASSERT_TRUE(match);
  EXPECT_EQ(24, match->profile->highWaterMark);
  EXPECT_EQ(2, match->profile->capacity);
}

TEST(RequestBodyRegistryTest, configuration_loader_refuses_invalid_values) {
  IniSettingMap ini;
  for (auto const& [field, value] :
       std::vector<std::pair<std::string, std::string>>{
         {"QueueHighWaterMark", "0"}, {"QueueHighWaterMark", "2147483648"},
         {"QueueHighWaterMark", "4294967297"}, {"QueueHighWaterMark", "-1"},
         {"Capacity", "0"}, {"Capacity", "4294967296"},
         {"Capacity", "4294967297"}, {"Capacity", "-1"}}) {
    SCOPED_TRACE(field + "=" + value);
    Hdf hdf;
    configureProfile(hdf);
    hdf["Server"]["RequestBodyReaders"][0][field] = value;
    EXPECT_THROW(Registry::load(ini, hdf), std::invalid_argument);
  }
  Hdf hdf;
  configureProfile(hdf);
  auto route = hdf["Server"]["RequestBodyReaders"][0]["Routes"][0];
  route["PathMode"] = "unsupported";
  EXPECT_THROW(Registry::load(ini, hdf), std::invalid_argument);
  route["PathMode"] = "raw";
  route["VirtualHosts"][0] = "not_a_configured_request_body_virtual_host";
  EXPECT_THROW(Registry::load(ini, hdf), std::invalid_argument);
}

TEST(RequestBodyRegistryTest, ini_configuration_loads_nested_profile_and_route) {
  IniSettingMap ini;
  Config::ParseIniString(
    "hhvm.server.request_body_readers[uploads][name] = uploads\n"
    "hhvm.server.request_body_readers[uploads][queue_high_water_mark] = 24\n"
    "hhvm.server.request_body_readers[uploads][capacity] = 2\n"
    "hhvm.server.request_body_readers[uploads][routes][only][virtual_hosts][0] = default\n"
    "hhvm.server.request_body_readers[uploads][routes][only][hosts][0] = api.example.test\n"
    "hhvm.server.request_body_readers[uploads][routes][only][path] = /v1/items/{item}/uploads\n"
    "hhvm.server.request_body_readers[uploads][routes][only][path_mode] = www\n",
    ini);
  Hdf hdf;
  auto registry = Registry::load(ini, hdf);
  auto match = registry->select("default", "API.EXAMPLE.TEST:443", "POST",
                                "/V1/ITEMS/a/UPLOADS/", true);
  ASSERT_TRUE(match);
  EXPECT_EQ("uploads", match->profile->name);
  EXPECT_EQ(24, match->profile->highWaterMark);
  EXPECT_EQ(2, match->profile->capacity);
  EXPECT_EQ(std::vector<std::string>{"a"}, match->segments);
}

TEST(RequestBodyRegistryTest, distinct_profiles_select_their_own_routes) {
  auto first = profile();
  auto second = profile();
  second.name = "other";
  second.routes.front().hosts = {"other.example.test"};
  Registry hosts({first, second});
  auto a = hosts.select("default", "api.example.test", "POST",
                         "/v1/items/a/uploads", true);
  auto b = hosts.select("default", "other.example.test", "POST",
                         "/v1/items/b/uploads", true);
  ASSERT_TRUE(a && b);
  EXPECT_EQ("uploads", a->profile->name);
  EXPECT_EQ("other", b->profile->name);
  EXPECT_NE(a->profile, b->profile);

  second.routes.front().hosts = first.routes.front().hosts;
  for (auto const& [left, right, leftTarget, rightTarget] :
       std::vector<std::array<std::string, 4>>{
         {"/x/{a}", "/y/{b}", "/x/one", "/y/two"},
         {"/a/{x}/b", "/a/{y}/c", "/a/one/b", "/a/two/c"}}) {
    first.routes.front().path = left;
    second.routes.front().path = right;
    Registry paths({first, second});
    a = paths.select("default", "api.example.test", "POST", leftTarget, true);
    b = paths.select("default", "api.example.test", "POST", rightTarget, true);
    ASSERT_TRUE(a && b);
    EXPECT_EQ("uploads", a->profile->name);
    EXPECT_EQ("other", b->profile->name);
    EXPECT_NE(a->profile, b->profile);
  }
}

TEST(RequestBodyRegistryTest, literal_prefix_neighbors_share_profile_admission) {
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    SCOPED_TRACE(mode == Registry::PathMode::Raw ? "raw" : "www");
    auto config = profile(mode);
    auto const base = config.routes.front();
    config.routes.clear();
    for (auto const path : {"/upload", "/uploads", "/uploads/status"}) {
      auto route = base;
      route.path = path;
      config.routes.push_back(std::move(route));
    }
    Registry registry({config});
    std::shared_ptr<RequestBodyProfile> shared;
    for (auto const path : {"/upload", "/uploads", "/uploads/status"}) {
      auto match = registry.select("default", "api.example.test", "POST",
                                    path, true);
      ASSERT_TRUE(match);
      EXPECT_EQ(path, match->path);
      EXPECT_TRUE(match->segments.empty());
      if (!shared) shared = match->profile;
      EXPECT_EQ(shared, match->profile);
    }
    auto alias = registry.select("alias", "api.beta.example.test", "POST",
                                  "/uploads", true);
    ASSERT_TRUE(alias);
    EXPECT_EQ(shared, alias->profile);
    ASSERT_TRUE(shared->tryAcquire());
    EXPECT_FALSE(alias->profile->tryAcquire());
    shared->release();
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                  "/uploaded", true));
    auto folded = registry.select("default", "api.example.test", "POST",
                                   "/UPLOADS/", true);
    if (mode == Registry::PathMode::WWW) {
      ASSERT_TRUE(folded);
      EXPECT_EQ("/uploads", folded->path);
      EXPECT_TRUE(folded->segments.empty());
    } else {
      EXPECT_FALSE(folded);
    }
  }
}

TEST(RequestBodyRegistryTest, literal_neighbors_in_distinct_profiles_are_independent) {
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    auto first = profile(mode);
    auto second = profile(mode);
    first.routes.front().path = "/upload";
    second.routes.front().path = "/uploads";
    second.name = "other";
    Registry registry({first, second});
    auto a = registry.select("default", "api.example.test", "POST",
                              "/upload", true);
    auto b = registry.select("default", "api.example.test", "POST",
                              "/uploads", true);
    ASSERT_TRUE(a && b);
    EXPECT_NE(a->profile, b->profile);
    ASSERT_TRUE(a->profile->tryAcquire());
    ASSERT_TRUE(b->profile->tryAcquire());
    a->profile->release();
    b->profile->release();
  }
}

TEST(RequestBodyRegistryTest, duplicate_literals_and_www_case_variants_are_refused) {
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    auto config = profile(mode);
    config.routes.front().path = "/upload";
    config.routes.push_back(config.routes.front());
    EXPECT_THROW(Registry({config}), std::invalid_argument);
  }
  auto www = profile();
  www.routes.front().path = "/Upload";
  auto variant = www.routes.front();
  variant.path = "/uPLOAD";
  www.routes.push_back(std::move(variant));
  EXPECT_THROW(Registry({www}), std::invalid_argument);

  auto raw = profile(Registry::PathMode::Raw);
  raw.routes.front().path = "/Upload";
  variant = raw.routes.front();
  variant.path = "/upload";
  raw.routes.push_back(std::move(variant));
  Registry registry({raw});
  for (auto const path : {"/Upload", "/upload"}) {
    auto match = registry.select("default", "api.example.test", "POST",
                                  path, true);
    ASSERT_TRUE(match);
    EXPECT_EQ(path, match->path);
  }
}

TEST(RequestBodyRegistryTest, raw_capture_case_variants_select_own_profiles) {
  for (auto const& [left, right, leftTarget, rightTarget] :
       std::vector<std::array<std::string, 4>>{
         {"/Upload/{id}", "/upload/{id}", "/Upload/One", "/upload/Two"},
         {"/items/{id}/Upload", "/items/{id}/upload",
          "/items/One/Upload", "/items/Two/upload"}}) {
    for (auto const reversed : {false, true}) {
      SCOPED_TRACE(testing::Message() << left << ":" << reversed);
      auto first = profile(Registry::PathMode::Raw);
      auto second = profile(Registry::PathMode::Raw);
      first.routes.front().path = left;
      second.routes.front().path = right;
      second.name = "other";
      auto configs = std::vector<Registry::Profile>{first, second};
      if (reversed) std::reverse(configs.begin(), configs.end());
      Registry registry(std::move(configs));
      auto a = registry.select("default", "api.example.test", "POST",
                                leftTarget, true);
      auto b = registry.select("default", "api.example.test", "POST",
                                rightTarget, true);
      ASSERT_TRUE(a && b);
      EXPECT_EQ("uploads", a->profile->name);
      EXPECT_EQ("other", b->profile->name);
      EXPECT_NE(a->profile, b->profile);
      EXPECT_EQ(left, a->path);
      EXPECT_EQ(right, b->path);
      EXPECT_EQ(std::vector<std::string>{"One"}, a->segments);
      EXPECT_EQ(std::vector<std::string>{"Two"}, b->segments);
    }
  }
}

TEST(RequestBodyRegistryTest, capture_case_variants_preserve_overlap_guards) {
  for (auto const& [left, right] :
       std::vector<std::pair<std::string, std::string>>{
         {"/Upload/{id}", "/upload/{id}"},
         {"/items/{id}/Upload", "/items/{id}/upload"}}) {
    for (auto const& [leftMode, rightMode] :
         std::vector<std::pair<Registry::PathMode, Registry::PathMode>>{
           {Registry::PathMode::WWW, Registry::PathMode::WWW},
           {Registry::PathMode::Raw, Registry::PathMode::WWW},
           {Registry::PathMode::WWW, Registry::PathMode::Raw}}) {
      SCOPED_TRACE(left);
      auto first = profile(leftMode);
      auto second = profile(rightMode);
      first.routes.front().path = left;
      second.routes.front().path = right;
      second.name = "other";
      EXPECT_THROW(Registry({first, second}), std::invalid_argument);
      EXPECT_THROW(Registry({second, first}), std::invalid_argument);
    }
  }
  auto first = profile(Registry::PathMode::Raw);
  auto second = first;
  second.name = "other";
  EXPECT_THROW(Registry({first, second}), std::invalid_argument);
  EXPECT_THROW(Registry({second, first}), std::invalid_argument);
}

TEST(RequestBodyRegistryTest, literal_fix_preserves_capture_and_mixed_mode_guards) {
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    auto literal = profile(mode);
    auto capture = profile(mode);
    literal.routes.front().path = "/upload";
    capture.routes.front().path = "/{item}";
    capture.name = "other";
    EXPECT_THROW(Registry({literal, capture}), std::invalid_argument);
    EXPECT_THROW(Registry({capture, literal}), std::invalid_argument);
  }
  auto raw = profile(Registry::PathMode::Raw);
  auto www = profile(Registry::PathMode::WWW);
  raw.routes.front().path = "/upload";
  www.routes.front().path = "/uploads";
  www.name = "other";
  EXPECT_THROW(Registry({raw, www}), std::invalid_argument);
  EXPECT_THROW(Registry({www, raw}), std::invalid_argument);
}

TEST(RequestBodyRegistryTest, mode_caches_preserve_candidate_views_and_capture_order) {
  for (auto const reversed : {false, true}) {
    SCOPED_TRACE(reversed);
    auto config = profile();
    auto const base = config.routes.front();
    config.routes.clear();
    for (auto const& [mode, path] :
         std::vector<std::pair<Registry::PathMode, std::string>>{
           {Registry::PathMode::Raw, "/ra/{item}"},
           {Registry::PathMode::Raw, "/rb/{item}"},
           {Registry::PathMode::WWW, "/wa/{item}"},
           {Registry::PathMode::WWW, "/wb/{item}"}}) {
      auto route = base;
      route.mode = mode;
      route.path = path;
      config.routes.push_back(std::move(route));
    }
    if (reversed) std::reverse(config.routes.begin(), config.routes.end());
    Registry registry({config});
    auto shortValue = registry.select("default", "api.example.test", "POST",
                                       "/wb/%61", true);
    ASSERT_TRUE(shortValue);
    EXPECT_EQ("/wb/{item}", shortValue->path);
    EXPECT_EQ(std::vector<std::string>({"%61", "a"}), shortValue->segments);
    auto relative = registry.select("default", "api.example.test", "POST",
                                     "wb/a", true);
    ASSERT_TRUE(relative);
    EXPECT_EQ(std::vector<std::string>{"a"}, relative->segments);
    auto raw = registry.select("default", "api.example.test", "POST",
                                "/rb/%61", false);
    ASSERT_TRUE(raw);
    EXPECT_EQ("/rb/{item}", raw->path);
    EXPECT_EQ(std::vector<std::string>{"%61"}, raw->segments);
    std::string capture(512, 'a');
    auto longValue = registry.select("default", "api.example.test", "POST",
                                      "/wb/" + capture + "%23Z", true);
    ASSERT_TRUE(longValue);
    EXPECT_EQ(std::vector<std::string>({capture + "%23Z", capture + "#Z"}),
              longValue->segments);
  }
}

TEST(RequestBodyRegistryTest, post_only_and_utf8_policy_are_mode_scoped) {
  std::string capture = "a";
  capture.push_back('\0');
  capture.push_back(static_cast<char>(0xff));
  capture += "b";
  auto const target = "/v1/items/" + capture + "/uploads";
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    Registry registry({profile(mode)});
    for (auto const replace : {false, true}) {
      auto match = registry.select("default", "api.example.test", "POST",
                                    target, replace);
      ASSERT_TRUE(match);
      auto const expected = mode == Registry::PathMode::Raw ? capture :
        replace ? std::string("a\xef\xbf\xbd\xef\xbf\xbd") + "b" : "ab";
      EXPECT_EQ(std::vector<std::string>{expected}, match->segments);
      EXPECT_EQ("/v1/items/{item}/uploads", match->path);
      EXPECT_FALSE(registry.select("default", "api.example.test", "post",
                                    target, replace));
    }
  }
}

TEST(RequestBodyRegistryTest, empty_registry_is_disabled) {
  Registry registry({});
  EXPECT_TRUE(registry.empty());
  EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                               "/v1/items/a/uploads", true));
}

TEST(RequestBodyRegistryTest, www_path_forms_preserve_captured_values) {
  Registry registry({profile()});
  std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
    {"/v1/items/a/uploads", {"a"}},
    {"/v1/items/a/uploads?", {"a"}},
    {"/v1/items/a/uploads?other=/x/y", {"a"}},
    {"/v1/items/a/uploads/", {"a"}},
    {"/V1/ITEMS/MiXeD/UPLOADS", {"MiXeD"}},
    {"/v1/items//uploads", {""}},
    {"/v1/items/./uploads", {"."}},
    {"/v1/items/../uploads", {".."}},
    {"/v1/items/a+b/uploads", {"a+b"}},
    {"/v1/items/a#b/uploads", {"a#b"}},
    {"http://api.example.test/v1/items/a/uploads", {"a"}},
    {"HTTPS://api.example.test:443/v1/items/a/uploads?x=1", {"a"}},
    {"v1/items/a/uploads", {"a"}},
    {"/%76%31/items/a/uploads", {"a"}},
    {"/v1/items%2Fa%2Fuploads", {"a"}},
    {"/v1/items/a%2Fb/uploads", {"a%2Fb"}},
    {"/v1/items/a%252Fb/uploads", {"a%252Fb", "a%2Fb"}},
    {"/v1/items/a%23b/uploads", {"a%23b", "a#b"}},
    {"/v1/items/a%3Fb/uploads", {"a%3Fb", "a?b"}},
    {"/v1/items/a%zz/uploads", {"a%zz"}},
  };
  for (auto const& [target, expected] : cases) {
    SCOPED_TRACE(target);
    auto match = registry.select("default", "api.example.test", "POST",
                                 target, true);
    ASSERT_TRUE(match);
    EXPECT_EQ("uploads", match->profile->name);
    EXPECT_EQ(expected, match->segments);
  }
}

TEST(RequestBodyRegistryTest, www_absolute_targets_preserve_path_extraction) {
  Registry registry({profile()});
  for (auto const target : {
      "http://api.example.test?next=/v1/items/a/uploads",
      "http://api.example.test?next=1/v1/items/a/uploads?other=/x",
      "http://api.example.test:/v1/items/a/uploads",
      "http://api.example.test:99999/v1/items/a/uploads",
      "http://other.example.test/v1/items/a/uploads",
      "http://user@other.example.test/v1/items/a/uploads",
      "http:///v1/items/a/uploads"}) {
    SCOPED_TRACE(target);
    // Host is the caller's post-codec value; not all forms are HTTP/1-valid.
    auto match = registry.select("default", "api.example.test", "POST",
                                  target, true);
    ASSERT_TRUE(match);
    EXPECT_EQ("/v1/items/{item}/uploads", match->path);
    EXPECT_EQ(std::vector<std::string>{"a"}, match->segments);
  }
}

TEST(RequestBodyRegistryTest, missing_paths_do_not_match_www_root_capture) {
  auto config = profile();
  config.routes.front().path = "/{id}";
  Registry registry({config});
  // RequestUrl::validatePartialUrl rejects a missing or empty extracted path.
  for (auto const target : {"", "?next=item", "http://api.example.test",
                            "https://api.example.test:443?next=item",
                            "http://api.example.test#fragment",
                            "http://api.example.test?next=%2Fitem"}) {
    SCOPED_TRACE(target);
    for (auto const replace : {false, true}) {
      EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                    target, replace));
    }
  }
  // Missing paths differ from nonempty relative paths and an explicit root.
  for (auto const& [target, expected] :
       std::vector<std::pair<std::string, std::string>>{
         {"/item", "item"}, {"item", "item"}, {"/", ""},
         {"http://api.example.test/", ""},
         {"http://api.example.test?next=/item", "item"}}) {
    SCOPED_TRACE(target);
    auto match = registry.select("default", "api.example.test", "POST",
                                  target, true);
    ASSERT_TRUE(match);
    EXPECT_EQ(std::vector<std::string>{expected}, match->segments);
  }
}

TEST(RequestBodyRegistryTest, path_tokens_preserve_empty_and_binary_captures) {
  for (auto const mode : {Registry::PathMode::Raw, Registry::PathMode::WWW}) {
    Registry registry({profile(mode)});
    auto empty = registry.select("default", "api.example.test", "POST",
                                   "/v1/items//uploads", true);
    if (mode == Registry::PathMode::WWW) {
      ASSERT_TRUE(empty);
      EXPECT_EQ(std::vector<std::string>{""}, empty->segments);
    } else {
      EXPECT_FALSE(empty);
    }
    auto trailing = registry.select("default", "api.example.test", "POST",
                                      "/v1/items/a/uploads/", true);
    EXPECT_EQ(mode == Registry::PathMode::WWW, trailing.has_value());
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                  "/v1/items/a/uploads//", true));
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                  "/v1/items///uploads", true));
  }
  Registry raw({profile(Registry::PathMode::Raw)});
  std::string capture("a\0b", 3);
  auto match = raw.select("default", "api.example.test", "POST",
                           "/v1/items/" + capture + "/uploads", true);
  ASSERT_TRUE(match);
  EXPECT_EQ(std::vector<std::string>{capture}, match->segments);
}

TEST(RequestBodyRegistryTest, target_authority_does_not_override_effective_host) {
  Registry registry({profile()});
  EXPECT_TRUE(registry.select("default", "api.example.test", "POST",
    "http://other.example.test/v1/items/a/uploads", true));
  EXPECT_FALSE(registry.select("default", "other.example.test", "POST",
    "http://api.example.test/v1/items/a/uploads", true));
}

TEST(RequestBodyRegistryTest, absolute_neighbor_queries_do_not_select_uploads) {
  Registry registry({profile()});
  for (auto const target : {
      "http://api.example.test/v1/items?next=/a/uploads",
      "http://api.example.test?next=/v1/items",
      "http://api.example.test?next=%2Fv1%2Fitems%2Fa%2Fuploads",
      "http://api.example.test?next=/v1/items/a/uploads#fragment"}) {
    SCOPED_TRACE(target);
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                 target, true));
  }
}

TEST(RequestBodyRegistryTest, does_not_consume_neighboring_endpoint_bodies) {
  Registry registry({profile()});
  std::vector<std::string> targets = {
    "/v1/items", "/v1/items/", "/v1/items?mode=upload",
    "/v1/items/a/uploads//", "//v1/items/a/uploads",
    "/v1//items/a/uploads", "/v1/items/a/uploads/extra",
    "/v1/items/a/uploads#fragment", "/%2576%2531/items/a/uploads",
  };
  for (auto const& target : targets) {
    SCOPED_TRACE(target);
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                 target, true));
  }
  for (auto const method : {"GET", "PUT", "PATCH", "OPTIONS", "post"}) {
    SCOPED_TRACE(method);
    EXPECT_FALSE(registry.select("default", "api.example.test", method,
                                 "/v1/items/a/uploads", true));
  }
}

TEST(RequestBodyRegistryTest, hosts_and_virtual_host_aliases_share_one_profile) {
  Registry registry({profile()});
  auto first = registry.select("default", "api.example.test", "POST",
                               "/v1/items/a/uploads", true);
  ASSERT_TRUE(first);
  for (auto const host : {"API.EXAMPLE.TEST", "api.example.test:443",
                          "api.beta.example.test", "api.1.od.example.test:80"}) {
    auto alias = registry.select("alias", host, "POST",
                                 "/v1/items/b/uploads", true);
    ASSERT_TRUE(alias);
    EXPECT_EQ(first->profile, alias->profile);
    ASSERT_TRUE(first->profile->tryAcquire());
    EXPECT_FALSE(alias->profile->tryAcquire());
    first->profile->release();
  }
  EXPECT_FALSE(registry.select("other", "api.example.test", "POST",
                               "/v1/items/a/uploads", true));
  for (auto const host : {"example.test", "x.api.example.test",
                          "api.example.test.", "api.example.test:port",
                          "api.a b.example.test", "api.a@b.example.test",
                          "api.x:y.example.test", "api..x.example.test",
                          "api.a..b.example.test"}) {
    EXPECT_FALSE(registry.select("default", host, "POST",
                                 "/v1/items/a/uploads", true));
  }
}

TEST(RequestBodyRegistryTest, host_wildcard_matches_one_or_more_whole_labels) {
  auto config = profile();
  config.routes.front().hosts = {"api.*.example.test"};
  Registry registry({config});
  for (auto const host : {"api.a.example.test", "api.a.b.example.test"}) {
    SCOPED_TRACE(host);
    EXPECT_TRUE(registry.select("default", host, "POST",
                                 "/v1/items/a/uploads", true));
  }
  for (auto const host : {"api.example.test", "api..example.test",
                          "api.a..b.example.test"}) {
    SCOPED_TRACE(host);
    EXPECT_FALSE(registry.select("default", host, "POST",
                                  "/v1/items/a/uploads", true));
  }
}

TEST(RequestBodyRegistryTest, raw_mode_does_not_apply_www_normalization) {
  Registry registry({profile(Registry::PathMode::Raw)});
  EXPECT_TRUE(registry.select("default", "api.example.test", "POST",
                              "/v1/items/a/uploads?x=1", true));
  for (auto const target : {"/V1/items/a/uploads", "/v1/items/a/uploads/",
                            "v1/items/a/uploads", "/v1/items//uploads",
                            "/%76%31/items/a/uploads", "/v1/items/a#b/uploads"}) {
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                 target, true));
  }
}

TEST(RequestBodyRegistryTest, utf8_repair_follows_replace_and_remove_modes) {
  Registry registry({profile()});
  for (auto const target : {"/v%001/items/a/uploads", "/v%ff1/items/a/uploads"}) {
    EXPECT_TRUE(registry.select("default", "api.example.test", "POST",
                                target, false));
    EXPECT_FALSE(registry.select("default", "api.example.test", "POST",
                                 target, true));
  }
  auto repaired = registry.select("default", "api.example.test", "POST",
                                   "/v1/items/a%00b/uploads", true);
  ASSERT_TRUE(repaired);
  std::vector<std::string> expected = {"a%00b", std::string("a\xef\xbf\xbd") + "b"};
  EXPECT_EQ(expected, repaired->segments);
}

TEST(RequestBodyRegistryTest, mixed_modes_cannot_share_a_fragment_prefix) {
  auto raw = profile(Registry::PathMode::Raw);
  auto www = profile(Registry::PathMode::WWW);
  www.name = "other";
  www.routes.front().path = "/v1/items/a/{item}/other";
  constexpr auto target = "/v1/items/a/uploads#tail/other";
  EXPECT_TRUE(Registry({raw}).select("default", "api.example.test", "POST",
                                    target, true));
  EXPECT_TRUE(Registry({www}).select("default", "api.example.test", "POST",
                                    target, true));
  EXPECT_THROW(Registry({raw, www}), std::invalid_argument);
  EXPECT_THROW(Registry({www, raw}), std::invalid_argument);

  www.routes.front().path = "/v2/items/{item}/other";
  EXPECT_NO_THROW(Registry({raw, www}));
}

TEST(RequestBodyRegistryTest, malformed_and_overlapping_configuration_is_refused) {
  for (auto const path : {"v1/items/{x}", "/v1/items/{x}/", "/v1/{x}/{y}",
                          "/v1/items/a{x}", "/v1/items/{}/uploads",
                          "/v1/items/{x-y}/uploads", "/v1//items/{x}",
                          "/v1/items/%61", "/v1/items/a?x", "/v1/items/a#x"}) {
    auto config = profile();
    config.routes.front().path = path;
    EXPECT_THROW(Registry({std::move(config)}), std::invalid_argument);
  }
  auto duplicate = profile();
  EXPECT_THROW(Registry({duplicate, duplicate}), std::invalid_argument);
  auto overlap = duplicate;
  overlap.name = "other";
  overlap.routes.front().hosts = {"api.beta.example.test"};
  EXPECT_THROW(Registry({duplicate, overlap}), std::invalid_argument);
  overlap.routes.front().path = "/v1/items/a/{other}/uploads";
  EXPECT_THROW(Registry({duplicate, overlap}), std::invalid_argument);
  auto badLimit = profile();
  badLimit.highWaterMark = 0;
  EXPECT_THROW(Registry({badLimit}), std::invalid_argument);
  badLimit = profile();
  badLimit.capacity = 0;
  EXPECT_THROW(Registry({badLimit}), std::invalid_argument);
  for (auto const host : {"api.**.example.test", "a*pi.example.test", "api/example",
                          "api..example.test"}) {
    auto badHost = profile();
    badHost.routes.front().hosts = {host};
    EXPECT_THROW(Registry({badHost}), std::invalid_argument);
  }
}

}
}
