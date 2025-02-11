INCLUDES = -I $(top_srcdir)/lib \
           -I $(top_srcdir) \
           -I $(top_srcdir)/warmrestart \
           -I switch \
           -I flex_counter \
           -I debug_counter \
           -I port \
           -I pbh \
           -I nhg

if GCOV_ENABLED
SUBDIRS = p4orch/tests
endif

CFLAGS_SAI = -I /usr/include/sai

swssdir = $(datadir)/swss

dist_swss_DATA = \
		 eliminate_events.lua \
		 rif_rates.lua \
		 pfc_detect_innovium.lua  \
		 pfc_detect_mellanox.lua  \
		 pfc_detect_broadcom.lua \
		 pfc_detect_marvell.lua \
		 pfc_detect_barefoot.lua \
		 pfc_detect_nephos.lua \
		 pfc_detect_cisco-8000.lua \
		 pfc_detect_vs.lua \
		 pfc_restore.lua \
		 pfc_restore_cisco-8000.lua \
		 port_rates.lua \
		 watermark_queue.lua \
		 watermark_pg.lua \
		 watermark_bufferpool.lua \
		 lagids.lua \
		 tunnel_rates.lua \
		 trap_rates.lua

bin_PROGRAMS = orchagent routeresync orchagent_restart_check

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

orchagent_SOURCES = \
            main.cpp \
            $(top_srcdir)/lib/gearboxutils.cpp \
            $(top_srcdir)/lib/subintf.cpp \
            $(top_srcdir)/lib/recorder.cpp \
            orchdaemon.cpp \
            orch.cpp \
            notifications.cpp \
            nhgorch.cpp \
            nhgbase.cpp \
            cbf/cbfnhgorch.cpp  \
            cbf/nhgmaporch.cpp \
            routeorch.cpp \
            mplsrouteorch.cpp \
            neighorch.cpp \
            intfsorch.cpp \
            port/port_capabilities.cpp \
            port/porthlpr.cpp \
            portsorch.cpp \
            fabricportsorch.cpp \
            fgnhgorch.cpp \
            copporch.cpp \
            tunneldecaporch.cpp \
            qosorch.cpp \
            bufferorch.cpp \
            mirrororch.cpp \
            fdborch.cpp \
            aclorch.cpp \
            pbh/pbhcap.cpp \
            pbh/pbhcnt.cpp \
            pbh/pbhmgr.cpp \
            pbh/pbhrule.cpp \
            pbhorch.cpp \
            saihelper.cpp \
            saiattr.cpp \
            switch/switch_capabilities.cpp \
            switch/switch_helper.cpp \
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
            natorch.cpp \
            mlagorch.cpp \
            isolationgrouporch.cpp \
            muxorch.cpp \
            macsecorch.cpp \
            lagid.cpp \
            bfdorch.cpp \
            srv6orch.cpp \
            response_publisher.cpp \
            nvgreorch.cpp \
            zmqorch.cpp \
            dash/dashorch.cpp \
            dash/dashrouteorch.cpp \
            dash/dashvnetorch.cpp \
            dash/dashaclorch.cpp \
            dash/dashaclgroupmgr.cpp \
            dash/dashtagmgr.cpp \
            dash/pbutils.cpp \
            twamporch.cpp

orchagent_SOURCES += flex_counter/flex_counter_manager.cpp flex_counter/flex_counter_stat_manager.cpp flex_counter/flow_counter_handler.cpp flex_counter/flowcounterrouteorch.cpp
orchagent_SOURCES += debug_counter/debug_counter.cpp debug_counter/drop_counter.cpp
orchagent_SOURCES += p4orch/p4orch.cpp \
		     p4orch/p4orch_util.cpp \
		     p4orch/p4oidmapper.cpp \
 		     p4orch/tables_definition_manager.cpp \
		     p4orch/router_interface_manager.cpp \
		     p4orch/gre_tunnel_manager.cpp \
		     p4orch/neighbor_manager.cpp \
		     p4orch/next_hop_manager.cpp \
		     p4orch/route_manager.cpp \
		     p4orch/acl_util.cpp \
		     p4orch/acl_table_manager.cpp \
		     p4orch/acl_rule_manager.cpp \
		     p4orch/wcmp_manager.cpp \
		     p4orch/mirror_session_manager.cpp \
		     p4orch/l3_admit_manager.cpp \
 		     p4orch/ext_tables_manager.cpp

orchagent_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
orchagent_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
orchagent_LDADD = $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lpthread -lsairedis -lsaimeta -lsaimetadata -lswsscommon -lzmq -lprotobuf -ldashapi -ljemalloc

routeresync_SOURCES = routeresync.cpp
routeresync_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
routeresync_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
routeresync_LDADD = $(LDFLAGS_ASAN) -lswsscommon

orchagent_restart_check_SOURCES = orchagent_restart_check.cpp
orchagent_restart_check_CPPFLAGS = $(DBGFLAGS) $(AM_CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
orchagent_restart_check_LDADD = $(LDFLAGS_ASAN) -lhiredis -lswsscommon -lpthread

if GCOV_ENABLED
orchagent_SOURCES += ../gcovpreload/gcovpreload.cpp
routeresync_SOURCES += ../gcovpreload/gcovpreload.cpp
orchagent_restart_check_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
orchagent_SOURCES += $(top_srcdir)/lib/asan.cpp
routeresync_SOURCES += $(top_srcdir)/lib/asan.cpp
orchagent_restart_check_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

