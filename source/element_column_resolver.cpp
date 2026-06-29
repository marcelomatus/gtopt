/**
 * @file      element_column_resolver.cpp
 * @brief     Resolve LP column indices for user-constraint element references
 * @date      Mon Mar 24 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <charconv>
#include <format>
#include <functional>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/constraint_expr.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/names_registry.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ── Element-ID parsing ───────────────────────────────────────────────────────

/**
 * @brief Convert a constraint-expression element-id string to a `SingleId`.
 *
 * Three accepted forms:
 *  - `"uid:N"` — UID N (as written by the constraint parser for bare integers)
 *  - `"N"`     — pure decimal integer → UID N
 *  - anything else → Name lookup
 */
[[nodiscard]] SingleId parse_element_id(const std::string& element_id)
{
  // "uid:N" form produced by the constraint parser for bare-integer references
  if (element_id.starts_with("uid:")) {
    const std::string_view digits = std::string_view {element_id}.substr(4);
    Uid val {};
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), val);
    const bool ok = ec == std::errc {} && ptr == digits.data() + digits.size();
    if (ok) {
      return val;
    }
    // fall through to name if parse fails
    return Name {element_id};
  }

  // Bare integer string (purely decimal, no letters)
  if (!element_id.empty()
      && std::ranges::all_of(
          element_id, [](unsigned char c) { return std::isdigit(c) != 0; }))
  {
    Uid val {};
    const auto [ptr, ec] = std::from_chars(
        element_id.data(), element_id.data() + element_id.size(), val);
    const bool ok =
        ec == std::errc {} && ptr == element_id.data() + element_id.size();
    if (ok) {
      return val;
    }
  }

  // Otherwise treat as a name
  return Name {element_id};
}

}  // anonymous namespace

// ── Per-element column resolution ────────────────────────────────────────────

