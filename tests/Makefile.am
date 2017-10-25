CFLAGS_SAI = -I /usr/include/sai
INCLUDES = -I ../orchagent  -I $(top_srcdir)/cfgorch

bin_PROGRAMS = tests

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

CFLAGS_GTEST = -I $(top_srcdir)/../sonic-swss-common/googletest/googletest/include
LDADD_GTEST = $(top_srcdir)/../sonic-swss-common/googletest/build/googlemock/gtest/libgtest_main.a \
        $(top_srcdir)/../sonic-swss-common/googletest/build/googlemock/gtest/libgtest.a

tests_SOURCES = swssnet_ut.cpp cfgorch_ut.cpp $(top_srcdir)/cfgorch/cfgorch.cpp

tests_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_GTEST) $(CFLAGS_SAI)
tests_LDADD = $(LDADD_GTEST) -lnl-genl-3 -lhiredis -lhiredis -lpthread \
        -lswsscommon -lswsscommon
