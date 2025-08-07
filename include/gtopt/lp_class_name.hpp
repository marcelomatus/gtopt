/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct with modern C++23 features
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold both the full and
 * short names for LP object classes with C++23 features.
 */
#pragma once

#include <format>
#include <string_view>
#include <utility>  // for std::pair-like structured binding

namespace gtopt {

struct LPClassName : std::string_view {
    [[nodiscard]] constexpr auto full_name(this auto&& self) noexcept -> std::string_view {
        return std::forward_like<decltype(self)>(self);
    }

    [[nodiscard]] constexpr auto short_name(this auto&& self) noexcept -> std::string_view {
        return std::forward_like<decltype(self)>(self.m_short_name);
    }

    explicit constexpr LPClassName(std::string_view pfull_name,
                                 std::string_view pshort_name) noexcept
        : std::string_view(pfull_name),
          m_short_name(pshort_name) {}

    // Structured binding support
    template <std::size_t I>
    [[nodiscard]] constexpr auto get() const noexcept {
        if constexpr (I == 0) return full_name();
        else if constexpr (I == 1) return short_name();
    }

private:
    std::string_view m_short_name;
};

}  // namespace gtopt

// Structured binding support
template <>
struct std::tuple_size<gtopt::LPClassName> : std::integral_constant<std::size_t, 2> {};

template <std::size_t I>
struct std::tuple_element<I, gtopt::LPClassName> {
    using type = std::string_view;
};

// Specialize std::formatter for LPClassName
namespace std {
template <>
struct formatter<gtopt::LPClassName> : formatter<string_view> {
    constexpr auto parse(format_parse_context& ctx) {
        return formatter<string_view>::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const gtopt::LPClassName& name, FormatContext& ctx) const {
        return formatter<string_view>::format(name.full_name(), ctx);
    }
};
}  // namespace std
