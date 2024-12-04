#pragma once

#include "zmqorch.h"
#include "bulker.h"
#include "ipaddress.h"

#include "dash_api/pa_validation.pb.h"

class DashVnetOrch;

using PaValidationEntryAddresses = std::vector<swss::IpAddress>;

struct PaValidationEntry
{
    sai_object_id_t vnet_oid;
    uint32_t vni;
    swss::IpAddress address;
};

using PaValidationEntryList = std::vector<PaValidationEntry>;

struct IpAddressHash {
    std::size_t operator()(swss::IpAddress value) const;
};

class DashPaValidationOrch : public ZmqOrch
{
public:
    DashPaValidationOrch(swss::DBConnector *db, swss::ZmqServer *zmqServer);
    void setVnetOrch(const DashVnetOrch *vnetOrch);
    task_process_status addPaValidationEntries(const PaValidationEntryList& entries);
    task_process_status removePaValidationEntries(const PaValidationEntryList& entries);

private:
    using AddressRefcountMap = std::unordered_map<swss::IpAddress, uint64_t, IpAddressHash>;
    using VniAddressMap = std::unordered_map<uint32_t, AddressRefcountMap>;

    const DashVnetOrch *vnetOrch_;
    VniAddressMap vni_map_;

    void doTask(ConsumerBase& consumer);
    void doTaskPaValidation(ConsumerBase& consumer);

    bool entryRefCountInc(const PaValidationEntry& entry);
    bool entryRefCountDec(const PaValidationEntry& entry);
    bool fetchVniAddresses(uint32_t vni, PaValidationEntryAddresses& addresses) const;
    void paValidationConfigProcess(const PaValidationEntryList& toCreate, const PaValidationEntryList& toRemove);
    task_process_status paValidationConfigApply(const PaValidationEntryList& toCreate, const PaValidationEntryList& toRemove);

    bool parseVni(const std::string& key, uint32_t& vni);
    bool toAddresses(const dash::pa_validation::PaValidation& entry, PaValidationEntryAddresses &addresses);
};
