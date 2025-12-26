#pragma once

#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include "request_parser.h"
#include "portsorch.h"
#include "vrforch.h"
#include "timer.h"

enum class MAP_T
{
    MAP_TO_INVALID,
    VNI_TO_VLAN_ID,
    VLAN_ID_TO_VNI,
    VRID_TO_VNI,
    VNI_TO_VRID,
    BRIDGE_TO_VNI,
    VNI_TO_BRIDGE
};

typedef enum
{
    TUNNEL_MAP_T_VLAN=0,
    TUNNEL_MAP_T_BRIDGE,
    TUNNEL_MAP_T_VIRTUAL_ROUTER,
    TUNNEL_MAP_T_MAX_MAPPER,
} tunnel_map_type_t;

#define TUNNELMAP_SET_VLAN(x) ((x)|= (1<<TUNNEL_MAP_T_VLAN))
#define TUNNELMAP_SET_VRF(x) ((x)|= (1<<TUNNEL_MAP_T_VIRTUAL_ROUTER))
#define TUNNELMAP_SET_BRIDGE(x) ((x)|= (1<<TUNNEL_MAP_T_BRIDGE))

#define IS_TUNNELMAP_SET_VLAN(x) ((x)& (1<<TUNNEL_MAP_T_VLAN))
#define IS_TUNNELMAP_SET_VRF(x) ((x)& (1<<TUNNEL_MAP_T_VIRTUAL_ROUTER))
#define IS_TUNNELMAP_SET_BRIDGE(x) ((x)& (1<<TUNNEL_MAP_T_BRIDGE))

#define TUNNEL_STAT_COUNTER_FLEX_COUNTER_GROUP "TUNNEL_STAT_COUNTER"
#define TUNNEL_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000
#define LOCAL_TUNNEL_PORT_PREFIX "Port_SRC_VTEP_"
#define EVPN_TUNNEL_PORT_PREFIX  "Port_EVPN_"
#define EVPN_TUNNEL_NAME_PREFIX  "EVPN_"

#define MIN_VLAN_ID 1
#define MAX_VLAN_ID 4095

#define MAX_VNI_ID 16777215
#define DEFAULT_TUNNEL_ENCAP_TTL 255

typedef enum
{
    TNL_CREATION_SRC_CLI,
    TNL_CREATION_SRC_EVPN
} tunnel_creation_src_t;

typedef enum
{
    TUNNEL_MAP_USE_COMMON_ENCAP_DECAP,
    TUNNEL_MAP_USE_COMMON_DECAP_DEDICATED_ENCAP,
    TUNNEL_MAP_USE_DECAP_ONLY,
    TUNNEL_MAP_USE_DEDICATED_ENCAP_DECAP
} tunnel_map_use_t;

struct tunnel_ids_t
{
    sai_object_id_t tunnel_encap_id[TUNNEL_MAP_T_MAX_MAPPER+1];
    sai_object_id_t tunnel_decap_id[TUNNEL_MAP_T_MAX_MAPPER+1];
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_term_id;
};

struct nh_key_t
{
    IpAddress ip_addr;
    MacAddress mac_address;
    uint32_t vni=0;

    nh_key_t() = default;

    nh_key_t(IpAddress ipAddr, MacAddress macAddress=MacAddress(), uint32_t vnId=0)
    {
        ip_addr = ipAddr;
        mac_address = macAddress;
        vni = vnId;
    };

    bool operator== (const nh_key_t& rhs) const
    {
        if (!(ip_addr == rhs.ip_addr) || mac_address != rhs.mac_address || vni != rhs.vni)
        {
            return false;
        }
        return true;
    }
};

struct nh_key_hash
{
    size_t operator() (const nh_key_t& key) const
    {
        stringstream ss;
        ss << key.ip_addr.to_string() << key.mac_address.to_string() << std::to_string(key.vni);
        return std::hash<std::string>() (ss.str());
    }
};

struct nh_tunnel_t
{
    sai_object_id_t nh_id;
    int             ref_count;
};

typedef enum {
    TUNNEL_USER_IMR,
    TUNNEL_USER_MAC,
    TUNNEL_USER_IP
} tunnel_user_t;

class VxlanTunnel;

typedef struct tunnel_refcnts_s
{
   uint32_t      imr_refcnt;
   uint32_t      mac_refcnt;
   uint32_t      ip_refcnt;
   uint32_t      spurious_add_imr_refcnt;
   uint32_t      spurious_del_imr_refcnt;
} tunnel_refcnt_t;

typedef struct tunnel_map_entry_s
{
   sai_object_id_t map_entry_id;
   uint32_t        vlan_id;
   uint32_t        vni_id;
} tunnel_map_entry_t;

