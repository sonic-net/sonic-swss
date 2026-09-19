/*
 * Copyright 2024
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

#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <string>
#include "warm_restart.h"
#include "poeparser.h"


static void usage()
{
    std::cout << "Usage: poesyncd [-p poe_config.json]" << std::endl;
    std::cout << "       -p poe_config.json: import poe config" << std::endl;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("poesyncd");
    SWSS_LOG_ENTER();
    std::string config_file;
    int opt;

    while ((opt = getopt(argc, argv, "p:h")) != -1 )
    {
        switch (opt)
        {
        case 'p':
            config_file.assign(optarg);
            break;
        case 'h':
            usage();
            return EXIT_FAILURE;
        default: /* '?' */
            usage();
            return EXIT_FAILURE;
        }
    }

    swss::WarmStart::initialize("poesyncd", "swss");
    swss::WarmStart::checkWarmStart("poesyncd", "swss");

    if (config_file.empty())
    {
        SWSS_LOG_ERROR("Missing PoE config");
        return EXIT_FAILURE;
    }

    try
    {
        PoeParser parser(config_file);
        parser.notifyConfigDone(false);
        if (parser.loadConfig() && parser.storeConfigToDb())
        {
            parser.notifyConfigDone(true);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to parse PoE config");
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("Exception \"%s\" had been thrown in poesyncd daemon", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
