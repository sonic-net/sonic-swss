#include "tokenize.h"
#include "qosorch.h"
#include "logger.h"
#include "crmorch.h"
#include "sai_serialize.h"
#include "cbf/nhgmaporch.h"

#include <inttypes.h>
#include <stdlib.h>
#include <sstream>
#include <iostream>
#include <string>
#include <climits>

using namespace std;

extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_scheduler_api_t *sai_scheduler_api;
extern sai_wred_api_t *sai_wred_api;
extern sai_qos_map_api_t *sai_qos_map_api;
extern sai_scheduler_group_api_t *sai_scheduler_group_api;
extern sai_acl_api_t* sai_acl_api;

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;
extern QosOrch *gQosOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

map<string, sai_ecn_mark_mode_t> ecn_map = {
    {"ecn_none", SAI_ECN_MARK_MODE_NONE},
    {"ecn_green", SAI_ECN_MARK_MODE_GREEN},
    {"ecn_yellow", SAI_ECN_MARK_MODE_YELLOW},
    {"ecn_red",SAI_ECN_MARK_MODE_RED},
    {"ecn_green_yellow", SAI_ECN_MARK_MODE_GREEN_YELLOW},
    {"ecn_green_red", SAI_ECN_MARK_MODE_GREEN_RED},
    {"ecn_yellow_red", SAI_ECN_MARK_MODE_YELLOW_RED},
    {"ecn_all", SAI_ECN_MARK_MODE_ALL}
};

enum {
    GREEN_DROP_PROBABILITY_SET  = (1U << 0),
    YELLOW_DROP_PROBABILITY_SET = (1U << 1),
    RED_DROP_PROBABILITY_SET    = (1U << 2)
};

// field_name is what is expected in CONFIG_DB PORT_QOS_MAP table
map<string, sai_port_attr_t> qos_to_attr_map = {
    {dscp_to_tc_field_name, SAI_PORT_ATTR_QOS_DSCP_TO_TC_MAP},
    {mpls_tc_to_tc_field_name, SAI_PORT_ATTR_QOS_MPLS_EXP_TO_TC_MAP},
    {dot1p_to_tc_field_name, SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP},
    {tc_to_queue_field_name, SAI_PORT_ATTR_QOS_TC_TO_QUEUE_MAP},
    {tc_to_pg_map_field_name, SAI_PORT_ATTR_QOS_TC_TO_PRIORITY_GROUP_MAP},
    {pfc_to_pg_map_name, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP},
    {pfc_to_queue_map_name, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_QUEUE_MAP},
    {scheduler_field_name, SAI_PORT_ATTR_QOS_SCHEDULER_PROFILE_ID},
    {dscp_to_fc_field_name, SAI_PORT_ATTR_QOS_DSCP_TO_FORWARDING_CLASS_MAP},
    {exp_to_fc_field_name, SAI_PORT_ATTR_QOS_MPLS_EXP_TO_FORWARDING_CLASS_MAP}
};

map<string, sai_meter_type_t> scheduler_meter_map = {
    {"packets", SAI_METER_TYPE_PACKETS},
    {"bytes", SAI_METER_TYPE_BYTES}
};

type_map QosOrch::m_qos_maps = {
    {CFG_DSCP_TO_TC_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_DOT1P_TO_TC_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_TC_TO_QUEUE_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_SCHEDULER_TABLE_NAME, new object_reference_map()},
    {CFG_WRED_PROFILE_TABLE_NAME, new object_reference_map()},
    {CFG_PORT_QOS_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_QUEUE_TABLE_NAME, new object_reference_map()},
    {CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_DSCP_TO_FC_MAP_TABLE_NAME, new object_reference_map()},
    {CFG_EXP_TO_FC_MAP_TABLE_NAME, new object_reference_map()},
};

map<string, string> qos_to_ref_table_map = {
    {dscp_to_tc_field_name, CFG_DSCP_TO_TC_MAP_TABLE_NAME},
    {mpls_tc_to_tc_field_name, CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME},
    {dot1p_to_tc_field_name, CFG_DOT1P_TO_TC_MAP_TABLE_NAME},
    {tc_to_queue_field_name, CFG_TC_TO_QUEUE_MAP_TABLE_NAME},
    {tc_to_pg_map_field_name, CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME},
    {pfc_to_pg_map_name, CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME},
    {pfc_to_queue_map_name, CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME},
    {scheduler_field_name, CFG_SCHEDULER_TABLE_NAME},
    {wred_profile_field_name, CFG_WRED_PROFILE_TABLE_NAME},
    {dscp_to_fc_field_name, CFG_DSCP_TO_FC_MAP_TABLE_NAME},
    {exp_to_fc_field_name, CFG_EXP_TO_FC_MAP_TABLE_NAME}
};

#define DSCP_MAX_VAL 63
#define EXP_MAX_VAL 7

