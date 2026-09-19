# Methodology — KPP fast benchmarks

## Goal

Head-to-head file-backed comparison of `kpp` vs `kyber` (Figure 1), `kpp`
vs all available schedulers on a mixed workload (Figure 2), and queue-depth
scaling for the primary pair (Figure 3), within a single ~45 min run
(~35 min fallback without `adios`).

## Test path and device handling

- All I/O targets a regular `fio-test-file` inside `--target-dir`, which must
  resolve under `/mnt/data/` on a filesystem backed by `nvme0n1`
  (`realpath` prefix check plus `findmnt` mount-source check). The runner
  creates the target dir at runtime only; `--dry-run` creates nothing.
- No raw-device I/O is performed by this harness. The parent block device
  name is derived from the `findmnt` source only to locate the
  `/sys/block/<disk>/queue/scheduler` switch; the fio `filename` is always
  the regular test file.
- Scheduler under test is set before every run by writing the scheduler
  file; before/after state plus timestamp is appended to
  `results/scheduler-log.tsv`. A `trap` on EXIT/INT/TERM restores `kpp`.

## fio jobs

- Engine: `direct=1`, `ioengine=libaio`, `numjobs=1`, `size=4G`,
  `time_based`, `group_reporting` (fixed in every job file).
- Timing: `runtime=25`, `ramp_time=3`, with a `10 s` cooldown sleep between
  runs (~43 s wall per run including switch/parse overhead).
- Jobs: `randread-4k` (`randread`, `4k`), `randwrite-4k` (`randwrite`, `4k`),
  `randrw-70-30-4k` (`randrw`, `rwmixread=70`, `4k`), `seqread-128k`
  (`read`, `128k`).
- `__QD__`, `__RUNTIME__`, `__RAMP__`, `__FILENAME__` are substituted per
  cell from the job templates; the queue depth per cell follows the matrix
  in the README (QD8 base; QD1/QD32 sweep for `kpp`/`kyber` randread;
  `adios` seqread omitted for the time budget).

## Matrix and repeats

- Full: 27 cells x 2 repeats = 54 runs across 6 schedulers
  (`none`, `mq-deadline`, `kyber`, `bfq`, `kpp`, `adios`).
- Fallback: 20 cells x 2 repeats = 40 runs (QD8-only, no `adios`, no sweep).
- Tiers: FULL = randread QD8 + randrw QD8 everywhere (12 cells, 24 runs,
  4 per scheduler); SUB = remainder (15 cells, 30 runs, 5 per scheduler avg).
- Repeats are back-to-back per cell (`rep1`, `rep2`); aggregates store both
  values with `mean`/`min`/`max` and `n=2`.

## Metrics

- Per repeat, total IOPS = read IOPS + write IOPS from the fio JSON job.
- Per repeat, p99 latency (ms) = max(read clat p99, write clat p99),
  converted from `clat_ns` percentile `99.000000` (falling back to clat mean
  if the percentile key is absent).
- Per cell, the reported point is the mean of the 2 repeats; the error bar
  is the full range (`min`–`max`). `plot_fast.py` aborts if any plotted
  cell has `n != 2` or a value count other than 2.

## Figures

- Figure 1: grouped bars, kyber vs kpp at QD8, one group per workload;
  panel (a) IOPS, panel (b) p99 latency.
- Figure 2: bars per scheduler on randrw 70/30 QD8 (`kpp` highlighted);
  panel (a) IOPS, panel (b) p99 latency.
- Figure 3: lines vs queue depth (QD1/8/32, log-base-2 x-axis) on randread
  for `kpp` and `kyber`; panel (a) IOPS, panel (b) p99 latency.
- All figures at `dpi=300`. Axis units, legend entries, panel captions, and
  the bottom footnote are read from `results/metadata.json`, not hardcoded.

## Emergency stop and failure handling

- `BENCH_ABORT=1` (alias `IOSCHED_ABORT=1`) is checked before every run and
  aborts with exit 130 prior to any further scheduler switch or fio launch.
- `set -euo pipefail` fail-fast; unknown CLI flags exit 2; missing
  `--target-dir` exits 2; safety-fence violations exit 1; matrix drift in
  `--dry-run` (anything but 54/40) exits 1.
- Missing fio/python3, an unresolvable scheduler file, or a missing result
  cell at plot time are hard errors (non-zero exit with a message).

## Limitations and threats

- File-backed on a single `nvme0n1`-backed filesystem: results reflect that
  filesystem/ mount options and page-cache bypass via `direct=1`, not raw
  device behavior; do not generalize to other media without rerunning.
- `n=2` gives range bars only, not confidence intervals; treat small deltas
  as suggestive until a deeper (`n>=5`) confirmation run is done.
- The `adios` seqread cell is intentionally omitted from the full matrix to
  hold the 54-run budget; sequential coverage for `adios` comes from its
  QD8 randread cell only.
- Cooldown is a fixed 10 s sleep, not a measured idle/thermal gate; back to
  back scheduler switches can carry thermal or cache history.
