INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(top_srcdir)/cfgmgr

bin_PROGRAMS = portsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

portsyncd_SOURCES = portsyncd.cpp linksync.cpp  $(top_srcdir)/cfgmgr/shellcmd.h

portsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
portsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
portsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon

