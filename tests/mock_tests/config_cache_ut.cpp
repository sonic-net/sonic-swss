#include "config_cache.h"
#include "gtest/gtest.h"

namespace config_cache_ut
{
    using namespace swss;
    using namespace std;

    struct ConfigCacheTest : public ::testing::Test
    {

    };

    TEST_F(ConfigCacheTest, IncrementalUpdate)
    {
        int call_count = 0;

        ConfigCache cc([&](const std::string& key, const std::string& field, const std::string& old_value, const std::string &new_value, void* context) {
            ++call_count;
            return true;
        });

        cc.config("key", "field", "value");
        ASSERT_EQ(1, call_count);
        ASSERT_TRUE(cc.exist("key"));
        // Do the same config again, verify the call count increase
        call_count = 0;
        cc.config("key", "field", "value");
        ASSERT_EQ(1, call_count);
        // Do a different config, verify the call count increase
        cc.config("key", "field", "new_value");
        ASSERT_EQ(2, call_count);
        // Update default config with new field, verify update success
        call_count = 0;
        ConfigEntry ce {{"field1", "value1"}};
        cc.applyDefault("key", ce);
        ASSERT_EQ(1, call_count);
        // Update default config with existing field, verify update should not happen
        call_count = 0;
        ce["field"] = "default";
        cc.applyDefault("key", ce);
        ASSERT_EQ(0, call_count);

        cc.remove("key");
        ASSERT_FALSE(cc.exist("key"));
    }
}