"""Compare the hardware bench (data/hw_bench.csv) with the simulator's loss-emulation block (data/ns3_campaign_C.csv).

Same scenario on both sides: 20 chunks of 244 B, receiver drops every non-SACK frame with probability p.
Per (SF, mode, p): completion rate (Wilson 95 % interval), mean chunks transmitted and mean sender time (normal 95 % CI).
`overlap` = the hardware interval intersects the simulator interval; for time the hardware is also reported
relative to the simulator (the chip adds a roughly constant per-frame overhead).
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
PLOTS = os.path.join(HERE, "..", "plots", "campaign")
os.makedirs(PLOTS, exist_ok=True)
COLORS = {1: "#9a9a9a", 2: "#5b5b5b", 3: "#1f77b4", 4: "#d62728"}


def wilson(k, n, z=1.96):
    if n == 0:
        return np.nan, np.nan
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * np.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return c - h, c + h


def mean_ci(x):
    x = np.asarray(x, float)
    x = x[~np.isnan(x)]
    if len(x) == 0:
        return np.nan, np.nan, np.nan
    h = 1.96 * x.std(ddof=1) / np.sqrt(len(x)) if len(x) > 1 else 0.0
    return x.mean(), x.mean() - h, x.mean() + h


def main():
    hw = pd.read_csv(os.path.join(DATA, "hw_bench.csv"))
    sim = pd.read_csv(os.path.join(DATA, "ns3_campaign_C.csv"))
    sim["lossPct"] = sim["label"].str.extract(r"loss(\d+)").astype(int)
    sim["mode"] = sim["mode"].str.replace("Mode", "").astype(int)
    sim["complete"] = (sim.uniqueChunks == sim.totalChunks).astype(int)
    sim["durationMs"] = sim.senderDoneS * 1000.0
    hw["complete"] = hw.rxComplete.astype(int)

    rows = []
    for (sf, mode, loss), h in hw.groupby(["sf", "mode", "lossPct"]):
        s = sim[(sim.sf == sf) & (sim["mode"] == mode) & (sim.lossPct == loss)]
        hl, hh = wilson(h.complete.sum(), len(h))
        sl, sh = wilson(s.complete.sum(), len(s))
        hc, hcl, hch = mean_ci(h.chunksTx)
        sc, scl, sch = mean_ci(s.senderChunksTx)
        ht, htl, hth = mean_ci(h.durationMs)
        st, stl, sth = mean_ci(s.durationMs)
        rows.append(dict(sf=sf, mode=mode, lossPct=loss, hw_n=len(h), sim_n=len(s),
                         hw_complete=h.complete.mean(), hw_lo=hl, hw_hi=hh,
                         sim_complete=s.complete.mean(), sim_lo=sl, sim_hi=sh,
                         complete_overlap=bool(hl <= sh and sl <= hh),
                         hw_chunksTx=hc, hw_chunksTx_lo=hcl, hw_chunksTx_hi=hch,
                         sim_chunksTx=sc, sim_chunksTx_lo=scl, sim_chunksTx_hi=sch,
                         chunksTx_overlap=bool(hcl <= sch and scl <= hch),
                         hw_ms=ht, hw_ms_lo=htl, hw_ms_hi=hth, sim_ms=st, sim_ms_lo=stl, sim_ms_hi=sth,
                         hw_over_sim_time=ht / st if st else np.nan))
    res = pd.DataFrame(rows).sort_values(["sf", "mode", "lossPct"])
    res.to_csv(os.path.join(DATA, "hw_vs_sim.csv"), index=False)
    print(res[["sf", "mode", "lossPct", "hw_n", "hw_complete", "sim_complete", "complete_overlap",
               "hw_chunksTx", "sim_chunksTx", "chunksTx_overlap", "hw_over_sim_time"]].round(2).to_string(index=False))

    for sf, d in res.groupby("sf"):
        fig, axes = plt.subplots(1, 3, figsize=(15, 4.2))
        for ax, (key, label) in zip(axes, [("complete", "Completed transfers (fraction)"),
                                           ("chunksTx", "Chunks transmitted"), ("ms", "Sender time (ms)")]):
            for mode, g in d.groupby("mode"):
                c = COLORS[mode]
                x = g.lossPct.values
                ax.fill_between(x, g[f"sim_lo" if key == "complete" else f"sim_{key}_lo"],
                                g[f"sim_hi" if key == "complete" else f"sim_{key}_hi"], color=c, alpha=0.18, lw=0)
                ax.plot(x, g["sim_complete" if key == "complete" else f"sim_{key}"], color=c, lw=1.6,
                        label=f"Mode {mode} simulator")
                y = g["hw_complete" if key == "complete" else f"hw_{key}"].values
                lo = g["hw_lo" if key == "complete" else f"hw_{key}_lo"].values
                hi = g["hw_hi" if key == "complete" else f"hw_{key}_hi"].values
                ax.errorbar(x, y, yerr=[y - lo, hi - y], fmt="o", ms=5, color=c, mfc="white", capsize=2,
                            label=f"Mode {mode} hardware")
            ax.set_xlabel("Receiver frame loss (%)")
            ax.set_ylabel(label)
            ax.grid(alpha=0.3)
        axes[0].legend(fontsize=7)
        fig.suptitle(f"Hardware bench vs simulator, SF{sf}, 20 chunks (bands/bars: 95 % intervals)")
        fig.tight_layout()
        fig.savefig(os.path.join(PLOTS, f"HW_vs_sim_SF{sf}.png"), dpi=300)
        plt.close(fig)


if __name__ == "__main__":
    main()
