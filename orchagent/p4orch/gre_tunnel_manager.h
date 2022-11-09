#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch/router_interface_manager.h"
#include "response_publisher_interface.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
}

// P4GreTunnelEntry holds GreTunnelManager's internal cache of P4 GRE tunnel
// entry. Example: P4RT_TABLE:FIXED_TUNNEL_TABLE:{"match/tunnel_id":"tunnel-1"}
//   "action" = "mark_for_tunnel_encap",
//   "param/router_interface_id" = "intf-eth-1/2/3",
//   "param/encap_src_ip" = "2607:f8b0:8096:3110::1",
//   "param/encap_dst_ip" = "2607:f8b0:8096:311a::2",
//   "controller_metadata" = "..."
struct P4GreTunnelEntry
{
    // Key of this entry, built from tunnel_id.
    std::string tunnel_key;

    // Fields from P4 table.
    // Match
    std::string tunnel_id;
    // Action
    std::string router_interface_id;
    swss::IpAddress encap_src_ip;
    swss::IpAddress encap_dst_ip;
    // neighbor_id is required to be equal to encap_dst_ip by BRCM. And the
    // neighbor entry needs to be created before GRE tunnel object
    swss::IpAddress neighbor_id;

    // SAI OID associated with this entry.
    sai_object_id_t tunnel_oid = SAI_NULL_OBJECT_ID;
    // SAI OID of a loopback rif for SAI_TUNNEL_ATTR_OVERLAY_INTERFACE
    sai_object_id_t overlay_if_oid = SAI_NULL_OBJECT_ID;
    // SAI OID of the router_interface_id for SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE
    sai_object_id_t underlay_if_oid = SAI_NULL_OBJECT_ID;

    P4GreTunnelEntry(const std::string &tunnel_id, const std::string &router_interface_id,
                     const swss::IpAddress &encap_src_ip, const swss::IpAddress &encap_dst_ip,
                     const swss::IpAddress &neighbor_id);
};

// GreTunnelManager listens to changes in table APP_P4RT_TUNNEL_TABLE_NAME and
// creates/updates/deletes tunnel SAI object accordingly.
class GreTunnelManager : public ObjectManagerInterface
{
  public:
    GreTunnelManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }

    virtual ~GreTunnelManager() = default;

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;

    ReturnCodeOr<const P4GreTunnelEntry> getConstGreTunnelEntry(const std::string &gre_tunnel_key);

  private:
    // Gets the internal cached GRE tunnel entry by its key.
    // Return nullptr if corresponding GRE tunnel entry is not cached.
    P4GreTunnelEntry *getGreTunnelEntry(const std::string &gre_tunnel_key);

    // Deserializes an entry from table APP_P4RT_TUNNEL_TABLE_NAME.
    ReturnCodeOr<P4GreTunnelAppDbEntry> deserializeP4GreTunnelAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    // Processes add operation for an entry.
    ReturnCode processAddRequest(const P4GreTunnelAppDbEntry &app_db_entry);

    // Creates an GRE tunnel in the GRE tunnel table. Return true on success.
    ReturnCode createGreTunnel(P4GreTunnelEntry &gre_tunnel_entry);

    // Processes update operation for an entry.
    ReturnCode processUpdateRequest(const P4GreTunnelAppDbEntry &app_db_entry, P4GreTunnelEntry *gre_tunnel_entry);

    // Processes delete operation for an entry.
    ReturnCode processDeleteRequest(const std::string &gre_tunnel_key);

    // Deletes a GRE tunnel in the GRE tunnel table. Return true on success.
    ReturnCode removeGreTunnel(const std::string &gre_tunnel_key);

    std::string verifyStateCache(const P4GreTunnelAppDbEntry &app_db_entry, const P4GreTunnelEntry *gre_tunnel_entry);
    std::string verifyStateAsicDb(const P4GreTunnelEntry *gre_tunnel_entry);

    // m_greTunnelTable: gre_tunnel_key, P4GreTunnelEntry
    std::unordered_map<std::string, P4GreTunnelEntry> m_greTunnelTable;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class GreTunnelManagerTest;
    friend class NextHopManagerTest;
};
