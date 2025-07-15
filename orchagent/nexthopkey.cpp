extern "C"
{
#include <saitypes.h>
}

#include "nexthopkey.h"

std::size_t hash_value(const NextHopKey& obj) {
    std::size_t nh_hash = 0;

    boost::hash_combine(nh_hash, obj.ip_address.to_string());
    boost::hash_combine(nh_hash, obj.alias);
    boost::hash_combine(nh_hash, obj.vni);
    boost::hash_combine(nh_hash, obj.mac_address.to_string());
    boost::hash_combine(nh_hash, obj.label_stack.to_string());
    boost::hash_combine(nh_hash, obj.weight);
    boost::hash_combine(nh_hash, obj.srv6_segment);
    boost::hash_combine(nh_hash, obj.srv6_source);
    boost::hash_combine(nh_hash, obj.srv6_vpn_sid);

    return nh_hash;
}
