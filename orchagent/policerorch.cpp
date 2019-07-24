#include "sai.h"
#include "policerorch.h"

#include "converter.h"

using namespace std;
using namespace swss;

extern sai_policer_api_t*   sai_policer_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch* gPortsOrch;

static const string meter_type_field           = "METER_TYPE";
static const string mode_field                 = "MODE";
static const string color_source_field         = "COLOR_SOURCE";
static const string cbs_field                  = "CBS";
static const string cir_field                  = "CIR";
static const string pbs_field                  = "PBS";
static const string pir_field                  = "PIR";
static const string green_packet_action_field  = "GREEN_PACKET_ACTION";
static const string red_packet_action_field    = "RED_PACKET_ACTION";
static const string yellow_packet_action_field = "YELLOW_PACKET_ACTION";

static const map<string, sai_meter_type_t> meter_type_map = {
    {"PACKETS", SAI_METER_TYPE_PACKETS},
    {"BYTES", SAI_METER_TYPE_BYTES}
};

static const map<string, sai_policer_mode_t> policer_mode_map = {
    {"SR_TCM", SAI_POLICER_MODE_SR_TCM},
    {"TR_TCM", SAI_POLICER_MODE_TR_TCM},
    {"STORM_CONTROL", SAI_POLICER_MODE_STORM_CONTROL}
};

static const map<string, sai_policer_color_source_t> policer_color_source_map = {
    {"AWARE", SAI_POLICER_COLOR_SOURCE_AWARE},
    {"BLIND", SAI_POLICER_COLOR_SOURCE_BLIND}
};

static const map<string, sai_packet_action_t> packet_action_map = {
    {"DROP", SAI_PACKET_ACTION_DROP},
    {"FORWARD", SAI_PACKET_ACTION_FORWARD},
    {"COPY", SAI_PACKET_ACTION_COPY},
    {"COPY_CANCEL", SAI_PACKET_ACTION_COPY_CANCEL},
    {"TRAP", SAI_PACKET_ACTION_TRAP},
    {"LOG", SAI_PACKET_ACTION_LOG},
    {"DENY", SAI_PACKET_ACTION_DENY},
    {"TRANSIT", SAI_PACKET_ACTION_TRANSIT}
};

bool PolicerOrch::policerExists(const string &name)
{
    SWSS_LOG_ENTER();

    return m_syncdPolicers.find(name) != m_syncdPolicers.end();
}

bool PolicerOrch::getPolicerOid(const string &name, sai_object_id_t &oid)
{
    SWSS_LOG_ENTER();

    if (policerExists(name))
    {
        oid = m_syncdPolicers[name];
        SWSS_LOG_NOTICE("Get policer %s oid:%lx", name.c_str(), oid);
        return true;
    }

    return false;
}

bool PolicerOrch::increaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    ++m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is increased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}

bool PolicerOrch::decreaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    --m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is decreased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}

PolicerOrch::PolicerOrch(DBConnector* db, string tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

void PolicerOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;

        auto key = kfvKey(tuple);
        auto op = kfvOp(tuple);

        if (op == SET_COMMAND)
        {
            if (m_syncdPolicers.find(key) != m_syncdPolicers.end())
            {
                SWSS_LOG_ERROR("Policer %s already exists", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            vector<sai_attribute_t> attrs;
            bool meter_type = false, mode = false;

            for (auto i = kfvFieldsValues(tuple).begin();
                    i != kfvFieldsValues(tuple).end(); ++i)
            {
                auto field = to_upper(fvField(*i));
                auto value = to_upper(fvValue(*i));

                SWSS_LOG_DEBUG("attribute: %s value: %s", field.c_str(), value.c_str());

                sai_attribute_t attr;

                if (field == meter_type_field)
                {
                    attr.id = SAI_POLICER_ATTR_METER_TYPE;
                    attr.value.s32 = (sai_meter_type_t) meter_type_map.at(value);
                    meter_type = true;
                }
                else if (field == mode_field)
                {
                    attr.id = SAI_POLICER_ATTR_MODE;
                    attr.value.s32 = (sai_policer_mode_t) policer_mode_map.at(value);
                    mode = true;
                }
                else if (field == color_source_field)
                {
                    attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                    attr.value.s32 = policer_color_source_map.at(value);
                }
                else if (field == cbs_field)
                {
                    attr.id = SAI_POLICER_ATTR_CBS;
                    attr.value.u64 = stoul(value);
                }
                else if (field == cir_field)
                {
                    attr.id = SAI_POLICER_ATTR_CIR;
                    attr.value.u64 = stoul(value);
                }
                else if (field == pbs_field)
                {
                    attr.id = SAI_POLICER_ATTR_PBS;
                    attr.value.u64 = stoul(value);
                }
                else if (field == pir_field)
                {
                    attr.id = SAI_POLICER_ATTR_PIR;
                    attr.value.u64 = stoul(value);
                }
                else if (field == red_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else if (field == green_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else if (field == yellow_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown policer attribute %s specified",
                            field.c_str());
                    continue;
                }

                attrs.push_back(attr);
            }

            if (!meter_type || !mode)
            {
                SWSS_LOG_ERROR("Failed to create policer %s,\
                        missing madatory fields", key.c_str());
            }

            sai_object_id_t policer_id;
            sai_status_t status = sai_policer_api->create_policer(
                &policer_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create policer %s, rv:%d",
                        key.c_str(), status);
                it++;
                continue;
            }

            SWSS_LOG_NOTICE("Created policer %s", key.c_str());
            m_syncdPolicers[key] = policer_id;
            m_policerRefCounts[key] = 0;
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdPolicers.find(key) == m_syncdPolicers.end())
            {
                SWSS_LOG_ERROR("Policer %s does not exists", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (m_policerRefCounts[key] > 0)
            {
                SWSS_LOG_INFO("Policer %s is still referenced", key.c_str());
                it++;
                continue;
            }

            sai_status_t status = sai_policer_api->remove_policer(
                    m_syncdPolicers.at(key));
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                        key.c_str(), status);
                it++;
                continue;
            }

            SWSS_LOG_NOTICE("Removed policer %s", key.c_str());
            m_syncdPolicers.erase(key);
            m_policerRefCounts.erase(key);
            it = consumer.m_toSync.erase(it);
        }
    }
}