typedef std::map<uint32_t, std::pair<sai_object_id_t, sai_object_id_t>> TunnelMapEntries;
typedef std::unordered_map<nh_key_t, nh_tunnel_t, nh_key_hash> TunnelNHs;
typedef std::map<std::string, tunnel_refcnt_t> TunnelUsers;

enum class VxlanTunnelTTLMode
{
    NOT_SET,
    PIPE,
    UNIFORM
};

class VxlanTunnel
{
public:
    VxlanTunnel(string name, IpAddress srcIp, IpAddress dstIp, tunnel_creation_src_t src, VxlanTunnelTTLMode ttl_mode = VxlanTunnelTTLMode::NOT_SET);
    ~VxlanTunnel();

    bool isActive() const
    {
        return active_;
    }

    sai_object_id_t addEncapMapperEntry(sai_object_id_t obj, uint32_t vni, 
                                        tunnel_map_type_t type=TUNNEL_MAP_T_VIRTUAL_ROUTER);
    sai_object_id_t addDecapMapperEntry(sai_object_id_t obj, uint32_t vni,
                                        tunnel_map_type_t type=TUNNEL_MAP_T_VIRTUAL_ROUTER);

    void insertMapperEntry(sai_object_id_t encap, sai_object_id_t decap, uint32_t vni);
    std::pair<sai_object_id_t, sai_object_id_t> getMapperEntry(uint32_t vni);

    sai_object_id_t getTunnelId() const
    {
        return ids_.tunnel_id;
    }

    sai_object_id_t getDecapMapId(tunnel_map_type_t type) const
    {
        return ids_.tunnel_decap_id[type];
    }

    sai_object_id_t getEncapMapId(tunnel_map_type_t type) const
    {
        return ids_.tunnel_encap_id[type];
    }

    string getTunnelName() const
    {
        return tunnel_name_;
    }

    sai_object_id_t getTunnelTermId() const
    {
        return ids_.tunnel_term_id;
    }

    const IpAddress getSrcIP()
    {
        return src_ip_;
    }

    void updateNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni, sai_object_id_t nhId);
    bool removeNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);
    sai_object_id_t getNextHop(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni) const;

    void incNextHopRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);
    void decNextHopRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);

    bool deleteMapperHw(uint8_t mapper_list, tunnel_map_use_t map_src);
    bool createMapperHw(uint8_t mapper_list, tunnel_map_use_t map_src);
    bool createTunnelHw(uint8_t mapper_list, tunnel_map_use_t map_src, bool with_term = true, sai_uint8_t encap_ttl=DEFAULT_TUNNEL_ENCAP_TTL);
    bool deleteTunnelHw(uint8_t mapper_list, tunnel_map_use_t map_src, bool with_term = true);
    void deletePendingSIPTunnel();
    void increment_spurious_imr_add(const std::string remote_vtep);
    void increment_spurious_imr_del(const std::string remote_vtep);
    void updateRemoteEndPointRefCnt(bool , tunnel_refcnt_t& , tunnel_user_t );
    // Total Routes using the DIP tunnel. 
    int getRemoteEndPointRefCnt(const std::string);
    int getRemoteEndPointIMRRefCnt(const std::string);
    int getRemoteEndPointIPRefCnt(const std::string);
    // Total DIP tunnels associated with this SIP tunnel.
    int getDipTunnelCnt();
    bool createDynamicDIPTunnel(const string dip, tunnel_user_t usr);
    bool deleteDynamicDIPTunnel(const string dip, tunnel_user_t usr, bool update_refcnt = true);
    bool isTunnelReferenced(void);
    void updateRemoteEndPointIpRef(const std::string remote_vtep, bool inc);
    uint32_t vlan_vrf_vni_count = 0;
    bool del_tnl_hw_pending = false;

private:
    string tunnel_name_;
    bool active_ = false;

    tunnel_ids_t ids_ = {{0}, {0}, 0, 0};
    std::pair<MAP_T, MAP_T> tunnel_map_ = { MAP_T::MAP_TO_INVALID, MAP_T::MAP_TO_INVALID };

    TunnelMapEntries tunnel_map_entries_;
    TunnelNHs nh_tunnels_;

    IpAddress src_ip_;
    IpAddress dst_ip_ = 0x0;

    TunnelUsers tnl_users_;
    VxlanTunnel* vtep_ptr=NULL;
    tunnel_creation_src_t src_creation_;
    VxlanTunnelTTLMode decap_ttl_mode_; // Decap TTL mode: NOT_SET, PIPE, or UNIFORM (default is NOT_SET)
    uint8_t encap_dedicated_mappers_ = 0;
    uint8_t decap_dedicated_mappers_ = 0;
};

