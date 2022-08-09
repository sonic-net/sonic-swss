ORCHAGENT_DIR = $(top_srcdir)/orchagent
P4ORCH_DIR = $(ORCHAGENT_DIR)/p4orch
INCLUDES = -I $(top_srcdir) -I $(ORCHAGENT_DIR) -I $(P4ORCH_DIR) -I $(top_srcdir)/lib -I $(ORCHAGENT_DIR)/flex_counter

CFLAGS_SAI = -I /usr/include/sai

TESTS = p4orch_tests p4orch_tests_asan p4orch_tests_tsan p4orch_tests_usan

noinst_PROGRAMS = p4orch_tests p4orch_tests_asan p4orch_tests_tsan p4orch_tests_usan

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -lgtest -lgtest_main -lgmock -lgmock_main
CFLAGS_COVERAGE = --coverage -fprofile-arcs -ftest-coverage
LDADD_COVERAGE = -lgcov
CFLAGS_ASAN = -fsanitize=address
CFLAGS_TSAN = -fsanitize=thread
CFLAGS_USAN = -fsanitize=undefined

p4orch_tests_SOURCES = $(ORCHAGENT_DIR)/orch.cpp \
		       $(ORCHAGENT_DIR)/vrforch.cpp \
		       $(ORCHAGENT_DIR)/vxlanorch.cpp \
		       $(ORCHAGENT_DIR)/copporch.cpp \
		       $(ORCHAGENT_DIR)/switchorch.cpp \
		       $(ORCHAGENT_DIR)/request_parser.cpp \
		       $(ORCHAGENT_DIR)/flex_counter/flex_counter_manager.cpp \
		       $(ORCHAGENT_DIR)/flex_counter/flow_counter_handler.cpp \
		       $(P4ORCH_DIR)/p4oidmapper.cpp \
		       $(P4ORCH_DIR)/p4orch.cpp \
		       $(P4ORCH_DIR)/p4orch_util.cpp \
		       $(P4ORCH_DIR)/router_interface_manager.cpp \
		       $(P4ORCH_DIR)/neighbor_manager.cpp \
		       $(P4ORCH_DIR)/next_hop_manager.cpp \
		       $(P4ORCH_DIR)/route_manager.cpp \
		       $(P4ORCH_DIR)/acl_util.cpp \
		       $(P4ORCH_DIR)/acl_table_manager.cpp \
		       $(P4ORCH_DIR)/acl_rule_manager.cpp \
		       $(P4ORCH_DIR)/wcmp_manager.cpp \
		       $(P4ORCH_DIR)/mirror_session_manager.cpp \
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
		       p4oidmapper_test.cpp \
		       p4orch_util_test.cpp \
		       return_code_test.cpp \
		       route_manager_test.cpp \
		       next_hop_manager_test.cpp \
		       wcmp_manager_test.cpp \
		       acl_manager_test.cpp \
		       router_interface_manager_test.cpp \
		       neighbor_manager_test.cpp \
		       mirror_session_manager_test.cpp \
		       test_main.cpp \
		       mock_sai_acl.cpp \
		       mock_sai_hostif.cpp \
		       mock_sai_serialize.cpp \
		       mock_sai_switch.cpp \
		       mock_sai_udf.cpp

p4orch_tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_COVERAGE) $(CFLAGS_SAI)
p4orch_tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_COVERAGE) $(CFLAGS_SAI)
p4orch_tests_LDADD = $(LDADD_GTEST) $(LDADD_COVERAGE) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq

p4orch_tests_asan_SOURCES = $(p4orch_tests_SOURCES)
p4orch_tests_asan_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_ASAN) $(CFLAGS_SAI)
p4orch_tests_asan_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_ASAN) $(CFLAGS_SAI)
p4orch_tests_asan_LDFLAGS = $(CFLAGS_ASAN)
p4orch_tests_asan_LDADD = $(LDADD_GTEST) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq

p4orch_tests_tsan_SOURCES = $(p4orch_tests_SOURCES)
p4orch_tests_tsan_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_TSAN) $(CFLAGS_SAI)
p4orch_tests_tsan_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_TSAN) $(CFLAGS_SAI)
p4orch_tests_tsan_LDFLAGS = $(CFLAGS_TSAN)
p4orch_tests_tsan_LDADD = $(LDADD_GTEST) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq

p4orch_tests_usan_SOURCES = $(p4orch_tests_SOURCES)
p4orch_tests_usan_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_USAN) $(CFLAGS_SAI)
p4orch_tests_usan_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_USAN) $(CFLAGS_SAI)
p4orch_tests_usan_LDFLAGS = $(CFLAGS_USAN)
p4orch_tests_usan_LDADD = $(LDADD_GTEST) -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq
