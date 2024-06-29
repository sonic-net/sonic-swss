#include <map>
#include <set>
#include <inttypes.h>
#include <iomanip>

#include "switchorch.h"
#include "crmorch.h"
#include "converter.h"
#include "notifier.h"
#include "notificationproducer.h"
#include "macaddress.h"
#include "return_code.h"
#include "saihelper.h"
#include "sai_serialize.h"
#include "notifications.h"
#include "redisapi.h"

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_acl_api_t *sai_acl_api;
extern sai_hash_api_t *sai_hash_api;
extern MacAddress gVxlanMacAddress;
extern CrmOrch *gCrmOrch;
extern event_handle_t g_events_handle;
extern string gMyAsicName;

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
    {"vxlan_router_mac",                    SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC},
    {"ecmp_hash_offset",                    SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_OFFSET},
    {"lag_hash_offset",                     SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_OFFSET}
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

const map<string, sai_switch_attr_t> switch_asic_sdk_health_event_severity_to_switch_attribute_map =
{
    {"fatal", SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY},
    {"warning", SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY},
    {"notice", SAI_SWITCH_ATTR_REG_NOTICE_SWITCH_ASIC_SDK_HEALTH_CATEGORY}
};

const map<sai_switch_asic_sdk_health_severity_t, string> switch_asic_sdk_health_event_severity_reverse_map =
{
    {SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_FATAL, "fatal"},
    {SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_WARNING, "warning"},
    {SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_NOTICE, "notice"},
};

const map<sai_switch_asic_sdk_health_category_t, string> switch_asic_sdk_health_event_category_reverse_map =
{
    {SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_SW, "software"},
    {SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW, "firmware"},
    {SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_CPU_HW, "cpu_hw"},
    {SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_ASIC_HW, "asic_hw"}
};

const map<string, sai_switch_asic_sdk_health_category_t> switch_asic_sdk_health_event_category_map =
{
    {"software", SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_SW},
    {"firmware", SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW},
    {"cpu_hw", SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_CPU_HW},
    {"asic_hw", SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_ASIC_HW}
};

const std::set<sai_switch_asic_sdk_health_category_t> switch_asic_sdk_health_event_category_universal_set =
{
    SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_SW,
    SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_FW,
    SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_CPU_HW,
    SAI_SWITCH_ASIC_SDK_HEALTH_CATEGORY_ASIC_HW
};

const std::set<std::string> switch_non_sai_attribute_set = {"ordered_ecmp"};

void SwitchOrch::set_switch_pfc_dlr_init_capability()
{
    vector<FieldValueTuple> fvVector;

    /* Query PFC DLR INIT capability */
    bool rv = querySwitchCapability(SAI_OBJECT_TYPE_QUEUE, SAI_QUEUE_ATTR_PFC_DLR_INIT);
    if (rv == false)
    {
        SWSS_LOG_INFO("Queue level PFC DLR INIT configuration is not supported");
        m_PfcDlrInitEnable = false;
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PFC_DLR_INIT_CAPABLE, "false");
    }
    else 
    {
        SWSS_LOG_INFO("Queue level PFC DLR INIT configuration is supported");
        m_PfcDlrInitEnable = true;
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PFC_DLR_INIT_CAPABLE, "true");
    }
    set_switch_capability(fvVector);
}

