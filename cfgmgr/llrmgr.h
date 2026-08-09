#ifndef __LLRMGR__
#define __LLRMGR__

#include "orch.h"
#include "producerstatetable.h"
#include <string>
#include <map>
#include <set>
#include <unordered_map>

using namespace std;

namespace swss {

struct LlrProfileEntry
{
    string speed;
    string cable;
    map<string, string> params;
};

struct PortInfo
{
    string speed;
    string cable_length;
    string llr_mode;        
    string llr_local;       
    string llr_remote;      
    string llr_profile;     // current effective profile name published to APPL_DB
    string user_profile;    // explicit user profile from CONFIG_DB

    PortInfo() : llr_mode("static"), llr_local("disabled"), llr_remote("disabled") {}

    bool hasLlrActive() const
    {
        return llr_local == "enabled" && llr_remote == "enabled";
    }

    bool hasRequiredFields() const
    {
        return !speed.empty() && !cable_length.empty();
    }
};

class LlrMgr: public Orch
{
public:
    LlrMgr(DBConnector *cfgDb, DBConnector *stateDb, DBConnector *applDb, const string &profileFile, vector<TableConnector> &table_connectors);
    using Orch::doTask;

    bool readLlrProfileLookupFile(const string &file);
    const LlrProfileEntry* getProfileEntry(const string &speed, const string &cable) const;

private:
    Table m_cfgLlrProfileTable;
    Table m_cfgLlrPortTable;
    Table m_cfgCableLenTable;
    Table m_statePortTable;
    ProducerStateTable m_appLlrProfileTable;
    ProducerStateTable m_appLlrPortTable;

    // INI-file lookup: (speed, cable) -> LlrProfileEntry
    map<pair<string, string>, LlrProfileEntry> m_profileLookup;

    // Port information cache: port -> PortInfo
    map<string, PortInfo> m_portInfoLookup;

    map<string, set<string>> m_profileRefCount;

    void doTask(Consumer &consumer);

    // INI field-name aliases -> DB field names
    static const unordered_map<string, string> kAliasToDbFieldNames;

    typedef void (LlrMgr::*HandlerFunc)(Consumer &consumer);
    static const unordered_map<string, HandlerFunc> kTableHandlers;

    // Handler functions for subscribed DB tables
    void handleLlrPortTable(Consumer &consumer);
    void handleLlrProfileTable(Consumer &consumer);
    void handlePortCableLenTable(Consumer &consumer);
    void handleStatePortTable(Consumer &consumer);

    // Compute effective profile for a port and update APPL_DB accordingly.
    bool generateLlrProfile(const string &port);

    void disableAndDeleteProfile(const string &port);

    // Decrement refcount for the port's current profile; delete APPL_DB entry when last ref gone.
    void deleteProfile(const string &port);

    // Increment refcount; write APPL_DB LLR_PROFILE_TABLE entry on first reference.
    void createProfile(const string &profileName, const vector<FieldValueTuple> &fields, const string &port);

    // Publish LLR_PORT_TABLE entry for the port to APPL_DB.
    void publishLlrPortToApplDb(const string &port);

    // Get CFG_DB CABLE_LENGTH table and populate cable_length to PortInfo.
    void getCableLengthCfgDb(const string &port);

    // Read STATE_DB PORT_TABLE and populate speed to PortInfo.
    void getSpeedStateDb(const string &port);

    string resolveDbFieldName(const string &field) const;
    string trim(const string &s) const;
};

} // namespace swss

#endif // __LLRMGR__