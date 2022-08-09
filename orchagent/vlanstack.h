struct VlanStackKey
{
    sai_object_id_t port_oid;
    uint16_t s_vlanid;

    VlanStackKey()
    {}

    VlanStackKey(sai_object_id_t port_oid,
                 uint16_t s_vlanid)
        : port_oid(port_oid)
        , s_vlanid(s_vlanid)
    {}

    bool operator<(const VlanStackKey& right) const
    {
        if (port_oid == right.port_oid)
        {
           return s_vlanid < right.s_vlanid;
        }
        else
        {
            return port_oid < right.port_oid;
        }
        return port_oid < right.port_oid;
    }
};

struct VlanStackEntry
{
    uint8_t priority;
    map<uint16_t, sai_object_id_t> push_entries;

    sai_object_id_t pop_entry;
};

struct VlanStack
{
    std::map<VlanStackKey, VlanStackEntry> vlan_stack_map;
};

struct VlanXlateKey
{
    sai_object_id_t port_oid;
    uint16_t s_vlanid;

    VlanXlateKey()
    {}

    VlanXlateKey(sai_object_id_t port_oid,
        uint16_t s_vlanid)
        : port_oid(port_oid)
        , s_vlanid(s_vlanid)
    {}

    bool operator<(const VlanXlateKey& right) const
    {
        if (port_oid == right.port_oid)
        {
            return s_vlanid < right.s_vlanid;
        }
        else
        {
            return port_oid < right.port_oid;
        }
        return port_oid < right.port_oid;
    }
};

struct VlanXlateEntry
{
    uint16_t c_vlanid;
    sai_object_id_t ingress_entry;
    sai_object_id_t egress_entry;

    VlanXlateEntry() {}

    VlanXlateEntry(uint16_t c_vlanid,
        sai_object_id_t ingress_entry,
        sai_object_id_t egress_entry)
        : c_vlanid(c_vlanid)
        , ingress_entry(ingress_entry)
        , egress_entry(egress_entry)
    {}
};

struct VlanXlate
{
    std::map<VlanXlateKey, VlanXlateEntry> vlan_xlate_map;
};
