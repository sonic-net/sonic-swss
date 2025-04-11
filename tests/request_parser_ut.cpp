#include <gtest/gtest.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "macaddress.h"
#include "orchagent/orch.h"
#include "orchagent/request_parser.h"

using namespace swss;

const request_description_t request_description1 = {
    { REQ_T_STRING },
    {
        { "v4",            REQ_T_BOOL },
        { "v6",            REQ_T_BOOL },
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "ttl_action",    REQ_T_PACKET_ACTION },
        { "ip_opt_action", REQ_T_PACKET_ACTION },
        { "l3_mc_action",  REQ_T_PACKET_ACTION },
        { "nlist",         REQ_T_SET },
    },
    { } // no mandatory attributes
};

class TestRequest1 : public Request
{
public:
    TestRequest1() : Request(request_description1, '|') { }
};

const request_description_t request_description2 = {
    { REQ_T_STRING, REQ_T_MAC_ADDRESS, REQ_T_STRING },
    {
        { "v4",            REQ_T_BOOL },
        { "v6",            REQ_T_BOOL },
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "ttl_action",    REQ_T_PACKET_ACTION },
        { "ip_opt_action", REQ_T_PACKET_ACTION },
        { "l3_mc_action",  REQ_T_PACKET_ACTION },
        { "just_string",   REQ_T_STRING },
        { "vlan",          REQ_T_VLAN },
    },
    { "just_string" }
};

class TestRequest2 : public Request
{
public:
    TestRequest2() : Request(request_description2, '|') { }
};

const request_description_t request_description3 = {
    { REQ_T_STRING, REQ_T_STRING },
    {
        { "v4",            REQ_T_BOOL },
        { "v6",            REQ_T_BOOL },
        { "nlist",         REQ_T_SET  },
    },
    { }  // no mandatory attributes
};

class TestRequest3 : public Request
{
public:
    TestRequest3() : Request(request_description3, ':') { }
};

