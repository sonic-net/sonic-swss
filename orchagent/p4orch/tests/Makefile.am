ORCHAGENT_DIR = $(top_srcdir)/orchagent
P4ORCH_DIR = $(ORCHAGENT_DIR)/p4orch
include $(top_srcdir)/orchagent/p4orch/shared_sources.mk

INCLUDES = -I $(top_srcdir) -I $(ORCHAGENT_DIR) -I $(P4ORCH_DIR) -I $(top_srcdir)/lib -I $(ORCHAGENT_DIR)/flex_counter

CFLAGS_SAI = -I /usr/include/sai

TESTS = p4orch_tests

check_PROGRAMS = p4orch_tests
noinst_LIBRARIES = libp4orchshared.a

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -lgtest -lgtest_main -lgmock -lgmock_main

libp4orchshared_a_SOURCES = $(P4ORCH_SHARED_SOURCES)
libp4orchshared_a_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN)

p4orch_tests_SOURCES = $(top_srcdir)/tests/mock_tests/fake_response_publisher.cpp \
		       fake_portorch.cpp \
		       fake_crmorch.cpp \
		       fake_flexcounterorch.cpp \
		       fake_flowcounterrouteorch.cpp \
		       fake_routeorch.cpp \
		       fake_dbconnector.cpp \
		       fake_producertable.cpp \
		       fake_consumerstatetable.cpp \
		       fake_subscriberstatetable.cpp \
		       fake_notificationconsumer.cpp \
		       fake_notificationconsumerstatsorch.cpp \
		       fake_table.cpp \
		       fake_aclorch.cpp \
		       fake_zmqserver.cpp \
		       fake_namelabelmapper.cpp \
		       p4oidmapper_test.cpp \
		       namelabelmapper_test.cpp \
		       p4orch_test.cpp \
		       p4orch_util_test.cpp \
		       return_code_test.cpp \
		       route_manager_test.cpp \
		       l3_multicast_manager_test.cpp \
		       ip_multicast_manager_test.cpp \
		       gre_tunnel_manager_test.cpp \
		       next_hop_manager_test.cpp \
		       wcmp_manager_test.cpp \
		       acl_manager_test.cpp \
		       l3_admit_manager_test.cpp \
		       router_interface_manager_test.cpp \
		       neighbor_manager_test.cpp \
		       mirror_session_manager_test.cpp \
                       tunnel_decap_group_manager_test.cpp \
		       test_main.cpp \
		       mock_sai_acl.cpp \
		       mock_sai_bridge.cpp \
		       mock_sai_hostif.cpp \
		       mock_sai_ipmc.cpp \
		       mock_sai_ipmc_group.cpp \
		       mock_sai_l2mc.cpp \
		       mock_sai_l2mc_group.cpp \
		       mock_sai_my_mac.cpp \
		       mock_sai_serialize.cpp \
		       mock_sai_router_interface.cpp \
		       mock_sai_rpf_group.cpp \
		       mock_sai_neighbor.cpp \
		       mock_sai_next_hop.cpp \
		       mock_sai_route.cpp \
		       mock_sai_switch.cpp \
		       mock_sai_udf.cpp \
		       mock_sai_tunnel.cpp

p4orch_tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN)
p4orch_tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN)
p4orch_tests_LDADD = libp4orchshared.a $(LDADD_GTEST) $(LDFLAGS_ASAN) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq

$(p4orch_tests_OBJECTS): CXXFLAGS := $(filter-out -O%,$(CXXFLAGS)) -O0

LOG_DRIVER = $(top_srcdir)/run-gtest-suite.py