const request_description_t vxlan_tunnel_request_description = {
            { REQ_T_STRING },
            {
                { "src_ip", REQ_T_IP },
                { "dst_ip", REQ_T_IP },
                { "ttl_mode", REQ_T_STRING },
            },
            { "src_ip" }
};

class VxlanTunnelRequest : public Request
{
public:
    VxlanTunnelRequest() : Request(vxlan_tunnel_request_description, ':') { }
};

typedef std::unique_ptr<VxlanTunnel> VxlanTunnel_T;
typedef std::map<std::string, VxlanTunnel_T> VxlanTunnelTable;
typedef std::map<uint32_t, uint16_t> VxlanVniVlanMapTable;
typedef std::map<IpAddress, VxlanTunnel*> VTEPTable;

class VxlanTunnelOrch : public Orch2
{
public:
    VxlanTunnelOrch(DBConnector *statedb, DBConnector *db, const std::string& tableName);

    bool isTunnelExists(const std::string& tunnelName) const
    {
        return vxlan_tunnel_table_.find(tunnelName) != std::end(vxlan_tunnel_table_);
    }

    VxlanTunnel* getVxlanTunnel(const std::string& tunnelName)
    {
        return vxlan_tunnel_table_.at(tunnelName).get();
    }

    bool addTunnel(const std::string tunnel_name,VxlanTunnel* tnlptr)
    {
       vxlan_tunnel_table_[tunnel_name] = (VxlanTunnel_T)tnlptr;
       return true;
    }

    bool delTunnel(const std::string tunnel_name)
    {
       vxlan_tunnel_table_.erase(tunnel_name);
       return true;
    }

    bool isVTEPExists(const IpAddress& sip) const
    {
        return vtep_table_.find(sip) != std::end(vtep_table_);
    }

    VxlanTunnel* getVTEP(const IpAddress& sip)
    {
        return vtep_table_.at(sip);
    }

    void addVTEP(VxlanTunnel* pvtep,const IpAddress& sip)
    {
       vtep_table_[sip] = pvtep;
    }


    bool createVxlanTunnelMap(string tunnelName, tunnel_map_type_t mapType, uint32_t vni,
                              sai_object_id_t encap, sai_object_id_t decap, uint8_t encap_ttl=DEFAULT_TUNNEL_ENCAP_TTL);

    bool removeVxlanTunnelMap(string tunnelName, uint32_t vni);

    sai_object_id_t
    createNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni=0);

    bool
    removeNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni=0);

    bool getTunnelPort(const std::string& vtep,Port& tunnelPort, bool local=false);

    bool addTunnelUser(string remote_vtep, uint32_t vni_id,
                       uint32_t vlan, tunnel_user_t usr,
                       sai_object_id_t vrf_id=SAI_NULL_OBJECT_ID);

    bool delTunnelUser(string remote_vtep, uint32_t vni_id,
                       uint32_t vlan, tunnel_user_t usr,
                       sai_object_id_t vrf_id=SAI_NULL_OBJECT_ID);

    void deleteTunnelPort(Port &tunnelPort);

    void addRemoveStateTableEntry(const string, IpAddress&, IpAddress&, tunnel_creation_src_t, const string, bool);

    std::string getTunnelPortName(const std::string& vtep, bool local=false);
    void getTunnelNameFromDIP(const string& dip, string& tunnel_name);
    void getTunnelNameFromPort(string& tunnel_portname, string& tunnel_name);
    void getTunnelDIPFromPort(Port& tunnelPort, string& remote_vtep);
    void updateDbTunnelOperStatus(string tunnel_portname,
                                               sai_port_oper_status_t status);
    uint16_t getVlanMappedToVni(const uint32_t vni)
    {
        if (vxlan_vni_vlan_map_table_.find(vni) != std::end(vxlan_vni_vlan_map_table_))
        {
            return vxlan_vni_vlan_map_table_.at(vni);
        }
        else
        {
            return 0;
        }
    }

    void addVlanMappedToVni(uint32_t vni, uint16_t vlan_id)
    {
        vxlan_vni_vlan_map_table_[vni] = vlan_id;
    }

    void delVlanMappedToVni(uint32_t vni)
    {
        vxlan_vni_vlan_map_table_.erase(vni);
    }

    unordered_set<string> generateTunnelCounterStats();
    void generateTunnelCounterMap();
    void addTunnelToFlexCounter(sai_object_id_t oid, const std::string &name);
    void removeTunnelFromFlexCounter(sai_object_id_t oid, const std::string &name);
    bool isDipTunnelsSupported(void)
    {
        return is_dip_tunnel_supported;
    }


