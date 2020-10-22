#include <net/ethernet.h>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <exception>

#include "sai.h"
#include "macaddress.h"
#include "ipaddress.h"
#include "orch.h"
#include "request_parser.h"

using namespace std;
using namespace swss;


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
    size_t key_item_start = 0;
    size_t key_item_end = full_key_.find(key_separator_);
    while (key_item_end != std::string::npos)
    {
        key_items.push_back(full_key_.substr(key_item_start, key_item_end - key_item_start));
        key_item_start = key_item_end + 1;
        key_item_end = full_key_.find(key_separator_, key_item_start);
    }
    key_items.push_back(full_key_.substr(key_item_start, full_key_.length()));

    /*
     * Attempt to parse an IPv6 address only if the following conditions are met:
     * - The key separator is ":" 
     *     - The above logic will already correctly parse IPv6 addresses using other key separators
     *     - Special consideration is only needed for ":" key separators since IPv6 addresses also use ":" as the field separator
     * - The number of parsed key items exceeds the number of expected key items
     *     - If we have too many key items and the last key item is supposed to be an IP or prefix, there is a chance that it was an 
     *       IPv6 address that got segmented during parsing
     *     - The above logic will always break an IPv6 address into at least 2 parts, so if an IPv6 address has been parsed incorrectly it will
     *       always increase the number of key items
     * - The last key item is an IP address or prefix
     *     - This runs under the assumption that an IPv6 address, if present, will always be the last key item
     */
    if (key_separator_ == ':' and 
        key_items.size() > number_of_key_items_ and 
        (request_description_.key_item_types.back() == REQ_T_IP or request_description_.key_item_types.back() == REQ_T_IP_PREFIX))
    {
        // Remove key_items so that key_items.size() is correct, then assemble the removed items into an IPv6 address
        std::vector<std::string> ip_addr_groups(--key_items.begin() + number_of_key_items_, key_items.end());
        key_items.erase(--key_items.begin() + number_of_key_items_, key_items.end());

        std::string ip_string;

        for (const auto &i : ip_addr_groups)
        {
            ip_string += i + ":";
        }
        ip_string.pop_back(); // remove extra ":" from end of string

        key_items.push_back(ip_string);
    }
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
            case REQ_T_IP:
                key_item_ip_addresses_[i] = parseIpAddress(key_items[i]);
                break;
            case REQ_T_IP_PREFIX:
                key_item_ip_prefix_[i] = parseIpPrefix(key_items[i]);
                break;
            case REQ_T_UINT:
                key_item_uint_[i] = parseUint(key_items[i]);
                break;
            default:
                throw std::logic_error(std::string("Not implemented key type parser. Key '")
                                     + full_key_
                                     + std::string("'. Key item:")
                                     + key_items[i]);
        }
    }
}

void Request::parseAttrs(const KeyOpFieldsValuesTuple& request)
{
    const auto not_found = std::end(request_description_.attr_item_types);

    for (auto i = kfvFieldsValues(request).begin();
         i != kfvFieldsValues(request).end(); i++)
    {
        if (fvField(*i) == "empty" || fvField(*i) == "NULL")
        {
            // if name of the attribute is 'empty' or 'NULL', just skip it.
            // it's used when we don't have any attributes, but we have to provide one for redis
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
            case REQ_T_VLAN:
                attr_item_vlan_[fvField(*i)] = parseVlan(fvValue(*i));
                break;
            case REQ_T_IP:
                attr_item_ip_[fvField(*i)] = parseIpAddress(fvValue(*i));
                break;
            case REQ_T_IP_PREFIX:
                attr_item_ip_prefix_[fvField(*i)] = parseIpPrefix(fvValue(*i));
                break;
            case REQ_T_UINT:
                attr_item_uint_[fvField(*i)] = parseUint(fvValue(*i));
                break;
            case REQ_T_SET:
                attr_item_set_[fvField(*i)] = parseSet(fvValue(*i));
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

IpAddress Request::parseIpAddress(const std::string& str)
{
    try
    {
        IpAddress addr(str);
        return addr;
    }
    catch (std::invalid_argument& _)
    {
        throw std::invalid_argument(std::string("Invalid ip address: ") + str);
    }
}

IpPrefix Request::parseIpPrefix(const std::string& str)
{
    try
    {
        IpPrefix pfx(str);
        return pfx;
    }
    catch (std::invalid_argument& _)
    {
        throw std::invalid_argument(std::string("Invalid ip prefix: ") + str);
    }
}

set<string> Request::parseSet(const std::string& str)
{
    try
    {
        set<string> str_set;
        string substr;
        std::istringstream iss(str);
        while (getline(iss, substr, ','))
        {
            str_set.insert(substr);
        }
        return str_set;
    }
    catch (std::invalid_argument& _)
    {
        throw std::invalid_argument(std::string("Invalid string set"));
    }
}

uint64_t Request::parseUint(const std::string& str)
{
    try
    {
        uint64_t ret = std::stoul(str);
        return ret;
    }
    catch(std::invalid_argument& _)
    {
        throw std::invalid_argument(std::string("Invalid unsigned integer: ") + str);
    }
    catch(std::out_of_range& _)
    {
        throw std::invalid_argument(std::string("Out of range unsigned integer: ") + str);
    }
}

uint16_t Request::parseVlan(const std::string& str)
{
    uint16_t ret = 0;

    const auto vlan_prefix = std::string("Vlan");
    const auto prefix_len = vlan_prefix.length();

    if (str.substr(0, prefix_len) != vlan_prefix)
    {
        throw std::invalid_argument(std::string("Invalid vlan interface: ") + str);
    }

    try
    {
        ret = static_cast<uint16_t>(std::stoul(str.substr(prefix_len)));
    }
    catch(std::invalid_argument& _)
    {
        throw std::invalid_argument(std::string("Invalid vlan id: ") + str);
    }
    catch(std::out_of_range& _)
    {
        throw std::invalid_argument(std::string("Out of range vlan id: ") + str);
    }

    if (ret == 0 || ret > 4094)
    {
        throw std::invalid_argument(std::string("Out of range vlan id: ") + str);
    }

    return ret;
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
