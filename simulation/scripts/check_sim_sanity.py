"""Sanity checks for lora-multipacket-sim (run after every simulator change).

Usage: python3 check_sim_sanity.py   (needs a built scratch/lora-multipacket-sim in NS3_DIR)
"""
import os
import subprocess

NS3_DIR = os.path.join(os.path.dirname(__file__), "..", "ns3-workspace", "ns-3-dev")
BIN = os.path.join(NS3_DIR, "build", "scratch", "ns3.48-lora-multipacket-sim-default")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(NS3_DIR, "build", "lib"))
LOSSY = ["--env=URBAN", "--n=4.0", "--pl1km=113", "--m=1.0", "--sigma=0"]  # ~-8 dB mean SNR at 8.2 km, SF7


def run(distance, mode, seed=1, extra=("--env=LOS",), sf=7):
    cmd = [BIN, f"--distance={distance}", f"--mode={mode}", f"--sf={sf}", f"--seed={seed}", *extra]
    out = subprocess.run(cmd, capture_output=True, text=True, env=ENV, check=True).stdout
    f = next(l for l in out.splitlines() if l.startswith("RESULT:"))[7:].split(",")
    return dict(unique=int(f[4]), total=int(f[5]), rounds=int(f[6]), last_new=float(f[7]),
                ok=int(f[10]), done=float(f[11]))


# 1. No loss: every mode delivers all 20 chunks, with no repair rounds.
for m in ["Mode1", "Mode2", "Mode3", "Mode4", "SWBase", "SWOpt", "SWBaseConn"]:
    r = run(100, m)
    assert r["unique"] == r["total"] == 20 and r["rounds"] == 0 and r["ok"] == 1, (m, r)

# 2. No loss: delivery time is ~20 back-to-back frames (SF7/125 kHz, 255 B frame = ~0.40 s each).
r = run(100, "Mode3")
assert 20 * 0.39 < r["last_new"] < 20 * 0.42, r
# Stop-and-wait pays one ACK per frame, so it must be slower than one SACK per burst.
assert run(100, "SWBase")["done"] > run(100, "Mode3")["done"]

# 3. Loss: best-effort loses chunks; SACK modes recover them with selective repair rounds.
lossy = [run(8200, "Mode1", s, LOSSY) for s in range(1, 6)]
assert all(r["unique"] < 20 for r in lossy), lossy
rel = [run(8200, "Mode3", s, LOSSY) for s in range(1, 6)]
# (a sender can still give up after R_max lost SACKs although the receiver has everything: ok=0, unique=20)
assert all(r["unique"] == 20 and r["rounds"] >= 1 for r in rel), rel
assert sum(r["ok"] for r in rel) >= 4, rel

# 4. Unique counting: a repair round must never push delivered chunks above the total.
assert all(r["unique"] <= r["total"] for r in rel + lossy)

print("sim sanity: OK")
