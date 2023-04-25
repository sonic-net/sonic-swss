#include <string>
#include <vector>

#include "response_publisher.h"
#include "mock_response_publisher.h"

/* This mock plugs into this fake response publisher implementation
 * when needed to test code that uses response publisher. */
std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

ResponsePublisher::ResponsePublisher() : m_db("APPL_STATE_DB", 0) {}

void ResponsePublisher::publish(
    const std::string& table, const std::string& key,
    const std::vector<swss::FieldValueTuple>& intent_attrs,
    const ReturnCode& status,
    const std::vector<swss::FieldValueTuple>& state_attrs, bool replace)
{
    if (gMockResponsePublisher)
    {
        gMockResponsePublisher->publish(table, key, intent_attrs, status, state_attrs, replace);
    }
}

void ResponsePublisher::publish(
    const std::string& table, const std::string& key,
    const std::vector<swss::FieldValueTuple>& intent_attrs,
    const ReturnCode& status, bool replace)
{
    if (gMockResponsePublisher)
    {
        gMockResponsePublisher->publish(table, key, intent_attrs, status, replace);
    }
}

void ResponsePublisher::writeToDB(
    const std::string& table, const std::string& key,
    const std::vector<swss::FieldValueTuple>& values, const std::string& op,
    bool replace) {}
