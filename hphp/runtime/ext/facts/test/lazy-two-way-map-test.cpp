/*
+----------------------------------------------------------------------+
| HipHop for PHP                                                       |
+----------------------------------------------------------------------+
| Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
+----------------------------------------------------------------------+
| This source key is subject to version 3.01 of the PHP license,      |
| that is bundled with this package in the key LICENSE, and is        |
| available through the world-wide-web at the following url:           |
| http://www.php.net/license/3_01.txt                                  |
| If you did not receive a copy of the PHP license and are unable to   |
| obtain it through the world-wide-web, please send a note to          |
| license@php.net so we can mail you a copy immediately.               |
+----------------------------------------------------------------------+
*/

#include <memory>
#include <string>
#include <vector>

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include "hphp/runtime/ext/facts/lazy-two-way-map.h"
#include "hphp/runtime/ext/facts/test/matchers.h"
#include "hphp/runtime/ext/facts/test/printers.h" // @donotremove
#include "hphp/runtime/ext/facts/test/string-data-fake.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsUnsetOptional;
using ::testing::NiceMock;
using ::testing::Optional;
using ::testing::Return;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;
using ::testing::Values;

namespace HPHP {
namespace Facts {

// Ensure we're using the test Optional matcher instead of HPHP::Optional.
using ::testing::Optional;

std::map<std::string, std::string> key_table;

template <>
inline StringPtr getVersionKey<std::string>(const std::string& key) {
  return makeStringPtr(key);
}

namespace {

struct MockVersionProvider : public LazyTwoWayMapVersionProvider {
  virtual ~MockVersionProvider() {}

  MOCK_METHOD(std::uint64_t, getVersion, (const StringPtr&), (const, noexcept));
  MOCK_METHOD(void, bumpVersion, (const StringPtr& key), (noexcept));
};

class LazyTwoWayMapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_versions = std::make_shared<NiceMock<MockVersionProvider>>();
  }

  void TearDown() override {
    TestStringTable::getInstance()->clear();
  }

  void setVersion(int version) {
    ON_CALL(*m_versions, getVersion(_)).WillByDefault(Return(version));
  }

  std::shared_ptr<NiceMock<MockVersionProvider>> m_versions;
};

class MultiversionLazyTwoWayMapTest
    : public LazyTwoWayMapTest,
      public ::testing::WithParamInterface<int> {};

// Why are there tests for version 0 and version 1?  Version 0 is considered
// special in that it indicates the data is from a source like the database
// rather than something we've parsed.  There is variation in behavior between
// version 0 and version > 0.
INSTANTIATE_TEST_SUITE_P(
    TestWithVersion0AndVersion1,
    MultiversionLazyTwoWayMapTest,
    Values(0, 1));

TEST_P(MultiversionLazyTwoWayMapTest, EmptyMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};

  setVersion(GetParam());
  EXPECT_THAT(map.getValuesForKey("soda"), IsUnsetOptional());
  EXPECT_THAT(map.getKeysForValue("cola"), IsUnsetOptional());
}

