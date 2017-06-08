INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = swssconfig swssplayer

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

swssconfig_SOURCES = swssconfig.cpp

swssconfig_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssconfig_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssconfig_LDADD = -lswsscommon

swssplayer_SOURCES = swssplayer.cpp

swssplayer_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssplayer_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
swssplayer_LDADD = -lswsscommon
