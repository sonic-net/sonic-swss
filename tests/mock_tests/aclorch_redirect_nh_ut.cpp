#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"

/*
 * ACL rules with REDIRECT_ACTION "<ip>@<intf>" (or a next hop group) follow the neighbor:
 *  - a rule configured before its next hop is resolved is created once the neighbor is added;
 *  - a rule whose neighbor is removed is taken out of the ASIC and comes back with the neighbor.
 */
namespace aclorch_redirect_nh_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    static const string RIF_PORT = ETHERNET0;
    static const string NH_IP1 = "10.0.0.2";
    static const string NH_IP2 = "10.0.0.3";
    static const string NH_MAC1 = "00:00:0a:00:00:02";
    static const string NH_MAC2 = "00:00:0a:00:00:03";

    class AclRedirectNeighborTest : public MockOrchTest
    {
    protected:
        string acl_table_type = "PBR_TABLE_TYPE";
        string acl_table = "PBR_TABLE";
        string acl_rule = "RULE_1";

        void ApplyInitialConfigs() override
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ETHERNET0, ports[ETHERNET0]);
            port_table.set(ETHERNET4, ports[ETHERNET4]);
            port_table.set("PortConfigDone", { { "count", to_string(2) } });
            port_table.set("PortInitDone", { {} });
            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            intf_table.set(RIF_PORT, { { "NULL", "NULL" }, { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(RIF_PORT + intf_table.getTableNameSeparator() + "10.0.0.1/24",
                           { { "scope", "global" }, { "family", "IPv4" } });
            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();
        }

        void PostSetUp() override
        {
            doAclTask(CFG_ACL_TABLE_TYPE_TABLE_NAME, {
                { acl_table_type, SET_COMMAND,
                  { { ACL_TABLE_TYPE_MATCHES, MATCH_SRC_IP },
                    { ACL_TABLE_TYPE_ACTIONS, ACTION_REDIRECT_ACTION } } } });
            doAclTask(CFG_ACL_TABLE_TABLE_NAME, {
                { acl_table, SET_COMMAND,
                  { { ACL_TABLE_TYPE, acl_table_type }, { ACL_TABLE_STAGE, STAGE_INGRESS } } } });
            ASSERT_NE(gAclOrch->getTableById(acl_table), SAI_NULL_OBJECT_ID);
        }

        void doAclTask(const string &table, const deque<KeyOpFieldsValuesTuple> &entries)
        {
            /* Use the orch's own consumer, the one rules are re-queued into */
            auto consumer = dynamic_cast<Consumer *>(gAclOrch->getExecutor(table));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync(entries);
            static_cast<Orch *>(gAclOrch)->doTask(*consumer);
        }

        void addRule(const string &redirect)
        {
            doAclTask(CFG_ACL_RULE_TABLE_NAME, {
                { acl_table + "|" + acl_rule, SET_COMMAND,
                  { { RULE_PRIORITY, "100" },
                    { MATCH_SRC_IP, "192.168.10.0/24" },
                    { ACTION_REDIRECT_ACTION, redirect } } } });
        }

        void delRule()
        {
            doAclTask(CFG_ACL_RULE_TABLE_NAME, { { acl_table + "|" + acl_rule, DEL_COMMAND, {} } });
        }

        void neighborOp(const string &ip, const string &op, const string &mac = "")
        {
            auto consumer = dynamic_cast<Consumer *>(gNeighOrch->getExecutor(APP_NEIGH_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            vector<FieldValueTuple> fvs;
            if (op == SET_COMMAND)
            {
                fvs = { { "neigh", mac }, { "family", "IPv4" } };
            }
            consumer->addToSync({ { RIF_PORT + ":" + ip, op, fvs } });
            static_cast<Orch *>(gNeighOrch)->doTask(*consumer);
        }

        /* One orchagent main-loop pass for the ACL orch: process re-queued rules */
        void runAclOrch()
        {
            static_cast<Orch *>(gAclOrch)->doTask();
        }

        string ruleStatus()
        {
            Table state(m_state_db.get(), STATE_ACL_RULE_TABLE_NAME);
            string status;
            state.hget(acl_table + "|" + acl_rule, "status", status);
            return status;
        }

        bool ruleInstalled()
        {
            return gAclOrch->getAclRule(acl_table, acl_rule) != nullptr;
        }

        bool waiting()
        {
            return gAclOrch->isAclRuleWaitingForNextHop(acl_table, acl_rule);
        }

        int nhRefCount(const string &ip)
        {
            return gNeighOrch->getNextHopRefCount(NextHopKey(ip, RIF_PORT));
        }
    };

    TEST_F(AclRedirectNeighborTest, RuleCreatedBeforeNeighborBecomesActiveOnResolve)
    {
        addRule(NH_IP1 + "@" + RIF_PORT);

        ASSERT_FALSE(ruleInstalled());
        ASSERT_TRUE(waiting());
        ASSERT_EQ(ruleStatus(), "Inactive");

        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        ASSERT_TRUE(gNeighOrch->hasNextHop(NextHopKey(NH_IP1, RIF_PORT)));
        runAclOrch();

        ASSERT_TRUE(ruleInstalled());
        ASSERT_FALSE(waiting());
        ASSERT_EQ(ruleStatus(), "Active");
        ASSERT_EQ(nhRefCount(NH_IP1), 1);

        delRule();
        ASSERT_FALSE(ruleInstalled());
        ASSERT_EQ(nhRefCount(NH_IP1), 0);
    }

    TEST_F(AclRedirectNeighborTest, NeighborRemovalDeactivatesRuleAndRelearnRestoresIt)
    {
        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        addRule(NH_IP1 + "@" + RIF_PORT);
        ASSERT_TRUE(ruleInstalled());
        ASSERT_EQ(ruleStatus(), "Active");
        ASSERT_EQ(nhRefCount(NH_IP1), 1);

        /* The ACL reference no longer blocks the neighbor removal */
        neighborOp(NH_IP1, DEL_COMMAND);
        ASSERT_FALSE(gNeighOrch->hasNextHop(NextHopKey(NH_IP1, RIF_PORT)));
        ASSERT_FALSE(ruleInstalled());
        ASSERT_TRUE(waiting());
        ASSERT_EQ(ruleStatus(), "Inactive");

        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        runAclOrch();
        ASSERT_TRUE(ruleInstalled());
        ASSERT_FALSE(waiting());
        ASSERT_EQ(ruleStatus(), "Active");
        ASSERT_EQ(nhRefCount(NH_IP1), 1);

        delRule();
        neighborOp(NH_IP1, DEL_COMMAND);
        ASSERT_FALSE(gNeighOrch->hasNextHop(NextHopKey(NH_IP1, RIF_PORT)));
    }

    TEST_F(AclRedirectNeighborTest, CancelledNeighborRemovalRestoresRule)
    {
        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        addRule(NH_IP1 + "@" + RIF_PORT);
        ASSERT_TRUE(ruleInstalled());

        /* Another user (e.g. a route) keeps the next hop: the DEL stays pending */
        NextHopKey nh(NH_IP1, RIF_PORT);
        gNeighOrch->increaseNextHopRefCount(nh);

        neighborOp(NH_IP1, DEL_COMMAND);
        ASSERT_TRUE(gNeighOrch->hasNextHop(nh));
        ASSERT_FALSE(ruleInstalled());
        ASSERT_TRUE(waiting());

        /* Neighbor learned again with the same MAC: the pending DEL is dropped */
        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        runAclOrch();
        ASSERT_TRUE(ruleInstalled());
        ASSERT_FALSE(waiting());
        ASSERT_EQ(ruleStatus(), "Active");
        ASSERT_EQ(gNeighOrch->getNextHopRefCount(nh), 2);

        delRule();
        gNeighOrch->decreaseNextHopRefCount(nh);
        neighborOp(NH_IP1, DEL_COMMAND);
        ASSERT_FALSE(gNeighOrch->hasNextHop(nh));
    }

    TEST_F(AclRedirectNeighborTest, DeletedWaitingRuleIsNotRecreated)
    {
        addRule(NH_IP1 + "@" + RIF_PORT);
        ASSERT_TRUE(waiting());

        delRule();
        ASSERT_FALSE(waiting());

        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        runAclOrch();
        ASSERT_FALSE(ruleInstalled());
        ASSERT_EQ(nhRefCount(NH_IP1), 0);

        neighborOp(NH_IP1, DEL_COMMAND);
    }

    TEST_F(AclRedirectNeighborTest, NextHopGroupWaitsForAllMembers)
    {
        string nhg = NH_IP1 + "@" + RIF_PORT + "," + NH_IP2 + "@" + RIF_PORT;

        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        addRule(nhg);
        ASSERT_FALSE(ruleInstalled());
        ASSERT_TRUE(waiting());

        neighborOp(NH_IP2, SET_COMMAND, NH_MAC2);
        /* Mock ports are oper down: group members on a down port are skipped */
        gNeighOrch->ifChangeInformNextHop(RIF_PORT, true);
        runAclOrch();
        ASSERT_TRUE(ruleInstalled());
        ASSERT_FALSE(waiting());
        ASSERT_EQ(ruleStatus(), "Active");
        ASSERT_TRUE(gRouteOrch->hasNextHopGroup(NextHopGroupKey(nhg)));

        /* Losing one member takes the rule out; the group is released with it */
        neighborOp(NH_IP2, DEL_COMMAND);
        ASSERT_FALSE(ruleInstalled());
        ASSERT_TRUE(waiting());
        ASSERT_FALSE(gRouteOrch->hasNextHopGroup(NextHopGroupKey(nhg)));
        ASSERT_EQ(nhRefCount(NH_IP1), 0);

        neighborOp(NH_IP2, SET_COMMAND, NH_MAC2);
        gNeighOrch->ifChangeInformNextHop(RIF_PORT, true);
        runAclOrch();
        ASSERT_TRUE(ruleInstalled());
        ASSERT_EQ(ruleStatus(), "Active");

        delRule();
        ASSERT_FALSE(gRouteOrch->hasNextHopGroup(NextHopGroupKey(nhg)));
        neighborOp(NH_IP1, DEL_COMMAND);
        neighborOp(NH_IP2, DEL_COMMAND);
    }

    TEST_F(AclRedirectNeighborTest, NewConfigSupersedesWaitingRule)
    {
        neighborOp(NH_IP2, SET_COMMAND, NH_MAC2);

        addRule(NH_IP1 + "@" + RIF_PORT);
        ASSERT_TRUE(waiting());

        /* The rule is re-configured to a resolved next hop */
        addRule(NH_IP2 + "@" + RIF_PORT);
        ASSERT_TRUE(ruleInstalled());
        ASSERT_FALSE(waiting());
        ASSERT_EQ(nhRefCount(NH_IP2), 1);

        /* The old target resolving later changes nothing */
        neighborOp(NH_IP1, SET_COMMAND, NH_MAC1);
        runAclOrch();
        ASSERT_EQ(nhRefCount(NH_IP1), 0);
        ASSERT_EQ(nhRefCount(NH_IP2), 1);

        delRule();
        neighborOp(NH_IP1, DEL_COMMAND);
        neighborOp(NH_IP2, DEL_COMMAND);
    }
}
