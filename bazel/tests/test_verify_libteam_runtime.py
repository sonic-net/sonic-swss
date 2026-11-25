"""Regressions for SWSS's installed libteam consumer boundary."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_libteam_runtime import check_loaded, check_needed, check_no_defined_netlink


class LibteamRuntimeTest(unittest.TestCase):
    def test_runtime_target_preserves_both_consumers_without_duplicate_labels(self):
        root = Path(__file__).resolve().parents[2]
        namespace = {}
        exec((root / "tools/bazel/deps.bzl").read_text(), namespace)
        expected = set(namespace["TEAM_DEPS"] + namespace["TLM_TEAM_DEPS"])
        libnl = set(namespace["_LIBNL_DEPS"])
        for group_name, shared_labels in (
            ("SWSS_DEPS", {"@sonic_swss_common//:libswsscommon_shared"}),
            ("CFGMGR_DEPS", {"@sonic_swss_common//:libswsscommon_shared",
                             "@sonic_sairedis//meta:saimeta_shared"}),
            ("ORCHAGENT_DEPS", {"@sonic_swss_common//:libswsscommon_shared",
                                "@sonic_sairedis//meta:saimeta_shared",
                                "@sonic_sairedis//lib:sairedis_shared"}),
            ("TEAM_DEPS", {"@sonic_swss_common//:libswsscommon_shared",
                           "@libteam//:libteam", "@libteam//:libteamdctl"}),
            ("TLM_TEAM_DEPS", {"@sonic_swss_common//:libswsscommon_shared",
                               "@libteam//:libteamdctl"}),
        ):
            entries = namespace[group_name]
            last_shared = max(entries.index(label) for label in shared_labels)
            first_libnl = min(entries.index(label) for label in libnl)
            self.assertGreater(first_libnl, last_shared, group_name)
        captured = {}

        class Depset:
            def __init__(self, values):
                self.values = list(dict.fromkeys(values))

            def to_list(self):
                return self.values

        def runtime_libraries(name, deps):
            if len(deps) != len(set(deps)):
                raise ValueError("duplicate dependency label")
            captured[name] = deps

        namespace.update(load=lambda *args: None, cc_test=lambda **kwargs: None,
                         depset=Depset, runtime_libraries=runtime_libraries)
        build = (root / "bazel/BUILD.bazel").read_text()
        exec(build, namespace)
        actual = captured["libteam_consumer_runtime_libraries"]
        self.assertEqual(set(actual), expected)
        self.assertEqual(len(actual), len(expected))
        self.assertLessEqual({"@libteam//:libteam", "@libteam//:libteamdctl",
                              "@swss_debian//libjansson-dev:libjansson"}, expected)
        # The former expression fails before compilation, just as Bazel's
        # label_list did in CodeQL job 127121481405.
        broken = build.replace("depset(TEAM_DEPS + TLM_TEAM_DEPS).to_list()",
                               "TEAM_DEPS + TLM_TEAM_DEPS")
        with self.assertRaisesRegex(ValueError, "duplicate dependency label"):
            exec(broken, namespace)

    def test_required_library_split_matches_the_native_consumers(self):
        team = (
            "Shared library: [libteam.so.5]\n"
            "Shared library: [libteamdctl.so.0]\n"
            "Shared library: [libnl-route-3.so.200]\n"
            "Shared library: [libc.so.6]"
        )
        self.assertEqual(
            check_needed("teamsyncd", team),
            ["libc.so.6", "libnl-route-3.so.200", "libteam.so.5", "libteamdctl.so.0"],
        )
        self.assertEqual(check_needed("tlm_teamd", "Shared library: [libteamdctl.so.0]"), ["libteamdctl.so.0"])
        teammgr = (
            "Shared library: [libnl-3.so.200]\n"
            "Shared library: [libnl-route-3.so.200]\n"
            "Shared library: [libc.so.6]"
        )
        self.assertEqual(
            check_needed("teammgrd", teammgr),
            ["libc.so.6", "libnl-3.so.200", "libnl-route-3.so.200"],
        )
        for program, changed in (("teamsyncd", "Shared library: [libteam.so.5]"),
                                 ("tlm_teamd", team), ("teammgrd", team)):
            with self.subTest(program=program), self.assertRaisesRegex(ValueError, "Unexpected libteam dependencies"):
                check_needed(program, changed)
        for program, changed in (
            ("teamsyncd", "Shared library: [libteam.so.5]\nShared library: [libteamdctl.so.0]"),
            ("tlm_teamd", "Shared library: [libteamdctl.so.0]\nShared library: [libnl-3.so.200]"),
            ("teammgrd", "Shared library: [libc.so.6]"),
        ):
            with self.subTest(program=program), self.assertRaisesRegex(ValueError, "Unexpected libnl dependencies"):
                check_needed(program, changed)

    def test_consumers_do_not_export_static_libnl_symbols(self):
        check_no_defined_netlink("teamsyncd", "00000000000083c0 T main\n")
        for program, symbol in (
            ("teamsyncd", "0000000000014320 T nl_socket_alloc\n"),
            ("teammgrd", "0000000000023100 T rtnl_link_get_type\n"),
        ):
            with self.subTest(program=program), self.assertRaisesRegex(
                ValueError, "Consumer embeds static libnl symbols"
            ):
                check_no_defined_netlink(program, symbol)

    def test_loader_requires_the_installed_owner_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            declared = stage / "declared-libraries"
            declared.mkdir()
            libraries = {name: stage / name for name in ("libteam.so.5", "libteamdctl.so.0")}
            for path in libraries.values():
                path.write_bytes(b"owned payload")
            netlink = {name: declared / name for name in ("libnl-3.so.200", "libnl-route-3.so.200")}
            for path in netlink.values():
                path.write_bytes(b"netlink payload")
            netlink_loaded = "\n".join(name + " => " + str(path) + " (0x0)" for name, path in netlink.items())
            loaded = "\n".join(
                [name + " => " + str(path) + " (0x0)" for name, path in libraries.items()]
                + [netlink_loaded]
            )
            check_loaded("teamsyncd", loaded, libraries, declared)
            control = "libteamdctl.so.0 => " + str(libraries["libteamdctl.so.0"]) + " (0x0)\n" + netlink_loaded
            check_loaded("tlm_teamd", control, libraries, declared)
            check_loaded("teammgrd", "libc.so.6 => /lib/libc.so.6 (0x0)", libraries, declared)
            wrong = loaded.replace(str(stage / "libteam.so.5"), "/usr/lib/x86_64-linux-gnu/libteam.so.5")
            with self.assertRaisesRegex(ValueError, "outside the installed owner TAR"):
                check_loaded("teamsyncd", wrong, libraries, declared)
            wrong_netlink = loaded.replace(str(declared), "/usr/lib/x86_64-linux-gnu")
            with self.assertRaisesRegex(ValueError, "loaded libnl outside the declared closure"):
                check_loaded("teamsyncd", wrong_netlink, libraries, declared)
            missing_netlink = "\n".join(name + " => " + str(path) + " (0x0)" for name, path in libraries.items())
            with self.assertRaisesRegex(ValueError, "Missing loaded libnl libraries"):
                check_loaded("teamsyncd", missing_netlink, libraries, declared)
            for invalid in (control, loaded + "\n" + control, loaded + "\nlibother.so => not found"):
                with self.subTest(loader=invalid), self.assertRaises(ValueError):
                    check_loaded("teamsyncd", invalid, libraries, declared)


if __name__ == "__main__":
    unittest.main()
