#include <string.h>
#include <errno.h>
#include <system_error>
#include <sys/socket.h>
#include <linux/if.h>
#include <netlink/route/link.h>
#include <chrono>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "warm_restart.h"
#include "teamsync.h"

#include <team.h>
#include <teamdctl.h>
#include <unistd.h>

using namespace std;
using namespace std::chrono;
using namespace swss;

/* Taken from drivers/net/team/team.c */
#define TEAM_DRV_NAME "team"

TeamSync::TeamSync(DBConnector *db, DBConnector *stateDb, Select *select) :
    m_select(select),
    m_lagTable(db, APP_LAG_TABLE_NAME),
    m_lagMemberTable(db, APP_LAG_MEMBER_TABLE_NAME),
    m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME)
{
    WarmStart::initialize(TEAMSYNCD_APP_NAME, "teamd");
    WarmStart::checkWarmStart(TEAMSYNCD_APP_NAME, "teamd");
    m_warmstart = WarmStart::isWarmStart();

    if (m_warmstart)
    {
        m_start_time = steady_clock::now();
        auto warmRestartIval = WarmStart::getWarmStartTimer(TEAMSYNCD_APP_NAME, "teamd");
        m_pending_timeout = warmRestartIval ? warmRestartIval : DEFAULT_WR_PENDING_TIMEOUT;
        m_lagTable.create_temp_view();
        m_lagMemberTable.create_temp_view();
        WarmStart::setWarmStartState(TEAMSYNCD_APP_NAME, WarmStart::INITIALIZED);
        SWSS_LOG_NOTICE("Starting in warmstart mode");
    }
}

void TeamSync::periodic()
{
    if (m_warmstart)
    {
        auto diff = duration_cast<seconds>(steady_clock::now() - m_start_time);
        if(diff.count() > m_pending_timeout)
        {
            applyState();
            m_warmstart = false; // apply state just once
            WarmStart::setWarmStartState(TEAMSYNCD_APP_NAME, WarmStart::RECONCILED);
        }
    }

    doSelectableTask();
}

void TeamSync::doSelectableTask()
{
    /* Retire the exact old objects, not whichever LAG now has the name. */
    for (const auto &sync : m_selectablesToRemove)
    {
        m_select->removeSelectable(sync.get());
    }

    m_selectablesToRemove.clear();

    /* Start to track the new team instances */
    for (const auto &sync : m_selectablesToAdd)
    {
        m_select->addSelectable(sync.get());
    }

    m_selectablesToAdd.clear();
}

void TeamSync::applyState()
{
    SWSS_LOG_NOTICE("Applying state");

    m_lagTable.apply_temp_view();
    m_lagMemberTable.apply_temp_view();
}

void TeamSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_link *link = (struct rtnl_link *)obj;
    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_DELLINK))
        return;

    LagId lagId{rtnl_link_get_name(link), rtnl_link_get_ifindex(link)};
    /* Listens to LAG messages */
    char *type = rtnl_link_get_type(link);
    if (!type || (strcmp(type, TEAM_DRV_NAME) != 0))
        return;

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_LOWER_UP;
    if (type)
    {
        SWSS_LOG_INFO(" nlmsg type:%d key:%s admin:%d oper:%d ifindex:%d type:%s",
                       nlmsg_type, lagId.first.c_str(), admin, oper, lagId.second, type);
    }
    else
    {
        SWSS_LOG_INFO(" nlmsg type:%d key:%s admin:%d oper:%d ifindex:%d",
                       nlmsg_type, lagId.first.c_str(), admin, oper, lagId.second);
    }

    if (nlmsg_type == RTM_DELLINK)
    {
        removeLag(lagId);
        return;
    }

    unsigned int mtu = rtnl_link_get_mtu(link);
    addLag(lagId, admin, oper, mtu);
}

