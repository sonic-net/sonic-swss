#ifndef __REQUEST_PARSER_H
#define __REQUEST_PARSER_H

typedef enum _request_types_t
{
    REQ_T_BOOL,
    REQ_T_STRING,
    REQ_T_MAC_ADDRESS,
    REQ_T_PACKET_ACTION,
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
    Request(const request_description_t& request_description, const char key_separator)
        : request_description_(request_description),
          key_separator_(key_separator),
          is_parsed_(false),
          number_of_key_items_(request_description.key_item_types.size())
    {
    }

    bool Parse(const SyncMap::iterator& request)
    {
        if (!ParseOperation(request)) return false;
        if (!ParseKey(request)) return false;
        if (!ParseAttrs(request)) return false;

        return true;
    }

    const std::string& getFullKey()
    {
        assert(is_parsed_);
        return full_key_;
    }

    const std::string& getKeyString(int position)
    {
        assert(is_parsed_);
        return key_item_strings_[position];
    }

    const MacAddress& getKeyMacAddress(int position)
    {
        assert(is_parsed_);
        return key_item_mac_addresses_[position];
    }

    const std::vector<std::string> getAttrFieldNames()
    {
        assert(is_parsed_);
        return attr_names_;
    }

    const std::string& getAttrString(const std::string& attr_name)
    {
        return attr_item_strings_[attr_name];
    }

    bool getAttrBool(const std::string& attr_name)
    {
        return attr_item_bools_[attr_name];
    }

    const MacAddress& getAttrMacAddress(const std::string& attr_name)
    {
        return attr_item_mac_addresses_[attr_name];
    }

    sai_packet_action_t getAttrPacketAction(const std::string& attr_name)
    {
        return attr_item_packet_actions_[attr_name];
    }


private:
    bool ParseOperation(const SyncMap::iterator& request);
    bool ParseKey(const SyncMap::iterator& request);
    bool ParseAttrs(const SyncMap::iterator& request);
    bool ParseBool(const std::string& str, bool& value);
    bool ParsePacketAction(const std::string& str, sai_packet_action_t& packet_action);

    request_description_t request_description_;
    SyncMap::iterator request_;
    char key_separator_;
    bool is_parsed_;
    size_t number_of_key_items_;
    std::string full_key_;
    std::vector<std::string> attr_names_;
    std::unordered_map<int, std::string> key_item_strings_;
    std::unordered_map<int, MacAddress> key_item_mac_addresses_;
    std::unordered_map<std::string, std::string> attr_item_strings_;
    std::unordered_map<std::string, bool> attr_item_bools_;
    std::unordered_map<std::string, MacAddress> attr_item_mac_addresses_;
    std::unordered_map<std::string, sai_packet_action_t> attr_item_packet_actions_;
    std::string operation_;
};

#endif // __REQUEST_PARSER_H
