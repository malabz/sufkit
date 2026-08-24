#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -u

executable=$1
reference=$2
output=$3
log_directory=$4

"${executable}" --reference "${reference}" --method sa32 \
  --output "${output}" >"${log_directory}/race-1.stdout" \
  2>"${log_directory}/race-1.stderr" &
first_pid=$!
"${executable}" --reference "${reference}" --method sa32 \
  --output "${output}" >"${log_directory}/race-2.stdout" \
  2>"${log_directory}/race-2.stderr" &
second_pid=$!

set +e
wait "${first_pid}"
first_status=$?
wait "${second_pid}"
second_status=$?
set -e

if { [[ ${first_status} -eq 0 && ${second_status} -ne 0 ]] ||
     [[ ${second_status} -eq 0 && ${first_status} -ne 0 ]]; }; then
  exit 0
fi

echo "expected exactly one concurrent writer to succeed; statuses were " \
  "${first_status} and ${second_status}" >&2
exit 1
