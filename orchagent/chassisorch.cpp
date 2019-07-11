#include "chassisorch.h"
#include "routeorch.h"

ChassisOrch::ChassisOrch(
    DBConnector* cfgDb,
    DBConnector* applDb, 
    const std::vector<std::string>& tableNames, 
    VNetRouteOrch* vNetRouteOrch) :
    Orch(cfgDb, tableNames),
    m_passThroughRouteTable(applDb, APP_PASS_THROUGH_ROUTE_TABLE_NAME),
    m_vNetRouteOrch(vNetRouteOrch)
{
}

void ChassisOrch::handleMirrorSessionMessage(const std::string & op, const IpAddress& ip)
{
    SWSS_LOG_ENTER();

    if (op == SET_COMMAND)
    {
        m_vNetRouteOrch->attach(this, ip);
    }
    else
    {
        m_vNetRouteOrch->detach(this, ip);
    }
}

void ChassisOrch::update(SubjectType type, void* ctx)
{
    NextHopUpdate* updateInfo = reinterpret_cast<NextHopUpdate *>(ctx);
    if (updateInfo->destination.isZero())
    {
        addRouteToChassisInterVrfForwardingIpTable(updateInfo->prefix);
    }
    else
    {
        deleteRouteFromChassisInterVrfForwardingIpTable(updateInfo->prefix);
    }
}

void ChassisOrch::addRouteToChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx)
{
    SWSS_LOG_ENTER();

    if (m_hasBroadcastedRoute.find(ipPfx) != m_hasBroadcastedRoute.end())
    {
        return;
    }
    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("redistribute", "true");
    fvVector.emplace_back("source", "ORCHAGENT_MIRROR_SESSION");
    m_passThroughRouteTable.set(ipPfx.to_string(), fvVector);
    m_hasBroadcastedRoute.insert(ipPfx);
}

void ChassisOrch::deleteRouteFromChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx)
{
    SWSS_LOG_ENTER();

    if (m_hasBroadcastedRoute.find(ipPfx) == m_hasBroadcastedRoute.end())
    {
        return;
    }
    m_passThroughRouteTable.del(ipPfx.to_string());
    m_hasBroadcastedRoute.erase(ipPfx);
}

void ChassisOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const std::string & tableName = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        if (tableName == CFG_PASS_THROUGH_ROUTE_TABLE_NAME)
        {
            auto t = it->second;
            const std::string & op = kfvOp(t);
            const std::string & ip = kfvKey(t);
            handleMirrorSessionMessage(op, ip);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown table : %s", tableName.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }

}

