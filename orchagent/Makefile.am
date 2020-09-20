INCLUDES = -I $(top_srcdir)/lib -I $(top_srcdir) -I $(top_srcdir)/warmrestart -I flex_counter -I debug_counter

CFLAGS_SAI = -I /usr/include/sai

swssdir = $(datadir)/swss

dist_swss_DATA = \
		 rif_rates.lua \
		 pfc_detect_innovium.lua  \
		 pfc_detect_mellanox.lua  \
		 pfc_detect_broadcom.lua \
		 pfc_detect_barefoot.lua \
		 pfc_detect_nephos.lua \
		 pfc_restore.lua \
		 port_rates.lua \
		 watermark_queue.lua \
		 watermark_pg.lua \
		 watermark_bufferpool.lua

bin_PROGRAMS = orchagent routeresync orchagent_restart_check

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

orchagent_SOURCES = \
            main.cpp \
            $(top_srcdir)/lib/gearboxutils.cpp \
            orchdaemon.cpp \
            orch.cpp \
            notifications.cpp \
            routeorch.cpp \
            neighorch.cpp \
            intfsorch.cpp \
            portsorch.cpp \
            copporch.cpp \
            tunneldecaporch.cpp \
            qosorch.cpp \
            bufferorch.cpp \
            mirrororch.cpp \
            fdborch.cpp \
            aclorch.cpp \
            saihelper.cpp \
            switchorch.cpp \
            pfcwdorch.cpp \
            pfcactionhandler.cpp \
            crmorch.cpp \
            request_parser.cpp \
            vrforch.cpp \
            countercheckorch.cpp \
            vxlanorch.cpp \
            vnetorch.cpp \
            dtelorch.cpp \
            flexcounterorch.cpp \
            watermarkorch.cpp \
            policerorch.cpp \
            sfloworch.cpp \
            chassisorch.cpp \
            debugcounterorch.cpp \
            natorch.cpp

orchagent_SOURCES += flex_counter/flex_counter_manager.cpp flex_counter/flex_counter_stat_manager.cpp
orchagent_SOURCES += debug_counter/debug_counter.cpp debug_counter/drop_counter.cpp

orchagent_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_LDADD = -lnl-3 -lnl-route-3 -lpthread -lsairedis -lswsscommon -lsaimeta -lsaimetadata -lzmq

routeresync_SOURCES = routeresync.cpp
routeresync_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_LDADD = -lswsscommon

orchagent_restart_check_SOURCES = orchagent_restart_check.cpp
orchagent_restart_check_CPPFLAGS = $(DBGFLAGS) $(AM_CPPFLAGS) $(CFLAGS_COMMON)
orchagent_restart_check_LDADD = -lhiredis -lswsscommon -lpthread
