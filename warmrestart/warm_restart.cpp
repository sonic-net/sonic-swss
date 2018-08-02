#include <string>
#include <vector>
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

    warmStart.m_stateDb          = std::make_shared<swss::DBConnector>(STATE_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    warmStart.m_stateRedisClient = std::make_shared<swss::RedisClient>(warmStart.m_stateDb.get());

    warmStart.m_cfgDb          = std::make_shared<swss::DBConnector>(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    warmStart.m_cfgRedisClient = std::make_shared<swss::RedisClient>(warmStart.m_cfgDb.get());

    bool system_warm_start = false;
    warmStart.enabled = false;

    // Check system level warm restart config first
    auto pvalue = warmStart.m_cfgRedisClient->hget(CFG_WARM_RESTART_TABLE_NAME"|system", "enable");
    if (pvalue != NULL  && *pvalue == "true")
    {
        system_warm_start = true;
    }

    // docker level warm restart configuration
    // TODO: remove the fixed docker name
    pvalue = warmStart.m_cfgRedisClient->hget(CFG_WARM_RESTART_TABLE_NAME"|" + docker_name, "enable");
    if (pvalue == NULL && system_warm_start)
    {
        // No application level warm restart configuration, take from systel level config.
        warmStart.enabled = true;
    }
    if (pvalue != NULL && *pvalue == "true")
    {
        warmStart.enabled = true;
    }

    // For cold start, the whole state db will be flushed including warm start table.
    // Create the entry here.
    if (!warmStart.enabled)
    {
        warmStart.m_stateRedisClient->hset(STATE_WARM_RESTART_TABLE_NAME"|" + app_name, "restart_count", "0");
        return true;
    }

    uint32_t restart_count = 0;
    pvalue = warmStart.m_stateRedisClient->hget(STATE_WARM_RESTART_TABLE_NAME"|" + app_name, "restart_count");
    if (pvalue == NULL)
    {
        SWSS_LOG_WARN("%s doing warm start, but restart_count not found in stateDB %s table",
                app_name.c_str(), STATE_WARM_RESTART_TABLE_NAME);
    }
    else
    {
        restart_count = (uint32_t)stoul(*pvalue);
    }

    restart_count++;
    warmStart.m_stateRedisClient->hset(STATE_WARM_RESTART_TABLE_NAME"|" + app_name,
            "restart_count", std::to_string(restart_count));
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

    warmStart.m_stateRedisClient->hset(STATE_WARM_RESTART_TABLE_NAME"|" + app_name, "state", warmStartStateNameMap.at(state).c_str());
    SWSS_LOG_NOTICE("%s warm start state changed to %s", app_name.c_str(), warmStartStateNameMap.at(state).c_str());
}

}
