#include "llrmgr.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <boost/algorithm/string.hpp>

using namespace std;
using namespace swss;

// Field name mappings (INI aliases -> DB field names)
const unordered_map<string,string> LlrMgr::kAliasToDbFieldNames = {
    {"outstanding_frames",  "max_outstanding_frames"},
    {"outstanding_bytes",   "max_outstanding_bytes"},
    {"replay_timer",        "max_replay_timer"},
    {"replay_count",        "max_replay_count"},
    {"pcs_lost_timeout",    "pcs_lost_timeout"},
    {"data_age_timeout",    "data_age_timeout"},
    {"ctlos_spacing",       "ctlos_spacing_bytes"},
    {"init_action",         "init_action"},
    {"flush_action",        "flush_action"},
};

// DB table handler map
const unordered_map<string, LlrMgr::HandlerFunc> LlrMgr::kTableHandlers = {
    {CFG_LLR_PORT_TABLE_NAME,        &LlrMgr::handleLlrPortTable},
    {CFG_LLR_PROFILE_TABLE_NAME,     &LlrMgr::handleLlrProfileTable},
    {CFG_PORT_CABLE_LEN_TABLE_NAME,  &LlrMgr::handlePortCableLenTable},
    {STATE_PORT_TABLE_NAME,          &LlrMgr::handleStatePortTable},
};

void LlrMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string tableName = consumer.getTableName();
    
    // Look up the handler function for table name to handler mapping
    auto it = kTableHandlers.find(tableName);
    if (it != kTableHandlers.end())
    {
        (this->*(it->second))(consumer);
    }
    else
    {
        SWSS_LOG_WARN("Unknown table name: %s", tableName.c_str());
    }
}

