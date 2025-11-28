#include "mock_orch_test.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "mock_table.h"
#define protected public
#define private public
#include "dash/dashenifwdorch.h"
#undef public
#undef protected

using namespace ::testing;

namespace dashenifwdorch_ut 
{
       /* Mock API Calls to other orchagents */
       class MockEniFwdCtx : public EniFwdCtxBase {
       public:
              using EniFwdCtxBase::EniFwdCtxBase;

              void initialize() override {}
              MOCK_METHOD(std::string, getRouterIntfsAlias, (const swss::IpAddress&, const string& vrf), (override));
              MOCK_METHOD(bool, isNeighborResolved, (const NextHopKey&), (override));
              MOCK_METHOD(void, resolveNeighbor, (const NextHopKey&), (override));
              MOCK_METHOD(bool, findVnetVni, (const std::string&, uint64_t&), (override));
              MOCK_METHOD(bool, findVnetTunnel, (const std::string&, std::string&), (override));
              MOCK_METHOD((std::map<std::string, Port>&), getAllPorts, (), (override));
       };

       class DashEniFwdOrchTest : public Test
       {
       public:
              unique_ptr<DBConnector> cfgDb;
              unique_ptr<DBConnector> applDb;
              unique_ptr<DBConnector> chassisApplDb;
              unique_ptr<Table> dpuTable;
              unique_ptr<Table> remoteDpuTable;
              unique_ptr<Table> vdpuTable;

              unique_ptr<Table> eniFwdTable;
              unique_ptr<Table> aclRuleTable;
              unique_ptr<DashEniFwdOrch> eniOrch;
              shared_ptr<MockEniFwdCtx> ctx;

              /* Test values */
              string alias_dpu = "Vlan1000";
              string test_vip = "10.2.0.1/32";
              string vnet_name = "Vnet_1000";
              string tunnel_name = "mock_tunnel";
              string test_mac = "aa:bb:cc:dd:ee:ff";
              string test_mac2 = "ff:ee:dd:cc:bb:aa";
              string test_mac_key = "AABBCCDDEEFF";
              string test_mac2_key = "FFEEDDCCBBAA";
              string local_pav4 = "10.0.0.1";
              string remote_pav4 = "10.0.0.2";
              string remote_2_pav4 = "10.0.0.3";
              string local_npuv4 = "20.0.0.1";
              string remote_npuv4 = "20.0.0.2";
              string remote_2_npuv4 = "20.0.0.3";

              std::map<std::string, Port> allPorts;
              uint64_t test_vni = 1000;
              int BASE_PRIORITY = 9996;

              void populateDpuTable()
              {
                     /* Add 1 local and 1 cluster DPU */
                     dpuTable->set("local_dpu", 
                     {
                            { DashEniFwd::PA_V4, local_pav4 },
                            { DashEniFwd::STATE, "up" },
                            { "gnmi_port", "50051" },
                            { "local_port", "8080" },
                     }, SET_COMMAND);

                     dpuTable->set("local_down_dpu",
                     {
                            { DashEniFwd::PA_V4, local_pav4 },
                            { DashEniFwd::STATE, "down" },
                     }, SET_COMMAND);

                     remoteDpuTable->set("remote_dpu", 
                     {
                            { DashEniFwd::PA_V4, remote_pav4 },
                            { DashEniFwd::NPU_V4, remote_npuv4 },
                     }, SET_COMMAND);

                     remoteDpuTable->set("remote_dpu2", 
                     {
                            { DashEniFwd::PA_V4, remote_2_pav4 },
                            { DashEniFwd::NPU_V4, remote_2_npuv4 },
                     }, SET_COMMAND);

                     vdpuTable->set("vdpu0", 
                     {
                            { DashEniFwd::DPU_IDS, "local_dpu" },
                     }, SET_COMMAND);

                     vdpuTable->set("vdpu1", 
                     {
                            { DashEniFwd::DPU_IDS, "remote_dpu" },
                     }, SET_COMMAND);

                     vdpuTable->set("vdpu2", 
                     {
                            { DashEniFwd::DPU_IDS, "remote_dpu2" },
                     }, SET_COMMAND);

                     vdpuTable->set("vdpu3", 
                     {
                            { DashEniFwd::DPU_IDS, "invalid_dpu" },
                     }, SET_COMMAND);

                     vdpuTable->set("vdpu4", 
                     {
                            { DashEniFwd::DPU_IDS, "local_down_dpu" },
                     }, SET_COMMAND);
              }

