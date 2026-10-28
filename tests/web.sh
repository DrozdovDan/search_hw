#!/usr/bin/env bash
set -euo pipefail
engine="$(realpath "$1")"
assets="$(realpath "$2")"
test_dir="$(mktemp -d)"
server_pid=""
cleanup() {
  if [[ -n "$server_pid" ]]; then kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi
  # Каталог создаётся самим тестом через mktemp и содержит только его данные.
  rm -r -- "$test_dir"
}
trap cleanup EXIT
cat > "$test_dir/corpus.jsonl" <<'JSONL'
{"docid":"doc-0","url":"https://example.com/zero","title":"Machine <script>alert(1)</script>","headings":"Заголовок\nВторой заголовок","body":"Machine learning. Полный текст за пределами короткого сниппета."}
{"docid":"doc-1","url":"https://example.com/one","title":"Machine model","headings":null,"body":"machine model"}
{"docid":"doc-2","url":"javascript:alert(1)","title":"Machine Python","headings":"Unsafe URL","body":"machine python"}
{"docid":"doc-3","url":"https://example.com/three","title":"Machine Python model","headings":"Three","body":"machine python model"}
JSONL
"$engine" index --corpus "$test_dir/corpus.jsonl" --output "$test_dir/index" --snippet-bytes 5 > "$test_dir/build.json"
# Формат официального корпуса: docid содержит смещение в распакованном потоке.
# Первый документ пересекает границу 128 КиБ, третий — начало нового gzip member.
mkdir "$test_dir/shards"
padding="$(head -c 132000 /dev/zero | tr '\0' a)"
printf '{"docid":"msmarco_doc_00_0","title":"Machine","headings":"Большой документ","body":"%s FULL END"}\n' "$padding" > "$test_dir/part1.jsonl"
second_offset="$(wc -c < "$test_dir/part1.jsonl")"
printf '{"docid":"msmarco_doc_00_%s","title":"Machine two","headings":null,"body":"Second body"}\n' "$second_offset" >> "$test_dir/part1.jsonl"
third_offset="$(wc -c < "$test_dir/part1.jsonl")"
printf '{"docid":"msmarco_doc_00_%s","title":"Machine three","headings":"Третий","body":"Third body"}\n' "$third_offset" > "$test_dir/part2.jsonl"
gzip -c "$test_dir/part1.jsonl" > "$test_dir/shards/msmarco_doc_00.gz"
gzip -c "$test_dir/part2.jsonl" >> "$test_dir/shards/msmarco_doc_00.gz"
tar -cf "$test_dir/corpus.tar" -C "$test_dir/shards" msmarco_doc_00.gz
"$engine" index --corpus "$test_dir/corpus.tar" --output "$test_dir/index-official" > "$test_dir/build-official.json"
port=$((40000 + $$ % 20000))
"$engine" serve --index "$test_dir/index" --indexes-root "$test_dir" --web-root "$assets" \
  --cache-dir "$test_dir/cache" --port "$port" > "$test_dir/server.log" 2>&1 &
server_pid=$!
base="http://127.0.0.1:$port"
ready=0
for attempt in {1..60}; do
  if curl -fsS "$base/healthz" -o "$test_dir/health.json" 2>/dev/null; then ready=1; break; fi
  if ! kill -0 "$server_pid" 2>/dev/null; then cat "$test_dir/server.log"; exit 1; fi
  sleep .1
done
[[ "$ready" == 1 ]]
curl -fsS "$base/" -o "$test_dir/index.html"
cmp "$assets/index.html" "$test_dir/index.html"
curl -fsS "$base/api/indexes" | jq -e '.default == "index" and .indexes[0].documents == 4' > /dev/null
curl -fsS "$base/api/search" -H 'Content-Type: application/json' \
  -d '{"query":"machine ANDNOT (python OR model)","limit":1}' > "$test_dir/first.json"
jq -e '.results | map(.internal_id) == [0]' "$test_dir/first.json" > /dev/null
curl -fsS "$base/api/search" -H 'Content-Type: application/json' \
  -d '{"query":"machine ANDNOT (python ANDNOT model)","limit":100}' | \
  jq -e '(.results | map(.internal_id)) == [0,1,3]' > /dev/null
curl -fsS "$base/api/search" -H 'Content-Type: application/json' \
  -d '{"query":"machine","limit":2}' | jq -e '.has_more and (.results | map(.internal_id)) == [0,1]' > /dev/null
curl -fsS "$base/api/search" -H 'Content-Type: application/json' \
  -d '{"query":"machine","limit":2,"after":1,"mode":"sequential"}' | \
  jq -e '(.has_more | not) and (.results | map(.internal_id)) == [2,3]' > /dev/null
curl -fsS "$base/api/document?index=index&id=0" > "$test_dir/document.json"
jq -e '.title == "Machine <script>alert(1)</script>" and .headings == "Заголовок\nВторой заголовок" and (.body | contains("за пределами короткого сниппета"))' \
  "$test_dir/document.json" > /dev/null
curl -fsS "$base/api/document?index=index&id=1" | jq -e '.headings == ""' > /dev/null
curl -fsS "$base/api/document?index=index&id=0" -o "$test_dir/cached.json"
cmp "$test_dir/document.json" "$test_dir/cached.json"
curl -fsS "$base/api/document?index=index-official&id=0" | \
  jq -e '.docid == "msmarco_doc_00_0" and (.body | endswith(" FULL END")) and (.body | length) == 132009' > /dev/null
curl -fsS "$base/api/document?index=index-official&id=1" | \
  jq -e --arg docid "msmarco_doc_00_$second_offset" '.docid == $docid and .headings == "" and .body == "Second body"' > /dev/null
curl -fsS "$base/api/document?index=index-official&id=2" | \
  jq -e --arg docid "msmarco_doc_00_$third_offset" '.docid == $docid and .body == "Third body"' > /dev/null
for body in '{"query":"machine AND"}' '{"query":"machine","limit":-1}' '{"query":"machine","limit":1.5}' '{"query":"machine","mode":"oops"}'; do
  status="$(curl -sS -o "$test_dir/error.json" -w '%{http_code}' "$base/api/search" -H 'Content-Type: application/json' -d "$body")"
  [[ "$status" == 400 ]]
  jq -e '.error | length > 0' "$test_dir/error.json" > /dev/null
done
for path in '/api/document?index=index&id=99' '/api/document?index=../other&id=0' '/not-a-file'; do
  status="$(curl -sS -o "$test_dir/error.json" -w '%{http_code}' "$base$path")"
  [[ "$status" == 404 ]]
done
curl -fsS "$base/healthz" | jq -e '.status == "ok"' > /dev/null
printf 'Web API integration passed\n'
