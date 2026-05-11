/*
 * PRBS (Pseudo-Random Bit Sequence) Handler
 *
 * Helper class for PRBS configuration and result caching for per-lane diagnostics
 */

#include "prbshandler.h"
#include "logger.h"
#include "schema.h"
#include "converter.h"
#include "sai_serialize.h"
#include "stringutility.h"
#include <time.h>

extern sai_port_api_t *sai_port_api;
extern sai_object_id_t gSwitchId;

using namespace std;
using namespace swss;

// Maximum number of lanes per port
// Covers 400G/800G ports with up to 8 serdes lanes
// Using 16 as safe upper bound for vendor implementations
constexpr uint32_t MAX_PRBS_LANES = 16;

static const map<string, sai_port_prbs_pattern_t> prbs_pattern_map = {
    {"none",    SAI_PORT_PRBS_PATTERN_AUTO},
    {"PRBS7",   SAI_PORT_PRBS_PATTERN_PRBS7},
    {"PRBS9",   SAI_PORT_PRBS_PATTERN_PRBS9},
    {"PRBS10",  SAI_PORT_PRBS_PATTERN_PRBS10},
    {"PRBS11",  SAI_PORT_PRBS_PATTERN_PRBS11},
    {"PRBS13",  SAI_PORT_PRBS_PATTERN_PRBS13},
    {"PRBS15",  SAI_PORT_PRBS_PATTERN_PRBS15},
    {"PRBS16",  SAI_PORT_PRBS_PATTERN_PRBS16},
    {"PRBS20",  SAI_PORT_PRBS_PATTERN_PRBS20},
    {"PRBS23",  SAI_PORT_PRBS_PATTERN_PRBS23},
    {"PRBS31",  SAI_PORT_PRBS_PATTERN_PRBS31},
    {"PRBS32",  SAI_PORT_PRBS_PATTERN_PRBS32},
    {"PRBS49",  SAI_PORT_PRBS_PATTERN_PRBS49},
    {"PRBS58",  SAI_PORT_PRBS_PATTERN_PRBS58},
    {"PRBS7Q",  SAI_PORT_PRBS_PATTERN_PRBS7Q},
    {"PRBS9Q",  SAI_PORT_PRBS_PATTERN_PRBS9Q},
    {"PRBS13Q", SAI_PORT_PRBS_PATTERN_PRBS13Q},
    {"PRBS15Q", SAI_PORT_PRBS_PATTERN_PRBS15Q},
    {"PRBS23Q", SAI_PORT_PRBS_PATTERN_PRBS23Q},
    {"PRBS31Q", SAI_PORT_PRBS_PATTERN_PRBS31Q},
    {"SSPRQ",   SAI_PORT_PRBS_PATTERN_SSPRQ}
};

// PRBS mode mapping (string to SAI enum)
static const map<string, sai_port_prbs_config_t> prbs_mode_map = {
    {"both", SAI_PORT_PRBS_CONFIG_ENABLE_TX_RX},
    {"rx", SAI_PORT_PRBS_CONFIG_ENABLE_RX},
    {"tx", SAI_PORT_PRBS_CONFIG_ENABLE_TX},
    {"disabled", SAI_PORT_PRBS_CONFIG_DISABLE}
};

PrbsHandler::PrbsHandler(swss::Table &stateTable, swss::Table &laneResultTable, swss::Table &resultsTable) :
    m_stateTable(stateTable),
    m_laneResultTable(laneResultTable),
    m_resultsTable(resultsTable)
{

}

// RX status enum to string
static const map<sai_port_prbs_rx_status_t, string> prbs_rx_status_str = {
    {SAI_PORT_PRBS_RX_STATUS_OK, "OK"},
    {SAI_PORT_PRBS_RX_STATUS_LOCK_WITH_ERRORS, "LOCK_WITH_ERRORS"},
    {SAI_PORT_PRBS_RX_STATUS_NOT_LOCKED, "NOT_LOCKED"},
    {SAI_PORT_PRBS_RX_STATUS_LOST_LOCK, "LOST_LOCK"}
};

