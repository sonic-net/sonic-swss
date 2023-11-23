#ifndef SWSS_VRRPORCH_H
#define SWSS_VRRPORCH_H

#include "orch.h"
#include "portsorch.h"
#include "vrforch.h"
#include "observer.h"

#include "macaddress.h"
#include "vrrpintf.h"

#include <map>

struct VrrpAppl
{
    VrrpIntf vrrp;
    MacAddress vmac;
    bool state;
};

/* vrrp_name, VrrpApple */
using VrrpEntry = map<string, VrrpAppl>;
/* parent_name, VrrpEntry */
using VrrpTable = map<string, VrrpEntry>;

class VrrpOrch : public Orch, public Observer
{
public:
    VrrpOrch(DBConnector *appdb, string tableName, VRFOrch *vrf_orch, PortsOrch *port_orch);
    void update(SubjectType, void *);
    using Orch::doTask;

    const VrrpTable& getSyncdVrrps(void)
    {
        return m_syncdVrrps;
    }

private:
    void doTask(Consumer &consumer);

    VRFOrch *m_vrfOrch;
    PortsOrch *m_portsOrch;

    VrrpTable m_syncdVrrps;
    /* <vrrp_name, vrif_id> */
    map<string, sai_object_id_t> m_vrifs;

    bool setVrrpIntf(const Port &port, const VrrpIntf &vrrp, const MacAddress &vmac, const bool state);
    bool removeVrrpIntf(const Port &port, const VrrpIntf &vrrp);

    bool addVirtualRouterIntf(const Port &port, const MacAddress &vmac, sai_object_id_t& vrif_id);
    bool removeVirtualRouterIntf(const Port &port, const sai_object_id_t vrif_id);

    // bool setVrrpIntf(const string &alias, sai_object_id_t vrf_id);
    // bool removeVrrpIntf(const string &alias, sai_object_id_t vrf_id);
};

#endif /* SWSS_VRRPORCH_H */
