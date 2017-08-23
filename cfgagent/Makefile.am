INCLUDES = -I $(top_srcdir)
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = cfgagentd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

cfgagentd_SOURCES = cfgagentd.cpp vlancfgagent.cpp portcfgagent.cpp intfcfgagent.cpp switchcfgagent.cpp cfgorch.cpp

cfgagentd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
cfgagentd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
cfgagentd_LDADD = -lswsscommon

SUBDIRS = cfgmgrtest
