INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = teamsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

teamsyncd_SOURCES = teamsyncd.cpp teamsync.cpp

teamsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
teamsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
teamsyncd_LDADD = -lnl-3 -lnl-route-3 -lhiredis -lteam \
	-L$(top_srcdir)/common -lswsscommon
