#!/usr/bin/env python
"""Grade congestion-aware packing at MINIMUM CHANNEL WIDTH.

This is the only regime where the result can mean anything. The whole existing
75-circuit harness runs at a fixed `route_chan_width 300`, where measured peak
channel utilization is 0.35-0.87 -- routing never binds, so a routability lever
has nothing to show. Three earlier congestion-aware arms were graded there and
all came back inside noise.

So these runs omit --route_chan_width entirely and let VPR binary-search the
minimum routable channel width. The headline metric is dW_min; routed wirelength
is secondary; CPD is a *guard* (it must not regress), not the headline. If this
mechanism works at all, the shape of the win is lower W_min and lower routed WL
with CPD roughly flat.

BOTH global placers are graded, because a nesterov-only result would be
uninterpretable: the analysis predicts nesterov has the LEAST headroom for a
congestion-relief mechanism (its raw GP is already more spread than b2b's), so a
nesterov null cannot distinguish "the mechanism does not work" from "the
mechanism works but nesterov had nothing to relieve". The b2b arm is what
separates those, and it also gives the nesterov/b2b ratio move directly.

Deliberately dropped: the pinutil-vs-gain decomposition (which of the two levers
does the work). That is a secondary question; it can be answered later if there
is any signal to decompose.
"""

import csv
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path("/u5/w223zhan/vtr-verilog-to-routing")
VPR = ROOT / ".agents/bin/vpr_expt_20260729"  # snapshot: source tree is being cleaned up
ARCH = ROOT / "vtr_flow/arch/COFFE_22nm/k6FracN10LB_mem20K_complexDSP_customSB_22nm.xml"
CIRCUIT_DIR = ROOT / "vtr_flow/benchmarks/koios_ap_blif"
OUT = Path(__file__).parent / "wmin"
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", "18"))

CIRCUITS = [
    # Ordered by MEASURED full-flow runtime (grade sweep, seconds at W=300), not by
    # device width -- width does not predict runtime (dla_like.small is 88 wide and
    # the slowest circuit in the set at 232 min, vs dnnweaver at 192 wide / 155 min).
    # Cheapest first so that complete paired arm sets arrive early instead of after
    # the most expensive circuit has finished.
    ("softmax", 56),            # ~7 min
    ("lenet", 41),              # ~10 min
    ("spmv", 84),               # ~14 min
    ("tpu_like.small.os", 136), # ~38 min
    ("attention_layer", 88),    # ~56 min
    ("bnn", 95),                # ~97 min
    ("clstm_like.small", 130),  # ~139 min
    ("dnnweaver", 192),         # ~155 min
    ("dla_like.small", 88),     # ~232 min
]
SEEDS = [1, 2, 3]
NESTEROV = ["--ap_global_placer", "nonlinear-nesterov"]
B2B = ["--ap_global_placer", "simpl", "--ap_analytical_solver", "lp-b2b"]

# arm -> (env overrides, global-placer arguments)
ARMS = {
    "nest_off": ({}, NESTEROV),
    "nest_cong": ({"VPR_APPACK_CONGESTION": "both"}, NESTEROV),
    "b2b_off": ({}, B2B),
    "b2b_cong": ({"VPR_APPACK_CONGESTION": "both"}, B2B),
}

METRICS = {
    # The headline. From vpr.route_min_chan_width.txt.
    "min_chan_width": r"Best routing used a channel width factor of (\d+)",
    "crit_path_delay": r"Critical path: (.*) ns",
    "total_wirelength": r"\s*Total wirelength: (\d+)",
    "post_fl_cpd": r"Initial placement estimated Critical Path Delay \(CPD\): (.*) ns",
    "post_dp_cpd": r"Placement estimated critical path delay \(least slack\): (.*) ns",
    "num_clb": r"\s*Netlist clb blocks:\s*(\d+)",
    "total_runtime": r"The entire flow of VPR took (.*) seconds",
}

