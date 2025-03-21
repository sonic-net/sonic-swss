#pragma once

#define LIBVS 1
#define LIBSAIREDIS 2
#define WITH_SAI LIBVS

#include "gtest/gtest.h"
#include "tests/mock_tests/portal.h"
#include "tests/mock_tests/saispy.h"

#include "tests/mock_tests/check.h"

namespace ut_helper
{
    sai_status_t initSaiApi(const std::map<std::string, std::string> &profile);
    sai_status_t uninitSaiApi();

    map<string, vector<FieldValueTuple>> getInitialSaiPorts();
}
