#pragma once

#include "acltable.h"
#include "orch.h"
#include "timer.h"
#include "flex_counter/flex_counter_manager.h"
#include "switch/switch_capabilities.h"
#include "switch/switch_helper.h"
#include "switch/trimming/capabilities.h"
#include "switch/trimming/helper.h"

#define DEFAULT_ASIC_SENSORS_POLLER_INTERVAL 60
#define ASIC_SENSORS_POLLER_STATUS "ASIC_SENSORS_POLLER_STATUS"
#define ASIC_SENSORS_POLLER_INTERVAL "ASIC_SENSORS_POLLER_INTERVAL"

#define SWITCH_CAPABILITY_TABLE_PORT_TPID_CAPABLE                      "PORT_TPID_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_LAG_TPID_CAPABLE                       "LAG_TPID_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_ORDERED_ECMP_CAPABLE                   "ORDERED_ECMP_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_PFC_DLR_INIT_CAPABLE                   "PFC_DLR_INIT_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_PORT_EGRESS_SAMPLE_CAPABLE             "PORT_EGRESS_SAMPLE_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_PATH_TRACING_CAPABLE                   "PATH_TRACING_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_ICMP_OFFLOAD_CAPABLE                   "ICMP_OFFLOAD_CAPABLE"

#define ASIC_SDK_HEALTH_EVENT_ELIMINATE_INTERVAL 3600
#define SWITCH_CAPABILITY_TABLE_ASIC_SDK_HEALTH_EVENT_CAPABLE          "ASIC_SDK_HEALTH_EVENT"
#define SWITCH_CAPABILITY_TABLE_REG_FATAL_ASIC_SDK_HEALTH_CATEGORY     "REG_FATAL_ASIC_SDK_HEALTH_CATEGORY"
#define SWITCH_CAPABILITY_TABLE_REG_WARNING_ASIC_SDK_HEALTH_CATEGORY   "REG_WARNING_ASIC_SDK_HEALTH_CATEGORY"
#define SWITCH_CAPABILITY_TABLE_REG_NOTICE_ASIC_SDK_HEALTH_CATEGORY    "REG_NOTICE_ASIC_SDK_HEALTH_CATEGORY"
#define SWITCH_CAPABILITY_TABLE_PORT_INGRESS_MIRROR_CAPABLE            "PORT_INGRESS_MIRROR_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_PORT_EGRESS_MIRROR_CAPABLE             "PORT_EGRESS_MIRROR_CAPABLE"

#define SWITCH_STAT_COUNTER_FLEX_COUNTER_GROUP "SWITCH_STAT_COUNTER"

struct WarmRestartCheck
{
    bool    checkRestartReadyState;
    bool    noFreeze;
    bool    skipPendingTaskCheck;
};

class SwitchOrch : public Orch
{
public:
    SwitchOrch(swss::DBConnector *db, std::vector<TableConnector>& connectors, TableConnector switchTable);
    bool checkRestartReady() { return m_warmRestartCheck.checkRestartReadyState; }
    bool checkRestartNoFreeze() { return m_warmRestartCheck.noFreeze; }
    bool skipPendingTaskCheck() { return m_warmRestartCheck.skipPendingTaskCheck; }
    void checkRestartReadyDone() { m_warmRestartCheck.checkRestartReadyState = false; }
    void restartCheckReply(const std::string &op, const std::string &data, std::vector<swss::FieldValueTuple> &values);
    bool setAgingFDB(uint32_t sec);
    void set_switch_capability(const std::vector<swss::FieldValueTuple>& values);
    void get_switch_capability(const std::string& capability, std::string& val);
    bool querySwitchCapability(sai_object_type_t sai_object, sai_attr_id_t attr_id);
    bool checkPfcDlrInitEnable() { return m_PfcDlrInitEnable; }
    void set_switch_pfc_dlr_init_capability();

    // Return reference to ACL group created for each stage and the bind point is
    // the switch
    std::map<sai_acl_stage_t, referenced_object> &getAclGroupsBindingToSwitch();
    // Initialize the ACL groups bind to Switch
    void initAclGroupsBindToSwitch();

    bool checkOrderedEcmpEnable() { return m_orderedEcmpEnable; }

    void onSwitchAsicSdkHealthEvent(sai_object_id_t switch_id,
                                    sai_switch_asic_sdk_health_severity_t severity,
                                    sai_timespec_t timestamp,
                                    sai_switch_asic_sdk_health_category_t category,
                                    sai_switch_health_data_t data,
                                    const sai_u8_list_t &description);

    inline bool isFatalEventReceived() const
        {
            return (m_fatalEventCount != 0);
        }

    bool bindAclTableToSwitch(acl_stage_type_t stage, sai_object_id_t table_id);
    bool unbindAclTableFromSwitch(acl_stage_type_t stage, sai_object_id_t table_id);

    // Statistics
    void generateSwitchCounterIdList();

    // Mirror capability interface for MirrorOrch
    bool isPortIngressMirrorSupported() const { return m_portIngressMirrorSupported; }
    bool isPortEgressMirrorSupported() const { return m_portEgressMirrorSupported; }

private:
    void doTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
    void doCfgSwitchHashTableTask(Consumer &consumer);
    void doCfgSwitchTrimmingTableTask(Consumer &consumer);
    void doCfgSensorsTableTask(Consumer &consumer);
    void doCfgSuppressAsicSdkHealthEventTableTask(Consumer &consumer);
    void doAppSwitchTableTask(Consumer &consumer);
    void initSensorsTable();
    void querySwitchTpidCapability();
    void querySwitchPortEgressSampleCapability();
    void querySwitchPortMirrorCapability();

