AC_INIT([sonic-swss],[1.0])
AC_CONFIG_SRCDIR([])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])
ARFLAGS=cr
AC_SUBST(ARFLAGS)
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

dnl HAVE_STP: legacy conditional, now derived from --enable-stp.
dnl Removed AC_CHECK_FILE(/usr/include/stp_ipc.h) so the STP deb is not
dnl required as a build dependency when STP is disabled.
dnl Set after AC_ARG_ENABLE(stp,...) below via AM_CONDITIONAL.

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

AC_ARG_ENABLE(p4rt,
[  --enable-p4rt           Enable P4RT orch (default: yes)],
[case "${enableval}" in
	yes) enable_p4rt=true ;;
	no)  enable_p4rt=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-p4rt) ;;
esac],[enable_p4rt=true])
AM_CONDITIONAL(INCLUDE_P4RT, test x$enable_p4rt = xtrue)
if test x$enable_p4rt = xtrue; then
    AC_DEFINE(INCLUDE_P4RT, 1, [Include P4RT orch])
fi

AC_ARG_ENABLE(dash,
[  --enable-dash           Enable DASH orch (default: yes)],
[case "${enableval}" in
	yes) enable_dash=true ;;
	no)  enable_dash=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-dash) ;;
esac],[enable_dash=true])
AM_CONDITIONAL(INCLUDE_DASH, test x$enable_dash = xtrue)
if test x$enable_dash = xtrue; then
    AC_DEFINE(INCLUDE_DASH, 1, [Include DASH orch])
fi

AC_ARG_ENABLE(nat,
[  --enable-nat            Enable NAT orch (default: yes)],
[case "${enableval}" in
	yes) enable_nat=true ;;
	no)  enable_nat=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-nat) ;;
esac],[enable_nat=true])
AM_CONDITIONAL(INCLUDE_NAT, test x$enable_nat = xtrue)
if test x$enable_nat = xtrue; then
    AC_DEFINE(INCLUDE_NAT, 1, [Include NAT orch])
fi

AC_ARG_ENABLE(macsec,
[  --enable-macsec         Enable MACsec orch (default: yes)],
[case "${enableval}" in
	yes) enable_macsec=true ;;
	no)  enable_macsec=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-macsec) ;;
esac],[enable_macsec=true])
AM_CONDITIONAL(INCLUDE_MACSEC, test x$enable_macsec = xtrue)
if test x$enable_macsec = xtrue; then
    AC_DEFINE(INCLUDE_MACSEC, 1, [Include MACsec orch])
fi

AC_ARG_ENABLE(mux,
[  --enable-mux            Enable MUX orch (default: yes)],
[case "${enableval}" in
	yes) enable_mux=true ;;
	no)  enable_mux=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-mux) ;;
esac],[enable_mux=true])
AM_CONDITIONAL(INCLUDE_MUX, test x$enable_mux = xtrue)
if test x$enable_mux = xtrue; then
    AC_DEFINE(INCLUDE_MUX, 1, [Include MUX orch])
fi

AC_ARG_ENABLE(dtel,
[  --enable-dtel           Enable DTel orch (default: yes)],
[case "${enableval}" in
	yes) enable_dtel=true ;;
	no)  enable_dtel=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-dtel) ;;
esac],[enable_dtel=true])
AM_CONDITIONAL(INCLUDE_DTEL, test x$enable_dtel = xtrue)
if test x$enable_dtel = xtrue; then
    AC_DEFINE(INCLUDE_DTEL, 1, [Include DTel orch])
fi

AC_ARG_ENABLE(isogrp,
[  --enable-isogrp         Enable IsolationGroup orch (default: yes)],
[case "${enableval}" in
	yes) enable_isogrp=true ;;
	no)  enable_isogrp=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-isogrp) ;;
esac],[enable_isogrp=true])
AM_CONDITIONAL(INCLUDE_ISOGRP, test x$enable_isogrp = xtrue)
if test x$enable_isogrp = xtrue; then
    AC_DEFINE(INCLUDE_ISOGRP, 1, [Include IsolationGroup orch])
fi

AC_ARG_ENABLE(stp,
[  --enable-stp            Enable STP orch (default: yes)],
[case "${enableval}" in
	yes) enable_stp=true ;;
	no)  enable_stp=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-stp) ;;
esac],[enable_stp=true])
AM_CONDITIONAL(INCLUDE_STP, test x$enable_stp = xtrue)
AM_CONDITIONAL(HAVE_STP, test x$enable_stp = xtrue)
if test x$enable_stp = xtrue; then
    AC_DEFINE(INCLUDE_STP, 1, [Include STP orch])
fi

AC_ARG_ENABLE(mlag,
[  --enable-mlag           Enable MLAG orch and mclagsyncd (default: yes)],
[case "${enableval}" in
	yes) enable_mlag=true ;;
	no)  enable_mlag=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-mlag) ;;
