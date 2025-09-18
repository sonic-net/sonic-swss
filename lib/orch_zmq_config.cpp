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
    const char* nsid = std::getenv("NAMESPACE_ID");
    std::string nsid_str = nsid ? std::string(nsid) : "";
    if (!nsid_str.empty())
    {
        try
        {
            // namespace start from 0, using original ZMQ port for global namespace
            zmq_port += std::stoi(nsid) + 1;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to convert %s to int, fallback to default port", nsid_str.c_str());
        }
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

    // To prevent message loss between ZmqServer's bind operation and the creation of ZmqProducerStateTable,
    // use lazy binding and call bind() only after the handler has been registered.
    return std::make_shared<ZmqServer>(zmq_address, vrf, true);
}

bool swss::get_feature_status(std::string feature, bool default_value)
{
    std::shared_ptr<std::string> enabled = nullptr;

    try
    {
        swss::DBConnector config_db("CONFIG_DB", 0);
        enabled = config_db.hget("DEVICE_METADATA|localhost", feature);
    }
    catch (const std::runtime_error &e)
    {
        SWSS_LOG_ERROR("Not found feature %s failed with exception: %s", feature.c_str(), e.what());
        return default_value;
    }

    if (!enabled)
    {
        SWSS_LOG_NOTICE("Not found feature %s status, return default value.", feature.c_str());
        return default_value;
    }

    SWSS_LOG_NOTICE("Get feature %s status: %s", feature.c_str(), enabled->c_str());
    return *enabled == "true";
}

std::shared_ptr<swss::ZmqClient> swss::create_local_zmq_client(std::string feature, bool default_value)
{
    auto enable = get_feature_status(feature, default_value);
    if (enable) {
        SWSS_LOG_NOTICE("Feature %s enabled, Create ZMQ client : %s", feature.c_str(), ZMQ_LOCAL_ADDRESS);
        return create_zmq_client(ZMQ_LOCAL_ADDRESS);
    }

    return nullptr;
}

std::shared_ptr<swss::ProducerStateTable> swss::createProducerStateTable(DBConnector *db, const std::string &tableName, std::shared_ptr<swss::ZmqClient> zmqClient)
{
    swss::ProducerStateTable *tablePtr = nullptr;
    if (zmqClient != nullptr) {
        SWSS_LOG_NOTICE("Create ZmqProducerStateTable : %s", tableName.c_str());
        tablePtr = new swss::ZmqProducerStateTable(db, tableName, *zmqClient);
    }
    else {
        SWSS_LOG_NOTICE("Create ProducerStateTable : %s", tableName.c_str());
        tablePtr = new swss::ProducerStateTable(db, tableName);
    }

    return std::shared_ptr<swss::ProducerStateTable>(tablePtr);
}

std::shared_ptr<swss::ProducerStateTable> swss::createProducerStateTable(RedisPipeline *pipeline, const std::string& tableName, bool buffered, std::shared_ptr<swss::ZmqClient> zmqClient)
{
    swss::ProducerStateTable *tablePtr = nullptr;
    if (zmqClient != nullptr) {
        SWSS_LOG_NOTICE("Create ZmqProducerStateTable : %s", tableName.c_str());
        tablePtr = new swss::ZmqProducerStateTable(pipeline, tableName, *zmqClient);
    }
    else {
        SWSS_LOG_NOTICE("Create ProducerStateTable : %s", tableName.c_str());
        tablePtr = new swss::ProducerStateTable(pipeline, tableName, buffered);
    }

    return std::shared_ptr<swss::ProducerStateTable>(tablePtr);
}
