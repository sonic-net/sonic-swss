INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

bin_PROGRAMS = portsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

portsyncd_SOURCES = portsyncd.cpp linksync.cpp $(top_srcdir)/warmrestart/warm_restart.cpp $(top_srcdir)/warmrestart/warm_restart.h

portsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
portsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
portsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon

