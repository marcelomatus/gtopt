/**
 * @file      system_lp.hpp
 * @brief     Header of
 * @date      Sat Mar 29 19:16:40 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <tuple>
#include <vector>

#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/scenery_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

template<typename T,
         typename SC = SystemContext,
         typename OC = OutputContext,
         typename LP = LinearProblem>
concept AddToLP = requires(T obj, const SC& sc, OC& oc, LP& lp) {
  {
    obj.add_to_lp(sc, lp)
  } -> std::same_as<bool>;
  {
    obj.add_to_output(oc)
  } -> std::same_as<bool>;
};

static_assert(AddToLP<BusLP>);

class SystemLP
{
public:
  using scenery_vector_t = std::vector<SceneryLP>;

  SystemLP(SystemLP&&) noexcept = default;
  SystemLP(const SystemLP&) = default;
  SystemLP() = delete;
  SystemLP& operator=(SystemLP&&) noexcept = default;
  SystemLP& operator=(const SystemLP&) noexcept = default;
  ~SystemLP() = default;

  explicit SystemLP(System&& psystem = {});

  constexpr auto&& sceneries() const { return m_sceneries_; }
  constexpr auto&& stages() const { return m_stages_; }
  constexpr auto&& blocks() const { return m_blocks_; }
  constexpr auto&& options() const { return m_options_; }
  auto&& scenery(const SceneryIndex s) const { return sceneries().at(s); }

  template<typename Element>
  constexpr auto push_back(Element&& e)
  {
    return std::get<Collection<Element>>(m_collections_)
        .push_back(std::forward<Element>(e));
  }

  template<typename Element>
  constexpr auto&& elements()
  {
    return std::get<Collection<Element>>(m_collections_).elements();
  }

  template<typename Element>
  constexpr auto&& elements() const
  {
    return std::get<Collection<Element>>(m_collections_).elements();
  }

  template<typename Element, template<typename> class Id>
  constexpr auto element_index(const Id<Element>& id) const
  {
    return std::get<Collection<Element>>(m_collections_).element_index(id);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id)
  {
    return std::get<Collection<Element>>(m_collections_).element(id);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id) const
  {
    return std::get<Collection<Element>>(m_collections_).element(id);
  }

  const auto& name() const { return m_system_.name; }

  void add_to_lp(LinearProblem& lp);
  void write_out(const LinearInterface& li) const;

private:
  System m_system_;

  SystemOptionsLP m_options_;

  std::vector<BlockLP> m_blocks_;
  std::vector<StageLP> m_stages_;
  std::vector<SceneryLP> m_sceneries_;

  SystemContext sc;
  InputContext ic;

  std::tuple<Collection<BusLP>,
             Collection<DemandLP>,
             Collection<GeneratorLP>,
             Collection<LineLP>>
      m_collections_;
};

}  // namespace gtopt
