/**
 * @file      output_context.hpp
 * @brief     Output context for writing LP results to Parquet/CSV
 * @date      Mon Mar 24 20:39:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the OutputContext class and the probe_parquet_codec
 * utility for writing LP solution results to Parquet or CSV files.
 */

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

/// Probe the Arrow/Parquet runtime to determine the best available codec for
/// the requested name.  Uses `arrow::util::Codec::IsAvailable()` — the
/// correct runtime check — rather than `parquet::IsCodecSupported()`, which
/// only validates the enum value and does **not** detect codecs that were
/// absent when the Arrow library was compiled.
///
/// Falls back (with a WARN log) to `"gzip"`, then to `""` (uncompressed) when
/// the requested codec is unavailable.
///
/// **Call once at program startup** (e.g. in `gtopt_main()` after loading
/// options) and store the result in `planning.options.output_compression` so
/// that every downstream write uses the same pre-validated codec without
/// re-probing on each file.
[[nodiscard]] std::string probe_parquet_codec(std::string_view requested);

namespace detail
{
/// True when @p T is a per-block inner map (the `mapped_type` of an
/// STB-style holder's outer map): it exposes `mapped_type` and is not
/// itself the scalar index value.  Used by `OutputContext::first_index_of`
/// to decide whether the outer map's value is a nested block map (STB) or
/// the index value directly (ST / T / GSTB).
template<typename T, typename = void>
inline constexpr bool is_nested_block_map_v = false;
template<typename T>
inline constexpr bool is_nested_block_map_v<
    T,
    std::void_t<typename std::remove_cvref_t<T>::mapped_type,
                decltype(std::declval<std::remove_cvref_t<T>>().begin())>> =
    true;

/// The LP-index value type stored in an index holder
/// (`ColIndex` / `RowIndex`).  For STB-style nested holders this is the
/// inner block map's `mapped_type`; for ST / T / GSTB it is the outer
/// map's `mapped_type` directly.
template<typename IndexHolder, typename = void>
struct holder_index_value
{
  using type = typename std::remove_cvref_t<IndexHolder>::mapped_type;
};
template<typename IndexHolder>
struct holder_index_value<
    IndexHolder,
    std::void_t<typename std::remove_cvref_t<
        typename std::remove_cvref_t<IndexHolder>::mapped_type>::mapped_type>>
{
  using type = typename std::remove_cvref_t<
      typename std::remove_cvref_t<IndexHolder>::mapped_type>::mapped_type;
};
template<typename IndexHolder>
using holder_index_value_t = typename holder_index_value<IndexHolder>::type;
}  // namespace detail

class OutputContext
{
public:
  /// Row-label lookup for the long-form output emission: the grid-slot
  /// index of a sparse field entry indexes these vectors to recover the
  /// row's (scenario, stage, block) uids.  One instance per holder
  /// layout (STB / ST / T) is built at construction from the FlatHelper
  /// uid grids; ST leaves `block_uids` empty and T leaves
  /// `scenario_uids` + `block_uids` empty — absent axes emit 0 in the
  /// long table, matching the historical writer.
  struct PreludeUids
  {
    std::vector<Uid> scenario_uids;
    std::vector<Uid> stage_uids;  ///< always populated; defines row count
    std::vector<Uid> block_uids;
  };

  /// One element's output stream under a `(class, field, suffix)` key:
  /// sparse `(grid-slot, value)` entries ascending by slot — exactly
  /// the holder's own samples, no dense-grid intermediate (the former
  /// wide layout scanned scenarios × blocks slots per stream at write
  /// time).  Explicit zero values are kept here and dropped at write
  /// time (the long form omits zero rows), which preserves the
  /// post-collection integer-snapping hook (`add_col_sol_integer`).
  struct FieldEntry
  {
    Name name;  ///< per-element field label (`uid:42` / `<name>:42`)
    SparseEntries entries;
    const PreludeUids* prelude;
  };

  using FieldVector = std::vector<FieldEntry>;

