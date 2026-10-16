#include "intfguard.h"
#include <gtest/gtest.h>

TEST(IntfGuard, PrepareDoesNotCertifyRetirement)
{
    IntfGuard guard;
    for (const auto &invalid : {"", "0", "-1", "opaque", "18446744073709551616"})
    {
        EXPECT_FALSE(guard.prepareCurrent("Ethernet0", invalid));
    }
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "1"));
    EXPECT_TRUE(guard.isHeld("Ethernet0"));
    EXPECT_FALSE(guard.isHeld("Ethernet00"));
    EXPECT_FALSE(guard.release("Ethernet0", "1"));
}

TEST(IntfGuard, OnlyMatchingOwnerReleasesAfterRetirement)
{
    IntfGuard guard;
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "1"));
    ASSERT_TRUE(guard.retire("Ethernet0"));
    EXPECT_TRUE(guard.isHeld("Ethernet0"));
    EXPECT_FALSE(guard.release("Ethernet0", "2"));
    ASSERT_TRUE(guard.release("Ethernet0", "1"));
    EXPECT_FALSE(guard.isHeld("Ethernet0"));
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "1"));
    EXPECT_FALSE(guard.isHeld("Ethernet0"));
}

TEST(IntfGuard, NewCurrentRequestInheritsFenceWithoutOldAcknowledgement)
{
    IntfGuard guard;
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "1"));
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "2"));
    EXPECT_TRUE(guard.isHeld("Ethernet0"));
    EXPECT_FALSE(guard.release("Ethernet0", "1"));
    EXPECT_FALSE(guard.cancel("Ethernet0", "1"));
    EXPECT_FALSE(guard.release("Ethernet0", "2"));
}

TEST(IntfGuard, RejectedOldPrepareCannotBecomeEligibleAfterRelease)
{
    IntfGuard guard;
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "2"));
    EXPECT_FALSE(guard.prepareCurrent("Ethernet0", "1"));
    ASSERT_TRUE(guard.retire("Ethernet0"));
    ASSERT_TRUE(guard.release("Ethernet0", "2"));
    EXPECT_FALSE(guard.prepareCurrent("Ethernet0", "1"));
    EXPECT_FALSE(guard.isHeld("Ethernet0"));
}

TEST(IntfGuard, IndependentAliasesAndCancellationKeepTheirOwner)
{
    IntfGuard guard;
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "1"));
    ASSERT_TRUE(guard.prepareCurrent("Ethernet4", "1"));
    ASSERT_TRUE(guard.cancel("Ethernet0", "1"));
    EXPECT_TRUE(guard.isHeld("Ethernet4"));
    EXPECT_FALSE(guard.retire("Ethernet8"));
    ASSERT_TRUE(guard.prepareCurrent("Ethernet0", "2"));
    EXPECT_TRUE(guard.isHeld("Ethernet0"));
}
