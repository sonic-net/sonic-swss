INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = cfgmgr

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif


cfgmgr_SOURCES = cfgmgr.cpp vlancfgmgr.cpp portcfgmgr.cpp \
						lagcfgmgr.cpp intfcfgmgr.cpp
cfgmgr_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
cfgmgr_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
cfgmgr_LDADD =  -lswsscommon