esac],[enable_mlag=true])
AM_CONDITIONAL(INCLUDE_MLAG, test x$enable_mlag = xtrue)
if test x$enable_mlag = xtrue; then
    AC_DEFINE(INCLUDE_MLAG, 1, [Include MLAG orch])
fi

AC_ARG_ENABLE(gearsyncd,
[  --enable-gearsyncd      Enable gearsyncd daemon (default: yes)],
[case "${enableval}" in
	yes) enable_gearsyncd=true ;;
	no)  enable_gearsyncd=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-gearsyncd) ;;
esac],[enable_gearsyncd=true])
AM_CONDITIONAL(INCLUDE_GEARSYNCD, test x$enable_gearsyncd = xtrue)

AC_ARG_ENABLE(chassis,
[  --enable-chassis        Enable chassis orch and fabricmgrd (default: yes)],
[case "${enableval}" in
	yes) enable_chassis=true ;;
	no)  enable_chassis=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-chassis) ;;
esac],[enable_chassis=true])
AM_CONDITIONAL(INCLUDE_CHASSIS, test x$enable_chassis = xtrue)
if test x$enable_chassis = xtrue; then
    AC_DEFINE(INCLUDE_CHASSIS, 1, [Include chassis orch])
fi

AC_ARG_ENABLE(pbh,
[  --enable-pbh            Enable PBH orch (default: yes)],
[case "${enableval}" in
	yes) enable_pbh=true ;;
	no)  enable_pbh=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-pbh) ;;
esac],[enable_pbh=true])
AM_CONDITIONAL(INCLUDE_PBH, test x$enable_pbh = xtrue)
if test x$enable_pbh = xtrue; then
    AC_DEFINE(INCLUDE_PBH, 1, [Include PBH orch])
fi

AC_ARG_ENABLE(nvgre,
[  --enable-nvgre          Enable NVGRE tunnel orch (default: yes)],
[case "${enableval}" in
	yes) enable_nvgre=true ;;
	no)  enable_nvgre=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-nvgre) ;;
esac],[enable_nvgre=true])
AM_CONDITIONAL(INCLUDE_NVGRE, test x$enable_nvgre = xtrue)
if test x$enable_nvgre = xtrue; then
    AC_DEFINE(INCLUDE_NVGRE, 1, [Include NVGRE tunnel orch])
fi

AC_ARG_ENABLE(twamp,
[  --enable-twamp          Enable TWAMP orch (default: yes)],
[case "${enableval}" in
	yes) enable_twamp=true ;;
	no)  enable_twamp=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-twamp) ;;
esac],[enable_twamp=true])
AM_CONDITIONAL(INCLUDE_TWAMP, test x$enable_twamp = xtrue)
if test x$enable_twamp = xtrue; then
    AC_DEFINE(INCLUDE_TWAMP, 1, [Include TWAMP orch])
fi

AC_ARG_ENABLE(icmp-offload,
[  --enable-icmp-offload   Enable ICMP offload orch (default: yes)],
[case "${enableval}" in
	yes) enable_icmp_offload=true ;;
	no)  enable_icmp_offload=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-icmp-offload) ;;
esac],[enable_icmp_offload=true])
AM_CONDITIONAL(INCLUDE_ICMP, test x$enable_icmp_offload = xtrue)
if test x$enable_icmp_offload = xtrue; then
    AC_DEFINE(INCLUDE_ICMP, 1, [Include ICMP offload orch])
fi

AC_ARG_ENABLE(tunneldecap,
[  --enable-tunneldecap    Enable tunnel decap orch (default: yes)],
[case "${enableval}" in
	yes) enable_tunneldecap=true ;;
	no)  enable_tunneldecap=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-tunneldecap) ;;
esac],[enable_tunneldecap=true])
AM_CONDITIONAL(INCLUDE_TUNNELDECAP, test x$enable_tunneldecap = xtrue)
if test x$enable_tunneldecap = xtrue; then
    AC_DEFINE(INCLUDE_TUNNELDECAP, 1, [Include tunnel decap orch])
fi

AC_ARG_ENABLE(cbf,
[  --enable-cbf            Enable CBF NHG orch (default: yes)],
[case "${enableval}" in
	yes) enable_cbf=true ;;
	no)  enable_cbf=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-cbf) ;;
esac],[enable_cbf=true])
AM_CONDITIONAL(INCLUDE_CBF, test x$enable_cbf = xtrue)
if test x$enable_cbf = xtrue; then
    AC_DEFINE(INCLUDE_CBF, 1, [Include CBF NHG orch])
fi

AC_ARG_ENABLE(fgnhg,
[  --enable-fgnhg          Enable fine-grained NHG orch (default: yes)],
[case "${enableval}" in
	yes) enable_fgnhg=true ;;
	no)  enable_fgnhg=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-fgnhg) ;;
