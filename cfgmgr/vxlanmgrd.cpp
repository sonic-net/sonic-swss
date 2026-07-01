#include <unistd.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "macaddress.h"
#include "producerstatetable.h"
#include "vxlanmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

MacAddress gMacAddress;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vxlanmgrd");

    SWSS_LOG_NOTICE("--- Starting vxlanmgrd ---");

    try
    {

        DBConnector cfgDb("CONFIG_DB", 0);
        DBConnector appDb("APPL_DB", 0);
        DBConnector stateDb("STATE_DB", 0);

        WarmStart::initialize("vxlanmgrd", "swss");
        WarmStart::checkWarmStart("vxlanmgrd", "swss");
        if (WarmStart::isWarmStart())
        {
            WarmStart::setWarmStartState("vxlanmgrd", WarmStart::INITIALIZED);
        }

        vector<std::string> cfg_vnet_tables = {
            CFG_VNET_TABLE_NAME,
            CFG_VXLAN_TUNNEL_TABLE_NAME,
            CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
            CFG_VXLAN_EVPN_NVO_TABLE_NAME,
        };

        VxlanMgr vxlanmgr(&cfgDb, &appDb, &stateDb, cfg_vnet_tables);

        std::vector<Orch *> cfgOrchList = {&vxlanmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        /*
         * swss service starts after interfaces-config.service which will have
         * switch_mac set.
         * Dynamic switch_mac update is not supported for now.
         */
        Table table(&cfgDb, "DEVICE_METADATA");
        std::vector<FieldValueTuple> ovalues;
        table.get("localhost", ovalues);
        auto it = std::find_if( ovalues.begin(), ovalues.end(), [](const FieldValueTuple& t){ return t.first == "mac";} );
        if ( it == ovalues.end() ) {
            throw runtime_error("couldn't find MAC address of the device from config DB");
        }
        gMacAddress = MacAddress(it->second);

        /*
         * Replay VXLAN port settings from DEVICE_METADATA to APP_DB SWITCH_TABLE
         * so that orchagent/SAI picks them up on restart/reload.
         * These fields (vxlan_port, vxlan_sport, vxlan_mask) are persisted in
         * DEVICE_METADATA by the CLI but orchagent only subscribes to APP_DB
         * SWITCH_TABLE, so we bridge the gap here at startup.
         */
        const vector<string> vxlan_fields = {"vxlan_port", "vxlan_sport", "vxlan_mask"};
        vector<FieldValueTuple> switchFvs;
        for (const auto &field : vxlan_fields)
        {
            auto fit = std::find_if(ovalues.begin(), ovalues.end(),
                [&field](const FieldValueTuple& t){ return t.first == field; });
            if (fit != ovalues.end())
            {
                switchFvs.emplace_back(fit->first, fit->second);
                SWSS_LOG_NOTICE("Replaying DEVICE_METADATA field %s=%s to APP_DB SWITCH_TABLE",
                                fit->first.c_str(), fit->second.c_str());
            }
        }
        if (!switchFvs.empty())
        {
            ProducerStateTable appSwitchTable(&appDb, APP_SWITCH_TABLE_NAME);
            appSwitchTable.set("switch", switchFvs);
            SWSS_LOG_NOTICE("Replayed %zu VXLAN port field(s) to APP_DB SWITCH_TABLE", switchFvs.size());
        }

        auto in_recon = true;
        vxlanmgr.beginReconcile(true);

        if (WarmStart::isWarmStart())
        {
            vxlanmgr.waitTillReadyToReconcile();
            vxlanmgr.restoreVxlanNetDevices();
            WarmStart::setWarmStartState("vxlanmgrd", WarmStart::REPLAYED);
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                if (true == in_recon)
                {
                    in_recon = false;
                    vxlanmgr.endReconcile(false);
                    if (WarmStart::isWarmStart())
                    {
                        WarmStart::setWarmStartState("vxlanmgrd", WarmStart::RECONCILED);
                    }
                }
                vxlanmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
