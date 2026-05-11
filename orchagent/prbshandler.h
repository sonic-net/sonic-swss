/*
 * PRBS (Pseudo-Random Bit Sequence) Handler
 * Helper class for managing PRBS configuration and result caching
 */

#ifndef SWSS_PRBSHANDLER_H
#define SWSS_PRBSHANDLER_H

#include "port.h"
#include "table.h"

extern "C" {
#include "sai.h"
#include "saiport.h"
}

#include <string>
#include <map>
#include <vector>

typedef std::vector<sai_uint32_t> PortSupportedPrbsPatterns;

class PrbsHandler
{
public:
    PrbsHandler(swss::Table &stateTable, swss::Table &laneResultTable, swss::Table &resultsTable);

    /*
     * Handle PRBS pattern configuration from APP_DB.
     * Sets SAI_PORT_ATTR_PRBS_PATTERN on the port.
     * Must be called BEFORE handlePrbsConfig() to ensure pattern
     * is set before enabling PRBS.
     *
     * @param port: Port object
     * @param prbs_pattern_str: PRBS pattern string ("PRBS7", "PRBS9", etc.)
     * @return true if successful, false otherwise
     */
    bool handlePrbsPatternConfig(swss::Port &port, const std::string &prbs_pattern_str);

    /*
     * Handle PRBS mode configuration from APP_DB.
     * Sets SAI_PORT_ATTR_PRBS_CONFIG on the port (enable/disable).
     * Pattern should be set first via handlePrbsPatternConfig().
     *
     * @param port: Port object
     * @param prbs_mode_str: PRBS mode string ("both", "rx", "tx", "disabled")
     * @return true if successful, false otherwise
     */
    bool handlePrbsConfig(swss::Port &port, const std::string &prbs_mode_str);

    /*
     * Clear old PRBS test results from STATE_DB by port alias
     */
    void clearPrbsResults(const std::string &port_alias);

    /*
     * Remove all PRBS entries from STATE_DB (used during initialization/cleanup)
     */
    void removeAllPrbsEntries();

    /*
     * Query SAI for supported PRBS polynomial values via
     * get_port_attribute(SAI_PORT_ATTR_SUPPORTED_PRBS_PATTERN).
     * Populates supported_patterns with the values the platform supports.
     */
    void getPortSupportedPrbsPatterns(const std::string &alias, sai_object_id_t port_id,
                                      PortSupportedPrbsPatterns &supported_patterns);

    /*
     * Query and cache supported PRBS patterns for a port, and publish
     * them to portStateTable for operator visibility.
     */
    void initPortSupportedPrbsPatterns(const std::string &alias, sai_object_id_t port_id,
                                       swss::Table &portStateTable);

    /*
     * Check if a PRBS pattern is supported on a port.
     * @param alias: Port alias
     * @param port_id: Port ID
     * @param pattern: PRBS pattern
     * @param portStateTable: Port state table
     * @return true if supported, false otherwise
     */
    bool isPrbsPatternSupported(const std::string& alias, sai_object_id_t port_id, const std::string &prbs_pattern_str, swss::Table &portStateTable);

    /*
     * Clear supported PRBS patterns for a port.
     * @param port_id: Port ID
     */
    void clearSupportedPrbsPatterns(sai_object_id_t port_id);
private:
    swss::Table &m_stateTable;       // STATE_DB:PORT_PRBS_TEST table
    swss::Table &m_laneResultTable;  // STATE_DB:PORT_PRBS_LANE_RESULT table
    swss::Table &m_resultsTable;    // STATE_DB:PORT_PRBS_RESULTS table (aggregate: total_lanes, lock_status, error_count)

    std::map<sai_object_id_t, PortSupportedPrbsPatterns> m_portSupportedPrbsPatterns;

    /*
     * Disable PRBS on a port and capture results
     */
    bool handlePrbsDisable(swss::Port &port);

    /*
     * Capture PRBS test results from SAI and write to STATE_DB
     */
    bool capturePrbsResults(swss::Port &port);
};

#endif /* SWSS_PRBSHANDLER_H */
