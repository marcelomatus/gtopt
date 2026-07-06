/**
 * @file      input_context.hpp
 * @brief     Input context for reading scheduled data into LP elements
 * @date      Sat Mar 22 22:54:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the InputContext class, which provides methods for
 * reading Arrow-based indexed arrays and schedules into LP element data.
 */

#pragma once

#include <gtopt/array_index_traits.hpp>
#include <gtopt/element_context.hpp>

namespace gtopt
{

class SystemContext;
class SystemLP;

class InputContext
    : public ElementContext<SystemLP>
    , public InputTraits
{
public:
  explicit InputContext(const SystemContext& system_context);

  [[nodiscard]] constexpr auto&& system_context() const
  {
    return m_system_context_.get();
  }

  template<typename Type, typename FSched, typename... Uids>
  auto get_array_index(const FSched& sched,
                       std::string_view cname,
                       const Id& id) const
  {
    SPDLOG_DEBUG(
        "get_array_index: cname '{}' id '{} {}'", cname, id.first, id.second);

    // The arrow input index is scene/phase-invariant, so it lives once on the
    // shared SimulationLP and is reused across every per-cell build instead of
    // being rebuilt (and the parquet re-read) per InputContext — that per-cell
    // rebuild was the long-direct-input LP-build blow-up.  make_array_index
    // fetches the shared cache from system_context.simulation() and takes its
    // lock (the per-(scene, phase) SystemLP builds run concurrently); doing it
    // there keeps the SimulationLP/SystemContext completeness off this header.
    return make_array_index<Type, FSched, Uids...>(
        m_system_context_.get(), cname, sched, id);
  }

  // Bring the template element_index from the base class into scope so that
  // the non-template overloads below do not hide it for other element types.
  using ElementContext<SystemLP>::element_index;

  // Non-template element_index overloads — declared here, defined in
  // input_context.cpp (which includes system_lp.hpp).  These allow callers
  // such as reserve_provision_lp.cpp to resolve element indices for these
  // specific types without themselves including system_lp.hpp.
  template<typename Element>
  [[nodiscard]] auto element_index(const ObjectSingleId<Element>& id) const
      -> ElementIndex<Element>;

private:
  std::reference_wrapper<const SystemContext> m_system_context_;
};

}  // namespace gtopt
