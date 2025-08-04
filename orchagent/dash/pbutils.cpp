#include "pbutils.h"


using namespace std;
using namespace swss;
using namespace google::protobuf;

bool to_sai(const dash::types::IpVersion &pb_version, sai_ip_addr_family_t &sai_ip_family)
{
    switch (pb_version)
    {
    case dash::types::IP_VERSION_IPV4:
        sai_ip_family = SAI_IP_ADDR_FAMILY_IPV4;
        break;
    case dash::types::IP_VERSION_IPV6:
        sai_ip_family = SAI_IP_ADDR_FAMILY_IPV6;
        break;
    default:
        return false;
    }

    return true;
}

bool to_sai(const dash::types::IpAddress &pb_address, sai_ip_address_t &sai_address)
{
    SWSS_LOG_ENTER();

    if (pb_address.has_ipv4())
    {
        sai_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        sai_address.addr.ip4 = pb_address.ipv4();
    }
    else if (pb_address.has_ipv6())
    {
        sai_address.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(sai_address.addr.ip6, pb_address.ipv6().c_str(), sizeof(sai_address.addr.ip6));
    }
    else
    {
        SWSS_LOG_WARN("The protobuf IP address %s is invalid", pb_address.DebugString().c_str());
        return false;
    }

    return true;
}

bool to_sai(const dash::types::IpPrefix &pb_prefix, sai_ip_prefix_t &sai_prefix)
{
    SWSS_LOG_ENTER();

    if (pb_prefix.ip().has_ipv4() && pb_prefix.mask().has_ipv4())
    {
        sai_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        sai_prefix.addr.ip4 = pb_prefix.ip().ipv4();
        sai_prefix.mask.ip4 = pb_prefix.mask().ipv4();
    }
    else if (pb_prefix.ip().has_ipv6() && pb_prefix.mask().has_ipv6())
    {
        sai_prefix.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(sai_prefix.addr.ip6, pb_prefix.ip().ipv6().c_str(), sizeof(sai_prefix.addr.ip6));
        memcpy(sai_prefix.mask.ip6, pb_prefix.mask().ipv6().c_str(), sizeof(sai_prefix.mask.ip6));
    }
    else
    {
        SWSS_LOG_WARN("The protobuf IP prefix %s is invalid", pb_prefix.DebugString().c_str());
        return false;
    }

    return true;
}

bool to_sai(const RepeatedPtrField<dash::types::IpPrefix> &pb_prefixes, vector<sai_ip_prefix_t> &sai_prefixes)
{
    SWSS_LOG_ENTER();

    sai_prefixes.clear();
    sai_prefixes.reserve(pb_prefixes.size());

    for (auto &pb_prefix : pb_prefixes)
    {
        sai_ip_prefix_t sai_prefix;
        if (!to_sai(pb_prefix, sai_prefix))
        {
            sai_prefixes.clear();
            return false;
        }
        sai_prefixes.push_back(sai_prefix);
    }

    return true;
}

bool to_sai(const dash::types::ValueOrRange &pb_range, sai_u32_range_t &sai_range)
{
    SWSS_LOG_ENTER();

    if (pb_range.has_value())
    {
        sai_range.min = pb_range.value();
        sai_range.max = pb_range.value();
    }
    else if (pb_range.has_range())
    {
        if (pb_range.range().min() > pb_range.range().max())
        {
            SWSS_LOG_WARN("The range %s is invalid", pb_range.range().DebugString().c_str());
            return false;
        }
        sai_range.min = pb_range.range().min();
        sai_range.max = pb_range.range().max();
    }
    else
    {
        SWSS_LOG_WARN("The ValueOrRange %s is invalid", pb_range.DebugString().c_str());
        return false;
    }
    return true;
}

ip_addr_t to_swss(const dash::types::IpAddress &pb_address)
{
    SWSS_LOG_ENTER();

    ip_addr_t ip_address;
    if (pb_address.has_ipv4())
    {
        ip_address.family = AF_INET;
        ip_address.ip_addr.ipv4_addr = pb_address.ipv4();
    }
    else if (pb_address.has_ipv6())
    {
        ip_address.family = AF_INET6;
        memcpy(ip_address.ip_addr.ipv6_addr, pb_address.ipv6().c_str(), sizeof(ip_address.ip_addr.ipv6_addr));
    }
    else
    {
        SWSS_LOG_THROW("The protobuf IP address %s is invalid", pb_address.DebugString().c_str());
    }

    return ip_address;
}

