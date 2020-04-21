#ifndef __VRFORCH_H
#define __VRFORCH_H

#include "request_parser.h"

extern sai_object_id_t gVirtualRouterId;

struct VrfEntry
{
    sai_object_id_t vrf_id;
    int             ref_count;
};

struct L3VNIEntry
{
    uint16_t vlan_id;
    bool     l3_vni;
};

typedef std::unordered_map<std::string, VrfEntry> VRFTable;
typedef std::unordered_map<sai_object_id_t, std::string> VRFIdNameTable;
typedef std::unordered_map<std::string, uint32_t> VRFNameVNIMapTable;
typedef std::unordered_map<uint32_t, L3VNIEntry> L3VNITable;

const request_description_t request_description = {
    { REQ_T_STRING },
    {
        { "v4",            REQ_T_BOOL },
        { "v6",            REQ_T_BOOL },
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "ttl_action",    REQ_T_PACKET_ACTION },
        { "ip_opt_action", REQ_T_PACKET_ACTION },
        { "l3_mc_action",  REQ_T_PACKET_ACTION },
        { "fallback",      REQ_T_BOOL },
        { "vni",           REQ_T_UINT }
    },
    { } // no mandatory attributes
};

class VRFRequest : public Request
{
public:
    VRFRequest() : Request(request_description, ':') { }
};


class VRFOrch : public Orch2
{
public:
    VRFOrch(swss::DBConnector *appDb, const std::string& appTableName, swss::DBConnector *stateDb, const std::string& stateTableName) :
        Orch2(appDb, appTableName, request_),
        m_stateVrfObjectTable(stateDb, stateTableName)
    {
    }

    bool isVRFexists(const std::string& name) const
    {
        return vrf_table_.find(name) != std::end(vrf_table_);
    }

    sai_object_id_t getVRFid(const std::string& name) const
    {
        if (vrf_table_.find(name) != std::end(vrf_table_))
        {
            return vrf_table_.at(name).vrf_id;
        }
        else
        {
            return gVirtualRouterId;
        }
    }

    std::string getVRFname(sai_object_id_t vrf_id) const
    {
        if (vrf_id == gVirtualRouterId)
        {
            return std::string("");
        }
        if (vrf_id_table_.find(vrf_id) != std::end(vrf_id_table_))
        {
            return vrf_id_table_.at(vrf_id);
        }
        else
        {
            return std::string("");
        }
    }

    void increaseVrfRefCount(const std::string& name)
    {
        if (vrf_table_.find(name) != std::end(vrf_table_))
        {
            vrf_table_.at(name).ref_count++;
        }
    }

    void increaseVrfRefCount(sai_object_id_t vrf_id)
    {
        if (vrf_id != gVirtualRouterId)
        {
            increaseVrfRefCount(getVRFname(vrf_id));
        }
    }

    void decreaseVrfRefCount(const std::string& name)
    {
        if (vrf_table_.find(name) != std::end(vrf_table_))
        {
            vrf_table_.at(name).ref_count--;
        }
    }

    void decreaseVrfRefCount(sai_object_id_t vrf_id)
    {
        if (vrf_id != gVirtualRouterId)
        {
            decreaseVrfRefCount(getVRFname(vrf_id));
        }
    }

    int getVrfRefCount(const std::string& name)
    {
        if (vrf_table_.find(name) != std::end(vrf_table_))
            return vrf_table_.at(name).ref_count;
        else
            return -1;
    }

    uint32_t getVRFmappedVNI(const std::string& vrf_name) const
    {
        if (vrf_vni_map_table_.find(vrf_name) != std::end(vrf_vni_map_table_))
        {
            return vrf_vni_map_table_.at(vrf_name);
        }
        else
        {
            return 0;
        }
    }

    int getL3VniVlan(const uint32_t vni) const
    {
        if (l3vni_table_.find(vni) != std::end(l3vni_table_))
            return l3vni_table_.at(vni).vlan_id;
        else
            return (-1);
    }
    int updateL3VniVlan(uint32_t vni, uint16_t vlan_id);
private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);
    bool updateVrfVNIMap(const std::string& vrf_name, uint32_t vni);
    bool delVrfVNIMap(const std::string& vrf_name, uint32_t vni);

    VRFTable vrf_table_;
    VRFIdNameTable vrf_id_table_;
    VRFRequest request_;
    VRFNameVNIMapTable vrf_vni_map_table_;
    swss::Table m_stateVrfObjectTable;
    L3VNITable l3vni_table_;
};

#endif // __VRFORCH_H
