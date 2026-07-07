INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart $(FPM_INCLUDES) -I $(top_srcdir)/lib

bin_PROGRAMS = fpmsyncd fpmsyncd_restart_check

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

fpmsyncd_SOURCES = fpmsyncd.cpp fpmlink.cpp routesync.cpp warmreboot_handler.cpp \
                    $(top_srcdir)/warmrestart/warmRestartHelper.cpp \
                    $(top_srcdir)/lib/orch_zmq_config.cpp

fpmsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon

fpmsyncd_restart_check_SOURCES = fpmsyncd_restart_check.cpp
fpmsyncd_restart_check_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_restart_check_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
fpmsyncd_restart_check_LDADD = $(LDFLAGS_ASAN) -lhiredis -lswsscommon -lpthread

if GCOV_ENABLED
fpmsyncd_SOURCES += ../gcovpreload/gcovpreload.cpp
fpmsyncd_restart_check_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
fpmsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
fpmsyncd_restart_check_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

