INCLUDES = -I $(top_srcdir) -I$(top_srcdir)/lib

bin_PROGRAMS = tlm_teamd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

tlm_teamd_SOURCES = main.cpp teamdctl_mgr.cpp values_store.cpp

tlm_teamd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_ASAN)
tlm_teamd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(JANSSON_CFLAGS) $(CFLAGS_ASAN)
tlm_teamd_LDADD = $(LDFLAGS_ASAN) -lhiredis -lswsscommon -lteamdctl $(JANSSON_LIBS)

if GCOV_ENABLED
tlm_teamd_LDADD += -lgcovpreload
endif

if ASAN_ENABLED
tlm_teamd_SOURCES += $(top_srcdir)/lib/asan.cpp
endif

