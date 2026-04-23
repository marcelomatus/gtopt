// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_constraint_expr_detail.cpp
 * @brief     Unit tests for `gtopt::detail::eval_if_atom` and
 *            `gtopt::detail::check_convexity` — the two pure helpers
 *            extracted out of `user_constraint_lp.cpp` so F8's
 *            if-condition evaluator and F5/F7's convexity predicate
 *            can be exercised without building an LP.
 */

#include <doctest/doctest.h>
#include <gtopt/constraint_expr.hpp>
#include <gtopt/linear_parser.hpp>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

TEST_CASE("detail::eval_if_atom — scalar comparison operators")  // NOLINT
{
  IfCondAtom atom;
  atom.coord = IfCondAtom::Coord::Stage;
  atom.number = 5;

  SUBCASE("Eq true / false")
  {
    atom.op = IfCondAtom::Op::Eq;
    CHECK(detail::eval_if_atom(atom, 5));
    CHECK_FALSE(detail::eval_if_atom(atom, 4));
  }
  SUBCASE("Ne true / false")
  {
    atom.op = IfCondAtom::Op::Ne;
    CHECK(detail::eval_if_atom(atom, 4));
    CHECK_FALSE(detail::eval_if_atom(atom, 5));
  }
  SUBCASE("Lt boundary")
  {
    atom.op = IfCondAtom::Op::Lt;
    CHECK(detail::eval_if_atom(atom, 4));
    CHECK_FALSE(detail::eval_if_atom(atom, 5));
    CHECK_FALSE(detail::eval_if_atom(atom, 6));
  }
  SUBCASE("Le boundary")
  {
    atom.op = IfCondAtom::Op::Le;
    CHECK(detail::eval_if_atom(atom, 4));
    CHECK(detail::eval_if_atom(atom, 5));
    CHECK_FALSE(detail::eval_if_atom(atom, 6));
  }
  SUBCASE("Gt boundary")
  {
    atom.op = IfCondAtom::Op::Gt;
    CHECK_FALSE(detail::eval_if_atom(atom, 4));
    CHECK_FALSE(detail::eval_if_atom(atom, 5));
    CHECK(detail::eval_if_atom(atom, 6));
  }
  SUBCASE("Ge boundary")
  {
    atom.op = IfCondAtom::Op::Ge;
    CHECK_FALSE(detail::eval_if_atom(atom, 4));
    CHECK(detail::eval_if_atom(atom, 5));
    CHECK(detail::eval_if_atom(atom, 6));
  }
  SUBCASE("Scalar op with empty number returns false")
  {
    atom.number = std::nullopt;
    atom.op = IfCondAtom::Op::Eq;
    CHECK_FALSE(detail::eval_if_atom(atom, 5));
  }
}

TEST_CASE("detail::eval_if_atom — set membership (`in { … }`)")  // NOLINT
{
  IfCondAtom atom;
  atom.coord = IfCondAtom::Coord::Block;
  atom.op = IfCondAtom::Op::In;
  atom.set_values = {1, 3, 5};

  CHECK(detail::eval_if_atom(atom, 1));
  CHECK(detail::eval_if_atom(atom, 3));
  CHECK(detail::eval_if_atom(atom, 5));
  CHECK_FALSE(detail::eval_if_atom(atom, 2));
  CHECK_FALSE(detail::eval_if_atom(atom, 4));
  CHECK_FALSE(detail::eval_if_atom(atom, 6));

  SUBCASE("empty set is always false")
  {
    atom.set_values.clear();
    CHECK_FALSE(detail::eval_if_atom(atom, 0));
    CHECK_FALSE(detail::eval_if_atom(atom, 1));
  }
}

TEST_CASE("detail::check_convexity — UpperEnvelope (abs/max)")  // NOLINT
{
  using CK = ConvexKind;
  using CT = ConstraintType;

  // Standard good cases.
  CHECK(detail::check_convexity(CK::UpperEnvelope, CT::LESS_EQUAL, +1.0));
  CHECK(detail::check_convexity(CK::UpperEnvelope, CT::GREATER_EQUAL, -1.0));

  // Wrong sign relative to direction — must be rejected.
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::LESS_EQUAL, -1.0));
  CHECK_FALSE(
      detail::check_convexity(CK::UpperEnvelope, CT::GREATER_EQUAL, +1.0));

  // Zero coefficient is not acceptable (the wrapper contributes nothing
  // to the row and the sign-of-coef predicate is undefined).
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::LESS_EQUAL, 0.0));
  CHECK_FALSE(
      detail::check_convexity(CK::UpperEnvelope, CT::GREATER_EQUAL, 0.0));

  // EQUAL and RANGE are non-convex for any nonlinear wrapper.
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::EQUAL, +1.0));
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::EQUAL, -1.0));
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::RANGE, +1.0));
  CHECK_FALSE(detail::check_convexity(CK::UpperEnvelope, CT::RANGE, -1.0));
}

TEST_CASE("detail::check_convexity — LowerEnvelope (min)")  // NOLINT
{
  using CK = ConvexKind;
  using CT = ConstraintType;

  // Mirror of UpperEnvelope with direction flipped.
  CHECK(detail::check_convexity(CK::LowerEnvelope, CT::GREATER_EQUAL, +1.0));
  CHECK(detail::check_convexity(CK::LowerEnvelope, CT::LESS_EQUAL, -1.0));

  CHECK_FALSE(
      detail::check_convexity(CK::LowerEnvelope, CT::GREATER_EQUAL, -1.0));
  CHECK_FALSE(detail::check_convexity(CK::LowerEnvelope, CT::LESS_EQUAL, +1.0));

  CHECK_FALSE(
      detail::check_convexity(CK::LowerEnvelope, CT::GREATER_EQUAL, 0.0));
  CHECK_FALSE(detail::check_convexity(CK::LowerEnvelope, CT::EQUAL, +1.0));
  CHECK_FALSE(detail::check_convexity(CK::LowerEnvelope, CT::RANGE, +1.0));
}
