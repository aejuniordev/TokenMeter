#!/usr/bin/env bash
# Monta os arquivos da release em dist/release:
#   TokenMeter.exe, TokenMeter-<versão>-win64.zip e SHA256SUMS.txt
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION="${1:-dev}"; VERSION="${VERSION#v}"
OUT=dist/release; STAGE=dist/stage/TokenMeter
rm -rf dist/release dist/stage
mkdir -p "$OUT" "$STAGE/exemplos"
cp dist/TokenMeter.exe "$STAGE/"
cp README.md "$STAGE/LEIA-ME.md"
cp exemplos/*.py exemplos/*.mjs exemplos/*.ps1 "$STAGE/exemplos/"
(cd dist/stage && zip -qr "../release/TokenMeter-$VERSION-win64.zip" TokenMeter)
cp dist/TokenMeter.exe "$OUT/"
(cd "$OUT" && sha256sum TokenMeter.exe "TokenMeter-$VERSION-win64.zip" > SHA256SUMS.txt)
ls -l "$OUT"; cat "$OUT/SHA256SUMS.txt"