TEST_P(MultiversionLazyTwoWayMapTest, SetValuesForKey) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"casino games"};
  std::vector<std::string> values1{"poker", "blackjack"};
  std::vector<std::string> values2{"slot machines", "roulette"};

  int version = GetParam();

  setVersion(version);
  map.setValuesForKey(key, values1);
  EXPECT_THAT(
      map.getValuesForKey(key), Optional(UnorderedElementsAreArray(values1)));

  setVersion(version + 1);
  map.setValuesForKey(key, values2);
  EXPECT_THAT(
      map.getValuesForKey(key), Optional(UnorderedElementsAreArray(values2)));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetValuesForKeyWithEmptyMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"furniture"};
  std::vector<std::string> values{"desk", "dressed"};

  int version = GetParam();

  // If we get the key from an empty map, we should get an empty result,
  // regardless of the version.
  setVersion(version);
  EXPECT_THAT(map.getValuesForKey(key), IsUnsetOptional());

  // ...and changing the version shouldn't change anything.
  setVersion(version + 1);
  EXPECT_THAT(map.getValuesForKey(key), IsUnsetOptional());

  // ...and if we add something to the map, we should see it.
  map.setValuesForKey(key, values);
  EXPECT_THAT(
      map.getValuesForKey(key), Optional(UnorderedElementsAreArray(values)));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetValuesForKeyWithPopulatedMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"annoying birds"};
  std::string value1{"woodpecker"};
  std::string value2{"magpie"};

  int version = GetParam();

  // If there's a value in the map for the current version of the key, we should
  // return it.
  setVersion(version);
  map.setValuesForKey(key, {value1});
  EXPECT_THAT(map.getValuesForKey(key), Optional(UnorderedElementsAre(value1)));

  // If we change the map, we should only see the values associated with the
  // change, and not the original values.
  map.setValuesForKey(key, {value2});
  EXPECT_THAT(map.getValuesForKey(key), Optional(UnorderedElementsAre(value2)));

  // Changing the version of the key doesn't change anything.
  EXPECT_THAT(map.getValuesForKey(key), Optional(UnorderedElementsAre(value2)));

  // If the map is emptied, we shouldn't get an empty list back.
  map.setValuesForKey(key, {});
  EXPECT_THAT(map.getValuesForKey(key), Optional(IsEmpty()));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetValuesForKeyFromSourceWithEmptyMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"furniture"};
  std::string value1{"desk"};
  std::string value2{"dresser"};

  int version = GetParam();
  setVersion(version);

  // If we get values for a key with an empty map...
  EXPECT_THAT(map.getValuesForKey(key, {value1}), UnorderedElementsAre(value1));
  if (version == 0) {
    // ...and the version is 0, the map should be populated with those elements.
    EXPECT_THAT(
        map.getValuesForKey(key), Optional(UnorderedElementsAre(value1)));
  } else {
    // ... and the version is not zero, the map should not be populated with the
    // elements provided by the source.
    EXPECT_THAT(map.getValuesForKey(key), IsUnsetOptional());
  }

  // If we bump the version...
  setVersion(version + 1);
  if (version == 0) {
    // ... and we just landed on a non-zero version, then the map should not be
    // populated with the provided values.
    EXPECT_THAT(
        map.getValuesForKey(key, {value2}), UnorderedElementsAre(value1));
  } else {
    // ... and we were already on a non-zero version, the map should continue to
    // ignore the provided values.
    EXPECT_THAT(
        map.getValuesForKey(key, {value2}), UnorderedElementsAre(value2));
  }
}

TEST_P(
    MultiversionLazyTwoWayMapTest,
    GetValuesForKeyFromSourceWithPopulatedMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"fruit"};
  std::vector<std::string> valuesA{"pineapple", "guava"};
  std::vector<std::string> valuesB{"banana", "orange"};
  int version = GetParam();

  const auto& expected = (version == 0) ? valuesB : valuesA;

  setVersion(version);
  map.setValuesForKey(key, valuesA);

  // If we are on version and we getValuesForKey with provided values, those
  // will overwrite the existing values.  If we aren't non version zero, the
  // provided values will be ignored.
  EXPECT_THAT(
      map.getValuesForKey(key, valuesB), UnorderedElementsAreArray(expected));

  setVersion(version + 1);
  // Regardless of what version we were on before, we expect that the provided
  // values will now be ignored because we're definitely on a non-zero version.
  EXPECT_THAT(
      map.getValuesForKey(key, valuesB), UnorderedElementsAreArray(expected));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetKeysForValueWithEmptyMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string value{"lactose"};
  setVersion(GetParam());

  // getKeysForValue with a second parameter has to be called at least once
  // before getKeysForValue without a second parameter can be called.
  EXPECT_THAT(map.getKeysForValue(value, {}), IsEmpty());

  EXPECT_THAT(map.getKeysForValue(value), IsUnsetOptional());
}

