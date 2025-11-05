extern "C" {
#include <sai.h>
#include <saistatus.h>
}


#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

#include "orch.h"

class RouteOrch
{
public:
    RouteOrch();

    void addLinkLocalRouteToMe(sai_object_id_t vrf_id, swss::IpPrefix linklocal_prefix);
    void delLinkLocalRouteToMe(sai_object_id_t vrf_id, swss::IpPrefix linklocal_prefix);
};

RouteOrch::RouteOrch() {};
void RouteOrch::addLinkLocalRouteToMe(sai_object_id_t vrf_id, swss::IpPrefix linklocal_prefix) {};
void RouteOrch::delLinkLocalRouteToMe(sai_object_id_t vrf_id, swss::IpPrefix linklocal_prefix) {};
