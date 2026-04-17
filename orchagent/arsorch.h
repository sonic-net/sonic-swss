#pragma once

#include "orch.h"
#include "observer.h"
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
    std::string     defaultArsObject;
};

struct ArsObjectEntry
{
    sai_object_id_t arsOid   = SAI_NULL_OBJECT_ID;
    sai_ars_mode_t  mode     = SAI_ARS_MODE_FLOWLET_QUALITY;
    uint32_t        idleTime = 256;
    uint32_t        maxFlows = 512;
    // admin_state on an ARS_OBJECT gates whether NHGs / LAGs that reference
    // it actually carry the SAI ARS binding. Defaults to true so existing
    // configs that omit the field (YANG default "down" notwithstanding)
    // continue to bind — operators who explicitly write admin_state=down
    // now cause unbinding, which matches intent.
    bool            enabled  = true;
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
    double   loadScalingFactorRaw = 0.0;
    bool     loadScalingFactorAuto = false;
};

class ArsOrch : public Orch, public Observer
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
    std::string getArsObjectForPrefix(const std::string &prefix) const;
    void writeArsNhgState(const std::string &nhgName, bool degraded, const std::string &reason = "");
    void removeArsNhgState(const std::string &nhgName);

    void update(SubjectType type, void *cntx) override;

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
    bool bindArsToLag(const std::string &lagName, sai_object_id_t arsOid);
    bool unbindArsFromLag(const std::string &lagName);

    bool setPortArsEnable(const std::string &portName, bool enable);

    // Wholesale enable/disable of the ARS data-plane state. Called from the
    // global ARS|GLOBAL admin_state transitions so that a toggle to "down"
    // actually removes ARS from the data plane (rather than just flipping
    // m_arsEnabled), and the opposite toggle to "up" rebuilds it from the
    // cached CONFIG_DB view in m_arsInterfaces / m_arsObjects.
    void disableArsDataPlane();
    void enableArsDataPlane();
    bool setPortArsScalingFactor(const std::string &portName, const ArsPortProfileEntry &pp);
    bool setPortArsLinkUtilThreshold(const std::string &portName, uint32_t threshold);
    bool setPortArsWeights(const std::string &portName, uint32_t pastWeight, uint32_t futureWeight);
    bool setPortArsLoadBands(const std::string &portName, const ArsPortProfileEntry &pp);
    void applyPortProfileToInterface(const std::string &portName, const std::string &profileName);
    void createDefaultProfileIfNeeded();

    void doArsPortChannelTask(Consumer &consumer);

    void publishArsCaps();
    void publishArsProfileState(const std::string &profileName, const ArsProfileEntry &entry);

    // Parse an assign_mode CLI/CONFIG_DB string. Returns true on success and
    // writes the SAI mode into *out; returns false (and does NOT modify *out)
    // for any string not in arsModeLookup so the caller can skip the update
    // rather than silently coerce to a default.
    bool parseArsMode(const std::string &modeStr, sai_ars_mode_t *out) const;

    SwitchOrch *m_switchOrch;
    PortsOrch  *m_portsOrch;
    swss::Table m_stateArsCapTable;
    swss::Table m_stateArsProfileTable;
    swss::Table m_stateArsNhgTable;
    swss::Table m_cfgArsTable;

    bool m_arsEnabled = false;
    std::string m_globalProfileName;
    sai_object_id_t m_activeSwitchProfileOid = SAI_NULL_OBJECT_ID;

    std::unordered_map<std::string, ArsProfileEntry>    m_arsProfiles;
    std::unordered_map<std::string, ArsObjectEntry>     m_arsObjects;
    // m_arsInterfaces holds physical port entries (ARS_INTERFACES).
    // m_arsLags holds PortChannel entries (ARS_PORTCHANNELS).
    // Previously both shared m_arsInterfaces and the only thing keeping the
    // deferred-LAG-bind loop correct was the Ethernet/PortChannel naming
    // convention — a LAG named 'Ethernet…' (unusual but allowed) would have
    // been bound with the wrong SAI path. Separate maps remove that hazard.
    std::unordered_map<std::string, ArsInterfaceEntry>  m_arsInterfaces;
    std::unordered_map<std::string, ArsInterfaceEntry>  m_arsLags;
    std::unordered_map<std::string, ArsPortProfileEntry> m_arsPortProfiles;
    std::set<std::string> m_arsEnabledPorts;
    std::set<std::string> m_arsEnabledLags;

    std::unordered_map<std::string, std::string> m_nexthopArsBindings;
};
