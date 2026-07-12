/**
 * @file      flat_helper.hpp
 * @brief     Helper for flattening multi-dimensional optimization data into LP
 * format
 * @date      Wed May 14 22:18:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @class FlatHelper
 * @brief Converts scenario/stage/block indexed data into flat vectors for
 * saving
 *
 * This helper class transforms hierarchical optimization data (organized by
 * scenarios, stages and blocks) into flat vectors suitable for data saving as
 * in parquet files. It handles:
 * - Active element filtering (only processes active scenarios/stages/blocks)
 * - Value projection and scaling during flattening
 * - Generation of validity markers for sparse problems
 * - Multiple index holder types (GSTB, STB, ST, T)
 *
 * Key Features:
 * - Efficient constexpr accessors for active elements
 * - Thread-safe operations (all methods are const)
 * - Support for custom projection and scaling
 * - Detailed error checking during construction
 * - Move semantics for efficient data transfer
 *
 * Typical Usage:
 * 1. Construct with active element indices
 * 2. Call flat() with indexed data holders
 * 3. Use returned vectors in LP matrix construction
 *
 * @note Accessor methods are noexcept (many constexpr); the uid-collector
 * builders (stb_uids/st_uids/t_uids) allocate and may throw std::bad_alloc
 * @see SystemContext which inherits and uses this functionality
 * @see GSTBIndexHolder, STBIndexHolder, STIndexHolder, TIndexHolder
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ranges>
#include <utility>
#include <vector>

#include <gtopt/index_holder.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @class FlatHelper
 * @brief Converts multi-dimensional optimization data into flat vectors for LP
 * formulation
 *
 * This helper class transforms scenario/stage/block indexed data into:
 * - Flat vectors for LP matrix construction
 * - Valid/invalid markers for sparse problems
 * - Scaled values using provided factors
 *
 * Key features:
 * - Handles active element filtering (only processes active
 * scenarios/stages/blocks)
 * - Applies scaling factors during flattening
 * - Supports multiple index holder types (GSTB, STB, ST, T)
 * - Provides constexpr accessors for active elements
 * - Thread-safe operations (all methods are const)
 *
 * @note Accessor methods are noexcept (many constexpr); the uid-collector
 * builders (stb_uids/st_uids/t_uids) allocate and may throw std::bad_alloc
 * @see SystemContext which inherits and uses this functionality
 */

struct STBUids
{
  std::vector<Uid> scenario_uids;
  std::vector<Uid> stage_uids;
  std::vector<Uid> block_uids;

  void reserve(size_t size)
  {
    scenario_uids.reserve(size);
    stage_uids.reserve(size);
    block_uids.reserve(size);
  }
};

struct STUids
{
  std::vector<Uid> scenario_uids;
  std::vector<Uid> stage_uids;

  void reserve(size_t size)
  {
    scenario_uids.reserve(size);
    stage_uids.reserve(size);
  }
};

struct TUids
{
  std::vector<Uid> stage_uids;

  void reserve(size_t size) { stage_uids.reserve(size); }
};

/// One (grid-slot, value) sample produced by `FlatHelper::flat_sparse`:
/// `first` is the flat grid-slot index that the dense `flat()` vector
/// fills for the same holder entry, `second` the projected + scaled
/// value.  Entries are sorted ascending by slot, so consumers emit rows
/// in exactly the dense scan's (scenario, stage, block) order.
using SparseEntry = std::pair<uint32_t, double>;
using SparseEntries = std::vector<SparseEntry>;

