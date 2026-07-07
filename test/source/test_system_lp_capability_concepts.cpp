// SPDX-License-Identifier: BSD-3-Clause
//
// Piece-1 capability-concept discriminators (system_lp.hpp).  The visitors in
// system_lp.cpp dispatch each build/output pass via `if constexpr
// (HasAddTo*<T>)`, so these concepts are the single source of truth for "which
// pass does this element participate in".  This test pins their classification
// with mock element shapes (declarations only — concepts check expression
// validity in an unevaluated context, so no definitions / linkage needed) plus
// a couple of real engine types.  See
// docs/design/future_cost_and_user_model.md.

#include <doctest/doctest.h>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Unique outer namespace so unity-build batching can't collide these helper
// names with another test file's mocks.
namespace test_syslp_caps
{

/// Common operational element (Generator / Line / Reservoir shape):
/// per-(scenario, stage) build + output, no planning scope.
struct StageOnlyMock
{
  bool add_to_lp(SystemContext&,
                 const ScenarioLP&,
                 const StageLP&,
                 LinearProblem&);
  bool add_to_output(OutputContext&);
};

/// FutureCost shape: global-scope planning build + output, no stage build.
struct GlobalMock
{
  bool add_to_global_lp(SystemContext&,
                        const SceneLP&,
                        const PhaseLP&,
                        LinearProblem&);
  bool add_to_output(OutputContext&);
};

/// Recourse / per-phase state-variable shape: phase-scope planning + output.
struct PhaseMock
{
  bool add_to_phase_lp(SystemContext&,
                       const SceneLP&,
                       const PhaseLP&,
                       LinearProblem&);
  bool add_to_output(OutputContext&);
};

/// UserModel shape: per-block build + BOTH planning scopes + output.
struct BothPlanningMock
{
  bool add_to_lp(SystemContext&,
                 const ScenarioLP&,
                 const StageLP&,
                 LinearProblem&);
  bool add_to_phase_lp(SystemContext&,
                       const SceneLP&,
                       const PhaseLP&,
                       LinearProblem&);
  bool add_to_global_lp(SystemContext&,
                        const SceneLP&,
                        const PhaseLP&,
                        LinearProblem&);
  bool add_to_output(OutputContext&);
};

/// Passive / reference-only carrier: no pass methods at all.
struct DataOnlyMock
{
};

// ── Stage-only: the operational default ───────────────────────────────────
static_assert(HasAddToLp<StageOnlyMock>);
static_assert(HasAddToOutput<StageOnlyMock>);
static_assert(AddToLP<StageOnlyMock>);
static_assert(!HasAddToPhaseLp<StageOnlyMock>);
static_assert(!HasAddToGlobalLp<StageOnlyMock>);
static_assert(!HasAddToPlanning<StageOnlyMock>);

// ── Global (FutureCost): planning + output, NOT the full stage AddToLP ─────
static_assert(!HasAddToLp<GlobalMock>);
static_assert(HasAddToOutput<GlobalMock>);
static_assert(HasAddToGlobalLp<GlobalMock>);
static_assert(!HasAddToPhaseLp<GlobalMock>);
static_assert(HasAddToPlanning<GlobalMock>);
static_assert(!AddToLP<GlobalMock>);

// ── Phase (recourse state var): phase-scope only ──────────────────────────
static_assert(HasAddToPhaseLp<PhaseMock>);
static_assert(!HasAddToGlobalLp<PhaseMock>);
static_assert(HasAddToPlanning<PhaseMock>);
static_assert(!HasAddToLp<PhaseMock>);

// ── UserModel: stage + both planning scopes ───────────────────────────────
static_assert(HasAddToLp<BothPlanningMock>);
static_assert(HasAddToPhaseLp<BothPlanningMock>);
static_assert(HasAddToGlobalLp<BothPlanningMock>);
static_assert(HasAddToPlanning<BothPlanningMock>);
static_assert(AddToLP<BothPlanningMock>);

// ── Data-only: skipped by every pass ──────────────────────────────────────
static_assert(!HasAddToLp<DataOnlyMock>);
static_assert(!HasAddToOutput<DataOnlyMock>);
static_assert(!HasAddToPhaseLp<DataOnlyMock>);
static_assert(!HasAddToGlobalLp<DataOnlyMock>);
static_assert(!HasAddToPlanning<DataOnlyMock>);

// ── Real engine elements stay stage-scope, never planning ─────────────────
static_assert(AddToLP<GeneratorLP>);
static_assert(!HasAddToPlanning<GeneratorLP>);
static_assert(AddToLP<ReservoirLP>);
static_assert(!HasAddToPlanning<ReservoirLP>);

}  // namespace test_syslp_caps

TEST_CASE("system_lp capability concepts classify element passes")  // NOLINT
{
  using namespace test_syslp_caps;

  SUBCASE("stage-only operational element")
  {
    CHECK(AddToLP<StageOnlyMock>);
    CHECK_FALSE(HasAddToPlanning<StageOnlyMock>);
  }

  SUBCASE("global-scope planning element (FutureCost shape)")
  {
    CHECK(HasAddToGlobalLp<GlobalMock>);
    CHECK(HasAddToPlanning<GlobalMock>);
    CHECK(HasAddToOutput<GlobalMock>);
    // No per-(scenario, stage) build ⇒ skipped by the operational pass.
    CHECK_FALSE(HasAddToLp<GlobalMock>);
    CHECK_FALSE(AddToLP<GlobalMock>);
  }

  SUBCASE("phase-scope planning element (recourse state var)")
  {
    CHECK(HasAddToPhaseLp<PhaseMock>);
    CHECK(HasAddToPlanning<PhaseMock>);
    CHECK_FALSE(HasAddToGlobalLp<PhaseMock>);
  }

  SUBCASE("UserModel shape participates in every pass")
  {
    CHECK(AddToLP<BothPlanningMock>);
    CHECK(HasAddToPhaseLp<BothPlanningMock>);
    CHECK(HasAddToGlobalLp<BothPlanningMock>);
  }

  SUBCASE("data-only carrier is skipped everywhere")
  {
    CHECK_FALSE(HasAddToLp<DataOnlyMock>);
    CHECK_FALSE(HasAddToOutput<DataOnlyMock>);
    CHECK_FALSE(HasAddToPlanning<DataOnlyMock>);
  }

  SUBCASE("real engine elements are stage-scope, never planning")
  {
    CHECK(HasAddToPlanning<GeneratorLP> == false);
    CHECK(HasAddToPlanning<ReservoirLP> == false);
  }
}