SwitchOrch::SwitchOrch(DBConnector *db, vector<TableConnector>& connectors, TableConnector switchTable):
        Orch(connectors),
        m_switchTable(switchTable.first, switchTable.second),
        m_db(db),
        m_stateDb(new DBConnector(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_asicSensorsTable(new Table(m_stateDb.get(), ASIC_TEMPERATURE_INFO_TABLE_NAME)),
        m_sensorsPollerTimer (new SelectableTimer((timespec { .tv_sec = DEFAULT_ASIC_SENSORS_POLLER_INTERVAL, .tv_nsec = 0 }))),
        m_stateDbForNotification(new DBConnector(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_asicSdkHealthEventTable(new Table(m_stateDbForNotification.get(), STATE_ASIC_SDK_HEALTH_EVENT_TABLE_NAME))
{
    m_restartCheckNotificationConsumer = new NotificationConsumer(db, "RESTARTCHECK");
    auto restartCheckNotifier = new Notifier(m_restartCheckNotificationConsumer, this, "RESTARTCHECK");
    Orch::addExecutor(restartCheckNotifier);

    initAsicSdkHealthEventNotification();
    set_switch_pfc_dlr_init_capability();
    initSensorsTable();
    querySwitchTpidCapability();
    querySwitchPortEgressSampleCapability();
    querySwitchHashDefaults();

    auto executorT = new ExecutableTimer(m_sensorsPollerTimer, this, "ASIC_SENSORS_POLL_TIMER");
    Orch::addExecutor(executorT);
}

void SwitchOrch::initAsicSdkHealthEventNotification()
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<FieldValueTuple> fvVector;
    vector<tuple<sai_switch_attr_t, const string, const string>> reg_severities = {
        {SAI_SWITCH_ATTR_REG_FATAL_SWITCH_ASIC_SDK_HEALTH_CATEGORY, SWITCH_CAPABILITY_TABLE_REG_FATAL_ASIC_SDK_HEALTH_CATEGORY, "fatal"},
        {SAI_SWITCH_ATTR_REG_WARNING_SWITCH_ASIC_SDK_HEALTH_CATEGORY, SWITCH_CAPABILITY_TABLE_REG_WARNING_ASIC_SDK_HEALTH_CATEGORY, "warning"},
        {SAI_SWITCH_ATTR_REG_NOTICE_SWITCH_ASIC_SDK_HEALTH_CATEGORY, SWITCH_CAPABILITY_TABLE_REG_NOTICE_ASIC_SDK_HEALTH_CATEGORY, "notice"}
    };

    bool supported = querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_SWITCH_ASIC_SDK_HEALTH_EVENT_NOTIFY);
    if (supported)
    {
        attr.id = SAI_SWITCH_ATTR_SWITCH_ASIC_SDK_HEALTH_EVENT_NOTIFY;
        attr.value.ptr = (void *)on_switch_asic_sdk_health_event;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to register ASIC/SDK health event handler: %s", sai_serialize_status(status).c_str());
            supported = false;
        }
        else
        {
            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_ASIC_SDK_HEALTH_EVENT_CAPABLE, "true");
        }
    }
    else
    {
        SWSS_LOG_NOTICE("ASIC/SDK health event is not supported");
    }

    DBConnector cfgDb("CONFIG_DB", 0);
    Table cfgSuppressASHETable(&cfgDb, CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);
    string suppressedCategories;
    bool atLeastOneSupported = false;

    if (!supported)
    {
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_ASIC_SDK_HEALTH_EVENT_CAPABLE, "false");
        for (auto c : reg_severities)
        {
            fvVector.emplace_back(get<1>(c), "false");
        }
        set_switch_capability(fvVector);

        return;
    }

    for (auto c : reg_severities)
    {
        supported = querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, get<0>(c));
        if (supported)
        {
            cfgSuppressASHETable.hget(get<2>(c), "categories", suppressedCategories);
            registerAsicSdkHealthEventCategories(get<0>(c), get<2>(c), suppressedCategories, true);
            suppressedCategories.clear();

            m_supportedAsicSdkHealthEventAttributes.insert(get<0>(c));
            fvVector.emplace_back(get<1>(c), "true");
        }
        else
        {
            SWSS_LOG_NOTICE("Unsupport to register ASIC/SDK health categories for severity %s", get<2>(c).c_str());
            fvVector.emplace_back(get<1>(c), "false");
        }
        atLeastOneSupported = atLeastOneSupported || supported;
    }

    set_switch_capability(fvVector);

    if (atLeastOneSupported)
    {
        try
        {
            // Load the Lua script to eliminate oldest entries
            string eliminateEventsLuaScript = swss::loadLuaScript("eliminate_events.lua");
            m_eliminateEventsSha = swss::loadRedisScript(m_stateDb.get(), eliminateEventsLuaScript);

            // Init timer
            auto interv = timespec { .tv_sec = ASIC_SDK_HEALTH_EVENT_ELIMINATE_INTERVAL, .tv_nsec = 0 };
            m_eliminateEventsTimer = new SelectableTimer(interv);
            auto executor = new ExecutableTimer(m_eliminateEventsTimer, this, "ASIC_SDK_HEALTH_EVENT_ELIMINATE_TIMER");
            Orch::addExecutor(executor);
            m_eliminateEventsTimer->start();
        }
        catch (...)
        {
            // This can happen only on mock test. If it happens on a real switch, we should log an error message
            SWSS_LOG_ERROR("Unable to load the Lua script to eliminate events\n");
        }
    }
}

