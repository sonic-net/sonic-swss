INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(FPM_PATH)

bin_PROGRAMS = fpmsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fpmsyncd_SOURCES = fpmsyncd.cpp fpmlink.cpp routesync.cpp $(top_srcdir)/warmrestart/warmRestartHelper.cpp

fpmsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon -lyaml-cpp

if GCOV_ENABLED
fpmsyncd_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
fpmsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

