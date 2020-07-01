#include <cstring>

#include <logger.h>

#include "teamdctl_mgr.h"

///
/// The destructor clean up handlers to teamds
///
TeamdCtlMgr::~TeamdCtlMgr()
{
    for (const auto & p: m_handlers)
    {
        const auto & lag_name = p.first;
        const auto & tdc = m_handlers[lag_name];
        teamdctl_disconnect(tdc);
        teamdctl_free(tdc);
        SWSS_LOG_NOTICE("Exiting. Disconnecting from teamd. LAG '%s'", lag_name.c_str());
    }
}

///
/// Returns true, if we have LAG with name lag_name
/// in the manager.
/// @param lag_name a name for LAG interface
/// @return true if has key, false if doesn't
///
bool TeamdCtlMgr::has_key(const std::string & lag_name) const
{
    return m_handlers.find(lag_name) != m_handlers.end();
}

///
/// Adds a LAG interface with lag_name to the manager
/// This method allocates structures to connect to teamd
/// @param lag_name a name for LAG interface
/// @return true if the lag was added successfully, false otherwise
///
bool TeamdCtlMgr::add_lag(const std::string & lag_name)
{
    if (has_key(lag_name))
    {
        SWSS_LOG_DEBUG("The LAG '%s' was already added. Skip adding it.", lag_name.c_str());
        return true;
    }
    else
    {
        auto tdc = teamdctl_alloc();
        if (!tdc)
        {
            SWSS_LOG_ERROR("Can't allocate memory for teamdctl handler. LAG='%s'", lag_name.c_str());
            return false;
        }

        int err = teamdctl_connect(tdc, lag_name.c_str(), nullptr, nullptr);
        if (err)
        {
            SWSS_LOG_ERROR("Can't connect to teamd LAG='%s', error='%s'", lag_name.c_str(), strerror(-err));
            teamdctl_free(tdc);
            return false;
        }
        m_handlers.emplace(lag_name, tdc);
        SWSS_LOG_NOTICE("The LAG '%s' has been added.", lag_name.c_str());
    }

    return true;
}

///
/// Removes a LAG interface with lag_name from the manager
/// This method deallocates teamd structures
/// @param lag_name a name for LAG interface
/// @return true if the lag was removed successfully, false otherwise
///
bool TeamdCtlMgr::remove_lag(const std::string & lag_name)
{
    if (has_key(lag_name))
    {
        auto tdc = m_handlers[lag_name];
        teamdctl_disconnect(tdc);
        teamdctl_free(tdc);
        m_handlers.erase(lag_name);
        SWSS_LOG_NOTICE("The LAG '%s' has been removed.", lag_name.c_str());
    }
    else
    {
        SWSS_LOG_WARN("The LAG '%s' hasn't been added. Can't remove it", lag_name.c_str());
    }
    return true;
}

///
/// Get json dump from teamd for LAG interface with name lag_name
/// @param lag_name a name for LAG interface
/// @return a pair. First element of the pair is true, if the method is successful
///         false otherwise. If the first element is true, the second element has a dump
///         otherwise the second element is an empty string
///
TeamdCtlDump TeamdCtlMgr::get_dump(const std::string & lag_name)
{
    TeamdCtlDump res = { false, "" };
    if (has_key(lag_name))
    {
        auto tdc = m_handlers[lag_name];
        char * dump;
        int r = teamdctl_state_get_raw_direct(tdc, &dump);
        if (r == 0)
        {
            res = { true, std::string(dump) };
        }
        else
        {
            SWSS_LOG_ERROR("Can't get dump for LAG '%s'. Skipping", lag_name.c_str());
        }
    }
    else
    {
        SWSS_LOG_ERROR("Can't update state. LAG not found. LAG='%s'", lag_name.c_str());
    }

    return res;
}

///
/// Get dumps for all registered LAG interfaces
/// @return vector of pairs. Each pair first value is a name of LAG, second value is a dump
///
TeamdCtlDumps TeamdCtlMgr::get_dumps()
{
    TeamdCtlDumps res;

    for (const auto & p: m_handlers)
    {
        const auto & lag_name = p.first;
        const auto & result = get_dump(lag_name);
        const auto & status = result.first;
        const auto & dump = result.second;
        if (status)
        {
            res.push_back({ lag_name, dump });
        }
    }

    return res;
}