  /// Map key for `field_vector_map`: (cname, fname, sname).
  /// All three are static `string_view` literals (class_name / field-name
  /// constants / fixed suffix tag), so this is a zero-allocation key —
  /// replaces the former `pair<string_view, Name>` whose `Name` (std::string)
  /// was freshly heap-allocated via `as_label(fname, sname)` on every call.
  using ClassFieldName = std::array<std::string_view, 3>;
  struct ClassFieldNameHash
  {
    [[nodiscard]] std::size_t operator()(const ClassFieldName& k) const noexcept
    {
      // Mix three string_view hashes with golden-ratio constants.  Keys
      // are short (≤ 2 dozen chars each) and the population is tiny
      // (a few dozen per OutputContext) so a plain combine is fine.
      constexpr std::uint64_t kMix = 0x9e3779b97f4a7c15ULL;
      const std::uint64_t h0 = std::hash<std::string_view> {}(k[0]);
      const std::uint64_t h1 = std::hash<std::string_view> {}(k[1]);
      const std::uint64_t h2 = std::hash<std::string_view> {}(k[2]);
      std::uint64_t h = h0;
      h ^= h1 + kMix + (h << 6U) + (h >> 2U);
      h ^= h2 + kMix + (h << 6U) + (h >> 2U);
      return static_cast<std::size_t>(h);
    }
  };
  using FieldVectorMap =
      std::unordered_map<ClassFieldName, FieldVector, ClassFieldNameHash>;

  explicit OutputContext(const SystemContext& psc,
                         LinearInterface& linear_interface,
                         SceneUid scene_uid = make_uid<Scene>(0),
                         PhaseUid phase_uid = make_uid<Phase>(0),
                         bool is_continuous_phase = false);

  [[nodiscard]] auto&& options() const noexcept { return sc.get().options(); }

  /// Borrow the owning `SystemContext` so output sites can reach the
  /// persistent registries (e.g. `simulation()`, `system()`) that survive
  /// per-cell LP rebuilds under `low_memory = compress`.  Used by
  /// `FutureCostLP::add_to_output` to SELF-FIND its α columns + rebase offset
  /// at write time rather than relying on a per-cell stash.
  [[nodiscard]] const SystemContext& system_context() const noexcept
  {
    return sc.get();
  }

  /// Physical primal value at the given LP column.  Equivalent to
  /// `linear_interface().get_col_sol()[col]` but reads from the
  /// already-cached `col_sol_span` so callers in `add_to_output`
  /// don't need to re-fetch the LP-solution view.  Used by the
  /// reconstruction sites (e.g. the planned P0 demand-failure
  /// rewrite) that need to read a surviving variable's primal value
  /// in order to compute the substituted variable's output.
  [[nodiscard]] constexpr double primal(ColIndex col) const noexcept
  {
    return col_sol_span[col];
  }

  /// Physical LP reduced cost (in `$/[unit]`) at the given LP column.
  /// Counterpart to :func:`primal` that reads the already-cached
  /// `col_cost_span`.  Used by `LineLP::add_to_output` (and any other
  /// reconstruction site) that combines per-direction LP columns into
  /// a single output stem and needs the rc of each piece to choose a
  /// signed combined rc.
  [[nodiscard]] constexpr double cost(ColIndex col) const noexcept
  {
    return col_cost_span[col];
  }

  /// True when the solve published reduced costs.  False under the
  /// dual-recovery bail (MIP primal preserved, duals unavailable —
  /// see `fix_mip_and_resolve_duals`): reconstruction sites that read
  /// `cost()` per column MUST check this and skip their `:cost`
  /// streams instead of indexing the empty span (production SIGABRT,
  /// 2026-07-13, span:302 hardening assert).
  [[nodiscard]] constexpr bool has_col_costs() const noexcept
  {
    return !col_cost_span.empty();
  }

  /// Row-dual counterpart of `has_col_costs()`.
  [[nodiscard]] constexpr bool has_row_duals() const noexcept
  {
    return !row_dual_span.empty();
  }

  // ── Value-emit overloads (precomputed `(s,t,b) → double`) ────────
  //
  // Counterpart to the index-emit overloads below.  These take a
  // holder whose values are already physical doubles (rather than
  // LP column / row indices) and emit them directly, applying the
  // same per-block factor pipeline as the index variant so the CSV
  // output is bit-for-bit compatible with the LP-emitted form.
  //
  // Use case: model rewrites that fold an LP variable away via
  // substitution (e.g. the planned P0 demand-failure rewrite
  // `fail = lmax − load`).  The substituted variable no longer
  // exists as an LP column, so its `:sol` / `:cost` outputs must be
  // reconstructed from observable quantities (parameters + the
  // surviving variable's primal/dual values) and emitted directly.
  // The new `add_to_output` site builds an
  // `STBIndexHolder<double>` of reconstructed values and hands it
  // to these methods — same semantics, no LP column lookup.

