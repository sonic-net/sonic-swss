#include <stdlib.h>
#include <iostream>
#include <vector>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "logger.h"
#include "orch_zmq_config.h"
#include "zmqclient.h"
#include "zmqproducerstatetable.h"

using namespace std;
using namespace swss;

void usage(char **argv)
{
    cout << "Usage: " << argv[0] << " [start|stop]" << endl;
}

int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_INFO);

    SWSS_LOG_ENTER();
    DBConnector db("APPL_DB", 0);
    std::shared_ptr<ProducerStateTable> producerStateTablePtr = nullptr;
    std::shared_ptr<ZmqClient> zmqClient = nullptr;

    auto enable_route_zmq = get_feature_status("orch_route_zmq_enabled", false);
    if (enable_route_zmq) {
        zmqClient = create_zmq_client(ZMQ_LOCAL_ADDRESS);
        SWSS_LOG_NOTICE("routeresync initialize ZMQ client : %s", ZMQ_LOCAL_ADDRESS);

        auto ptr = new ZmqProducerStateTable(&db, APP_ROUTE_TABLE_NAME, *zmqClient);
        producerStateTablePtr = std::shared_ptr<swss::ProducerStateTable>(ptr);
    }
    else {
        auto ptr = new ProducerStateTable(&db, APP_ROUTE_TABLE_NAME);
        producerStateTablePtr = std::shared_ptr<swss::ProducerStateTable>(ptr);
    }

    if (argc != 2)
    {
        usage(argv);
        exit(EXIT_FAILURE);
    }

    std::string op = std::string(argv[1]);
    if (op == "stop")
    {
        producerStateTablePtr->del("resync");
    }
    else if (op == "start")
    {
        FieldValueTuple fv("nexthop", "0.0.0.0");
        std::vector<FieldValueTuple> fvVector = { fv };
        producerStateTablePtr->set("resync", fvVector);
    }
    else
    {
        usage(argv);
        exit(EXIT_FAILURE);
    }

    return EXIT_SUCCESS;
}
