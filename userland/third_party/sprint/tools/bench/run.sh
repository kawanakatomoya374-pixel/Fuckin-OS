#!/bin/sh
# usage: tools/bench/run.sh [path-to-sprint]   (default build/sprint)
S=${1:-build/sprint}
D=$(dirname "$0")
for f in "$D"/*.sp; do
  s=$(date +%s.%N); "$S" run "$f" >/dev/null; e=$(date +%s.%N)
  printf '%-10s %.2fs\n' "$(basename "$f" .sp)" "$(echo "$e - $s" | bc)"
done
