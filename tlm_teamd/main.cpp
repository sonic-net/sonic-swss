#include <csignal>
#include <iostream>
#include <deque>

#include <logger.h>
#include <select.h>
#include <dbconnector.h>
#include <subscriberstatetable.h>

#include "teamdctl_mgr.h"
#include "values_store.h"
#include "subintf.h"


bool g_run = true;


/// This function extract all available updates from the table
/// and add or remove LAG interfaces from the TeamdCtlMgr
///
/// @param table reference to the SubscriberStateTable
/// @param mgr   reference to the TeamdCtlMgr
///
void update_interfaces(swss::SubscriberStateTable & table, TeamdCtlMgr & mgr)
{
    std::deque<swss::KeyOpFieldsValuesTuple> entries;

    table.pops(entries);
    for (const auto & entry: entries)
    {
        const auto & lag_name = kfvKey(entry);
        const auto & op = kfvOp(entry);

        if (lag_name.find(VLAN_SUB_INTERFACE_SEPARATOR) != string::npos)
        {
            SWSS_LOG_INFO("Skip subintf %s statedb event", lag_name.c_str());
            continue;
        }
        if (op == "SET")
        {
            mgr.add_lag(lag_name);
        }
        else if (op == "DEL")
        {
            mgr.remove_lag(lag_name);
        }
        else
        {
            SWSS_LOG_WARN("Got invalid operation: '%s' with key '%s'", op.c_str(), lag_name.c_str());
        }
    }
}

///
/// Signal handler
///
void sig_handler(int signo)
{
    (void)signo;
    g_run = false;
}

///
/// main function
///
int main()
{
    const int ms_select_timeout = 1000;

    sighandler_t sig_res;

    sig_res = signal(SIGTERM, sig_handler);
    if (sig_res == SIG_ERR)
    {
        std::cerr << "Can't set signal handler for SIGTERM\n";
        return -1;
    }

    sig_res = signal(SIGINT, sig_handler);
    if (sig_res == SIG_ERR)
    {
        std::cerr << "Can't set signal handler for SIGINT\n";
        return -1;
    }

    int rc = 0;
    try
    {
        swss::Logger::linkToDbNative("tlm_teamd");
        SWSS_LOG_NOTICE("Starting");
        swss::DBConnector db("STATE_DB", 0);

        ValuesStore values_store(&db);
        TeamdCtlMgr teamdctl_mgr;

        swss::Select s;
        swss::Selectable * event;
        swss::SubscriberStateTable sst_lag(&db, STATE_LAG_TABLE_NAME);
        s.addSelectable(&sst_lag);

        while (g_run && rc == 0)
        {
            int res = s.select(&event, ms_select_timeout);
            if (res == swss::Select::OBJECT)
            {
                update_interfaces(sst_lag, teamdctl_mgr);
                values_store.update(teamdctl_mgr.get_dumps(false));
            }
            else if (res == swss::Select::ERROR)
            {
                SWSS_LOG_ERROR("Select returned ERROR");
                rc = -2;
            }
            else if (res == swss::Select::TIMEOUT)
            {
                teamdctl_mgr.process_add_queue();
                // In the case of lag removal, there is a scenario where the select::TIMEOUT
                // occurs, it triggers get_dumps incorrectly for resource which was in process of 
                // getting deleted. The fix here is to retry and check if this is a real failure.
                values_store.update(teamdctl_mgr.get_dumps(true));
            }
            else
            {
                SWSS_LOG_ERROR("Select returned unknown value");
                rc = -3;
            }
	    }
        SWSS_LOG_NOTICE("Exiting");
    }
    catch (const std::exception & e)
    {
        std::cerr << "Exception \"" << e.what() << "\" had been thrown" << std::endl;
        SWSS_LOG_ERROR("Exception '%s' had been thrown", e.what());
        rc = -1;
    }

    return rc;
}
