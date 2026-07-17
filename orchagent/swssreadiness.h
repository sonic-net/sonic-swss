#pragma once

#include <ctime>
#include <string>
#include <set>
#include "dbconnector.h"
#include "logger.h"

class SwssReadinessManager
{
public:
    explicit SwssReadinessManager(swss::DBConnector *stateDb)
        : m_stateDb(stateDb) {}

    void registerModule(const std::string &name)
    {
        m_registered.insert(name);
        SWSS_LOG_NOTICE("SwssReadiness: registered module '%s' (%zu total)",
                        name.c_str(), m_registered.size());
    }

    void signalDone(const std::string &name)
    {
        if (m_signalled) return;

        if (m_registered.find(name) == m_registered.end())
        {
            SWSS_LOG_WARN("SwssReadiness: unknown module '%s' signalled done",
                          name.c_str());
            return;
        }

        if (m_done.insert(name).second)
        {
            SWSS_LOG_NOTICE("SwssReadiness: module '%s' done (%zu/%zu)",
                            name.c_str(), m_done.size(), m_registered.size());
            checkAndSignal();
        }
    }

    bool isReady() const { return m_signalled; }

private:
    void checkAndSignal()
    {
        if (m_signalled) return;
        for (const auto &mod : m_registered)
        {
            if (m_done.find(mod) == m_done.end()) return;
        }
        try
        {
            m_stateDb->hset("FEATURE|swss", "up_status", "true");
            m_stateDb->hset("FEATURE|swss", "update_time",
                            std::to_string(std::time(nullptr)));
            m_signalled = true;
            SWSS_LOG_NOTICE("SwssReadiness: all modules done, "
                            "signalling app readiness "
                            "(FEATURE|swss:up_status=true)");
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("SwssReadiness: failed to set up_status: %s",
                           e.what());
        }
    }

    swss::DBConnector *m_stateDb;
    std::set<std::string> m_registered;
    std::set<std::string> m_done;
    bool m_signalled = false;
};

extern SwssReadinessManager *gSwssReadiness;
