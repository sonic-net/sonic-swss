#include <inttypes.h>
#include <limits.h>
#include <unordered_map>
#include <algorithm>
#include "aclorch.h"
#include "logger.h"
#include "schema.h"
#include "ipprefix.h"
#include "converter.h"
#include "tokenize.h"
#include "timer.h"
#include "crmorch.h"
#include "sai_serialize.h"

using namespace std;
using namespace swss;

map<acl_range_properties_t, AclRange*> AclRange::m_ranges;
sai_uint32_t AclRule::m_minPriority = 0;
sai_uint32_t AclRule::m_maxPriority = 0;

swss::DBConnector AclOrch::m_countersDb("COUNTERS_DB", 0);
swss::Table AclOrch::m_countersTable(&m_countersDb, "COUNTERS");

extern sai_acl_api_t*    sai_acl_api;
extern sai_port_api_t*   sai_port_api;
extern sai_switch_api_t* sai_switch_api;
extern sai_object_id_t   gSwitchId;
extern PortsOrch*        gPortsOrch;
extern CrmOrch *gCrmOrch;
extern SwitchOrch *gSwitchOrch;
extern string gMySwitchType;

#define MIN_VLAN_ID 1    // 0 is a reserved VLAN ID
#define MAX_VLAN_ID 4095 // 4096 is a reserved VLAN ID

#define STATE_DB_ACL_ACTION_FIELD_IS_ACTION_LIST_MANDATORY "is_action_list_mandatory"
#define STATE_DB_ACL_ACTION_FIELD_ACTION_LIST              "action_list"
#define STATE_DB_ACL_L3V4V6_SUPPORTED                      "supported_L3V4V6"
#define COUNTERS_ACL_COUNTER_RULE_MAP "ACL_COUNTER_RULE_MAP"

#define ACL_COUNTER_DEFAULT_POLLING_INTERVAL_MS 10000 // ms
#define ACL_COUNTER_DEFAULT_ENABLED_STATE false

const int TCP_PROTOCOL_NUM = 6; // TCP protocol number

acl_rule_attr_lookup_t aclMatchLookup =
{
    { MATCH_IN_PORTS,          SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS },
    { MATCH_OUT_PORT,          SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT },
    { MATCH_OUT_PORTS,         SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS },
    { MATCH_SRC_IP,            SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP },
    { MATCH_DST_IP,            SAI_ACL_ENTRY_ATTR_FIELD_DST_IP },
    { MATCH_SRC_IPV6,          SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6 },
    { MATCH_DST_IPV6,          SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6 },
    { MATCH_L4_SRC_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT },
    { MATCH_L4_DST_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT },
    { MATCH_ETHER_TYPE,        SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE },
    { MATCH_VLAN_ID,           SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID },
    { MATCH_IP_PROTOCOL,       SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL },
    { MATCH_NEXT_HEADER,       SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER },
    { MATCH_TCP_FLAGS,         SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS },
    { MATCH_IP_TYPE,           SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE },
    { MATCH_DSCP,              SAI_ACL_ENTRY_ATTR_FIELD_DSCP },
    { MATCH_TC,                SAI_ACL_ENTRY_ATTR_FIELD_TC },
    { MATCH_ICMP_TYPE,         SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE },
    { MATCH_ICMP_CODE,         SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE },
    { MATCH_ICMPV6_TYPE,       SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE },
    { MATCH_ICMPV6_CODE,       SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE },
    { MATCH_L4_SRC_PORT_RANGE, SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE },
    { MATCH_L4_DST_PORT_RANGE, SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE },
    { MATCH_TUNNEL_VNI,        SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI },
    { MATCH_INNER_ETHER_TYPE,  SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE },
    { MATCH_INNER_IP_PROTOCOL, SAI_ACL_ENTRY_ATTR_FIELD_INNER_IP_PROTOCOL },
    { MATCH_INNER_L4_SRC_PORT, SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_SRC_PORT },
    { MATCH_INNER_L4_DST_PORT, SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_DST_PORT },
    { MATCH_BTH_OPCODE,        SAI_ACL_ENTRY_ATTR_FIELD_BTH_OPCODE},
    { MATCH_AETH_SYNDROME,     SAI_ACL_ENTRY_ATTR_FIELD_AETH_SYNDROME}
};

static acl_range_type_lookup_t aclRangeTypeLookup =
{
    { MATCH_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE },
    { MATCH_L4_DST_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE },
};

static acl_bind_point_type_lookup_t aclBindPointTypeLookup =
{
    { BIND_POINT_TYPE_PORT,        SAI_ACL_BIND_POINT_TYPE_PORT },
    { BIND_POINT_TYPE_PORTCHANNEL, SAI_ACL_BIND_POINT_TYPE_LAG  },
};

static acl_rule_attr_lookup_t aclL3ActionLookup =
{
    { ACTION_PACKET_ACTION,                    SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION },
    { ACTION_REDIRECT_ACTION,                  SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT },
    { ACTION_DO_NOT_NAT_ACTION,                SAI_ACL_ENTRY_ATTR_ACTION_NO_NAT },
};

static acl_rule_attr_lookup_t aclMirrorStageLookup =
{
    { ACTION_MIRROR_INGRESS_ACTION, SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS},
    { ACTION_MIRROR_EGRESS_ACTION,  SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS},
};

static acl_rule_attr_lookup_t aclDTelActionLookup =
{
    { ACTION_DTEL_FLOW_OP,                  SAI_ACL_ENTRY_ATTR_ACTION_ACL_DTEL_FLOW_OP },
    { ACTION_DTEL_INT_SESSION,              SAI_ACL_ENTRY_ATTR_ACTION_DTEL_INT_SESSION },
    { ACTION_DTEL_DROP_REPORT_ENABLE,       SAI_ACL_ENTRY_ATTR_ACTION_DTEL_DROP_REPORT_ENABLE },
    { ACTION_DTEL_TAIL_DROP_REPORT_ENABLE,  SAI_ACL_ENTRY_ATTR_ACTION_DTEL_TAIL_DROP_REPORT_ENABLE },
    { ACTION_DTEL_FLOW_SAMPLE_PERCENT,      SAI_ACL_ENTRY_ATTR_ACTION_DTEL_FLOW_SAMPLE_PERCENT },
    { ACTION_DTEL_REPORT_ALL_PACKETS,       SAI_ACL_ENTRY_ATTR_ACTION_DTEL_REPORT_ALL_PACKETS }
};

static acl_rule_attr_lookup_t aclOtherActionLookup =
{
    { ACTION_COUNTER,                       SAI_ACL_ENTRY_ATTR_ACTION_COUNTER}
};

static acl_packet_action_lookup_t aclPacketActionLookup =
{
    { PACKET_ACTION_FORWARD, SAI_PACKET_ACTION_FORWARD },
    { PACKET_ACTION_DROP,    SAI_PACKET_ACTION_DROP },
};

static acl_dtel_flow_op_type_lookup_t aclDTelFlowOpTypeLookup =
{
    { DTEL_FLOW_OP_NOP,                SAI_ACL_DTEL_FLOW_OP_NOP },
    { DTEL_FLOW_OP_POSTCARD,           SAI_ACL_DTEL_FLOW_OP_POSTCARD },
    { DTEL_FLOW_OP_INT,                SAI_ACL_DTEL_FLOW_OP_INT },
    { DTEL_FLOW_OP_IOAM,               SAI_ACL_DTEL_FLOW_OP_IOAM }
};

static acl_stage_type_lookup_t aclStageLookUp =
{
    {STAGE_INGRESS, ACL_STAGE_INGRESS },
    {STAGE_EGRESS,  ACL_STAGE_EGRESS }
};

static const acl_capabilities_t defaultAclActionsSupported =
{
    {
        ACL_STAGE_INGRESS,
        AclActionCapabilities
        {
            {
                SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                SAI_ACL_ACTION_TYPE_MIRROR_INGRESS,
                SAI_ACL_ACTION_TYPE_NO_NAT
            },
            false
        }
    },
    {
        ACL_STAGE_EGRESS,
        AclActionCapabilities
        {
            {
                SAI_ACL_ACTION_TYPE_PACKET_ACTION
            },
            false
        }
    }
};

static acl_table_action_list_lookup_t defaultAclActionList =
{
    {
        // L3
        TABLE_TYPE_L3,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            }
        }
    },
    {
        // L3V6
        TABLE_TYPE_L3V6,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            }
        }
    },
    {
        // L3V4V6
        TABLE_TYPE_L3V4V6,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION,
                    SAI_ACL_ACTION_TYPE_REDIRECT
                }
            }
        }
    },
    {
        // MIRROR
        TABLE_TYPE_MIRROR,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_INGRESS
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_EGRESS
                }
            }
        }
    },
    {
        // MIRRORV6
        TABLE_TYPE_MIRRORV6,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_INGRESS
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_EGRESS
                }
            }
        }
    },
    {
        // MIRROR_DSCP
        TABLE_TYPE_MIRROR_DSCP,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_INGRESS
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_MIRROR_EGRESS
                }
            }
        }
    },
    {
        // TABLE_TYPE_PFCWD
        TABLE_TYPE_PFCWD,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            }
        }
    },
    {
        // MCLAG
        TABLE_TYPE_MCLAG,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            }
        }
    },
    {
        // MUX
        TABLE_TYPE_MUX,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            }
        }
    },
    {
        // DROP
        TABLE_TYPE_DROP,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_ACTION_TYPE_PACKET_ACTION
                }
            }
        }
    }
};

// The match fields for certain ACL table type are not exactly the same between INGRESS and EGRESS.
// For example, we can only match IN_PORT for PFCWD table type at INGRESS.
// Hence we need to specify stage particular matching fields in stageMandatoryMatchFields
static acl_table_match_field_lookup_t stageMandatoryMatchFields =
{
    {
        // TABLE_TYPE_PFCWD
        TABLE_TYPE_PFCWD,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS
                }
            }
        }
    },
    {
        // TABLE_TYPE_DROP
        TABLE_TYPE_DROP,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS
                }
            }
        }
    },
    {
        TABLE_TYPE_L3,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE
                }
            }
        }
    },
    {
        TABLE_TYPE_L3V6,
        {
            {
                ACL_STAGE_INGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE
                }
            },
            {
                ACL_STAGE_EGRESS,
                {
                    SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE
                }
            }
        }
    }
};

static acl_ip_type_lookup_t aclIpTypeLookup =
{
    { IP_TYPE_ANY,         SAI_ACL_IP_TYPE_ANY },
    { IP_TYPE_IP,          SAI_ACL_IP_TYPE_IP },
    { IP_TYPE_NON_IP,      SAI_ACL_IP_TYPE_NON_IP },
    { IP_TYPE_IPv4ANY,     SAI_ACL_IP_TYPE_IPV4ANY },
    { IP_TYPE_NON_IPv4,    SAI_ACL_IP_TYPE_NON_IPV4 },
    { IP_TYPE_IPv6ANY,     SAI_ACL_IP_TYPE_IPV6ANY },
    { IP_TYPE_NON_IPv6,    SAI_ACL_IP_TYPE_NON_IPV6 },
    { IP_TYPE_ARP,         SAI_ACL_IP_TYPE_ARP },
    { IP_TYPE_ARP_REQUEST, SAI_ACL_IP_TYPE_ARP_REQUEST },
    { IP_TYPE_ARP_REPLY,   SAI_ACL_IP_TYPE_ARP_REPLY }
};

static map<sai_acl_counter_attr_t, sai_acl_counter_attr_t> aclCounterLookup =
{
    {SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT,   SAI_ACL_COUNTER_ATTR_BYTES},
    {SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT, SAI_ACL_COUNTER_ATTR_PACKETS},
};

static map<AclObjectStatus, string> aclObjectStatusLookup =
{
    {AclObjectStatus::ACTIVE, "Active"},
    {AclObjectStatus::INACTIVE, "Inactive"},
    {AclObjectStatus::PENDING_CREATION, "Pending creation"},
    {AclObjectStatus::PENDING_REMOVAL, "Pending removal"}
};

static sai_acl_table_attr_t AclEntryFieldToAclTableField(sai_acl_entry_attr_t attr)
{
    if (!IS_ATTR_ID_IN_RANGE(attr, ACL_ENTRY, FIELD))
    {
        SWSS_LOG_THROW("ACL entry attribute is not a in a range of SAI_ACL_ENTRY_ATTR_FIELD_* attribute: %d", attr);
    }
    return static_cast<sai_acl_table_attr_t>(SAI_ACL_TABLE_ATTR_FIELD_START + (attr - SAI_ACL_ENTRY_ATTR_FIELD_START));
}

static sai_acl_action_type_t AclEntryActionToAclAction(sai_acl_entry_attr_t attr)
{
    if (!IS_ATTR_ID_IN_RANGE(attr, ACL_ENTRY, ACTION))
    {
        SWSS_LOG_THROW("ACL entry attribute is not a in a range of SAI_ACL_ENTRY_ATTR_ACTION_* attribute: %d", attr);
    }
    return static_cast<sai_acl_action_type_t>(attr - SAI_ACL_ENTRY_ATTR_ACTION_START);
}

static string getAttributeIdName(sai_object_type_t objectType, sai_attr_id_t attr)
{
    const auto* meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, attr);
    if (!meta)
    {
        SWSS_LOG_THROW("Metadata null pointer returned by sai_metadata_get_attr_metadata");
    }
    return meta->attridname;
}

AclTableMatchInterface::AclTableMatchInterface(sai_acl_table_attr_t matchField):
    m_matchField(matchField)
{
    if (!IS_ATTR_ID_IN_RANGE(m_matchField, ACL_TABLE, FIELD))
    {
        SWSS_LOG_THROW("Invalid match table attribute %d", m_matchField);
    }
}

sai_acl_table_attr_t AclTableMatchInterface::getId() const
{
    return m_matchField;
}

AclTableMatch::AclTableMatch(sai_acl_table_attr_t matchField):
    AclTableMatchInterface(matchField)
{
    const auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_TABLE, getId());
    if (!meta)
    {
        SWSS_LOG_THROW("Failed to get metadata for attribute for SAI_OBJECT_TYPE_ACL_TABLE: attribute %d", getId());
    }

    if (meta->attrvaluetype != SAI_ATTR_VALUE_TYPE_BOOL)
    {
        SWSS_LOG_THROW("This API does not allow to set match with a non boolean value type");
    }
}

sai_attribute_t AclTableMatch::toSaiAttribute()
{
    return sai_attribute_t{
        .id = getId(),
        .value = {
            .booldata = true,
        },
    };
}

bool AclTableMatch::validateAclRuleMatch(const AclRule& rule) const
{
    // no need to validate rule configuration
    return true;
}

AclTableRangeMatch::AclTableRangeMatch(set<sai_acl_range_type_t> rangeTypes):
    AclTableMatchInterface(SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE),
    m_rangeList(rangeTypes.begin(), rangeTypes.end())
{
}

sai_attribute_t AclTableRangeMatch::toSaiAttribute()
{
    return sai_attribute_t{
        .id = getId(),
        .value = {
            .s32list = {
                .count = static_cast<uint32_t>(m_rangeList.size()),
                .list = m_rangeList.data(),
            },
        },
    };
}

bool AclTableRangeMatch::validateAclRuleMatch(const AclRule& rule) const
{
    const auto& rangeConfig = rule.getRangeConfig();
    for (const auto& range: rangeConfig)
    {
        if (find(m_rangeList.begin(), m_rangeList.end(), range.rangeType) == m_rangeList.end())
        {
            SWSS_LOG_ERROR("Range match %s is not supported on table %s",
                sai_metadata_get_acl_range_type_name(range.rangeType), rule.getTableId().c_str());
            return false;
        }
    }

    return true;
}

string AclTableType::getName() const
{
    return m_name;
}

const set<sai_acl_bind_point_type_t>& AclTableType::getBindPointTypes() const
{
    return m_bpointTypes;
}

const map<sai_acl_table_attr_t, shared_ptr<AclTableMatchInterface>>& AclTableType::getMatches() const
{
    return m_matches;
}

const set<sai_acl_action_type_t>& AclTableType::getActions() const
{
    return m_aclAcitons;
}

bool AclTableType::addAction(sai_acl_action_type_t action)
{
    m_aclAcitons.insert(action);
    return true;
}

bool AclTableType::addMatch(shared_ptr<AclTableMatchInterface> match)
{
    m_matches.emplace(match->getId(), match);
    return true;
}

AclTableTypeBuilder& AclTableTypeBuilder::withName(string name)
{
    m_tableType.m_name = name;
    return *this;
}

AclTableTypeBuilder& AclTableTypeBuilder::withBindPointType(sai_acl_bind_point_type_t bpointType)
{
    m_tableType.m_bpointTypes.insert(bpointType);
    return *this;
}

AclTableTypeBuilder& AclTableTypeBuilder::withMatch(shared_ptr<AclTableMatchInterface> match)
{
    m_tableType.m_matches.emplace(match->getId(), match);
    return *this;
}

AclTableTypeBuilder& AclTableTypeBuilder::withAction(sai_acl_action_type_t action)
{
    m_tableType.m_aclAcitons.insert(action);
    return *this;
}

AclTableType AclTableTypeBuilder::build()
{
    auto tableType = m_tableType;
    m_tableType = AclTableType();
    return tableType;
}

