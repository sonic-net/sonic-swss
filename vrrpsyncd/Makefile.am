INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/cfgmgr -I $(top_srcdir)/lib

bin_PROGRAMS = vrrpsyncd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vrrpsyncd_SOURCES = vrrpsyncd.cpp vrrpsync.cpp $(top_srcdir)/cfgmgr/shellcmd.h $(top_srcdir)/lib/vrrpintf.cpp 

vrrpsyncd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
vrrpsyncd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
vrrpsyncd_LDADD = -lnl-3 -lnl-route-3 -lswsscommon

if GCOV_ENABLED
vrrpsyncd_LDADD += -lgcovpreload
endif
