#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

#include "saiattributelist.h"

struct Check
{
    static bool AttrListEq(sai_object_type_t objecttype, const std::vector<sai_attribute_t> &act_attr_list, SaiAttributeList &exp_attr_list)
    {
        if (act_attr_list.size() != exp_attr_list.get_attr_count())
        {
            return false;
        }

        for (uint32_t i = 0; i < exp_attr_list.get_attr_count(); ++i)
        {
            sai_attr_id_t id = exp_attr_list.get_attr_list()[i].id;
            auto meta = sai_metadata_get_attr_metadata(objecttype, id);

            assert(meta != nullptr);

            // The following id can not serialize, check id only
            if (id == SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE || id == SAI_ACL_BIND_POINT_TYPE_PORT || id == SAI_ACL_BIND_POINT_TYPE_LAG)
            {
                if (id != act_attr_list[i].id)
                {
                    auto meta_act = sai_metadata_get_attr_metadata(objecttype, act_attr_list[i].id);

                    if (meta_act)
                    {
                        std::cerr << "AttrListEq failed\n";
                        std::cerr << "Actual:   " << meta_act->attridname << "\n";
                        std::cerr << "Expected: " << meta->attridname << "\n";
                    }
                }

                continue;
            }

            const int MAX_BUF_SIZE = 0x4000;
            std::string act_str;
            std::string exp_str;

            act_str.reserve(MAX_BUF_SIZE);
            exp_str.reserve(MAX_BUF_SIZE);

            auto act_len = sai_serialize_attribute_value(&act_str[0], meta, &act_attr_list[i].value);
            auto exp_len = sai_serialize_attribute_value(&exp_str[0], meta, &exp_attr_list.get_attr_list()[i].value);

            assert(act_len < act_str.size());
            assert(act_len < exp_str.size());

            if (act_len != exp_len)
            {
                std::cerr << "AttrListEq failed\n";
                std::cerr << "Actual:   " << act_str << "\n";
                std::cerr << "Expected: " << exp_str << "\n";
                return false;
            }

            if (act_str != exp_str)
            {
                std::cerr << "AttrListEq failed\n";
                std::cerr << "Actual:   " << act_str << "\n";
                std::cerr << "Expected: " << exp_str << "\n";
                return false;
            }
        }

        return true;
    }
};