void SwitchOrch::initAclGroupsBindToSwitch()
{
    // Create an ACL group per stage, INGRESS, EGRESS and PRE_INGRESS
    for (auto stage_it : aclStageLookup)
    {
        auto status = createAclGroup(fvValue(stage_it), &m_aclGroups[fvValue(stage_it)]);
        if (!status.ok())
        {
            status.prepend("Failed to create ACL group for stage " + fvField(stage_it) + ": ");
            SWSS_LOG_THROW("%s", status.message().c_str());
        }
        SWSS_LOG_NOTICE("Created ACL group for stage %s", fvField(stage_it).c_str());
        status = bindAclGroupToSwitch(fvValue(stage_it), m_aclGroups[fvValue(stage_it)]);
        if (!status.ok())
        {
            status.prepend("Failed to bind ACL group to stage " + fvField(stage_it) + ": ");
            SWSS_LOG_THROW("%s", status.message().c_str());
        }
    }
}

std::map<sai_acl_stage_t, referenced_object> &SwitchOrch::getAclGroupsBindingToSwitch()
{
    return m_aclGroups;
}

ReturnCode SwitchOrch::createAclGroup(const sai_acl_stage_t &group_stage, referenced_object *acl_grp)
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

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_acl_api->create_acl_table_group(&acl_grp->m_saiObjectId, gSwitchId,
                                                                       (uint32_t)acl_grp_attrs.size(),
                                                                       acl_grp_attrs.data()),
                                   "Failed to create ACL group for stage " << group_stage);
    if (group_stage == SAI_ACL_STAGE_INGRESS || group_stage == SAI_ACL_STAGE_PRE_INGRESS ||
        group_stage == SAI_ACL_STAGE_EGRESS)
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, (sai_acl_stage_t)group_stage,
                                       SAI_ACL_BIND_POINT_TYPE_SWITCH);
    }
    SWSS_LOG_INFO("Suceeded to create ACL group %s in stage %d ",
                  sai_serialize_object_id(acl_grp->m_saiObjectId).c_str(), group_stage);
    return ReturnCode();
}

ReturnCode SwitchOrch::bindAclGroupToSwitch(const sai_acl_stage_t &group_stage, const referenced_object &acl_grp)
{
    SWSS_LOG_ENTER();

    auto switch_attr_it = aclStageToSwitchAttrLookup.find(group_stage);
    if (switch_attr_it == aclStageToSwitchAttrLookup.end())
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Failed to set ACL group(" << acl_grp.m_saiObjectId
                             << ") to the SWITCH bind point at stage " << group_stage);
    }
    sai_attribute_t attr;
    attr.id = switch_attr_it->second;
    attr.value.oid = acl_grp.m_saiObjectId;
    auto sai_status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(sai_status) << "[SAI] Failed to set_switch_attribute with attribute.id="
                                                    << attr.id << " and acl group oid=" << acl_grp.m_saiObjectId);
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
                bool ret = false;
                bool unsupported_attr = false;
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

                    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_OFFSET:
                        ret = querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_OFFSET);
                        if (ret == false)
                        {
                            unsupported_attr = true;
                        }
                        else
                        {
                            attr.value.u8 = to_uint<uint8_t>(value);
                        }
                        break;
                    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_OFFSET:
                        ret = querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_OFFSET);
                        if (ret == false)
                        {
                            unsupported_attr = true;
                        }
                        else
                        {
                            attr.value.u8 = to_uint<uint8_t>(value);
                        }
                        break;

                    default:
                        invalid_attr = true;
                        break;
                }
                if (invalid_attr || unsupported_attr)
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

