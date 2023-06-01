#pragma once

extern "C" {
#include <saihash.h>
}

#include <unordered_map>
#include <set>
#include <string>

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

    std::unordered_map<std::string, std::string> fieldValueMap;
};