TEST_P(
    MultiversionLazyTwoWayMapTest,
    GetKeysForValueWithSourceMustBeCalledBeforeGetKeysWithValue) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"fizzy water flavors"};
  std::string value{"lime"};
  setVersion(GetParam());

  EXPECT_THAT(map.getKeysForValue(value), IsUnsetOptional());

  map.setValuesForKey(key, {value});
  EXPECT_THAT(map.getKeysForValue(value), IsUnsetOptional());

  EXPECT_THAT(map.getKeysForValue(value, {key}), ElementsAre(key));
  EXPECT_THAT(map.getKeysForValue(value), Optional(ElementsAre(key)));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetKeysForValueWithPopulatedMap) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"leavening"};
  std::string value{"baking soda"};
  std::vector<std::string> values{value, "baking powder"};
  setVersion(GetParam());

  map.setValuesForKey(key, values);

  // getKeysForValue with a second parameter has to be called at least once
  // before getKeysForValue without a second parameter can be called.
  EXPECT_THAT(map.getKeysForValue(value, {}), ElementsAre(key));
  EXPECT_THAT(map.getKeysForValue(value), Optional(ElementsAre("leavening")));
}

TEST_P(
    MultiversionLazyTwoWayMapTest,
    GetKeysForValueFromPopulatedMapAndEmptySource) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"vegetable"};
  std::string value{"gai lan"};
  std::vector<std::string> keys{key};
  std::vector<std::string> values{value};
  setVersion(GetParam());

  map.setValuesForKey(key, values);
  EXPECT_THAT(map.getKeysForValue(value, {}), UnorderedElementsAre(key));
}

TEST_P(
    MultiversionLazyTwoWayMapTest,
    GetKeysForValueFromEmptyMapAndPopulatedSource) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"grains"};
  std::string value{"quinoa"};
  std::vector<std::string> keys{key};

  int version = GetParam();
  setVersion(version);

  auto result = map.getKeysForValue(value, keys);
  if (version == 0) {
    // If the current version is zero, the data added would have version 0
    // as well, so we would expect to get our input back.
    EXPECT_THAT(result, UnorderedElementsAre(key));
  } else {
    // If the current version is non-zero, the data added would have version 0
    // but then would immediately be considered stale, as if the map had been
    // since updated with an set of keys for the given value.
    EXPECT_THAT(result, IsEmpty());
  }
}

TEST_P(
    MultiversionLazyTwoWayMapTest,
    GetKeysForValueFromPopulatedMapAndPopulatedSource) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string keyA{"color"};
  std::string keyB{"fish"};
  std::string value{"salmon"};

  int version = GetParam();
  setVersion(version);

  map.setValuesForKey(keyA, {value});
  auto result = map.getKeysForValue(value, {keyB});
  if (version == 0) {
    // If the current version is zero, the source data would be merged with
    // what is already in the map.
    EXPECT_THAT(result, UnorderedElementsAre(keyA, keyB));
  } else {
    // If the current version is non-zero, the source data would be added with
    // version zero, which is immediately considered stale.  This would be
    // like a read from the database being added after a parsing result
    // already added data to the map.
    EXPECT_THAT(result, UnorderedElementsAre(keyA));
  }
}

TEST_P(MultiversionLazyTwoWayMapTest, GetKeysForValueWithMultipleUpdates) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"sort algorithms"};
  std::string value1{"quick"};
  std::string value2{"merge"};
  std::string value3{"bubble"};
  std::string value4{"insertion"};

  int version = GetParam();
  setVersion(version);

  setVersion(version);

  map.setValuesForKey(key, {value1});
  if (version == 0) {
    EXPECT_THAT(
        map.getValuesForKey(key, {value4}), UnorderedElementsAre(value4));
  } else {
    EXPECT_THAT(
        map.getValuesForKey(key, {value4}), UnorderedElementsAre(value1));
  }

  setVersion(version + 1);
  map.setValuesForKey(key, {value2});
  EXPECT_THAT(map.getValuesForKey(key, {value4}), UnorderedElementsAre(value2));

  map.setValuesForKey(key, {value3});
  setVersion(version + 2);
  EXPECT_THAT(map.getValuesForKey(key, {value4}), UnorderedElementsAre(value3));
}

