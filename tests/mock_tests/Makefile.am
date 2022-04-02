FLEX_CTR_DIR = $(top_srcdir)/orchagent/flex_counter
DEBUG_CTR_DIR = $(top_srcdir)/orchagent/debug_counter
P4_ORCH_DIR = $(top_srcdir)/orchagent/p4orch

INCLUDES = -I $(FLEX_CTR_DIR) -I $(DEBUG_CTR_DIR) -I $(top_srcdir)/lib

CFLAGS_SAI = -I /usr/include/sai

TESTS = tests

noinst_PROGRAMS = tests

LDADD_SAI = -lsaimeta -lsaimetadata -lsaivs -lsairedis

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -L/usr/src/gtest

tests_SOURCES = aclorch_ut.cpp \
                portsorch_ut.cpp \
                routeorch_ut.cpp \
                qosorch_ut.cpp \
                bufferorch_ut.cpp \
                saispy_ut.cpp \
                consumer_ut.cpp \
                ut_saihelper.cpp \
                mock_orchagent_main.cpp \
                mock_dbconnector.cpp \
                mock_consumerstatetable.cpp \
                mock_table.cpp \
                mock_hiredis.cpp \
                mock_redisreply.cpp \
                bulker_ut.cpp \
                fake_response_publisher.cpp \
                $(top_srcdir)/lib/gearboxutils.cpp \
                $(top_srcdir)/lib/subintf.cpp \
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
                $(top_srcdir)/orchagent/portsorch.cpp \
                $(top_srcdir)/orchagent/fabricportsorch.cpp \
                $(top_srcdir)/orchagent/copporch.cpp \
                $(top_srcdir)/orchagent/tunneldecaporch.cpp \
                $(top_srcdir)/orchagent/qosorch.cpp \
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
                $(top_srcdir)/orchagent/switchorch.cpp \
                $(top_srcdir)/orchagent/pfcwdorch.cpp \
                $(top_srcdir)/orchagent/pfcactionhandler.cpp \
                $(top_srcdir)/orchagent/policerorch.cpp \
                $(top_srcdir)/orchagent/crmorch.cpp \
                $(top_srcdir)/orchagent/request_parser.cpp \
                $(top_srcdir)/orchagent/vrforch.cpp \
                $(top_srcdir)/orchagent/countercheckorch.cpp \
                $(top_srcdir)/orchagent/vxlanorch.cpp \
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
                $(top_srcdir)/orchagent/lagid.cpp \
                $(top_srcdir)/orchagent/bfdorch.cpp \
                $(top_srcdir)/orchagent/srv6orch.cpp \
                $(top_srcdir)/orchagent/nvgreorch.cpp

tests_SOURCES += $(FLEX_CTR_DIR)/flex_counter_manager.cpp $(FLEX_CTR_DIR)/flex_counter_stat_manager.cpp $(FLEX_CTR_DIR)/flow_counter_handler.cpp
tests_SOURCES += $(DEBUG_CTR_DIR)/debug_counter.cpp $(DEBUG_CTR_DIR)/drop_counter.cpp
tests_SOURCES += $(P4_ORCH_DIR)/p4orch.cpp \
		 $(P4_ORCH_DIR)/p4orch_util.cpp \
		 $(P4_ORCH_DIR)/p4oidmapper.cpp \
		 $(P4_ORCH_DIR)/router_interface_manager.cpp \
		 $(P4_ORCH_DIR)/neighbor_manager.cpp \
		 $(P4_ORCH_DIR)/next_hop_manager.cpp \
		 $(P4_ORCH_DIR)/route_manager.cpp \
		 $(P4_ORCH_DIR)/acl_util.cpp \
		 $(P4_ORCH_DIR)/acl_table_manager.cpp \
		 $(P4_ORCH_DIR)/acl_rule_manager.cpp \
		 $(P4_ORCH_DIR)/wcmp_manager.cpp \
		 $(P4_ORCH_DIR)/mirror_session_manager.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) -I$(top_srcdir)/orchagent
tests_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3
