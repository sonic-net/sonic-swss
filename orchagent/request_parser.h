#ifndef __REQUEST_PARSER_H
#define __REQUEST_PARSER_H

#include "ipaddress.h"
#include "ipprefix.h"
#include <sstream>
#include <set>
#include <vector>

typedef enum _request_types_t
{
    REQ_T_NOT_USED,
    REQ_T_BOOL,
    REQ_T_STRING,
    REQ_T_MAC_ADDRESS,
    REQ_T_PACKET_ACTION,
    REQ_T_IP,
    REQ_T_IP_PREFIX,
    REQ_T_VLAN,
    REQ_T_UINT,
    REQ_T_SET,
    REQ_T_MAC_ADDRESS_LIST,
    REQ_T_IP_LIST,
    REQ_T_UINT_LIST,
    REQ_T_BOOL_LIST,
} request_types_t;

typedef struct _request_description
{
    std::vector<request_types_t> key_item_types;
    std::unordered_map<std::string, request_types_t> attr_item_types;
    std::vector<std::string> mandatory_attr_items;
} request_description_t;

class Request
{
public:
    void parse(const swss::KeyOpFieldsValuesTuple& request);
    void clear();

    const std::string& getOperation() const
    {
        assert(is_parsed_);
        return operation_;
    }

    const std::string& getFullKey() const
    {
        assert(is_parsed_);
        return full_key_;
    }

    const std::string& getKeyString(int position) const
    {
        assert(is_parsed_);
        return key_item_strings_.at(position);
    }

    const swss::MacAddress& getKeyMacAddress(int position) const
    {
        assert(is_parsed_);
        return key_item_mac_addresses_.at(position);
    }

    const swss::IpAddress& getKeyIpAddress(int position) const
    {
        assert(is_parsed_);
        return key_item_ip_addresses_.at(position);
    }

    const swss::IpPrefix& getKeyIpPrefix(int position) const
    {
        assert(is_parsed_);
        return key_item_ip_prefix_.at(position);
    }

    const uint64_t& getKeyUint(int position) const
    {
        assert(is_parsed_);
        return key_item_uint_.at(position);
    }

    const std::unordered_set<std::string>& getAttrFieldNames() const
    {
        assert(is_parsed_);
        return attr_names_;
    }

    const std::string& getAttrString(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_strings_.at(attr_name);
    }

    bool getAttrBool(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_bools_.at(attr_name);
    }

    const swss::MacAddress& getAttrMacAddress(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_mac_addresses_.at(attr_name);
    }

    sai_packet_action_t getAttrPacketAction(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_packet_actions_.at(attr_name);
    }

    uint16_t getAttrVlan(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_vlan_.at(attr_name);
    }

    swss::IpAddress getAttrIP(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_ip_.at(attr_name);
    }

    swss::IpPrefix getAttrIpPrefix(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_ip_prefix_.at(attr_name);
    }

    const uint64_t& getAttrUint(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_uint_.at(attr_name);
    }

    const std::set<std::string>& getAttrSet(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_set_.at(attr_name);
    }

    void setTableName(std::string& table_name)
    {
        table_name_ = table_name;
    }

    const std::string& getTableName() const
    {
        assert(is_parsed_);
        return table_name_;
    }

    const std::vector<swss::IpAddress>& getAttrIPList(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_ip_list_.at(attr_name);
    }

    const std::vector<swss::MacAddress>& getAttrMacAddressList(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_mac_addresses_list_.at(attr_name);
    }

    const std::vector<uint64_t>& getAttrUintList(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_uint_list_.at(attr_name);
    }

    const std::vector<bool> getAttrBoolList(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_bool_list_.at(attr_name);
    }

protected:
    Request(const request_description_t& request_description, const char key_separator, bool relaxed_attr_parsing = false)
        : request_description_(request_description),
          key_separator_(key_separator),
          is_parsed_(false),
          relaxed_attr_parsing_(relaxed_attr_parsing),
          number_of_key_items_(request_description.key_item_types.size())
    {
    }


private:
    void parseOperation(const swss::KeyOpFieldsValuesTuple& request);
    void parseKey(const swss::KeyOpFieldsValuesTuple& request);
    void parseAttrs(const swss::KeyOpFieldsValuesTuple& request);
    bool parseBool(const std::string& str);
    swss::MacAddress parseMacAddress(const std::string& str);
    swss::IpAddress parseIpAddress(const std::string& str);
    swss::IpPrefix parseIpPrefix(const std::string& str);
    uint64_t parseUint(const std::string& str);
    uint16_t parseVlan(const std::string& str);
    std::set<std::string> parseSet(const std::string& str);
    std::vector<swss::IpAddress> parseIpAddressList(const std::string& str);
    std::vector<swss::MacAddress> parseMacAddressList(const std::string& str);
    std::vector<uint64_t> parseUintList(const std::string& str);
    std::vector<bool> parseBoolList(const std::string& str);

    sai_packet_action_t parsePacketAction(const std::string& str);

    const request_description_t& request_description_;
    char key_separator_;
    bool is_parsed_;
    size_t number_of_key_items_;
    // Enable if only interested in only a subset of attributes
    bool relaxed_attr_parsing_;

    std::string table_name_;
    std::string operation_;
    std::string full_key_;
    std::unordered_map<int, std::string> key_item_strings_;
    std::unordered_map<int, swss::MacAddress> key_item_mac_addresses_;
    std::unordered_map<int, swss::IpAddress> key_item_ip_addresses_;
    std::unordered_map<int, swss::IpPrefix> key_item_ip_prefix_;
    std::unordered_map<int, uint64_t> key_item_uint_;
    std::unordered_set<std::string> attr_names_;
    // FIXME: Make one union with all the values, except string
    std::unordered_map<std::string, std::string> attr_item_strings_;
    std::unordered_map<std::string, bool> attr_item_bools_;
    std::unordered_map<std::string, std::vector<bool>> attr_item_bool_list_;
    std::unordered_map<std::string, swss::MacAddress> attr_item_mac_addresses_;
    std::unordered_map<std::string, sai_packet_action_t> attr_item_packet_actions_;
    std::unordered_map<std::string, uint16_t> attr_item_vlan_;
    std::unordered_map<std::string, swss::IpAddress> attr_item_ip_;
    std::unordered_map<std::string, swss::IpPrefix> attr_item_ip_prefix_;
    std::unordered_map<std::string, uint64_t> attr_item_uint_;
    std::unordered_map<std::string, std::set<std::string>> attr_item_set_;
    std::unordered_map<std::string, std::vector<swss::IpAddress>> attr_item_ip_list_;
    std::unordered_map<std::string, std::vector<swss::MacAddress>> attr_item_mac_addresses_list_;
    std::unordered_map<std::string, std::vector<uint64_t>> attr_item_uint_list_;
};

#endif // __REQUEST_PARSER_H
