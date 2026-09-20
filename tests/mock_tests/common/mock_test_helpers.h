#pragma once

#include <vector>

extern "C" {
#include "sai.h"
}

namespace mock_test_helpers
{
    // The mock-test harness runs orchagent against libsaivs in-process with no
    // syncd and no populated ASIC_DB (unlike a VS run). Tests therefore verify
    // what an orch programs by capturing the sai_attribute_t list it passes to a
    // mocked SAI create/set call (see mock_sai_api.h) and inspecting it, rather
    // than reading ASIC_DB. These helpers operate on such a captured list.

    // Find an attribute by id in a captured SAI attribute list -- the mock-test
    // equivalent of reading a single attribute back from ASIC_DB. Returns true
    // and fills `out` with the attribute value if the id is present.
    bool findAttr(const std::vector<sai_attribute_t> &attrs, sai_attr_id_t id,
                  sai_attribute_value_t &out);
}
