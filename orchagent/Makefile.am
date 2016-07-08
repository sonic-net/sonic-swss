INCLUDES = -I $(top_srcdir)

CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = orchagent routeresync

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

if SAITHRIFT
SAI_RPC_INCLUDE_DIR = ../libsaiserver/include
SAI_RPC_LIB_DIR = ../libsaiserver/lib
CFLAGS_SAI += -I$(SAI_RPC_INCLUDE_DIR) -DSAITHRIFT=yes
endif

orchagent_SOURCES = main.cpp orchdaemon.cpp orch.cpp routeorch.cpp neighorch.cpp intfsorch.cpp portsorch.cpp

orchagent_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
orchagent_LDADD = -lnl-3 -lnl-route-3 -lpthread -lsairedis -lswsscommon

if SAITHRIFT
orchagent_LDADD += -L$(SAI_RPC_LIB_DIR) -lrpcserver -lthrift
endif

routeresync_SOURCES = routeresync.cpp
routeresync_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
routeresync_LDADD = -lswsscommon