task_process_status QosMapHandler::processWorkItem(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string qos_object_name = kfvKey(tuple);
    string qos_map_type_name = consumer.getTableName();
    string op = kfvOp(tuple);

    if (QosOrch::getTypeMap()[qos_map_type_name]->find(qos_object_name) != QosOrch::getTypeMap()[qos_map_type_name]->end())
    {
        sai_object = (*(QosOrch::getTypeMap()[qos_map_type_name]))[qos_object_name].m_saiObjectId;
    }
    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> attributes;
        if (!convertFieldValuesToAttributes(tuple, attributes))
        {
            return task_process_status::task_invalid_entry;
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            if (!modifyQosItem(sai_object, attributes))
            {
                SWSS_LOG_ERROR("Failed to set [%s:%s]", qos_map_type_name.c_str(), qos_object_name.c_str());
                freeAttribResources(attributes);
                return task_process_status::task_failed;
            }
            SWSS_LOG_NOTICE("Set [%s:%s]", qos_map_type_name.c_str(), qos_object_name.c_str());
        }
        else
        {
            sai_object = addQosItem(attributes);
            if (sai_object == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Failed to create [%s:%s]", qos_map_type_name.c_str(), qos_object_name.c_str());
                freeAttribResources(attributes);
                return task_process_status::task_failed;
            }
            (*(QosOrch::getTypeMap()[qos_map_type_name]))[qos_object_name].m_saiObjectId = sai_object;
            SWSS_LOG_NOTICE("Created [%s:%s]", qos_map_type_name.c_str(), qos_object_name.c_str());
        }
        freeAttribResources(attributes);
    }
    else if (op == DEL_COMMAND)
    {
        if (SAI_NULL_OBJECT_ID == sai_object)
        {
            SWSS_LOG_ERROR("Object with name:%s not found.", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        if (gQosOrch->isObjectBeingReferenced(QosOrch::getTypeMap(), qos_map_type_name, qos_object_name))
        {
            auto hint = gQosOrch->objectReferenceInfo(QosOrch::getTypeMap(), qos_map_type_name, qos_object_name);
            SWSS_LOG_NOTICE("Can't remove object %s due to being referenced (%s)", qos_object_name.c_str(), hint.c_str());
            return task_process_status::task_need_retry;
        }
        if (!removeQosItem(sai_object))
        {
            SWSS_LOG_ERROR("Failed to remove dscp_to_tc map. db name:%s sai object:%" PRIx64, qos_object_name.c_str(), sai_object);
            return task_process_status::task_failed;
        }
        auto it_to_delete = (QosOrch::getTypeMap()[qos_map_type_name])->find(qos_object_name);
        (QosOrch::getTypeMap()[qos_map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

bool QosMapHandler::modifyQosItem(sai_object_id_t sai_object, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status = sai_qos_map_api->set_qos_map_attribute(sai_object, &attributes[0]);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to modify map. status:%d", sai_status);
        return false;
    }
    return true;
}

bool QosMapHandler::removeQosItem(sai_object_id_t sai_object)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("Removing QosMap object:%" PRIx64, sai_object);
    sai_status_t sai_status = sai_qos_map_api->remove_qos_map(sai_object);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to remove map, status:%d", sai_status);
        return false;
    }
    return true;
}

void QosMapHandler::freeAttribResources(vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    delete[] attributes[0].value.qosmap.list;
}

bool DscpToTcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t list_attr;
    sai_qos_map_list_t dscp_map_list;
    dscp_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    dscp_map_list.list = new sai_qos_map_t[dscp_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        dscp_map_list.list[ind].key.dscp = (uint8_t)stoi(fvField(*i));
        dscp_map_list.list[ind].value.tc = (uint8_t)stoi(fvValue(*i));
        SWSS_LOG_DEBUG("key.dscp:%d, value.tc:%d", dscp_map_list.list[ind].key.dscp, dscp_map_list.list[ind].value.tc);
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = dscp_map_list.count;
    list_attr.value.qosmap.list = dscp_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

void DscpToTcMapHandler::applyDscpToTcMapToSwitch(sai_attr_id_t attr_id, sai_object_id_t map_id)
{
    SWSS_LOG_ENTER();
    bool rv = true;

    /* Query DSCP_TO_TC QoS map at switch capability */
    rv = gSwitchOrch->querySwitchDscpToTcCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP);
    if (rv == false)
    {
        SWSS_LOG_ERROR("Switch level DSCP to TC QoS map configuration is not supported");
        return;
    }

    /* Apply DSCP_TO_TC QoS map at switch */
    sai_attribute_t attr;
    attr.id = attr_id;
    attr.value.oid = map_id;

    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to apply DSCP_TO_TC QoS map to switch rv:%d", status);
        return;
    }

    if (map_id != gQosOrch->m_globalDscpToTcMap)
        gQosOrch->m_globalDscpToTcMap = map_id;

    SWSS_LOG_NOTICE("Applied DSCP_TO_TC QoS map to switch successfully");
}

sai_object_id_t DscpToTcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;

    sai_attribute_t qos_map_attr;
    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.u32 = SAI_QOS_MAP_TYPE_DSCP_TO_TC;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create dscp_to_tc map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object:%" PRIx64, sai_object);

    applyDscpToTcMapToSwitch(SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP, sai_object);

    return sai_object;
}

bool DscpToTcMapHandler::removeQosItem(sai_object_id_t sai_object)
{
    SWSS_LOG_ENTER();

    if (sai_object == gQosOrch->m_globalDscpToTcMap)
    {
        // The current global dscp to tc map is about to be removed.
        // Find another one to set to the switch or NULL in case this is the last one
        const auto &dscpToTcObjects = (*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME]);
        bool found = false;
        for (const auto &ref : dscpToTcObjects)
        {
            if (ref.second.m_saiObjectId == sai_object)
                continue;
            SWSS_LOG_NOTICE("Current global dscp_to_tc map is about to be removed, set it to %s %" PRIx64, ref.first.c_str(), ref.second.m_saiObjectId);
            applyDscpToTcMapToSwitch(SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP, ref.second.m_saiObjectId);
            found = true;
            break;
        }
        if (!found)
        {
            applyDscpToTcMapToSwitch(SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP, SAI_NULL_OBJECT_ID);
        }
    }

    SWSS_LOG_DEBUG("Removing DscpToTcMap object:%" PRIx64, sai_object);
    sai_status_t sai_status = sai_qos_map_api->remove_qos_map(sai_object);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to remove DSCP_TO_TC map, status:%d", sai_status);
        return false;
    }
    return true;
}

task_process_status QosOrch::handleDscpToTcTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    DscpToTcMapHandler dscp_tc_handler;
    return dscp_tc_handler.processWorkItem(consumer);
}

bool MplsTcToTcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t list_attr;
    sai_qos_map_list_t exp_map_list;
    exp_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    exp_map_list.list = new sai_qos_map_t[exp_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        exp_map_list.list[ind].key.mpls_exp = (uint8_t)stoi(fvField(*i));
        exp_map_list.list[ind].value.tc = (uint8_t)stoi(fvValue(*i));
        SWSS_LOG_DEBUG("key.exp:%d, value.tc:%d", exp_map_list.list[ind].key.mpls_exp, exp_map_list.list[ind].value.tc);
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = exp_map_list.count;
    list_attr.value.qosmap.list = exp_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t MplsTcToTcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;

    sai_attribute_t qos_map_attr;
    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.u32 = SAI_QOS_MAP_TYPE_MPLS_EXP_TO_TC;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create exp_to_tc map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object:%" PRIx64, sai_object);
    return sai_object;
}

task_process_status QosOrch::handleMplsTcToTcTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    MplsTcToTcMapHandler mpls_tc_to_tc_handler;
    return mpls_tc_to_tc_handler.processWorkItem(consumer);
}

bool Dot1pToTcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_qos_map_list_t dot1p_map_list;

    // Allocated resources are freed in freeAttribResources() call
    dot1p_map_list.list = new sai_qos_map_t[kfvFieldsValues(tuple).size()]();
    int i = 0;
    for (const auto &fv : kfvFieldsValues(tuple))
    {
        try
        {
            dot1p_map_list.list[i].key.dot1p = static_cast<sai_uint8_t>(stoi(fvField(fv)));
            dot1p_map_list.list[i].value.tc = static_cast<sai_cos_t>(stoi(fvValue(fv)));
        }
        catch (const std::invalid_argument &e)
        {
            SWSS_LOG_ERROR("Invalid dot1p to tc argument %s:%s to %s()", fvField(fv).c_str(), fvValue(fv).c_str(), e.what());
            continue;
        }
        catch (const std::out_of_range &e)
        {
            SWSS_LOG_ERROR("Out of range dot1p to tc argument %s:%s to %s()", fvField(fv).c_str(), fvValue(fv).c_str(), e.what());
            continue;
        }

        i++;
    }
    dot1p_map_list.count = static_cast<uint32_t>(i);

    sai_attribute_t attr;
    attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    attr.value.qosmap.count = dot1p_map_list.count;
    attr.value.qosmap.list = dot1p_map_list.list;
    attributes.push_back(attr);

    return true;
}

