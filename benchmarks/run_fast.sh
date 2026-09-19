#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# KPP fast benchmark harness (file-backed fio on the NVMe test filesystem).
#
# Matrix (FULL, 6 schedulers): 27 cells x 2 repeats = 54 runs.
#   QD8 base: none/mq-deadline/kyber/bfq/kpp x 4 workloads (randread/randwrite/
#     randrw/seqread at QD8) = 20 cells; adios x 3 workloads at QD8
#     (randread/randwrite/randrw; seqread omitted for the time budget) = 3 cells.
#   QD sweep: kpp + kyber randread at QD1 and QD32 (QD8 already in base) = 4 cells.
#   Total: 20 + 3 + 4 = 27 cells x 2 = 54 runs.
# Fallback (adios unavailable): QD8-only, 5 schedulers x 4 workloads = 20 cells
#   x 2 = 40 runs (sweep tier skipped to hold the ~35 min budget).
#
# Tiers for the <5 min per-scheduler note:
#   FULL tier = randread QD8 + randrw QD8 for every scheduler
#     (12 cells = 24 runs, 4 runs per scheduler).
#   SUB tier = everything else (15 cells = 30 runs, 5 runs per scheduler avg).
#
# Timing per run: runtime 25 s + ramp 3 s + cooldown 10 s + ~5 s fio/switch
#   overhead ~= 43 s. Per-scheduler avg 9 x 43 s ~= 387 s (~6.5 min, slightly
#   over the 5 min guideline); subset tiers stay ~4-5 min (see README).
#   Total ~= 54 x 43 s ~= 38.7 min + overhead ~= 45 min;
#   fallback ~= 40 x 43 s ~= 28.7 min + overhead ~= 35 min.
#
# Safety: target must resolve under /mnt/data/ and must sit on a filesystem
#   whose mount source names nvme0n1. Only regular files under the target dir
#   are used (size=4G test file); no raw-device I/O is performed here.
#
# Usage:
#   sudo ./run_fast.sh --target-dir /mnt/data/kpp-fast [--dry-run]
#   BENCH_ABORT=1 sudo ./run_fast.sh --target-dir /mnt/data/kpp-fast
#
# Emergency brake: set BENCH_ABORT=1 (or IOSCHED_ABORT=1) to abort cleanly
#   before the next run starts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS_DIR="$SCRIPT_DIR/jobs"
RESULTS_DIR="$SCRIPT_DIR/results"

RUNTIME=25
RAMP=3
COOLDOWN=10
REPEATS=2
FIO="${FIO:-fio}"

TARGET_DIR=""
DRY_RUN=0

SCHEDS_FULL="none mq-deadline kyber bfq kpp adios"

usage() {
    cat <<'EOF'
Usage: run_fast.sh --target-dir /mnt/data/<name> [--dry-run]

Options:
  --target-dir DIR   Required. Test directory; must resolve under /mnt/data/
                     and sit on the nvme0n1-backed filesystem. Created at
                     runtime (not during --dry-run).
  --dry-run          Print the planned matrix and run counts (54 full /
                     40 without adios) without touching scheduler, fio,
                     or the target directory.
  -h, --help         Show this help.

Environment:
  BENCH_ABORT=1 (or IOSCHED_ABORT=1)  Emergency brake: abort before next run.
  FIO=fio                             fio binary override.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --target-dir)
            TARGET_DIR="${2:-}"
            shift 2
            ;;
        --target-dir=*)
            TARGET_DIR="${1#--target-dir=}"
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$TARGET_DIR" ]; then
    echo "ERROR: --target-dir is required (e.g. --target-dir /mnt/data/kpp-fast)." >&2
    exit 2
fi

# Static matrix: one line per cell as "sched|workload|qd|jobfile".
# QD8 base (23 cells) + kpp/kyber randread QD1/QD32 sweep (4 cells) = 27 cells.
plan_full_matrix() {
    local s
    for s in none mq-deadline kyber bfq kpp; do
        echo "$s|randread|8|randread-4k.fio"
        echo "$s|randwrite|8|randwrite-4k.fio"
        echo "$s|randrw|8|randrw-70-30-4k.fio"
        echo "$s|seqread|8|seqread-128k.fio"
    done
    echo "adios|randread|8|randread-4k.fio"
    echo "adios|randwrite|8|randwrite-4k.fio"
    echo "adios|randrw|8|randrw-70-30-4k.fio"
    echo "kpp|randread|1|randread-4k.fio"
    echo "kpp|randread|32|randread-4k.fio"
    echo "kyber|randread|1|randread-4k.fio"
    echo "kyber|randread|32|randread-4k.fio"
}

