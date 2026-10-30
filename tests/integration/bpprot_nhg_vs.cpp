extern "C" {
#include <sai.h>
#include <saimetadata.h>
}
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

static const char *profile(sai_switch_profile_id_t, const char *key)
{
    if (!strcmp(key, "SAI_VS_SWITCH_TYPE")) return "SAI_VS_SWITCH_TYPE_BCM56850";
    if (!strcmp(key, "SAI_VS_HOSTIF_USE_TAP_DEVICE")) return "false";
    return nullptr;
}
static int next(sai_switch_profile_id_t, const char **, const char **) { return -1; }
int main()
{
    const auto *metadata = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL, SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC);
    assert(metadata && metadata->iscreateandset);
    sai_service_method_table_t methods{profile, next};
    assert(sai_api_initialize(0, &methods) == SAI_STATUS_SUCCESS);
    sai_switch_api_t *sw; sai_router_interface_api_t *rif; sai_tunnel_api_t *tunnel;
    assert(sai_api_query(SAI_API_SWITCH, reinterpret_cast<void **>(&sw)) == SAI_STATUS_SUCCESS);
    assert(sai_api_query(SAI_API_ROUTER_INTERFACE, reinterpret_cast<void **>(&rif)) == SAI_STATUS_SUCCESS);
    assert(sai_api_query(SAI_API_TUNNEL, reinterpret_cast<void **>(&tunnel)) == SAI_STATUS_SUCCESS);
    sai_attribute_t init{}; init.id=SAI_SWITCH_ATTR_INIT_SWITCH; init.value.booldata=true;
    sai_object_id_t sid; assert(sw->create_switch(&sid,1,&init) == SAI_STATUS_SUCCESS);
    sai_attribute_t vr{}; vr.id=SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
    assert(sw->get_switch_attribute(sid,1,&vr) == SAI_STATUS_SUCCESS);
    sai_attribute_t r[2]{}; r[0].id=SAI_ROUTER_INTERFACE_ATTR_TYPE; r[0].value.s32=SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    r[1].id=SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID; r[1].value.oid=vr.value.oid;
    sai_object_id_t rid; assert(rif->create_router_interface(&rid,sid,2,r) == SAI_STATUS_SUCCESS);
    sai_attribute_t a[5]{};
    a[0].id=SAI_TUNNEL_ATTR_TYPE; a[0].value.s32=SAI_TUNNEL_TYPE_VXLAN;
    a[1].id=SAI_TUNNEL_ATTR_PEER_MODE; a[1].value.s32=SAI_TUNNEL_PEER_MODE_P2P;
    a[2].id=SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE; a[2].value.oid=rid;
    a[3].id=SAI_TUNNEL_ATTR_ENCAP_SRC_IP; a[3].value.ipaddr.addr_family=SAI_IP_ADDR_FAMILY_IPV4; a[3].value.ipaddr.addr.ip4=0x0100000a;
    a[4].id=SAI_TUNNEL_ATTR_ENCAP_DST_IP; a[4].value.ipaddr.addr_family=SAI_IP_ADDR_FAMILY_IPV4; a[4].value.ipaddr.addr.ip4=0x0200000a;
    sai_object_id_t oid; assert(tunnel->create_tunnel(&oid,sid,5,a)==SAI_STATUS_SUCCESS);
    sai_attribute_t mac{}; mac.id=SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC; mac.value.mac[0]=2; mac.value.mac[5]=3;
    assert(tunnel->set_tunnel_attribute(oid,&mac)==SAI_STATUS_SUCCESS);
    sai_attribute_t observed{}; observed.id=SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC;
    assert(tunnel->get_tunnel_attribute(oid,1,&observed)==SAI_STATUS_SUCCESS);
    assert(!memcmp(mac.value.mac,observed.value.mac,sizeof(sai_mac_t)));
    sai_next_hop_api_t *nh; sai_next_hop_group_api_t *nhg;
    assert(sai_api_query(SAI_API_NEXT_HOP,reinterpret_cast<void **>(&nh))==SAI_STATUS_SUCCESS);
    assert(sai_api_query(SAI_API_NEXT_HOP_GROUP,reinterpret_cast<void **>(&nhg))==SAI_STATUS_SUCCESS);
    sai_attribute_t n[3]{};
    n[0].id=SAI_NEXT_HOP_ATTR_TYPE; n[0].value.s32=SAI_NEXT_HOP_TYPE_BRIDGE_PORT;
    n[1].id=SAI_NEXT_HOP_ATTR_IP; n[1].value.ipaddr=a[4].value.ipaddr;
    n[2].id=SAI_NEXT_HOP_ATTR_TUNNEL_ID; n[2].value.oid=oid;
    sai_object_id_t nhid; assert(nh->create_next_hop(&nhid,sid,3,n)==SAI_STATUS_SUCCESS);
    sai_attribute_t g{}; g.id=SAI_NEXT_HOP_GROUP_ATTR_TYPE; g.value.s32=SAI_NEXT_HOP_GROUP_TYPE_BRIDGE_PORT;
    sai_object_id_t gid1,gid2;
    assert(nhg->create_next_hop_group(&gid1,sid,1,&g)==SAI_STATUS_SUCCESS);
    assert(nhg->create_next_hop_group(&gid2,sid,1,&g)==SAI_STATUS_SUCCESS);
    sai_attribute_t m[2]{};
    m[0].id=SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID; m[0].value.oid=gid1;
    m[1].id=SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID; m[1].value.oid=nhid;
    sai_object_id_t gm1,gm2;
    assert(nhg->create_next_hop_group_member(&gm1,sid,2,m)==SAI_STATUS_SUCCESS);
    m[0].value.oid=gid2; assert(nhg->create_next_hop_group_member(&gm2,sid,2,m)==SAI_STATUS_SUCCESS);
    // Mutate RMAC while both groups actively reference the same next hop.
    // The tunnel, next hop and group-member identities remain readable/stable.
    mac.value.mac[5]=0x22;
    assert(tunnel->set_tunnel_attribute(oid,&mac)==SAI_STATUS_SUCCESS);
    assert(tunnel->get_tunnel_attribute(oid,1,&observed)==SAI_STATUS_SUCCESS);
    assert(!memcmp(mac.value.mac,observed.value.mac,sizeof(sai_mac_t)));
    sai_attribute_t readHop{}; readHop.id=SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    assert(nh->get_next_hop_attribute(nhid,1,&readHop)==SAI_STATUS_SUCCESS);
    assert(readHop.value.oid==oid);
    sai_attribute_t readMember{}; readMember.id=SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    for (const auto member : {gm1,gm2}) {
        assert(nhg->get_next_hop_group_member_attribute(member,1,&readMember)==SAI_STATUS_SUCCESS);
        assert(readMember.value.oid==nhid);
    }
    assert(nh->remove_next_hop(nhid)==SAI_STATUS_OBJECT_IN_USE);
    assert(tunnel->remove_tunnel(oid)==SAI_STATUS_OBJECT_IN_USE);
    const auto stableGroup=gid1;
    assert(nhg->remove_next_hop_group_member(gm1)==SAI_STATUS_SUCCESS);
    m[0].value.oid=gid1; assert(nhg->create_next_hop_group_member(&gm1,sid,2,m)==SAI_STATUS_SUCCESS);
    assert(gid1==stableGroup);
    sai_attribute_t readGroup{}; readGroup.id=SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    assert(nhg->get_next_hop_group_attribute(gid1,1,&readGroup)==SAI_STATUS_SUCCESS);
    assert(readGroup.value.s32==SAI_NEXT_HOP_GROUP_TYPE_BRIDGE_PORT);
    assert(nhg->remove_next_hop_group_member(gm2)==SAI_STATUS_SUCCESS);
    assert(nh->remove_next_hop(nhid)==SAI_STATUS_OBJECT_IN_USE);
    assert(nhg->remove_next_hop_group(gid1)==SAI_STATUS_OBJECT_IN_USE);
    assert(nhg->get_next_hop_group_attribute(gid1,1,&readGroup)==SAI_STATUS_SUCCESS);
    assert(nhg->remove_next_hop_group_member(gm1)==SAI_STATUS_SUCCESS);
    assert(nh->remove_next_hop(nhid)==SAI_STATUS_SUCCESS);
    assert(nhg->remove_next_hop_group(gid1)==SAI_STATUS_SUCCESS);
    assert(nhg->remove_next_hop_group(gid2)==SAI_STATUS_SUCCESS);
    sai_attribute_t defaultMac{}; defaultMac.id=SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC;
    assert(sw->get_switch_attribute(sid,1,&defaultMac)==SAI_STATUS_SUCCESS);
    memcpy(mac.value.mac,defaultMac.value.mac,sizeof(sai_mac_t));
    assert(tunnel->set_tunnel_attribute(oid,&mac)==SAI_STATUS_SUCCESS);
    assert(tunnel->get_tunnel_attribute(oid,1,&observed)==SAI_STATUS_SUCCESS);
    assert(!memcmp(mac.value.mac,observed.value.mac,sizeof(sai_mac_t)));
    assert(tunnel->remove_tunnel(oid)==SAI_STATUS_SUCCESS);
    assert(rif->remove_router_interface(rid)==SAI_STATUS_SUCCESS);
    assert(sw->remove_switch(sid)==SAI_STATUS_SUCCESS);
    assert(sai_api_uninitialize()==SAI_STATUS_SUCCESS);
    puts("PASS: real libVS BRIDGE_PORT NH/NHG sharing, stable group membership churn, deletion dependency ordering and tunnel RMAC; no DVS testbed");
}
