#include "common/vxlan_ut_helpers.h"

#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "vxlanorch.h"

using namespace std;

extern Directory<Orch*> gDirectory;

namespace vxlan_ut_helpers {

void setUpVxlanPort(string vtep_ip_addr, sai_object_id_t vtep_obj_id)
{
    PortsOrch* portsOrch = gDirectory.get<PortsOrch*>();
    assert(portsOrch != nullptr);

    VxlanTunnelOrch* vxlanTunnelOrch = gDirectory.get<VxlanTunnelOrch*>();
    assert(vxlanTunnelOrch != nullptr);

    EvpnNvoOrch* evpnNvoOrch = gDirectory.get<EvpnNvoOrch*>();
    assert(evpnNvoOrch != nullptr);

    string port_alias = EVPN_TUNNEL_PORT_PREFIX + vtep_ip_addr;
    sai_object_id_t oid = vtep_obj_id;

    Port port(port_alias, Port::PHY);
    portsOrch->m_portList[port_alias] = port;
    portsOrch->saiOidToAlias[oid] = port_alias;
    portsOrch->m_portList[port_alias].m_tunnel_id = oid;

    string tunnel_name = EVPN_TUNNEL_NAME_PREFIX + vtep_ip_addr;
    vxlanTunnelOrch->vxlan_tunnel_table_[tunnel_name] =  std::unique_ptr<VxlanTunnel>(new VxlanTunnel(tunnel_name, IpAddress("5.5.5.5"), IpAddress(vtep_ip_addr), TNL_CREATION_SRC_CLI));
    evpnNvoOrch->source_vtep_ptr = vxlanTunnelOrch->vxlan_tunnel_table_.at(tunnel_name).get();
    evpnNvoOrch->source_vtep_ptr->updateRemoteEndPointIpRef(vtep_ip_addr, true);
}

void setUpVxlanMember(string vtep_ip_addr, sai_object_id_t vtep_obj_id, string vlan)
{
    PortsOrch* portsOrch = gDirectory.get<PortsOrch*>();
    assert(portsOrch != nullptr);

    string port_alias = EVPN_TUNNEL_PORT_PREFIX + vtep_ip_addr;

    /* Add Bridge Port */
    portsOrch->m_portList[port_alias].m_bridge_port_id = vtep_obj_id;
    portsOrch->saiOidToAlias[vtep_obj_id] = port_alias;
    portsOrch->m_portList[vlan].m_members.insert(port_alias);

    Port port = portsOrch->m_portList[port_alias];

    PortUpdate update = { port, true };
    portsOrch->notify(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, static_cast<void *>(&update));
}

} /* namespace vxlan_ut_helpers */