# Fallback matrix: QD8-only without adios and without the sweep tier.
# 5 schedulers x 4 workloads = 20 cells.
plan_fallback_matrix() {
    local s
    for s in none mq-deadline kyber bfq kpp; do
        echo "$s|randread|8|randread-4k.fio"
        echo "$s|randwrite|8|randwrite-4k.fio"
        echo "$s|randrw|8|randrw-70-30-4k.fio"
        echo "$s|seqread|8|seqread-128k.fio"
    done
}

# FULL tier cells (priority head-to-head): randread QD8 + randrw QD8, all scheds.
plan_full_tier() {
    local s
    for s in $SCHEDS_FULL; do
        echo "$s|randread|8|randread-4k.fio"
        echo "$s|randrw|8|randrw-70-30-4k.fio"
    done
}

brake_requested() {
    [ "${BENCH_ABORT:-0}" = "1" ] || [ "${IOSCHED_ABORT:-0}" = "1" ]
}

if [ "$DRY_RUN" = "1" ]; then
    FULL_CELLS="$(plan_full_matrix | wc -l)"
    FULL_RUNS=$((FULL_CELLS * REPEATS))
    FALLBACK_CELLS="$(plan_fallback_matrix | wc -l)"
    FALLBACK_RUNS=$((FALLBACK_CELLS * REPEATS))
    TIER_CELLS="$(plan_full_tier | wc -l)"
    TIER_RUNS=$((TIER_CELLS * REPEATS))
    echo "DRY-RUN fast harness plan (no fio, no scheduler switch, no target writes)"
    echo "  Full matrix: $FULL_CELLS cells x $REPEATS repeats = $FULL_RUNS runs (54 with adios)"
    echo "  Fallback without adios: $FALLBACK_CELLS cells x $REPEATS repeats = $FALLBACK_RUNS runs (40 without adios)"
    echo "  FULL tier: $TIER_CELLS cells x $REPEATS repeats = $TIER_RUNS runs (4 per scheduler)"
    echo "  Timing: runtime=$RUNTIME ramp=$RAMP cooldown=$COOLDOWN repeats=$REPEATS (~43 s per run)"
    echo "  Cells (full matrix, sched|workload|qd|job):"
    plan_full_matrix | sed 's/^/    /'
    if [ "$FULL_RUNS" -ne 54 ] || [ "$FALLBACK_RUNS" -ne 40 ]; then
        echo "ERROR: matrix drift (expected 54 full / 40 without adios)." >&2
        exit 1
    fi
    exit 0
fi

if brake_requested; then
    echo "ABORT requested via BENCH_ABORT/IOSCHED_ABORT=1; exiting before any run." >&2
    exit 130
fi

