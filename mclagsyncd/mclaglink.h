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

#ifndef __MCLAGLINK__
#define __MCLAGLINK__

#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <exception>
#include <string>
#include <map>
#include <set>

#include "producerstatetable.h"
#include "selectable.h"
#include "redisclient.h"
#include "mclagsyncd/mclag.h"

namespace swss {

#define ETHER_ADDR_STR_LEN 18
#define MAX_L_PORT_NAME 20

struct mclag_fdb_info
{
    char mac[ETHER_ADDR_STR_LEN];
    unsigned int vid;
    char port_name[MAX_L_PORT_NAME];
    short type;     /*dynamic or static*/
    short op_type;  /*add or del*/
};

struct mclag_fdb
{
    std::string mac;
    unsigned int vid;
    std::string port_name;
    std::string type;/*dynamic or static*/

    mclag_fdb(std::string val_mac, unsigned int val_vid, std::string val_pname,
              std::string val_type) : mac(val_mac), vid(val_vid), port_name(val_pname), type(val_type)
    {
    }
    mclag_fdb()
    {
    }

    bool operator <(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac)
            return mac < fdb.mac;
        else if (vid != fdb.vid)
            return vid < fdb.vid;
        else
            return port_name < fdb.port_name;
        //else if (port_name != fdb.port_name) return port_name < fdb.port_name;
        //else return type <fdb.type;
    }

    bool operator ==(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac)
            return 0;
        if (vid != fdb.vid)
            return 0;
        return 1;
    }

};

class MclagLink : public Selectable {
public:
    const int MSG_BATCH_SIZE;
    std::map<std::string, std:: string> *p_learn;
    ProducerStateTable * p_port_tbl;
    ProducerStateTable * p_lag_tbl;
    ProducerStateTable * p_tnl_tbl;
    ProducerStateTable * p_intf_tbl;
    ProducerStateTable *p_fdb_tbl;
    ProducerStateTable *p_acl_tbl;
    ProducerStateTable *p_acl_rule_tbl;
    DBConnector *p_appl_db;
    RedisClient *p_redisClient_2_asic;
    RedisClient *p_redisClient_2_counters;
    std::set <mclag_fdb> *p_old_fdb;

    MclagLink(int port = MCLAG_DEFAULT_PORT);
    virtual ~MclagLink();

    /* Wait for connection (blocking) */
    void accept();

    int getFd() override;
    void readData() override;

    /* readMe throws MclagConnectionClosedException when connection is lost */
    class MclagConnectionClosedException : public std::exception
    {
    };

private:
    unsigned int m_bufSize;
    char *m_messageBuffer;
    char *m_messageBuffer_send;
    unsigned int m_pos;

    bool m_connected;
    bool m_server_up;
    int m_server_socket;
    int m_connection_socket;

    void mclagsyncd_get_oid_2_port_name_map(std::unordered_map<std::string, std:: string> & port_map);
    void mclagsyncd_get_bridgePortId_2_attrPortId_map(std::map<std::string, std:: string> *oid_map);
    void mclagsyncd_get_vid_by_bvid(std::string &bvid, std::string &vlanid);
    void mclagsyncd_get_fdb_set(std::set<mclag_fdb> *fdb_set);
    void mclagsyncd_set_port_isolate(char *msg);
    void mclagsyncd_set_port_mac_learn_mode(char *msg);
    void mclagsyncd_set_fdb_flush();
    void mclagsyncd_set_fdb_flush_by_port(char *msg);
    void mclagsyncd_set_intf_mac(char *msg);
    void mclagsyncd_set_fdb_entry(char *msg, int msg_len);
    ssize_t  mclagsyncd_get_fdb_changes(char *msg_buf);
    void mclag_connection_lost_handle_port_isolate();
    void mclag_connection_lost_handle_port_learn_mode();
    void mclag_connection_lost();
};

}
#endif