class FlatHelper
{
public:
  explicit FlatHelper(const SimulationLP& simulation)
      : m_simulation_(simulation)
      // Active scenarios = (a) the scenario's own ``is_active()`` is true
      // AND (b) the scenario's parent scene is active.  The earlier
      // implementation only checked (a) and silently included scenarios
      // whose parent scene had ``"active": 0`` set, producing
      // column-length mismatches at write time
      // (``Column 3 ... expected length 3570 but got length 8160`` —
      // ratio = N_total_scenarios / N_active_scenarios = 16/7 on
      // juan/gtopt_iplp 2026-05-02 trace_34).  We walk the active
      // scene list and pick up each active scene's scenarios; this
      // produces the expected length consistently with the write
      // path's per-scene iteration.
      , m_active_scenarios_(std::ranges::to<std::vector>(
            simulation.scenes() | std::views::filter(&SceneLP::is_active)
            | std::views::transform(
                [](const SceneLP& scene)
                {
                  return scene.scenarios()
                      | std::views::filter(&ScenarioLP::is_active)
                      | std::views::transform(&ScenarioLP::uid);
                })
            | std::views::join))
      , m_active_stages_(std::ranges::to<std::vector>(
            simulation.stages() | std::views::filter(&StageLP::is_active)
            | std::views::transform(&StageLP::uid)))
      , m_active_stage_blocks_(std::ranges::to<std::vector>(
            simulation.stages() | std::views::filter(&StageLP::is_active)
            | std::views::transform(
                [](const StageLP& stage)
                {
                  return std::ranges::to<std::vector>(
                      stage.blocks() | std::views::transform(&BlockLP::uid));
                })))
      , m_active_blocks_(std::ranges::to<std::vector>(
            simulation.blocks() | std::views::transform(&BlockLP::uid)))
  {
    // Precompute the uid → slot-position maps used by the indexed sparse
    // fill in flat().  Built once per FlatHelper (it lives as long as
    // SystemContext) so each flat() call touches only the holder's own
    // entries instead of scanning the whole scenario × block grid — the
    // per-cell write-out cost drops from O(whole grid) to O(holder size)
    // per element-field stream.
    map_reserve(m_scenario_pos_, m_active_scenarios_.size());
    for (size_t si = 0; si < m_active_scenarios_.size(); ++si) {
      m_scenario_pos_.emplace(m_active_scenarios_[si], si);
    }

    map_reserve(m_stage_slots_, m_active_stages_.size());
    size_t block_base = 0;
    for (size_t ti = 0; ti < m_active_stages_.size(); ++ti) {
      const auto& blocks = m_active_stage_blocks_[ti];
      StageSlot slot {
          .stage_pos = ti,
          .block_base = block_base,
          .block_pos = {},
      };
      map_reserve(slot.block_pos, blocks.size());
      for (size_t bi = 0; bi < blocks.size(); ++bi) {
        slot.block_pos.emplace(blocks[bi], bi);
      }
      block_base += blocks.size();
      m_stage_slots_.emplace(m_active_stages_[ti], std::move(slot));
    }
    m_stage_block_stride_ = block_base;
  }

  [[nodiscard]] constexpr const SimulationLP& simulation() const noexcept
  {
    return m_simulation_.get();
  }

  [[nodiscard]] constexpr const auto& active_scenarios() const noexcept
  {
    return m_active_scenarios_;
  }

  [[nodiscard]] constexpr const auto& active_stages() const noexcept
  {
    return m_active_stages_;
  }

  [[nodiscard]] constexpr const auto& active_stage_blocks() const noexcept
  {
    return m_active_stage_blocks_;
  }

  [[nodiscard]] constexpr const auto& active_blocks() const noexcept
  {
    return m_active_blocks_;
  }

  [[nodiscard]] constexpr bool is_first_scenario(
      const ScenarioUid& scenario_uid) const noexcept
  {
    return !m_active_scenarios_.empty()
        && scenario_uid == m_active_scenarios_.front();
  }

  [[nodiscard]] constexpr size_t active_scenario_count() const noexcept
  {
    return m_active_scenarios_.size();
  }

  [[nodiscard]] constexpr size_t active_stage_count() const noexcept
  {
    return m_active_stages_.size();
  }

  [[nodiscard]] constexpr size_t active_block_count() const noexcept
  {
    return m_active_blocks_.size();
  }