# ---- Safety fence: target must live under /mnt/data/ on nvme0n1-backed fs ----
TARGET_REAL="$(realpath -m "$TARGET_DIR")"
case "$TARGET_REAL" in
    /mnt/data/*) ;;
    *)
        echo "ERROR: --target-dir must resolve under /mnt/data/ (got: $TARGET_REAL)." >&2
        exit 1
        ;;
esac

MOUNT_SRC="$(findmnt -n -o SOURCE --target "$TARGET_REAL" 2>/dev/null || true)"
if [ -z "$MOUNT_SRC" ]; then
    # Target may not exist yet: check the closest existing parent instead.
    PARENT="$TARGET_REAL"
    while [ ! -e "$PARENT" ] && [ "$PARENT" != "/mnt/data" ] && [ "$PARENT" != "/" ]; do
        PARENT="$(dirname "$PARENT")"
    done
    MOUNT_SRC="$(findmnt -n -o SOURCE --target "$PARENT" 2>/dev/null || true)"
fi
case "$MOUNT_SRC" in
    *nvme0n1*)
        ;;
    *)
        echo "ERROR: target filesystem must be backed by nvme0n1 (source: '${MOUNT_SRC:-unknown}')." >&2
        echo "  Refusing to run outside the designated NVMe test filesystem." >&2
        exit 1
        ;;
esac

# Resolve the parent block device for scheduler switching from the mount source.
SRC_BASE="$(basename "$MOUNT_SRC")"
# If the source basename is already a whole disk, keep it as-is; otherwise
# strip the partition suffix (nvme0n1p2 -> nvme0n1, sda1 -> sda).
# NVMe/MMC-style partitions use a 'p' separator, so strip only trailing
# p<digits> for those; other names strip trailing digits.
if [ -e "/sys/block/$SRC_BASE/queue/scheduler" ]; then
    PARENT_DEV="$SRC_BASE"
else
    case "$SRC_BASE" in
        nvme*n*p[0-9]*|mmcblk*p[0-9]*|nbd*p[0-9]*|loop*p[0-9]*)
            PARENT_DEV="$(printf '%s' "$SRC_BASE" | sed 's/p[0-9][0-9]*$//')"
            ;;
        *)
            PARENT_DEV="$(printf '%s' "$SRC_BASE" | sed 's/[0-9][0-9]*$//')"
            ;;
    esac
fi
SCHED_FILE="/sys/block/$PARENT_DEV/queue/scheduler"
if [ ! -f "$SCHED_FILE" ]; then
    echo "ERROR: scheduler file not found: $SCHED_FILE (derived from mount source $MOUNT_SRC)." >&2
    exit 1
fi

if ! command -v "$FIO" >/dev/null 2>&1; then
    echo "ERROR: fio not found (FIO=$FIO). Install fio first; this script never installs it." >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found." >&2
    exit 1
fi

mkdir -p "$TARGET_DIR" "$RESULTS_DIR" "$RESULTS_DIR/raw"
TEST_FILE="$TARGET_REAL/fio-test-file"

AVAIL_SCHEDS="$(cat "$SCHED_FILE" 2>/dev/null || true)"
has_sched() {
    local want="$1"
    case " $AVAIL_SCHEDS " in
        *"[$want]"*) return 0 ;;
        *" $want "*) return 0 ;;
        *) return 1 ;;
    esac
}

# Pick the live matrix: full when adios is available, else QD8-only fallback.
if has_sched adios; then
    MATRIX_FN="plan_full_matrix"
    MODE="full-6sched"
else
    MATRIX_FN="plan_fallback_matrix"
    MODE="fallback-no-adios"
    echo "NOTE: adios not available; using fallback QD8-only matrix (40 runs)." >&2
fi

SCHED_LOG="$RESULTS_DIR/scheduler-log.tsv"
METADATA="$RESULTS_DIR/metadata.json"

cleanup_restore_kpp() {
    if has_sched kpp 2>/dev/null; then
        printf '%s' "kpp" | tee "$SCHED_FILE" >/dev/null 2>&1 || true
        echo "Restored scheduler to kpp (trap)." >&2 || true
    fi
}
trap cleanup_restore_kpp EXIT INT TERM

FIO_VER="$("$FIO" --version 2>/dev/null || echo unknown)"
KERN="$(uname -r 2>/dev/null || echo unknown)"
HOST="$(hostname 2>/dev/null || echo unknown)"
STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

python3 - "$METADATA" <<EOF
import json, sys
meta = {
    "generated_at": "$STAMP",
    "host": "$HOST",
    "kernel": "$KERN",
    "fio_version": "$FIO_VER",
    "mode": "$MODE",
    "target_dir": "$TARGET_DIR",
    "target_realpath": "$TARGET_REAL",
    "mount_source": "$MOUNT_SRC",
    "schedulers_full": "$SCHEDS_FULL".split(),
    "repeats": $REPEATS,
    "timing": {"runtime_s": $RUNTIME, "ramp_s": $RAMP, "cooldown_s": $COOLDOWN,
               "per_run_wall_s": 43},
    "units": {"iops": "IOPS", "lat": "ms", "qd": "queue depth"},
    "legend": {"schedulers": "$SCHEDS_FULL".split(),
               "workloads": ["randread", "randwrite", "randrw", "seqread"]},
    "captions": {
        "fig1": "Figure 1. kyber vs kpp at QD8 (IOPS and p99 latency, n=2, range bars).",
        "fig2": "Figure 2. kpp vs all schedulers on randrw 70/30 QD8 (n=2, range bars).",
        "fig3": "Figure 3. QD scaling on randread 4k for kpp/kyber (QD1/8/32, n=2, range bars)."
    },
    "footnote": "n=2 repeats per cell; bars show range (min-max); file-backed fio (size 4G, direct=1, libaio, numjobs 1, time_based, group_reporting); target on nvme0n1-backed /mnt/data filesystem.",
    "abort_env": "BENCH_ABORT=1 (or IOSCHED_ABORT=1)",
}
json.dump(meta, open(sys.argv[1], "w"), indent=2)
EOF

printf 'timestamp\trun_idx\tscheduler_target\tscheduler_before\tscheduler_after\tcell\n' > "$SCHED_LOG"

run_idx=0
total_cells="$($MATRIX_FN | wc -l)"
total_runs=$((total_cells * REPEATS))

MATRIX_TMP="$(mktemp)"
$MATRIX_FN > "$MATRIX_TMP"
while IFS='|' read -r sched workload qd jobfile; do
    for rep in $(seq 1 "$REPEATS"); do
        if brake_requested; then
            echo "ABORT requested via BENCH_ABORT/IOSCHED_ABORT=1; stopping before next run." >&2
            exit 130
        fi
        run_idx=$((run_idx + 1))
        BEFORE="$(cat "$SCHED_FILE" 2>/dev/null || echo unknown)"
        printf '%s' "$sched" | tee "$SCHED_FILE" >/dev/null
        AFTER="$(cat "$SCHED_FILE" 2>/dev/null || echo unknown)"
        printf '%s\t%d\t%s\t%s\t%s\t%s|%s|qd%s|rep%d\n' \
            "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$run_idx" "$sched" "$BEFORE" "$AFTER" \
            "$sched" "$workload" "$qd" "$rep" >> "$SCHED_LOG"
        echo "[$run_idx/$total_runs] $sched $workload qd$qd rep$rep ($jobfile)"

        TMP_JOB="$(mktemp)"
        sed -e "s/__QD__/$qd/g" \
            -e "s/__RUNTIME__/$RUNTIME/g" \
            -e "s/__RAMP__/$RAMP/g" \
            -e "s|__FILENAME__|$TEST_FILE|g" \
            "$JOBS_DIR/$jobfile" > "$TMP_JOB"
        RAW_JSON="$RESULTS_DIR/raw/${sched}_${workload}_qd${qd}_rep${rep}.json"
        "$FIO" --output-format=json --output="$RAW_JSON" "$TMP_JOB"
        rm -f "$TMP_JOB"

        if [ "$rep" = "$REPEATS" ]; then
            # Aggregate the REPEATS raw outputs for this cell into results/*.json.
            python3 - "$sched" "$workload" "$qd" "$RESULTS_DIR" "$RESULTS_DIR/raw" <<'PYEOF'
import glob, json, sys
sched, workload, qd = sys.argv[1], sys.argv[2], int(sys.argv[3])
resdir, rawdir = sys.argv[4], sys.argv[5]
pat = f"{rawdir}/{sched}_{workload}_qd{qd}_rep*.json"
files = sorted(glob.glob(pat))
if len(files) != 2:
    sys.exit(f"ERROR: expected 2 repeats for {sched}/{workload}/qd{qd}, found {len(files)}: {files}")
iops_vals, lat_vals = [], []
for f in files:
    d = json.load(open(f))
    job = d["jobs"][0]
    r, w = job["read"], job["write"]
    iops_vals.append(float(r.get("iops", 0)) + float(w.get("iops", 0)))
    def p99(x):
        try:
            return float(x["clat_ns"]["percentile"]["99.000000"]) / 1e6
        except KeyError:
            return float(x["clat_ns"]["mean"]) / 1e6
    lat_vals.append(max(p99(r), p99(w)))
out = {
    "scheduler": sched, "workload": workload, "qd": qd, "n": len(files),
    "iops": {"values": iops_vals, "mean": sum(iops_vals) / len(iops_vals),
             "min": min(iops_vals), "max": max(iops_vals)},
    "lat_p99_ms": {"values": lat_vals, "mean": sum(lat_vals) / len(lat_vals),
                   "min": min(lat_vals), "max": max(lat_vals)},
}
json.dump(out, open(f"{resdir}/{sched}_{workload}_qd{qd}.json", "w"), indent=2)
PYEOF
        fi

        if [ "$run_idx" -lt "$total_runs" ]; then
            sleep "$COOLDOWN"
        fi
    done
done < "$MATRIX_TMP"
rm -f "$MATRIX_TMP"

echo "Done. Results in $RESULTS_DIR (mode $MODE). Scheduler restored to kpp via trap."
