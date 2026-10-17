INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I $(FPM_PATH) -I $(top_srcdir)/lib

bin_PROGRAMS = fpmsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fpmsyncd_SOURCES = fpmsyncd.cpp fpmlink.cpp routesync.cpp routesendcoalescer.cpp $(top_srcdir)/warmrestart/warmRestartHelper.cpp \
                    $(top_srcdir)/lib/orch_zmq_config.cpp

fpmsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon -ljemalloc

check-local: fpmsyncd
	@readelf -d fpmsyncd | grep -q libjemalloc || \
	    { echo "fpmsyncd does not retain libjemalloc"; exit 1; }
	@readelf --dyn-syms -W fpmsyncd | \
	    awk '$$4 == "OBJECT" && $$5 == "GLOBAL" && $$6 == "DEFAULT" && $$7 != "UND" && $$8 == "malloc_conf" { found=1 } END { exit !found }' || \
	    { echo "fpmsyncd does not export malloc_conf"; exit 1; }
	@strings fpmsyncd | grep -Fxq "background_thread:true" || \
	    { echo "fpmsyncd jemalloc configuration is missing"; exit 1; }

if GCOV_ENABLED
fpmsyncd_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
fpmsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp $(top_srcdir)/lib/asan_ctor.cpp
endif