[[nodiscard]] std::optional<ResolvedCol> resolve_single_col(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref,
    const LinearProblem& lp)
{
  // Singleton-class scalars (`options.*`, `system.*`) carry no element
  // id and resolve to a constant numeric, never to an LP column — leave
  // them to `resolve_single_param`'s scalar branch.
  if (ref.element_id.empty()) {
    return std::nullopt;
  }

  const auto single_id = parse_element_id(ref.element_id);
  BlockUid buid = block.uid();

  // ── Inter-block "_prev" suffix ─────────────────────────────────────
  // ``generator("X").generation_prev`` resolves to ``gen[t−1]`` in
  // the chronological block sequence — used by PLEXOS-style Ramp
  // Up/Down Coefficient constraints reformulated as
  //   coef × gen[t] − coef × gen[t−1] ≤ rhs
  // The suffix is stripped here, the base attribute name reused for
  // the registry lookup, and ``buid`` redirected to the immediately
  // preceding block in the stage's chronological ordering.  When
  // the current block is the FIRST block (or the stage is not
  // chronological), no prior LP column exists — return nullopt so
  // the caller falls through to ``resolve_single_param``, which
  // handles the boundary case via ``commitment.initial_status``.
  std::string normalised_attr {ref.attribute};
  static constexpr std::string_view PREV_SUFFIX {"_prev"};
  // Convention shared with DecisionVariableLP: a `block_state` variable
  // registers its cross-phase incoming column under `<attr>` + this suffix
  // (i.e. `value` → `value_in`, == DecisionVariableLP::IncomingName).  Kept as
  // a named constant (like PREV_SUFFIX) so the coupling is not a bare literal.
  static constexpr std::string_view INCOMING_SUFFIX {"_in"};

  // ── prev(<ref>): lagged reference for block_state storage volumes ──────
  // Resolves to the previous chronological block's column within the stage.
  // At the FIRST block of the stage it resolves to the cross-phase INCOMING
  // column (`<attr>_in`, aliased across the stage's blocks), which the SDDP
  // forward pass pins to the previous phase's end-of-phase value (or, on the
  // first phase, the fixed initial value).  This is the `vol[b-1]` term that
  // lets a user constraint express a per-block storage balance.
  if (ref.prev_wrapped) {
    const auto& blocks = stage.blocks();
    const auto it = std::ranges::find_if(
        blocks, [buid](const BlockLP& b) { return b.uid() == buid; });
    if (it == blocks.end()) {
      return std::nullopt;
    }
    if (it == blocks.begin()) {
      // First block of the stage → the cross-phase/stage INCOMING column
      // (`value_in`); buid stays at the first block.  This is a coarse state
      // column, NOT an intra-stage lag, so it needs no chronological order
      // (in particular a single-block stage is fine here).
      normalised_attr += INCOMING_SUFFIX;
    } else if (!stage.is_chronological()) {
      // Interior lag requires a chronological block sequence; without one
      // there is no well-defined "previous block".
      SPDLOG_WARN(
          "user_constraint: prev({}({}).{}) references a prior-block value in "
          "a non-chronological stage — dropping term",
          ref.element_type,
          ref.element_id,
          ref.attribute);
      return std::nullopt;
    } else {
      // Interior block → the same attribute on the previous block.
      buid = std::prev(it)->uid();
    }
  } else if (normalised_attr.ends_with(PREV_SUFFIX)) {
    normalised_attr.erase(normalised_attr.size() - PREV_SUFFIX.size());
    if (!stage.is_chronological()) {
      SPDLOG_WARN(
          "user_constraint: '{}({}).{}' references prior-block value in "
          "a non-chronological stage — dropping term",
          ref.element_type,
          ref.element_id,
          ref.attribute);
      return std::nullopt;
    }
    const auto& blocks = stage.blocks();
    const auto it = std::ranges::find_if(
        blocks, [buid](const BlockLP& b) { return b.uid() == buid; });
    if (it == blocks.end() || it == blocks.begin()) {
      // First block (or block not in this stage) — let
      // resolve_single_param emit the initial-state constant.
      return std::nullopt;
    }
    buid = std::prev(it)->uid();
  }

  // 1. Convert element_id (Uid or Name) into a concrete Uid.  Names
  //    are looked up via the AMPL element-name registry populated by
  //    each element's `add_to_lp` on first invocation.
  const auto uid_opt = [&]() -> std::optional<Uid>
  {
    if (std::holds_alternative<Uid>(single_id)) {
      return std::get<Uid>(single_id);
    }
    if (std::holds_alternative<Name>(single_id)) {
      return sc.lookup_ampl_element_uid(ref.element_type,
                                        std::get<Name>(single_id));
    }
    return std::nullopt;
  }();

  if (!uid_opt) {
    // Unknown element name.  Returning nullopt here flows back to
    // `stamp_ref`, where `element_known` is computed as false, so the
    // strict caller in `user_constraint_lp.cpp` raises an informative,
    // process-terminating error (with a "did you mean ...?" hint).  The
    // throw is the authoritative diagnostic; this is just a trace so we
    // do NOT double-log a WARN that wrongly implies tolerance.
    SPDLOG_DEBUG("user_constraint: unknown {} name '{}' (column resolution)",
                 ref.element_type,
                 ref.element_id);
    return std::nullopt;
  }

  // 2. Generic AMPL variable registry lookup — primary path.  All
  //    migrated elements populate this map in their `add_to_lp`, so
  //    we can look up the column without per-element-type dispatch.
  if (const auto col = sc.find_ampl_col(ref.element_type,
                                        *uid_opt,
                                        normalised_attr,
                                        scenario.uid(),
                                        stage.uid(),
                                        buid))
  {
    // Optional additive offset: physical = LP * scale + offset.
    // Returns 0.0 for every element that didn't register offsets —
    // the common case.  Demand's Option C registration is the only
    // current user; see `demand_lp.cpp`.
    return ResolvedCol {
        .col = *col,
        .scale = lp.get_col_scale(*col),
        .offset = sc.find_ampl_offset(ref.element_type,
                                      *uid_opt,
                                      normalised_attr,
                                      scenario.uid(),
                                      stage.uid(),
                                      buid),
    };
  }

  // 3. Fallback: `bus.theta` columns are created lazily by
  //    `LineLP::add_kirchhoff_rows` through `theta_cols_at` and are
  //    therefore not yet known to the registry at `BusLP::add_to_lp`
  //    time.  Keep the bespoke lookup so PAMPL expressions can still
  //    reference theta once the lines have populated the map.
  if (ref.element_type == "bus" && ref.attribute == BusLP::ThetaName) {
    try {
      const auto& bus_lp = sc.get_element(ObjectSingleId<BusLP> {single_id});
      if (auto col = bus_lp.lookup_theta_col(scenario, stage, buid)) {
        return ResolvedCol {
            .col = *col,
            .scale = lp.get_col_scale(*col),
        };
      }
    } catch (const std::exception& ex) {
      // The bus id did not resolve to a BusLP.  nullopt flows back to the
      // strict caller, which raises the authoritative informative error;
      // trace only here.
      SPDLOG_DEBUG("user_constraint: cannot resolve {}.{}('{}'): {}",
                   ref.element_type,
                   ref.attribute,
                   ref.element_id,
                   ex.what());
    }
    return std::nullopt;
  }

  return std::nullopt;
}

