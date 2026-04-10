#pragma once

#include "orch.h"
#include "portsorch.h"
#include "switchorch.h"
#include "nexthopgroupkey.h"

#include <unordered_map>
#include <string>
#include <set>

struct ArsProfileEntry
{
    sai_object_id_t profileOid = SAI_NULL_OBJECT_ID;
    sai_ars_profile_algo_t algorithm = SAI_ARS_PROFILE_ALGO_EWMA;
    bool            loadPastEnable   = true;
    uint32_t        loadPastWeight   = 16;
    bool            loadFutureEnable = true;
    uint32_t        loadFutureWeight = 16;
    bool            loadCurrentEnable = false;
    uint32_t        loadCurrentWeight = 0;
    uint32_t        loadExponent      = 2;
    uint32_t        maxFlows          = 0;
    uint32_t        loadPastMinVal    = 0;
    uint32_t        loadPastMaxVal    = 0;
    uint32_t        loadFutureMinVal  = 0;
    uint32_t        loadFutureMaxVal  = 0;
    uint32_t        loadCurrentMinVal = 0;
    uint32_t        loadCurrentMaxVal = 0;
    bool            ipv4Enable        = true;
    bool            ipv6Enable        = true;
    uint32_t        samplingInterval  = 0;
    uint32_t        randomSeed        = 0;
    uint32_t        quantizationType  = 0;
    uint32_t        profileLinkUtilThreshold = 0;
    uint32_t        profileIdleTime   = 0;
};

struct ArsObjectEntry
{
    sai_object_id_t arsOid   = SAI_NULL_OBJECT_ID;
    sai_ars_mode_t  mode     = SAI_ARS_MODE_FLOWLET_QUALITY;
    uint32_t        idleTime = 256;
    uint32_t        maxFlows = 512;
    bool            enabled  = false;
    std::string     profileName;
    std::string     portProfileName;
};

struct ArsInterfaceEntry
{
    bool        enabled   = false;
    std::string arsObject;
    std::string portProfile;
    uint32_t    linkUtilThreshold = 0;
    uint32_t    weight = 1;
};

struct ArsPortProfileEntry
{
    uint32_t loadPastMinVal    = 0;
    uint32_t loadPastMaxVal    = 0;
    uint32_t loadFutureMinVal  = 0;
    uint32_t loadFutureMaxVal  = 0;
    uint32_t loadCurrentMinVal = 0;
    uint32_t loadCurrentMaxVal = 0;
    bool     enabled           = false;
    uint32_t portLoadPastWeight   = 0;
    uint32_t portLoadFutureWeight = 0;
    uint32_t loadScalingFactor    = 0;
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
    bool bindArsToNhg(sai_object_id_t nhgOid, sai_object_id_t arsOid);
    bool unbindArsFromNhg(sai_object_id_t nhgOid);
    sai_object_id_t resolveArsForNhg(sai_object_id_t nhgOid, const NextHopGroupKey &nhgKey);
    std::string getArsObjectForPort(const std::string &portName) const;

private:
    void doTask(Consumer &consumer) override;

    void doArsGlobalTask(Consumer &consumer);
    void doArsProfileTask(Consumer &consumer);
    void doArsObjectTask(Consumer &consumer);
    void doArsInterfaceTask(Consumer &consumer);
    void doArsPortProfileTask(Consumer &consumer);
    void doArsNexthopsTask(Consumer &consumer);

    bool createArsProfile(const std::string &name, const ArsProfileEntry &entry);
    bool removeArsProfile(const std::string &name);
    bool updateArsProfileAttr(sai_object_id_t oid, sai_ars_profile_attr_t attrId, uint32_t val);
    bool updateArsProfileAttrBool(sai_object_id_t oid, sai_ars_profile_attr_t attrId, bool val);

    bool createArsObject(const std::string &name, const ArsObjectEntry &entry);
    bool removeArsObject(const std::string &name);
    bool setArsObjectAttr(sai_object_id_t oid, sai_ars_attr_t attrId, uint32_t val);

    bool bindArsProfileToSwitch(sai_object_id_t profileOid);

    bool setPortArsEnable(const std::string &portName, bool enable);
    bool setPortArsScalingFactor(const std::string &portName, uint32_t factor);
    bool setPortArsWeights(const std::string &portName, uint32_t pastWeight, uint32_t futureWeight);
    bool setPortArsLoadBands(const std::string &portName, const ArsPortProfileEntry &pp);
    void applyPortProfileToInterface(const std::string &portName, const std::string &profileName);

    void doArsPortChannelTask(Consumer &consumer);

    void publishArsCaps();

    sai_ars_mode_t parseArsMode(const std::string &modeStr) const;

    SwitchOrch *m_switchOrch;
    PortsOrch  *m_portsOrch;
    swss::Table m_stateArsCapTable;
    swss::Table m_cfgArsTable;

    bool m_arsEnabled = false;
    std::string m_globalProfileName;
    sai_object_id_t m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;

    std::unordered_map<std::string, ArsProfileEntry>    m_arsProfiles;
    std::unordered_map<std::string, ArsObjectEntry>     m_arsObjects;
    std::unordered_map<std::string, ArsInterfaceEntry>  m_arsInterfaces;
    std::unordered_map<std::string, ArsPortProfileEntry> m_arsPortProfiles;
    std::set<std::string> m_arsEnabledPorts;

    std::unordered_map<std::string, std::string> m_nexthopArsBindings;
};