sai_object_id_t Dot1pToTcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    vector<sai_attribute_t> attrs;

    sai_attribute_t attr;
    attr.id = SAI_QOS_MAP_ATTR_TYPE;
    attr.value.u32 = SAI_QOS_MAP_TYPE_DOT1P_TO_TC;
    attrs.push_back(attr);

    attrs.push_back(attributes[0]);

    sai_object_id_t object_id;
    sai_status_t sai_status = sai_qos_map_api->create_qos_map(&object_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create dot1p_to_tc map. status: %s", sai_serialize_status(sai_status).c_str());
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object: 0x%" PRIx64, object_id);
    return object_id;
}

task_process_status QosOrch::handleDot1pToTcTable(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    Dot1pToTcMapHandler dot1p_tc_handler;
    return dot1p_tc_handler.processWorkItem(consumer);
}

bool TcToQueueMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t list_attr;
    sai_qos_map_list_t tc_map_list;
    tc_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    tc_map_list.list = new sai_qos_map_t[tc_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        tc_map_list.list[ind].key.tc = (uint8_t)stoi(fvField(*i));
        tc_map_list.list[ind].value.queue_index = (uint8_t)stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = tc_map_list.count;
    list_attr.value.qosmap.list = tc_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t TcToQueueMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;
    sai_attribute_t qos_map_attr;

    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.s32 = SAI_QOS_MAP_TYPE_TC_TO_QUEUE;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;
}

task_process_status QosOrch::handleTcToQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    TcToQueueMapHandler tc_queue_handler;
    return tc_queue_handler.processWorkItem(consumer);
}

void WredMapHandler::freeAttribResources(vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
}

bool WredMapHandler::convertBool(string str, bool &val)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("input:%s", str.c_str());
    if("true" == str)
    {
        val = true;
    }
    else if ("false" == str)
    {
        val = false;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid input specified");
        return false;
    }
    return true;
}

bool WredMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == yellow_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == yellow_min_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_MIN_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == green_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_GREEN_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == green_min_threshold_field_name)
        {
           attr.id = SAI_WRED_ATTR_GREEN_MIN_THRESHOLD;
           attr.value.s32 = stoi(fvValue(*i));
           attribs.push_back(attr);
        }
        else if (fvField(*i) == red_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == red_min_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_MIN_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == green_drop_probability_field_name)
        {
            attr.id = SAI_WRED_ATTR_GREEN_DROP_PROBABILITY;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == yellow_drop_probability_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_DROP_PROBABILITY;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == red_drop_probability_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_DROP_PROBABILITY;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_green_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_GREEN_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_yellow_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_red_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == ecn_field_name)
        {
            attr.id = SAI_WRED_ATTR_ECN_MARK_MODE;
            sai_ecn_mark_mode_t ecn = ecn_map.at(fvValue(*i));
            attr.value.s32 = ecn;
            attribs.push_back(attr);
        }
        else {
            SWSS_LOG_ERROR("Unknown wred profile field:%s", fvField(*i).c_str());
            return false;
        }
    }
    return true;
}

bool WredMapHandler::modifyQosItem(sai_object_id_t sai_object, vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    for (auto attr : attribs)
    {
        sai_status = sai_wred_api->set_wred_attribute(sai_object, &attr);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set wred profile attribute, id:%d, status:%d", attr.id, sai_status);
            return false;
        }
    }
    return true;
}

sai_object_id_t WredMapHandler::addQosItem(const vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    uint8_t drop_prob_set = 0;

    attr.id = SAI_WRED_ATTR_WEIGHT;
    attr.value.s32 = 0;
    attrs.push_back(attr);

    for(auto attrib : attribs)
    {
        attrs.push_back(attrib);

        if (attrib.id == SAI_WRED_ATTR_GREEN_DROP_PROBABILITY)
        {
            drop_prob_set |= GREEN_DROP_PROBABILITY_SET;
        }
        else if (attrib.id == SAI_WRED_ATTR_YELLOW_DROP_PROBABILITY)
        {
            drop_prob_set |= YELLOW_DROP_PROBABILITY_SET;
        }
        else if (attrib.id == SAI_WRED_ATTR_RED_DROP_PROBABILITY)
        {
            drop_prob_set |= RED_DROP_PROBABILITY_SET;
        }
    }
    if (!(drop_prob_set & GREEN_DROP_PROBABILITY_SET))
    {
        attr.id = SAI_WRED_ATTR_GREEN_DROP_PROBABILITY;
        attr.value.s32 = 100;
        attrs.push_back(attr);
    }
    if (!(drop_prob_set & YELLOW_DROP_PROBABILITY_SET))
    {
        attr.id = SAI_WRED_ATTR_YELLOW_DROP_PROBABILITY;
        attr.value.s32 = 100;
        attrs.push_back(attr);
    }
    if (!(drop_prob_set & RED_DROP_PROBABILITY_SET))
    {
        attr.id = SAI_WRED_ATTR_RED_DROP_PROBABILITY;
        attr.value.s32 = 100;
        attrs.push_back(attr);
    }

    sai_status = sai_wred_api->create_wred(&sai_object, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create wred profile: %d", sai_status);
        return false;
    }
    return sai_object;
}

bool WredMapHandler::removeQosItem(sai_object_id_t sai_object)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_status = sai_wred_api->remove_wred(sai_object);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to remove scheduler profile, status:%d", sai_status);
        return false;
    }
    return true;
}

task_process_status QosOrch::handleWredProfileTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    WredMapHandler wred_handler;
    return wred_handler.processWorkItem(consumer);
}

bool TcToPgHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  tc_to_pg_map_list;
    tc_to_pg_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    tc_to_pg_map_list.list = new sai_qos_map_t[tc_to_pg_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        tc_to_pg_map_list.list[ind].key.tc = (uint8_t)stoi(fvField(*i));
        tc_to_pg_map_list.list[ind].value.pg = (uint8_t)stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = tc_to_pg_map_list.count;
    list_attr.value.qosmap.list = tc_to_pg_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t TcToPgHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;
    sai_attribute_t qos_map_attr;

    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.s32 = SAI_QOS_MAP_TYPE_TC_TO_PRIORITY_GROUP;
    qos_map_attrs.push_back(qos_map_attr);
    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

task_process_status QosOrch::handleTcToPgTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    TcToPgHandler tc_to_pg_handler;
    return tc_to_pg_handler.processWorkItem(consumer);
}

bool PfcPrioToPgHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  pfc_prio_to_pg_map_list;
    pfc_prio_to_pg_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    pfc_prio_to_pg_map_list.list = new sai_qos_map_t[pfc_prio_to_pg_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        pfc_prio_to_pg_map_list.list[ind].key.prio = (uint8_t)stoi(fvField(*i));
        pfc_prio_to_pg_map_list.list[ind].value.pg = (uint8_t)stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = pfc_prio_to_pg_map_list.count;
    list_attr.value.qosmap.list = pfc_prio_to_pg_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t PfcPrioToPgHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;
    sai_attribute_t qos_map_attr;

    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.s32 = SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_PRIORITY_GROUP;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

task_process_status QosOrch::handlePfcPrioToPgTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    PfcPrioToPgHandler pfc_prio_to_pg_handler;
    return pfc_prio_to_pg_handler.processWorkItem(consumer);
}

bool PfcToQueueHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  pfc_to_queue_map_list;
    pfc_to_queue_map_list.count = (uint32_t)kfvFieldsValues(tuple).size();
    pfc_to_queue_map_list.list = new sai_qos_map_t[pfc_to_queue_map_list.count]();
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        pfc_to_queue_map_list.list[ind].key.prio = (uint8_t)stoi(fvField(*i));
        pfc_to_queue_map_list.list[ind].value.queue_index = (uint8_t)stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = pfc_to_queue_map_list.count;
    list_attr.value.qosmap.list = pfc_to_queue_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t PfcToQueueHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;

    vector<sai_attribute_t> qos_map_attrs;
    sai_attribute_t qos_map_attr;

    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.s32 = SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_QUEUE;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

bool DscpToFcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();

    sai_uint8_t max_num_fcs = NhgMapOrch::getMaxNumFcs();

    sai_attribute_t list_attr;
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = (uint32_t)kfvFieldsValues(tuple).size();
    list_attr.value.qosmap.list = new sai_qos_map_t[list_attr.value.qosmap.count]();
    uint32_t ind = 0;

    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        try
        {
            auto value = stoi(fvField(*i));
            if (value < 0)
            {
                SWSS_LOG_ERROR("DSCP value %d is negative", value);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            else if (value > DSCP_MAX_VAL)
            {
                SWSS_LOG_ERROR("DSCP value %d is greater than max value %d", value, DSCP_MAX_VAL);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            list_attr.value.qosmap.list[ind].key.dscp = static_cast<sai_uint8_t>(value);

            // FC value must be in range [0, max_num_fcs)
            value = stoi(fvValue(*i));
            if ((value < 0) || (value >= max_num_fcs))
            {
                SWSS_LOG_ERROR("FC value %d is either negative, or bigger than max value %d", value, max_num_fcs - 1);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            list_attr.value.qosmap.list[ind].value.fc = static_cast<sai_uint8_t>(value);

            SWSS_LOG_DEBUG("key.dscp:%d, value.fc:%d",
                            list_attr.value.qosmap.list[ind].key.dscp,
                            list_attr.value.qosmap.list[ind].value.fc);
        }
        catch(const invalid_argument& e)
        {
            SWSS_LOG_ERROR("Got exception during conversion: %s", e.what());
            delete[] list_attr.value.qosmap.list;
            return false;
        }
    }
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t DscpToFcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;

    sai_attribute_t qos_map_attr;
    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.u32 = SAI_QOS_MAP_TYPE_DSCP_TO_FORWARDING_CLASS;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object,
                                                gSwitchId,
                                                (uint32_t)qos_map_attrs.size(),
                                                qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create dscp_to_fc map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object:%" PRIx64, sai_object);
    return sai_object;
}

task_process_status QosOrch::handleDscpToFcTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    DscpToFcMapHandler dscp_fc_handler;
    return dscp_fc_handler.processWorkItem(consumer);
}

bool ExpToFcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple,
                                                        vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();

    sai_uint8_t max_num_fcs = NhgMapOrch::getMaxNumFcs();

    sai_attribute_t list_attr;
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = (uint32_t)kfvFieldsValues(tuple).size();
    list_attr.value.qosmap.list = new sai_qos_map_t[list_attr.value.qosmap.count]();
    uint32_t ind = 0;

    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        try
        {
            auto value = stoi(fvField(*i));
            if (value < 0)
            {
                SWSS_LOG_ERROR("EXP value %d is negative", value);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            else if (value > EXP_MAX_VAL)
            {
                SWSS_LOG_ERROR("EXP value %d is greater than max value %d", value, EXP_MAX_VAL);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            list_attr.value.qosmap.list[ind].key.mpls_exp = static_cast<sai_uint8_t>(value);

            // FC value must be in range [0, max_num_fcs)
            value = stoi(fvValue(*i));
            if ((value < 0) || (value >= max_num_fcs))
            {
                SWSS_LOG_ERROR("FC value %d is either negative, or bigger than max value %hu", value, max_num_fcs - 1);
                delete[] list_attr.value.qosmap.list;
                return false;
            }
            list_attr.value.qosmap.list[ind].value.fc = static_cast<sai_uint8_t>(value);

            SWSS_LOG_DEBUG("key.mpls_exp:%d, value.fc:%d",
                            list_attr.value.qosmap.list[ind].key.mpls_exp,
                            list_attr.value.qosmap.list[ind].value.fc);
        }
        catch(const invalid_argument& e)
        {
            SWSS_LOG_ERROR("Got exception during conversion: %s", e.what());
            delete[] list_attr.value.qosmap.list;
            return false;
        }
    }
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t ExpToFcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    vector<sai_attribute_t> qos_map_attrs;

    sai_attribute_t qos_map_attr;
    qos_map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attr.value.u32 = SAI_QOS_MAP_TYPE_MPLS_EXP_TO_FORWARDING_CLASS;
    qos_map_attrs.push_back(qos_map_attr);

    qos_map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attr.value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attr.value.qosmap.list = attributes[0].value.qosmap.list;
    qos_map_attrs.push_back(qos_map_attr);

    sai_status = sai_qos_map_api->create_qos_map(&sai_object, gSwitchId, (uint32_t)qos_map_attrs.size(), qos_map_attrs.data());
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create exp_to_fc map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object:%" PRIx64, sai_object);
    return sai_object;
}

task_process_status QosOrch::handleExpToFcTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    ExpToFcMapHandler exp_fc_handler;
    return exp_fc_handler.processWorkItem(consumer);
}

