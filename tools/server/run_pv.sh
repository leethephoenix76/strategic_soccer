#!/bin/bash
# Possession value experiment on the server (07.11.2026).
#   ./tools/server/run_pv.sh            build, then all stages (resumable)
#   ./tools/server/run_pv.sh suite      only the v4 pair at 90/180 s
#   ./tools/server/run_pv.sh cons       only static + depths + report (600 positions)
# Results: results_pv/ (needs results/cons_positions.csv from the earlier waves).
set -e
cd "$(dirname "$0")/../.."
PY=${PY:-python3}
BASE=goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-b8-t120
PV=goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-pv-b8-t120
export SS_RESULTS=${SS_RESULTS:-$PWD/results_pv}
mkdir -p "$SS_RESULTS"
[ -f "$SS_RESULTS/cons_positions.csv" ] || cp results/cons_positions.csv "$SS_RESULTS/" \
    || { echo "results/cons_positions.csv is missing"; exit 1; }
what=${1:-all}
if [ "$what" == all ]; then
    ./build_linux.sh --tests 2>&1 | tee "$SS_RESULTS/build.log" | tail -8
    grep -q "FAILED" "$SS_RESULTS/build.log" && { echo "tests failed"; exit 1; }
fi
if [ "$what" == all ] || [ "$what" == suite ]; then
    $PY tools/server/suite_pair.py --bots "$BASE,$PV" --budgets 90,180 2>&1 | tee -a "$SS_RESULTS/suite.log"
fi
if [ "$what" == all ] || [ "$what" == cons ]; then
    $PY tools/server/consistency.py static --bot "$PV" 2>&1 | tee -a "$SS_RESULTS/cons.log"
    $PY tools/server/consistency.py depths --bot "$PV" --depths 1,2,3,4,5 --deep 100 --deeper 0 2>&1 \
        | tee -a "$SS_RESULTS/cons.log"
    $PY tools/server/consistency.py report --bot "$PV" 2>&1 | tee -a "$SS_RESULTS/cons.log"
fi
tar czf "soccer_results_pv_$(hostname -s)_$(date +%Y%m%d_%H%M).tgz" "$(basename "$SS_RESULTS")"
echo "done: $(ls -t soccer_results_pv_*.tgz | head -1)"
