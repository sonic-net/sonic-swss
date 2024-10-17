/*
 * Copyright 2019 Cisco Systems.  The term "Cisco Systems" refers to Cisco Systems Inc.
 * and/or its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <boost/program_options.hpp>

#include "logger.h"

namespace po = boost::program_options;
using namespace std;

void parseArgs(int argc, char **argv)
{
    try {
        string swss_log_level;
        string swss_log_location;
        po::options_description log_desc("Supported logging options");
        log_desc.add_options()
            ("help", "Display program help")
            ("swss-log-level,l", po::value<string>(&swss_log_level)->default_value("NOTICE"),
             "Sets the SWSS logging level. Supported levels are: "
             "EMERG, ALERT, CRIT, ERROR, WARN, NOTICE, INFO, DEBUG")
            ("swss-log-output", po::value<string>(&swss_log_location)->default_value("SYSLOG"),
             "Sends SWSS logs to the desired output stream. Supported locations are: SYSLOG, STDOUT, STDERR")
        ;

        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(log_desc).allow_unregistered().run(), vm);
        po::notify(vm);

        if (vm.count("help")) {
            cout << log_desc << "\n";
        }

        swss::Logger::getInstance().swssPrioNotify("UT", swss_log_level);
        swss::Logger::getInstance().swssOutputNotify("UT", swss_log_location);
    } catch (exception &e) {
        cerr << "Error while parsing arguments: " << e.what() << "\n";
    }
}

int main(int argc, char **argv)
{
    parseArgs(argc, argv);
    testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}
