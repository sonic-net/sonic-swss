INCLUDES = -I$(top_srcdir)/lib -I $(top_srcdir) -I $(top_srcdir)/orchagent -I $(top_srcdir)/warmrestart -I $(top_srcdir)/orchagent/flex_counter
CFLAGS_SAI = -I /usr/include/sai
LIBNL_CFLAGS = -I/usr/include/libnl3
LIBNL_LIBS = -lnl-genl-3 -lnl-route-3 -lnl-3
SAIMETA_LIBS = -lsaimeta -lsaimetadata -lzmq
COMMON_LIBS = -lswsscommon -lpthread

bin_PROGRAMS = vlanmgrd teammgrd portmgrd intfmgrd buffermgrd vrfmgrd nbrmgrd vxlanmgrd sflowmgrd natmgrd coppmgrd tunnelmgrd macsecmgrd fabricmgrd stpmgrd

cfgmgrdir = $(datadir)/swss

dist_cfgmgr_DATA = \
		buffer_check_headroom_mellanox.lua \
		buffer_headroom_mellanox.lua \
		buffer_pool_mellanox.lua \
		buffer_check_headroom_vs.lua \
		buffer_headroom_vs.lua \
		buffer_pool_vs.lua \
		buffer_check_headroom_barefoot.lua \
		buffer_headroom_barefoot.lua \
		buffer_pool_barefoot.lua

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

COMMON_ORCH_SOURCE = $(top_srcdir)/orchagent/orch.cpp \
				$(top_srcdir)/orchagent/request_parser.cpp \
				$(top_srcdir)/orchagent/response_publisher.cpp \
				$(top_srcdir)/lib/recorder.cpp

vlanmgrd_SOURCES = vlanmgrd.cpp vlanmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
vlanmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vlanmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vlanmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

teammgrd_SOURCES = teammgrd.cpp teammgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
teammgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
teammgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
teammgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

portmgrd_SOURCES = portmgrd.cpp portmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
portmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
portmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
portmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

fabricmgrd_SOURCES = fabricmgrd.cpp fabricmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
fabricmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
fabricmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
fabricmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

intfmgrd_SOURCES = intfmgrd.cpp intfmgr.cpp $(top_srcdir)/lib/subintf.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
intfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
intfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
intfmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

buffermgrd_SOURCES = buffermgrd.cpp buffermgr.cpp buffermgrdyn.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
buffermgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
buffermgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
buffermgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

vrfmgrd_SOURCES = vrfmgrd.cpp vrfmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
vrfmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vrfmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vrfmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

nbrmgrd_SOURCES = nbrmgrd.cpp nbrmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
nbrmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(LIBNL_CFLAGS) $(CFLAGS_ASAN)
nbrmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(LIBNL_CPPFLAGS) $(CFLAGS_ASAN)
nbrmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS) $(LIBNL_LIBS)

vxlanmgrd_SOURCES = vxlanmgrd.cpp vxlanmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
vxlanmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vxlanmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
vxlanmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

sflowmgrd_SOURCES = sflowmgrd.cpp sflowmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
sflowmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
sflowmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
sflowmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

natmgrd_SOURCES = natmgrd.cpp natmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
natmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
natmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
natmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

coppmgrd_SOURCES = coppmgrd.cpp coppmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
coppmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
coppmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
coppmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

tunnelmgrd_SOURCES = tunnelmgrd.cpp tunnelmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
tunnelmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
tunnelmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
tunnelmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

macsecmgrd_SOURCES = macsecmgrd.cpp macsecmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
macsecmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
macsecmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
macsecmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)


stpmgrd_SOURCES = stpmgrd.cpp stpmgr.cpp $(COMMON_ORCH_SOURCE) shellcmd.h
stpmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
stpmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
stpmgrd_LDADD = $(LDFLAGS_ASAN) $(COMMON_LIBS) $(SAIMETA_LIBS)

if GCOV_ENABLED
vlanmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
teammgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
portmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
fabricmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
intfmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
buffermgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
vrfmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
nbrmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
vxlanmgrd_SOURCES +=  ../gcovpreload/gcovpreload.cpp
sflowmgrd_SOURCES +=  ../gcovpreload/gcovpreload.cpp
natmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
coppmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
tunnelmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
macsecmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
stpmgrd_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
vlanmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
teammgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
portmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
intfmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
buffermgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
vrfmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
nbrmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
vxlanmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
sflowmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
natmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
coppmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
tunnelmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
macsecmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
fabricmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
stpmgrd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