  constexpr void add_col_sol_values(std::string_view cname,
                                    std::string_view col_name,
                                    const Id& id,
                                    const STBIndexHolder<double>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field_values(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     &stb_prelude,
                     block_factor_matrix_t {});
  }

  /// Extras-gated variant of `add_col_sol_values`.  Same on-disk
  /// shape (`<col_name>_sol.parquet`) and same encoding; gated on
  /// `OutputFlags::extras` instead of `solution` so the caller can
  /// keep the stream around for opt-in audits without bloating the
  /// default footprint.  Used by `GeneratorLP::add_to_output` for
  /// the per-block VOM / fuel cost decomposition (no current consumer
  /// reads these — `srmc_sol` already covers the marginal-cost use
  /// case).
  constexpr void add_col_sol_values_extras(std::string_view cname,
                                           std::string_view col_name,
                                           const Id& id,
                                           const STBIndexHolder<double>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field_values(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     &stb_prelude,
                     block_factor_matrix_t {});
  }

  constexpr void add_col_cost_values(std::string_view cname,
                                     std::string_view col_name,
                                     const Id& id,
                                     const STBIndexHolder<double>& holder)
  {
    if (!emit_reduced_cost(cname)) {
      return;
    }
    // Mirrors the `:cost` factor pipeline of `add_col_cost`: applies
    // `block_icost_factors` so the emitted scalar lands on the same
    // physical-cost scale that an LP-derived reduced cost would.
    // Callers that wish to emit values already on physical scale
    // (no further multiplication) should construct the holder so
    // each entry is `physical_value × block_ecost_factor[s,t,b]`.
    add_field_values(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     &stb_prelude,
                     sc.get().block_icost_factors());
  }

  /// Extras-gated variant of `add_col_cost_values`.  Same on-disk
  /// shape (`<col_name>_cost.parquet`) and same factor pipeline;
  /// gated on `OutputFlags::extras` instead of `reduced_cost` so the
  /// caller can keep the stream around for opt-in audits without
  /// bloating the default footprint.  Used by `LineLP::add_to_output`
  /// for the inactive-direction reduced cost ``flown_cost`` that
  /// complements the default-emitted signed combined ``flow_cost``.
  constexpr void add_col_cost_values_extras(
      std::string_view cname,
      std::string_view col_name,
      const Id& id,
      const STBIndexHolder<double>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field_values(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     &stb_prelude,
                     sc.get().block_icost_factors());
  }

  constexpr void add_row_dual_values(std::string_view cname,
                                     std::string_view row_name,
                                     const Id& id,
                                     const STBIndexHolder<double>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    add_field_values(cname,
                     row_name,
                     "dual",
                     id,
                     holder,
                     &stb_prelude,
                     sc.get().block_icost_factors());
  }

  /// Read-only view of the accumulated field map, keyed by
  /// `(class_name, field_name, suffix)`.  Each entry is a `FieldVector`
  /// of sparse `FieldEntry` streams (see above).  Exposed for regression
  /// tests that need to assert on the schema (column presence, naming)
  /// or the collected values without going end-to-end through the
  /// Parquet writer.  Not part of the production code path — `write`
  /// consumes the map directly.
  [[nodiscard]] constexpr const auto& fields() const noexcept
  {
    return field_vector_map;
  }

