#include "mock_test_helpers.h"

namespace mock_test_helpers
{
    bool findAttr(const std::vector<sai_attribute_t> &attrs, sai_attr_id_t id,
                  sai_attribute_value_t &out)
    {
        for (const auto &attr : attrs)
        {
            if (attr.id == id)
            {
                out = attr.value;
                return true;
            }
        }
        return false;
    }
}