bool SwitchOrch::setSwitchHashFieldListSai(const SwitchHash &hash, bool isEcmpHash) const
{
    const auto &oid = isEcmpHash ? m_switchHashDefaults.ecmpHash.oid : m_switchHashDefaults.lagHash.oid;
    const auto &hfSet = isEcmpHash ? hash.ecmp_hash.value : hash.lag_hash.value;

    std::vector<sai_int32_t> hfList;
    std::transform(
        hfSet.cbegin(), hfSet.cend(), std::back_inserter(hfList),
        [](sai_native_hash_field_t value) { return static_cast<sai_int32_t>(value); }
    );

    sai_attribute_t attr;

    attr.id = SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST;
    attr.value.s32list.list = hfList.data();
    attr.value.s32list.count = static_cast<sai_uint32_t>(hfList.size());

    auto status = sai_hash_api->set_hash_attribute(oid, &attr);
    return status == SAI_STATUS_SUCCESS;
}

bool SwitchOrch::setSwitchHashAlgorithmSai(const SwitchHash &hash, bool isEcmpHash) const
{
    sai_attribute_t attr;

    attr.id = isEcmpHash ? SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM : SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM;
    attr.value.s32 = static_cast<sai_int32_t>(isEcmpHash ? hash.ecmp_hash_algorithm.value : hash.lag_hash_algorithm.value);

    auto status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    return status == SAI_STATUS_SUCCESS;
}

