#!/usr/bin/env bash
# ui.sh: drive diefastdiehard instances on a headless Xvfb display.
#
#   ui.sh up                         start Xvfb + build the xinput helper
#   ui.sh launch NAME [dfdh args]    start dfdh, wait for its window, place it
#                                    next to the others; log: $UI_OUT/NAME.log
#   ui.sh wait NAME REGEX [SEC]      wait for REGEX in NAME's log (default 20 s)
#   ui.sh shot NAME [FILE]           window screenshot (default $UI_OUT/NAME.png)
#   ui.sh ocr NAME                   shot + OCR: "cx cy w h text" per line,
#                                    cx/cy = window-relative click point
#   ui.sh click NAME X Y [BTN]       click at window-relative X Y
#   ui.sh click NAME "TEXT" [NTH]    OCR, click the NTH (1) match of TEXT
#   ui.sh move NAME X Y              mouse motion only (hover)
#   ui.sh key NAME KEYSYM...         e.g. grave Return Escape Up a
#   ui.sh hold NAME SEC KEYSYM...    hold keys down for SEC (gameplay controls)
#   ui.sh type NAME "TEXT"           type text (focused NK edit field)
#   ui.sh console NAME "CMD"         dev console: open, type CMD, Return, close
#   ui.sh lua NAME ['CODE']          run lua (CODE or stdin) via the fifo, print
#                                    the log lines it produced (print() logs)
#   ui.sh win NAME                   print NAME's window id
#   ui.sh down                       kill the launched instances + Xvfb
#
# Env: UI_OUT (state/logs/screenshots, default ${TMPDIR:-/tmp}/dfdh-ui),
#      UI_DISPLAY (:99), UI_SCREEN (3840x1200x24),
#      DFDH_BIN (easybuild-debug/dfdh, relative to the repo root).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${UI_OUT:-${TMPDIR:-/tmp}/dfdh-ui}
export DISPLAY=${UI_DISPLAY:-:99}
xi=$out/xinput
mkdir -p "$out"

die() { echo "ui.sh: $*" >&2; exit 1; }
win() { [[ -s $out/$1.win ]] || die "no instance '$1' (launch it first)"; cat "$out/$1.win"; }

cmd=${1:-}; shift || true
case $cmd in
up)
    if [[ ! -x $xi || $here/xinput.c -nt $xi ]]; then
        gcc -O1 -o "$xi" "$here/xinput.c" -lX11 || die "helper build failed (needs libX11 headers)"
    fi
    if ! "$xi" wins >/dev/null 2>&1; then
        Xvfb "$DISPLAY" -screen 0 "${UI_SCREEN:-3840x1200x24}" -nolisten tcp >"$out/xvfb.log" 2>&1 &
        for _ in $(seq 50); do "$xi" wins >/dev/null 2>&1 && break; sleep 0.1; done
        "$xi" wins >/dev/null 2>&1 || die "Xvfb did not start, see $out/xvfb.log"
    fi
    echo "display $DISPLAY up, state in $out"
    ;;
launch)
    name=${1:?NAME}; shift
    bin=${DFDH_BIN:-easybuild-debug/dfdh}
    [[ $bin = /* ]] || bin=$root/$bin
    [[ -x $bin ]] || die "$bin not built"
    bdir=$(dirname "$bin")
    # assets resolve from cwd: dist dirs carry their own data/ + fs.cfg
    cwd=$root; [[ -f $bdir/fs.cfg ]] && cwd=$bdir
    before=$("$xi" wins | cut -d' ' -f1)
    (cd "$cwd" && LD_LIBRARY_PATH=$bdir/3rd/lib:$bdir/3rd/lib64:$bdir/submodules/luacpp/3rd/lib:$bdir/submodules/luacpp/3rd/lib64 \
        exec "$bin" --lua-exec-fifo="$out/$name.fifo" "$@" </dev/null >"$out/$name.log" 2>&1) &
    echo $! >"$out/$name.pid"
    id=
    for _ in $(seq 200); do
        id=$("$xi" wins | cut -d' ' -f1 | grep -vxF -e "${before:-none}" | head -n1 || true)
        [[ -n $id ]] && break
        kill -0 "$(cat "$out/$name.pid")" 2>/dev/null || { tail "$out/$name.log"; die "$name exited"; }
        sleep 0.1
    done
    [[ -n $id ]] || die "no window for $name"
    echo "$id" >"$out/$name.win"
    # side by side at y=0: x = right edge of the rightmost other window
    x=$("$xi" wins | awk -v id="$id" '$1 != id { split($2, s, "x"); r = $3 + s[1]; if (r > m) m = r } END { print m + 0 }')
    "$xi" place "$id" "$x" 0
    echo "$name: window $id at ($x,0), pid $(cat "$out/$name.pid"), log $out/$name.log"
    ;;
wait)
    name=${1:?NAME}; re=${2:?REGEX}; t=${3:-20}
    for _ in $(seq $((t * 10))); do
        grep -qE -- "$re" "$out/$name.log" && { grep -E -- "$re" "$out/$name.log" | tail -n1; exit 0; }
        sleep 0.1
    done
    tail -n5 "$out/$name.log"; die "timeout waiting for /$re/ in $name.log"
    ;;
shot)
    name=${1:?NAME}; f=${2:-$out/$name.png}
    import -window "$(win "$name")" "$f"
    echo "$f"
    ;;
ocr | click)
    name=${1:?NAME}
    if [[ $cmd = click && ${2:-} =~ ^[0-9]+$ ]]; then
        "$xi" click "$(win "$name")" "$2" "${3:?Y}" "${4:-1}"
        exit
    fi
    import -window "$(win "$name")" "$out/$name.png"
    # 2x upscale + light-on-dark inversion: tesseract misses the small NK font otherwise
    convert "$out/$name.png" -resize 200% -colorspace Gray -negate -threshold 60% "$out/$name.ocr.png"
    tesseract "$out/$name.ocr.png" - --psm 11 tsv 2>/dev/null | awk -F'\t' '
        NR > 1 && $12 ~ /[^ ]/ && $11 > 30 {
            printf "%d %d %d %d %s\n", ($7 + $9 / 2) / 2, ($8 + $10 / 2) / 2, $9 / 2, $10 / 2, $12
        }' >"$out/$name.ocr"
    if [[ $cmd = ocr ]]; then cat "$out/$name.ocr"; exit; fi
    text=${2:?TEXT}; nth=${3:-1}
    # match TEXT as a run of consecutive words on one line (left to right),
    # ignoring punctuation: OCR reads a checkbox box as a quote ("“ready")
    hit=$(sort -k2,2n -k1,1n "$out/$name.ocr" | LC_ALL=C awk -v q="$text" -v nth="$nth" '
        function norm(s) { s = tolower(s); gsub(/[^a-z0-9_]/, "", s); return s }
        BEGIN { nq = split(q, qw, " "); for (k in qw) qw[k] = norm(qw[k]) }
        { n++; X[n] = $1; Y[n] = $2; W[n] = $3; T[n] = norm($5) }
        END {
            for (i = 1; i + nq - 1 <= n; i++) {
                ok = 1
                for (k = 0; k < nq && ok; k++) {
                    j = i + k
                    if (T[j] != qw[k + 1] || (k && (Y[j] - Y[i] > 6 || Y[i] - Y[j] > 6))) ok = 0
                }
                if (ok && ++c == nth) {
                    l = X[i] - W[i] / 2; r = X[i + nq - 1] + W[i + nq - 1] / 2
                    print int((l + r) / 2), Y[i]; exit
                }
            }
        }')
    [[ -n $hit ]] || { cat "$out/$name.ocr" >&2; die "text '$text' not found on $name (OCR above)"; }
    read -r x y <<<"$hit"
    "$xi" click "$(win "$name")" "$x" "$y"
    echo "clicked '$text' on $name at ($x,$y)"
    ;;
move) "$xi" move "$(win "${1:?NAME}")" "${2:?X}" "${3:?Y}" ;;
key) name=${1:?NAME}; shift; "$xi" key "$(win "$name")" "$@" ;;
hold)
    w=$(win "${1:?NAME}"); t=${2:?SEC}; shift 2
    "$xi" keydown "$w" "$@"; sleep "$t"; "$xi" keyup "$w" "$@"
    ;;
type) "$xi" type "$(win "${1:?NAME}")" "${2:?TEXT}" ;;
console)
    w=$(win "${1:?NAME}")
    "$xi" key "$w" grave
    sleep 0.1
    "$xi" type "$w" "${2:?CMD}"
    "$xi" key "$w" Return
    sleep 0.1
    "$xi" key "$w" grave
    ;;
lua)
    name=${1:?NAME}; fifo=$out/$name.fifo; log=$out/$name.log
    [[ -p $fifo ]] || die "no lua fifo for '$name' (launch it first)"
    code=${2:-$(cat)}
    off=$(wc -c <"$log")
    mark="ui.sh lua done $$.$RANDOM"
    # the marker runs after CODE in the same chunk: no top-level `return` in CODE
    printf '%s\nprint("%s")\n' "$code" "$mark" | timeout 5 tee "$fifo" >/dev/null ||
        die "cannot write $fifo ($name not running?)"
    for _ in $(seq 100); do
        tail -c +$((off + 1)) "$log" | grep -qaF -e "$mark" -e "lua error" && break
        sleep 0.05
    done
    tail -c +$((off + 1)) "$log" | grep -avF "$mark" | grep -a "^\[" || true
    tail -c +$((off + 1)) "$log" | grep -qaF "$mark" || die "lua chunk failed or timed out"
    ;;
win) win "${1:?NAME}" ;;
down)
    pids=()
    for p in "$out"/*.pid; do
        [[ -s $p ]] || continue
        pids+=("$(cat "$p")")
        : >"$p"; : >"${p%.pid}.win"
    done
    pids+=($(pgrep -f "^Xvfb $DISPLAY " || true))
    ((${#pids[@]})) && kill "${pids[@]}" 2>/dev/null || true
    # wait for the exit: a following `up` would race the dying Xvfb
    for p in "${pids[@]}"; do
        for _ in $(seq 50); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    done
    echo "down"
    ;;
*) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; [[ -z $cmd ]] ;;
esac
