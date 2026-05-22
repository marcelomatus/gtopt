/**
 * @file      reservoir_lp.hpp
 * @brief     Defines the ReservoirLP class for linear programming
 * representation
 * @date      Wed Jul 30 23:15:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReservoirLP class which provides the linear
 * programming representation of water reservoirs, including their storage
 * constraints and relationships with junctions and waterways.
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using ReservoirLPId = ObjectId<class ReservoirLP>;
using ReservoirLPSId = ObjectSingleId<class ReservoirLP>;

/**
 * @brief Linear programming representation of a water reservoir
 *
 * This class extends StorageLP to provide LP-specific functionality for
 * reservoirs, including:
 * - Storage capacity constraints
 * - Water extraction constraints
 * - Relationships with connected junctions
 */
class ReservoirLP : public StorageLP<ObjectLP<Reservoir>>
{
public:
  static constexpr std::string_view ExtractionName {"extraction"};

  /// Parquet stem for the per-block volume-balance dual.  Industry
  /// convention names this quantity "water value" (PLEXOS Water
  /// Storage `Shadow Price`, PSR-SDDP "valor del agua", PLP
  /// `cmgcse`, PyPSA `mu_energy_balance` — explicitly aliased as
  /// "water value" in PyPSA docs).  We publish under that name so
  /// the Parquet column is directly comparable with those reports.
  /// The Battery class keeps the generic `EnergyName` ("energy")
  /// since the equivalent term in storage literature is the more
  /// neutral "marginal storage value".  Only the dual stem is
  /// renamed; the underlying LP variable / row names — and thus
  /// `Reservoir.energy.sol` / `Reservoir.energy.cost` — are
  /// unchanged.
  static constexpr std::string_view WaterValueName {"water_value"};

  using StorageBase = StorageLP<ObjectLP<Reservoir>>;

  explicit ReservoirLP(const Reservoir& preservoir, const InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto junction_sid() const noexcept
  {
    return JunctionLPSId {reservoir().junction};
  }

  [[nodiscard]] constexpr auto flow_conversion_rate() const noexcept
  {
    // Default to the struct-side ``Reservoir::default_flow_conversion_rate``
    // (= 0.0036 hm³/(m³/s·h), the physical hm³ ⇄ m³/s conversion 3600/1e6).
    // The previous literal ``3.6`` here disagreed with the struct
    // default by a factor of 1000 — if a converter ever forgot to
    // emit ``flow_conversion_rate`` on a Reservoir, every unit of
    // turbine flow debited the reservoir 1000× more than the
    // physical hm³ change, inflating hydro generation by ~60 % on
    // the CEN PCP daily bundle (gtopt 457 578 MWh vs PLEXOS 285 854
    // MWh measured 2026-05-21 before the plexos2gtopt writer was
    // patched to set the field explicitly).  Aligning the LP-side
    // fallback with the struct default prevents that class of bug
    // from recurring if another converter ships an unset field.
    return reservoir().flow_conversion_rate.value_or(
        Element::default_flow_conversion_rate);
  }

  [[nodiscard]] constexpr auto spillway_cost() const noexcept
  {
    return reservoir().spillway_cost;
  }

  [[nodiscard]] constexpr auto spillway_capacity() const noexcept
  {
    return reservoir().spillway_capacity.value_or(+6'000.0);
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Return the extraction column indices for (scenario, stage).
  ///
  /// Extraction columns represent the water extraction flow from the
  /// reservoir into its connected junction.
  [[nodiscard]] constexpr const auto& extraction_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return extraction_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_capacity(StageUid s) const { return capacity.at(s); }
  /// @}

private:
  OptTRealSched capacity;
  OptTBRealSched scost;
  STBIndexHolder<ColIndex> extraction_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `Reservoir::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Reservoir"`).
static_assert(ReservoirLP::Element::class_name == LPClassName {"Reservoir"},
              "Reservoir::class_name must remain \"Reservoir\"");

}  // namespace gtopt
