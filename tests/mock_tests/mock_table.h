#pragma once

#include <cstddef>
#include <string>

#include "table.h"

// Use this field in the mock test to simulate an exception during hget.
#define HGET_THROW_EXCEPTION_FIELD_NAME "hget_throw_exception"

namespace testing_db
{
    void reset();
    void resetOperationCounters();
    std::size_t getProducerSetCount(const std::string &tableName);
    std::size_t getProducerDelCount(const std::string &tableName);
}
