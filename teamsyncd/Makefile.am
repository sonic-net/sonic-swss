INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = teamsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

teamsyncd_SOURCES = teamsyncd.cpp teamsync.cpp

teamsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
teamsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
teamsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lhiredis -lswsscommon -lteam

if GCOV_ENABLED
teamsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
teamsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

