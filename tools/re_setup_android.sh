#!/bin/bash
# Builds the Android release's reverse-engineering groundwork in re/android/ (never committed) from
# your own unpacked APK (unzip it first):
#   tools/re_setup_android.sh <unpacked APK folder>
# Needs llvm-nm / llvm-objdump and Ghidra (GHIDRA=/opt/ghidra by default). Produces, per native
# library of the game (lib/<abi>/lib*_native.so, Thumb-2, its functions exported by name):
#   re/android/<lib>/symbols.txt, disasm.txt     llvm-nm -D, llvm-objdump
#   re/android/<lib>/decomp.c, functions.tsv, features.jsonl, ghidra/
# and for the Java side (classes.dex): re/android/dex/decomp.c, functions.tsv (Ghidra's Dalvik).
set -e
[ $# -eq 1 ] || { sed -n 2,4p "$0"; exit 1; }
R="$(cd "$(dirname "$0")/.." && pwd)"; T="$R/tools"; G="${GHIDRA:-/opt/ghidra}/support/analyzeHeadless"
A="$1"
for so in "$A"/lib/*/lib*_native.so; do
    n="$(basename "$so" .so)"; o="$R/re/android/$n"
    mkdir -p "$o/ghidra"
    echo "native: $n"
    llvm-nm -D -n --defined-only "$so" > "$o/symbols.txt"
    llvm-objdump -d --triple=thumbv7 "$so" > "$o/disasm.txt"
    "$G" "$o/ghidra" "$n" -import "$so" -overwrite -scriptPath "$T/ghidra" \
        -postScript ExportDecomp.java "$o" -postScript ExportFeatures.java "$o/features.jsonl" > "$o/ghidra.log" 2>&1
done
if [ -f "$A/classes.dex" ]; then
    o="$R/re/android/dex"; mkdir -p "$o/ghidra"
    echo "dex"
    "$G" "$o/ghidra" dex -import "$A/classes.dex" -overwrite -scriptPath "$T/ghidra" \
        -postScript ExportDecomp.java "$o" > "$o/ghidra.log" 2>&1
fi
echo "done: re/android"
