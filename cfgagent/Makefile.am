INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/orchagent
CFLAGS_SAI = -I /usr/include/sai

bin_PROGRAMS = vlanmgrd

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

vlanmgrd_SOURCES = vlanmgrd.cpp vlanmgr.cpp $(top_srcdir)/orchagent/orchbase.cpp shellcmd.h
vlanmgrd_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanmgrd_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON) $(CFLAGS_SAI)
vlanmgrd_LDADD = -lswsscommon

