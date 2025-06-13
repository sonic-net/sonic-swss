load("@rules_pkg//pkg:deb.bzl", "pkg_deb")
load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_filegroup", "pkg_files")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//:swss_helpers.bzl", "create_pkg_files_of_stripped_binaries")

config_setting(
    name = "debug_mode",
    values = {"compilation_mode": "dbg"},
)

config_setting(
    name = "enable_gcov",
    define_values = {"enable_gcov": "true"},
)

config_setting(
    name = "asan",
    define_values = {"asan": "true"},
)

config_setting(
    name = "tsan",
    define_values = {"tsan": "true"},
)

config_setting(
    name = "usan",
    define_values = {"usan": "true"},
)

# --- Configuration Files ---

pkg_files(
    name = "config_files",
    srcs = ["//swssconfig:sample_netbouncer_config"],
    attributes = pkg_attributes(
        group = "root",
        mode = "0644",
        user = "root",
    ),
    prefix = "/etc/swss/config.d",
)

# --- Documentation ---

filegroup(
    name = "changelog",
    srcs = ["debian/changelog"],
)

pkg_files(
    name = "doc_files",
    srcs = [":changelog"],
    attributes = pkg_attributes(
        group = "root",
        mode = "0644",
        user = "root",
    ),
    prefix = "/usr/share/doc/swss",
)

# --- State Verification Data ---

pkg_files(
    name = "sonic_state_verification_data",
    srcs = [
        "//cfgmgr:cfgmgr_state_verification_data",
        "//orchagent:orchagent_state_verification_data",
    ],
    attributes = pkg_attributes(
        group = "root",
        mode = "0644",
        user = "root",
    ),
    prefix = "/usr/share/sonic",
)

# --- SWSS Lua Scripts ---

pkg_files(
    name = "swss_data_lua_scripts",
    srcs = [
        "//cfgmgr:cfgmgr_lua_scripts",
        "//orchagent:orchagent_data",
    ],
    attributes = pkg_attributes(
        group = "root",
        mode = "0644",
        user = "root",
    ),
    prefix = "/usr/share/swss",
)

# --- SWSS Python scripts ---

pkg_files(
    name = "swss_python_scripts",
    srcs = [
        "//fpmsyncd:bgp_eoiu_marker_py_src",
        "//mgmtintfsyncd:mgtmintfsyncd_py_src",
        "//neighsyncd:restore_neighbors_py_src",
    ],
    attributes = pkg_attributes(
        group = "root",
        mode = "0755",
        user = "root",
    ),
    prefix = "/usr/bin",
)

# --- /usr/bin files ---

all_binary_labels = [
    "//cfgmgr:bookkeepmgrd",
    "//cfgmgr:bridgemgrd",
    "//cfgmgr:buffermgrd",
    "//cfgmgr:coppmgrd",
    "//cfgmgr:intfmgrd",
    "//cfgmgr:nbrmgrd",
    "//cfgmgr:portmgrd",
    "//cfgmgr:sflowmgrd",
    "//cfgmgr:stateverificationmgrd",
    "//cfgmgr:switchmgrd",
    "//cfgmgr:teammgrd",
    "//cfgmgr:tunnelmgrd",
    "//cfgmgr:vlanmgrd",
    "//cfgmgr:vrfmgrd",
    "//cfgmgr:vxlanmgrd",
    "//fdbsyncd:fdbsyncd",
    "//fpmsyncd:fpmsyncd",
    "//gearsyncd:gearsyncd",
    "//mclagsyncd:mclagsyncd",
    "//neighsyncd:neighsyncd",
    "//orchagent:orchagent",
    "//orchagent:orchagent_restart_check",
    "//orchagent:routeresync",
    "//portsyncd:portsyncd",
    "//swssconfig:swssconfig",
    "//swssconfig:swssplayer",
    "//teamsyncd:teamsyncd",
    "//tlm_teamd:tlm_teamd",
]

create_pkg_files_of_stripped_binaries(
    name = "all_binaries",
    targets = all_binary_labels,
)

pkg_files(
    name = "gcovpreload",
    srcs = [
        "//gcovpreload",
    ],
    attributes = pkg_attributes(
        group = "root",
        mode = "0777",
        user = "root",
    ),
    prefix = "/usr/lib",
)

pkg_files(
    name = "lcov_cobertura",
    srcs = ["//gcovpreload:lcov_cobertura"],
    attributes = pkg_attributes(
        group = "root",
        mode = "0755",
        user = "root",
    ),
    prefix = "/usr/bin",
)

pkg_filegroup(
    name = "all-data",
    srcs = [
        ":all_binaries.stripped",  # All the /usr/bin executables (stripped)
        ":config_files",  # /etc/swss/config.d/netbouncer.json
        ":doc_files",  # /usr/share/doc/swss/changelog.gz
        ":sonic_state_verification_data",  # /usr/share/sonic/*.json
        ":swss_data_lua_scripts",  # /usr/share/swss/*.lua
        ":swss_python_scripts",
    ] + select(
        {
            "//:enable_gcov": [
                ":gcovpreload",
                ":lcov_cobertura",
            ],
            "//conditions:default": [],
        },
    ),
)

pkg_tar(
    name = "debian-data",
    srcs = [":all-data"],
    extension = "tar.gz",
)

pkg_tar(
    name = "debian-debug-data",
    srcs = [":all_binaries.debug"],
    extension = "tar.gz",
)

pkg_deb(
    name = "swss_deb_pkg",
    architecture = "amd64",
    built_using = "bazel",
    data = ":debian-data",
    depends = [
        "libc6",  #(>= 2.17)
        "libgcc-s1",  #(>= 3.0)
        "libhiredis0.14",  #(>= 0.14.0)
        "libjansson4",  #(>= 2.3)
        "libnl-3-200",  #(>= 3.5.0-1)
        "libnl-route-3-200",  #(>= 3.5.0-1)
        "libsaimetadata",
        "libsairedis",
        "libstdc++6",  #(>= 9)
        "libswsscommon",  #(>= 1.0.0)
        "libteam5",  #(>= 1.27)
        "libteamdctl0",  #(>= 1.9)
    ],
    description = "Sonic SWSS",
    maintainer = "GPINS SWSS Team (Google)",
    package = "sonic-swss",

    # Currently uses static naming
    package_file_name = "swss_1.0.0_amd64.deb",
    version = "1.0.0",
)

pkg_deb(
    name = "swss_deb_dbg_pkg",
    architecture = "amd64",
    built_using = "bazel",
    data = ":debian-debug-data",
    depends = [
        "libc6",  #(>= 2.17)
        "libgcc-s1",  #(>= 3.0)
        "libhiredis0.14",  #(>= 0.14.0)
        "libjansson4",  #(>= 2.3)
        "libnl-3-200",  #(>= 3.5.0-1)
        "libnl-route-3-200",  #(>= 3.5.0-1)
        "libsaimetadata",
        "libsairedis",
        "libstdc++6",  #(>= 9)
        "libswsscommon",  #(>= 1.0.0)
        "libteam5",  #(>= 1.27)
        "libteamdctl0",  #(>= 1.9)
    ],
    description = "Sonic SWSS",
    maintainer = "GPINS SWSS Team (Google)",
    package = "sonic-swss-dbg",

    # Currently uses static naming
    package_file_name = "swss-dbg_1.0.0_amd64.deb",
    version = "1.0.0",
)
