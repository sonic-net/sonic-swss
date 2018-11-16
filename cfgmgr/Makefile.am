INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent -I $(top_srcdir)/warmrestart
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = vlanmgrd teammgrd portmgrd intfmgrd buffermgrd vrfmgrd nbrmgrd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vlanmgrd_SOURCES = vlanmgrd.cpp vlanmgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
vlanmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanmgrd_LDADD = -lswsscommon

teammgrd_SOURCES = teammgrd.cpp teammgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
teammgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
teammgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
teammgrd_LDADD = -lswsscommon

portmgrd_SOURCES = portmgrd.cpp portmgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
portmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
portmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
portmgrd_LDADD = -lswsscommon

intfmgrd_SOURCES = intfmgrd.cpp intfmgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
intfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfmgrd_LDADD = -lswsscommon

buffermgrd_SOURCES = buffermgrd.cpp buffermgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
buffermgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
buffermgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
buffermgrd_LDADD = -lswsscommon

vrfmgrd_SOURCES = vrfmgrd.cpp vrfmgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
vrfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vrfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vrfmgrd_LDADD = -lswsscommon

nbrmgrd_SOURCES = nbrmgrd.cpp nbrmgr.cpp $(top_srcdir)/orchagent/orch.cpp $(top_srcdir)/orchagent/request_parser.cpp shellcmd.h
nbrmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
nbrmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
nbrmgrd_LDADD = -lswsscommon