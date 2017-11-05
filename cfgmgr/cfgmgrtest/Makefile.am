INCLUDES = -I $(top_srcdir)  -I $(top_srcdir)/orchagent -I $(top_srcdir)/cfgmgr
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = cfgmgr

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif


cfgmgr_SOURCES = cfgmgr.cpp vlancfgmgr.cpp intfcfgmgr.cpp switchcfgmgr.cpp
cfgmgr_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
cfgmgr_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
cfgmgr_LDADD =  -lswsscommon
