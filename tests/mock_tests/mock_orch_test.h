#define private public
#include "orchagent/directory.h"
#undef private
#define protected public
#include "orchagent/orch.h"
#undef protected
#include "tests/mock_tests/ut_helper.h"
#include "tests/mock_tests/mock_orchagent_main.h"
#include "gtest/gtest.h"
#include <string>


namespace mock_orch_test
{
    static const string PEER_SWITCH_HOSTNAME = "peer_hostname";
    static const string PEER_IPV4_ADDRESS = "1.1.1.1";
    static const string ACTIVE_INTERFACE = "Ethernet4";
    static const string STANDBY_INTERFACE = "Ethernet8";
    static const string ETHERNET0 = "Ethernet0";
    static const string ETHERNET4 = "Ethernet4";
    static const string ETHERNET8 = "Ethernet8";
    static const string ETHERNET12 = "Ethernet12";
    static const string ACTIVE_STATE = "active";
    static const string STANDBY_STATE = "standby";
    static const string STATE = "state";
    static const string VLAN_1000 = "Vlan1000";
    static const string VLAN_2000 = "Vlan2000";
    static const string VLAN_3000 = "Vlan3000";
    static const string VLAN_4000 = "Vlan4000";
    static const string SERVER_IP1 = "192.168.0.2";
    static const string SERVER_IP2 = "192.168.0.3";
    static const string MAC1 = "62:f9:65:10:2f:01";
    static const string MAC2 = "62:f9:65:10:2f:02";
    static const string MAC3 = "62:f9:65:10:2f:03";
    static const string MAC4 = "62:f9:65:10:2f:04";
    static const string MAC5 = "62:f9:65:10:2f:05";

    class MockOrchTest: public ::testing::Test
    {
    protected:
        std::vector<Orch **> ut_orch_list;
        std::set<Orch **> global_orch_list;
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        MuxOrch *m_MuxOrch;
        MuxCableOrch *m_MuxCableOrch;
        MuxCable *m_MuxCable;
        TunnelDecapOrch *m_TunnelDecapOrch;
        MuxStateOrch *m_MuxStateOrch;
        FlexCounterOrch *m_FlexCounterOrch;
        VxlanTunnelOrch *m_VxlanTunnelOrch;
        VNetOrch *m_vnetOrch;
        DashOrch *m_DashOrch;
        DashVnetOrch *m_dashVnetOrch;
        DashRouteOrch *m_DashRouteOrch;
        DashTunnelOrch *m_DashTunnelOrch;

        void PrepareSai();
        void SetUp();
        void TearDown();
        virtual void ApplyInitialConfigs();
        virtual void PostSetUp();
        virtual void PreTearDown();
        virtual void ApplySaiMock();
    };
}
