#include <string>
#include <climits>
#include "logger.h"
#include "schema.h"
#include "warm_restart.h"

namespace swss {

const WarmStart::WarmStartStateNameMap WarmStart::warmStartStateNameMap =
{
    {INITIALIZED,   "initialized"},
    {RESTORED,      "restored"},
    {RECONCILED,    "reconciled"}
};

WarmStart &WarmStart::getInstance(void)
{
    static WarmStart m_warmStart;
    return m_warmStart;
}

/*
 * WarmStart's initialization method -- to be invoked once per application.
 */
void WarmStart::initialize(const std::string &app_name,
                           const std::string &docker_name)
{
    auto& warmStart = getInstance();

    if (warmStart.m_initialized)
    {
        return;
    }

    warmStart.m_stateDb =
        std::make_shared<swss::DBConnector>(STATE_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);

    warmStart.m_stateWarmRestartTable =
        std::unique_ptr<Table>(new Table(warmStart.m_stateDb.get(), STATE_WARM_RESTART_TABLE_NAME));

    warmStart.m_cfgDb =
        std::make_shared<swss::DBConnector>(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);

    warmStart.m_cfgWarmRestartTable =
        std::unique_ptr<Table>(new Table(warmStart.m_cfgDb.get(), CFG_WARM_RESTART_TABLE_NAME));

    setWarmStartState(app_name, WarmStart::INITIALIZED);

    warmStart.m_initialized = true;
}

/*
 * <1> Upon system reboot, the system enable knob will be checked.
 * If enabled, database data will be preserved, if not, database will be flushed.
 * No need to check docker level knobs in this case since the whole system is being rebooted .

 * <2> Upon docker service start, first to check system knob.
 * if enabled, docker warm start should be performed, otherwise system warm reboot will be ruined.
 * If system knob disabled, while docker knob enabled, this is likely an individual docker warm restart request.

 * Within each application which should take care warm start case,
 * when the system knob or docker knob enabled, we do further check on the
 * actual warm start state ( restart_count), if no warm start state data available,
 * the database has been flushed, do cold start. Otherwise warm start.
 */

/*
 * Method to verify/obtain the state of Warm-Restart feature for any warm-reboot
 * capable component. Typically, this function will be called during initialization of
 * SONiC modules; however, method could be invoked at any given point to verify the
 * latest state of Warm-Restart functionality.
 */
bool WarmStart::checkWarmStart(const std::string &app_name,
                               const std::string &docker_name)
{
    std::string value;

    auto& warmStart = getInstance();

    // Check system level warm-restart config first
    warmStart.m_cfgWarmRestartTable->hget("system", "enable", value);
    if (value == "true")
    {
        warmStart.m_enabled = true;
    }

    // Check docker level warm-restart configuration
    warmStart.m_cfgWarmRestartTable->hget(docker_name, "enable", value);
    if (value == "true")
    {
        warmStart.m_enabled = true;
    }

    // For cold start, the whole state db will be flushed including warm start table.
    // Create the entry for this app here.
    if (!warmStart.m_enabled)
    {
        warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count", "0");
        return false;
    }

    uint32_t restart_count = 0;
    warmStart.m_stateWarmRestartTable->hget(app_name, "restart_count", value);
    if (value == "")
    {
        SWSS_LOG_WARN("%s doing warm start, but restart_count not found in stateDB %s table, fall back to cold start",
                app_name.c_str(), STATE_WARM_RESTART_TABLE_NAME);
        warmStart.m_enabled = false;
        warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count", "0");
        return false;
    }
    else
    {
        restart_count = (uint32_t)stoul(value);
    }

    restart_count++;
    warmStart.m_stateWarmRestartTable->hset(app_name, "restart_count",
                                            std::to_string(restart_count));

    SWSS_LOG_NOTICE("%s doing warm start, restart count %d", app_name.c_str(),
                    restart_count);

    return true;
}

/*
 * Obtain the time-interval defined by a warm-restart-capable application
 * corresponding to the amount of time required to complete a full-restart cycle.
 * This time-duration (warmStartTimer) will be taken into account by the
 * warm-restart logic to kick-off the reconciliation process of this application.
 * A returned value of '0' implies that no valid interval was found.
 */
uint32_t WarmStart::getWarmStartTimer(const std::string &app_name,
                                      const std::string &docker_name)
{
    auto& warmStart = getInstance();
    std::string timer_name = app_name + "_timer";
    std::string timer_value_str;

    warmStart.m_cfgWarmRestartTable->hget(docker_name, timer_name, timer_value_str);

    unsigned long int temp_value = strtoul(timer_value_str.c_str(), NULL, 0);

    if (temp_value != 0 && temp_value != ULONG_MAX &&
        temp_value <= MAXIMUN_WARMRESTART_TIMER_VALUE)
    {
        SWSS_LOG_NOTICE("Getting warmStartTimer for docker: %s, app: %s, value: %lu",
                        docker_name.c_str(), app_name.c_str(), temp_value);
        return (uint32_t)temp_value;
    }
    else
    {
        SWSS_LOG_NOTICE("warmStartTimer is not configured or invalid for docker: %s, app: %s",
                        docker_name.c_str(), app_name.c_str());
        return 0;
    }
}

bool WarmStart::isWarmStart(void)
{
    auto& warmStart = getInstance();

    return warmStart.m_enabled;
}

// Set the WarmStart FSM state for a particular application.
void WarmStart::setWarmStartState(const std::string &app_name, WarmStartState state)
{
    auto& warmStart = getInstance();

    warmStart.m_stateWarmRestartTable->hset(app_name,
                                            "state",
                                            warmStartStateNameMap.at(state).c_str());

    SWSS_LOG_NOTICE("%s warm start state changed to %s",
                    app_name.c_str(),
                    warmStartStateNameMap.at(state).c_str());
}

}
