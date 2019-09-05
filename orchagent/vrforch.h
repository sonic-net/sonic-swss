#ifndef __VRFORCH_H
#define __VRFORCH_H

#include "request_parser.h"

extern sai_object_id_t gVirtualRouterId;

struct VrfEntry
{
    sai_object_id_t vrf_id;
    int             ref_count;
};

typedef std::unordered_map<std::string, VrfEntry> VRFTable;
typedef std::unordered_map<sai_object_id_t, std::string> VRFId2NameTable;

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
    VRFOrch(DBConnector *appDb, const std::string& appTableName, DBConnector *stateDb, const std::string& stateTableName) :
        Orch2(appDb, appTableName, request_),
        m_stateVrfTable(stateDb, stateTableName)
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

    string getVRFname(sai_object_id_t vrf_id) const
    {
        if (vrf_id == gVirtualRouterId)
        {
            return string("");
        }
        if (vrf_id_table_.find(vrf_id) != std::end(vrf_id_table_))
        {
            return vrf_id_table_.at(vrf_id);
        }
        else
        {
            return string("");
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

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VRFTable vrf_table_;
    VRFId2NameTable vrf_id_table_;
    VRFRequest request_;
    Table m_stateVrfTable;
};

#endif // __VRFORCH_H
