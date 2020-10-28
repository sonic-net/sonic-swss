#include <cstring>
#include <algorithm>

#include <logger.h>

#include "teamdctl_mgr.h"

///
/// Custom function for libteamdctl logger. IT is empty to prevent libteamdctl to spam us with the error messages
/// @param tdc teamdctl descriptor
/// @param priority priority of the message
/// @param file file where error was raised
/// @param line line in the file where error was raised
/// @param fn function where the error was raised
/// @param format format of the error message
/// @param args arguments of the error message
void teamdctl_log_function(struct teamdctl *tdc, int priority,
                           const char *file, int line,
                           const char *fn, const char *format,
                           va_list args)
{

}


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
/// Public method to add a LAG interface with lag_name to the manager
/// This method tries to add. If the method can't add the LAG interface,
/// this action will be postponed.
/// @param lag_name a name for LAG interface
/// @return true if the lag was added or postponed successfully, false otherwise
///
bool TeamdCtlMgr::add_lag(const std::string & lag_name)
{
    if (has_key(lag_name))
    {
        SWSS_LOG_DEBUG("The LAG '%s' was already added. Skip adding it.", lag_name.c_str());
        return true;
    }
    return try_add_lag(lag_name);
}

///
/// Try to adds a LAG interface with lag_name to the manager
/// This method allocates structures to connect to teamd
/// if the method can't add, it will retry to add next time
/// @param lag_name a name for LAG interface
/// @return true if the lag was added successfully, false otherwise
///
bool TeamdCtlMgr::try_add_lag(const std::string & lag_name)
{
    if (m_lags_to_add.find(lag_name) == m_lags_to_add.end())
    {
        m_lags_to_add[lag_name] = 0;
    }

    int attempt = m_lags_to_add[lag_name];

    auto tdc = teamdctl_alloc();
    if (!tdc)
    {
        SWSS_LOG_ERROR("Can't allocate memory for teamdctl handler. LAG='%s'. attempt=%d", lag_name.c_str(), attempt);
        m_lags_to_add[lag_name]++;
        return false;
    }

    teamdctl_set_log_fn(tdc, &teamdctl_log_function);

    int err = teamdctl_connect(tdc, lag_name.c_str(), nullptr, "usock");
    if (err)
    {
        if (attempt != 0)
        {
            SWSS_LOG_WARN("Can't connect to teamd LAG='%s', error='%s'. attempt=%d", lag_name.c_str(), strerror(-err), attempt);
        }
        teamdctl_free(tdc);
        m_lags_to_add[lag_name]++;
        return false;
    }

    m_handlers.emplace(lag_name, tdc);
    m_lags_to_add.erase(lag_name);
    SWSS_LOG_NOTICE("The LAG '%s' has been added.", lag_name.c_str());

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
    else if (m_lags_to_add.find(lag_name) != m_lags_to_add.end())
    {
        m_lags_to_add.erase(lag_name);
        SWSS_LOG_DEBUG("The LAG '%s' has been removed from adding queue.", lag_name.c_str());
    }
    else
    {
        SWSS_LOG_WARN("The LAG '%s' hasn't been added. Can't remove it", lag_name.c_str());
    }
    return true;
}

///
/// Process the queue with postponed add operations for LAG.
///
void TeamdCtlMgr::process_add_queue()
{
    std::vector<std::string> lag_names_to_add;
    std::transform(m_lags_to_add.begin(), m_lags_to_add.end(), lag_names_to_add.begin(), [](auto pair) { return pair.first; });
    for (const auto lag_name: lag_names_to_add)
    {
        bool result = try_add_lag(lag_name);
        if (!result)
        {
            if (m_lags_to_add[lag_name] == TeamdCtlMgr::max_attempts_to_add)
            {
                SWSS_LOG_ERROR("Can't connect to teamd after %d attempts. LAG '%s'", TeamdCtlMgr::max_attempts_to_add, lag_name.c_str());
                m_lags_to_add.erase(lag_name);
            }
        }
    }
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

