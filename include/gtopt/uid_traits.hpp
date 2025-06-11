/**
 * @file      uid_traits.hpp
 * @brief     Traits for UID-based data access
 * @date      Mon Mar 24 01:48:02 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines traits for UID-based data access patterns
 */

#pragma once

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/uididx_traits.hpp>

namespace gtopt {

/**
 * @brief Traits for UID-based data access patterns
 * 
 * Provides type aliases and mappings for accessing data using UID keys
 */
struct UidTraits {
  template<typename Key, typename Value>
  using base_map_t = gtopt::flat_map<Key, Value>;

  using ClassNameType = std::string_view;
  using FieldNameType = std::string_view;
  using CFNameUid = std::tuple<ClassNameType, FieldNameType, Uid>;
  using CFName = std::tuple<ClassNameType, FieldNameType>;

  using ArrowChunkedArray = gtopt::ArrowChunkedArray;
  using ArrowTable = gtopt::ArrowTable;

  template<typename... Uid>
  using ArrowUidIdx = typename UidToArrowIdx<Uid...>::UidIdx;

  template<typename... Uid>
  using arrow_array_uid_idx_t =
      std::tuple<ArrowChunkedArray, ArrowUidIdx<Uid...>>;

  template<typename... Uid>
  using array_uid_idx_map_t =
      base_map_t<CFNameUid, arrow_array_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using arrow_table_uid_idx_t = std::tuple<ArrowTable, ArrowUidIdx<Uid...>>;

  template<typename... Uid>
  using arrow_table_map_t = base_map_t<CFName, arrow_table_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using table_uid_idx_map_t = base_map_t<CFName, arrow_table_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using VectorUidIdx = typename UidToVectorIdx<Uid...>::UidIdx;

  template<typename... Uid>
  using vector_uid_idx_t = VectorUidIdx<Uid...>;

  template<typename... Uid>
  using idx_key_t = typename UidToVectorIdx<Uid...>::IndexKey;

  template<typename value_type, typename... Uid>
  using idx_vector_t =
      typename mvector_traits<value_type, idx_key_t<Uid...>>::vector_type;

  template<typename... Uid>
  using array_table_vector_uid_idx_t = std::tuple<array_uid_idx_map_t<Uid...>,
                                                  table_uid_idx_map_t<Uid...>,
                                                  vector_uid_idx_t<Uid...>>;
  template<typename... Uid>
  using array_vector_uid_idx_v =
      std::variant<arrow_array_uid_idx_t<Uid...>, vector_uid_idx_t<Uid...>>;
};

} // namespace gtopt