TEST_P(MultiversionLazyTwoWayMapTest, GetValuesForKeyWithMultipleUpdates) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key1{"have vitamin c"};
  std::string key2{"citrus"};
  std::string key3{"soda flavors"};

  std::string value1{"lime"};
  std::string value2{"orange"};

  int version = GetParam();
  setVersion(version);

  if (version == 0) {
    EXPECT_THAT(
        map.getValuesForKey(key1, {value1}), UnorderedElementsAre(value1));
  } else {
    EXPECT_THAT(
        map.getValuesForKey(key1, {value1}), UnorderedElementsAre(value1));
  }

  setVersion(version + 1);
  map.setValuesForKey(key2, {value1});
  EXPECT_THAT(map.getValuesForKey(key2, {}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key2, {value2}), UnorderedElementsAre(value1));

  setVersion(version + 2);
  map.setValuesForKey(key3, {value1});
  EXPECT_THAT(map.getValuesForKey(key3, {}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key3, {value2}), UnorderedElementsAre(value1));
}

TEST_F(LazyTwoWayMapTest, MultipleKeysAndMultipleValueVersion0) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key1{"things in books"};
  std::string key2{"database components"};
  std::vector<std::string> keys{key1, key2};
  std::string value1{"index"};
  std::string value2{"tables"};

  setVersion(0);

  // Since the version is zero, we expect that the values for key1 will be
  // populated with the values provided, and then those will be returned.
  EXPECT_THAT(
      map.getValuesForKey(key1, {value1}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key1), Optional(UnorderedElementsAre(value1)));
  EXPECT_THAT(map.getKeysForValue(value1, {}), UnorderedElementsAre(key1));
  EXPECT_THAT(
      map.getKeysForValue(value1), Optional(UnorderedElementsAre(key1)));

  setVersion(1);
  // We've bumped the version so all of the keys are considered stale now.
  EXPECT_THAT(map.getKeysForValue(value1, keys), IsEmpty());
  EXPECT_THAT(map.getKeysForValue(value1), Optional(IsEmpty()));

  // Getting the values for the key shouldn't be affected by a value from
  // the source since the version is greater than zero.
  // Verifying this with an empty set:
  EXPECT_THAT(map.getValuesForKey(key1, {}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key1), Optional(UnorderedElementsAre(value1)));
  // Verifying this with a value specified:
  EXPECT_THAT(
      map.getValuesForKey(key1, {value2}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key1), Optional(UnorderedElementsAre(value1)));

  // Nothing we did should affect a different key.
  EXPECT_THAT(map.getValuesForKey(key2, {}), IsEmpty());
  EXPECT_THAT(map.getValuesForKey(key2), IsUnsetOptional());
}

TEST_F(LazyTwoWayMapTest, MultipleKeysToMultipleValues) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string file1{"www/file1.php"};
  std::string file2{"www/file2.php"};
  std::string file3{"www/file3.php"};
  std::string file4{"www/file4.php"};
  std::string attribute1{"attribute1"};
  std::string attribute2{"attribute2"};
  std::string attribute3{"attribute3"};
  std::string attribute4{"attribute4"};

  // Example scenario:  Files can have multiple attributes and attributes can
  // be used in multiple files.
  //
  // file1 has attributes 1 and 2 | attribute1 is on file4 and file1
  // file2 has attributes 2 and 3 | attribute2 is on file1 and file2
  // file3 has attributes 3 and 4 | attribute3 is on file2 and file3
  // file4 has attributes 4 and 1 | attribute4 is on file3 and file4
  map.setValuesForKey(file1, {attribute1, attribute2});
  map.setValuesForKey(file2, {attribute2, attribute3});
  map.setValuesForKey(file3, {attribute3, attribute4});
  map.setValuesForKey(file4, {attribute4, attribute1});

  EXPECT_THAT(
      map.getValuesForKey(file1),
      Optional(UnorderedElementsAre(attribute1, attribute2)));
  EXPECT_THAT(
      map.getValuesForKey(file2),
      Optional(UnorderedElementsAre(attribute2, attribute3)));
  EXPECT_THAT(
      map.getValuesForKey(file3),
      Optional(UnorderedElementsAre(attribute3, attribute4)));
  EXPECT_THAT(
      map.getValuesForKey(file4),
      Optional(UnorderedElementsAre(attribute4, attribute1)));
  EXPECT_THAT(
      map.getKeysForValue(attribute1, {}), UnorderedElementsAre(file4, file1));
  EXPECT_THAT(
      map.getKeysForValue(attribute1),
      Optional(UnorderedElementsAre(file4, file1)));
  EXPECT_THAT(
      map.getKeysForValue(attribute2, {}), UnorderedElementsAre(file1, file2));
  EXPECT_THAT(
      map.getKeysForValue(attribute2),
      Optional(UnorderedElementsAre(file1, file2)));
  EXPECT_THAT(
      map.getKeysForValue(attribute3, {}), UnorderedElementsAre(file2, file3));
  EXPECT_THAT(
      map.getKeysForValue(attribute3),
      Optional(UnorderedElementsAre(file2, file3)));
  EXPECT_THAT(
      map.getKeysForValue(attribute4, {}), UnorderedElementsAre(file3, file4));
  EXPECT_THAT(
      map.getKeysForValue(attribute4),
      Optional(UnorderedElementsAre(file3, file4)));
}

