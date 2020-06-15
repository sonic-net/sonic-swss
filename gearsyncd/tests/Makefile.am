INCLUDES = -I.. -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(top_srcdir)/cfgmgr

noinst_PROGRAMS = testphyparser testgearparser
noinst_LIBRARIES = libcunit.a

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

testphyparser_SOURCES = testphyparser.cpp ../gearparserbase.cpp ../phyparser.cpp $(top_srcdir)/cfgmgr/shellcmd.h 
testgearparser_SOURCES = testgearparser.cpp ../gearparserbase.cpp ../gearboxparser.cpp ../phyparser.cpp $(top_srcdir)/cfgmgr/shellcmd.h 

libcunit_a_SOURCES = cunit/Automated.c cunit/Console.c cunit/MyMem.c cunit/TestRun.c cunit/Basic.c cunit/CUError.c cunit/TestDB.c cunit/Util.c

testphyparser_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(ASAN_CFLAGS)
testgearparser_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(ASAN_CFLAGS)

testphyparser_LDADD = -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS) $(ASAN_LDFLAGS) libcunit.a
testgearparser_LDADD = -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS) $(ASAN_LDFLAGS) libcunit.a
