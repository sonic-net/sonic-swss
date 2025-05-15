INCLUDES = -I $(top_srcdir) -I$(top_srcdir)/lib

bin_PROGRAMS = swssconfig swssplayer

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

swssconfig_SOURCES = swssconfig.cpp

swssconfig_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
swssconfig_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
swssconfig_LDADD = $(LDFLAGS_ASAN) -lswsscommon

swssplayer_SOURCES = swssplayer.cpp

swssplayer_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
swssplayer_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
swssplayer_LDADD = $(LDFLAGS_ASAN) -lswsscommon

if GCOV_ENABLED
swssconfig_SOURCES += ../gcovpreload/gcovpreload.cpp
swssplayer_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
swssconfig_SOURCES += $(top_srcdir)/lib/asan.cpp
swssplayer_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

swssconfig_SOURCES += $(top_srcdir)/lib/orch_zmq_config.cpp
swssplayer_SOURCES += $(top_srcdir)/lib/orch_zmq_config.cpp
