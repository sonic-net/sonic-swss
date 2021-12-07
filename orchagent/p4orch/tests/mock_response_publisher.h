#pragma once

#include <gmock/gmock.h>

#include "response_publisher_interface.h"

class MockResponsePublisher : public ResponsePublisherInterface
{
  public:
    MOCK_METHOD6(publish, void(const std::string &table, const std::string &key,
                               const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                               const std::vector<swss::FieldValueTuple> &state_attrs, bool replace));
    MOCK_METHOD5(publish,
                 void(const std::string &table, const std::string &key,
                      const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status, bool replace));
    MOCK_METHOD5(writeToDB,
                 void(const std::string &table, const std::string &key,
                      const std::vector<swss::FieldValueTuple> &values, const std::string &op, bool replace));
};
