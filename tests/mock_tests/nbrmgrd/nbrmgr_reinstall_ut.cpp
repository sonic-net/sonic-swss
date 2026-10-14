#include "gtest/gtest.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include "../mock_table.h"
#include "warm_restart.h"
#define private public
#define protected public
#include "nbrmgr.h"
#undef private
#undef protected

/*
 * The kernel flushes the PERMANENT neighbours nbrmgrd installed from the
 * CONFIG_DB NEIGH table when the netdev goes admin down or its MAC is set.
 * nbrmgrd puts them back: at once when the netdev is up, else when it comes up.
 *
 * "lo" (ifindex 1) is used as the interface of the immediate path: it exists
 * and is up in any network namespace. No interface is in the STATE_DB
 * INTERFACE_TABLE, so a re-installed entry stays pending in the consumer,
 * where the test sees it, and nothing is programmed into the kernel of the
 * test host (setNeighbor's netlink calls are wrapped in this binary anyway).
 */
namespace nbrmgr_reinstall_ut
{
    static const int LO_IFINDEX = 1;

    struct NbrMgrReinstallTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::NbrMgr> m_nbrMgr;

        void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("nbrmgrd", "swss");

            std::vector<std::string> tables = { CFG_NEIGH_TABLE_NAME };
            m_nbrMgr = std::make_shared<swss::NbrMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);

            cfgSet("lo|10.0.0.1", "00:11:22:33:44:55", "IPv4");
            cfgSet("lo|FC00:0::1", "00:11:22:33:44:56", "IPv6");
            cfgSet("Ethernet4|10.0.0.5", "00:11:22:33:44:57", "IPv4");
            /* records the static entries; nothing is sent to the kernel while the
             * interfaces are not ready, so drop the pending copies */
            static_cast<Orch *>(m_nbrMgr.get())->doTask();
            ASSERT_EQ(m_nbrMgr->m_staticNeigh.size(), 3u);
            consumer()->m_toSync.clear();
        }

        void cfgSet(const std::string &key, const std::string &mac, const std::string &family)
        {
            std::vector<swss::FieldValueTuple> fvs = { { "neigh", mac }, { "family", family } };
            swss::Table neigh(m_config_db.get(), CFG_NEIGH_TABLE_NAME);
            neigh.set(key, fvs);
            consumer()->addToSync(std::deque<swss::KeyOpFieldsValuesTuple>{ { key, SET_COMMAND, fvs } });
        }

        void delNeigh(int ifindex, const char *ip, int family = AF_INET)
        {
            struct rtnl_neigh *n = rtnl_neigh_alloc();
            unsigned char buf[16] = {0};
            int af = (family == AF_BRIDGE) ? AF_INET : family;
            inet_pton(af, ip, buf);
            struct nl_addr *dst = nl_addr_build(af, buf, af == AF_INET ? 4 : 16);
            rtnl_neigh_set_family(n, family);
            rtnl_neigh_set_ifindex(n, ifindex);
            rtnl_neigh_set_dst(n, dst);
            m_nbrMgr->onMsg(RTM_DELNEIGH, (struct nl_object *)n);
            nl_addr_put(dst);
            rtnl_neigh_put(n);
        }

        void newLink(const char *name, unsigned int flags)
        {
            struct rtnl_link *l = rtnl_link_alloc();
            rtnl_link_set_name(l, name);
            rtnl_link_set_flags(l, flags);
            m_nbrMgr->onMsg(RTM_NEWLINK, (struct nl_object *)l);
            rtnl_link_put(l);
        }

        Consumer *consumer()
        {
            return dynamic_cast<Consumer *>(m_nbrMgr->getExecutor(CFG_NEIGH_TABLE_NAME));
        }

        std::set<std::string> pending()
        {
            std::set<std::string> keys;
            for (const auto &it : consumer()->m_toSync)
            {
                keys.insert(it.first);
            }
            return keys;
        }
    };

    TEST_F(NbrMgrReinstallTest, FlushedWhileUpIsReinstalledAtOnce)
    {
        delNeigh(LO_IFINDEX, "10.0.0.1");
        EXPECT_EQ(pending(), (std::set<std::string>{ "lo|10.0.0.1" }));

        /* the kernel reports the canonical IPv6 form */
        delNeigh(LO_IFINDEX, "fc00::1", AF_INET6);
        EXPECT_EQ(pending(), (std::set<std::string>{ "lo|10.0.0.1", "lo|FC00:0::1" }));
        EXPECT_TRUE(m_nbrMgr->m_flushedStatic.empty());
    }

    TEST_F(NbrMgrReinstallTest, IgnoresOtherNeighbors)
    {
        delNeigh(LO_IFINDEX, "10.0.0.9");               /* not configured */
        delNeigh(LO_IFINDEX, "10.0.0.1", AF_BRIDGE);    /* FDB notification */
        EXPECT_TRUE(pending().empty());
        EXPECT_TRUE(m_nbrMgr->m_flushedStatic.empty());
    }

    TEST_F(NbrMgrReinstallTest, DeletedConfigIsNotReinstalled)
    {
        swss::Table neigh(m_config_db.get(), CFG_NEIGH_TABLE_NAME);
        neigh.del("lo|10.0.0.1");
        consumer()->addToSync(std::deque<swss::KeyOpFieldsValuesTuple>{ { "lo|10.0.0.1", DEL_COMMAND, {} } });
        static_cast<Orch *>(m_nbrMgr.get())->doTask();
        ASSERT_TRUE(pending().empty());

        delNeigh(LO_IFINDEX, "10.0.0.1");
        EXPECT_TRUE(pending().empty());
    }

    TEST_F(NbrMgrReinstallTest, FlushedWhileDownIsReinstalledOnUp)
    {
        /* what a flush on a down netdev records (Ethernet4 does not exist here) */
        m_nbrMgr->m_flushedStatic["Ethernet4"].insert("Ethernet4|10.0.0.5");

        newLink("Ethernet8", IFF_UP);   /* another interface */
        newLink("Ethernet4", 0);        /* still down */
        EXPECT_TRUE(pending().empty());

        newLink("Ethernet4", IFF_UP);
        EXPECT_EQ(pending(), (std::set<std::string>{ "Ethernet4|10.0.0.5" }));
        EXPECT_TRUE(m_nbrMgr->m_flushedStatic.empty());
    }

    TEST_F(NbrMgrReinstallTest, DeferredWhenNetdevIsMissingOrDown)
    {
        /* a netdev that does not exist cannot be named: the delete is ignored */
        delNeigh(0, "10.0.0.5");
        EXPECT_TRUE(pending().empty());
        EXPECT_TRUE(m_nbrMgr->m_flushedStatic.empty());
        EXPECT_FALSE(m_nbrMgr->isNetdevUp("Ethernet4"));
        EXPECT_TRUE(m_nbrMgr->isNetdevUp("lo"));
    }
}
