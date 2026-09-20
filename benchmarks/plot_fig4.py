"""Figure 4: QD32 ABAB n=5, fresh window, traces armed (greenfield)."""
import json
import pathlib
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

_here = pathlib.Path(__file__).parent
RAW = _here / "raw" if (_here / "raw").is_dir() else _here / "results" / "raw"
OUT = pathlib.Path(__file__).parent / "charts" / "fig4_qd32_abab_n5.png"
ORDER = [("kyber", 1), ("kpp", 1), ("kyber", 2), ("kpp", 2), ("kyber", 3),
         ("kpp", 3), ("kyber", 4), ("kpp", 4), ("kyber", 5), ("kpp", 5)]


def load(s, n):
    d = json.loads((RAW / f"v5_{s}{n}_randread_qd32.json").read_text())
    r = d["jobs"][0]["read"]
    return r["iops"] / 1000.0, r["clat_ns"]["percentile"]["99.000000"] / 1e6


pts = [(s, n, *load(s, n)) for s, n in ORDER]
for s, n, iops, p99 in pts:
    print(f"{s}{n} {iops:.1f}k p99 {p99:.2f}ms")

plt.style.use("seaborn-v0_8-whitegrid")
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
x = np.arange(len(ORDER))
labels = [f"{s}{n}" for s, n, _, _ in pts]
colors = ["#1f77b4" if s == "kyber" else "#ff7f0e" for s, _, _, _ in pts]
ax1.bar(x, [p[2] for p in pts], color=colors)
ax1.set_ylabel("Throughput (kIOPS)")
ax1.set_title("QD32 throughput in run order (A=kyber, B=kpp)")
ax2.bar(x, [p[3] for p in pts], color=colors)
ax2.set_yscale("log")
ax2.set_ylabel("p99 latency (ms, log scale)")
ax2.set_xticks(x)
ax2.set_xticklabels(labels)
ax2.set_title("p99 latency in run order, log scale")
import matplotlib.patches as mpatches
handles = [mpatches.Patch(color="#1f77b4", label="kyber (A)"),
           mpatches.Patch(color="#ff7f0e", label="kpp (B)")]
ax1.legend(handles=handles, fontsize=9, frameon=True)
ax2.legend(handles=handles, fontsize=9, frameon=True)
fig.suptitle("Figure 4. QD32 interleaved ABAB, n=5 per scheduler, fresh window")
fig.text(
    0.5,
    0.01,
    "Run order interleaved A1 B1 A2 B2 A3 B3 A4 B4 A5 B5 on one prepped 4G "
    "file, runtime 30s, ramp 8s, 15s cooldowns. Traces armed: zero "
    "kpp_throttled, 9 kpp_adjust all on WRITE and DISCARD, none cutting "
    "READ depth, so no token throttle explains any gap. kpp mean 37.4k "
    "stable across the window. kyber swings 70.4k to 0.35k. fio 3.42, "
    "kernel 7.2.6-1-cachyos-kpp with timer hygiene fix, NVME 512GB. "
    "2026-09-20.",
    ha="center",
    fontsize=7,
)
fig.tight_layout(rect=[0, 0.08, 1, 0.94])
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=300, bbox_inches="tight")
print("wrote", OUT, OUT.stat().st_size, "bytes")