void TeamSync::addLag(const LagId &lagId, bool admin_state, bool oper_state, unsigned int mtu)
{
    const string &lagName = lagId.first;
    shared_ptr<TeamPortSync> tsync;
    auto lag = m_teamSelectables.find(lagId);
    if (lag != m_teamSelectables.end())
    {
        if (lag->first != lagId)
            removeLag(lag->first);
        else
            tsync = lag->second;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin_state ? "up" : "down");
    FieldValueTuple o("oper_status", oper_state ? "up" : "down");
    FieldValueTuple m("mtu", std::to_string(mtu));
    fvVector.push_back(a);
    fvVector.push_back(o);
    fvVector.push_back(m);

    /* Return when the team instance has already been tracked */
    if (tsync)
    {
        if (tsync->admin_state == admin_state && tsync->oper_state == oper_state && tsync->mtu == mtu)
            return;
        tsync->admin_state = admin_state;
        tsync->oper_state = oper_state;
        tsync->mtu = mtu;
    }

    FieldValueTuple s("state", "ok");
    fvVector.push_back(s);

    /* Publish APP_LAG immediately so orchagent can allocate and free LAG ids on
     * rapid add/del; RTM_DELLINK always removes APP_LAG even when TeamPortSync
     * never succeeded.  STATE_LAG is written only after teamd is ready (#3984). */
    m_lagTable.set(lagName, fvVector);

    if (!tsync)
    {
        /* Create the team instance.
         * On container restart, teamsyncd may receive RTM_NEWLINK for pre-existing
         * team devices (from the kernel's initial dump) before teammgrd has had a
         * chance to recreate them via "teamd -r".  The recreate deletes the old
         * kernel device first, so team_init() fails with EADDRNOTAVAIL (errno 99)
         * because the ifindex is no longer valid.  Catch the exception here so it
         * does not propagate through NetLink::readData() into Select::poll_descriptors,
         * which would abort the entire netlink processing loop.  When teamd finishes
         * recreating the device the kernel emits a fresh RTM_NEWLINK with the correct
         * new ifindex and addLag() is called again, at which point initialization
         * succeeds. */
        shared_ptr<TeamPortSync> sync;
        try
        {
            sync = make_shared<TeamPortSync>(lagId, &m_lagMemberTable);
        }
        catch (const system_error& e)
        {
            m_teamSelectables.emplace(lagId, nullptr);
            SWSS_LOG_ERROR("addLag: Failed to initialize team handler for LAG %s "
                           "(ifindex %d): %d:%s. Waiting for next RTM_NEWLINK.",
                           lagName.c_str(), lagId.second, e.code().value(), e.what());
            return;
        }

        sync->admin_state = admin_state;
        sync->oper_state = oper_state;
        sync->mtu = mtu;
        sync->onChange();
        m_stateLagTable.set(lagName, fvVector);
        m_teamSelectables[lagId] = sync;
        m_selectablesToAdd.insert(sync);

        SWSS_LOG_INFO("Add %s admin_status:%s oper_status:%s, mtu: %d",
                       lagName.c_str(), admin_state ? "up" : "down",
                       oper_state ? "up" : "down", mtu);
    }
    else
    {
        m_stateLagTable.set(lagName, fvVector);
    }
}

void TeamSync::removeLag(LagId lagId)
{
    const string &lagName = lagId.first;
    auto lag = m_teamSelectables.find(lagId);
    if (lag != m_teamSelectables.end() && lag->first != lagId)
    {
        SWSS_LOG_INFO("Ignore delete for old LAG %s ifindex %d (current %d)",
                      lagName.c_str(), lagId.second, lag->first.second);
        return;
    }

    if (lag != m_teamSelectables.end() && lag->second)
    {
        auto sync = lag->second;
        /* Select may still dispatch this object in the current poll batch. */
        team_change_handler_unregister(sync->m_team, &TeamPortSync::gPortChangeHandler, sync.get());
        for (auto member : sync->m_lagMembers)
        {
            m_lagMemberTable.del(lagName + ":" + member.first);

            SWSS_LOG_INFO("Remove member %s before removing LAG %s",
                    member.first.c_str(), lagName.c_str());
        }

        if (m_selectablesToAdd.erase(sync) == 0)
        {
            m_selectablesToRemove.insert(sync);
        }
    }

    if (lag != m_teamSelectables.end())
        m_teamSelectables.erase(lag);

    /* Preserved rows must also be invalidated when no handler was tracked. */
    m_lagTable.del(lagName);
    m_stateLagTable.del(lagName);

    SWSS_LOG_INFO("Remove LAG %s", lagName.c_str());
}

