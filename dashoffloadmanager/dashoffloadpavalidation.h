#pragma once

#include "dpuinfoprovider.h"
#include "zmqorch.h"
#include "zmqserver.h"
#include "producerstatetable.h"
#include "ipaddress.h"

#include "dash_api/pa_validation.pb.h"

#include <unordered_map>
#include <unordered_set>

struct PaValidationEntry
{
    std::string vni;
    std::vector<swss::IpAddress> addresses;

    PaValidationEntry(const std::string& vni, const dash::pa_validation::PaValidation &entry);
};

struct IpAddressHash
{
    std::size_t operator()(swss::IpAddress value) const;
};

class DashPaVlidationOffloadOrch : public ZmqOrch
{
public:
    DashPaVlidationOffloadOrch(const DpuInfo& dpuInfo, swss::DBConnector *applDb, swss::ZmqServer *zmqServer);
    ~DashPaVlidationOffloadOrch();
    void doTask(ConsumerBase& consumer);
    void cleanup();

private:
    static std::once_flag table_type_create_once;

    DpuInfo m_dpuInfo;
    std::string m_dpuLogKey;

    swss::ProducerStateTable m_applAclTableTypeTable;
    swss::ProducerStateTable m_applAclTableTable;
    swss::ProducerStateTable m_applAclRuleTable;

    std::string m_offloadAclTable;
    std::unordered_map<std::string, std::unordered_set<swss::IpAddress, IpAddressHash>> m_vni_map;

    using AclRule = std::tuple<std::string, std::vector<swss::FieldValueTuple>>;

    void initializeAclConfig();
    void createAclTableType();
    void addOffloadAclTable();
    void buildAclRules(const PaValidationEntry& entry, size_t rule_index_base, std::vector<AclRule> &forward_rules, AclRule &drop_rule);
    void addOffloadAclConfig(const PaValidationEntry& entry, bool new_vni, size_t rule_index_base);
    void removeOffloadAclConfig(const std::string& vni, size_t num_of_rules);

    void addPaValidationEntry(PaValidationEntry &entry);
    void removePaValidationEntry(const std::string& paValidationKey);

    bool validateEntryAdd(PaValidationEntry& entry, size_t& num_of_existing_addresses) const;
    void entryAddToVniMap(const PaValidationEntry& entry);

    std::string makeAclRuleKey(const std::string &vni, size_t rule_index) const;
    std::string makeAclDropRuleKey(const std::string &vni) const;
};
