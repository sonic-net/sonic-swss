INCLUDES = -I $(top_srcdir)/lib

CFLAGS_SAI = -I /usr/include/sai

TESTS = tests

if !HAVE_SAI
SUBDIRS = mock_tests
endif

noinst_PROGRAMS = tests

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST = -L/usr/src/gtest

tests_SOURCES = swssnet_ut.cpp request_parser_ut.cpp ../orchagent/request_parser.cpp            \
        quoted_ut.cpp ../lib/recorder.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI) -I../orchagent
tests_LDADD = $(LDADD_GTEST) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lyaml-cpp -lgtest -lgtest_main