bool PrbsHandler::handlePrbsPatternConfig(swss::Port &port, const string &prbs_pattern_str)
{
    SWSS_LOG_ENTER();

    string alias = port.m_alias;
    sai_object_id_t port_id = port.m_port_id;

    SWSS_LOG_INFO("Handling PRBS pattern config for port %s: pattern=%s",
                  alias.c_str(), prbs_pattern_str.c_str());

    if (prbs_pattern_str.empty())
    {
        SWSS_LOG_WARN("Empty PRBS pattern string for port %s, skipping", alias.c_str());
        return true;
    }

    sai_attribute_t attr = {};

    auto pattern_it = prbs_pattern_map.find(prbs_pattern_str);
    if (pattern_it == prbs_pattern_map.end())
    {
        SWSS_LOG_ERROR("Invalid PRBS pattern: %s for port %s",
                       prbs_pattern_str.c_str(), alias.c_str());
        return false;
    }
    attr.id = SAI_PORT_ATTR_PRBS_PATTERN;
    attr.value.s32 = pattern_it->second;

    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PRBS pattern %s on port %s: %s",
                       prbs_pattern_str.c_str(), alias.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Set PRBS pattern to %s on port %s",
                    prbs_pattern_str.c_str(), alias.c_str());

    return true;
}

bool PrbsHandler::handlePrbsConfig(swss::Port &port, const string &prbs_mode_str)
{
    SWSS_LOG_ENTER();

    string alias = port.m_alias;

    SWSS_LOG_INFO("Handling PRBS mode config for port %s: mode=%s",
                  alias.c_str(), prbs_mode_str.c_str());

    // Parse PRBS mode
    auto mode_it = prbs_mode_map.find(prbs_mode_str);
    if (mode_it == prbs_mode_map.end())
    {
        SWSS_LOG_ERROR("Invalid PRBS mode: %s for port %s", prbs_mode_str.c_str(), alias.c_str());
        return false;
    }
    sai_port_prbs_config_t prbs_mode = mode_it->second;

    // Check if this is a disable operation
    if (prbs_mode == SAI_PORT_PRBS_CONFIG_DISABLE)
    {
        return handlePrbsDisable(port);
    }

    // Enable PRBS with current pattern (already set via handlePrbsPatternConfig)
    sai_object_id_t port_id = port.m_port_id;
    sai_attribute_t attr = {};
    sai_status_t status;

    // Clear old PRBS results before starting new test
    clearPrbsResults(alias);
    SWSS_LOG_INFO("Cleared old PRBS results for port %s before starting new test", alias.c_str());

    // Set PRBS config mode
    attr.id = SAI_PORT_ATTR_PRBS_CONFIG;
    attr.value.s32 = prbs_mode;

    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PRBS config mode on port %s: %s",
                       alias.c_str(), sai_serialize_status(status).c_str());
        vector<FieldValueTuple> fail_fvs;
        fail_fvs.emplace_back("status", "failed");
        m_stateTable.set(alias, fail_fvs);
        return false;
    }

    SWSS_LOG_NOTICE("Enabled PRBS on port %s (mode=%s)", alias.c_str(), prbs_mode_str.c_str());

    // Query actual pattern being used
    string actual_pattern_str = "unknown";
    attr.id = SAI_PORT_ATTR_PRBS_PATTERN;
    status = sai_port_api->get_port_attribute(port_id, 1, &attr);
    if (status == SAI_STATUS_SUCCESS)
    {
        for (const auto &kv : prbs_pattern_map)
        {
            if (kv.second == static_cast<sai_port_prbs_pattern_t>(attr.value.s32))
            {
                actual_pattern_str = (kv.first == "none") ? "AUTO" : kv.first;
                break;
            }
        }
    }

    vector<FieldValueTuple> fvs;
    fvs.emplace_back("status", "running");
    fvs.emplace_back("mode", prbs_mode_str);
    fvs.emplace_back("pattern", actual_pattern_str);

    time_t now = time(nullptr);
    fvs.emplace_back("start_time", to_string(now));

    m_stateTable.set(alias, fvs);

    SWSS_LOG_INFO("Created PRBS test record in STATE_DB for port %s", alias.c_str());

    return true;
}

