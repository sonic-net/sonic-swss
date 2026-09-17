#ifndef __MACSECMGR__
#define __MACSECMGR__

#include <orch.h>
#include <swss/schema.h>
#include <swss/boolean.h>

#include <cinttypes>
#include <map>
#include <set>
#include <vector>
#include <sstream>

#include <sys/types.h>

#include "macsecmkastatus.h"

#ifndef STATE_MACSEC_MKA_SESSION_TABLE_NAME
#define STATE_MACSEC_MKA_SESSION_TABLE_NAME "MACSEC_MKA_SESSION_TABLE"
#endif

#ifndef STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME
#define STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME "MACSEC_MKA_PARTICIPANT_TABLE"
#endif

namespace swss {

class MACsecMgr : public Orch
{
public:
    using Orch::doTask;
    MACsecMgr(DBConnector *cfgDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    ~MACsecMgr();
    void doTask();
private:
    void doTask(Consumer &consumer);

public:
    using TaskArgs = std::vector<FieldValueTuple>;
    struct MACsecProfile
    {
        std::uint32_t       priority = 255;
        enum CipherSuite
        {
            GCM_AES_128,
            GCM_AES_256,
            GCM_AES_XPN_128,
            GCM_AES_XPN_256,
        }                   cipher_suite = GCM_AES_128;
        std::string         primary_cak;
        std::string         primary_ckn;
        std::string         fallback_cak;
        std::string         fallback_ckn;
        enum Policy
        {
            INTEGRITY_ONLY,
            SECURITY,
        }                   policy = SECURITY;
        swss::AlphaBoolean  enable_replay_protect = false;
        std::uint32_t       replay_window = 0;
        swss::AlphaBoolean  send_sci = true;
        std::uint32_t       rekey_period = 0;
        bool update(const TaskArgs & ta);
    };

    struct MKASession
    {
        std::string profile_name;
        // wpa_supplicant communication socket
        std::string sock;
        // wpa_supplicant process id
        pid_t       wpa_supplicant_pid = 0;
        std::string network_id;
        MACsecProfile applied_profile;
        std::string pending_old_ckn;
        bool pending_primary = false;
        std::string config_error;
    };

private:
    std::map<std::string, struct MACsecProfile> m_profiles;
    std::map<std::string, MKASession>           m_macsec_ports;

    task_process_status removeProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    task_process_status loadProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    task_process_status enableMACsec(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status disableMACsec(const std::string & port_name, const TaskArgs & port_attr);


    Table m_statePortTable;
    Table m_cfgPortTable;
    Table m_stateMkaSessionTable;
    Table m_stateMkaParticipantTable;
    SelectableTimer *m_mkaStatusTimer = nullptr;
    bool m_startupStateReconciled = false;

    bool isPortStateOk(const std::string & port_name);
    pid_t startWPASupplicant(const std::string & sock) const;
    bool stopWPASupplicant(pid_t pid) const;
    bool configureMACsec(const std::string & port_name, MKASession & session, const MACsecProfile & profile) const;
    bool unconfigureMACsec(const std::string & port_name, const MKASession & session) const;

    bool collectMKAStatus(
        const std::string &port_name,
        MKASession &session,
        MKASessionStatus *status = nullptr,
        bool allowDesiredReplacement = false);
    bool queryMKAStatus(
        const std::string &port_name,
        const MKASession &session,
        MKASessionStatus &status,
        std::string &error) const;
    bool validateExpectedParticipants(
        const MKASession &session,
        const MKASessionStatus &status,
        const MACsecProfile *desired,
        bool allowDesiredReplacement,
        std::string &error) const;
    void publishMKAStatus(
        const std::string &port_name,
        MKASession &session,
        const MKASessionStatus &status);
    void markMKAQueryError(
        const std::string &port_name,
        MKASession &session,
        const std::string &reason);
    void setConfigState(
        const std::string &port_name,
        MKASession &session,
        const std::string &error);
    void deleteOperationalState(const std::string &port_name);
    void reconcileStartupState();
    void doTask(SelectableTimer &timer) override;
    void sweepMKAStatus();
    bool preflightRollover(
        const std::string &port_name,
        MKASession &session,
        const MACsecProfile &desired);
    bool reconcilePort(
        const std::string &port_name,
        MKASession &session,
        const MACsecProfile &desired);
    bool removeParticipant(
        const std::string &port_name,
        const MKASession &session,
        const std::string &ckn) const;
    bool updateNetworkParticipant(
        const std::string &port_name,
        const MKASession &session,
        const MACsecProfile &desired,
        bool primary) const;
    bool addParticipant(
        const std::string &port_name,
        const MKASession &session,
        const MACsecProfile &desired,
        bool primary) const;
};

}

#endif
