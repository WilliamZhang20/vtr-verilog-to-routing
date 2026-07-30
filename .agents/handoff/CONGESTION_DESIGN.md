# Congestion-aware packing (FPGA RUDY) — implementation

Date: 2026-07-29
Status: **code landed, default-off, smoke-tested. Not graded.**

Implements the design from
`.agents/plans/congestion_aware_packing_analysis.md`. Read that first — it
explains why this is expected to be a b2b-favouring absolute-QoR change rather
than a nesterov-vs-b2b differentiator, and why it cannot be graded on the
`route_chan_width 300` harness.

## The hole this fills

APPack is distance-aware but not congestion-aware, and every existing lever is a
function of **logical block type**, never of position on the die:

| Mechanism | Location | Granularity |
|---|---|---|
| Gain attenuation | `greedy_candidate_selector.cpp` (`get_molecule_gain`) | per candidate distance |
| Max distance threshold | `appack_max_dist_th_manager.cpp` | per block type |
| Unrelated-clustering radius | `appack_unrelated_clustering_manager.*` | per block type |
| **External pin utilization** | `t_ext_pin_util_targets` (`vpr_types.h`) | **per block type**, only ever loosened, only after a global device-fit failure |

The last row is the packer's actual routability lever. Making it positional is
the point of this change.

## What was built

New `vpr/src/pack/appack_congestion_map.{h,cpp}` — `APPackCongestionMap`:

- **Wire demand (RUDY)**: each atom net spreads its half-perimeter uniformly
  over the bounding box of its atoms' flat-placement positions. Nets above 64
  pins are skipped — their bounding box covers the device, so they add a
  near-uniform offset carrying no spatial information.
- **Pin demand**: each atom's pin count deposited at its tile. This is the
  FPGA-specific term. Pin access, not channel area, is what packing actually
  decides, and it is what connects the map to `t_ext_pin_util_targets`.
- **Criticality**: per tile, the max pre-cluster setup criticality of any net
  touching an atom there. Used as a guard, see below.
- Both demand terms are normalized by their mean over *occupied* tiles, so 1.0
  is an average tile, then averaged together.

Wired into `APPackContext` (built once, in `try_pack`), consumed at two points:

1. **Positional pin utilization** (the real lever). `LegalizationCluster` gained
   an `ext_pin_util_scale`, with `ClusterLegalizer::set_cluster_ext_pin_util_scale()`.
   `GreedyClusterer` sets it from the congestion map once the cluster's flat
   position is known (after `create_cluster_gain_stats`). A congested tile packs
   its clusters less densely → more, emptier clusters → lower pin demand there.
2. **Congestion-scaled gain attenuation** in `get_molecule_gain`: clusters in
   congested regions become pickier about pulling in distant molecules.

## The criticality guard, and why the smoke justifies it

The analysis predicted that positional pin-utilization tightening is the same
physical mechanism as the "looser packing" arm of 2026-07-22, which dominated on
laggards but **destroyed timing winners** (`attention_layer` +30.7%,
`lstm` +23.5% CPD) because it spreads critical logic — and that it would only
avoid that if congested and critical regions are disjoint.

The smoke says they are **not**. On `lenet`:

```
APPack congestion map ENABLED (mode=both alpha=0.2 min_util=0.6 gain_alpha=0.3 crit_th=0.8).
  occupied tiles=1268 peak congestion=2.268 above-average=579 (tightened=292, critical-exempt=287)
```

**287 of 579 above-average tiles (49.6%) are timing-critical.** Without the
guard, half of all tightening would have landed directly on critical logic —
which is precisely how the earlier experiment failed. The guard is not optional
defensive coding; it is load-bearing.

## Environment gates (all default off/neutral)

| Variable | Default | Meaning |
|---|---|---|
| `VPR_APPACK_CONGESTION` | `off` | `off` \| `pinutil` \| `gain` \| `both` |
| `VPR_APPACK_CONGESTION_ALPHA` | `0.2` | Strength of pin-utilization tightening |
| `VPR_APPACK_CONGESTION_MIN_UTIL` | `0.6` | Floor on the pin-utilization multiplier |
| `VPR_APPACK_CONGESTION_GAIN_ALPHA` | `0.3` | Strength of gain attenuation |
| `VPR_APPACK_CONGESTION_CRIT_TH` | `0.8` | Above this local criticality, both levers are off |

## Inertness

With `VPR_APPACK_CONGESTION` unset, `lenet` seed 1 produces post-FL CPD
**13.2877 ns** and **1241** CLBs on the new binary — identical to a run of the
same circuit on the pre-change binary. No congestion log line is emitted,
`is_valid()` is false, all three hooks are skipped, and `ext_pin_util_scale`
stays at 1.0 so the multiply in `add_mol_to_cluster` is skipped too.

## Single-circuit smoke (NOT a result)

`lenet`, seed 1, `mode=both`: post-FL CPD 13.2877 → 12.3469 ns, CLBs 1241 → 1239.
One circuit, one seed, on the smallest design in the suite, against a 6.1%
median seed-to-seed spread. It shows the mechanism is live and does not break
packing. It is not evidence of a gain.

## Planned grading run — `wmin_sweep.py` (queued, not yet launched)

Phase 1: arms `off` / `pinutil` / `both`, 9 circuits x 3 seeds = 81 runs,
`nonlinear-nesterov` only, **at minimum channel width** (no
`--route_chan_width`, so VPR binary-searches W_min). Headline metric is
`min_chan_width`; routed WL secondary; CPD a guard, not the headline.

Phase 2, only if phase 1 shows signal: add an `lp-b2b` arm and report the
nesterov/b2b ratio move explicitly.

Every arm records `gate_seen` — the check that was missing when an earlier
sweep silently ran a binary without the feature under test and returned a table
of exact 1.0000 ratios.

Min-W routing is far slower than fixed-W (a binary search over channel widths),
so this is an overnight-scale sweep, and it is queued behind the density-gradient
fix and the arch-field re-test.

## How this must be graded

From the analysis, unchanged:

1. **At or near W_min, not `route_chan_width 300`.** Peak channel utilization on
   the existing suite is 0.35–0.87 — routing never binds, so a routability lever
   cannot show a gain there.
2. **>= 3 seeds**, given the noise floor.
3. **Primary metrics are dW_min and routed WL**; CPD is a guard, not the
   headline. If this works, the win shape is lower minimum channel width and
   wirelength with CPD roughly flat.
4. **Run both `nonlinear-nesterov` and `lp-b2b`** and report the ratio change
   explicitly — it is expected to move toward b2b, since nesterov's raw GP is
   already more spread and so has less congestion to relieve.

## Known limitations

- Normalization is *relative* (per-map mean), not absolute against routing
  channel capacity. For VTR's typically uniform `chan_width_dist` this is a
  spatially constant factor and cannot change any decision, but an absolute
  normalization would be needed to reason about W_min directly.
- The map is built once from the initial flat placement and never refreshed —
  it does not react to the clusters being formed.
- `APPackMaxDistThManager` was left per-block-type; only the two levers above
  are positional.