  [[nodiscard]] constexpr bool is_first_stage(
      const StageUid& stage_uid) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_uid == m_active_stages_.front();
  }

  [[nodiscard]] constexpr bool is_first_stage(
      const StageLP& stage) const noexcept
  {
    return is_first_stage(stage.uid());
  }

  [[nodiscard]] constexpr bool is_last_stage(
      const StageUid& stage_uid) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_uid == m_active_stages_.back();
  }

  [[nodiscard]] constexpr auto prev_stage(const StageLP& stage) const noexcept
  {
    return simulation().prev_stage(stage);
  }

  [[nodiscard]] STBUids stb_uids() const;
  [[nodiscard]] STUids st_uids() const;
  [[nodiscard]] TUids t_uids() const;

  /**
   * @brief Sparse long-form counterpart of flat() for GSTB holders.
   *
   * Collects ONLY the holder's own entries as `(grid-slot, value)` pairs
   * sorted ascending by slot.  The slot index is exactly the position
   * the dense flat() vector fills for the same entry
   * (`si * stride + block_base + bi`), so callers can label each sample
   * with the slot's (scenario, stage, block) uids without materialising
   * the dense grid.  This is the write-out hot path: per element-field
   * stream the work is O(H log H) in the holder size H instead of
   * O(active grid).
   *
   * Stray holder keys (inactive scenario/stage uid, unknown uid, block
   * uid filed under the wrong stage, P1 zero-bound-elided blocks) are
   * skipped exactly as the dense fill skips them.  Explicit zero values
   * are KEPT (the long-form writer drops them at emission time, after
   * any post-collection value snapping).
   *
   * @param hstb The GSTB-indexed data holder
   * @param proj Projection function to apply to each value
   * @param factor Optional scaling factors (applied after projection)
   * @return (grid-slot, value) entries, slot-ascending
   */
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat_sparse(
      const GSTBIndexHolder<Value>& hstb,
      Projection proj,
      const Factor& factor = Factor()) const -> SparseEntries
  {
    SparseEntries entries;
    entries.reserve(hstb.size());
    for (const auto& [key, entry] : hstb) {
      const auto& [suid, tuid, buid] = key;
      const auto sit = m_scenario_pos_.find(suid);
      if (sit == m_scenario_pos_.end()) {
        continue;
      }
      const auto tit = m_stage_slots_.find(tuid);
      if (tit == m_stage_slots_.end()) {
        continue;
      }
      const auto& slot = tit->second;
      const auto bit = slot.block_pos.find(buid);
      if (bit == slot.block_pos.end()) {
        continue;
      }

      const auto si = sit->second;
      const auto bi = bit->second;
      const auto idx = (si * m_stage_block_stride_) + slot.block_base + bi;
      const auto value = proj(entry);
      entries.emplace_back(
          static_cast<uint32_t>(idx),
          factor.empty() ? value : value * factor[si][slot.stage_pos][bi]);
    }
    std::ranges::sort(entries, {}, &SparseEntry::first);
    return entries;
  }

  /// Sparse long-form counterpart of flat() for STB holders.  See the
  /// GSTB overload for the contract.  Blocks not present in a stage's
  /// active block list are simply skipped: the P1 zero-bound skip in
  /// `generator_lp.cpp:131` / `waterway_lp.cpp:72` / `demand_lp.cpp:206`
  /// may have elided individual block entries while leaving the outer
  /// (scenario, stage) key populated; elided slots emit no entry, same
  /// as the outer-key-missing case.
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat_sparse(
      const STBIndexHolder<Value>& hstb,
      Projection proj,
      const Factor& factor = Factor()) const -> SparseEntries
  {
    SparseEntries entries;
    entries.reserve(hstb.size());
    for (const auto& [key, inner] : hstb) {
      const auto& [suid, tuid] = key;
      const auto sit = m_scenario_pos_.find(suid);
      if (sit == m_scenario_pos_.end()) {
        continue;
      }
      const auto tit = m_stage_slots_.find(tuid);
      if (tit == m_stage_slots_.end()) {
        continue;
      }
      const auto si = sit->second;
      const auto& slot = tit->second;
      const auto base = (si * m_stage_block_stride_) + slot.block_base;
      for (const auto& [buid, held] : inner) {
        const auto bit = slot.block_pos.find(buid);
        if (bit == slot.block_pos.end()) {
          continue;
        }
        const auto bi = bit->second;
        const auto value = proj(held);
        entries.emplace_back(
            static_cast<uint32_t>(base + bi),
            factor.empty() ? value : value * factor[si][slot.stage_pos][bi]);
      }
    }
    std::ranges::sort(entries, {}, &SparseEntry::first);
    return entries;
  }

  /// flat_sparse() overload that applies an additional
  /// per-(scenario,stage) scale factor on top of the block-level
  /// @p factor — sparse counterpart of the st_scale flat() overload
  /// below (StorageLP daily-cycle dual back-scaling).
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat_sparse(
      const STBIndexHolder<Value>& hstb,
      Projection proj,
      const Factor& factor,
      const STIndexHolder<double>& st_scale) const -> SparseEntries
  {
    SparseEntries entries;
    entries.reserve(hstb.size());
    for (const auto& [key, inner] : hstb) {
      const auto& [suid, tuid] = key;
      const auto sit = m_scenario_pos_.find(suid);
      if (sit == m_scenario_pos_.end()) {
        continue;
      }
      const auto tit = m_stage_slots_.find(tuid);
      if (tit == m_stage_slots_.end()) {
        continue;
      }
      const auto si = sit->second;
      const auto& slot = tit->second;
      const auto base = (si * m_stage_block_stride_) + slot.block_base;

      const auto ss_iter = st_scale.find({suid, tuid});
      const double ss = (ss_iter != st_scale.end()) ? ss_iter->second : 1.0;

      for (const auto& [buid, held] : inner) {
        const auto bit = slot.block_pos.find(buid);
        if (bit == slot.block_pos.end()) {
          continue;
        }
        const auto bi = bit->second;
        const auto value = proj(held);
        entries.emplace_back(static_cast<uint32_t>(base + bi),
                             factor.empty()
                                 ? value * ss
                                 : value * ss * factor[si][slot.stage_pos][bi]);
      }
    }
    std::ranges::sort(entries, {}, &SparseEntry::first);
    return entries;
  }

  /// Sparse long-form counterpart of flat() for ST holders.  Slot index
  /// is `si * active_stage_count + ti`.
  template<typename Projection,
           typename Value = Index,
           typename Factor = scenario_stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat_sparse(
      const STIndexHolder<Value>& hst,
      Projection proj,
      const Factor& factor = Factor()) const -> SparseEntries
  {
    SparseEntries entries;
    entries.reserve(hst.size());
    for (const auto& [key, held] : hst) {
      const auto& [suid, tuid] = key;
      const auto sit = m_scenario_pos_.find(suid);
      if (sit == m_scenario_pos_.end()) {
        continue;
      }
      const auto tit = m_stage_slots_.find(tuid);
      if (tit == m_stage_slots_.end()) {
        continue;
      }
      const auto si = sit->second;
      const auto ti = tit->second.stage_pos;
      const auto idx = (si * m_active_stages_.size()) + ti;
      const auto value = proj(held);
      entries.emplace_back(static_cast<uint32_t>(idx),
                           factor.empty() ? value : value * factor[si][ti]);
    }
    std::ranges::sort(entries, {}, &SparseEntry::first);
    return entries;
  }

  /// Sparse long-form counterpart of flat() for T holders.  Slot index
  /// is the active-stage position.
  template<typename Projection = std::identity,
           typename Value = Index,
           typename Factor = stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat_sparse(const TIndexHolder<Value>& ht,
                                           Projection proj = {},
                                           const Factor& factor = {}) const
      -> SparseEntries
  {
    SparseEntries entries;
    entries.reserve(ht.size());
    for (const auto& [tuid, held] : ht) {
      const auto tit = m_stage_slots_.find(tuid);
      if (tit == m_stage_slots_.end()) {
        continue;
      }
      const auto ti = tit->second.stage_pos;
      const auto value = proj(held);
      entries.emplace_back(static_cast<uint32_t>(ti),
                           factor.empty() ? value : value * factor[ti]);
    }
    std::ranges::sort(entries, {}, &SparseEntry::first);
    return entries;
  }

  /**
   * @brief Flattens GSTB-indexed data into dense vectors with optional
   * scaling
   *
   * Processes a 3D (Scenario/Stage/Block) indexed container into:
   * - Values vector with projected/scaled values
   * - Valid vector marking which indices had data
   *
   * Implemented as flat_sparse() + a dense scatter, preserving the
   * historical dense contract exactly (see scatter_flat).  The write-out
   * path no longer uses this — OutputContext consumes flat_sparse()
   * directly — but the dense form remains for reference/tests and any
   * consumer that wants the full grid.
   *
   * @param hstb The GSTB-indexed data holder
   * @param proj Projection function to apply to each value
   * @param factor Optional scaling factors (applied after projection)
   * @return Pair of (values, valid) vectors
   *
   * @note Complexity: O(H log G) where H is the holder size and G the
   * active grid — only the holder's own entries are visited (indexed
   * sparse fill); the returned vectors still span the full
   * active_scenarios * active_blocks grid.
   * @note If factor is provided, must match active element dimensions
   */
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const GSTBIndexHolder<Value>& hstb,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    if (hstb.empty()) {
      return std::pair {std::vector<double> {}, std::vector<bool> {}};
    }

    return scatter_flat(flat_sparse(hstb, proj, factor),
                        m_active_scenarios_.size() * m_active_blocks_.size(),
                        m_active_scenarios_.size() * m_stage_block_stride_);
  }

  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const STBIndexHolder<Value>& hstb,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    if (hstb.empty()) {
      return std::pair {std::vector<double> {}, std::vector<bool> {}};
    }

    return scatter_flat(flat_sparse(hstb, proj, factor),
                        m_active_scenarios_.size() * m_active_blocks_.size(),
                        m_active_scenarios_.size() * m_stage_block_stride_);
  }

  /// flat() overload that applies an additional per-(scenario,stage) scale
  /// factor on top of the block-level @p factor.  Used by
  /// StorageLP::add_to_output to back-scale volume-balance duals when the
  /// daily-cycle option was active for a stage (scale = 24/stage_dur).
  ///
  /// The @p st_scale lookup is performed once per (scenario,stage) pair,
  /// outside the block loop, so there is no per-block overhead.  When a
  /// (suid,tuid) key is absent from @p st_scale the scale defaults to 1.0
  /// (i.e. no scaling is applied for that stage), making this a safe drop-in
  /// for the plain flat() call when @p st_scale is empty.
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const STBIndexHolder<Value>& hstb,
                                    Projection proj,
                                    const Factor& factor,
                                    const STIndexHolder<double>& st_scale) const
  {
    if (hstb.empty()) {
      return std::pair {std::vector<double> {}, std::vector<bool> {}};
    }

    return scatter_flat(flat_sparse(hstb, proj, factor, st_scale),
                        m_active_scenarios_.size() * m_active_blocks_.size(),
                        m_active_scenarios_.size() * m_stage_block_stride_);
  }

  template<typename Projection,
           typename Value = Index,
           typename Factor = scenario_stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const STIndexHolder<Value>& hst,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    if (hst.empty()) {
      return std::pair {std::vector<double> {}, std::vector<bool> {}};
    }

    const auto size = m_active_scenarios_.size() * m_active_stages_.size();
    return scatter_flat(flat_sparse(hst, proj, factor), size, size);
  }

  template<typename Projection = std::identity,
           typename Value = Index,
           typename Factor = stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const TIndexHolder<Value>& ht,
                                    Projection proj = {},
                                    const Factor& factor = {}) const
  {
    if (ht.empty()) {
      return std::pair {std::vector<double> {}, std::vector<bool> {}};
    }

    const auto size = m_active_stages_.size();
    return scatter_flat(flat_sparse(ht, proj, factor), size, size);
  }

