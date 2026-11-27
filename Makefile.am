if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd poesyncd
else
SUBDIRS = fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd poesyncd
endif


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
