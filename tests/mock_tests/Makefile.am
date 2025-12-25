FLEX_CTR_DIR = $(top_srcdir)/orchagent/flex_counter
DEBUG_CTR_DIR = $(top_srcdir)/orchagent/debug_counter
P4_ORCH_DIR = $(top_srcdir)/orchagent/p4orch
DASH_ORCH_DIR = $(top_srcdir)/orchagent/dash
DASH_PROTO_DIR = $(top_srcdir)/orchagent/dash/proto

CFLAGS = -g -O0
CXXFLAGS = -g -O0

CFLAGS_SAI = -I /usr/include/sai

TESTS = tests tests_intfmgrd tests_teammgrd tests_portsyncd tests_fpmsyncd tests_response_publisher

noinst_PROGRAMS = tests tests_intfmgrd tests_teammgrd tests_portsyncd tests_fpmsyncd tests_response_publisher

LDADD_SAI = -lsaimeta -lsaimetadata -lsaivs -lsairedis

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -L/usr/src/gtest

## Orchagent Unit Tests

tests_INCLUDES = -I $(FLEX_CTR_DIR) -I $(DEBUG_CTR_DIR) -I $(top_srcdir)/lib -I$(top_srcdir)/cfgmgr -I$(top_srcdir)/orchagent -I$(P4_ORCH_DIR)/tests -I$(DASH_ORCH_DIR) -I$(top_srcdir)/warmrestart

