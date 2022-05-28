#include <map>
#include <set>
#include <inttypes.h>

#include "switchorch.h"
#include "crmorch.h"
#include "converter.h"
#include "notifier.h"
#include "notificationproducer.h"
#include "macaddress.h"
#include "return_code.h"

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_acl_api_t *sai_acl_api;
extern MacAddress gVxlanMacAddress;
extern CrmOrch *gCrmOrch;

const map<string, sai_switch_attr_t> switch_attribute_map =
{
    {"fdb_unicast_miss_packet_action",      SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION},
    {"fdb_broadcast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION},
    {"fdb_multicast_miss_packet_action",    SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION},
    {"ecmp_hash_seed",                      SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED},
    {"lag_hash_seed",                       SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED},
    {"fdb_aging_time",                      SAI_SWITCH_ATTR_FDB_AGING_TIME},
    {"debug_shell_enable",                  SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE},
    {"vxlan_port",                          SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT},
    {"vxlan_router_mac",                    SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC}
};

const map<string, sai_switch_tunnel_attr_t> switch_tunnel_attribute_map =
{
    {"vxlan_sport", SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT},
    {"vxlan_mask",  SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT_MASK}
};

const map<string, sai_packet_action_t> packet_action_map =
{
    {"drop",    SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"trap",    SAI_PACKET_ACTION_TRAP}
};


const std::set<std::string> switch_non_sai_attribute_set = {"ordered_ecmp"};

SwitchOrch::SwitchOrch(DBConnector *db, vector<TableConnector>& connectors, TableConnector switchTable):
        Orch(connectors),
        m_switchTable(switchTable.first, switchTable.second),
        m_db(db),
        m_stateDb(new DBConnector(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_asicSensorsTable(new Table(m_stateDb.get(), ASIC_TEMPERATURE_INFO_TABLE_NAME)),
        m_sensorsPollerTimer (new SelectableTimer((timespec { .tv_sec = DEFAULT_ASIC_SENSORS_POLLER_INTERVAL, .tv_nsec = 0 })))
{
    m_restartCheckNotificationConsumer = new NotificationConsumer(db, "RESTARTCHECK");
    auto restartCheckNotifier = new Notifier(m_restartCheckNotificationConsumer, this, "RESTARTCHECK");
    Orch::addExecutor(restartCheckNotifier);

    initSensorsTable();
    querySwitchTpidCapability();
    auto executorT = new ExecutableTimer(m_sensorsPollerTimer, this, "ASIC_SENSORS_POLL_TIMER");
    Orch::addExecutor(executorT);
}

void SwitchOrch::initAclGroupsBindToSwitch()
{
    // Create an ACL group per stage, INGRESS, EGRESS and PRE_INGRESS
    for (auto stage_it : aclStageLookup)
    {
        sai_object_id_t group_oid;
        auto status = createAclGroup(fvValue(stage_it), &group_oid);
        if (!status.ok())
        {
            status.prepend("Failed to create ACL group for stage " + fvField(stage_it) + ": ");
            SWSS_LOG_THROW("%s", status.message().c_str());
        }
        SWSS_LOG_NOTICE("Created ACL group for stage %s", fvField(stage_it).c_str());
        m_aclGroups[fvValue(stage_it)] = group_oid;
        status = bindAclGroupToSwitch(fvValue(stage_it), group_oid);
        if (!status.ok())
        {
            status.prepend("Failed to bind ACL group to stage " + fvField(stage_it) + ": ");
            SWSS_LOG_THROW("%s", status.message().c_str());
        }
    }
}

const std::map<sai_acl_stage_t, sai_object_id_t> &SwitchOrch::getAclGroupOidsBindingToSwitch()
{
    return m_aclGroups;
}

ReturnCode SwitchOrch::createAclGroup(const sai_acl_stage_t &group_stage, sai_object_id_t *acl_grp_oid)
{
    SWSS_LOG_ENTER();

    std::vector<sai_attribute_t> acl_grp_attrs;
    sai_attribute_t acl_grp_attr;
    acl_grp_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE;
    acl_grp_attr.value.s32 = group_stage;
    acl_grp_attrs.push_back(acl_grp_attr);

    acl_grp_attr.id = SAI_ACL_TABLE_GROUP_ATTR_TYPE;
    acl_grp_attr.value.s32 = SAI_ACL_TABLE_GROUP_TYPE_PARALLEL;
    acl_grp_attrs.push_back(acl_grp_attr);

    acl_grp_attr.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    std::vector<int32_t> bpoint_list;
    bpoint_list.push_back(SAI_ACL_BIND_POINT_TYPE_SWITCH);
    acl_grp_attr.value.s32list.count = (uint32_t)bpoint_list.size();
    acl_grp_attr.value.s32list.list = bpoint_list.data();
    acl_grp_attrs.push_back(acl_grp_attr);

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_acl_api->create_acl_table_group(
                                       acl_grp_oid, gSwitchId, (uint32_t)acl_grp_attrs.size(), acl_grp_attrs.data()),
                                   "Failed to create ACL group for stage " << group_stage);
    if (group_stage == SAI_ACL_STAGE_INGRESS || group_stage == SAI_ACL_STAGE_PRE_INGRESS ||
        group_stage == SAI_ACL_STAGE_EGRESS)
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, (sai_acl_stage_t)group_stage,
                                       SAI_ACL_BIND_POINT_TYPE_SWITCH);
    }
    SWSS_LOG_INFO("Suceeded to create ACL group %s in stage %d ", sai_serialize_object_id(*acl_grp_oid).c_str(),
                  group_stage);
    return ReturnCode();
}

