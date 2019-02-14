INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = mclagsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

mclagsyncd_SOURCES = mclagsyncd.cpp mclaglink.cpp

mclagsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
mclagsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
mclagsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon 
