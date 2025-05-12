#ifndef SWSS_ORCH_ZMQ_CONFIG_H
#define SWSS_ORCH_ZMQ_CONFIG_H

#include <string.h>
#include <set>

#include "dbconnector.h"
#include "zmqclient.h"

/*
 * swssconfig will only connect to local orchagent ZMQ endpoint.
 */
#define ZMQ_LOCAL_ADDRESS               "tcp://localhost"

namespace swss {

std::set<std::string> load_zmq_tables();

std::shared_ptr<ZmqClient> create_zmq_client(std::string zmq_address);

}

#endif /* SWSS_ORCH_ZMQ_CONFIG_H */
