#pragma once

#include "table.h"

// Use this field in the mock test to simulate an exception during hget.
#define HGET_THROW_EXCEPTION_FIELD_NAME "hget_throw_exception"

namespace testing_db
{
    void reset();
}
