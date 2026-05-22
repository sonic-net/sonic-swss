#!/usr/bin/env python3

import argparse
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

    test_args = [f"--gtest_output=xml:{args.test_name}_tr.xml"]
    if args.color_tests:
        test_args.append("--gtest_color=yes")
    else:
        test_args.append("--gtest_color=no")

    try:
        soft_nofile, hard_nofile = resource.getrlimit(resource.RLIMIT_NOFILE)
        target_nofile = min(max(soft_nofile, 4096), hard_nofile)
        if target_nofile > soft_nofile:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target_nofile, hard_nofile))
    except (OSError, ValueError):
        pass

    test_process = subprocess.run(args.test_binary + test_args, stdin=subprocess.DEVNULL, stdout=args.log_file, stderr=subprocess.STDOUT)
    args.log_file.flush()

    junit_xml_path = f"{args.test_name}_tr.xml"
    # On slower emulated architectures the gtest XML file can appear just after
    # the test process exits. Retry briefly so a transient visibility delay does
    # not mask otherwise valid test results as a harness failure.
    junit_xml = None
    last_error = None
    for _ in range(50):
        try:
            junit_xml = etree.parse(junit_xml_path)
            break
        except (OSError, etree.XMLSyntaxError) as err:
            last_error = err
            time.sleep(0.1)

    if junit_xml is None:
        print(f"ERROR: gtest XML output '{junit_xml_path}' was not available for {args.test_name}: {last_error}", file=args.log_file)
        print(f":test-result: FAIL {args.test_name}:missing-gtest-xml", file=args.trs_file)
        args.log_file.flush()
        args.trs_file.flush()
        sys.exit(test_process.returncode or 1)
    junit_xml_root = junit_xml.getroot()
    # This code matches how gtest structures the results in its junit output format.
    testsuites = junit_xml_root if junit_xml_root.tag == "testsuites" else [junit_xml_root]
    for testsuite in testsuites:
        testsuite_name = testsuite.get("name")
        for testcase in testsuite:
            testcase_name = testcase.get("name")
            was_skipped = testcase.get("result") == "suppressed"
            has_failed = len(list(testcase)) > 0
            result = "SKIP" if was_skipped else "FAIL" if has_failed else "PASS"
            print(f":test-result: {result} {testsuite_name}:{testcase_name}", file=args.trs_file)

    args.trs_file.flush()
    sys.exit(test_process.returncode)


if __name__ == "__main__":
    main()