bool SwitchOrch::setSwitchHash(const SwitchHash &hash)
{
    SWSS_LOG_ENTER();

    auto hObj = swHlpr.getSwHash();
    auto cfgUpd = false;

    if (hash.ecmp_hash.is_set)
    {
        if (hObj.ecmp_hash.value != hash.ecmp_hash.value)
        {
            if (swCap.isSwitchEcmpHashSupported())
            {
                if (!swCap.validateSwitchHashFieldCap(hash.ecmp_hash.value))
                {
                    SWSS_LOG_ERROR("Failed to validate switch ECMP hash: capability is not supported");
                    return false;
                }

                if (!setSwitchHashFieldListSai(hash, true))
                {
                    SWSS_LOG_ERROR("Failed to set switch ECMP hash in SAI");
                    return false;
                }

                cfgUpd = true;
            }
            else
            {
                SWSS_LOG_WARN("Switch ECMP hash configuration is not supported: skipping ...");
            }
        }
    }
    else
    {
        if (hObj.ecmp_hash.is_set)
        {
            SWSS_LOG_ERROR("Failed to remove switch ECMP hash configuration: operation is not supported");
            return false;
        }
    }

    if (hash.lag_hash.is_set)
    {
        if (hObj.lag_hash.value != hash.lag_hash.value)
        {
            if (swCap.isSwitchLagHashSupported())
            {
                if (!swCap.validateSwitchHashFieldCap(hash.lag_hash.value))
                {
                    SWSS_LOG_ERROR("Failed to validate switch LAG hash: capability is not supported");
                    return false;
                }

                if (!setSwitchHashFieldListSai(hash, false))
                {
                    SWSS_LOG_ERROR("Failed to set switch LAG hash in SAI");
                    return false;
                }

                cfgUpd = true;
            }
            else
            {
                SWSS_LOG_WARN("Switch LAG hash configuration is not supported: skipping ...");
            }
        }
    }
    else
    {
        if (hObj.lag_hash.is_set)
        {
            SWSS_LOG_ERROR("Failed to remove switch LAG hash configuration: operation is not supported");
            return false;
        }
    }

    if (hash.ecmp_hash_algorithm.is_set)
    {
        if (!hObj.ecmp_hash_algorithm.is_set || (hObj.ecmp_hash_algorithm.value != hash.ecmp_hash_algorithm.value))
        {
            if (swCap.isSwitchEcmpHashAlgorithmSupported())
            {
                if (!swCap.validateSwitchEcmpHashAlgorithmCap(hash.ecmp_hash_algorithm.value))
                {
                    SWSS_LOG_ERROR("Failed to validate switch ECMP hash algorithm: capability is not supported");
                    return false;
                }

                if (!setSwitchHashAlgorithmSai(hash, true))
                {
                    SWSS_LOG_ERROR("Failed to set switch ECMP hash algorithm in SAI");
                    return false;
                }

                cfgUpd = true;
            }
            else
            {
                SWSS_LOG_WARN("Switch ECMP hash algorithm configuration is not supported: skipping ...");
            }
        }
    }
    else
    {
        if (hObj.ecmp_hash_algorithm.is_set)
        {
            SWSS_LOG_ERROR("Failed to remove switch ECMP hash algorithm configuration: operation is not supported");
            return false;
        }
    }

    if (hash.lag_hash_algorithm.is_set)
    {
        if (!hObj.lag_hash_algorithm.is_set || (hObj.lag_hash_algorithm.value != hash.lag_hash_algorithm.value))
        {
            if (swCap.isSwitchLagHashAlgorithmSupported())
            {
                if (!swCap.validateSwitchLagHashAlgorithmCap(hash.lag_hash_algorithm.value))
                {
                    SWSS_LOG_ERROR("Failed to validate switch LAG hash algorithm: capability is not supported");
                    return false;
                }

                if (!setSwitchHashAlgorithmSai(hash, false))
                {
                    SWSS_LOG_ERROR("Failed to set switch LAG hash algorithm in SAI");
                    return false;
                }

                cfgUpd = true;
            }
            else
            {
                SWSS_LOG_WARN("Switch LAG hash algorithm configuration is not supported: skipping ...");
            }
        }
    }
    else
    {
        if (hObj.lag_hash_algorithm.is_set)
        {
            SWSS_LOG_ERROR("Failed to remove switch LAG hash algorithm configuration: operation is not supported");
            return false;
        }
    }

    // Don't update internal cache when config remains unchanged
    if (!cfgUpd)
    {
        SWSS_LOG_NOTICE("Switch hash in SAI is up-to-date");
        return true;
    }

    swHlpr.setSwHash(hash);

    SWSS_LOG_NOTICE("Set switch hash in SAI");

    return true;
}