  // ── STB/GSTB block-indexed overloads ─────────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const GSTBIndexHolder<ColIndex>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
  }

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const STBIndexHolder<ColIndex>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
  }

  /// Integer-snapping variant of ``add_col_sol`` for binary / integer
  /// LP columns (commitment ``status`` / ``startup`` / ``shutdown``).
  /// The MIP solver returns these within a small feasibility tolerance
  /// (~1e-6) of 0 or 1, but in practice the float32 grid + per-column
  /// decimal rounding leaves a sub-percent tail of fractional reports
  /// (verified 2026-05-29 on CEN PCP MIP K=4: 1,657 / 103,523
  /// status_sol entries were fractional, e.g. 0.058, 0.629, despite
  /// CPLEX reporting gap = 0 %).  Snap each value to ``int(round(v))``
  /// after the standard ``add_field`` collection so the output parquet
  /// carries clean 0/1 integers, matching the LP variable's declared
  /// type.  Calling site for ``SimpleCommitmentLP`` / ``CommitmentLP``.
  ///
  /// LP-relax gate: when the phase is solved as a continuous LP (via
  /// ``model_options.continuous_phases`` or per-phase ``continuous:
  /// true``), the binary variable is genuinely fractional and the
  /// fractional reading is the meaningful output — snapping would
  /// corrupt it.  The dual-extraction pass also re-solves
  /// LP-relaxed, but with the binaries fixed at their MIP 0/1
  /// values, so col_sol is already integer-valued there and snapping
  /// would be a no-op; we still skip it to keep the policy "snap iff
  /// the solver was asked to enforce integrality on this phase".
  constexpr void add_col_sol_integer(std::string_view cname,
                                     std::string_view col_name,
                                     const Id& id,
                                     const STBIndexHolder<ColIndex>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
    if (m_is_continuous_phase_) {
      return;
    }
    // Post-process: snap the just-appended entry's values to the
    // nearest integer.  ``add_field`` returns early on empty
    // holder / span, in which case ``field_vector_map`` may be
    // unchanged — guard via ``find`` + ``back()`` existence.  Values
    // snapped to exactly 0 are dropped by the long-form writer at
    // emission time, same as any other explicit zero.
    const ClassFieldName key {cname, col_name, "sol"};
    auto it = field_vector_map.find(key);
    if (it == field_vector_map.end() || it->second.empty()) {
      return;
    }
    auto& field = it->second.back();
    for (auto& entry : field.entries) {
      entry.second = std::round(entry.second);
    }
  }

  /// Extras-gated variant of `add_col_sol(..., STBIndexHolder<ColIndex>)`.
  /// File name stays `<col_name>_sol.parquet`; only the gate moves to
  /// `OutputFlags::extras`.  Used by LineLP for piecewise-segment
  /// slices, line losses, and overload slacks — none of which any
  /// current consumer reads.
  constexpr void add_col_sol_extras(std::string_view cname,
                                    std::string_view col_name,
                                    const Id& id,
                                    const STBIndexHolder<ColIndex>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
  }

  /// Sum-of-cols solution overload: writes Σ col_sol[col] for each
  /// block.  Used by `piecewise_direct` line-loss mode to emit
  /// `Line.flowp:sol` / `Line.flown:sol` as the per-block segment-sum
  /// even though no aggregator LP column was created.  The output
  /// scalar is the same value the AMPL resolver computes for
  /// `line.flowp` via the multi-col registration (see
  /// `AmplVariable::block_cols_sum` and `LineLP::add_to_lp`), so
  /// downstream consumers — solution.csv, the `line.flow` derivation,
  /// PAMPL user-constraints — see a consistent flow scalar across LP /
  /// AMPL / output regardless of whether the line is aggregator-mode or
  /// direct-mode.
  constexpr void add_col_sol(
      std::string_view cname,
      std::string_view col_name,
      const Id& id,
      const STBIndexHolder<std::vector<ColIndex>>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field_sum(cname,
                  col_name,
                  "sol",
                  id,
                  holder,
                  col_sol_span,
                  &stb_prelude,
                  block_factor_matrix_t {});
  }

  /// Extras-gated variant of
  /// `add_col_sol(..., STBIndexHolder<std::vector<ColIndex>>)`.
  /// Used by `LineLP` to publish the consolidated
  /// ``Line/loss_sol.parquet`` from a merged ``lossp ∪ lossn`` holder:
  /// per-(line, scenario, stage, block) value is
  /// ``Σ col_sol[col_k]`` over whichever direction-specific loss
  /// columns were populated on that cell (at most one of A→B / B→A
  /// has loss at any block; merging gives one schema-stable file
  /// instead of paired direction parquets).  Empty-skip semantics
  /// mirror the non-extras overload.
  constexpr void add_col_sol_extras(
      std::string_view cname,
      std::string_view col_name,
      const Id& id,
      const STBIndexHolder<std::vector<ColIndex>>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field_sum(cname,
                  col_name,
                  "sol",
                  id,
                  holder,
                  col_sol_span,
                  &stb_prelude,
                  block_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const GSTBIndexHolder<ColIndex>& holder)
  {
    if (!emit_reduced_cost(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &stb_prelude,
              block_factor_for(col_scale_type_of(holder)));
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const STBIndexHolder<ColIndex>& holder)
  {
    if (!emit_reduced_cost(cname)) {
      return;
    }
    // The inverse cost-factor family is chosen per element from the
    // column's stored `cost_scale_type`: a STOCK column (e.g. storage
    // energy state, Energy) reads back $/stored-unit WITHOUT the per-block
    // 1/duration term that a power column ($/MWh, Power default) carries.
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &stb_prelude,
              block_factor_for(col_scale_type_of(holder)));
  }

  /// Extras-gated variant of `add_col_cost(..., STBIndexHolder<ColIndex>)`.
  /// Used by GeneratorLP for heat-rate slack reduced costs and by
  /// LineLP for overload-slack reduced costs.  Both streams are
  /// retained for future audit use but read by no current consumer.
  constexpr void add_col_cost_extras(std::string_view cname,
                                     std::string_view col_name,
                                     const Id& id,
                                     const STBIndexHolder<ColIndex>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const GSTBIndexHolder<RowIndex>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              block_factor_for(row_scale_type_of(holder)));
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STBIndexHolder<RowIndex>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    // The inverse cost-factor family is chosen per element from the row's
    // stored `cost_scale_type`: stock / commodity duals (storage energy
    // balance → water value $/CMD, fuel-offtake $/fuel-unit; Energy) are
    // duration-independent and read back WITHOUT the 1/duration term that
    // per-block power duals (LMP, Power default) need.
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              block_factor_for(row_scale_type_of(holder)));
  }

  /// Extras-gated variant of `add_row_dual(..., STBIndexHolder<RowIndex>)`.
  /// Used by GeneratorLP for the capacity-row dual.  The dual is the
  /// shadow price on the per-block `gen <= pmax` constraint —
  /// informative for capacity-expansion studies, but unused by the
  /// dispatch / marginal-unit pipelines.
  constexpr void add_row_dual_extras(std::string_view cname,
                                     std::string_view row_name,
                                     const Id& id,
                                     const STBIndexHolder<RowIndex>& holder)
  {
    if (!emit_extras(cname)) {
      return;
    }
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  /// add_row_dual with an additional per-(scenario,stage) back-scale factor.
  /// Used by StorageLP::add_to_output for daily-cycle dual correction.
  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STBIndexHolder<RowIndex>& holder,
                              const STIndexHolder<double>& st_scale)
  {
    if (!emit_dual(cname)) {
      return;
    }
    // Per-element inverse cost-factor family (Power / Energy / Raw) selected
    // from the row's stored `cost_scale_type`, composed with the extra
    // per-(scenario,stage) back-scale `st_scale` (daily-cycle correction).
    add_field_st_scaled(cname,
                        row_name,
                        "dual",
                        id,
                        holder,
                        row_dual_span,
                        st_scale,
                        block_factor_for(row_scale_type_of(holder)));
  }

  /// add_row_dual using discount-only scaling (`scale_obj / discount[t]`).
  constexpr void add_row_dual_raw(std::string_view cname,
                                  std::string_view row_name,
                                  const Id& id,
                                  const STBIndexHolder<RowIndex>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              sc.get().block_discount_icost_factors());
  }

  // ── ST scenario-stage-indexed overloads ──────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const STIndexHolder<ColIndex>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &st_prelude,
              scenario_stage_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const STIndexHolder<ColIndex>& holder)
  {
    if (!emit_reduced_cost(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &st_prelude,
              ss_factor_for(col_scale_type_of(holder)));
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STIndexHolder<RowIndex>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    // Per-element inverse cost-factor family from the row's stored
    // `cost_scale_type`: a per-stage STOCK / commodity dual ($/stored-unit,
    // $/fuel-unit, $/tonne; Energy) gets the duration-free back-scale,
    // unlike per-stage power/energy-flow duals (Power default).
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &st_prelude,
              ss_factor_for(row_scale_type_of(holder)));
  }

  // ── T stage-indexed overloads ────────────────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const TIndexHolder<ColIndex>& holder)
  {
    if (!emit_solution(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &t_prelude,
              stage_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const TIndexHolder<ColIndex>& holder)
  {
    if (!emit_reduced_cost(cname)) {
      return;
    }
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &t_prelude,
              stage_factor_for(col_scale_type_of(holder)));
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const TIndexHolder<RowIndex>& holder)
  {
    if (!emit_dual(cname)) {
      return;
    }
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &t_prelude,
              stage_factor_for(row_scale_type_of(holder)));
  }

  /// Which output fields were requested for this context.
  [[nodiscard]] auto output_flags() const noexcept -> OutputFlags
  {
    return m_output_selection_.atoms;
  }
  [[nodiscard]] auto output_selection() const noexcept -> const OutputSelection&
  {
    return m_output_selection_;
  }

  // Per-(atom, cname) gating.  The `cname` is the element-class
  // string each `*LP::add_to_output` passes as the first argument to
  // `add_col_sol` / `add_col_cost` / `add_row_dual` — i.e. the same
  // top-level directory name the parquet output uses (`Generator`,
  // `Bus`, `Line`, …).  When the user supplies a per-atom class
  // allow-list (e.g. `--write-out rc:Generator,Line`), only those
  // cnames pass the gate.  When no scope is configured for an atom,
  // every cname passes.

  [[nodiscard]] auto emit_solution(std::string_view cname) const noexcept
      -> bool
  {
    return m_output_selection_.emits(OutputFlags::solution, cname);
  }
  [[nodiscard]] auto emit_dual(std::string_view cname) const noexcept -> bool
  {
    return m_output_selection_.emits(OutputFlags::dual, cname);
  }
  [[nodiscard]] auto emit_reduced_cost(std::string_view cname) const noexcept
      -> bool
  {
    return m_output_selection_.emits(OutputFlags::reduced_cost, cname);
  }
  /// Opt-in gate for streams that no current consumer reads (heat-rate
  /// slacks, per-block cost decomposition, line piecewise / loss /
  /// overload slack columns, capacity row duals).  `*LP::add_to_output`
  /// routes those calls through `add_*_extras` overloads which check
  /// this gate instead of the default sol/dual/rc gate.
  [[nodiscard]] auto emit_extras(std::string_view cname) const noexcept -> bool
  {
    return m_output_selection_.emits(OutputFlags::extras, cname);
  }

  void write() const;

