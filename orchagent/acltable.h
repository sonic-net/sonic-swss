#ifndef SWSS_ACLTABLE_H
#define SWSS_ACLTABLE_H

extern "C" {
#include "sai.h"
}

#include <set>
#include <string>
#include <vector>
#include <map>

using namespace std;

/* TODO: move all acltable and aclrule implementation out of aclorch */

#define TABLE_INGRESS     "INGRESS"
#define TABLE_EGRESS      "EGRESS"
#define TABLE_STAGE       "STAGE"

typedef enum
{
    ACL_STAGE_UNKNOWN,
    ACL_STAGE_INGRESS,
    ACL_STAGE_EGRESS
} acl_stage_type_t;

typedef map<string, acl_stage_type_t> acl_stage_type_lookup_t;

#endif /* SWSS_ACLTABLE_H */
