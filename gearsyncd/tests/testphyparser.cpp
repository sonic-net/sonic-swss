/*
 * Copyright 2020 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "phyparser.h"
#include <iostream>
#include <unistd.h>
#include "cunit/Basic.h"
#include "logger.h"

void usage()
{
  std::cout << "Usage: testphyparser" << std::endl;
}

bool handlePhyConfigFile(std::string file, bool warm);

void positiveConfigTest()
{
    bool ret;

    ret = handlePhyConfigFile("tests/configs/positive/phy1_config_1.json", false);
    CU_ASSERT_EQUAL(ret, true);
    ret = handlePhyConfigFile("tests/configs/positive/phy2_config_1.json", false);
    CU_ASSERT_EQUAL(ret, true);
}

void negativeConfigTest()
{
    bool ret;

    ret = handlePhyConfigFile("tests/configs/negative/phy_config_invalid_json.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_missing_lanes.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_system_side.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_local_lane_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_tx_polarity.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_rx_polarity.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_line_tx_lanemap.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_line_rx_lanemap.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_line_to_system_lanemap.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_lanes_missing_mdio_addr.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_mdio_addr.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_system_speed.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_system_fec.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_system_auto_neg.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_system_loopback.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_system_training.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_speed.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_fec.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_auto_neg.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_media_type.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_intf_type.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_loopback.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_training.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_adver_speed.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_adver_fec.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_adver_auto_neg.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_adver_asym_pause.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handlePhyConfigFile("tests/configs/negative/phy_config_ports_missing_line_adver_media_type.json", false);
    CU_ASSERT_EQUAL(ret, false);
}

int unitTestMain()
{
    CU_pSuite pSuite = NULL;
    CU_pTest pTest = NULL;
    int ret = 0;

    if (CUE_SUCCESS != CU_initialize_registry()) 
    {
        printf("%s: cunit failed to initialize registry\n", __FUNCTION__);
        ret = CU_get_error();
    } 
    else 
    {

        CU_basic_set_mode(CU_BRM_VERBOSE);

        // Run the tests and show the run summary
        pSuite = CU_add_suite("phy_config_suite", 0, 0);
        if (NULL == pSuite) 
        {
            printf("%s: cunit failed to create suite\n", __FUNCTION__);
            CU_cleanup_registry();
            ret = CU_get_error();
        }
    }

    // Add the test to the suite
    if (ret == 0) 
    {
        pTest = CU_add_test(pSuite, "phy_positive_config_test", positiveConfigTest);
      
        if (pTest != NULL) 
        { 
            pTest = CU_add_test(pSuite, "phy_negative_config_test", negativeConfigTest);
        }

        if (pTest == NULL) 
        {
            CU_cleanup_registry();
            printf("%s: cunit failed to add test\n", __FUNCTION__);
            ret = CU_get_error();
        }
    }
    if (ret == 0) 
    {
        CU_basic_run_tests();
    }
    return ret;
}

bool handlePhyConfigFile(std::string file, bool warm)
{
    PhyParser p;
    bool ret;

    p.setWriteToDb(true);
    p.setConfigPath(file);
    ret = p.parse();
    return ret;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("testphyparser");

    unitTestMain();
    return 1;
}