private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);
    void doTask(swss::SelectableTimer&);

    VxlanTunnelTable vxlan_tunnel_table_;
    VxlanTunnelRequest request_;
    VxlanVniVlanMapTable vxlan_vni_vlan_map_table_;
    VTEPTable vtep_table_;
    Table m_stateVxlanTable;
    std::map<sai_object_id_t, std::string> m_pendingAddToFlexCntr;
    FlexCounterManager vxlan_tunnel_stat_manager;
    bool m_isTunnelCounterMapGenerated = false;
    FlexCounterManager *tunnel_stat_manager;
    unique_ptr<Table> m_tunnelNameTable;
    unique_ptr<Table> m_tunnelTypeTable;
    unique_ptr<Table> m_vidToRidTable;
    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_asic_db;
    SelectableTimer* m_FlexCounterUpdTimer = nullptr;
    bool is_dip_tunnel_supported;
};

const request_description_t vxlan_tunnel_map_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vni",  REQ_T_UINT },
                { "vlan", REQ_T_VLAN },
            },
            { "vni", "vlan" }
};

typedef std::map<std::string, tunnel_map_entry_t> VxlanTunnelMapTable;

class VxlanTunnelMapRequest : public Request
{
public:
    VxlanTunnelMapRequest() : Request(vxlan_tunnel_map_request_description, ':') { }
};

class VxlanTunnelMapOrch : public Orch2
{
public:
    VxlanTunnelMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelMapExists(const std::string& name) const
    {
        return vxlan_tunnel_map_table_.find(name) != std::end(vxlan_tunnel_map_table_);
    }

    bool isVniVlanMapExists(uint32_t vni_id, std::string& vniVlanMapName, sai_object_id_t *tnl_map_entry_id, uint32_t *vlan_id);

    void updateTnlMapId(std::string vniVlanMapName, sai_object_id_t tunnel_map_id);
private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanTunnelMapTable vxlan_tunnel_map_table_;
    VxlanTunnelMapRequest request_;
};

const request_description_t vxlan_vrf_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vni", REQ_T_UINT },
                { "vrf", REQ_T_STRING },
            },
            { "vni", "vrf" }
};

class VxlanVrfRequest : public Request
{
public:
    VxlanVrfRequest() : Request(vxlan_vrf_request_description, ':') { }
};

struct vrf_map_entry_t {
    sai_object_id_t encap_id;
    sai_object_id_t decap_id;
    bool isL2Vni;
    std::string vniVlanMapName;
    uint32_t vlan_id;
    uint32_t vni_id;
};

typedef std::map<string, vrf_map_entry_t> VxlanVrfTable;
typedef std::map<string, sai_object_id_t> VxlanVrfTunnel;

class VxlanVrfMapOrch : public Orch2
{
public:
    VxlanVrfMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    typedef std::pair<sai_object_id_t, sai_object_id_t> handler_pair;

    bool isVrfMapExists(const std::string& name) const
    {
        return vxlan_vrf_table_.find(name) != std::end(vxlan_vrf_table_);
    }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanVrfTable vxlan_vrf_table_;
    VxlanVrfTunnel vxlan_vrf_tunnel_;
    VxlanVrfRequest request_;
};

//---------------- EVPN_REMOTE_VNI table ---------------------

const request_description_t evpn_remote_vni_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vni",  REQ_T_UINT },
            },
            { "vni" }
};

class EvpnRemoteVniRequest : public Request
{
public:
    EvpnRemoteVniRequest() : Request(evpn_remote_vni_request_description, ':') { }
};

class EvpnRemoteVnip2pOrch : public Orch2
{
public:
    EvpnRemoteVnip2pOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }


private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    EvpnRemoteVniRequest request_;
};

class EvpnRemoteVnip2mpOrch : public Orch2
{
public:
    EvpnRemoteVnip2mpOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }


private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    EvpnRemoteVniRequest request_;
};

//------------- EVPN_NVO Table -------------------------

const request_description_t evpn_nvo_request_description = {
            { REQ_T_STRING},
            {
                { "source_vtep",  REQ_T_STRING },
            },
            { "source_vtep" }
};

class EvpnNvoRequest : public Request
{
public:
    EvpnNvoRequest() : Request(evpn_nvo_request_description, ':') { }
};

class EvpnNvoOrch : public Orch2
{
public:
    EvpnNvoOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    VxlanTunnel* getEVPNVtep() 
    { 
        return source_vtep_ptr;
    }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    EvpnNvoRequest request_;
    VxlanTunnel* source_vtep_ptr=NULL;
};
