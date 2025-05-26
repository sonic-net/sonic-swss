#ifndef SWSS_ORCH_ZMQ_CONFIG_H
#define SWSS_ORCH_ZMQ_CONFIG_H

#include <memory>
#include <string.h>
#include <set>

#include "dbconnector.h"
#include "zmqclient.h"
#include "zmqserver.h"
#include "zmqproducerstatetable.h"

/*
 * swssconfig will only connect to local orchagent ZMQ endpoint.
 */
#define ZMQ_LOCAL_ADDRESS               "tcp://localhost"
namespace swss {

std::set<std::string> load_zmq_tables();

int get_zmq_port();

std::shared_ptr<ZmqClient> create_zmq_client(std::string zmq_address, std::string vrf="");

std::shared_ptr<ZmqServer> create_zmq_server(std::string zmq_address, std::string vrf="");

bool get_feature_status(std::string feature, bool default_value);

std::shared_ptr<swss::ZmqClient> create_zmq_client(std::string feature, bool default_value);

std::shared_ptr<swss::ProducerStateTable> createProducerStateTable(DBConnector *db, const std::string &tableName, std::shared_ptr<swss::ZmqClient> zmqClient);

std::shared_ptr<swss::ProducerStateTable> createProducerStateTable(RedisPipeline *pipeline, const std::string &tableName, bool buffered, std::shared_ptr<swss::ZmqClient> zmqClient);
}

#endif /* SWSS_ORCH_ZMQ_CONFIG_H */
