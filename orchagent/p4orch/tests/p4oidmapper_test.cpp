#include "orchagent/p4orch/p4oidmapper.h"

#include <gtest/gtest.h>

#include <limits>

#include "sai_serialize.h"

extern "C"
{
#include "sai.h"
}

namespace
{

constexpr char *kNextHopObject1 = "NextHop1";
constexpr char *kNextHopObject2 = "NextHop2";
constexpr char *kRouteObject1 = "Route1";
constexpr char *kRouteObject2 = "Route2";
constexpr sai_object_id_t kOid1 = 1;
constexpr sai_object_id_t kOid2 = 2;

std::string convertToDBField(_In_ const sai_object_type_t object_type, _In_ const std::string &key)
{
    return sai_serialize_object_type(object_type) + ":" + key;
}

TEST(P4OidMapperTest, MapperTest)
{
    P4OidMapper mapper;
    EXPECT_TRUE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1));
    EXPECT_TRUE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid2,
                              /*ref_count=*/100));
    EXPECT_TRUE(mapper.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
    EXPECT_TRUE(mapper.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2,
                                   /*ref_count=*/200));

    EXPECT_EQ(2, mapper.getNumEntries(SAI_OBJECT_TYPE_NEXT_HOP));
    EXPECT_EQ(2, mapper.getNumEntries(SAI_OBJECT_TYPE_ROUTE_ENTRY));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));

    sai_object_id_t oid;
    EXPECT_TRUE(mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, &oid));
    EXPECT_EQ(kOid1, oid);
    EXPECT_TRUE(mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, &oid));
    EXPECT_EQ(kOid2, oid);
    EXPECT_TRUE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1).empty());
    EXPECT_TRUE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid2).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid2).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid1).empty());

    uint32_t ref_count;
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, &ref_count));
    EXPECT_EQ(0, ref_count);
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, &ref_count));
    EXPECT_EQ(100, ref_count);
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1, &ref_count));
    EXPECT_EQ(0, ref_count);
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, &ref_count));
    EXPECT_EQ(200, ref_count);
    EXPECT_TRUE(mapper.increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, &ref_count));
    EXPECT_EQ(1, ref_count);
    EXPECT_TRUE(mapper.decreaseRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));
    EXPECT_TRUE(mapper.getRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, &ref_count));
    EXPECT_EQ(199, ref_count);

    EXPECT_TRUE(mapper.decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_TRUE(mapper.eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_EQ(1, mapper.getNumEntries(SAI_OBJECT_TYPE_NEXT_HOP));
    EXPECT_EQ(2, mapper.getNumEntries(SAI_OBJECT_TYPE_ROUTE_ENTRY));
    EXPECT_FALSE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));

    mapper.eraseAllOIDs(SAI_OBJECT_TYPE_ROUTE_ENTRY);
    EXPECT_EQ(1, mapper.getNumEntries(SAI_OBJECT_TYPE_NEXT_HOP));
    EXPECT_EQ(0, mapper.getNumEntries(SAI_OBJECT_TYPE_ROUTE_ENTRY));
    EXPECT_FALSE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_TRUE(mapper.existsOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
    EXPECT_FALSE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
    EXPECT_FALSE(mapper.existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid2).empty());
}

TEST(P4OidMapperTest, ErrorTest)
{
    P4OidMapper mapper;
    EXPECT_TRUE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1));
    EXPECT_TRUE(mapper.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, std::numeric_limits<uint32_t>::max()));

    // Set existing OID should fail.
    EXPECT_FALSE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid2));
    EXPECT_FALSE(mapper.setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));

    // Get non-existing OID should fail.
    sai_object_id_t oid;
    EXPECT_FALSE(mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, &oid));

    // Get OID with nullptr should fail.
    EXPECT_FALSE(mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, nullptr));

    // Get non-existing ref count should fail.
    uint32_t ref_count;
    EXPECT_FALSE(mapper.getRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1, &ref_count));

    // Get ref count with nullptr should fail.
    EXPECT_FALSE(mapper.getRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, nullptr));

    // Erase non-existing OID should fail.
    EXPECT_FALSE(mapper.eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));

    // Erase OID with non-zero ref count should fail.
    EXPECT_FALSE(mapper.eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));

    // Increase max ref count should fail.
    EXPECT_FALSE(mapper.increaseRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));

    // Increase non-existing ref count should fail.
    EXPECT_FALSE(mapper.increaseRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));

    // Decrease zero ref count should fail.
    EXPECT_FALSE(mapper.decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));

    // Decrease non-existing ref count should fail.
    EXPECT_FALSE(mapper.decreaseRefCount(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
}

TEST(P4OidMapperTest, VerifyMapperTest)
{
    P4OidMapper mapper;
    swss::Table table(nullptr, "P4RT_KEY_TO_OID");
    EXPECT_TRUE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1));
    EXPECT_TRUE(mapper.setOID(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid2,
                              /*ref_count=*/100));

    EXPECT_TRUE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1).empty());
    EXPECT_TRUE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid2).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid2).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, kOid1).empty());
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, "invalid", kOid1).empty());

    // Verification should fail if OID in DB mismatches.
    table.hset("", convertToDBField(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1), sai_serialize_object_id(kOid2));
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1).empty());

    // Verification should fail if OID in DB is not found.
    table.hdel("", convertToDBField(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
    EXPECT_FALSE(mapper.verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, kOid1).empty());
}

} // namespace