bool AclTableTypeParser::parse(const std::string& key,
                               const vector<swss::FieldValueTuple>& fieldValues,
                               AclTableTypeBuilder& builder)
{
    builder.withName(key);

    for (const auto& fieldValue: fieldValues)
    {
        auto field = to_upper(fvField(fieldValue));
        auto value = to_upper(fvValue(fieldValue));

        SWSS_LOG_DEBUG("field %s, value %s", field.c_str(), value.c_str());

        if (field == ACL_TABLE_TYPE_MATCHES)
        {
            if (!parseAclTableTypeMatches(value, builder))
            {
                return false;
            }
        }
        else if (field == ACL_TABLE_TYPE_ACTIONS)
        {
            if (!parseAclTableTypeActions(value, builder))
            {
                return false;
            }
        }
        else if (field == ACL_TABLE_TYPE_BPOINT_TYPES)
        {
            if (!parseAclTableTypeBindPointTypes(value, builder))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown field %s: value %s", field.c_str(), value.c_str());
            return false;
        }
    }

    return true;
}

bool AclTableTypeParser::parseAclTableTypeMatches(const std::string& value, AclTableTypeBuilder& builder)
{
    auto matches = tokenize(value, comma);
    set<sai_acl_range_type_t> saiRangeTypes;

    for (const auto& match: matches)
    {
        auto matchIt = aclMatchLookup.find(match);
        auto rangeMatchIt = aclRangeTypeLookup.find(match);

        if (rangeMatchIt != aclRangeTypeLookup.end())
        {
            auto rangeType = rangeMatchIt->second;
            saiRangeTypes.insert(rangeType);
        }
        else if (matchIt != aclMatchLookup.end())
        {
            auto saiMatch = AclEntryFieldToAclTableField(matchIt->second);
            builder.withMatch(make_unique<AclTableMatch>(saiMatch));
        }
        else
        {
            SWSS_LOG_ERROR("Unknown match %s", match.c_str());
            return false;
        }
    }

    if (!saiRangeTypes.empty())
    {
        builder.withMatch(make_unique<AclTableRangeMatch>(saiRangeTypes));
    }

    return true;
}

bool AclTableTypeParser::parseAclTableTypeActions(const std::string& value, AclTableTypeBuilder& builder)
{
    auto actions = tokenize(value, comma);
    for (const auto& action: actions)
    {
        sai_acl_entry_attr_t saiActionAttr = SAI_ACL_ENTRY_ATTR_ACTION_END;

        auto l3Action = aclL3ActionLookup.find(action);
        auto mirrorAction = aclMirrorStageLookup.find(action);
        auto dtelAction = aclDTelActionLookup.find(action);
        auto otherAction = aclOtherActionLookup.find(action);

        if (l3Action != aclL3ActionLookup.end())
        {
            saiActionAttr = l3Action->second;
        }
        else if (mirrorAction != aclMirrorStageLookup.end())
        {
            saiActionAttr = mirrorAction->second;
        }
        else if (dtelAction != aclDTelActionLookup.end())
        {
            saiActionAttr = dtelAction->second;
        }
        else if (otherAction != aclOtherActionLookup.end())
        {
            saiActionAttr = otherAction->second;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown action %s", action.c_str());
            return false;
        }
        SWSS_LOG_INFO("Added action %s", action.c_str());

        builder.withAction(AclEntryActionToAclAction(saiActionAttr));
    }

    return true;
}

bool AclTableTypeParser::parseAclTableTypeBindPointTypes(const std::string& value, AclTableTypeBuilder& builder)
{
    auto bpointTypes = tokenize(value, comma);
    for (const auto& bpointType: bpointTypes)
    {
        auto bpointIt = aclBindPointTypeLookup.find(bpointType);
        if (bpointIt == aclBindPointTypeLookup.end())
        {
            SWSS_LOG_ERROR("Unknown bind point %s", bpointType.c_str());
            return false;
        }

        auto saiBpointType = bpointIt->second;
        builder.withBindPointType(saiBpointType);
    }

    return true;
}

AclRule::AclRule(AclOrch *pAclOrch, string rule, string table, bool createCounter) :
    m_pAclOrch(pAclOrch),
    m_id(rule),
    m_ruleOid(SAI_NULL_OBJECT_ID),
    m_counterOid(SAI_NULL_OBJECT_ID),
    m_priority(0),
    m_createCounter(createCounter)
{
    auto tableOid = pAclOrch->getTableById(table);
    m_pTable = pAclOrch->getTableByOid(tableOid);
    if (!m_pTable)
    {
        SWSS_LOG_THROW("Failed to find ACL table %s. ACL table must exist at the time of creating AclRule", table.c_str());
    }
}

bool AclRule::validateAddPriority(string attr_name, string attr_value)
{
    bool status = false;

    if (attr_name == RULE_PRIORITY)
    {
        char *endp = NULL;
        errno = 0;
        auto priority = (uint32_t)strtol(attr_value.c_str(), &endp, 0);
        // check conversion was successful and the value is within the allowed range
        status = (errno == 0) &&
                 (endp == attr_value.c_str() + attr_value.size()) &&
                 setPriority(priority);
    }

    return status;
}

bool AclRule::validateAddMatch(string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    sai_acl_field_data_t matchData{};
    vector<sai_object_id_t> inPorts;
    vector<sai_object_id_t> outPorts;

    matchData.enable = true;

    try
    {
        if (aclMatchLookup.find(attr_name) == aclMatchLookup.end())
        {
            return false;
        }
        else if (attr_name == MATCH_IN_PORTS)
        {
            auto ports = tokenize(attr_value, ',');

            if (ports.size() == 0)
            {
                return false;
            }

            for (auto alias : ports)
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    SWSS_LOG_ERROR("Failed to locate port %s", alias.c_str());
                    return false;
                }

                if (port.m_type != Port::PHY)
                {
                    SWSS_LOG_ERROR("Cannot bind rule to %s: IN_PORTS can only match physical interfaces", alias.c_str());
                    return false;
                }

                inPorts.push_back(port.m_port_id);
            }

            matchData.data.objlist.count = static_cast<uint32_t>(inPorts.size());
            matchData.data.objlist.list = inPorts.data();
        }
        else if (attr_name == MATCH_OUT_PORTS)
        {
            auto ports = tokenize(attr_value, ',');

            if (ports.size() == 0)
            {
                return false;
            }

            for (auto alias : ports)
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    SWSS_LOG_ERROR("Failed to locate port %s", alias.c_str());
                    return false;
                }

                if (port.m_type != Port::PHY)
                {
                    SWSS_LOG_ERROR("Cannot bind rule to %s: OUT_PORTS can only match physical interfaces", alias.c_str());
                    return false;
                }

                outPorts.push_back(port.m_port_id);
            }

            matchData.data.objlist.count = static_cast<uint32_t>(outPorts.size());
            matchData.data.objlist.list = outPorts.data();
        }
        else if (attr_name == MATCH_OUT_PORT)
        {
            auto alias = attr_value;
            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate port %s", alias.c_str());
                return false;
            }
            if (port.m_type != Port::PHY)
            {
                SWSS_LOG_ERROR("Cannot bind rule to %s: OUT_PORT can only match physical interfaces", alias.c_str());
                return false;
            }

            matchData.data.oid = port.m_port_id;
        }
        else if (attr_name == MATCH_IP_TYPE)
        {
            if (!processIpType(attr_value, matchData.data.u32))
            {
                SWSS_LOG_ERROR("Invalid IP type %s", attr_value.c_str());
                return false;
            }

            matchData.mask.u32 = 0xFFFFFFFF;
        }
        else if (attr_name == MATCH_TCP_FLAGS)
        {
            // Support both exact value match and value/mask match
            auto flag_data = tokenize(attr_value, '/');

            matchData.data.u8 = to_uint<uint8_t>(flag_data[0], 0, 0x3F);

            if (flag_data.size() == 2)
            {
                matchData.mask.u8 = to_uint<uint8_t>(flag_data[1], 0, 0x3F);
            }
            else
            {
                matchData.mask.u8 = 0x3F;
            }
        }
        else if (attr_name == MATCH_ETHER_TYPE || attr_name == MATCH_L4_SRC_PORT || attr_name == MATCH_L4_DST_PORT)
        {
            matchData.data.u16 = to_uint<uint16_t>(attr_value);
            matchData.mask.u16 = 0xFFFF;
        }
        else if (attr_name == MATCH_VLAN_ID)
        {
            matchData.data.u16 = to_uint<uint16_t>(attr_value);
            matchData.mask.u16 = 0xFFF;

            if (matchData.data.u16 < MIN_VLAN_ID || matchData.data.u16 > MAX_VLAN_ID)
            {
                SWSS_LOG_ERROR("Invalid VLAN ID: %s", attr_value.c_str());
                return false;
            }
        }
        else if (attr_name == MATCH_DSCP)
        {
            /* Support both exact value match and value/mask match */
            auto dscp_data = tokenize(attr_value, '/');

            matchData.data.u8 = to_uint<uint8_t>(dscp_data[0], 0, 0x3F);

            if (dscp_data.size() == 2)
            {
                matchData.mask.u8 = to_uint<uint8_t>(dscp_data[1], 0, 0x3F);
            }
            else
            {
                matchData.mask.u8 = 0x3F;
            }
        }
        else if (attr_name == MATCH_IP_PROTOCOL || attr_name == MATCH_NEXT_HEADER)
        {
            matchData.data.u8 = to_uint<uint8_t>(attr_value);
            matchData.mask.u8 = 0xFF;
        }
        else if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
        {
            IpPrefix ip(attr_value);

            if (!ip.isV4())
            {
                SWSS_LOG_ERROR("IP type is not v4 type");
                return false;
            }
            matchData.data.ip4 = ip.getIp().getV4Addr();
            matchData.mask.ip4 = ip.getMask().getV4Addr();
        }
        else if (attr_name == MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6)
        {
            IpPrefix ip(attr_value);
            if (ip.isV4())
            {
                SWSS_LOG_ERROR("IP type is not v6 type");
                return false;
            }
            memcpy(matchData.data.ip6, ip.getIp().getV6Addr(), 16);
            memcpy(matchData.mask.ip6, ip.getMask().getV6Addr(), 16);
        }
        else if ((attr_name == MATCH_L4_SRC_PORT_RANGE) || (attr_name == MATCH_L4_DST_PORT_RANGE))
        {
            AclRangeConfig rangeConfig{};
            if (sscanf(attr_value.c_str(), "%d-%d", &rangeConfig.min, &rangeConfig.max) != 2)
            {
                SWSS_LOG_ERROR("Range parse error. Attribute: %s, value: %s", attr_name.c_str(), attr_value.c_str());
                return false;
            }

            rangeConfig.rangeType = aclRangeTypeLookup[attr_name];

            // check boundaries
            if ((rangeConfig.min > USHRT_MAX) ||
                (rangeConfig.max > USHRT_MAX) ||
                (rangeConfig.min > rangeConfig.max))
            {
                SWSS_LOG_ERROR("Range parse error. Invalid range value. Attribute: %s, value: %s", attr_name.c_str(), attr_value.c_str());
                return false;
            }

            m_rangeConfig.push_back(rangeConfig);

            return m_pTable->validateAclRuleMatch(SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE, *this);
        }
        else if (attr_name == MATCH_TC)
        {
            matchData.data.u8 = to_uint<uint8_t>(attr_value);
            matchData.mask.u8 = 0xFF;
        }
        else if (attr_name == MATCH_ICMP_TYPE || attr_name == MATCH_ICMP_CODE ||
                attr_name == MATCH_ICMPV6_TYPE || attr_name == MATCH_ICMPV6_CODE)
        {
            matchData.data.u8 = to_uint<uint8_t>(attr_value);
            matchData.mask.u8 = 0xFF;
        }
        else if (attr_name == MATCH_TUNNEL_VNI)
        {
            matchData.data.u32 = to_uint<uint32_t>(attr_value);
            matchData.mask.u32 = 0xFFFFFFFF;
        }
        else if (attr_name == MATCH_INNER_ETHER_TYPE || attr_name == MATCH_INNER_L4_SRC_PORT ||
            attr_name == MATCH_INNER_L4_DST_PORT)
        {
            matchData.data.u16 = to_uint<uint16_t>(attr_value);
            matchData.mask.u16 = 0xFFFF;
        }
        else if (attr_name == MATCH_INNER_IP_PROTOCOL)
        {
            matchData.data.u8 = to_uint<uint8_t>(attr_value);
            matchData.mask.u8 = 0xFF;
        }
        else if (attr_name == MATCH_BTH_OPCODE)
        {
            auto opcode_data = tokenize(attr_value, '/');

            if (opcode_data.size() == 2)
            {
                matchData.data.u8 = to_uint<uint8_t>(opcode_data[0]);
                matchData.mask.u8 = to_uint<uint8_t>(opcode_data[1]);
            }
            else
            {
                SWSS_LOG_ERROR("Invalid BTH_OPCODE configuration: %s, expected format <data>/<mask>", attr_value.c_str());
                return false;
            }
        }
        else if (attr_name == MATCH_AETH_SYNDROME)
        {
            auto syndrome_data = tokenize(attr_value, '/');

            if (syndrome_data.size() == 2)
            {
                matchData.data.u8 = to_uint<uint8_t>(syndrome_data[0]);
                matchData.mask.u8 = to_uint<uint8_t>(syndrome_data[1]);
            }
            else
            {
                SWSS_LOG_ERROR("Invalid AETH_SYNDROME configuration: %s, expected format <data>/<mask>", attr_value.c_str());
                return false;
            }
        }
    }
    catch (exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value. Error: %s", attr_name.c_str(), attr_value.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value.", attr_name.c_str(), attr_value.c_str());
        return false;
    }

    // TODO: For backwards compatibility, users can substitute IP_PROTOCOL for NEXT_HEADER.
    // This should be removed in a future release.
    if ((m_pTable->type.getName() == TABLE_TYPE_MIRRORV6 || m_pTable->type.getName() == TABLE_TYPE_L3V6)
            && attr_name == MATCH_IP_PROTOCOL)
    {
        SWSS_LOG_WARN("Support for IP protocol on IPv6 tables will be removed in a future release, please switch to using NEXT_HEADER instead!");
        attr_name = MATCH_NEXT_HEADER;
    }

    return setMatch(aclMatchLookup[attr_name], matchData);
}

bool AclRule::processIpType(string type, sai_uint32_t &ip_type)
{
    SWSS_LOG_ENTER();

    auto it = aclIpTypeLookup.find(to_upper(type));

    if (it == aclIpTypeLookup.end())
    {
        return false;
    }

    ip_type = it->second;

    return true;
}

bool AclRule::create()
{
    if (m_createCounter && !createCounter())
    {
        return false;
    }

    if (!createRule())
    {
        removeCounter();
        return false;
    }

    return true;
}

bool AclRule::createRule()
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> rule_attrs;
    sai_object_id_t range_objects[2];
    sai_object_list_t range_object_list = {0, range_objects};

    sai_attribute_t attr;
    sai_status_t status;

    // store table oid this rule belongs to
    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = m_pTable->getOid();
    rule_attrs.push_back(attr);

    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = m_priority;
    rule_attrs.push_back(attr);

    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    rule_attrs.push_back(attr);

    // add reference to the counter
    if (m_createCounter)
    {
        attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
        attr.value.aclaction.parameter.oid = m_counterOid;
        attr.value.aclaction.enable = true;
        rule_attrs.push_back(attr);
    }

    if (!m_rangeConfig.empty())
    {
        for (const auto& rangeConfig: m_rangeConfig)
        {
            SWSS_LOG_INFO("Creating range object %u..%u", rangeConfig.min, rangeConfig.max);

            AclRange *range = AclRange::create(rangeConfig.rangeType, rangeConfig.min, rangeConfig.max);
            if (!range)
            {
                // release already created range if any
                AclRange::remove(range_objects, range_object_list.count);
                return false;
            }

            m_ranges.push_back(range);
            range_objects[range_object_list.count++] = range->getOid();
        }

        attr.id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.objlist = range_object_list;
        rule_attrs.push_back(attr);
    }

    // store matches
    for (auto& it : m_matches)
    {
        attr = it.second.getSaiAttr();
        rule_attrs.push_back(attr);
    }

    // store actions
    for (auto& it : m_actions)
    {
        attr = it.second.getSaiAttr();
        rule_attrs.push_back(attr);
    }

    status = sai_acl_api->create_acl_entry(&m_ruleOid, gSwitchId, (uint32_t)rule_attrs.size(), rule_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            SWSS_LOG_NOTICE("ACL rule %s already exists", m_id.c_str());
            return true;
        }
        SWSS_LOG_ERROR("Failed to create ACL rule %s, rv:%d",
                m_id.c_str(), status);
        AclRange::remove(range_objects, range_object_list.count);
        decreaseNextHopRefCount();
    }

    if (status == SAI_STATUS_SUCCESS)
    {
        gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_pTable->getOid());
    }

    return (status == SAI_STATUS_SUCCESS);
}

void AclRule::decreaseNextHopRefCount()
{
    if (!m_redirect_target_next_hop.empty())
    {
        m_pAclOrch->m_neighOrch->decreaseNextHopRefCount(NextHopKey(m_redirect_target_next_hop));
        m_redirect_target_next_hop.clear();
    }
    if (!m_redirect_target_next_hop_group.empty())
    {
        NextHopGroupKey target = NextHopGroupKey(m_redirect_target_next_hop_group);
        m_pAclOrch->m_routeOrch->decreaseNextHopRefCount(target);
        // remove next hop group in case it's not used by anything else
        if (m_pAclOrch->m_routeOrch->isRefCounterZero(target))
        {
            if (m_pAclOrch->m_routeOrch->removeNextHopGroup(target))
            {
                SWSS_LOG_DEBUG("Removed acl redirect target next hop group '%s'", m_redirect_target_next_hop_group.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove unused next hop group '%s'", m_redirect_target_next_hop_group.c_str());
                // FIXME: what else could we do here?
            }
        }
        m_redirect_target_next_hop_group.clear();
    }

    return;
}

bool AclRule::isActionSupported(sai_acl_entry_attr_t action) const
{
    return m_pAclOrch->isAclActionSupported(m_pTable->stage, AclEntryActionToAclAction(action));
}

bool AclRule::removeRule()
{
    SWSS_LOG_ENTER();

    if (m_ruleOid == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    auto status = sai_acl_api->remove_acl_entry(m_ruleOid);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_NOTICE("ACL rule already deleted");
            m_ruleOid = SAI_NULL_OBJECT_ID;
            return true;
        }
        SWSS_LOG_ERROR("Failed to delete ACL rule, status %s", sai_serialize_status(status).c_str());
        return false;
    }

    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_pTable->getOid());

    m_ruleOid = SAI_NULL_OBJECT_ID;

    decreaseNextHopRefCount();

    return true;
}

