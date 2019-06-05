CFLAGS_SAI = -I /usr/include/sai
INCLUDES = -I ../orchagent

bin_PROGRAMS = tests

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

LDADD_SAI = -lsaimeta -lsaimetadata -lsaivs -lsairedis

CFLAGS_GTEST =
LDADD_GTEST = -L/usr/src/gtest

tests_SOURCES = swssnet_ut.cpp request_parser_ut.cpp aclorch_ut.cpp saispy_ut.cpp \
                mock_orchagent_main.cpp \
                mock_dbconnector.cpp \
                mock_consumerstatetable.cpp \
                mock_hiredis.cpp \
                mock_redisreply.cpp \
                ../orchagent/orchdaemon.cpp \
                ../orchagent/orch.cpp \
                ../orchagent/notifications.cpp \
                ../orchagent/routeorch.cpp \
                ../orchagent/neighorch.cpp \
                ../orchagent/intfsorch.cpp \
                ../orchagent/portsorch.cpp \
                ../orchagent/copporch.cpp \
                ../orchagent/tunneldecaporch.cpp \
                ../orchagent/qosorch.cpp \
                ../orchagent/bufferorch.cpp \
                ../orchagent/mirrororch.cpp \
                ../orchagent/fdborch.cpp \
                ../orchagent/aclorch.cpp \
                ../orchagent/saihelper.cpp \
                ../orchagent/switchorch.cpp \
                ../orchagent/pfcwdorch.cpp \
                ../orchagent/pfcactionhandler.cpp \
                ../orchagent/policerorch.cpp \
                ../orchagent/crmorch.cpp \
                ../orchagent/request_parser.cpp \
                ../orchagent/vrforch.cpp \
                ../orchagent/countercheckorch.cpp \
                ../orchagent/vxlanorch.cpp \
                ../orchagent/vnetorch.cpp \
                ../orchagent/dtelorch.cpp \
                ../orchagent/flexcounterorch.cpp \
                ../orchagent/watermarkorch.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_LDADD = $(LDADD_GTEST) $(LDADD_SAI) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main
