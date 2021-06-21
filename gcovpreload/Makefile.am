INCLUDES = -I $(top_srcdir) -I $(top_srcdir)/gcovpreload

lib_LTLIBRARIES = libgcovpreload.la

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

libgcovpreload_la_SOURCES = gcovpreload.cpp

libgcovpreload_la_CFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
libgcovpreload_la_CPPFLAGS = $(DBGFLAGS) $(AM_CFLAGS) $(CFLAGS_COMMON)
libgcovpreload_la_LDFLAGS = -shared
libgcovpreload_la_LIBADD = -lnl-3 -lnl-route-3 -lswsscommon -lpthread

all:
	sudo cp .libs/libgcovpreload.so.0.0.0 /usr/lib
	sudo ln -s libgcovpreload.so.0.0.0 /usr/lib/libgcovpreload.so