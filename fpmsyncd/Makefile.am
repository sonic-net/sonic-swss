INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(FPM_PATH)

bin_PROGRAMS = fpmsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fpmsyncd_SOURCES = fpmsyncd.cpp fpmlink.cpp routesync.cpp errfpmroute.cpp errfpmroute.h $(top_srcdir)/warmrestart/warmRestartHelper.cpp $(top_srcdir)/warmrestart/warmRestartHelper.h 

fpmsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
fpmsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
fpmsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon
