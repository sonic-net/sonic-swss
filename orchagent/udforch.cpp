#include <inttypes.h>
#include "udforch.h"
#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include <nlohmann/json.hpp>

using namespace std;
using namespace swss;

extern sai_udf_api_t* sai_udf_api;
extern sai_object_id_t gSwitchId;

UdfGroup::UdfGroup(const UdfGroupConfig& config) : m_config(config), m_oid(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();
}

UdfGroup::~UdfGroup()
{
    SWSS_LOG_ENTER();
    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        remove();
    }
}

bool UdfGroup::create()
{
    SWSS_LOG_ENTER();

    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("UDF group %s already exists with OID: 0x%" PRIx64,
                       m_config.name.c_str(), m_oid);
        return false;
    }

    if (!isValidUdfGroupLength(m_config.length))
    {
        SWSS_LOG_ERROR("Invalid UDF group length %d for group %s",
                       m_config.length, m_config.name.c_str());
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_UDF_GROUP_ATTR_TYPE;
    attr.value.s32 = m_config.type;
    attrs.push_back(attr);

    attr.id = SAI_UDF_GROUP_ATTR_LENGTH;
    attr.value.u16 = m_config.length;
    attrs.push_back(attr);

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF group %s", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->create_udf_group(&m_oid, gSwitchId,
                                                        static_cast<uint32_t>(attrs.size()),
                                                        attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create UDF group %s, SAI status: %s",
                       m_config.name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Created UDF group %s OID: 0x%" PRIx64 " type: %s length: %d",
                  m_config.name.c_str(), m_oid,
                  getUdfGroupTypeString(m_config.type).c_str(), m_config.length);
    return true;
}

bool UdfGroup::remove()
{
    SWSS_LOG_ENTER();

    if (m_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("UDF group %s does not exist or already removed", m_config.name.c_str());
        return true;
    }

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF group %s removal", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->remove_udf_group(m_oid);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove UDF group %s (OID: 0x%" PRIx64 "), SAI status: %s",
                       m_config.name.c_str(), m_oid, sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Removed UDF group %s OID: 0x%" PRIx64,
                  m_config.name.c_str(), m_oid);
    m_oid = SAI_NULL_OBJECT_ID;
    return true;
}

UdfMatch::UdfMatch(const UdfMatchConfig& config) : m_config(config), m_oid(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();
}

UdfMatch::~UdfMatch()
{
    SWSS_LOG_ENTER();
    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        remove();
    }
}

bool UdfMatch::create()
{
    SWSS_LOG_ENTER();

    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("UDF match %s already exists with OID: 0x%" PRIx64,
                       m_config.name.c_str(), m_oid);
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr = {};

    // UDF_MATCH L2/L3/GRE attrs are sai_acl_field_data_t — the enable flag
    // must be set to true for the criteria to take effect in SAI.
    if (m_config.l2_type != 0 || m_config.l2_type_mask != 0)
    {
        attr.id = SAI_UDF_MATCH_ATTR_L2_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.u16 = m_config.l2_type;
        attr.value.aclfield.mask.u16 = m_config.l2_type_mask;
        attrs.push_back(attr);
    }

    if (m_config.l3_type != 0 || m_config.l3_type_mask != 0)
    {
        attr.id = SAI_UDF_MATCH_ATTR_L3_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.u8 = m_config.l3_type;
        attr.value.aclfield.mask.u8 = m_config.l3_type_mask;
        attrs.push_back(attr);
    }

    if (m_config.gre_type != 0 || m_config.gre_type_mask != 0)
    {
        attr.id = SAI_UDF_MATCH_ATTR_GRE_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.u16 = m_config.gre_type;
        attr.value.aclfield.mask.u16 = m_config.gre_type_mask;
        attrs.push_back(attr);
    }

    if (m_config.l4_dst_port != 0 || m_config.l4_dst_port_mask != 0)
    {
        attr.id = SAI_UDF_MATCH_ATTR_L4_DST_PORT_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.u16 = m_config.l4_dst_port;
        attr.value.aclfield.mask.u16 = m_config.l4_dst_port_mask;
        attrs.push_back(attr);
    }

    attr.id = SAI_UDF_MATCH_ATTR_PRIORITY;
    attr.value.u8 = m_config.priority;
    attrs.push_back(attr);

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF match %s", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->create_udf_match(&m_oid, gSwitchId,
                                                        static_cast<uint32_t>(attrs.size()),
                                                        attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create UDF match %s, SAI status: %s",
                       m_config.name.c_str(), sai_serialize_status(status).c_str());
        m_oid = SAI_NULL_OBJECT_ID;
        return false;
    }

    SWSS_LOG_NOTICE("Created UDF match %s OID: 0x%" PRIx64 " priority: %d",
                  m_config.name.c_str(), m_oid, m_config.priority);
    return true;
}

