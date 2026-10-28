#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

formatter="${CLANG_FORMAT:-clang-format-18}"
if ! command -v "$formatter" > /dev/null 2>&1; then
  printf 'Не найден %s. Установите clang-format-18 или задайте CLANG_FORMAT.\n' "$formatter" >&2
  exit 1
fi

# Внешние библиотеки и файлы сборки сохраняют собственное оформление.
mapfile -d '' -t sources < <(rg --files -0 include src tests -g '*.cpp' -g '*.hpp' | sort -z)
case "${1:---check}" in
  --check) "$formatter" --dry-run --Werror --style=file "${sources[@]}" ;;
  --write) "$formatter" -i --style=file "${sources[@]}" ;;
  *) printf 'Использование: %s [--check|--write]\n' "$0" >&2; exit 1 ;;
esac
