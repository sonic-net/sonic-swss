INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

bin_PROGRAMS = fdbsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fdbsyncd_SOURCES = fdbsyncd.cpp fdbsync.cpp $(top_srcdir)/warmrestart/warmRestartAssist.cpp

fdbsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(CFLAGS_ASAN)
fdbsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS) $(CFLAGS_ASAN)
fdbsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS)

if GCOV_ENABLED
fdbsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
fdbsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

