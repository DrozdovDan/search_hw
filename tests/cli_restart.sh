#!/usr/bin/env bash
set -euo pipefail
engine=$1
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
cat > "$temporary/corpus.jsonl" <<'EOF'
{"docid":"d1","title":"Machine","url":"https://example.org/1","headings":"","body":"learning"}
{"docid":"d2","title":"Deep","url":"","headings":"","body":"learning python"}
{"docid":"d3","title":"","url":"","headings":"","body":""}
EOF
"$engine" index --corpus "$temporary/corpus.jsonl" --output "$temporary/index" --segment-docs 1 --threads 2 > "$temporary/build.json"
"$engine" search --index "$temporary/index" --query '(machine OR deep) ANDNOT python' > "$temporary/first"
"$engine" search --index "$temporary/index" --query '(machine OR deep) ANDNOT python' > "$temporary/second"
cmp "$temporary/first" "$temporary/second"
rg -q '"docid":"d1"' "$temporary/first"
[[ $(wc -l < "$temporary/first") -eq 1 ]]
"$engine" validate --index "$temporary/index"
"$engine" stats --index "$temporary/index" > "$temporary/stats"
rg -q '"documents": 3' "$temporary/stats"
if "$engine" search --index "$temporary/index" --query 'machine AND' 2>/dev/null; then exit 1; fi
if "$engine" index --corpus "$temporary/corpus.jsonl" --output "$temporary/index" 2>/dev/null; then exit 1; fi
if "$engine" search --index "$temporary/index" --query machine --top-k -1 2>/dev/null; then exit 1; fi
printf 'machine\n:quit\n' | "$engine" interactive --index "$temporary/index" > "$temporary/interactive"
rg -q '"docid":"d1"' "$temporary/interactive"
