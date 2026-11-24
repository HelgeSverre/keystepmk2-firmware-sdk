#!/bin/bash
# Re-run the seeding and export scripts on an already imported image.   ghidra/reexport.sh 1.0.4
set -euo pipefail
cd "$(dirname "$0")"
V="${1:-1.0.4}"
GHIDRA="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.1.4/libexec}"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
"$GHIDRA/support/analyzeHeadless" project "ks2_$V" -process "ks2_$V.bin" -noanalysis \
  -scriptPath scripts -postScript KsSeed.java -postScript KsExport.java "out/$V"
