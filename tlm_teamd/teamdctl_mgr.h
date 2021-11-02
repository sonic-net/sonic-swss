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
    TeamdCtlDump get_dump(const std::string & lag_name);
    TeamdCtlDumps get_dumps();

private:
    bool has_key(const std::string & lag_name) const;
    bool try_add_lag(const std::string & lag_name);

    std::unordered_map<std::string, struct teamdctl*> m_handlers;
    std::unordered_map<std::string, int> m_lags_to_add;

    const int max_attempts_to_add = 10;
};