bool UdfMatch::remove()
{
    SWSS_LOG_ENTER();

    if (m_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("UDF match %s does not exist or already removed", m_config.name.c_str());
        return true;
    }

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF match %s removal", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->remove_udf_match(m_oid);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove UDF match %s (OID: 0x%" PRIx64 "), SAI status: %s",
                       m_config.name.c_str(), m_oid, sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Removed UDF match %s OID: 0x%" PRIx64,
                  m_config.name.c_str(), m_oid);
    m_oid = SAI_NULL_OBJECT_ID;
    return true;
}

Udf::Udf(const UdfConfig& config) : m_config(config), m_oid(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();
}

Udf::~Udf()
{
    SWSS_LOG_ENTER();
    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        remove();
    }
}

bool Udf::create()
{
    SWSS_LOG_ENTER();

    if (m_oid != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("UDF %s already exists with OID: 0x%" PRIx64,
                       m_config.name.c_str(), m_oid);
        return false;
    }

    if (m_config.match_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Invalid UDF match ID for UDF %s", m_config.name.c_str());
        return false;
    }

    if (m_config.group_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Invalid UDF group ID for UDF %s", m_config.name.c_str());
        return false;
    }

    if (!isValidUdfOffset(m_config.offset))
    {
        SWSS_LOG_ERROR("Invalid UDF offset %d for UDF %s",
                       m_config.offset, m_config.name.c_str());
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_UDF_ATTR_MATCH_ID;
    attr.value.oid = m_config.match_id;
    attrs.push_back(attr);

    attr.id = SAI_UDF_ATTR_GROUP_ID;
    attr.value.oid = m_config.group_id;
    attrs.push_back(attr);

    attr.id = SAI_UDF_ATTR_BASE;
    attr.value.s32 = m_config.base;
    attrs.push_back(attr);

    attr.id = SAI_UDF_ATTR_OFFSET;
    attr.value.u16 = m_config.offset;
    attrs.push_back(attr);

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF %s", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->create_udf(&m_oid, gSwitchId,
                                                  static_cast<uint32_t>(attrs.size()),
                                                  attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create UDF %s, SAI status: %s",
                       m_config.name.c_str(), sai_serialize_status(status).c_str());
        m_oid = SAI_NULL_OBJECT_ID;
        return false;
    }

    SWSS_LOG_NOTICE("Created UDF %s OID: 0x%" PRIx64
                   " match: 0x%" PRIx64 " group: 0x%" PRIx64
                   " base: %s offset: %d",
                  m_config.name.c_str(), m_oid, m_config.match_id, m_config.group_id,
                  getUdfBaseTypeString(m_config.base).c_str(), m_config.offset);
    return true;
}

