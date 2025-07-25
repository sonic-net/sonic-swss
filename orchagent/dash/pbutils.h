#pragma once

#include <vector>
#include <limits>

#include <saitypes.h>
#include <sai.h>
#include <logger.h>
#include <ipaddress.h>

#include "dash_api/types.pb.h"

bool to_sai(const dash::types::IpVersion &pb_version, sai_ip_addr_family_t &sai_ip_family);

bool to_sai(const dash::types::IpAddress &pb_address, sai_ip_address_t &sai_address);

bool to_sai(const dash::types::IpPrefix &pb_prefix, sai_ip_prefix_t &sai_prefix);

bool to_sai(const google::protobuf::RepeatedPtrField<dash::types::IpPrefix> &pb_prefixes, std::vector<sai_ip_prefix_t> &sai_prefixes);

bool to_sai(const dash::types::ValueOrRange &pb_range, sai_u32_range_t &sai_range);

template<typename RangeType>
bool to_sai(const dash::types::ValueOrRange &pb_range, RangeType &sai_range)
{
    SWSS_LOG_ENTER();

    using range_type = typename std::conditional<std::is_same<RangeType, sai_u32_range_t>::value, uint32_t,
                        typename std::conditional<std::is_same<RangeType, sai_s32_range_t>::value, int32_t,
                            typename std::conditional<std::is_same<RangeType, sai_u16_range_t>::value, uint16_t,
                            void>::type>::type>::type;

    if (pb_range.has_range())
    {
        if (pb_range.range().min() > pb_range.range().max() || pb_range.range().min() < std::numeric_limits<range_type>::min() || pb_range.range().max() > std::numeric_limits<range_type>::max())
        {
            SWSS_LOG_WARN("The range %s is invalid", pb_range.range().DebugString().c_str());
            return false;
        }
        sai_range.min = static_cast<range_type>(pb_range.range().min());
        sai_range.max = static_cast<range_type>(pb_range.range().max());
    }
    else
    {
        if (pb_range.value() < std::numeric_limits<range_type>::min() || pb_range.value() > std::numeric_limits<range_type>::max())
        {
            SWSS_LOG_WARN("The value %s is invalid", std::to_string(pb_range.value()).c_str());
            return false;
        }
        sai_range.min = static_cast<range_type>(pb_range.value());
        sai_range.max = static_cast<range_type>(pb_range.value());
    }

    return true;
}

template<typename RangeType>
bool to_sai(const google::protobuf::RepeatedPtrField<dash::types::ValueOrRange> &pb_ranges, std::vector<RangeType> &sai_ranges)
{
    SWSS_LOG_ENTER();

    sai_ranges.clear();
    sai_ranges.reserve(pb_ranges.size());

    for (auto &pb_range: pb_ranges)
    {
        RangeType sai_range;
        if (!to_sai(pb_range, sai_range))
        {
            sai_ranges.clear();
            return false;
        }
        sai_ranges.push_back(sai_range);
    }

    return true;
}

swss::ip_addr_t to_swss(const dash::types::IpAddress &pb_address);

std::string to_string(const dash::types::IpAddress &pb_address);

sai_uint16_t to_sai(const dash::types::HaRole ha_role);

dash::types::HaRole to_pb(const sai_dash_ha_role_t ha_role);

bool to_pb(const std::string &ha_role, dash::types::HaRole &pb_ha_role);

bool to_pb(const std::string &ha_owner, dash::types::HaOwner &pb_ha_owner);

bool to_pb(const std::string &ha_scope, dash::types::HaScope &pb_ha_scope);
