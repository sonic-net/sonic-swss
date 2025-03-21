#pragma once

#include <deque>

#include "orchagent/orch.h"
#include "orchagent/p4orch/object_manager_interface.h"
#include "orchagent/p4orch/p4oidmapper.h"
#include "orchagent/p4orch/p4orch_util.h"
#include "orchagent/response_publisher_interface.h"
#include "orchagent/return_code.h"

#define EMPTY_STRING ""

struct P4L3AdmitEntry
{
    std::string port_name; // Optional
    swss::MacAddress mac_address_data;
    swss::MacAddress mac_address_mask;
    sai_uint32_t priority;
    sai_object_id_t l3_admit_oid = SAI_NULL_OBJECT_ID;

    P4L3AdmitEntry() = default;
    P4L3AdmitEntry(const swss::MacAddress &mac_address_data, const swss::MacAddress &mac_address_mask,
                   const sai_uint32_t &priority, const std::string &port_name)
        : port_name(port_name), mac_address_data(mac_address_data), mac_address_mask(mac_address_mask),
          priority(priority)
    {
    }
};

// L3Admit manager is responsible for subscribing to APPL_DB FIXED_L3_ADMIT
// table.
//
// Example without optional port
// P4RT_TABLE:FIXED_L3_ADMIT_TABLE:{\"match/dst_mac\":\"00:02:03:04:00:00&ff:ff:ff:ff:00:00\",\"priority\":2030}
// "action": "admit_to_l3"
// "controller_metadata": "..."
//
// Example with optional port
// P4RT_TABLE:FIXED_L3_ADMIT_TABLE:{\"match/dst_mac\":\"00:02:03:04:00:00&ff:ff:ff:ff:00:00\",\"match/in_port\":\"Ethernet0\",\"priority\":2030}
// "action": "admit_to_l3"
// "controller_metadata": "..."
//
// Example without optional port/dst_mac
// P4RT:FIXED_L3_ADMIT_TABLE:{\"priority\":2030}
// "action": "admit_to_l3"
// "controller_metadata": "..."
class L3AdmitManager : public ObjectManagerInterface
{
  public:
    L3AdmitManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }

    virtual ~L3AdmitManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

  private:
    // Gets the internal cached next hop entry by its key.
    // Return nullptr if corresponding next hop entry is not cached.
    P4L3AdmitEntry *getL3AdmitEntry(const std::string &l3_admit_key);

    // Deserializes an entry from table APP_P4RT_L3_ADMIT_TABLE_NAME.
    ReturnCodeOr<P4L3AdmitAppDbEntry> deserializeP4L3AdmitAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    ReturnCode processAddRequest(const P4L3AdmitAppDbEntry &app_db_entry, const std::string &l3_admit_key);

    // Creates a L3 Admit entry. Return true on success.
    ReturnCode createL3Admit(P4L3AdmitEntry &l3_admit_entry);

    ReturnCode processDeleteRequest(const std::string &l3_admit_key);

    // Deletes a L3 Admit entry. Return true on success.
    ReturnCode removeL3Admit(const std::string &l3_admit_key);

    // state verification DB helper functions. Return err string or empty string.
    std::string verifyStateCache(const P4L3AdmitAppDbEntry &app_db_entry, const P4L3AdmitEntry *l3_admit_entry);
    std::string verifyStateAsicDb(const P4L3AdmitEntry *l3_admit_entry);

    // m_l3AdmitTable: l3_admit_key, P4L3AdmitEntry
    std::unordered_map<std::string, P4L3AdmitEntry> m_l3AdmitTable;

    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
    P4OidMapper *m_p4OidMapper;

    friend class L3AdmitManagerTest;
};
