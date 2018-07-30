#include <string>
#include <vector>
#include "dbconnector.h"
#include "logger.h"
#include "table.h"
#include "schema.h"
#include "warm_restart.h"

using namespace swss;

static bool warm_start = false;

bool isWarmStart()
{
    return warm_start;
}

// Check warm start flag at the very begining of application, do it once for each process.
void checkWarmStart(DBConnector *db, const std::string &app_name)
{
    std::unique_ptr<Table>  warmStartTable = std::unique_ptr<Table>(new Table(db, APP_WARM_RESTART_TABLE_NAME));
    std::vector<FieldValueTuple> vfv;

    if (warmStartTable->get(app_name, vfv))
    {
        for (auto &fv: vfv)
        {
            if (fvField(fv) == "restart_count")
            {
                std::vector<FieldValueTuple> tmp;
                uint32_t restart_count = (uint32_t)stoul(fvValue(fv));

                restart_count++;
                FieldValueTuple tuple("restart_count", std::to_string(restart_count));
                tmp.push_back(tuple);
                warmStartTable->set(app_name, tmp);
                warm_start = true;
                SWSS_LOG_NOTICE("%s doing warm start, restart count %d", app_name.c_str(), restart_count);
                break;
            }
        }
    }

    // For cold start, the whole appl db will be flushed including warm start table.
    // Create the entry here.
    if (!warm_start)
    {
        vfv.clear();
        FieldValueTuple tuple("restart_count", "0");
        vfv.push_back(tuple);
        warmStartTable->set(app_name, vfv);
    }

}

// Set the state restored flag
void setWarmStartRestoreState(DBConnector *appl_db, const std::string &app_name, WarmStartStateType state)
{
    std::unique_ptr<Table>  warmStartTable = std::unique_ptr<Table>(new Table(appl_db, APP_WARM_RESTART_TABLE_NAME));
    // Set the state flag
    std::vector<FieldValueTuple> tmp;
    FieldValueTuple tuple("state", warm_start_state_name_map.at(state).c_str());
    tmp.push_back(tuple);
    warmStartTable->set(app_name, tmp);
}
