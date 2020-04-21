INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent -I $(top_srcdir)/orchagent/flex_counter

CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = mclagsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

mclagsyncd_SOURCES = mclagsyncd.cpp mclaglink.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp

mclagsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
mclagsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
mclagsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon 
