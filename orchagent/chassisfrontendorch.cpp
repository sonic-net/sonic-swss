#include "chassisfrontendorch.h"

ChassisFrontendOrch::ChassisFrontendOrch(DBConnector *appDb, const std::vector<std::string> &tableNames) :
    Orch(appDb, tableNames),
    m_chassisInterVrfForwardingIpTable(appDb, APP_CHASSIS_INTER_VRF_FORWARDING_IP_TABLE_NAME)
{
}

void ChassisFrontendOrch::handleVNetRoutesMessage(const std::string & op, const IpPrefix& ipPfx)
{
    SWSS_LOG_ENTER();

    if (op == SET_COMMAND)
    {
        if (m_vNetRouteTable.find(ipPfx) == m_vNetRouteTable.end())
        {
            updateChassisInterVrfForwardingIp(op, ipPfx);
            m_vNetRouteTable.insert(ipPfx);
        }
    }
    else
    {
        if (m_vNetRouteTable.find(ipPfx) != m_vNetRouteTable.end())
        {
            updateChassisInterVrfForwardingIp(op, ipPfx);
            m_vNetRouteTable.erase(ipPfx);
        }
    }
}

void ChassisFrontendOrch::handleMirrorSessionMessage(const std::string & op, const IpAddress& ip)
{
    SWSS_LOG_ENTER();

    if (op == SET_COMMAND)
    {
        if (m_mirrorSessionIpInChassisTable.find(ip) == m_mirrorSessionIpInChassisTable.end())
        {
            updateChassisInterVrfForwardingIp(op, ip);
            m_mirrorSessionIpInChassisTable.insert(ip);
        }
    }
    else
    {
        if (m_mirrorSessionIpInChassisTable.find(ip) != m_mirrorSessionIpInChassisTable.end())
        {
            updateChassisInterVrfForwardingIp(op, ip);
            m_mirrorSessionIpInChassisTable.erase(ip);
        }
    }
}

void ChassisFrontendOrch::updateChassisInterVrfForwardingIp(const std::string & op, const IpAddress& mirrorSessionIp)
{
    SWSS_LOG_ENTER();

    for (const auto & vnetRoute : m_vNetRouteTable)
    {
        if (vnetRoute.isAddressInSubnet(mirrorSessionIp))
        {
            if (op == SET_COMMAND)
            {
                addRouteToChassisInterVrfForwardingIpTable(vnetRoute);
            }
            else
            {
                deleteRouteFromChassisInterVrfForwardingIpTable(vnetRoute);
            }
            break;
        }
    }
}

void ChassisFrontendOrch::updateChassisInterVrfForwardingIp(const std::string & op, const IpPrefix& vNetRoute)
{
    SWSS_LOG_ENTER();

    for (const auto & mirrorSessionIp : m_mirrorSessionIpInChassisTable)
    {
        if (vNetRoute.isAddressInSubnet(mirrorSessionIp))
        {
            if (op == SET_COMMAND)
            {
                addRouteToChassisInterVrfForwardingIpTable(vNetRoute);
            }
            else
            {
                deleteRouteFromChassisInterVrfForwardingIpTable(vNetRoute);
            }
            break;
        }
    }
}

void ChassisFrontendOrch::addRouteToChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx)
{
    SWSS_LOG_ENTER();

    if (m_hasBroadcastedRoute.find(ipPfx) != m_hasBroadcastedRoute.end())
    {
        return;
    }
    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("distribute", "true");
    fvVector.emplace_back("source", "ORCHAGENT_MIRROR_SESSION");
    m_chassisInterVrfForwardingIpTable.set(ipPfx.to_string(), fvVector);
    m_hasBroadcastedRoute.insert(ipPfx);
}

void ChassisFrontendOrch::deleteRouteFromChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx)
{
    SWSS_LOG_ENTER();

    if (m_hasBroadcastedRoute.find(ipPfx) == m_hasBroadcastedRoute.end())
    {
        return;
    }
    m_chassisInterVrfForwardingIpTable.del(ipPfx.to_string());
    m_hasBroadcastedRoute.erase(ipPfx);
}

void ChassisFrontendOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const std::string & tableName = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        if (tableName == APP_MIRROR_SESSION_IP_IN_CHASSIS_TABLE_NAME)
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

