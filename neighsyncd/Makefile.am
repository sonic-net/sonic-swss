INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = neighsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

neighsyncd_SOURCES = neighsyncd.cpp neighsync.cpp

neighsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
neighsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
neighsyncd_LDADD = -lnl-3 -lnl-route-3 -lhiredis -lswsscommon