bool PrbsHandler::handlePrbsDisable(swss::Port &port)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port_id = port.m_port_id;
    string alias = port.m_alias;
    sai_status_t status;
    sai_attribute_t attr = {};

    // Disable PRBS first
    attr.id = SAI_PORT_ATTR_PRBS_CONFIG;
    attr.value.s32 = SAI_PORT_PRBS_CONFIG_DISABLE;

    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to disable PRBS on port %s: %s",
                       alias.c_str(), sai_serialize_status(status).c_str());

        // Update test status to errored
        vector<FieldValueTuple> fvs;
        fvs.emplace_back("status", "errored");

        m_stateTable.set(alias, fvs);

        return false;
    }

    SWSS_LOG_NOTICE("Disabled PRBS on port %s", alias.c_str());

    // Query PRBS results after successful disable
    if (!capturePrbsResults(port))
    {
        SWSS_LOG_WARN("Failed to capture PRBS results for port %s", alias.c_str());
        // Continue even if capture fails - disable was successful
    }

    // Update test status to stopped
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("status", "stopped");

    time_t now = time(nullptr);
    fvs.emplace_back("stop_time", to_string(now));

    m_stateTable.set(alias, fvs);

    return true;
}

bool PrbsHandler::capturePrbsResults(swss::Port &port)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port_id = port.m_port_id;
    string alias = port.m_alias;
    sai_status_t status;

    vector<FieldValueTuple> test_fvs;
    if (!m_stateTable.get(alias, test_fvs))
    {
        SWSS_LOG_WARN("No PRBS test data found for port %s", alias.c_str());
        return false;
    }

    // Skip RX capture for TX-only mode (mode stored in PORT_PRBS_TEST)
    for (const auto &fv : test_fvs)
    {
        if (fvField(fv) == "mode" && fvValue(fv) == "tx")
        {
            return true;
        }
    }

    // Try per-lane PRBS first (SAI_PORT_ATTR_PRBS_PER_LANE_RX_STATE_LIST)
    sai_attribute_t rx_state_attr = {};
    uint32_t lane_count = 0;
    rx_state_attr.id = SAI_PORT_ATTR_PRBS_PER_LANE_RX_STATE_LIST;
    sai_prbs_per_lane_rx_state_t rx_states[MAX_PRBS_LANES] = {};
    rx_state_attr.value.prbs_rx_state_list.count = MAX_PRBS_LANES;
    rx_state_attr.value.prbs_rx_state_list.list = rx_states;

    status = sai_port_api->get_port_attribute(port_id, 1, &rx_state_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        // Per-lane not supported: use per-port SAI_PORT_ATTR_PRBS_RX_STATE and populate PORT_PRBS_RESULTS
        SWSS_LOG_WARN("Failed to get PRBS RX state (per-lane) for port %s: %s",
            alias.c_str(), sai_serialize_status(status).c_str());
    }
    else
    {
        lane_count = rx_state_attr.value.prbs_rx_state_list.count;

        vector<FieldValueTuple> result_fvs;
        result_fvs.emplace_back("total_lanes", to_string(lane_count));
        m_resultsTable.set(alias, result_fvs);

        // Query SAI_PORT_ATTR_PRBS_PER_LANE_BER_LIST (BER metrics)
        // Use lane_count from per-lane RX state query so buffer matches the port's actual lane count
        sai_attribute_t ber_attr = {};
        ber_attr.id = SAI_PORT_ATTR_PRBS_PER_LANE_BER_LIST;
        sai_prbs_per_lane_bit_error_rate_t ber_list[MAX_PRBS_LANES] = {};
        ber_attr.value.prbs_ber_list.count = lane_count;
        ber_attr.value.prbs_ber_list.list = ber_list;

        status = sai_port_api->get_port_attribute(port_id, 1, &ber_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Failed to get PRBS BER list for port %s: %s",
                        alias.c_str(), sai_serialize_status(status).c_str());
        }
        else
        {
            for (uint32_t i = 0; i < lane_count; i++)
            {
                auto rx_status_it = prbs_rx_status_str.find(rx_states[i].rx_state.rx_status);
                string rx_status_str = (rx_status_it != prbs_rx_status_str.end()) ?
                                    rx_status_it->second : "UNKNOWN";

                vector<FieldValueTuple> lane_fvs;
                lane_fvs.emplace_back("rx_status", rx_status_str);
                lane_fvs.emplace_back("error_count", to_string(rx_states[i].rx_state.error_count));

                if (status == SAI_STATUS_SUCCESS && i < ber_attr.value.prbs_ber_list.count)
                {
                    lane_fvs.emplace_back("ber_mantissa", to_string(ber_list[i].ber.mantissa));
                    lane_fvs.emplace_back("ber_exponent", to_string(ber_list[i].ber.exponent));
                }

                string lane_key = alias + "|" + to_string(i);
                m_laneResultTable.set(lane_key, lane_fvs);
            }
        }
    }

    sai_attribute_t port_rx_attr = {};
    port_rx_attr.id = SAI_PORT_ATTR_PRBS_RX_STATE;
    status = sai_port_api->get_port_attribute(port_id, 1, &port_rx_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to get PRBS RX state (per-port) for port %s: %s",
                    alias.c_str(), sai_serialize_status(status).c_str());
    }
    else
    {
        auto rx_status_it = prbs_rx_status_str.find(port_rx_attr.value.rx_state.rx_status);
        string rx_status_str = (rx_status_it != prbs_rx_status_str.end()) ?
                                rx_status_it->second : "UNKNOWN";

        vector<FieldValueTuple> result_fvs;
        result_fvs.emplace_back("rx_status", rx_status_str);
        result_fvs.emplace_back("error_count", to_string(port_rx_attr.value.rx_state.error_count));
        m_resultsTable.set(alias, result_fvs);
    }

    return true;
}

