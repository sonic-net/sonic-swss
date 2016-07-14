#ifndef SWSS_TUNNELDECAPORCH_H
#define SWSS_TUNNELDECAPORCH_H

#include <arpa/inet.h>

#include "orch.h"
#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"

class TunnelDecapOrch : public Orch
{
public:
    TunnelDecapOrch(DBConnector *db, string tableName);

private:

    bool addDecapTunnel(string type, IpAddresses dst_ip, string dscp, string ecn, string ttl);

    void doTask(Consumer& consumer);
};
#endif