NEUTRAL_ENV = {
    "VPR_NESTEROV_ADAPT_DENSITY": "1",
    "VPR_NESTEROV_CAPACITY_MASK": "0",
    "LD_LIBRARY_PATH": "/u5/w223zhan/.local/lib",
}
CLEARED_ENV = [
    "VPR_APPACK_CONGESTION", "VPR_APPACK_CONGESTION_ALPHA",
    "VPR_APPACK_CONGESTION_MIN_UTIL", "VPR_APPACK_CONGESTION_GAIN_ALPHA",
    "VPR_APPACK_CONGESTION_CRIT_TH", "VPR_NESTEROV_ARCH_FIELD",
    "VPR_NESTEROV_CROSSDIM_AFFINITY", "VPR_NESTEROV_DENSITY_SCHEDULE",
    "VPR_NESTEROV_DENSITY_RAMP", "VPR_NESTEROV_FD_AUDIT",
    "VPR_NESTEROV_CAPACITY_AUDIT", "VPR_NESTEROV_ITER_TELEM",
    "VPR_NESTEROV_DENSITY_TELEM", "VPR_APPACK_LEGACY_DIST_GROW",
    "VPR_SA_WASH_TRACE", "VPR_DLA_TRACE", "VPR_PLACE_DELAY_CACHE",
]


def run_one(job):
    arm, circuit, width, seed = job
    work = OUT / arm / f"{circuit}_seed{seed}"
    log = work / "vpr.out"
    if log.exists() and "The entire flow of VPR took" in log.read_text(errors="ignore"):
        return job, "cached"
    work.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    for key in CLEARED_ENV:
        env.pop(key, None)
    env.update(NEUTRAL_ENV)
    arm_env, placer_args = ARMS[arm]
    env.update(arm_env)

    # No --route_chan_width: VPR binary-searches the minimum routable width.
    cmd = [
        str(VPR), str(ARCH), str(CIRCUIT_DIR / f"{circuit}.blif"),
        "--device", "auto", "--device_width", str(width),
        "--analytical_place", "--route", "--analysis",
        *placer_args,
        "--seed", str(seed),
    ]
    with log.open("w") as handle:
        rc = subprocess.call(cmd, cwd=work, stdout=handle, stderr=subprocess.STDOUT, env=env)
    return job, f"rc={rc}"


def parse_one(arm, circuit, seed):
    log = OUT / arm / f"{circuit}_seed{seed}" / "vpr.out"
    if not log.exists():
        return None
    text = log.read_text(errors="ignore")
    row = {"arm": arm, "circuit": circuit, "seed": seed}
    for name, pattern in METRICS.items():
        found = re.findall(pattern, text)
        row[name] = found[-1] if found else ""
    row["ok"] = bool(row.get("total_runtime"))
    # The gate must have engaged, or the arm is void -- this is the check that
    # was missing when an earlier sweep silently ran a feature-less binary.
    row["gate_seen"] = (not ARMS[arm][0]) or "APPack congestion map ENABLED" in text
    row["placer"] = "b2b" if arm.startswith("b2b") else "nesterov"
    row["cong"] = "on" if ARMS[arm][0] else "off"
    return row


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    # Interleave arms within each (circuit, seed): arm-major ordering would run
    # every control before any treatment, so no paired comparison exists until the
    # sweep is two-thirds done. This way each completed (circuit, seed) yields a
    # full set of paired arms immediately.
    jobs = [
        (arm, circuit, width, seed)
        for circuit, width in CIRCUITS
        for seed in SEEDS
        for arm in ARMS
    ]
    print(f"{len(jobs)} min-W runs across {len(ARMS)} arms, {MAX_PARALLEL} at a time", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as pool:
        for job, status in pool.map(run_one, jobs):
            done += 1
            print(f"[{done}/{len(jobs)}] {job[0]:<8} {job[1]:<20} seed{job[3]} {status}", flush=True)

    rows = [
        row
        for arm in ARMS
        for circuit, _ in CIRCUITS
        for seed in SEEDS
        if (row := parse_one(arm, circuit, seed)) is not None
    ]
    csv_path = OUT / "results.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {csv_path} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