ReturnCode SwitchOrch::bindAclGroupToSwitch(const sai_acl_stage_t &group_stage, const sai_object_id_t &acl_grp_oid)
{
    SWSS_LOG_ENTER();

    auto switch_attr_it = aclStageToSwitchAttrLookup.find(group_stage);
    if (switch_attr_it == aclStageToSwitchAttrLookup.end())
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Failed to set ACL group(" << acl_grp_oid << ") to the SWITCH bind point at stage "
                             << group_stage);
    }
    sai_attribute_t attr;
    attr.id = switch_attr_it->second;
    attr.value.oid = acl_grp_oid;
    auto sai_status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(sai_status) << "[SAI] Failed to set_switch_attribute with attribute.id="
                                                    << attr.id << " and acl group oid=" << acl_grp_oid);
    }
    return ReturnCode();
}

void SwitchOrch::doCfgSensorsTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string table_attr = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            FieldValueTuple fvt = kfvFieldsValues(t)[0];
            SWSS_LOG_NOTICE("ASIC sensors : set %s(%s) to %s", table_attr.c_str(), fvField(fvt).c_str(), fvValue(fvt).c_str());

            if (table_attr == ASIC_SENSORS_POLLER_STATUS)
            {
                if (fvField(fvt) == "admin_status")
                {
                    if (fvValue(fvt) == "enable" && !m_sensorsPollerEnabled)
                    {
                        m_sensorsPollerTimer->start();
                        m_sensorsPollerEnabled = true;
                    }
                    else if (fvValue(fvt) == "disable")
                    {
                        m_sensorsPollerEnabled = false;
                    }
                    else
                    {
                        SWSS_LOG_ERROR("ASIC sensors : unsupported operation for poller state %d",m_sensorsPollerEnabled);
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("ASIC sensors : unsupported field in attribute %s", ASIC_SENSORS_POLLER_STATUS);
                }
            }
            else if (table_attr == ASIC_SENSORS_POLLER_INTERVAL)
            {
                auto interval=to_int<time_t>(fvValue(fvt));

                if (fvField(fvt) == "interval")
                {
                    if (interval != m_sensorsPollerInterval)
                    {
                        auto intervT = timespec { .tv_sec = interval , .tv_nsec = 0 };
                        m_sensorsPollerTimer->setInterval(intervT);
                        m_sensorsPollerInterval = interval;
                        m_sensorsPollerIntervalChanged = true;
                    }
                    else
                    {
                        SWSS_LOG_INFO("ASIC sensors : poller interval unchanged : %s seconds", to_string(m_sensorsPollerInterval).c_str());
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("ASIC sensors : unsupported field in attribute %s", ASIC_SENSORS_POLLER_INTERVAL);
                }
            }
            else
            {
                SWSS_LOG_ERROR("ASIC sensors : unsupported attribute %s", table_attr.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("ASIC sensors : unsupported operation %s",op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void SwitchOrch::setSwitchNonSaiAttributes(swss::FieldValueTuple &val)
{
    auto attribute = fvField(val);
    auto value = fvValue(val);

    if (attribute == "ordered_ecmp")
    {
        vector<FieldValueTuple> fvVector;
        if (value == "true")
        {
            const auto* meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_TYPE);
            if (meta && meta->isenum)
            {
                vector<int32_t> values_list(meta->enummetadata->valuescount);
                sai_s32_list_t values;
                values.count = static_cast<uint32_t>(values_list.size());
                values.list = values_list.data();

                auto status = sai_query_attribute_enum_values_capability(gSwitchId,
                                                                         SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                                                         SAI_NEXT_HOP_GROUP_ATTR_TYPE,
                                                                         &values);
                if (status == SAI_STATUS_SUCCESS)
                {
                    for (size_t i = 0; i < values.count; i++)
                    {
                        if (values.list[i] == SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP)
                        {
                            m_orderedEcmpEnable = true;
                            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_ORDERED_ECMP_CAPABLE, "true");
                            set_switch_capability(fvVector);
                            SWSS_LOG_NOTICE("Ordered ECMP/Nexthop-Group is configured");
                            return;
                        }
                    }
                }
            }
        }
        m_orderedEcmpEnable = false;
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_ORDERED_ECMP_CAPABLE, "false");
        set_switch_capability(fvVector);
        SWSS_LOG_NOTICE("Ordered ECMP/Nexthop-Group is not configured");
        return;
    }
}
sai_status_t SwitchOrch::setSwitchTunnelVxlanParams(swss::FieldValueTuple &val)
{
    auto attribute = fvField(val);
    auto value = fvValue(val);
    sai_attribute_t attr;
    sai_status_t status;

    if (!m_vxlanSportUserModeEnabled)
    {
        // Enable Vxlan src port range feature
        vector<sai_attribute_t> attrs;
        attr.id = SAI_SWITCH_TUNNEL_ATTR_TUNNEL_TYPE;
        attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
        attrs.push_back(attr);
        attr.id = SAI_SWITCH_TUNNEL_ATTR_TUNNEL_VXLAN_UDP_SPORT_MODE;
        attr.value.s32 = SAI_TUNNEL_VXLAN_UDP_SPORT_MODE_USER_DEFINED;
        attrs.push_back(attr);

        status = sai_switch_api->create_switch_tunnel(&m_switchTunnelId, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create switch_tunnel object, rv:%d",  status);
            return status;
        }

        m_vxlanSportUserModeEnabled = true;
    }

    attr.id = switch_tunnel_attribute_map.at(attribute);
    switch (attr.id)
    {
        case SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT:
            attr.value.u16 = to_uint<uint16_t>(value);
            break;
        case SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT_MASK:
            attr.value.u8 = to_uint<uint8_t>(value);
            break;
        default:
            SWSS_LOG_ERROR("Invalid switch tunnel attribute id %d", attr.id);
            return SAI_STATUS_SUCCESS;
    }

    status  = sai_switch_api->set_switch_tunnel_attribute(m_switchTunnelId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set tunnnel switch attribute %s to %s, rv:%d", attribute.c_str(), value.c_str(), status);
        return status;
    }

    SWSS_LOG_NOTICE("Set switch attribute %s to %s", attribute.c_str(), value.c_str());
    return SAI_STATUS_SUCCESS;
}

void SwitchOrch::doAppSwitchTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        auto op = kfvOp(t);
        bool retry = false;

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                auto attribute = fvField(i);

                if (switch_non_sai_attribute_set.find(attribute) != switch_non_sai_attribute_set.end())
                {
                    setSwitchNonSaiAttributes(i);
                    continue;
                }
                else if (switch_attribute_map.find(attribute) == switch_attribute_map.end())
                {
                    // Check additionally 'switch_tunnel_attribute_map' for Switch Tunnel
                    if (switch_tunnel_attribute_map.find(attribute) == switch_tunnel_attribute_map.end())
                    {
                        SWSS_LOG_ERROR("Unsupported switch attribute %s", attribute.c_str());
                        break;
                    }

                    auto status = setSwitchTunnelVxlanParams(i);
                    if ((status != SAI_STATUS_SUCCESS) && (handleSaiSetStatus(SAI_API_SWITCH, status) == task_need_retry))
                    {
                        retry = true;
                        break;
                    }

                    continue;
                }

                auto value = fvValue(i);

                sai_attribute_t attr;
                attr.id = switch_attribute_map.at(attribute);

                MacAddress mac_addr;
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

                    case SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE:
                        attr.value.booldata = to_uint<bool>(value);
                        break;

                    case SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT:
                        attr.value.u16 = to_uint<uint16_t>(value);
                        break;

                    case SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC:
                        mac_addr = value;
                        gVxlanMacAddress = mac_addr;
                        memcpy(attr.value.mac, mac_addr.getMac(), sizeof(sai_mac_t));
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
                    retry = (handleSaiSetStatus(SAI_API_SWITCH, status) == task_need_retry);
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

void SwitchOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    const string & table_name = consumer.getTableName();

    if (table_name == APP_SWITCH_TABLE_NAME)
    {
        doAppSwitchTableTask(consumer);
    }
    else if (table_name == CFG_ASIC_SENSORS_TABLE_NAME)
    {
        doCfgSensorsTableTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
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

bool SwitchOrch::setAgingFDB(uint32_t sec)
{
    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_FDB_AGING_TIME;
    attr.value.u32 = sec;
    auto status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set switch %" PRIx64 " fdb_aging_time attribute: %d", gSwitchId, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Set switch %" PRIx64 " fdb_aging_time %u sec", gSwitchId, sec);
    return true;
}

void SwitchOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (&timer == m_sensorsPollerTimer)
    {
        if (m_sensorsPollerIntervalChanged)
        {
            m_sensorsPollerTimer->reset();
            m_sensorsPollerIntervalChanged = false;
        }

        if (!m_sensorsPollerEnabled)
        {
            m_sensorsPollerTimer->stop();
            return;
        }

        sai_attribute_t attr;
        sai_status_t status;
        std::vector<FieldValueTuple> values;

        if (m_numTempSensors)
        {
            std::vector<int32_t> temp_list(m_numTempSensors);

            memset(&attr, 0, sizeof(attr));
            attr.id = SAI_SWITCH_ATTR_TEMP_LIST;
            attr.value.s32list.count = m_numTempSensors;
            attr.value.s32list.list = temp_list.data();

            status = sai_switch_api->get_switch_attribute(gSwitchId , 1, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {
                for (size_t i = 0; i < attr.value.s32list.count ; i++) {
                    const std::string &fieldName = "temperature_" + std::to_string(i);
                    values.emplace_back(fieldName, std::to_string(temp_list[i]));
                }
                m_asicSensorsTable->set("",values);
            }
            else
            {
                SWSS_LOG_ERROR("ASIC sensors : failed to get SAI_SWITCH_ATTR_TEMP_LIST: %d", status);
            }
        }

        if (m_sensorsMaxTempSupported)
        {
            memset(&attr, 0, sizeof(attr));
            attr.id = SAI_SWITCH_ATTR_MAX_TEMP;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {
                const std::string &fieldName = "maximum_temperature";
                values.emplace_back(fieldName, std::to_string(attr.value.s32));
                m_asicSensorsTable->set("",values);
            }
            else if (status ==  SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
            {
                m_sensorsMaxTempSupported = false;
                SWSS_LOG_INFO("ASIC sensors : SAI_SWITCH_ATTR_MAX_TEMP is not supported");
            }
            else
            {
                m_sensorsMaxTempSupported = false;
                SWSS_LOG_ERROR("ASIC sensors : failed to get SAI_SWITCH_ATTR_MAX_TEMP: %d", status);
            }
        }

        if (m_sensorsAvgTempSupported)
        {
            memset(&attr, 0, sizeof(attr));
            attr.id = SAI_SWITCH_ATTR_AVERAGE_TEMP;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {
                const std::string &fieldName = "average_temperature";
                values.emplace_back(fieldName, std::to_string(attr.value.s32));
                m_asicSensorsTable->set("",values);
            }
            else if (status ==  SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
            {
                m_sensorsAvgTempSupported = false;
                SWSS_LOG_INFO("ASIC sensors : SAI_SWITCH_ATTR_AVERAGE_TEMP is not supported");
            }
            else
            {
                m_sensorsAvgTempSupported = false;
                SWSS_LOG_ERROR("ASIC sensors : failed to get SAI_SWITCH_ATTR_AVERAGE_TEMP: %d", status);
            }
        }
    }
}

void SwitchOrch::initSensorsTable()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t status;
    std::vector<FieldValueTuple> values;

    if (!m_numTempSensorsInitialized)
    {
        memset(&attr, 0, sizeof(attr));
        attr.id = SAI_SWITCH_ATTR_MAX_NUMBER_OF_TEMP_SENSORS;

        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status == SAI_STATUS_SUCCESS)
        {
            m_numTempSensors = attr.value.u8;
            m_numTempSensorsInitialized = true;
        }
        else if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) || SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status)
                 || status ==  SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            m_numTempSensorsInitialized = true;
            SWSS_LOG_INFO("ASIC sensors : SAI_SWITCH_ATTR_MAX_NUMBER_OF_TEMP_SENSORS is not supported");
        }
        else
        {
            SWSS_LOG_ERROR("ASIC sensors : failed to get SAI_SWITCH_ATTR_MAX_NUMBER_OF_TEMP_SENSORS: 0x%x", status);
        }
    }

    if (m_numTempSensors)
    {
        std::vector<int32_t> temp_list(m_numTempSensors);

        memset(&attr, 0, sizeof(attr));
        attr.id = SAI_SWITCH_ATTR_TEMP_LIST;
        attr.value.s32list.count = m_numTempSensors;
        attr.value.s32list.list = temp_list.data();

        status = sai_switch_api->get_switch_attribute(gSwitchId , 1, &attr);
        if (status == SAI_STATUS_SUCCESS)
        {
            for (size_t i = 0; i < attr.value.s32list.count ; i++) {
                const std::string &fieldName = "temperature_" + std::to_string(i);
                values.emplace_back(fieldName, std::to_string(0));
            }
            m_asicSensorsTable->set("",values);
        }
        else
        {
            SWSS_LOG_ERROR("ASIC sensors : failed to get SAI_SWITCH_ATTR_TEMP_LIST: %d", status);
        }
    }

    if (m_sensorsMaxTempSupported)
    {
        const std::string &fieldName = "maximum_temperature";
        values.emplace_back(fieldName, std::to_string(0));
        m_asicSensorsTable->set("",values);
    }

    if (m_sensorsAvgTempSupported)
    {
        const std::string &fieldName = "average_temperature";
        values.emplace_back(fieldName, std::to_string(0));
        m_asicSensorsTable->set("",values);
    }
}

