#pragma once

extern "C" {
#include <saiswitch.h>
#include <saihash.h>
}

#include <unordered_map>
#include <set>
#include <string>

#include "switch_types.h"

class SwitchHash final
{
public:
    SwitchHash() = default;
    ~SwitchHash() = default;

    enum class HashFieldState {
        NOT_SET,        // Attribute not configured
        SET,            // Attribute has values (add/update)
        DELETE_ALL      // Remove entire attribute and associated SAI OID
    };

    struct HashField {
        std::set<sai_native_hash_field_t> value;
        HashFieldState state = HashFieldState::NOT_SET;

        void markForDeletion() {
            value.clear();
            state = HashFieldState::DELETE_ALL;
        }

        void setValue(const std::set<sai_native_hash_field_t>& val) {
            if (val.empty()) {
                markForDeletion();
            } else {
                value = val;
                state = HashFieldState::SET;
            }
        }

        bool isSet() const {
            return state == HashFieldState::SET;
        }

        bool isDeleteAll() const {
            return state == HashFieldState::DELETE_ALL;
        }

        bool isNotSet() const {
            return state == HashFieldState::NOT_SET;
        }
    };

    struct {
        std::set<sai_native_hash_field_t> value;
        bool is_set = false;
    } ecmp_hash;

    // Packet-type specific hash fields
    std::map<HashPktType, HashField> ecmp_hash_pkt_type;
    std::map<HashPktType, HashField> lag_hash_pkt_type;


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

    std::unordered_map<std::string, std::string> fieldValueMap;
};
