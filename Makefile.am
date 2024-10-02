if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd vrrpsyncd
else
SUBDIRS = fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd vrrpsyncd 
endif


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
