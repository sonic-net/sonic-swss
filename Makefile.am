if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd bfdsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd
else
SUBDIRS = fpmsyncd bfdsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd 
endif


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
