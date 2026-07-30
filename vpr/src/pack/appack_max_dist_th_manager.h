#pragma once
/**
 * @file
 * @author  Alex Singer
 * @date    May 2025
 * @brief   Declaration of a class that manages the max candidate distance
 *          thresholding optimization used within APPack.
 */

#include <algorithm>
#include <string>
#include <vector>
#include "physical_types.h"
#include "vtr_assert.h"

// Forward declarations.
class DeviceGrid;

/**
 * @brief Manager class which manages parsing and getting the max candidate
 *        distance thresholds for each of the logical block types in the
 *        architecture.
 *
 * The initializer of this class will set the max candidate distance thresholds
 * based on the arguments passed by the user.
 *
 * Within the packer, the get_max_dist_threshold method can be used to get the
 * max distance threshold for a given logical block type.
 */
class APPackMaxDistThManager {
    // To compute the max distance threshold, we use two numbers to compute it:
    //  - Scale: This is what fraction of the device the distance should be.
    //           i.e. the max distance threshold will be scale * (W + H) since
    //           W+H is the farthes L1 distance possible on the device.
    //  - Offset: This is the minimum threshold it can have. This prevents small
    //            devices with small scales having thresholds that are too small.
    // The following scales and offsets are set for interesting logical blocks
    // when the "auto" selection mode is used. The following numbers were
    // empirically found to work well.

    // This is the default scale and offset. Logical blocks that we do not
    // recognize as being of the special categories will have this threshold.
    static constexpr float default_max_dist_th_scale_ = 0.15f;
    static constexpr float default_max_dist_th_offset_ = 10.0f;

    // Logic blocks (such as CLBs and LABs) tend to have more resources on the
    // device, thus they have tighter thresholds. This was found to work well.
    static constexpr float logic_block_max_dist_th_scale_ = 0.02f;
    static constexpr float logic_block_max_dist_th_offset_ = 10.0f;

    // Memory blocks (i.e. blocks that contain pb_types of the memory class)
    // seem to have very touchy packing; thus these do not have the max
    // threshold to prevent them from creating too many clusters.
    static constexpr float memory_max_dist_th_scale_ = 1.0f;
    static constexpr float memory_max_dist_th_offset_ = 0.0f;

    // IO blocks tend to have very sparse resources and setting the offset too
    // low can create too many blocks. Set this to a higher value.
    static constexpr float io_max_dist_th_scale_ = 0.5f;
    static constexpr float io_max_dist_th_offset_ = 15.0f;

  public:
    // When packing fails, it may try to increase the max distance threshold to
    // resolve the failure. This scales the max distance threshold of the failing
    // logical block type. Historically this was 10x (often exploding to full-chip
    // after one failure). Default is gentler; cap vs the *initial* auto threshold
    // preserves flat-placement fidelity. Override both with
    // VPR_APPACK_LEGACY_DIST_GROW=1 (scale 10, uncapped).
    static constexpr float max_dist_th_fail_increase_scale_default_ = 2.0f;
    static constexpr float max_dist_th_vs_initial_cap_default_ = 3.0f;

  public:
    APPackMaxDistThManager() = default;

    /**
     * @brief Initializer for the manager class. The thresholds for each logical
     *        block type is selected here.
     *
     *  @param max_dist_ths
     *      An array of strings representing the user-defined max distance
     *      thresholds. This is passed from the command line.
     *  @param logical_block_types
     *      An array of all logical block types in the architecture.
     *  @param device_grid
     */
    void init(const std::vector<std::string>& max_dist_ths,
              const std::vector<t_logical_block_type>& logical_block_types,
              const DeviceGrid& device_grid);

    /**
     * @brief Get the max distance threshold of the given logical block type.
     */
    inline float get_max_dist_threshold(const t_logical_block_type& logical_block_ty) const {
        VTR_ASSERT_SAFE_MSG(is_initialized_,
                            "APPackMaxDistThManager has not been initialized, cannot call this method");
        VTR_ASSERT_SAFE_MSG((size_t)logical_block_ty.index < logical_block_dist_thresholds_.size(),
                            "Logical block type does not have a max distance threshold");

        return logical_block_dist_thresholds_[logical_block_ty.index];
    }

    /**
     * @brief Get the maximum distance possible on the device. This is the
     *        manhattan distance from the bottom-left corner of the device to
     *        the top-right.
     */
    inline float get_max_device_distance() const {
        VTR_ASSERT_SAFE_MSG(is_initialized_,
                            "APPackMaxDistThManager has not been initialized, cannot call this method");

        return max_distance_on_device_;
    }

