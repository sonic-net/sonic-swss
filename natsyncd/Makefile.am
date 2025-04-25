INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = natsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

natsyncd_SOURCES = natsyncd.cpp natsync.cpp $(top_srcdir)/warmrestart/warmRestartAssist.cpp

natsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
natsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
natsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lnl-nf-3 -lswsscommon

if GCOV_ENABLED
natsyncd_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
natsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

