INCLUDES = -I $(top_srcdir)

CFLAGS_SAI = -I /usr/include/sai/

swssdir = $(datadir)/swss

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g -DNDEBUG
endif

noinst_LTLIBRARIES = libflexcounter.la
libflexcounter_la_SOURCES = flex_counter_manager.cpp dynamic_flex_counter_manager.cpp
libflexcounter_la_CXXFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
libflexcounter_la_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
libflexcounter_la_LIBADD = -lswsscommon -lsaimetadata
