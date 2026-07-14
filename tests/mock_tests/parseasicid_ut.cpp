#include "gtest/gtest.h"
#include "parseasicid.h"

TEST(ParseAsicInstanceIdTest, NormalInput)
{
    bool truncated;
    std::string result = parseAsicInstanceId("Asic0", 255, truncated);
    EXPECT_EQ(result, "Asic0");
    EXPECT_FALSE(truncated);
}

TEST(ParseAsicInstanceIdTest, ExactMaxLength)
{
    std::string input(8, 'A');
    bool truncated;
    std::string result = parseAsicInstanceId(input.c_str(), 8, truncated);
    EXPECT_EQ(result, input);
    EXPECT_FALSE(truncated);
}

TEST(ParseAsicInstanceIdTest, ExceedsMaxLength)
{
    std::string input(10, 'B');
    bool truncated;
    std::string result = parseAsicInstanceId(input.c_str(), 8, truncated);
    EXPECT_EQ(result.size(), 8u);
    EXPECT_EQ(result, std::string(8, 'B'));
    EXPECT_TRUE(truncated);
}

TEST(ParseAsicInstanceIdTest, EmptyInput)
{
    bool truncated;
    std::string result = parseAsicInstanceId("", 255, truncated);
    EXPECT_EQ(result, "");
    EXPECT_FALSE(truncated);
}
