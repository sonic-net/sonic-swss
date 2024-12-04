#include "dpuinfoprovider.h"

#include "dbconnector.h"
#include "table.h"
#include "logger.h"
#include "tokenize.h"

#include <boost/algorithm/string/join.hpp>
#include <nlohmann/json.hpp>

using namespace std;
using namespace swss;
using json = nlohmann::json;

bool getDpuInfo(vector<DpuInfo> &info)
{
    DBConnector db("CONFIG_DB", 0);
    auto bridge_table = Table(&db, CFG_MID_PLANE_BRIDGE_TABLE_NAME);
    auto dpus_table = Table(&db, CFG_DPUS_TABLE_NAME);
    auto dhcp_table = Table(&db, CFG_DHCP_SERVER_IPV4_TABLE_NAME);

    string bridge;
    bridge_table.hget("GLOBAL", "bridge", bridge);
    if (bridge.empty())
    {
        SWSS_LOG_ERROR("Failed to get brdige info from %s", CFG_MID_PLANE_BRIDGE_TABLE_NAME);
        return false;
    }

    vector<string> dpus;
    dpus_table.getKeys(dpus);
    if (dpus.empty())
    {
        SWSS_LOG_ERROR("Failed get DPU list from %s table", CFG_DPUS_TABLE_NAME);
        return false;
    }

    sort(dpus.begin(), dpus.end());

    for (uint32_t dpuId = 0; dpuId < dpus.size(); dpuId++)
    {
        string dpuMidplane;
        dpus_table.hget(dpus[dpuId], "midplane_interface", dpuMidplane);
        if (dpuMidplane.empty())
        {
            SWSS_LOG_ERROR("Failed get DPU midplane for %s", dpus[dpuId].c_str());
            return false;
        }

        string dpuInterfacesJson;
        dpus_table.hget(dpus[dpuId], "interface", dpuInterfacesJson);
        if (dpuInterfacesJson.empty())
        {
            SWSS_LOG_ERROR("Failed get DPU interface for %s", dpus[dpuId].c_str());
            return false;
        }

        replace(dpuInterfacesJson.begin(), dpuInterfacesJson.end(), '\'', '"');

        string dpuInterface;
        auto parsed = json::parse(dpuInterfacesJson);
        if (parsed.is_discarded())
        {
            SWSS_LOG_ERROR("Failed to parse DPU interfaces from %s", dpuInterfacesJson.c_str());
            return false;
        }

        vector<string> interfaces;
        std::transform(parsed.items().begin(), parsed.items().end(), std::back_inserter(interfaces),
                        [](const auto& j) { return j.key(); });

        auto interfacesJoined = boost::algorithm::join(interfaces, ",");

        string dpuIpKey = bridge + "|" + dpuMidplane;
        string dpuIps;
        dhcp_table.hget(dpuIpKey, "ips@", dpuIps);

        auto dpuIpsList = tokenize(dpuIps, ',');
        if (dpuIpsList.empty())
        {
            SWSS_LOG_ERROR("Failed get DPU management IP for %s", dpus[dpuId].c_str());
            return false;
        }

        info.push_back(DpuInfo{dpuId, dpuIpsList[0], interfacesJoined});
    }

    return true;
}
