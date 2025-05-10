#include <iostream>
#include <fstream>
#include <regex>

#include "orch_zmq_config.h"

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
    return std::make_shared<ZmqClient>(zmq_address + ":" + std::to_string(zmq_port), vrf.c_str());
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

    return std::make_shared<ZmqServer>(zmq_address.c_str(), vrf.c_str());
}