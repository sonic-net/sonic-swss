if GCOV_ENABLED
SUBDIRS = gcovpreload fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd
else
SUBDIRS = fpmsyncd neighsyncd portsyncd mclagsyncd natsyncd fdbsyncd orchagent swssconfig cfgmgr tests gearsyncd 
endif

EXTRA_DIST = lib/fpm_protection_ids.h fpmsyncd/fpm/fpm_protection.h


if HAVE_LIBTEAM
SUBDIRS += teamsyncd tlm_teamd
endif
