INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart  -I /usr/include/sai

bin_PROGRAMS = bfdsyncd

CFLAGS_SAI = -I /usr/include/sai

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

bfdsyncd_SOURCES = bfdsyncd.cpp bfdlink.cpp $(top_srcdir)/warmrestart/warmRestartHelper.cpp

bfdsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
bfdsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
bfdsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon

if GCOV_ENABLED
bfdsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
bfdsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

