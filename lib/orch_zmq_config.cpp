#include <iostream>
#include <fstream>
#include <regex>

#include "dbconnector.h"
#include "logger.h"
#include "orch_zmq_config.h"
#include <stdio.h>

#define ZMQ_TABLE_CONFIGFILE       "/etc/swss/orch_zmq_tables.conf"

// ZMQ none IPV6 address with port, for example: tcp://127.0.0.1:5555 tcp://localhost:5555
const std::regex ZMQ_NONE_IPV6_ADDRESS_WITH_PORT("\\w+:\\/\\/[^:]+:\\d+");

// ZMQ IPV6 address with port, for example: tcp://[fe80::fb7:c6df:9d3a:3d7b]:5555
const std::regex ZMQ_IPV6_ADDRESS_WITH_PORT("\\w+:\\/\\/\\[.*\\]+:\\d+");

std::set<std::string> swss::load_zmq_tables()
{
    std::set<std::string> tables;
    std::ifstream config_file(ZMQ_TABLE_CONFIGFILE);
    if (config_file.is_open())
    {
        std::string table;
        while (std::getline(config_file, table))
        {
            tables.emplace(table);
        }
        config_file.close();
    }

    return tables;
}

int swss::get_zmq_port()
{
    auto zmq_port = ORCH_ZMQ_PORT;
    if (const char* nsid = std::getenv("NAMESPACE_ID"))
    {
        // namespace start from 0, using original ZMQ port for global namespace
        zmq_port += atoi(nsid) + 1;
    }

    return zmq_port;
}

std::shared_ptr<swss::ZmqClient> swss::create_zmq_client(std::string zmq_address, std::string vrf)
{
    // swssconfig running inside swss contianer, so need get ZMQ port according to namespace ID.
    auto zmq_port = get_zmq_port();
    zmq_address = zmq_address + ":" + std::to_string(zmq_port);
    SWSS_LOG_NOTICE("Create ZMQ server with address: %s, vrf: %s", zmq_address.c_str(), vrf.c_str());
    return std::make_shared<ZmqClient>(zmq_address, vrf);
}

std::shared_ptr<swss::ZmqServer> swss::create_zmq_server(std::string zmq_address, std::string vrf)
{
    // TODO: remove this check after orchagent.sh migrate to pass ZMQ address without port
    if (!std::regex_search(zmq_address, ZMQ_NONE_IPV6_ADDRESS_WITH_PORT)
            && !std::regex_search(zmq_address, ZMQ_IPV6_ADDRESS_WITH_PORT))
    {
        auto zmq_port = get_zmq_port();
        zmq_address = zmq_address + ":" + std::to_string(zmq_port);
    }

    SWSS_LOG_NOTICE("Create ZMQ server with address: %s, vrf: %s", zmq_address.c_str(), vrf.c_str());
    return std::make_shared<ZmqServer>(zmq_address, vrf);
}

bool swss::get_feature_status(std::string feature, bool default_value)
{
    swss::DBConnector config_db("CONFIG_DB", 0);
    auto enabled = config_db.hget("DEVICE_METADATA|localhost", feature);
    if (!enabled)
    {
        printf("Not found feature %s status, return default value.\n", feature.c_str());
        SWSS_LOG_NOTICE("Not found feature %s status, return default value.", feature.c_str());
        return default_value;
    }

    printf("Get feature %s status: %s\n", feature.c_str(), enabled->c_str());
    SWSS_LOG_NOTICE("Get feature %s status: %s", feature.c_str(), enabled->c_str());
    return *enabled == "true";
}
