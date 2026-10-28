#!/usr/bin/env bash
set -euo pipefail
engine="$(realpath "$1")"
project_dir="$(realpath "$2")"
test_dir="$(mktemp -d)"
export SEARCH_ENGINE="$engine"
export SEARCH_INDEX="$test_dir/index"
export SEARCH_WEB_STATE_DIR="$test_dir/state"
export SEARCH_PORT="$((40000 + $$ % 20000))"
export SEARCH_HOST=127.0.0.1
manager="$project_dir/scripts/web_server.sh"
cleanup() {
  "$manager" stop > /dev/null 2>&1 || true
  rm -r -- "$test_dir"
}
trap cleanup EXIT
printf '{"docid":"test","title":"machine","headings":"","body":"machine learning"}\n' > "$test_dir/corpus.jsonl"
"$engine" index --corpus "$test_dir/corpus.jsonl" --output "$SEARCH_INDEX" > "$test_dir/build.json"

# Параллельные start должны создать ровно один сервер и сохранить его PID.
"$manager" start > "$test_dir/start-1.txt" &
first_start=$!
"$manager" start > "$test_dir/start-2.txt" &
second_start=$!
wait "$first_start"
wait "$second_start"
read -r original_pid < "$SEARCH_WEB_STATE_DIR/web-server.pid"
curl --noproxy '*' -fsS "http://127.0.0.1:$SEARCH_PORT/healthz" | jq -e '.status == "ok"' > /dev/null
"$manager" start > /dev/null
[[ "$(cat "$SEARCH_WEB_STATE_DIR/web-server.pid")" == "$original_pid" ]]

# Воспроизведение ошибки: PID-файл заменён PID завершившегося запуска.
printf '99999999\n' > "$SEARCH_WEB_STATE_DIR/web-server.pid"
"$manager" status > /dev/null
[[ "$(cat "$SEARCH_WEB_STATE_DIR/web-server.pid")" == "$original_pid" ]]
rm -- "$SEARCH_WEB_STATE_DIR/web-server.pid"
"$manager" start > /dev/null
[[ "$(cat "$SEARCH_WEB_STATE_DIR/web-server.pid")" == "$original_pid" ]]

"$manager" stop > /dev/null
if "$manager" status > /dev/null; then printf 'Stopped server reported as running\n' >&2; exit 1; fi
if curl --noproxy '*' -fsS --max-time 1 "http://127.0.0.1:$SEARCH_PORT/healthz" > /dev/null 2>&1; then
  printf 'Stopped server still accepts requests\n' >&2; exit 1
fi
printf 'Web lifecycle passed: concurrent starts, repeated start, stale/missing PID recovery, stop\n'
