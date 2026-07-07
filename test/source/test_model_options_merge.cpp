// SPDX-License-Identifier: BSD-3-Clause
//
// Pins ModelOptions::merge() semantics for the fields added in this
// session — `naming_dialect` and `continuous_phases` — so cascade
// level inheritance (where each level's model_options is merged onto
// the previous level's effective model_options in `cascade_method.
// cpp::solve`) cannot silently drop them.

#include <doctest/doctest.h>
#include <gtopt/model_options.hpp>

using namespace gtopt;

TEST_CASE(
    "ModelOptions::merge — naming_dialect propagates from other")  // NOLINT
{
  // Cascade global model_options sets naming_dialect; the per-level
  // override is empty.  Merge must preserve the global value.
  ModelOptions base;
  base.naming_dialect = OptName {"plp"};

  ModelOptions override_empty;  // no naming_dialect

  base.merge(override_empty);

  REQUIRE(base.naming_dialect.has_value());
  CHECK(*base.naming_dialect == "plp");
}

TEST_CASE(
    "ModelOptions::merge — naming_dialect from other wins over base")  // NOLINT
{
  // Cascade global says gtopt, per-level explicitly switches to
  // plexos.  The per-level override wins — same merge_opt semantics
  // as every other optional field.
  ModelOptions base;
  base.naming_dialect = OptName {"gtopt"};

  ModelOptions override_plexos;
  override_plexos.naming_dialect = OptName {"plexos"};

  base.merge(override_plexos);

  REQUIRE(base.naming_dialect.has_value());
  CHECK(*base.naming_dialect == "plexos");
}

TEST_CASE(
    "ModelOptions::merge — naming_dialect stays unset when neither sets it")  // NOLINT
{
  ModelOptions a;
  ModelOptions b;

  a.merge(b);

  CHECK_FALSE(a.naming_dialect.has_value());
}

TEST_CASE(
    "ModelOptions::merge — continuous_phases propagates the same way")  // NOLINT
{
  // Parallel test for `continuous_phases`.  The cascade resume path
  // depends on this exact behaviour: --no-mip sets it to "all" on the
  // global ModelOptions, and that must reach every cascade level
  // (none of which override it explicitly under --no-mip).
  ModelOptions base;
  base.continuous_phases = OptName {"all"};

  ModelOptions override_empty;
  base.merge(override_empty);

  REQUIRE(base.continuous_phases.has_value());
  CHECK(*base.continuous_phases == "all");
}

TEST_CASE(
    "ModelOptions::has_any — naming_dialect counts toward truthiness")  // NOLINT
{
  // `has_any()` is used by cascade to decide whether a level's
  // model_options block carries any override at all.  Setting
  // naming_dialect alone must register as "yes, has overrides".
  ModelOptions opts;
  CHECK_FALSE(opts.has_any());

  opts.naming_dialect = OptName {"sddp"};
  CHECK(opts.has_any());

  opts.naming_dialect.reset();
  opts.continuous_phases = OptName {"all"};
  CHECK(opts.has_any());
}

TEST_CASE(
    "ModelOptions::covers — naming_dialect contributes to coverage")  // NOLINT
{
  // `covers(other)` returns true iff applying `other` on top of
  // `*this` would not change anything.  Used by cascade level 0 to
  // skip an LP rebuild when the caller-supplied PlanningLP already
  // covers the effective level 0 model options.
  ModelOptions self;
  self.naming_dialect = OptName {"plp"};

  // other does not override naming_dialect → covered.
  ModelOptions other_empty;
  CHECK(self.covers(other_empty));

  // other sets the same value → still covered (no change).
  ModelOptions other_same;
  other_same.naming_dialect = OptName {"plp"};
  CHECK(self.covers(other_same));

  // other sets a different value → NOT covered (would mutate self).
  ModelOptions other_diff;
  other_diff.naming_dialect = OptName {"plexos"};
  CHECK_FALSE(self.covers(other_diff));
}
