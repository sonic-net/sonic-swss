#include <iostream>
#include <fstream>

#include "orch_zmq_config.h"

#define ZMQ_TABLE_CONFIGFILE       "/etc/swss/orch_zmq_tables.conf"

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


std::shared_ptr<swss::ZmqClient> swss::create_zmq_client(std::string zmq_address)
{
    // swssconfig running inside swss contianer, so need get ZMQ port according to namespace ID.
    auto zmq_port = ORCH_ZMQ_PORT;
    if (const char* nsid = std::getenv("NAMESPACE_ID"))
    {
        // namespace start from 0, using original ZMQ port for global namespace
        zmq_port += atoi(nsid) + 1;
    }

    return std::make_shared<ZmqClient>(zmq_address + ":" + std::to_string(zmq_port));
}
