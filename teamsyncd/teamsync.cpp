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
#include "teamsync.h"

using namespace std;
using namespace swss;

/* Taken from drivers/net/team/team.c */
#define TEAM_DRV_NAME "team"

TeamSync::TeamSync(DBConnector *db, Select *select) :
    m_select(select),
    m_lagTable(db, APP_LAG_TABLE_NAME)
{
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

    bool tracked = m_teamPorts.find(lagName) != m_teamPorts.end();

    if ((nlmsg_type == RTM_DELLINK) && tracked)
    {
        /* Remove LAG ports and delete LAG */
        removeLag(lagName);
        return;
    }

    if ((nlmsg_type == RTM_NEWLINK) && tracked)
        return;

    /*
     * New LAG was dedcated for the first time. Sync admin and oper state since
     * portsyncd reflects only changes
     */
    addLag(lagName, rtnl_link_get_ifindex(link),
           rtnl_link_get_flags(link) & IFF_UP,
           rtnl_link_get_flags(link) & IFF_LOWER_UP,
           rtnl_link_get_mtu(link));
}

void TeamSync::addLag(const string &lagName, int ifindex, bool admin_state,
                      bool oper_state, unsigned int mtu)
{
    /* First add the LAG itself */
    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin_state ? "up" : "down");
    FieldValueTuple o("oper_status", oper_state ? "up" : "down");
    FieldValueTuple m("mtu", to_string(mtu));
    fvVector.push_back(a);
    fvVector.push_back(o);
    fvVector.push_back(m);
    m_lagTable.set(lagName, fvVector);

    /* Start adding ports to LAG */
    TeamPortSync *sync = new TeamPortSync(lagName, ifindex, &m_lagTable);
    m_select->addSelectable(sync);
    m_teamPorts[lagName] = shared_ptr<TeamPortSync>(sync);
}

void TeamSync::removeLag(const string &lagName)
{
    m_select->removeSelectable(m_teamPorts[lagName].get());
    m_teamPorts.erase(lagName);
    m_lagTable.del(lagName);
}

const struct team_change_handler TeamSync::TeamPortSync::gPortChangeHandler = {
    .func       = TeamSync::TeamPortSync::teamdHandler,
    .type_mask  = TEAM_PORT_CHANGE
};

TeamSync::TeamPortSync::TeamPortSync(const string &lagName, int ifindex,
                                     ProducerStateTable *lagTable) :
    m_lagTable(lagTable),
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
    if (err) {
        team_free(m_team);
        m_team = NULL;
        SWSS_LOG_ERROR("Unable to init team socket");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to init team socket");
    }

    err = team_change_handler_register(m_team, &gPortChangeHandler, this);
    if (err) {
        team_free(m_team);
        m_team = NULL;
        SWSS_LOG_ERROR("Unable to register port change event");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to register port change event");
    }

    onPortChange(true);
}

TeamSync::TeamPortSync::~TeamPortSync()
{
    if (m_team)
    {
        team_change_handler_unregister(m_team, &gPortChangeHandler, this);
        team_free(m_team);
    }
}

int TeamSync::TeamPortSync::onPortChange(bool isInit)
{
    struct team_port *port;
    team_for_each_port(port, m_team)
    {
        if (isInit || team_is_port_changed(port))
        {
            string key = m_lagName;
            key += ":";
            uint32_t ifindex = team_get_port_ifindex(port);
            char ifname[MAX_IFNAME + 1] = {0};
            key += team_ifindex2ifname(m_team, ifindex, ifname, MAX_IFNAME);

            if (team_is_port_removed(port))
            {
                m_lagTable->del(key);
            } else
            {
                std::vector<FieldValueTuple> fvVector;
                FieldValueTuple l("linkup", team_is_port_link_up(port) ? "up" : "down");
                FieldValueTuple s("speed", to_string(team_get_port_speed(port)) + "Mbit");
                FieldValueTuple d("duplex", team_get_port_duplex(port) ? "full" : "half");
                fvVector.push_back(l);
                fvVector.push_back(s);
                fvVector.push_back(d);
                m_lagTable->set(key, fvVector);
            }
        }
    }
    return 0;
}

int TeamSync::TeamPortSync::teamdHandler(struct team_handle *team, void *arg,
                                         team_change_type_mask_t type_mask)
{
    return ((TeamSync::TeamPortSync *)arg)->onPortChange(false);
}

void TeamSync::TeamPortSync::addFd(fd_set *fd)
{
    FD_SET(team_get_event_fd(m_team), fd);
}

bool TeamSync::TeamPortSync::isMe(fd_set *fd)
{
    return FD_ISSET(team_get_event_fd(m_team), fd);
}

int TeamSync::TeamPortSync::readCache()
{
    return NODATA;
}

void TeamSync::TeamPortSync::readMe()
{
    team_handle_events(m_team);
}
