#!/usr/bin/env bash
# ofgl.sh start [rate] [options] | plan [rate] [options] | stop | status: one monitor's pictures doubled by this CPU, as a detached program. stop ends the program; the overlay goes with it and the monitor is plain again.
# The rate, the first word after start:
#   24       24 in, 48 out
#   30       30 in, 60 out
#   60       60 in, 120 out. On a 120 Hz monitor (119.4 to 120.6 Hz, so 119.88 counts: 59.94 in) this is native, one picture a refresh. On a faster monitor it is --rate 60, each picture held whole refreshes.
#            A slower monitor cannot show 120 pictures a second: refused, with what native is there.
#   native   half the monitor's refresh in, the refresh out: the default
# plan prints the command line start would run with the same words, and starts nothing.
#   OFGL_OUTPUT   the monitor's name (local.conf; the environment, when set, wins)
#   OFGL_ARGS     this machine's own words, used when start or plan is given none (local.conf)
# Started from a shell with no session (SSH), it finds the user's own: the runtime directory, the first Wayland socket in it, the session bus. The right to cast is scripts/authorize.sh's, once.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
# The environment wins over local.conf: what is given on the command line for one run is more particular than the machine's file.
env_output=${OFGL_OUTPUT:-} env_args=${OFGL_ARGS:-}
if [[ -f local.conf ]]; then . ./local.conf; fi
OFGL_OUTPUT=${env_output:-${OFGL_OUTPUT:-}} OFGL_ARGS=${env_args:-${OFGL_ARGS:-}}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [[ -z ${WAYLAND_DISPLAY:-} ]]; then
    for s in "$XDG_RUNTIME_DIR"/wayland-[0-9]*; do
        [[ -S $s ]] && { export WAYLAND_DISPLAY=${s##*/}; break; }
    done
fi
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
output=${OFGL_OUTPUT:-}
usage="usage: ofgl.sh start [24 | 30 | 60 | native] [options] | plan [24 | 30 | 60 | native] [options] | stop | status"
hz() { printf '%d.%03d' $(($1 / 1000)) $(($1 % 1000)); }
# The words after start or plan, made into the program's command line, in cmd.
resolve() {
    [[ -n $output ]] || { echo "ofgl.sh: set OFGL_OUTPUT to the monitor's name, in the environment or local.conf" >&2; exit 2; }
    if (($# == 0)) && [[ -n ${OFGL_ARGS:-} ]]; then
        read -r -a own <<<"$OFGL_ARGS"
        set -- "${own[@]}"
    fi
    local rate=() mhz=
    case "${1:-}" in
        24 | 30)
            rate=(--rate "$1")
            shift
            ;;
        60)
            shift
            read -r _ _ mhz _ < <(out/ofgl --output "$output" --plan 2>/dev/null) || true
            [[ $mhz =~ ^[0-9]+$ ]] || { echo "ofgl.sh: no mode for $output" >&2; exit 2; }
            if ((mhz < 119400)); then
                echo "ofgl.sh: 60 in is 120 out, more than $output's $(hz "$mhz") Hz shows; native there is $(hz $((mhz / 2))) in, $(hz "$mhz") out" >&2
                exit 2
            fi
            if ((mhz > 120600)); then rate=(--rate 60); fi
            ;;
        native)
            shift
            ;;
    esac
    cmd=(out/ofgl --output "$output" "${rate[@]}" "$@")
}
case "${1:-}" in
    start)
        shift
        if pgrep -x ofgl >/dev/null; then
            echo "ofgl is already running"
            exit 0
        fi
        resolve "$@"
        mkdir -p logs/live out/run
        log="logs/live/$(date +%Y%m%d-%H%M%S).log"
        setsid nohup "${cmd[@]}" >"$log" 2>&1 </dev/null &
        echo "$log" >out/run/ofgl.log
        sleep 3
        if ! pgrep -x ofgl >/dev/null; then
            echo "ofgl.sh: it did not stay up:" >&2
            cat "$log" >&2
            exit 1
        fi
        echo "ofgl running on $output, log $log"
        ;;
    plan)
        shift
        resolve "$@"
        echo "${cmd[*]}"
        ;;
    stop)
        if pgrep -x ofgl >/dev/null; then
            pkill -x ofgl
            for _ in {1..40}; do
                pgrep -x ofgl >/dev/null || break
                sleep 0.25
            done
        fi
        scripts/excl.sh off || true
        ;;
    status)
        if pgrep -x ofgl >/dev/null; then
            echo "ofgl: running, pid $(pgrep -x ofgl)"
        else
            echo "ofgl: not running"
        fi
        [[ -f out/run/ofgl.log ]] && tail -n 1 "$(cat out/run/ofgl.log)"
        ;;
    *)
        echo "$usage" >&2
        exit 2
        ;;
esac
