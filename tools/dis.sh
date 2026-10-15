#!/bin/sh
# Usage: dis.sh <symbol>...  — print a function from the pre-generated ARMv7 disassembly
D="$(dirname "$0")/../re/disasm.txt"
for s in "$@"; do
  awk -v h="<$s>:" 'index($0, h) && /^[0-9a-f]+ </ {p=1; print; next} p && /^[0-9a-f]+ </ {exit} p' "$D"
done
