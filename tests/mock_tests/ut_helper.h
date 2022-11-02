#pragma once

#define LIBVS 1
#define LIBSAIREDIS 2
#define WITH_SAI LIBVS

#include "gtest/gtest.h"
#include "portal.h"
#include "saispy.h"

#include "check.h"

namespace ut_helper
{
    sai_status_t initSaiApi(const std::map<std::string, std::string> &profile);
    sai_status_t uninitSaiApi();

    map<string, vector<FieldValueTuple>> getInitialSaiPorts();
}
