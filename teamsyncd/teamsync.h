#ifndef __TEAMSYNC__
#define __TEAMSYNC__

#include <map>
#include <set>
#include <string>
#include <memory>
#include <unordered_map>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "selectable.h"
#include "select.h"
#include "selectabletimer.h"
#include "netmsg.h"
#include <team.h>

#define TEAMSYNCD_APP_NAME  "teamsyncd"
// seconds
const uint32_t DEFAULT_WR_PENDING_TIMEOUT = 70;

using namespace std::chrono;

namespace swss {

class TeamSync : public NetMsg
{
    friend class TeamPortSync;
public:
    TeamSync(DBConnector *db, DBConnector *stateDb, Select *select);

    void periodic();
    void cleanTeamSync();
    bool processTimerSelectable(Selectable *selectable);

    /* Listen to RTM_NEWLINK, RTM_DELLINK to track team devices */
    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    class TeamPortSync : public Selectable
    {
    public:
        enum { MAX_IFNAME = 64 };
        TeamPortSync(TeamSync *parent, const std::string &lagName, int ifindex,
                     ProducerStateTable *lagMemberTable);
        ~TeamPortSync();

        const std::string &getLagName() const
        {
            return m_lagName;
        }

        int getFd() override;
        uint64_t readData() override;

        /* member_name -> enabled|disabled */
        std::map<std::string, bool> m_lagMembers;

        struct MemberDebounceState
        {
            std::shared_ptr<SelectableTimer> timer;
            bool pendingDisable = false;
        };

        std::map<std::string, MemberDebounceState> m_memberDebounceStates;
        bool admin_state;
        bool oper_state;
        unsigned int mtu;
    protected:
        int onChange();
        static int teamdHandler(struct team_handle *th, void *arg,
                                team_change_type_mask_t type_mask);
        static const struct team_change_handler gPortChangeHandler;
    private:
        TeamSync *m_parent;
        ProducerStateTable *m_lagMemberTable;
        struct team_handle *m_team;
        std::string m_lagName;
        int m_ifindex;
    };

protected:
    void addLag(const std::string &lagName, int ifindex, bool admin_state,
                bool oper_state, unsigned int mtu);
    void removeLag(const std::string &lagName);

    void handleLagMemberStatus(TeamPortSync *lag, const std::string &memberName, bool observedEnabled);
    void applyLagMemberStatus(TeamPortSync *lag, const std::string &memberName, bool enabled);
    void ensureMemberTimer(TeamPortSync *lag, const std::string &memberName);
    void startPendingDisable(TeamPortSync *lag, const std::string &memberName);
    void cancelPendingDisable(TeamPortSync *lag, const std::string &memberName);
    void clearPendingDisablesForLag(TeamPortSync *lag);

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

    std::map<Selectable *, std::pair<TeamPortSync *, std::string>> m_timerToMember;
};

}

#endif
