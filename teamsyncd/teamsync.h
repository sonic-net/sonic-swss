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

#define TEAMSYNCD_APP_NAME  "teamsyncd"
// seconds
const uint32_t DEFAULT_WR_PENDING_TIMEOUT = 70;

using namespace std::chrono;

namespace swss {

class TeamSync : public NetMsg
{
public:
    TeamSync(DBConnector *db, DBConnector *stateDb, Select *select);

    void periodic();
    void cleanTeamSync();

    /* Listen to RTM_NEWLINK, RTM_DELLINK to track team devices */
    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    class TeamPortSync : public Selectable
    {
    public:
        enum { MAX_IFNAME = 64 };
        TeamPortSync(const std::string &lagName, int ifindex,
                     ProducerStateTable *lagMemberTable);
        ~TeamPortSync();

        int getFd() override;
        uint64_t readData() override;

        /* member_name -> enabled|disabled */
        std::map<std::string, bool> m_lagMembers;
        bool admin_state;
        bool oper_state;
        unsigned int mtu;
    protected:
        int onChange();
        static int teamdHandler(struct team_handle *th, void *arg,
                                team_change_type_mask_t type_mask);
        static const struct team_change_handler gPortChangeHandler;
    private:
        ProducerStateTable *m_lagMemberTable;
        struct team_handle *m_team;
        std::string m_lagName;
        int m_ifindex;
    };

protected:
    void addLag(const std::string &lagName, int ifindex, bool admin_state,
                bool oper_state, unsigned int mtu);
    void removeLag(const std::string &lagName);

    /* valid only in WR mode */
    void applyState();

    /* Handle all selectables add/removal events */
    void doSelectableTask();

private:
    Select *m_select;
    ProducerStateTable m_lagTable;
    ProducerStateTable m_lagMemberTable;
    Table m_stateLagTable;

    bool m_warmstart;
    std::unordered_map<std::string, std::vector<FieldValueTuple>> m_stateLagTablePreserved;
    steady_clock::time_point m_start_time;
    uint32_t m_pending_timeout;

    /* Store selectables needed to be updated in doSelectableTask function */
    std::set<std::string> m_selectablesToAdd;
    std::set<std::string> m_selectablesToRemove;
    std::map<std::string, std::shared_ptr<TeamPortSync> > m_teamSelectables;
};

}

#endif