bool AclRule::remove()
{
    SWSS_LOG_ENTER();

    if (!removeRule())
    {
        return false;
    }

    auto res = removeRanges();
    res &= removeCounter();

    return res;
}

void AclRule::updateInPorts()
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    auto attr = m_matches[SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS].getSaiAttr();
    attr.value.aclfield.enable = true;

    status = sai_acl_api->set_acl_entry_attribute(m_ruleOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update ACL rule %s, rv:%d", m_id.c_str(), status);
    }
}

bool AclRule::update(const AclRule& updatedRule)
{
    SWSS_LOG_ENTER();

    if (!m_rangeConfig.empty() || !updatedRule.m_rangeConfig.empty())
    {
        SWSS_LOG_ERROR("Updating range matches is currently not implemented");
        return false;
    }

    if (!updateCounter(updatedRule))
    {
        return false;
    }

    if (!updatePriority(updatedRule))
    {
        return false;
    }

    if (!updateMatches(updatedRule))
    {
        return false;
    }

    if (!updateActions(updatedRule))
    {
        return false;
    }

    return true;
}

bool AclRule::updateCounter(const AclRule& updatedRule)
{
    if (updatedRule.m_createCounter)
    {
        if (!enableCounter())
        {
            return false;
        }

        m_pAclOrch->registerFlexCounter(*this);
    }
    else
    {
        m_pAclOrch->deregisterFlexCounter(*this);

        if (!disableCounter())
        {
            return false;
        }
    }

    m_createCounter = updatedRule.m_createCounter;

    return true;
}

bool AclRule::updatePriority(const AclRule& updatedRule)
{
    if (m_priority == updatedRule.m_priority)
    {
        return true;
    }

    sai_attribute_t attr {};
    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.s32 = updatedRule.m_priority;
    if (!setAttribute(attr))
    {
        return false;
    }

    m_priority = updatedRule.m_priority;

    return true;
}

bool AclRule::updateMatches(const AclRule& updatedRule)
{
    vector<pair<sai_acl_entry_attr_t, SaiAttrWrapper>> matchesUpdated;
    vector<pair<sai_acl_entry_attr_t, SaiAttrWrapper>> matchesDisabled;

    // Diff by value to get new matches and updated matches
    // in a single set_difference pass.
    set_difference(
        updatedRule.m_matches.begin(),
        updatedRule.m_matches.end(),
        m_matches.begin(), m_matches.end(),
        back_inserter(matchesUpdated)
    );

    // Diff by key only to get delete matches. Assuming that
    // deleted matches mean setting a match attribute to disabled state.
    set_difference(
        m_matches.begin(), m_matches.end(),
        updatedRule.m_matches.begin(),
        updatedRule.m_matches.end(),
        back_inserter(matchesDisabled),
        [](auto& oldMatch, auto& newMatch)
        {
            return oldMatch.first < newMatch.first;
        }
    );

    for (const auto& attrPair: matchesDisabled)
    {
        auto attr = attrPair.second.getSaiAttr();
        attr.value.aclfield.enable = false;
        if (!setAttribute(attr))
        {
            return false;
        }
        m_matches.erase(attrPair.first);
    }

    for (const auto& attrPair: matchesUpdated)
    {
        auto attr = attrPair.second.getSaiAttr();
        if (!setAttribute(attr))
        {
            return false;
        }
        setMatch(attrPair.first, attr.value.aclfield);
    }

    return true;
}

bool AclRule::updateActions(const AclRule& updatedRule)
{
    vector<pair<sai_acl_entry_attr_t, SaiAttrWrapper>> actionsUpdated;
    vector<pair<sai_acl_entry_attr_t, SaiAttrWrapper>> actionsDisabled;

    // Diff by value to get new action and updated actions
    // in a single set_difference pass.
    set_difference(
        updatedRule.m_actions.begin(),
        updatedRule.m_actions.end(),
        m_actions.begin(), m_actions.end(),
        back_inserter(actionsUpdated)
    );

    // Diff by key only to get delete actions. Assuming that
    // action matches mean setting a action attribute to disabled state.
    set_difference(
        m_actions.begin(), m_actions.end(),
        updatedRule.m_actions.begin(),
        updatedRule.m_actions.end(),
        back_inserter(actionsDisabled),
        [](auto& oldAction, auto& newAction)
        {
            return oldAction.first < newAction.first;
        }
    );

    for (const auto& attrPair: actionsDisabled)
    {
        auto attr = attrPair.second.getSaiAttr();
        attr.value.aclaction.enable = false;
        if (!setAttribute(attr))
        {
            return false;
        }
        m_actions.erase(attrPair.first);
    }

    for (const auto& attrPair: actionsUpdated)
    {
        auto attr = attrPair.second.getSaiAttr();
        if (!setAttribute(attr))
        {
            return false;
        }
        setAction(attrPair.first, attr.value.aclaction);
    }

    return true;
}

bool AclRule::setPriority(const sai_uint32_t &value)
{
    if (!(value >= m_minPriority && value <= m_maxPriority))
    {
        SWSS_LOG_ERROR("Priority value %d is out of supported priority range %d-%d",
            value, m_minPriority, m_maxPriority);
        return false;
    }
    m_priority = value;
    return true;
}

bool AclRule::setAction(sai_acl_entry_attr_t actionId, sai_acl_action_data_t actionData)
{
    if (!isActionSupported(actionId))
    {
        SWSS_LOG_ERROR("Action attribute %s is not supported",
            getAttributeIdName(SAI_OBJECT_TYPE_ACL_ENTRY, actionId).c_str());
        return false;
    }

    // check if ACL action attribute entry parameter is an enum value
    const auto* meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, actionId);
    if (meta == nullptr)
    {
        SWSS_LOG_THROW("Metadata null pointer returned by sai_metadata_get_attr_metadata for action %d", actionId);
    }
    if (meta->isenum)
    {
        // if ACL action attribute requires enum value check if value is supported by the ASIC
        if (!m_pAclOrch->isAclActionEnumValueSupported(AclEntryActionToAclAction(actionId), actionData.parameter))
        {
            SWSS_LOG_ERROR("Action %s is not supported by ASIC",
                getAttributeIdName(SAI_OBJECT_TYPE_ACL_ENTRY, actionId).c_str());
            return false;
        }
    }

    sai_attribute_t attr;
    attr.id = actionId;
    attr.value.aclaction = actionData;

    m_actions[actionId] = SaiAttrWrapper(SAI_OBJECT_TYPE_ACL_ENTRY, attr);

    if (!m_pTable->validateAclRuleAction(actionId, *this))
    {
        return false;
    }

    return true;
}

bool AclRule::setMatch(sai_acl_entry_attr_t matchId, sai_acl_field_data_t matchData)
{
    sai_attribute_t attr;
    attr.id = matchId;
    attr.value.aclfield = matchData;

    m_matches[matchId] = SaiAttrWrapper(SAI_OBJECT_TYPE_ACL_ENTRY, attr);

    if (!m_pTable->validateAclRuleMatch(matchId, *this))
    {
        return false;
    }

    return true;
}

bool AclRule::setAttribute(sai_attribute_t attr)
{
    auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, attr.id);
    if (!meta)
    {
        SWSS_LOG_THROW("Failed to get metadata for attribute id %d", attr.id);
    }
    auto status = sai_acl_api->set_acl_entry_attribute(m_ruleOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update attribute %s on ACL rule %s in ACL table %s: %s",
                        meta->attridname, getId().c_str(), getTableId().c_str(),
                        sai_serialize_status(status).c_str());
        return false;
    }
    SWSS_LOG_INFO("Successfully updated action attribute %s on ACL rule %s in ACL table %s",
                   meta->attridname, getId().c_str(), getTableId().c_str());
    return true;
}

vector<sai_object_id_t> AclRule::getInPorts() const
{
    vector<sai_object_id_t> inPorts;
    auto it = m_matches.find(SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS);
    if (it == m_matches.end())
    {
        return inPorts;
    }
    auto attr = it->second.getSaiAttr();
    if (!attr.value.aclfield.enable)
    {
        return inPorts;
    }
    auto objlist = attr.value.aclfield.data.objlist;
    inPorts = vector<sai_object_id_t>(objlist.list, objlist.list + objlist.count);
    return inPorts;
}

string AclRule::getId() const
{
    return m_id;
}

string AclRule::getTableId() const
{
    return m_pTable->getId();
}

sai_object_id_t AclRule::getOid() const
{
    return m_ruleOid;
}

sai_object_id_t AclRule::getCounterOid() const
{
    return m_counterOid;
}

bool AclRule::hasCounter() const
{
    return getCounterOid() != SAI_NULL_OBJECT_ID;
}

const vector<AclRangeConfig>& AclRule::getRangeConfig() const
{
    return m_rangeConfig;
}

bool AclRule::getCreateCounter() const
{
    return m_createCounter;
}

shared_ptr<AclRule> AclRule::makeShared(AclOrch *acl, MirrorOrch *mirror, DTelOrch *dtel, const string& rule, const string& table, const KeyOpFieldsValuesTuple& data)
{
    shared_ptr<AclRule> aclRule;

    for (const auto& itr: kfvFieldsValues(data))
    {
        auto action = to_upper(fvField(itr));
        if (aclMirrorStageLookup.find(action) != aclMirrorStageLookup.cend() ||
            action == ACTION_MIRROR_ACTION /* implicitly ingress in old schema */)
        {
            return make_shared<AclRuleMirror>(acl, mirror, rule, table);
        }
        else if (aclL3ActionLookup.find(action) != aclL3ActionLookup.cend())
        {
            return make_shared<AclRulePacket>(acl, rule, table);
        }
        else if (aclDTelActionLookup.find(action) != aclDTelActionLookup.cend())
        {
            if (!dtel)
            {
                throw runtime_error("DTel feature is not enabled. Watchlists cannot be configured");
            }

            return make_shared<AclRuleDTelWatchListEntry>(acl, dtel, rule, table);
        }
    }

    if (!aclRule)
    {
        throw runtime_error("ACL rule action is not found in rule " + rule);
    }

    return aclRule;
}

