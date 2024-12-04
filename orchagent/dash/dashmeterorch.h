#pragma once

#include <boost/optional.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <deque>
#include <functional>

#include <saitypes.h>
#include <sai.h>
#include <logger.h>
#include <dbconnector.h>
#include <bulker.h>
#include <orch.h>
#include "zmqorch.h"
#include "zmqserver.h"

#include "dashorch.h"
#include "dash_api/meter_policy.pb.h"
#include "dash_api/meter_rule.pb.h"

///// To be deleted after sonic-swss-common/common/schema.h fix
#if 0 //B4C-->0
#define APP_DASH_METER_POLICY_TABLE_NAME    "DASH_METER_POLICY_TABLE"
#define APP_DASH_METER_RULE_TABLE_NAME      "DASH_METER_RULE_TABLE"
#define DASH_METER_COUNTER_ID_LIST "DASH_METER_COUNTER_ID_LIST"
#endif
///// End..To be deleted after sonic-swss-common/common/schema.h fix

#define METER_STAT_COUNTER_FLEX_COUNTER_GROUP "METER_STAT_COUNTER"
#define METER_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

struct MeterPolicyContext
{
    std::string meter_policy;
    dash::meter_policy::MeterPolicy metadata;
};
struct MeterPolicyEntry
{
    sai_object_id_t meter_policy_oid;
    dash::meter_policy::MeterPolicy metadata;
    int32_t rule_count;
    int32_t eni_bind_count;
};
typedef std::map<std::string, MeterPolicyEntry> MeterPolicyTable;

struct MeterRuleBulkContext
{
    std::string meter_policy;
    uint32_t rule_num;
    dash::meter_rule::MeterRule metadata;
    std::deque<sai_object_id_t> object_ids;
    std::deque<sai_status_t> object_statuses;
    MeterRuleBulkContext() {}
    MeterRuleBulkContext(const MeterRuleBulkContext&) = delete;
    MeterRuleBulkContext(MeterRuleBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
    }
};

struct MeterRuleEntry
{
    sai_object_id_t meter_rule_oid;
    dash::meter_rule::MeterRule metadata;
    std::string meter_policy;
    uint32_t rule_num;
};
typedef std::map<std::string, MeterRuleEntry> MeterRuleTable;


class DashMeterOrch : public ZmqOrch
{
public:
    using TaskArgs = std::vector<swss::FieldValueTuple>;

    DashMeterOrch(swss::DBConnector *db, const std::vector<std::string> &tables, DashOrch *dash_orch, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);
    sai_object_id_t getMeterPolicyOid(const std::string& meter_policy) const;
    int32_t getMeterPolicyEniBindCount(const std::string& meter_policy) const;
    void incrMeterPolicyEniBindCount(const std::string& meter_policy);
    void decrMeterPolicyEniBindCount(const std::string& meter_policy);
    void addEniToMeterFC(sai_object_id_t oid, const std::string& name);
    void removeEniFromMeterFC(sai_object_id_t oid, const std::string& name);
    void handleMeterFCStatusUpdate(bool is_enabled);

private:

    void doTask(swss::SelectableTimer&);
    void doTask(ConsumerBase &consumer);
    void doTaskMeterPolicyTable(ConsumerBase &consumer);
    void doTaskMeterRuleTable(ConsumerBase &consumer);

    bool addMeterPolicy(const std::string& meter_policy, MeterPolicyContext& ctxt);
    bool removeMeterPolicy(const std::string& meter_policy);

    uint32_t getMeterPolicyRuleCount(const std::string& meter_policy) const;
    sai_ip_addr_family_t getMeterPolicyAddrFamily(const std::string& meter_policy) const;
    bool isV4(const std::string& meter_policy) const;

    bool addMeterRule(const std::string& key, MeterRuleBulkContext& ctxt);
    bool addMeterRulePost(const std::string& key, const MeterRuleBulkContext& ctxt);
    bool removeMeterRule(const std::string& key, MeterRuleBulkContext& ctxt);
    bool removeMeterRulePost(const std::string& key, const MeterRuleBulkContext& ctxt);

    bool isMeterPolicyBound(const std::string& meter_policy) const;
    void incrMeterPolicyRuleCount(const std::string& meter_policy);
    void decrMeterPolicyRuleCount(const std::string& meter_policy);

    DashOrch *m_dash_orch;
    MeterPolicyTable meter_policy_entries_;
    MeterRuleTable meter_rule_entries_;
    ObjectBulker<sai_dash_meter_api_t> meter_rule_bulker_;
    bool m_meter_fc_status = false;
    FlexCounterManager m_meter_stat_manager;
    std::unordered_set<std::string> m_meter_counter_stats;
    std::map<sai_object_id_t, std::string> m_meter_stat_work_queue;
    std::unique_ptr<swss::Table> m_eni_name_table;
    std::unique_ptr<swss::Table> m_vid_to_rid_table;
    std::shared_ptr<swss::DBConnector> m_counter_db;
    std::shared_ptr<swss::DBConnector> m_asic_db;
    swss::SelectableTimer* m_meter_fc_update_timer = nullptr;
    //void refreshMeterFCStats(bool install);
};