TEST(request_parser, simpleKey)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "nlist", "name1" },
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4", "v6", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action", "nlist"}));
        EXPECT_TRUE(request.getAttrBool("v4"));
        EXPECT_TRUE(request.getAttrBool("v6"));
        EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_DROP);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
        EXPECT_TRUE(request.getAttrSet("nlist") == (std::set<std::string>{"name1"}));

    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, simpleKeyEmptyAttrs)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "empty", "empty" },
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_EQ(request.getAttrFieldNames(), std::unordered_set<std::string>());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, complexKey)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "false" },
                                     { "v6", "false" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "test_string" },
                                     { "vlan", "Vlan50" },
                                 }
                             };

    try
    {
        TestRequest2 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1|02:03:04:05:06:07|key2");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_STREQ(request.getKeyString(2).c_str(), "key2");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4", "v6", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action", "just_string", "vlan"}));
        EXPECT_FALSE(request.getAttrBool("v4"));
        EXPECT_FALSE(request.getAttrBool("v6"));
        EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_DROP);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
        EXPECT_STREQ(request.getAttrString("just_string").c_str(), "test_string");
        EXPECT_EQ(request.getAttrVlan("vlan"), 50);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, deleteOperation1)
{
    KeyOpFieldsValuesTuple t {"key1", "DEL",
                                 {
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "DEL");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{ }));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, deleteOperation2)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "DEL",
                                 {
                                 }
                             };

    try
    {
        TestRequest2 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "DEL");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1|02:03:04:05:06:07|key2");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_STREQ(request.getKeyString(2).c_str(), "key2");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{ }));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, deleteOperationWithAttr)
{
    KeyOpFieldsValuesTuple t {"key1", "DEL",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Delete operation request contains attributes");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongOperation)
{
    KeyOpFieldsValuesTuple t {"key1", "ABC",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Wrong operation: ABC");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongkey1)
{
    KeyOpFieldsValuesTuple t {"key1|key2", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Wrong number of key items. Expected 1 item(s). Key: 'key1|key2'");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongkey2)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Wrong number of key items. Expected 3 item(s). Key: 'key1'");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongkeyType1)
{
    KeyOpFieldsValuesTuple t {"key1|key2|key3", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid mac address: key2");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttributeNotFound)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v5", "true" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Unknown attribute name: v5");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongRequiredAttribute)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key3", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Mandatory attribute 'just_string' not found");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeBoolean)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "true1" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Can't parse boolean value 'true1'");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeMac)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "src_mac", "33456" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid mac address: 33456");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypePacketAction)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "something" }
                                 }
                             };

    try
    {
        TestRequest1 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Wrong packet action attribute value 'something'");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeVlan_wrong_name)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "123" },
                                     { "vlan", "Vln10" },
                                 }
                             };
    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid vlan interface: Vln10");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeVlan_out_of_high_bound)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "123" },
                                     { "vlan", "Vlan4095" },
                                 }
                             };
    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::out_of_range";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Out of range vlan id: Vlan4095");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeVlan_out_of_low_bound)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "123" },
                                     { "vlan", "Vlan0" },
                                 }
                             };
    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::out_of_range";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Out of range vlan id: Vlan0");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeVlan_out_of_int_range)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "123" },
                                     { "vlan", "Vlan1000000000000000000" },
                                 }
                             };
    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::out_of_range";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Out of range vlan id: Vlan1000000000000000000");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, wrongAttrTypeVlan_invalid_int)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "true" },
                                     { "v6", "true" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "123" },
                                     { "vlan", "Vlana100" },
                                 }
                             };
    try
    {
        TestRequest2 request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid vlan id: Vlana100");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, correctAttrTypePacketAction1)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "drop" },
                                     { "ip_opt_action", "forward" },
                                     { "l3_mc_action", "copy" },
                                 }
                             };
    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_DROP);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_FORWARD);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_COPY);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, correctAttrTypePacketAction2)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "copy_cancel" },
                                     { "ip_opt_action", "trap" },
                                     { "l3_mc_action", "log" },
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY_CANCEL);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_TRAP);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, emptyAttrValue1)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "nlist", "" },
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_TRUE(request.getAttrSet("nlist") == (std::set<std::string>{}));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, correctAttrTypePacketAction3)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "deny" },
                                     { "ip_opt_action", "transit" },
                                     { "l3_mc_action", "log" },
                                 }
                             };

    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_DENY);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_TRANSIT);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, correctParseAndClear)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "deny" },
                                     { "ip_opt_action", "transit" },
                                     { "l3_mc_action", "log" },
                                 }
                             };
    try
    {
        TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_NO_THROW(request.clear());

        EXPECT_NO_THROW(request.parse(t));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, incorrectParseAndClear)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "deny" },
                                     { "ip_opt_action", "transit" },
                                     { "l3_mc_action", "log" },
                                 }
                             };

    try
    {
         TestRequest1 request;

        EXPECT_NO_THROW(request.parse(t));

        request.parse(t);
        FAIL() << "Expected std::logic_error";
    }
    catch (const std::logic_error& e)
    {
        EXPECT_STREQ(e.what(), "The parser already has a parsed request");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

TEST(request_parser, correctClear)
{
    KeyOpFieldsValuesTuple t1 {"key1|02:03:04:05:06:07|key2", "SET",
                                 {
                                     { "v4", "false" },
                                     { "v6", "false" },
                                     { "src_mac", "02:03:04:05:06:07" },
                                     { "ttl_action", "copy" },
                                     { "ip_opt_action", "drop" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "test_string" },
                                     { "vlan", "Vlan1" },
                                 }
                              };

    KeyOpFieldsValuesTuple t2 {"key3|f2:f3:f4:f5:f6:f7|key4", "SET",
                                 {
                                     { "v4", "true" },
                                     { "src_mac", "f2:f3:f4:f5:f6:f7" },
                                     { "ttl_action", "log" },
                                     { "ip_opt_action", "copy" },
                                     { "l3_mc_action", "log" },
                                     { "just_string", "string" },
                                     { "vlan", "Vlan1024" },
                                 }
                              };

    KeyOpFieldsValuesTuple t3 {"key5|52:53:54:55:56:57|key6", "DEL",
                                 {
                                 }
                             };

    try
    {
        TestRequest2 request;

        // parse t1

        EXPECT_NO_THROW(request.parse(t1));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1|02:03:04:05:06:07|key2");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_STREQ(request.getKeyString(2).c_str(), "key2");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4", "v6", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action", "just_string", "vlan"}));
        EXPECT_FALSE(request.getAttrBool("v4"));
        EXPECT_FALSE(request.getAttrBool("v6"));
        EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "02:03:04:05:06:07");
        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_DROP);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
        EXPECT_STREQ(request.getAttrString("just_string").c_str(), "test_string");
        EXPECT_EQ(request.getAttrVlan("vlan"), 1);

        EXPECT_NO_THROW(request.clear());

        // parse t2

        EXPECT_NO_THROW(request.parse(t2));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key3|f2:f3:f4:f5:f6:f7|key4");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key3");
        EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "f2:f3:f4:f5:f6:f7");
        EXPECT_STREQ(request.getKeyString(2).c_str(), "key4");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action", "just_string", "vlan"}));
        EXPECT_TRUE(request.getAttrBool("v4"));
        EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "f2:f3:f4:f5:f6:f7");
        EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_LOG);
        EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_COPY);
        EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
        EXPECT_STREQ(request.getAttrString("just_string").c_str(), "string");
        EXPECT_EQ(request.getAttrVlan("vlan"), 1024);

        EXPECT_NO_THROW(request.clear());

        // parse t3

        EXPECT_NO_THROW(request.parse(t3));

        EXPECT_STREQ(request.getOperation().c_str(), "DEL");
        EXPECT_STREQ(request.getFullKey().c_str(), "key5|52:53:54:55:56:57|key6");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key5");
        EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "52:53:54:55:56:57");
        EXPECT_STREQ(request.getKeyString(2).c_str(), "key6");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{ }));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, anotherKeySeparator)
{
    KeyOpFieldsValuesTuple t {"key1:key2", "SET",
                                 {
                                     { "v4", "false" },
                                     { "v6", "false" },
                                     { "nlist", "name1,name2" },
                                 }
                             };

    try
    {
        TestRequest3 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1:key2");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(1).c_str(), "key2");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4", "v6", "nlist"}));
        EXPECT_FALSE(request.getAttrBool("v4"));
        EXPECT_FALSE(request.getAttrBool("v6"));
        EXPECT_TRUE(request.getAttrSet("nlist") == (std::set<std::string>{"name1", "name2"}));

    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