bool AclRule::enableCounter()
{
    SWSS_LOG_ENTER();

    if (m_counterOid != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    if (m_ruleOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("ACL rule %s doesn't exist in ACL table %s", m_id.c_str(), m_pTable->getId().c_str());
        return false;
    }

    if (!createCounter())
    {
        return false;
    }

    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
    attr.value.aclaction.parameter.oid = m_counterOid;
    attr.value.aclaction.enable = true;

    sai_status_t status = sai_acl_api->set_acl_entry_attribute(m_ruleOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to enable counter for ACL rule %s in ACL table %s", m_id.c_str(), m_pTable->getId().c_str());
        removeCounter();
        return false;
    }

    return true;
}

bool AclRule::disableCounter()
{
    SWSS_LOG_ENTER();

    if (m_counterOid == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    if (m_ruleOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("ACL rule %s doesn't exist in ACL table %s", m_id.c_str(), m_pTable->getId().c_str());
        return false;
    }

    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
    attr.value.aclaction.parameter.oid = SAI_NULL_OBJECT_ID;
    attr.value.aclaction.enable = false;

    sai_status_t status = sai_acl_api->set_acl_entry_attribute(m_ruleOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to disable counter for ACL rule %s in ACL table %s", m_id.c_str(), m_pTable->getId().c_str());
        return false;
    }

    if (!removeCounter())
    {
        return false;
    }

    return true;
}

bool AclRule::createCounter()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> counter_attrs;

    if (m_counterOid != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    attr.id = SAI_ACL_COUNTER_ATTR_TABLE_ID;
    attr.value.oid = m_pTable->getOid();
    counter_attrs.push_back(attr);

    for (const auto& counterAttrPair: aclCounterLookup)
    {
        tie(attr.id, std::ignore) = counterAttrPair;
        attr.value.booldata = true;
        counter_attrs.push_back(attr);
    }

    if (sai_acl_api->create_acl_counter(&m_counterOid, gSwitchId, (uint32_t)counter_attrs.size(), counter_attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create counter for the rule %s in table %s", m_id.c_str(), m_pTable->getId().c_str());
        return false;
    }

    gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, m_pTable->getOid());

    SWSS_LOG_INFO("Created counter for the rule %s in table %s", m_id.c_str(), m_pTable->getId().c_str());

    return true;
}

bool AclRule::removeRanges()
{
    SWSS_LOG_ENTER();
    if (!m_ranges.size())
    {
       //The Acl Rules which have mirror action will not have ranges created till the mirror becomes active
       SWSS_LOG_INFO("No Acl Range created for ACL Rule %s in table %s", m_id.c_str(), m_pTable->getId().c_str());
       return true;
    }

    for (const auto& rangeConfig: m_rangeConfig)
    {
        if (!AclRange::remove(rangeConfig.rangeType, rangeConfig.min, rangeConfig.max))
        {
            return false;
        }
    }
    return true;
}

bool AclRule::removeCounter()
{
    SWSS_LOG_ENTER();

    if (m_counterOid == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    if (sai_acl_api->remove_acl_counter(m_counterOid) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ACL counter for rule %s in table %s", m_id.c_str(), m_pTable->getId().c_str());
        return false;
    }

    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, m_pTable->getOid());

    m_counterOid = SAI_NULL_OBJECT_ID;

    SWSS_LOG_INFO("Removed counter for the rule %s in table %s", m_id.c_str(), m_pTable->getId().c_str());

    return true;
}

AclRulePacket::AclRulePacket(AclOrch *aclOrch, string rule, string table, bool createCounter) :
        AclRule(aclOrch, rule, table, createCounter)
{
}

bool AclRulePacket::validateAddAction(string attr_name, string _attr_value)
{
    SWSS_LOG_ENTER();

    string attr_value = to_upper(_attr_value);
    sai_acl_action_data_t actionData;

    auto action_str = attr_name;

    if (attr_name == ACTION_PACKET_ACTION)
    {
        const auto it = aclPacketActionLookup.find(attr_value);
        if (it != aclPacketActionLookup.cend())
        {
            actionData.parameter.s32 = it->second;
        }
        // handle PACKET_ACTION_REDIRECT in ACTION_PACKET_ACTION for backward compatibility
        else if (attr_value.find(PACKET_ACTION_REDIRECT) != string::npos)
        {
            // check that we have a colon after redirect rule
            size_t colon_pos = string(PACKET_ACTION_REDIRECT).length();

            if (attr_value.c_str()[colon_pos] != ':')
            {
                SWSS_LOG_ERROR("Redirect action rule must have ':' after REDIRECT");
                return false;
            }

            if (colon_pos + 1 == attr_value.length())
            {
                SWSS_LOG_ERROR("Redirect action rule must have a target after 'REDIRECT:' action");
                return false;
            }

            _attr_value = _attr_value.substr(colon_pos+1);

            sai_object_id_t param_id = getRedirectObjectId(_attr_value);
            if (param_id == SAI_NULL_OBJECT_ID)
            {
                return false;
            }
            actionData.parameter.oid = param_id;

            action_str = ACTION_REDIRECT_ACTION;
        }
        // handle PACKET_ACTION_DO_NOT_NAT in ACTION_PACKET_ACTION
        else if (attr_value == PACKET_ACTION_DO_NOT_NAT)
        {
            actionData.parameter.booldata = true;
            action_str = ACTION_DO_NOT_NAT_ACTION;
        }
        else
        {
            return false;
        }
    }
    else if (attr_name == ACTION_REDIRECT_ACTION)
    {
        sai_object_id_t param_id = getRedirectObjectId(_attr_value);
        if (param_id == SAI_NULL_OBJECT_ID)
        {
            return false;
        }
        actionData.parameter.oid = param_id;
    }
    else
    {
        return false;
    }

    actionData.enable = true;

    return setAction(aclL3ActionLookup[action_str], actionData);
}

// This method should return sai attribute id of the redirect destination
sai_object_id_t AclRulePacket::getRedirectObjectId(const string& redirect_value)
{

    string target = redirect_value;

    // Try to parse physical port and LAG first
    Port port;
    if (gPortsOrch->getPort(target, port))
    {
        if (port.m_type == Port::PHY)
        {
            return port.m_port_id;
        }
        else if (port.m_type == Port::LAG)
        {
            return port.m_lag_id;
        }
        else
        {
            SWSS_LOG_ERROR("Wrong port type for REDIRECT action. Only physical ports and LAG ports are supported");
            return SAI_NULL_OBJECT_ID;
        }
    }

    // Try to parse nexthop ip address and interface name
    try
    {
        NextHopKey nh(target);
        if (!m_pAclOrch->m_neighOrch->hasNextHop(nh))
        {
            SWSS_LOG_ERROR("ACL Redirect action target next hop ip: '%s' doesn't exist on the switch", nh.to_string().c_str());
            return SAI_NULL_OBJECT_ID;
        }

        m_redirect_target_next_hop = target;
        m_pAclOrch->m_neighOrch->increaseNextHopRefCount(nh);
        return m_pAclOrch->m_neighOrch->getNextHopId(nh);
    }
    catch (...)
    {
        // no error, just try next variant
    }

    // try to parse nh group the set of <ip address, interface name>
    try
    {
        NextHopGroupKey nhg(target);
        if (!m_pAclOrch->m_routeOrch->hasNextHopGroup(nhg))
        {
            SWSS_LOG_INFO("ACL Redirect action target next hop group: '%s' doesn't exist on the switch. Creating it.", nhg.to_string().c_str());

            if (!m_pAclOrch->m_routeOrch->addNextHopGroup(nhg))
            {
                SWSS_LOG_ERROR("Can't create required target next hop group '%s'", nhg.to_string().c_str());
                return SAI_NULL_OBJECT_ID;
            }
            SWSS_LOG_DEBUG("Created acl redirect target next hop group '%s'", nhg.to_string().c_str());
        }

        m_redirect_target_next_hop_group = target;
        m_pAclOrch->m_routeOrch->increaseNextHopRefCount(nhg);
        return m_pAclOrch->m_routeOrch->getNextHopGroupId(nhg);
    }
    catch (...)
    {
        // no error, just try next variant
    }

    SWSS_LOG_ERROR("ACL Redirect action target '%s' wasn't recognized", target.c_str());

    return SAI_NULL_OBJECT_ID;
}

bool AclRulePacket::validate()
{
    SWSS_LOG_ENTER();

    if ((m_rangeConfig.empty() && m_matches.empty()) || m_actions.size() != 1)
    {
        return false;
    }

    return true;
}

void AclRulePacket::onUpdate(SubjectType, void *)
{
    // Do nothing
}

AclRuleMirror::AclRuleMirror(AclOrch *aclOrch, MirrorOrch *mirror, string rule, string table) :
        AclRule(aclOrch, rule, table),
        m_state(false),
        m_pMirrorOrch(mirror)
{
}

bool AclRuleMirror::validateAddAction(string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    sai_acl_entry_attr_t action;

    const auto it = aclMirrorStageLookup.find(attr_name);
    if (it != aclMirrorStageLookup.cend())
    {
        action = it->second;
    }
    // handle ACTION_MIRROR_ACTION as ingress by default for backward compatibility
    else if (attr_name == ACTION_MIRROR_ACTION)
    {
        action = SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS;
    }
    else
    {
        return false;
    }

    m_sessionName = attr_value;

    // insert placeholder value, we'll set the session oid in AclRuleMirror::create()
    return setAction(action, sai_acl_action_data_t{});
}

bool AclRuleMirror::validate()
{
    SWSS_LOG_ENTER();

    if ((m_rangeConfig.empty() && m_matches.empty()) || m_sessionName.empty())
    {
        return false;
    }

    return true;
}

bool AclRuleMirror::createRule()
{
    SWSS_LOG_ENTER();

    return activate();
}

bool AclRuleMirror::removeRule()
{
    return deactivate();
}

bool AclRuleMirror::activate()
{
    SWSS_LOG_ENTER();

    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    bool state = false;

    if (!m_pMirrorOrch->sessionExists(m_sessionName))
    {
        SWSS_LOG_ERROR("Mirror rule references mirror session \"%s\" that does not exist yet", m_sessionName.c_str());
        return false;
    }

    if (!m_pMirrorOrch->getSessionStatus(m_sessionName, state))
    {
        SWSS_LOG_THROW("Failed to get mirror session state for session %s", m_sessionName.c_str());
    }

    if (!state)
    {
        return true;
    }

    if (!m_pMirrorOrch->getSessionOid(m_sessionName, oid))
    {
        SWSS_LOG_THROW("Failed to get mirror session OID for session %s", m_sessionName.c_str());
    }

    for (auto& it: m_actions)
    {
        auto attr = it.second.getSaiAttr();
        attr.value.aclaction.enable = true;
        attr.value.aclaction.parameter.objlist.list = &oid;
        attr.value.aclaction.parameter.objlist.count = 1;
        setAction(it.first, attr.value.aclaction);
    }

    // If the rule with mirror action is removed and then mirror is activated, create the counter before rule is created
    if (!hasCounter())
    {
        if (getCreateCounter() && !createCounter())
        {
            SWSS_LOG_ERROR("createCounter failed for Rule %s session %s", m_id.c_str(), m_sessionName.c_str());
            return false;
       }
    }

    if (!AclRule::createRule())
    {
        return false;
    }

    if (!m_pMirrorOrch->increaseRefCount(m_sessionName))
    {
        SWSS_LOG_THROW("Failed to increase mirror session reference count for session %s", m_sessionName.c_str());
    }

    m_state = true;

    return true;

}

bool AclRuleMirror::deactivate()
{
    SWSS_LOG_ENTER();

    if (!m_state)
    {
        return true;
    }

    if (!AclRule::removeRule())
    {
        return false;
    }

    if (!m_pMirrorOrch->decreaseRefCount(m_sessionName))
    {
        SWSS_LOG_THROW("Failed to decrease mirror session reference count for session %s", m_sessionName.c_str());
    }

    m_state = false;

    return true;
}

bool AclRuleMirror::update(const AclRule& rule)
{
    auto mirrorRule = dynamic_cast<const AclRuleMirror*>(&rule);
    if (!mirrorRule)
    {
        SWSS_LOG_ERROR("Cannot update mirror rule with a rule of a different type");
        return false;
    }

    SWSS_LOG_ERROR("Updating mirror rule is currently not implemented");
    return false;
}

void AclRuleMirror::onUpdate(SubjectType type, void *cntx)
{
    if (type != SUBJECT_TYPE_MIRROR_SESSION_CHANGE)
    {
        return;
    }

    MirrorSessionUpdate *update = static_cast<MirrorSessionUpdate *>(cntx);

    if (m_sessionName != update->name)
    {
        return;
    }

    if (update->active)
    {
        SWSS_LOG_INFO("Activating mirroring ACL %s for session %s", m_id.c_str(), m_sessionName.c_str());
        activate();
    }
    else
    {
        SWSS_LOG_INFO("Deactivating mirroring ACL %s for session %s", m_id.c_str(), m_sessionName.c_str());
        deactivate();
    }
}

AclTable::AclTable(AclOrch *pAclOrch, string id) noexcept : m_pAclOrch(pAclOrch), id(id)
{

}

AclTable::AclTable(AclOrch *pAclOrch) noexcept : m_pAclOrch(pAclOrch)
{

}

bool AclTable::addMandatoryActions()
{
    SWSS_LOG_ENTER();

    if (stage == ACL_STAGE_UNKNOWN)
    {
        return false;
    }

    if (!m_pAclOrch->isAclActionListMandatoryOnTableCreation(stage))
    {
        // No op if action list is not mandatory on table creation.
        return true;
    }
    if (!type.getActions().empty())
    {
        // No change if action_list is provided
        return true;
    }

    sai_acl_action_type_t acl_action = SAI_ACL_ACTION_TYPE_COUNTER;
    if (m_pAclOrch->isAclActionSupported(stage, acl_action))
    {
        SWSS_LOG_INFO("Add counter acl action");
        type.addAction(acl_action);
    }

    if (defaultAclActionList.count(type.getName()) != 0)
    {
        // Add the default action list
        for (auto action : defaultAclActionList[type.getName()][stage])
        {
            if (m_pAclOrch->isAclActionSupported(stage, action))
            {
                SWSS_LOG_INFO("Added default action for table type %s stage %s",
                                    type.getName().c_str(),
                                    ((stage == ACL_STAGE_INGRESS)? "INGRESS":"EGRESS"));
                type.addAction(action);
            }
        }
    }

    return true;
}

bool AclTable::addStageMandatoryRangeFields()
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";
    string sub_platform = getenv("sub_platform") ? getenv("sub_platform") : "";
    auto match = SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE;

    if ((platform == BRCM_PLATFORM_SUBSTRING) && (sub_platform != BRCM_DNX_PLATFORM_SUBSTRING) &&
        (stage == ACL_STAGE_EGRESS))
    {
        return false;
    }

    type.addMatch(make_shared<AclTableRangeMatch>(set<sai_acl_range_type_t>{
                    {SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE}}));
    SWSS_LOG_INFO("Added mandatory match field %s for table type %s stage %d",
                  sai_serialize_enum(match, &sai_metadata_enum_sai_acl_table_attr_t).c_str(),
                  type.getName().c_str(), stage);

    return true;
}


bool AclTable::addStageMandatoryMatchFields()
{
    SWSS_LOG_ENTER();

    if (stage == ACL_STAGE_UNKNOWN)
    {
        return false;
    }

    if (stageMandatoryMatchFields.count(type.getName()) != 0)
    {
        auto &fields_for_stage = stageMandatoryMatchFields[type.getName()];
        if (fields_for_stage.count(stage) != 0)
        {
            // Add the stage particular matching fields
            for (auto match : fields_for_stage[stage])
            {
                if (match != SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE)
                {
                    type.addMatch(make_shared<AclTableMatch>(match));
                    SWSS_LOG_INFO("Added mandatory match field %s for table type %s stage %d",
                        sai_serialize_enum(match, &sai_metadata_enum_sai_acl_table_attr_t).c_str(),
                        type.getName().c_str(), stage);
                }
                else
                {
                    addStageMandatoryRangeFields();
                }
            }
        }
    }

    return true;
}

bool AclTable::validateAddType(const AclTableType &tableType)
{
    SWSS_LOG_ENTER();

    type = tableType;

    return true;
}

bool AclTable::validateAddStage(const acl_stage_type_t &value)
{
    SWSS_LOG_ENTER();

    if (value == ACL_STAGE_UNKNOWN)
    {
        SWSS_LOG_ERROR("Failed to validate stage: unknown stage");
        return false;
    }

    stage = value;

    return true;
}

bool AclTable::validateAddPorts(const unordered_set<string> &value)
{
    SWSS_LOG_ENTER();

    for (const auto &itAlias: value)
    {
        Port port;
        if (!gPortsOrch->getPort(itAlias, port))
        {
            SWSS_LOG_INFO(
                "Add unready port %s to pending list for ACL table %s",
                itAlias.c_str(), id.c_str()
            );
            pendingPortSet.emplace(itAlias);
            continue;
        }

        sai_object_id_t bindPortOid;
        if (!AclOrch::getAclBindPortId(port, bindPortOid))
        {
            SWSS_LOG_ERROR(
                "Failed to get port %s bind port ID for ACL table %s",
                itAlias.c_str(), id.c_str()
            );
            return false;
        }

        link(bindPortOid);
        portSet.emplace(itAlias);
    }

    return true;
}

bool AclTable::validate()
{
    if (type.getName() == TABLE_TYPE_CTRLPLANE)
    {
        return true;
    }

    if (stage == ACL_STAGE_UNKNOWN)
    {
        return false;
    }

    if (type.getName() == TABLE_TYPE_L3V4V6)
    {
	    if (!m_pAclOrch->isAclL3V4V6TableSupported(stage))
	    {

                    SWSS_LOG_ERROR("Table %s: table type %s in stage %d not supported on this platform.",
				    id.c_str(), type.getName().c_str(), stage);
		    return false;
	    }
    }



    if (m_pAclOrch->isAclActionListMandatoryOnTableCreation(stage))
    {
        if (type.getActions().empty())
        {
            SWSS_LOG_ERROR("Action list for table %s is mandatory", id.c_str());
            return false;
        }
    }

    for (const auto& action: type.getActions())
    {
        if (!m_pAclOrch->isAclActionSupported(stage, action))
        {
            SWSS_LOG_ERROR("Action %s is not supported on table %s",
                sai_metadata_get_acl_action_type_name(action), id.c_str());
            return false;
        }
    }

    return true;
}

bool AclTable::validateAclRuleMatch(sai_acl_entry_attr_t matchId, const AclRule& rule) const
{
    const auto& tableMatches = type.getMatches();
    const auto tableMatchId = AclEntryFieldToAclTableField(matchId);
    const auto tableMatchIt = tableMatches.find(tableMatchId);
    if (tableMatchIt == tableMatches.end())
    {
        SWSS_LOG_ERROR("Match %s in rule %s is not supported by table %s",
            getAttributeIdName(SAI_OBJECT_TYPE_ACL_ENTRY, matchId).c_str(),
            rule.getId().c_str(), id.c_str());
        return false;
    }

    const auto& tableMatchAttrObject = tableMatchIt->second;
    if (!tableMatchAttrObject->validateAclRuleMatch(rule))
    {
        SWSS_LOG_ERROR("Match %s in rule configuration %s is invalid %s",
            getAttributeIdName(SAI_OBJECT_TYPE_ACL_ENTRY, matchId).c_str(),
            rule.getId().c_str(), id.c_str());
        return false;
    }

    return true;
}

bool AclTable::validateAclRuleAction(sai_acl_entry_attr_t actionId, const AclRule& rule) const
{
    // This means ACL table can hold rules with any action.
    if (!type.getActions().empty())
    {
        auto action = AclEntryActionToAclAction(actionId);
        if (!type.getActions().count(action))
        {
            SWSS_LOG_ERROR("Action %s is not supported on table %s",
                sai_metadata_get_acl_action_type_name(action), id.c_str());
            return false;
        }
    }

    return true;
}

bool AclTable::create()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> table_attrs;
    vector<int32_t> action_types_list {type.getActions().begin(), type.getActions().end()};
    vector<int32_t> bpoint_list {type.getBindPointTypes().begin(), type.getBindPointTypes().end()};

    attr.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = static_cast<uint32_t>(bpoint_list.size());
    attr.value.s32list.list = bpoint_list.data();
    table_attrs.push_back(attr);

    for (const auto& matchPair: type.getMatches())
    {
        table_attrs.push_back(matchPair.second->toSaiAttribute());
    }

    if (!action_types_list.empty())
    {
        attr.id= SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST;
        attr.value.s32list.count = static_cast<uint32_t>(action_types_list.size());
        attr.value.s32list.list = action_types_list.data();
        table_attrs.push_back(attr);
    }

    sai_acl_stage_t acl_stage;
    attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    acl_stage = (stage == ACL_STAGE_INGRESS) ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
    attr.value.s32 = acl_stage;
    table_attrs.push_back(attr);

    sai_status_t status = sai_acl_api->create_acl_table(&m_oid, gSwitchId, (uint32_t)table_attrs.size(), table_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    for (const auto& bpointType: type.getBindPointTypes())
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, acl_stage, bpointType);
    }

    return true;
}

void AclTable::onUpdate(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    // Only interested in port change
    if (type != SUBJECT_TYPE_PORT_CHANGE)
    {
        return;
    }

    PortUpdate *update = static_cast<PortUpdate *>(cntx);
    Port &port = update->port;

    sai_object_id_t bind_port_id;
    if (!AclOrch::getAclBindPortId(port, bind_port_id))
    {
        SWSS_LOG_ERROR("Failed to get port %s bind port ID",
                       port.m_alias.c_str());
        return;
    }

    if (update->add)
    {
        if (pendingPortSet.find(port.m_alias) != pendingPortSet.end())
        {
            link(bind_port_id);
            bind(bind_port_id);

            pendingPortSet.erase(port.m_alias);
            portSet.emplace(port.m_alias);

            SWSS_LOG_NOTICE("Bound port %s to ACL table %s",
                            port.m_alias.c_str(), id.c_str());
        }
    }
    else
    {
        if (portSet.find(port.m_alias) != portSet.end())
        {
            unbind(bind_port_id);
            unlink(bind_port_id);

            portSet.erase(port.m_alias);
            pendingPortSet.emplace(port.m_alias);

            SWSS_LOG_NOTICE("Unbound port %s from ACL table %s",
                            port.m_alias.c_str(), id.c_str());
        }
    }

}

bool AclTable::bind(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    assert(ports.find(portOid) != ports.end());

    sai_object_id_t group_member_oid;
    if (!gPortsOrch->bindAclTable(portOid, m_oid, group_member_oid, stage))
    {
        SWSS_LOG_ERROR("Failed to bind port oid: %" PRIx64 "", portOid);
        return false;
    }
    SWSS_LOG_NOTICE("Successfully bound port oid: %" PRIx64", group member oid:%" PRIx64 "",
                     portOid, group_member_oid);
    ports[portOid] = group_member_oid;
    return true;
}

bool AclTable::unbind(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    assert(ports.find(portOid) != ports.end());

    sai_object_id_t group_member_oid = ports[portOid];
    if (!gPortsOrch->unbindAclTable(portOid, m_oid, group_member_oid, stage))
    {
        return false;
    }
    SWSS_LOG_NOTICE("%" PRIx64" port is unbound from %s ACL table",
                    portOid, id.c_str());
    ports[portOid] = SAI_NULL_OBJECT_ID;
    return true;
}

bool AclTable::bind()
{
    SWSS_LOG_ENTER();

    for (const auto& portpair: ports)
    {
        sai_object_id_t portOid = portpair.first;
        bool suc = bind(portOid);
        if (!suc) return false;
    }
    return true;
}

bool AclTable::unbind()
{
    SWSS_LOG_ENTER();

    for (const auto& portpair: ports)
    {
        sai_object_id_t portOid = portpair.first;
        bool suc = unbind(portOid);
        if (!suc) return false;
    }
    return true;
}

void AclTable::link(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    ports.emplace(portOid, SAI_NULL_OBJECT_ID);
}

void AclTable::unlink(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    ports.erase(portOid);
}

