/**
 * @file
 * @brief Definition of the APPack congestion map.
 */

#include "appack_congestion_map.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "PreClusterTimingManager.h"
#include "atom_netlist.h"
#include "device_grid.h"
#include "vtr_log.h"

namespace {

/**
 * @brief Nets with more pins than this are ignored when estimating congestion.
 *
 * A very high fanout net's bounding box covers most of the device, so it adds a
 * near-uniform offset that carries no spatial information while dominating the
 * sum. This is the same reasoning the packer already applies to high-fanout
 * nets when computing gains.
 */
constexpr size_t kMaxNetFanout = 64;

/**
 * @brief Congestion (relative to the mean) at which the levers reach full strength.
 *
 * A tile at twice the average congestion gets the full tightening; beyond that
 * the response saturates rather than growing without bound.
 */
constexpr float kCongestionSaturation = 2.0f;

/**
 * @brief Weight of the pin-demand term relative to the wire-demand (RUDY) term.
 *
 * Pin access is the resource packing actually decides, so it is weighted equally
 * with wire demand rather than as a correction.
 */
constexpr float kPinDemandWeight = 1.0f;

e_appack_congestion_mode mode_from_env() {
    const char* env = std::getenv("VPR_APPACK_CONGESTION");
    if (!env || env[0] == '\0')
        return e_appack_congestion_mode::OFF;
    std::string value(env);
    if (value == "0" || value == "off")
        return e_appack_congestion_mode::OFF;
    if (value == "pinutil")
        return e_appack_congestion_mode::PIN_UTIL;
    if (value == "gain")
        return e_appack_congestion_mode::GAIN;
    if (value == "1" || value == "both")
        return e_appack_congestion_mode::BOTH;
    VTR_LOG_WARN("Ignoring invalid VPR_APPACK_CONGESTION='%s'; expected off|pinutil|gain|both.\n", env);
    return e_appack_congestion_mode::OFF;
}

float env_float(const char* name, float fallback) {
    const char* env = std::getenv(name);
    if (!env || env[0] == '\0')
        return fallback;
    char* end = nullptr;
    float value = std::strtof(env, &end);
    if (end == env || !std::isfinite(value)) {
        VTR_LOG_WARN("Ignoring invalid %s='%s'; expected a number.\n", name, env);
        return fallback;
    }
    return value;
}

} // namespace

