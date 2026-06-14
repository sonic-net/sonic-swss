#!/usr/bin/env python3
# Copyright (c) 2026 UpScale AI, Inc. All rights reserved.
#
# Gate PR coverage against the S3 dashboard baseline for sonic-swss.
#
# For each changed production source file in the PR:
#   - New file: line coverage >= --new-file-min (default 80%, set via repo variable).
#   - Modified existing file: line coverage >= (baseline - tolerance).
#     Tolerance is configurable via --tolerance (default from COVERAGE_DROP_TOLERANCE
#     env var, or 5 percentage points).
#   - Existing file with no baseline lcov data: always passes (was untested before).
#
# Usage:
#   python3 gate_pr_file_coverage.py \
#     --pr-lcov lcov.filtered.info \
#     --baseline-lcov baseline.info \
#     --compare-ref origin/upscaleai-202511 \
#     --new-file-min 80 \
#     --tolerance 5 \
#     --report-md coverage-gate.md

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".c", ".cc", ".cxx", ".h", ".hpp"}
HEADER_SUFFIXES = {".h", ".hpp"}
SKIP_PATH_MARKERS = (
    "/tests/",
    "tests/mock_tests/",
    "debian/",
    ".github/",
    "m4/",
)
TOP_LEVEL_PREFIXES = (
    "orchagent/",
    "fdbsyncd/",
    "fpmsyncd/",
    "gearsyncd/",
    "natsyncd/",
    "teamsyncd/",
    "portsyncd/",
    "countersyncd/",
    "lib/",
)


@dataclass(frozen=True)
class FileCoverage:
    lines_hit: int
    lines_found: int

    @property
    def percent(self) -> float:
        if self.lines_found == 0:
            return 100.0
        return 100.0 * self.lines_hit / self.lines_found


@dataclass(frozen=True)
class GateRule:
    threshold: float
    basis: str
    strict: bool  # True => PR must be > threshold; False => PR must be >= threshold

    def passes(self, pr_pct: float) -> bool:
        if self.strict:
            return pr_pct > self.threshold
        return pr_pct >= self.threshold

    @property
    def operator(self) -> str:
        return ">" if self.strict else ">="


def normalize_path(path: str) -> str:
    path = path.replace("\\", "/")
    if path.startswith("./"):
        path = path[2:]
    for prefix in TOP_LEVEL_PREFIXES:
        idx = path.find(prefix)
        if idx >= 0:
            return path[idx:]
    for marker in ("/build/sonic-swss/", "/sonic-swss/"):
        if marker in path:
            return path.split(marker, 1)[1]
    return path.lstrip("/")


def parse_lcov(path: Path) -> dict[str, FileCoverage]:
    records: dict[str, dict[str, int]] = {}
    current: str | None = None
    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("SF:"):
            current = normalize_path(line[3:])
            records.setdefault(current, {"hit": 0, "found": 0})
        elif line.startswith("DA:") and current is not None:
            _line_no, hit = line[3:].split(",", 1)
            records[current]["found"] += 1
            if int(hit) > 0:
                records[current]["hit"] += 1
        elif line == "end_of_record":
            current = None
    return {
        name: FileCoverage(v["hit"], v["found"])
        for name, v in records.items()
        if v["found"] > 0
    }


def lookup_file(files: dict[str, FileCoverage], repo_path: str) -> FileCoverage | None:
    norm = normalize_path(repo_path)
    if norm in files:
        return files[norm]
    suffix_matches = [cov for name, cov in files.items() if name == norm or name.endswith("/" + norm)]
    if len(suffix_matches) == 1:
        return suffix_matches[0]
    return None


def changed_source_files(compare_ref: str) -> list[str]:
    out = subprocess.check_output(
        ["git", "diff", "--name-only", f"{compare_ref}...HEAD"],
        text=True,
    )
    files: list[str] = []
    for path in out.splitlines():
        path = path.strip()
        if not path:
            continue
        suffix = Path(path).suffix.lower()
        if suffix not in SOURCE_SUFFIXES:
            continue
        if any(marker in path for marker in SKIP_PATH_MARKERS):
            continue
        files.append(path)
    return sorted(files)


def is_production_source(path: str) -> bool:
    norm = normalize_path(path)
    return any(norm.startswith(prefix) for prefix in TOP_LEVEL_PREFIXES)