const request_description_t request_description4 = {
    { REQ_T_STRING, },
    {
        { "v4",            REQ_T_BOOL },
        { "v5",            REQ_T_NOT_USED },
    },
    { } // no mandatory attributes
};

class TestRequest4 : public Request
{
public:
    TestRequest4() : Request(request_description4, '|') { }
};

TEST(request_parser, notDefinedAttrType)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "false" },
                                     { "v5", "abcde" },
                                 }
                             };
    try
    {
        TestRequest4 request;
        request.parse(t);
        FAIL() << "Expected std::logic_error";
    }
    catch (const std::logic_error& e)
    {
        EXPECT_STREQ(e.what(), "Not implemented attribute type parser for attribute:v5");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

const request_description_t request_description5 = {
    { REQ_T_STRING, REQ_T_NOT_USED},
    {
        { "v4",            REQ_T_BOOL },
    },
    { } // no mandatory attributes
};

class TestRequest5 : public Request
{
public:
    TestRequest5() : Request(request_description5, '|') { }
};

TEST(request_parser, notDefinedKeyType)
{
    KeyOpFieldsValuesTuple t {"key1|abcde", "SET",
                                 {
                                     { "v4", "false" },
                                 }
                             };

    try
    {
        TestRequest5 request;
        request.parse(t);
        FAIL() << "Expected std::logic_error";
    }
    catch (const std::logic_error& e)
    {
        EXPECT_STREQ(e.what(), "Not implemented key type parser. Key 'key1|abcde'. Key item:abcde");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

const request_description_t request_description6 = {
    { REQ_T_IP, REQ_T_UINT },
    {
        { "v4",            REQ_T_BOOL },
    },
    { } // no mandatory attributes
};

class TestRequest6 : public Request
{
public:
    TestRequest6() : Request(request_description6, '|') { }
};

TEST(request_parser, uint_and_ip_keys)
{
    KeyOpFieldsValuesTuple t {"10.1.2.3|12345", "SET",
                                 {
                                     { "v4", "false" },
                                 }
                             };

    try
    {
        TestRequest6 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "10.1.2.3|12345");
        EXPECT_EQ(request.getKeyIpAddress(0), IpAddress("10.1.2.3"));
        EXPECT_EQ(request.getKeyUint(1), 12345);
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"v4"}));
        EXPECT_FALSE(request.getAttrBool("v4"));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

TEST(request_parser, wrong_ip_key)
{
    KeyOpFieldsValuesTuple t {"10-1.2.3|12345", "SET",
                                 {
                                     { "v4", "false" },
                                 }
                             };

    try
    {
        TestRequest6 request;

        request.parse(t);

        FAIL() << "Expected std::invalid_argument error";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid ip address: 10-1.2.3");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

TEST(request_parser, wrong_uint_key_1)
{
    KeyOpFieldsValuesTuple t {"10.1.2.3|a12345", "SET",
                                 {
                                     { "v4", "false" },
                                 }
                             };

    try
    {
        TestRequest6 request;

        request.parse(t);

        FAIL() << "Expected std::invalid_argument error";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid unsigned integer: a12345");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

TEST(request_parser, wrong_uint_key_2)
{
    KeyOpFieldsValuesTuple t {"10.1.2.3|1234555555555555555555", "SET",
                                 {
                                     { "v4", "false" },
                                 }
                             };

    try
    {
        TestRequest6 request;

        request.parse(t);

        FAIL() << "Expected std::invalid_argument error";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(),"Out of range unsigned integer: 1234555555555555555555");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

const request_description_t request_description7 = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    {
        { "ip",            REQ_T_IP },
    },
    { }  // no mandatory attributes
};

class TestRequest7 : public Request
{
public:
    TestRequest7() : Request(request_description7, ':') { }
};

TEST(request_parser, prefix_key1)
{
    KeyOpFieldsValuesTuple t {"Ethernet1:10.1.1.1/24", "SET",
                                 {
                                     { "ip", "20.1.1.1" },
                                 }
                             };

    try
    {
        TestRequest7 request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "Ethernet1:10.1.1.1/24");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "Ethernet1");
        EXPECT_EQ(request.getKeyIpPrefix(1), IpPrefix("10.1.1.1/24"));
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"ip"}));
        EXPECT_EQ(request.getAttrIP("ip"), IpAddress("20.1.1.1"));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

TEST(request_parser, wrong_key_ip_prefix)
{
    KeyOpFieldsValuesTuple t {"Ethernet1:10.1.1.1.1/24", "SET",
                                 {
                                     { "empty" , "empty" },
                                 }
                             };

    try
    {
        TestRequest7 request;

        request.parse(t);

        FAIL() << "Expected std::invalid_argument error";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(),"Invalid ip prefix: 10.1.1.1.1/24");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::logic_error, not other exception";
    }
}

const request_description_t request_description_ipv6_addr = {
    { REQ_T_STRING, REQ_T_IP },
    { },
    { }  // no mandatory attributes
};

class TestRequestIpv6Addr : public Request
{
public:
    TestRequestIpv6Addr() : Request(request_description_ipv6_addr, ':') { }
};

const request_description_t request_description_ipv6_prefix = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    { },
    { }  // no mandatory attributes
};

