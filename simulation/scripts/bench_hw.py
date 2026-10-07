"""Hardware bench driver: two Heltec V3 boards (RX on --rx, TX on --tx), frame-loss emulation sweep.

Flash first:  pio run -e heltec_bench_rx -t upload && pio run -e heltec_bench_tx -t upload
Run:          python3 bench_hw.py                 (full sweep, resumable: rows already in the CSV are skipped)
              python3 bench_hw.py --smoke         (SF7, no loss, 2 trials per mode)
Output:       data/hw_bench.csv, one row per transfer:
    sf,mode,lossPct,trial,txOk,durationMs,chunksTx,rxUnique,rxComplete,rxRssiDbm,rxSnrDb
  rxUnique   = distinct data chunks that passed the receiver's loss emulation (same metric as the simulator's uniqueChunks)
  rxComplete = 1 if the receiver reassembled the whole message
Compare with the simulator: python3 run_campaign_ns3.py C  ->  data/ns3_campaign_C.csv
"""
import argparse
import csv
import os
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS_CONTROL = 0x10 | 0x20 | 0x40  # CONN_REQ / CONN_ACK / CONN_NACK
FIELDS = ["sf", "mode", "lossPct", "trial", "txOk", "durationMs", "chunksTx", "rxUnique", "rxComplete",
          "rxRssiDbm", "rxSnrDb"]


def wait_for(port, prefix, timeout):
    end = time.time() + timeout
    while time.time() < end:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith(prefix):
            return line
    raise TimeoutError(f"no '{prefix}' line within {timeout}s")


def drain(port, seconds):
    """Collect every line for `seconds` (the receiver keeps printing while retransmissions arrive)."""
    lines, end = [], time.time() + seconds
    while time.time() < end:
        line = port.readline().decode(errors="replace").strip()
        if line:
            lines.append(line)
    return lines


def summarise_rx(lines):
    chunks, rssi, snr, complete = set(), [], [], 0
    for line in lines:
        f = line.split(",")
        if f[0] == "RXF" and len(f) == 8 and f[5] == "0" and not (int(f[4], 16) & FLAGS_CONTROL):
            chunks.add((int(f[1]), int(f[2])))  # (msgId, chunkIndex)
            rssi.append(float(f[6]))
            snr.append(float(f[7]))
        elif f[0] == "RXMSG":
            complete = 1
    mean = lambda v: round(sum(v) / len(v), 1) if v else ""
    return len(chunks), complete, mean(rssi), mean(snr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rx", default="/dev/ttyUSB0")
    ap.add_argument("--tx", default="/dev/ttyUSB1")
    ap.add_argument("--sf", type=int, nargs="+", default=[7])
    ap.add_argument("--loss", type=int, nargs="+", default=[0, 5, 10, 20, 30, 40, 50])
    ap.add_argument("--modes", type=int, nargs="+", default=[1, 2, 3, 4])
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--chunks", type=int, default=20)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "data", "hw_bench.csv"))
    ap.add_argument("--smoke", action="store_true")
    args = ap.parse_args()
    if args.smoke:
        args.sf, args.loss, args.trials = [7], [0], 2

    done = set()
    if os.path.exists(args.out):
        with open(args.out) as f:
            done = {(r["sf"], r["mode"], r["lossPct"], r["trial"]) for r in csv.DictReader(f)}
    new_file = not os.path.exists(args.out)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    rx = serial.Serial(args.rx, 115200, timeout=0.5)
    tx = serial.Serial(args.tx, 115200, timeout=0.5)
    wait_for(rx, "READY,RX", 20)
    wait_for(tx, "READY,TX", 20)

    with open(args.out, "a", newline="") as out:
        w = csv.DictWriter(out, FIELDS)
        if new_file:
            w.writeheader()
        for sf in args.sf:
            for port in (rx, tx):
                port.write(f"SF {sf}\n".encode())
                wait_for(port, "OK,SF", 5)
            for mode in args.modes:
                for loss in args.loss:
                    rx.write(f"DROP {loss}\n".encode())
                    wait_for(rx, "OK,DROP", 5)
                    for trial in range(1, args.trials + 1):
                        if (str(sf), str(mode), str(loss), str(trial)) in done:
                            continue
                        rx.reset_input_buffer()
                        tx.write(f"TRIAL {mode} {args.chunks}\n".encode())
                        t = wait_for(tx, "TX,", 900).split(",")
                        unique, complete, rssi, snr = summarise_rx(drain(rx, 3.0))
                        row = dict(sf=sf, mode=mode, lossPct=loss, trial=trial, txOk=t[4], durationMs=t[5],
                                   chunksTx=t[6], rxUnique=unique, rxComplete=complete, rxRssiDbm=rssi, rxSnrDb=snr)
                        w.writerow(row)
                        out.flush()
                        print(row, flush=True)


if __name__ == "__main__":
    main()
