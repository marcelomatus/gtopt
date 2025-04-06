#pragma once

#include <gtopt/object_lp.hpp>
#include <gtopt/reserve_zone.hpp>

namespace gtopt
{

class ReserveZoneLP : public ObjectLP<ReserveZone>
{
public:
  constexpr static std::string_view ClassName = "ReserveZone";

  using Base = ObjectLP<ReserveZone>;

  explicit ReserveZoneLP(const InputContext& ic, ReserveZone&& preserve_zone);

  [[nodiscard]] constexpr auto&& reserve_zone() { return object(); }
  [[nodiscard]] constexpr auto&& reserve_zone() const { return object(); }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc, LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& urequirement_rows() const { return ur.requirement_rows; }
  [[nodiscard]] auto&& urequirement_cols() const { return ur.requirement_cols; }
  [[nodiscard]] auto&& drequirement_rows() const { return dr.requirement_rows; }
  [[nodiscard]] auto&& drequirement_cols() const { return dr.requirement_cols; }

private:
  struct Requirement
  {
    Requirement(const InputContext& ic,
                const std::string_view& cname,
                const Id& id,
                auto&& rreq,
                auto&& rcost);

    OptTBRealSched req;
    OptTRealSched cost;
    GSTBIndexHolder requirement_cols;
    GSTBIndexHolder requirement_rows;
  } ur, dr;
};

}  // namespace gtopt
