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
    DBConnector appl_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector asic_db(ASIC_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector state_db(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector config_db(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector counters_db(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    ProducerStateTable port_tbl(&appl_db, APP_PORT_TABLE_NAME);
    ProducerStateTable lag_tbl(&appl_db, APP_LAG_TABLE_NAME);
    ProducerStateTable tnl_tbl(&appl_db, APP_VXLAN_TUNNEL_TABLE_NAME);
    ProducerStateTable intf_tbl(&appl_db, APP_INTF_TABLE_NAME);
    ProducerStateTable fdb_tbl(&appl_db, APP_MCLAG_FDB_TABLE_NAME);
    ProducerStateTable acl_table_tbl(&appl_db, APP_ACL_TABLE_NAME);
    ProducerStateTable acl_rule_tbl(&appl_db, APP_ACL_RULE_TABLE_NAME);

    SubscriberStateTable state_fdb_tbl(&state_db, STATE_FDB_TABLE_NAME);
    Table mclag_tbl(&state_db, STATE_MCLAG_TABLE_NAME);
    Table mclag_local_intf_tbl(&state_db, STATE_MCLAG_LOCAL_INTF_TABLE_NAME);
    Table mclag_remote_intf_tbl(&state_db, STATE_MCLAG_REMOTE_INTF_TABLE_NAME);
    Table mclag_cfg_table(&config_db, CFG_MCLAG_TABLE_NAME);
    Table mclag_intf_cfg_table(&config_db, CFG_MCLAG_INTF_TABLE_NAME);
    Table state_fdb_table(&state_db, STATE_FDB_TABLE_NAME);
    Table device_metadata_tbl(&config_db, CFG_DEVICE_METADATA_TABLE_NAME);
    SubscriberStateTable mclag_cfg_tbl(&config_db, CFG_MCLAG_TABLE_NAME);
    SubscriberStateTable mclag_intf_cfg_tbl(&config_db, CFG_MCLAG_INTF_TABLE_NAME);
    SubscriberStateTable mclag_unique_ip_cfg_tbl(&config_db, CFG_MCLAG_UNIQUE_IP_TABLE_NAME);

    Table state_vlan_mbr_table(&state_db, STATE_VLAN_MEMBER_TABLE_NAME);
    SubscriberStateTable state_vlan_mbr_subscriber_table(&state_db, STATE_VLAN_MEMBER_TABLE_NAME);


    map <string, string> isolate;
    map <string, string> learn_mode;
    RedisPipeline pipeline(&appl_db);
    ProducerStateTable iso_grp_tbl(&appl_db, APP_ISOLATION_GROUP_TABLE_NAME);

    while (1)
    {
        try
        {
            Select s;
            MclagLink mclag(&s);

            mclag.p_learn = &learn_mode;
            mclag.p_port_tbl = &port_tbl;
            mclag.p_lag_tbl = &lag_tbl;
            mclag.p_tnl_tbl = &tnl_tbl;
            mclag.p_intf_tbl = &intf_tbl;
            mclag.p_fdb_tbl = &fdb_tbl;
            mclag.p_acl_table_tbl = &acl_table_tbl;
            mclag.p_acl_rule_tbl = &acl_rule_tbl;
            mclag.p_mclag_tbl = &mclag_tbl;
            mclag.p_mclag_local_intf_tbl = &mclag_local_intf_tbl;
            mclag.p_mclag_remote_intf_tbl = &mclag_remote_intf_tbl;
            mclag.p_device_metadata_tbl = &device_metadata_tbl;
            mclag.p_appl_db = &appl_db;
            mclag.p_asic_db = &asic_db;
            mclag.p_counters_db = &counters_db;

            mclag.p_iso_grp_tbl = &iso_grp_tbl;
            mclag.p_mclag_cfg_table = &mclag_cfg_table;
            mclag.p_state_fdb_tbl = &state_fdb_tbl;
            mclag.p_mclag_intf_cfg_table = &mclag_intf_cfg_table;
            mclag.p_mclag_intf_cfg_tbl = &mclag_intf_cfg_tbl;
            mclag.p_state_fdb_table = &state_fdb_table;
            mclag.p_state_vlan_mbr_table = &state_vlan_mbr_table;
            mclag.p_state_vlan_mbr_subscriber_table = &state_vlan_mbr_subscriber_table;


            mclag.mclagsyncd_fetch_system_mac_from_configdb();
            
            cout << "Waiting for connection..." << endl;
            mclag.accept();
            cout << "Connected!" << endl;

            mclag.mclagsyncd_fetch_mclag_config_from_configdb();
            mclag.mclagsyncd_fetch_mclag_interface_config_from_configdb();

            s.addSelectable(&mclag);

            //add mclag domain config table to selectable
            s.addSelectable(&mclag_cfg_tbl);
            SWSS_LOG_NOTICE("MCLagSYNCD Adding mclag_cfg_tbl to selectables");

            s.addSelectable(&state_fdb_tbl);
            SWSS_LOG_NOTICE(" MCLAGSYNCD Add state_fdb_tbl to selectables");

            s.addSelectable(&state_vlan_mbr_subscriber_table);
            SWSS_LOG_NOTICE(" MCLAGSYNCD Add state_vlan_mbr_table  to selectable");

            //add mclag unique ip table to selectable
            s.addSelectable(&mclag_unique_ip_cfg_tbl);
            SWSS_LOG_NOTICE("MCLagSYNCD Adding mclag_unique_ip_cfg_tbl to selectable");

            while (true)
            {
                Selectable *temps;

                /* Reading MCLAG messages forever (and calling "readData" to read them) */
                s.select(&temps);

                if (temps == (Selectable *)&state_fdb_tbl)
                {
                    SWSS_LOG_INFO(" MCLAGSYNCD Matching state_fdb_tbl selectable");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    state_fdb_tbl.pops(entries);
                    if (mclag.m_mclag_domains.size() > 0)
                        mclag.mclagsyncd_send_fdb_entries(entries);
                }
                else if ( temps == (Selectable *)&mclag_cfg_tbl ) //Reading MCLAG Domain Config Table
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag_cfg_tbl.pops(entries);
                    mclag.processMclagDomainCfg(entries);
                }
                else if (temps == (Selectable *)&mclag_intf_cfg_tbl)  //Reading MCLAG Interface Config Table 
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_intf_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag_intf_cfg_tbl.pops(entries);
                    mclag.mclagsyncd_send_mclag_iface_cfg(entries);
                }
                else if (temps == (Selectable *)&mclag_unique_ip_cfg_tbl)  //Reading MCLAG Unique IP Config Table
                {
                    SWSS_LOG_DEBUG("MCLAGSYNCD processing mclag_unique_ip_cfg_tbl notifications");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    mclag_unique_ip_cfg_tbl.pops(entries);
                    mclag.mclagsyncd_send_mclag_unique_ip_cfg(entries);
                }
                else if (temps == (Selectable *)&state_vlan_mbr_subscriber_table) //Reading stateDB vlan member table
                {
                    SWSS_LOG_DEBUG(" MCLAGSYNCD Matching state_vlan_mbr_table selectable");
                    std::deque<KeyOpFieldsValuesTuple> entries;
                    state_vlan_mbr_subscriber_table.pops(entries);
                    if (mclag.m_mclag_domains.size() > 0)
                        mclag.processVlanMemberTableUpdates(entries);
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
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}


