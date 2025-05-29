/**
 * @file      generator_lp.hpp
 * @brief     Linear Programming representation of a Generator for optimization
 * @date      Sat Mar 29 00:53:51 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The GeneratorLP class provides a linear programming (LP) compatible
 * representation of a Generator, which is a fundamental component for power
 * system optimization. It maintains the generator's operational constraints
 * and provides methods for LP formulation.
 *
 * @note Uses C++23 features including deducing this and structured bindings
 */

#pragma once

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system_context.hpp>
#include <string_view>

namespace gtopt
{

using GeneratorLPId = ObjectId<class GeneratorLP>;
using GeneratorLPSId = ObjectSingleId<class GeneratorLP>;

class GeneratorLP : public CapacityObjectLP<Generator>
{
public:
    constexpr static std::string_view ClassName = "Generator";

    using CapacityBase = CapacityObjectLP<Generator>;

    explicit GeneratorLP(const InputContext& ic, Generator pgenerator);

    // Structured binding support
    template<std::size_t I>
    [[nodiscard]] constexpr auto get() const noexcept {
        if constexpr (I == 0) return id();
        else if constexpr (I == 1) return generator();
        else if constexpr (I == 2) return bus();
    }

    // Generator access with deducing this
    [[nodiscard]] constexpr auto generator(this auto&& self) noexcept -> decltype(auto) {
        return FWD(self).object();
    }

    [[nodiscard]] constexpr auto bus(this auto&& self) noexcept {
        return BusLPSId{FWD(self).generator().bus};
    }

    [[nodiscard]] bool add_to_lp(SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp);

    [[nodiscard]] bool add_to_output(OutputContext& out) const;

    [[nodiscard]] const auto& generation_cols_at(
        const ScenarioIndex scenario_index,
        const StageIndex stage_index) const noexcept
    {
        return generation_cols.at({scenario_index, stage_index});
    }

private:
    OptTBRealSched pmin;
    OptTBRealSched pmax;
    OptTRealSched lossfactor;
    OptTRealSched gcost;

    STBIndexHolder generation_cols;
    STBIndexHolder capacity_rows;
};

} // namespace gtopt

// Structured binding support
namespace std
{
template<>
struct tuple_size<gtopt::GeneratorLP> : integral_constant<size_t, 3> {};

template<size_t I>
struct tuple_element<I, gtopt::GeneratorLP> {
    using type = decltype(declval<gtopt::GeneratorLP>().get<I>());
};
} // namespace std
