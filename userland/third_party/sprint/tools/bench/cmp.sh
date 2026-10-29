#!/bin/sh
# usage: tools/bench/cmp.sh [sprint] -- wall time per benchmark (best of 3): Sprint with the JIT, Sprint without it
# (SPRINT_JIT=0), Lua 5.4 and, when installed, LuaJIT. The last column is Sprint(JIT) / LuaJIT (smaller is faster).
S=${1:-build/sprint}; D=$(dirname "$0")
best() { b=999; for i in 1 2 3; do s=$(date +%s.%N); "$@" >/dev/null 2>&1; e=$(date +%s.%N); t=$(echo "$e - $s" | bc); b=$(echo "if ($t < $b) $t else $b" | bc); done; echo $b; }
HAVE_LJ=0; command -v luajit >/dev/null 2>&1 && HAVE_LJ=1
printf '%-9s %8s %8s %8s %8s %9s\n' bench jit nojit lua5.4 luajit 'jit/lj'
for l in "$D"/lua/*.lua; do n=$(basename "$l" .lua); [ -f "$D/$n.sp" ] || continue
  a=$(best "$S" run "$D/$n.sp"); a0=$(SPRINT_JIT=0 best "$S" run "$D/$n.sp"); b=$(best lua5.4 "$l")
  if [ $HAVE_LJ = 1 ]; then lj="$D/lua/$n.luajit.lua"; [ -f "$lj" ] || lj="$l"; j=$(best luajit "$lj"); r=$(echo "$a / $j" | bc -l); else j=0; r=0; fi
  printf '%-9s %8.2f %8.2f %8.2f %8.2f %9.2f\n' "$n" "$a" "$a0" "$b" "$j" "$r"; done