bool AclTable::add(shared_ptr<AclRule> newRule)
{
    SWSS_LOG_ENTER();

    string rule_id = newRule->getId();
    auto ruleIter = rules.find(rule_id);
    if (ruleIter != rules.end())
    {
        // If ACL rule already exists, delete it first
        if (ruleIter->second->hasCounter())
        {
            // Deregister the flex counter before deleting the rule
            // A new flex counter will be created when the new rule is added
            m_pAclOrch->deregisterFlexCounter(*(ruleIter->second));
        }
        if (ruleIter->second->remove())
        {
            rules.erase(ruleIter);
            SWSS_LOG_NOTICE("Successfully deleted ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
        }
    }

    if (newRule->create())
    {
        rules[rule_id] = newRule;
        SWSS_LOG_NOTICE("Successfully created ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to create ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
        return false;
    }
}

bool AclTable::remove(string rule_id)
{
    SWSS_LOG_ENTER();

    auto ruleIter = rules.find(rule_id);
    if (ruleIter != rules.end())
    {
        if (ruleIter->second->remove())
        {
            rules.erase(ruleIter);
            SWSS_LOG_NOTICE("Successfully deleted ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to delete ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_WARN("Skip deleting unknown ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
        return true;
    }
}

bool AclTable::updateRule(shared_ptr<AclRule> updatedRule)
{
    SWSS_LOG_ENTER();

    if (!updatedRule)
    {
        return false;
    }

    auto ruleId = updatedRule->getId();
    auto ruleIter = rules.find(ruleId);
    if (ruleIter == rules.end())
    {
        SWSS_LOG_ERROR("Failed to update ACL rule %s as it does not exist in %s",
                       ruleId.c_str(), id.c_str());
        return false;
    }

    if (!ruleIter->second->update(*updatedRule))
    {
        SWSS_LOG_ERROR("Failed to update ACL rule %s in table %s",
                       ruleId.c_str(), id.c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Successfully updated ACL rule %s in table %s",
                    ruleId.c_str(), id.c_str());
    return true;
}

bool AclTable::clear()
{
    SWSS_LOG_ENTER();

    for (auto& rulepair: rules)
    {
        auto& rule = *rulepair.second;

        if (rule.hasCounter())
        {
            m_pAclOrch->deregisterFlexCounter(rule);
        }

        bool suc = rule.remove();
        if (!suc)
        {
            SWSS_LOG_ERROR("Failed to delete ACL rule %s when removing the ACL table %s",
                    rule.getId().c_str(), id.c_str());
            return false;
        }
    }
    rules.clear();
    return true;
}

AclRuleDTelWatchListEntry::AclRuleDTelWatchListEntry(AclOrch *aclOrch, DTelOrch *dtel, string rule, string table) :
        AclRule(aclOrch, rule, table),
        m_pDTelOrch(dtel)
{
}

bool AclRuleDTelWatchListEntry::validateAddAction(string attr_name, string attr_val)
{
    SWSS_LOG_ENTER();

    sai_acl_action_data_t actionData;
    string attr_value = to_upper(attr_val);
    sai_object_id_t session_oid;

    if (!m_pDTelOrch ||
        (attr_name != ACTION_DTEL_FLOW_OP &&
        attr_name != ACTION_DTEL_INT_SESSION &&
        attr_name != ACTION_DTEL_FLOW_SAMPLE_PERCENT &&
        attr_name != ACTION_DTEL_REPORT_ALL_PACKETS &&
        attr_name != ACTION_DTEL_DROP_REPORT_ENABLE &&
        attr_name != ACTION_DTEL_TAIL_DROP_REPORT_ENABLE))
    {
        return false;
    }

    if (attr_name == ACTION_DTEL_FLOW_OP)
    {
        auto it = aclDTelFlowOpTypeLookup.find(attr_value);

        if (it == aclDTelFlowOpTypeLookup.end())
        {
            return false;
        }

        actionData.parameter.s32 = it->second;

        if (attr_value == DTEL_FLOW_OP_INT)
        {
            INT_enabled = true;
        }
        else
        {
            INT_enabled = false;
        }
    }

    if (attr_name == ACTION_DTEL_INT_SESSION)
    {
        m_intSessionId = attr_value;

        bool ret = m_pDTelOrch->getINTSessionOid(attr_value, session_oid);
        if (ret)
        {
            actionData.parameter.oid = session_oid;

            // Increase session reference count regardless of state to deny
            // attempt to remove INT session with attached ACL rules.
            if (!m_pDTelOrch->increaseINTSessionRefCount(m_intSessionId))
            {
                SWSS_LOG_ERROR("Failed to increase INT session %s reference count", m_intSessionId.c_str());
                return false;
            }

            INT_session_valid = true;
        } else {
            SWSS_LOG_ERROR("Invalid INT session id %s used for ACL action", m_intSessionId.c_str());
            INT_session_valid = false;
        }
    }

    if (attr_name == ACTION_DTEL_FLOW_SAMPLE_PERCENT)
    {
        actionData.parameter.u8 = to_uint<uint8_t>(attr_value);
    }

    actionData.enable = true;

    if (attr_name == ACTION_DTEL_REPORT_ALL_PACKETS ||
        attr_name == ACTION_DTEL_DROP_REPORT_ENABLE ||
        attr_name == ACTION_DTEL_TAIL_DROP_REPORT_ENABLE)
    {
        actionData.parameter.booldata = (attr_value == DTEL_ENABLED) ? true : false;
        actionData.enable = (attr_value == DTEL_ENABLED) ? true : false;
    }

    return setAction(aclDTelActionLookup[attr_name], actionData);
}

bool AclRuleDTelWatchListEntry::validate()
{
    SWSS_LOG_ENTER();

    if (!m_pDTelOrch)
    {
        return false;
    }

    if ((m_rangeConfig.empty() && m_matches.empty()) || m_actions.size() == 0)
    {
        return false;
    }

    return true;
}

bool AclRuleDTelWatchListEntry::createRule()
{
    SWSS_LOG_ENTER();

    return activate();
}

bool AclRuleDTelWatchListEntry::removeRule()
{
    return deactivate();
}

bool AclRuleDTelWatchListEntry::activate()
{
    SWSS_LOG_ENTER();

    if (!m_pDTelOrch)
    {
        return false;
    }

    if (INT_enabled && !INT_session_valid)
    {
        return true;
    }

    return AclRule::createRule();
}

bool AclRuleDTelWatchListEntry::deactivate()
{
    SWSS_LOG_ENTER();

    if (!m_pDTelOrch)
    {
        return false;
    }

    if (INT_enabled && !INT_session_valid)
    {
        return true;
    }

    if (!AclRule::removeRule())
    {
        return false;
    }

    if (INT_enabled && INT_session_valid)
    {
        if (!m_pDTelOrch->decreaseINTSessionRefCount(m_intSessionId))
        {
            SWSS_LOG_ERROR("Could not decrement INT session %s reference count", m_intSessionId.c_str());
            return false;
        }
    }

    return true;
}

void AclRuleDTelWatchListEntry::onUpdate(SubjectType type, void *cntx)
{
    sai_acl_action_data_t actionData;
    sai_object_id_t session_oid = SAI_NULL_OBJECT_ID;

    if (!m_pDTelOrch)
    {
        return;
    }

    if (type != SUBJECT_TYPE_INT_SESSION_CHANGE || !INT_enabled)
    {
        return;
    }

    DTelINTSessionUpdate *update = static_cast<DTelINTSessionUpdate *>(cntx);

    if (m_intSessionId != update->session_id)
    {
        return;
    }

    if (update->active)
    {
        SWSS_LOG_INFO("Activating INT watchlist %s for session %s", m_id.c_str(), m_intSessionId.c_str());

        bool ret = m_pDTelOrch->getINTSessionOid(m_intSessionId, session_oid);
        if (!ret)
        {
            SWSS_LOG_ERROR("Invalid INT session id used for ACL action");
            return;
        }

        actionData.enable = true;
        actionData.parameter.oid = session_oid;

        // Increase session reference count regardless of state to deny
        // attempt to remove INT session with attached ACL rules.
        if (!m_pDTelOrch->increaseINTSessionRefCount(m_intSessionId))
        {
            throw runtime_error("Failed to increase INT session reference count");
        }

        if (!setAction(SAI_ACL_ENTRY_ATTR_ACTION_DTEL_INT_SESSION, actionData))
        {
            SWSS_LOG_ERROR("Failed to set action SAI_ACL_ENTRY_ATTR_ACTION_DTEL_INT_SESSION");
            return;
        }

        INT_session_valid = true;

        activate();
    }
    else
    {
        SWSS_LOG_INFO("Deactivating INT watchlist %s for session %s", m_id.c_str(), m_intSessionId.c_str());
        deactivate();
        INT_session_valid = false;
    }
}

bool AclRuleDTelWatchListEntry::update(const AclRule& rule)
{
    auto dtelWatchListRule = dynamic_cast<const AclRuleDTelWatchListEntry*>(&rule);
    if (!dtelWatchListRule)
    {
        SWSS_LOG_ERROR("Cannot update DTEL watch list rule with a rule of a different type");
        return false;
    }

    SWSS_LOG_ERROR("Updating DTEL watch list rule is currently not implemented");
    return false;
}

AclRange::AclRange(sai_acl_range_type_t type, sai_object_id_t oid, int min, int max):
    m_oid(oid), m_refCnt(0), m_min(min), m_max(max), m_type(type)
{
    SWSS_LOG_ENTER();
}

AclRange *AclRange::create(sai_acl_range_type_t type, int min, int max)
{
    SWSS_LOG_ENTER();
    sai_status_t status;
    sai_object_id_t range_oid = SAI_NULL_OBJECT_ID;

    acl_range_properties_t rangeProperties = make_tuple(type, min, max);
    auto range_it = m_ranges.find(rangeProperties);
    if (range_it == m_ranges.end())
    {
        sai_attribute_t attr;
        vector<sai_attribute_t> range_attrs;

        // work around to avoid syncd termination on SAI error due to max count of ranges reached
        // can be removed when syncd start passing errors to the SAI callers
        char *platform = getenv("platform");
        if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
        {
            if (m_ranges.size() >= MLNX_MAX_RANGES_COUNT)
            {
                SWSS_LOG_ERROR("Maximum numbers of ACL ranges reached");
                return NULL;
            }
        }

        attr.id = SAI_ACL_RANGE_ATTR_TYPE;
        attr.value.s32 = type;
        range_attrs.push_back(attr);

        attr.id = SAI_ACL_RANGE_ATTR_LIMIT;
        attr.value.u32range.min = min;
        attr.value.u32range.max = max;
        range_attrs.push_back(attr);

        status = sai_acl_api->create_acl_range(&range_oid, gSwitchId, (uint32_t)range_attrs.size(), range_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create range object");
            return NULL;
        }

        SWSS_LOG_INFO("Created ACL Range object. Type: %d, range %d-%d, oid: %" PRIx64, type, min, max, range_oid);
        m_ranges[rangeProperties] = new AclRange(type, range_oid, min, max);

        range_it = m_ranges.find(rangeProperties);
    }
    else
    {
        SWSS_LOG_INFO("Reusing range object oid %" PRIx64 " ref count increased to %d", range_it->second->m_oid, range_it->second->m_refCnt);
    }

    // increase range reference count
    range_it->second->m_refCnt++;

    return range_it->second;
}

bool AclRange::remove(sai_acl_range_type_t type, int min, int max)
{
    SWSS_LOG_ENTER();

    auto range_it = m_ranges.find(make_tuple(type, min, max));

    if (range_it == m_ranges.end())
    {
        return false;
    }

    return range_it->second->remove();
}

bool AclRange::remove(sai_object_id_t *oids, int oidsCnt)
{
    SWSS_LOG_ENTER();

    for (int oidIdx = 0; oidIdx < oidsCnt; oidsCnt++)
    {
        for (auto it : m_ranges)
        {
            if (it.second->m_oid == oids[oidsCnt])
            {
                return it.second->remove();
            }
        }
    }

    return false;
}

bool AclRange::remove()
{
    SWSS_LOG_ENTER();

    if ((--m_refCnt) < 0)
    {
        throw runtime_error("Invalid ACL Range refCnt!");
    }

    if (m_refCnt == 0)
    {
        SWSS_LOG_INFO("Range object oid %" PRIx64 " ref count is %d, removing..", m_oid, m_refCnt);
        if (sai_acl_api->remove_acl_range(m_oid) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete ACL Range object oid: %" PRIx64, m_oid);
            return false;
        }
        auto range_it = m_ranges.find(make_tuple(m_type, m_min, m_max));

        m_ranges.erase(range_it);
        delete this;
    }
    else
    {
        SWSS_LOG_INFO("Range object oid %" PRIx64 " ref count decreased to %d", m_oid, m_refCnt);
    }

    return true;
}

void AclOrch::init(vector<TableConnector>& connectors, PortsOrch *portOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch)
{
    SWSS_LOG_ENTER();

    // Clear ACL_TABLE and ACL_RULE status from STATE_DB
    removeAllAclTableStatus();
    removeAllAclRuleStatus();

    // TODO: Query SAI to get mirror table capabilities
    // Right now, verified platforms that support mirroring IPv6 packets are
    // Broadcom and Mellanox. Virtual switch is also supported for testing
    // purposes.
    string platform = getenv("platform") ? getenv("platform") : "";
    string sub_platform = getenv("sub_platform") ? getenv("sub_platform") : "";
    if (platform == BRCM_PLATFORM_SUBSTRING ||
            platform == CISCO_8000_PLATFORM_SUBSTRING ||
            platform == MLNX_PLATFORM_SUBSTRING ||
            platform == BFN_PLATFORM_SUBSTRING  ||
            platform == MRVL_PLATFORM_SUBSTRING ||
            platform == INVM_PLATFORM_SUBSTRING ||
            platform == NPS_PLATFORM_SUBSTRING ||
            platform == XS_PLATFORM_SUBSTRING ||
            platform == VS_PLATFORM_SUBSTRING)
    {
        m_mirrorTableCapabilities =
        {
            { TABLE_TYPE_MIRROR, true },
            { TABLE_TYPE_MIRRORV6, true },
        };
    }
    else
    {
        m_mirrorTableCapabilities =
        {
            { TABLE_TYPE_MIRROR, true },
            { TABLE_TYPE_MIRRORV6, false },
        };
    }

    if ( platform == MRVL_PLATFORM_SUBSTRING ||
            platform == INVM_PLATFORM_SUBSTRING ||
            platform == VS_PLATFORM_SUBSTRING)
    {
	m_L3V4V6Capability =
        {
		{ACL_STAGE_INGRESS, true},
		{ACL_STAGE_EGRESS, true},
	};
    }
    else
    {
	m_L3V4V6Capability =
        {
		{ACL_STAGE_INGRESS, false},
		{ACL_STAGE_EGRESS, false},
	};

    }


    SWSS_LOG_NOTICE("%s switch capability:", platform.c_str());
    SWSS_LOG_NOTICE("    TABLE_TYPE_MIRROR: %s",
            m_mirrorTableCapabilities[TABLE_TYPE_MIRROR] ? "yes" : "no");
    SWSS_LOG_NOTICE("    TABLE_TYPE_MIRRORV6: %s",
            m_mirrorTableCapabilities[TABLE_TYPE_MIRRORV6] ? "yes" : "no");
    SWSS_LOG_NOTICE("    TABLE_TYPE_L3V4V6: Ingress [%s], Egress [%s]",
            m_L3V4V6Capability[ACL_STAGE_INGRESS] ? "yes" : "no",
            m_L3V4V6Capability[ACL_STAGE_EGRESS] ? "yes" : "no");


    // In Mellanox platform, V4 and V6 rules are stored in different tables
    // In Broadcom DNX platform also, V4 and V6 rules are stored in different tables
    if (platform == MLNX_PLATFORM_SUBSTRING ||
        platform == CISCO_8000_PLATFORM_SUBSTRING ||
        platform == MRVL_PLATFORM_SUBSTRING ||
        platform == XS_PLATFORM_SUBSTRING ||
        (platform == BRCM_PLATFORM_SUBSTRING && sub_platform == BRCM_DNX_PLATFORM_SUBSTRING))
    {
        m_isCombinedMirrorV6Table = false;
    }
    else
    {
        m_isCombinedMirrorV6Table = true;
    }


    // Store the capabilities in state database
    // TODO: Move this part of the code into syncd
    vector<FieldValueTuple> fvVector;
    for (auto const& it : m_mirrorTableCapabilities)
    {
        string value = it.second ? "true" : "false";
        if (it.first == TABLE_TYPE_MIRROR)
        {
            fvVector.emplace_back(TABLE_TYPE_MIRROR, value);
        }
        else if (it.first == TABLE_TYPE_MIRRORV6)
        {
            fvVector.emplace_back(TABLE_TYPE_MIRRORV6, value);
        }
        else
        {
            // ignore
        }
    }
    m_switchOrch->set_switch_capability(fvVector);

    if (gMySwitchType != "dpu")
    {
        sai_attribute_t attrs[2];
        attrs[0].id = SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY;
        attrs[1].id = SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY;

        sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 2, attrs);
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("Get ACL entry priority values, min: %u, max: %u", attrs[0].value.u32, attrs[1].value.u32);
            AclRule::setRulePriorities(attrs[0].value.u32, attrs[1].value.u32);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to get ACL entry priority min/max values, rv:%d", status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
            if (handle_status != task_process_status::task_success)
            {
                throw "AclOrch initialization failure";
            }
        }

        queryAclActionCapability();
    }

    for (auto stage: {ACL_STAGE_INGRESS, ACL_STAGE_EGRESS})
    {
        m_mirrorTableId[stage] = "";
        m_mirrorV6TableId[stage] = "";
    }

    initDefaultTableTypes(platform, sub_platform);

    // Attach observers
    m_mirrorOrch->attach(this);
    gPortsOrch->attach(this);
}

void AclOrch::initDefaultTableTypes(const string& platform, const string& sub_platform)
{
    SWSS_LOG_ENTER();

    AclTableTypeBuilder builder;

    addAclTableType(
        builder.withName(TABLE_TYPE_L3)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .build()
    );

    addAclTableType(
        builder.withName(TABLE_TYPE_L3V6)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .build()
    );


    addAclTableType(
        builder.withName(TABLE_TYPE_L3V4V6)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS))
            .build()
    );

    addAclTableType(
        builder.withName(TABLE_TYPE_MCLAG)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUT_PORTS))
            .withMatch(make_shared<AclTableRangeMatch>(set<sai_acl_range_type_t>{
                {SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE}}))
            .build()
    );

    // Use SAI_ACL_BIND_POINT_TYPE_SWITCH in BRCM DNX platforms to use shared egress ACL table for PFCWD.
    if (platform == BRCM_PLATFORM_SUBSTRING && sub_platform == BRCM_DNX_PLATFORM_SUBSTRING)
    {
        addAclTableType(
            builder.withName(TABLE_TYPE_PFCWD)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_SWITCH)
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TC))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT))
                .build()
        );
    }
    else
    {
        addAclTableType(
            builder.withName(TABLE_TYPE_PFCWD)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TC))
                .build()
        );
    }

    addAclTableType(
        builder.withName(TABLE_TYPE_DROP)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TC))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS))
            .build()
    );


    /*
     * Type of Tables and Supported Match Types (ASIC database)
     * |------------------------------------------------------------------|
     * |                   | TABLE_MIRROR | TABLE_MIRROR | TABLE_MIRRORV6 |
     * |    Match Type     |----------------------------------------------|
     * |                   |   combined   |          separated            |
     * |------------------------------------------------------------------|
     * | MATCH_SRC_IP      |      √       |      √       |                |
     * | MATCH_DST_IP      |      √       |      √       |                |
     * |------------------------------------------------------------------|
     * | MATCH_ICMP_TYPE   |      √       |      √       |                |
     * | MATCH_ICMP_CODE   |      √       |      √       |                |
     * |------------------------------------------------------------------|
     * | MATCH_SRC_IPV6    |      √       |              |       √        |
     * | MATCH_DST_IPV6    |      √       |              |       √        |
     * |------------------------------------------------------------------|
     * | MATCH_ICMPV6_TYPE |      √       |              |       √        |
     * | MATCH_ICMPV6_CODE |      √       |              |       √        |
     * |------------------------------------------------------------------|
     * | MATCH_IP_PROTOCOL |      √       |      √       |                |
     * | MATCH_NEXT_HEADER |      √       |              |       √        |
     * | -----------------------------------------------------------------|
     * | MATCH_ETHERTYPE   |      √       |      √       |                |
     * |------------------------------------------------------------------|
     * | MATCH_IN_PORTS    |      √       |      √       |                |
     * |------------------------------------------------------------------|
     */

    if (isAclMirrorV4Supported())
    {
        addAclTableType(
            builder.withName(TABLE_TYPE_MIRROR_DSCP)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DSCP))
                .build()
        );

        builder.withName(TABLE_TYPE_MIRROR)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
            .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS))
            .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DSCP))
            .withMatch(make_shared<AclTableRangeMatch>(set<sai_acl_range_type_t>{
                {SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE}}));

        if (isAclMirrorV6Supported() && isCombinedMirrorV6Table())
        {
            builder
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER));
        }
        addAclTableType(builder.build());
    }

    if (isAclMirrorV6Supported())
    {
        addAclTableType(
            builder.withName(TABLE_TYPE_MIRRORV6)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
                .withBindPointType(SAI_ACL_BIND_POINT_TYPE_LAG)
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
                .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DSCP))
                .withMatch(make_shared<AclTableRangeMatch>(set<sai_acl_range_type_t>{
                    {SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE}}))
                .build()
        );
    }

    // Placeholder for control plane tables
    addAclTableType(builder.withName(TABLE_TYPE_CTRLPLANE).build());
}

