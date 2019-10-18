#include <iostream>
#include <inttypes.h>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "warmRestartHelper.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/errfpmroute.h"
#include "errorlistener.h"
#include "errormap.h"

using namespace std;
using namespace swss;


/*
 * Default warm-restart timer interval for routing-stack app. To be used only if
 * no explicit value has been defined in configuration.
 */
const uint32_t DEFAULT_ROUTING_RESTART_INTERVAL = 120;

// Wait 3 seconds after detecting EOIU reached state
// TODO: support eoiu hold interval config
const uint32_t DEFAULT_EOIU_HOLD_INTERVAL = 3;

// Check if eoiu state reached by both ipv4 and ipv6
static bool eoiuFlagsSet(Table &bgpStateTable)
{
    string value;

    bgpStateTable.hget("IPv4|eoiu", "state", value);
    if (value != "reached")
    {
        SWSS_LOG_DEBUG("IPv4|eoiu state: %s", value.c_str());
        return false;
    }
    bgpStateTable.hget("IPv6|eoiu", "state", value);
    if (value != "reached")
    {
        SWSS_LOG_DEBUG("IPv6|eoiu state: %s", value.c_str());
        return false;
    }
    SWSS_LOG_NOTICE("Warm-Restart bgp eoiu reached for both ipv4 and ipv6");
    return true;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("fpmsyncd");
    try
    {
	DBConnector db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
	DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
	RedisPipeline pipeline(&db);
	RouteSync sync(&pipeline);

    DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    Table bgpStateTable(&stateDb, STATE_BGP_TABLE_NAME);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWROUTE, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELROUTE, &sync);

	ErrFpmRoute err_notif(&cfgDb);
	while (true)
	{
	    try
	    {
		FpmLink fpm;
		Select s;
		SelectableTimer warmStartTimer(timespec{0, 0});
		// Before eoiu flags detected, check them periodically. It also stop upon detection of reconciliation done.
		SelectableTimer eoiuCheckTimer(timespec{0, 0});
		// After eoiu flags are detected, start a hold timer before starting reconciliation.
		SelectableTimer eoiuHoldTimer(timespec{0, 0});
		/*
		 * Pipeline should be flushed right away to deal with state pending
		 * from previous try/catch iterations.
		 */
		pipeline.flush();

		cout << "Waiting for fpm-client connection..." << endl;
		fpm.accept();
		err_notif.setFd(fpm.getFd());
		cout << "Connected!" << endl;

		s.addSelectable(&fpm);
		s.addSelectable(&err_notif.cfgTrigger);

            /* If warm-restart feature is enabled, execute 'restoration' logic */
            bool warmStartEnabled = sync.m_warmStartHelper.checkAndStart();
            if (warmStartEnabled)
            {
                /* Obtain warm-restart timer defined for routing application */
                time_t warmRestartIval = sync.m_warmStartHelper.getRestartTimer();
                if (!warmRestartIval)
                {
                    warmStartTimer.setInterval(timespec{DEFAULT_ROUTING_RESTART_INTERVAL, 0});
                }
                else
                {
                    warmStartTimer.setInterval(timespec{warmRestartIval, 0});
                }

                /* Execute restoration instruction and kick off warm-restart timer */
                if (sync.m_warmStartHelper.runRestoration())
                {
                    warmStartTimer.start();
                    s.addSelectable(&warmStartTimer);
                    SWSS_LOG_NOTICE("Warm-Restart timer started.");
                }

                // Also start periodic eoiu check timer, first wait 5 seconds, then check every 1 second
                eoiuCheckTimer.setInterval(timespec{5, 0});
                eoiuCheckTimer.start();
                s.addSelectable(&eoiuCheckTimer);
                SWSS_LOG_NOTICE("Warm-Restart eoiuCheckTimer timer started.");
            }

            while (true)
            {
                Selectable *temps;

                /* Reading FPM messages forever (and calling "readMe" to read them) */
                s.select(&temps);

                /*
                 * Upon expiration of the warm-restart timer or eoiu Hold Timer, proceed to run the
                 * reconciliation process if not done yet and remove the timer from
                 * select() loop.
                 * Note:  route reconciliation always succeeds, it will not be done twice.
                 */
                if (temps == &warmStartTimer || temps == &eoiuHoldTimer)
                {
                    if (temps == &warmStartTimer)
                    {
                        SWSS_LOG_NOTICE("Warm-Restart timer expired.");
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("Warm-Restart EOIU hold timer expired.");
                    }
                    if (sync.m_warmStartHelper.inProgress())
                    {
						sync.dbExistVector.clear();
                        sync.m_warmStartHelper.reconcile();
                        SWSS_LOG_NOTICE("Warm-Restart reconciliation processed.");
                    }
                    // remove the one-shot timer.
                    s.removeSelectable(temps);
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
					//Send positive ACK for identical entries already present in APP_DB
					err_notif.sendImplicitAck(sync);
                }
                else if (temps == &eoiuCheckTimer)
                {
                    if (sync.m_warmStartHelper.inProgress())
                    {
                        if (eoiuFlagsSet(bgpStateTable))
                        {
                            /* Obtain eoiu hold timer defined for bgp docker */
                            uintmax_t eoiuHoldIval = WarmStart::getWarmStartTimer("eoiu_hold", "bgp");
                            if (!eoiuHoldIval)
                            {
                                eoiuHoldTimer.setInterval(timespec{DEFAULT_EOIU_HOLD_INTERVAL, 0});
                                eoiuHoldIval = DEFAULT_EOIU_HOLD_INTERVAL;
                            }
                            else
                            {
                                eoiuHoldTimer.setInterval(timespec{(time_t)eoiuHoldIval, 0});
                            }
                            eoiuHoldTimer.start();
                            s.addSelectable(&eoiuHoldTimer);
                            SWSS_LOG_NOTICE("Warm-Restart started EOIU hold timer which is to expire in %" PRIuMAX " seconds.", eoiuHoldIval);
                            s.removeSelectable(&eoiuCheckTimer);
                            continue;
                        }
                        eoiuCheckTimer.setInterval(timespec{1, 0});
                        // re-start eoiu check timer
                        eoiuCheckTimer.start();
                        SWSS_LOG_DEBUG("Warm-Restart eoiuCheckTimer restarted");
                    }
                    else
                    {
                        s.removeSelectable(&eoiuCheckTimer);
			}
		    }
		    else if (temps == (Selectable *)err_notif.routeErrorListener)
		    {
			std::string key; 
			std::string op;
			std::vector<swss::FieldValueTuple> values;
			if(!err_notif.routeErrorListener->getError(key, op, values))
			{
			    IpPrefix ip_prefix = IpPrefix(key);
			    SWSS_LOG_NOTICE("key=%s, operation=%s\n", key.c_str(), op.c_str());
			    IpAddresses ip_addresses;
			    string alias, strRc;
			    if (op == "remove")
				continue;
			    for (auto entry : values)
			    {
				if (fvField(entry) == "nexthop")
				    ip_addresses = IpAddresses(fvValue(entry));
				if (fvField(entry) == "ifname")
				    alias = fvValue(entry);
				if (fvField(entry) == "rc")
				    strRc = fvValue(entry);
			    }			
			    if (ip_addresses.getSize() == 0)
			    {
				SWSS_LOG_NOTICE("ip address size is empty\n");
				continue;
			    }
			    err_notif.sendMsg(ip_prefix, ip_addresses,  alias, false, sync, strRc); 
			}
		    }
		    else if(temps == (Selectable *)&err_notif.cfgTrigger)
		    {
			std::deque<KeyOpFieldsValuesTuple> entries;
			err_notif.cfgTrigger.pops(entries);
			string cfgEnable;
			for (auto entry: entries)
			{
			    std::string key = kfvKey(entry);
			    std::string op = kfvOp(entry);
			    SWSS_LOG_NOTICE("key=%s, operation=%s\n", key.c_str(), op.c_str());
			    for (auto i : kfvFieldsValues(entry))
			    {
				if (fvField(i) == "enable")
				    cfgEnable = fvValue(i);
			    }
			}
			SWSS_LOG_NOTICE("New cfgEnable=%s,existing routeErrorListener count=%d\n", cfgEnable.c_str(), s.isQueueEmpty());
			if("true" == cfgEnable)
			{
			    err_notif.routeErrorListener = new ErrorListener(APP_ROUTE_TABLE_NAME, (ERR_NOTIFY_FAIL | ERR_NOTIFY_POSITIVE_ACK));
			    s.addSelectable(err_notif.routeErrorListener);
			}
			else
			{
			    s.removeSelectable(err_notif.routeErrorListener);
			    delete err_notif.routeErrorListener;
                    }
                }
                else if (!warmStartEnabled || sync.m_warmStartHelper.isReconciled())
                {
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
            }
        }
        catch (FpmLink::FpmConnectionClosedException &e)
        {
            cout << "Connection lost, reconnecting..." << endl;
        }
        catch (const exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
		SWSS_LOG_ERROR("Exception %s  had been thrown in deamon loc-1",e.what());
            return 0;
        }
	}
    }
    catch (const exception& e)
    {
	cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
	SWSS_LOG_ERROR("Exception %s  had been thrown in deamon loc-2",e.what());
	return 0;
    }

    return 1;
}
