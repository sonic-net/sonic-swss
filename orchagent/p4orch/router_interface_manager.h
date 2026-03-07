#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "macaddress.h"
#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
}

struct P4RouterInterfaceEntry
{
    std::string router_interface_id;
    std::string port_name;
    swss::MacAddress src_mac_address;
    uint16_t vlan_id = 0;
    bool has_vlan_id = false;
    sai_object_id_t router_interface_oid = 0;

    P4RouterInterfaceEntry() = default;
    P4RouterInterfaceEntry(const std::string &router_intf_id, const std::string &port,
                           const swss::MacAddress& mac_address,
                           uint16_t vlan_id, bool has_vlan)
        : router_interface_id(router_intf_id), port_name(port), 
          src_mac_address(mac_address),
          vlan_id(vlan_id),
          has_vlan_id(has_vlan) {}
};

// P4RouterInterfaceTable: Router Interface key, P4RouterInterfaceEntry
typedef std::unordered_map<std::string, P4RouterInterfaceEntry> P4RouterInterfaceTable;

class RouterInterfaceManager : public ObjectManagerInterface
{
  public:
    RouterInterfaceManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }
    virtual ~RouterInterfaceManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

  private:
    ReturnCodeOr<P4RouterInterfaceAppDbEntry> deserializeRouterIntfEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);
    ReturnCode validateRouterInterfaceAppDbEntry(
        const P4RouterInterfaceAppDbEntry& app_db_entry);
    ReturnCode validateRouterInterfaceEntryOperation(
        const P4RouterInterfaceAppDbEntry& app_db_entry, const std::string& operation);
    P4RouterInterfaceEntry *getRouterInterfaceEntry(const std::string &router_intf_key);
    std::vector<ReturnCode> createRouterInterfaces(
        const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries);
    std::vector<ReturnCode> removeRouterInterfaces(
        const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries);
    std::vector<ReturnCode> updateRouterInterfaces(
        const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries);
    ReturnCode processEntries(
        const std::vector<P4RouterInterfaceAppDbEntry>& entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update);
    std::string verifyStateCache(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                 const P4RouterInterfaceEntry *router_intf_entry);
    std::string verifyStateAsicDb(const P4RouterInterfaceEntry *router_intf_entry);

    P4RouterInterfaceTable m_routerIntfTable;
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class RouterInterfaceManagerTest;
};
