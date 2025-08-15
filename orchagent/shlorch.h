#ifndef __SHLORCH_H__
#define __SHLORCH_H__

#include "orch.h"
#include "port.h"
#include "observer.h"

#define SHL_VTEPS_LIST "vteps"

typedef enum ShlIsolationGroupStatus
{
    SHL_ISO_GRP_STATUS_RETRY = -100,
    SHL_ISO_GRP_STATUS_FAIL,
    SHL_ISO_GRP_STATUS_INVALID_PARAM,
    SHL_ISO_GRP_STATUS_SUCCESS = 0
} shl_isolation_group_status_t;

class ShlIsolationGroup: public Observer, public Subject
{
public:
    ShlIsolationGroup(string name):
        m_name(name),
        m_oid(SAI_NULL_OBJECT_ID)
    {
    }

    // Create Isolation group in SAI
    shl_isolation_group_status_t create();

    // Delete Isolation group in SAI
    shl_isolation_group_status_t destroy();

    // Add Isolation group member
    shl_isolation_group_status_t addMember(Port &port);

    // Delete Isolation group member
    shl_isolation_group_status_t delMember(Port &port, bool do_fwd_ref=false);

    // Apply the Isolation group to bind port
    shl_isolation_group_status_t bind(string vtep);
    shl_isolation_group_status_t bind(Port &port);

    // Remove the Isolation group from bind port
    shl_isolation_group_status_t unbind(string vtep, bool do_fwd_ref=false);

    long unsigned int getNumOfMembers();
    long unsigned int getNumOfPendingMembers();
    long unsigned int getNumOfBindPorts();
    long unsigned int getNumOfPendingBindports();

    sai_object_id_t getIsolationGroupOid() {
        return m_oid;
    }

    sai_object_id_t getIsolationGroupMemberOid(string port) {
        return m_members[port];
    }

    void notifyObservers(SubjectType type, void *cntx)
    {
        this->notify(type, cntx);
    }

    void update(SubjectType, void *);

    vector<string> getBindPorts()
    {
        return m_bind_ports;
    }

    map<string, sai_object_id_t> getMemberPorts()
    {
        return m_members;
    }

protected:
    sai_object_id_t m_oid;  // sai isolation group object oid
    string m_name;  // represents vtep ip address
    map<string, sai_object_id_t> m_members; // Members {port Name, isolation group member OID}
    vector<string> m_bind_ports; // Port to which this Isolation Group is applied.
    vector<string> m_pending_members;  // tracks the access bridge ports which are not completely setup
    vector<string> m_pending_bind_ports;  // tracks the tunnel bridge ports which are not completely setup
};

class ShlOrch : public Orch, public Observer
{
public:
    ShlOrch(vector<TableConnector> &connectors);

    ~ShlOrch();

    shared_ptr<ShlIsolationGroup>
    getIsolationGroup(string vtep_ip_addr);

    shl_isolation_group_status_t
    addMemberToIsolationGroupPerVtep(vector<string> &addVtepList, Port &port);

    shl_isolation_group_status_t
    delMemberFromIsolationGroupPerVtep(vector<string> &delVtepList, Port &port);

    long unsigned int getIsolationGroupCount();

    long unsigned int getVtepsListCount();

    void update(SubjectType, void *);

    map<string, shared_ptr<ShlIsolationGroup>> getIsolationGroupsList()
    {
        return m_isolationGrps;
    }

private:
    void
    doTask(Consumer &consumer);

    void
    doShlTblTask(Consumer &consumer);

    map<string, shared_ptr<ShlIsolationGroup>> m_isolationGrps;  // {vtep_ip_addr, Isolation group}
    map<string, vector<string>> m_vtep_list;  // {ifname, vtep_list}
};

#endif /* __SHLORCH_H__ */
