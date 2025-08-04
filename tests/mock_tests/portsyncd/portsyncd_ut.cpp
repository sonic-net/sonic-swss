#include "gtest/gtest.h"
#include <net/if.h>
#include <netlink/route/link.h>
#include "mock_table.h"
#define private public 
#include "linksync.h"
#undef private

struct if_nameindex *if_ni_mock = NULL;

/* Mock if_nameindex() call */
extern "C" {
    struct if_nameindex *__wrap_if_nameindex()
    {
        return if_ni_mock;
    }
}

/* Mock if_freenameindex() call */
extern "C" {
    void __wrap_if_freenameindex(struct if_nameindex *ptr)
    {
        return ;
    }
}

extern std::string mockCmdStdcout;
extern std::vector<std::string> mockCallArgs;
std::set<std::string> g_portSet;
bool g_init = false;
std::string g_switchType;

void writeToApplDB(swss::ProducerStateTable &p, swss::DBConnector &cfgDb)
{
    swss::Table table(&cfgDb, CFG_PORT_TABLE_NAME);
    std::vector<swss::FieldValueTuple> ovalues;
    std::vector<std::string> keys;
    table.getKeys(keys);

    for ( auto &k : keys )
    {
        table.get(k, ovalues);
        std::vector<swss::FieldValueTuple> attrs;
        for ( auto &v : ovalues )
        {
            swss::FieldValueTuple attr(v.first, v.second);
            attrs.push_back(attr);
        }
        p.set(k, attrs);
        g_portSet.insert(k);
    }
}

/*
Test Fixture 
*/
namespace portsyncd_ut
{
    struct PortSyncdTest : public ::testing::Test
    {   
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::Table> m_portCfgTable;
        std::shared_ptr<swss::Table> m_portAppTable;

        virtual void SetUp() override
        {   
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
            m_portCfgTable = std::make_shared<swss::Table>(m_config_db.get(), CFG_PORT_TABLE_NAME);
            m_portAppTable = std::make_shared<swss::Table>(m_app_db.get(), APP_PORT_TABLE_NAME);
        }

        virtual void TearDown() override {
            if (if_ni_mock != NULL) free(if_ni_mock);
            if_ni_mock = NULL;
        }
    };

    /* Helper Methods */
    void populateCfgDb(swss::Table* tbl){
        /* populate config db with Eth0 and Eth4 objects */
        std::vector<swss::FieldValueTuple> vec;
        vec.emplace_back("admin_status", "down"); 
        vec.emplace_back("index", "2");
        vec.emplace_back("lanes", "4,5,6,7");
        vec.emplace_back("mtu", "9100");
        vec.emplace_back("speed", "10000");
        vec.emplace_back("alias", "etp1");
        tbl->set("Ethernet0", vec);
        vec.pop_back();
        vec.emplace_back("alias", "etp1");
        tbl->set("Ethernet4", vec);
    }

    /* Create internal ds holding netdev ifaces for eth0 & lo */
    inline struct if_nameindex * populateNetDev(){
        struct if_nameindex *if_ni_temp;
        /* Construct a mock if_nameindex array */
        if_ni_temp = (struct if_nameindex*) calloc(3, sizeof(struct if_nameindex));

        if_ni_temp[2].if_index = 0;
        if_ni_temp[2].if_name = NULL;

        if_ni_temp[1].if_index = 16222;
        if_ni_temp[1].if_name = "eth0";

        if_ni_temp[0].if_index = 1;
        if_ni_temp[0].if_name = "lo";

        return if_ni_temp;
    }

    /* Create internal ds holding netdev ifaces for lo & Ethernet0 */
    inline struct if_nameindex * populateNetDevAdvanced(){
        struct if_nameindex *if_ni_temp;
        /* Construct a mock if_nameindex array */
        if_ni_temp = (struct if_nameindex*) calloc(3, sizeof(struct if_nameindex));

        if_ni_temp[2].if_index = 0;
        if_ni_temp[2].if_name = NULL;

        if_ni_temp[1].if_index = 142;
        if_ni_temp[1].if_name = "Ethernet0";

        if_ni_temp[0].if_index = 1;
        if_ni_temp[0].if_name = "lo";

        return if_ni_temp;
    }

    /* Draft a rtnl_link msg */
    struct nl_object* draft_nlmsg(const std::string& name,
                                 std::vector<unsigned int> flags,
                                 const std::string& type,
                                 const std::string& ll_add,
                                 int ifindex,
                                 unsigned int mtu,
                                 int master_ifindex = 0){
                                
        struct rtnl_link* nl_obj =  rtnl_link_alloc();
        if (!nl_obj){
            throw std::runtime_error("netlink: rtnl_link object allocation failed");
        }
        /* Set name for rtnl link object */
        rtnl_link_set_name(nl_obj, name.c_str());

        /* Set flags */
        for (auto nlflag : flags){
            rtnl_link_set_flags(nl_obj, nlflag);
        }

        /* Set type */
        if (!type.empty()){
            rtnl_link_set_type(nl_obj, type.c_str());
        }
        
        /* Set Link layer Address */
        struct nl_addr * ll_addr;
        int result = nl_addr_parse(ll_add.c_str(), AF_LLC, &ll_addr);
        if (result < 0){
            throw std::runtime_error("netlink: Link layer address allocation failed");
        }
        rtnl_link_set_addr(nl_obj, ll_addr);

        /* Set ifindex */
        rtnl_link_set_ifindex(nl_obj, ifindex);

        /* Set mtu */
        rtnl_link_set_mtu(nl_obj, mtu);

        /* Set master_ifindex if any */
        if (master_ifindex){
            rtnl_link_set_master(nl_obj, master_ifindex);
        }

        return (struct nl_object*)nl_obj;
    }

