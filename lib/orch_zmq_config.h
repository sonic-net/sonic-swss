#ifndef SWSS_ORCH_ZMQ_CONFIG_H
#define SWSS_ORCH_ZMQ_CONFIG_H

#include <string.h>
#include <set>

#include "dbconnector.h"
#include "zmqclient.h"

/*
 * Orchagent ZMQ enabled table will stored in "DEVICE_METADATA|localhost" table "orch_zmq_tables" field.
 */
#define DEVICE_METADATA_LOCALHOST       "DEVICE_METADATA|localhost"
#define ZMQ_TABLE_LIST_FIELD            "orch_zmq_tables@"
#define LIST_FIELD_DELIMETER            ','


/*
 * swssconfig will only connect to local orchagent ZMQ endpoint.
 */
#define ZMQ_LOCAL_ADDRESS               "tcp://localhost"

namespace swss {

std::set<std::string> load_zmq_tables();

std::shared_ptr<ZmqClient> create_zmq_client(std::string zmq_address);

}

#endif /* SWSS_ORCH_ZMQ_CONFIG_H */