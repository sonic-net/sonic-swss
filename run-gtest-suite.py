#!/usr/bin/env python3

import argparse
import subprocess
from lxml import etree
import sys


def main():
    parser = argparse.ArgumentParser(prog='run-gtest-suite')
    parser.add_argument('--test-name', type=str, required=True, help='The name of the test')
    parser.add_argument('--log-file', type=argparse.FileType('w'), required=True, help='The log file created by the test binary')
    parser.add_argument('--trs-file', type=argparse.FileType('w'), required=True, help='The trs file created by this script')
    parser.add_argument('--color-tests', type=bool, help='Whether to produce color output or not')
    parser.add_argument('--collect-skipped-logs', type=bool, help='Whether to include logs of skipped tests (unused)')
    parser.add_argument('--expect-failure', type=bool, help='Unused')
    parser.add_argument('--enable-hard-errors', type=bool, help='Unused')
    parser.add_argument('test_binary', nargs='+')
    args = parser.parse_args()

    test_args = [f"--gtest_output=xml:{args.test_name}_tr.xml"]
    if args.color_tests:
        test_args.append("--gtest_color=yes")
    else:
        test_args.append("--gtest_color=no")

    test_process = subprocess.run(args.test_binary + test_args, stdin=subprocess.DEVNULL, stdout=args.log_file)

    junit_xml = etree.parse(f"{args.test_name}_tr.xml")
    junit_xml_root = junit_xml.getroot()
    for testsuite in junit_xml_root.getchildren():
        testsuite_name = testsuite.get("name")
        for testcase in testsuite.getchildren():
            testcase_name = testcase.get("name")
            was_skipped = testcase.get("result") == "suppressed"
            has_failed = len(testcase.getchildren()) > 0
            result = "SKIP" if was_skipped else "FAIL" if has_failed else "PASS"
            print(f":test-result: {result} {testsuite_name}:{testcase_name}", file=args.trs_file)

    sys.exit(test_process.returncode)


if __name__ == "__main__":
    main()
