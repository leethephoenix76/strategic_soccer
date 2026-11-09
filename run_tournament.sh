#!/bin/bash
# Round-robin bot tournament on the server (08.11.2026). README_TOURNAMENT.md.
#   ./run_tournament.sh smoke          build + engine tests + a 1-minute check of the chain
#   ./run_tournament.sh run [opts]     the tournament (resumable), then the archive of results
#   ./run_tournament.sh status         progress and the standings so far
#   ./run_tournament.sh pack           only the archive soccer_tournament_<host>_<date>.tgz
# opts go to tools/server/tournament.py: --seeds 7,19,31,43 --time 120 --jobs N --turns 22
set -e
cd "$(dirname "$0")"
PY=${PY:-python3}
what=${1:-run}
shift || true
pack() {
    local name="soccer_tournament_$(hostname -s)_$(date +%Y%m%d_%H%M).tgz"
    tar czf "$name" --exclude='tournament/ssbot' --exclude='*.part.*' tournament
    echo "archive: $PWD/$name"
}
case "$what" in
smoke)
    ./build_linux.sh 2>&1 | tee build_tournament.log | tail -3
    build/linux/engine_tests | tail -1
    $PY tools/server/tournament.py smoke "$@" | tail -16
    echo "smoke OK: now  nohup ./run_tournament.sh run > tournament.log 2>&1 &"
    ;;
run)
    [ -x build/linux/ssbot ] || ./build_linux.sh 2>&1 | tee build_tournament.log | tail -3
    $PY tools/server/tournament.py run "$@" || true
    pack
    ;;
status) $PY tools/server/tournament.py status "$@" ;;
pack) pack ;;
*) echo "usage: $0 smoke|run|status|pack [options]"; exit 2 ;;
esac
