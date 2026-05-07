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

static const timespec LAG_MEMBER_DISABLE_DEBOUNCE = {0, 10 * 1000 * 1000};

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

bool TeamSync::processTimerSelectable(Selectable *selectable)
{
    auto timerIt = m_timerToMember.find(selectable);
    if (timerIt == m_timerToMember.end())
    {
        SWSS_LOG_WARN("Detect untracked debounce timer selectable %p!",
                selectable);
        return false;
    }

    auto lag = timerIt->second.first;
    auto memberName = timerIt->second.second;
    auto stateIt = lag->m_memberDebounceStates.find(memberName);
    if (stateIt == lag->m_memberDebounceStates.end() || !stateIt->second.pendingDisable)
    {
        return true;
    }

    auto timer = stateIt->second.timer;

    timer->stop();
    stateIt->second.pendingDisable = false;

    applyLagMemberStatus(lag, memberName, false);
    SWSS_LOG_NOTICE("Debounce timeout expired for LAG %s member %s, apply disabled",
            lag->getLagName().c_str(), memberName.c_str());

    return true;
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

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_LOWER_UP;
    unsigned int ifindex = rtnl_link_get_ifindex(link);

    if (type)
    {
        SWSS_LOG_INFO(" nlmsg type:%d key:%s admin:%d oper:%d ifindex:%d type:%s",
                       nlmsg_type, lagName.c_str(), admin, oper, ifindex, type);
    }
    else
    {
        SWSS_LOG_INFO(" nlmsg type:%d key:%s admin:%d oper:%d ifindex:%d",
                       nlmsg_type, lagName.c_str(), admin, oper, ifindex);
    }

    if (nlmsg_type == RTM_DELLINK)
    {
        if (m_teamSelectables.find(lagName) != m_teamSelectables.end())
        {
            /* Remove LAG ports and delete LAG */
            removeLag(lagName);
        }
        return;
    }

    unsigned int mtu = rtnl_link_get_mtu(link);
    addLag(lagName, rtnl_link_get_ifindex(link),
           rtnl_link_get_flags(link) & IFF_UP,
           rtnl_link_get_flags(link) & IFF_LOWER_UP, mtu);
}

void TeamSync::addLag(const string &lagName, int ifindex, bool admin_state,
                      bool oper_state, unsigned int mtu)
{
    /* Set the LAG */
    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin_state ? "up" : "down");
    FieldValueTuple o("oper_status", oper_state ? "up" : "down");
    FieldValueTuple m("mtu", std::to_string(mtu));
    fvVector.push_back(a);
    fvVector.push_back(o);
    fvVector.push_back(m);
    m_lagTable.set(lagName, fvVector);

    SWSS_LOG_INFO("Add %s admin_status:%s oper_status:%s, mtu: %d",
                   lagName.c_str(), admin_state ? "up" : "down", oper_state ? "up" : "down", mtu);

    bool lag_update = true;
    /* Return when the team instance has already been tracked */
    if (m_teamSelectables.find(lagName) != m_teamSelectables.end())
    {
        auto tsync = m_teamSelectables[lagName];
        if (tsync->admin_state == admin_state && tsync->oper_state == oper_state && tsync->mtu == mtu)
            return;
        tsync->admin_state = admin_state;
        tsync->oper_state = oper_state;
        tsync->mtu = mtu;
        lag_update = false;
    }

    FieldValueTuple s("state", "ok");
    fvVector.push_back(s);
    if (m_warmstart)
    {
        m_stateLagTablePreserved[lagName] = fvVector;
    }
    else
    {
        m_stateLagTable.set(lagName, fvVector);
    }

    if (lag_update)
    {
        /* Create the team instance */
        auto sync = make_shared<TeamPortSync>(this, lagName, ifindex, &m_lagMemberTable);
        m_teamSelectables[lagName] = sync;
        m_selectablesToAdd.insert(lagName);
    }
}

void TeamSync::applyLagMemberStatus(TeamPortSync *lag, const string &memberName, bool enabled)
{
    string key = lag->getLagName() + ":" + memberName;
    vector<FieldValueTuple> v;
    FieldValueTuple l("status", enabled ? "enabled" : "disabled");
    v.push_back(l);
    m_lagMemberTable.set(key, v);
    lag->m_lagMembers[memberName] = enabled;

    SWSS_LOG_NOTICE("Set LAG %s member %s with status %s",
            lag->getLagName().c_str(), memberName.c_str(), enabled ? "enabled" : "disabled");
}

void TeamSync::ensureMemberTimer(TeamPortSync *lag, const string &memberName)
{
    if (lag->m_memberDebounceStates.find(memberName) != lag->m_memberDebounceStates.end())
    {
        return;
    }

    TeamSync::TeamPortSync::MemberDebounceState state;
    state.timer = make_shared<SelectableTimer>(LAG_MEMBER_DISABLE_DEBOUNCE);

    lag->m_memberDebounceStates[memberName] = state;
    m_timerToMember[state.timer.get()] = make_pair(lag, memberName);
    m_select->addSelectable(state.timer.get());
}

void TeamSync::startPendingDisable(TeamPortSync *lag, const string &memberName)
{
    ensureMemberTimer(lag, memberName);
    auto &state = lag->m_memberDebounceStates[memberName];
    if (state.pendingDisable)
    {
        return;
    }

    state.pendingDisable = true;
    state.timer->start();

    SWSS_LOG_NOTICE("Start pending disable debounce for LAG %s member %s",
            lag->getLagName().c_str(), memberName.c_str());
}

