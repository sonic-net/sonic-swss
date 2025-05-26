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

    auto zmq_address_with_port = zmq_address + ":" + std::to_string(zmq_port);

    SWSS_LOG_NOTICE("Create ZMQ client with address: %s", zmq_address_with_port.c_str());
    return std::make_shared<ZmqClient>(zmq_address_with_port);
}

bool swss::get_feature_status(std::string feature, bool default_value)
{
    swss::DBConnector config_db("CONFIG_DB", 0);
    auto enabled = config_db.hget("DEVICE_METADATA|localhost", feature);
    if (!enabled)
    {
        SWSS_LOG_NOTICE("Not found feature %s status, return default value.", feature.c_str());
        return default_value;
    }

    SWSS_LOG_NOTICE("Get feature %s status: %s", feature.c_str(), enabled->c_str());
    return *enabled == "true";
}

std::shared_ptr<swss::ZmqClient> swss::create_zmq_client(std::string feature, bool default_value)
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