void PrbsHandler::clearPrbsResults(const string &port_alias)
{
    SWSS_LOG_ENTER();

    m_stateTable.del(port_alias);
    m_resultsTable.del(port_alias);

    for (uint32_t i = 0; i < MAX_PRBS_LANES; i++)
    {
        string lane_key = port_alias + "|" + to_string(i);
        m_laneResultTable.del(lane_key);
    }

    SWSS_LOG_INFO("Cleared PRBS test results for port %s (test, results, and per-lane data)", port_alias.c_str());
}

void PrbsHandler::removeAllPrbsEntries()
{
    SWSS_LOG_ENTER();

    // Get all PORT_PRBS_TEST entries from STATE_DB
    std::vector<std::string> keys;
    m_stateTable.getKeys(keys);

    // Remove all PRBS entries
    for (const auto& key : keys)
    {
        SWSS_LOG_NOTICE("Removing PRBS entries for port %s", key.c_str());
        clearPrbsResults(key);
    }
}

void PrbsHandler::getPortSupportedPrbsPatterns(const string &alias, sai_object_id_t port_id,
                                                PortSupportedPrbsPatterns &supported_patterns)
{
    supported_patterns.clear();

    sai_attribute_t attr;
    vector<int32_t> pattern_list(prbs_pattern_map.size());
    attr.id = SAI_PORT_ATTR_SUPPORTED_PRBS_PATTERN;
    attr.value.s32list.count = static_cast<uint32_t>(pattern_list.size());
    attr.value.s32list.list = pattern_list.data();

    sai_status_t status = sai_port_api->get_port_attribute(port_id, 1, &attr);
    if (status == SAI_STATUS_BUFFER_OVERFLOW)
    {
        uint32_t needed = attr.value.s32list.count;
        pattern_list.resize(needed);
        attr.value.s32list.count = needed;
        attr.value.s32list.list = pattern_list.data();
        status = sai_port_api->get_port_attribute(port_id, 1, &attr);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
            SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status) ||
            status == SAI_STATUS_NOT_SUPPORTED ||
            status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            SWSS_LOG_NOTICE("SAI_PORT_ATTR_SUPPORTED_PRBS_PATTERN not supported for port %s id=%" PRIx64,
                            alias.c_str(), port_id);
        }
        else
        {
            SWSS_LOG_WARN("Failed to get supported PRBS patterns for port %s id=%" PRIx64 ": %s",
                          alias.c_str(), port_id, sai_serialize_status(status).c_str());
        }
        return;
    }

    for (uint32_t i = 0; i < attr.value.s32list.count; i++)
    {
        supported_patterns.push_back(static_cast<sai_uint32_t>(attr.value.s32list.list[i]));
    }
    SWSS_LOG_NOTICE("Supported PRBS patterns for port %s id=%" PRIx64 ": count=%u",
                    alias.c_str(), port_id, attr.value.s32list.count);
}