void APPackCongestionMap::init(const FlatPlacementInfo& flat_placement_info,
                               const AtomNetlist& atom_netlist,
                               const DeviceGrid& device_grid,
                               const PreClusterTimingManager& pre_cluster_timing_manager) {
    mode_ = mode_from_env();
    if (mode_ == e_appack_congestion_mode::OFF)
        return;
    if (!flat_placement_info.valid) {
        VTR_LOG_WARN("APPack congestion map requested but no flat placement is available; disabling.\n");
        mode_ = e_appack_congestion_mode::OFF;
        return;
    }

    alpha_ = env_float("VPR_APPACK_CONGESTION_ALPHA", alpha_);
    min_pin_util_scale_ = env_float("VPR_APPACK_CONGESTION_MIN_UTIL", min_pin_util_scale_);
    gain_alpha_ = env_float("VPR_APPACK_CONGESTION_GAIN_ALPHA", gain_alpha_);
    criticality_threshold_ = env_float("VPR_APPACK_CONGESTION_CRIT_TH", criticality_threshold_);

    width_ = device_grid.width();
    height_ = device_grid.height();
    if (width_ == 0 || height_ == 0) {
        mode_ = e_appack_congestion_mode::OFF;
        return;
    }
    congestion_.assign(width_ * height_, 0.f);
    criticality_.assign(width_ * height_, 0.f);

    auto tile_of = [&](AtomBlockId blk_id, int& x, int& y) {
        float raw_x = flat_placement_info.blk_x_pos[blk_id];
        float raw_y = flat_placement_info.blk_y_pos[blk_id];
        if (raw_x == FlatPlacementInfo::UNDEFINED_POS || raw_y == FlatPlacementInfo::UNDEFINED_POS)
            return false;
        x = std::clamp(static_cast<int>(raw_x), 0, static_cast<int>(width_) - 1);
        y = std::clamp(static_cast<int>(raw_y), 0, static_cast<int>(height_) - 1);
        return true;
    };

    // Wire-demand term (RUDY): spread each net's half-perimeter uniformly over
    // the bounding box of its atoms.
    std::vector<float> wire_demand(width_ * height_, 0.f);
    for (AtomNetId net_id : atom_netlist.nets()) {
        auto net_pins = atom_netlist.net_pins(net_id);
        if (net_pins.size() < 2 || net_pins.size() > kMaxNetFanout)
            continue;

        int min_x = static_cast<int>(width_), max_x = -1;
        int min_y = static_cast<int>(height_), max_y = -1;
        for (AtomPinId pin_id : net_pins) {
            int x, y;
            if (!tile_of(atom_netlist.pin_block(pin_id), x, y))
                continue;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
        if (max_x < 0 || max_y < 0)
            continue;

        float box_width = static_cast<float>(max_x - min_x + 1);
        float box_height = static_cast<float>(max_y - min_y + 1);
        // Half-perimeter wire estimate spread over the box area: the density a
        // uniform router would see if this net's wire were smeared over its
        // bounding box.
        float density = (box_width + box_height) / (box_width * box_height);
        for (int y = min_y; y <= max_y; y++) {
            for (int x = min_x; x <= max_x; x++)
                wire_demand[y * width_ + x] += density;
        }
    }

    // Pin-demand term, and the criticality guard, both from atom positions.
    std::vector<float> pin_demand(width_ * height_, 0.f);
    bool timing_valid = pre_cluster_timing_manager.is_valid();
    for (AtomBlockId blk_id : atom_netlist.blocks()) {
        int x, y;
        if (!tile_of(blk_id, x, y))
            continue;
        int idx = y * width_ + x;
        pin_demand[idx] += static_cast<float>(atom_netlist.block_pins(blk_id).size());

        if (!timing_valid)
            continue;
        // A tile is as critical as the most critical net touching an atom in it.
        float block_criticality = 0.f;
        for (AtomPinId pin_id : atom_netlist.block_pins(blk_id)) {
            AtomNetId net_id = atom_netlist.pin_net(pin_id);
            if (!net_id.is_valid())
                continue;
            block_criticality = std::max(block_criticality,
                                         pre_cluster_timing_manager.calc_net_setup_criticality(net_id, atom_netlist));
        }
        criticality_[idx] = std::max(criticality_[idx], block_criticality);
    }

    // Normalize each term by its mean over occupied tiles, then combine. Using
    // the mean over *occupied* tiles (rather than all tiles) keeps the scale
    // meaningful on designs that fill a small fraction of the device.
    auto normalize = [](std::vector<float>& values) {
        double sum = 0.;
        size_t occupied = 0;
        for (float value : values) {
            if (value > 0.f) {
                sum += value;
                occupied++;
            }
        }
        if (occupied == 0)
            return;
        float mean = static_cast<float>(sum / occupied);
        if (mean <= 0.f)
            return;
        for (float& value : values)
            value /= mean;
    };
    normalize(wire_demand);
    normalize(pin_demand);

    for (size_t idx = 0; idx < congestion_.size(); idx++) {
        congestion_[idx] = (wire_demand[idx] + kPinDemandWeight * pin_demand[idx])
                           / (1.f + kPinDemandWeight);
    }

    log_summary();
}

int APPackCongestionMap::tile_index(int x, int y) const {
    if (x < 0 || y < 0 || static_cast<size_t>(x) >= width_ || static_cast<size_t>(y) >= height_)
        return -1;
    return y * static_cast<int>(width_) + x;
}

float APPackCongestionMap::get_congestion(int x, int y) const {
    int idx = tile_index(x, y);
    if (idx < 0 || congestion_.empty())
        return 0.f;
    return congestion_[idx];
}

float APPackCongestionMap::get_criticality(int x, int y) const {
    int idx = tile_index(x, y);
    if (idx < 0 || criticality_.empty())
        return 0.f;
    return criticality_[idx];
}

float APPackCongestionMap::congestion_pressure(int x, int y) const {
    int idx = tile_index(x, y);
    if (idx < 0 || congestion_.empty())
        return 0.f;
    // Timing guard: never trade timing for routability where the critical path
    // lives. Spreading critical logic is how the equivalent "looser packing"
    // experiment destroyed timing-limited circuits.
    if (criticality_[idx] > criticality_threshold_)
        return 0.f;
    float excess = congestion_[idx] - 1.f;
    if (excess <= 0.f)
        return 0.f;
    return std::min(1.f, excess / (kCongestionSaturation - 1.f));
}

float APPackCongestionMap::get_ext_pin_util_scale(int x, int y) const {
    if (mode_ != e_appack_congestion_mode::PIN_UTIL && mode_ != e_appack_congestion_mode::BOTH)
        return 1.f;
    float scale = 1.f - alpha_ * congestion_pressure(x, y);
    return std::clamp(scale, min_pin_util_scale_, 1.f);
}

float APPackCongestionMap::get_gain_attenuation(int x, int y) const {
    if (mode_ != e_appack_congestion_mode::GAIN && mode_ != e_appack_congestion_mode::BOTH)
        return 1.f;
    float attenuation = 1.f - gain_alpha_ * congestion_pressure(x, y);
    return std::clamp(attenuation, 0.f, 1.f);
}

void APPackCongestionMap::log_summary() const {
    if (congestion_.empty())
        return;
    float max_congestion = 0.f;
    size_t occupied = 0;
    size_t above_average = 0;
    size_t tightened = 0;
    size_t critical_exempt = 0;
    for (size_t idx = 0; idx < congestion_.size(); idx++) {
        if (congestion_[idx] <= 0.f)
            continue;
        occupied++;
        max_congestion = std::max(max_congestion, congestion_[idx]);
        if (congestion_[idx] > 1.f) {
            above_average++;
            if (criticality_[idx] > criticality_threshold_)
                critical_exempt++;
            else
                tightened++;
        }
    }
    const char* mode_name = mode_ == e_appack_congestion_mode::PIN_UTIL ? "pinutil"
                            : mode_ == e_appack_congestion_mode::GAIN   ? "gain"
                            : mode_ == e_appack_congestion_mode::BOTH   ? "both"
                                                                        : "off";
    VTR_LOG("APPack congestion map ENABLED (mode=%s alpha=%g min_util=%g gain_alpha=%g crit_th=%g).\n",
            mode_name, alpha_, min_pin_util_scale_, gain_alpha_, criticality_threshold_);
    VTR_LOG("  occupied tiles=%zu peak congestion=%.3f above-average=%zu (tightened=%zu, critical-exempt=%zu).\n",
            occupied, max_congestion, above_average, tightened, critical_exempt);
}
