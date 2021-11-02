INCLUDES = -I $(top_srcdir)

bin_PROGRAMS = tlm_teamd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

tlm_teamd_SOURCES = main.cpp teamdctl_mgr.cpp values_store.cpp

tlm_teamd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
tlm_teamd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(JANSSON_CFLAGS)
tlm_teamd_LDADD = -lhiredis -lswsscommon -lteamdctl $(JANSSON_LIBS)
