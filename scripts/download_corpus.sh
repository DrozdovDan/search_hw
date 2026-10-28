#!/usr/bin/env bash
set -euo pipefail
destination=${1:-data/msmarco_v2_doc.tar}
expected=eea90100409a254fdb157b8e4e349deb
expected_size=34648862720
mkdir -p "$(dirname "$destination")"
verify() { printf '%s  %s\n' "$expected" "$destination" | md5sum --check --status; }
if [[ -f "$destination" ]] && [[ $(stat -c %s "$destination") -eq $expected_size ]]; then
    if verify; then
        echo 'Архив уже скачан, MD5 совпадает.'
        exit 0
    fi
    echo 'Полный файл имеет неверный MD5. Сохраните его отдельно перед повторной загрузкой.' >&2
    exit 1
fi
present=0
[[ ! -f "$destination" ]] || present=$(stat -c %s "$destination")
available=$(df -B1 --output=avail "$(dirname "$destination")" | tail -1)
if (( available < expected_size - present + 1073741824 )); then
    echo 'Недостаточно места для загрузки и резервного гигабайта.' >&2
    exit 1
fi
wget -c --header='X-Ms-Version: 2019-12-12' --timeout=60 --tries=3 \
    --progress=dot:giga -O "$destination" \
    'https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco_v2_doc.tar'
verify || { echo 'MD5 архива не совпадает; корпус не подтверждён.' >&2; exit 1; }
echo 'Корпус загружен, официальный MD5 совпадает.'
