INCLUDES = -I $(top_srcdir)/lib \
           -I $(top_srcdir) \
           -I $(top_srcdir)/warmrestart \
           -I switch \
           -I flex_counter \
           -I debug_counter \
           -I port \
           -I pbh \
           -I nhg

if INCLUDE_P4RT
SUBDIRS = p4orch/tests
else
SUBDIRS =
endif

CFLAGS_SAI = -I /usr/include/sai

BUILT_SOURCES = swss_pch.h.gch
CLEANFILES = swss_pch.h.gch

PCH_CPPFLAGS = $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
               $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) \
               $(CFLAGS_ASAN) $(CPPFLAGS) $(CXXFLAGS)

swss_pch.h.gch: $(srcdir)/swss_pch.h
	@echo "  [PCH]    $<"
	@$(CXX) $(PCH_CPPFLAGS) -x c++-header -o $@ $<

swssdir = $(datadir)/swss

dist_swss_DATA = \
		 nvda_port_trim_drop.lua \
		 eliminate_events.lua \
		 rif_rates.lua \
		 pfc_detect_marvell-teralynx.lua  \
		 pfc_detect_mellanox.lua  \
		 pfc_detect_broadcom.lua \
		 pfc_detect_marvell-prestera.lua \
		 pfc_detect_barefoot.lua \
		 pfc_detect_nephos.lua \
		 pfc_detect_cisco-8000.lua \
		 pfc_detect_vs.lua \
		 pfc_restore.lua \
		 pfc_restore_cisco-8000.lua \
		 port_rates.lua \
		 port_flr.lua \
		 drop_monitor.lua \
		 watermark_queue.lua \
		 watermark_pg.lua \
		 watermark_bufferpool.lua \
		 lagids.lua \
		 tunnel_rates.lua \
		 trap_rates.lua

bin_PROGRAMS = orchagent routeresync orchagent_restart_check
noinst_LIBRARIES = libswssorchagent.a

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

ORCH_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN) \
                -include $(srcdir)/swss_pch.h -Winvalid-pch

libswssorchagent_a_SOURCES = \
            orchdaemon.cpp \
            orch.cpp \
            notifications.cpp \
            nhgorch.cpp \
            nhgbase.cpp \
            routeorch.cpp \
            neighorch.cpp \
            intfsorch.cpp \
            port/port_capabilities.cpp \
            port/porthlpr.cpp \
            portsorch.cpp \
            fabricportsorch.cpp \
            copporch.cpp \
            qosorch.cpp \
            buffer/bufferhelper.cpp \
            bufferorch.cpp \
            mirrororch.cpp \
            fdborch.cpp \
            aclorch.cpp \
            saihelper.cpp \
            saiattr.cpp \
            switch/switch_capabilities.cpp \
            switch/switch_helper.cpp \
            switch/trimming/capabilities.cpp \
            switch/trimming/helper.cpp \
            switchorch.cpp \
            pfcwdorch.cpp \
            pfcactionhandler.cpp \
            crmorch.cpp \
            request_parser.cpp \
            vrforch.cpp \
            countercheckorch.cpp \
            vxlanorch.cpp \
            tunneltermhelper.cpp \
            vnetorch.cpp \
            flexcounterorch.cpp \
            watermarkorch.cpp \
            policerorch.cpp \
            lagid.cpp \
            bfdorch.cpp \
            response_publisher.cpp \
            zmqorch.cpp \
            arsorch.cpp \
            nexthopkey.cpp \
            high_frequency_telemetry/hftelorch.cpp \
            high_frequency_telemetry/hftelprofile.cpp \
            high_frequency_telemetry/counternameupdater.cpp \
            high_frequency_telemetry/hftelutils.cpp \
            high_frequency_telemetry/hftelgroup.cpp

libswssorchagent_a_SOURCES += flex_counter/flex_counter_manager.cpp flex_counter/flex_counter_stat_manager.cpp flex_counter/flow_counter_handler.cpp flex_counter/flowcounterrouteorch.cpp
libswssorchagent_a_SOURCES += debug_counter/debug_counter.cpp debug_counter/drop_counter.cpp