tests_SOURCES = aclorch_ut.cpp \
                aclorch_rule_ut.cpp \
                portsorch_ut.cpp \
                routeorch_ut.cpp \
                qosorch_ut.cpp \
                bufferorch_ut.cpp \
                buffermgrdyn_ut.cpp \
                fdborch/flush_syncd_notif_ut.cpp \
                copp_ut.cpp \
                copporch_ut.cpp \
                saispy_ut.cpp \
                consumer_ut.cpp \
                sfloworh_ut.cpp \
                tunneldecaporch_ut.cpp \
                ut_saihelper.cpp \
                mock_orchagent_main.cpp \
                mock_dbconnector.cpp \
                mock_consumerstatetable.cpp \
                mock_subscriberstatetable.cpp \
                common/mock_shell_command.cpp \
                mock_table.cpp \
                mock_hiredis.cpp \
                mock_redisreply.cpp \
                mock_sai_api.cpp \
                bulker_ut.cpp \
                portmgr_ut.cpp \
                sflowmgrd_ut.cpp \
                fake_response_publisher.cpp \
                swssnet_ut.cpp \
                flowcounterrouteorch_ut.cpp \
                orchdaemon_ut.cpp \
                intfsorch_ut.cpp \
                mux_rollback_ut.cpp \
                warmrestartassist_ut.cpp \
                test_failure_handling.cpp \
                switchorch_ut.cpp \
                warmrestarthelper_ut.cpp \
                neighorch_ut.cpp \
                dashenifwdorch_ut.cpp \
                dashorch_ut.cpp \
                dashvnetorch_ut.cpp \
                dashhaorch_ut.cpp \
                dashrouteorch_ut.cpp \
                dashportmaporch_ut.cpp \
                twamporch_ut.cpp \
                stporch_ut.cpp \
                flexcounter_ut.cpp \
                mock_orch_test.cpp \
                mock_dash_orch_test.cpp \
                zmq_orch_ut.cpp \
                retrycache_ut.cpp \
                mock_saihelper.cpp \
                mirrororch_ut.cpp \
                $(top_srcdir)/warmrestart/warmRestartHelper.cpp \
                $(top_srcdir)/lib/gearboxutils.cpp \
                $(top_srcdir)/lib/subintf.cpp \
                $(top_srcdir)/lib/recorder.cpp \
                $(top_srcdir)/lib/orch_zmq_config.cpp \
                $(top_srcdir)/orchagent/orchdaemon.cpp \
                $(top_srcdir)/orchagent/orch.cpp \
                $(top_srcdir)/orchagent/notifications.cpp \
                $(top_srcdir)/orchagent/routeorch.cpp \
                $(top_srcdir)/orchagent/mplsrouteorch.cpp \
                $(top_srcdir)/orchagent/fgnhgorch.cpp \
                $(top_srcdir)/orchagent/nhgbase.cpp \
                $(top_srcdir)/orchagent/nhgorch.cpp \
                $(top_srcdir)/orchagent/cbf/cbfnhgorch.cpp \
                $(top_srcdir)/orchagent/cbf/nhgmaporch.cpp \
                $(top_srcdir)/orchagent/neighorch.cpp \
                $(top_srcdir)/orchagent/intfsorch.cpp \
                $(top_srcdir)/orchagent/port/port_capabilities.cpp \
                $(top_srcdir)/orchagent/port/porthlpr.cpp \
                $(top_srcdir)/orchagent/portsorch.cpp \
                $(top_srcdir)/orchagent/fabricportsorch.cpp \
                $(top_srcdir)/orchagent/copporch.cpp \
                $(top_srcdir)/orchagent/tunneldecaporch.cpp \
                $(top_srcdir)/orchagent/qosorch.cpp \
                $(top_srcdir)/orchagent/buffer/bufferhelper.cpp \
                $(top_srcdir)/orchagent/bufferorch.cpp \
                $(top_srcdir)/orchagent/mirrororch.cpp \
                $(top_srcdir)/orchagent/fdborch.cpp \
                $(top_srcdir)/orchagent/aclorch.cpp \
                $(top_srcdir)/orchagent/pbh/pbhcap.cpp \
                $(top_srcdir)/orchagent/pbh/pbhcnt.cpp \
                $(top_srcdir)/orchagent/pbh/pbhmgr.cpp \
                $(top_srcdir)/orchagent/pbh/pbhrule.cpp \
                $(top_srcdir)/orchagent/pbhorch.cpp \
                $(top_srcdir)/orchagent/saihelper.cpp \
                $(top_srcdir)/orchagent/saiattr.cpp \
                $(top_srcdir)/orchagent/switch/switch_capabilities.cpp \
                $(top_srcdir)/orchagent/switch/switch_helper.cpp \
                $(top_srcdir)/orchagent/switch/trimming/capabilities.cpp \
                $(top_srcdir)/orchagent/switch/trimming/helper.cpp \
                $(top_srcdir)/orchagent/switchorch.cpp \
                $(top_srcdir)/orchagent/pfcwdorch.cpp \
                $(top_srcdir)/orchagent/pfcactionhandler.cpp \
                $(top_srcdir)/orchagent/policerorch.cpp \
                $(top_srcdir)/orchagent/crmorch.cpp \
                $(top_srcdir)/orchagent/request_parser.cpp \
                $(top_srcdir)/orchagent/vrforch.cpp \
                $(top_srcdir)/orchagent/countercheckorch.cpp \
                $(top_srcdir)/orchagent/vxlanorch.cpp \
                $(top_srcdir)/orchagent/tunneltermhelper.cpp \
                $(top_srcdir)/orchagent/vnetorch.cpp \
                $(top_srcdir)/orchagent/dtelorch.cpp \
                $(top_srcdir)/orchagent/flexcounterorch.cpp \
                $(top_srcdir)/orchagent/watermarkorch.cpp \
                $(top_srcdir)/orchagent/chassisorch.cpp \
                $(top_srcdir)/orchagent/sfloworch.cpp \
                $(top_srcdir)/orchagent/debugcounterorch.cpp \
                $(top_srcdir)/orchagent/natorch.cpp \
                $(top_srcdir)/orchagent/muxorch.cpp \
                $(top_srcdir)/orchagent/mlagorch.cpp \
                $(top_srcdir)/orchagent/isolationgrouporch.cpp \
                $(top_srcdir)/orchagent/macsecorch.cpp \
                $(top_srcdir)/orchagent/macsecpost.cpp \
                $(top_srcdir)/orchagent/lagid.cpp \
                $(top_srcdir)/orchagent/bfdorch.cpp \
                $(top_srcdir)/orchagent/icmporch.cpp \
                $(top_srcdir)/orchagent/srv6orch.cpp \
                $(top_srcdir)/orchagent/nvgreorch.cpp \
                $(top_srcdir)/cfgmgr/portmgr.cpp \
                $(top_srcdir)/cfgmgr/sflowmgr.cpp \
                $(top_srcdir)/orchagent/zmqorch.cpp \
                $(top_srcdir)/orchagent/dash/dashenifwdorch.cpp \
                $(top_srcdir)/orchagent/dash/dashenifwdinfo.cpp \
                $(top_srcdir)/orchagent/dash/dashaclorch.cpp \
                $(top_srcdir)/orchagent/dash/dashorch.cpp \
                $(top_srcdir)/orchagent/dash/dashaclgroupmgr.cpp \
                $(top_srcdir)/orchagent/dash/dashtagmgr.cpp \
                $(top_srcdir)/orchagent/dash/dashrouteorch.cpp \
                $(top_srcdir)/orchagent/dash/dashtunnelorch.cpp \
                $(top_srcdir)/orchagent/dash/dashvnetorch.cpp \
                $(top_srcdir)/orchagent/dash/dashhaorch.cpp \
                $(top_srcdir)/orchagent/dash/dashmeterorch.cpp \
                $(top_srcdir)/orchagent/dash/dashportmaporch.cpp \
                $(top_srcdir)/cfgmgr/buffermgrdyn.cpp \
                $(top_srcdir)/warmrestart/warmRestartAssist.cpp \
                $(top_srcdir)/orchagent/dash/pbutils.cpp \
                $(top_srcdir)/cfgmgr/coppmgr.cpp \
                $(top_srcdir)/orchagent/twamporch.cpp \
                $(top_srcdir)/orchagent/stporch.cpp \
                $(top_srcdir)/orchagent/nexthopkey.cpp \
                $(top_srcdir)/orchagent/high_frequency_telemetry/hftelorch.cpp \
                $(top_srcdir)/orchagent/high_frequency_telemetry/hftelprofile.cpp \
                $(top_srcdir)/orchagent/high_frequency_telemetry/counternameupdater.cpp \
                $(top_srcdir)/orchagent/high_frequency_telemetry/hftelutils.cpp \
                $(top_srcdir)/orchagent/high_frequency_telemetry/hftelgroup.cpp