class TestRequestIpv6Prefix : public Request
{
public:
    TestRequestIpv6Prefix() : Request(request_description_ipv6_prefix, ':') { }
};

const request_description_t request_desc_ipv6_addr_only = {
    { REQ_T_IP },
    { },
    { }
};

class TestRequestIpv6AddrOnly: public Request
{
public:
    TestRequestIpv6AddrOnly() : Request(request_desc_ipv6_addr_only, ':') { }
};

const request_description_t request_desc_ipv6_prefix_only = {
    { REQ_T_IP_PREFIX },
    { },
    { }
};

class TestRequestIpv6PrefixOnly: public Request
{
public:
    TestRequestIpv6PrefixOnly() : Request(request_desc_ipv6_prefix_only, ':') { }
};

std::vector<std::string> ipv6_addresses = {"2001:db8:3c4d:0015:0000:0000:1a2f:1a2b", "2001:db8:3c4d:0015::1a2f:1a2b", "::2001:db8:3c4d:0015:1a2f:1a2b", "2001:db8:3c4d:0015:1a2f:1a2b::", "::"};
std::vector<std::string> ipv6_addresses_invalid = {"2001:db8:0015:0000:1a2f:1a2b", "5552001:db8:3c4d:0015::1a2f:1a2b", "::2001:zdb8:3c4d:0015:1a2f:1a2b", "2001:db8:3c4d:0015::::1a2f:1aeer2b::"};
std::vector<std::string> ipv6_prefixes = {"2001:db8:3c4d:0015:0000:0000:1a2f:1a2b/16", "2001:db8:3c4d:0015::1a2f:1a2b/32", "::2001:db8:3c4d:0015:1a2f:1a2b/24", "2001:db8:3c4d:0015:1a2f:1a2b::/8", "::/16"};
std::vector<std::string> ipv6_prefixes_invalid = {"2001:db8:0015:0000:1a2f:1a2b/16", "5552001:db8:3c4d:0015::1a2f:1a2b/32", "::2001:zdb8:3c4d:0015:1a2f:1a2b/24", "2001:db8:3c4d:0015::::1a2f:1aeer2b::/8"};