std::string to_string(const dash::types::IpAddress &pb_address)
{
    SWSS_LOG_ENTER();

    return IpAddress(to_swss(pb_address)).to_string();
}

sai_uint16_t to_sai(const dash::types::HaRole ha_role)
{
    SWSS_LOG_ENTER();

    sai_dash_ha_role_t sai_ha_role = SAI_DASH_HA_ROLE_DEAD;

    switch (ha_role)
    {
        case dash::types::HA_ROLE_DEAD:
            sai_ha_role = SAI_DASH_HA_ROLE_DEAD;
            break;
        case dash::types::HA_ROLE_ACTIVE:
            sai_ha_role = SAI_DASH_HA_ROLE_ACTIVE;
            break;
        case dash::types::HA_ROLE_STANDBY:
            sai_ha_role = SAI_DASH_HA_ROLE_STANDBY;
            break;
        case dash::types::HA_ROLE_STANDALONE:
            sai_ha_role = SAI_DASH_HA_ROLE_STANDALONE;
            break;
        case dash::types::HA_ROLE_SWITCHING_TO_ACTIVE:
            sai_ha_role = SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE;
            break;
        default:
            SWSS_LOG_ERROR("Invalid HA Role %s", dash::types::HaRole_Name(ha_role).c_str());
    }

    return static_cast<sai_uint16_t>(sai_ha_role);
}

dash::types::HaRole to_pb(const sai_dash_ha_role_t ha_role)
{
    SWSS_LOG_ENTER();

    switch (ha_role)
    {
        case SAI_DASH_HA_ROLE_DEAD:
            return dash::types::HA_ROLE_DEAD;
        case SAI_DASH_HA_ROLE_ACTIVE:
            return dash::types::HA_ROLE_ACTIVE;
        case SAI_DASH_HA_ROLE_STANDBY:
            return dash::types::HA_ROLE_STANDBY;
        case SAI_DASH_HA_ROLE_STANDALONE:
            return dash::types::HA_ROLE_STANDALONE;
        case SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE:
            return dash::types::HA_ROLE_SWITCHING_TO_ACTIVE;
        default:
            return dash::types::HA_ROLE_DEAD;
    }
}

bool to_pb(const std::string &ha_role, dash::types::HaRole &pb_ha_role)
{
    SWSS_LOG_ENTER();

    if (ha_role == "dead")
    {
        pb_ha_role = dash::types::HA_ROLE_DEAD;
    }
    else if (ha_role == "active")
    {
        pb_ha_role = dash::types::HA_ROLE_ACTIVE;
    }
    else if (ha_role == "standby")
    {
        pb_ha_role = dash::types::HA_ROLE_STANDBY;
    }
    else if (ha_role == "standalone")
    {
        pb_ha_role = dash::types::HA_ROLE_STANDALONE;
    }
    else if (ha_role == "switching_to_active")
    {
        pb_ha_role = dash::types::HA_ROLE_SWITCHING_TO_ACTIVE;
    }
    else
    {
        SWSS_LOG_NOTICE("Unspecified HA Role %s, defaulting to dead", ha_role.c_str());
        pb_ha_role = dash::types::HA_ROLE_DEAD;
        return false;
    }

    return true;
}

bool to_pb(const std::string &ha_owner, dash::types::HaOwner &pb_ha_owner)
{
    SWSS_LOG_ENTER();

    if (ha_owner == "switch")
    {
        pb_ha_owner = dash::types::HA_OWNER_SWITCH;
    }
    else if (ha_owner == "dpu")
    {
        pb_ha_owner = dash::types::HA_OWNER_DPU;
    }
    else
    {
        SWSS_LOG_NOTICE("Unspecified HA Owner %s, defaulting to DPU", ha_owner.c_str());
        pb_ha_owner = dash::types::HA_OWNER_DPU;
        return false;
    }

    return true;
}

bool to_pb(const std::string &ha_scope, dash::types::HaScope &pb_ha_scope)
{
    SWSS_LOG_ENTER();

    if (ha_scope == "eni")
    {
        pb_ha_scope = dash::types::HA_SCOPE_ENI;
    }
    else if (ha_scope == "dpu")
    {
        pb_ha_scope = dash::types::HA_SCOPE_DPU;
    }
    else
    {
        SWSS_LOG_NOTICE("Unspecified HA Scope %s, defaulting to DPU", ha_scope.c_str());
        pb_ha_scope = dash::types::HA_SCOPE_DPU;
        return false;
    }

    return true;
}
