FLEX_CTR_DIR = $(top_srcdir)/orchagent/flex_counter
DEBUG_CTR_DIR = $(top_srcdir)/orchagent/debug_counter
P4_ORCH_DIR = $(top_srcdir)/orchagent/p4orch
DASH_ORCH_DIR = $(top_srcdir)/orchagent/dash
DASH_PROTO_DIR = $(top_srcdir)/orchagent/dash/proto

CFLAGS = -g -O0
CXXFLAGS = -g -O0

CFLAGS_SAI = -I /usr/include/sai

BUILT_SOURCES = tests_pch.h tests_pch.h.gch
CLEANFILES = tests_pch.h tests_pch.h.gch

TESTS_PCH_CPPFLAGS = $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
                     $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) \
                     $(CFLAGS_ASAN) $(tests_INCLUDES) $(CPPFLAGS) $(CXXFLAGS)

tests_pch.h: $(top_srcdir)/orchagent/swss_pch.h
	@sed 's/SWSS_ORCHAGENT_PCH_H/SWSS_TESTS_PCH_H/g' $< > $@

tests_pch.h.gch: tests_pch.h
	@echo "  [PCH]    $< (tests, -O0)"
	@$(CXX) $(TESTS_PCH_CPPFLAGS) -x c++-header -o $@ $<

TESTS = tests tests_intfmgrd tests_teammgrd tests_portsyncd tests_fpmsyncd tests_response_publisher tests_teamsyncd

noinst_PROGRAMS = tests tests_intfmgrd tests_teammgrd tests_portsyncd tests_fpmsyncd tests_response_publisher tests_teamsyncd

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
                vxlanorch_ut.cpp \
                routeorch_ut.cpp \
                qosorch_ut.cpp \
                bufferorch_ut.cpp \
                buffermgrdyn_ut.cpp \
                fdborch/flush_syncd_notif_ut.cpp \
                copp_ut.cpp \
                copporch_ut.cpp \
                saispy_ut.cpp \
                consumer_ut.cpp \
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
                vrforch_ut.cpp \
                config_request_relaxed_ut.cpp \
                vrf_schema_hardening_ut.cpp \
                mux_rollback_ut.cpp \
                warmrestartassist_ut.cpp \
                test_failure_handling.cpp \
                switchorch_ut.cpp \
                warmrestarthelper_ut.cpp \
                neighorch_ut.cpp \
                arsorch_ut.cpp \
                flexcounter_ut.cpp \
                portphyattr_ut.cpp \
                portphyserdesattr_ut.cpp \
                hftelprofile_ut.cpp \
                mock_orch_test.cpp \
                zmq_orch_ut.cpp \
                mock_saihelper.cpp \
                mirrororch_ut.cpp \
                hftelorch_ut.cpp \
                hftelorch_is_supported_sai_wrap.cpp \
                $(top_srcdir)/warmrestart/warmRestartHelper.cpp \
                $(top_srcdir)/lib/gearboxutils.cpp \
                $(top_srcdir)/lib/subintf.cpp \
                $(top_srcdir)/lib/recorder.cpp \
                $(top_srcdir)/lib/orch_zmq_config.cpp \
                $(top_srcdir)/cfgmgr/portmgr.cpp \
                $(top_srcdir)/cfgmgr/sflowmgr.cpp \
                $(top_srcdir)/cfgmgr/buffermgrdyn.cpp \
                $(top_srcdir)/cfgmgr/vrf_appl_fields.cpp \
                $(top_srcdir)/warmrestart/warmRestartAssist.cpp \
                $(top_srcdir)/cfgmgr/coppmgr.cpp

if INCLUDE_TUNNELDECAP
tests_SOURCES += tunneldecaporch_ut.cpp
endif


if INCLUDE_TWAMP
tests_SOURCES += twamporch_ut.cpp
endif

if INCLUDE_SFLOW
tests_SOURCES += sfloworh_ut.cpp
endif

if INCLUDE_STP
tests_SOURCES += stporch_ut.cpp
endif

if INCLUDE_SRV6
tests_SOURCES += srv6orch_ut.cpp
endif

if INCLUDE_DASH
tests_SOURCES += dashenifwdorch_ut.cpp \
                dashorch_ut.cpp \
                dashvnetorch_ut.cpp \
                dashhaorch_ut.cpp \
                dashrouteorch_ut.cpp \
                dashportmaporch_ut.cpp \
                mock_dash_orch_test.cpp
endif

tests_SOURCES += $(P4_ORCH_DIR)/tests/mock_sai_switch.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(CFLAGS_ASAN) $(tests_INCLUDES) \
                 -include ./tests_pch.h -Winvalid-pch
tests_LDFLAGS = -Wl,--wrap=sai_query_stats_st_capability -Wl,--wrap=sai_query_attribute_capability
tests_LDADD = $(top_builddir)/orchagent/libswssorchagent.a $(LDFLAGS_ASAN) \
        $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main -lzmq -lnl-3 -lnl-route-3 -lgmock -lgmock_main -lprotobuf

if INCLUDE_DASH
tests_LDADD += -ldashapi
endif

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
                         fpmsyncd/ut_helpers_fpmsyncd.cpp \
                         fpmsyncd/ut_helpers_drain_barrier.cpp \
                         fpmsyncd/test_drain_barrier_state.cpp \
                         fpmsyncd/test_drain_barrier_gate.cpp \
                         fpmsyncd/test_drain_barrier_fpmlink.cpp \
                         fpmsyncd/test_drain_barrier_handler.cpp \
                         fpmsyncd/test_drain_barrier_startup.cpp \
                         fake_netlink.cpp \
                         fake_warmstarthelper.cpp \
                         fake_producerstatetable.cpp \
                         mock_dbconnector.cpp \
                         mock_table.cpp \
                         mock_hiredis.cpp \
                         $(top_srcdir)/lib/orch_zmq_config.cpp \
                         $(top_srcdir)/warmrestart/ \
                         $(top_srcdir)/fpmsyncd/fpmlink.cpp \
                         $(top_srcdir)/fpmsyncd/routesync.cpp \
                         $(top_srcdir)/fpmsyncd/warmreboot_handler.cpp

if INCLUDE_SRV6
tests_fpmsyncd_SOURCES += fpmsyncd/receive_srv6_steer_routes_ut.cpp \
                          fpmsyncd/receive_srv6_mysids_ut.cpp
endif

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

tests_teamsyncd_SOURCES = teamsync_ut.cpp \
                          $(top_srcdir)/teamsyncd/teamsync.cpp \
                          mock_dbconnector.cpp \
                          mock_table.cpp \
                          mock_hiredis.cpp \
                          mock_redisreply.cpp

tests_teamsyncd_INCLUDES = $(tests_INCLUDES) -I$(top_srcdir)/teamsyncd
tests_teamsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_teamsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) $(tests_teamsyncd_INCLUDES)
tests_teamsyncd_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 \
        -lswsscommon -ldl -lhiredis -lgtest -lgtest_main -lpthread -lteam -lteamdctl -lnl-route-3