task_process_status QosOrch::handlePfcToQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    PfcToQueueHandler pfc_to_queue_handler;
    return pfc_to_queue_handler.processWorkItem(consumer);
}

QosOrch::QosOrch(DBConnector *db, vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    initTableHandlers();
};

type_map& QosOrch::getTypeMap()
{
    SWSS_LOG_ENTER();
    return m_qos_maps;
}

void QosOrch::initTableHandlers()
{
    SWSS_LOG_ENTER();
    m_qos_handler_map.insert(qos_handler_pair(CFG_DSCP_TO_TC_MAP_TABLE_NAME, &QosOrch::handleDscpToTcTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME, &QosOrch::handleMplsTcToTcTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_DOT1P_TO_TC_MAP_TABLE_NAME, &QosOrch::handleDot1pToTcTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_TC_TO_QUEUE_MAP_TABLE_NAME, &QosOrch::handleTcToQueueTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_SCHEDULER_TABLE_NAME, &QosOrch::handleSchedulerTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_QUEUE_TABLE_NAME, &QosOrch::handleQueueTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_PORT_QOS_MAP_TABLE_NAME, &QosOrch::handlePortQosMapTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_WRED_PROFILE_TABLE_NAME, &QosOrch::handleWredProfileTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_DSCP_TO_FC_MAP_TABLE_NAME, &QosOrch::handleDscpToFcTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_EXP_TO_FC_MAP_TABLE_NAME, &QosOrch::handleExpToFcTable));

    m_qos_handler_map.insert(qos_handler_pair(CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, &QosOrch::handleTcToPgTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME, &QosOrch::handlePfcPrioToPgTable));
    m_qos_handler_map.insert(qos_handler_pair(CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME, &QosOrch::handlePfcToQueueTable));
}

