if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd dashoffloadmanager
else
SUBDIRS = fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd dashoffloadmanager
endif


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