void SwitchOrch::doCfgSwitchHashTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto &map = consumer.m_toSync;
    auto it = map.begin();

    while (it != map.end())
    {
        auto keyOpFieldsValues = it->second;
        auto key = kfvKey(keyOpFieldsValues);
        auto op = kfvOp(keyOpFieldsValues);

        SWSS_LOG_INFO("KEY: %s, OP: %s", key.c_str(), op.c_str());

        if (key.empty())
        {
            SWSS_LOG_ERROR("Failed to parse switch hash key: empty string");
            it = map.erase(it);
            continue;
        }

        SwitchHash hash;

        if (op == SET_COMMAND)
        {
            for (const auto &cit : kfvFieldsValues(keyOpFieldsValues))
            {
                auto fieldName = fvField(cit);
                auto fieldValue = fvValue(cit);

                SWSS_LOG_INFO("FIELD: %s, VALUE: %s", fieldName.c_str(), fieldValue.c_str());

                hash.fieldValueMap[fieldName] = fieldValue;
            }

            if (swHlpr.parseSwHash(hash))
            {
                if (!setSwitchHash(hash))
                {
                    SWSS_LOG_ERROR("Failed to set switch hash: ASIC and CONFIG DB are diverged");
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_ERROR("Failed to remove switch hash: operation is not supported: ASIC and CONFIG DB are diverged");
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation(%s)", op.c_str());
        }

        it = map.erase(it);
    }
}

void SwitchOrch::registerAsicSdkHealthEventCategories(sai_switch_attr_t saiSeverity, const string &severityString, const string &suppressed_category_list, bool isInitializing)
{
    sai_status_t status;
    set<sai_switch_asic_sdk_health_category_t> interested_categories_set = switch_asic_sdk_health_event_category_universal_set;

    SWSS_LOG_INFO("Register ASIC/SDK health event for severity %s(%d) with categories [%s] suppressed", severityString.c_str(), saiSeverity, suppressed_category_list.c_str());

    if (!suppressed_category_list.empty())
    {
        auto &&categories = tokenize(suppressed_category_list, ',');
        for (auto category : categories)
        {
            try
            {
                interested_categories_set.erase(switch_asic_sdk_health_event_category_map.at(category));
            }
            catch (std::out_of_range &e)
            {
                SWSS_LOG_ERROR("Unknown ASIC/SDK health category %s to suppress", category.c_str());
                continue;
            }
        }
    }

    if (isInitializing && interested_categories_set.empty())
    {
        SWSS_LOG_INFO("All categories are suppressed for severity %s", severityString.c_str());
        return;
    }

    vector<int32_t> sai_categories(interested_categories_set.begin(), interested_categories_set.end());
    sai_attribute_t attr;

    attr.id = saiSeverity;
    attr.value.s32list.count = (uint32_t)sai_categories.size();
    attr.value.s32list.list = sai_categories.data();
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to register ASIC/SDK health event categories for severity %s, status: %s", severityString.c_str(), sai_serialize_status(status).c_str());
    }
}

void SwitchOrch::doCfgSuppressAsicSdkHealthEventTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto &map = consumer.m_toSync;
    auto it = map.begin();

    while (it != map.end())
    {
        auto keyOpFieldsValues = it->second;
        auto key = kfvKey(keyOpFieldsValues);
        auto op = kfvOp(keyOpFieldsValues);

        SWSS_LOG_INFO("KEY: %s, OP: %s", key.c_str(), op.c_str());

        if (key.empty())
        {
            SWSS_LOG_ERROR("Failed to parse switch hash key: empty string");
            it = map.erase(it);
            continue;
        }

        sai_switch_attr_t saiSeverity;
        try
        {
            saiSeverity = switch_asic_sdk_health_event_severity_to_switch_attribute_map.at(key);
        }
        catch (std::out_of_range &e)
        {
            SWSS_LOG_ERROR("Unknown severity %s in SUPPRESS_ASIC_SDK_HEALTH_EVENT table", key.c_str());
            it = map.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            bool categoriesConfigured = false;
            bool continueMainLoop = false;
            for (const auto &cit : kfvFieldsValues(keyOpFieldsValues))
            {
                auto fieldName = fvField(cit);
                auto fieldValue = fvValue(cit);

                SWSS_LOG_INFO("FIELD: %s, VALUE: %s", fieldName.c_str(), fieldValue.c_str());

                if (m_supportedAsicSdkHealthEventAttributes.find(saiSeverity) == m_supportedAsicSdkHealthEventAttributes.end())
                {
                    SWSS_LOG_NOTICE("Unsupport to register categories on severity %d", saiSeverity);
                    it = map.erase(it);
                    continueMainLoop = true;
                    break;
                }

                if (fieldName == "categories")
                {
                    registerAsicSdkHealthEventCategories(saiSeverity, key, fieldValue);
                    categoriesConfigured = true;
                }
            }

            if (continueMainLoop)
            {
                continue;
            }

            if (!categoriesConfigured)
            {
                registerAsicSdkHealthEventCategories(saiSeverity, key);
            }
        }
        else if (op == DEL_COMMAND)
        {
            registerAsicSdkHealthEventCategories(saiSeverity, key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation(%s)", op.c_str());
        }

        it = map.erase(it);
    }
}

void SwitchOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const auto &tableName = consumer.getTableName();

    if (tableName == APP_SWITCH_TABLE_NAME)
    {
        doAppSwitchTableTask(consumer);
    }
    else if (tableName == CFG_ASIC_SENSORS_TABLE_NAME)
    {
        doCfgSensorsTableTask(consumer);
    }
    else if (tableName == CFG_SWITCH_HASH_TABLE_NAME)
    {
        doCfgSwitchHashTableTask(consumer);
    }
    else if (tableName == CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME)
    {
        doCfgSuppressAsicSdkHealthEventTableTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table : %s", tableName.c_str());
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

void SwitchOrch::onSwitchAsicSdkHealthEvent(sai_object_id_t switch_id,
                                            sai_switch_asic_sdk_health_severity_t severity,
                                            sai_timespec_t timestamp,
                                            sai_switch_asic_sdk_health_category_t category,
                                            sai_switch_health_data_t data,
                                            const sai_u8_list_t &description)
{
    std::vector<swss::FieldValueTuple> values;
    const string &severity_str = switch_asic_sdk_health_event_severity_reverse_map.at(severity);
    const string &category_str = switch_asic_sdk_health_event_category_reverse_map.at(category);
    string description_str;
    const std::time_t &t = (std::time_t)timestamp.tv_sec;
    stringstream time_ss;
    time_ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");

    switch (data.data_type)
    {
    case SAI_HEALTH_DATA_TYPE_GENERAL:
    {
        vector<uint8_t> description_with_terminator(description.list, description.list + description.count);
        // Add the terminate character
        description_with_terminator.push_back(0);
        description_str = string(reinterpret_cast<char*>(description_with_terminator.data()));
        // Remove unprintable characters but keep CR and NL
        if (description_str.end() !=
            description_str.erase(std::remove_if(
                                  description_str.begin(),
                                  description_str.end(),
                                  [](unsigned char x) {
                                      return (x != 0x0d) && (x != 0x0a) && !std::isprint(x);
                                  }),
                                  description_str.end()))
        {
            SWSS_LOG_NOTICE("Unprintable characters in description of ASIC/SDK health event");
        }
        break;
    }
    default:
        SWSS_LOG_ERROR("Unknown data type %d when receiving ASIC/SDK health event", data.data_type);
        // Do not return. The ASIC/SDK health event will still be recorded but without the description
        break;
    }

    event_params_t params = {
        { "sai_timestamp", time_ss.str() },
        { "severity", severity_str },
        { "category", category_str },
        { "description", description_str }};

    string asic_name_str;
    if (!gMyAsicName.empty())
    {
        asic_name_str = "asic " + gMyAsicName + ",";
        params["asic_name"] = gMyAsicName;
    }

    if (severity == SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_FATAL)
    {
        SWSS_LOG_ERROR("[%s] ASIC/SDK health event occurred at %s, %scategory %s: %s", severity_str.c_str(), time_ss.str().c_str(), asic_name_str.c_str(), category_str.c_str(), description_str.c_str());
    }
    else
    {
        SWSS_LOG_NOTICE("[%s] ASIC/SDK health event occurred at %s, %scategory %s: %s", severity_str.c_str(), time_ss.str().c_str(), asic_name_str.c_str(), category_str.c_str(), description_str.c_str());
    }

    values.emplace_back("severity", severity_str);
    values.emplace_back("category", category_str);
    values.emplace_back("description", description_str);

    m_asicSdkHealthEventTable->set(time_ss.str(),values);

    event_publish(g_events_handle, "asic-sdk-health-event", &params);

    if (severity == SAI_SWITCH_ASIC_SDK_HEALTH_SEVERITY_FATAL)
    {
        m_fatalEventCount++;
    }
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
    else if (&timer == m_eliminateEventsTimer)
    {
        auto ret = swss::runRedisScript(*m_stateDb, m_eliminateEventsSha, {}, {});
        for (auto str: ret)
        {
            SWSS_LOG_INFO("Eliminate ASIC/SDK health %s", str.c_str());
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

void SwitchOrch::querySwitchPortEgressSampleCapability()
{
    vector<FieldValueTuple> fvVector;
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attr_capability_t capability;

    // Check if SAI is capable of handling Port egress sample.
    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_PORT,
                            SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE, &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Could not query port egress Sample capability %d", status);
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_EGRESS_SAMPLE_CAPABLE, "false");
    }
    else
    {
        if (capability.set_implemented)
        {
            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_EGRESS_SAMPLE_CAPABLE, "true");
        }
        else
        {
            fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PORT_EGRESS_SAMPLE_CAPABLE, "false");
        }
        SWSS_LOG_NOTICE("port egress Sample capability %d", capability.set_implemented);
    }
    set_switch_capability(fvVector);
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

bool SwitchOrch::getSwitchHashOidSai(sai_object_id_t &oid, bool isEcmpHash) const
{
    sai_attribute_t attr;
    attr.id = isEcmpHash ? SAI_SWITCH_ATTR_ECMP_HASH : SAI_SWITCH_ATTR_LAG_HASH;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    auto status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    oid = attr.value.oid;

    return true;
}

void SwitchOrch::querySwitchHashDefaults()
{
    SWSS_LOG_ENTER();

    if (!getSwitchHashOidSai(m_switchHashDefaults.ecmpHash.oid, true))
    {
        SWSS_LOG_WARN("Failed to get switch ECMP hash OID");
    }

    if (!getSwitchHashOidSai(m_switchHashDefaults.lagHash.oid, false))
    {
        SWSS_LOG_WARN("Failed to get switch LAG hash OID");
    }
}

bool SwitchOrch::querySwitchCapability(sai_object_type_t sai_object, sai_attr_id_t attr_id)
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

// Bind ACL table (with bind type switch) to switch
bool SwitchOrch::bindAclTableToSwitch(acl_stage_type_t stage, sai_object_id_t table_id)
{
    sai_attribute_t attr;
    if ( stage == ACL_STAGE_INGRESS ) {
        attr.id = SAI_SWITCH_ATTR_INGRESS_ACL;
    } else {
        attr.id = SAI_SWITCH_ATTR_EGRESS_ACL;
    }
    attr.value.oid = table_id;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    string stage_str = (stage == ACL_STAGE_INGRESS) ? "ingress" : "egress";
    if (status == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Bind %s acl table %" PRIx64" to switch", stage_str.c_str(), table_id);
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to bind %s acl table %" PRIx64" to switch", stage_str.c_str(), table_id);
        return false;
    }
}

// Unbind ACL table from swtich
bool SwitchOrch::unbindAclTableFromSwitch(acl_stage_type_t stage,sai_object_id_t table_id)
{
    sai_attribute_t attr;
    if ( stage == ACL_STAGE_INGRESS ) {
        attr.id = SAI_SWITCH_ATTR_INGRESS_ACL;
    } else {
        attr.id = SAI_SWITCH_ATTR_EGRESS_ACL;
    }
    attr.value.oid = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    string stage_str = (stage == ACL_STAGE_INGRESS) ? "ingress" : "egress";
    if (status == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Unbind %s acl table %" PRIx64" to switch", stage_str.c_str(), table_id);
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to unbind %s acl table %" PRIx64" to switch", stage_str.c_str(), table_id);
        return false;
    }
}
