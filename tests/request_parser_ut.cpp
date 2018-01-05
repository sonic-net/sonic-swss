#include <gtest/gtest.h>
#include <unordered_map>
#include <string>
#include <vector>

#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "request_parser.cpp"


const request_description_t request_description1 = {
    { REQ_T_STRING },
    {
        { "v4",            REQ_T_BOOL },
        { "v6",            REQ_T_BOOL },
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "ttl_action",    REQ_T_PACKET_ACTION },
        { "ip_opt_action", REQ_T_PACKET_ACTION },
        { "l3_mc_action",  REQ_T_PACKET_ACTION },
    },
    {} // no mandatory attributes
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
    },
    {"just_string"}
};

class TestRequest2 : public Request
{
public:
    TestRequest2() : Request(request_description2, '|') { }
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
                                     { "l3_mc_action", "log" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_TRUE(r);
    EXPECT_STREQ(request.getOperation().c_str(), "SET");
    EXPECT_STREQ(request.getFullKey().c_str(), "key1");
    EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
    EXPECT_TRUE(request.getAttrFieldNames() == (std::vector<std::string>{"v4", "v6", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action"}));
    EXPECT_TRUE(request.getAttrBool("v4"));
    EXPECT_TRUE(request.getAttrBool("v6"));
    EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "02:03:04:05:06:07");
    EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY);
    EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_DROP);
    EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
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
                                     { "just_string", "test_string"},
                                 }
                             };

    TestRequest2 request;
    
    bool r = request.Parse(t);
    EXPECT_TRUE(r);

    EXPECT_STREQ(request.getOperation().c_str(), "SET");
    EXPECT_STREQ(request.getFullKey().c_str(), "key1|02:03:04:05:06:07|key2");
    EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
    EXPECT_STREQ(request.getKeyMacAddress(1).to_string().c_str(), "02:03:04:05:06:07");
    EXPECT_STREQ(request.getKeyString(2).c_str(), "key2");
    EXPECT_TRUE(request.getAttrFieldNames() == (std::vector<std::string>{"v4", "v6", "src_mac", "ttl_action", "ip_opt_action", "l3_mc_action", "just_string"}));
    EXPECT_FALSE(request.getAttrBool("v4"));
    EXPECT_FALSE(request.getAttrBool("v6"));
    EXPECT_STREQ(request.getAttrMacAddress("src_mac").to_string().c_str(), "02:03:04:05:06:07");
    EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY);
    EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_DROP);
    EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
    EXPECT_STREQ(request.getAttrString("just_string").c_str(), "test_string");
}

TEST(request_parser, deleteOperation)
{
    KeyOpFieldsValuesTuple t {"key1", "DEL",
                                 {
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_TRUE(r);
    EXPECT_STREQ(request.getOperation().c_str(), "DEL");
    EXPECT_STREQ(request.getFullKey().c_str(), "key1");
    EXPECT_STREQ(request.getKeyString(0).c_str(), "key1");
    EXPECT_TRUE(request.getAttrFieldNames() == (std::vector<std::string>{ }));
}

TEST(request_parser, deleteOperationWithAttr)
{
    KeyOpFieldsValuesTuple t {"key1", "DEL",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongOperation)
{
    KeyOpFieldsValuesTuple t {"key1", "ABC",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongkey1)
{
    KeyOpFieldsValuesTuple t {"key1|key2", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongkey2)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest2 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongkeyType1)
{
    KeyOpFieldsValuesTuple t {"key1|key2|key3", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest2 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongAttributeNotFound)
{
    KeyOpFieldsValuesTuple t {"key1|key2", "SET",
                                 {
                                     { "v5", "true" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongRequiredAttribute)
{
    KeyOpFieldsValuesTuple t {"key1|02:03:04:05:06:07|key3", "SET",
                                 {
                                     { "v4", "true" }
                                 }
                             };

    TestRequest2 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongAttrTypeBoolean)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "v4", "true1" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongAttrTypeMac)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "src_mac", "33456" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
}

TEST(request_parser, wrongAttrTypePacketAction)
{
    KeyOpFieldsValuesTuple t {"key1", "SET",
                                 {
                                     { "ttl_action", "something" }
                                 }
                             };

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_FALSE(r);
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

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_TRUE(r);
    EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_DROP);
    EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_FORWARD);
    EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_COPY);
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

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_TRUE(r);
    EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_COPY_CANCEL);
    EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_TRAP);
    EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
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

    TestRequest1 request;

    bool r = request.Parse(t);
    EXPECT_TRUE(r);
    EXPECT_EQ(request.getAttrPacketAction("ttl_action"),    SAI_PACKET_ACTION_DENY);
    EXPECT_EQ(request.getAttrPacketAction("ip_opt_action"), SAI_PACKET_ACTION_TRANSIT);
    EXPECT_EQ(request.getAttrPacketAction("l3_mc_action"),  SAI_PACKET_ACTION_LOG);
}

// FIXME: add a test to test unsupported key type
// FIXME: add a test to check a parser with different key separator