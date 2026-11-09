#!/bin/bash
# Strategic Soccer: the server runs (docs/server_plan.md, README_SERVER.md).
#
#   ./run_server.sh smoke            a few minutes: build, tests, every stage on tiny settings
#   ./run_server.sh all              every stage below in order (a day or more)
#   ./run_server.sh scaling curves   chosen stages only
#   nohup ./run_server.sh all > run.log 2>&1 &     (or inside tmux / screen)
#
# Stages: info build scaling calib curves ladder node consistency candidates verify report pack
# Every stage continues where it stopped: rerun the same command after an interruption.
# Settings (environment): JOBS - searches at once (default: physical cores),
#   LEVELS - reference levels "width x depth", DEPTHS - ladder depths, BUDGETS - curves seconds,
#   CONS_DEPTHS / CONS_DEEP / CONS_DEEPER - consistency depths and the subsets searched at 5 and 6.
#   Measured on the server (03.11): one search at depth 6 does not fit a 6 h timeout.
set -u
cd "$(dirname "$0")"
sed -i 's/\r$//' build_linux.sh tools/server/*.py tools/server/*.txt 2>/dev/null   # files edited on Windows

read LOGICAL PHYS < <(python3 -c "import sys; sys.path.insert(0, 'tools/server'); import sslib; print(*sslib.cpu_info())")
[ "${PHYS:-0}" -gt 0 ] || { LOGICAL=$(nproc); PHYS=$LOGICAL; }
JOBS=${JOBS:-$PHYS}
LEVELS=${LEVELS:-8x3,8x4,16x3,8x5,16x4}
DEPTHS=${DEPTHS:-1,2,3,4,5}
CONS_DEPTHS=${CONS_DEPTHS:-1,2,3,4,5}   # consistency: 1-4 for all, 5 for CONS_DEEP positions, 6 for CONS_DEEPER
CONS_DEEP=${CONS_DEEP:-100}
CONS_DEEPER=${CONS_DEEPER:-0}
BUDGETS=${BUDGETS:-30,60,120,240,480,960,1920,3840}
PY=${PY:-python3}
export SS_RESULTS=${SS_RESULTS:-$PWD/results}
mkdir -p "$SS_RESULTS/logs"

stage() {   # stage NAME command...: runs with a log, stamps start and end
    local name=$1; shift
    echo "=== $(date '+%F %T') $name: $*" | tee -a "$SS_RESULTS/logs/stages.log"
    "$@" 2>&1 | tee -a "$SS_RESULTS/logs/$name.log"
    local rc=${PIPESTATUS[0]}
    echo "=== $(date '+%F %T') $name done (exit $rc)" | tee -a "$SS_RESULTS/logs/stages.log"
    return $rc
}

B="$PY tools/server/bench.py"
R="$PY tools/server/reference.py"
K="$PY tools/server/consistency.py"

run() {
    case $1 in
    info)       stage info $B info ;;
    build)      stage build ./build_linux.sh --tests || { echo "build failed"; exit 1; } ;;
    scaling)    stage scaling $B scaling --depth 3
                stage scaling $B scaling --depth 4 ;;
    calib)      stage calib $B calib --depth 3 --jobs "$PHYS,$LOGICAL" ;;
    curves)     stage curves $B curves --budgets "$BUDGETS" --jobs "$JOBS" ;;
    ladder)     stage ladder $B ladder --depths "$DEPTHS" --jobs "$JOBS" ;;
    node)       stage node $B ladder --depths 1,2,3,4 --root-as-node --jobs "$JOBS" ;;   # the root as an inner node (03.11)
    consistency) stage consistency $K sample --max 600
                stage consistency $K static --jobs "$JOBS"
                stage consistency $K depths --depths "$CONS_DEPTHS" --deep "$CONS_DEEP" --deeper "$CONS_DEEPER" --jobs "$JOBS"
                stage consistency $K depths --depths 1,2,3 --root-as-node --jobs "$JOBS"
                stage consistency $K horizon --count 150 --line-depth 3 --check 1,2,3 --jobs "$JOBS"
                stage consistency $K report ;;
    candidates) stage candidates $R candidates --root-width 16 --root-depth 3 --jobs "$JOBS" --levels "$LEVELS" ;;
    verify)     stage verify $R verify --levels "$LEVELS" --jobs "$JOBS" ;;
    report)     stage report $R report --levels "$LEVELS" ;;
    pack)       local f="soccer_results_$(hostname -s)_$(date +%Y%m%d_%H%M).tgz"
                tar czf "$f" -C "$(dirname "$SS_RESULTS")" "$(basename "$SS_RESULTS")" && echo "packed: $PWD/$f" ;;
    smoke)      # tiny versions of every stage, into results_smoke/
                export SS_RESULTS=$PWD/results_smoke; mkdir -p "$SS_RESULTS/logs"
                C=g7_30b_episode,g2_148w_longpass
                stage info $B info
                stage build ./build_linux.sh --tests || exit 1
                stage scaling $B scaling --depth 2 --cases $C --threads 1,2,$PHYS
                stage calib $B calib --depth 2 --cases $C --jobs $PHYS
                stage curves $B curves --budgets 2,5 --cases $C --jobs $PHYS
                stage ladder $B ladder --depths 1,2,3 --cases $C --jobs $PHYS
                stage consistency $K sample --max 20
                stage consistency $K static --jobs $PHYS
                stage consistency $K depths --depths 1,2 --deep 0 --deeper 0 --jobs $PHYS
                stage consistency $K horizon --count 4 --line-depth 1 --check 1 --jobs $PHYS
                stage consistency $K report
                stage candidates $R candidates --root-width 8 --root-depth 2 --cases $C --jobs $PHYS --levels 8x1,8x2
                stage verify $R verify --levels 8x1,8x2 --cases $C --jobs $PHYS
                stage report $R report --levels 8x1,8x2 --cases $C
                echo "smoke: see $SS_RESULTS (ref_report.txt, cons_report.txt, *.csv, logs/)" ;;
    *)          echo "unknown stage $1"; exit 2 ;;
    esac
}

[ $# -gt 0 ] || { sed -n '2,15p' "$0"; exit 0; }
if [ "$1" == "all" ]; then set -- info build scaling calib curves ladder node consistency candidates verify report pack; fi
echo "physical cores $PHYS, logical $LOGICAL, jobs $JOBS, results $SS_RESULTS"
for s in "$@"; do run "$s"; done
