MAKEFLAGS = --no-print-directory

if GCOV_ENABLED
SUBDIRS = gcovpreload
else
SUBDIRS =
endif

SUBDIRS += orchagent cfgmgr neighsyncd fpmsyncd portsyncd fdbsyncd swssconfig

if BUILD_TESTS
SUBDIRS += tests
endif

if INCLUDE_NAT
SUBDIRS += natsyncd
endif

if INCLUDE_MLAG
SUBDIRS += mclagsyncd
endif

if INCLUDE_GEARSYNCD
SUBDIRS += gearsyncd
endif

if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
