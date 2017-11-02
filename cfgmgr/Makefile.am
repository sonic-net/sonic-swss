INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = intfmgrd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif



intfmgrd_SOURCES = intfmgrd.cpp intfmgr.cpp $(top_srcdir)/orchagent/orch.cpp shellcmd.h
intfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfmgrd_LDADD = -lswsscommon

SUBDIRS = cfgmgrtest
