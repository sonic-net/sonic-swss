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

AC_ARG_WITH(extra-usr-lib,
[  --with-extra-usr-lib=DIR
                          prefix where extra libraries are installed],
[AC_SUBST(LDFLAGS, "$LDFLAGS -L${withval}")])

AC_CHECK_LIB([nl-3], [nl_addr_alloc])
AC_CHECK_LIB([nl-genl-3], [nl_socket_get_cb])
AC_CHECK_LIB([nl-route-3], [rtnl_route_nh_get_encap_mpls_dst])
AC_CHECK_LIB([nl-nf-3], [nfnl_connect])

CFLAGS_COMMON="-std=c++14 -Wall -fPIC -Wno-write-strings -I/usr/include/swss"

AC_ARG_WITH(libnl-3.0-inc,
[  --with-libnl-3.0-inc=DIR
                           prefix where libnl-3.0 includes are installed],
[AC_SUBST(CPPFLAGS, "$CPPFLAGS -I${withval}")
 AC_SUBST(LIBNL_INC_DIR, "${withval}")])

if test "${with_libnl_3_0_inc+set}" != set; then
    CFLAGS_COMMON+=" -I/usr/include/libnl3"
fi

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

# Code testing coverage with gcov
AC_MSG_CHECKING(whether to build with gcov testing)
AC_ARG_ENABLE(gcov, AS_HELP_STRING([--enable-gcov], [Whether to enable gcov testing]),, enable_gcov=no)

if test "x$enable_gcov" = "xyes"; then
    if test "$GCC" = "no"; then
        AC_MSG_ERROR(not compiling with gcc, which is required for gcov testing)
    fi

    CFLAGS_COMMON+=" -fprofile-arcs -ftest-coverage"
    AC_SUBST(CFLAGS_COMMON)

    LDFLAGS+=" -fprofile-arcs -lgcov"
    AC_SUBST(LDFLAGS)
fi

AM_CONDITIONAL(GCOV_ENABLED, test x$enable_gcov = xyes)
AC_MSG_RESULT($enable_gcov)

AC_SUBST(CFLAGS_COMMON)

AC_CONFIG_FILES([
    Makefile
    orchagent/Makefile
    fpmsyncd/Makefile
    neighsyncd/Makefile
    gearsyncd/Makefile
    fdbsyncd/Makefile
    natsyncd/Makefile
    portsyncd/Makefile
    teamsyncd/Makefile
    tlm_teamd/Makefile
    mclagsyncd/Makefile
    swssconfig/Makefile
    cfgmgr/Makefile
    tests/Makefile
    orchagent/p4orch/tests/Makefile
])

# If no SAI library is installed, compile with SAIVS and run unit tests
AM_COND_IF([HAVE_SAI],[],
           [AC_CONFIG_FILES([tests/mock_tests/Makefile])])

AC_OUTPUT
