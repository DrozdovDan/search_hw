#!/usr/bin/env bash
set -euo pipefail
engine=${ENGINE:-./build/search_engine}
corpus=${1:-data/synthetic.jsonl}
prefix=${2:-data/synthetic}
report=${3:-reports/synthetic}
mkdir -p "$(dirname "$report")"
if [[ ! -f "$corpus" ]]; then "$engine" generate --output "$corpus" --documents 1000000 --seed 42; fi
for size in 10000 100000 1000000; do
    index="$prefix-$size"
    if [[ ! -d "$index" ]]; then
        /usr/bin/time -v -o "$report-$size.time.txt" "$engine" index \
            --corpus "$corpus" --output "$index" --max-docs "$size" \
            --threads 8 --memory-limit-mb 512 --merge-fan-in 8 \
            > "$report-$size.build.json"
    fi
    "$engine" validate --index "$index" > "$report-$size.validate.txt"
    "$engine" stats --index "$index" > "$report-$size.stats.json"
    for advance in binary sequential; do
        "$engine" benchmark --index "$index" --queries benchmarks/queries.txt \
            --repeats 100 --top-k 10 --advance "$advance" > "$report-$size-$advance.json"
    done
done
"$engine" benchmark-intersection > "$report-intersections.json"
