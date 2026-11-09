#!/bin/bash
# Linux build (g++ 9+ or clang++): build/linux/ssbot and the test programs.
#   ./build_linux.sh            optimized build (-O2 -march=native)
#   ./build_linux.sh --tests    also runs the tests
#   CXX=clang++ ./build_linux.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -DNDEBUG -march=native -pthread -Isrc"
OUT=build/linux
mkdir -p $OUT
SRCS="src/bot/*.cpp src/engine/*.cpp"
echo "ssbot..."
$CXX $FLAGS -o $OUT/ssbot tools/ssbot.cpp $SRCS &
echo "bot_tests..."
$CXX $FLAGS -o $OUT/bot_tests tests/bot_tests.cpp $SRCS &
$CXX $FLAGS -o $OUT/engine_tests tests/engine_tests.cpp src/engine/*.cpp &
$CXX $FLAGS -o $OUT/ball_race_tests tests/ball_race_tests.cpp $SRCS &
$CXX $FLAGS -o $OUT/two_vs_one_tests tests/two_vs_one_tests.cpp $SRCS &
wait
echo "built: $(ls $OUT)"
if [ "$1" == "--tests" ]; then
    for t in engine_tests bot_tests ball_race_tests two_vs_one_tests; do
        echo "== $t"; $OUT/$t | tail -1
    done
fi
