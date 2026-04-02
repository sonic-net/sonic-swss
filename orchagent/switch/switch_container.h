#pragma once

extern "C" {
#include <saiswitch.h>
#include <saihash.h>
}

#include <unordered_map>
#include <set>
#include <string>

enum class EcmpType
{
    ECMP_STATIC,
    ECMP_CONSISTENT,
    ECMP_RESILIENT
};

class SwitchHash final
{
public:
    SwitchHash() = default;
    ~SwitchHash() = default;

    struct {
        std::set<sai_native_hash_field_t> value;
        bool is_set = false;
    } ecmp_hash;

    struct {
        std::set<sai_native_hash_field_t> value;
        bool is_set = false;
    } lag_hash;

    struct {
        sai_hash_algorithm_t value;
        bool is_set = false;
    } ecmp_hash_algorithm;

    struct {
        sai_hash_algorithm_t value;
        bool is_set = false;
    } lag_hash_algorithm;

    struct {
        uint32_t value = 0;
        bool is_set = false;
    } ecmp_hash_seed;

    struct {
        EcmpType value = EcmpType::ECMP_STATIC;
        bool is_set = false;
    } ecmp_type;

    struct {
        uint32_t value = 0;
        bool is_set = false;
    } ecmp_resilient_hash_buckets;

    struct {
        uint32_t value = 0;
        bool is_set = false;
    } ecmp_resilient_active_flow_timer;

    struct {
        uint32_t value = 0;
        bool is_set = false;
    } ecmp_resilient_max_unbalanced_time;

    std::unordered_map<std::string, std::string> fieldValueMap;
};
