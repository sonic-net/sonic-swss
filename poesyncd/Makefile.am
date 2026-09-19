INCLUDES = -I $(top_srcdir)/lib -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(top_srcdir)/cfgmgr

bin_PROGRAMS = poesyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

poesyncd_SOURCES = $(top_srcdir)/lib/poecfg.cpp poesyncd.cpp poeparser.cpp

poesyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(CFLAGS_ASAN)

poesyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS)

if GCOV_ENABLED
poesyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
poesyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

