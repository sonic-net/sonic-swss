#include "zmqserver.h"

using namespace std;

namespace swss {

ZmqServer::ZmqServer(const std::string& endpoint)
    : m_endpoint(endpoint) {}

ZmqServer::~ZmqServer() {}

void ZmqServer::registerMessageHandler(const std::string dbName,
                                       const std::string tableName,
                                       ZmqMessageHandler* handler) {}

void ZmqServer::sendMsg(
    const std::string& dbName, const std::string& tableName,
    const std::vector<swss::KeyOpFieldsValuesTuple>& values) {}

ZmqMessageHandler* ZmqServer::findMessageHandler(const std::string dbName,
                                                 const std::string tableName) {
  return nullptr;
}

void ZmqServer::handleReceivedData(const char* buffer, const size_t size) {}

void ZmqServer::mqPollThread() {}

}  // namespace swss
