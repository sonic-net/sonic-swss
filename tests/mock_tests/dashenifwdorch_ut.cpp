#include "tests/mock_tests/mock_orch_test.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "tests/mock_tests/mock_table.h"
#define protected public
#define private public
#include "orchagent/dash/dashenifwdorch.h"
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
              MOCK_METHOD(sai_object_id_t, createNextHopTunnel, (std::string, IpAddress), (override));
              MOCK_METHOD(bool, removeNextHopTunnel, (std::string, IpAddress), (override));
              MOCK_METHOD((std::map<std::string, Port>&), getAllPorts, (), (override));
       };

       class DashEniFwdOrchTest : public Test
       {
       public:
              unique_ptr<DBConnector> cfgDb;
              unique_ptr<DBConnector> applDb;
              unique_ptr<DBConnector> chassisApplDb;
              unique_ptr<Table> dpuTable;
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

              uint64_t test_vni = 1234;
              uint64_t test_vni2 = 5678;
              int BASE_PRIORITY = 9996;

              void populateDpuTable()
              {
                     /* Add 1 local and 1 cluster DPU */
                     dpuTable->set("1", 
                     {
                            { DPU_TYPE, "local" },
                            { DPU_PA_V4, local_pav4 },
                            { DPU_NPU_V4, local_npuv4 },
                     }, SET_COMMAND);

                     dpuTable->set("2", 
                     {
                            { DPU_TYPE, "cluster" },
                            { DPU_PA_V4, remote_pav4 },
                            { DPU_NPU_V4, remote_npuv4 },
                     }, SET_COMMAND);

                     dpuTable->set("3", 
                     {
                            { DPU_TYPE, "cluster" },
                            { DPU_PA_V4, remote_2_pav4 },
                            { DPU_NPU_V4, remote_2_npuv4 },
                     }, SET_COMMAND);
              }

              void populateVip()
              {
                     Table vipTable(cfgDb.get(), CFG_VIP_TABLE_TMP);
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

              void SetUp() override {  
                     testing_db::reset();                   
                     cfgDb = make_unique<DBConnector>("CONFIG_DB", 0);
                     applDb = make_unique<DBConnector>("APPL_DB", 0);
                     chassisApplDb = make_unique<DBConnector>("CHASSIS_APP_DB", 0);
                     /* Initialize tables */
                     dpuTable = make_unique<Table>(cfgDb.get(), CFG_DPU_TABLE);
                     eniFwdTable = make_unique<Table>(applDb.get(), APP_DASH_ENI_FORWARD_TABLE);
                     aclRuleTable = make_unique<Table>(applDb.get(), APP_ACL_RULE_TABLE_NAME);
                     /* Populate DPU Configuration */
                     populateDpuTable();
                     populateVip();
                     eniOrch = make_unique<DashEniFwdOrch>(cfgDb.get(), applDb.get(), APP_DASH_ENI_FORWARD_TABLE, nullptr);

                     /* Clear the default context and Patch with the Mock */
                     ctx = make_shared<MockEniFwdCtx>(cfgDb.get(), applDb.get());
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
              dpu_type_t type = dpu_type_t::EXTERNAL;
              swss::IpAddress pa_v4;
              swss::IpAddress npu_v4;
              
              EniFwdCtx ctx(cfgDb.get(), applDb.get());
              ctx.populateDpuRegistry();

              EXPECT_TRUE(ctx.dpu_info.getType(1, type));
              EXPECT_EQ(type, dpu_type_t::LOCAL);
              EXPECT_TRUE(ctx.dpu_info.getType(2, type));
              EXPECT_EQ(type, dpu_type_t::CLUSTER);

              EXPECT_TRUE(ctx.dpu_info.getPaV4(1, pa_v4));
              EXPECT_EQ(pa_v4.to_string(), local_pav4);
              EXPECT_TRUE(ctx.dpu_info.getPaV4(2, pa_v4));
              EXPECT_EQ(pa_v4.to_string(), remote_pav4);

              EXPECT_TRUE(ctx.dpu_info.getNpuV4(1, npu_v4));
              EXPECT_EQ(npu_v4.to_string(), local_npuv4);
              EXPECT_TRUE(ctx.dpu_info.getNpuV4(2, npu_v4));
              EXPECT_EQ(npu_v4.to_string(), remote_npuv4);
              
              vector<uint64_t> ids = {1, 2, 3};
              EXPECT_EQ(ctx.dpu_info.getIds(), ids);
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
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(4).WillRepeatedly(Return(true));
              EXPECT_CALL(*ctx, resolveNeighbor(nh)).Times(0);

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "1" }, // Local endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni) }
                                       } 
                                   }
                            }
                     )
              );

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN", {
                            { ACTION_REDIRECT_ACTION , local_pav4 }, { MATCH_DST_IP, test_vip }, 
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + INBOUND) },
                            { MATCH_INNER_DST_MAC, test_mac }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + OUTBOUND) },
                            { MATCH_INNER_SRC_MAC, test_mac }, { MATCH_TUNNEL_VNI, to_string(test_vni) }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + INBOUND_TERM) },
                            { MATCH_INNER_DST_MAC, test_mac },
                            { MATCH_TUNNEL_TERM, "true"}
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + OUTBOUND_TERM) },
                            { MATCH_INNER_SRC_MAC, test_mac }, { MATCH_TUNNEL_VNI, to_string(test_vni) },
                            { MATCH_TUNNEL_TERM, "true"}
              });
       }

       /* 
              Infer VNI by reading from the VnetOrch, resolved Neighbor
       */
       TEST_F(DashEniFwdOrchTest, LocalNeighbor_NoVNI)
       {
              auto nh_ip = swss::IpAddress(local_pav4);
              NextHopKey nh = {nh_ip, alias_dpu};

              EXPECT_CALL(*ctx, findVnetVni(vnet_name, _)).Times(1) // Called once per ENI
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(test_vni2),
                     Return(true)
              ));

              EXPECT_CALL(*ctx, getRouterIntfsAlias(nh_ip, _)).WillOnce(Return(alias_dpu));
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(4).WillRepeatedly(Return(true));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "1" }, // Local endpoint is the primary
                                          // No Explicit VNI from the DB, should be inferred from the VNetOrch
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + OUTBOUND) },
                            { MATCH_INNER_SRC_MAC, test_mac2 }, { MATCH_TUNNEL_VNI, to_string(test_vni2) }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_DST_IP, test_vip },
                            { RULE_PRIORITY, to_string(BASE_PRIORITY + OUTBOUND_TERM) },
                            { MATCH_INNER_SRC_MAC, test_mac2 }, { MATCH_TUNNEL_VNI, to_string(test_vni2) },
                            { MATCH_TUNNEL_TERM, "true"}
              });
       }

       /* 
              Verify MAC direction
       */
       TEST_F(DashEniFwdOrchTest, LocalNeighbor_MacDirection)
       {
              auto nh_ip = swss::IpAddress(local_pav4);
              NextHopKey nh = {nh_ip, alias_dpu};

              EXPECT_CALL(*ctx, getRouterIntfsAlias(nh_ip, _)).WillOnce(Return(alias_dpu));
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(4).WillRepeatedly(Return(true));

              doDashEniFwdTableTask(applDb.get(),
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "1" }, // Local endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni2) },
                                          { ENI_FWD_OUT_MAC_LOOKUP, "dst" }
                                       }
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT", {
                            { MATCH_INNER_DST_MAC, test_mac2 },
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT_TERM", {
                            { MATCH_INNER_DST_MAC, test_mac2 }, { MATCH_TUNNEL_TERM, "true"}
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
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(9).WillRepeatedly(Return(false));
              /* resolveNeighbor is called because the neigh is not resolved */
              EXPECT_CALL(*ctx, resolveNeighbor(nh)).Times(9); /* 1 per rule + 1 for initLocalEndpoints */

              eniOrch->initLocalEndpoints();

              /* Populate 2 ENI's */
              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "1" }, // Local endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni) }
                                       } 
                                   },
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "1" }, // Local endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni2) }
                                       }
                                   }
                            }
                     )
              );

              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_IN");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_IN_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_OUT");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_OUT_TERM");

              /* Neighbor is resolved, Trigger a nexthop update (1 for Neigh Update) * 4 for Types of Rules */
              EXPECT_CALL(*ctx, isNeighborResolved(nh)).Times(8).WillRepeatedly(Return(true));

              NeighborEntry temp_entry = nh;
              NeighborUpdate update = { temp_entry, MacAddress(), true };
              eniOrch->update(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_TUNNEL_TERM, "true"}
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT", {
                            { ACTION_REDIRECT_ACTION, local_pav4 },
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }, { MATCH_TUNNEL_TERM, "true"},
                            { MATCH_INNER_SRC_MAC, test_mac2 },
              });
       }

       /* 
              Remote Endpoint
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor)
       {      
              IpAddress remote_npuv4_ip = IpAddress(remote_npuv4);
              EXPECT_CALL(*ctx, getRouterIntfsAlias(_, _)).WillOnce(Return(alias_dpu));
              /* calls to neighOrch expected for tunn termination entries */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(4).WillRepeatedly(Return(true));

              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(4) // Once per non-tunnel term rules
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));

              EXPECT_CALL(*ctx, createNextHopTunnel(tunnel_name, remote_npuv4_ip)).Times(1)
                     .WillRepeatedly(Return(0x400000000064d)); // Only once since same NH object will be re-used

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "2" }, // Remote endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni) }
                                       } 
                                   },
                                   {
                                       vnet_name + ":" + test_mac2,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "2" }, // Remote endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni2) }
                                       } 
                                   }
                            }
                     )
              );

              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN", {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name }
              });
              /* Tunnel termiantion rule should be local endpoint */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }
              });

              /* Rules for second ENI */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac2_key + "_OUT", {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name }
              });

              /* Check Ref count */
              ASSERT_TRUE(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name) != ctx->remote_nh_map_.end());
              EXPECT_EQ(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name)->second.first, 4); /* 4 rules using this NH */
              EXPECT_EQ(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name)->second.second, 0x400000000064d);

              EXPECT_CALL(*ctx, removeNextHopTunnel(tunnel_name, remote_npuv4_ip)).WillOnce(Return(true));

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
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_IN");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_IN_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_OUT");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac2_key + "_OUT_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_IN");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_IN_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_OUT");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_OUT_TERM");
              /* Check the tunnel is removed */
              ASSERT_TRUE(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name) == ctx->remote_nh_map_.end());
       }

       /* 
              Remote Endpoint with an update to switch to Local Endpoint
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor_SwitchToLocal)
       {
              IpAddress remote_npuv4_ip = IpAddress(remote_npuv4);
              EXPECT_CALL(*ctx, getRouterIntfsAlias(_, _)).WillOnce(Return(alias_dpu));
              /* 2 calls made for tunnel termination rules */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(2).WillRepeatedly(Return(true));
              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(2) // Once per non-tunnel term rules
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));
              EXPECT_CALL(*ctx, createNextHopTunnel(tunnel_name, remote_npuv4_ip)).Times(1)
                     .WillRepeatedly(Return(0x400000000064d)); // Only once since same NH object will be re-used

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "1,2" },
                                          { ENI_FWD_PRIMARY, "2" }, // Remote endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni) }
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN", {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT", {
                            { ACTION_REDIRECT_ACTION, remote_npuv4 + "@" + tunnel_name }
              });
              ASSERT_TRUE(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name) != ctx->remote_nh_map_.end());
              EXPECT_EQ(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name)->second.first, 2); /* 4 rules using this NH */
              EXPECT_EQ(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name)->second.second, 0x400000000064d);

              /* 2 calls will be made for non tunnel termination rules after primary switch */
              EXPECT_CALL(*ctx, isNeighborResolved(_)).Times(2).WillRepeatedly(Return(true));
              EXPECT_CALL(*ctx, removeNextHopTunnel(tunnel_name, remote_npuv4_ip)).WillOnce(Return(true));

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_PRIMARY, "1" }, // Primary is Local now
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }
              });
              /* Check ACL Rules  */
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT_TERM", {
                            { ACTION_REDIRECT_ACTION, local_pav4 }
              });
              /* Check the tunnel is removed */
              ASSERT_TRUE(ctx->remote_nh_map_.find(remote_npuv4 + "@" + tunnel_name) == ctx->remote_nh_map_.end());
       }

       /* 
              T1 doesn't host the ENI, Both the enndpoints are Remote. 
              No Tunnel Termination Rules expected 
       */
       TEST_F(DashEniFwdOrchTest, RemoteNeighbor_NoTunnelTerm)
       {      
              IpAddress remote_npuv4_ip = IpAddress(remote_2_npuv4);
              EXPECT_CALL(*ctx, findVnetTunnel(vnet_name, _)).Times(2) // Only two rules are created
                     .WillRepeatedly(DoAll(
                     SetArgReferee<1>(tunnel_name),
                     Return(true)
              ));

              EXPECT_CALL(*ctx, createNextHopTunnel(tunnel_name, remote_npuv4_ip)).Times(1)
                     .WillRepeatedly(Return(0x400000000064d)); // Only once since same NH object will be re-used

              doDashEniFwdTableTask(applDb.get(), 
                     deque<KeyOpFieldsValuesTuple>(
                            {
                                   {
                                       vnet_name + ":" + test_mac,
                                       SET_COMMAND,
                                       {
                                          { ENI_FWD_VDPU_IDS, "2,3" },
                                          { ENI_FWD_PRIMARY, "3" }, // Remote endpoint is the primary
                                          { ENI_FWD_OUT_VNI, to_string(test_vni) }
                                       } 
                                   }
                            }
                     )
              );

              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_IN", {
                            { ACTION_REDIRECT_ACTION, remote_2_npuv4 + "@" + tunnel_name }
              });
              checkKFV(aclRuleTable.get(), "ENI:" + vnet_name + "_" + test_mac_key+ "_OUT", {
                            { ACTION_REDIRECT_ACTION, remote_2_npuv4 + "@" + tunnel_name }
              });

              /* Tunnel termination rules are not installed */
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_IN_TERM");
              checkRuleUninstalled("ENI:" + vnet_name + "_" + test_mac_key+ "_OUT_TERM");
       }

       /* 
              Test ACL Table and Table Type config
       */
       TEST_F(DashEniFwdOrchTest, TestAclTableConfig)
       {
              /* Create a set of ports */
              std::map<std::string, Port> allPorts;
              allPorts["Ethernet0"] = Port("Ethernet0", Port::PHY);
              allPorts["Ethernet4"] = Port("Ethernet4", Port::PHY);
              allPorts["Ethernet8"] = Port("Ethernet8", Port::PHY);
              allPorts["Ethernet16"] = Port("Ethernet16", Port::PHY);
              allPorts["PortChannel1011"] = Port("PortChannel1012", Port::LAG);
              allPorts["PortChannel1012"] = Port("Ethernet16", Port::LAG);
              allPorts["PortChannel1011"].m_members.insert("Ethernet8");
              allPorts["PortChannel1012"].m_members.insert("Ethernet16");
              EXPECT_CALL(*ctx, getAllPorts()).WillOnce(ReturnRef(allPorts));

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

              eniOrch->initAclTableCfg();

              checkKFV(&aclTableType, "ENI_REDIRECT", {
                     { ACL_TABLE_TYPE_MATCHES, "TUNNEL_VNI,DST_IP,INNER_SRC_MAC,INNER_DST_MAC,TUNNEL_TERM" },
                     { ACL_TABLE_TYPE_ACTIONS, "REDIRECT_ACTION" },
                     { ACL_TABLE_TYPE_BPOINT_TYPES, "PORT,PORTCHANNEL" }
              });

              checkKFV(&aclTable, "ENI", {
                     { ACL_TABLE_TYPE, "ENI_REDIRECT" },
                     { ACL_TABLE_STAGE, STAGE_INGRESS },
                     { ACL_TABLE_PORTS, "Ethernet0,PortChannel1011,PortChannel1012" }
              });
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
              ASSERT_NO_THROW(ctx.createNextHopTunnel("tunnel0", IpAddress("10.0.0.1")));
              ASSERT_NO_THROW(ctx.removeNextHopTunnel("tunnel0", IpAddress("10.0.0.1")));
       }
}
