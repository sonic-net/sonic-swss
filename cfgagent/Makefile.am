INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = intfconfd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif


intfconfd_SOURCES = intfconfd.cpp intfconf.cpp $(top_srcdir)/orchagent/orchbase.cpp
intfconfd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfconfd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
intfconfd_LDADD = -lswsscommon

