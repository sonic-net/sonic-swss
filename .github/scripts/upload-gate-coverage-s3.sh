#!/usr/bin/env bash
# Upload gate coverage to S3. One upload per repo/branch/UTC-day; skip if already present.
set -euo pipefail

REPO_SLUG="${1:?usage: upload-gate-coverage-s3.sh <repo-slug> [subpath]}"
SUBPATH="${2:-}"
: "${S3_COVERAGE_BUCKET:?S3_COVERAGE_BUCKET required}"
: "${GATE_BRANCH_NAME:?GATE_BRANCH_NAME required}"

HTML_DIR="${HTML_DIR:-html}"
COVERAGE_INFO="${COVERAGE_INFO:-coverage.info}"
COVERAGE_XML="${COVERAGE_XML:-coverage.xml}"

DAY="$(date -u +"%Y-%m-%d")"
SAFE_REF="${GATE_BRANCH_NAME//\//_}"
if [ -n "$SUBPATH" ]; then
  S3_PREFIX="${REPO_SLUG}/${SUBPATH}"
else
  S3_PREFIX="${REPO_SLUG}"
fi
S3_DEST="s3://${S3_COVERAGE_BUCKET}/${S3_PREFIX}/${DAY}/${SAFE_REF}"

if aws s3 ls "${S3_DEST}/coverage.info" >/dev/null 2>&1; then
  echo "Coverage report already uploaded today: ${S3_DEST}/"
  exit 0
fi

if [ ! -d "${HTML_DIR}" ]; then
  echo "::error::${HTML_DIR}/ missing"
  exit 1
fi
if [ ! -f "${COVERAGE_INFO}" ]; then
  echo "::error::${COVERAGE_INFO} missing"
  exit 1
fi

aws s3 sync "${HTML_DIR}/" "${S3_DEST}/" --exclude "*.gcda" --exclude "*.gcno"
aws s3 cp "${COVERAGE_INFO}" "${S3_DEST}/coverage.info"
if [ -f "${COVERAGE_XML}" ]; then
  aws s3 cp "${COVERAGE_XML}" "${S3_DEST}/coverage.xml"
fi
echo "Uploaded to ${S3_DEST}/"
