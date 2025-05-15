#include "gtest/gtest.h"
#include <string>
#include "schema.h"
#include "ut_helper.h"

#define protected public
#include "orch.h"
#include "zmqorch.h"
#undef protected

using namespace std;

TEST(ZmqOrchTest, CreateZmqOrchWitTableNames)
{   
    vector<table_name_with_pri_t> tables = {
        { "TABLE_1", 1 },
        { "TABLE_2", 2 },
        { "TABLE_3", 3 }
    };

    auto app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    auto zmq_orch = make_shared<ZmqOrch>(app_db.get(), tables, nullptr);
    
    EXPECT_EQ(zmq_orch->getSelectables().size(), tables.size());
}
