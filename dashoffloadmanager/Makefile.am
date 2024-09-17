INCLUDES = -I $(top_srcdir)/lib -I $(top_srcdir) -I $(top_srcdir)/orchagent -I $(top_srcdir)/orchagent/flex_counter
CFLAGS_SAI = -I /usr/include/sai
DASH_LIBS = -lprotobuf -ldashapi
MISC_LIBS = -lzmq -lswsscommon -l:libboost_program_options.a
SAIMETA_LIBS = -lsaimeta -lsaimetadata

COMMON_ORCH_SOURCE = $(top_srcdir)/orchagent/orch.cpp \
				$(top_srcdir)/orchagent/zmqorch.cpp \
				$(top_srcdir)/orchagent/request_parser.cpp \
				$(top_srcdir)/orchagent/response_publisher.cpp \
				$(top_srcdir)/lib/recorder.cpp

bin_PROGRAMS = dashoffloadmanager

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

dashoffloadmanager_SOURCES = main.cpp dpuinfoprovider.cpp dashoffloadmanager.cpp dashoffloadpavalidation.cpp $(COMMON_ORCH_SOURCE)
dashoffloadmanager_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI) $(CFLAGS_ASAN)
dashoffloadmanager_LDADD = $(LDFLAGS_ASAN) $(MISC_LIBS) $(SAIMETA_LIBS) $(DASH_LIBS)

if GCOV_ENABLED
dashoffloadmanager_SOURCES += ../gcovpreload/gcovpreload.cpp
endif

if ASAN_ENABLED
dashoffloadmanager_SOURCES += $(top_srcdir)/lib/asan.cpp
endif
