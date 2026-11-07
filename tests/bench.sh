#!/bin/sh
# Benchmark helper (not used at runtime).  Prints measured numbers only.
#
#   tests/bench.sh [BINARY] [MIB]
#
# Generates test files with build/gen (MIB each, default 100) in
# $BENCH_DIR (default ~/.cache/dcjson-bench) and measures, per file:
#   parse     --check --stats: best of three runs (warm page cache),
#             throughput, values, peak RSS
#   terminal  bytes sent per key in a 120x40 256-colour terminal
#             (build/tests --bench-render)
set -eu
BIN=${1:-./dcjson}
MIB=${2:-100}
DIR=${BENCH_DIR:-$HOME/.cache/dcjson-bench}
GEN=build/gen
mkdir -p "$DIR"
[ -x "$GEN" ] || make -s build/gen
[ -x build/tests ] || make -s build/tests

echo "binary   : $BIN ($(stat -c %s "$BIN") bytes, $(size "$BIN" | awk 'NR==2 {print "text " $1 ", data " $2 ", bss " $3}'))"
echo "cpu      : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
echo
printf '%-9s %9s %9s %12s %12s %10s\n' kind size seconds throughput values peak-rss
for k in records numbers strings deep lines; do
    f="$DIR/$k-$MIB.json"
    [ -s "$f" ] || "$GEN" "$k" "$MIB" > "$f"
    best=""
    for r in 1 2 3; do
        out=$("$BIN" --check --stats $( [ "$k" = lines ] && echo --lines ) "$f" 2>&1 >/dev/null)
        s=$(echo "$out" | sed -n 's/.* in \([0-9.]*\) s .*/\1/p' | head -1)
        if [ -z "$best" ] || awk "BEGIN{exit !($s < $best)}"; then best=$s; line=$out; fi
    done
    size=$(echo "$line" | sed -n 's/.*parsed \(.*\) in .*/\1/p' | head -1)
    rate=$(echo "$line" | sed -n 's/.*(\(.*\)\/s).*/\1/p' | head -1)
    vals=$(echo "$line" | sed -n 's/.*, \([0-9,]*\) values.*/\1/p' | head -1)
    rss=$(echo "$line" | sed -n 's/.*peak rss \([0-9]*\) KiB.*/\1/p' | head -1)
    printf '%-9s %9s %9s %10s/s %12s %7s MiB\n' "$k" "$size" "$best" "$rate" "$vals" "$((rss / 1024))"
done
echo
echo "terminal output, records file:"
./build/tests --bench-render "$DIR/records-$MIB.json"
