/**
 * @file      input_context.hpp
 * @brief     Header of
 * @date      Sat Mar 22 22:54:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
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

    return make_array_index<Type,
                            decltype(m_array_table_maps_),
                            FSched,
                            Uids...>(
        m_system_context_.get(), cname, m_array_table_maps_, sched, id);
  }

private:
  std::reference_wrapper<const SystemContext> m_system_context_;

  mutable std::tuple<
      array_table_vector_uid_idx_t<ScenarioUid, StageUid, BlockUid>,
      array_table_vector_uid_idx_t<ScenarioUid, StageUid>,
      array_table_vector_uid_idx_t<StageUid, BlockUid>,
      array_table_vector_uid_idx_t<StageUid>>
      m_array_table_maps_;
};

}  // namespace gtopt
