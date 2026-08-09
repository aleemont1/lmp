import os
import subprocess
import itertools
import time
from multiprocessing import Pool

NS3_DIR = "/home/aleemont/uni/LoRaMultiPacket/simulation/ns3-workspace/ns-3-dev"
RESULTS_FILE = "/home/aleemont/uni/LoRaMultiPacket/simulation/data/ns3_results.csv"

distances = list(range(100, 15100, 100))
sfs = [7, 9, 10]
modes = ["Mode1", "Mode2", "Mode3", "Mode4"]
envs = ["LOS", "URBAN", "RURAL"]
seeds = list(range(1, 31)) # 30 trials — 95% CI (IEEE journal standard, as per supervisor feedback)

combinations = list(itertools.product(distances, sfs, modes, envs, seeds))
total_sims = len(combinations)
print(f"Total simulations to run: {total_sims}")

# Rebuild once before starting
subprocess.run(["./ns3", "build"], cwd=NS3_DIR, check=True)

def run_sim(args):
    dist, sf, mode, env, seed = args
    cmd_str = f"scratch/lora-multipacket-sim --distance={dist} --mode={mode} --sf={sf} --env={env} --seed={seed}"
    cmd = ["./ns3", "run", cmd_str, "--no-build"]
    
    try:
        res = subprocess.run(cmd, cwd=NS3_DIR, capture_output=True, text=True, check=False)
        if res.returncode != 0:
            print(f"Error in sim (dist={dist}, sf={sf}, mode={mode}, env={env}, seed={seed}): {res.stderr}")
        for line in res.stdout.splitlines():
            if line.startswith("RESULT:"):
                return line[7:] # strip RESULT:
    except Exception as e:
        pass
    return None

if __name__ == "__main__":
    start_time = time.time()
    workers = min(12, os.cpu_count() or 4)
    
    os.makedirs(os.path.dirname(RESULTS_FILE), exist_ok=True)
    # Clear the file first
    with open(RESULTS_FILE, "w") as f:
        pass
        
    completed = 0
    with Pool(workers) as p:
        # Use imap_unordered to process as they finish and write incrementally
        for result in p.imap_unordered(run_sim, combinations):
            completed += 1
            if result:
                with open(RESULTS_FILE, "a") as f:
                    f.write(result + "\n")
            if completed % 500 == 0:
                print(f"Completed {completed}/{total_sims} simulations...")
    
    end_time = time.time()
    print(f"All {total_sims} simulations completed in {end_time - start_time:.2f} seconds.")
    print(f"Results incrementally saved to {RESULTS_FILE}")
