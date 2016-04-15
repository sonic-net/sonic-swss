#include <stdlib.h>
#include <iostream>
#include <vector>
#include "dbconnector.h"
#include "producertable.h"
#include "logger.h"

using namespace std;
using namespace swss;

void usage(char **argv)
{
    SWSS_LOG_ENTER();

    cout << "Usage: " << argv[0] << " [start|stop]" << endl;
}

int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_DEBUG);

    SWSS_LOG_ENTER();

    DBConnector db(APPL_DB, "localhost", 6379, 0);
    ProducerTable r(&db, APP_ROUTE_TABLE_NAME);

    if (argc != 2)
    {
        usage(argv);
        exit(EXIT_FAILURE);
    }

    std::string op = std::string(argv[1]);
    if (op == "stop")
    {
        r.del("resync");
    }
    else if (op == "start")
    {
        FieldValueTuple fv("nexthop", "0.0.0.0");
        std::vector<FieldValueTuple> fvVector = { fv };
        r.set("resync", fvVector);
    }
    else
    {
        usage(argv);
        exit(EXIT_FAILURE);
    }

    return EXIT_SUCCESS;
}
