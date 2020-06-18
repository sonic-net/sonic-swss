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

#include "gearboxparser.h"
#include <iostream>
#include <unistd.h>
#include "cunit/Basic.h"
#include "logger.h"

void usage()
{
  std::cout << "Usage: testgearparser" << std::endl;
}

bool handleGearboxConfigFile(std::string file, bool warm);

void positiveConfigTest()
{
    bool ret;

    ret = handleGearboxConfigFile("tests/configs/positive/gearbox_config_1.json", false);
    CU_ASSERT_EQUAL(ret, true);
}

void negativeConfigTest()
{
    bool ret;

    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_invalid_array.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_invalid_json.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phys.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_name.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_address.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_lib_name.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_firmware_path.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_config_file.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_sai_init_config_file.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_phy_access.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_phy_bus_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_invalid_phy_config_file.json", false);
    CU_ASSERT_EQUAL(ret, false);

    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_interfaces.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_ethernet_index.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_ethernet_phy_id.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_ethernet_line_lanes.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_missing_ethernet_system_lanes.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_empty_ethernet_system_lanes.json", false);
    CU_ASSERT_EQUAL(ret, false);
    ret = handleGearboxConfigFile("tests/configs/negative/gearbox_config_empty_ethernet_line_lanes.json", false);
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
        pSuite = CU_add_suite("gearbox_config_suite", 0, 0);
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
        pTest = CU_add_test(pSuite, "gearbox_positive_config_test", positiveConfigTest);
        if (pTest != NULL) 
        {
            pTest = CU_add_test(pSuite, "gearbox_negative_config_test", negativeConfigTest);
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

bool handleGearboxConfigFile(std::string file, bool warm)
{
    GearboxParser p;
    bool ret;

    p.setWriteToDb(true);
    p.setConfigPath(file);
    ret = p.parse();
    return ret;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("testgearparser");

    unitTestMain();
    return 1;
}