// ── Compound-aware row emission ──────────────────────────────────────────────

/// Resolve and stamp a single (class, uid, attribute) reference into
/// `row` with coefficient `coef`.  Handles both single-col registrations
/// (the common case) and **multi-col sum** registrations used by virtual
/// aggregators like `line.flowp` under `piecewise_direct` line-loss mode
/// (no aggregator LP col — `flowp` resolves to `Σ flowp_seg_k`, so we
/// stamp `coef` on every segment col).  See
/// `AmplVariable::block_cols_sum` for the sum-of-cols data layout.
///
/// Returns `{emitted, offset_shift}`.  `offset_shift` is `coef × offset`
/// when the resolved column carries a non-zero AMPL offset (e.g.
/// demand's Option C `neg_fail = load − lmax`); the caller folds the
/// shift onto the row's RHS via the existing `param_shift` accumulator.
namespace
{
/// Resolve the *constant* parts of an element reference — the element-id
/// parse, the name→uid lookup, and the AMPL-variable find.  Constant per
/// (class, id, attribute) for a fixed (scenario, stage); see
/// `ElementRefResolution`.
[[nodiscard]] ElementRefResolution resolve_element_ref(const SystemContext& sc,
                                                       const ElementRef& ref,
                                                       ScenarioUid scenario_uid,
                                                       StageUid stage_uid)
{
  ElementRefResolution r;
  const auto single_id = parse_element_id(ref.element_id);
  r.element_id_was_name = std::holds_alternative<Name>(single_id);
  if (std::holds_alternative<Uid>(single_id)) {
    r.uid = std::get<Uid>(single_id);
  } else if (r.element_id_was_name) {
    r.uid =
        sc.lookup_ampl_element_uid(ref.element_type, std::get<Name>(single_id));
  }
  if (r.uid) {
    r.var = sc.find_ampl_variable(
        ref.element_type, *r.uid, ref.attribute, scenario_uid, stage_uid);
  }
  return r;
}

/// Memoised resolution: return the cached `ElementRefResolution` for
/// `ref` (keyed by address) or compute-and-cache it.  Only the stable
/// top-level term refs are cached; with `cache == nullptr` (non-constraint
/// callers and transient compound-leg refs) it resolves into `scratch`.
[[nodiscard]] const ElementRefResolution& cached_resolve_element_ref(
    AmplResolveCache* cache,
    ElementRefResolution& scratch,
    const SystemContext& sc,
    const ElementRef& ref,
    ScenarioUid scenario_uid,
    StageUid stage_uid)
{
  if (cache != nullptr) {
    if (const auto it = cache->find(&ref); it != cache->end()) {
      return it->second;
    }
    return cache
        ->emplace(&ref, resolve_element_ref(sc, ref, scenario_uid, stage_uid))
        .first->second;
  }
  scratch = resolve_element_ref(sc, ref, scenario_uid, stage_uid);
  return scratch;
}

[[nodiscard]] ResolveColResult stamp_ref(const SystemContext& sc,
                                         const ScenarioLP& scenario,
                                         const StageLP& stage,
                                         const BlockLP& block,
                                         const ElementRef& ref,
                                         double coef,
                                         SparseRow& row,
                                         const LinearProblem& lp,
                                         AmplResolveCache* resolve_cache)
{
  // Resolve element_id (uid|name) once; multi-col path needs the uid.
  // Also remember whether the element_id was supplied as a NAME (which
  // is verified by the lookup, proving the UID exists in the registry)
  // or as a bare/uid:N integer (whose existence is NOT verified by the
  // parse step alone).  The distinction matters for the LP-attr-dormant
  // leniency below: a bare integer like ``generator(999)`` must NOT be
  // treated as a known element just because some other generator
  // registers the requested attribute.
  // Resolve the id-parse, name→uid lookup and AMPL-variable find ONCE per
  // ElementRef and reuse across the constraint's block loop — all three are
  // constant per (class, id, attribute) for the fixed (scenario, stage), so
  // this collapses the previous per-(term, block) re-parse + two hash
  // lookups (parse_element_id / lookup_ampl_element_uid / find_ampl_variable)
  // into one resolve per term.  Compound legs below pass a null cache (their
  // ElementRef is a transient stack temporary) and always resolve fresh.
  ElementRefResolution scratch;
  const auto& res = cached_resolve_element_ref(
      resolve_cache, scratch, sc, ref, scenario.uid(), stage.uid());
  const auto& uid_opt = res.uid;
  const bool element_id_was_name = res.element_id_was_name;

  // ``element_known`` flips to true when the reference is a *defined*
  // silent-0 case rather than a typo / unsupported reference.  Two
  // accepted sub-cases (both flow to "term contributes 0 to LHS"):
  //
  //   (1) Per-cell-dormant: the (class, element_uid, attribute) triple
  //       is registered as an LP variable somewhere in the simulation
  //       (any scene/phase/scenario/stage) but not for THIS specific
  //       (scenario, stage, block).  Typical: pmax=0 in some blocks of
  //       a per-block ``pmax`` schedule — other blocks register the
  //       column.
  //
  //   (2) LP-attr-dormant: the element NAME resolved to a real UID
  //       (so the element genuinely exists), AND the (class, attribute)
  //       pair is registered for at least ONE element of the class
  //       somewhere — i.e. the attribute IS a valid LP variable on the
  //       class, just not materialised on THIS element because it has
  //       zero capacity over the entire horizon.  Typical: a generator
  //       with ``pmax = 0`` and no ``pmax_profile`` (``PANGUE_U1`` on
  //       CEN PCP weekly) — its ``generation`` column is never created
  //       by GeneratorLP, but PLEXOS treats coefficient × 0 = 0 as a
  //       valid contribution.  The match condition is broader than
  //       (1): the registry hit can be on ANY element of the same
  //       class.  Restricted to NAME references because a bare integer
  //       like ``generator(999)`` is not proven to exist by the parser
  //       alone (parse_element_id happily wraps any digit string in a
  //       ``Uid``) — out-of-range bare UIDs must keep throwing.
  //
  // Both cases are PLEXOS-faithful — the underlying variable is
  // implicitly 0 and the term legitimately contributes 0 to the LHS.
  //
  // Critically, this still rejects:
  //   * Unknown class names.
  //   * Misspelled attribute on a known class (no element registers
  //     the (class, attribute) pair anywhere — e.g. ``generattion``).
  //   * Misspelled element name on a known class (uid_opt is nullopt).
  //   * Bare-uid integer references whose UID is not registered
  //     (``generator(999).generation`` when 999 is not a real gen).
  //
  // The simple "uid_opt has value" check isn't enough: a misspelled
  // attribute on a real element would otherwise leak through and
  // silently make the UC vacuous.
  const auto element_known_silent_zero = [&]() -> bool
  {
    if (!uid_opt.has_value()) {
      return false;
    }
    // Case (1): exact (class, uid, attribute) triple registered.
    if (sc.find_ampl_variable_for_element(
            ref.element_type, *uid_opt, ref.attribute))
    {
      return true;
    }
    // Case (2): LP-attr-dormant — element NAME resolved (so the UID is
    // genuinely registered) and the (class, attribute) pair is
    // registered for at least one element of the class.  Catches the
    // all-horizon zero-capacity case.  Restricted to name references
    // so out-of-range bare-uid integers keep throwing.
    if (!element_id_was_name) {
      return false;
    }
    return sc.find_ampl_class_attribute(ref.element_type, ref.attribute);
  };

  // `prev(<ref>)` must NOT take the fast `res.var` path: that resolves the
  // CURRENT block's column under the (unchanged) attribute, but a lagged
  // reference needs the previous block's column (or, at the first block, the
  // `value_in` incoming column).  Skip to `resolve_single_col`, which honours
  // `ref.prev_wrapped` — mirroring how a `_prev`-suffixed attribute escapes
  // the fast path because its normalised name isn't registered.
  if (uid_opt && !ref.prev_wrapped) {
    // Read every per-block shape directly off the cached registry entry
    // (`res.var`, resolved once above) — weighted-sum / sum / single col +
    // offset — instead of a separate hash+find per shape per block.  This
    // is the hot path on user-constraint-heavy cases (CEN PLEXOS,
    // irrigation): one registry resolve per term, then pure per-block reads.
    if (const auto* var = res.var) {
      const auto buid = block.uid();
      // (a) Weighted sum-of-cols.  Used by `FuelLP` for
      // ``fuel("X").offtake = Σ heat_rate_g · dur · gen_g`` and by
      // `LineLP` for the signed ``line.flow = +flowp − flown``; each leg
      // carries its own coefficient.
      if (const auto weighted = var->weighted_cols_at(buid); !weighted.empty())
      {
        for (const auto& [col, weight] : weighted) {
          row[col] += coef * weight;
        }
        return {.emitted = true, .offset_shift = 0.0, .element_known = true};
      }
      // (b) Unweighted sum-of-cols (virtual aggregator, e.g.
      // ``piecewise_direct`` segments) — every leg has weight 1.  These
      // registrations never carry offsets in current code.
      if (const auto cols = var->cols_at(buid); !cols.empty()) {
        for (const auto& col : cols) {
          row[col] += coef;
        }
        return {.emitted = true, .offset_shift = 0.0, .element_known = true};
      }
      // (c) Single col (the common case) + optional Option-C offset; the
      // caller folds `coef × offset` onto the row RHS via param_shift.
      if (const auto col = var->col_at(buid)) {
        row[*col] += coef;
        return {
            .emitted = true,
            .offset_shift = coef * var->offset_at(buid),
            .element_known = true,
        };
      }
      // Registered for this (class, uid, attribute) here but no column
      // for THIS block (e.g. per-block pmax=0): a defined silent-zero —
      // the element is known so the term legitimately contributes 0.
      return {.emitted = false, .offset_shift = 0.0, .element_known = true};
    }
  }

  // `ref.attribute` is not directly registered under this key: `_prev`
  // chronological refs (a normalised attribute on the previous block) and
  // lazily-created `bus.theta` columns are resolved by the bespoke
  // single-col path.
  if (auto resolved = resolve_single_col(sc, scenario, stage, block, ref, lp)) {
    row[resolved->col] += coef;
    return {
        .emitted = true,
        .offset_shift = coef * resolved->offset,
        .element_known = true,
    };
  }
  return {
      .emitted = false,
      .offset_shift = 0.0,
      .element_known = element_known_silent_zero(),
  };
}
}  // namespace

