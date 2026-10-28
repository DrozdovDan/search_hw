#!/usr/bin/env bash
set -euo pipefail
engine=${ENGINE:-./build/search_engine}
corpus=${1:-data/msmarco_v2_doc.tar}
prefix=${2:-data/msmarco}
report=${3:-reports/msmarco}
memory_limit=${MEMORY_LIMIT_MB:-512}
mkdir -p "$(dirname "$report")"
printf 'eea90100409a254fdb157b8e4e349deb  %s\n' "$corpus" | md5sum -c > "$report-md5.txt"
for preset in 10k 100k 1m full; do
    index="$prefix-$preset"
    if [[ ! -d "$index" ]]; then
        /usr/bin/time -v -o "$report-$preset.time.txt" "$engine" index \
            --corpus "$corpus" --output "$index" --preset "$preset" \
            --threads 8 --memory-limit-mb "$memory_limit" --merge-fan-in 8 \
            > "$report-$preset.build.json"
    fi
    "$engine" validate --index "$index" > "$report-$preset.validate.txt"
    "$engine" stats --index "$index" > "$report-$preset.stats.json"
    for advance in binary sequential; do
        "$engine" benchmark --index "$index" --queries benchmarks/msmarco_queries.txt \
            --repeats 100 --top-k 10 --advance "$advance" > "$report-$preset-$advance.json"
    done
done
