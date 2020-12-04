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

PKG_CHECK_MODULES([JANSSON], [jansson])

AC_CHECK_LIB([sai], [sai_object_type_query],
    AM_CONDITIONAL(HAVE_SAI, true),
   [AC_MSG_WARN([libsai is not installed.])
    AM_CONDITIONAL(HAVE_SAI, false)])

AC_ARG_ENABLE(debug,
[  --enable-debug      Compile with debugging flags],
[case "${enableval}" in
	yes) debug=true ;;
	no)  debug=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-debug) ;;
esac],[debug=false])
AM_CONDITIONAL(DEBUG, test x$debug = xtrue)

AC_ARG_WITH(extra-inc,
[  --with-extra-inc=DIR
                          prefix where extra includes are installed],
[AC_SUBST(CPPFLAGS, "$CPPFLAGS -I${withval} -I${withval}/swss -I${withval}/sai")])

AC_ARG_WITH(extra-lib,
[  --with-extra-lib=DIR
                          prefix where extra libraries are installed],
[AC_SUBST(LDFLAGS, "$LDFLAGS -L${withval}")])

CFLAGS_COMMON="-std=c++14 -Wall -fPIC -Wno-write-strings -I/usr/include/libnl3 -I/usr/include/swss"

CFLAGS_COMMON+=" -Werror"
CFLAGS_COMMON+=" -Wno-reorder"
CFLAGS_COMMON+=" -Wcast-align"
CFLAGS_COMMON+=" -Wcast-qual"
CFLAGS_COMMON+=" -Wconversion"
CFLAGS_COMMON+=" -Wdisabled-optimization"
CFLAGS_COMMON+=" -Wextra"
CFLAGS_COMMON+=" -Wfloat-equal"
CFLAGS_COMMON+=" -Wformat=2"
CFLAGS_COMMON+=" -Wformat-nonliteral"
CFLAGS_COMMON+=" -Wformat-security"
CFLAGS_COMMON+=" -Wformat-y2k"
CFLAGS_COMMON+=" -Wimport"
CFLAGS_COMMON+=" -Winit-self"
CFLAGS_COMMON+=" -Winvalid-pch"
CFLAGS_COMMON+=" -Wlong-long"
CFLAGS_COMMON+=" -Wmissing-field-initializers"
CFLAGS_COMMON+=" -Wmissing-format-attribute"
CFLAGS_COMMON+=" -Wno-aggregate-return"
CFLAGS_COMMON+=" -Wno-padded"
CFLAGS_COMMON+=" -Wno-switch-enum"
CFLAGS_COMMON+=" -Wno-unused-parameter"
CFLAGS_COMMON+=" -Wpacked"
CFLAGS_COMMON+=" -Wpointer-arith"
CFLAGS_COMMON+=" -Wredundant-decls"
CFLAGS_COMMON+=" -Wstack-protector"
CFLAGS_COMMON+=" -Wstrict-aliasing=3"
CFLAGS_COMMON+=" -Wswitch"
CFLAGS_COMMON+=" -Wswitch-default"
CFLAGS_COMMON+=" -Wunreachable-code"
CFLAGS_COMMON+=" -Wunused"
CFLAGS_COMMON+=" -Wvariadic-macros"
CFLAGS_COMMON+=" -Wno-switch-default"
CFLAGS_COMMON+=" -Wno-long-long"
CFLAGS_COMMON+=" -Wno-redundant-decls"

AC_SUBST(CFLAGS_COMMON)

AC_CONFIG_FILES([
    Makefile
    orchagent/Makefile
    fpmsyncd/Makefile
    neighsyncd/Makefile
    gearsyncd/Makefile
    natsyncd/Makefile
    portsyncd/Makefile
    teamsyncd/Makefile
    tlm_teamd/Makefile
    mclagsyncd/Makefile
    fdbsyncd/Makefile
    swssconfig/Makefile
    cfgmgr/Makefile
    tests/Makefile
])

# If no SAI library is installed, compile with SAIVS and run unit tests
AM_COND_IF([HAVE_SAI],[],
           [AC_CONFIG_FILES([tests/mock_tests/Makefile])])

AC_OUTPUT
