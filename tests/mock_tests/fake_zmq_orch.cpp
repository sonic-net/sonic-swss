#include "zmqorch.h"
#include "dbconnector.h"
#include "zmqserver.h"

#include <vector>
#include <string>


ZmqOrch::ZmqOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, swss::ZmqServer *zmqServer) {};
void ZmqOrch::doTask(Consumer &consumer) { };
