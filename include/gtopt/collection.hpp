/**
 * @file      collection.hpp
 * @brief     Header for efficient typed element collection container
 * @date      Fri Apr 25 00:33:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a Collection container optimized for efficient
 * element lookup by UID, name, or index, with strong move semantics support.
 */

#pragma once

#include <map>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/element_index.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/single_id.hpp>
#include <spdlog/spdlog.h>

/**
 * This file defines the Collection class, which is a central data structure
 * for managing indexed collections of elements. It makes extensive use of move
 * semantics for efficient memory management, particularly for container
 * operations.
 */

namespace gtopt
{

/**
 * Concept that ensures a type can be safely moved.
 * Uses C++23 _v trait form. nothrow_move_constructible implies
 * move_constructible, so the redundant check is removed.
 */
template<typename Type>
concept CopyMove = std::is_nothrow_move_constructible_v<Type>;

/**
 * @brief A container for managing typed elements with efficient lookup by name,
 * UID, or index
 *
 * The Collection class provides a central repository for elements of a specific
 * type, with optimized access through multiple lookup methods. It enforces
 * unique UIDs and names across all contained elements.
 *
 * @tparam Type The element type stored in the collection (must satisfy CopyMove
 * concept)
 * @tparam VectorType The container type used for storage (defaults to
 * std::vector)
 * @tparam IndexType The strongly-typed index used for safe element access
 */
template<CopyMove Type,
         typename VectorType = std::vector<Type>,
         typename IndexType = ElementIndex<Type>>
class Collection
{
  using element_type = Type;
  using vector_t = VectorType;
  using index_t = vector_t::size_type;
  using name_t = std::string;

  using name_map_t = std::map<name_t, index_t>;
  using uid_map_t = gtopt::flat_map<Uid, index_t>;

  using value_name_t = name_map_t::value_type;
  using value_uid_t = uid_map_t::value_type;

  // Primary storage for elements
  vector_t element_vector {};

  // Index maps for O(1) lookup by name or UID
  name_map_t name_map {};
  uid_map_t uid_map {};

  /**
   * @brief Helper function to retrieve an element index from a map using a key
   * @tparam FirstMap The type of map to search in
   * @tparam Key The key type to lookup
   * @param first_map The map to search in
   * @param key The key to search for
   * @return A strongly-typed index to the element
   */
  template<typename FirstMap, typename Key>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          Key&& key) noexcept(false)
  {
    return IndexType {first_map.at(std::forward<Key>(key))};
  }

  /**
   * @brief Overload for retrieving an element index by name
   */
  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& /* first_map */,
                                          const SecondMap& second_map,
                                          const Name& name) noexcept(false)
  {
    return IndexType {get_element_index(second_map, name)};
  }

  /**
   * @brief Overload for retrieving an element index by UID
   */
  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& /* second_map */,
                                          const Uid& uid) noexcept(false)
  {
    return IndexType {get_element_index(first_map, uid)};
  }

  /**
   * @brief Overload for retrieving an element index from a compound ID
   * Attempts to find the element first by UID, then by name if not found
   */
  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& second_map,
                                          const Id& id) noexcept(false)
  {
    const auto iter = first_map.find(std::get<0>(id));
    if (iter != first_map.end()) {
      return IndexType {iter->second};
    }

    return get_element_index(second_map, std::get<1>(id));
  }

  /**
   * @brief Overload for retrieving an element index from a SingleId
   * Uses either the UID or name based on which is contained in the SingleId
   */
  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& first_map,
                                          const SecondMap& second_map,
                                          const SingleId& id) noexcept(false)
  {
    return (id.index() == 0) ? get_element_index(first_map, std::get<0>(id))
                             : get_element_index(second_map, std::get<1>(id));
  }

  /**
   * @brief Identity function when the ID is already an index
   * This overload allows passing an existing IndexType directly
   */
  template<typename FirstMap, typename SecondMap>
  constexpr static auto get_element_index(const FirstMap& /*first_map*/,
                                          const SecondMap& /*second_map*/,
                                          const IndexType& id) noexcept
  {
    return id;
  }

