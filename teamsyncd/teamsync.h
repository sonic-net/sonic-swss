#ifndef __TEAMSYNC__
#define __TEAMSYNC__

#include <map>
#include <string>
#include <memory>
#include "common/dbconnector.h"
#include "common/producertable.h"
#include "common/selectable.h"
#include "common/select.h"
#include "common/netmsg.h"
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
                     ProducerTable *lagTable);
        ~TeamPortSync();

        virtual void addFd(fd_set *fd);
        virtual bool isMe(fd_set *fd);
        virtual int readCache();
        virtual void readMe();

    protected:
        int onPortChange(bool isInit);
        static int teamdHandler(struct team_handle *th, void *arg,
                                team_change_type_mask_t type_mask);
        static const struct team_change_handler gPortChangeHandler;
    private:
        ProducerTable *m_lagTable;
        struct team_handle *m_team;
        std::string m_lagName;
        int m_ifindex;
    };

protected:
    void addLag(const std::string &lagName, int ifindex, bool admin_state,
                bool oper_state, unsigned int mtu);
    void removeLag(const std::string &lagName);

private:
    Select *m_select;
    ProducerTable m_lagTable;
    std::map<std::string, std::shared_ptr<TeamPortSync> > m_teamPorts;
};

}

#endif