void TeamSync::cancelPendingDisable(TeamPortSync *lag, const string &memberName)
{
    auto stateIt = lag->m_memberDebounceStates.find(memberName);
    if (stateIt == lag->m_memberDebounceStates.end() || !stateIt->second.pendingDisable)
    {
        return;
    }

    stateIt->second.timer->stop();
    stateIt->second.pendingDisable = false;

    SWSS_LOG_NOTICE("Cancel pending disable debounce for LAG %s member %s",
            lag->getLagName().c_str(), memberName.c_str());
}

void TeamSync::clearPendingDisablesForLag(TeamPortSync *lag)
{
    vector<string> members;
    for (const auto &it : lag->m_memberDebounceStates)
    {
        if (it.second.pendingDisable)
        {
            members.push_back(it.first);
        }
    }

    for (const auto &memberName : members)
    {
        cancelPendingDisable(lag, memberName);
    }
}

void TeamSync::handleLagMemberStatus(TeamPortSync *lag, const string &memberName, bool observedEnabled)
{
    auto it = lag->m_lagMembers.find(memberName);
    bool appliedEnabled = (it != lag->m_lagMembers.end()) && it->second;

    auto debounceIt = lag->m_memberDebounceStates.find(memberName);
    bool hasPendingDisable = (debounceIt != lag->m_memberDebounceStates.end()) &&
                             debounceIt->second.pendingDisable;

    if (observedEnabled)
    {
        if (hasPendingDisable)
        {
            cancelPendingDisable(lag, memberName);
        }
        else if (!appliedEnabled)
        {
            applyLagMemberStatus(lag, memberName, true);
        }
        return;
    }

    if (it == lag->m_lagMembers.end())
    {
        applyLagMemberStatus(lag, memberName, false);
    }
    else if (appliedEnabled && !hasPendingDisable)
    {
        startPendingDisable(lag, memberName);
    }
}

void TeamSync::removeLag(const string &lagName)
{
    /* Delete all members */
    auto selectable = m_teamSelectables[lagName];

    clearPendingDisablesForLag(selectable.get());

    for (auto it : selectable->m_lagMembers)
    {
        auto stateIt = selectable->m_memberDebounceStates.find(it.first);
        if (stateIt != selectable->m_memberDebounceStates.end())
        {
            m_timerToMember.erase(stateIt->second.timer.get());
            m_select->removeSelectable(stateIt->second.timer.get());
            selectable->m_memberDebounceStates.erase(stateIt);
        }

        m_lagMemberTable.del(lagName + ":" + it.first);

        SWSS_LOG_INFO("Remove member %s before removing LAG %s",
                it.first.c_str(), lagName.c_str());
    }

    /* Delete the LAG */
    m_lagTable.del(lagName);

    SWSS_LOG_INFO("Remove LAG %s", lagName.c_str());

    /* Return when the team instance hasn't been tracked before */
    if (m_teamSelectables.find(lagName) == m_teamSelectables.end())
        return;

    if (m_warmstart)
    {
        m_stateLagTablePreserved.erase(lagName);
    }
    else
    {
        m_stateLagTable.del(lagName);
    }

    m_selectablesToRemove.insert(lagName);
}

void TeamSync::cleanTeamSync()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Cleaning up LAG teamd resources ...");

    for (const auto& it: m_teamSelectables)
    {
        /* Cleanup LAG */
        removeLag(it.first);
    }
    return;
}

const struct team_change_handler TeamSync::TeamPortSync::gPortChangeHandler = {
    .func       = TeamSync::TeamPortSync::teamdHandler,
    .type_mask  = TEAM_PORT_CHANGE | TEAM_OPTION_CHANGE
};

TeamSync::TeamPortSync::TeamPortSync(TeamSync *parent, const string &lagName, int ifindex,
                                     ProducerStateTable *lagMemberTable) :
    m_parent(parent),
    m_lagMemberTable(lagMemberTable),
    m_lagName(lagName),
    m_ifindex(ifindex)
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

            int err = team_init(m_team, ifindex);
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

            err = teamdctl_connect(m_teamdctl, lagName.c_str(), nullptr, "usock");
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
                          lagName.c_str(), e.code().value(), e.what(), count);

            if (++count == max_retries)
            {
                throw;
            }

            sleep(1);
        }
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

    if (!m_parent)
    {
        SWSS_LOG_ERROR("TeamSync pointer is NULL!");
        return -1;
    }

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
        m_parent->handleLagMemberStatus(this, it.first, it.second);
    }

    vector<string> removedMembers;
    for (auto it : m_lagMembers)
    {
        if (tmp_lag_members.find(it.first) == tmp_lag_members.end())
        {
            removedMembers.push_back(it.first);
        }
    }

    for (const auto &memberName : removedMembers)
    {
        string key = m_lagName + ":" + memberName;

        m_parent->cancelPendingDisable(this, memberName);

        auto stateIt = m_memberDebounceStates.find(memberName);
        if (stateIt != m_memberDebounceStates.end())
        {
            m_parent->m_timerToMember.erase(stateIt->second.timer.get());
            m_parent->m_select->removeSelectable(stateIt->second.timer.get());
            m_memberDebounceStates.erase(stateIt);
        }

        m_lagMemberTable->del(key);
        m_lagMembers.erase(memberName);

        SWSS_LOG_INFO("Remove member %s from LAG %s",
                memberName.c_str(), m_lagName.c_str());
    }

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
