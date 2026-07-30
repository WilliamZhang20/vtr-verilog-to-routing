#!/usr/bin/env python
"""Falsification test: CAN congestion-aware packing move W_min at all?

The 108-run min-W sweep is testing a setting too weak to answer this. At
alpha=0.2 / min_util=0.6 the pin-utilization scale only reaches [0.8, 1.0], the
criticality guard exempts up to 52% of congested tiles, cluster count moves under
1%, and W_min is unchanged in 7 of 11 paired runs. "No effect" there is
indistinguishable from "not engaged".

So this drives the lever hard -- alpha=0.7, min_util=0.4 (scale down to 0.4),
crit_th=0.95 so the guard barely suppresses anything -- and asks one binary
question: does W_min move? A CPD regression is ACCEPTABLE here; the point is to
establish whether the mechanism has any authority over routability at all.

  - W_min still pinned  => the lever cannot influence routing; delete the feature.
  - W_min moves         => there is a real frontier and the shipped default is
                           simply mis-tuned; tune toward it and grade properly.

Both placers, because the analysis predicts more headroom on b2b. Three cheap
circuits x 2 seeds: enough to see a W_min shift, not enough to grade QoR, which
is deliberate -- this is a mechanism probe, not a QoR arm.
"""

import csv
import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path("/u5/w223zhan/vtr-verilog-to-routing")
VPR = ROOT / "build/vpr/vpr"
ARCH = ROOT / "vtr_flow/arch/COFFE_22nm/k6FracN10LB_mem20K_complexDSP_customSB_22nm.xml"
CIRCUIT_DIR = ROOT / "vtr_flow/benchmarks/koios_ap_blif"
OUT = Path(__file__).parent / "falsify"
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", "6"))

CIRCUITS = [("softmax", 56), ("lenet", 41), ("spmv", 84)]
SEEDS = [1, 2]
NESTEROV = ["--ap_global_placer", "nonlinear-nesterov"]
B2B = ["--ap_global_placer", "simpl", "--ap_analytical_solver", "lp-b2b"]

STRONG = {
    "VPR_APPACK_CONGESTION": "both",
    "VPR_APPACK_CONGESTION_ALPHA": "0.7",
    "VPR_APPACK_CONGESTION_MIN_UTIL": "0.4",
    "VPR_APPACK_CONGESTION_GAIN_ALPHA": "0.7",
    "VPR_APPACK_CONGESTION_CRIT_TH": "0.95",
}
ARMS = {
    "nest_off": ({}, NESTEROV),
    "nest_strong": (STRONG, NESTEROV),
    "b2b_off": ({}, B2B),
    "b2b_strong": (STRONG, B2B),
}

METRICS = {
    "min_chan_width": r"Best routing used a channel width factor of (\d+)",
    "crit_path_delay": r"Critical path: (.*) ns",
    "total_wirelength": r"\s*Total wirelength: (\d+)",
    "num_clb": r"\s*Netlist clb blocks:\s*(\d+)",
    "tightened": r"above-average=\d+ \(tightened=(\d+)",
    "crit_exempt": r"critical-exempt=(\d+)\)",
    "total_runtime": r"The entire flow of VPR took (.*) seconds",
}

NEUTRAL = {"LD_LIBRARY_PATH": "/u5/w223zhan/.local/lib"}
CLEARED = [k for k in STRONG] + [
    "VPR_NESTEROV_DENSITY_TELEM", "VPR_SA_WASH_TRACE", "VPR_DLA_TRACE",
    "VPR_APPACK_LEGACY_DIST_GROW", "VPR_PLACE_DELAY_CACHE",
]


def run_one(job):
    arm, circuit, width, seed = job
    work = OUT / arm / f"{circuit}_seed{seed}"
    log = work / "vpr.out"
    if log.exists() and "The entire flow of VPR took" in log.read_text(errors="ignore"):
        return job, "cached"
    work.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for k in CLEARED:
        env.pop(k, None)
    env.update(NEUTRAL)
    arm_env, placer = ARMS[arm]
    env.update(arm_env)
    cmd = [str(VPR), str(ARCH), str(CIRCUIT_DIR / f"{circuit}.blif"),
           "--device", "auto", "--device_width", str(width),
           "--analytical_place", "--route", "--analysis",
           *placer, "--seed", str(seed)]
    with log.open("w") as h:
        rc = subprocess.call(cmd, cwd=work, stdout=h, stderr=subprocess.STDOUT, env=env)
    return job, f"rc={rc}"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    jobs = [(a, c, w, s) for c, w in CIRCUITS for s in SEEDS for a in ARMS]
    print(f"{len(jobs)} runs, {MAX_PARALLEL} at a time (strong setting: {STRONG})", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as pool:
        for job, status in pool.map(run_one, jobs):
            done += 1
            print(f"[{done}/{len(jobs)}] {job[0]:<12} {job[1]:<10} seed{job[3]} {status}", flush=True)

    rows = []
    for arm in ARMS:
        for c, _ in CIRCUITS:
            for s in SEEDS:
                f = OUT / arm / f"{c}_seed{s}" / "vpr.out"
                if not f.exists():
                    continue
                t = f.read_text(errors="ignore")
                if "The entire flow of VPR took" not in t:
                    continue
                row = {"arm": arm, "circuit": c, "seed": s}
                for k, p in METRICS.items():
                    m = re.findall(p, t)
                    row[k] = m[-1] if m else ""
                row["gate_seen"] = (not ARMS[arm][0]) or "APPack congestion map ENABLED" in t
                rows.append(row)
    if rows:
        csv_path = OUT / "results.csv"
        with csv_path.open("w", newline="") as h:
            w = csv.DictWriter(h, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {csv_path} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
