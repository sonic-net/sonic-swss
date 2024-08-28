#include "orch.h"
#include "subscriberstatetable.h"
#include "voqstatsorch.h"
#include "portsorch.h"

using namespace swss;
using namespace std;

extern PortsOrch *gPortsOrch;
extern string gMyHostName;
extern string gMyAsicName;

VoqStatsOrch::VoqStatsOrch(
        DBConnector *countersDb,
        DBConnector *chassisCountersDb,
        const std::vector<std::string> &tableNames) :
        Orch(countersDb, tableNames),
        m_voqCntrsTable(chassisCountersDb, CHASSIS_COUNTERS_VOQ)
{
    auto consumerStateTable = new SubscriberStateTable(countersDb, COUNTERS_TABLE,
                              TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto consumer = new Consumer(consumerStateTable, this, COUNTERS_TABLE);
    Orch::addExecutor(consumer);
}

void VoqStatsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        const std::string & op = kfvOp(t);
        const std::string & ip = kfvKey(t);
        const std::vector<FieldValueTuple> fvt = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            // ip is of the form oid:0x1010000000000d1
            updateChassisCountersDb(ip, fvt);
        }
        else if(op == DEL_COMMAND)
        {
            SWSS_LOG_WARN("Unsupported op %s", op.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void VoqStatsOrch::updateChassisCountersDb(const string & key, const std::vector<FieldValueTuple> & fvt)
{
    SWSS_LOG_ENTER();
    string voqName = gPortsOrch->getVoqName(key);
    // if it wasn't a VOQ counter that fired the reactor then early return
    if (voqName=="")
    {
        return;
    }

    // Construct the key for CHASSIS_COUNTERS_VOQ table
    // Linecard1 | asic0 | Ethernet120 @ Linecard2 | asic1 : 7
    // Where Linecard1 | asic0 | Ethernet120 is the sysport corresponding to the VOQ
    // The VOQ physically exists on asic1 of Linecard2
    // index is the VOQ index

    string chassisCountersKey = voqName.substr(0,voqName.find(":")) + "@" + gMyHostName + "|" + gMyAsicName + voqName.substr(voqName.find(":"));
    for (std::pair<std::basic_string<char>, std::basic_string<char> > i: fvt)
    {
        m_voqCntrsTable.hset(chassisCountersKey, i.first, i.second);
    }
}
