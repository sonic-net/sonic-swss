#ifndef SWSS_ORCH_ZMQ_CONFIG_H
#define SWSS_ORCH_ZMQ_CONFIG_H

#include <memory>
#include <string.h>
#include <set>

#include "dbconnector.h"
#include "zmqclient.h"
#include "zmqserver.h"
#include "zmqproducerstatetable.h"
#include "zmqrouteserver.h"

/*
 * swssconfig will only connect to local orchagent ZMQ endpoint.
 */
#define ZMQ_LOCAL_ADDRESS               "tcp://localhost"

/*
 * Feature flag to enable the gNMI service to send DASH events to orchagent via the ZMQ channel.
 */
#define ORCH_NORTHBOND_DASH_ZMQ_ENABLED "orch_northbond_dash_zmq_enabled"

/*
 * Route performance knob in SYSTEM_DEFAULTS table.
 */
#define SYSTEM_DEFAULTS_SWSS_ZMQ_KEY    "SYSTEM_DEFAULTS|swss_zmq"
#define SYSTEM_DEFAULTS_STATUS_FIELD    "status"

/*
 * STATE_DB tables that arm a warm or fast restart. The ZMQ route path supports
 * neither, so either one is mutually exclusive with swss_zmq. fast-reboot arms
 * both keys; both are checked so the gate does not depend on them staying
 * coupled.
 */
#define STATE_WARM_RESTART_ENABLE_KEY_PREFIX "WARM_RESTART_ENABLE_TABLE|"
#define STATE_FAST_RESTART_ENABLE_KEY        "FAST_RESTART_ENABLE_TABLE|system"
#define STATE_RESTART_ENABLE_FIELD           "enable"

/* Refusal message, shared by every entry point that reports it. */
#define ROUTE_PERF_ZMQ_CONFLICT_MSG \
    "swss_zmq is enabled together with %s, which is unsupported. " \
    "Disable one of them: the ZMQ route path and warm/fast restart " \
    "are mutually exclusive."

namespace swss {

std::set<std::string> load_zmq_tables();

int get_zmq_port();

std::shared_ptr<ZmqClient> create_zmq_client(std::string zmq_address, std::string vrf="");

std::shared_ptr<ZmqServer> create_zmq_server(std::string zmq_address, std::string vrf="");
std::shared_ptr<ZmqRouteServer> create_zmq_route_server(std::string zmq_address, std::string vrf="");

bool get_feature_status(std::string feature, bool default_value);

/*
 * True when a warm or fast restart is armed in STATE_DB. `scope` is set to the
 * key that matched, for logging. Warm restart is checked for system, bgp and
 * swss; fast restart for system.
 */
bool warm_or_fast_restart_enabled(std::string &scope);

bool get_route_perf_zmq_enabled();

/*
 * True when the ZMQ route path is configured together with a warm or fast
 * restart, which is unsupported. `scope` reports the restart key that matched.
 */
bool route_perf_zmq_conflict(std::string &scope);

std::shared_ptr<swss::ZmqClient> create_route_perf_zmq_client();

std::shared_ptr<swss::ZmqClient> create_local_zmq_client(std::string feature, bool default_value);

std::shared_ptr<swss::ProducerStateTable> createProducerStateTable(DBConnector *db, const std::string &tableName, std::shared_ptr<swss::ZmqClient> zmqClient);

std::shared_ptr<swss::ProducerStateTable> createProducerStateTable(RedisPipeline *pipeline, const std::string &tableName, bool buffered, std::shared_ptr<swss::ZmqClient> zmqClient);
}

#endif /* SWSS_ORCH_ZMQ_CONFIG_H */