void AclOrch::queryAclActionCapability()
{
    SWSS_LOG_ENTER();

    sai_status_t status {SAI_STATUS_FAILURE};
    sai_attribute_t attr;
    vector<int32_t> action_list;

    attr.id = SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status == SAI_STATUS_SUCCESS)
    {
        const auto max_action_count = attr.value.u32;

        for (auto stage_attr: {SAI_SWITCH_ATTR_ACL_STAGE_INGRESS, SAI_SWITCH_ATTR_ACL_STAGE_EGRESS})
        {
            auto stage = (stage_attr == SAI_SWITCH_ATTR_ACL_STAGE_INGRESS ? ACL_STAGE_INGRESS : ACL_STAGE_EGRESS);
            auto stage_str = (stage_attr == SAI_SWITCH_ATTR_ACL_STAGE_INGRESS ? STAGE_INGRESS : STAGE_EGRESS);
            action_list.resize(static_cast<size_t>(max_action_count));

            attr.id = stage_attr;
            attr.value.aclcapability.action_list.list  = action_list.data();
            attr.value.aclcapability.action_list.count = max_action_count;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {

                SWSS_LOG_INFO("Supported %s action count %d:", stage_str,
                              attr.value.aclcapability.action_list.count);

                auto& capabilities = m_aclCapabilities[stage];

                for (size_t i = 0; i < static_cast<size_t>(attr.value.aclcapability.action_list.count); i++)
                {
                    auto action = static_cast<sai_acl_action_type_t>(action_list[i]);
                    capabilities.actionList.insert(action);
                    SWSS_LOG_INFO("    %s", sai_serialize_enum(action, &sai_metadata_enum_sai_acl_action_type_t).c_str());
                }
                capabilities.isActionListMandatoryOnTableCreation = attr.value.aclcapability.is_action_list_mandatory;
            }
            else
            {
                SWSS_LOG_WARN("Failed to query ACL %s action capabilities - "
                        "API assumed to be not implemented, using defaults",
                        stage_str);
                initDefaultAclActionCapabilities(stage);
            }

            // put capabilities in state DB
            putAclActionCapabilityInDB(stage);
        }
    }
    else
    {
        SWSS_LOG_WARN("Failed to query maximum ACL action count - "
                "API assumed to be not implemented, using defaults capabilities for both %s and %s",
                STAGE_INGRESS, STAGE_EGRESS);
        for (auto stage: {ACL_STAGE_INGRESS, ACL_STAGE_EGRESS})
        {
            initDefaultAclActionCapabilities(stage);
            putAclActionCapabilityInDB(stage);
        }
    }

    /* For those ACL action entry attributes for which acl parameter is enumeration (metadata->isenum == true)
     * we can query enum values which are implemented by vendor SAI.
     * For this purpose we may want to use "sai_query_attribute_enum_values_capability"
     * from SAI object API call (saiobject.h).
     * However, right now libsairedis does not support SAI object API, so we will just
     * put all values as supported for now.
     */

    queryAclActionAttrEnumValues(ACTION_PACKET_ACTION,
                                 aclL3ActionLookup,
                                 aclPacketActionLookup);
    queryAclActionAttrEnumValues(ACTION_DTEL_FLOW_OP,
                                 aclDTelActionLookup,
                                 aclDTelFlowOpTypeLookup);
}

void AclOrch::putAclActionCapabilityInDB(acl_stage_type_t stage)
{
    vector<FieldValueTuple> fvVector;
    auto stage_str = (stage == ACL_STAGE_INGRESS ? STAGE_INGRESS : STAGE_EGRESS);

    auto field = std::string("ACL_ACTIONS") + '|' + stage_str;
    auto& capabilities = m_aclCapabilities[stage];
    auto& acl_action_set = capabilities.actionList;

    string delimiter;
    ostringstream acl_action_value_stream;
    ostringstream is_action_list_mandatory_stream;

    for (const auto& action_map: {aclL3ActionLookup, aclMirrorStageLookup, aclDTelActionLookup})
    {
        for (const auto& it: action_map)
        {
            auto saiAction = AclEntryActionToAclAction(it.second);
            if (acl_action_set.find(saiAction) != acl_action_set.cend())
            {
                acl_action_value_stream << delimiter << it.first;
                delimiter = comma;
            }
        }
    }


    is_action_list_mandatory_stream << boolalpha << capabilities.isActionListMandatoryOnTableCreation;

    fvVector.emplace_back(STATE_DB_ACL_ACTION_FIELD_IS_ACTION_LIST_MANDATORY, is_action_list_mandatory_stream.str());
    fvVector.emplace_back(STATE_DB_ACL_ACTION_FIELD_ACTION_LIST, acl_action_value_stream.str());

    for (auto const& it : m_L3V4V6Capability)
    {
        string value = it.second ? "true" : "false";
        if (it.first == stage)
        {
            fvVector.emplace_back(STATE_DB_ACL_L3V4V6_SUPPORTED, value);
        }
    }

    m_aclStageCapabilityTable.set(stage_str, fvVector);
}

void AclOrch::initDefaultAclActionCapabilities(acl_stage_type_t stage)
{
    m_aclCapabilities[stage] = defaultAclActionsSupported.at(stage);

    SWSS_LOG_INFO("Assumed %s %zu actions to be supported:",
            stage == ACL_STAGE_INGRESS ? STAGE_INGRESS : STAGE_EGRESS,
            m_aclCapabilities[stage].actionList.size());

    for (auto action: m_aclCapabilities[stage].actionList)
    {
        SWSS_LOG_INFO("    %s", sai_serialize_enum(action, &sai_metadata_enum_sai_acl_action_type_t).c_str());
    }
    // put capabilities in state DB
    putAclActionCapabilityInDB(stage);
}

template<typename AclActionAttrLookupT>
void AclOrch::queryAclActionAttrEnumValues(const string &action_name,
                                           const acl_rule_attr_lookup_t& ruleAttrLookupMap,
                                           const AclActionAttrLookupT lookupMap)
{
    vector<FieldValueTuple> fvVector;
    auto acl_attr = ruleAttrLookupMap.at(action_name);
    auto acl_action = AclEntryActionToAclAction(acl_attr);

    /* if the action is not supported then no need to do secondary query for
     * supported values
     */
    if (isAclActionSupported(ACL_STAGE_INGRESS, acl_action) ||
        isAclActionSupported(ACL_STAGE_EGRESS, acl_action))
    {
        string delimiter;
        ostringstream acl_action_value_stream;
        auto field = std::string("ACL_ACTION") + '|' + action_name;

        const auto* meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_ENTRY, acl_attr);
        if (meta == nullptr)
        {
            SWSS_LOG_THROW("Metadata null pointer returned by sai_metadata_get_attr_metadata for action %s",
                           action_name.c_str());
        }

        if (!meta->isenum)
        {
            SWSS_LOG_THROW("%s is not an enum", action_name.c_str());
        }

        vector<int32_t> values_list(meta->enummetadata->valuescount);
        sai_s32_list_t values;
        values.count = static_cast<uint32_t>(values_list.size());
        values.list = values_list.data();

        auto status = sai_query_attribute_enum_values_capability(gSwitchId,
                                                                 SAI_OBJECT_TYPE_ACL_ENTRY,
                                                                 acl_attr,
                                                                 &values);
        if (status == SAI_STATUS_SUCCESS)
        {
            for (size_t i = 0; i < values.count; i++)
            {
                m_aclEnumActionCapabilities[acl_action].insert(values.list[i]);
            }
        }
        else
        {
            SWSS_LOG_WARN("Failed to query enum values supported for ACL action %s - "
                    "API is not implemented, assuming all values are supported for this action",
                    action_name.c_str());
            /* assume all enum values are supported */
            for (size_t i = 0; i < meta->enummetadata->valuescount; i++)
            {
                m_aclEnumActionCapabilities[acl_action].insert(meta->enummetadata->values[i]);
            }
        }

        // put supported values in DB
        for (const auto& it: lookupMap)
        {
            const auto foundIt = m_aclEnumActionCapabilities[acl_action].find(it.second);
            if (foundIt == m_aclEnumActionCapabilities[acl_action].cend())
            {
                continue;
            }
            acl_action_value_stream << delimiter << it.first;
            delimiter = comma;
        }

        fvVector.emplace_back(field, acl_action_value_stream.str());
    }

    m_switchOrch->set_switch_capability(fvVector);
}

AclOrch::AclOrch(vector<TableConnector>& connectors, DBConnector* stateDb, SwitchOrch *switchOrch,
        PortsOrch *portOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch, DTelOrch *dtelOrch) :
        Orch(connectors),
        m_aclStageCapabilityTable(stateDb, STATE_ACL_STAGE_CAPABILITY_TABLE_NAME),
        m_aclTableStateTable(stateDb, STATE_ACL_TABLE_TABLE_NAME),
        m_aclRuleStateTable(stateDb, STATE_ACL_RULE_TABLE_NAME),
        m_switchOrch(switchOrch),
        m_mirrorOrch(mirrorOrch),
        m_neighOrch(neighOrch),
        m_routeOrch(routeOrch),
        m_dTelOrch(dtelOrch),
        m_flex_counter_manager(
            ACL_COUNTER_FLEX_COUNTER_GROUP,
            StatsMode::READ,
            ACL_COUNTER_DEFAULT_POLLING_INTERVAL_MS,
            ACL_COUNTER_DEFAULT_ENABLED_STATE
        )
{
    SWSS_LOG_ENTER();

    init(connectors, portOrch, mirrorOrch, neighOrch, routeOrch);

    if (m_dTelOrch)
    {
        m_dTelOrch->attach(this);
        createDTelWatchListTables();
    }
}

AclOrch::~AclOrch()
{
    m_mirrorOrch->detach(this);

    if (m_dTelOrch)
    {
        m_dTelOrch->detach(this);
    }

    deleteDTelWatchListTables();
}

void AclOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (type != SUBJECT_TYPE_MIRROR_SESSION_CHANGE &&
            type != SUBJECT_TYPE_INT_SESSION_CHANGE &&
            type != SUBJECT_TYPE_PORT_CHANGE)
    {
        return;
    }

    // ACL table deals with port change
    // ACL rule deals with mirror session change and int session change
    for (auto& table : m_AclTables)
    {
        if (type == SUBJECT_TYPE_PORT_CHANGE)
        {
            table.second.onUpdate(type, cntx);
        }
        else
        {
            for (auto& rule : table.second.rules)
            {
                rule.second->onUpdate(type, cntx);
            }
        }
    }
}

void AclOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();

    if (table_name == CFG_ACL_TABLE_TABLE_NAME || table_name == APP_ACL_TABLE_TABLE_NAME)
    {
        doAclTableTask(consumer);
    }
    else if (table_name == CFG_ACL_RULE_TABLE_NAME || table_name == APP_ACL_RULE_TABLE_NAME)
    {
        doAclRuleTask(consumer);
    }
    else if (table_name == CFG_ACL_TABLE_TYPE_TABLE_NAME || table_name == APP_ACL_TABLE_TYPE_TABLE_NAME)
    {
        doAclTableTypeTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }
}

void AclOrch::getAddDeletePorts(AclTable    &newT,
                                AclTable    &curT,
                                set<string> &addSet,
                                set<string> &delSet)
{
    set<string> newPortSet, curPortSet;

    // Collect new ports
    for (auto p : newT.pendingPortSet)
    {
        newPortSet.insert(p);
    }
    for (auto p : newT.portSet)
    {
        newPortSet.insert(p);
    }

    // Collect current ports
    for (auto p : curT.pendingPortSet)
    {
        curPortSet.insert(p);
    }
    for (auto p : curT.portSet)
    {
        curPortSet.insert(p);
    }

    // Get all the ports to be added
    std::set_difference(newPortSet.begin(), newPortSet.end(),
                        curPortSet.begin(), curPortSet.end(),
                        std::inserter(addSet, addSet.end()));
    // Get all the ports to be deleted
    std::set_difference(curPortSet.begin(), curPortSet.end(),
                        newPortSet.begin(), newPortSet.end(),
                        std::inserter(delSet, delSet.end()));

}

bool AclOrch::updateAclTablePorts(AclTable &newTable, AclTable &curTable)
{
    sai_object_id_t    port_oid = SAI_NULL_OBJECT_ID;
    set<string>        addPortSet, deletePortSet;

    SWSS_LOG_ENTER();
    getAddDeletePorts(newTable, curTable, addPortSet, deletePortSet);

    // Lets first unbind and unlink ports to be removed
    for (auto p : deletePortSet)
    {
        SWSS_LOG_NOTICE("Deleting port %s from ACL list %s",
                        p.c_str(), curTable.id.c_str());
        if (curTable.pendingPortSet.find(p) != curTable.pendingPortSet.end())
        {
            SWSS_LOG_NOTICE("Removed:%s from pendingPortSet", p.c_str());
            curTable.pendingPortSet.erase(p);
        }
        else if (curTable.portSet.find(p) != curTable.portSet.end())
        {
            Port port;
            if (!gPortsOrch->getPort(p, port))
            {
                SWSS_LOG_ERROR("Unable to retrieve OID for port %s", p.c_str());
                continue;
            }

            getAclBindPortId(port, port_oid);
            assert(port_oid != SAI_NULL_OBJECT_ID);
            assert(curTable.ports.find(port_oid) != curTable.ports.end());
            if (curTable.ports[port_oid] != SAI_NULL_OBJECT_ID)
            {
                // Unbind and unlink
                SWSS_LOG_NOTICE("Unbind and Unlink:%s", p.c_str());
                curTable.unbind(port_oid);
                curTable.unlink(port_oid);
            }
            SWSS_LOG_NOTICE("Removed:%s from portSet", p.c_str());
            curTable.portSet.erase(p);
        }
    }

    // Now link and bind ports to be added
    for (auto p : addPortSet)
    {
        SWSS_LOG_NOTICE("Adding port %s to ACL list %s",
                        p.c_str(), curTable.id.c_str());
        Port port;
        if (!gPortsOrch->getPort(p, port))
        {
            curTable.pendingPortSet.emplace(p);
            continue;
        }

        if (!getAclBindPortId(port, port_oid))
        {
            // We do NOT expect this to happen at all.
            // If at all happens, lets catch it here!
            throw runtime_error("updateAclTablePorts: Couldn't find portOID");
        }

        curTable.portSet.emplace(p);

        // Link and bind
        SWSS_LOG_NOTICE("Link and Bind:%s", p.c_str());
        curTable.link(port_oid);
        curTable.bind(port_oid);
    }
    return true;
}

