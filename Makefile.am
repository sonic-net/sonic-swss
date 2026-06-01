MAKEFLAGS = --no-print-directory

if GCOV_ENABLED
SUBDIRS = gcovpreload orchagent cfgmgr tests fpmsyncd neighsyncd portsyncd \
          mclagsyncd natsyncd fdbsyncd swssconfig gearsyncd
else
SUBDIRS = orchagent cfgmgr tests fpmsyncd neighsyncd portsyncd \
          mclagsyncd natsyncd fdbsyncd swssconfig gearsyncd
endif

if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
