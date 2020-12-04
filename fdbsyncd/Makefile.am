INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

bin_PROGRAMS = fdbsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fdbsyncd_SOURCES = fdbsyncd.cpp fdbsync.cpp $(top_srcdir)/warmrestart/warmRestartAssist.cpp

fdbsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS)
fdbsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(COV_CFLAGS)
fdbsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon $(COV_LDFLAGS)


