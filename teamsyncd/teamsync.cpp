#include <string.h>
#include <errno.h>
#include <system_error>
#include <sys/socket.h>
#include <linux/if.h>
#include <netlink/route/link.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "warm_restart.h"
#include "teamsync.h"

using namespace std;
using namespace swss;

/* Taken from drivers/net/team/team.c */
#define TEAM_DRV_NAME "team"

TeamSync::TeamSync(DBConnector *db, DBConnector *stateDb, Select *select) :
    m_select(select),
    m_lagTable(db, APP_LAG_TABLE_NAME),
    m_lagMemberTable(db, APP_LAG_MEMBER_TABLE_NAME),
    m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
    m_warmstart(WarmStart::isWarmStart())
{
    if (m_warmstart)
    {
        m_lagTable.create_temp_view();
        m_lagMemberTable.create_temp_view();
        SWSS_LOG_NOTICE("Starting in warmstart mode");
    }
}

void TeamSync::doSelectableTask()
{
    /* Start to track the new team instances */
    for (auto s : m_selectablesToAdd)
    {
        m_select->addSelectable(m_teamSelectables[s].get());
    }

    m_selectablesToAdd.clear();

    /* No longer track the deprecated team instances */
    for (auto s : m_selectablesToRemove)
    {
        m_select->removeSelectable(m_teamSelectables[s].get());
        m_teamSelectables.erase(s);
    }

    m_selectablesToRemove.clear();
}

void TeamSync::applyState()
{
    if (!m_warmstart)
    {
        return;
    }

    SWSS_LOG_NOTICE("Applying state");

    m_lagTable.apply_temp_view();
    m_lagMemberTable.apply_temp_view();

    for(auto &it: m_stateLagTablePreserved)
    {
        const auto &lagName  = it.first;
        const auto &fvVector = it.second;
        m_stateLagTable.set(lagName, fvVector);
    }
    m_stateLagTablePreserved.clear();

    m_warmstart = false;
}

void TeamSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_link *link = (struct rtnl_link *)obj;
    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_DELLINK))
        return;

    string lagName = rtnl_link_get_name(link);

    /* Listens to LAG messages */
    char *type = rtnl_link_get_type(link);
    if (!type || (strcmp(type, TEAM_DRV_NAME) != 0))
        return;

    if (nlmsg_type == RTM_DELLINK)
    {
        /* Remove LAG ports and delete LAG */
        removeLag(lagName);
        return;
    }

    addLag(lagName, rtnl_link_get_ifindex(link),
           rtnl_link_get_flags(link) & IFF_UP,
           rtnl_link_get_flags(link) & IFF_LOWER_UP);
}

void TeamSync::addLag(const string &lagName, int ifindex, bool admin_state,
                      bool oper_state)
{
    /* Set the LAG */
    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin_state ? "up" : "down");
    FieldValueTuple o("oper_status", oper_state ? "up" : "down");
    fvVector.push_back(a);
    fvVector.push_back(o);
    m_lagTable.set(lagName, fvVector);

    SWSS_LOG_INFO("Add %s admin_status:%s oper_status:%s",
                   lagName.c_str(), admin_state ? "up" : "down", oper_state ? "up" : "down");

    /* Return when the team instance has already been tracked */
    if (m_teamSelectables.find(lagName) != m_teamSelectables.end())
        return;

    fvVector.clear();
    FieldValueTuple s("state", "ok");
    fvVector.push_back(s);
    if(m_warmstart)
    {
        m_stateLagTablePreserved[lagName] = fvVector;
    }
    else
    {
        m_stateLagTable.set(lagName, fvVector);
    }

    /* Create the team instance */
    auto sync = make_shared<TeamPortSync>(lagName, ifindex, &m_lagMemberTable);
    m_teamSelectables[lagName] = sync;
    m_selectablesToAdd.insert(lagName);
}