    inline void free_nlobj(struct nl_object* msg){
         nl_object_free(msg);
    }
}

namespace portsyncd_ut
{
    TEST_F(PortSyncdTest, test_cacheOldIfaces)
    {  
        if_ni_mock = populateNetDevAdvanced();
        swss::LinkSync sync(m_app_db.get(), m_state_db.get());
        ASSERT_EQ(mockCallArgs.back(), "ip link set \"Ethernet0\" down");
        ASSERT_NE(sync.m_ifindexOldNameMap.find(142), sync.m_ifindexOldNameMap.end());
        ASSERT_EQ(sync.m_ifindexOldNameMap[142], "Ethernet0");
    }

    TEST_F(PortSyncdTest, test_onMsgNewLink)
    {
        swss::LinkSync sync(m_app_db.get(), m_state_db.get());
        /* Write config to Config DB */
        populateCfgDb(m_portCfgTable.get());
        swss::DBConnector cfg_db_conn("CONFIG_DB", 0);

        /* Handle CFG DB notifs and Write them to APPL_DB */
        swss::ProducerStateTable p(m_app_db.get(), APP_PORT_TABLE_NAME);
        writeToApplDB(p, cfg_db_conn);

        /* Generate a netlink notification about the netdev iface */
        std::vector<unsigned int> flags = {IFF_UP, IFF_RUNNING};
        struct nl_object* msg = draft_nlmsg("Ethernet0",
                                            flags,
                                            "sx_netdev",
                                            "1c:34:da:1c:9f:00",
                                            142,
                                            9100,
                                            0);
        sync.onMsg(RTM_NEWLINK, msg);

        /* Verify if the update has been written to State DB */
        std::vector<swss::FieldValueTuple> ovalues;
        ASSERT_EQ(sync.m_statePortTable.get("Ethernet0", ovalues), true);
        for (auto value : ovalues){
            if (fvField(value) == "state") {ASSERT_EQ(fvValue(value), "ok");}
            if (fvField(value) == "mtu") {ASSERT_EQ(fvValue(value), "9100");}
            if (fvField(value) == "netdev_oper_status") {ASSERT_EQ(fvValue(value), "up");}
            if (fvField(value) == "admin_status") {ASSERT_EQ(fvValue(value), "up");}
            if (fvField(value) == "speed") {ASSERT_EQ(fvValue(value), "10000");}
        }

        /* Verify if the internal strctures are updated as expected */
        ASSERT_NE(sync.m_ifindexNameMap.find(142), sync.m_ifindexNameMap.end());
        ASSERT_EQ(sync.m_ifindexNameMap[142], "Ethernet0");

        /* Free Nl_object */
        free_nlobj(msg);
    }

    TEST_F(PortSyncdTest, test_onMsgDelLink){

        swss::LinkSync sync(m_app_db.get(), m_state_db.get());

        /* Write config to Config DB */
        populateCfgDb(m_portCfgTable.get());
        swss::DBConnector cfg_db_conn("CONFIG_DB", 0);

        /* Handle CFG DB notifs and Write them to APPL_DB */
        swss::ProducerStateTable p(m_app_db.get(), APP_PORT_TABLE_NAME);
        writeToApplDB(p, cfg_db_conn);;

        /* Generate a netlink notification about the netdev iface */
        std::vector<unsigned int> flags = {IFF_UP, IFF_RUNNING};
        struct nl_object* msg = draft_nlmsg("Ethernet0",
                                            flags,
                                            "sx_netdev",
                                            "1c:34:da:1c:9f:00",
                                            142,
                                            9100,
                                            0);
        sync.onMsg(RTM_NEWLINK, msg);

        /* Verify if the update has been written to State DB */
        std::vector<swss::FieldValueTuple> ovalues;
        ASSERT_EQ(sync.m_statePortTable.get("Ethernet0", ovalues), true);

        /* Free Nl_object */
        free_nlobj(msg);

        /* Generate a DELLINK Notif */
        msg = draft_nlmsg("Ethernet0",
                           flags,
                           "sx_netdev",
                           "1c:34:da:1c:9f:00",
                           142,
                           9100,
                           0);

        sync.onMsg(RTM_DELLINK, msg);
        ovalues.clear();

        /* Verify if the state_db entry is cleared */
        ASSERT_EQ(sync.m_statePortTable.get("Ethernet0", ovalues), false);
    }

    TEST_F(PortSyncdTest, test_onMsgIgnoreOldNetDev){
        if_ni_mock = populateNetDevAdvanced();
        swss::LinkSync sync(m_app_db.get(), m_state_db.get());
        ASSERT_EQ(mockCallArgs.back(), "ip link set \"Ethernet0\" down");
        ASSERT_NE(sync.m_ifindexOldNameMap.find(142), sync.m_ifindexOldNameMap.end());
        ASSERT_EQ(sync.m_ifindexOldNameMap[142], "Ethernet0");

        /* Generate a netlink notification about the netdev iface */
        std::vector<unsigned int> flags;
        struct nl_object* msg = draft_nlmsg("Ethernet0",
                                            flags,
                                            "sx_netdev",
                                            "1c:34:da:1c:9f:00",
                                            142,
                                            9100,
                                            0);
        sync.onMsg(RTM_NEWLINK, msg);

        /* Verify if nothing is written to state_db */
        std::vector<swss::FieldValueTuple> ovalues;
        ASSERT_EQ(sync.m_statePortTable.get("Ethernet0", ovalues), false);
    }
}
