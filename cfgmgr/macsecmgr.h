#ifndef __MACSECMGR__
#define __MACSECMGR__

#include <orch.h>
#include <swss/schema.h>

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
    MACsecMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    ~MACsecMgr();
private:
    void doTask(Consumer &consumer);

public:
    using TaskArgs = std::vector<FieldValueTuple>;
    struct MACsecProfile
    {
        std::uint8_t  priority;
        std::string   cipher_suite;
        std::string   primary_cak;
        std::string   primary_ckn;
        std::string   fallback_cak;
        std::string   fallback_ckn;
        enum Policy
        {
            INTEGRITY_ONLY,
            SECURITY,
        }             policy;
        bool          enable_replay_protect;
        std::uint32_t replay_window;
        bool          send_sci;
        std::uint32_t rekey_period;
        bool update(const TaskArgs & ta);
    };

    struct MKASession
    {
        std::string profile_name;
        // wpa_supplicant communication socket
        std::string sock;
        // wpa_supplicant process id
        pid_t       wpa_supplicant_pid;
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
    bool configureMACsec(const std::string & port_name, const MKASession & session, const MACsecProfile & profile) const;
    bool unconfigureMACsec(const std::string & port_name, const MKASession & session) const;
};

}

#endif