ResolveColResult resolve_col_to_row(const SystemContext& sc,
                                    const ScenarioLP& scenario,
                                    const StageLP& stage,
                                    const BlockLP& block,
                                    const ElementRef& ref,
                                    double base_coeff,
                                    SparseRow& row,
                                    const LinearProblem& lp,
                                    AmplResolveCache* resolve_cache)
{
  // 1. Direct single-attribute path FIRST — the common case (generation,
  //    line.flow, demand.load, converter.charge/discharge, …).  When the
  //    attribute is directly registered this returns immediately and the
  //    class-level compound-map probe below is skipped: that probe runs
  //    once per (term, block) and is pure overhead for ~every directly-
  //    registered attribute.  Safe to try first because no attribute is
  //    BOTH a compound recipe and a direct registration — the only
  //    remaining compound is ``converter.flow``, and a converter registers
  //    ``charge`` / ``discharge`` directly, never ``flow`` (so the two
  //    paths never both emit and precedence is immaterial).
  auto direct = stamp_ref(
      sc, scenario, stage, block, ref, base_coeff, row, lp, resolve_cache);
  if (direct.emitted) {
    return direct;
  }

  // 2. Compound path — class-level recipe of (coefficient,
  //    source_attribute), reached only when the attribute resolved to no
  //    direct column.  Expand each leg and stamp it.  A single known leg
  //    suffices to mark the compound element as known.
  if (const auto* legs = sc.find_ampl_compound(ref.element_type, ref.attribute))
  {
    ResolveColResult out;
    out.element_known = direct.element_known;
    for (const auto& leg : *legs) {
      ElementRef leg_ref = ref;
      leg_ref.attribute = std::string {leg.source_attribute};
      // Legs use a NULL cache: `leg_ref` is a transient stack temporary,
      // so caching it by address would be unsafe; compounds are rare
      // (only converter.flow) so this resolves fresh per (leg, block).
      const auto leg_res = stamp_ref(sc,
                                     scenario,
                                     stage,
                                     block,
                                     leg_ref,
                                     base_coeff * leg.coefficient,
                                     row,
                                     lp,
                                     nullptr);
      if (leg_res.emitted) {
        out.emitted = true;
        out.offset_shift += leg_res.offset_shift;
      }
      if (leg_res.element_known) {
        out.element_known = true;
      }
    }
    return out;
  }

  // 3. Neither a direct column nor a compound recipe: return the direct
  //    result (not emitted), which carries `element_known` for the
  //    caller's strict / silent-zero decision.
  return direct;
}

