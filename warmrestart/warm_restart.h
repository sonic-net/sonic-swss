#ifndef SWSS_WARM_RESTART_H
#define SWSS_WARM_RESTART_H

#include <string>

using namespace swss;

enum class WarmStartStateType
{
    INIT,
    RESTORED,
    SYNCED,
} ;

const std::map<WarmStartStateType, std::string> warm_start_state_name_map =
{
    {WarmStartStateType::INIT,    "init"},
    {WarmStartStateType::RESTORED, "restored"},
    {WarmStartStateType::SYNCED,    "synced"}
};

bool isWarmStart();
void checkWarmStart(DBConnector *appl_db, const std::string &app_name);
void setWarmStartRestoreState(DBConnector *appl_db, const std::string &app_name, WarmStartStateType state);

#endif