tests_SOURCES += $(FLEX_CTR_DIR)/flex_counter_manager.cpp $(FLEX_CTR_DIR)/flex_counter_stat_manager.cpp $(FLEX_CTR_DIR)/flow_counter_handler.cpp $(FLEX_CTR_DIR)/flowcounterrouteorch.cpp
tests_SOURCES += $(DEBUG_CTR_DIR)/debug_counter.cpp $(DEBUG_CTR_DIR)/drop_counter.cpp
tests_SOURCES += $(P4_ORCH_DIR)/p4orch.cpp \
		 $(P4_ORCH_DIR)/p4orch_util.cpp \
		 $(P4_ORCH_DIR)/p4oidmapper.cpp \
		 $(P4_ORCH_DIR)/tables_definition_manager.cpp \
		 $(P4_ORCH_DIR)/router_interface_manager.cpp \
		 $(P4_ORCH_DIR)/neighbor_manager.cpp \
		 $(P4_ORCH_DIR)/next_hop_manager.cpp \
		 $(P4_ORCH_DIR)/route_manager.cpp \
		 $(P4_ORCH_DIR)/acl_util.cpp \
		 $(P4_ORCH_DIR)/acl_table_manager.cpp \
		 $(P4_ORCH_DIR)/acl_rule_manager.cpp \
		 $(P4_ORCH_DIR)/wcmp_manager.cpp \
		 $(P4_ORCH_DIR)/mirror_session_manager.cpp \
		 $(P4_ORCH_DIR)/gre_tunnel_manager.cpp \
		 $(P4_ORCH_DIR)/l3_admit_manager.cpp \
		 $(P4_ORCH_DIR)/l3_multicast_manager.cpp \
		 $(P4_ORCH_DIR)/ext_tables_manager.cpp \
		 $(P4_ORCH_DIR)/tests/mock_sai_switch.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_INCLUDES)
tests_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3 -lgmock -lgmock_main -lprotobuf -ldashapi

## portsyncd unit tests

tests_portsyncd_SOURCES = portsyncd/portsyncd_ut.cpp \
                          $(top_srcdir)/lib/recorder.cpp \
                          $(top_srcdir)/portsyncd/linksync.cpp \
                          mock_dbconnector.cpp \
                          common/mock_shell_command.cpp \
                          mock_table.cpp \
                          mock_hiredis.cpp \
                          mock_redisreply.cpp

tests_portsyncd_INCLUDES = -I $(top_srcdir)/portsyncd -I $(top_srcdir)/cfgmgr -I $(top_srcdir)/lib
tests_portsyncd_CXXFLAGS = -Wl,-wrap,if_nameindex -Wl,-wrap,if_freenameindex
tests_portsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST)
tests_portsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(tests_portsyncd_INCLUDES)
tests_portsyncd_LDADD = $(LDADD_GTEST) -lnl-genl-3 -lhiredis -lhiredis \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lnl-3 -lnl-route-3 -lpthread