task_process_status QosOrch::handleSchedulerTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;
    string qos_map_type_name = CFG_SCHEDULER_TABLE_NAME;
    string qos_object_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    if (m_qos_maps[qos_map_type_name]->find(qos_object_name) != m_qos_maps[qos_map_type_name]->end())
    {
        sai_object = (*(m_qos_maps[qos_map_type_name]))[qos_object_name].m_saiObjectId;
        if (sai_object == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Error sai_object must exist for key %s", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
    }
    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> sai_attr_list;
        sai_attribute_t attr;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            if (fvField(*i) == scheduler_algo_type_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_SCHEDULING_TYPE;
                if (fvValue(*i) == scheduler_algo_DWRR)
                {
                    attr.value.s32 = SAI_SCHEDULING_TYPE_DWRR;
                }
                else if (fvValue(*i) == scheduler_algo_WRR)
                {
                    attr.value.s32 = SAI_SCHEDULING_TYPE_WRR;
                }
                else if (fvValue(*i) == scheduler_algo_STRICT)
                {
                    attr.value.s32 = SAI_SCHEDULING_TYPE_STRICT;
                }
                else {
                    SWSS_LOG_ERROR("Unknown scheduler type value:%s", fvField(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_weight_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_SCHEDULING_WEIGHT;
                attr.value.u8 = (uint8_t)stoi(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_priority_field_name)
            {
                // TODO: The meaning is to be able to adjust priority of the given scheduler group.
                // However currently SAI model does not provide such ability.
            }
            else if (fvField(*i) == scheduler_meter_type_field_name)
            {
                sai_meter_type_t meter_value = scheduler_meter_map.at(fvValue(*i));
                attr.id = SAI_SCHEDULER_ATTR_METER_TYPE;
                attr.value.s32 = meter_value;
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_min_bandwidth_rate_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_MIN_BANDWIDTH_RATE;
                attr.value.u64 = stoull(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_min_bandwidth_burst_rate_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_MIN_BANDWIDTH_BURST_RATE;
                attr.value.u64 = stoull(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_max_bandwidth_rate_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_MAX_BANDWIDTH_RATE;
                attr.value.u64 = stoull(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_max_bandwidth_burst_rate_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_MAX_BANDWIDTH_BURST_RATE;
                attr.value.u64 = stoull(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else {
                SWSS_LOG_ERROR("Unknown field:%s", fvField(*i).c_str());
                return task_process_status::task_invalid_entry;
            }
        }

        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            for (auto attr : sai_attr_list)
            {
                sai_status = sai_scheduler_api->set_scheduler_attribute(sai_object, &attr);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("fail to set scheduler attribute, id:%d", attr.id);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_SCHEDULER, sai_status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return handle_status;
                    }
                }
            }
        }
        else
        {
            sai_status = sai_scheduler_api->create_scheduler(&sai_object, gSwitchId, (uint32_t)sai_attr_list.size(), sai_attr_list.data());
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create scheduler profile [%s:%s], rv:%d",
                               qos_map_type_name.c_str(), qos_object_name.c_str(), sai_status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_SCHEDULER, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return handle_status;
                }
            }
            SWSS_LOG_NOTICE("Created [%s:%s]", qos_map_type_name.c_str(), qos_object_name.c_str());
            (*(m_qos_maps[qos_map_type_name]))[qos_object_name].m_saiObjectId = sai_object;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (SAI_NULL_OBJECT_ID == sai_object)
        {
            SWSS_LOG_ERROR("Object with name:%s not found.", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        if (gQosOrch->isObjectBeingReferenced(QosOrch::getTypeMap(), qos_map_type_name, qos_object_name))
        {
            auto hint = gQosOrch->objectReferenceInfo(QosOrch::getTypeMap(), qos_map_type_name, qos_object_name);
            SWSS_LOG_NOTICE("Can't remove object %s due to being referenced (%s)", qos_object_name.c_str(), hint.c_str());
            return task_process_status::task_need_retry;
        }
        sai_status = sai_scheduler_api->remove_scheduler(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove scheduler profile. db name:%s sai object:%" PRIx64, qos_object_name.c_str(), sai_object);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_SCHEDULER, sai_status);
            if (handle_status != task_process_status::task_success)
            {
                return handle_status;
            }
        }
        auto it_to_delete = (m_qos_maps[qos_map_type_name])->find(qos_object_name);
        (m_qos_maps[qos_map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

sai_object_id_t QosOrch::getSchedulerGroup(const Port &port, const sai_object_id_t queue_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t    sai_status;

    const auto it = m_scheduler_group_port_info.find(port.m_port_id);
    if (it == m_scheduler_group_port_info.end())
    {
        /* Get max sched groups count */
        attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_SCHEDULER_GROUPS;
        sai_status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to get number of scheduler groups for port:%s", port.m_alias.c_str());
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, sai_status);
            if (handle_status != task_process_status::task_success)
            {
                return SAI_NULL_OBJECT_ID;
            }
        }

        /* Get total groups list on the port */
        uint32_t groups_count = attr.value.u32;
        std::vector<sai_object_id_t> groups(groups_count);

        attr.id = SAI_PORT_ATTR_QOS_SCHEDULER_GROUP_LIST;
        attr.value.objlist.list = groups.data();
        attr.value.objlist.count = groups_count;
        sai_status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to get scheduler group list for port:%s", port.m_alias.c_str());
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, sai_status);
            if (handle_status != task_process_status::task_success)
            {
                return SAI_NULL_OBJECT_ID;
            }
        }

        m_scheduler_group_port_info[port.m_port_id] = {
            .groups = std::move(groups),
            .child_groups = std::vector<std::vector<sai_object_id_t>>(groups_count),
            .group_has_been_initialized = std::vector<bool>(groups_count)
        };

        SWSS_LOG_INFO("Port %s has been initialized with %u group(s)", port.m_alias.c_str(), groups_count);
    }

    /* Lookup groups to which queue belongs */
    auto& scheduler_group_port_info = m_scheduler_group_port_info[port.m_port_id];
    const auto& groups = scheduler_group_port_info.groups;
    for (uint32_t ii = 0; ii < groups.size() ; ii++)
    {
        const auto& group_id = groups[ii];
        const auto& child_groups_per_group = scheduler_group_port_info.child_groups[ii];
        if (child_groups_per_group.empty())
        {
            if (scheduler_group_port_info.group_has_been_initialized[ii])
            {
                // skip this iteration if it has been initialized which means there're no children in this group
                SWSS_LOG_INFO("No child group for port %s group 0x%" PRIx64 ", skip", port.m_alias.c_str(), group_id);
                continue;
            }

            attr.id = SAI_SCHEDULER_GROUP_ATTR_CHILD_COUNT;//Number of queues/groups childs added to scheduler group
            sai_status = sai_scheduler_group_api->get_scheduler_group_attribute(group_id, 1, &attr);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to get child count for scheduler group:0x%" PRIx64 " of port:%s", group_id, port.m_alias.c_str());
                task_process_status handle_status = handleSaiGetStatus(SAI_API_SCHEDULER_GROUP, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return SAI_NULL_OBJECT_ID;
                }
            }

            uint32_t child_count = attr.value.u32;

            SWSS_LOG_INFO("Port %s group 0x%" PRIx64 " has been initialized with %u child group(s)", port.m_alias.c_str(), group_id, child_count);
            scheduler_group_port_info.group_has_been_initialized[ii] = true;

            // skip this iteration if there're no children in this group
            if (child_count == 0)
            {
                continue;
            }

            vector<sai_object_id_t> child_groups(child_count);
            attr.id = SAI_SCHEDULER_GROUP_ATTR_CHILD_LIST;
            attr.value.objlist.list = child_groups.data();
            attr.value.objlist.count = child_count;
            sai_status = sai_scheduler_group_api->get_scheduler_group_attribute(group_id, 1, &attr);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed to get child list for scheduler group:0x%" PRIx64 " of port:%s", group_id, port.m_alias.c_str());
                task_process_status handle_status = handleSaiGetStatus(SAI_API_SCHEDULER_GROUP, sai_status);
                if (handle_status != task_process_status::task_success)
                {
                    return SAI_NULL_OBJECT_ID;
                }
            }

            scheduler_group_port_info.child_groups[ii] = std::move(child_groups);
        }

        for (const auto& child_group_id: child_groups_per_group)
        {
            if (child_group_id == queue_id)
            {
                return group_id;
            }
        }
    }

    return SAI_NULL_OBJECT_ID;
}

bool QosOrch::applySchedulerToQueueSchedulerGroup(Port &port, size_t queue_ind, sai_object_id_t scheduler_profile_id)
{
    SWSS_LOG_ENTER();

    if (port.m_queue_ids.size() <= queue_ind)
    {
        SWSS_LOG_ERROR("Invalid queue index specified:%zd", queue_ind);
        return false;
    }

    const sai_object_id_t queue_id = port.m_queue_ids[queue_ind];

    const sai_object_id_t group_id = getSchedulerGroup(port, queue_id);
    if(group_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to find a scheduler group for port: %s queue: %zu", port.m_alias.c_str(), queue_ind);
        return false;
    }

    /* Apply scheduler profile to all port groups  */
    sai_attribute_t attr;
    sai_status_t    sai_status;

    attr.id = SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID;
    attr.value.oid = scheduler_profile_id;

    sai_status = sai_scheduler_group_api->set_scheduler_group_attribute(group_id, &attr);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed applying scheduler profile:0x%" PRIx64 " to scheduler group:0x%" PRIx64 ", port:%s", scheduler_profile_id, group_id, port.m_alias.c_str());
        task_process_status handle_status = handleSaiSetStatus(SAI_API_SCHEDULER_GROUP, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_DEBUG("port:%s, scheduler_profile_id:0x%" PRIx64 " applied to scheduler group:0x%" PRIx64, port.m_alias.c_str(), scheduler_profile_id, group_id);

    return true;
}

bool QosOrch::applyWredProfileToQueue(Port &port, size_t queue_ind, sai_object_id_t sai_wred_profile)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t    sai_status;
    sai_object_id_t queue_id;

    if (port.m_queue_ids.size() <= queue_ind)
    {
        SWSS_LOG_ERROR("Invalid queue index specified:%zd", queue_ind);
        return false;
    }
    queue_id = port.m_queue_ids[queue_ind];

    attr.id = SAI_QUEUE_ATTR_WRED_PROFILE_ID;
    attr.value.oid = sai_wred_profile;
    sai_status = sai_queue_api->set_queue_attribute(queue_id, &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set queue attribute:%d", sai_status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_QUEUE, sai_status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

task_process_status QosOrch::handleQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    Port port;
    bool result;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    size_t queue_ind = 0;
    vector<string> tokens;

    sai_uint32_t range_low, range_high;
    vector<string> port_names;

    ref_resolve_status  resolve_result;
    // sample "QUEUE: {Ethernet4|0-1}"
    tokens = tokenize(key, config_db_key_delimiter);
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    port_names = tokenize(tokens[0], list_item_delimiter);
    if (!parseIndexRange(tokens[1], range_low, range_high))
    {
        SWSS_LOG_ERROR("Failed to parse range:%s", tokens[1].c_str());
        return task_process_status::task_invalid_entry;
    }

    bool donotChangeScheduler = false;
    bool donotChangeWredProfile = false;
    sai_object_id_t sai_scheduler_profile;
    sai_object_id_t sai_wred_profile;

    if (op == SET_COMMAND)
    {
        string scheduler_profile_name;
        resolve_result = resolveFieldRefValue(m_qos_maps, scheduler_field_name,
                                              qos_to_ref_table_map.at(scheduler_field_name), tuple,
                                              sai_scheduler_profile, scheduler_profile_name);
        if (ref_resolve_status::success != resolve_result)
        {
            if (resolve_result != ref_resolve_status::field_not_found)
            {
                if(ref_resolve_status::not_resolved == resolve_result)
                {
                    SWSS_LOG_INFO("Missing or invalid scheduler reference");
                    return task_process_status::task_need_retry;
                }
                SWSS_LOG_ERROR("Resolving scheduler reference failed");
                return task_process_status::task_failed;
            }

            if (doesObjectExist(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, scheduler_field_name, scheduler_profile_name))
            {
                SWSS_LOG_NOTICE("QUEUE|%s %s was configured but is not any more. Remove it", key.c_str(), scheduler_field_name.c_str());
                removeMeFromObjsReferencedByMe(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, scheduler_field_name, scheduler_profile_name);
                sai_scheduler_profile = SAI_NULL_OBJECT_ID;
            }
            else
            {
                // Did not exist and do not exist. No action
                donotChangeScheduler = true;
            }
        }
        else
        {
            setObjectReference(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, scheduler_field_name, scheduler_profile_name);
            SWSS_LOG_INFO("QUEUE %s Field %s %s has been resolved to %" PRIx64 , key.c_str(), scheduler_field_name.c_str(), scheduler_profile_name.c_str(), sai_scheduler_profile);
        }

        string wred_profile_name;
        resolve_result = resolveFieldRefValue(m_qos_maps, wred_profile_field_name,
                                              qos_to_ref_table_map.at(wred_profile_field_name), tuple,
                                              sai_wred_profile, wred_profile_name);
        if (ref_resolve_status::success != resolve_result)
        {
            if (resolve_result != ref_resolve_status::field_not_found)
            {
                if(ref_resolve_status::not_resolved == resolve_result)
                {
                    SWSS_LOG_INFO("Missing or invalid wred profile reference");
                    return task_process_status::task_need_retry;
                }
                SWSS_LOG_ERROR("Resolving wred profile reference failed");
                return task_process_status::task_failed;
            }

            if (doesObjectExist(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, wred_profile_field_name, wred_profile_name))
            {
                SWSS_LOG_NOTICE("QUEUE|%s %s was configured but is not any more. Remove it", key.c_str(), wred_profile_field_name.c_str());
                removeMeFromObjsReferencedByMe(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, wred_profile_field_name, wred_profile_name);
                sai_wred_profile = SAI_NULL_OBJECT_ID;
            }
            else
            {
                donotChangeWredProfile = true;
            }
        }
        else
        {
            setObjectReference(m_qos_maps, CFG_QUEUE_TABLE_NAME, key, wred_profile_field_name, wred_profile_name);
        }
    }
    else if (op == DEL_COMMAND)
    {
        removeObject(QosOrch::getTypeMap(), CFG_QUEUE_TABLE_NAME, key);
        sai_scheduler_profile = SAI_NULL_OBJECT_ID;
        sai_wred_profile = SAI_NULL_OBJECT_ID;
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return task_process_status::task_invalid_entry;
    }

    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        SWSS_LOG_DEBUG("processing range:%d-%d", range_low, range_high);
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            queue_ind = ind;
            SWSS_LOG_DEBUG("processing queue:%zd", queue_ind);

            if (!donotChangeScheduler)
            {
                result = applySchedulerToQueueSchedulerGroup(port, queue_ind, sai_scheduler_profile);

                if (!result)
                {
                    SWSS_LOG_ERROR("Failed setting field:%s to port:%s, queue:%zd, line:%d", scheduler_field_name.c_str(), port.m_alias.c_str(), queue_ind, __LINE__);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Applied scheduler to port:%s", port_name.c_str());
            }

            if (!donotChangeWredProfile)
            {
                result = applyWredProfileToQueue(port, queue_ind, sai_wred_profile);

                if (!result)
                {
                    SWSS_LOG_ERROR("Failed setting field:%s to port:%s, queue:%zd, line:%d", wred_profile_field_name.c_str(), port.m_alias.c_str(), queue_ind, __LINE__);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Applied wred profile to port:%s", port_name.c_str());
            }
        }
    }
    SWSS_LOG_DEBUG("finished");
    return task_process_status::task_success;
}

bool QosOrch::applyMapToPort(Port &port, sai_attr_id_t attr_id, sai_object_id_t map_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = attr_id;
    attr.value.oid = map_id;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed setting sai object:%" PRIx64 " for port:%s, status:%d", map_id, port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

task_process_status QosOrch::ResolveMapAndApplyToPort(
    Port                    &port,
    sai_port_attr_t         port_attr,
    string                  field_name,
    KeyOpFieldsValuesTuple  &tuple,
    string                  op)
{
    SWSS_LOG_ENTER();

    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    string object_name;
    bool result;
    ref_resolve_status resolve_result = resolveFieldRefValue(m_qos_maps, field_name,
                           qos_to_ref_table_map.at(field_name), tuple, sai_object, object_name);
    if (ref_resolve_status::success == resolve_result)
    {
        if (op == SET_COMMAND)
        {
            result = applyMapToPort(port, port_attr, sai_object);
        }
        else if (op == DEL_COMMAND)
        {
            // NOTE: The map is un-bound from the port. But the map itself still exists.
            result = applyMapToPort(port, port_attr, SAI_NULL_OBJECT_ID);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            return task_process_status::task_invalid_entry;
        }
        if (!result)
        {
            SWSS_LOG_ERROR("Failed setting field:%s to port:%s, line:%d", field_name.c_str(), port.m_alias.c_str(), __LINE__);
            return task_process_status::task_failed;
        }
        SWSS_LOG_DEBUG("Applied field:%s to port:%s, line:%d", field_name.c_str(), port.m_alias.c_str(), __LINE__);
        return task_process_status::task_success;
    }
    else if (resolve_result != ref_resolve_status::field_not_found)
    {
        if(ref_resolve_status::not_resolved == resolve_result)
        {
            SWSS_LOG_INFO("Missing or invalid %s reference", field_name.c_str());
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving %s reference failed", field_name.c_str());
        return task_process_status::task_failed;
    }
    return task_process_status::task_success;
}

task_process_status QosOrch::handlePortQosMapTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> port_names = tokenize(key, list_item_delimiter);

    if (op == DEL_COMMAND)
    {
        /* Handle DEL command. Just set all the maps to oid:0x0 */
        for (string port_name : port_names)
        {
            Port port;

            /* Skip port which is not found */
            if (!gPortsOrch->getPort(port_name, port))
            {
                SWSS_LOG_ERROR("Failed to apply QoS maps to port %s. Port is not found.", port_name.c_str());
                continue;
            }

            for (auto &mapRef : qos_to_attr_map)
            {
                string referenced_obj;
                if (!doesObjectExist(m_qos_maps, CFG_PORT_QOS_MAP_TABLE_NAME, key, mapRef.first, referenced_obj))
                {
                    continue;
                }

                sai_attribute_t attr;
                attr.id = mapRef.second;
                attr.value.oid = SAI_NULL_OBJECT_ID;

                sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove %s on port %s, rv:%d",
                                   mapRef.first.c_str(), port_name.c_str(), status);
                    task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
                    if (handle_status != task_process_status::task_success)
                    {
                        return task_process_status::task_invalid_entry;
                    }
                }
                SWSS_LOG_INFO("Removed %s on port %s", mapRef.first.c_str(), port_name.c_str());
            }

            if (!gPortsOrch->setPortPfc(port.m_port_id, 0))
            {
                SWSS_LOG_ERROR("Failed to disable PFC on port %s", port_name.c_str());
            }

            SWSS_LOG_INFO("Disabled PFC on port %s", port_name.c_str());
        }

        removeObject(m_qos_maps, CFG_PORT_QOS_MAP_TABLE_NAME, key);

        return task_process_status::task_success;
    }

    sai_uint8_t pfc_enable = 0;
    map<sai_port_attr_t, pair<string, sai_object_id_t>> update_list;
    for (auto it = kfvFieldsValues(tuple).begin(); it != kfvFieldsValues(tuple).end(); it++)
    {
        /* Check all map instances are created before applying to ports */
        if (qos_to_attr_map.find(fvField(*it)) != qos_to_attr_map.end())
        {
            sai_object_id_t id;
            string object_name;
            string &map_type_name = fvField(*it), &map_name = fvValue(*it);
            ref_resolve_status status = resolveFieldRefValue(m_qos_maps, map_type_name, qos_to_ref_table_map.at(map_type_name), tuple, id, object_name);

            if (status != ref_resolve_status::success)
            {
                SWSS_LOG_INFO("Port QoS map %s is not yet created", map_name.c_str());
                return task_process_status::task_need_retry;
            }

            update_list[qos_to_attr_map[map_type_name]] = make_pair(map_name, id);
            setObjectReference(m_qos_maps, CFG_PORT_QOS_MAP_TABLE_NAME, key, map_type_name, object_name);
        }

        if (fvField(*it) == pfc_enable_name)
        {
            vector<string> queue_indexes;
            queue_indexes = tokenize(fvValue(*it), list_item_delimiter);
            for(string q_ind : queue_indexes)
            {
                sai_uint8_t q_val = (uint8_t)stoi(q_ind);
                pfc_enable |= (uint8_t)(1 << q_val);
            }
        }
    }

    /* Remove any map that was configured but isn't there any longer. */
    for (auto &mapRef : qos_to_attr_map)
    {
        auto &sai_attribute = mapRef.second;
        if (update_list.find(sai_attribute) == update_list.end())
        {
            string referenced_obj;
            if (!doesObjectExist(m_qos_maps, CFG_PORT_QOS_MAP_TABLE_NAME, key, mapRef.first, referenced_obj))
            {
                continue;
            }
            SWSS_LOG_NOTICE("PORT_QOS_MAP|%s %s was configured but is not any more. Remove it", key.c_str(), mapRef.first.c_str());
            removeMeFromObjsReferencedByMe(m_qos_maps, CFG_PORT_QOS_MAP_TABLE_NAME, key, mapRef.first, referenced_obj);
            update_list[mapRef.second] = make_pair("NULL", SAI_NULL_OBJECT_ID);
        }
    }

    for (string port_name : port_names)
    {
        Port port;

        /* Skip port which is not found */
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Failed to apply QoS maps to port %s. Port is not found.", port_name.c_str());
            continue;
        }

        /* Apply a list of attributes to be applied */
        for (auto it = update_list.begin(); it != update_list.end(); it++)
        {
            sai_attribute_t attr;
            attr.id = it->first;
            attr.value.oid = it->second.second;

            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to apply %s to port %s, rv:%d",
                               it->second.first.c_str(), port_name.c_str(), status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    return task_process_status::task_invalid_entry;
                }
            }
            SWSS_LOG_INFO("Applied %s to port %s", it->second.first.c_str(), port_name.c_str());
        }

        sai_uint8_t old_pfc_enable = 0;
        if (!gPortsOrch->getPortPfc(port.m_port_id, &old_pfc_enable))
        {
            SWSS_LOG_ERROR("Failed to retrieve PFC bits on port %s", port_name.c_str());
        }

        if (pfc_enable || old_pfc_enable)
        {
            if (!gPortsOrch->setPortPfc(port.m_port_id, pfc_enable))
            {
                SWSS_LOG_ERROR("Failed to apply PFC bits 0x%x to port %s", pfc_enable, port_name.c_str());
            }

            SWSS_LOG_INFO("Applied PFC bits 0x%x to port %s", pfc_enable, port_name.c_str());
        }
    }

    SWSS_LOG_NOTICE("Applied QoS maps to ports");
    return task_process_status::task_success;
}

