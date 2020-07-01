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
    bool add_lag(const std::string & kag_name);
    bool remove_lag(const std::string & kag_name);
    TeamdCtlDump get_dump(const std::string & lag_name);
    TeamdCtlDumps get_dumps();

private:
    bool has_key(const std::string & lag_name) const;

    std::unordered_map<std::string, struct teamdctl*> m_handlers;
};
