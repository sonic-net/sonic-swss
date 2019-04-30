#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

#include "saiattributelist.h"

struct Check {
    static bool AttrListEq(sai_object_type_t objecttype, const std::vector<sai_attribute_t>& act_attr_list, /*const*/ SaiAttributeList& exp_attr_list)
    {
        if (act_attr_list.size() != exp_attr_list.get_attr_count()) {
            return false;
        }

        for (uint32_t i = 0; i < exp_attr_list.get_attr_count(); ++i) {
            sai_attr_id_t id = exp_attr_list.get_attr_list()[i].id;
            auto meta = sai_metadata_get_attr_metadata(objecttype, id);

            assert(meta != nullptr);

            // The following id can not serialize, check id only
            if (id == SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE) {
                if (id != act_attr_list[i].id) {
                    auto meta_act = sai_metadata_get_attr_metadata(objecttype, act_attr_list[i].id);

                    if (meta_act) {
                        std::cerr << "AttrListEq failed\n";
                        std::cerr << "Actual:   " << meta_act->attridname << "\n";
                        std::cerr << "Expected: " << meta->attridname << "\n";        
                    }
                }

                continue;
            }

            char act_buf[0x4000];
            char exp_buf[0x4000];

            auto act_len = sai_serialize_attribute_value(act_buf, meta, &act_attr_list[i].value);
            auto exp_len = sai_serialize_attribute_value(exp_buf, meta, &exp_attr_list.get_attr_list()[i].value);

            // auto act = sai_serialize_attr_value(*meta, act_attr_list[i].value, false);
            // auto exp = sai_serialize_attr_value(*meta, &exp_attr_list.get_attr_list()[i].value, false);

            assert(act_len < sizeof(act_buf));
            assert(exp_len < sizeof(exp_buf));

            if (act_len != exp_len) {
                std::cout << "AttrListEq failed\n";
                std::cout << "Actual:   " << act_buf << "\n";
                std::cout << "Expected: " << exp_buf << "\n";
                return false;
            }

            if (strcmp(act_buf, exp_buf) != 0) {
                std::cout << "AttrListEq failed\n";
                std::cout << "Actual:   " << act_buf << "\n";
                std::cout << "Expected: " << exp_buf << "\n";
                return false;
            }
        }

        return true;
    }
};
