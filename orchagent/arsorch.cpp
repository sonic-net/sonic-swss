#include "arsorch.h"
#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include "converter.h"

#include <algorithm>

using namespace std;
using namespace swss;

extern sai_ars_api_t*         sai_ars_api;
extern sai_ars_profile_api_t* sai_ars_profile_api;
extern sai_object_id_t        gSwitchId;

static const map<string, sai_ars_mode_t> arsModeLookup = {
    {"flowlet-quality",  SAI_ARS_MODE_FLOWLET_QUALITY},
    {"flowlet-random",   SAI_ARS_MODE_FLOWLET_RANDOM},
    {"packet-quality",   SAI_ARS_MODE_PER_PACKET_QUALITY},
    {"packet-random",    SAI_ARS_MODE_PER_PACKET_RANDOM},
    {"fixed",            SAI_ARS_MODE_FIXED},
};

ArsOrch::ArsOrch(DBConnector *configDb,
                 DBConnector *stateDb,
                 const vector<string> &tableNames,
                 SwitchOrch *switchOrch,
                 PortsOrch  *portsOrch)
    : Orch(configDb, tableNames),
      m_switchOrch(switchOrch),
      m_portsOrch(portsOrch),
      m_stateArsCapTable(stateDb, STATE_ARS_CAPABILITY_TABLE_NAME),
      m_cfgArsTable(configDb, CFG_ARS_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    publishArsCaps();
}

void ArsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    const string &tableName = consumer.getTableName();

    if (tableName == CFG_ARS_TABLE_NAME)
        doArsGlobalTask(consumer);
    else if (tableName == CFG_ARS_PROFILE_TABLE_NAME)
        doArsProfileTask(consumer);
    else if (tableName == CFG_ARS_OBJECT_TABLE_NAME)
        doArsObjectTask(consumer);
    else if (tableName == CFG_ARS_INTERFACES_TABLE_NAME)
        doArsInterfaceTask(consumer);
    else if (tableName == CFG_ARS_PORT_PROFILE_TABLE_NAME)
        doArsPortProfileTask(consumer);
    else if (tableName == CFG_ARS_NEXTHOPS_TABLE_NAME)
        doArsNexthopsTask(consumer);
    else
        SWSS_LOG_ERROR("ArsOrch: unknown table %s", tableName.c_str());
}

/* ── Global enable/disable ───────────────────────────────────────────── */

void ArsOrch::doArsGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string key = kfvKey(kfv);
        string op  = kfvOp(kfv);

        if (key != "GLOBAL")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            bool wantEnable = false;
            string profileName;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                if (fvField(fv) == "admin_state")
                    wantEnable = (fvValue(fv) == "up");
                else if (fvField(fv) == "profile")
                    profileName = fvValue(fv);
            }

            if (wantEnable && !m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally enabled");
                m_arsEnabled = true;
            }
            else if (!wantEnable && m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing globally disabled");
                m_arsEnabled = false;
            }

            if (!profileName.empty() && profileName != m_globalProfileName)
            {
                auto profIt = m_arsProfiles.find(profileName);
                if (profIt != m_arsProfiles.end() &&
                    profIt->second.profileOid != SAI_NULL_OBJECT_ID)
                {
                    bindArsProfileToSwitch(profIt->second.profileOid);
                    m_globalProfileName = profileName;
                    SWSS_LOG_NOTICE("ARS: bound profile '%s' to switch",
                                    profileName.c_str());
                }
                else
                {
                    m_globalProfileName = profileName;
                    SWSS_LOG_NOTICE("ARS: profile '%s' requested but not yet "
                                    "created — will bind when available",
                                    profileName.c_str());
                }
            }
            else if (profileName.empty() && !m_globalProfileName.empty())
            {
                bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
                m_globalProfileName.clear();
                SWSS_LOG_NOTICE("ARS: unbound profile from switch");
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsEnabled)
            {
                SWSS_LOG_NOTICE("ARS: Adaptive Routing global entry removed — disabling");
                m_arsEnabled = false;
            }
            if (m_activeSwitchProfileOid != SAI_NULL_OBJECT_ID)
            {
                bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
                m_globalProfileName.clear();
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Profile (EWMA tuning) ──────────────────────────────────────── */

void ArsOrch::doArsProfileTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsProfileEntry entry;
            if (m_arsProfiles.count(name))
                entry = m_arsProfiles[name];

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "load_past_weight")    entry.loadPastWeight   = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_weight")  entry.loadFutureWeight = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_weight") entry.loadCurrentWeight = static_cast<uint32_t>(stoul(value));
                else if (field == "load_exponent")       entry.loadExponent      = static_cast<uint32_t>(stoul(value));
                else if (field == "max_flows")           entry.maxFlows          = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_min_val")   entry.loadPastMinVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_max_val")   entry.loadPastMaxVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_min_val") entry.loadFutureMinVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_max_val") entry.loadFutureMaxVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_min_val") entry.loadCurrentMinVal = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_max_val") entry.loadCurrentMaxVal = static_cast<uint32_t>(stoul(value));
                else if (field == "port_load_past")      entry.loadPastEnable    = (value == "true");
                else if (field == "port_load_future")    entry.loadFutureEnable  = (value == "true");
                else if (field == "ipv4_enable")         entry.ipv4Enable        = (value == "true");
                else if (field == "ipv6_enable")         entry.ipv6Enable        = (value == "true");
                else if (field == "sampling_interval")   entry.samplingInterval  = static_cast<uint32_t>(stoul(value));
                else if (field == "random_seed")         entry.randomSeed        = static_cast<uint32_t>(stoul(value));
                else if (field == "algorithm")
                {
                    /* Only EWMA supported; log if different */
                    if (value != "EWMA")
                        SWSS_LOG_WARN("ARS: unsupported algorithm '%s', using EWMA", value.c_str());
                }
                else
                    SWSS_LOG_WARN("ARS: unknown profile field '%s'", field.c_str());
            }

            /* loadCurrentEnable derived from weight > 0 for SAI compat */
            entry.loadCurrentEnable = (entry.loadCurrentWeight > 0);

            if (entry.profileOid == SAI_NULL_OBJECT_ID)
            {
                if (!createArsProfile(name, entry))
                {
                    SWSS_LOG_ERROR("ARS: failed to create profile %s", name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                entry.profileOid = m_arsProfiles[name].profileOid;

                if (!m_globalProfileName.empty() && m_globalProfileName == name &&
                    m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
                {
                    bindArsProfileToSwitch(entry.profileOid);
                }
            }
            else
            {
                sai_object_id_t oid = entry.profileOid;
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT,   entry.loadPastWeight);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT,  entry.loadFutureWeight);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST,       entry.loadPastEnable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE,     entry.loadFutureEnable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT,    entry.loadCurrentEnable);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT,       entry.loadExponent);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_MAX_FLOWS,                entry.maxFlows);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL,        entry.loadPastMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL,        entry.loadPastMaxVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL,      entry.loadFutureMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL,      entry.loadFutureMaxVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL,     entry.loadCurrentMinVal);
                updateArsProfileAttr(oid, SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL,     entry.loadCurrentMaxVal);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV4,          entry.ipv4Enable);
                updateArsProfileAttrBool(oid, SAI_ARS_PROFILE_ATTR_ENABLE_IPV6,          entry.ipv6Enable);
                m_arsProfiles[name] = entry;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!removeArsProfile(name))
                SWSS_LOG_ERROR("ARS: failed to remove profile %s", name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Object (per-NHG) ───────────────────────────────────────────── */

void ArsOrch::doArsObjectTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsObjectEntry entry;
            if (m_arsObjects.count(name))
                entry = m_arsObjects[name];

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "assign_mode") entry.mode     = parseArsMode(value);
                else if (field == "idle_time")   entry.idleTime = static_cast<uint32_t>(stoul(value));
                else if (field == "max_flows")   entry.maxFlows = static_cast<uint32_t>(stoul(value));
                else if (field == "admin_state") entry.enabled  = (value == "up");
                else if (field == "profile")     entry.profileName = value;
            }

            if (!m_arsEnabled)
            {
                SWSS_LOG_WARN("ARS: global ARS not enabled, deferring object %s", name.c_str());
                ++it;
                continue;
            }

            if (entry.arsOid == SAI_NULL_OBJECT_ID)
            {
                if (!createArsObject(name, entry))
                {
                    SWSS_LOG_ERROR("ARS: failed to create object %s", name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            else
            {
                sai_object_id_t oid = entry.arsOid;
                setArsObjectAttr(oid, SAI_ARS_ATTR_MODE,      (uint32_t)entry.mode);
                setArsObjectAttr(oid, SAI_ARS_ATTR_IDLE_TIME,  entry.idleTime);
                setArsObjectAttr(oid, SAI_ARS_ATTR_MAX_FLOWS,  entry.maxFlows);
                m_arsObjects[name] = entry;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!removeArsObject(name))
                SWSS_LOG_ERROR("ARS: failed to remove object %s", name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Interfaces (per-port membership) ────────────────────────────── */

void ArsOrch::doArsInterfaceTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string portName = kfvKey(kfv);
        string op       = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsInterfaceEntry entry;
            if (m_arsInterfaces.count(portName))
                entry = m_arsInterfaces[portName];

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "admin_state")              entry.enabled = (value == "up");
                else if (field == "ars_object")               entry.arsObject = value;
                else if (field == "port_profile")             entry.portProfile = value;
                else if (field == "link_utilization_threshold") entry.linkUtilThreshold = static_cast<uint32_t>(stoul(value));
                else if (field == "weight")                   { /* stored in YANG but no SAI attr */ }
                else
                    SWSS_LOG_WARN("ARS: unknown interface field '%s' on %s",
                                  field.c_str(), portName.c_str());
            }

            bool prevEnabled = (m_arsEnabledPorts.find(portName) != m_arsEnabledPorts.end());

            if (entry.enabled && !prevEnabled)
            {
                if (setPortArsEnable(portName, true))
                    m_arsEnabledPorts.insert(portName);
                else
                    SWSS_LOG_ERROR("ARS: failed to enable ARS on port %s", portName.c_str());
            }
            else if (!entry.enabled && prevEnabled)
            {
                if (setPortArsEnable(portName, false))
                    m_arsEnabledPorts.erase(portName);
                else
                    SWSS_LOG_ERROR("ARS: failed to disable ARS on port %s", portName.c_str());
            }

            m_arsInterfaces[portName] = entry;

            if (!entry.arsObject.empty())
            {
                SWSS_LOG_NOTICE("ARS: interface %s associated with ARS object '%s'",
                                portName.c_str(), entry.arsObject.c_str());
            }
            if (!entry.portProfile.empty())
            {
                SWSS_LOG_NOTICE("ARS: interface %s bound to port-profile '%s'",
                                portName.c_str(), entry.portProfile.c_str());
            }
            if (entry.linkUtilThreshold > 0)
            {
                SWSS_LOG_NOTICE("ARS: interface %s link-utilization-threshold=%u%%",
                                portName.c_str(), entry.linkUtilThreshold);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsEnabledPorts.count(portName))
            {
                setPortArsEnable(portName, false);
                m_arsEnabledPorts.erase(portName);
            }
            m_arsInterfaces.erase(portName);
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Port Profile ────────────────────────────────────────────────── */

void ArsOrch::doArsPortProfileTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string name = kfvKey(kfv);
        string op   = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            ArsPortProfileEntry entry;
            if (m_arsPortProfiles.count(name))
                entry = m_arsPortProfiles[name];

            for (auto &fv : kfvFieldsValues(kfv))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if      (field == "load_past_min_val")    entry.loadPastMinVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_past_max_val")    entry.loadPastMaxVal    = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_min_val")  entry.loadFutureMinVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_future_max_val")  entry.loadFutureMaxVal  = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_min_val") entry.loadCurrentMinVal = static_cast<uint32_t>(stoul(value));
                else if (field == "load_current_max_val") entry.loadCurrentMaxVal = static_cast<uint32_t>(stoul(value));
                else if (field == "enable" || field == "port_load_past_weight" ||
                         field == "port_load_future_weight" || field == "load_scaling_factor")
                {
                    SWSS_LOG_NOTICE("ARS: port-profile %s field '%s' = '%s' (stored, no SAI attr)",
                                    name.c_str(), field.c_str(), value.c_str());
                }
            }

            m_arsPortProfiles[name] = entry;
            SWSS_LOG_NOTICE("ARS: port-profile '%s' updated", name.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            m_arsPortProfiles.erase(name);
            SWSS_LOG_NOTICE("ARS: port-profile '%s' removed", name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── ARS Nexthops (prefix → ARS object binding for routeorch) ────────── */

void ArsOrch::doArsNexthopsTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &kfv = it->second;
        string prefix = kfvKey(kfv);
        string op     = kfvOp(kfv);

        if (op == SET_COMMAND)
        {
            string arsObjName;
            for (auto &fv : kfvFieldsValues(kfv))
            {
                if (fvField(fv) == "ars_object")
                    arsObjName = fvValue(fv);
            }

            if (arsObjName.empty())
            {
                SWSS_LOG_WARN("ARS: nexthop entry for prefix %s has no ars_object",
                              prefix.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_nexthopArsBindings[prefix] = arsObjName;
            SWSS_LOG_NOTICE("ARS: prefix %s mapped to ARS object '%s'",
                            prefix.c_str(), arsObjName.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            m_nexthopArsBindings.erase(prefix);
            SWSS_LOG_NOTICE("ARS: prefix %s ARS binding removed", prefix.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

/* ── SAI helpers: ARS Profile ────────────────────────────────────────── */

bool ArsOrch::createArsProfile(const string &name, const ArsProfileEntry &entry)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_ARS_PROFILE_ATTR_ALGO;
    attr.value.s32 = entry.algorithm;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST;
    attr.value.booldata = entry.loadPastEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT;
    attr.value.u8 = (uint8_t)entry.loadPastWeight;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE;
    attr.value.booldata = entry.loadFutureEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT;
    attr.value.u8 = (uint8_t)entry.loadFutureWeight;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT;
    attr.value.booldata = entry.loadCurrentEnable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT;
    attr.value.u8 = (uint8_t)entry.loadExponent;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV4;
    attr.value.booldata = entry.ipv4Enable;
    attrs.push_back(attr);

    attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV6;
    attr.value.booldata = entry.ipv6Enable;
    attrs.push_back(attr);

    if (entry.maxFlows > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_MAX_FLOWS;
        attr.value.u32 = entry.maxFlows;
        attrs.push_back(attr);
    }

    if (entry.loadPastMinVal > 0 || entry.loadPastMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL;
        attr.value.u32 = entry.loadPastMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL;
        attr.value.u32 = entry.loadPastMaxVal;
        attrs.push_back(attr);
    }

    if (entry.loadFutureMinVal > 0 || entry.loadFutureMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL;
        attr.value.u32 = entry.loadFutureMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL;
        attr.value.u32 = entry.loadFutureMaxVal;
        attrs.push_back(attr);
    }

    if (entry.loadCurrentMinVal > 0 || entry.loadCurrentMaxVal > 0)
    {
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL;
        attr.value.u32 = entry.loadCurrentMinVal;
        attrs.push_back(attr);
        attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL;
        attr.value.u32 = entry.loadCurrentMaxVal;
        attrs.push_back(attr);
    }

    sai_object_id_t profileOid;
    sai_status_t status = sai_ars_profile_api->create_ars_profile(
        &profileOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: create_ars_profile failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: created profile %s OID 0x%" PRIx64, name.c_str(), profileOid);

    ArsProfileEntry stored = entry;
    stored.profileOid = profileOid;
    m_arsProfiles[name] = stored;

    if (m_activeSwitchProfileOid == SAI_NULL_OBJECT_ID)
    {
        bindArsProfileToSwitch(profileOid);
    }

    return true;
}

bool ArsOrch::removeArsProfile(const string &name)
{
    SWSS_LOG_ENTER();

    auto it = m_arsProfiles.find(name);
    if (it == m_arsProfiles.end())
        return true;

    sai_object_id_t oid = it->second.profileOid;

    if (oid == m_activeSwitchProfileOid)
    {
        bindArsProfileToSwitch(SAI_NULL_OBJECT_ID);
    }

    sai_status_t status = sai_ars_profile_api->remove_ars_profile(oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: remove_ars_profile failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: removed profile %s", name.c_str());
    m_arsProfiles.erase(it);
    return true;
}

bool ArsOrch::updateArsProfileAttr(sai_object_id_t oid, sai_ars_profile_attr_t attrId, uint32_t val)
{
    sai_attribute_t attr;
    attr.id = attrId;

    switch (attrId)
    {
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT:
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT:
    case SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT:
        attr.value.u8 = (uint8_t)val;
        break;
    default:
        attr.value.u32 = val;
        break;
    }

    sai_status_t status = sai_ars_profile_api->set_ars_profile_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_profile_attribute (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

bool ArsOrch::updateArsProfileAttrBool(sai_object_id_t oid, sai_ars_profile_attr_t attrId, bool val)
{
    sai_attribute_t attr;
    attr.id = attrId;
    attr.value.booldata = val;

    sai_status_t status = sai_ars_profile_api->set_ars_profile_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_profile_attribute bool (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

/* ── SAI helpers: ARS Object (per-NHG) ───────────────────────────────── */

bool ArsOrch::createArsObject(const string &name, const ArsObjectEntry &entry)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_ARS_ATTR_MODE;
    attr.value.s32 = entry.mode;
    attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_IDLE_TIME;
    attr.value.u32 = entry.idleTime;
    attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_MAX_FLOWS;
    attr.value.u32 = entry.maxFlows;
    attrs.push_back(attr);

    sai_object_id_t arsOid;
    sai_status_t status = sai_ars_api->create_ars(
        &arsOid, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: create_ars failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: created object %s OID 0x%" PRIx64 " mode %d",
                    name.c_str(), arsOid, (int)entry.mode);

    ArsObjectEntry stored = entry;
    stored.arsOid = arsOid;
    m_arsObjects[name] = stored;
    return true;
}

bool ArsOrch::removeArsObject(const string &name)
{
    SWSS_LOG_ENTER();

    auto it = m_arsObjects.find(name);
    if (it == m_arsObjects.end())
        return true;

    sai_status_t status = sai_ars_api->remove_ars(it->second.arsOid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: remove_ars failed for %s: %s",
                       name.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: removed object %s", name.c_str());
    m_arsObjects.erase(it);
    return true;
}

bool ArsOrch::setArsObjectAttr(sai_object_id_t oid, sai_ars_attr_t attrId, uint32_t val)
{
    sai_attribute_t attr;
    attr.id = attrId;
    if (attrId == SAI_ARS_ATTR_MODE)
        attr.value.s32 = (int32_t)val;
    else
        attr.value.u32 = val;

    sai_status_t status = sai_ars_api->set_ars_attribute(oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set_ars_attribute (attr %d) failed: %s",
                       attrId, sai_serialize_status(status).c_str());
        return false;
    }
    return true;
}

/* ── Bind profile to switch, bind ARS to NHG ─────────────────────────── */

bool ArsOrch::bindArsProfileToSwitch(sai_object_id_t profileOid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    attr.value.oid = profileOid;

    extern sai_switch_api_t *sai_switch_api;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: bind profile to switch failed: %s",
                       sai_serialize_status(status).c_str());
        return false;
    }

    m_activeSwitchProfileOid = profileOid;
    SWSS_LOG_NOTICE("ARS: bound profile OID 0x%" PRIx64 " to switch", profileOid);
    return true;
}

bool ArsOrch::bindArsToNhg(sai_object_id_t nhgOid, sai_object_id_t arsOid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID;
    attr.value.oid = arsOid;

    extern sai_next_hop_group_api_t *sai_next_hop_group_api;
    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_attribute(nhgOid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: bind ARS OID 0x%" PRIx64 " to NHG 0x%" PRIx64 " failed: %s",
                       arsOid, nhgOid, sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: bound ARS OID 0x%" PRIx64 " to NHG 0x%" PRIx64, arsOid, nhgOid);
    return true;
}

/* ── Per-port ARS enable via SAI_PORT_ATTR_ARS_ENABLE ─────────────────── */

bool ArsOrch::setPortArsEnable(const string &portName, bool enable)
{
    SWSS_LOG_ENTER();

    Port port;
    if (!m_portsOrch->getPort(portName, port))
    {
        SWSS_LOG_ERROR("ARS: port %s not found", portName.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ARS_ENABLE;
    attr.value.booldata = enable;

    extern sai_port_api_t *sai_port_api;
    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ARS: set SAI_PORT_ATTR_ARS_ENABLE on %s failed: %s",
                       portName.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    SWSS_LOG_NOTICE("ARS: port %s ARS %s", portName.c_str(), enable ? "enabled" : "disabled");
    return true;
}

/* ── Publish ARS capabilities to STATE_DB ─────────────────────────────── */

void ArsOrch::publishArsCaps()
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> caps;
    caps.emplace_back("ars_supported", "true");
    caps.emplace_back("modes_supported",
                      "flowlet-quality,flowlet-random,packet-quality,packet-random,fixed");
    m_stateArsCapTable.set("switch", caps);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

sai_ars_mode_t ArsOrch::parseArsMode(const string &modeStr) const
{
    auto it = arsModeLookup.find(modeStr);
    if (it != arsModeLookup.end())
        return it->second;

    SWSS_LOG_WARN("ARS: unknown mode '%s', defaulting to flowlet-quality", modeStr.c_str());
    return SAI_ARS_MODE_FLOWLET_QUALITY;
}

sai_object_id_t ArsOrch::getArsProfileOid(const string &name) const
{
    auto it = m_arsProfiles.find(name);
    return (it != m_arsProfiles.end()) ? it->second.profileOid : SAI_NULL_OBJECT_ID;
}

sai_object_id_t ArsOrch::getArsObjectOid(const string &name) const
{
    auto it = m_arsObjects.find(name);
    return (it != m_arsObjects.end()) ? it->second.arsOid : SAI_NULL_OBJECT_ID;
}
