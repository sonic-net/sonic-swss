INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

bin_PROGRAMS = neighsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

neighsyncd_SOURCES = neighsyncd.cpp neighsync.cpp $(top_srcdir)/warmrestart/warmRestartAssist.cpp

neighsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
neighsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
neighsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon

if GCOV_ENABLED
neighsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
neighsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