// ── Per-element parameter resolution ────────────────────────────────────────

[[nodiscard]] std::optional<double> resolve_single_param(
    const SystemContext& sc,
    [[maybe_unused]] const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref)
{
  // ── singleton class scalar (options.*, system.*, stage.*) ───────────
  // No element id, no element-level variation.  Most of these are
  // immutable for the SimulationLP lifetime and live in the scalar
  // registry, but `stage.*` reads metadata of the *active* stage and
  // therefore has to be resolved against the StageLP argument.
  if (ref.element_id.empty()) {
    if (ref.element_type == StageLP::ClassName) {
      if (ref.attribute == StageLP::MonthName) {
        const auto m = stage.month();
        if (m.has_value()) {
          return static_cast<double>(std::to_underlying(*m));
        }
        return std::nullopt;
      }
      if (ref.attribute == StageLP::UidName) {
        return static_cast<double>(stage.uid());
      }
      if (ref.attribute == StageLP::DurationName) {
        return stage.duration();
      }
      // Unknown `stage.*` attribute.  Returning nullopt flows back to
      // the element-term branch in `user_constraint_lp.cpp`, which then
      // raises an informative, process-terminating error.  Trace only —
      // do NOT WARN-and-imply-tolerance here.
      SPDLOG_DEBUG("user_constraint: unknown stage attribute '{}'",
                   ref.attribute);
      return std::nullopt;
    }
    if (auto val = sc.find_ampl_scalar(ref.element_type, ref.attribute)) {
      return val;
    }
    // Unknown singleton scalar (e.g. `options.bogus`).  Same contract as
    // above: nullopt → strict caller throws an informative error; trace
    // only here.
    SPDLOG_DEBUG("user_constraint: unknown scalar {}.{}",
                 ref.element_type,
                 ref.attribute);
    return std::nullopt;
  }

  // ── "_prev" suffix at the boundary ─────────────────────────────────
  // ``resolve_single_col`` strips a trailing ``_prev`` and looks up the
  // SAME class+uid+base-attribute at the previous chronological block.
  // It returns nullopt for the FIRST block (no prior column) and for
  // non-chronological stages.  Both cases land here.  Treat the missing
  // prior value as 0 (cold-start: the unit was off before t=0).  This
  // makes ``coef × generation_prev`` cleanly vanish for the boundary,
  // matching PLEXOS's default behaviour for ramp constraints at the
  // start of a horizon.  A follow-up can read
  // ``commitment.initial_status × pmax_at_block_0`` instead for warm
  // starts; until then the cold-start assumption is conservative
  // (no ramp budget is consumed against a phantom prior dispatch).
  if (ref.attribute.ends_with("_prev")) {
    return 0.0;
  }

  const auto single_id = parse_element_id(ref.element_id);
  const auto suid = stage.uid();
  const auto buid = block.uid();

  // Convert element_id (Uid or Name) into a concrete Uid.  Names are
  // looked up via the AMPL element-name registry populated by each
  // element's `add_to_lp` on first invocation.  Same path as
  // `resolve_single_col` — see lookup_ampl_element_uid.
  const auto uid_opt = [&]() -> std::optional<Uid>
  {
    if (std::holds_alternative<Uid>(single_id)) {
      return std::get<Uid>(single_id);
    }
    if (std::holds_alternative<Name>(single_id)) {
      return sc.lookup_ampl_element_uid(ref.element_type,
                                        std::get<Name>(single_id));
    }
    return std::nullopt;
  }();

  if (!uid_opt) {
    // Unknown element name on the parameter path.  nullopt flows back to
    // the caller, which raises the authoritative informative error; trace
    // only here to avoid double-logging a WARN that implies tolerance.
    SPDLOG_DEBUG("user_constraint: unknown {} name '{}' (param resolution)",
                 ref.element_type,
                 ref.element_id);
    return std::nullopt;
  }

  // Normalise the attribute name via the runtime naming-dialects
  // registry.  Class-scoped lookup first (entries under
  // `class_aliases[]` — e.g. `flow_right.discharge → target`,
  // `flow_right.use_value → uvalue` — both legacy gtopt aliases
  // that collide with canonicals on other classes); falls back to
  // the global table for class-blind aliases like `marginal_cost
  // → gcost`, `tmax → tmax_ab`, etc.  See
  // docs/analysis/naming-conventions.md §10.4.
  auto attr = std::string_view {ref.attribute};
  if (const auto canonical =
          NamesRegistry::instance().canonical_for(ref.element_type, attr);
      canonical.has_value())
  {
    attr = *canonical;
  }

  try {
    // Registry-driven dispatch.  Every per-class param exposed to user
    // constraints is registered as `(class_name, attribute) -> fn` in
    // `ampl_dispatch_registry.cpp::register_ampl_param_dispatchers`.  The
    // function is a thin shim into the element's `param_X(s, b)`
    // accessor, so a single map probe + indirect call replaces the
    // legacy per-class if/else chain.  Adding a new (class, attribute)
    // means registering one function pointer — no change here.
    if (const auto fn = sc.find_ampl_param(ref.element_type, attr)) {
      return fn(sc, *uid_opt, suid, buid);
    }
    return std::nullopt;
  } catch (const std::exception& ex) {
    // The registered param accessor threw (e.g. out-of-range lookup).
    // nullopt flows back to the caller; in the element-term path the
    // strict resolver then raises the authoritative informative error.
    // Trace only here.
    SPDLOG_DEBUG(
        std::format("user_constraint: cannot resolve param {}.{}('{}'): {}",
                    ref.element_type,
                    ref.attribute,
                    ref.element_id,
                    ex.what()));
    return std::nullopt;
  }
}

