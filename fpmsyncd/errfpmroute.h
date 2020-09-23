/*
 * Copyright 2019 Broadcom Inc.
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


#ifndef SWSS_ERRFPMROUTE_H
#define SWSS_ERRFPMROUTE_H

#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "subscriberstatetable.h"
#include "warmRestartHelper.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include <string>
#include "fpmsyncd/routesync.h"
#include "errorlistener.h"
using namespace std; 
namespace swss {
class ErrFpmRoute {
public:
    ErrFpmRoute(DBConnector *db);
    ErrorListener *routeErrorListener;
    SubscriberStateTable cfgTrigger;
    int m_conn_socket;
    void sendMsg(IpPrefix ip_prefix,  IpAddresses ip_addresses, string alias, bool blackhole, RouteSync &, string strRc);
    void sendImplicitAck(RouteSync &);
    void setFd(int sock_id);
    int addattr_l(struct nlmsghdr *n, unsigned int maxlen, int type,
                  const void *data, unsigned int alen);
    int rta_addattr_l(struct rtattr *rta, unsigned int maxlen, int type,
          const void *data, unsigned int alen);
private:
};
}
#endif /* SWSS_ERRFPMROUTE_H */