              void populateVip()
              {
                     Table vipTable(cfgDb.get(), DashEniFwd::VIP_TABLE);
                     vipTable.set(test_vip, {{}});
              }

              void doDashEniFwdTableTask(DBConnector* applDb, const deque<KeyOpFieldsValuesTuple> &entries)
              {
                     auto consumer = unique_ptr<Consumer>(new Consumer(
                            new swss::ConsumerStateTable(applDb, APP_DASH_ENI_FORWARD_TABLE, 1, 1), 
                            eniOrch.get(), APP_DASH_ENI_FORWARD_TABLE));

                     consumer->addToSync(entries);
                     eniOrch->doTask(*consumer);
              }

              void checkKFV(Table* m_table, const std::string& key, const std::vector<FieldValueTuple>& expectedValues) {
                     std::string val;
                     for (const auto& fv : expectedValues) {
                            const std::string& field = fvField(fv);
                            const std::string& expectedVal = fvValue(fv);
                            EXPECT_TRUE(m_table->hget(key, field, val))
                            << "Failed to retrieve field " << field << " from key " << key;
                            EXPECT_EQ(val, expectedVal)
                            << "Mismatch for field " << field << " for key " << key 
                            << ": expected " << expectedVal << ", got " << val;
                     }
              }

              void checkRuleUninstalled(string key)
              {
                     std::string val;
                     EXPECT_FALSE(aclRuleTable->hget(key, MATCH_DST_IP, val))
                     << key << ": Still Exist";
              }

              void checkNoKeyExists(Table* m_table, string expected_key)
              {
                     std::string val;
                     std::vector<std::string> keys;
                     m_table->getKeys(keys);
                     for (auto& key : keys) {
                            if (key == expected_key)
                            {
                                   EXPECT_FALSE(true) << expected_key << ": Still Exist";
                            }
                     }
              }

              void SetUp() override {  
                     testing_db::reset();                   
                     cfgDb = make_unique<DBConnector>("CONFIG_DB", 0);
                     applDb = make_unique<DBConnector>("APPL_DB", 0);
                     chassisApplDb = make_unique<DBConnector>("CHASSIS_APP_DB", 0);
                     /* Initialize tables */
                     dpuTable = make_unique<Table>(cfgDb.get(), DashEniFwd::DPU_TABLE);
                     remoteDpuTable = make_unique<Table>(cfgDb.get(), DashEniFwd::REMOTE_DPU_TABLE);
                     vdpuTable = make_unique<Table>(cfgDb.get(), DashEniFwd::VDPU_TABLE);
                     
                     eniFwdTable = make_unique<Table>(applDb.get(), APP_DASH_ENI_FORWARD_TABLE);
                     aclRuleTable = make_unique<Table>(applDb.get(), APP_ACL_RULE_TABLE_NAME);
                     /* Populate DPU Configuration */
                     populateDpuTable();
                     populateVip();
                     eniOrch = make_unique<DashEniFwdOrch>(cfgDb.get(), applDb.get(), APP_DASH_ENI_FORWARD_TABLE, nullptr);

                     /* Clear the default context and Patch with the Mock */
                     ctx = make_shared<MockEniFwdCtx>(cfgDb.get(), applDb.get());
                     /* Create a set of ports */
                     allPorts["Ethernet0"] = Port("Ethernet0", Port::PHY);
                     allPorts["Ethernet4"] = Port("Ethernet4", Port::PHY);
                     allPorts["Ethernet8"] = Port("Ethernet8", Port::PHY);
                     allPorts["Ethernet16"] = Port("Ethernet16", Port::PHY);
                     allPorts["PortChannel1011"] = Port("PortChannel1012", Port::LAG);
                     allPorts["PortChannel1012"] = Port("Ethernet16", Port::LAG);
                     allPorts["PortChannel1011"].m_members.insert("Ethernet8");
                     allPorts["PortChannel1012"].m_members.insert("Ethernet16");
                     ON_CALL(*ctx, getAllPorts()).WillByDefault(ReturnRef(allPorts));

                     eniOrch->ctx.reset();
                     eniOrch->ctx = ctx;
                     eniOrch->ctx->populateDpuRegistry();
                     eniOrch->ctx_initialized_ = true;
              }
       };