TEST_F(LazyTwoWayMapTest, StaleKeys) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};
  std::string key{"frog"};
  std::string value{"kermit"};
  std::vector<std::string> values{value};
  std::vector<std::string> keys{key};

  setVersion(0);

  map.setValuesForKey(key, values);
  map.getKeysForValue(value, {});

  setVersion(1);

  // The key should no longer be returned since its version is stale
  auto result = map.getKeysForValue(value);
  EXPECT_THAT(result, Optional(IsEmpty()));
}

// When calling getKeysForValue, data from the source argument should not
// overwrite data which is already in the map.
//
// Example Scenario:  somefile1.php contains constant1 and constant2 The
// source db says somefile2.php contains those.  But it should get ignored in
// favor of what's already in memory.
TEST_F(LazyTwoWayMapTest, SourceDataIsMoreStale) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};

  setVersion(1);

  std::string key1{"www/somefile1.php"};
  std::string key2{"www/somefile2.php"};
  std::string value1{"constant1"};
  std::string value2{"constant2"};

  map.setValuesForKey(key1, {value1, value2});

  // The source data here will essentially be ignored because we already
  // associate value1 with key1 in memory.
  map.getKeysForValue(value1, {key2});

  auto result = map.getKeysForValue(value1);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, ElementsAre(key1));
}

// Scenario:  Nothing is in the memory map yet, but values from the source
// have been provided.  A later update overwrites these.
TEST_F(LazyTwoWayMapTest, SourceDataIsOverwrittenByNewerData) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};

  std::string key1{"www/somefile1.php"};
  std::string key2{"www/somefile2.php"};
  std::string value1{"constant1"};
  std::string value2{"constant2"};

  setVersion(0);
  EXPECT_THAT(map.getKeysForValue(value1, {key2}), ElementsAre(key2));
  EXPECT_THAT(map.getKeysForValue(value1), Optional(ElementsAre(key2)));

  setVersion(1);

  // The source associated key2 with value1, but now we're going to associate
  // value1 with key1 instead.  When
  map.setValuesForKey(key1, {value1, value2});

  EXPECT_THAT(map.getKeysForValue(value1), Optional(ElementsAre(key1)));
  EXPECT_THAT(map.getKeysForValue(value2), IsUnsetOptional());
}

TEST_F(LazyTwoWayMapTest, DifferentKeysWithDifferentVersions) {
  LazyTwoWayMap<std::string, std::string> map{m_versions};

  std::string key1{"baby things"};
  std::string key2{"lab things"};
  std::string value1{"formula"};
  std::string value2{"distilled water"};

  EXPECT_CALL(*m_versions, getVersion(::testing::Eq(key1)))
      .WillRepeatedly(Return(0));
  EXPECT_CALL(*m_versions, getVersion(::testing::Eq(key2)))
      .WillRepeatedly(Return(1));

  // Since the version for key1 is 0, the memory map should get populated with
  // values from the source.
  EXPECT_THAT(
      map.getValuesForKey(key1, {value1}), UnorderedElementsAre(value1));
  EXPECT_THAT(
      map.getValuesForKey(key1), Optional(UnorderedElementsAre(value1)));

  // And since the version for key2 is 1, we treat this as having parsed valid
  // information previously populating the map, so we shouldn't overwrite it.
  EXPECT_THAT(
      map.getValuesForKey(key2, {value2}), UnorderedElementsAre(value2));
  EXPECT_THAT(map.getValuesForKey(key2), IsUnsetOptional());
}

} // namespace
} // namespace Facts
} // namespace HPHP
