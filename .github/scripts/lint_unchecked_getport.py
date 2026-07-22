#!/usr/bin/env python3
# Copyright (c) 2026 UpScale AI, Inc. All rights reserved.
#
# Lint: forbid statement-form gPortsOrch->getPort() calls that ignore the
# return value (UPSW-7294).
#
# Why: getPort() failure leaves the output Port default-constructed (empty
# m_alias, m_port_id == 0, empty m_queue_ids vector). Code that proceeds
# without checking the return value operates on that empty object; indexing
# m_queue_ids on it is a NULL dereference that crashed orchagent on a
# `DEL PFC_WD|GLOBAL` (see pfcwdorch.cpp history).
#
# Correct form:
#     Port port;
#     if (!gPortsOrch->getPort(name, port))
#     {
#         ... handle the miss ...
#     }
#
# Forbidden form (what this lint matches):
#     gPortsOrch->getPort(name, port);      // result silently ignored
#
# This is a ratchet: pre-existing violations are allowlisted per file below.
# New violations fail the build. When a file is cleaned up, lower (or remove)
# its allowlist entry so the count can only shrink.
#
# Limitation: only single-line statement-form calls are matched. That is the
# shape of every known violation; multi-line calls whose result feeds an
# `if` are already compliant.

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

# file (relative to repo root) -> maximum allowed unchecked call sites.
# Ratchet DOWN only. Do not add entries or raise counts; fix the call site
# instead (check the return value and handle the miss). All pre-existing
# violations were fixed with the introduction of this lint (UPSW-7294),
# so the allowlist starts empty.
ALLOWLIST: dict[str, int] = {}

# Statement-form call: entire line is `gPortsOrch->getPort(...);` with the
# result discarded (optionally followed by a comment).
PATTERN = re.compile(r"^\s*gPortsOrch->getPort\s*\([^;]*\)\s*;\s*(//.*|/\*.*)?$")

SCAN_GLOBS = ("orchagent/**/*.cpp",)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]

    violations: dict[str, list[int]] = {}
    for glob in SCAN_GLOBS:
        for path in sorted(repo_root.glob(glob)):
            rel = path.relative_to(repo_root).as_posix()
            lines = path.read_text(errors="replace").splitlines()
            hits = [i + 1 for i, line in enumerate(lines) if PATTERN.match(line)]
            if hits:
                violations[rel] = hits

    counts = Counter({f: len(hits) for f, hits in violations.items()})
    failed = False

    for f, hits in sorted(violations.items()):
        allowed = ALLOWLIST.get(f, 0)
        if counts[f] > allowed:
            failed = True
            print(f"FAIL: {f}: {counts[f]} unchecked gPortsOrch->getPort() call(s), allowlist permits {allowed}")
            for line in hits:
                print(f"  {f}:{line}: check the return value instead of discarding it")

    for f, allowed in sorted(ALLOWLIST.items()):
        actual = counts.get(f, 0)
        if actual < allowed:
            print(f"NOTE: {f}: allowlist permits {allowed} but only {actual} remain "
                  f"- lower its ALLOWLIST entry in {Path(__file__).name} to ratchet down")

    if failed:
        print()
        print("Unchecked gPortsOrch->getPort() discards the success/failure result and")
        print("continues with a default-constructed Port (UPSW-7294). Wrap the call:")
        print("    if (!gPortsOrch->getPort(name, port)) { /* handle miss */ }")
        return 1

    total = sum(counts.values())
    print(f"OK: {total} allowlisted unchecked getPort() call site(s), no new violations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
