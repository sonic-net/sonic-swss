INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = portsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

portsyncd_SOURCES = $(top_srcdir)/lib/gearboxutils.cpp portsyncd.cpp linksync.cpp  $(top_srcdir)/cfgmgr/shellcmd.h

portsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
portsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
portsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon

if GCOV_ENABLED
portsyncd_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
portsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

