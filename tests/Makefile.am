CFLAGS_SAI = -I /usr/include/sai
INCLUDES = -I ../orchagent

bin_PROGRAMS = tests

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST =
LDADD_GTEST =

tests_SOURCES = swssnet_ut.cpp request_parser_ut.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_LDADD = $(LDADD_GTEST) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon -lgtest -lgtest_main