def existed_at_base(path: str, compare_ref: str) -> bool:
    result = subprocess.run(
        ["git", "cat-file", "-e", f"{compare_ref}:{path}"],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gate_rule(
    path: str,
    base_cov: FileCoverage | None,
    new_file_min: float,
    tolerance: float,
    compare_ref: str,
) -> GateRule:
    if not existed_at_base(path, compare_ref):
        return GateRule(
            threshold=new_file_min,
            basis=f"new file minimum {new_file_min:.2f}%",
            strict=False,
        )
    if base_cov is not None:
        if base_cov.percent >= 100.0:
            return GateRule(
                threshold=100.0,
                basis="baseline file 100% (modified file; cannot exceed 100%)",
                strict=False,
            )
        effective = max(0.0, base_cov.percent - tolerance)
        return GateRule(
            threshold=effective,
            basis=f"baseline {base_cov.percent:.2f}% minus {tolerance:.1f}pp tolerance (modified file)",
            strict=False,
        )
    return GateRule(
        threshold=0.0,
        basis="pre-existing file with no baseline lcov data (skip)",
        strict=False,
    )


def main() -> int:
    default_new_min = float(os.environ.get("NEW_FILE_COVERAGE_MIN", "80"))
    default_tolerance = float(os.environ.get("COVERAGE_DROP_TOLERANCE", "5"))
    parser = argparse.ArgumentParser(description="Gate PR file coverage against S3 baseline")
    parser.add_argument("--pr-lcov", type=Path, required=True)
    parser.add_argument("--baseline-lcov", type=Path, required=True)
    parser.add_argument("--compare-ref", required=True)
    parser.add_argument("--new-file-min", type=float, default=default_new_min)
    parser.add_argument("--tolerance", type=float, default=default_tolerance,
                        help="Allowed coverage drop in percentage points (default: env COVERAGE_DROP_TOLERANCE or 5)")
    parser.add_argument("--gate-headers", action="store_true",
                        default=os.environ.get("GATE_HEADER_FILES", "false").lower() == "true",
                        help="Include .h/.hpp files in gating (default: skip headers)")
    parser.add_argument("--report-md", type=Path, default=Path("coverage-gate.md"))
    args = parser.parse_args()

    if not args.pr_lcov.is_file():
        print(f"ERROR: PR lcov missing: {args.pr_lcov}", file=sys.stderr)
        return 1
    if not args.baseline_lcov.is_file():
        print(f"ERROR: baseline lcov missing: {args.baseline_lcov}", file=sys.stderr)
        return 1

    pr_files = parse_lcov(args.pr_lcov)
    baseline_files = parse_lcov(args.baseline_lcov)
    changed = [p for p in changed_source_files(args.compare_ref) if is_production_source(p)]

    print(f"New file minimum coverage: {args.new_file_min:.2f}%")
    print(f"Modified file tolerance: {args.tolerance:.1f} percentage points")
    print(f"Gate headers: {args.gate_headers}")
    print(f"Changed production source files: {len(changed)}")

    failures: list[str] = []
    rows: list[str] = []

    for path in changed:
        if not args.gate_headers and Path(path).suffix.lower() in HEADER_SUFFIXES:
            rows.append(f"| `{path}` | — | — | header file (skipped) | SKIP |")
            print(f"  [SKIP] {path}: header file excluded from gating")
            continue

        pr_cov = lookup_file(pr_files, path)
        base_cov = lookup_file(baseline_files, path)
        pr_pct = pr_cov.percent if pr_cov else 0.0
        rule = gate_rule(path, base_cov, args.new_file_min, args.tolerance, args.compare_ref)
        ok = rule.passes(pr_pct)
        status = "PASS" if ok else "FAIL"
        rows.append(
            f"| `{path}` | {pr_pct:.2f}% | {rule.operator} {rule.threshold:.2f}% | {rule.basis} | {status} |"
        )
        if not ok:
            failures.append(
                f"{path}: PR {pr_pct:.2f}% does not meet required {rule.operator} "
                f"{rule.threshold:.2f}% ({rule.basis})"
            )
        print(
            f"  [{status}] {path}: PR={pr_pct:.2f}% required {rule.operator} "
            f"{rule.threshold:.2f}% ({rule.basis})"
        )

    md = [
        "## SWSS coverage gate (S3 baseline)",
        "",
        f"- New file minimum coverage: **{args.new_file_min:.2f}%**",
        f"- Modified file rule: PR coverage **≥** (baseline − {args.tolerance:.1f}pp) from S3",
        f"- Pre-existing untested files: **skipped** (no baseline lcov data)",
        f"- Compare ref: `{args.compare_ref}`",
        f"- Changed production files checked: **{len(changed)}**",
        "",
        "| File | PR coverage | Required | Basis | Result |",
        "| --- | ---: | --- | --- | --- |",
        *rows,
        "",
    ]
    if failures:
        md += ["### Failures", ""] + [f"- {item}" for item in failures] + [""]
    else:
        md += ["All changed production files meet the coverage gate.", ""]
    args.report_md.write_text("\n".join(md))

    if not changed:
        print("No changed production source files to gate.")
        return 0
    if failures:
        print("\nCoverage gate FAILED:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("\nCoverage gate PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
