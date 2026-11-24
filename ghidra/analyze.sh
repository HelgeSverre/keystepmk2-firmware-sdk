#!/bin/bash
# Import a firmware image into a Ghidra project and export decompiled C, a function index and strings.
#   ghidra/analyze.sh [version]      e.g. ghidra/analyze.sh 1.0.4   ->  ghidra/out/1.0.4/
set -euo pipefail
cd "$(dirname "$0")"
V="${1:-1.0.4}"
GHIDRA="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.1.4/libexec}"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
IMAGE=$(ls ../firmware/"$V"/*.bin | head -1)
mkdir -p work project out
# The image has a 64-byte header; the body is what the device loads at 0x08008000.
tail -c +65 "$IMAGE" > "work/ks2_$V.bin"
"$GHIDRA/support/analyzeHeadless" project "ks2_$V" \
  -import "work/ks2_$V.bin" -overwrite \
  -loader BinaryLoader -loader-baseAddr 0x08008000 \
  -processor "ARM:LE:32:Cortex" -cspec default \
  -scriptPath scripts -preScript KsSetup.java -postScript KsSeed.java -postScript KsExport.java "out/$V" \
  -analysisTimeoutPerFile 2400
