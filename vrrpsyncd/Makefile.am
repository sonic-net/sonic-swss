INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

bin_PROGRAMS = vrrpsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vrrpsyncd_SOURCES = vrrpsyncd.cpp vrrpsync.cpp $(top_srcdir)/warmrestart/warmRestartAssist.cpp

vrrpsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(SONIC_COMMON_CFLAGS)
vrrpsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(SONIC_COMMON_CFLAGS)
vrrpsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon $(SONIC_COMMON_LDFLAGS) 
