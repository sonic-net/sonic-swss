#ifndef __REQUEST_PARSER_H
#define __REQUEST_PARSER_H

typedef enum _request_types_t
{
    REQ_T_BOOL,
    REQ_T_STRING,
    REQ_T_MAC_ADDRESS,
    REQ_T_PACKET_ACTION,
    REQ_T_NOT_USED,
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
    void parse(const KeyOpFieldsValuesTuple& request);
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

    const MacAddress& getKeyMacAddress(int position) const
    {
        assert(is_parsed_);
        return key_item_mac_addresses_.at(position);
    }

    const std::vector<std::string>& getAttrFieldNames() const // FIXME: return set
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

    const MacAddress& getAttrMacAddress(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_mac_addresses_.at(attr_name);
    }

    sai_packet_action_t getAttrPacketAction(const std::string& attr_name) const
    {
        assert(is_parsed_);
        return attr_item_packet_actions_.at(attr_name);
    }

protected:
    Request(const request_description_t& request_description, const char key_separator)
        : request_description_(request_description),
          key_separator_(key_separator),
          is_parsed_(false),
          number_of_key_items_(request_description.key_item_types.size())
    {
    }


private:
    void parseOperation(const KeyOpFieldsValuesTuple& request);
    void parseKey(const KeyOpFieldsValuesTuple& request);
    void parseAttrs(const KeyOpFieldsValuesTuple& request);
    bool parseBool(const std::string& str);
    MacAddress parseMacAddress(const std::string& str);
    sai_packet_action_t parsePacketAction(const std::string& str);

    const request_description_t& request_description_;
    char key_separator_;
    bool is_parsed_;

    std::string operation_;
    size_t number_of_key_items_;
    std::string full_key_;
    std::vector<std::string> attr_names_;
    std::unordered_map<int, std::string> key_item_strings_;
    std::unordered_map<int, MacAddress> key_item_mac_addresses_;
    std::unordered_map<std::string, std::string> attr_item_strings_;
    std::unordered_map<std::string, bool> attr_item_bools_;
    std::unordered_map<std::string, MacAddress> attr_item_mac_addresses_;
    std::unordered_map<std::string, sai_packet_action_t> attr_item_packet_actions_;
};

#endif // __REQUEST_PARSER_H
