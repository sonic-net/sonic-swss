INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = intfsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

intfsyncd_SOURCES = intfsyncd.cpp intfsync.cpp

intfsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
intfsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
intfsyncd_LDADD = -lnl-3 -lnl-route-3 -lhiredis -lswsscommon