void LlrMgr::handleLlrPortTable(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string port = kfvKey(t);
        string op   = kfvOp(t);

        if (op == SET_COMMAND)
        {
            bool isNewPort = (m_portInfoLookup.count(port) == 0);
            auto &portInfo = m_portInfoLookup[port];

            if (isNewPort)
            {
                // New port - Populate cable_length and speed in m_portInfoLookup for this port.
                getCableLengthCfgDb(port);
                getSpeedStateDb(port);
            }

            string oldLocal   = portInfo.llr_local;
            string oldRemote  = portInfo.llr_remote;
            string oldProfile = portInfo.user_profile;

            bool invalidEntry = false;
            for (auto &fieldValue : kfvFieldsValues(t))
            {
                const string &field = fvField(fieldValue);
                const string &value = fvValue(fieldValue);

                if (field == "llr_mode")
                {
                    if (value != "static")
                    {
                        SWSS_LOG_ERROR("Unsupported llr_mode '%s' for port %s; only 'static' is supported",
                                       value.c_str(), port.c_str());
                        invalidEntry = true;
                        break;
                    }
                    portInfo.llr_mode = value;
                }
                else if (field == "llr_local" || field == "llr_remote")
                {
                    if (value != "enabled" && value != "disabled")
                    {
                        SWSS_LOG_ERROR("Invalid value '%s' for field %s on port %s; rejecting entry",
                                       value.c_str(), field.c_str(), port.c_str());
                        invalidEntry = true;
                        break;
                    }
                    if (field == "llr_local")
                        portInfo.llr_local = value;
                    else
                        portInfo.llr_remote = value;
                }
                else if (field == "llr_profile")
                {
                    portInfo.user_profile = value;
                }
                else
                {
                    SWSS_LOG_WARN("Unknown LLR_PORT field '%s' for port %s", field.c_str(), port.c_str());
                }
            }

            if (invalidEntry)
            {
                if (isNewPort)
                {
                    m_portInfoLookup.erase(port);
                }
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // Only publish when profile is ready and publish-relevant fields changed.
            bool wasActive = (!isNewPort && oldLocal == "enabled" && oldRemote == "enabled");

            if (generateLlrProfile(port) &&
                (isNewPort ||
                 portInfo.llr_local != oldLocal ||
                 portInfo.llr_remote != oldRemote ||
                 portInfo.user_profile != oldProfile))
            {
                publishLlrPortToApplDb(port);
            }
            else if (wasActive && !portInfo.hasLlrActive())
            {
                // Port transitioned from active to inactive — clean up profile and APPL_DB.
                disableAndDeleteProfile(port);
            }
        }
        else if (op == DEL_COMMAND)
        {
            // LLR_PORT_TABLE is published only when profile is bind. Hence attempt delete
            // from APPL_DB only when profile is bind.
            if (m_portInfoLookup.count(port) && !m_portInfoLookup[port].llr_profile.empty())
            {
                m_appLlrPortTable.del(port);
            }

            deleteProfile(port);
            m_portInfoLookup.erase(port);
            SWSS_LOG_NOTICE("Deleted LLR port entry for %s from APPL_DB", port.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LlrMgr::handleLlrProfileTable(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string profileName = kfvKey(t);
        string op          = kfvOp(t);

        if (op == SET_COMMAND)
        {
            // User-defined profile created or updated in CONFIG_DB.
            vector<FieldValueTuple> profileFields(kfvFieldsValues(t).begin(), kfvFieldsValues(t).end());

            // Validate mandatory fields before publishing to APPL_DB.
            bool hasFrames = false, hasBytes = false;
            for (const auto &fv : profileFields)
            {
                if (fvField(fv) == "max_outstanding_frames") hasFrames = true;
                if (fvField(fv) == "max_outstanding_bytes")  hasBytes  = true;
            }

            if (!hasFrames || !hasBytes)
            {
                SWSS_LOG_ERROR("LLR_PROFILE '%s': missing mandatory fields (max_outstanding_frames/max_outstanding_bytes); skipping",
                               profileName.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // Only publish to APPL_DB if any of the LLR enabled port is currently references
            // this profile. createProfile() will publish to APPL_DB when a port first binds
            // to it.
            auto refIt = m_profileRefCount.find(profileName);
            if (refIt != m_profileRefCount.end())
            {
                m_appLlrProfileTable.set(profileName, profileFields);
                SWSS_LOG_NOTICE("User-defined LLR profile '%s' published to APPL_DB", profileName.c_str());
            }
            else
            {
                SWSS_LOG_INFO("User-defined LLR profile '%s' stored; not yet bound to any port",
                              profileName.c_str());
            }

            // Re-evaluate ports whose user_profile matches this profile but currently
            // fell back to INI (e.g. profile arrived after LLR_PORT was configured).
            for (auto &portEntry : m_portInfoLookup)
            {
                if (portEntry.second.user_profile == profileName &&
                    portEntry.second.llr_profile != profileName &&
                    portEntry.second.hasLlrActive())
                {
                    if (generateLlrProfile(portEntry.first))
                    {
                        publishLlrPortToApplDb(portEntry.first);
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            // User deleted a profile from CONFIG_DB.
            // Remove from APPL_DB only if no port currently holds a refcount on it;
            // ports still using it will trigger deletion via deleteProfile when they are torn down.
            auto refIt = m_profileRefCount.find(profileName);
            bool hasRefs = (refIt != m_profileRefCount.end() && !refIt->second.empty());
            if (!hasRefs)
            {
                m_appLlrProfileTable.del(profileName);
            }

            // For each port whose user_profile pointed here, clear the override and
            // fall back to INI-derived profile.
            for (auto &portEntry : m_portInfoLookup)
            {
                if (portEntry.second.user_profile == profileName)
                {
                    portEntry.second.user_profile.clear();
                    SWSS_LOG_NOTICE("Port %s user profile '%s' deleted; falling back to INI-derived profile",
                                    portEntry.first.c_str(), profileName.c_str());
                    if (portEntry.second.hasLlrActive())
                    {
                        generateLlrProfile(portEntry.first);
                        publishLlrPortToApplDb(portEntry.first);
                    }
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LlrMgr::handlePortCableLenTable(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            // Each field is a port name; the value is the cable length string (e.g. "40m")
            set<string> updatedPorts;
            for (auto &fieldValue : kfvFieldsValues(t))
            {
                const string &port        = fvField(fieldValue);
                const string &cableLength = fvValue(fieldValue);
                updatedPorts.insert(port);

                if (m_portInfoLookup.count(port))
                {
                    m_portInfoLookup[port].cable_length = cableLength;
                    SWSS_LOG_INFO("Cable length for port %s set to %s", port.c_str(), cableLength.c_str());
                    if (!generateLlrProfile(port))
                    {
                        if (!m_portInfoLookup[port].llr_profile.empty())
                        {
                            disableAndDeleteProfile(port);
                        }
                    }
                    else
                    {
                        publishLlrPortToApplDb(port);
                    }
                }
                else
                {
                    SWSS_LOG_DEBUG("Cable length for port %s skipped (no LLR config)", port.c_str());
                }
            }

            // Detect ports whose cable_length field was removed.
            // any tracked port absent from the update has had its field deleted.
            for (auto &portEntry : m_portInfoLookup)
            {
                if (!portEntry.second.cable_length.empty() &&
                    updatedPorts.find(portEntry.first) == updatedPorts.end())
                {
                    SWSS_LOG_NOTICE("Port %s: cable length removed; clearing", portEntry.first.c_str());
                    portEntry.second.cable_length.clear();
                    if (portEntry.second.hasLlrActive())
                    {
                        SWSS_LOG_NOTICE("Port %s: LLR active; disabling due to cable length removal",
                                        portEntry.first.c_str());
                        disableAndDeleteProfile(portEntry.first);
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            // The table key is a cable-length entry name (e.g. "AZURE"); iterate ports
            for (auto &fieldValue : kfvFieldsValues(t))
            {
                const string &port = fvField(fieldValue);
                if (m_portInfoLookup.count(port))
                {
                    m_portInfoLookup[port].cable_length.clear();
                    SWSS_LOG_INFO("Cleared cable length for port %s", port.c_str());
                    if (m_portInfoLookup[port].hasLlrActive())
                    {
                        SWSS_LOG_NOTICE("Port %s: cable length removed while LLR active; disabling LLR",
                                        port.c_str());
                        disableAndDeleteProfile(port);
                    }
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LlrMgr::handleStatePortTable(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string port = kfvKey(t);
        string op   = kfvOp(t);

        if (op == SET_COMMAND)
        {
            // Only track speed for ports that have LLR configured.
            // Ports without an LLR_PORT entry in CONFIG_DB are ignored here;
            // their speed will be back-filled by getSpeedStateDb() when LLR
            // is later configured via handleLlrPortTable.
            if (!m_portInfoLookup.count(port))
            {
                SWSS_LOG_DEBUG("Speed update for port %s skipped (no LLR config)", port.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            bool speedUpdated = false;
            for (auto &fieldValue : kfvFieldsValues(t))
            {
                if (fvField(fieldValue) == "speed")
                {
                    m_portInfoLookup[port].speed = fvValue(fieldValue);
                    speedUpdated = true;
                }
            }

            if (speedUpdated)
            {
                if (!generateLlrProfile(port))
                {
                    // Profile not computable (e.g. no INI entry for this speed/cable pair).
                    // If port had an active profile, disable LLR gracefully.
                    if (!m_portInfoLookup[port].llr_profile.empty())
                    {
                        disableAndDeleteProfile(port);
                    }
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                publishLlrPortToApplDb(port);
            }
            else
            {
                // Speed absent from SET — port has no operational speed.
                // Clear cached speed; disable LLR if it was active.
                SWSS_LOG_NOTICE("Port %s: speed absent from STATE_DB; clearing",
                                port.c_str());
                m_portInfoLookup[port].speed.clear();
                if (!m_portInfoLookup[port].llr_profile.empty())
                {
                    disableAndDeleteProfile(port);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_portInfoLookup.count(port))
            {
                // Clear speed; preserve LLR intent so port auto-recovers when speed returns.
                m_portInfoLookup[port].speed.clear();
                if (!m_portInfoLookup[port].llr_profile.empty())
                {
                    SWSS_LOG_NOTICE("Port %s: state entry deleted while LLR active; disabling",
                                    port.c_str());
                    disableAndDeleteProfile(port);
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

LlrMgr::LlrMgr(DBConnector *cfgDb, DBConnector *stateDb, DBConnector *applDb, const string &profileFile, vector<TableConnector> &table_connectors) :
    Orch(table_connectors),
    m_cfgLlrProfileTable(cfgDb, CFG_LLR_PROFILE_TABLE_NAME),
    m_cfgLlrPortTable(cfgDb, CFG_LLR_PORT_TABLE_NAME),
    m_cfgCableLenTable(cfgDb, CFG_PORT_CABLE_LEN_TABLE_NAME),
    m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
    m_appLlrProfileTable(applDb, APP_LLR_PROFILE_TABLE_NAME),
    m_appLlrPortTable(applDb, APP_LLR_PORT_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    readLlrProfileLookupFile(profileFile);
}

string LlrMgr::trim(const string &s) const
{
    return boost::algorithm::trim_copy(s);
}

string LlrMgr::resolveDbFieldName(const string &field) const
{
    auto it = kAliasToDbFieldNames.find(field);
    if (it != kAliasToDbFieldNames.end()) return it->second;
    return "";
}

bool LlrMgr::readLlrProfileLookupFile(const string &filepath)
{
    SWSS_LOG_ENTER();
    ifstream in(filepath);

    if (!in.is_open())
    {
        SWSS_LOG_NOTICE("Failed to open LLR lookup file %s", filepath.c_str());
        return false;
    }

    vector<string> header;
    bool headerParsed = false;
    size_t lineNo = 0;
    size_t inserted = 0;

    string line;
    while (getline(in, line))
    {
        ++lineNo;
        string raw = trim(line);
        if (raw.empty()) continue;
        if (raw[0] == '#') continue; // skip comment lines anywhere in the file

        if (!headerParsed)
        {
            // First non-empty, non-comment line is header
            istringstream iss(raw);
            string tok;
            while (iss >> tok) header.push_back(tok);

            if (header.size() < 4)
            {
                SWSS_LOG_WARN("Line %zu: header must have at least 4 tokens (speed cable outstanding_frames outstanding_bytes)", lineNo);
                header.clear();
                continue;
            }

            bool okFirst = (header[0] == "speed");
            bool okSecond = (header[1] == "cable");
            if (!okFirst || !okSecond)
            {
                SWSS_LOG_WARN("Line %zu: first two header tokens must be ('speed cable')", lineNo);
                header.clear();
                continue;
            }

            // Map INI aliases to DB field names (from index 2 onwards)
            for (size_t i = 2; i < header.size(); ++i)
            {
                string dbField = resolveDbFieldName(header[i]);
                if (dbField.empty())
                {
                    SWSS_LOG_WARN("Line %zu: unknown header field '%s' ignored", lineNo, header[i].c_str());
                    header[i] = ""; // mark ignore
                }
                else
                {
                    header[i] = dbField;
                }
            }

            // Verify mandatory columns (outstanding_frames, outstanding_bytes) are present.
            bool hasFramesCol = false, hasBytesCol = false;
            for (size_t i = 2; i < header.size(); ++i)
            {
                if (header[i] == "max_outstanding_frames") hasFramesCol = true;
                if (header[i] == "max_outstanding_bytes")  hasBytesCol  = true;
            }
            if (!hasFramesCol || !hasBytesCol)
            {
                SWSS_LOG_ERROR("Line %zu: header missing mandatory columns (outstanding_frames/outstanding_bytes)", lineNo);
                header.clear();
                continue;
            }

            headerParsed = true;
            SWSS_LOG_NOTICE("Parsed LLR header with %zu columns", header.size());
            continue; // Now move to next line for data
        }

        // Data rows are expected immediately after the header.
        // Tokenize data row
        istringstream iss(raw);
        vector<string> tokens;
        string t;
        while (iss >> t) tokens.push_back(t);
        if (tokens.size() < 2)
        {
            SWSS_LOG_WARN("Line %zu: insufficient columns (need at least 2)", lineNo);
            continue;
        }
        string speed = tokens[0];
        string cable = tokens[1];

        if (m_profileLookup.count({speed, cable}))
        {
            SWSS_LOG_WARN("Line %zu: duplicate (speed, cable) %s,%s ignored", lineNo, speed.c_str(), cable.c_str());
            continue;
        }

        LlrProfileEntry entry;
        entry.speed = speed;
        entry.cable = cable;

        // map remaining tokens to header fields (skip if value '-' or header[i] empty)
        size_t limit = min(tokens.size(), header.size());
        for (size_t i = 2; i < limit; ++i)
        {
            if (header[i].empty()) continue;
            const string &val = tokens[i];

            // '-' indicates profile field is unsupported on the platform
            if (val == "-") continue;
            entry.params[header[i]] = val;
        }

        // Validate mandatory fields are present and not '-' (dash).
        if (entry.params.find("max_outstanding_frames") == entry.params.end() ||
            entry.params.find("max_outstanding_bytes") == entry.params.end())
        {
            SWSS_LOG_WARN("Line %zu: missing mandatory fields (outstanding_frames/outstanding_bytes) for speed=%s cable=%s; skipping",
                          lineNo, speed.c_str(), cable.c_str());
            continue;
        }

        m_profileLookup[{speed, cable}] = entry;
        ++inserted;
    }

    SWSS_LOG_INFO("llr profile lookup parsing complete: %zu entries", inserted);
    return inserted > 0;
}

const LlrProfileEntry* LlrMgr::getProfileEntry(const string &speed, const string &cable) const
{
    auto it = m_profileLookup.find({speed, cable});
    if (it == m_profileLookup.end()) return nullptr;
    return &it->second;
}

bool LlrMgr::generateLlrProfile(const string &port)
{
    SWSS_LOG_ENTER();

    auto portIt = m_portInfoLookup.find(port);
    if (portIt == m_portInfoLookup.end())
    {
        SWSS_LOG_DEBUG("generateLlrProfile: no cache entry for port %s", port.c_str());
        return false;
    }

    PortInfo &portInfo = portIt->second;

    // Only generate/retain profiles when both local and remote are active.
    // If not active, release any previously bound profile and skip profile generation.
    if (!portInfo.hasLlrActive())
    {
        if (!portInfo.llr_profile.empty())
        {
            deleteProfile(port);
        }
        return false;
    }

    // Determine the new effective profile name and its attribute fields.
    string newProfileName;
    vector<FieldValueTuple> newProfileFields;

    if (!portInfo.user_profile.empty())
    {
        vector<FieldValueTuple> cfgFields;
        if (m_cfgLlrProfileTable.get(portInfo.user_profile, cfgFields) && !cfgFields.empty())
        {
            bool hasFrames = false, hasBytes = false;
            for (const auto &fv : cfgFields)
            {
                if (fvField(fv) == "max_outstanding_frames") hasFrames = true;
                if (fvField(fv) == "max_outstanding_bytes")  hasBytes  = true;
            }

            if (hasFrames && hasBytes)
            {
                newProfileName  = portInfo.user_profile;
                newProfileFields = cfgFields;
            }
            else
            {
                SWSS_LOG_WARN("Port %s: user_profile '%s' missing mandatory fields; falling back to INI",
                              port.c_str(), portInfo.user_profile.c_str());
            }
        }
        else
        {
            SWSS_LOG_WARN("Port %s: user_profile '%s' not found in CONFIG_DB; falling back to INI",
                          port.c_str(), portInfo.user_profile.c_str());
        }
    }

    if (newProfileName.empty())
    {
        // INI-derived profile.
        if (!portInfo.hasRequiredFields())
        {
            SWSS_LOG_DEBUG("Port %s: speed or cable_length not yet available; deferring profile generation",
                           port.c_str());
            return false;
        }

        const string &speed  = portInfo.speed;
        const string &cable  = portInfo.cable_length;

        if (cable == "0m")
        {
            SWSS_LOG_INFO("Port %s: cable length is 0m; skipping profile generation", port.c_str());
            return true;
        }

        const LlrProfileEntry *entry = getProfileEntry(speed, cable);
        if (!entry)
        {
            SWSS_LOG_ERROR("Port %s: no INI profile entry for speed=%s cable=%s",
                           port.c_str(), speed.c_str(), cable.c_str());
            return false;
        }

        newProfileName = "llr_" + speed + "_" + cable + "_profile";
        for (const auto &kv : entry->params)
        {
            newProfileFields.emplace_back(kv.first, kv.second);
        }
    }

    // No change needed.
    if (newProfileName == portInfo.llr_profile)
    {
        return true;
    }

    // Apply new profile change.
    deleteProfile(port);
    createProfile(newProfileName, newProfileFields, port);
    portInfo.llr_profile = newProfileName;

    SWSS_LOG_NOTICE("Port %s: effective profile set to '%s'", port.c_str(), newProfileName.c_str());
    return true;
}

void LlrMgr::deleteProfile(const string &port)
{
    auto portIt = m_portInfoLookup.find(port);
    if (portIt == m_portInfoLookup.end()) return;

    const string &oldName = portIt->second.llr_profile;
    if (oldName.empty()) return;

    // Delete Profile only if reference count is zero after erasing this port's reference.
    auto refIt = m_profileRefCount.find(oldName);
    if (refIt != m_profileRefCount.end())
    {
        refIt->second.erase(port);
        if (refIt->second.empty())
        {
            m_appLlrProfileTable.del(oldName);
            m_profileRefCount.erase(refIt);
            SWSS_LOG_NOTICE("LLR profile '%s' removed from APPL_DB", oldName.c_str());
        }
    }

    portIt->second.llr_profile.clear();
}

void LlrMgr::createProfile(const string &profileName, const vector<FieldValueTuple> &fields, const string &port)
{
    auto &refSet = m_profileRefCount[profileName];
    refSet.insert(port);
    if (refSet.size() == 1)
    {
        // First reference: publish to APPL_DB.
        m_appLlrProfileTable.set(profileName, fields);
        SWSS_LOG_NOTICE("LLR profile '%s' published to APPL_DB", profileName.c_str());
    }
}

void LlrMgr::publishLlrPortToApplDb(const string &port)
{
    auto portIt = m_portInfoLookup.find(port);
    if (portIt == m_portInfoLookup.end()) return;

    const PortInfo &portInfo = portIt->second;

    // Publish only when both LLR directions are enabled and a profile is bound.
    // If LLR was previously active and one direction is now disabled,
    // remove the APPL_DB entry so llrorch disables SAI.
    if (!portInfo.hasLlrActive())
    {
        m_appLlrPortTable.del(port);
        SWSS_LOG_INFO("Port %s: LLR not fully active (local=%s remote=%s); removed from APPL_DB",
                      port.c_str(), portInfo.llr_local.c_str(), portInfo.llr_remote.c_str());
        return;
    }

    if (portInfo.llr_profile.empty())
    {
        SWSS_LOG_WARN("Port %s: LLR profile not yet bound; delaying publish until profile is resolved",
                      port.c_str());
        return;
    }

    vector<FieldValueTuple> portFields = {
        {"llr_mode",    portInfo.llr_mode},
        {"llr_local",   portInfo.llr_local},
        {"llr_remote",  portInfo.llr_remote},
        {"llr_profile", portInfo.llr_profile},
    };
    m_appLlrPortTable.set(port, portFields);
    SWSS_LOG_INFO("Port %s published to APPL_DB LLR_PORT_TABLE (mode=%s local=%s remote=%s profile=%s)",
                  port.c_str(),
                  portInfo.llr_mode.c_str(),
                  portInfo.llr_local.c_str(),
                  portInfo.llr_remote.c_str(),
                  portInfo.llr_profile.c_str());
}

void LlrMgr::disableAndDeleteProfile(const string &port)
{
    SWSS_LOG_ENTER();

    auto portIt = m_portInfoLookup.find(port);
    if (portIt == m_portInfoLookup.end()) return;

    // Delete LLR_PORT first before the profile is removed.
    // portInfo llr_local/remote intent is preserved so
    // the port auto-recovers (profile re-created, APPL_DB re-published) when the
    // missing cable_length or speed is restored.
    m_appLlrPortTable.del(port);
    deleteProfile(port);

    SWSS_LOG_NOTICE("Port %s: LLR disabled and profile deleted from APPL_DB", port.c_str());
}

void LlrMgr::getCableLengthCfgDb(const string &port)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    m_cfgCableLenTable.getKeys(keys);

    for (const auto &key : keys)
    {
        string cableLen;
        if (m_cfgCableLenTable.hget(key, port, cableLen))
        {
            m_portInfoLookup[port].cable_length = cableLen;
            SWSS_LOG_DEBUG("Port %s: fetched cable length from CFG_DB: %s",
                          port.c_str(), cableLen.c_str());
            return;
        }
    }

    SWSS_LOG_DEBUG("Port %s: no cable length found in CFG_DB CABLE_LENGTH table", port.c_str());
}

void LlrMgr::getSpeedStateDb(const string &port)
{
    SWSS_LOG_ENTER();

    string speed;
    if (m_statePortTable.hget(port, "speed", speed))
    {
        m_portInfoLookup[port].speed = speed;
        SWSS_LOG_DEBUG("Port %s: fetched speed from STATE_DB: %s",
                      port.c_str(), speed.c_str());
    }
    else
    {
        SWSS_LOG_DEBUG("Port %s: no speed found in STATE_DB PORT_TABLE", port.c_str());
    }
}