INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = swssconfig

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

swssconfig_SOURCES = swssconfig.cpp

swssconfig_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssconfig_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssconfig_LDADD = -lnl-3 -lnl-route-3 -lhiredis -lswsscommon
