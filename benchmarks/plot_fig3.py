"""Figure 3: six schedulers, 4k randread QD8, prepped file (greenfield)."""
import json
import pathlib
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt

COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]

_here = pathlib.Path(__file__).parent
RAW = _here / "raw" if (_here / "raw").is_dir() else _here / "results" / "raw"
OUT = pathlib.Path(__file__).parent / "charts" / "fig3_all_scheds_randread_qd8.png"
SCHEDS = ["kyber", "kpp", "bfq", "mq-deadline", "adios", "none"]


def load(s):
    iops, p99 = [], []
    for rep in (1, 2):
        d = json.loads((RAW / f"v6_{s}_randread_qd8_rep{rep}.json").read_text())
        r = d["jobs"][0]["read"]
        iops.append(r["iops"])
        p99.append(r["clat_ns"]["percentile"]["99.000000"] / 1000.0)
    return np.array(iops), np.array(p99)


data = {s: load(s) for s in SCHEDS}
for s in SCHEDS:
    print(s, [round(v, 1) for v in data[s][0].tolist()])

x = np.arange(len(SCHEDS))
w = 0.6
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
plt.style.use("seaborn-v0_8-whitegrid")
means = [data[s][0].mean() / 1000.0 for s in SCHEDS]
errs = [[m - data[s][0].min() / 1000.0 for s, m in zip(SCHEDS, means)],
        [data[s][0].max() / 1000.0 - m for s, m in zip(SCHEDS, means)]]
ax1.bar(x, means, w, yerr=errs, capsize=4, color=COLORS)
ax1.set_xticks(x)
ax1.set_xticklabels(SCHEDS, rotation=20)
ax1.set_ylabel("Throughput (kIOPS)")
ax1.set_title("4k randread QD8 throughput by scheduler")
pmeans = [data[s][1].mean() / 1000.0 for s in SCHEDS]
perrs = [[m - data[s][1].min() / 1000.0 for s, m in zip(SCHEDS, pmeans)],
         [data[s][1].max() / 1000.0 - m for s, m in zip(SCHEDS, pmeans)]]
ax2.bar(x, pmeans, w, yerr=perrs, capsize=4, color=COLORS)
handles = [mpatches.Patch(color=c, label=s) for s, c in zip(SCHEDS, COLORS)]
ax1.legend(handles=handles, fontsize=7, loc="upper right", frameon=True)
ax2.legend(handles=handles, fontsize=7, loc="upper right", frameon=True)
ax2.set_xticks(x)
ax2.set_xticklabels(SCHEDS, rotation=20)
ax2.set_yscale("log")
ax2.set_ylabel("p99 latency (ms, log scale, mean of 2)")
ax2.set_title("p99 latency by scheduler (wide bars mean unstable reps)")
fig.suptitle("Figure 3. All schedulers, 4k random read, QD8, prepped file")
fig.text(
    0.5,
    0.01,
    "fio 3.42, kernel 7.2.6-1-cachyos-kpp, NVME 512GB on /mnt/data "
    "(btrfs zstd:3), direct=1, prepped 4G file (seq write once, then reads), "
    "runtime 25s, ramp 3s, n=2, bars mean, error bars range. adios and none "
    "each lost one rep to late session instability, so their means underread. "
    "kyber and kpp were run back to back on the same file state. 2026-09-20.",
    ha="center",
    fontsize=7,
)
fig.tight_layout(rect=[0, 0.1, 1, 0.92])
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=300, bbox_inches="tight")
print("wrote", OUT, OUT.stat().st_size, "bytes")