    /**
     * @brief Set the max distance threshold of the given logical block type.
     */
    inline void set_max_dist_threshold(const t_logical_block_type& logical_block_ty,
                                       float new_threshold) {
        VTR_ASSERT_SAFE_MSG(is_initialized_,
                            "APPackMaxDistThManager has not been initialized, cannot call this method");
        VTR_ASSERT_SAFE_MSG((size_t)logical_block_ty.index < logical_block_dist_thresholds_.size(),
                            "Logical block type does not have a max distance threshold");

        logical_block_dist_thresholds_[logical_block_ty.index] = new_threshold;
    }

    /// @brief Initial (auto/user) threshold before pack-failure growth.
    inline float get_initial_max_dist_threshold(const t_logical_block_type& logical_block_ty) const {
        VTR_ASSERT_SAFE_MSG(is_initialized_,
                            "APPackMaxDistThManager has not been initialized, cannot call this method");
        VTR_ASSERT_SAFE_MSG((size_t)logical_block_ty.index < initial_logical_block_dist_thresholds_.size(),
                            "Logical block type does not have an initial max distance threshold");
        return initial_logical_block_dist_thresholds_[logical_block_ty.index];
    }

    /// @brief Upper bound for failure-driven growth of this type's threshold.
    inline float get_max_allowed_dist_threshold(const t_logical_block_type& logical_block_ty) const {
        const float initial = get_initial_max_dist_threshold(logical_block_ty);
        const float vs_initial = initial * max_dist_th_vs_initial_cap_;
        return std::min(max_distance_on_device_, vs_initial);
    }

    /// @brief True if failure-driven growth can still raise this type's threshold.
    inline bool can_increase_max_dist_threshold(const t_logical_block_type& logical_block_ty) const {
        return get_max_dist_threshold(logical_block_ty) + 1e-3f
               < get_max_allowed_dist_threshold(logical_block_ty);
    }

    /**
     * @brief Grow the max distance threshold after a pack failure, respecting the
     *        per-type cap vs the initial threshold.
     * @return true if the threshold was increased.
     */
    bool try_increase_max_dist_threshold(const t_logical_block_type& logical_block_ty);

    float max_dist_th_fail_increase_scale() const { return max_dist_th_fail_increase_scale_; }

    /// @brief True when VPR_APPACK_LEGACY_DIST_GROW=1 (uncapped grow + full-chip UC).
    bool legacy_dist_grow() const { return legacy_dist_grow_; }

    /**
     * @brief Max unrelated-clustering search radius that still respects GP fidelity.
     *
     * Matches the pack-fail max-distance cap (or full device in legacy mode).
     * High-effort unrelated clustering must not search farther than this —
     * otherwise it bypasses the candidate-distance cap and washes GP.
     */
    inline float get_max_allowed_unrelated_tile_dist(const t_logical_block_type& logical_block_ty) const {
        if (legacy_dist_grow_) {
            return get_max_device_distance();
        }
        return get_max_allowed_dist_threshold(logical_block_ty);
    }

  private:
    /**
     * @brief Helper method that initializes the thresholds of all logical
     *        block types to reasonable numbers based on the characteristics
     *        of the logical block type.
     */
    void auto_set_max_distance_thresholds(const std::vector<t_logical_block_type>& logical_block_types,
                                          const DeviceGrid& device_grid);

    /**
     * @brief Helper method that sets the thresholds based on the user-provided
     *        strings.
     */
    void set_max_distance_thresholds_from_strings(const std::vector<std::string>& max_dist_ths,
                                                  const std::vector<t_logical_block_type>& logical_block_types);

    /// @brief A flag which shows if the thresholds have been computed or not.
    bool is_initialized_ = false;

    /// @brief The max distance thresholds of all logical blocks in the architecture.
    ///        This is initialized in the constructor and accessed during packing.
    std::vector<float> logical_block_dist_thresholds_;

    /// @brief Snapshot of thresholds after init (before pack-failure growth).
    std::vector<float> initial_logical_block_dist_thresholds_;

    /// @brief This is the maximum manhattan distance possible on the device. This
    ///        is the distance of traveling from the bottom-left corner of the device
    ///        to the top right.
    float max_distance_on_device_ = 0.f;

    float max_dist_th_fail_increase_scale_ = max_dist_th_fail_increase_scale_default_;
    float max_dist_th_vs_initial_cap_ = max_dist_th_vs_initial_cap_default_;
    bool legacy_dist_grow_ = false;
};
