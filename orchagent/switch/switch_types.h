#pragma once

#include <string>
#include <map>
#include <utility>
#include <algorithm>
#include <cctype>

// Packet type enum for hash configuration
enum class HashPktType {
    IPV4,
    IPV6,
    IPNIP,
    IPV4_RDMA,
    IPV6_RDMA
};

// Helper function to convert HashPktType to string
inline std::string hashPktTypeToString(HashPktType type) {
    switch (type) {
        case HashPktType::IPV4: return "ipv4";
        case HashPktType::IPV6: return "ipv6";
        case HashPktType::IPNIP: return "ipnip";
        case HashPktType::IPV4_RDMA: return "ipv4_rdma";
        case HashPktType::IPV6_RDMA: return "ipv6_rdma";
        default: return "unknown";
    }
}

// Helper function to validate HashPktType
inline bool isValidHashPktType(HashPktType type)
{
    switch (type)
    {
        case HashPktType::IPV4:
        case HashPktType::IPV6:
        case HashPktType::IPNIP:
        case HashPktType::IPV4_RDMA:
        case HashPktType::IPV6_RDMA:
            return true;
        default:
            return false;
    }
}

// Helper function to extract packet type from field name
inline bool extractHashPktType(const std::string &fieldName, HashPktType &pktType, bool &isEcmp) {
    static const std::map<std::string, std::pair<HashPktType, bool>> fieldToPktTypeMap = {
        {"ecmp_hash_ipv4", {HashPktType::IPV4, true}},
        {"ecmp_hash_ipv6", {HashPktType::IPV6, true}},
        {"ecmp_hash_ipnip", {HashPktType::IPNIP, true}},
        {"ecmp_hash_ipv4_rdma", {HashPktType::IPV4_RDMA, true}},
        {"ecmp_hash_ipv6_rdma", {HashPktType::IPV6_RDMA, true}},
        {"lag_hash_ipv4", {HashPktType::IPV4, false}},
        {"lag_hash_ipv6", {HashPktType::IPV6, false}},
        {"lag_hash_ipnip", {HashPktType::IPNIP, false}},
        {"lag_hash_ipv4_rdma", {HashPktType::IPV4_RDMA, false}},
        {"lag_hash_ipv6_rdma", {HashPktType::IPV6_RDMA, false}}
    };

    // Normalize field name to match keys:
    // - case-insensitive
    // - treat '-' and '_' equivalently in the suffix
    std::string key = fieldName;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(key.begin(), key.end(), '-', '_');

    auto it = fieldToPktTypeMap.find(key);
    if (it != fieldToPktTypeMap.end()) {
        pktType = it->second.first;
        isEcmp = it->second.second;
        return true;
    }
    return false;
}
