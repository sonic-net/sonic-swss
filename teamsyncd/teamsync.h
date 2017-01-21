#ifndef __TEAMSYNC__
#define __TEAMSYNC__

#include <map>
#include <string>
#include <memory>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "selectable.h"
#include "select.h"
#include "netmsg.h"
#include <team.h>

namespace swss {

class TeamSync : public NetMsg
{
public:
    TeamSync(DBConnector *db, Select *select);

    /*
     * Listens to RTM_NEWLINK and RTM_DELLINK to undestand if there is a new
     * team device
     */
    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    class TeamPortSync : public Selectable
    {
    public:
        enum { MAX_IFNAME = 64 };
        TeamPortSync(const std::string &lagName, int ifindex,
                     ProducerStateTable *lagTable);
        ~TeamPortSync();

        virtual void addFd(fd_set *fd);
        virtual bool isMe(fd_set *fd);
        virtual int readCache();
        virtual void readMe();

    protected:
        int onChange();
        static int teamdHandler(struct team_handle *th, void *arg,
                                team_change_type_mask_t type_mask);
        static const struct team_change_handler gPortChangeHandler;
    private:
        ProducerStateTable *m_lagTable;
        struct team_handle *m_team;
        std::string m_lagName;
        int m_ifindex;
        std::map<std::string, bool> m_lagMembers; /* map[ifname] = status (enabled|disabled) */
    };

protected:
    void addLag(const std::string &lagName, int ifindex, bool admin_state,
                bool oper_state, unsigned int mtu);
    void removeLag(const std::string &lagName);

private:
    Select *m_select;
    ProducerStateTable m_lagTable;
    std::map<std::string, std::shared_ptr<TeamPortSync> > m_teamPorts;
};

}

#endif