void SwitchOrch::set_switch_capability(const std::vector<FieldValueTuple>& values)
{
     m_switchTable.set("switch", values);
}

void SwitchOrch::querySwitchTpidCapability()
{
    SWSS_LOG_ENTER();
    // Check if SAI is capable of handling TPID config and store result in StateDB switch capability table
    {
        vector<FieldValueTuple> fvVector;
        sai_status_t status = SAI_STATUS_SUCCESS;
        sai_attr_capability_t capability;

        // Check if SAI is capable of handling TPID for Port
        status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_TPID, &capability);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Could not query port TPID capability %d", status);
            // Since pre-req of TPID support requires querry capability failed, it means TPID not supported
            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_TPID_CAPABLE, "false");
        }
        else
        {
            if (capability.set_implemented)
            {
                fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_TPID_CAPABLE, "true");
            }
            else
            {
                fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_TPID_CAPABLE, "false");
            }
            SWSS_LOG_NOTICE("port TPID capability %d", capability.set_implemented);
        }
        // Check if SAI is capable of handling TPID for LAG
        status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_TPID, &capability);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Could not query LAG TPID capability %d", status);
            // Since pre-req of TPID support requires querry capability failed, it means TPID not supported
            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_LAG_TPID_CAPABLE, "false");
        }
        else
        {
            if (capability.set_implemented)
            {
                fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_LAG_TPID_CAPABLE, "true");
            }
            else
            {
                fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_LAG_TPID_CAPABLE, "false");
            }
            SWSS_LOG_NOTICE("LAG TPID capability %d", capability.set_implemented);
        }
        set_switch_capability(fvVector);
    }
}

bool SwitchOrch::querySwitchDscpToTcCapability(sai_object_type_t sai_object, sai_attr_id_t attr_id)
{
    SWSS_LOG_ENTER();

    /* Check if SAI is capable of handling Switch level DSCP to TC QoS map */
    vector<FieldValueTuple> fvVector;
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attr_capability_t capability;

    status = sai_query_attribute_capability(gSwitchId, sai_object, attr_id, &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Could not query switch level DSCP to TC map %d", status);
        return false;
    }
    else 
    {
        if (capability.set_implemented)
        {
            return true;
        }
        else 
        {
            return false;
        }
    }
}
