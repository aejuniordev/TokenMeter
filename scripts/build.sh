#!/usr/bin/env bash
# Compila o TokenMeter.exe para Windows 64 bits com MinGW-w64.
# Funciona no Linux (cross-compile, como na pipeline) e no MSYS2/w64devkit.
#
#   ./scripts/build.sh            -> versão "dev"
#   ./scripts/build.sh 1.3.0      -> grava 1.3.0 no .exe (Propriedades > Detalhes e janela de debug)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-dev}"
VERSION="${VERSION#v}"
NUM="$(printf '%s' "$VERSION" | sed -E 's/[^0-9.].*$//')"   # 1.3.0-beta.1 -> 1.3.0
IFS='.' read -r MAJ MIN PAT _ <<< "${NUM}..."
MAJ="${MAJ:-0}"; MIN="${MIN:-0}"; PAT="${PAT:-0}"

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  CC="${CC:-x86_64-w64-mingw32-gcc}"; WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
else
  CC="${CC:-gcc}"; WINDRES="${WINDRES:-windres}"
fi

mkdir -p dist
echo "==> TokenMeter $VERSION ($MAJ.$MIN.$PAT) com $CC"
"$WINDRES" -DTM_VER_MAJOR="$MAJ" -DTM_VER_MINOR="$MIN" -DTM_VER_PATCH="$PAT" \
  "-DTM_VERSION_STR=\\\"$VERSION\\\"" tokenmeter.rc -O coff -o dist/tokenmeter.res
"$CC" -O2 -s -Wall -Wextra -municode -mwindows "-DTM_VERSION=\"$VERSION\"" \
  tokenmeter.c dist/tokenmeter.res -o dist/TokenMeter.exe \
  -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcomctl32 -lole32 -luuid
ls -l dist/TokenMeter.exe