// ── Sum predicate evaluation ────────────────────────────────────────────────

namespace
{

/// Compare a metadata value against a predicate's RHS.  String-vs-number
/// mismatches always return false (the predicate is unsatisfied).
[[nodiscard]] bool eval_predicate(const SumPredicate& pred,
                                  const AmplMetadataValue& value)
{
  using Op = SumPredicate::Op;

  // Set membership: stringify both sides.
  if (pred.op == Op::In) {
    std::string s;
    if (std::holds_alternative<std::string>(value)) {
      s = std::get<std::string>(value);
    } else {
      s = std::format("{}", std::get<double>(value));
    }
    return std::ranges::find(pred.set_values, s) != pred.set_values.end();
  }

  // Numeric predicate.
  if (pred.number_value.has_value()) {
    if (!std::holds_alternative<double>(value)) {
      return false;
    }
    const double lhs = std::get<double>(value);
    const double rhs = *pred.number_value;
    switch (pred.op) {
      case Op::Eq:
        return lhs == rhs;
      case Op::Ne:
        return lhs != rhs;
      case Op::Lt:
        return lhs < rhs;
      case Op::Le:
        return lhs <= rhs;
      case Op::Gt:
        return lhs > rhs;
      case Op::Ge:
        return lhs >= rhs;
      default:
        return false;
    }
  }

  // String predicate.
  if (pred.string_value.has_value()) {
    if (!std::holds_alternative<std::string>(value)) {
      return false;
    }
    const auto& lhs = std::get<std::string>(value);
    const auto& rhs = *pred.string_value;
    switch (pred.op) {
      case Op::Eq:
        return lhs == rhs;
      case Op::Ne:
        return lhs != rhs;
      case Op::Lt:
        return lhs < rhs;
      case Op::Le:
        return lhs <= rhs;
      case Op::Gt:
        return lhs > rhs;
      case Op::Ge:
        return lhs >= rhs;
      default:
        return false;
    }
  }

  return false;
}

/// Return true iff the element identified by (class_name, element_uid)
/// satisfies every predicate in @p filters (AND semantics).  An element
/// with no registered metadata fails any non-empty filter list.
[[nodiscard]] bool element_passes_filters(
    const SystemContext& sc,
    std::string_view class_name,
    Uid element_uid,
    const std::vector<SumPredicate>& filters)
{
  if (filters.empty()) {
    return true;
  }
  const auto* metadata = sc.find_ampl_element_metadata(class_name, element_uid);
  if (metadata == nullptr) {
    return false;
  }
  for (const auto& pred : filters) {
    auto it = std::ranges::find_if(
        *metadata, [&](const auto& kv) { return kv.first == pred.attr; });
    if (it == metadata->end()) {
      return false;
    }
    if (!eval_predicate(pred, it->second)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ── Sum-reference resolution ─────────────────────────────────────────────────

double collect_sum_cols(const SystemContext& sc,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        const BlockLP& block,
                        const SumElementRef& sum_ref,
                        double base_coeff,
                        SparseRow& row,
                        const LinearProblem& lp)
{
  double offset_shift = 0.0;
  // Helper lambda: add one ElementRef to the row.  Uses the compound-aware
  // `resolve_col_to_row` so sums over a compound attribute (e.g.
  // `sum(line(all).flow)`) correctly expand each leg.  The per-leg
  // offset shift (Option C demand etc.) is accumulated into the
  // outer `offset_shift` so the caller can fold it onto the row RHS.
  auto add_one = [&](const std::string& eid)
  {
    ElementRef ref;
    ref.element_type = sum_ref.element_type;
    ref.element_id = eid;
    ref.attribute = sum_ref.attribute;

    const auto res = resolve_col_to_row(
        sc, scenario, stage, block, ref, base_coeff, row, lp);
    offset_shift += res.offset_shift;
  };

  if (sum_ref.all_elements) {
    // Registry-driven dispatch.  The class iterator was registered in
    // `ampl_dispatch_registry.cpp::register_ampl_iterator_dispatchers` —
    // a function pointer that walks `sc.elements<LP>()` for the matching
    // LP class.  Filter evaluation stays here because the iterator
    // layer is decoupled from `constraint_expr.hpp`; we pass a
    // captureless C-style callback + state struct so the iterator
    // signature can be a plain function pointer (no `std::function`).
    if (const auto iter_fn = sc.find_ampl_iter(sum_ref.element_type)) {
      struct IterState
      {
        const SystemContext* sc;
        std::string_view class_name;
        const std::vector<SumPredicate>* filters;
        std::function<void(Uid)>* add_uid;
      };
      std::function<void(Uid)> add_uid = [&](Uid uid)
      { add_one(as_label(uid)); };
      IterState state {
          .sc = &sc,
          .class_name = sum_ref.element_type,
          .filters = &sum_ref.filters,
          .add_uid = &add_uid,
      };
      iter_fn(
          sc,
          &state,
          +[](void* p, Uid uid)
          {
            auto* s = static_cast<IterState*>(p);
            if (!element_passes_filters(
                    *s->sc, s->class_name, uid, *s->filters)) {
              return;
            }
            (*s->add_uid)(uid);
          });
    }
  } else {
    for (const auto& eid : sum_ref.element_ids) {
      add_one(eid);
    }
  }
  return offset_shift;
}

}  // namespace gtopt
