#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>

#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"


bool Request::ParseOperation(const KeyOpFieldsValuesTuple& request)
{
    operation_ = kfvOp(request);
    if (operation_ != SET_COMMAND && operation_ != DEL_COMMAND)
    {
        // FIXME: message
        return false;
    }

    return true;
}

bool Request::ParseKey(const KeyOpFieldsValuesTuple& request)
{
    full_key_ = kfvKey(request);

    // split the key by separator
    std::vector<std::string> key_items;
    size_t i = 0;
    size_t position = full_key_.find(key_separator_);
    while (position != std::string::npos)
    {
        key_items.push_back(full_key_.substr(i, position - i));
        i = position + 1;
        position = full_key_.find(key_separator_, i);
    }
    key_items.push_back(full_key_.substr(i, full_key_.length()));

    if (key_items.size() != number_of_key_items_)
    {
        SWSS_LOG_ERROR("Can't parse request key. Wrong number of key items. Expected %lu items: %s",
            number_of_key_items_, full_key_.c_str());
        return false;
    }

    // check types of the key items
    for (int i = 0; i < static_cast<int>(number_of_key_items_); i++)
    {
        switch(request_description_.key_item_types[i])
        {
            case REQ_T_STRING:
                key_item_strings_[i] = std::move(key_items[i]);
                break;
            case REQ_T_MAC_ADDRESS:
                uint8_t mac[6]; // FIXME: correct mac address length
                if (!MacAddress::parseMacString(key_items[i], mac))
                {
                    // FIXME: Error message
                    return false;
                }
                key_item_mac_addresses_[i] = MacAddress(key_items[i]);
                break;
            default:
                SWSS_LOG_ERROR("Not implemented key type parser for key: %s", full_key_.c_str()); // show type
                return false;
        }
    }

    return true;
}

bool Request::ParseAttrs(const KeyOpFieldsValuesTuple& request)
{
    const auto not_found = std::end(request_description_.attr_item_types);

    for (auto i = kfvFieldsValues(request).begin();
         i != kfvFieldsValues(request).end(); i++)
    {
        const auto item = request_description_.attr_item_types.find(fvField(*i));
        if (item == not_found)
        {
            SWSS_LOG_INFO("Unknown attribute %s", fvField(*i).c_str());
            return false;
        }
        attr_names_.push_back(fvField(*i));
        switch(item->second)
        {
            case REQ_T_STRING:
                attr_item_strings_[fvField(*i)] = fvValue(*i);
                break;
            case REQ_T_BOOL:
                bool value;
                if (!ParseBool(fvValue(*i), value))
                {
                    return false;
                }
                attr_item_bools_[fvField(*i)] = value;
                break;
            case REQ_T_MAC_ADDRESS:
                uint8_t mac[6];
                if (!MacAddress::parseMacString(fvValue(*i), mac))
                {
                    return false;
                }
                attr_item_mac_addresses_[fvField(*i)] = MacAddress(mac);
                break;
            case REQ_T_PACKET_ACTION:
                sai_packet_action_t packet_action;
                if (!ParsePacketAction(fvValue(*i), packet_action))
                {
                    return false;
                }
                attr_item_packet_actions_[fvField(*i)] = packet_action;
                break;
            default:
                SWSS_LOG_ERROR("Not implemented attr type parser for attr: %s", fvField(*i).c_str()); // show type
                return false;
        }
    }

    if (operation_ == DEL_COMMAND && attr_names_.size() > 0)
    {
        // FIXME: no attributes for delete operations
        return false;
    }

    std::set<std::string> attr_names(std::begin(attr_names_), std::end(attr_names_));
    for (const auto& attr: request_description_.mandatory_attr_items)
    {
        if (attr_names.find(attr) == std::end(attr_names))
        {
            // FIXME: mandatory attribute not found
            return false;
        }
    }

    return true;
}

bool Request::ParseBool(const std::string& str, bool& value)
{
    if (str == "true")
    {
        value = true;
        return true;
    }

    if (str == "false")
    {
        value = false;
        return true;
    }

    SWSS_LOG_ERROR("Can't parse boolean value '%s'", str.c_str());

    return false;
}

bool Request::ParsePacketAction(const std::string& str, sai_packet_action_t& packet_action)
{
    std::unordered_map<std::string, sai_packet_action_t> m = {
        {"drop", SAI_PACKET_ACTION_DROP},
        {"forward", SAI_PACKET_ACTION_FORWARD},
        {"copy", SAI_PACKET_ACTION_COPY},
        {"copy_cancel", SAI_PACKET_ACTION_COPY_CANCEL},
        {"trap", SAI_PACKET_ACTION_TRAP},
        {"log", SAI_PACKET_ACTION_LOG},
        {"deny", SAI_PACKET_ACTION_DENY},
        {"transit", SAI_PACKET_ACTION_TRANSIT},
    };

    const auto found = m.find(str);
    if (found == std::end(m))
    {
        SWSS_LOG_ERROR("Wrong packet action attribute value '%s'", str.c_str());
        return false;
    }

    packet_action = found->second;
    return true;
}
