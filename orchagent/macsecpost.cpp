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

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%a %b %d %H:%M:%S %Y", std::gmtime(&now_c));
    FieldValueTuple lastUpdateTimeFvt("last_update_time", buffer);
    fvts.push_back(lastUpdateTimeFvt);

    macsecPostStateTable.set("sai", fvts);
}

string getMacsecPostState(DBConnector *stateDb)
{
    std::string postState = "";
    std::vector<FieldValueTuple> fvts;
    Table macsecPostStateTable = Table(stateDb, STATE_FIPS_MACSEC_POST_TABLE_NAME);
    if (macsecPostStateTable.get("sai", fvts))
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
