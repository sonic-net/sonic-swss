#ifndef SWSS_WARM_RESTART_H
#define SWSS_WARM_RESTART_H

#include <string>

using namespace swss;

bool isWarmStart();
void checkWarmStart(DBConnector *appl_db, const std::string &app_name);
void setWarmStartRestoreState(DBConnector *db, const std::string &app_name, bool restored);

#endif
