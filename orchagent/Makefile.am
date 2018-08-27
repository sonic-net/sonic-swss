INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/warmrestart

CFLAGS_SAI = -I /usr/include/sai

swssdir = $(datadir)/swss

dist_swss_DATA = \
		 pfc_detect_mellanox.lua  \
		 pfc_detect_broadcom.lua \
		 pfc_detect_barefoot.lua \
		 pfc_restore.lua

bin_PROGRAMS = orchagent routeresync

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

orchagent_SOURCES = \
            main.cpp \
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
            dtelorch.cpp \
            flexcounterorch.cpp \
            acltable.h \
            aclorch.h \
            bufferorch.h \
            copporch.h \
            directory.h \
            fdborch.h \
            intfsorch.h \
            mirrororch.h \
            neighorch.h \
            notifications.h \
            observer.h \
            orch.h \
            orchdaemon.h \
            pfcactionhandler.h \
            pfcwdorch.h \
            port.h \
            portsorch.h \
            qosorch.h \
            routeorch.h \
            saihelper.h \
            switchorch.h \
            swssnet.h \
            tunneldecaporch.h \
            crmorch.h        \
            request_parser.h \
            vrforch.h \
            dtelorch.h \
            countercheckorch.h \
            vxlanorch.h \
            flexcounterorch.h   \
            $(top_srcdir)/warmrestart/warm_restart.cpp \
            $(top_srcdir)/warmrestart/warm_restart.h

orchagent_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_LDADD = -lnl-3 -lnl-route-3 -lpthread -lsairedis -lswsscommon -lsaimetadata

routeresync_SOURCES = routeresync.cpp
routeresync_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_LDADD = -lswsscommon
