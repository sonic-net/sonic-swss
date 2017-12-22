#ifndef __VRFORCH_H
#define __VRFORCH_H

struct VRFEntry
{
    sai_object_id_t id;
    std::string name;
};

typedef std::unordered_map<std::string, sai_object_id_t> VRFTable;

class VRFRequest : public Request
{
    const request_description_t request_description = {
        1,
        { "string" },
        {
            { "v4",            REQ_T_BOOL },
            { "v6",            REQ_T_BOOL },
            { "src_mac",       REQ_T_MAC_ADDRESS },
            { "ttl_action",    REQ_T_PACKET_ACTION },
            { "ip_opt_action", REQ_T_PACKET_ACTION },
            { "l3_mc_action",  REQ_T_PACKET_ACTION },
        },
        { } // no mandatory attributes
    };
    VRFRequest(const KeyOpFieldsValuesTuple& request) : Request(request_description, request, '|') { }
};

class VRFOrch : public Orch
{
public:
    VRFOrch(DBConnector *db, const std::string& tableName);
    virtual void doTask(Consumer& consumer);
private:
    bool AddVRF(const VRFRequest& request);
    bool DeleteVRF(const VRFRequest& request);

    VRFTable vrf_table_;
};

#endif // __VRFORCH_H