void TeamSync::removeLag(const string &lagName)
{
    /* Delete all members */
    auto selectable = m_teamSelectables[lagName];
    for (auto it : selectable->m_lagMembers)
    {
        m_lagMemberTable.del(lagName + ":" + it.first);
    }

    /* Delete the LAG */
    m_lagTable.del(lagName);

    SWSS_LOG_INFO("Remove %s", lagName.c_str());

    /* Return when the team instance hasn't been tracked before */
    if (m_teamSelectables.find(lagName) == m_teamSelectables.end())
        return;

    if(m_warmstart)
    {
        m_stateLagTablePreserved.erase(lagName);
    }
    else
    {
        m_stateLagTable.del(lagName);
    }

    m_selectablesToRemove.insert(lagName);
}

const struct team_change_handler TeamSync::TeamPortSync::gPortChangeHandler = {
    .func       = TeamSync::TeamPortSync::teamdHandler,
    .type_mask  = TEAM_PORT_CHANGE | TEAM_OPTION_CHANGE
};

TeamSync::TeamPortSync::TeamPortSync(const string &lagName, int ifindex,
                                     ProducerStateTable *lagMemberTable) :
    m_lagMemberTable(lagMemberTable),
    m_lagName(lagName),
    m_ifindex(ifindex)
{
    m_team = team_alloc();
    if (!m_team)
    {
        SWSS_LOG_ERROR("Unable to allocated team socket");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to allocated team socket");
    }

    int err = team_init(m_team, ifindex);
    if (err)
    {
        team_free(m_team);
        m_team = NULL;
        SWSS_LOG_ERROR("Unable to init team socket");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to init team socket");
    }

    err = team_change_handler_register(m_team, &gPortChangeHandler, this);
    if (err)
    {
        team_free(m_team);
        m_team = NULL;
        SWSS_LOG_ERROR("Unable to register port change event");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to register port change event");
    }

    /* Sync LAG at first */
    onChange();
}

TeamSync::TeamPortSync::~TeamPortSync()
{
    if (m_team)
    {
        team_change_handler_unregister(m_team, &gPortChangeHandler, this);
        team_free(m_team);
    }
}

int TeamSync::TeamPortSync::onChange()
{
    struct team_port *port;
    map<string, bool> tmp_lag_members;

    /* Check each port  */
    team_for_each_port(port, m_team)
    {
        uint32_t ifindex;
        char ifname[MAX_IFNAME + 1] = {0};
        bool enabled;

        ifindex = team_get_port_ifindex(port);

        /* Skip if interface is not found */
        if (!team_ifindex2ifname(m_team, ifindex, ifname, MAX_IFNAME))
        {
            SWSS_LOG_INFO("Interface ifindex(%u) is not found", ifindex);
            continue;
        }

        /* Skip the member that is removed from the LAG */
        if (team_is_port_removed(port))
        {
            continue;
        }

        team_get_port_enabled(m_team, ifindex, &enabled);
        tmp_lag_members[string(ifname)] = enabled;
    }

    /* Compare old and new LAG members and set/del accordingly */
    for (auto it : tmp_lag_members)
    {
        if (m_lagMembers.find(it.first) == m_lagMembers.end() || it.second != m_lagMembers[it.first])
        {
            string key = m_lagName + ":" + it.first;
            vector<FieldValueTuple> v;
            FieldValueTuple l("status", it.second ? "enabled" : "disabled");
            v.push_back(l);
            m_lagMemberTable->set(key, v);
        }
    }

    for (auto it : m_lagMembers)
    {
        if (tmp_lag_members.find(it.first) == tmp_lag_members.end())
        {
            string key = m_lagName + ":" + it.first;
            m_lagMemberTable->del(key);
        }
    }

    /* Replace the old LAG members with the new ones */
    m_lagMembers = tmp_lag_members;
    return 0;
}

int TeamSync::TeamPortSync::teamdHandler(struct team_handle *team, void *arg,
                                         team_change_type_mask_t type_mask)
{
    return ((TeamSync::TeamPortSync *)arg)->onChange();
}

int TeamSync::TeamPortSync::getFd()
{
    return team_get_event_fd(m_team);
}

void TeamSync::TeamPortSync::readData()
{
    team_handle_events(m_team);
}
