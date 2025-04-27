#include "orch_zmq_config.h"

std::set<std::string> swss::load_zmq_tables()
{
    DBConnector db("CONFIG_DB", 0, false);
    auto zmq_tables = db.hget(DEVICE_METADATA_LOCALHOST, ZMQ_TABLE_LIST_FIELD);

    std::set<std::string> tables;
    if (zmq_tables)
    {
        std::string table;
        std::stringstream table_stream(*zmq_tables);
        while(std::getline(table_stream, table, LIST_FIELD_DELIMETER))
        {
            tables.emplace(table);
        }
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