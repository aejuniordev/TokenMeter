#!/usr/bin/env bash
# Teste rápido do .exe no Wine (Linux): abre o programa sem tela, confere o
# servidor local, envia um uso e um limite e verifica o estado devolvido.
#
#   ./scripts/smoke-test.sh dist/TokenMeter.exe
set -euo pipefail
EXE="$(realpath "${1:-dist/TokenMeter.exe}")"
PORT=47821
WINE="$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)"
export WINEPREFIX="${WINEPREFIX:-$(mktemp -d)/wine}" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export DISPLAY=:97
WUSER="${USER:-$(id -un)}"

Xvfb :97 -screen 0 1280x800x24 >/dev/null 2>&1 &
XPID=$!
cleanup() { wineserver -k >/dev/null 2>&1 || /usr/lib/wine/wineserver -k >/dev/null 2>&1 || true; kill "$XPID" 2>/dev/null || true; }
trap cleanup EXIT

if curl -fsS -m 2 "http://127.0.0.1:$PORT/state" >/dev/null 2>&1; then
  echo "ERRO: já existe algo na porta $PORT; feche-o antes do teste"; exit 1
fi

echo "==> preparando o Wine"
timeout 180 "$WINE" wineboot -i >/dev/null 2>&1 || true
# sem consulta ao Claude Code durante o teste
mkdir -p "$WINEPREFIX/drive_c/users/$WUSER/AppData/Roaming"
printf '[TokenMeter]\r\nrefresh_minutes=0\r\n' > "$WINEPREFIX/drive_c/users/$WUSER/AppData/Roaming/TokenMeter.ini"

echo "==> abrindo $EXE"
"$WINE" "$EXE" >/dev/null 2>&1 &
for i in $(seq 1 60); do
  if curl -fsS -m 2 "http://127.0.0.1:$PORT/state" >/dev/null 2>&1; then break; fi
  sleep 1
done
curl -fsS -m 5 "http://127.0.0.1:$PORT/state" >/dev/null || { echo "ERRO: o TokenMeter não respondeu"; exit 1; }

echo "==> enviando dados de teste"
curl -fsS -m 5 -X POST "http://127.0.0.1:$PORT/usage" \
  -d '{"model":"claude-opus-5-5","usage":{"input_tokens":1000,"cache_read_input_tokens":240000,"cache_creation_input_tokens":4000,"output_tokens":5000}}' >/dev/null
NOW=$(date +%s)
curl -fsS -m 5 -X POST "http://127.0.0.1:$PORT/limits" \
  -d "{\"rate_limits\":{\"five_hour\":{\"used_percentage\":64,\"resets_at\":$((NOW+3600))}}}" >/dev/null
STATE="$(curl -fsS -m 5 "http://127.0.0.1:$PORT/state")"
echo "$STATE"
python3 - "$STATE" <<'PY'
import json, sys
s = json.loads(sys.argv[1])
assert abs(s["percent"] - 25.0) < 0.01, s["percent"]
assert s["limits"]["five_hour"]["used_percentage"] == 64
assert 0 < s["limits"]["five_hour"]["seconds_left"] <= 3600
print("OK: servidor, contexto e limites funcionando")
PY