void TeamSync::cleanTeamSync()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Cleaning up LAG teamd resources ...");

    while (!m_teamSelectables.empty())
    {
        removeLag(m_teamSelectables.begin()->first);
    }
    return;
}

const struct team_change_handler TeamSync::TeamPortSync::gPortChangeHandler = {
    .func       = TeamSync::TeamPortSync::teamdHandler,
    .type_mask  = TEAM_PORT_CHANGE | TEAM_OPTION_CHANGE
};

TeamSync::TeamPortSync::TeamPortSync(const LagId &lagId, ProducerStateTable *lagMemberTable) :
    m_lagMemberTable(lagMemberTable),
    m_lagId(lagId)
{
    int count = 0;
    int max_retries = 3;

    while (true)
    {
        try
        {
            m_team = team_alloc();
            if (!m_team)
            {
                throw system_error(make_error_code(errc::address_not_available),
                                   "Unable to allocate team socket");
            }

            int err = team_init(m_team, m_lagId.second);
            if (err)
            {
                team_free(m_team);
                m_team = NULL;
                throw system_error(make_error_code(errc::address_not_available),
                                   "Unable to initialize team socket");
            }

            err = team_change_handler_register(m_team, &gPortChangeHandler, this);
            if (err)
            {
                team_free(m_team);
                m_team = NULL;
                throw system_error(make_error_code(errc::address_not_available),
                                   "Unable to register port change event");
            }

            struct teamdctl *m_teamdctl = teamdctl_alloc();
            if (!m_teamdctl)
            {
                team_free(m_team);
                m_team = NULL;
                throw system_error(make_error_code(errc::address_not_available),
                                   "Unable to allocate teamdctl socket");
            }

            err = teamdctl_connect(m_teamdctl, m_lagId.first.c_str(), nullptr, "usock");
            if (err)
            {
                team_free(m_team);
                m_team = NULL;
                teamdctl_free(m_teamdctl);
                throw system_error(make_error_code(errc::connection_refused),
                                   "Unable to connect to teamd");
            }

            char *response;
            err = teamdctl_config_get_raw_direct(m_teamdctl, &response);
            if (err)
            {
                team_free(m_team);
                m_team = NULL;
                teamdctl_disconnect(m_teamdctl);
                teamdctl_free(m_teamdctl);
                throw system_error(make_error_code(errc::io_error),
                                   "Unable to get config from teamd (to prove that it is running and alive)");
            }

            teamdctl_disconnect(m_teamdctl);
            teamdctl_free(m_teamdctl);

            break;
        }
        catch (const system_error& e)
        {
            SWSS_LOG_WARN("Failed to initialize team handler. LAG=%s error=%d:%s, attempt=%d",
                          m_lagId.first.c_str(), e.code().value(), e.what(), count);

            if (++count == max_retries)
            {
                throw;
            }

            sleep(1);
        }
    }
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
            string key = m_lagId.first + ":" + it.first;
            vector<FieldValueTuple> v;
            FieldValueTuple l("status", it.second ? "enabled" : "disabled");
            v.push_back(l);
            m_lagMemberTable->set(key, v);

            SWSS_LOG_INFO("Set LAG %s member %s with status %s",
                    m_lagId.first.c_str(), it.first.c_str(), it.second ? "enabled" : "disabled");
        }
    }

    for (auto it : m_lagMembers)
    {
        if (tmp_lag_members.find(it.first) == tmp_lag_members.end())
        {
            string key = m_lagId.first + ":" + it.first;
            m_lagMemberTable->del(key);

            SWSS_LOG_INFO("Remove member %s from LAG %s",
                    it.first.c_str(), m_lagId.first.c_str());
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

uint64_t TeamSync::TeamPortSync::readData()
{
    team_handle_events(m_team);
    return 0;
}
