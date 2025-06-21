/**
 * @file      state_variable.hpp
 * @brief     State variable representation for optimization problems
 * @author    marcelo
 * @date      Fri May  9 18:31:14 2025
 * @copyright BSD-3-Clause
 *
 * Defines the StateVariable class representing decision variables in
 * optimization models. Each variable is associated with specific phases and
 * stages in the power system model and tracks its position in the optimization
 * matrix.
 */

#pragma once

#include <string>
#include <utility>
#include <optional>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase_lp.hpp>

namespace gtopt
{

// Dedicated struct for state variable key instead of tuple
struct StateVariableKey {
    std::string label;
    StageUid stage_uid;
    
    auto operator<=>(const StateVariableKey&) const = default;
};

class StateVariable
{
public:
  // Use perfect forwarding and constraints
  template<typename... Var>
    requires (std::is_constructible_v<std::string, Var> && ...)
  [[nodiscard]] static auto key(const StageLP& stage, Var&&... var)
      -> StateVariableKey
  {
    return {as_label(std::forward<Var>(var)...), stage.uid()};
  }

  constexpr explicit StateVariable(Name name,
                                   LinearInterface& lp,
                                   Index col,
                                   const StageLP& stage) noexcept
      : m_name_(std::move(name))
      , m_lp_(lp)
      , m_col_(col)
      , m_stage_uid_(stage.uid())
  {
  }

  [[nodiscard]] constexpr NameView name() const noexcept { return m_name_; }

  [[nodiscard]] constexpr Index col() const noexcept { return m_col_; }

  // Use reference_wrapper + optional for better semantics
  using state_client_t = std::pair<std::reference_wrapper<LinearInterface>, std::optional<Index>>;

  // Use string_view to avoid allocations
  [[nodiscard]] constexpr bool register_client(std::string_view name,
                                               LinearInterface& lp,
                                               std::optional<Index> col = std::nullopt) noexcept
  {
    return m_clients_.emplace(std::string(name), state_client_t {lp, col}).second;
  }

  // Additional helper to get clients
  [[nodiscard]] auto get_clients() const noexcept -> std::span<const typename decltype(m_clients_)::value_type>
  {
    return m_clients_;
  }

private:
  Name m_name_;
  std::reference_wrapper<LinearInterface> m_lp_;
  Index m_col_ {unknown_index};
  StageUid m_stage_uid_ {unknown_uid};

  flat_map<Name, state_client_t> m_clients_;
};

}  // namespace gtopt