bool AclOrch::updateAclTable(AclTable &currentTable, AclTable &newTable)
{
    SWSS_LOG_ENTER();

    currentTable.description = newTable.description;
    if (!updateAclTablePorts(newTable, currentTable))
    {
        SWSS_LOG_ERROR("Failed to update ACL table port list");
        return false;
    }

    return true;
}

bool AclOrch::updateAclTable(string table_id, AclTable &table)
{
    SWSS_LOG_ENTER();

    auto tableOid = getTableById(table_id);
    if (tableOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to update ACL table %s: object doesn't exist", table_id.c_str());
        return false;
    }

    if (!updateAclTable(m_AclTables.at(tableOid), table))
    {
        SWSS_LOG_ERROR("Failed to update ACL table %s", table_id.c_str());
        return false;
    }

    return true;
}

bool AclOrch::addAclTable(AclTable &newTable)
{
    SWSS_LOG_ENTER();

    string table_id = newTable.id;
    if (newTable.type.getName() == TABLE_TYPE_CTRLPLANE)
    {
        m_ctrlAclTables.emplace(table_id, newTable);
        SWSS_LOG_NOTICE("Created control plane ACL table %s", newTable.id.c_str());
        return true;
    }

    sai_object_id_t table_oid = getTableById(table_id);
    auto table_stage = newTable.stage;

    if (table_oid != SAI_NULL_OBJECT_ID)
    {
        /* If ACL table exists, remove the table first.*/
        if (!removeAclTable(table_id))
        {
            SWSS_LOG_ERROR("Failed to remove existing ACL table %s before adding the new one",
                    table_id.c_str());
            return false;
        }
    }
    else
    {
        // If ACL table is new, check for the existence of current mirror tables
        // Note: only one table per mirror type can be created
        auto table_type = newTable.type;
        if (table_type.getName() == TABLE_TYPE_MIRROR || table_type.getName() == TABLE_TYPE_MIRRORV6)
        {
            string mirror_type;
            if (table_type.getName() == TABLE_TYPE_MIRROR && !m_mirrorTableId[table_stage].empty())
            {
                mirror_type = TABLE_TYPE_MIRROR;
            }

            if (table_type.getName() == TABLE_TYPE_MIRRORV6 && !m_mirrorV6TableId[table_stage].empty())
            {
                mirror_type = TABLE_TYPE_MIRRORV6;
            }

            if (!mirror_type.empty())
            {
                string stage_str = table_stage == ACL_STAGE_INGRESS ? "INGRESS" : "EGRESS";
                SWSS_LOG_ERROR(
                    "Mirror table %s (%s) has already been created",
                    mirror_type.c_str(),
                    stage_str.c_str());
                return false;
            }
        }
    }

    // Check if a separate mirror table is needed or not based on the platform
    if (newTable.type.getName() == TABLE_TYPE_MIRROR || newTable.type.getName() == TABLE_TYPE_MIRRORV6)
    {
        if (m_isCombinedMirrorV6Table &&
                (!m_mirrorTableId[table_stage].empty() ||
                !m_mirrorV6TableId[table_stage].empty())) {
            string orig_table_name;

            // If v4 table is created, mark v6 table is created
            if (!m_mirrorTableId[table_stage].empty())
            {
                orig_table_name = m_mirrorTableId[table_stage];
                m_mirrorV6TableId[table_stage] = newTable.id;
            }
            // If v6 table is created, mark v4 table is created
            else
            {
                orig_table_name = m_mirrorV6TableId[table_stage];
                m_mirrorTableId[table_stage] = newTable.id;
            }

            SWSS_LOG_NOTICE("Created ACL table %s as a sibling of %s",
                    newTable.id.c_str(), orig_table_name.c_str());

            return true;
        }
    }
    // Update matching field according to ACL stage
    newTable.addStageMandatoryMatchFields();

    // Add mandatory ACL action if not present
    // We need to call addMandatoryActions here because addAclTable is directly called in other orchs.
    // The action_list is already added if the ACL table creation is triggered by CONFIGDD, but calling addMandatoryActions
    // twice will make no effect
    newTable.addMandatoryActions();
    if (createBindAclTable(newTable, table_oid))
    {
        m_AclTables[table_oid] = newTable;
        SWSS_LOG_NOTICE("Created ACL table %s oid:%" PRIx64,
                newTable.id.c_str(), table_oid);

        // Mark the existence of the mirror table
        if (newTable.type.getName() == TABLE_TYPE_MIRROR)
        {
            m_mirrorTableId[table_stage] = table_id;
        }
        else if (newTable.type.getName() == TABLE_TYPE_MIRRORV6)
        {
            m_mirrorV6TableId[table_stage] = table_id;
        }

        // We use SAI_ACL_BIND_POINT_TYPE_SWITCH for PFCWD table in DNX platform.
        // This bind type requires to bind the table to switch.
        string platform = getenv("platform") ? getenv("platform") : "";
        string sub_platform = getenv("sub_platform") ? getenv("sub_platform") : "";
        if (platform == BRCM_PLATFORM_SUBSTRING && sub_platform == BRCM_DNX_PLATFORM_SUBSTRING &&
            newTable.type.getName() == TABLE_TYPE_PFCWD)
        {
            if(!gSwitchOrch->bindAclTableToSwitch(ACL_STAGE_EGRESS, newTable.getOid()))
            {
                return false;
            }
            newTable.bindToSwitch = true;
        }

        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to create ACL table %s", table_id.c_str());
        return false;
    }
}

bool AclOrch::removeAclTable(string table_id)
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Skip deleting ACL table %s. Table does not exist.", table_id.c_str());
        return true;
    }

    /* If ACL rules associate with this table, remove the rules first.*/
    bool suc = m_AclTables[table_oid].clear();
    if (!suc) return false;

    // Unbind table from switch if needed.
    AclTable &table = m_AclTables.at(table_oid);
    if (table.bindToSwitch)
    {
        // Only bind egress table to switch for now.
        assert(table->stage == ACL_STAGE_EGRESS);
        if(!gSwitchOrch->unbindAclTableFromSwitch(ACL_STAGE_EGRESS, table.getOid()))
        {
            return false;
        }
    }

    if (deleteUnbindAclTable(table_oid) == SAI_STATUS_SUCCESS)
    {
        auto stage = m_AclTables[table_oid].stage;
        auto type = m_AclTables[table_oid].type;

        sai_acl_stage_t sai_stage = (stage == ACL_STAGE_INGRESS) ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
        for (const auto& bpointType: type.getBindPointTypes())
        {
            gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, sai_stage, bpointType, table_oid);
        }

        SWSS_LOG_NOTICE("Successfully deleted ACL table %s", table_id.c_str());
        m_AclTables.erase(table_oid);

        // Clear mirror table information
        // If the v4 and v6 ACL mirror tables are combined together,
        // remove both of them.
        if (m_mirrorTableId[stage] == table_id)
        {
            m_mirrorTableId[stage].clear();
            if (m_isCombinedMirrorV6Table)
            {
                m_mirrorV6TableId[stage].clear();
            }
        }
        else if (m_mirrorV6TableId[stage] == table_id)
        {
            m_mirrorV6TableId[stage].clear();
            if (m_isCombinedMirrorV6Table)
            {
                m_mirrorTableId[stage].clear();
            }
        }

        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to delete ACL table %s.", table_id.c_str());
        return false;
    }
}

bool AclOrch::addAclTableType(const AclTableType& tableType)
{
    SWSS_LOG_ENTER();

    if (tableType.getName().empty())
    {
        SWSS_LOG_ERROR("Received table type without a name");
        return false;
    }

    if (m_AclTableTypes.find(tableType.getName()) != m_AclTableTypes.end())
    {
        SWSS_LOG_ERROR("Table type %s already exists", tableType.getName().c_str());
        return false;
    }

    m_AclTableTypes.emplace(tableType.getName(), tableType);
    return true;
}

bool AclOrch::removeAclTableType(const string& tableTypeName)
{
    // It is Ok to remove table type that is in use by AclTable.
    // AclTable holds a copy of AclTableType and there is no
    // SAI object associated with AclTableType.
    // So it is no harm to remove it without validation.
    // The upper layer can although ensure that
    // user does not remove table type that is referenced
    // by an ACL table.
    if (!m_AclTableTypes.erase(tableTypeName))
    {
        SWSS_LOG_ERROR("Unknown table type %s", tableTypeName.c_str());
        return false;
    }

    return true;
}

bool AclOrch::addAclRule(shared_ptr<AclRule> newRule, string table_id)
{
    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to add ACL rule in ACL table %s. Table doesn't exist", table_id.c_str());
        return false;
    }

    if (!m_AclTables[table_oid].add(newRule))
    {
        return false;
    }

    if (newRule->hasCounter())
    {
        registerFlexCounter(*newRule);
    }

    return true;
}

bool AclOrch::removeAclRule(string table_id, string rule_id)
{
    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Skip removing rule %s from ACL table %s. Table does not exist", rule_id.c_str(), table_id.c_str());
        return true;
    }

    auto rule = getAclRule(table_id, rule_id);
    if (!rule)
    {
        SWSS_LOG_NOTICE("ACL rule [%s] in table [%s] already deleted",
                        rule_id.c_str(), table_id.c_str());
        return true;
    }

    if (rule->hasCounter())
    {
        deregisterFlexCounter(*rule);
    }

    return m_AclTables[table_oid].remove(rule_id);
}

AclRule* AclOrch::getAclRule(string table_id, string rule_id)
{
    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Table %s does not exist", table_id.c_str());
        return nullptr;
    }

    const auto& rule_it = m_AclTables[table_oid].rules.find(rule_id);
    if (rule_it == m_AclTables[table_oid].rules.end())
    {
        SWSS_LOG_INFO("Rule %s doesn't exist", rule_id.c_str());
        return nullptr;
    }

    return rule_it->second.get();
}

bool AclOrch::updateAclRule(string table_id, string rule_id, string attr_name, void *data, bool oper)
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = getTableById(table_id);
    string attr_value;

    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to update ACL rule in ACL table %s. Table doesn't exist", table_id.c_str());
        return false;
    }

    auto rule_it = m_AclTables[table_oid].rules.find(rule_id);
    if (rule_it == m_AclTables[table_oid].rules.end())
    {
        SWSS_LOG_ERROR("Failed to update ACL rule in ACL table %s. Rule doesn't exist", rule_id.c_str());
        return false;
    }

    switch (aclMatchLookup[attr_name])
    {
        case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS:
        {
            sai_object_id_t port_oid = *(sai_object_id_t *)data;
            vector<sai_object_id_t> in_ports = rule_it->second->getInPorts();

            if (oper == RULE_OPER_ADD)
            {
                in_ports.push_back(port_oid);
            }
            else
            {
                for (auto port_iter = in_ports.begin(); port_iter != in_ports.end(); port_iter++)
                {
                    if (*port_iter == port_oid)
                    {
                        in_ports.erase(port_iter);
                        break;
                    }
                }
            }

            for (const auto& port_iter: in_ports)
            {
                Port p;
                gPortsOrch->getPort(port_iter, p);
                attr_value += p.m_alias;
                attr_value += ',';
            }

            if (!attr_value.empty())
            {
                attr_value.pop_back();
            }

            rule_it->second->validateAddMatch(MATCH_IN_PORTS, attr_value);
            rule_it->second->updateInPorts();
        }
        break;

        default:
            SWSS_LOG_ERROR("Acl rule update not supported for attr name %s", attr_name.c_str());
        break;
    }

    return true;
}

bool AclOrch::updateAclRule(string table_id, string rule_id, bool enableCounter)
{
    SWSS_LOG_ENTER();

    auto tableOid = getTableById(table_id);
    if (tableOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR(
            "Failed to update ACL rule %s: ACL table %s doesn't exist",
            rule_id.c_str(),
            table_id.c_str()
        );
        return false;
    }

    const auto &cit = m_AclTables.at(tableOid).rules.find(rule_id);
    if (cit == m_AclTables.at(tableOid).rules.cend())
    {
        SWSS_LOG_ERROR(
            "Failed to update ACL rule %s in ACL table %s: object doesn't exist",
            rule_id.c_str(),
            table_id.c_str()
        );
        return false;
    }

    auto &rule = cit->second;

    if (enableCounter)
    {
        if (!rule->enableCounter())
        {
            SWSS_LOG_ERROR(
                "Failed to enable ACL counter for ACL rule %s in ACL table %s",
                rule_id.c_str(),
                table_id.c_str()
            );
            return false;
        }

        registerFlexCounter(*rule);
        return true;
    }

    deregisterFlexCounter(*rule);
    if (!rule->disableCounter())
    {
        SWSS_LOG_ERROR(
            "Failed to disable ACL counter for ACL rule %s in ACL table %s",
            rule_id.c_str(),
            table_id.c_str()
        );
        return false;
    }

    return true;
}

bool AclOrch::updateAclRule(shared_ptr<AclRule> updatedRule)
{
    SWSS_LOG_ENTER();

    auto tableId = updatedRule->getTableId();
    sai_object_id_t tableOid = getTableById(tableId);
    if (tableOid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to add ACL rule in ACL table %s. Table doesn't exist", tableId.c_str());
        return false;
    }

    if (!m_AclTables[tableOid].updateRule(updatedRule))
    {
        return false;
    }

    return true;
}

bool AclOrch::isCombinedMirrorV6Table()
{
    return m_isCombinedMirrorV6Table;
}

bool AclOrch::isAclMirrorTableSupported(string type) const
{
    const auto &cit = m_mirrorTableCapabilities.find(type);
    if (cit == m_mirrorTableCapabilities.cend())
    {
        return false;
    }

    return cit->second;
}

bool AclOrch::isAclMirrorV4Supported() const
{
    return isAclMirrorTableSupported(TABLE_TYPE_MIRROR);
}

bool AclOrch::isAclMirrorV6Supported() const
{
    return isAclMirrorTableSupported(TABLE_TYPE_MIRRORV6);
}

bool AclOrch::isAclActionListMandatoryOnTableCreation(acl_stage_type_t stage) const
{
    const auto& it = m_aclCapabilities.find(stage);
    if (it == m_aclCapabilities.cend())
    {
        return false;
    }
    return it->second.isActionListMandatoryOnTableCreation;
}

bool AclOrch::isAclL3V4V6TableSupported(acl_stage_type_t stage) const
{
    const auto& it = m_L3V4V6Capability.find(stage);
    if (it == m_L3V4V6Capability.cend())
    {
        return false;
    }
    return it->second;
}

bool AclOrch::isAclActionSupported(acl_stage_type_t stage, sai_acl_action_type_t action) const
{
    const auto& it = m_aclCapabilities.find(stage);
    if (it == m_aclCapabilities.cend())
    {
        return false;
    }
    const auto& actionCapability = it->second;
    return actionCapability.actionList.find(action) != actionCapability.actionList.cend();
}

bool AclOrch::isAclActionEnumValueSupported(sai_acl_action_type_t action, sai_acl_action_parameter_t param) const
{
    const auto& it = m_aclEnumActionCapabilities.find(action);
    if (it == m_aclEnumActionCapabilities.cend())
    {
        return false;
    }
    return it->second.find(param.s32) != it->second.cend();
}

