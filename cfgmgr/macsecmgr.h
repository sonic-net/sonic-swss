#ifndef __MACSECMGR__
#define __MACSECMGR__

#include <orch.h>
#include <swss/schema.h>
#include <swss/boolean.h>

#include <cinttypes>
#include <map>
#include <vector>
#include <sstream>

#include <sys/types.h>

namespace swss {

class MACsecMgr : public Orch
{
public:
    using Orch::doTask;
    MACsecMgr(DBConnector *cfgDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    ~MACsecMgr();
private:
    void doTask(Consumer &consumer);

public:
    using TaskArgs = std::vector<FieldValueTuple>;
    struct MACsecProfile
    {
        std::uint32_t       priority;
        enum CipherSuite
        {
            GCM_AES_128,
            GCM_AES_256,
            GCM_AES_XPN_128,
            GCM_AES_XPN_256,
        }                   cipher_suite;
        std::string         primary_cak;
        std::string         primary_ckn;
        std::string         fallback_cak;
        std::string         fallback_ckn;
        enum Policy
        {
            INTEGRITY_ONLY,
            SECURITY,
        }                   policy;
        swss::AlphaBoolean  enable_replay_protect;
        std::uint32_t       replay_window;
        swss::AlphaBoolean  send_sci;
        std::uint32_t       rekey_period;
        bool update(const TaskArgs & ta);
    };

    struct MKASession
    {
        std::string profile_name;
        // wpa_supplicant communication socket
        std::string sock;
        // wpa_supplicant process id
        pid_t       wpa_supplicant_pid;
        // Key material currently applied to wpa_supplicant, in the encoded form
        // held in CONFIG_DB. A hot update diffs against this rather than against
        // the previous profile, so a partially applied update is retried against
        // what the port actually has. The fallback pair is empty when the port
        // has no fallback CA.
        std::string primary_cak;
        std::string primary_ckn;
        std::string fallback_cak;
        std::string fallback_ckn;
    };

private:
    std::map<std::string, struct MACsecProfile> m_profiles;
    std::map<std::string, MKASession>           m_macsec_ports;

    task_process_status removeProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    task_process_status loadProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    task_process_status enableMACsec(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status disableMACsec(const std::string & port_name, const TaskArgs & port_attr);


    Table m_statePortTable;

    bool isPortStateOk(const std::string & port_name);
    pid_t startWPASupplicant(const std::string & sock) const;
    bool stopWPASupplicant(pid_t pid) const;
    bool configureMACsec(const std::string & port_name, MKASession & session, const MACsecProfile & profile) const;
    bool unconfigureMACsec(const std::string & port_name, const MKASession & session) const;

    // One MKA participant reported by macsec_mka_list.
    struct MKAParticipant
    {
        std::string ckn;
        bool        fallback = false;
    };

    static const MKAParticipant * findParticipant(
        const std::vector<MKAParticipant> & participants,
        const std::string & ckn);

    // Runtime MKA participant management over the per-port wpa_supplicant ctrl
    // socket, wrapping macsec_add_mka / macsec_del_mka / macsec_mka_list.

    // Add an MKA participant. 'fallback' marks it as a standby CA. Idempotent,
    // and re-adds the CKN when it is present holding the other role.
    bool addMKA(
        const std::string & sock,
        const std::string & port_name,
        const std::string & ckn,
        const std::string & cak,
        bool fallback) const;
    // Remove an MKA participant. Absent CKN is a no-op success.
    bool delMKA(
        const std::string & sock,
        const std::string & port_name,
        const std::string & ckn) const;
    std::vector<MKAParticipant> getMKAParticipants(
        const std::string & sock,
        const std::string & port_name) const;
    // Apply 'profile' to a live port with runtime commands instead of restarting
    // the MKA session, recording what was applied on 'session'.
    bool hotUpdateProfile(
        const std::string & port_name,
        MKASession & session,
        const MACsecProfile & profile) const;
};

}

#endif
