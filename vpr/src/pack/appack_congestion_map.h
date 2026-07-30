#pragma once
/**
 * @file
 * @brief Declaration of a congestion estimate built from the AP flat placement
 *        and consumed by APPack during packing.
 *
 * Motivation. APPack is distance-aware but not congestion-aware: the gain
 * attenuation, the max-distance thresholds and the unrelated-clustering radius
 * are all functions of the *logical block type*, never of where on the die a
 * cluster is being built. The packer's actual routability lever --
 * `t_ext_pin_util_targets`, the external pin utilization a cluster may reach
 * before it is closed -- is likewise looked up by block-type name, is only ever
 * loosened, and only in response to a global device-fit failure.
 *
 * Packing is the one stage where a routability decision is irreversible: once
 * atoms share a cluster, that tile's pin demand is fixed and no amount of
 * detailed placement or routing can undo it. This map makes the pin-utilization
 * target and the candidate gain vary with a per-tile congestion estimate.
 *
 * Everything here is inert unless VPR_APPACK_CONGESTION selects a mode.
 */

#include <vector>
#include "flat_placement_types.h"
#include "physical_types.h"

class AtomNetlist;
class DeviceGrid;
class PreClusterTimingManager;

/**
 * @brief Which congestion-driven levers are active.
 */
enum class e_appack_congestion_mode {
    OFF,      ///< No congestion awareness (default).
    PIN_UTIL, ///< Scale the per-cluster external pin utilization target.
    GAIN,     ///< Attenuate candidate gain in congested regions.
    BOTH      ///< Both of the above.
};

/**
 * @brief Per-tile congestion and criticality estimated from the flat placement.
 *
 * The congestion estimate is an FPGA-adapted RUDY (Rectangular Uniform wire
 * DensitY): each net spreads its estimated wire demand uniformly over the
 * bounding box of its atoms' flat-placement positions. Two FPGA-specific
 * additions matter:
 *
 *  - A **pin-demand** term. On FPGAs the resource that actually binds at the
 *    pack/place boundary is very often cluster input-pin access rather than
 *    channel area, and pin demand is precisely what packing decides. This term
 *    is what connects the map to `t_ext_pin_util_targets`.
 *  - A **criticality** map. Tightening pin utilization in space is the same
 *    physical mechanism as looser packing, which is known to spread critical
 *    logic and destroy timing-limited circuits. Congested and timing-critical
 *    regions are usually not disjoint, so the lever is suppressed wherever the
 *    local criticality is high.
 *
 * Normalization is *relative*: both terms are divided by their mean over
 * occupied tiles, so a value of 1.0 is an average tile. An absolute
 * normalization by routing-channel capacity would be needed to compare against
 * a minimum-channel-width study, but VTR channel-width distributions are
 * usually uniform, in which case dividing by them is a spatially constant
 * factor and cannot change any of the decisions below.
 */
class APPackCongestionMap {
  public:
    /**
     * @brief Build the map. Does nothing unless a mode is selected and the flat
     *        placement is valid.
     */
    void init(const FlatPlacementInfo& flat_placement_info,
              const AtomNetlist& atom_netlist,
              const DeviceGrid& device_grid,
              const PreClusterTimingManager& pre_cluster_timing_manager);

    /// @brief True when a mode is active and the map was built.
    inline bool is_valid() const { return mode_ != e_appack_congestion_mode::OFF && !congestion_.empty(); }

    /// @brief The selected mode (OFF when this feature is disabled).
    inline e_appack_congestion_mode mode() const { return mode_; }

    /**
     * @brief Relative congestion at a tile. 1.0 is an average occupied tile.
     */
    float get_congestion(int x, int y) const;

    /**
     * @brief Highest pre-cluster setup criticality of any atom in a tile.
     */
    float get_criticality(int x, int y) const;

    /**
     * @brief Multiplier for a cluster's external pin utilization target.
     *
     * Returns 1.0 (no change) for average-or-better tiles, for timing-critical
     * tiles, and whenever the PIN_UTIL lever is not selected. Otherwise returns
     * a value in [min_pin_util_scale, 1.0): a congested tile packs its clusters
     * less densely, leaving more, emptier clusters and lower pin demand there.
     */
    float get_ext_pin_util_scale(int x, int y) const;

    /**
     * @brief Multiplier applied to a candidate molecule's gain.
     *
     * Returns 1.0 unless the GAIN lever is selected and the tile is congested
     * and non-critical, in which case clusters there become pickier about
     * accepting distant molecules.
     */
    float get_gain_attenuation(int x, int y) const;

    /// @brief Log a one-line summary of the built map.
    void log_summary() const;

  private:
    /// @brief Flatten a tile coordinate, or return -1 when out of range.
    int tile_index(int x, int y) const;

    /**
     * @brief Shared shape of both levers: how far above average this tile is,
     *        gated to zero on timing-critical tiles.
     *
     * @return 0.0 for an average-or-better or critical tile, up to 1.0 for a
     *         tile at or beyond `kCongestionSaturation` times the average.
     */
    float congestion_pressure(int x, int y) const;

    e_appack_congestion_mode mode_ = e_appack_congestion_mode::OFF;
    size_t width_ = 0;
    size_t height_ = 0;
    /// @brief [tile] relative congestion; 1.0 is an average occupied tile.
    std::vector<float> congestion_;
    /// @brief [tile] max pre-cluster setup criticality of the atoms placed there.
    std::vector<float> criticality_;

    // Tunables, all overridable by environment variable so an experiment does
    // not need a rebuild.
    float alpha_ = 0.2f;                 ///< Strength of the pin-utilization tightening.
    float min_pin_util_scale_ = 0.6f;    ///< Floor on the pin-utilization multiplier.
    float gain_alpha_ = 0.3f;            ///< Strength of the gain attenuation.
    float criticality_threshold_ = 0.8f; ///< Above this local criticality, both levers are off.
};
