#!/bin/sh
# Local regression tests (need level files exported from your own disks; CI has none).
# Usage: tests/run.sh data/level1.bin [data/level2.bin]
set -e
L1=$(realpath "${1:?level file}"); L2=$(realpath "${2:-$1}")
cd "$(dirname "$0")"
gcc -w -O1 -g -fsanitize=address,undefined -o /tmp/cad_fuzz fuzz.c -lm
gcc -w -O1 -o /tmp/cad_carry carry.c -lm
for s in 1 2 3 4; do ASAN_OPTIONS=detect_leaks=0 /tmp/cad_fuzz "$L1" $s; done
/tmp/cad_carry "$L1" "$L2"
