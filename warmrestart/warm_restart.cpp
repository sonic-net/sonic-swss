#include <string>
#include "logger.h"
#include "schema.h"
#include "warm_restart.h"

namespace swss {

const WarmStart::WarmStartStateNameMap WarmStart::warmStartStateNameMap =
{
    {INIT,          "init"},
    {RESTORED,      "restored"},
    {RECONCILED,    "reconciled"}
};

WarmStart &WarmStart::getInstance()
{
    static WarmStart m_warmStart;
    return m_warmStart;
}

// Check warm start flag at the very begining of application, do it once for each process.
bool WarmStart::checkWarmStart(const std::string &app_name, const std::string &docker_name)
{
    auto& warmStart = getInstance();

    if (warmStart.m_stateDb)
    {
        return true;
    }

    warmStart.m_stateDb     = std::make_shared<swss::DBConnector>(STATE_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    warmStart.m_stateWarmRestartTable = std::unique_ptr<Table>(new Table(warmStart.m_stateDb.get(), STATE_WARM_RESTART_TABLE_NAME));

    warmStart.m_cfgDb       = std::make_shared<swss::DBConnector>(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    warmStart.m_cfgWarmRestartTable   = std::unique_ptr<Table>(new Table(warmStart.m_cfgDb.get(), CFG_WARM_RESTART_TABLE_NAME));

    warmStart.enabled = false;

    std::string value;
    // Check system level warm restart config first
    warmStart.m_cfgWarmRestartTable->hget("system", "enable", value);
    if (value == "true")
    {
        warmStart.enabled = true;
    }

    // docker level warm restart configuration
    warmStart.m_cfgWarmRestartTable->hget(docker_name, "enable", value);
    if (value == "true")
    {
        warmStart.enabled = true;
    }

    // For cold start, the whole state db will be flushed including warm start table.
    // Create the entry for this app here.
    if (!warmStart.enabled)
    {
        warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count", "0");
        return true;
    }

    uint32_t restart_count = 0;
    warmStart.m_stateWarmRestartTable->hget(app_name, "restart_count", value);
    if (value == "")
    {
        SWSS_LOG_WARN("%s doing warm start, but restart_count not found in stateDB %s table, fall back to cold start",
                app_name.c_str(), STATE_WARM_RESTART_TABLE_NAME);
        warmStart.enabled = false;
        warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count", "0");
        return true;
    }
    else
    {
        restart_count = (uint32_t)stoul(value);
    }

    restart_count++;
    warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count", std::to_string(restart_count));
    SWSS_LOG_NOTICE("%s doing warm start, restart count %d", app_name.c_str(), restart_count);

    return true;
}

bool WarmStart::isWarmStart()
{
    auto& warmStart = getInstance();

    return warmStart.enabled;
}

// Set the state restored flag
void WarmStart::setWarmStartState(const std::string &app_name, WarmStartState state)
{
    auto& warmStart = getInstance();

    warmStart.m_stateWarmRestartTable->hset(app_name, "state", warmStartStateNameMap.at(state).c_str());
    SWSS_LOG_NOTICE("%s warm start state changed to %s", app_name.c_str(), warmStartStateNameMap.at(state).c_str());
}

}