bool Udf::remove()
{
    SWSS_LOG_ENTER();

    if (m_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("UDF %s does not exist or already removed", m_config.name.c_str());
        return true;
    }

    if (!sai_udf_api)
    {
        SWSS_LOG_ERROR("SAI UDF API is not available for UDF %s removal", m_config.name.c_str());
        return false;
    }

    sai_status_t status = sai_udf_api->remove_udf(m_oid);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove UDF %s (OID: 0x%" PRIx64 "), SAI status: %s",
                       m_config.name.c_str(), m_oid, sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Removed UDF %s OID: 0x%" PRIx64,
                  m_config.name.c_str(), m_oid);
    m_oid = SAI_NULL_OBJECT_ID;
    return true;
}

UdfOrch::UdfOrch(DBConnector *configDb, const vector<string> &tableNames) :
    Orch(configDb, tableNames)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("UdfOrch initialized with %zu tables", tableNames.size());
}

UdfOrch::~UdfOrch()
{
    SWSS_LOG_ENTER();
    m_udfs.clear();
    m_udfMatches.clear();
    m_udfGroups.clear();
}

void UdfOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    const string& tableName = consumer.getTableName();

    if (tableName == CFG_UDF_TABLE_NAME)
    {
        doUdfFieldTask(consumer);
    }
    else if (tableName == CFG_UDF_SELECTOR_TABLE_NAME)
    {
        doUdfSelectorTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tableName.c_str());
    }
}