private:
  std::reference_wrapper<const SystemContext> sc;

  SceneUid m_scene_uid_;
  PhaseUid m_phase_uid_;
  bool m_is_continuous_phase_ {false};

  OutputSelection m_output_selection_ {OutputFlags::all};

  ScaledView col_sol_span;
  ScaledView col_cost_span;
  ScaledView row_dual_span;

  /// Per-column / per-row objective time-basis (Power / Energy / Raw),
  /// borrowed from the LinearInterface (frozen after flatten).  Drives the
  /// per-element inverse cost-factor selection in `cost_factor_for_col` /
  /// `dual_factor_for_row`.  Out-of-range (post-flatten) indices default to
  /// `Power` via `LinearInterface::*_cost_scale_type_at`-equivalent logic
  /// inlined in those helpers.
  std::span<const ConstraintScaleType> col_cost_scale_types;
  std::span<const ConstraintScaleType> row_cost_scale_types;

  PreludeUids stb_prelude;
  PreludeUids st_prelude;
  PreludeUids t_prelude;

  FieldVectorMap field_vector_map;

  // ── cost-factor selection by per-element time-basis ──────────────
  //
  // Within a single `add_*` call the holder addresses exactly one
  // element instance, so every column / row it references shares the
  // same `cost_scale_type` (set at the element's `add_to_lp` site).
  // We therefore read the time-basis of the holder's first stored
  // index once and pick the inverse cost-factor family accordingly,
  // rather than branching per cell.  Selection (readback is the
  // inverse of the objective fold, per element):
  //   Power  → block_icost_factors            (÷ prob·disc·duration)
  //   Energy → block_icost_factors_no_duration(÷ prob·disc)
  //   Raw    → empty matrix                    (÷ 1; only scale_objective,
  //                                             applied in get_col_cost)

  /// Representative `cost_scale_type` of the column referenced by an
  /// index holder.  Reads the first stored ColIndex; defaults to
  /// `Power` for empty holders or out-of-range (post-flatten) indices.
  template<typename IndexHolder>
  [[nodiscard]] ConstraintScaleType col_scale_type_of(
      const IndexHolder& holder) const noexcept
  {
    const auto col = first_index_of(holder);
    if (!col) {
      return ConstraintScaleType::Power;
    }
    const auto i = static_cast<size_t>(*col);
    return i < col_cost_scale_types.size() ? col_cost_scale_types[i]
                                           : ConstraintScaleType::Power;
  }

  /// Representative `cost_scale_type` of the row referenced by an index
  /// holder.  See `col_scale_type_of`.
  template<typename IndexHolder>
  [[nodiscard]] ConstraintScaleType row_scale_type_of(
      const IndexHolder& holder) const noexcept
  {
    const auto row = first_index_of(holder);
    if (!row) {
      return ConstraintScaleType::Power;
    }
    const auto i = static_cast<size_t>(*row);
    return i < row_cost_scale_types.size() ? row_cost_scale_types[i]
                                           : ConstraintScaleType::Power;
  }

  /// Block-level inverse cost-factor matrix for a per-block (STB / GSTB)
  /// readback, chosen by the element's @p type.  Power → with-duration,
  /// Energy → duration-free, Raw → empty (no per-cell descale).
  [[nodiscard]] const block_factor_matrix_t& block_factor_for(
      ConstraintScaleType type) const
  {
    static const block_factor_matrix_t kEmpty {};
    switch (type) {
      case ConstraintScaleType::Energy:
        return sc.get().block_icost_factors_no_duration();
      case ConstraintScaleType::Raw:
        return kEmpty;
      case ConstraintScaleType::Power:
        break;
    }
    return sc.get().block_icost_factors();
  }

  /// Per-(scenario, stage) inverse cost-factor matrix for an ST readback,
  /// chosen by the element's @p type.  See `block_factor_for`.
  [[nodiscard]] const scenario_stage_factor_matrix_t& ss_factor_for(
      ConstraintScaleType type) const
  {
    static const scenario_stage_factor_matrix_t kEmpty {};
    switch (type) {
      case ConstraintScaleType::Energy:
        return sc.get().scenario_stage_icost_factors_no_duration();
      case ConstraintScaleType::Raw:
        return kEmpty;
      case ConstraintScaleType::Power:
        break;
    }
    return sc.get().scenario_stage_icost_factors();
  }

  /// Per-stage inverse cost-factor matrix for a T readback, chosen by the
  /// element's @p type.  See `block_factor_for`.
  [[nodiscard]] const stage_factor_matrix_t& stage_factor_for(
      ConstraintScaleType type) const
  {
    static const stage_factor_matrix_t kEmpty {};
    switch (type) {
      case ConstraintScaleType::Energy:
        return sc.get().stage_icost_factors_no_duration();
      case ConstraintScaleType::Raw:
        return kEmpty;
      case ConstraintScaleType::Power:
        break;
    }
    return sc.get().stage_icost_factors();
  }

  /// Extract the first stored index value from any STB / ST / T / GSTB
  /// index holder (nested or flat map).  Returns `std::nullopt` when the
  /// holder is empty or the (nested) inner map has no entries.  The inner
  /// value type is the LP column / row index (`ColIndex` / `RowIndex`).
  template<typename IndexHolder>
  [[nodiscard]] static constexpr auto first_index_of(
      const IndexHolder& holder) noexcept
      -> std::optional<typename detail::holder_index_value_t<IndexHolder>>
  {
    using Value = typename detail::holder_index_value_t<IndexHolder>;
    if (holder.empty()) {
      return std::nullopt;
    }
    const auto& front = holder.begin()->second;
    if constexpr (detail::is_nested_block_map_v<decltype(front)>) {
      // STB-style: outer (scenario,stage) → inner block → index.
      if (front.empty()) {
        return std::nullopt;
      }
      return Value {front.begin()->second};
    } else {
      // ST / T / GSTB-style: value is the index directly.
      return Value {front};
    }
  }

  // ── private helpers ──────────────────────────────────────────────

  [[nodiscard]] constexpr auto field_name(const Id& id) const
  {
    return options().use_uid_fname() ? as_label<':'>("uid", get_uid(id))
                                     : as_label<':'>(get_name(id), get_uid(id));
  }

  /// Collects one element's field stream as sparse long-form entries.
  /// `FlatHelper::flat_sparse` visits only the holder's own samples and
  /// returns `(grid-slot, value)` pairs in the dense scan's slot order,
  /// so the written long tables are bit-identical to the former
  /// dense-intermediate pipeline while skipping the O(active grid)
  /// fill + rescan per stream (the per-cell write-out hot path).
  template<typename IndexHolder,
           typename Span,
           typename Factor = std::span<double>>
  void add_field(std::string_view cname,
                 std::string_view fname,
                 std::string_view sname,
                 const Id& id,
                 const IndexHolder& holder,
                 const Span& value_span,
                 const PreludeUids* prelude,
                 const Factor& factor)
  {
    if (holder.empty() || value_span.empty()) {
      return;
    }

    auto entries = sc.get().flat_sparse(
        holder, [&](auto i) { return value_span[i]; }, factor);

    if (entries.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, fname, sname}].emplace_back(
        field_name(id), std::move(entries), prelude);
  }

  /// add_field variant that **emits precomputed values directly** —
  /// no LP-side index lookup.  `IndexHolder` is expected to be
  /// `STBIndexHolder<double>` whose per-block values are already in
  /// the desired physical scale.  The same `factor` pipeline as
  /// `add_field` is applied (block-level scale on each value) so the
  /// CSV output is byte-compatible with the LP-emitted form when
  /// the caller has prepared values that match the LP would have
  /// produced.  Backs the `add_col_sol_values` / `add_col_cost_values`
  /// / `add_row_dual_values` public overloads, used by model rewrites
  /// that fold an LP variable away via algebraic substitution (e.g.
  /// the planned P0 demand-failure `fail = lmax − load`).
  template<typename Factor = std::span<double>>
  void add_field_values(std::string_view cname,
                        std::string_view fname,
                        std::string_view sname,
                        const Id& id,
                        const STBIndexHolder<double>& holder,
                        const PreludeUids* prelude,
                        const Factor& factor)
  {
    if (holder.empty()) {
      return;
    }

    // Identity projection: the holder already carries doubles, so
    // pass each one through unchanged into the standard sparse
    // pipeline.  `factor` still applies post-projection — matching
    // the index-based `add_field` so block scaling stays uniform.
    auto entries =
        sc.get().flat_sparse(holder, [](double v) { return v; }, factor);

    if (entries.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, fname, sname}].emplace_back(
        field_name(id), std::move(entries), prelude);
  }

  /// add_field variant for **sum-of-cols** holders.  `IndexHolder` is
  /// expected to be `STBIndexHolder<std::vector<ColIndex>>` (a per-
  /// block list of cols).  Each block's projection is
  /// `Σ value_span[c]` over the cols in the list, then optionally
  /// multiplied by the per-block factor like the single-col variant.
  /// Used to emit `flowp` / `flown` solution and reduced-cost columns
  /// for `piecewise_direct` line-loss mode where no aggregator LP col
  /// exists.
  template<typename IndexHolder,
           typename Span,
           typename Factor = std::span<double>>
  void add_field_sum(std::string_view cname,
                     std::string_view fname,
                     std::string_view sname,
                     const Id& id,
                     const IndexHolder& holder,
                     const Span& value_span,
                     const PreludeUids* prelude,
                     const Factor& factor)
  {
    if (holder.empty() || value_span.empty()) {
      return;
    }

    auto sum_proj = [&](const auto& cols)
    {
      double s = 0.0;
      for (const auto& c : cols) {
        s += value_span[c];
      }
      return s;
    };
    auto entries = sc.get().flat_sparse(holder, std::move(sum_proj), factor);

    if (entries.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, fname, sname}].emplace_back(
        field_name(id), std::move(entries), prelude);
  }

  /// add_field variant with additional per-(scenario,stage) back-scale.
  /// @p factor is the per-element-selected block-level inverse cost-factor
  /// matrix (Power → with-duration, Energy → duration-free, Raw → empty),
  /// composed with the per-(scenario,stage) @p st_scale.
  template<typename IndexHolder, typename Span>
  void add_field_st_scaled(std::string_view cname,
                           std::string_view fname,
                           std::string_view sname,
                           const Id& id,
                           const IndexHolder& holder,
                           const Span& value_span,
                           const STIndexHolder<double>& st_scale,
                           const block_factor_matrix_t& factor)
  {
    if (holder.empty() || value_span.empty()) {
      return;
    }

    auto entries = sc.get().flat_sparse(
        holder, [&](auto i) { return value_span[i]; }, factor, st_scale);

    if (entries.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, fname, sname}].emplace_back(
        field_name(id), std::move(entries), &stb_prelude);
  }
};

}  // namespace gtopt