esac],[enable_fgnhg=true])
AM_CONDITIONAL(INCLUDE_FGNHG, test x$enable_fgnhg = xtrue)
if test x$enable_fgnhg = xtrue; then
    AC_DEFINE(INCLUDE_FGNHG, 1, [Include fine-grained NHG orch])
fi

AC_ARG_ENABLE(srv6,
[  --enable-srv6           Enable SRv6 orch (default: yes)],
[case "${enableval}" in
	yes) enable_srv6=true ;;
	no)  enable_srv6=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-srv6) ;;
esac],[enable_srv6=true])
AM_CONDITIONAL(INCLUDE_SRV6, test x$enable_srv6 = xtrue)
if test x$enable_srv6 = xtrue; then
    AC_DEFINE(INCLUDE_SRV6, 1, [Include SRv6 orch])
fi

AC_ARG_ENABLE(mpls,
[  --enable-mpls           Enable MPLS route orch (default: yes)],
[case "${enableval}" in
	yes) enable_mpls=true ;;
	no)  enable_mpls=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-mpls) ;;
esac],[enable_mpls=true])
AM_CONDITIONAL(INCLUDE_MPLS, test x$enable_mpls = xtrue)
if test x$enable_mpls = xtrue; then
    AC_DEFINE(INCLUDE_MPLS, 1, [Include MPLS route orch])
fi

AC_ARG_ENABLE(hft,
[  --enable-hft            Enable High Frequency Telemetry (default: yes)],
[case "${enableval}" in
	yes) enable_hft=true ;;
	no)  enable_hft=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-hft) ;;
esac],[enable_hft=true])
AM_CONDITIONAL(INCLUDE_HFT, test x$enable_hft = xtrue)
if test x$enable_hft = xtrue; then
    AC_DEFINE(INCLUDE_HFT, 1, [Include High Frequency Telemetry])
fi

AC_ARG_ENABLE(tests,
[  --enable-tests          Build test programs (default: yes)],
[case "${enableval}" in
	yes) enable_tests=true ;;
	no)  enable_tests=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-tests) ;;
esac],[enable_tests=true])
AM_CONDITIONAL(BUILD_TESTS, test x$enable_tests = xtrue)

AC_ARG_WITH([fpm],
  [AS_HELP_STRING([--with-fpm=DIR], [path to FPM protocol headers])],
  [FPM_PATH="$withval"],
  [FPM_PATH=""])
AC_SUBST([FPM_PATH])
case "$FPM_PATH" in
  yes) AC_MSG_ERROR([--with-fpm requires a directory argument, e.g. --with-fpm=/usr/src/sonic-fpm]) ;;
  no|"") FPM_INCLUDES="" ;;
  *)     FPM_INCLUDES="-I $FPM_PATH" ;;
esac
AC_SUBST([FPM_INCLUDES])

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

CFLAGS_COMMON="-std=c++14 -Wall -fPIC -Wno-write-strings -I/usr/include/swss -I/usr/include"

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
CFLAGS_COMMON+=" -Wno-error=missing-field-initializers"

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

AC_ARG_ENABLE(asan,
[  --enable-asan      Compile with address sanitizer],
[case "${enableval}" in
	yes) asan_enabled=true ;;
	no)  asan_enabled=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-asan) ;;
esac],[asan_enabled=false])

if test "x$asan_enabled" = "xtrue"; then
    CFLAGS_ASAN+=" -fsanitize=address"
    CFLAGS_ASAN+=" -DASAN_ENABLED"
    CFLAGS_ASAN+=" -ggdb -fno-omit-frame-pointer -U_FORTIFY_SOURCE"
    CFLAGS_ASAN+=" -Wno-maybe-uninitialized"
    AC_SUBST(CFLAGS_ASAN)

    LDFLAGS_ASAN+=" -fsanitize=address"
    AC_SUBST(LDFLAGS_ASAN)
fi

AM_CONDITIONAL(ASAN_ENABLED, test x$asan_enabled = xtrue)

AC_CHECK_PROG([HAVE_MOLD], [mold], [yes], [no])
if test "x$HAVE_MOLD" = "xyes"; then
    LDFLAGS+=" -fuse-ld=mold"
    AC_SUBST(LDFLAGS)
fi

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
])

AM_COND_IF([BUILD_TESTS],[
    AC_CONFIG_FILES([tests/Makefile
                     orchagent/p4orch/tests/Makefile])
])

# If no SAI library is installed, compile with SAIVS and run unit tests
AM_COND_IF([BUILD_TESTS],[
    AM_COND_IF([HAVE_SAI],[],
               [AC_CONFIG_FILES([tests/mock_tests/Makefile])])
])

AC_OUTPUT
