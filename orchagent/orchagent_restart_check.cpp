#include <iostream>
#include <sstream>

#include <unistd.h>
#include <getopt.h>

#include "notificationproducer.h"
#include "notificationconsumer.h"
#include "select.h"
#include "logger.h"

int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_NOTICE);
    SWSS_LOG_ENTER();

    std::string op = "orchagent";

    swss::DBConnector db(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    swss::NotificationProducer restartQuery(&db, "RESTARTCHECK");

    swss::NotificationConsumer restartQueryReply(&db, "RESTARTCHECKREPLY");
    swss::Select s;
    s.addSelectable(&restartQueryReply);
    swss::Selectable *sel;

    std::vector<swss::FieldValueTuple> values;
    SWSS_LOG_NOTICE("requested %s to do warm restart state check and freeze if ready", op.c_str());
    restartQuery.send(op, op, values);

    int result = s.select(&sel, 3000);
    if (result == swss::Select::OBJECT)
    {
        std::string op_ret, data;

        restartQueryReply.pop(op_ret, data, values);
        if (op_ret == "READY")
        {
            SWSS_LOG_NOTICE("RESTARTCHECK success, %s is frozen and ready for warm restart", op.c_str());
            std::cout << "RESTARTCHECK succeeded" << std::endl;
            return EXIT_SUCCESS;
        }
        else
        {
            SWSS_LOG_NOTICE("RESTARTCHECK failed, %s is not ready for warm restart", op.c_str());
        }
    }
    else if (result == swss::Select::TIMEOUT)
    {
        SWSS_LOG_NOTICE("RESTARTCHECK for %s timed out", op.c_str());
    }
    else
    {
        SWSS_LOG_NOTICE("RESTARTCHECK for %s error", op.c_str());
    }
    std::cout << "RESTARTCHECK failed" << std::endl;
    return EXIT_FAILURE;
}
