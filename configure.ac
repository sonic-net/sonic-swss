AC_INIT([sonic-swss],[1.0])
AC_CONFIG_SRCDIR([])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE([foreign])
AC_LANG_C
AC_LANG([C++])
AC_PROG_CC
AC_PROG_CXX
AC_PROG_LIBTOOL
AC_HEADER_STDC

AC_CHECK_LIB([hiredis], [redisConnect],,
    AC_MSG_ERROR([libhiredis is not installed.]))

AC_CHECK_LIB([nl-genl-3], [genl_connect])

AC_CHECK_LIB([team], [team_alloc],
    AM_CONDITIONAL(HAVE_LIBTEAM, true),
   [AC_MSG_WARN([libteam is not installed.])
    AM_CONDITIONAL(HAVE_LIBTEAM, false)])

AC_ARG_ENABLE(debug,
[  --enable-debug      Compile with debugging flags],
[case "${enableval}" in
	yes) debug=true ;;
	no)  debug=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-debug) ;;
esac],[debug=false])
AM_CONDITIONAL(DEBUG, test x$debug = xtrue)

AC_ARG_ENABLE(gtest,
[  --enable-gtest       Compile with googletest flags],
[case "${enableval}" in
	yes) gtest=true ;;
	no)  gtest=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-gtest) ;;
esac],[gtest=false])
AM_CONDITIONAL(GTEST, test x$gtest = xtrue)

CFLAGS_COMMON="-std=c++11 -Wall -fPIC -Wno-write-strings -I/usr/include/libnl3 -I/usr/include/swss"
CFLAGS_COMMON+=" -Werror"
CFLAGS_COMMON+=" -Wno-reorder"
AC_SUBST(CFLAGS_COMMON)

AC_CONFIG_FILES([
    Makefile
    orchagent/Makefile
    fpmsyncd/Makefile
    neighsyncd/Makefile
    intfsyncd/Makefile
    cfgagent/Makefile
    cfgagent/cfgmgrtest/Makefile
    portsyncd/Makefile
    teamsyncd/Makefile
    swssconfig/Makefile
    tests/Makefile
])

AC_OUTPUT
