#ifndef __MACSECMGR__
#define __MACSECMGR__

// The following definitions should be moved to schema.h

#define CFG_MACSEC_PROFILE_TABLE_NAME           "MACSEC_PROFILE"

// End define

#include <cinttypes>
#include <map>
#include <vector>
#include <sstream>

#include <sys/types.h>

#include <orch.h>

#undef SWSS_LOG_ERROR
#undef SWSS_LOG_WARN
#undef SWSS_LOG_NOTICE
#undef SWSS_LOG_INFO
#undef SWSS_LOG_DEBUG
#undef SWSS_LOG_ENTER

#define SWSS_LOG_ERROR(MSG, ...)       printf("ERROR :- %s: " MSG"\n", __FUNCTION__, ##__VA_ARGS__)
#define SWSS_LOG_WARN(MSG, ...)        printf("WARN :- %s: " MSG"\n", __FUNCTION__, ##__VA_ARGS__)
#define SWSS_LOG_NOTICE(MSG, ...)      printf("NOTICE :- %s: " MSG"\n", __FUNCTION__, ##__VA_ARGS__)
#define SWSS_LOG_INFO(MSG, ...)        printf("INFO :- %s: " MSG"\n", __FUNCTION__, ##__VA_ARGS__)
#define SWSS_LOG_DEBUG(MSG, ...)       printf("DEBUG :- %s: " MSG"\n", __FUNCTION__, ##__VA_ARGS__)
#define SWSS_LOG_ENTER()               printf("ENTER %s : %d\n", __FUNCTION__, __LINE__)

namespace swss {

class MACsecMgr : public Orch
{
public:
    using Orch::doTask;
    MACsecMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);

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
            BYPASS,
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

    enum TaskResult
    {
        FINISHED,
        UNFINISHED,
        ERROR,
    };
    TaskResult removeProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    TaskResult loadProfile(const std::string & profile_name, const TaskArgs & profile_attr);
    TaskResult enableMACsec(const std::string & port_name, const TaskArgs & port_attr);
    TaskResult disableMACsec(const std::string & port_name, const TaskArgs & port_attr);


    Table m_statePortTable;

    bool isPortStateOk(const std::string & port_name);
    pid_t startWPASupplicant(const std::string & sock) const;
    bool stopWPASupplicant(pid_t pid) const;
    bool enableMACsec(const std::string & port_name, const MKASession & session, const MACsecProfile & profile) const;
    bool disableMACsec(const std::string & port_name, const MKASession & session) const;
};

}

#endif
