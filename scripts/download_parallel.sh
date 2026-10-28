#!/usr/bin/env bash
set -euo pipefail
# Запасной транспорт для серверов, игнорирующих x-ms-range AzCopy. Обычный
# download_corpus.sh использует wget -c. Здесь каждый HTTP Range продолжается
# вручную с сохранённой длины части; HTTP-заголовки проверяются до дописывания.
total=34648862720
chunk=33554432
url=https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco_v2_doc.tar
expected=eea90100409a254fdb157b8e4e349deb
if [[ ${1:-} == --part ]]; then
    part_number=$2
    parts=$3
    begin=$((part_number * chunk))
    end=$((begin + chunk - 1))
    (( end < total )) || end=$((total - 1))
    printf -v name '%s/%05d' "$parts" "$part_number"
    network=()
    [[ ${DOWNLOAD_DIRECT:-0} != 1 ]] || network=(--noproxy '*')
    # После прерывания всего дерева процессов curl мог оставить полученный хвост.
    # Принимаем его только если сервер подтвердил точно нужный диапазон.
    if [[ -f "$name.tail" && -f "$name.headers" ]]; then
        have=0
        [[ ! -f "$name" ]] || have=$(stat -c %s "$name")
        first=$((begin + have))
        if tr -d '\r' < "$name.headers" | rg -qi "^Content-Range: bytes $first-$end/$total$"; then
            cat "$name.tail" >> "$name"
        fi
        rm -f "$name.tail" "$name.headers"
    fi
    for attempt in {1..8}; do
        have=0
        [[ ! -f "$name" ]] || have=$(stat -c %s "$name")
        (( have != end - begin + 1 )) || exit 0
        (( have < end - begin + 1 )) || { echo "Повреждена длина части $part_number" >&2; exit 1; }
        first=$((begin + have))
        status=0
        curl "${network[@]}" -fsS --connect-timeout 15 --max-time 180 --max-filesize "$((end - first + 1))" \
            --header 'X-Ms-Version: 2019-12-12' --range "$first-$end" \
            -D "$name.headers" -o "$name.tail" "$url" || status=$?
        if [[ -f "$name.tail" ]] && tr -d '\r' < "$name.headers" | \
            rg -qi "^Content-Range: bytes $first-$end/$total$"; then
            cat "$name.tail" >> "$name"
        else
            echo "Неподходящий HTTP Range для части $part_number, попытка $attempt, код $status" >&2
        fi
        rm -f "$name.tail" "$name.headers"
    done
    [[ -f "$name" && $(stat -c %s "$name") -eq $((end - begin + 1)) ]] || exit 1
    exit 0
fi
destination=${1:-data/msmarco_v2_doc.tar}
parallel=${2:-32}
mkdir -p "$(dirname "$destination")"
if [[ -f "$destination" && $(stat -c %s "$destination") -eq $total ]]; then
    printf '%s  %s\n' "$expected" "$destination" | md5sum -c
    exit
fi
available=$(df -B1 --output=avail "$(dirname "$destination")" | tail -1)
(( available > total * 2 + 1073741824 )) || { echo 'Нужно место для частей и сборки архива.' >&2; exit 1; }
parts="$destination.parts"
mkdir -p "$parts"
# Уже скачанный wget префикс повторно не передаём по сети. split сохраняет
# последнюю неполную часть, которую рабочий процесс затем продолжит через Range.
if [[ -f "$destination" && ! -f "$parts/.imported" ]] && ! compgen -G "$parts/00000" > /dev/null; then
    split -b "$chunk" --numeric-suffixes=0 --suffix-length=5 "$destination" "$parts/"
    touch "$parts/.imported"
fi
count=$(((total + chunk - 1) / chunk))
seq 0 "$((count - 1))" | xargs -P "$parallel" -I '{}' bash "$0" --part '{}' "$parts"
cat "$parts"/[0-9][0-9][0-9][0-9][0-9] > "$destination.assembled"
printf '%s  %s\n' "$expected" "$destination.assembled" | md5sum -c
mv "$destination.assembled" "$destination"
echo 'Все части получены, официальный MD5 совпадает.'