private:
  /// Scatters sparse (slot, value) entries into the historical dense
  /// flat() contract:
  ///  - vectors span @p size grid slots, `0.0` / `false` defaults for
  ///    non-owned slots;
  ///  - `values` elided (empty) iff no holder entry landed on an active
  ///    slot;
  ///  - `valid` elided iff every iterated slot was hit — the historical
  ///    dense loop's running `need_valids |= count != ++idx` check is
  ///    equivalent to comparing the final hit count against the number
  ///    of iterated slots (@p iterated), because once a slot misses the
  ///    running count can never catch up with idx again.
  [[nodiscard]] static auto scatter_flat(const SparseEntries& entries,
                                         size_t size,
                                         size_t iterated)
      -> std::pair<std::vector<double>, std::vector<bool>>
  {
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);
    for (const auto& [idx, value] : entries) {
      values[idx] = value;
      valid[idx] = true;
    }
    const bool need_values = !entries.empty();
    const bool need_valids = entries.size() != iterated;
    return std::pair {need_values ? std::move(values) : std::vector<double> {},
                      need_valids ? std::move(valid) : std::vector<bool> {}};
  }

  /// Precomputed slot info for one active stage, used by the indexed
  /// sparse fill in flat(): position among active stages, offset of the
  /// stage's first block within a scenario's block span, and the
  /// block-uid → in-stage block position map.
  struct StageSlot
  {
    size_t stage_pos {};
    size_t block_base {};
    flat_map<BlockUid, size_t> block_pos;
  };

  std::reference_wrapper<const SimulationLP> m_simulation_;

  std::vector<ScenarioUid> m_active_scenarios_ {};
  std::vector<StageUid> m_active_stages_ {};
  std::vector<std::vector<BlockUid>> m_active_stage_blocks_ {};
  std::vector<BlockUid> m_active_blocks_ {};

  /// uid → position maps for the indexed sparse fill (see constructor).
  flat_map<ScenarioUid, size_t> m_scenario_pos_ {};
  flat_map<StageUid, StageSlot> m_stage_slots_ {};
  /// Total number of blocks iterated per scenario by the historical dense
  /// scan (= Σ active_stage_blocks[ti].size()); also the per-scenario
  /// stride of the flat slot index.
  size_t m_stage_block_stride_ {0};
};

}  // namespace gtopt
