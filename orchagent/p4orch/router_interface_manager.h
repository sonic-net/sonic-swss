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
    sai_object_id_t router_interface_oid = 0;

    P4RouterInterfaceEntry() = default;
    P4RouterInterfaceEntry(const std::string &router_intf_id, const std::string &port,
                           const swss::MacAddress &mac_address)
        : router_interface_id(router_intf_id), port_name(port), src_mac_address(mac_address)
    {
    }
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

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

  private:
    ReturnCodeOr<P4RouterInterfaceAppDbEntry> deserializeRouterIntfEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);
    P4RouterInterfaceEntry *getRouterInterfaceEntry(const std::string &router_intf_key);
    ReturnCode createRouterInterface(const std::string &router_intf_key, P4RouterInterfaceEntry &router_intf_entry);
    ReturnCode removeRouterInterface(const std::string &router_intf_key);
    ReturnCode setSourceMacAddress(P4RouterInterfaceEntry *router_intf_entry, const swss::MacAddress &mac_address);
    ReturnCode processAddRequest(const P4RouterInterfaceAppDbEntry &app_db_entry, const std::string &router_intf_key);
    ReturnCode processUpdateRequest(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                    P4RouterInterfaceEntry *router_intf_entry);
    ReturnCode processDeleteRequest(const std::string &router_intf_key);

    P4RouterInterfaceTable m_routerIntfTable;
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class RouterInterfaceManagerTest;
};
