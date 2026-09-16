#!/usr/bin/env bash
#
# Callgrind driver for this project.
#
# Callgrind counts the instructions actually executed and attributes them to
# the source line (needs -g, already in CFLAGS). It does not sample: two runs
# of the same binary yield the exact same number. That makes it the right tool
# to answer "did this change reduce the work?", a question a wall clock cannot
# answer on this machine (measured spread on a graph BFS in TurboGraph core:
# +-3 ms out of ~22, while the instruction count is exact).

# Coroutines: callgrind follows the context switches (every stack is
# registered with valgrind by co_pool_create). --func co_resume collects
# everything that runs inside a coroutine, since all of it is reached through
# that call; the switch routines are assembly and can never be inlined away,
# unlike small static C functions at -O2.
#
# The flip side: Ir is not time. It ignores memory latency and mispredicted
# branches. Use --cache and --branch for those; they simulate the cache
# hierarchy and the branch predictor.
#
# Usage:
#   tools/profile.sh [options] [-- program-arguments]
#
#   -b, --bin PATH     binary to profile               (default: bin/main)
#   -f, --func NAME    collect only inside NAME and what it calls
#   -c, --cache        simulate L1/LL (cache misses)
#   -B, --branch       simulate the branch predictor
#   -l, --lines        also annotate sources line by line
#   -C, --callers N    split each function by its call chain, N levels deep:
#                      tells apart the same helper called from different places
#   -n, --top N        how many functions to list       (default: 15)
#   -s, --save NAME    store this profile as baseline NAME
#   -d, --diff NAME    compare this run against baseline NAME
#   -h, --help         this message
#
# Examples:
#   tools/profile.sh                                  # whole program
#   tools/profile.sh -f co_resume -c -B               # coroutine code only, cache and branches
#   tools/profile.sh -C 2                             # who is calling the hot helpers
#   tools/profile.sh -f co_resume -s before           # store the baseline
#   ...edit the code, rebuild...
#   tools/profile.sh -f co_resume -d before           # what changed, per function
#
set -euo pipefail

# Always run from the project root: the out/ path is relative to it.
cd "$(dirname "$0")/.."

BIN=bin/main
FUNC=""
TOP=15
SAVE=""
DIFF=""
LINES=0
CALLERS=""
SIM=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--bin)    BIN="$2"; shift 2 ;;
        -f|--func)   FUNC="$2"; shift 2 ;;
        -n|--top)    TOP="$2"; shift 2 ;;
        -s|--save)   SAVE="$2"; shift 2 ;;
        -d|--diff)   DIFF="$2"; shift 2 ;;
        -c|--cache)  SIM+=(--cache-sim=yes); shift ;;
        -B|--branch) SIM+=(--branch-sim=yes); shift ;;
        -l|--lines)  LINES=1; shift ;;
        -C|--callers) CALLERS="$2"; shift 2 ;;
        -h|--help)   sed -n '2,/^set -euo/p' "$0" | sed 's/^# \?//;$d'; exit 0 ;;
        --)          shift; break ;;
        *)           echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

[[ -x "$BIN" ]] || { echo "binary not found or not executable: $BIN" >&2; exit 1; }
command -v valgrind >/dev/null || { echo "valgrind is not installed" >&2; exit 1; }

DIR=out/profile
mkdir -p "$DIR"
OUT="$DIR/${SAVE:-current}.out"
LOG="$DIR/${SAVE:-current}.log"

# --separate-callers attributes a function's cost to the chain that reached it,
# so one helper shows up once per caller. No code change: callgrind reconstructs
# the chain from the stack it already tracks.
SEP=()
[[ -n "$CALLERS" ]] && SEP=(--separate-callers="$CALLERS")

TOGGLE=()
if [[ -n "$FUNC" ]]; then
    TOGGLE=(--toggle-collect="$FUNC")
    echo ">> collecting inside $FUNC only"
else
    echo ">> collecting the whole program"
fi

echo ">> valgrind --tool=callgrind ${SIM[*]-} -> $OUT"
valgrind --tool=callgrind \
         --callgrind-out-file="$OUT" \
         "${TOGGLE[@]}" "${SEP[@]+"${SEP[@]}"}" "${SIM[@]+"${SIM[@]}"}" \
         "$BIN" "$@" >/dev/null 2>"$LOG"

# "Collected: 0" means the run produced no events at all: a silent failure
# worth catching, since callgrind_annotate would happily print an empty table.
if ! grep -qE '^==[0-9]+== *Collected *: *[1-9]' "$LOG"; then
    echo "!! no events collected." >&2
    if [[ -n "$FUNC" ]]; then
        echo "!! was '$FUNC' actually called? (exact name; not inlined away by -O2?)" >&2
    fi
    echo "!! did the program start from the project root?" >&2
    exit 1
fi

echo
grep -E '^==[0-9]+== +(I +refs|D +refs|D1 +miss rate|LL misses|LL miss rate|Branches|Mispredicts|Mispred rate)' "$LOG" \
    | sed 's/^==[0-9]*== */   /'
# Only a few event columns, otherwise the per-function table is 14 columns
# wide and unreadable: Ir (instructions), DLmr (last-level read misses, i.e.
# trips to RAM) and Bcm (mispredicted conditional branches) are the three that
# explain where the cycles go. The totals for everything else are printed above.
SHOW=Ir
[[ " ${SIM[*]-} " == *cache-sim* ]]  && SHOW="$SHOW,DLmr,DLmw"
[[ " ${SIM[*]-} " == *branch-sim* ]] && SHOW="$SHOW,Bcm"

echo
echo "-- hottest functions ($SHOW) --------------------------------------------"
callgrind_annotate --threshold=99 --show="$SHOW" "$OUT" 2>/dev/null \
    | sed -n '/file:function/,/^$/p' | sed 's| \[[^]]*\]$||' | head -n "$((TOP + 2))"

if [[ "$LINES" == 1 ]]; then
    echo
    echo "-- sources annotated line by line --------------------------------------"
    callgrind_annotate --auto=yes "$OUT" 2>/dev/null | sed -n '/Auto-annotated source/,$p'
fi

if [[ -n "$DIFF" ]]; then
    BASE="$DIR/$DIFF.out"
    [[ -f "$BASE" ]] || { echo "no such baseline: $BASE (create it with --save $DIFF)" >&2; exit 1; }
    [[ -x bin/cgdiff ]] || make --no-print-directory bin/cgdiff
    echo
    bin/cgdiff "$BASE" "$OUT" "$DIFF" "${SAVE:-current}" "$TOP"
fi

[[ -n "$SAVE" ]] && echo && echo ">> stored as baseline '$SAVE' ($OUT)"
exit 0
