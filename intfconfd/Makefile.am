INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/cfgorch

bin_PROGRAMS = intfconfd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

intfconfd_SOURCES = intfconfd.cpp intfconf.cpp $(top_srcdir)/cfgorch/cfgorch.cpp

intfconfd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
intfconfd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
intfconfd_LDADD = -lswsscommon
