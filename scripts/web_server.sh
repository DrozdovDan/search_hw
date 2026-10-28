#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
state_dir="${SEARCH_WEB_STATE_DIR:-$project_dir/data}"
mkdir -p "$state_dir"
pid_file="$state_dir/web-server.pid"
log_file="$state_dir/web-server.log"
expected_engine="$(realpath "${SEARCH_ENGINE:-$project_dir/build/search_engine}")"
port="${SEARCH_PORT:-8080}"
[[ "$port" =~ ^[0-9]{1,5}$ ]] && ((10#$port > 0 && 10#$port <= 65535)) || {
  printf 'SEARCH_PORT должен быть числом от 1 до 65535\n' >&2; exit 1;
}
# Одновременные start/stop не должны перезаписывать PID друг другу.
exec 9> "$state_dir/web-server.lock"
flock -w 15 9 || { printf 'Другая команда управления сервером ещё работает\n' >&2; exit 1; }
is_our_server() {
  local candidate="$1" executable
  local -a arguments
  [[ "$candidate" =~ ^[0-9]+$ ]] || return 1
  kill -0 "$candidate" 2>/dev/null || return 1
  executable="$(readlink "/proc/$candidate/exe" 2>/dev/null)" || return 1
  [[ "${executable% (deleted)}" == "$expected_engine" ]] || return 1
  # Читаем argv напрямую, без конвейера с rg -q и возможного SIGPIPE.
  mapfile -d '' -t arguments < "/proc/$candidate/cmdline" || return 1
  [[ "${arguments[1]:-}" == serve ]]
}
listeners() { ss -H -ltnp "( sport = :$port )" 2>/dev/null || true; }
remember_pid() {
  printf '%s\n' "$server_pid" > "$pid_file.tmp.$$"
  mv -f -- "$pid_file.tmp.$$" "$pid_file"
}
is_running() {
  if [[ -f "$pid_file" ]] && read -r server_pid < "$pid_file" && is_our_server "$server_pid"; then return 0; fi
  # После неудачного ручного запуска PID-файл может отсутствовать или указывать
  # на завершившийся процесс. Восстанавливаем его по нашему слушающему сокету.
  local sockets candidate
  sockets="$(listeners)"
  while [[ "$sockets" =~ pid=([0-9]+) ]]; do
    candidate="${BASH_REMATCH[1]}"
    sockets="${sockets#*pid=$candidate}"
    if is_our_server "$candidate"; then server_pid="$candidate"; remember_pid; return 0; fi
  done
  return 1
}
command="${1:-status}"
if [[ $# -gt 0 ]]; then shift; fi
case "$command" in
  start)
    if is_running; then printf 'Web server is running, PID %s\n' "$server_pid"; exit 0; fi
    if [[ -n "$(listeners)" ]]; then
      printf 'Порт %s уже занят другим процессом. Выберите другой: SEARCH_PORT=8090 %s start\n' "$port" "$0" >&2
      exit 1
    fi
    # Новая сессия отделяет процесс от SSH/терминала; stdin/stdout не удерживают
    # соединение. PID записывается уже из отделённого процесса перед exec.
    starting_file="$(mktemp "$state_dir/web-start.XXXXXX")"
    trap 'rm -f -- "$starting_file"' EXIT
    # PID становится официальным только ПОСЛЕ успешного bind и проверки HTTP.
    # Дескриптор flock закрываем у сервера, чтобы он не удерживал замок постоянно.
    setsid --fork sh -c 'printf "%s\n" "$$" > "$1"; shift; exec "$@"' sh \
      "$starting_file" "$project_dir/scripts/run_web.sh" "$@" > "$log_file" 2>&1 < /dev/null 9>&-
    probe_host="${SEARCH_HOST:-127.0.0.1}"
    [[ "$probe_host" != 0.0.0.0 ]] || probe_host=127.0.0.1
    for attempt in {1..30}; do
      if read -r server_pid < "$starting_file" && is_our_server "$server_pid" && \
          [[ "$(listeners)" == *"pid=$server_pid,"* ]] && \
          curl --noproxy '*' -fsS --max-time 1 "http://$probe_host:$port/healthz" > /dev/null 2>&1; then
        remember_pid
        printf 'Web server started, PID %s; log: %s\n' "$server_pid" "$log_file"; exit 0
      fi
      sleep .1
    done
    cat "$log_file"; exit 1
    ;;
  stop)
    if ! is_running; then printf 'Web server is not running\n'; exit 0; fi
    kill "$server_pid"
    for attempt in {1..100}; do
      if ! is_running; then rm -f -- "$pid_file"; printf 'Web server stopped\n'; exit 0; fi
      sleep .1
    done
    printf 'Server is finishing active requests; PID %s\n' "$server_pid"
    ;;
  status)
    if is_running; then printf 'Web server is running, PID %s\n' "$server_pid"; else printf 'Web server is not running\n'; exit 1; fi
    ;;
  *) printf 'Usage: %s start|stop|status\n' "$0"; exit 1 ;;
esac
