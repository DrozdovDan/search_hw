#!/usr/bin/env bash
set -euo pipefail
# Запуск из корня проекта сохраняет разрешение путей в старых манифестах.
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
exec "${SEARCH_ENGINE:-./build/search_engine}" serve \
  --index "${SEARCH_INDEX:-data/index-full}" \
  --host "${SEARCH_HOST:-0.0.0.0}" \
  --port "${SEARCH_PORT:-8080}" \
  "$@"
