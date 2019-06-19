
#ifndef SWSS_CHASSISFRONTENDORCH_H
#define SWSS_CHASSISFRONTENDORCH_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#define APP_MIRROR_SESSION_IP_IN_CHASSIS_TABLE_NAME "MIRROR_SESSION_IP_IN_CHASSIS_TABLE"
#define APP_CHASSIS_INTER_VRF_FORWARDING_IP_TABLE_NAME "CHASSIS_INTER_VRF_FORWARDING_IP_TABLE"

class ChassisFrontendOrch : public Orch
{
public:
    ChassisFrontendOrch(DBConnector *appDb, const std::vector<std::string> &tableNames);

    void handleVNetRoutesMessage(const std::string & op, const IpPrefix& ipPfx);
    void handleMirrorSessionMessage(const std::string & op, const IpAddress& ip);

private:
    void updateChassisInterVrfForwardingIp(const std::string & op, const IpAddress& mirrorSessionIp);
    void updateChassisInterVrfForwardingIp(const std::string & op, const IpPrefix& vNetRoute);

    void addRouteToChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx);
    void deleteRouteFromChassisInterVrfForwardingIpTable(const IpPrefix& ipPfx);

    virtual void doTask(Consumer &consumer);

    std::set<IpPrefix>  m_vNetRouteTable;
    std::set<IpAddress> m_mirrorSessionIpInChassisTable;
    std::set<IpPrefix>  m_hasBroadcastedRoute;

    ProducerStateTable m_chassisInterVrfForwardingIpTable;

};

#endif