public:
  /**
   * Builds the mapping structures for efficient element lookup by name or UID.
   * Uses move semantics to efficiently transfer ownership from temporary maps
   * to member maps once they're fully constructed.
   */
  void build_maps()
  {
    uid_map_t puid_map;
    map_reserve(puid_map, element_vector.size());
    name_map_t pname_map;

    for (index_t i = 0; auto&& e : element_vector) {
      auto&& [uid, name] = e.id();

      if (!puid_map.emplace(uid, i).second) {
        const auto msg =
            std::format("in class {}, non-unique uid {} or name {}",
                        Type::ClassName,
                        uid,
                        name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      if (!pname_map.emplace(name_t {name}, i).second) {
        const auto msg =
            std::format("in class {}, non-unique name {} or uid {}",
                        Type::ClassName,
                        name,
                        uid);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
      ++i;
    }

    // Use move semantics to efficiently transfer ownership from temporary maps
    uid_map = std::move(puid_map);
    name_map = std::move(pname_map);
  }

  /**
   * Special member functions with explicit noexcept specifications
   * to enable move plannings and provide strong exception-safety
   * guarantees.
   */
  Collection(Collection&&) noexcept = default;
  Collection(const Collection&) = default;
  Collection() = default;
  Collection& operator=(Collection&&) noexcept = default;
  Collection& operator=(const Collection&) = default;
  ~Collection() = default;

  /**
   * Constructor that takes ownership of an existing vector.
   * Uses move semantics to avoid copying the elements.
   * @param pelements Vector containing the elements to store
   */
  explicit Collection(vector_t pelements)
      : element_vector(std::move(pelements))
  {
    build_maps();
  }

  /**
   * Adds an element to the collection using perfect forwarding.
   * The template parameter allows both lvalue and rvalue references
   * to be passed efficiently without unnecessary copies.
   *
   * @param element The element to add (can be lvalue or rvalue)
   * @return The index of the newly added element
   */
  template<typename EType>
  auto push_back(EType&& element)
  {
    auto&& [uid, name] = element.id();
    const auto idx = element_vector.size();

    if (!uid_map.emplace(uid, idx).second) {
      const auto msg = std::format("in class {}, non-unique uid {} or name {}",
                                   Type::ClassName,
                                   uid,
                                   name);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    if (!name_map.emplace(name_t {name}, idx).second) {
      const auto msg = std::format("in class {}, non-unique name {} or uid {}",
                                   Type::ClassName,
                                   name,
                                   uid);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    // Use std::forward to preserve value category (lvalue/rvalue)
    element_vector.emplace_back(std::forward<EType>(element));

    return IndexType {idx};
  }

  /**
   * @brief Get the index of an element by its ID (name, UID, or compound ID)
   * @tparam ID The type of identifier to lookup
   * @param id The identifier to lookup
   * @return A strongly-typed index to the element
   * @throws std::out_of_range if the element is not found
   */
  template<typename ID>
  [[nodiscard]] constexpr auto element_index(const ID& id) const noexcept(false)
  {
    return get_element_index(uid_map, name_map, id);
  }

  /**
   * @brief Get a reference to an element by its ID (const version)
   * @tparam ID The type of identifier to lookup
   * @param id The identifier to lookup
   * @return A const reference to the requested element
   * @throws std::out_of_range if the element is not found
   */
  template<typename Self, typename ID>
  constexpr auto&& element(this Self&& self, const ID& id)
  {
    const auto index = self.element_index(id);
    return std::forward<Self>(self).element_vector.at(index);
  }

  /**
   * @brief Get a reference to the underlying element vector (mutable version)
   * @return A mutable reference to the element vector
   */
  template<typename Self>
  constexpr auto&& elements(this Self&& self) noexcept
  {
    return std::forward<Self>(self).element_vector;
  }

  /**
   * @brief Get the number of elements in the collection
   * @return The element count
   */
  [[nodiscard]] constexpr auto size() const noexcept
  {
    return element_vector.size();
  }

  [[nodiscard]] constexpr auto empty() const noexcept
  {
    return element_vector.empty();
  }
};

/**
 * Visits each element in multiple collections and applies an operation to them.
 * Uses perfect forwarding throughout to minimize copies and maximize
 * performance.
 *
 * This function is marked noexcept to enable compiler plannings and
 * provide stronger exception safety guarantees.
 *
 * @param collections Tuple of collections to visit
 * @param op Operation to apply to each element
 * @param args Additional arguments to forward to the operation
 * @return Count of successful operations
 */
template<typename Collections, typename Op, typename... Args>
constexpr auto visit_elements(Collections&& collections,
                              Op op,
                              Args&&... args)  // NOLINT
{
  std::size_t count = 0;

  std::apply(
      [&](auto&&... coll)
      {
        (void)((... ||
                [&]()
                {
                  if (coll.elements().empty()) {
                    return false;
                  }

                  for (auto&& element :
                       std::forward<decltype(coll)>(coll).elements()) {
                    if (op(element, std::forward<Args>(args)...)) [[likely]] {
                      ++count;
                    }
                  }
                  return false;
                }()));
      },
      std::forward<Collections>(collections));

  return count;
}

}  // namespace gtopt
