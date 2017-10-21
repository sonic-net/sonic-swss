INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/cfgorch

bin_PROGRAMS = vlanconfd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vlanconfd_SOURCES = vlanconfd.cpp vlanconf.cpp switchconfvlan.cpp $(top_srcdir)/cfgorch/cfgorch.cpp

vlanconfd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
vlanconfd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
vlanconfd_LDADD = -lswsscommon
