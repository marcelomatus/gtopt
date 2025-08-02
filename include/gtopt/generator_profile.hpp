/**
 * @file      generator_profile.hpp
 * @brief     Header of
 * @date      Tue Apr  1 21:20:35 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/generator.hpp>

namespace gtopt
{

struct GeneratorProfile
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId generator {unknown_uid};
  STBRealFieldSched profile {};
  OptTRealFieldSched scost {};

  // Structured binding support
  template<std::size_t I>
  [[nodiscard]] constexpr auto get() const noexcept {
    if constexpr (I == 0) { return uid; }
    else if constexpr (I == 1) { return name; }
    else if constexpr (I == 2) { return active; }
    else if constexpr (I == 3) { return generator; }
    else if constexpr (I == 4) { return profile; }
    else if constexpr (I == 5) { return scost; }
  }
};

}  // namespace gtopt
