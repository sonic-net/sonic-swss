/*
 * Copyright 2019 Broadcom.  The term "Broadcom" refers to Broadcom Inc.
 * and/or its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __ISOLATIONGROUPORCH_H__
#define __ISOLATIONGROUPORCH_H__

#include "orch.h"
#include "port.h"
#include "observer.h"

#define ISOLATION_GRP_DESCRIPTION       "DESCRIPTION"
#define ISOLATION_GRP_TYPE              "TYPE"
#define ISOLATION_GRP_PORTS             "PORTS"
#define ISOLATION_GRP_MEMBERS           "MEMBERS"
#define ISOLATION_GRP_TYPE_PORT         "port"
#define ISOLATION_GRP_TYPE_BRIDGE_PORT  "bridge-port"

typedef enum IsolationGroupType
{
    ISOLATION_GROUP_TYPE_INVALID,
    ISOLATION_GROUP_TYPE_PORT,
    ISOLATION_GROUP_TYPE_BRIDGE_PORT
} isolation_group_type_t;

typedef enum IsolationGroupStatus
{
    ISO_GRP_STATUS_RETRY = -100,
    ISO_GRP_STATUS_FAIL,
    ISO_GRP_STATUS_INVALID_PARAM,
    ISO_GRP_STATUS_SUCCESS = 0
}isolation_group_status_t;

class IsolationGroup: public Observer, public Subject
{
public:
    string m_description;

    IsolationGroup(string name, isolation_group_type_t type = ISOLATION_GROUP_TYPE_PORT, string description=""):
        m_name(name),
        m_description(description),
        m_type(type),
        m_oid(SAI_NULL_OBJECT_ID)
    {
    }

    // Create Isolation group in SAI
    isolation_group_status_t create();

    // Delete Isolation group in SAI
    isolation_group_status_t destroy();

    // Add Isolation group member
    isolation_group_status_t addMember(Port &port);

    // Delete Isolation group member
    isolation_group_status_t delMember(Port &port, bool do_fwd_ref=false);

    // Set Isolation group members to the input. May involve adding or deleting members
    isolation_group_status_t setMembers(string ports);

    // Apply the Isolation group to all linked ports
    isolation_group_status_t bind(Port &port);

    // Remove the Isolation group from all linked ports
    isolation_group_status_t unbind(Port &port, bool do_fwd_ref=false);

    // Set Isolation group binding to the input. May involve bind
    isolation_group_status_t setBindPorts(string ports);

    void update(SubjectType, void *);

    isolation_group_type_t
    getType()
    {
        return m_type;
    }

    void notifyObservers(SubjectType type, void *cntx)
    {
        this->notify(type, cntx);
    }

protected:
    string m_name;
    isolation_group_type_t m_type;
    sai_object_id_t m_oid;
    map<string, sai_object_id_t> m_members; // Members Name -> Member OID
    vector<string> m_bind_ports; // Ports in which this Iso Group is applied.
    vector<string> m_pending_members;
    vector<string> m_pending_bind_ports;
};

class IsoGrpOrch : public Orch, public Observer
{
public:
    IsoGrpOrch(vector<TableConnector> &connectors);

    ~IsoGrpOrch();

    shared_ptr<IsolationGroup>
    getIsolationGroup(string name);

    isolation_group_status_t
    addIsolationGroup(string name, isolation_group_type_t type, string descr, string bindPorts, string memPorts);

    isolation_group_status_t
    delIsolationGroup(string name);

    void update(SubjectType, void *);

private:
    void
    doTask(Consumer &consumer);

    void
    doIsoGrpTblTask(Consumer &consumer);

    map<string, shared_ptr<IsolationGroup>> m_isolationGrps;
};

struct IsolationGroupUpdate
{
    IsolationGroup *group;
    bool add;
};


#endif /* __ISOLATIONGROUPORCH_H__ */
