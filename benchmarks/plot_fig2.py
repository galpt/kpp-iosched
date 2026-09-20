"""Figure 2: KPP vs Kyber across patterns, desktop-under-load POV (greenfield)."""
import json
import pathlib
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

RAW = pathlib.Path(__file__).parent / "results" / "raw"
OUT = pathlib.Path(__file__).parent / "charts" / "fig2_patterns_kyber_vs_kpp.png"

PATTERNS = [
    ("randread", "read", "kIOPS"),
    ("randwrite", "write", "kIOPS"),
    ("randrw", "read", "kIOPS"),
    ("seqread", "read", "MiB/s"),
]


PREFIX = {"randread": "v6", "randwrite": "v7", "randrw": "v7",
          "seqread": "v7"}


def load(sched, pat, key):
    vals = []
    for rep in (1, 2):
        pre = PREFIX[pat]
        d = json.loads((RAW / f"{pre}_{sched}_{pat}_qd8_rep{rep}.json").read_text())
        j = d["jobs"][0][key]
        if pat == "seqread":
            vals.append(j["bw"] / 1024.0)
        else:
            vals.append(j["iops"] / 1000.0)
    return np.array(vals)


fig, axes = plt.subplots(2, 2, figsize=(11, 7))
fig.suptitle("Figure 2. KPP vs Kyber across patterns, desktop under load, QD8")
for ax, (pat, key, unit) in zip(axes.flat, PATTERNS):
    k = load("kyber", pat, key)
    p = load("kpp", pat, key)
    x = np.arange(1)
    w = 0.35
    for vals, label, off in ((k, "kyber", -w / 2), (p, "kpp", w / 2)):
        m = vals.mean()
        ax.bar(x + off, [m], w, yerr=[[m - vals.min()], [vals.max() - m]],
               capsize=4, label=label)
    d = (p.mean() - k.mean()) / k.mean() * 100
    ax.set_xticks(x)
    ax.set_xticklabels([pat])
    ax.set_ylabel(unit)
    ax.set_title(f"{pat} ({d:+.1f} percent kpp vs kyber)")
    ax.legend(frameon=True, fontsize=8)
fig.text(
    0.5,
    0.01,
    "First person desktop view. Same live desktop under load for both, "
    "so background noise hits each scheduler alike. fio 3.42, kernel "
    "7.2.6-1-cachyos-kpp, NVME 512GB on /mnt/data (btrfs zstd:3), "
    "direct=1, fresh 4G file per pattern with one prep write each, "
    "runtime 25s, ramp 3s, n=2, bars show mean, "
    "error bars show range. Wide bars on writes mean the filesystem moved "
    "under the test, not hidden. 2026-09-20.",
    ha="center",
    fontsize=7,
    wrap=True,
)
fig.tight_layout(rect=[0, 0.08, 1, 0.93])
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=300, bbox_inches="tight")
print("wrote", OUT, OUT.stat().st_size, "bytes")
