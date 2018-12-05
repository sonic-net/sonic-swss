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

// seconds
#define WR_PENDING_TIME 70

namespace swss {

class TeamSync : public NetMsg
{
public:
    TeamSync(DBConnector *db, DBConnector *stateDb, Select *select);

    /* valid only in WR mode */
    void applyState();

    /* Listen to RTM_NEWLINK, RTM_DELLINK to track team devices */
    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    /* Handle all selectables add/removal events */
    void doSelectableTask();

    class TeamPortSync : public Selectable
    {
    public:
        enum { MAX_IFNAME = 64 };
        TeamPortSync(const std::string &lagName, int ifindex,
                     ProducerStateTable *lagMemberTable);
        ~TeamPortSync();

        int getFd() override;
        void readData() override;

        /* member_name -> enabled|disabled */
        std::map<std::string, bool> m_lagMembers;
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
                bool oper_state);
    void removeLag(const std::string &lagName);

private:
    Select *m_select;
    ProducerStateTable m_lagTable;
    ProducerStateTable m_lagMemberTable;
    Table m_stateLagTable;

    bool m_warmstart;
    std::unordered_map<std::string, std::vector<FieldValueTuple>> m_stateLagTablePreserved;

    /* Store selectables needed to be updated in doSelectableTask function */
    std::set<std::string> m_selectablesToAdd;
    std::set<std::string> m_selectablesToRemove;
    std::map<std::string, std::shared_ptr<TeamPortSync> > m_teamSelectables;
};

}

#endif
