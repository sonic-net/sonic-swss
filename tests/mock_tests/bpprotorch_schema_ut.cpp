// Compile the runtime header with the name supplied by generated cfg_schema.h.
#ifndef CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME
#define CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME "BRIDGE_PORT_PROTECTION"
#endif
#include "bpprotorch.h"
#include <gtest/gtest.h>

TEST(BpProtectionSchemaTest, GeneratedTableNameMatchesRuntime)
{
    EXPECT_STREQ(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, "BRIDGE_PORT_PROTECTION");
}