void UdfOrch::doUdfFieldTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        const string& key = kfvKey(t);       // udf_name
        const string& op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            UdfGroupConfig config = {};
            config.name = key;
            config.type = SAI_UDF_GROUP_TYPE_GENERIC;  // Default

            for (const auto& field : kfvFieldsValues(t))
            {
                const string& fieldName = fvField(field);
                const string& fieldValue = fvValue(field);

                if (fieldName == "field_type")
                {
                    config.type = getUdfGroupType(fieldValue);
                }
                else if (fieldName == "length")
                {
                    config.length = static_cast<uint16_t>(stoul(fieldValue));
                }
                // "description" is informational only, not passed to SAI
            }

            if (config.length == 0)
            {
                SWSS_LOG_ERROR("UDF %s missing required field: length", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!addUdfGroup(key, config))
            {
                SWSS_LOG_ERROR("Failed to add UDF group for %s, discarding entry", key.c_str());
            }
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (removeUdfGroup(key))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                ++it;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void UdfOrch::doUdfSelectorTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        const string& key = kfvKey(t);       // "udf_name|selector_name"
        const string& op = kfvOp(t);

        // Parse composite key: udf_name|selector_name
        auto keyTokens = tokenize(key, '|');
        if (keyTokens.size() != 2)
        {
            SWSS_LOG_ERROR("Invalid UDF_SELECTOR key format: %s (expected udf_name|selector_name)", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        const string& udfName     = keyTokens[0];
        const string& selectorName = keyTokens[1];
        string selectorKey = udfName + "|" + selectorName;

        if (op == SET_COMMAND)
        {
            string baseStr;
            uint16_t offset = 0;
            UdfMatchConfig matchConfig = {};

            string selectJson, matchJson;
            for (const auto& field : kfvFieldsValues(t))
            {
                if (fvField(field) == "select")
                    selectJson = fvValue(field);
                else if (fvField(field) == "match")
                    matchJson = fvValue(field);
            }

            // Python str() uses single quotes; normalize to valid JSON
            auto normJson = [](string& s) {
                replace(s.begin(), s.end(), '\'', '"');
            };
            normJson(selectJson);
            normJson(matchJson);

            if (selectJson.empty())
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: missing required 'select' field", selectorKey.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            try
            {
                auto sel = nlohmann::json::parse(selectJson);
                if (sel.contains("base"))
                    baseStr = sel["base"].get<string>();
                if (sel.contains("offset"))
                    offset = static_cast<uint16_t>(stoul(sel["offset"].get<string>()));
            }
            catch (const nlohmann::json::exception& e)
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: failed to parse 'select' JSON: %s",
                               selectorKey.c_str(), e.what());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (matchJson.empty())
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: missing required 'match' field", selectorKey.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            try
            {
                auto mat = nlohmann::json::parse(matchJson);
                if (mat.contains("l2_type"))
                    matchConfig.l2_type = static_cast<uint16_t>(stoul(mat["l2_type"].get<string>(), nullptr, 0));
                if (mat.contains("l2_type_mask"))
                    matchConfig.l2_type_mask = static_cast<uint16_t>(stoul(mat["l2_type_mask"].get<string>(), nullptr, 0));
                if (mat.contains("l3_type"))
                    matchConfig.l3_type = static_cast<uint8_t>(stoul(mat["l3_type"].get<string>(), nullptr, 0));
                if (mat.contains("l3_type_mask"))
                    matchConfig.l3_type_mask = static_cast<uint8_t>(stoul(mat["l3_type_mask"].get<string>(), nullptr, 0));
                if (mat.contains("gre_type"))
                    matchConfig.gre_type = static_cast<uint16_t>(stoul(mat["gre_type"].get<string>(), nullptr, 0));
                if (mat.contains("gre_type_mask"))
                    matchConfig.gre_type_mask = static_cast<uint16_t>(stoul(mat["gre_type_mask"].get<string>(), nullptr, 0));
                if (mat.contains("l4_dst_port"))
                    matchConfig.l4_dst_port = static_cast<uint16_t>(stoul(mat["l4_dst_port"].get<string>(), nullptr, 0));
                if (mat.contains("l4_dst_port_mask"))
                    matchConfig.l4_dst_port_mask = static_cast<uint16_t>(stoul(mat["l4_dst_port_mask"].get<string>(), nullptr, 0));
                if (mat.contains("priority"))
                    matchConfig.priority = static_cast<uint8_t>(stoul(mat["priority"].get<string>()));
            }
            catch (const nlohmann::json::exception& e)
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: failed to parse 'match' JSON: %s",
                               selectorKey.c_str(), e.what());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!isValidUdfBase(baseStr))
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: invalid or missing 'base' field '%s'",
                              selectorKey.c_str(), baseStr.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!isValidUdfOffset(offset))
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: invalid offset %u", selectorKey.c_str(), offset);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (matchConfig.l2_type != 0 && matchConfig.l2_type_mask == 0)
                matchConfig.l2_type_mask = 0xFFFF;
            if (matchConfig.l3_type != 0 && matchConfig.l3_type_mask == 0)
                matchConfig.l3_type_mask = 0xFF;
            if (matchConfig.gre_type != 0 && matchConfig.gre_type_mask == 0)
                matchConfig.gre_type_mask = 0xFFFF;
            if (matchConfig.l4_dst_port != 0 && matchConfig.l4_dst_port_mask == 0)
                matchConfig.l4_dst_port_mask = 0xFFFF;

            bool hasL2  = (matchConfig.l2_type  != 0 || matchConfig.l2_type_mask  != 0);
            bool hasL3  = (matchConfig.l3_type  != 0 || matchConfig.l3_type_mask  != 0);
            bool hasGre = (matchConfig.gre_type != 0 || matchConfig.gre_type_mask != 0);
            bool hasL4  = (matchConfig.l4_dst_port != 0 || matchConfig.l4_dst_port_mask != 0);
            if (!hasL2 && !hasL3 && !hasGre && !hasL4)
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: at least one match criterion required",
                              selectorKey.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            sai_object_id_t groupOid = getUdfGroupOid(udfName);
            if (groupOid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("UDF_SELECTOR %s: parent UDF %s not found yet, retrying",
                             selectorKey.c_str(), udfName.c_str());
                ++it;
                continue;
            }

            UdfMatchSignature sig = buildMatchSignature(matchConfig);
            string matchName = getOrCreateSharedMatch(sig, matchConfig);
            if (matchName.empty())
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: failed to create/get SAI UDF match", selectorKey.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            sai_object_id_t matchOid = getUdfMatchOid(matchName);

            UdfConfig udfConfig = {};
            udfConfig.name = selectorKey;
            udfConfig.group_id = groupOid;
            udfConfig.match_id = matchOid;
            udfConfig.base = getUdfBaseType(baseStr);
            udfConfig.offset = offset;

            if (!addUdf(selectorKey, udfConfig))
            {
                SWSS_LOG_ERROR("UDF_SELECTOR %s: failed to create SAI UDF", selectorKey.c_str());
                releaseSharedMatch(matchName);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_selectorToMatchName[selectorKey] = matchName;

            SWSS_LOG_NOTICE("UDF_SELECTOR %s created: base=%s offset=%u match=%s",
                           selectorKey.c_str(), baseStr.c_str(), offset, matchName.c_str());

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            // Block deletion of last selector while ACL rules reference this UDF
            if (getUdfRuleRefCount(udfName) > 0)
            {
                // Count how many SAI UDF entries still exist for this group
                sai_object_id_t groupOid = getUdfGroupOid(udfName);
                uint32_t remainingSelectors = 0;
                for (const auto& udfEntry : m_udfs)
                {
                    if (udfEntry.second->getConfig().group_id == groupOid)
                        remainingSelectors++;
                }

                if (remainingSelectors <= 1)
                {
                    SWSS_LOG_ERROR("Cannot remove last UDF_SELECTOR %s: UDF %s still referenced "
                                   "by %u ACL rule(s) — delete the rules first",
                                   selectorKey.c_str(), udfName.c_str(),
                                   getUdfRuleRefCount(udfName));
                    ++it;
                    continue;
                }
            }

            if (!removeUdf(selectorKey))
            {
                    ++it;
                continue;
            }

            auto matchIt = m_selectorToMatchName.find(selectorKey);
            if (matchIt != m_selectorToMatchName.end())
            {
                releaseSharedMatch(matchIt->second);
                m_selectorToMatchName.erase(matchIt);
            }

            SWSS_LOG_NOTICE("UDF_SELECTOR %s removed", selectorKey.c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

UdfMatchSignature UdfOrch::buildMatchSignature(const UdfMatchConfig& config)
{
    UdfMatchSignature sig;
    sig.l2_type      = config.l2_type;
    sig.l2_type_mask = config.l2_type_mask;
    sig.l3_type      = config.l3_type;
    sig.l3_type_mask = config.l3_type_mask;
    sig.gre_type      = config.gre_type;
    sig.gre_type_mask = config.gre_type_mask;
    sig.l4_dst_port      = config.l4_dst_port;
    sig.l4_dst_port_mask = config.l4_dst_port_mask;
    sig.priority      = config.priority;
    return sig;
}

string UdfOrch::getOrCreateSharedMatch(const UdfMatchSignature& sig, const UdfMatchConfig& config)
{
    auto matchIt = m_matchSigToName.find(sig);
    if (matchIt != m_matchSigToName.end())
    {
        const string& existingName = matchIt->second;
        m_matchRefCount[existingName]++;
        SWSS_LOG_NOTICE("Reusing shared UDF match %s (refcount=%u)",
                       existingName.c_str(), m_matchRefCount[existingName]);
        return existingName;
    }

    string matchName = "shared_match_" + to_string(m_sharedMatchCounter++);

    UdfMatchConfig newConfig = config;
    newConfig.name = matchName;

    if (!addUdfMatch(matchName, newConfig))
    {
        SWSS_LOG_ERROR("Failed to create shared UDF match %s", matchName.c_str());
        return "";
    }

    m_matchSigToName[sig] = matchName;
    m_matchRefCount[matchName] = 1;

    SWSS_LOG_NOTICE("Created new shared UDF match %s (refcount=1)", matchName.c_str());
    return matchName;
}

void UdfOrch::releaseSharedMatch(const string& matchName)
{
    auto refIt = m_matchRefCount.find(matchName);
    if (refIt == m_matchRefCount.end())
    {
        SWSS_LOG_WARN("releaseSharedMatch: match %s not in refcount map", matchName.c_str());
        return;
    }

    refIt->second--;
    SWSS_LOG_DEBUG("Shared match %s refcount decremented to %u", matchName.c_str(), refIt->second);

    if (refIt->second == 0)
    {
        removeUdfMatch(matchName);
        m_matchRefCount.erase(refIt);

        for (auto sigIt = m_matchSigToName.begin(); sigIt != m_matchSigToName.end(); ++sigIt)
        {
            if (sigIt->second == matchName)
            {
                m_matchSigToName.erase(sigIt);
                break;
            }
        }

        SWSS_LOG_NOTICE("Removed shared UDF match %s (refcount reached 0)", matchName.c_str());
    }
}

bool UdfOrch::addUdfGroup(const string& name, const UdfGroupConfig& config)
{
    SWSS_LOG_ENTER();

    if (!isValidUdfName(name))
    {
        SWSS_LOG_ERROR("Invalid UDF group name: %s", name.c_str());
        return false;
    }

    auto it = m_udfGroups.find(name);
    if (it != m_udfGroups.end())
    {
        const UdfGroupConfig& existingConfig = it->second->getConfig();

        if (existingConfig.type != config.type)
        {
            SWSS_LOG_ERROR("Cannot modify UDF group %s: TYPE field is immutable (existing: %s, new: %s)",
                          name.c_str(),
                          getUdfGroupTypeString(existingConfig.type).c_str(),
                          getUdfGroupTypeString(config.type).c_str());
            return false;
        }

        if (existingConfig.length != config.length)
        {
            SWSS_LOG_ERROR("Cannot modify UDF group %s: LENGTH field is immutable (existing: %u, new: %u)",
                          name.c_str(), existingConfig.length, config.length);
            return false;
        }

        SWSS_LOG_NOTICE("UDF group %s already exists with identical configuration", name.c_str());
        return true;
    }

    auto udfGroup = make_unique<UdfGroup>(config);
    if (!udfGroup->create())
    {
        SWSS_LOG_ERROR("Failed to create UDF group %s", name.c_str());
        return false;
    }

    m_udfGroups[name] = move(udfGroup);
    SWSS_LOG_NOTICE("Added UDF group %s", name.c_str());
    return true;
}

bool UdfOrch::removeUdfGroup(const string& name)
{
    SWSS_LOG_ENTER();

    auto it = m_udfGroups.find(name);
    if (it == m_udfGroups.end())
    {
        SWSS_LOG_WARN("UDF group %s not found", name.c_str());
        return true;
    }

    auto refIt = m_udfGroupRefCount.find(name);
    if (refIt != m_udfGroupRefCount.end() && refIt->second > 0)
    {
        SWSS_LOG_ERROR("Cannot remove UDF group %s: still referenced by %u ACL table(s)",
                       name.c_str(), refIt->second);
        return false;
    }

    sai_object_id_t group_oid = it->second->getOid();

    for (const auto& udfEntry : m_udfs)
    {
        const string& udfName = udfEntry.first;
        const auto& udf = udfEntry.second;
        if (udf->getConfig().group_id == group_oid)
        {
            SWSS_LOG_ERROR("Cannot remove UDF group %s: referenced by UDF %s",
                          name.c_str(), udfName.c_str());
            return false;
        }
    }

    if (!it->second->remove())
    {
        SWSS_LOG_ERROR("Failed to remove UDF group %s from hardware", name.c_str());
        return false;
    }

    m_udfGroups.erase(it);
    SWSS_LOG_NOTICE("Removed UDF group %s", name.c_str());
    return true;
}

UdfGroup* UdfOrch::getUdfGroup(const string& name)
{
    auto it = m_udfGroups.find(name);
    return (it != m_udfGroups.end()) ? it->second.get() : nullptr;
}

sai_object_id_t UdfOrch::getUdfGroupOid(const string& name)
{
    UdfGroup* group = getUdfGroup(name);
    return group ? group->getOid() : SAI_NULL_OBJECT_ID;
}

bool UdfOrch::addUdfMatch(const string& name, const UdfMatchConfig& config)
{
    SWSS_LOG_ENTER();

    if (!isValidUdfName(name))
    {
        SWSS_LOG_ERROR("Invalid UDF match name: %s", name.c_str());
        return false;
    }

    auto it = m_udfMatches.find(name);
    if (it != m_udfMatches.end())
    {
        const UdfMatchConfig& existingConfig = it->second->getConfig();

        bool configChanged = false;
        if (existingConfig.l2_type != config.l2_type || existingConfig.l2_type_mask != config.l2_type_mask)
            configChanged = true;
        if (existingConfig.l3_type != config.l3_type || existingConfig.l3_type_mask != config.l3_type_mask)
            configChanged = true;
        if (existingConfig.gre_type != config.gre_type || existingConfig.gre_type_mask != config.gre_type_mask)
            configChanged = true;
        if (existingConfig.l4_dst_port != config.l4_dst_port || existingConfig.l4_dst_port_mask != config.l4_dst_port_mask)
            configChanged = true;
        if (existingConfig.priority != config.priority)
            configChanged = true;

        if (configChanged)
        {
            SWSS_LOG_ERROR("Cannot modify UDF match %s: match fields are immutable", name.c_str());
            return false;
        }

        SWSS_LOG_NOTICE("UDF match %s already exists with identical configuration", name.c_str());
        return true;
    }

    // Also called directly by BTH setup which bypasses doUdfSelectorTask
    bool hasL2  = (config.l2_type  != 0 || config.l2_type_mask  != 0);
    bool hasL3  = (config.l3_type  != 0 || config.l3_type_mask  != 0);
    bool hasGre = (config.gre_type != 0 || config.gre_type_mask != 0);
    bool hasL4  = (config.l4_dst_port != 0 || config.l4_dst_port_mask != 0);
    if (!hasL2 && !hasL3 && !hasGre && !hasL4)
    {
        SWSS_LOG_ERROR("UDF match %s must have at least one match criteria", name.c_str());
        return false;
    }

    auto udfMatch = make_unique<UdfMatch>(config);
    if (!udfMatch->create())
    {
        SWSS_LOG_ERROR("Failed to create UDF match %s", name.c_str());
        return false;
    }

    m_udfMatches[name] = move(udfMatch);
    SWSS_LOG_NOTICE("Added UDF match %s", name.c_str());
    return true;
}

bool UdfOrch::removeUdfMatch(const string& name)
{
    SWSS_LOG_ENTER();

    auto it = m_udfMatches.find(name);
    if (it == m_udfMatches.end())
    {
        SWSS_LOG_WARN("UDF match %s not found", name.c_str());
        return true;
    }

    sai_object_id_t match_oid = it->second->getOid();

    for (const auto& udfEntry : m_udfs)
    {
        const string& udfName = udfEntry.first;
        const auto& udf = udfEntry.second;
        if (udf->getConfig().match_id == match_oid)
        {
            SWSS_LOG_ERROR("Cannot remove UDF match %s: referenced by UDF %s",
                          name.c_str(), udfName.c_str());
            return false;
        }
    }

    if (!it->second->remove())
    {
        SWSS_LOG_ERROR("Failed to remove UDF match %s from hardware", name.c_str());
        return false;
    }

    m_udfMatches.erase(it);
    SWSS_LOG_NOTICE("Removed UDF match %s", name.c_str());
    return true;
}

UdfMatch* UdfOrch::getUdfMatch(const string& name)
{
    auto it = m_udfMatches.find(name);
    return (it != m_udfMatches.end()) ? it->second.get() : nullptr;
}

sai_object_id_t UdfOrch::getUdfMatchOid(const string& name)
{
    UdfMatch* match = getUdfMatch(name);
    return match ? match->getOid() : SAI_NULL_OBJECT_ID;
}

bool UdfOrch::addUdf(const string& name, const UdfConfig& config)
{
    SWSS_LOG_ENTER();

    if (!isValidUdfName(name))
    {
        SWSS_LOG_ERROR("Invalid UDF name: %s", name.c_str());
        return false;
    }

    auto it = m_udfs.find(name);
    if (it != m_udfs.end())
    {
        const UdfConfig& existingConfig = it->second->getConfig();

        if (existingConfig.base != config.base ||
            existingConfig.offset != config.offset ||
            existingConfig.match_id != config.match_id ||
            existingConfig.group_id != config.group_id)
        {
            SWSS_LOG_ERROR("Cannot modify UDF %s: fields are immutable", name.c_str());
            return false;
        }

        SWSS_LOG_NOTICE("UDF %s already exists with identical configuration", name.c_str());
        return true;
    }

    if (config.match_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Cannot create UDF %s: UDF_MATCH not specified or invalid", name.c_str());
        return false;
    }

    if (config.group_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Cannot create UDF %s: UDF_GROUP not specified or invalid", name.c_str());
        return false;
    }

    auto udf = make_unique<Udf>(config);
    if (!udf->create())
    {
        SWSS_LOG_ERROR("Failed to create UDF %s", name.c_str());
        return false;
    }

    m_udfs[name] = move(udf);
    SWSS_LOG_NOTICE("Added UDF %s", name.c_str());
    return true;
}

bool UdfOrch::removeUdf(const string& name)
{
    SWSS_LOG_ENTER();

    auto it = m_udfs.find(name);
    if (it == m_udfs.end())
    {
        SWSS_LOG_WARN("UDF %s not found", name.c_str());
        return true;
    }

    if (!it->second->remove())
    {
        SWSS_LOG_ERROR("Failed to remove UDF %s from hardware", name.c_str());
        return false;
    }

    m_udfs.erase(it);
    SWSS_LOG_NOTICE("Removed UDF %s", name.c_str());
    return true;
}

Udf* UdfOrch::getUdf(const string& name)
{
    auto it = m_udfs.find(name);
    return (it != m_udfs.end()) ? it->second.get() : nullptr;
}

void UdfOrch::incrementGroupRefCount(const string& name)
{
    m_udfGroupRefCount[name]++;
    SWSS_LOG_DEBUG("UDF group %s ref count incremented to %u", name.c_str(), m_udfGroupRefCount[name]);
}

void UdfOrch::decrementGroupRefCount(const string& name)
{
    auto it = m_udfGroupRefCount.find(name);
    if (it == m_udfGroupRefCount.end() || it->second == 0)
    {
        SWSS_LOG_WARN("UDF group %s ref count decrement underflow", name.c_str());
        return;
    }
    it->second--;
    SWSS_LOG_DEBUG("UDF group %s ref count decremented to %u", name.c_str(), it->second);
}

uint32_t UdfOrch::getGroupRefCount(const string& name) const
{
    auto it = m_udfGroupRefCount.find(name);
    return (it != m_udfGroupRefCount.end()) ? it->second : 0;
}

void UdfOrch::incrementUdfRuleRefCount(const string& udfName)
{
    m_udfRuleRefCount[udfName]++;
    SWSS_LOG_DEBUG("UDF %s rule ref count incremented to %u",
                   udfName.c_str(), m_udfRuleRefCount[udfName]);
}

void UdfOrch::decrementUdfRuleRefCount(const string& udfName)
{
    auto it = m_udfRuleRefCount.find(udfName);
    if (it == m_udfRuleRefCount.end() || it->second == 0)
    {
        SWSS_LOG_WARN("UDF %s rule ref count decrement underflow", udfName.c_str());
        return;
    }
    it->second--;
    SWSS_LOG_DEBUG("UDF %s rule ref count decremented to %u", udfName.c_str(), it->second);
}

uint32_t UdfOrch::getUdfRuleRefCount(const string& udfName) const
{
    auto it = m_udfRuleRefCount.find(udfName);
    return (it != m_udfRuleRefCount.end()) ? it->second : 0;
}
