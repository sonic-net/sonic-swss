INCLUDES = -I $(top_srcdir)/lib -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(top_srcdir)/cfgmgr

bin_PROGRAMS = gearsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

gearsyncd_SOURCES = $(top_srcdir)/lib/gearboxutils.cpp gearsyncd.cpp gearparserbase.cpp gearboxparser.cpp phyparser.cpp $(top_srcdir)/cfgmgr/shellcmd.h

gearsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(CFLAGS_ASAN)

gearsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS)

if GCOV_ENABLED
gearsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
gearsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

