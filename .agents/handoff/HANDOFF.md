# Packer work — handoff

Date: 2026-07-30. Branch `packer-backup`, staged but **not committed**.

Cluster access ended mid-experiment; both sweeps were still running. Everything
below is derived from the 255 completed runs captured in `all_runs.csv`, which is
the durable copy — the per-run `vpr.out` files under `.agents/results/` were not
committed and may be gone.

## What is staged on this branch

Four packer changes, all default-off or behaviour-preserving:

| change | files |
|---|---|
| **Congestion-aware packing** (new) | `appack_congestion_map.{h,cpp}`, hooks in `appack_context.h`, `cluster_legalizer.{h,cpp}`, `greedy_clusterer.cpp`, `greedy_candidate_selector.cpp` |
| **APPack distance cap** | `appack_max_dist_th_manager.{h,cpp}`, `full_legalizer.{h,cpp}`, `pack.{h,cpp}` |
| **Capped unrelated-clustering radius** | `appack_unrelated_clustering_manager.h` |
| **Packer lookahead optimization** | `cluster_legalizer.cpp` |

Non-packer edits were surgically removed and preserved in
`.agents/patches/nonpacker_removed_20260730.patch` (`.gitignore`, the Nesterov
parse regex in `qor_ap_fixed_chan_width.txt`, delay-model caching in
`PlacementDelayModelCreator.cpp`, the `vpr/vpr` symlink retarget).

## Congestion-aware packing — status: promising, NOT committable

It fills a real structural hole: `t_ext_pin_util_targets` (the packer's only
routability lever) is looked up **by block-type name**, is spatially uniform, and
is only ever *loosened*, and only after a global device-fit failure. This makes
it positional, driven by an FPGA-adapted RUDY (wire demand + **pin demand**, the
term that actually binds at the pack/place boundary) with a criticality guard.

**Gentle default (`alpha=0.2`, `min_util=0.6`, `crit_th=0.8`), n=15/placer:**

| placer | W_min | routed CPD | #CLB | runs with W_min unchanged |
|---|---:|---:|---:|---:|
| nesterov | −1.41% | −0.36% | +0.47% | 9/15 |
| b2b | −0.73% | +1.82% | +0.51% | 8/15 |

**Strong setting (`alpha=0.7`, `min_util=0.4`, `crit_th=0.95`), n=6/placer:**

| placer | W_min | #CLB | routed CPD |
|---|---:|---:|---:|
| nesterov | −1.57% (4 moved, **4 improved**) | **+16.6%** | +1.09% |
| b2b | −1.35% (2 moved, **2 improved**) | **+15.0%** | −3.11% |

**The key result:** at the strong setting W_min moved in **6 of 12** paired runs
and **every single move improved it** — zero regressions. The mechanism has real
authority over routability. The engagement counters confirm the gentle default
was simply *not engaged*: `tightened` rose 287 → 516–594 tiles and
`crit_exempt` collapsed 309 → 25–116 once `crit_th` went 0.8 → 0.95, i.e. the
criticality guard was suppressing roughly half the lever at the shipped default.

**Why it is not committable:** the strong setting buys W_min with **+15–17%
cluster count** (up to +36% on `softmax`, which gets *zero* W_min benefit because
it is floor-pinned at W=120 in every arm). Nobody has shown a setting that buys
W_min at acceptable area.

**Statistical honesty:** packing is seed-independent (`--seed` only drives SA and
the router), so the 12 strong-setting runs are ~6 independent circuit×placer
units, 3 of which moved. Sign test p≈0.25. Directional, not significant.
Per-seed CPD variation within an identical packing (e.g. `softmax`/nesterov:
−4.1%, −3.2%, **+9.5%** with byte-identical clusters) is pure downstream noise
and must not be read as a packing effect.

### Next steps, with a kill condition

1. **Find the knee** — sweep `alpha` ∈ {0.35, 0.5} at `crit_th` ≈ 0.9.
   **If no setting delivers a W_min reduction at under ~2% area cost, delete the
   feature.** That is a handful of runs, not another 108.
2. **Add an area guard** — nothing currently caps cluster inflation, so a
   circuit with no routability headroom can pay +36% area for nothing. This is a
   design gap, not tuning.