       /*
              Test getting the PA, NPU address of a DPU and dpuType
       */
       TEST_F(DashEniFwdOrchTest, TestDpuRegistry) 
       {
              dpu_type_t type;
              swss::IpAddress pa_v4;
              swss::IpAddress npu_v4;
              
              EniFwdCtx ctx(cfgDb.get(), applDb.get());
              ctx.populateDpuRegistry();

              EXPECT_TRUE(ctx.dpu_info.getType("vdpu0", type));
              EXPECT_EQ(type, dpu_type_t::LOCAL);
              EXPECT_TRUE(ctx.dpu_info.getPaV4("vdpu0", pa_v4));
              EXPECT_EQ(pa_v4.to_string(), local_pav4);
              
              EXPECT_TRUE(ctx.dpu_info.getType("vdpu1", type));
              EXPECT_EQ(type, dpu_type_t::CLUSTER);
              EXPECT_TRUE(ctx.dpu_info.getPaV4("vdpu1", pa_v4));
              EXPECT_EQ(pa_v4.to_string(), remote_pav4);
              EXPECT_TRUE(ctx.dpu_info.getNpuV4("vdpu1", npu_v4));
              EXPECT_EQ(npu_v4.to_string(), remote_npuv4);
       
              EXPECT_TRUE(ctx.dpu_info.getNpuV4("vdpu2", npu_v4));
              EXPECT_EQ(npu_v4.to_string(), remote_2_npuv4);
              
              /* Invalid DPU */
              EXPECT_FALSE(ctx.dpu_info.getNpuV4("vdpu3", npu_v4));
              EXPECT_FALSE(ctx.dpu_info.getType("vdpu3", type));
              EXPECT_FALSE(ctx.dpu_info.getPaV4("vdpu3", pa_v4));
              
              /* Down DPU */
              EXPECT_FALSE(ctx.dpu_info.getNpuV4("vdpu4", npu_v4));
              EXPECT_FALSE(ctx.dpu_info.getType("vdpu4", type));
              EXPECT_FALSE(ctx.dpu_info.getPaV4("vdpu4", pa_v4));

              vector<std::string> exp_ids = {"vdpu0", "vdpu1", "vdpu2"};
              auto ids = ctx.dpu_info.getIds();
              std::sort(ids.begin(), ids.end());
              EXPECT_EQ(ids, exp_ids);
       }

