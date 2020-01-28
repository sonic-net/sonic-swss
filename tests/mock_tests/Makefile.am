FLEX_CTR_DIR = $(top_srcdir)/orchagent/flex_counter
DEBUG_CTR_DIR = $(top_srcdir)/orchagent/debug_counter

INCLUDES = -I $(FLEX_CTR_DIR) -I $(DEBUG_CTR_DIR)

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
                saispy_ut.cpp \
                ut_saihelper.cpp \
                mock_orchagent_main.cpp \
                mock_dbconnector.cpp \
                mock_consumerstatetable.cpp \
                mock_table.cpp \
                mock_hiredis.cpp \
                mock_redisreply.cpp \
                $(top_srcdir)/orchagent/orchdaemon.cpp \
                $(top_srcdir)/orchagent/orch.cpp \
                $(top_srcdir)/orchagent/notifications.cpp \
                $(top_srcdir)/orchagent/routeorch.cpp \
                $(top_srcdir)/orchagent/neighorch.cpp \
                $(top_srcdir)/orchagent/intfsorch.cpp \
                $(top_srcdir)/orchagent/portsorch.cpp \
                $(top_srcdir)/orchagent/copporch.cpp \
                $(top_srcdir)/orchagent/tunneldecaporch.cpp \
                $(top_srcdir)/orchagent/qosorch.cpp \
                $(top_srcdir)/orchagent/bufferorch.cpp \
                $(top_srcdir)/orchagent/mirrororch.cpp \
                $(top_srcdir)/orchagent/fdborch.cpp \
                $(top_srcdir)/orchagent/aclorch.cpp \
                $(top_srcdir)/orchagent/saihelper.cpp \
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
                $(top_srcdir)/orchagent/natorch.cpp 

tests_SOURCES += $(FLEX_CTR_DIR)/flex_counter_manager.cpp $(FLEX_CTR_DIR)/flex_counter_stat_manager.cpp
tests_SOURCES += $(DEBUG_CTR_DIR)/debug_counter.cpp $(DEBUG_CTR_DIR)/drop_counter.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) -I$(top_srcdir)/orchagent
tests_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main