if INCLUDE_CBF
libswssorchagent_a_SOURCES += cbf/cbfnhgorch.cpp cbf/nhgmaporch.cpp
endif

if INCLUDE_FGNHG
libswssorchagent_a_SOURCES += fgnhgorch.cpp
endif

if INCLUDE_TUNNELDECAP
libswssorchagent_a_SOURCES += tunneldecaporch.cpp
endif

if INCLUDE_NAT
libswssorchagent_a_SOURCES += natorch.cpp
endif

if INCLUDE_MLAG
libswssorchagent_a_SOURCES += mlagorch.cpp
endif

if INCLUDE_MUX
libswssorchagent_a_SOURCES += muxorch.cpp
endif

if INCLUDE_MACSEC
libswssorchagent_a_SOURCES += macsecorch.cpp macsecpost.cpp
endif

if INCLUDE_DTEL
libswssorchagent_a_SOURCES += dtelorch.cpp
endif

if INCLUDE_STP
libswssorchagent_a_SOURCES += stporch.cpp
endif

if INCLUDE_ISOGRP
libswssorchagent_a_SOURCES += isolationgrouporch.cpp
endif

if INCLUDE_PBH
libswssorchagent_a_SOURCES += pbh/pbhcap.cpp \
            pbh/pbhcnt.cpp \
            pbh/pbhmgr.cpp \
            pbh/pbhrule.cpp \
            pbhorch.cpp
endif

if INCLUDE_NVGRE
libswssorchagent_a_SOURCES += nvgreorch.cpp
endif

if INCLUDE_TWAMP
libswssorchagent_a_SOURCES += twamporch.cpp
endif

if INCLUDE_ICMP
libswssorchagent_a_SOURCES += icmporch.cpp
endif

if INCLUDE_DASH
libswssorchagent_a_SOURCES += dash/dashenifwdorch.cpp \
            dash/dashenifwdinfo.cpp \
            dash/dashorch.cpp \
            dash/dashrouteorch.cpp \
            dash/dashvnetorch.cpp \
            dash/dashaclorch.cpp \
            dash/dashaclgroupmgr.cpp \
            dash/dashmeterorch.cpp \
            dash/dashtagmgr.cpp \
            dash/dashtunnelorch.cpp \
            dash/pbutils.cpp \
            dash/dashhaorch.cpp \
            dash/dashportmaporch.cpp
endif

if INCLUDE_SRV6
libswssorchagent_a_SOURCES += srv6orch.cpp
endif

if INCLUDE_MPLS
libswssorchagent_a_SOURCES += mplsrouteorch.cpp
endif

if INCLUDE_CHASSIS
libswssorchagent_a_SOURCES += chassisorch.cpp
endif

if INCLUDE_P4RT
libswssorchagent_a_SOURCES += p4orch/p4orch.cpp \
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
endif

libswssorchagent_a_CPPFLAGS = $(ORCH_CPPFLAGS)

orchagent_SOURCES = \
            main.cpp \
            $(top_srcdir)/lib/gearboxutils.cpp \
            $(top_srcdir)/lib/subintf.cpp \
            $(top_srcdir)/lib/recorder.cpp \
            $(top_srcdir)/lib/orch_zmq_config.cpp

orchagent_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
orchagent_CPPFLAGS = $(ORCH_CPPFLAGS)
orchagent_LDADD = libswssorchagent.a $(LDFLAGS_ASAN) -lnl-3 -lnl-route-3 -lpthread -lsairedis -lsaimeta -lsaimetadata -lswsscommon -lzmq -lprotobuf -ljemalloc

if INCLUDE_DASH
orchagent_LDADD += -ldashapi
endif

if INCLUDE_SFLOW
libswssorchagent_a_SOURCES += sfloworch.cpp
endif

if INCLUDE_DEBUG_COUNTER
libswssorchagent_a_SOURCES += debugcounterorch.cpp
endif

if INCLUDE_TAM
libswssorchagent_a_SOURCES += tamorch.cpp
endif


routeresync_SOURCES = routeresync.cpp \
             $(top_srcdir)/lib/orch_zmq_config.cpp
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