void QosOrch::doTask()
{
    SWSS_LOG_ENTER();

    auto *port_qos_map_cfg_exec = getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME);

    for (const auto &it : m_consumerMap)
    {
        auto *exec = it.second.get();

        if (exec == port_qos_map_cfg_exec)
        {
            continue;
        }

        exec->drain();
    }

    port_qos_map_cfg_exec->drain();
}

void QosOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        /* Make sure the handler is initialized for the task */
        auto qos_map_type_name = consumer.getTableName();
        if (m_qos_handler_map.find(qos_map_type_name) == m_qos_handler_map.end())
        {
            SWSS_LOG_ERROR("Task %s handler is not initialized", qos_map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        auto task_status = (this->*(m_qos_handler_map[qos_map_type_name]))(consumer);
        switch(task_status)
        {
            case task_process_status::task_success :
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_invalid_entry :
                SWSS_LOG_ERROR("Failed to process invalid QOS task");
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_failed :
                SWSS_LOG_ERROR("Failed to process QOS task, drop it");
                it = consumer.m_toSync.erase(it);
                return;
            case task_process_status::task_need_retry :
                SWSS_LOG_INFO("Failed to process QOS task, retry it");
                it++;
                break;
            default:
                SWSS_LOG_ERROR("Invalid task status %d", task_status);
                it = consumer.m_toSync.erase(it);
                break;
        }
    }
}
