"""Figure 1: KPP vs Kyber, 4k randread QD8 (greenfield, no legacy code)."""
import json
import pathlib
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

RAW = pathlib.Path(__file__).parent / "results" / "raw"
OUT = pathlib.Path(__file__).parent / "charts" / "fig1_kyber_vs_kpp.png"


def load(sched):
    iops, p99 = [], []
    for rep in (1, 2):
        d = json.loads((RAW / f"v6_{sched}_randread_qd8_rep{rep}.json").read_text())
        r = d["jobs"][0]["read"]
        iops.append(r["iops"])
        p99.append(r["clat_ns"]["percentile"]["99.000000"] / 1000.0)
    return np.array(iops), np.array(p99)


kyber_iops, kyber_p99 = load("kyber")
kpp_iops, kpp_p99 = load("kpp")

for name, a in (("kyber_iops", kyber_iops), ("kpp_iops", kpp_iops)):
    print(name, a.tolist(), "mean", round(float(a.mean()), 1))

d_iops = (kpp_iops.mean() - kyber_iops.mean()) / kyber_iops.mean() * 100
d_p99 = (kpp_p99.mean() - kyber_p99.mean()) / kyber_p99.mean() * 100
print(f"delta_iops_pct={d_iops:.2f} delta_p99_pct={d_p99:.2f}")

plt.style.use("seaborn-v0_8-whitegrid")
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 5))
x = np.arange(1)
w = 0.35


def bars(ax, k, p, unit):
    m1, m2 = k.mean(), p.mean()
    e1 = [[m1 - k.min()], [k.max() - m1]]
    e2 = [[m2 - p.min()], [p.max() - m2]]
    ax.bar(x - w / 2, [m1], w, yerr=e1, capsize=4, label="kyber")
    ax.bar(x + w / 2, [m2], w, yerr=e2, capsize=4, label="kpp")
    ax.set_xticks(x)
    ax.set_xticklabels(["4k randread\nQD8"])
    ax.set_ylabel(unit)


bars(ax1, kyber_iops / 1000.0, kpp_iops / 1000.0, "Throughput (kIOPS)")
bars(ax2, kyber_p99, kpp_p99, "p99 latency (us)")
ax1.legend(frameon=True)
ax2.legend(frameon=True)
ax1.set_title(f"Throughput ({d_iops:+.1f} percent kpp vs kyber)")
ax2.set_title(f"p99 latency ({d_p99:+.1f} percent kpp vs kyber)")
fig.suptitle("Figure 1. KPP vs Kyber, 4k random read, queue depth 8")
fig.text(
    0.5,
    0.01,
    "fio 3.42, kernel 7.2.6-1-cachyos-kpp with timer hygiene fix, NVME 512GB on /mnt/data "
    "(btrfs zstd:3), direct=1, 4G file, runtime 25s, ramp 3s, n=2, "
    "bars show mean, error bars show range. 2026-09-20 (fixed kernel).",
    ha="center",
    fontsize=7,
)
fig.tight_layout(rect=[0, 0.06, 1, 0.92])
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=300, bbox_inches="tight")
print("wrote", OUT, OUT.stat().st_size, "bytes")
