// Copyright (c) 2025 Arista Networks, Inc.  All rights reserved.
// Arista Networks, Inc. Confidential and Proprietary.

#include "dbconnector.h"
#include "macsecpost.h"
#include "redisutility.h"
#include "schema.h"
#include "table.h"

namespace swss {

void setMacsecPostState(DBConnector *stateDb, string postState)
{
    Table macsecPostStateTable = Table(stateDb, STATE_FIPS_MACSEC_POST_TABLE_NAME);
    vector<FieldValueTuple> fvts;
    FieldValueTuple postStateFvt("post_state", postState);
    fvts.push_back(postStateFvt);
    macsecPostStateTable.set("global", fvts);
}

string getMacsecPostState(DBConnector *stateDb)
{
    std::string postState = "";
    std::vector<FieldValueTuple> fvts;
    Table macsecPostStateTable = Table(stateDb, STATE_FIPS_MACSEC_POST_TABLE_NAME);
    if (macsecPostStateTable.get("global", fvts))
    {
        auto state = fvsGetValue(fvts, "post_state", true);
        if (state)
        {
            postState = *state;
        }
    }
    return postState;
}

}
