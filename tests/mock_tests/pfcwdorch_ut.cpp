#include "pfcwdorch.h"

#include <gtest/gtest.h>

TEST(PfcWdPlatformTest, MapsCiscoNamesToExistingPlugin)
{
    EXPECT_EQ(getPfcWdPluginPlatform("cisco-8000"), "cisco-8000");
    EXPECT_EQ(getPfcWdPluginPlatform("cisco"), "cisco-8000");
}

TEST(PfcWdPlatformTest, PreservesOtherPlatformNames)
{
    EXPECT_EQ(getPfcWdPluginPlatform("broadcom"), "broadcom");
}
