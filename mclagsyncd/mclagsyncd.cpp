/* Copyright(c) 2016-2019 Nephos.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 *  Maintainer: Jim Jiang from nephos
 */
#include <iostream>
#include "logger.h"
#include <map>
#include "select.h"
#include "logger.h"
#include "netdispatcher.h"
#include "mclagsyncd/mclaglink.h"
#include "schema.h"
#include <set>

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("mclagsyncd");

    DBConnector appl_db("APPL_DB", 0);
    RedisPipeline pipeline(&appl_db);

    DBConnector config_db("CONFIG_DB", 0);
    SubscriberStateTable mclag_cfg_tbl(&config_db, CFG_MCLAG_TABLE_NAME);

    map <string, string> learn_mode;
    while (1)
    {
        try
        {
            Select s;
            MclagLink mclag(&s);

            mclag.mclagsyncdFetchSystemMacFromConfigdb();

            cout << "Waiting for connection..." << endl;
            mclag.accept();
            cout << "Connected!" << endl;

            mclag.mclagsyncdFetchMclagConfigFromConfigdb();
            mclag.mclagsyncdFetchMclagInterfaceConfigFromConfigdb();

            s.addSelectable(&mclag);

            //add mclag domain config table to selectable
            s.addSelectable(&mclag_cfg_tbl);
            SWSS_LOG_NOTICE("MCLagSYNCD Adding mclag_cfg_tbl to selectables");

            while (true)
            {
                Selectable *temps;
                int ret;

                /* Reading MCLAG messages forever (and calling "readData" to read them) */
                ret = s.select(&temps);

                if (ret == Select::ERROR)
                {
                    SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                    continue;
                }

                if(temps == (Selectable *)mclag.getStateFdbTable())
                {
                    SWSS_LOG_INFO(" MCLAGSYNCD Matching state_fdb_tbl selectable");
                    mclag.processStateFdb((SubscriberStateTable *)temps);
                }
                else if ( temps == (Selectable *)&mclag_cfg_tbl ) //Reading MCLAG Domain Config Table
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag_cfg_tbl.pops(entries);
                    mclag.processMclagDomainCfg(entries);
                }
                else if ( temps == (Selectable *)mclag.getMclagIntfCfgTable() )  //Reading MCLAG Interface Config Table 
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_intf_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag.getMclagIntfCfgTable()->pops(entries);
                    mclag.mclagsyncdSendMclagIfaceCfg(entries);
                }
                else if (temps == (Selectable *)mclag.getMclagUniqueCfgTable())  //Reading MCLAG Unique IP Config Table
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_unique_ip_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag.getMclagUniqueCfgTable()->pops(entries);
                    mclag.mclagsyncdSendMclagUniqueIpCfg(entries);
                }
                else if (temps == (Selectable *)mclag.getStateVlanMemberTable())
                {
                    SWSS_LOG_INFO(" MCLAGSYNCD Matching vlan Member selectable");
                    mclag.processStateVlanMember((SubscriberStateTable *)temps);
                }
                else
                {
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
            }
        }
        catch (MclagLink::MclagConnectionClosedException &e)
        {
            cout << "Connection lost, reconnecting..." << endl;
        }
        catch (const exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in daemon" << endl;
            return 0;
        }
    }

    return 1;
}


