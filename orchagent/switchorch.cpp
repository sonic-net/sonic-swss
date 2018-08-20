#include <map>

#include "switchorch.h"
#include "converter.h"
#include "notifier.h"
#include "notificationproducer.h"

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;

const map<string, sai_switch_attr_t> switch_attribute_map =
{
    {"fdb_unicast_miss_packet_action",      SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION},
    {"fdb_broadcast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION},
    {"fdb_multicast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION},
    {"ecmp_hash_seed",                      SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED},
    {"lag_hash_seed",                       SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED},
    {"fdb_aging_time",                      SAI_SWITCH_ATTR_FDB_AGING_TIME}
};

const map<string, sai_packet_action_t> packet_action_map =
{
    {"drop",    SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"trap",    SAI_PACKET_ACTION_TRAP}
};

SwitchOrch::SwitchOrch(DBConnector *db, string tableName) :
        Orch(db, tableName),
        m_db(db)
{
    m_restartCheckNotificationConsumer = new NotificationConsumer(db, "RESTARTCHECK");
    auto restartCheckNotifier = new Notifier(m_restartCheckNotificationConsumer, this);
    // restartCheckNotifier->setName("RESTARTCHECK");
    Orch::addExecutor("RESTARTCHECK", restartCheckNotifier);
}

void SwitchOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            bool retry = false;

            for (auto i : kfvFieldsValues(t))
            {
                auto attribute = fvField(i);

                if (switch_attribute_map.find(attribute) == switch_attribute_map.end())
                {
                    SWSS_LOG_ERROR("Unsupported switch attribute %s", attribute.c_str());
                    break;
                }

                auto value = fvValue(i);

                sai_attribute_t attr;
                attr.id = switch_attribute_map.at(attribute);

                bool invalid_attr = false;
                switch (attr.id)
                {
                    case SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION:
                    case SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION:
                    case SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION:
                        if (packet_action_map.find(value) == packet_action_map.end())
                        {
                            SWSS_LOG_ERROR("Unsupported packet action %s", value.c_str());
                            invalid_attr = true;
                            break;
                        }
                        attr.value.s32 = packet_action_map.at(value);
                        break;

                    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:
                    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:
                        attr.value.u32 = to_uint<uint32_t>(value);
                        break;

                    case SAI_SWITCH_ATTR_FDB_AGING_TIME:
                        attr.value.u32 = to_uint<uint32_t>(value);
                        break;

                    default:
                        invalid_attr = true;
                        break;
                }
                if (invalid_attr)
                {
                    /* break from kfvFieldsValues for loop */
                    break;
                }

                sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to set switch attribute %s to %s, rv:%d",
                            attribute.c_str(), value.c_str(), status);
                    retry = true;
                    break;
                }

                SWSS_LOG_NOTICE("Set switch attribute %s to %s", attribute.c_str(), value.c_str());
            }
            if (retry == true)
            {
                it++;
            }
            else
            {
                it = consumer.m_toSync.erase(it);
            }
        }
        else
        {
            SWSS_LOG_WARN("Unsupported operation");
            it = consumer.m_toSync.erase(it);
        }
    }
}

void SwitchOrch::doTask(NotificationConsumer& consumer)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_restartCheckNotificationConsumer)
    {
        return;
    }

    m_warmRestartCheck.checkRestartReadyState = false;
    m_warmRestartCheck.noFreeze = false;
    m_warmRestartCheck.skipPendingTaskCheck = false;

    SWSS_LOG_NOTICE("RESTARTCHECK notification for %s ", op.c_str());
    if (op == "orchagent")
    {
        string s  =  op;

        m_warmRestartCheck.checkRestartReadyState = true;
        for (auto &i : values)
        {
            s += "|" + fvField(i) + ":" + fvValue(i);

            if (fvField(i) == "NoFreeze" && fvValue(i) == "true")
            {
                m_warmRestartCheck.noFreeze = true;
            }
            if (fvField(i) == "SkipPendingTaskCheck" && fvValue(i) == "true")
            {
                m_warmRestartCheck.skipPendingTaskCheck = true;
            }
        }
        SWSS_LOG_NOTICE("%s", s.c_str());
    }
}

void SwitchOrch::restartCheckReply(const string &op, const string &data, std::vector<FieldValueTuple> &values)
{
    NotificationProducer restartRequestReply(m_db, "RESTARTCHECKREPLY");
    restartRequestReply.send(op, data, values);
    checkRestartReadyDone();
}
