#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include <teamdctl.h>

using TeamdCtlDump = std::pair<bool, std::string>;
using TeamdCtlDumpsEntry = std::pair<std::string, std::string>;
using TeamdCtlDumps = std::vector<TeamdCtlDumpsEntry>;

class TeamdCtlMgr
{
public:
    TeamdCtlMgr() = default;
    ~TeamdCtlMgr();
    bool add_lag(const std::string & lag_name);
    bool remove_lag(const std::string & lag_name);
    void process_add_queue();
    // Retry logic added to prevent incorrect error reporting in dump API's
    TeamdCtlDump get_dump(const std::string & lag_name, bool to_retry);
    TeamdCtlDumps get_dumps(bool to_retry);
    int sendIpcToTeamd(const std::string& command,
                      const std::vector<std::string>& args,
                      std::string& response_out);
    bool m_teamdUnifiedProcMode = false;


private:
    bool has_key(const std::string & lag_name) const;
    bool try_add_lag(const std::string & lag_name);

    std::unordered_map<std::string, struct teamdctl*> m_handlers;
    std::unordered_map<std::string, int> m_lags_to_add;
    std::unordered_map<std::string, int> m_lags_err_retry;
    int sockfd;

    const int max_attempts_to_add = 10;
};


#define TEAMD_MULTI_SOCK_PATH "/var/run/teamd/teamd-unified.sock"
#define TEAMD_IPC_REQ "REQUEST"
