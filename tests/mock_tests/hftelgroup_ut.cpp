#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include "high_frequency_telemetry/hftelgroup.h"

namespace hftelgroup_test
{
    using namespace std;

    class HFTelGroupTest : public ::testing::Test {};

    TEST_F(HFTelGroupTest, UpdateObjects_AssignsLabelsStartingFromOne)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4", "Ethernet8"};
        group.updateObjects(names);

        auto &objs = group.getObjects();
        ASSERT_EQ(objs.size(), 3u);

        // Every object should have a label >= 1
        for (const auto &obj : objs)
        {
            EXPECT_GE(obj.second, 1);
            EXPECT_LE(obj.second, 3);
        }

        // Labels should be unique
        set<sai_uint16_t> labels;
        for (const auto &obj : objs)
        {
            labels.insert(obj.second);
        }
        EXPECT_EQ(labels.size(), 3u);
    }

    TEST_F(HFTelGroupTest, UpdateObjects_ClearsPrevious)
    {
        HFTelGroup group("port");

        set<string> names1 = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names1);
        ASSERT_EQ(group.getObjects().size(), 2u);

        set<string> names2 = {"Ethernet8"};
        group.updateObjects(names2);
        ASSERT_EQ(group.getObjects().size(), 1u);
        EXPECT_TRUE(group.isObjectInGroup("Ethernet8"));
        EXPECT_FALSE(group.isObjectInGroup("Ethernet0"));
    }

    TEST_F(HFTelGroupTest, UpdateStatsIDs_MoveSemantics)
    {
        HFTelGroup group("port");

        set<sai_stat_id_t> ids = {100, 200, 300};
        group.updateStatsIDs(move(ids));

        auto &stats = group.getStatsIDs();
        ASSERT_EQ(stats.size(), 3u);
        EXPECT_TRUE(stats.count(100));
        EXPECT_TRUE(stats.count(200));
        EXPECT_TRUE(stats.count(300));
    }

    TEST_F(HFTelGroupTest, UpdateStatsIDs_ReplacePrevious)
    {
        HFTelGroup group("queue");

        set<sai_stat_id_t> ids1 = {10, 20};
        group.updateStatsIDs(move(ids1));
        ASSERT_EQ(group.getStatsIDs().size(), 2u);

        set<sai_stat_id_t> ids2 = {30};
        group.updateStatsIDs(move(ids2));
        ASSERT_EQ(group.getStatsIDs().size(), 1u);
        EXPECT_TRUE(group.getStatsIDs().count(30));
    }

    TEST_F(HFTelGroupTest, IsSameObjects_True)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names);
        EXPECT_TRUE(group.isSameObjects(names));
    }

    TEST_F(HFTelGroupTest, IsSameObjects_DifferentSize)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names);

        set<string> different = {"Ethernet0"};
        EXPECT_FALSE(group.isSameObjects(different));
    }

    TEST_F(HFTelGroupTest, IsSameObjects_DifferentContent)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names);

        set<string> different = {"Ethernet0", "Ethernet8"};
        EXPECT_FALSE(group.isSameObjects(different));
    }

    TEST_F(HFTelGroupTest, IsObjectInGroup)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names);

        EXPECT_TRUE(group.isObjectInGroup("Ethernet0"));
        EXPECT_TRUE(group.isObjectInGroup("Ethernet4"));
        EXPECT_FALSE(group.isObjectInGroup("Ethernet8"));
    }

    TEST_F(HFTelGroupTest, GetObjectNamesAndLabels)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4"};
        group.updateObjects(names);

        auto result = group.getObjectNamesAndLabels();
        auto &obj_names = result.first;
        auto &obj_labels = result.second;
        ASSERT_EQ(obj_names.size(), 2u);
        ASSERT_EQ(obj_labels.size(), 2u);

        // Verify names and labels are paired correctly
        for (size_t i = 0; i < obj_names.size(); ++i)
        {
            auto it = group.getObjects().find(obj_names[i]);
            ASSERT_NE(it, group.getObjects().end());
            EXPECT_EQ(obj_labels[i], to_string(it->second));
        }
    }

    TEST_F(HFTelGroupTest, EmptyGroup)
    {
        HFTelGroup group("port");

        EXPECT_TRUE(group.getObjects().empty());
        EXPECT_TRUE(group.getStatsIDs().empty());
        EXPECT_FALSE(group.isObjectInGroup("anything"));

        set<string> empty_set;
        EXPECT_TRUE(group.isSameObjects(empty_set));
    }

    /*
     * updateObjectsPreservingLabels() (MIXED mode's label-preservation path,
     * HFTelProfile::setObjectNames' counterpart to legacy updateObjects()):
     * a name already tracked keeps its existing label; only genuinely-new
     * names consume a fresh one from start_label upward.
     */
    TEST_F(HFTelGroupTest, PreservingLabels_FirstCall_AllocatesEveryName)
    {
        HFTelGroup group("port");

        set<string> names = {"Ethernet0", "Ethernet4", "Ethernet8"};
        size_t allocated = group.updateObjectsPreservingLabels(names, 1);

        EXPECT_EQ(allocated, 3u);
        ASSERT_EQ(group.getObjects().size(), 3u);
        set<sai_uint16_t> labels;
        for (const auto &obj : group.getObjects())
        {
            EXPECT_GE(obj.second, 1);
            EXPECT_LE(obj.second, 3);
            labels.insert(obj.second);
        }
        EXPECT_EQ(labels.size(), 3u);
    }

    TEST_F(HFTelGroupTest, PreservingLabels_UnchangedNamesKeepTheirLabel)
    {
        HFTelGroup group("port");

        group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet4", "Ethernet8"}, 1);
        auto before = group.getObjects();
        sai_uint16_t label_eth0 = before.at("Ethernet0");
        sai_uint16_t label_eth8 = before.at("Ethernet8");

        // Remove Ethernet4, add Ethernet12. start_label continues from where
        // the profile's m_next_label would be (4, since 3 labels were
        // already allocated) - mirrors how HFTelProfile::setObjectNames
        // calls this.
        size_t allocated =
            group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet8", "Ethernet12"}, 4);

        EXPECT_EQ(allocated, 1u);
        ASSERT_EQ(group.getObjects().size(), 3u);
        EXPECT_FALSE(group.isObjectInGroup("Ethernet4"));
        EXPECT_EQ(group.getObjects().at("Ethernet0"), label_eth0);
        EXPECT_EQ(group.getObjects().at("Ethernet8"), label_eth8);
        EXPECT_EQ(group.getObjects().at("Ethernet12"), 4);
    }

    TEST_F(HFTelGroupTest, PreservingLabels_NoChange_AllocatesNothing)
    {
        HFTelGroup group("port");

        group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet4"}, 1);
        auto before = group.getObjects();

        size_t allocated =
            group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet4"}, 3);

        EXPECT_EQ(allocated, 0u);
        EXPECT_EQ(group.getObjects(), before);
    }

    TEST_F(HFTelGroupTest, PreservingLabels_AllNamesReplaced_AllocatesEveryOne)
    {
        HFTelGroup group("port");

        group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet4"}, 1);

        size_t allocated =
            group.updateObjectsPreservingLabels({"Ethernet8", "Ethernet12"}, 3);

        EXPECT_EQ(allocated, 2u);
        EXPECT_FALSE(group.isObjectInGroup("Ethernet0"));
        EXPECT_FALSE(group.isObjectInGroup("Ethernet4"));
        // Names are visited in std::set (lexicographic) order, so "Ethernet12"
        // is labelled before "Ethernet8"; only require the labels be 3 and 4.
        std::set<sai_uint16_t> labels;
        for (const auto &entry : group.getObjects())
        {
            labels.insert(entry.second);
        }
        EXPECT_EQ(labels, (std::set<sai_uint16_t>{3, 4}));
    }

    TEST_F(HFTelGroupTest, PreservingLabels_RepeatedSmallUpdates_NeverDuplicatesALabel)
    {
        HFTelGroup group("port");
        sai_uint16_t next_label = 1;

        next_label += static_cast<sai_uint16_t>(
            group.updateObjectsPreservingLabels({"Ethernet0", "Ethernet4", "Ethernet8"}, next_label));
        // Simulate 5 incremental single-object churns, the scenario the fix
        // targets: each one should only ever allocate at most one new label.
        vector<set<string>> updates = {
            {"Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"},
            {"Ethernet4", "Ethernet8", "Ethernet12"},
            {"Ethernet4", "Ethernet8", "Ethernet12", "Ethernet16"},
            {"Ethernet8", "Ethernet12", "Ethernet16"},
            {"Ethernet8", "Ethernet12", "Ethernet16", "Ethernet20"},
        };
        for (const auto &names : updates)
        {
            size_t allocated = group.updateObjectsPreservingLabels(names, next_label);
            EXPECT_LE(allocated, 1u);
            next_label += static_cast<sai_uint16_t>(allocated);
        }

        set<sai_uint16_t> labels;
        for (const auto &obj : group.getObjects())
        {
            EXPECT_TRUE(labels.insert(obj.second).second) << "duplicate label " << obj.second;
        }
    }
}
