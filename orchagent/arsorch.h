#pragma once

#include "orch.h"
#include "portsorch.h"
#include "switchorch.h"

#include <unordered_map>
#include <string>
#include <set>

struct ArsProfileEntry
{
    sai_object_id_t profileOid = SAI_NULL_OBJECT_ID;
    uint32_t        loadPastWeight   = 16;
    uint32_t        loadFutureWeight = 16;
    bool            loadCurrentEnable = false;
    uint32_t        loadExponent      = 2;
    uint32_t        maxFlows          = 0;
    uint32_t        loadPastMinVal    = 0;
    uint32_t        loadPastMaxVal    = 0;
    uint32_t        loadFutureMinVal  = 0;
    uint32_t        loadFutureMaxVal  = 0;
    uint32_t        loadCurrentMinVal = 0;
    uint32_t        loadCurrentMaxVal = 0;
};

struct ArsObjectEntry
{
    sai_object_id_t arsOid   = SAI_NULL_OBJECT_ID;
    sai_ars_mode_t  mode     = SAI_ARS_MODE_FLOWLET_QUALITY;
    uint32_t        idleTime = 256;
    uint32_t        maxFlows = 512;
    bool            enabled  = false;
    std::string     profileName;
};

class ArsOrch : public Orch
{
public:
    ArsOrch(swss::DBConnector *configDb,
            swss::DBConnector *stateDb,
            const std::vector<std::string> &tableNames,
            SwitchOrch *switchOrch,
            PortsOrch  *portsOrch);

    bool isArsEnabled() const { return m_arsEnabled; }
    sai_object_id_t getArsProfileOid(const std::string &name) const;
    sai_object_id_t getArsObjectOid(const std::string &name) const;

private:
    void doTask(Consumer &consumer) override;

    void doArsGlobalTask(Consumer &consumer);
    void doArsProfileTask(Consumer &consumer);
    void doArsObjectTask(Consumer &consumer);
    void doArsInterfaceTask(Consumer &consumer);

    bool createArsProfile(const std::string &name, const ArsProfileEntry &entry);
    bool removeArsProfile(const std::string &name);
    bool updateArsProfileAttr(sai_object_id_t oid, sai_ars_profile_attr_t attrId, uint32_t val);
    bool updateArsProfileAttrBool(sai_object_id_t oid, sai_ars_profile_attr_t attrId, bool val);

    bool createArsObject(const std::string &name, const ArsObjectEntry &entry);
    bool removeArsObject(const std::string &name);
    bool setArsObjectAttr(sai_object_id_t oid, sai_ars_attr_t attrId, uint32_t val);

    bool bindArsProfileToSwitch(sai_object_id_t profileOid);
    bool bindArsToNhg(sai_object_id_t nhgOid, sai_object_id_t arsOid);

    bool setPortArsEnable(const std::string &portName, bool enable);

    void publishArsCaps();

    sai_ars_mode_t parseArsMode(const std::string &modeStr) const;

    SwitchOrch *m_switchOrch;
    PortsOrch  *m_portsOrch;
    swss::Table m_stateArsCapTable;
    swss::Table m_cfgArsTable;

    bool m_arsEnabled = false;
    sai_object_id_t m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;

    std::unordered_map<std::string, ArsProfileEntry> m_arsProfiles;
    std::unordered_map<std::string, ArsObjectEntry>  m_arsObjects;
    std::set<std::string> m_arsEnabledPorts;
};