TEST(request_parser, ipv6_addr_key_item)
{
    for (const std::string &ipv6_addr : ipv6_addresses)
    {
        std::string key_string = "key1:" + ipv6_addr;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Addr request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), key_string.c_str());
            EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
            EXPECT_EQ(request.getKeyIpAddress(1), IpAddress(ipv6_addr));
            EXPECT_FALSE(request.getKeyIpAddress(1).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, ipv6_addr_key_item_empty_str)
{
    for (const std::string &ipv6_addr : ipv6_addresses)
    {
        std::string key_string = ":" + ipv6_addr;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Addr request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), key_string.c_str());
            EXPECT_STREQ(request.getKeyString(0).c_str(), "");
            EXPECT_EQ(request.getKeyIpAddress(1), IpAddress(ipv6_addr));
            EXPECT_FALSE(request.getKeyIpAddress(1).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, ipv6_addr_key_item_only)
{
    for (const std::string &ipv6_addr : ipv6_addresses)
    {
        KeyOpFieldsValuesTuple t {ipv6_addr, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6AddrOnly request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), ipv6_addr.c_str());
            EXPECT_EQ(request.getKeyIpAddress(0), IpAddress(ipv6_addr));
            EXPECT_FALSE(request.getKeyIpAddress(0).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, ipv6_prefix_key_item)
{
    for (const std::string &ipv6_prefix : ipv6_prefixes)
    {
        std::string key_string = "key1:" + ipv6_prefix;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Prefix request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), key_string.c_str());
            EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
            EXPECT_EQ(request.getKeyIpPrefix(1), IpPrefix(ipv6_prefix));
            EXPECT_FALSE(request.getKeyIpPrefix(1).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, ipv6_prefix_key_item_empty_str)
{
    for (const std::string &ipv6_prefix: ipv6_prefixes)
    {
        std::string key_string = ":" + ipv6_prefix;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Prefix request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), key_string.c_str());
            EXPECT_STREQ(request.getKeyString(0).c_str(), "");
            EXPECT_EQ(request.getKeyIpPrefix(1), IpPrefix(ipv6_prefix));
            EXPECT_FALSE(request.getKeyIpPrefix(1).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, ipv6_prefix_key_item_only)
{
    for (const std::string &ipv6_prefix : ipv6_prefixes)
    {
        KeyOpFieldsValuesTuple t {ipv6_prefix, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6PrefixOnly request;

            EXPECT_NO_THROW(request.parse(t));

            EXPECT_STREQ(request.getOperation().c_str(), "SET");
            EXPECT_STREQ(request.getFullKey().c_str(), ipv6_prefix.c_str());
            EXPECT_EQ(request.getKeyIpPrefix(0), IpPrefix(ipv6_prefix));
            EXPECT_FALSE(request.getKeyIpPrefix(0).isV4());
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, invalid_ipv6_prefix_key_item)
{
    for (const std::string &ipv6_prefix : ipv6_prefixes_invalid)
    {
        std::string key_string = "key1:" + ipv6_prefix;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Prefix request;

            EXPECT_THROW(request.parse(t), std::invalid_argument);
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

TEST(request_parser, invalid_ipv6_addr_key_item)
{
    for (const std::string &ipv6_addr : ipv6_addresses_invalid)
    {
        std::string key_string = "key1:" + ipv6_addr;
        KeyOpFieldsValuesTuple t {key_string, "SET",
                                    { }
                                };

        try
        {
            TestRequestIpv6Addr request;

            EXPECT_THROW(request.parse(t), std::invalid_argument);
        }
        catch (const std::exception& e)
        {
            FAIL() << "Got unexpected exception " << e.what();
        }
        catch (...)
        {
            FAIL() << "Got unexpected exception";
        }
    }
}

const request_description_t request_description_multi_value = {
    { REQ_T_STRING },
    {
        { "endpoint",    REQ_T_IP_LIST },
        { "vni",         REQ_T_UINT_LIST },
        { "mac_address", REQ_T_MAC_ADDRESS_LIST },
    },
    { } // no mandatory attributes
};

class TestRequestMultiValue : public Request
{
public:
    TestRequestMultiValue() : Request(request_description_multi_value, ':') { }
};

TEST(request_parser, multipleValues)
{
    size_t value_size = 3;
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "endpoint", "1.1.1.1,2.2.2.2,3.3.3.3" },
                                     { "vni", "11111,11112,11113" },
                                     { "mac_address", "02:03:04:05:06:07,12:13:14:15:16:17,22:23:24:25:26:27" },
                                 }
                             };

    try
    {
        TestRequestMultiValue request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"endpoint", "vni", "mac_address"}));
        auto ep_list = request.getAttrIPList("endpoint");
        auto vni_list = request.getAttrUintList("vni");
        auto mac_list = request.getAttrMacAddressList("mac_address");
        std::vector<std::string> expected_ep{ "1.1.1.1", "2.2.2.2", "3.3.3.3" };
        std::vector<uint64_t> expected_vni{ 11111, 11112, 11113 };
        std::vector<std::string> expected_mac{ "02:03:04:05:06:07", "12:13:14:15:16:17", "22:23:24:25:26:27" };
        EXPECT_EQ(ep_list.size(), value_size);
        EXPECT_EQ(vni_list.size(), value_size);
        EXPECT_EQ(mac_list.size(), value_size);
        for (size_t idx = 0; idx < value_size; idx++)
        {
            EXPECT_STREQ(ep_list[idx].to_string().c_str(), expected_ep[idx].c_str());
            EXPECT_EQ(vni_list[idx], expected_vni[idx]);
            EXPECT_STREQ(mac_list[idx].to_string().c_str(), expected_mac[idx].c_str());
        }
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, multipleValuesIpv6)
{
    size_t value_size = 3;
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "endpoint", "fc00::72,fc00::76,fc00::7a" },
                                     { "vni", "11111,11112,11113" },
                                     { "mac_address", "02:03:04:05:06:07,12:13:14:15:16:17,22:23:24:25:26:27" },
                                 }
                             };

    try
    {
        TestRequestMultiValue request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "key1");
        EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
        EXPECT_TRUE(request.getAttrFieldNames() == (std::unordered_set<std::string>{"endpoint", "vni", "mac_address"}));
        auto ep_list = request.getAttrIPList("endpoint");
        auto vni_list = request.getAttrUintList("vni");
        auto mac_list = request.getAttrMacAddressList("mac_address");
        std::vector<std::string> expected_ep{ "fc00::72", "fc00::76", "fc00::7a" };
        std::vector<uint64_t> expected_vni{ 11111, 11112, 11113 };
        std::vector<std::string> expected_mac{ "02:03:04:05:06:07", "12:13:14:15:16:17", "22:23:24:25:26:27" };
        EXPECT_EQ(ep_list.size(), value_size);
        EXPECT_EQ(vni_list.size(), value_size);
        EXPECT_EQ(mac_list.size(), value_size);
        for (size_t idx = 0; idx < value_size; idx++)
        {
            EXPECT_STREQ(ep_list[idx].to_string().c_str(), expected_ep[idx].c_str());
            EXPECT_EQ(vni_list[idx], expected_vni[idx]);
            EXPECT_STREQ(mac_list[idx].to_string().c_str(), expected_mac[idx].c_str());
        }
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}

