#pragma once

extern "C" {
#include "sai.h"
}

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "observer.h"

using namespace std;

/* TODO: move all acltable and aclrule implementation out of aclorch */

#define STAGE_INGRESS     "INGRESS"
#define STAGE_EGRESS      "EGRESS"
#define TABLE_INGRESS     STAGE_INGRESS
#define TABLE_EGRESS      STAGE_EGRESS
#define TABLE_STAGE       "STAGE"

typedef enum
{
    ACL_STAGE_UNKNOWN,
    ACL_STAGE_INGRESS,
    ACL_STAGE_EGRESS
} acl_stage_type_t;

typedef enum
{
    ACL_TABLE_UNKNOWN,
    ACL_TABLE_L3,
    ACL_TABLE_L3V6,
    ACL_TABLE_MIRROR,
    ACL_TABLE_MIRRORV6,
    ACL_TABLE_MIRROR_DSCP,
    ACL_TABLE_PFCWD,
    ACL_TABLE_CTRLPLANE,
    ACL_TABLE_DTEL_FLOW_WATCHLIST,
    ACL_TABLE_DTEL_DROP_WATCHLIST
} acl_table_type_t;

typedef map<string, acl_stage_type_t> acl_stage_type_lookup_t;

class AclOrch;
class AclRule;

class AclTable {
    sai_object_id_t m_oid;
    AclOrch *m_pAclOrch;
public:
    string id;
    string description;
    acl_table_type_t type;
    acl_stage_type_t stage;

    // Map port oid to group member oid
    std::map<sai_object_id_t, sai_object_id_t> ports;
    // Map rule name to rule data
    map<string, shared_ptr<AclRule>> rules;
    // Set to store the ACL table port alias
    set<string> portSet;
    // Set to store the not cofigured ACL table port alias
    set<string> pendingPortSet;

    AclTable()
        : m_pAclOrch(NULL)
        , type(ACL_TABLE_UNKNOWN)
        , m_oid(SAI_NULL_OBJECT_ID)
        , stage(ACL_STAGE_INGRESS)
    {}

    AclTable(AclOrch *aclOrch)
        : m_pAclOrch(aclOrch)
        , type(ACL_TABLE_UNKNOWN)
        , m_oid(SAI_NULL_OBJECT_ID)
        , stage(ACL_STAGE_INGRESS)
    {}

    sai_object_id_t getOid() { return m_oid; }
    string getId() { return id; }
    bool validate();
    bool create();

    // Bind the ACL table to a port which is alread linked
    bool bind(sai_object_id_t portOid);
    // Unbind the ACL table to a port which is alread linked
    bool unbind(sai_object_id_t portOid);
    // Bind the ACL table to all ports linked
    bool bind();
    // Unbind the ACL table to all ports linked
    bool unbind();
    // Link the ACL table with a port, for future bind or unbind
    void link(sai_object_id_t portOid);
    // Add or overwrite a rule into the ACL table
    bool add(shared_ptr<AclRule> newRule);
    // Remove a rule from the ACL table
    bool remove(string rule_id);
    // Remove all rules from the ACL table
    bool clear();
    // Update table subject to changes
    void update(SubjectType, void *);
};