void PrbsHandler::initPortSupportedPrbsPatterns(const string &alias, sai_object_id_t port_id,
                                                 Table &portStateTable)
{
    SWSS_LOG_ENTER();

    if (m_portSupportedPrbsPatterns.count(port_id))
    {
        return;
    }
    PortSupportedPrbsPatterns supported_prbs_patterns;
    getPortSupportedPrbsPatterns(alias, port_id, supported_prbs_patterns);
    m_portSupportedPrbsPatterns[port_id] = supported_prbs_patterns;

    if (supported_prbs_patterns.empty())
    {
        SWSS_LOG_INFO("No supported_prbs_patterns exposed to STATE_DB for port %s (enum capability unavailable or empty)",
                      alias.c_str());
        return;
    }

    vector<string> pattern_names;
    for (const auto &val : supported_prbs_patterns)
    {
        bool found = false;
        for (const auto &kv : prbs_pattern_map)
        {
            if (static_cast<sai_uint32_t>(kv.second) == val)
            {
                pattern_names.push_back(kv.first);
                found = true;
                break;
            }
        }
        if (!found)
        {
            pattern_names.push_back(to_string(val));
        }
    }

    vector<FieldValueTuple> v;
    string supported_prbs_patterns_str = swss::join(',', pattern_names.begin(), pattern_names.end());
    v.emplace_back(make_pair("supported_prbs_patterns", supported_prbs_patterns_str));

    portStateTable.set(alias, v);
}

bool PrbsHandler::isPrbsPatternSupported(const std::string& alias, sai_object_id_t port_id, const string &prbs_pattern_str, swss::Table &portStateTable)
{
    initPortSupportedPrbsPatterns(alias, port_id, portStateTable);

    auto pattern_it = prbs_pattern_map.find(prbs_pattern_str);
    if (pattern_it == prbs_pattern_map.end())
    {
        SWSS_LOG_ERROR("Invalid PRBS pattern: %s for port %s", prbs_pattern_str.c_str(), alias.c_str());
        return false;
    }
    sai_uint32_t pattern = static_cast<sai_uint32_t>(pattern_it->second);

    return std::find(m_portSupportedPrbsPatterns[port_id].begin(), m_portSupportedPrbsPatterns[port_id].end(), pattern) != m_portSupportedPrbsPatterns[port_id].end();
}

void PrbsHandler::clearSupportedPrbsPatterns(sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();
    m_portSupportedPrbsPatterns.erase(port_id);
    SWSS_LOG_INFO("Cleared supported PRBS patterns for port %" PRIx64, port_id);
}
