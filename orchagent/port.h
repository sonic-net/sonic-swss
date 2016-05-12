#ifndef SWSS_PORT_H
#define SWSS_PORT_H

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include <set>
#include <string>

namespace swss {

class Port
{
public:
    enum Type {
        MGMT,
        PHY_PORT,
        LOOPBACK,
        VLAN,
        LAG,
        UNKNOWN
    } ;

    Port() {};
    Port(std::string alias, Type type) :
            m_alias(alias), m_type(type) {};

    bool getQueue(size_t queue_ind, sai_object_id_t &queue_id);
    bool getPG(size_t pg_ind, sai_object_id_t &pg);
    
    inline bool operator<(const Port &o) const
    {
        return m_alias< o.m_alias;
    }

    inline bool operator==(const Port &o) const
    {
        return m_alias == o.m_alias;
    }

    inline bool operator!=(const Port &o) const
    {
        return !(*this == o);
    }

    std::string         m_alias;
    Type                m_type;
    int                 m_index;        // PHY_PORT: index
    int                 m_ifindex;
    sai_object_id_t     m_port_id;
    sai_vlan_id_t       m_vlan_id;
    sai_object_id_t     m_vlan_member_id;
    sai_object_id_t     m_rif_id;
    sai_object_id_t     m_hif_id;
    sai_object_id_t     m_lag_id;
};

}

#endif /* SWSS_PORT_H */
