#!/bin/sh
# Compare the performance of two builds of termesh, scene by scene.
#
#   scripts/perf-check.sh <new-binary> [<old-binary>]
#
# Wall-clock time on a shared machine is too noisy to catch a 5% slowdown, so
# each benchmark scene is run under valgrind's cachegrind, which simulates
# the program and counts the instructions it executes. That count depends
# only on the code and its input: it comes out the same to within a fraction
# of a percent on any machine, any day.
#
# Exits 1 if any scene executes more than PERF_THRESHOLD percent (default 5)
# more instructions in the new build than in the old one. Set PERF_ALLOW=1
# to report without failing - for a change that is slower on purpose.
#
# Also reports scenes whose counters or checksum changed: not a failure, but
# a change in what is drawn should not come as a surprise.
set -eu

new=${1:?usage: perf-check.sh <new-binary> [<old-binary>]}
old=${2:-}
threshold=${PERF_THRESHOLD:-5}
frames=${PERF_FRAMES:-4}
summary=${GITHUB_STEP_SUMMARY:-/dev/null}

if ! command -v valgrind >/dev/null 2>&1; then
    echo "perf-check: valgrind is not installed" >&2
    exit 2
fi

# Instructions executed by one scene.
instructions() {
    valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null \
        "$1" --benchmark-scene "$2" --threads 1 --frames "$frames" 2>&1 >/dev/null |
        sed -n 's/.*I *refs: *\([0-9,]*\).*/\1/p' | tr -d ','
}

# The reproducible part of a scene's results: counters and checksum.
counters() {
    "$1" --benchmark-scene "$2" --threads 1 --frames "$frames" | awk -v s="$2" '$1 == s { print $3, $4, $5, $NF }'
}

have_old=0
if [ -n "$old" ] && [ -x "$old" ] && "$old" --benchmark-list >/dev/null 2>&1; then have_old=1; fi

{
    echo "### Performance"
    echo
    if [ "$have_old" = 1 ]; then
        echo "Instructions executed per scene ($frames frames, one thread), previous commit against this one."
        echo
        echo "| scene | before | after | change | output |"
        echo "| --- | ---: | ---: | ---: | --- |"
    else
        echo "No earlier build with a benchmark suite to compare against; this commit only."
        echo
        echo "| scene | instructions |"
        echo "| --- | ---: |"
    fi
} | tee -a "$summary"

status=0
for scene in $("$new" --benchmark-list); do
    after=$(instructions "$new" "$scene")
    if [ -z "$after" ]; then
        echo "perf-check: no instruction count for $scene" >&2
        exit 2
    fi
    if [ "$have_old" != 1 ]; then
        echo "| $scene | $after |" | tee -a "$summary"
        continue
    fi
    if ! "$old" --benchmark-list | grep -qx "$scene"; then
        echo "| $scene | - | $after | new | |" | tee -a "$summary"
        continue
    fi
    before=$(instructions "$old" "$scene")
    change=$(awk -v a="$after" -v b="$before" 'BEGIN { printf "%+.2f", (a / b - 1) * 100 }')
    output=same
    if [ "$(counters "$new" "$scene")" != "$(counters "$old" "$scene")" ]; then output=changed; fi
    flag=""
    if awk -v c="$change" -v t="$threshold" 'BEGIN { exit !(c > t) }'; then
        flag=" :warning:"
        status=1
    fi
    echo "| $scene | $before | $after | $change%$flag | $output |" | tee -a "$summary"
done

if [ "$status" = 1 ]; then
    echo
    if [ "${PERF_ALLOW:-0}" = 1 ]; then
        echo "Slower than the threshold of $threshold%, but allowed for this commit." | tee -a "$summary"
        exit 0
    fi
    echo "At least one scene is more than $threshold% slower. If that is intended, put [perf-ok] in the commit message." |
        tee -a "$summary"
fi
exit $status
