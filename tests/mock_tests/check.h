#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

#include "SaiAttributeList.h"

#include <hiredis/hiredis.h>
#include "swss/dbconnector.h"
#include "swss/logger.h"
#include "sai_serialize.h"
#include "string.h"

struct Check
{
    static bool AttrListEq(sai_object_type_t objecttype, const std::vector<sai_attribute_t> &act_attr_list, saimeta::SaiAttributeList &exp_attr_list)
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

            const sai_attribute_t* act = &act_attr_list[i];
            const sai_attribute_t* exp = &exp_attr_list.get_attr_list()[i];
            if (!Check::AttrValue(objecttype, id, act, exp))
            {
                return false;
            }
        }

        return true;
    }

    static bool AttrValue(sai_object_type_t objecttype, sai_attr_id_t id, const sai_attribute_t* act, const sai_attribute_t* exp)
    {
        auto meta = sai_metadata_get_attr_metadata(objecttype, id);
        assert(meta != nullptr);

        const int MAX_BUF_SIZE = 0x4000;
        std::vector<char> act_buf(MAX_BUF_SIZE);
        std::vector<char> exp_buf(MAX_BUF_SIZE);

        act_buf.reserve(MAX_BUF_SIZE);
        exp_buf.reserve(MAX_BUF_SIZE);

        auto act_len = sai_serialize_attribute_value(act_buf.data(), meta, &act->value);
        auto exp_len = sai_serialize_attribute_value(exp_buf.data(), meta, &exp->value);

        assert(act_len < act_str.size());
        assert(act_len < exp_str.size());

        act_buf.resize(act_len);
        exp_buf.resize(exp_len);

        std::string act_str(act_buf.begin(), act_buf.end());
        std::string exp_str(exp_buf.begin(), exp_buf.end());

        if (act_len != exp_len)
        {
            std::cerr << "AttrValue length failed\n";
            std::cerr << "Actual:   " << act_len << "," << act_str << "\n";
            std::cerr << "Expected: " << exp_len << "," << exp_str << "\n";
            return false;
        }

        if (act_str != exp_str)
        {
            std::cerr << "AttrValue string failed\n";
            std::cerr << "Actual:   " << act_str << "\n";
            std::cerr << "Expected: " << exp_str << "\n";
            return false;
        }
        return true;
    }

    static bool AttrListSubset(sai_object_type_t objecttype, const std::vector<sai_attribute_t> &act_attr_list,
                               saimeta::SaiAttributeList &exp_attr_list, const std::vector<bool> skip_check)
    {
        /* 
            Size of attributes should be equal and in the same order. 
            If the validation has to be skipped for certain attributes populate the skip_check.
        */
        if (act_attr_list.size() != exp_attr_list.get_attr_count())
        {
            std::cerr << "AttrListSubset size mismatch\n";
            return false;
        }
        if (act_attr_list.size() != skip_check.size())
        {
            std::cerr << "AttrListSubset size mismatch\n";
            return false;
        }

        for (uint32_t i = 0; i < exp_attr_list.get_attr_count(); ++i)
        {
            if (skip_check[i])
            {
                continue;
            }
            sai_attr_id_t id = exp_attr_list.get_attr_list()[i].id;
            const sai_attribute_t* act = &act_attr_list[i];
            const sai_attribute_t* exp = &exp_attr_list.get_attr_list()[i];
            if (!Check::AttrValue(objecttype, id, act, exp))
            {
                return false;
            }
        }
        return true;
    }
};
