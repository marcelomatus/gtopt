#pragma once

#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/single_id.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

template<typename Element>
struct ElementIndex : StrongIndexType<Element>
{
  using Base = StrongIndexType<Element>;
  using Base::Base;

  constexpr static auto Unknown = std::string::npos;

  ElementIndex()
      : Base(Unknown)
  {
  }
};

template<typename Type>
concept CopyMove = std::is_move_constructible<Type>::value  // NOLINT
    && std::is_nothrow_move_constructible<Type>::value;  // NOLINT

template<CopyMove Type,
         typename VectorType = std::vector<Type>,
         typename IndexType = ElementIndex<Type>>
class Collection
{
  using element_type = Type;
  using vector_t = VectorType;
  using index_t = typename vector_t::size_type;
  using name_t = std::string;

  using name_map_t = gtopt::flat_map<name_t, index_t>;
  using uid_map_t = gtopt::flat_map<Uid, index_t>;

  using value_name_t = typename name_map_t::value_type;
  using value_uid_t = typename uid_map_t::value_type;

  vector_t element_vector;
  name_map_t name_map;
  uid_map_t uid_map;

  //
  template<typename FirstMap, typename Key>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const Key& key)
  {
    return IndexType {first_map.at(key)};
  }

  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& /* first_map */,
                                          const SecondMap& second_map,
                                          const Name& name)
  {
    return IndexType {get_element_index(second_map, name)};
  }

  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& /* second_map */,
                                          const Uid& uid)
  {
    return IndexType {get_element_index(first_map, uid)};
  }

  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& second_map,
                                          const Id& id)
  {
    const auto iter = first_map.find(std::get<0>(id));
    if (iter != first_map.end()) {
      return IndexType {iter->second};
    }

    return get_element_index(second_map, std::get<1>(id));
  }

  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& second_map,
                                          const SingleId& id)
  {
    return (id.index() == 0) ? get_element_index(first_map, std::get<0>(id))
                             : get_element_index(second_map, std::get<1>(id));
  }

  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& /*first_map*/,
                                          const SecondMap& /*second_map*/,
                                          const IndexType& id)
  {
    return id;
  }

public:
  void build_maps()
  {
    uid_map_t puid_map;
    puid_map.reserve(element_vector.size());
    name_map_t pname_map;
    pname_map.reserve(element_vector.size());

    for (index_t i = 0; auto&& e : element_vector) {
      auto&& [uid, name] = e.id();

      if (!puid_map.emplace(uid, i).second) {
        const auto msg =
            fmt::format("in class {}, non-unique uid {} or name {}",
                        Type::ClassName,
                        uid,
                        name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      if (!pname_map.emplace(name_t {name}, i).second) {
        const auto msg =
            fmt::format("in class {}, non-unique name {} or uid {}",
                        Type::ClassName,
                        name,
                        uid);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
      ++i;
    }

    uid_map = std::move(puid_map);
    name_map = std::move(pname_map);
  }

  Collection(Collection&&) noexcept = default;
  Collection(const Collection&) = default;
  Collection() = default;
  Collection& operator=(Collection&&) noexcept = default;
  Collection& operator=(const Collection&) noexcept = default;
  ~Collection() = default;

  explicit Collection(const vector_t& pelements)
      : element_vector(pelements)
  {
    build_maps();
  }

  explicit Collection(vector_t&& pelements)
      : element_vector(std::move(pelements))
  {
    build_maps();
  }

  auto push_back(element_type&& element)
  {
    auto&& [uid, name] = element.id();
    const auto idx = element_vector.size();

    if (!uid_map.emplace(uid, idx).second) {
      const auto msg = fmt::format("in class {}, non-unique uid {} or name {}",
                                   Type::ClassName,
                                   uid,
                                   name);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    if (!name_map.emplace(name_t {name}, idx).second) {
      const auto msg = fmt::format("in class {}, non-unique name {} or uid {}",
                                   Type::ClassName,
                                   name,
                                   uid);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    element_vector.emplace_back(std::move(element));

    return IndexType {idx};
  }

  template<typename ID>
  constexpr auto element_index(const ID& id) const
  {
    return get_element_index(uid_map, name_map, id);
  }

  template<typename ID>
  constexpr auto&& element(const ID& id) const
  {
    return element_vector.at(element_index(id));
  }

  template<typename ID>
  constexpr auto&& element(const ID& id)
  {
    return element_vector.at(element_index(id));
  }

  constexpr auto&& elements() { return element_vector; }
  constexpr auto&& elements() const { return element_vector; }
  constexpr auto size() const { return element_vector.size(); }
};

}  // namespace gtopt
