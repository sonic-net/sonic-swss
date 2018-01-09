#include <net/ethernet.h>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <exception>

#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"


void Request::parse(const KeyOpFieldsValuesTuple& request)
{
    if (is_parsed_)
    {
        throw std::logic_error("The parser already has a parsed request");
    }

    parseOperation(request);
    parseKey(request);
    parseAttrs(request);

    is_parsed_ = true;
}

void Request::clear()
{
    operation_.clear();
    full_key_.clear();
    attr_names_.clear();
    key_item_strings_.clear();
    key_item_mac_addresses_.clear();
    attr_item_strings_.clear();
    attr_item_bools_.clear();
    attr_item_mac_addresses_.clear();
    attr_item_packet_actions_.clear();

    is_parsed_ = false;
}

void Request::parseOperation(const KeyOpFieldsValuesTuple& request)
{
    operation_ = kfvOp(request);
    if (operation_ != SET_COMMAND && operation_ != DEL_COMMAND)
    {
        throw std::invalid_argument(std::string("Wrong operation: ") + operation_);
    }
}

void Request::parseKey(const KeyOpFieldsValuesTuple& request)
{
    full_key_ = kfvKey(request);

    // split the key by separator
    std::vector<std::string> key_items;
    size_t f_position = 0;
    size_t e_position = full_key_.find(key_separator_);
    while (e_position != std::string::npos)
    {
        key_items.push_back(full_key_.substr(f_position, e_position - f_position));
        f_position = e_position + 1;
        e_position = full_key_.find(key_separator_, f_position);
    }
    key_items.push_back(full_key_.substr(f_position, full_key_.length()));

    if (key_items.size() != number_of_key_items_)
    {
        throw std::invalid_argument(std::string("Wrong number of key items. Expected ")
                                  + std::to_string(number_of_key_items_)
                                  + std::string(" item(s). Key: '")
                                  + full_key_
                                  + std::string("'"));
    }

    // check types of the key items
    for (int i = 0; i < static_cast<int>(number_of_key_items_); i++)
    {
        switch(request_description_.key_item_types[i])
        {
            case REQ_T_STRING:
                key_item_strings_[i] = key_items[i];
                break;
            case REQ_T_MAC_ADDRESS:
                key_item_mac_addresses_[i] = parseMacAddress(key_items[i]);
                break;
            default:
                throw std::logic_error(std::string("Not implemented key type parser. Key") + key_items[i]);
        }
    }
}

void Request::parseAttrs(const KeyOpFieldsValuesTuple& request)
{
    const auto not_found = std::end(request_description_.attr_item_types);

    for (auto i = kfvFieldsValues(request).begin();
         i != kfvFieldsValues(request).end(); i++)
    {
        if (fvField(*i) == "empty")
        {
            // if name of the attribute is 'empty', just skip it.
            // it's using when we don't have any attributes, but we have to provide one for redis
            continue;
        }
        const auto item = request_description_.attr_item_types.find(fvField(*i));
        if (item == not_found)
        {
            throw std::invalid_argument(std::string("Unknown attribute name: ") + fvField(*i));
        }
        attr_names_.insert(fvField(*i));
        switch(item->second)
        {
            case REQ_T_STRING:
                attr_item_strings_[fvField(*i)] = fvValue(*i);
                break;
            case REQ_T_BOOL:
                attr_item_bools_[fvField(*i)] = parseBool(fvValue(*i));
                break;
            case REQ_T_MAC_ADDRESS:
                attr_item_mac_addresses_[fvField(*i)] = parseMacAddress(fvValue(*i));
                break;
            case REQ_T_PACKET_ACTION:
                attr_item_packet_actions_[fvField(*i)] = parsePacketAction(fvValue(*i));
                break;
            default:
                throw std::logic_error(std::string("Not implemented attribute type parser for attribute:") + fvField(*i));
        }
    }

    if (operation_ == DEL_COMMAND && attr_names_.size() > 0)
    {
        throw std::invalid_argument("Delete operation request contains attributes");
    }

    if (operation_ == SET_COMMAND)
    {
        for (const auto& attr: request_description_.mandatory_attr_items)
        {
            if (attr_names_.find(attr) == std::end(attr_names_))
            {
                throw std::invalid_argument(std::string("Mandatory attribute '") + attr + std::string("' not found"));
            }
        }
    }
}

bool Request::parseBool(const std::string& str)
{
    if (str == "true")
    {
        return true;
    }

    if (str == "false")
    {
        return false;
    }

    throw std::invalid_argument(std::string("Can't parse boolean value '") + str + std::string("'"));
}

MacAddress Request::parseMacAddress(const std::string& str)
{
    uint8_t mac[ETHER_ADDR_LEN];

    if (!MacAddress::parseMacString(str, mac))
    {
        throw std::invalid_argument(std::string("Invalid mac address: ") + str);
    }

    return MacAddress(mac);
}

sai_packet_action_t Request::parsePacketAction(const std::string& str)
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
        throw std::invalid_argument(std::string("Wrong packet action attribute value '") + str + std::string("'"));
    }

    return found->second;
}
