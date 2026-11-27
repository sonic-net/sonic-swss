#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

#include "gtest/gtest.h"
#include "gearboxparser.h"
#include "gearboxutils.h"
#include "mock_table.h"

namespace
{
class GearboxPortStatsConfigTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        testing_db::reset();
        char configPath[] = "/tmp/gearbox-stats-config-XXXXXX";
        char phyPath[] = "/tmp/gearbox-stats-phy-XXXXXX";
        int configFd = mkstemp(configPath);
        ASSERT_NE(configFd, -1);
        close(configFd);
        config_path = configPath;
        int phyFd = mkstemp(phyPath);
        ASSERT_NE(phyFd, -1);
        close(phyFd);
        phy_path = phyPath;

        std::ofstream phyFile(phy_path);
        phyFile << R"({
            "lanes": [{"index": 0, "local_lane_id": 0, "system_side": true,
                "tx_polarity": 0, "rx_polarity": 0, "line_tx_lanemap": 0,
                "line_rx_lanemap": 0, "line_to_system_lanemap": 0, "mdio_addr": "0"}],
            "ports": [{"index": 0, "mdio_addr": "0", "system_speed": 25000,
                "system_fec": "rs", "system_auto_neg": false,
                "system_loopback": "none", "system_training": false,
                "line_speed": 25000, "line_fec": "rs", "line_auto_neg": false,
                "line_media_type": "fiber", "line_intf_type": "cr",
                "line_loopback": "none", "line_training": false,
                "line_adver_speed": [25000], "line_adver_fec": [1],
                "line_adver_auto_neg": false, "line_adver_asym_pause": false,
                "line_adver_media_type": "fiber"}]
        })";
        ASSERT_TRUE(phyFile.good());

        auto phy = json::parse(R"({"phy_id": 1, "name": "test-phy", "address": "0",
            "lib_name": "libsai.so", "firmware_path": "", "sai_init_config_file": "",
            "phy_access": "mdio", "bus_id": 0, "context_id": 1, "hwinfo": ""})");
        phy["config_file"] = phy_path;
        auto other = phy;
        other["phy_id"] = 2;
        config = {{"phys", json::array({phy, other})},
                  {"interfaces", json::array({{{"name", "Ethernet0"}, {"index", 0},
                      {"phy_id", 1}, {"system_lanes", {0}}, {"line_lanes", {1}}}})}};
    }

    void TearDown() override
    {
        if (!config_path.empty())
        {
            std::remove(config_path.c_str());
        }
        if (!phy_path.empty())
        {
            std::remove(phy_path.c_str());
        }
        testing_db::reset();
    }

    bool parse()
    {
        std::ofstream file(config_path);
        file << config;
        file.close();
        GearboxParser parser;
        parser.setWriteToDb(true);
        parser.setConfigPath(config_path);
        return parser.parse();
    }

    std::map<int, swss::gearbox_phy_t> loadPhys()
    {
        swss::DBConnector db("APPL_DB", 0);
        // ProducerStateTable's mock stores records in its logical table name.
        swss::Table table(&db, "GEARBOX_TABLE");
        swss::GearboxUtils utils;
        return utils.loadPhyMap(&table);
    }

    std::string config_path;
    std::string phy_path;
    json config;
};

TEST_F(GearboxPortStatsConfigTest, DefaultsToBothSides)
{
    ASSERT_TRUE(parse());
    const auto phys = loadPhys();
    ASSERT_EQ(phys.size(), 2u);
    EXPECT_EQ(phys.at(1).port_stats_supported_sides, "both");
    EXPECT_EQ(phys.at(2).port_stats_supported_sides, "both");
}

TEST_F(GearboxPortStatsConfigTest, LineOnlyDoesNotAffectOtherPhys)
{
    config["phys"][0]["port_stats_supported_sides"] = "line";
    ASSERT_TRUE(parse());
    const auto phys = loadPhys();
    EXPECT_EQ(phys.at(1).port_stats_supported_sides, "line");
    EXPECT_EQ(phys.at(2).port_stats_supported_sides, "both");
}

TEST_F(GearboxPortStatsConfigTest, RemovingJsonFieldRestoresDefault)
{
    config["phys"][0]["port_stats_supported_sides"] = "line";
    ASSERT_TRUE(parse());
    config["phys"][0].erase("port_stats_supported_sides");
    ASSERT_TRUE(parse());
    EXPECT_EQ(loadPhys().at(1).port_stats_supported_sides, "both");
}

TEST_F(GearboxPortStatsConfigTest, SystemOnlyAndExplicitBothSides)
{
    config["phys"][0]["port_stats_supported_sides"] = "system";
    config["phys"][1]["port_stats_supported_sides"] = "both";
    ASSERT_TRUE(parse());
    const auto phys = loadPhys();
    EXPECT_EQ(phys.at(1).port_stats_supported_sides, "system");
    EXPECT_EQ(phys.at(2).port_stats_supported_sides, "both");
}

TEST_F(GearboxPortStatsConfigTest, RejectsMalformedJsonCapabilities)
{
    const std::vector<json> invalid = {
        nullptr, false, 1, "", "host", "LINE", " line", "line,system", "both,line",
        json::object(), json::array(), json::array({"line"}),
        json::array({"system", "line"}), json::array({"both"}), json::array({"line", 1})
    };
    for (const auto &value : invalid)
    {
        SCOPED_TRACE(value.dump());
        config["phys"][0]["port_stats_supported_sides"] = value;
        EXPECT_FALSE(parse());
    }
}

TEST_F(GearboxPortStatsConfigTest, LoadsConfigDbStyleCapability)
{
    swss::DBConnector db("APPL_DB", 0);
    swss::Table table(&db, "GEARBOX_TABLE");
    for (const std::string value : {"line", "system", "both"})
    {
        SCOPED_TRACE(value);
        table.set("phy:1", {{"phy_id", "1"}, {"port_stats_supported_sides", value}});
        EXPECT_EQ(loadPhys().at(1).port_stats_supported_sides, value);
    }
}

TEST_F(GearboxPortStatsConfigTest, DefaultsForLegacyDatabaseRecord)
{
    swss::DBConnector db("APPL_DB", 0);
    swss::Table table(&db, "GEARBOX_TABLE");
    table.set("phy:1", {{"phy_id", "1"}});
    EXPECT_EQ(loadPhys().at(1).port_stats_supported_sides, "both");
}

TEST_F(GearboxPortStatsConfigTest, RejectsMalformedDatabaseCapabilities)
{
    swss::DBConnector db("APPL_DB", 0);
    swss::Table table(&db, "GEARBOX_TABLE");
    for (const std::string value : {"", "host", "LINE", " line", "line,system", "system,line", "both,line", "line,line", "line,", "[\"line\"]"})
    {
        SCOPED_TRACE(value);
        table.set("phy:1", {{"phy_id", "1"}, {"port_stats_supported_sides", value}});
        EXPECT_THROW(loadPhys(), std::invalid_argument);
    }
}
}