TEST(request_parser, multipleValuesInvalidIP)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "endpoint", "1.1.1.1,invalid_ip,3.3.3.3" },
                                     { "vni", "11111,11112,11113" },
                                     { "mac_address", "02:03:04:05:06:07,12:13:14:15:16:17,22:23:24:25:26:27" },
                                 }
                             };

    try
    {
        TestRequestMultiValue request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid ip address list: 1.1.1.1,invalid_ip,3.3.3.3");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

TEST(request_parser, multipleValuesInvalidMac)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "endpoint", "1.1.1.1,2.2.2.2,3.3.3.3" },
                                     { "vni", "11111,11112,11113" },
                                     { "mac_address", "02:03:04:05:06:07,invalid_mac,22:23:24:25:26:27" },
                                 }
                             };

    try
    {
        TestRequestMultiValue request;
        request.parse(t);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument& e)
    {
        EXPECT_STREQ(e.what(), "Invalid mac address list: 02:03:04:05:06:07,invalid_mac,22:23:24:25:26:27");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, not other exception";
    }
}

/*
Check MAC key
*/
const request_description_t test_mac_key = {
    { REQ_T_STRING, REQ_T_MAC_ADDRESS },
    {
        { "f1",               REQ_T_STRING }
    },
    { }
};

class TestRequestMacKey: public Request
{
public:
    TestRequestMacKey() : Request(test_mac_key, ':') { }
};

TEST(request_parser, mac_key_parse_checker)
{
    KeyOpFieldsValuesTuple t {"Vnet_1000:ab:b1:ca:dd:e1:f2", "SET",
                                { }
                            };

    try
    {
        TestRequestMacKey request;

        EXPECT_NO_THROW(request.parse(t));

        EXPECT_STREQ(request.getOperation().c_str(), "SET");
        EXPECT_STREQ(request.getFullKey().c_str(), "Vnet_1000:ab:b1:ca:dd:e1:f2");
        EXPECT_EQ(request.getKeyString(0), "Vnet_1000");
        EXPECT_EQ(request.getKeyMacAddress(1), MacAddress("ab:b1:ca:dd:e1:f2"));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Got unexpected exception " << e.what();
    }
    catch (...)
    {
        FAIL() << "Got unexpected exception";
    }
}
