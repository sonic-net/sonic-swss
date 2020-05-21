/* Copyright(c) 2016-2019 Nephos.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 *  Maintainer: Jim Jiang from nephos
 */
#include <iostream>
#include <fstream>
#include <map>
#include "logger.h"
#include "netdispatcher.h"
#include "select.h"
#include "mclaglink.h"

using namespace std;
using namespace swss;

#define RAPID_TIMEOUT 50
#define SLOW_TIMEOUT 2147483647

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

int main(int argc, char **argv)
{

    Logger::linkToDbNative("mclagsyncd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting mclagsyncd ---");

    while (1)
    {
        try
        {
            Select s;
            int timeout = RAPID_TIMEOUT;

            MclagServerLink serverlink;
            serverlink.m_pSelect = &s;

            s.addSelectable(&serverlink);

            while (true)
            {
                Selectable *temps;

                int ret;
                ret = s.select(&temps, timeout);

                if (ret == Select::ERROR)
                {
                    SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                    continue;
                }

                if (ret == Select::TIMEOUT)
                {
                    vector<MclagLink *>::iterator link_it;
                    for (link_it = serverlink.m_linkList.begin(); link_it != serverlink.m_linkList.end();)
                    {
                        MclagLink *link = (*link_it);
                        link->notifyFdbChange();

                        if (link->m_connectionState == false)
                        {
                            vector<Selectable *> selectables;
                            link_it = serverlink.m_linkList.erase(link_it);
                            s.removeSelectable(link);
                            selectables = link->getFdbGatherSelectables();
                            for(auto it : selectables)
                            {
                                s.removeSelectable(it);
                            }

                            delete link;
                        }
                        else
                            link_it++;
                    }

                    timeout = SLOW_TIMEOUT;

                    continue;
                }

                if (typeid(*temps) == typeid(MclagServerLink) || typeid(*temps) == typeid(MclagLink))
                    continue;

                auto *c = (Executor *)temps;
                c->execute();
                timeout = RAPID_TIMEOUT;
            }
        }
        catch (const exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}

