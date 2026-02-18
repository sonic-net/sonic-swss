#include "namelabelmapper.h"
#include <gtest/gtest.h>
#include <limits>
#include "sai_serialize.h"
extern "C" {
#include "sai.h"
}
namespace {
constexpr char* kNextHopObject1 = "NextHop1";
constexpr char* kNextHopObject2 = "NextHop2";
constexpr char* kRouteObject1 = "Route1";
constexpr char* kRouteObject2 = "Route2";
std::string cache_dump =
    R"({
    "SAI_OBJECT_TYPE_NEXT_HOP": {
        "NextHop1": "1111",
        "NextHop2": "2222"
    },
    "SAI_OBJECT_TYPE_ROUTE_ENTRY": {
        "Route1": "3333",
        "Route2": "4444"
    }
})";
TEST(NameLabelMapperTest, MapperTest) {
  NameLabelMapper mapper;
  std::string label1, label2, label3, label4;
  EXPECT_FALSE(
      mapper.allocateLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label1));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label1));
  EXPECT_FALSE(
      mapper.allocateLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, label2));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, label2));
  EXPECT_FALSE(
      mapper.allocateLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1, label3));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1, label3));
  EXPECT_FALSE(
      mapper.allocateLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, label4));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, label4));
  EXPECT_EQ(2, mapper.getNumEntries(SAI_OBJECT_TYPE_NEXT_HOP));
  EXPECT_EQ(2, mapper.getNumEntries(SAI_OBJECT_TYPE_ROUTE_ENTRY));
  EXPECT_TRUE(mapper.existsLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
  EXPECT_TRUE(mapper.existsLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
  EXPECT_TRUE(mapper.existsLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
  EXPECT_TRUE(mapper.existsLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));
  std::string label;
  EXPECT_TRUE(
      mapper.getLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label));
  EXPECT_EQ(label1, label);
  EXPECT_TRUE(
      mapper.getLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, label));
  EXPECT_EQ(label2, label);
  mapper.eraseLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1);
  mapper.eraseLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2);
  mapper.eraseLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1);
  EXPECT_EQ(1, mapper.getNumEntries(SAI_OBJECT_TYPE_NEXT_HOP));
  EXPECT_EQ(0, mapper.getNumEntries(SAI_OBJECT_TYPE_ROUTE_ENTRY));
  EXPECT_FALSE(mapper.existsLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1));
  EXPECT_TRUE(mapper.existsLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
  EXPECT_FALSE(mapper.existsLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1));
  EXPECT_FALSE(mapper.existsLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2));
}
TEST(NameLabelMapperTest, ErrorTest) {
  NameLabelMapper mapper;
  std::string label1;
  EXPECT_FALSE(
      mapper.allocateLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label1));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label1));
  // Set existing Label should fail.
  std::string label2 = "abcdefg";
  EXPECT_FALSE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label2));
  // Get non-existing Label should fail.
  std::string label3;
  EXPECT_FALSE(
      mapper.getLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, label3));
  // Erase non-existing Label should fail.
  EXPECT_FALSE(mapper.eraseLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2));
}
TEST(NameLabelMapperTest, DumpEmptyStateCacheTest) {
  NameLabelMapper mapper;
  std::string msg = mapper.dumpStateCache();
  EXPECT_EQ(msg, "{}");
}
TEST(NameLabelMapperTest, DumpStateCacheTest) {
  NameLabelMapper mapper;
  std::string label1 = "1111", label2 = "2222", label3 = "3333",
              label4 = "4444";
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject1, label1));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_NEXT_HOP, kNextHopObject2, label2));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject1, label3));
  EXPECT_TRUE(
      mapper.setLabel(SAI_OBJECT_TYPE_ROUTE_ENTRY, kRouteObject2, label4));
  std::string msg = mapper.dumpStateCache();
  EXPECT_EQ(msg, cache_dump);
}
}  // namespace
