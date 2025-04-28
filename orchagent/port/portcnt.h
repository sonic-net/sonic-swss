#pragma once

extern "C" {
#include <saiport.h>
#include <saibridge.h>
}

#include <cstdbool>
#include <cstdint>

#include <unordered_map>
#include <set>
#include <vector>
#include <string>

#include "../port.h"

class PortConfig final
{
public:
    PortConfig() = default;
    ~PortConfig() = default;

    PortConfig(const std::string &key, const std::string &op) noexcept
    {
        this->key = key;
        this->op = op;
    }

    struct {
        std::string value;
        bool is_set = false;
    } alias; // Port alias

    struct {
        std::uint16_t value;
        bool is_set = false;
    } index; // Interface index

    struct {
        std::set<std::uint32_t> value;
        bool is_set = false;
    } lanes; // Lane information of a physical port

    struct {
        std::uint32_t value;
        bool is_set = false;
    } speed; // Port speed

    struct {
        bool value;
        bool is_set = false;
    } autoneg; // Port autoneg

    struct {
        std::set<std::uint32_t> value;
        bool is_set = false;
    } adv_speeds; // Port advertised speeds

    struct {
        sai_port_interface_type_t value;
        bool is_set = false;
    } interface_type; // Port interface type

    struct {
        std::set<sai_port_interface_type_t> value;
        bool is_set = false;
    } adv_interface_types; // Port advertised interface types

    struct {
        sai_port_fec_mode_t value;
        bool is_set = false;
        bool override_fec = false;
    } fec; // Port FEC

    struct {
        std::uint32_t value;
        bool is_set = false;
    } mtu; // Port MTU

    struct {
        std::uint32_t value;
        bool is_set = false;
    } dhcp_rate_limit; // Port dhcp_rate_limit

    struct {
        std::uint16_t value;
        bool is_set = false;
    } tpid; // Port TPID

    struct {
        sai_port_priority_flow_control_mode_t value;
        bool is_set = false;
    } pfc_asym; // Port asymmetric PFC

    struct {
        sai_bridge_port_fdb_learning_mode_t value;
        bool is_set = false;
    } learn_mode; // Port FDB learn mode

    struct {
        bool value;
        bool is_set = false;
    } link_training; // Port link training

    struct {

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } preemphasis; // Port serdes pre-emphasis

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } idriver; // Port serdes idriver

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } ipredriver; // Port serdes ipredriver

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } pre1; // Port serdes pre1

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } pre2; // Port serdes pre2

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } pre3; // Port serdes pre3

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } main; // Port serdes main

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } post1; // Port serdes post1

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } post2; // Port serdes post2

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } post3; // Port serdes post3

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } attn; // Port serdes attn

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } ob_m2lp; // Port serdes ob_m2lp

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } ob_alev_out; // Port serdes ob_alev_out

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } obplev; // Port serdes obplev

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } obnlev; // Port serdes obnlev

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } regn_bfm1p; // Port serdes regn_bfm1p

        struct {
            std::vector<std::uint32_t> value;
            bool is_set = false;
        } regn_bfm1n; // Port serdes regn_bfm1n

        struct {
            bool value;
            bool is_set = false;
        } unreliable_los; // Port unreliable_los

    } serdes; // Port serdes

    struct {
        swss::Port::Role value;
        bool is_set = false;
    } role; // Port role

    struct {
        bool value;
        bool is_set = false;
    } admin_status; // Port admin status

    struct {
        std::string value;
        bool is_set = false;
    } description; // Port description

    struct {
        std::string value;
        bool is_set = false;
    } subport; // Port subport

    struct {
        std::uint16_t value;
        bool is_set = false;
    } pt_intf_id; // Port interface ID for Path Tracing

    struct {
        sai_port_path_tracing_timestamp_type_t value;
        bool is_set = false;
    } pt_timestamp_template; // Port timestamp template for Path Tracing

    struct {
        sai_redis_link_event_damping_algorithm_t value;
        bool is_set = false;
    } link_event_damping_algorithm; // Port link event damping algorithm

    struct {

        struct {
            uint32_t value;
            bool is_set = false;
        } max_suppress_time; // Max suppress time

        struct {
            uint32_t value;
            bool is_set = false;
        } decay_half_life; // Decay half life

        struct {
            uint32_t value;
            bool is_set = false;
        } suppress_threshold; // Suppress threshold

        struct {
            uint32_t value;
            bool is_set = false;
        } reuse_threshold; // Reuse threshold

        struct {
            uint32_t value;
            bool is_set = false;
        } flap_penalty; // Flap penalty

    } link_event_damping_config; // Port link event damping config

    std::string key;
    std::string op;

    std::unordered_map<std::string, std::string> fieldValueMap;
};
