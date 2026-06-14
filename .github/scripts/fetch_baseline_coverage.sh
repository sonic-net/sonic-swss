#!/usr/bin/env bash
# Download the latest sonic-swss baseline coverage.info from S3 for gating.
set -euo pipefail

: "${S3_COVERAGE_BUCKET:?S3_COVERAGE_BUCKET required}"
BASELINE_BRANCH="${COVERAGE_BASELINE_BRANCH:-upscaleai-202511}"
SAFE_REF="${BASELINE_BRANCH//\//_}"
OUT="${1:-baseline.info}"

mapfile -t KEYS < <(
  aws s3api list-objects-v2 \
    --bucket "${S3_COVERAGE_BUCKET}" \
    --prefix "sonic-swss/" \
    --query "Contents[?ends_with(Key, \`${SAFE_REF}/coverage.info\`)].Key" \
    --output text 2>/dev/null | tr '\t' '\n' | grep -v '^None$' || true
)

if [ "${#KEYS[@]}" -eq 0 ]; then
  echo "::error::No baseline coverage.info found in s3://${S3_COVERAGE_BUCKET}/sonic-swss/ for ${SAFE_REF}"
  exit 1
fi

# Pick lexicographically latest stamp path (ISO8601 / date prefixes sort correctly).
BASELINE_KEY="$(printf '%s\n' "${KEYS[@]}" | sort | tail -1)"
echo "Using baseline coverage: s3://${S3_COVERAGE_BUCKET}/${BASELINE_KEY}"
aws s3 cp "s3://${S3_COVERAGE_BUCKET}/${BASELINE_KEY}" "${OUT}"
