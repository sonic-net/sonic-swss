INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = mclagsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

mclagsyncd_SOURCES = mclagsyncd.cpp mclaglink.cpp

mclagsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
mclagsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
mclagsyncd_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lswsscommon

if GCOV_ENABLED
mclagsyncd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
mclagsyncd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