void AclOrch::doAclTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string op = kfvOp(t);

        SWSS_LOG_DEBUG("OP: %s, TABLE_ID: %s", op.c_str(), table_id.c_str());

        if (op == SET_COMMAND)
        {
            AclTable newTable(this);
            string tableTypeName;
            bool bAllAttributesOk = true;

            newTable.id = table_id;
            // Scan all attributes
            for (auto itp : kfvFieldsValues(t))
            {
                string attr_name = to_upper(fvField(itp));
                string attr_value = fvValue(itp);

                SWSS_LOG_DEBUG("TABLE ATTRIBUTE: %s : %s", attr_name.c_str(), attr_value.c_str());

                if (attr_name == ACL_TABLE_DESCRIPTION)
                {
                    newTable.description = attr_value;
                }
                else if (attr_name == ACL_TABLE_TYPE)
                {
                    if (!processAclTableType(attr_value, tableTypeName))
                    {
                        SWSS_LOG_ERROR("Failed to process ACL table %s type",
                                table_id.c_str());
                        bAllAttributesOk = false;
                        break;
                    }
                }
                else if (attr_name == ACL_TABLE_PORTS)
                {
                    if (!processAclTablePorts(attr_value, newTable))
                    {
                        SWSS_LOG_ERROR("Failed to process ACL table %s ports",
                                table_id.c_str());
                        bAllAttributesOk = false;
                        break;
                    }
                }
                else if (attr_name == ACL_TABLE_STAGE)
                {
                   if (!processAclTableStage(attr_value, newTable.stage))
                   {
                       SWSS_LOG_ERROR("Failed to process ACL table %s stage",
                               table_id.c_str());
                       bAllAttributesOk = false;
                       break;
                   }
                }
                else if (attr_name == ACL_TABLE_SERVICES)
                {
                    // TODO: validate control plane ACL table has this attribute
                    continue;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown table attribute '%s'", attr_name.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }

            auto tableType = getAclTableType(tableTypeName);
            if (!tableType)
            {
                it++;
                continue;
            }

            newTable.validateAddType(*tableType);
            // Add mandatory ACL action if not present
            newTable.addMandatoryActions();
            // validate and create/update ACL Table
            if (bAllAttributesOk && newTable.validate())
            {
                // If the the table already exists and meets the below condition(s)
                // update the table. Otherwise delete and re-create
                // Condition 1: Table's TYPE and STAGE hasn't changed

                sai_object_id_t table_oid = getTableById(table_id);
                if (table_oid != SAI_NULL_OBJECT_ID &&
                    !isAclTableTypeUpdated(newTable.type.getName(),
                                           m_AclTables[table_oid]) &&
                    !isAclTableStageUpdated(newTable.stage,
                                            m_AclTables[table_oid]))
                {
                    // Update the existing table using the info in newTable
                    if (updateAclTable(m_AclTables[table_oid], newTable))
                    {
                        SWSS_LOG_NOTICE("Successfully updated existing ACL table %s",
                                        table_id.c_str());
                        // Mark ACL table as ACTIVE
                        setAclTableStatus(table_id, AclObjectStatus::ACTIVE);
                        it = consumer.m_toSync.erase(it);
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to update existing ACL table %s",
                                        table_id.c_str());
                        it++;
                    }
                }
                else
                {
                    if (addAclTable(newTable))
                    {
                        // Mark ACL table as ACTIVE
                        setAclTableStatus(table_id, AclObjectStatus::ACTIVE);
                        it = consumer.m_toSync.erase(it);
                    }
                    else
                    {
                        setAclTableStatus(table_id, AclObjectStatus::PENDING_CREATION);
                        it++;
                    }
                }
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                // Mark the ACL table as inactive if the configuration is invalid
                setAclTableStatus(table_id, AclObjectStatus::INACTIVE);
                SWSS_LOG_ERROR("Failed to create ACL table %s, invalid configuration",
                        table_id.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeAclTable(table_id))
            {
                // Remove ACL table status from STATE_DB
                removeAclTableStatus(table_id);
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                // Set the status of ACL_TABLE to pending removal if removeAclTable returns error
                setAclTableStatus(table_id, AclObjectStatus::PENDING_REMOVAL);
                it++;
            }
        }
        else
        {
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
    }
}

void AclOrch::doAclRuleTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string rule_id = key.substr(found + 1);
        string op = kfvOp(t);

        SWSS_LOG_INFO("OP: %s, TABLE_ID: %s, RULE_ID: %s", op.c_str(), table_id.c_str(), rule_id.c_str());

        if (table_id.empty())
        {
            SWSS_LOG_WARN("ACL rule with RULE_ID: %s is not valid as TABLE_ID is empty", rule_id.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            bool bAllAttributesOk = true;
            shared_ptr<AclRule> newRule;

            // Get the ACL table OID
            sai_object_id_t table_oid = getTableById(table_id);

            /* ACL table is not yet created or ACL table is a control plane table */
            if (table_oid == SAI_NULL_OBJECT_ID)
            {

                /* Skip the control plane rules */
                if (m_ctrlAclTables.find(table_id) != m_ctrlAclTables.end())
                {
                    SWSS_LOG_INFO("Skip control plane ACL rule %s", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                SWSS_LOG_INFO("Wait for ACL table %s to be created", table_id.c_str());
                it++;
                continue;
            }

            auto type = m_AclTables[table_oid].type.getName();
            auto stage = m_AclTables[table_oid].stage;
            if (type == TABLE_TYPE_MIRROR || type == TABLE_TYPE_MIRRORV6)
            {
                type = table_id == m_mirrorTableId[stage] ? TABLE_TYPE_MIRROR : TABLE_TYPE_MIRRORV6;
            }


            try
            {
                newRule = AclRule::makeShared(this, m_mirrorOrch, m_dTelOrch, rule_id, table_id, t);
            }
            catch (exception &e)
            {
                SWSS_LOG_ERROR("Error while creating ACL rule %s: %s", rule_id.c_str(), e.what());
                it = consumer.m_toSync.erase(it);
                return;
            }
            bool bHasTCPFlag = false;
            bool bHasIPProtocol = false;
            bool bHasIPV4 = false;
            bool bHasIPV6 = false;
            for (const auto& itr : kfvFieldsValues(t))
            {
                string attr_name = to_upper(fvField(itr));
                string attr_value = fvValue(itr);

                SWSS_LOG_INFO("ATTRIBUTE: %s %s", attr_name.c_str(), attr_value.c_str());
                if (attr_name == MATCH_TCP_FLAGS)
                {
                    bHasTCPFlag = true;
                }
                if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
                {
                    bHasIPV4 = true;
                }
                if (attr_name == MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6)
                {
                    bHasIPV6 = true;
                }
                if (attr_name == MATCH_IP_PROTOCOL || attr_name == MATCH_NEXT_HEADER)
                {
                    bHasIPProtocol = true;
                }
                if (newRule->validateAddPriority(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added priority attribute");
                }
                else if (newRule->validateAddMatch(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added match attribute '%s'", attr_name.c_str());
                }
                else if (newRule->validateAddAction(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added action attribute '%s'", attr_name.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown or invalid rule attribute '%s : %s'", attr_name.c_str(), attr_value.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }
            // If acl rule is to match TCP_FLAGS, and IP_PROTOCOL(NEXT_HEADER) is not set
            // we set IP_PROTOCOL(NEXT_HEADER) to 6 to match TCP explicitly
            if (bHasTCPFlag && !bHasIPProtocol)
            {
                string attr_name;
                if (type == TABLE_TYPE_MIRRORV6 || type == TABLE_TYPE_L3V6)
                {
                    attr_name = MATCH_NEXT_HEADER;
                }
                else
                {
                    attr_name = MATCH_IP_PROTOCOL;

                }
                string attr_value = std::to_string(TCP_PROTOCOL_NUM);
                if (newRule->validateAddMatch(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Automatically added match attribute '%s : %s'", attr_name.c_str(), attr_value.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to add attribute '%s : %s'", attr_name.c_str(), attr_value.c_str());
                }
            }

            if (bHasIPV4 && bHasIPV6)
	    {
		    if (type == TABLE_TYPE_L3V4V6)
		    {
			    SWSS_LOG_ERROR("Rule '%s' is invalid since it has both v4 and v6 matchfields.", rule_id.c_str());
			    bAllAttributesOk = false;
		    }
	    }

            // validate and create ACL rule
            if (bAllAttributesOk && newRule->validate())
            {
                if (addAclRule(newRule, table_id))
                {
                    setAclRuleStatus(table_id, rule_id, AclObjectStatus::ACTIVE);
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    setAclRuleStatus(table_id, rule_id, AclObjectStatus::PENDING_CREATION);
                    it++;
                }
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                // Mark the rule inactive if the configuration is invalid
                setAclRuleStatus(table_id, rule_id, AclObjectStatus::INACTIVE);
                SWSS_LOG_ERROR("Failed to create ACL rule. Rule configuration is invalid");
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeAclRule(table_id, rule_id))
            {
                removeAclRuleStatus(table_id, rule_id);
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                // Mark pending removal status if removeAclRule returns error
                setAclRuleStatus(table_id, rule_id, AclObjectStatus::PENDING_REMOVAL);
                it++;
            }
        }
        else
        {
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
    }
}

void AclOrch::doAclTableTypeTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto keyOpFieldValues = it->second;
        auto key = kfvKey(keyOpFieldValues);
        auto op = kfvOp(keyOpFieldValues);

        if (op == SET_COMMAND)
        {
            AclTableTypeBuilder builder;
            if (!AclTableTypeParser().parse(key, kfvFieldsValues(keyOpFieldValues), builder))
            {
                SWSS_LOG_ERROR("Failed to parse ACL table type configuration %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            addAclTableType(builder.build());
            SWSS_LOG_NOTICE("Created ACL table type %s", key.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            removeAclTableType(key);
            SWSS_LOG_NOTICE("Removed ACL table type %s", key.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool AclOrch::processAclTablePorts(string portList, AclTable &aclTable)
{
    SWSS_LOG_ENTER();

    auto port_list = tokenize(portList, ',');
    set<string> ports(port_list.begin(), port_list.end());

    for (auto alias : ports)
    {
        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_INFO("Add unready port %s to pending list for ACL table %s",
                    alias.c_str(), aclTable.id.c_str());
            aclTable.pendingPortSet.emplace(alias);
            continue;
        }

        sai_object_id_t bind_port_id;
        if (!getAclBindPortId(port, bind_port_id))
        {
            SWSS_LOG_ERROR("Failed to get port %s bind port ID for ACL table %s",
                    alias.c_str(), aclTable.id.c_str());
            return false;
        }

        aclTable.link(bind_port_id);
        aclTable.portSet.emplace(alias);
    }

    return true;
}

bool AclOrch::isAclTableTypeUpdated(string table_type, AclTable &t)
{
    if (m_isCombinedMirrorV6Table && (table_type == TABLE_TYPE_MIRROR || table_type == TABLE_TYPE_MIRRORV6))
    {
        // TABLE_TYPE_MIRRORV6 and TABLE_TYPE_MIRROR should be treated as same type in combined scenario
        return !(t.type.getName() == TABLE_TYPE_MIRROR || t.type.getName() == TABLE_TYPE_MIRRORV6);
    }
    return (table_type != t.type.getName());
}

bool AclOrch::processAclTableType(string type, string &out_table_type)
{
    SWSS_LOG_ENTER();

    if (type.empty())
    {
        return false;
    }

    out_table_type = type;

    return true;
}

bool AclOrch::isAclTableStageUpdated(acl_stage_type_t acl_stage, AclTable &t)
{
    return (acl_stage != t.stage);
}

bool AclOrch::processAclTableStage(string stage, acl_stage_type_t &acl_stage)
{
    SWSS_LOG_ENTER();

    auto iter = aclStageLookUp.find(to_upper(stage));

    if (iter == aclStageLookUp.end())
    {
        acl_stage = ACL_STAGE_UNKNOWN;
        return false;
    }

    acl_stage = iter->second;

    return true;
}

const AclTableType* AclOrch::getAclTableType(const string& tableTypeName) const
{
    auto it = m_AclTableTypes.find(to_upper(tableTypeName));
    if (it == m_AclTableTypes.end())
    {
        SWSS_LOG_INFO("Failed to find ACL table type %s", tableTypeName.c_str());
        return nullptr;
    }

    return &it->second;
}

sai_object_id_t AclOrch::getTableById(string table_id)
{
    SWSS_LOG_ENTER();

    if (table_id.empty())
    {
        SWSS_LOG_WARN("table_id is empty");
        return SAI_NULL_OBJECT_ID;
    }

    for (auto it : m_AclTables)
    {
        if (it.second.id == table_id)
        {
            return it.first;
        }
    }

    // Check if the table is a mirror table and a sibling mirror table is created
    for (auto stage: {ACL_STAGE_INGRESS, ACL_STAGE_EGRESS}) {
        if (m_isCombinedMirrorV6Table &&
                (table_id == m_mirrorTableId[stage] || table_id == m_mirrorV6TableId[stage]))
        {
            // If the table is v4, the corresponding v6 table is already created
            if (table_id == m_mirrorTableId[stage])
            {
                return getTableById(m_mirrorV6TableId[stage]);
            }
            // If the table is v6, the corresponding v4 table is already created
            else
            {
                return getTableById(m_mirrorTableId[stage]);
            }
        }
    }

    return SAI_NULL_OBJECT_ID;
}

const AclTable *AclOrch::getTableByOid(sai_object_id_t oid) const
{
   const auto& it = m_AclTables.find(oid);
   if (it == m_AclTables.cend())
   {
       return nullptr;
   }
   return &it->second;
}

bool AclOrch::createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid)
{
    SWSS_LOG_ENTER();

    bool suc = aclTable.create();
    if (!suc) return false;

    table_oid = aclTable.getOid();
    sai_status_t status = bindAclTable(aclTable);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind table %s to ports",
                aclTable.id.c_str());
        return false;
    }
    return true;
}

sai_status_t AclOrch::deleteUnbindAclTable(sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    if ((status = bindAclTable(m_AclTables[table_oid], false)) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind table %s",
                m_AclTables[table_oid].id.c_str());
        return status;
    }

    return sai_acl_api->remove_acl_table(table_oid);
}

sai_status_t AclOrch::bindAclTable(AclTable &aclTable, bool bind)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    SWSS_LOG_NOTICE("%s table %s to ports", bind ? "Bind" : "Unbind", aclTable.id.c_str());

    if (aclTable.ports.empty())
    {
        SWSS_LOG_WARN("Port list is empty for %s table", aclTable.id.c_str());
        return SAI_STATUS_SUCCESS;
    }

    bind ? aclTable.bind() : aclTable.unbind();

    return status;
}

void AclOrch::createDTelWatchListTables()
{
    SWSS_LOG_ENTER();

    AclTableTypeBuilder builder;

    AclTable dtelWLTable(this, TABLE_TYPE_DTEL_FLOW_WATCHLIST);

    dtelWLTable.validateAddStage(ACL_STAGE_INGRESS);
    dtelWLTable.validateAddType(builder
        .withBindPointType(SAI_ACL_BIND_POINT_TYPE_SWITCH)
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TUNNEL_VNI))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_INNER_ETHER_TYPE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DSCP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER))
        .withAction(SAI_ACL_ACTION_TYPE_ACL_DTEL_FLOW_OP)
        .withAction(SAI_ACL_ACTION_TYPE_DTEL_INT_SESSION)
        .withAction(SAI_ACL_ACTION_TYPE_DTEL_DROP_REPORT_ENABLE)
        .withAction(SAI_ACL_ACTION_TYPE_DTEL_TAIL_DROP_REPORT_ENABLE)
        .withAction(SAI_ACL_ACTION_TYPE_DTEL_REPORT_ALL_PACKETS)
        .withAction(SAI_ACL_ACTION_TYPE_DTEL_FLOW_SAMPLE_PERCENT)
        .build()
    );
    dtelWLTable.setDescription("Dataplane Telemetry Watchlist table");

    addAclTable(dtelWLTable);
}

void AclOrch::deleteDTelWatchListTables()
{
    SWSS_LOG_ENTER();

    removeAclTable(TABLE_TYPE_DTEL_FLOW_WATCHLIST);
}

void AclOrch::registerFlexCounter(const AclRule& rule)
{
    SWSS_LOG_ENTER();

    auto ruleIdentifier = generateAclRuleIdentifierInCountersDb(rule);
    auto counterOidStr = sai_serialize_object_id(rule.getCounterOid());

    unordered_set<string> serializedCounterStatAttrs;
    for (const auto& counterAttrPair: aclCounterLookup)
    {
        sai_acl_counter_attr_t id {};
        tie(std::ignore, id) = counterAttrPair;
        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_ACL_COUNTER, id);
        if (!meta)
        {
            SWSS_LOG_THROW("SAI Bug: Failed to get metadata of attribute %d for SAI_OBJECT_TYPE_ACL_COUNTER", id);
        }
        serializedCounterStatAttrs.insert(sai_serialize_attr_id(*meta));
    }

    m_flex_counter_manager.setCounterIdList(rule.getCounterOid(), CounterType::ACL_COUNTER, serializedCounterStatAttrs);
    m_countersDb.hset(COUNTERS_ACL_COUNTER_RULE_MAP, ruleIdentifier, counterOidStr);
}

void AclOrch::deregisterFlexCounter(const AclRule& rule)
{
    auto ruleIdentifier = generateAclRuleIdentifierInCountersDb(rule);
    m_countersDb.hdel(COUNTERS_ACL_COUNTER_RULE_MAP, ruleIdentifier);
    m_flex_counter_manager.clearCounterIdList(rule.getCounterOid());
}

string AclOrch::generateAclRuleIdentifierInCountersDb(const AclRule& rule) const
{
    return rule.getTableId() + m_countersTable.getTableNameSeparator() + rule.getId();
}

bool AclOrch::getAclBindPortId(Port &port, sai_object_id_t &port_id)
{
    SWSS_LOG_ENTER();

    switch (port.m_type)
    {
        case Port::PHY:
            if (port.m_lag_member_id != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("Invalid configuration. Bind table to LAG member %s is not allowed", port.m_alias.c_str());
                return false;
            }
            else
            {
                port_id = port.m_port_id;
            }
            break;
        case Port::LAG:
            port_id = port.m_lag_id;
            break;
        case Port::VLAN:
            port_id = port.m_vlan_info.vlan_oid;
            break;
        default:
            SWSS_LOG_ERROR("Failed to process port. Incorrect port %s type %d", port.m_alias.c_str(), port.m_type);
            return false;
    }

    return true;
}

// Set the status of ACL table in STATE_DB
void AclOrch::setAclTableStatus(string table_name, AclObjectStatus status)
{
    vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("status", aclObjectStatusLookup[status]);
    m_aclTableStateTable.set(table_name, fvVector);
}

// Remove the status record of given ACL table from STATE_DB
void AclOrch::removeAclTableStatus(string table_name)
{
    m_aclTableStateTable.del(table_name);
}

// Set the status of ACL rule in STATE_DB
void AclOrch::setAclRuleStatus(string table_name, string rule_name, AclObjectStatus status)
{
    vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("status", aclObjectStatusLookup[status]);
    m_aclRuleStateTable.set(table_name + string("|") + rule_name, fvVector);
}

// Remove the status record of given ACL rule from STATE_DB
void AclOrch::removeAclRuleStatus(string table_name, string rule_name)
{
    m_aclRuleStateTable.del(table_name + string("|") + rule_name);
}

// Remove all ACL table status from STATE_DB
void AclOrch::removeAllAclTableStatus()
{
    vector<string> keys;
    m_aclTableStateTable.getKeys(keys);

    for (auto key : keys)
    {
        m_aclTableStateTable.del(key);
    }
}

// Remove all ACL rule status from STATE_DB
void AclOrch::removeAllAclRuleStatus()
{
    vector<string> keys;
    m_aclRuleStateTable.getKeys(keys);
    for (auto key : keys)
    {
        m_aclRuleStateTable.del(key);
    }
}
