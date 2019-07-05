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
#include "netdispatcher.h"
#include "mclagsyncd/mclaglink.h"
#include<set>

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("mclagsyncd");
    DBConnector appl_db(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector asic_db(ASIC_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    DBConnector counters_db(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    ProducerStateTable port_tbl(&appl_db, APP_PORT_TABLE_NAME);
    ProducerStateTable lag_tbl(&appl_db, APP_LAG_TABLE_NAME);
    ProducerStateTable tnl_tbl(&appl_db, APP_VXLAN_TUNNEL_TABLE_NAME);
    ProducerStateTable intf_tbl(&appl_db, APP_INTF_TABLE_NAME);
    ProducerStateTable fdb_tbl(&appl_db, APP_FDB_TABLE_NAME);
    ProducerStateTable acl_tbl(&appl_db, APP_ACL_TABLE_NAME);
    ProducerStateTable acl_rule_tbl(&appl_db, APP_ACL_RULE_TABLE_NAME);
    RedisClient redisClient_2_asicDb(&asic_db);
    RedisClient redisClient_2_countersDb(&counters_db);
    map <string, string> isolate;
    map <string, string> learn_mode;
    RedisPipeline pipeline(&appl_db);
    set <mclag_fdb> old_fdb;

    while (1)
    {
        try
        {
            MclagLink mclag;
            Select s;

            mclag.p_learn = &learn_mode;
            mclag.p_port_tbl = &port_tbl;
            mclag.p_lag_tbl = &lag_tbl;
            mclag.p_tnl_tbl = &tnl_tbl;
            mclag.p_intf_tbl = &intf_tbl;
            mclag.p_fdb_tbl = &fdb_tbl;
            mclag.p_acl_tbl = &acl_tbl;
            mclag.p_acl_rule_tbl = &acl_rule_tbl;
            mclag.p_appl_db = &appl_db;
            mclag.p_redisClient_2_asic = &redisClient_2_asicDb;
            mclag.p_redisClient_2_counters = &redisClient_2_countersDb;
            mclag.p_old_fdb = &old_fdb;
            
            cout << "Waiting for connection..." << endl;
            mclag.accept();
            cout << "Connected!" << endl;

            s.addSelectable(&mclag);
            while (true)
            {
                Selectable *temps;
                
                /* Reading MCLAG messages forever (and calling "readData" to read them) */
                s.select(&temps);
                pipeline.flush();
                SWSS_LOG_DEBUG("Pipeline flushed");
            }
        }
        catch (MclagLink::MclagConnectionClosedException &e)
        {
            /*mclag_connection_lost();*/
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

