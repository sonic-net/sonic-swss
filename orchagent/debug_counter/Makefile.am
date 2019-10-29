INCLUDES = -I $(top_srcdir)

CFLAGS_SAI = -I /usr/include/sai/

swssdir = $(datadir)/swss

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

noinst_LTLIBRARIES = libdebugcounter.la
libdebugcounter_la_SOURCES = debug_counter.cpp drop_counter.cpp
libdebugcounter_la_CXXFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
libdebugcounter_la_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
libdebugcounter_la_LIBADD = -lswsscommon -lsaimetadata
