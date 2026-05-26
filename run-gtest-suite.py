#!/usr/bin/env python3

import argparse
import os
import subprocess
import time
from lxml import etree
import sys
import resource


def str2bool(value):
    if isinstance(value, bool):
        return value
    if value == "yes":
        return True
    if value == "no":
        return False
    return argparse.ArgumentTypeError(f"Invalid boolean value: {value}")


def raise_nofile_limit():
    try:
        soft_nofile, hard_nofile = resource.getrlimit(resource.RLIMIT_NOFILE)
        target_nofile = hard_nofile
        if target_nofile > soft_nofile:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target_nofile, hard_nofile))
    except (OSError, ValueError):
        pass


def iter_testsuites(junit_xml_root):
    # This matches how gtest structures the results in its junit output format.
    return junit_xml_root if junit_xml_root.tag == "testsuites" else [junit_xml_root]


def write_trs_results(junit_xml_root, trs_file):
    for testsuite in iter_testsuites(junit_xml_root):
        testsuite_name = testsuite.get("name")
        for testcase in testsuite:
            testcase_name = testcase.get("name")
            was_skipped = testcase.get("result") == "suppressed"
            has_failed = len(list(testcase)) > 0
            result = "SKIP" if was_skipped else "FAIL" if has_failed else "PASS"
            print(f":test-result: {result} {testsuite_name}:{testcase_name}", file=trs_file)


def add_int_attr(lhs, rhs, attr):
    lhs.set(attr, str(int(lhs.get(attr, "0")) + int(rhs.get(attr, "0"))))


def add_float_attr(lhs, rhs, attr):
    lhs.set(attr, str(float(lhs.get(attr, "0") or "0") + float(rhs.get(attr, "0") or "0")))


def merge_junit_xml(roots):
    merged = etree.Element("testsuites")
    for attr in ("tests", "failures", "disabled", "errors", "skipped"):
        merged.set(attr, "0")
    merged.set("time", "0")

    for root in roots:
        add_int_attr(merged, root, "tests")
        add_int_attr(merged, root, "failures")
        add_int_attr(merged, root, "disabled")
        add_int_attr(merged, root, "errors")
        if root.get("skipped") is not None:
            add_int_attr(merged, root, "skipped")
        add_float_attr(merged, root, "time")

        if root.tag == "testsuites":
            for testsuite in root:
                merged.append(testsuite)
        else:
            merged.append(root)

    return merged


def parse_junit_with_retry(junit_xml_path):
    # On slower emulated architectures the gtest XML file can appear just after
    # the test process exits. Retry briefly so a transient visibility delay does
    # not mask otherwise valid test results as a harness failure.
    last_error = None
    for _ in range(50):
        try:
            return etree.parse(junit_xml_path)
        except (OSError, etree.XMLSyntaxError) as err:
            last_error = err
            time.sleep(0.1)
    raise RuntimeError(last_error)


def run_gtest(args, test_args, junit_xml_path, shard_index=None, shard_count=None):
    env = os.environ.copy()
    if shard_index is not None and shard_count is not None:
        env["GTEST_TOTAL_SHARDS"] = str(shard_count)
        env["GTEST_SHARD_INDEX"] = str(shard_index)
        print(f"===== Running {args.test_name} shard {shard_index + 1}/{shard_count} =====", file=args.log_file)
        args.log_file.flush()

    return subprocess.run(
        args.test_binary + [f"--gtest_output=xml:{junit_xml_path}"] + test_args,
        stdin=subprocess.DEVNULL,
        stdout=args.log_file,
        stderr=subprocess.STDOUT,
        env=env,
    )


def main():
    parser = argparse.ArgumentParser(prog='run-gtest-suite')
    parser.add_argument('--test-name', type=str, required=True, help='The name of the test')
    parser.add_argument('--log-file', type=argparse.FileType('w'), required=True, help='The log file created by the test binary')
    parser.add_argument('--trs-file', type=argparse.FileType('w'), required=True, help='The trs file created by this script')
    parser.add_argument('--color-tests', type=str2bool, default=False, help='Whether to produce color output or not')
    parser.add_argument('--collect-skipped-logs', type=str2bool, default=False, help='Whether to include logs of skipped tests (unused)')
    parser.add_argument('--expect-failure', type=str2bool, default=False, help='Unused')
    parser.add_argument('--enable-hard-errors', type=str2bool, default=False, help='Unused')
    parser.add_argument('test_binary', nargs='+')
    args = parser.parse_args()

    test_args = []
    if args.color_tests:
        test_args.append("--gtest_color=yes")
    else:
        test_args.append("--gtest_color=no")

    raise_nofile_limit()

    # The aggregate mock test binary opens many DBConnector/Redis-backed test
    # fixtures in one process. On Trixie CI it has intermittently exhausted or
    # corrupted process resources and exited via signal before writing XML. Run
    # only that large binary in gtest shards; smaller binaries remain unchanged.
    shard_count = 1
    if args.test_name == "tests":
        shard_count = max(1, int(os.environ.get("SWSS_GTEST_SHARDS", "8")))

    junit_roots = []
    final_returncode = 0

    for shard_index in range(shard_count):
        if shard_count == 1:
            junit_xml_path = f"{args.test_name}_tr.xml"
            proc = run_gtest(args, test_args, junit_xml_path)
        else:
            junit_xml_path = f"{args.test_name}_shard_{shard_index}.xml"
            proc = run_gtest(args, test_args, junit_xml_path, shard_index, shard_count)

        args.log_file.flush()
        if proc.returncode and final_returncode == 0:
            final_returncode = proc.returncode

        try:
            junit_xml = parse_junit_with_retry(junit_xml_path)
        except RuntimeError as err:
            print(f"ERROR: gtest XML output '{junit_xml_path}' was not available for {args.test_name}: {err}", file=args.log_file)
            print(f":test-result: FAIL {args.test_name}:missing-gtest-xml", file=args.trs_file)
            args.log_file.flush()
            args.trs_file.flush()
            sys.exit(proc.returncode or 1)

        junit_xml_root = junit_xml.getroot()
        write_trs_results(junit_xml_root, args.trs_file)
        junit_roots.append(junit_xml_root)

        if shard_count > 1:
            try:
                os.unlink(junit_xml_path)
            except OSError:
                pass

    if shard_count > 1:
        merged_xml = merge_junit_xml(junit_roots)
        etree.ElementTree(merged_xml).write(f"{args.test_name}_tr.xml", encoding="UTF-8", xml_declaration=True)

    args.trs_file.flush()
    sys.exit(final_returncode)


if __name__ == "__main__":
    main()
