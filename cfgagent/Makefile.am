INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = vlanconfd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vlanconfd_SOURCES = vlanconfd.cpp vlanconf.cpp switchconf.cpp $(top_srcdir)/orchagent/orchbase.cpp
vlanconfd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanconfd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanconfd_LDADD = -lswsscommon