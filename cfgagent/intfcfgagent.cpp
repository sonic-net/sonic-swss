#include <string.h>
#include <errno.h>
#include <system_error>
#include <iostream>
#include <set>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "cfgagent/intfcfgagent.h"
#include "exec.h"

using namespace std;
using namespace swss;

IntfCfgAgent::IntfCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName) :
        CfgOrch(cfgDb, tableName),
        m_cfgIntfTableConsumer(cfgDb, tableName),
        m_intfTableProducer(appDb, APP_INTF_TABLE_NAME)
{

}

void IntfCfgAgent::syncCfgDB()
{
    CfgOrch::syncCfgDB(CFG_INTF_TABLE_NAME, m_cfgIntfTableConsumer);
}

bool IntfCfgAgent::setIntfIp(string &alias, string &opCmd, string &ipPrefixStr)
{
    string cmd;

    cmd = "ip address " + opCmd + " ";
    cmd += ipPrefixStr + " dev " + alias;
    swss::exec(cmd.c_str());
    return true;
}

void IntfCfgAgent::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        vector<string> keys = tokenize(kfvKey(t), ':');
        string alias(keys[0]);
        IpPrefix ip_prefix(kfvKey(t).substr(kfvKey(t).find(':')+1));

        SWSS_LOG_DEBUG("intfs doTask: %s", (dumpTuple(consumer, t)).c_str());

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            string opCmd("add");
            string ipPrefixStr = ip_prefix.to_string();
            setIntfIp(alias, opCmd, ipPrefixStr);
        }
        else if (op == DEL_COMMAND)
        {
            string opCmd("del");
            string ipPrefixStr = ip_prefix.to_string();
            setIntfIp(alias, opCmd, ipPrefixStr);
        }

        SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());

        it = consumer.m_toSync.erase(it);
            continue;
    }
}