## intfmgrd unit tests

tests_intfmgrd_SOURCES = intfmgrd/intfmgr_ut.cpp \
                         $(top_srcdir)/cfgmgr/intfmgr.cpp \
                         $(top_srcdir)/lib/subintf.cpp \
                         $(top_srcdir)/lib/recorder.cpp \
                         $(top_srcdir)/orchagent/orch.cpp \
                         $(top_srcdir)/orchagent/request_parser.cpp \
                         mock_orchagent_main.cpp \
                         mock_dbconnector.cpp \
                         mock_table.cpp \
                         mock_hiredis.cpp \
                         fake_response_publisher.cpp \
                         mock_redisreply.cpp \
                         common/mock_shell_command.cpp

tests_intfmgrd_INCLUDES = $(tests_INCLUDES) -I$(top_srcdir)/cfgmgr -I$(top_srcdir)/lib
tests_intfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_intfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_intfmgrd_INCLUDES)
tests_intfmgrd_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3 -lpthread -lgmock -lgmock_main

## teammgrd unit tests

tests_teammgrd_SOURCES = teammgrd/teammgr_ut.cpp \
                         $(top_srcdir)/cfgmgr/teammgr.cpp \
                         $(top_srcdir)/lib/subintf.cpp \
                         $(top_srcdir)/lib/recorder.cpp \
                         $(top_srcdir)/orchagent/orch.cpp \
                         $(top_srcdir)/orchagent/request_parser.cpp \
                         mock_orchagent_main.cpp \
                         mock_dbconnector.cpp \
                         mock_table.cpp \
                         mock_hiredis.cpp \
                         fake_response_publisher.cpp \
                         mock_redisreply.cpp \
                         common/mock_shell_command.cpp

tests_teammgrd_INCLUDES = $(tests_INCLUDES) -I$(top_srcdir)/cfgmgr -I$(top_srcdir)/lib
tests_teammgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_teammgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_teammgrd_INCLUDES)
tests_teammgrd_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -ldl -lhiredis \
        -lswsscommon -lgtest -lgtest_main -lzmq -lpthread -lgmock -lgmock_main

## fpmsyncd unit tests

tests_fpmsyncd_SOURCES = fpmsyncd/test_fpmlink.cpp \
                         fpmsyncd/test_routesync.cpp \
                         fpmsyncd/receive_srv6_steer_routes_ut.cpp \
                         fpmsyncd/receive_srv6_mysids_ut.cpp \
                         fpmsyncd/ut_helpers_fpmsyncd.cpp \
                         fake_netlink.cpp \
                         fake_warmstarthelper.cpp \
                         fake_producerstatetable.cpp \
                         mock_dbconnector.cpp \
                         mock_table.cpp \
                         mock_hiredis.cpp \
                         $(top_srcdir)/lib/orch_zmq_config.cpp \
                         $(top_srcdir)/warmrestart/ \
                         $(top_srcdir)/fpmsyncd/fpmlink.cpp \
                         $(top_srcdir)/fpmsyncd/routesync.cpp

tests_fpmsyncd_INCLUDES = $(tests_INCLUDES) -I$(top_srcdir)/tests_fpmsyncd -I$(top_srcdir)/lib -I$(top_srcdir)/warmrestart -I$(top_srcdir)/fpmsyncd
tests_fpmsyncd_CXXFLAGS = -Wl,-wrap,rtnl_link_i2name
tests_fpmsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_fpmsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_fpmsyncd_INCLUDES)
tests_fpmsyncd_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3 -lpthread -lgmock -lgmock_main

## response publisher unit tests

tests_response_publisher_SOURCES = response_publisher/response_publisher_ut.cpp \
                                   $(top_srcdir)/orchagent/response_publisher.cpp \
                                   $(top_srcdir)/lib/recorder.cpp \
                                   mock_orchagent_main.cpp \
                                   mock_dbconnector.cpp \
                                   mock_table.cpp \
                                   mock_hiredis.cpp \
                                   mock_redisreply.cpp

tests_response_publisher_INCLUDES = $(tests_INCLUDES)
tests_response_publisher_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_response_publisher_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_response_publisher_INCLUDES)
tests_response_publisher_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3 -lpthread