       /* 
              VNI is provided by HaMgrd, Resolve Neighbor
       */
       TEST_F(DashEniFwdOrchTest, LocalNeighbor) 
       {
              auto nh_ip = swss::IpAddress(local_pav4);
              NextHopKey nh = {nh_ip, alias_dpu};
              /* Mock calls to intfsOrch and neighOrch
                 If neighbor is already resolved, resolveNeighbor is not called  */
              EXPECT_CALL(*ctx, getRouterIntfsAlias(nh_ip, _)).WillOnce(Return(alias_dpu)); /* Once per local endpoint */
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(2).WillRepeatedly(Return(true));
              EXPECT_CALL(*ctx, resolveNeighbor(nh)).Times(0);

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu0" }, // Local endpoint is the primary
                                       } 
                                   }
                            }
                     )
              );

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key, {
                            { ACTION_REDIRECT_ACTION , local_pav4 }, { MATCH_DST_IP, test_vip }, 
                            { RULE_PRIORITY, to_string(BASE_PRIORITY) },
                            { MATCH_INNER_DST_MAC, test_mac }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + rule_type_t::TUNNEL_TERM) },
                            { MATCH_INNER_DST_MAC, test_mac },
                            { MATCH_TUNNEL_TERM, "true"}
              });
       }

       /*
              VNI is provided by HaMgrd, UnResolved Neighbor
       */
       TEST_F(DashEniFwdOrchTest, LocalNeighbor_Unresolved) 
       {
              auto nh_ip = swss::IpAddress(local_pav4);
              NextHopKey nh = {nh_ip, alias_dpu};
              /* 1 for initLocalEndpoints */
              EXPECT_CALL(*ctx, getRouterIntfsAlias(nh_ip, _)).WillOnce(Return(alias_dpu));

              /* Neighbor is not resolved, 1 per rule + 1 for initLocalEndpoints */
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(5).WillRepeatedly(Return(false));
              /* resolveNeighbor is called because the neigh is not resolved */
              EXPECT_CALL(*ctx, resolveNeighbor(nh)).Times(5); /* 1 per rule + 1 for initLocalEndpoints */

              eniOrch->initLocalEndpoints();

              /* Populate 2 ENI's */
              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu0" }, // Local endpoint is the primary
                                       } 
                                   },
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu0" }, // Local endpoint is the primary
                                       }
                                   }
                            }
                     )
              );

              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key);
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_TERM");
       
              /* Neighbor is resolved, Trigger a nexthop update (1 for Neigh Update) * 4 for Types of Rules */
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(4).WillRepeatedly(Return(true));

              NeighborEntry temp_entry = nh;
              NeighborUpdate update = { temp_entry, MacAddress(), true };
              eniOrch->update(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key, {
                            { ACTION_REDIRECT_ACTION, local_pav4 }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_TUNNEL_TERM, "true"}
              });
       }

       /* 
              Remote Endpoint
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor)
       {
              EXPECT_CALL(*ctx, getRouterIntfsAlias(_, _)).WillOnce(Return(alias_dpu));
              /* calls to neighOrch expected for tunn termination entries */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(2).WillRepeatedly(Return(true));

              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(2) // Once per non-tunnel term rules
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));

              EXPECT_CALL(*ctx, findVnetVni(vnet_name, _)).Times(2) // Called once per ENI
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(test_vni),
                     Return(true)
              ));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu1" }, // Remote endpoint is the primary
                                       } 
                                   },
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu1" }, // Remote endpoint is the primary
                                       } 
                                   }
                            }
                     )
              );

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key, {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name + "," + to_string(test_vni) }
              });

              /* Delete all ENI's */
              doDashEniFwdTableTask(applDb.get(),
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac2,
                                       DEL_COMMAND,
                                       { }
                                   },
                                   {
                                       vnet_name + ":" + test_mac,
                                       DEL_COMMAND,
                                       { } 
                                   }
                            }
                     )
              );
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key );
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key);
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_TERM");
       }

       /* 
              Remote Endpoint with an update to switch to Local Endpoint
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor_SwitchToLocal)
       {
              EXPECT_CALL(*ctx, getRouterIntfsAlias(_, _)).WillOnce(Return(alias_dpu));
              /* 1 calls made for tunnel termination rules */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(1).WillRepeatedly(Return(true));
              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(1) // Once per non-tunnel term rules
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));
              EXPECT_CALL(*ctx, findVnetVni(vnet_name, _)).Times(1) // Called once per ENI
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(test_vni),
                     Return(true)
              ));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu0,vdpu1" },
                                          { DashEniFwd::PRIMARY, "vdpu1" }, // Remote endpoint is the primary
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key, {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name + ',' + to_string(test_vni) }
              });

              /* 1 calls will be made for non tunnel termination rules after primary switch */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(1).WillRepeatedly(Return(true));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::PRIMARY, "vdpu0" }, // Primary is Local now
                                       } 
                                   }
                            }
                     )
              );
       }

       /* 
              T1 doesn't host the ENI, Both the enndpoints are Remote. 
              No Tunnel Termination Rules expected 
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor_NoTunnelTerm)
       {
              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(1) // Only 1 rule is created
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));
              EXPECT_CALL(*ctx, findVnetVni(vnet_name, _)).Times(1) // Called once per ENI
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(test_vni),
                     Return(true)
              ));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { DashEniFwd::VDPU_IDS, "vdpu1,vdpu2" },
                                          { DashEniFwd::PRIMARY, "vdpu2" }, // Remote endpoint is the primary
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key, {
                            { ACTION_REDIRECT_ACTION, remote_2_npuv4 + "@" + tunnel_name + ',' + to_string(test_vni) }
              });

              /* Tunnel termination rules are not installed */
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_TERM");
       }

       /* 
              Test ACL Table and Table Type config with reference counting
       */
       TEST_F(DashEniFwdOrchTest, TestAclTableConfig)
       {
              Table aclTableType(applDb.get(), APP_ACL_TABLE_TYPE_TABLE_NAME);
              Table aclTable(applDb.get(), APP_ACL_TABLE_TABLE_NAME);
              Table portTable(cfgDb.get(), CFG_PORT_TABLE_NAME);

              portTable.set("Ethernet0",
              {
                     { "lanes", "0,1,2,3" }
              }, SET_COMMAND);

              portTable.set("Ethernet4",
              {
                     { "lanes", "4,5,6,7" },
                     { PORT_ROLE, PORT_ROLE_DPC }
              }, SET_COMMAND);

              // Initially no ACL table should exist
              checkNoKeyExists(&aclTable, "ENI");
              checkNoKeyExists(&aclTableType, "ENI_REDIRECT");

              // Create first ACL rule - should create the table
              vector<FieldValueTuple> fv1 = {
                     { RULE_PRIORITY, "9996" },
                     { MATCH_DST_IP, test_vip },
                     { MATCH_INNER_DST_MAC, test_mac },
                     { ACTION_REDIRECT_ACTION, local_pav4 }
              };
              eniOrch->ctx->createAclRule("ENI:rule1", fv1);

              // Verify ACL table and table type were created after first rule
              checkKFV(&aclTableType, "ENI_REDIRECT", {
                     { ACL_TABLE_TYPE_MATCHES, "DST_IP,INNER_DST_MAC,TUNNEL_TERM" },
                     { ACL_TABLE_TYPE_ACTIONS, "REDIRECT_ACTION" },
                     { ACL_TABLE_TYPE_BPOINT_TYPES, "PORT,PORTCHANNEL" }
              });

              checkKFV(&aclTable, "ENI", {
                     { ACL_TABLE_TYPE, "ENI_REDIRECT" },
                     { ACL_TABLE_STAGE, STAGE_INGRESS },
                     { ACL_TABLE_PORTS, "Ethernet0,PortChannel1011,PortChannel1012" }
              });

              // Create second and third ACL rules - table should still exist
              vector<FieldValueTuple> fv2 = {
                     { RULE_PRIORITY, "9997" },
                     { MATCH_DST_IP, test_vip },
                     { MATCH_INNER_DST_MAC, test_mac2 },
                     { ACTION_REDIRECT_ACTION, local_pav4 }
              };
              eniOrch->ctx->createAclRule("ENI:rule2", fv2);

              vector<FieldValueTuple> fv3 = {
                     { RULE_PRIORITY, "9998" },
                     { MATCH_DST_IP, test_vip },
                     { MATCH_INNER_DST_MAC, test_mac },
                     { ACTION_REDIRECT_ACTION, remote_pav4 }
              };
              eniOrch->ctx->createAclRule("ENI:rule3", fv3);

              // Verify rule count is 3
              EXPECT_EQ(eniOrch->ctx->acl_rule_count_, 3);

              // Delete first two rules - table should still exist
              eniOrch->ctx->deleteAclRule("ENI:rule1");
              EXPECT_EQ(eniOrch->ctx->acl_rule_count_, 2);

              eniOrch->ctx->deleteAclRule("ENI:rule2");
              EXPECT_EQ(eniOrch->ctx->acl_rule_count_, 1);

              // Table should still exist
              checkKFV(&aclTable, "ENI", {
                     { ACL_TABLE_TYPE, "ENI_REDIRECT" }
              });

              // Delete last rule - table should be removed
              eniOrch->ctx->deleteAclRule("ENI:rule3");
              EXPECT_EQ(eniOrch->ctx->acl_rule_count_, 0);

              // Verify ACL table and table type were deleted after last rule
              checkNoKeyExists(&aclTable, "ENI");
              checkNoKeyExists(&aclTableType, "ENI_REDIRECT");
       }
}

namespace mock_orch_test
{
       TEST_F(MockOrchTest, EniFwdCtx)
       {
              EniFwdCtx ctx(m_config_db.get(), m_app_db.get());
              ASSERT_NO_THROW(ctx.initialize());

              NextHopKey nh(IpAddress("10.0.0.1"), "Ethernet0");
              ASSERT_NO_THROW(ctx.isNeighborResolved(nh));
              ASSERT_NO_THROW(ctx.resolveNeighbor(nh));
              ASSERT_NO_THROW(ctx.getRouterIntfsAlias(IpAddress("10.0.0.1")));

              uint64_t vni;
              ASSERT_NO_THROW(ctx.findVnetVni("Vnet_1000", vni));
              string tunnel;
              ASSERT_NO_THROW(ctx.findVnetTunnel("Vnet_1000", tunnel));
              ASSERT_NO_THROW(ctx.getAllPorts());
       }
}