    // Statistics
    void generateSwitchCounterNameMap() const;

    // Switch hash
    sai_status_t setSwitchHashFieldListSai(const sai_object_id_t oid, const std::set<sai_native_hash_field_t> &hfSet) const;
    sai_status_t createHashObjectSai(sai_object_id_t &oid, const std::set<sai_native_hash_field_t> &hfSet) const;
    sai_status_t setSwitchHashAttributeSai(sai_attr_id_t attrType, sai_object_id_t oid) const;
    bool setSwitchHashFieldList(const SwitchHash &hash, bool isEcmpHash);
    bool setSwitchHashAlgorithmSai(const SwitchHash &hash, bool isEcmpHash) const;
    bool setSwitchHash(const SwitchHash &hash);

    bool getSwitchHashOidSai(sai_object_id_t &oid, sai_attr_id_t attr_id) const;
    void querySwitchHashDefaults();
    void setSwitchIcmpOffloadCapability();

    // Switch trimming
    bool setSwitchTrimmingSizeSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimmingDscpModeSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimmingDscpSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimmingTcSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimmingQueueModeSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimmingQueueIndexSai(const SwitchTrimming &trim) const;
    bool setSwitchTrimming(const SwitchTrimming &trim);

    sai_status_t setSwitchTunnelVxlanParams(swss::FieldValueTuple &val);
    void setSwitchNonSaiAttributes(swss::FieldValueTuple &val);


    // Create the default ACL group for the given stage, bind point is
    // SAI_ACL_BIND_POINT_TYPE_SWITCH and group type is
    // SAI_ACL_TABLE_GROUP_TYPE_PARALLEL.
    ReturnCode createAclGroup(const sai_acl_stage_t &group_stage, referenced_object *acl_grp);

    // Bind the ACL group to switch for the given stage.
    // Set the SAI_SWITCH_ATTR_{STAGE}_ACL with the group oid.
    ReturnCode bindAclGroupToSwitch(const sai_acl_stage_t &group_stage, const referenced_object &acl_grp);

    swss::NotificationConsumer* m_restartCheckNotificationConsumer;
    void doTask(swss::NotificationConsumer& consumer);
    void doAsicSdkHealthEventNotificationConsumerTask(swss::NotificationConsumer& consumer);
    void doRestartCheckNotificationConsumerTask(swss::NotificationConsumer& consumer);
    swss::DBConnector *m_db;
    swss::Table m_switchTable;
    std::map<sai_acl_stage_t, referenced_object> m_aclGroups;
    sai_object_id_t m_switchTunnelId;

    // ASIC temperature sensors
    std::shared_ptr<swss::DBConnector> m_stateDb = nullptr;
    std::shared_ptr<swss::Table> m_asicSensorsTable= nullptr;
    swss::SelectableTimer* m_sensorsPollerTimer = nullptr;
    bool m_sensorsPollerEnabled = false;
    time_t m_sensorsPollerInterval = DEFAULT_ASIC_SENSORS_POLLER_INTERVAL;
    bool m_sensorsPollerIntervalChanged = false;
    uint8_t m_numTempSensors = 0;
    bool m_numTempSensorsInitialized = false;
    bool m_sensorsMaxTempSupported = true;
    bool m_sensorsAvgTempSupported = true;
    bool m_vxlanSportUserModeEnabled = false;
    bool m_orderedEcmpEnable = false;
    bool m_PfcDlrInitEnable = false;

    // Port mirror capabilities
    bool m_portIngressMirrorSupported = false;
    bool m_portEgressMirrorSupported = false;

    // ASIC SDK health event
    std::shared_ptr<swss::DBConnector> m_stateDbForNotification = nullptr;
    std::shared_ptr<swss::Table> m_asicSdkHealthEventTable = nullptr;
    std::set<sai_switch_attr_t> m_supportedAsicSdkHealthEventAttributes;
    std::string m_eliminateEventsSha;
    swss::SelectableTimer* m_eliminateEventsTimer = nullptr;
    uint32_t m_fatalEventCount = 0;

    void initAsicSdkHealthEventNotification();
    void registerAsicSdkHealthEventCategories(sai_switch_attr_t saiSeverity, const std::string &severityString, const std::string &suppressed_category_list="", bool isInitializing=false);

    // Switch hash SAI defaults
    struct {
        struct {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_object_id_t v4Oid = SAI_NULL_OBJECT_ID;
            sai_object_id_t v6Oid = SAI_NULL_OBJECT_ID;
            bool platformSupportsOnlyV4V6 = false; // platform supports only V4/V6 attr type
        } ecmpHash;
        struct {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_object_id_t v4Oid = SAI_NULL_OBJECT_ID;
            sai_object_id_t v6Oid = SAI_NULL_OBJECT_ID;
            bool platformSupportsOnlyV4V6 = false; // platform supports only V4/V6 attr type
        } lagHash;

    } m_switchHashDefaults;

    // Statistics
    FlexCounterManager m_counterManager;
    bool m_isSwitchCounterIdListGenerated = false;

    // Information contained in the request from
    // external program for orchagent pre-shutdown state check
    WarmRestartCheck m_warmRestartCheck = {false, false, false};

    // Switch OA capabilities
    SwitchCapabilities swCap;
    SwitchTrimmingCapabilities trimCap;

    // Switch OA helper
    SwitchHelper swHlpr;
    SwitchTrimmingHelper trimHlpr;
};