3. **Drop `softmax`** from the evaluation set — floor-pinned, can only add cost.
4. Grade at **W_min**, never at fixed `route_chan_width 300`: peak channel
   utilization there is 0.35–0.87, so routing never binds and a routability
   lever cannot show anything. Min-W costs only ~1.5–2x, not "several times" —
   routing is just 4–8% of total runtime on these circuits.

## Nesterov exact-gradient result (context; already committed elsewhere)

The density force was **not** the gradient of the density objective: it
central-differenced the potential on the grid and interpolated that, instead of
taking the derivative of the bilinear interpolant the mass deposition implies.
Measured error 0.56 worst / 0.12 mean relative on abundant logic, with sign
flips. The corrected form is round-off-limited (1.3e-07 at step 1e-2, growing as
1/step — the cancellation signature, not truncation).

**End-to-end it is NULL:** routed CPD −0.60% at n=22, **12w/10l**, inside the
3.7%-median seed noise. Driven by two opposing outliers (`attention_layer`
−13.8%, `dnnweaver` +23.1% on one seed). The aggregate wandered
−3.75% → +0.61% → −1.52% → −0.60% as circuits landed, which is itself the
signature of noise rather than effect. **Keep the fix for correctness — the
optimizer now follows the true gradient — not for QoR.**

Residual known imprecision, measured and judged not worth fixing: DC removal
subtracts the mean over *active* sites, and a scarce block sliding off a
capacity-free tile flips that tile's activity, so the charge is not a fixed
linear map of occupancy. A full-tile sweep bounds the resulting discontinuity at
**1.2e-07 relative**, and the activation window is ~1e-9 of a tile wide.

## The thing that should shape future work

Every GP-side lever graded in this session came back null on routed CPD:
architecture-aware field, cross-dimension affinity, overflow-aware restart, and
the exact gradient. The reason is measurable:

- arm-vs-base ratios correlate with the routed outcome at **+0.277 at post-FL**
  but **+0.955 at post-DP** — SA's output is nearly determined by SA itself, and
  the input mainly selects which basin.
- post-FL CPD over-predicts routed CPD by **1.09–2.05×, circuit-dependent**;
  post-DP is 0.99×. So a large part of the apparent "SA improvement" is the
  estimator becoming unbiased, roughly `1 − 1/bias`, not work SA performed.
- **Do not grade GP changes on post-FL CPD.** Use post-DP and routed.

Consequence: GP cannot win by optimizing harder against the same objective SA
re-optimizes. The levers that survive are ones SA *cannot undo* — SA relocates
clusters among legal sites but cannot re-cluster. That is why capped APPack is
the one large real win in this project's record, and why the packing stage is
the right place to spend effort.

**Untested idea worth keeping:** the old buggy field was effectively a *smoothed*
(wider-support) force. Smoothing early and exact late is a principled
multigrid-style continuation rather than an accidental bug, and it changes global
structure rather than refining local positions — so it is not obviously subject
to the SA-equalizes ceiling.

**Cheap diagnostic never run:** within a single circuit, generate ~30 perturbed
placements and measure **Spearman rank correlation** between the GP objective and
final routed CPD. Cross-circuit correlation conflates scale error with ordering
error. If within-circuit rank correlation is near zero, no amount of gradient or
force work in GP can help and only route-in-the-loop calibration will.

## Files

| file | contents |
|---|---|
| `all_runs.csv` | 255 completed runs, all four sweeps, all metrics |
| `SUMMARY_NUMBERS.txt` | the aggregates above, regenerable from the CSV |
| `../patches/nonpacker_removed_20260730.patch` | non-packer edits removed from this branch |
| `../results/appack_congestion_20260729/` | congestion design notes + sweep drivers |
| `../results/nesterov_field_schedule_20260728/` | earlier GP feature notes |

Sweeps were incomplete at cutoff: exact-field 66/81 (missing `dnnweaver` seeds
2–3 and all of `dla_like.small`), congestion 60/108 (missing `bnn`,
`clstm_like.small`, `dnnweaver`, `dla_like.small`). The missing circuits are the
large, timing-limited ones — i.e. the discriminating ones — so all aggregates
above are biased toward small circuits.
