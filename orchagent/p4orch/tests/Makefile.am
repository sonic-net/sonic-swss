ORCHAGENT_DIR = $(top_srcdir)/orchagent
P4ORCH_DIR = $(ORCHAGENT_DIR)/p4orch
INCLUDES = -I $(top_srcdir) -I $(ORCHAGENT_DIR) -I $(P4ORCH_DIR) -I $(top_srcdir)/lib -I $(ORCHAGENT_DIR)/flex_counter

CFLAGS_SAI = -I /usr/include/sai

TESTS = p4orch_tests

noinst_PROGRAMS = p4orch_tests

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -lgtest -lgtest_main -lgmock -lgmock_main

p4orch_tests_SOURCES = $(ORCHAGENT_DIR)/orch.cpp \
		       $(ORCHAGENT_DIR)/vrforch.cpp \
		       $(ORCHAGENT_DIR)/vxlanorch.cpp \
		       $(ORCHAGENT_DIR)/copporch.cpp \
		       $(ORCHAGENT_DIR)/switch/switch_capabilities.cpp \
		       $(ORCHAGENT_DIR)/switch/switch_helper.cpp \
		       $(ORCHAGENT_DIR)/switch/trimming/capabilities.cpp \
		       $(ORCHAGENT_DIR)/switch/trimming/helper.cpp \
		       $(ORCHAGENT_DIR)/switchorch.cpp \
		       $(ORCHAGENT_DIR)/request_parser.cpp \
		       $(top_srcdir)/lib/recorder.cpp \
		       $(ORCHAGENT_DIR)/flex_counter/flex_counter_manager.cpp \
		       $(ORCHAGENT_DIR)/flex_counter/flow_counter_handler.cpp \
		       $(ORCHAGENT_DIR)/port/port_capabilities.cpp \
		       $(ORCHAGENT_DIR)/port/porthlpr.cpp \
		       $(ORCHAGENT_DIR)/notifications.cpp \
		       $(P4ORCH_DIR)/p4oidmapper.cpp \
		       $(P4ORCH_DIR)/p4orch.cpp \
		       $(P4ORCH_DIR)/p4orch_util.cpp \
		       $(P4ORCH_DIR)/tables_definition_manager.cpp \
		       $(P4ORCH_DIR)/router_interface_manager.cpp \
		       $(P4ORCH_DIR)/gre_tunnel_manager.cpp \
		       $(P4ORCH_DIR)/neighbor_manager.cpp \
		       $(P4ORCH_DIR)/next_hop_manager.cpp \
		       $(P4ORCH_DIR)/route_manager.cpp \
		       $(P4ORCH_DIR)/acl_util.cpp \
		       $(P4ORCH_DIR)/acl_table_manager.cpp \
		       $(P4ORCH_DIR)/acl_rule_manager.cpp \
		       $(P4ORCH_DIR)/wcmp_manager.cpp \
		       $(P4ORCH_DIR)/mirror_session_manager.cpp \
		       $(P4ORCH_DIR)/l3_admit_manager.cpp \
		       $(P4ORCH_DIR)/l3_multicast_manager.cpp \
		       $(P4ORCH_DIR)/ext_tables_manager.cpp \
		       $(top_srcdir)/tests/mock_tests/fake_response_publisher.cpp \
		       fake_portorch.cpp \
		       fake_crmorch.cpp \
		       fake_flexcounterorch.cpp \
		       fake_flowcounterrouteorch.cpp \
		       fake_dbconnector.cpp \
		       fake_producertable.cpp \
		       fake_consumerstatetable.cpp \
		       fake_subscriberstatetable.cpp \
		       fake_notificationconsumer.cpp \
		       fake_table.cpp \
		       fake_aclorch.cpp \
		       p4oidmapper_test.cpp \
		       p4orch_test.cpp \
		       p4orch_util_test.cpp \
		       return_code_test.cpp \
		       route_manager_test.cpp \
		       l3_multicast_manager_test.cpp \
		       gre_tunnel_manager_test.cpp \
		       next_hop_manager_test.cpp \
		       wcmp_manager_test.cpp \
		       acl_manager_test.cpp \
		       l3_admit_manager_test.cpp \
		       router_interface_manager_test.cpp \
		       neighbor_manager_test.cpp \
		       mirror_session_manager_test.cpp \
		       test_main.cpp \
		       mock_sai_acl.cpp \
		       mock_sai_bridge.cpp \
		       mock_sai_hostif.cpp \
		       mock_sai_ipmc.cpp \
		       mock_sai_ipmc_group.cpp \
		       mock_sai_serialize.cpp \
		       mock_sai_router_interface.cpp \
		       mock_sai_rpf_group.cpp \
		       mock_sai_neighbor.cpp \
		       mock_sai_next_hop.cpp \
		       mock_sai_route.cpp \
		       mock_sai_switch.cpp \
		       mock_sai_udf.cpp

p4orch_tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN)
p4orch_tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN)
p4orch_tests_LDADD = $(LDADD_GTEST) $(LDFLAGS_ASAN) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq
