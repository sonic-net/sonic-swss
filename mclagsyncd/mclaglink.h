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

#include "select.h"
#include "orch.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "redisclient.h"
#include "mclagsyncd/mclag.h"

//namespace swss {

struct mclag_fdb_info
{
    char mac[MCLAG_ETHER_ADDR_STR_LEN];
    unsigned int vid;
    char port_name[MCLAG_MAX_L_PORT_NAME];
    short type;             /* dynamic or static */
    short op_type;          /* add or del */
};

struct mclag_fdb
{
    std::string mac;
    unsigned int vid;
    std::string port_name;
    std::string type;       /* dynamic or static */
    short op_type;          /* add or del */

    mclag_fdb(std::string val_mac, unsigned int val_vid, std::string val_pname,
              std::string val_type, short val_op_type):mac(val_mac),vid(val_vid),port_name(val_pname),type(val_type),op_type(val_op_type) {}
    mclag_fdb() {}

    bool operator <(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac)
            return mac < fdb.mac;
        else if (vid != fdb.vid)
            return vid < fdb.vid;
        else
            return port_name < fdb.port_name;
    }

    bool operator ==(const mclag_fdb &fdb) const
    {
        if (mac != fdb.mac || vid != fdb.vid)
            return 0;

        return 1;
    }
};

class MclagFdbGather:public Orch
{
public:
    MclagFdbGather(DBConnector *staDb, const std::vector<TableConnector> &tables);
    using Orch::doTask;
    std::vector<struct mclag_fdb> m_fdbEvent;
    std::set<mclag_fdb> m_fdbSet;
    void getFdbFromStatedb();

private:
    RedisClient m_redisClient;
    void doTask(Consumer &consumer);
    void doFdbUpdateTask(Consumer &consumer);
    void storeFdbChange(Consumer &consumer);
};

class MclagLink:public swss::Selectable
{
public:
    bool m_connectionState;
    MclagLink(int fd);
    virtual ~MclagLink();

    int getFd() override;
    uint64_t readData() override;
    void notifyFdbChange();
    std::vector<swss::Selectable *> getFdbGatherSelectables();

private:
    unsigned int m_pos;
    unsigned int m_bufSize;
    char m_msgBuf[MCLAG_MAX_MSG_LEN * MSG_BATCH_SIZE];
    char m_msgSndBuf[MCLAG_MAX_SEND_MSG_LEN];
    int m_connectionSocket;
    std::vector<struct mclag_fdb> *m_pFdbEvent;
    std::set<mclag_fdb> *m_pFdbSet;
    DBConnector m_applDb;
    DBConnector m_stateDb;
    ProducerStateTable m_portTable;
    ProducerStateTable m_lagTable;
    ProducerStateTable m_tnlTable;
    ProducerStateTable m_intfTable;
    ProducerStateTable m_fdbTable;
    ProducerStateTable m_aclTable;
    ProducerStateTable m_aclRuleTable;
    TableConnector m_stateFdbTable;
    std::vector<TableConnector> m_fdbGatherTables;
    MclagFdbGather m_fdbGather;

    void setPortIsolate(char *msg);
    void setPortLearnMode(char *msg);
    void flushFdb();
    void flushFdbByPort(char *msg);
    void setIntfMac(char *msg);
    void setFdbEntry(char *msg, int msg_len);
};

class MclagServerLink:public swss::Selectable
{
public:
    Select *m_pSelect;
    std::vector<MclagLink *> m_linkList;
    MclagServerLink(int port = MCLAG_DEFAULT_PORT);
    virtual ~MclagServerLink();
    void accept();
    int getFd() override;
    uint64_t readData() override;

private:
    bool m_serverUp;
    int m_serverSocket;
};

//}
#endif
