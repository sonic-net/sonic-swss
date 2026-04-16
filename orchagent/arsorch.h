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
    // Per-band congestion thresholds (in Mbps) that feed
    // SAI_ARS_PROFILE_ATTR_QUANT_BAND_{0,1,2}_MIN_THRESHOLD.
    // Required for Mellanox SAI to program the SDK congestion threshold via
    // sx_api_ar_congestion_threshold_set — the gating check in
    // are_ars_profile_thresholds_configured() only returns true when at least
    // one of the three band0/band1/band2 min thresholds is non-zero; otherwise
    // SAI falls back to the "hardened" profile and the quality signal cannot
    // tip EWMA-based flowlet reassignment regardless of load_*_max_val values.
    uint32_t        quantBand0MinThreshold = 0;
    uint32_t        quantBand1MinThreshold = 0;
    uint32_t        quantBand2MinThreshold = 0;
    std::string     defaultArsObject;
};

struct ArsObjectEntry
{
    sai_object_id_t arsOid   = SAI_NULL_OBJECT_ID;
    sai_ars_mode_t  mode     = SAI_ARS_MODE_FLOWLET_QUALITY;
    uint32_t        idleTime = 256;
    uint32_t        maxFlows = 512;
    bool            enabled  = false;
    bool            ipv4Enable = true;
    bool            ipv6Enable = true;
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
    bool setPortArsScalingFactor(const std::string &portName, const ArsPortProfileEntry &pp);
    bool setPortArsLinkUtilThreshold(const std::string &portName, uint32_t threshold);
    bool setPortArsWeights(const std::string &portName, uint32_t pastWeight, uint32_t futureWeight);
    bool setPortArsLoadBands(const std::string &portName, const ArsPortProfileEntry &pp);
    void applyPortProfileToInterface(const std::string &portName, const std::string &profileName);
    void createDefaultProfileIfNeeded();

    void doArsPortChannelTask(Consumer &consumer);

    void publishArsCaps();
    void publishArsProfileState(const std::string &profileName, const ArsProfileEntry &entry);

    sai_ars_mode_t parseArsMode(const std::string &modeStr) const;

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
    std::unordered_map<std::string, ArsInterfaceEntry>  m_arsInterfaces;
    std::unordered_map<std::string, ArsPortProfileEntry> m_arsPortProfiles;
    std::set<std::string> m_arsEnabledPorts;
    std::set<std::string> m_arsEnabledLags;

    std::unordered_map<std::string, std::string> m_nexthopArsBindings;
};
